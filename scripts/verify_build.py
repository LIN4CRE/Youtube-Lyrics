#!/usr/bin/env python3
"""Verify a built Windows executable before it can be published.

This is the gate that stops a build with missing resources from being copied
over a working distributable. It inspects the PE image itself rather than
trusting the build log:

  * DOS + PE signatures, x86-64 machine type, GUI subsystem
  * an RT_GROUP_ICON entry, with every referenced RT_ICON frame present
  * the three embedded wallpaper RCDATA resources
  * a VS_VERSION_INFO resource whose version matches VERSION
  * the size bound the updater accepts, and the SHA-256 of the file

Usage:
  python3 scripts/verify_build.py dist/YoutubeLyrics.exe
  python3 scripts/verify_build.py dist/YoutubeLyrics.exe --json out.json
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

RT_ICON = 3
RT_GROUP_ICON = 14
RT_VERSION = 16
RT_RCDATA = 10
IMAGE_SUBSYSTEM_WINDOWS_GUI = 2
PE32_PLUS_MAGIC = 0x20B
MACHINE_X64 = 0x8664
MIN_BYTES = 128 * 1024
MAX_BYTES = 64 * 1024 * 1024

# Resource ids from src/win/resource.h. Kept as literals so this checker does
# not depend on a C preprocessor.
WALLPAPER_IDS = {201: "city-rain.jpg", 202: "mountain-night.jpg", 203: "forest-mist.jpg"}


class VerifyError(Exception):
    pass


def u16(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def pe_headers(data: bytes) -> dict:
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise VerifyError("missing DOS/PE header")
    pe = u32(data, 0x3C)
    if pe < 0x40 or pe + 24 > len(data) or pe > (1 << 20):
        raise VerifyError("invalid PE header offset")
    if data[pe : pe + 4] != b"PE\0\0":
        raise VerifyError("invalid PE signature")
    machine = u16(data, pe + 4)
    if machine != MACHINE_X64:
        raise VerifyError(f"expected x86-64 machine 0x8664, found 0x{machine:04x}")
    number_of_sections = u16(data, pe + 6)
    optional_size = u16(data, pe + 20)
    optional = pe + 24
    if optional + optional_size > len(data):
        raise VerifyError("truncated optional header")
    magic = u16(data, optional)
    if magic != PE32_PLUS_MAGIC:
        raise VerifyError(f"expected a PE32+ optional header, found 0x{magic:04x}")
    subsystem = u16(data, optional + 68)
    section_table = optional + optional_size
    sections = []
    for i in range(number_of_sections):
        entry = section_table + i * 40
        if entry + 40 > len(data):
            raise VerifyError("truncated section table")
        name = data[entry : entry + 8].rstrip(b"\0").decode("ascii", "replace")
        virtual_size = u32(data, entry + 8)
        virtual_address = u32(data, entry + 12)
        raw_size = u32(data, entry + 16)
        raw_pointer = u32(data, entry + 20)
        sections.append(
            {
                "name": name,
                "virtual_address": virtual_address,
                "virtual_size": virtual_size,
                "raw_pointer": raw_pointer,
                "raw_size": raw_size,
            }
        )
    if not sections:
        raise VerifyError("no sections")
    return {
        "pe_offset": pe,
        "machine": machine,
        "subsystem": subsystem,
        "optional_size": optional_size,
        "sections": sections,
    }


def rva_to_offset(headers: dict, rva: int) -> int:
    for section in headers["sections"]:
        start = section["virtual_address"]
        size = max(section["virtual_size"], section["raw_size"])
        if start <= rva < start + size:
            return section["raw_pointer"] + (rva - start)
    raise VerifyError(f"RVA 0x{rva:x} is not inside any section")


def data_directories(data: bytes, headers: dict) -> tuple[int, int]:
    """Returns (offset of data directory 2, offset of data directory 14).

    The data directory array starts at optional_header + 112 on both PE32 and
    PE32+ (the PE32+ extra 16 bytes are in the *scalar* fields before it), and
    each entry is 8 bytes. Directory 2 is the resource table.
    """
    optional = headers["pe_offset"] + 24
    array = optional + 112
    return array + 2 * 8, array + 14 * 8


def resource_root(data: bytes, headers: dict) -> int:
    directory, _ = data_directories(data, headers)
    rva = u32(data, directory)
    if rva == 0:
        raise VerifyError("the executable has no resource directory")
    root = rva_to_offset(headers, rva)
    if root + 16 > len(data):
        raise VerifyError("truncated resource directory")
    return root


def resource_entries(data: bytes, directory: int) -> list[dict]:
    named = u16(data, directory + 12)
    ids = u16(data, directory + 14)
    entries = []
    for i in range(named + ids):
        entry = directory + 16 + i * 8
        if entry + 8 > len(data):
            raise VerifyError("truncated resource entry")
        name_field = u32(data, entry)
        offset_field = u32(data, entry + 4)
        entries.append(
            {
                "id": None if name_field & 0x80000000 else name_field,
                "is_directory": bool(offset_field & 0x80000000),
                "offset": offset_field & 0x7FFFFFFF,
            }
        )
    return entries


def walk_resources(data: bytes, headers: dict) -> dict:
    """Returns {type_id: {name_id: bytes}} for numeric names only."""
    root = resource_root(data, headers)
    out: dict[int, dict[int, bytes]] = {}
    for type_entry in resource_entries(data, root):
        if type_entry["id"] is None or not type_entry["is_directory"]:
            continue
        type_id = type_entry["id"]
        level = root + type_entry["offset"]
        out.setdefault(type_id, {})
        for name_entry in resource_entries(data, level):
            if name_entry["id"] is None or not name_entry["is_directory"]:
                continue
            lang_level = root + name_entry["offset"]
            for lang_entry in resource_entries(data, lang_level):
                if lang_entry["is_directory"]:
                    continue
                data_entry = root + lang_entry["offset"]
                if data_entry + 16 > len(data):
                    raise VerifyError("truncated resource data entry")
                rva = u32(data, data_entry)
                size = u32(data, data_entry + 4)
                offset = rva_to_offset(headers, rva)
                if offset + size > len(data):
                    raise VerifyError("resource data runs past the end of the file")
                out[type_id][name_entry["id"]] = data[offset : offset + size]
    return out


CONTAINER_KEYS = {"VS_VERSION_INFO", "StringFileInfo", "VarFileInfo", "Translation"}


def version_strings(block: bytes) -> dict[str, str]:
    """Reads the StringFileInfo values out of a VS_VERSIONINFO resource.

    Node layout: wLength, wValueLength, wType, szKey (NUL-terminated UTF-16),
    4-byte aligned, then a value and 4-byte aligned children. The wType field
    is not a reliable container marker (resource compilers emit 1 for
    StringFileInfo), so containers are recognised by their key.
    """
    values: dict[str, str] = {}

    def align4(value: int) -> int:
        return (value + 3) & ~3

    def read_key(offset: int, limit: int) -> tuple[str, int]:
        end = offset
        while end + 1 < limit and block[end : end + 2] != b"\0\0":
            end += 2
        return block[offset:end].decode("utf-16-le", "replace"), end + 2

    def parse_node(offset: int, limit: int, depth: int) -> None:
        if depth > 5 or offset + 6 > limit:
            return
        length = u16(block, offset)
        value_length = u16(block, offset + 2)
        node_end = min(limit, offset + length) if length else limit
        if node_end <= offset:
            return
        key, cursor = read_key(offset + 6, node_end)
        cursor = align4(cursor)
        is_language_table = len(key) == 8 and all(c in "0123456789abcdefABCDEF" for c in key)
        if key not in CONTAINER_KEYS and not is_language_table:
            raw = block[cursor : min(node_end, cursor + value_length * 2)]
            values[key] = raw.decode("utf-16-le", "replace").rstrip("\0")
            return
        # Containers skip the fixed VS_FIXEDFILEINFO block (13 fields) when set.
        child = align4(cursor + 52) if value_length else cursor
        while child + 6 <= node_end:
            child_length = u16(block, child)
            if child_length < 6:
                break
            parse_node(child, node_end, depth + 1)
            child = align4(child + child_length)

    parse_node(0, len(block), 0)
    return values


def validate(path: Path, allow_missing_resources: bool = False) -> dict:
    if not path.is_file():
        raise VerifyError(f"{path} does not exist")
    data = path.read_bytes()
    if not MIN_BYTES <= len(data) <= MAX_BYTES:
        raise VerifyError(
            f"executable size {len(data):,} bytes is outside the accepted "
            f"{MIN_BYTES:,}-{MAX_BYTES:,} byte range"
        )
    headers = pe_headers(data)
    if headers["subsystem"] != IMAGE_SUBSYSTEM_WINDOWS_GUI:
        raise VerifyError(
            f"expected a Windows GUI subsystem ({IMAGE_SUBSYSTEM_WINDOWS_GUI}), "
            f"found {headers['subsystem']}"
        )
    if allow_missing_resources:
        # Only reachable through YTL_ALLOW_ICONLESS_BUILD=1, which is never used
        # for a release. The PE itself must still be a real x64 GUI image.
        return {
            "path": str(path),
            "size_bytes": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
            "machine": f"0x{headers['machine']:04x}",
            "subsystem": "windows-gui",
            "version": None,
            "icon_sizes": [],
            "wallpapers": [],
            "resources_skipped": True,
        }

    resources = walk_resources(data, headers)
    icons = resources.get(RT_ICON, {})
    groups = resources.get(RT_GROUP_ICON, {})
    if not groups:
        raise VerifyError("the executable has no RT_GROUP_ICON: it would ship without an icon")
    if not icons:
        raise VerifyError("the executable has no RT_ICON frames")

    icon_sizes = []
    for group in groups.values():
        if len(group) < 6:
            raise VerifyError("truncated RT_GROUP_ICON")
        count = u16(group, 4)
        offset = 6
        for _ in range(count):
            if offset + 14 > len(group):
                raise VerifyError("truncated icon group entry")
            width = group[offset]
            height = group[offset + 1]
            planes = u16(group, offset + 4)
            bits = u16(group, offset + 6)
            size = u32(group, offset + 8)
            name_id = u16(group, offset + 12)
            if name_id not in icons:
                raise VerifyError(f"icon group references missing RT_ICON {name_id}")
            frame = icons[name_id]
            if len(frame) != size:
                raise VerifyError(f"RT_ICON {name_id} size {len(frame)} != group entry {size}")
            if planes not in (0, 1):
                raise VerifyError(f"RT_ICON {name_id} has {planes} planes")
            if bits not in (0, 32):
                raise VerifyError(f"RT_ICON {name_id} has {bits} bits per pixel")
            icon_sizes.append(256 if width == 0 else width)
            offset += 14
    for required in (16, 32, 48, 256):
        if required not in icon_sizes:
            raise VerifyError(f"the icon has no {required}px frame")
    if len(set(icon_sizes)) != len(icon_sizes):
        raise VerifyError(f"the icon repeats a size: {icon_sizes}")

    for resource_id, filename in WALLPAPER_IDS.items():
        payload = resources.get(RT_RCDATA, {}).get(resource_id)
        if not payload:
            raise VerifyError(f"missing embedded wallpaper {filename} (RCDATA {resource_id})")
        if payload[:2] != b"\xff\xd8":
            raise VerifyError(f"wallpaper {filename} is not a JPEG payload")
        if payload.rstrip()[-2:] != b"\xff\xd9":
            raise VerifyError(f"wallpaper {filename} has a truncated JPEG stream")

    version = resources.get(RT_VERSION, {})
    if not version:
        raise VerifyError("the executable has no RT_VERSION resource")
    block = next(iter(version.values()))
    strings = version_strings(block)
    expected = (ROOT / "VERSION").read_text(encoding="ascii").strip()
    # The string form carries the fourth (build) component, e.g. "1.5.0.0".
    for field in ("ProductVersion", "FileVersion"):
        found = strings.get(field)
        if found not in (expected, expected + ".0"):
            raise VerifyError(f"{field} {found!r} does not match VERSION {expected!r}")
    # The fixed file version must carry the same numbers.
    # VS_FIXEDFILEINFO starts 40 bytes in; dwFileVersionMS/LS follow its
    # signature and structure version at +8/+12. The low word of LS is the
    # build component, which the stub sets to zero.
    if len(block) >= 52:
        signature = u32(block, 40)
        if signature != 0xFEEF04BD:
            raise VerifyError(f"invalid VS_FIXEDFILEINFO signature 0x{signature:08x}")
        ms, ls = struct.unpack_from("<II", block, 48)
        numbers = (ms >> 16, ms & 0xFFFF, ls >> 16)
        expected_numbers = tuple(int(part) for part in expected.split("."))
        if numbers != expected_numbers:
            raise VerifyError(
                f"fixed file version {numbers} does not match VERSION {expected_numbers}"
            )
    if strings.get("OriginalFilename") != "YoutubeLyrics.exe":
        raise VerifyError("OriginalFilename is not YoutubeLyrics.exe")

    return {
        "path": str(path),
        "size_bytes": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "machine": f"0x{headers['machine']:04x}",
        "subsystem": "windows-gui",
        "version": expected,
        "icon_sizes": icon_sizes,
        "wallpapers": sorted(WALLPAPER_IDS.values()),
    }


def verify_ico(path: Path) -> dict:
    """Checks the standalone .ico in docs/brand for the same size coverage."""
    data = path.read_bytes()
    if len(data) < 6:
        raise VerifyError("icon file is too short")
    reserved, kind, count = struct.unpack_from("<HHH", data)
    if reserved != 0 or kind != 1 or count == 0:
        raise VerifyError("not a valid .ico container")
    sizes = []
    for i in range(count):
        entry = 6 + i * 16
        if entry + 16 > len(data):
            raise VerifyError("truncated .ico directory")
        width, height, _colours, _res, _planes, _bits, size, offset = struct.unpack_from(
            "<BBBBHHII", data, entry
        )
        if offset + size > len(data):
            raise VerifyError("truncated .ico frame")
        if width != height:
            raise VerifyError("icon frames must be square")
        sizes.append(256 if width == 0 else width)
    for required in (16, 20, 24, 32, 40, 48, 64, 96, 128, 256):
        if required not in sizes:
            raise VerifyError(f"{path.name} has no {required}px frame")
    if len(set(sizes)) != len(sizes):
        raise VerifyError(f"{path.name} repeats a size: {sizes}")
    return {"path": str(path), "sizes": sizes, "size_bytes": len(data)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("exe", type=Path, nargs="?", default=Path("dist/YoutubeLyrics.exe"))
    parser.add_argument("--ico", type=Path, default=None, help="also verify this .ico file")
    parser.add_argument("--json", type=Path, default=None, help="write the results as JSON")
    parser.add_argument(
        "--allow-missing-resources",
        action="store_true",
        help="accept an executable with no icon, version or wallpaper resources "
        "(throwaway builds only; never a release)",
    )
    args = parser.parse_args()

    try:
        report = validate(args.exe, allow_missing_resources=args.allow_missing_resources)
        if args.ico:
            report["ico"] = verify_ico(args.ico)
    except (VerifyError, OSError, struct.error) as exc:
        print(f"build verification failed: {exc}", file=sys.stderr)
        return 1

    if report.get("resources_skipped"):
        print(
            f"validated {report['path']} without resources: "
            f"{report['size_bytes']:,} bytes, {report['machine']} {report['subsystem']}"
        )
        print("  warning: no icon, version or wallpaper resources (throwaway build)")
    else:
        print(
            f"verified {report['path']}: {report['size_bytes']:,} bytes, "
            f"{report['machine']} {report['subsystem']}, version {report['version']}, "
            f"icon sizes {report['icon_sizes']}"
        )
        print(f"  embedded wallpapers: {', '.join(report['wallpapers'])}")
    print(f"  SHA-256: {report['sha256']}")
    if "ico" in report:
        print(f"  {report['ico']['path']}: sizes {report['ico']['sizes']}")
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
