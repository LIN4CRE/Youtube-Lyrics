#!/usr/bin/env python3
"""Make a linked Windows executable byte-for-byte reproducible.

The linker stamps every image with the time of the link (COFF header and each
debug-directory entry) and with a build GUID derived from it. Two builds of the
same source therefore never had the same SHA-256, so "the download is the build
of this commit" could not be checked by rebuilding. This rewrites those fields:

  * the timestamps become a fixed constant, and
  * the CodeView build GUID becomes a hash of the rest of the image,

so identical source and toolchain give identical bytes, and a different image
still gets a different GUID. Nothing else in the file is touched.

    python3 scripts/normalize_pe.py .build/YoutubeLyrics.exe
"""
from __future__ import annotations

import hashlib
import struct
import sys
from pathlib import Path

# 2020-01-01T00:00:00Z. Any fixed value works; zero is avoided because some
# tools treat a zero timestamp as "not an image".
FIXED_TIMESTAMP = 1577836800
DEBUG_DIRECTORY = 6
DEBUG_TYPE_CODEVIEW = 2


class PeError(ValueError):
    pass


def _rva_to_offset(data: bytes, pe: int, rva: int) -> int:
    sections = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    table = pe + 24 + optional_size
    for index in range(sections):
        entry = table + index * 40
        virtual_size, virtual_address, raw_size, raw_pointer = struct.unpack_from(
            "<IIII", data, entry + 8
        )
        if virtual_address <= rva < virtual_address + max(virtual_size, raw_size):
            return raw_pointer + (rva - virtual_address)
    raise PeError(f"RVA 0x{rva:x} is not inside any section")


def normalize(image: bytes) -> bytes:
    data = bytearray(image)
    if len(data) < 0x100 or data[:2] != b"MZ":
        raise PeError("not a PE image (no DOS header)")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if pe <= 0 or pe + 24 + 240 > len(data) or data[pe : pe + 4] != b"PE\0\0":
        raise PeError("not a PE image (no PE signature)")
    magic = struct.unpack_from("<H", data, pe + 24)[0]
    if magic != 0x20B:
        raise PeError(f"expected a PE32+ image, found optional-header magic 0x{magic:x}")

    struct.pack_into("<I", data, pe + 8, FIXED_TIMESTAMP)

    guid_offset = None
    directories = struct.unpack_from("<I", data, pe + 24 + 108)[0]
    if directories > DEBUG_DIRECTORY:
        rva, size = struct.unpack_from("<II", data, pe + 24 + 112 + DEBUG_DIRECTORY * 8)
        if rva and size:
            start = _rva_to_offset(data, pe, rva)
            if size % 28 or start + size > len(data):
                raise PeError("malformed debug directory")
            for entry in range(start, start + size, 28):
                struct.pack_into("<I", data, entry + 4, FIXED_TIMESTAMP)
                kind, data_size, _, pointer = struct.unpack_from("<IIII", data, entry + 12)
                if kind == DEBUG_TYPE_CODEVIEW and data_size >= 24:
                    if pointer + 24 > len(data) or data[pointer : pointer + 4] != b"RSDS":
                        raise PeError("unsupported CodeView record")
                    guid_offset = pointer + 4

    if guid_offset is not None:
        data[guid_offset : guid_offset + 16] = bytes(16)
        data[guid_offset : guid_offset + 16] = hashlib.sha256(bytes(data)).digest()[:16]
    return bytes(data)


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print("usage: normalize_pe.py <executable>", file=sys.stderr)
        return 2
    path = Path(argv[1])
    try:
        original = path.read_bytes()
        result = normalize(original)
    except (OSError, PeError, struct.error) as exc:
        print(f"normalize_pe: {path}: {exc}", file=sys.stderr)
        return 1
    if result != original:
        path.write_bytes(result)
    print(f"normalized {path} ({hashlib.sha256(result).hexdigest()})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
