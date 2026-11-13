#!/usr/bin/env python3
"""Generate Youtube-Lyrics' original, lightweight cinematic wallpaper set.

These procedural illustrations are generated locally from shapes and gradients;
no third-party photographs, network downloads, fonts, or hotlinked assets are used.
"""
from __future__ import annotations

import math
import random
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "src/win/wallpapers"
W, H = 1600, 900


def gradient(top: tuple[int, int, int], bottom: tuple[int, int, int]) -> Image.Image:
    im = Image.new("RGB", (W, H))
    px = im.load()
    for y in range(H):
        t = y / max(1, H - 1)
        row = tuple(round(top[i] * (1 - t) + bottom[i] * t) for i in range(3))
        for x in range(W):
            px[x, y] = row
    return im


def glow(im: Image.Image, xy: tuple[int, int], radius: int,
         color: tuple[int, int, int], alpha: int) -> None:
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    x, y = xy
    d.ellipse((x - radius, y - radius, x + radius, y + radius), fill=(*color, alpha))
    layer = layer.filter(ImageFilter.GaussianBlur(radius * 0.58))
    im.paste(Image.alpha_composite(im.convert("RGBA"), layer).convert("RGB"))


def soft_band(im: Image.Image, y: int, height: int,
              colour: tuple[int, int, int], alpha: int) -> None:
    """A horizontal band of light with feathered edges.

    Drawn into its own layer with a per-row alpha gradient and then blurred, so
    it reads as haze. A flat ImageDraw.rectangle (the obvious version) leaves a
    hard line that looks like a sheet of glass lying across the scene.
    """
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    px = layer.load()
    for row in range(max(0, y), min(H, y + height)):
        t = (row - y) / max(1, height - 1)
        # sine-window: transparent at both ends, strongest in the middle
        weight = math.sin(t * math.pi) ** 1.4
        px_row = int(alpha * weight)
        if px_row <= 0:
            continue
        for x in range(W):
            px[x, row] = (*colour, px_row)
    layer = layer.filter(ImageFilter.GaussianBlur(height * 0.35))
    im.paste(Image.alpha_composite(im.convert("RGBA"), layer).convert("RGB"))


def fade_band(layer: Image.Image, top: int, bottom: int, feather: int) -> Image.Image:
    """Feathers both edges of the content between `top` and `bottom`.

    The layer is transparent outside that band already; this only softens the
    two transitions so a filled polygon cannot show its own straight base.
    """
    px = layer.load()
    for row in range(layer.height):
        if row < top or row > bottom:
            continue
        distance = min(row - top, bottom - row)
        if distance >= feather:
            continue
        factor = distance / max(1, feather)
        for x in range(layer.width):
            r, g, b, a = px[x, row]
            if a:
                px[x, row] = (r, g, b, int(a * factor))
    return layer


def fade_from(layer: Image.Image, start_row: int, end_row: int) -> Image.Image:
    """Ramps a layer's alpha from 0 at start_row to full at end_row."""
    px = layer.load()
    span = max(1, end_row - start_row)
    for row in range(layer.height):
        if row <= start_row:
            factor = 0.0
        elif row >= end_row:
            continue
        else:
            factor = (row - start_row) / span
        if factor >= 1.0:
            continue
        for x in range(layer.width):
            r, g, b, a = px[x, row]
            px[x, row] = (r, g, b, int(a * factor))
    return layer


def vignette(im: Image.Image, strength: int = 90) -> Image.Image:
    # White center preserves the artwork; black edges add a restrained vignette.
    mask = Image.new("L", (W, H), 0)
    ImageDraw.Draw(mask).ellipse((-W * 0.2, -H * 0.35, W * 1.2, H * 1.35), fill=255)
    mask = mask.filter(ImageFilter.GaussianBlur(110))
    if strength < 255:
        mask = mask.point(lambda value: max(0, min(255, int(strength + value * (255 - strength) / 255))))
    dark = Image.new("RGB", (W, H), (0, 0, 0))
    return Image.composite(im, dark, mask)


def grain(im: Image.Image, seed: int) -> Image.Image:
    # Stable, very fine film grain; it disappears at normal desktop viewing size.
    rng = random.Random(seed)
    small = Image.new("L", (W // 6, H // 6))
    p = small.load()
    for y in range(small.height):
        for x in range(small.width):
            p[x, y] = rng.randrange(92, 165)
    noise = small.resize((W, H), Image.Resampling.BILINEAR).convert("RGB")
    return Image.blend(im, noise, 0.025)


def city() -> Image.Image:
    im = gradient((7, 13, 23), (11, 16, 25))
    glow(im, (450, 335), 310, (30, 66, 92), 88)
    glow(im, (1210, 375), 240, (61, 32, 20), 48)
    d = ImageDraw.Draw(im, "RGBA")
    # Distant, softened skyline; the middle horizon is intentionally open.
    buildings = [
        (0, 390, 142, 670), (112, 338, 230, 680), (222, 424, 318, 685),
        (315, 375, 405, 692), (420, 462, 520, 685), (530, 420, 602, 692),
        (1015, 435, 1102, 690), (1108, 350, 1202, 680),
        (1208, 408, 1294, 692), (1304, 322, 1418, 670), (1430, 385, 1548, 682),
        (1555, 438, 1600, 682),
    ]
    for i, (x0, y0, x1, y1) in enumerate(buildings):
        tone = 14 + (i * 3 % 9)
        d.rectangle((x0, y0, x1, y1), fill=(tone, tone + 6, tone + 13, 245))
        d.rectangle((x0 + 5, y0 + 6, x1 - 5, y0 + 9), fill=(70, 83, 97, 48))
        for wy in range(y0 + 20, y1 - 16, 29):
            for wx in range(x0 + 12, x1 - 8, 20):
                if (wx * 5 + wy * 3 + i * 11) % 13 == 0:
                    d.rectangle((wx, wy, wx + 4, wy + 8), fill=(232, 157, 84, 82))
                elif (wx + wy + i) % 9 == 0:
                    d.rectangle((wx, wy, wx + 4, wy + 7), fill=(106, 153, 183, 36))

    # Wet road, tapering to a quiet vanishing point. It is drawn into its own
    # layer and its top edge is faded, so the horizon dissolves into haze
    # instead of cutting the frame with one flat line.
    road = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(road).polygon(
        [(0, 900), (0, 706), (712, 622), (888, 622), (1600, 706), (1600, 900)],
        fill=(5, 8, 14, 244))
    road = fade_band(road, 600, 900, 120)
    im.paste(Image.alpha_composite(im.convert("RGBA"), road).convert("RGB"))
    soft_band(im, 578, 150, (34, 52, 70), 30)

    d = ImageDraw.Draw(im, "RGBA")
    # Fine perspective seams and sparse amber reflections, all low contrast.
    for x in (110, 315, 520, 710, 890, 1080, 1290, 1490):
        d.line((800 + (x - 800) * 0.06, 622, x, 900), fill=(90, 112, 133, 20), width=2)
    rng = random.Random(241)
    for _ in range(90):
        x = rng.choice([rng.randrange(0, 630), rng.randrange(970, 1600)])
        y = rng.randrange(680, 895)
        length = rng.randrange(18, 110)
        alpha = rng.randrange(8, 24)
        color = (226, 152, 82, alpha) if rng.random() < 0.55 else (107, 154, 195, alpha)
        d.line((x, y, x + rng.randrange(-4, 5), min(H, y + length)), fill=color,
               width=rng.choice([1, 2, 3]))
    im = im.filter(ImageFilter.GaussianBlur(0.35))
    return grain(vignette(im, 80), 912)


def mountain() -> Image.Image:
    im = gradient((5, 10, 22), (12, 21, 34))
    glow(im, (1195, 195), 245, (37, 63, 105), 56)
    d = ImageDraw.Draw(im, "RGBA")
    # Stars are sparse and low luminance to keep icons and lyrics readable.
    rng = random.Random(417)
    for _ in range(145):
        x, y = rng.randrange(W), rng.randrange(18, 500)
        r = rng.choice([1, 1, 1, 2])
        a = rng.randrange(30, 105)
        d.ellipse((x-r, y-r, x+r, y+r), fill=(192, 211, 236, a))
    d.ellipse((1238, 120, 1325, 207), fill=(217, 222, 222, 82))
    d.ellipse((1250, 108, 1347, 202), fill=(7, 13, 25, 150))
    # Layered ridges with a broad, dark valley through the text area.
    d.polygon([(0, 560), (130, 480), (240, 526), (390, 370), (515, 490),
               (670, 405), (835, 535), (1000, 432), (1140, 520), (1300, 352),
               (1452, 500), (1600, 405), (1600, 900), (0, 900)],
              fill=(21, 33, 51, 255))
    d.polygon([(0, 650), (170, 548), (320, 625), (495, 492), (660, 610),
               (825, 552), (1002, 670), (1175, 550), (1350, 625), (1490, 520),
               (1600, 590), (1600, 900), (0, 900)],
              fill=(13, 25, 40, 255))
    d.polygon([(0, 735), (205, 650), (340, 705), (525, 615), (720, 705),
               (865, 665), (1030, 740), (1215, 646), (1420, 720), (1600, 650),
               (1600, 900), (0, 900)],
              fill=(8, 17, 29, 255))
    # Soft valley light and a tiny cabin far off-centre.
    glow(im, (830, 635), 205, (76, 102, 128), 30)
    d = ImageDraw.Draw(im, "RGBA")
    d.rectangle((1080, 628, 1114, 651), fill=(12, 17, 22, 230))
    d.polygon([(1075, 630), (1097, 612), (1119, 630)], fill=(9, 13, 18, 235))
    d.rectangle((1092, 638, 1098, 647), fill=(240, 166, 86, 150))
    # Understated snow on only the furthest peaks.
    d.line((390, 370, 352, 411, 390, 394, 428, 431), fill=(162, 181, 198, 50), width=3)
    d.line((1300, 352, 1260, 401, 1300, 382, 1340, 423), fill=(164, 184, 205, 54), width=3)
    return grain(vignette(im, 90), 221)


def forest() -> Image.Image:
    im = gradient((5, 13, 13), (8, 17, 17))
    glow(im, (820, 500), 335, (40, 74, 65), 62)
    glow(im, (850, 575), 110, (175, 107, 54), 28)
    d = ImageDraw.Draw(im, "RGBA")
    # Distant tree line and bands of cool mist.
    rng = random.Random(812)
    for layer, base_y, color, scale, fade_top in [
        (0, 486, (20, 39, 37, 150), 0.72, 30),
        (1, 556, (13, 29, 28, 185), 0.88, 36),
    ]:
        trees = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        td = ImageDraw.Draw(trees)
        x = -40
        while x < W + 40:
            height = rng.randrange(95, 230) * scale
            width = rng.randrange(27, 56) * scale
            # Jitter the treetops so the tree line does not read as a comb.
            lean = rng.uniform(-0.10, 0.10) * width
            td.polygon([(x, base_y), (x + width / 2 + lean, base_y - height),
                        (x + width, base_y)], fill=color)
            x += rng.randrange(28, 62)
        # The base of a tree layer is a straight line by construction; fading it
        # into the mist below hides that seam.
        trees = fade_band(trees, base_y - 240, base_y + 10, fade_top * 2)
        im.paste(Image.alpha_composite(im.convert("RGBA"), trees).convert("RGB"))
        d = ImageDraw.Draw(im, "RGBA")
    # Two layers of feathered mist instead of flat rectangles, so there is no
    # visible seam across the frame.
    soft_band(im, 400, 280, (128, 160, 151), 26)
    # Path recedes into the clearing without a bright focal point.
    d.polygon([(455, 900), (664, 668), (752, 600), (860, 600), (962, 668),
               (1185, 900)], fill=(6, 13, 13, 120))
    d.line((790, 610, 606, 900), fill=(108, 132, 112, 16), width=3)
    d.line((845, 610, 1042, 900), fill=(108, 132, 112, 13), width=3)
    # Near-black framing trunks and delicate branches at the edges.
    for x, width, lean in ((45, 54, 10), (225, 31, -13), (1510, 62, -7), (1370, 31, 12)):
        d.polygon([(x, 0), (x + width, 0), (x + width + lean, 900), (x - 12, 900)],
                  fill=(3, 9, 10, 245))
        for y in range(135, 620, 100):
            d.line((x + width // 2, y, x - 48, y - 68), fill=(5, 12, 12, 220), width=8)
            d.line((x + width // 2, y + 28, x + width + 60, y - 35),
                   fill=(5, 12, 12, 210), width=7)
    # A few rain catches on the outer edges only.
    for _ in range(120):
        x = rng.choice([rng.randrange(0, 310), rng.randrange(1290, W)])
        y = rng.randrange(0, H)
        d.line((x, y, x - 3, y + rng.randrange(9, 25)), fill=(132, 170, 165, 25), width=1)
    return grain(vignette(im, 100), 392)


def save(name: str, im: Image.Image) -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    path = OUT / name
    im.save(path, format="JPEG", quality=82, optimize=True, progressive=True, subsampling=2)
    print(f"wrote {path.relative_to(ROOT)} ({path.stat().st_size:,} bytes)")


def main() -> None:
    save("city-rain.jpg", city())
    save("mountain-night.jpg", mountain())
    save("forest-mist.jpg", forest())


if __name__ == "__main__":
    main()
