#!/usr/bin/env python3
"""Generates the application icon (.ico) and a Windows .res resource file.

The icon mirrors the one the app draws for itself at runtime: a rounded purple
square with four equaliser bars. Building the .res here keeps the build free of
any Windows-only tooling.
"""
import os
import struct
import sys

from PIL import Image, ImageDraw

SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
TOP = (124, 92, 255)
BOTTOM = (60, 40, 150)


def draw_icon(size: int) -> Image.Image:
    scale = 4  # supersample, then downscale for clean edges
    s = size * scale
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))

    # vertical-ish diagonal gradient
    grad = Image.new("RGBA", (s, s))
    px = grad.load()
    for y in range(s):
        t = y / max(1, s - 1)
        r = int(TOP[0] + (BOTTOM[0] - TOP[0]) * t)
        g = int(TOP[1] + (BOTTOM[1] - TOP[1]) * t)
        b = int(TOP[2] + (BOTTOM[2] - TOP[2]) * t)
        for x in range(s):
            px[x, y] = (r, g, b, 255)

    mask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        [int(s * 0.02), int(s * 0.02), s - 1 - int(s * 0.02), s - 1 - int(s * 0.02)],
        radius=int(s * 0.24),
        fill=255,
    )
    img.paste(grad, (0, 0), mask)

    d = ImageDraw.Draw(img)
    bar_w = s * 0.105
    gap = s * 0.075
    total = 4 * bar_w + 3 * gap
    x = (s - total) * 0.5
    heights = [0.30, 0.52, 0.40, 0.62]
    for h in heights:
        bh = s * h
        y = (s - bh) * 0.5
        d.rounded_rectangle(
            [x, y, x + bar_w, y + bh], radius=bar_w * 0.5, fill=(255, 255, 255, 255)
        )
        x += bar_w + gap

    return img.resize((size, size), Image.LANCZOS)


def build_ico(path: str) -> list:
    images = [draw_icon(s) for s in SIZES]
    # Pillow derives every size from the largest image we hand it.
    master = images[-1]
    master.save(path, format="ICO", sizes=[(s, s) for s in SIZES])
    return images


def parse_ico(path: str) -> list:
    """Returns [(width_byte, height_byte, planes, bitcount, data_bytes)]."""
    with open(path, "rb") as f:
        data = f.read()
    reserved, itype, count = struct.unpack_from("<HHH", data, 0)
    assert reserved == 0 and itype == 1, "not an icon file"
    out = []
    for i in range(count):
        w, h, colors, res, planes, bits, size, offset = struct.unpack_from(
            "<BBBBHHII", data, 6 + i * 16
        )
        out.append((w, h, colors, res, planes, bits, data[offset:offset + size]))
    return out


def build_res(ico_path: str, res_path: str) -> None:
    entries = parse_ico(ico_path)

    RT_ICON = 3
    RT_GROUP_ICON = 14

    # Resource data blocks, each 4-byte aligned.
    blobs = []  # (type_id, name_id, payload)
    for i, (w, h, colors, res, planes, bits, payload) in enumerate(entries, start=1):
        blobs.append((RT_ICON, i, payload))

    group = struct.pack("<HHH", 0, 1, len(entries))
    for i, (w, h, colors, res, planes, bits, payload) in enumerate(entries, start=1):
        group += struct.pack(
            "<BBBBHHIH", w, h, colors, res, planes or 1, bits or 32, len(payload), i
        )
    blobs.append((RT_GROUP_ICON, 1, group))

    # Header: DataSize=0, HeaderSize=32, then one type directory per type id.
    types = sorted({t for t, _, _ in blobs})
    header = struct.pack("<II", 0, 32)
    dir_offset = len(header) + len(types) * 8
    type_dirs = b""
    data_region = b""
    # Data offsets are relative to the start of the file.
    data_start = dir_offset + len(blobs) * 16
    for t in types:
        names = [(n, p) for (tt, n, p) in blobs if tt == t]
        type_dirs += struct.pack("<HHH", t, len(names), 0)
    entry_bytes = b""
    for t in types:
        for name, payload in [(n, p) for (tt, n, p) in blobs if tt == t]:
            # Resource entries are 16 bytes: WORD name + 2 bytes padding + 3 DWORDs.
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


def main() -> int:
    out_dir = sys.argv[1] if len(sys.argv) > 1 else ".build"
    os.makedirs(out_dir, exist_ok=True)
    ico = os.path.join(out_dir, "app.ico")
    build_ico(ico)
    print(f"wrote {ico}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
