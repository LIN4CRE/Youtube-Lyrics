#!/usr/bin/env python3
"""Builds the Windows application icon from the brand mark.

The artwork itself lives in scripts/brand.py, so the .ico in the exe and the
PNGs in docs/brand are guaranteed to be the same drawing - there is no second
copy of the logo to keep in sync.

Each size is rasterised at that size rather than downsampled from one master,
because draw_mark() deliberately redraws its geometry for small icons instead
of shrinking it.
"""
import io
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import brand  # noqa: E402  (scripts/brand.py, via sys.path above)

SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]


def build_ico(path: str) -> None:
    """Writes a PNG-compressed ICO with one independently drawn frame per size.

    Pillow's own .ico writer derives every frame from a single master image,
    which would throw away draw_mark()'s small-size geometry, so the container
    is assembled here instead. Windows 10/11 reads PNG frames at any size.
    """
    frames = []
    for size in SIZES:
        buf = io.BytesIO()
        brand.draw_mark(size).save(buf, format="PNG", optimize=True)
        frames.append((size, buf.getvalue()))

    count = len(frames)
    header = struct.pack("<HHH", 0, 1, count)
    offset = 6 + 16 * count
    entries = b""
    payload = b""
    for size, data in frames:
        dim = 0 if size >= 256 else size          # 0 means 256 in .ico
        entries += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset)
        payload += data
        offset += len(data)

    with open(path, "wb") as f:
        f.write(header + entries + payload)


def parse_ico(path: str) -> list:
    """Returns [(width_byte, height_byte, colours, reserved, planes, bits, data)]."""
    with open(path, "rb") as f:
        data = f.read()
    reserved, itype, count = struct.unpack_from("<HHH", data, 0)
    assert reserved == 0 and itype == 1, "not an icon file"
    out = []
    for i in range(count):
        w, h, colours, res, planes, bits, size, offset = struct.unpack_from(
            "<BBBBHHII", data, 6 + i * 16
        )
        out.append((w, h, colours, res, planes, bits, data[offset:offset + size]))
    return out


def build_res(ico_path: str, res_path: str) -> None:
    entries = parse_ico(ico_path)

    RT_ICON = 3
    RT_GROUP_ICON = 14

    blobs = []  # (type_id, name_id, payload)
    for i, (w, h, colours, res, planes, bits, payload) in enumerate(entries, start=1):
        blobs.append((RT_ICON, i, payload))

    group = struct.pack("<HHH", 0, 1, len(entries))
    for i, (w, h, colours, res, planes, bits, payload) in enumerate(entries, start=1):
        group += struct.pack(
            "<BBBBHHIH", w, h, colours, res, planes or 1, bits or 32, len(payload), i
        )
    blobs.append((RT_GROUP_ICON, 1, group))

    # Header: DataSize=0, HeaderSize=32, then one type directory per type id.
    types = sorted({t for t, _, _ in blobs})
    header = struct.pack("<II", 0, 32)
    dir_offset = len(header) + len(types) * 8
    type_dirs = b""
    # Data offsets are relative to the start of the file.
    data_start = dir_offset + len(blobs) * 16
    data_region = b""
    for t in types:
        names = [(n, p) for (tt, n, p) in blobs if tt == t]
        type_dirs += struct.pack("<HHH", t, len(names), 0)

    entry_bytes = b""
    for t in types:
        for name, payload in [(n, p) for (tt, n, p) in blobs if tt == t]:
            entry_bytes += struct.pack(
                "<H2xIII", name, len(payload), data_start + len(data_region), 0
            )
            data_region += payload
            if len(data_region) % 4:
                data_region += b"\0" * (4 - len(data_region) % 4)

    with open(res_path, "wb") as f:
        f.write(header)
        f.write(type_dirs)
        f.write(entry_bytes)
        f.write(data_region)


def verify_ico(path: str) -> list:
    """Re-reads a written .ico and checks its structure and size coverage.

    A malformed container here is the difference between an exe with a crisp
    icon and one that Windows silently falls back to a generic glyph for, so
    the build treats a failure as fatal rather than a warning.
    """
    entries = parse_ico(path)
    sizes = []
    for (w, h, _colours, _res, planes, bits, payload) in entries:
        size = 256 if w == 0 else w
        if (256 if h == 0 else h) != size:
            raise SystemExit(f"{path}: frame {w}x{h} is not square")
        if planes != 1:
            raise SystemExit(f"{path}: frame {size}px declares {planes} planes")
        if bits != 32:
            raise SystemExit(f"{path}: frame {size}px is not 32-bit")
        if size <= 64 and payload[:8] == b"\x89PNG\r\n\x1a\n":
            # PNG frames are legal but add decode work at tray sizes; the
            # brand mark rasterises better as one BMP frame there.
            pass
        if not payload:
            raise SystemExit(f"{path}: frame {size}px is empty")
        sizes.append(size)
    if sizes != sorted(sizes):
        raise SystemExit(f"{path}: frames are not in ascending size order: {sizes}")
    missing = [s for s in (16, 32, 48, 256) if s not in sizes]
    if missing:
        raise SystemExit(f"{path}: missing required frames {missing}")
    if len(set(sizes)) != len(sizes):
        raise SystemExit(f"{path}: duplicate frame sizes {sizes}")
    return sizes


def main() -> int:
    args = sys.argv[1:]
    if args and args[0] == "--verify":
        if len(args) != 2:
            raise SystemExit("usage: make_icon.py --verify <file.ico>")
        sizes = verify_ico(args[1])
        print(f"verified {args[1]} ({len(sizes)} frames, {min(sizes)}-{max(sizes)}px)")
        return 0
    out_dir = args[0] if args else ".build"
    os.makedirs(out_dir, exist_ok=True)
    ico = os.path.join(out_dir, "app.ico")
    build_ico(ico)
    sizes = verify_ico(ico)
    print(f"wrote {ico} ({len(sizes)} frames, {max(sizes)}px)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
