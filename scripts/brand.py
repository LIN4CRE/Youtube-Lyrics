#!/usr/bin/env python3
"""Youtube-Lyrics brand artwork, drawn from code.

Every asset - the app icon, the exe icon, the GitHub banner, the pipeline
figure and the interface preview - comes out of this one file, from the same
mark, the same palette and the same fonts. Nothing here is stock art.

The mark
--------
A lyric sheet, not an equaliser: a vertical margin rule with three lines of
text beside it, and the line you are on lit amber with the karaoke wipe frozen
part-way across it. The bars are deliberately asymmetric, like real lines of a
song, and nothing glows or gradients at icon size.

Palette: ink, warm paper, one amber. Nothing else.

Fonts (all SIL Open Font License, fetched by scripts/fetch_fonts.sh):
  Instrument Serif - display serif for the wordmark
  Space Grotesk    - headings
  JetBrains Mono   - labels and numerals
  Inter            - interface type in the display preview

Usage:  python3 scripts/brand.py
"""
from __future__ import annotations

import os
import shutil
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

# --------------------------------------------------------------------- palette

INK = (11, 10, 15)
INK_SOFT = (20, 18, 25)
INK_CARD = (23, 21, 29)
PAPER = (243, 239, 230)
AMBER = (245, 165, 36)
MUTED = (139, 133, 120)

FONT_DIR = os.path.join(".build", "fonts")
BRAND_DIR = os.path.join("docs", "brand")

# ----------------------------------------------------------------------- utils


def font(name: str, size: int) -> ImageFont.FreeTypeFont:
    path = os.path.join(FONT_DIR, name)
    if not os.path.exists(path):
        raise SystemExit(f"font {path} missing - run scripts/fetch_fonts.sh first")
    return ImageFont.truetype(path, size)


def over(img: Image.Image, fn) -> Image.Image:
    """Draw on a transparent layer, then composite it.

    ImageDraw writes raw RGBA, so drawing a half-transparent colour straight
    onto a canvas punches a hole instead of blending. Everything that is not
    fully opaque goes through here.
    """
    lay = Image.new("RGBA", img.size, (0, 0, 0, 0))
    fn(ImageDraw.Draw(lay))
    return Image.alpha_composite(img, lay)


def spaced_text(draw, xy, text, fnt, fill, tracking=0.0):
    """Letter-spaced text - the small mono label look."""
    x, y = xy
    for ch in text:
        draw.text((x, y), ch, font=fnt, fill=fill)
        x += draw.textlength(ch, font=fnt) + tracking
    return x


def spaced_width(draw, text, fnt, tracking=0.0):
    if not text:
        return 0.0
    return sum(draw.textlength(c, font=fnt) for c in text) + tracking * (len(text) - 1)


def vertical_gradient(size, top, bottom) -> Image.Image:
    w, h = size
    strip = Image.new("RGB", (1, h))
    px = strip.load()
    for y in range(h):
        t = y / max(1, h - 1)
        px[0, y] = tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3))
    return strip.resize((w, h), Image.BILINEAR).convert("RGBA")


def radial_glow(size, centre, radius, colour, strength=1.0) -> Image.Image:
    """Soft radial lamp. Rendered small and upscaled - a cheap, smooth blur."""
    w, h = size
    sw, sh = max(2, w // 8), max(2, h // 8)
    mask = Image.new("L", (sw, sh), 0)
    px = mask.load()
    cx, cy = centre[0] / w * sw, centre[1] / h * sh
    r = max(1.0, radius / w * sw)
    for y in range(sh):
        for x in range(sw):
            d = ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5 / r
            v = max(0.0, 1.0 - d)
            px[x, y] = int(255 * v * v * strength)
    mask = mask.resize((w, h), Image.BICUBIC).filter(ImageFilter.GaussianBlur(w / 70))
    out = Image.new("RGBA", (w, h), colour + (0,))
    out.putalpha(mask)
    return out


# ------------------------------------------------------------------- the mark


def draw_mark(size: int, transparent: bool = False,
              border_alpha: int = 40) -> Image.Image:
    """The logo: a lyric sheet - ink lines on a warm paper tile.

    Paper, not ink, because every media-player icon on a dark taskbar is a dark
    squircle with a glowing shape in it; a printed sheet is the one thing that
    will not look like the others. The line you are on is the only colour.

    Small sizes are redrawn rather than scaled: at 16 px a hairline margin rule
    and 7 %-tall bars turn to mush, so strokes get proportionally heavier and
    the quiet lines come up to full ink.
    """
    ss = 4
    s = size * ss
    small = size <= 48
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))

    # The paper tile.
    if not transparent:
        tile = Image.new("RGBA", (s, s), (0, 0, 0, 0))
        ImageDraw.Draw(tile).rounded_rectangle(
            (0, 0, s - 1, s - 1), radius=int(s * 0.235), fill=PAPER + (255,))
        img = Image.alpha_composite(img, tile)
        # A hairline keeps the tile from bleeding into a white page.
        if not small:
            img = over(img, lambda d: d.rounded_rectangle(
                (s * 0.010, s * 0.010, s * 0.990, s * 0.990),
                radius=int(s * 0.228), outline=INK + (border_alpha,),
                width=max(1, int(s * 0.005))))

    # The sheet margin every line of the song hangs from.
    rule_x = s * (0.150 if small else 0.134)
    rule_w = s * (0.052 if small else 0.024)
    img = over(img, lambda d: d.rounded_rectangle(
        (rule_x, s * 0.215, rule_x + rule_w, s * 0.785), radius=rule_w / 2,
        fill=INK + (168 if small else 128,)))

    thin_h = s * (0.120 if small else 0.075)
    mid_h = s * (0.175 if small else 0.125)
    x0 = s * (0.268 if small else 0.240)
    # Quiet lines are printed ink, not pencil: at 105 alpha the tile read as
    # grey-on-cream and lost its lyric-sheet character at large sizes.
    thin_a = 190 if small else 150

    # (width, centre-y, height, colour) - None marks the line being sung.
    rows = [
        (0.350 if small else 0.340, 0.345, thin_h, INK + (thin_a,)),
        (0.580 if small else 0.600, 0.500, mid_h, None),
        (0.265 if small else 0.265, 0.655, thin_h, INK + (thin_a,)),
    ]
    for width_frac, cy, height, colour in rows:
        x1 = x0 + s * width_frac
        top, bottom = s * cy - height / 2, s * cy + height / 2
        if colour is None:
            def lit(d, x1=x1, top=top, bottom=bottom, height=height):
                # The whole line still to be sung, then the sung part in amber:
                # the karaoke wipe, caught mid-line.
                d.rounded_rectangle((x0, top, x1, bottom), radius=height / 2,
                                    fill=INK + (132,))
                d.rounded_rectangle((x0, top, x0 + (x1 - x0) * 0.66, bottom),
                                    radius=height / 2, fill=AMBER + (255,))
            img = over(img, lit)
        else:
            img = over(img, lambda d, x1=x1, top=top, bottom=bottom,
                       height=height, colour=colour: d.rounded_rectangle(
                     (x0, top, x1, bottom), radius=height / 2, fill=colour))

    return img.resize((size, size), Image.LANCZOS)


def _mark_svg() -> str:
    """Vector counterpart of draw_mark(), for crisp favicons and sharing."""
    return '''<g id="lyric-mark">
  <rect x="4" y="4" width="1016" height="1016" rx="240" fill="#f3efe6"
        stroke="#0b0a0f" stroke-opacity=".16" stroke-width="6"/>
  <rect x="137" y="220" width="24" height="584" rx="12"
        fill="#0b0a0f" fill-opacity=".502"/>
  <rect x="246" y="314" width="348" height="77" rx="38.5"
        fill="#0b0a0f" fill-opacity=".588"/>
  <rect x="246" y="448" width="614" height="128" rx="64"
        fill="#0b0a0f" fill-opacity=".518"/>
  <rect x="246" y="448" width="405" height="128" rx="64" fill="#f5a524"/>
  <rect x="246" y="632" width="271" height="77" rx="38.5"
        fill="#0b0a0f" fill-opacity=".588"/>
</g>'''


def build_vector_logos() -> None:
    """Write a reusable square mark and wordmark from the project's palette."""
    mark = _mark_svg()
    mark_svg = f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1024 1024"
  role="img" aria-labelledby="title desc">
  <title id="title">Youtube-Lyrics icon</title>
  <desc id="desc">A lyric sheet with one amber line highlighted mid-song.</desc>
  {mark}
</svg>
'''
    logo_svg = f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1480 300"
  role="img" aria-labelledby="title desc">
  <title id="title">Youtube-Lyrics</title>
  <desc id="desc">A lyric-sheet icon and the Youtube-Lyrics wordmark.</desc>
  <defs>{mark}</defs>
  <g transform="translate(24 24) scale(0.24609375)">
    <use href="#lyric-mark"/>
  </g>
  <text x="330" y="178" fill="#f3efe6"
        font-family="Instrument Serif, Georgia, serif" font-size="154"
        letter-spacing="-4">youtube</text>
  <text x="835" y="178" fill="#f5a524"
        font-family="Instrument Serif, Georgia, serif" font-size="154"
        font-style="italic" letter-spacing="-4">lyrics</text>
  <text x="340" y="240" fill="#8b8578"
        font-family="JetBrains Mono, Consolas, monospace" font-size="19"
        letter-spacing="3">FULL-SCREEN SYNCED LYRICS · WINDOWS 10 &amp; 11</text>
</svg>
'''
    with open(os.path.join(BRAND_DIR, "mark.svg"), "w", encoding="utf-8") as f:
        f.write(mark_svg)
    with open(os.path.join(BRAND_DIR, "logo.svg"), "w", encoding="utf-8") as f:
        f.write(logo_svg)
    print(f"wrote {os.path.join(BRAND_DIR, 'mark.svg')}")
    print(f"wrote {os.path.join(BRAND_DIR, 'logo.svg')}")


# ---------------------------------------------------------------------- banner


def build_banner(path: str, w: int = 2000, h: int = 640) -> None:
    """Two columns and one printed rule: the mark and wordmark on the left, a
    frozen frame of the display on the right.

    Drawn 2000 px wide because that is where it is read: GitHub renders the
    README at about 1012 px, so the small mono captions still land around 12 px
    instead of dissolving at 6.
    """
    img = vertical_gradient((w, h), (17, 15, 23), (9, 8, 12))
    img.alpha_composite(radial_glow((w, h), (150, 130), 820, AMBER, 0.09))
    img.alpha_composite(radial_glow((w, h), (1880, 780), 760, AMBER, 0.04))

    img = over(img, lambda d: d.rounded_rectangle(
        (22, 22, w - 22, h - 22), radius=16, outline=PAPER + (22,), width=2))

    split = 900
    img = over(img, lambda d: d.line([(split, 88), (split, h - 88)],
                                     fill=PAPER + (18,), width=2))

    # ---- left: mark, wordmark, one line of facts ----
    img.alpha_composite(draw_mark(136), (88, 92))

    serif = font("InstrumentSerif-Regular.ttf", 126)
    serif_it = font("InstrumentSerif-Italic.ttf", 126)
    d = ImageDraw.Draw(img)
    x, y = 86, 262
    d.text((x, y), "youtube", font=serif, fill=PAPER)
    x += d.textlength("youtube", font=serif) + 15
    d.text((x, y), "lyrics", font=serif_it, fill=AMBER)

    mono = font("JetBrainsMono-Regular.ttf", 24)
    spaced_text(d, (88, 470), "FULL-SCREEN SYNCED LYRICS", mono, (166, 159, 144), 2.6)
    mono_s = font("JetBrainsMono-Regular.ttf", 21)
    spaced_text(d, (88, 508), "FOR WINDOWS 10 AND 11", mono_s, (122, 116, 104), 2.2)
    mono_x = font("JetBrainsMono-Regular.ttf", 17)
    spaced_text(d, (88, 556), "ONE .EXE \u00b7 NO INSTALLER \u00b7 NO ACCOUNT",
                mono_x, (98, 93, 84), 1.7)

    # ---- right: a frozen frame of the display ----
    card = (956, 68, w - 68, h - 68)
    img = over(img, lambda d: d.rounded_rectangle(
        card, radius=18, fill=INK_CARD + (255,), outline=PAPER + (18,), width=2))
    cx = (card[0] + card[2]) / 2          # everything inside is centred, like the app

    d = ImageDraw.Draw(img)
    tw = d.textlength("Amber Fields", font=font("Inter-Regular.ttf", 19))
    d.text((cx - tw / 2, 112), "Amber Fields", font=font("Inter-Regular.ttf", 19),
           fill=(160, 153, 139) + (255,))
    tw = d.textlength("The Long Way Round", font=font("Inter-SemiBold.ttf", 27))
    d.text((cx - tw / 2, 140), "The Long Way Round",
           font=font("Inter-SemiBold.ttf", 27), fill=(233, 227, 215) + (255,))

    f_line = font("Inter-Regular.ttf", 34)
    f_act = font("Inter-SemiBold.ttf", 40)
    rows = [
        ("every light along the motorway", 146),
        ("counts the minutes back to you", 146),
        ("and the line i am on finally", None),
        ("burns a little brighter now", 146),
    ]
    y = 218
    for text, alpha in rows:
        if alpha is None:
            tw = d.textlength(text, font=f_act)
            x = cx - tw / 2
            img.alpha_composite(radial_glow((w, h), (cx, y + 28), 460, AMBER, 0.14))
            d = ImageDraw.Draw(img)
            d.text((x, y), text, font=f_act, fill=PAPER + (140,))
            clipped = Image.new("RGBA", (w, h), (0, 0, 0, 0))
            ImageDraw.Draw(clipped).text((x, y), text, font=f_act, fill=AMBER + (255,))
            mask = Image.new("L", (w, h), 0)
            ImageDraw.Draw(mask).rectangle((x - 8, y - 20, x + tw * 0.68, y + 64),
                                           fill=255)
            clipped.putalpha(Image.composite(clipped.getchannel("A"),
                                             Image.new("L", (w, h), 0), mask))
            img.alpha_composite(clipped)
            d = ImageDraw.Draw(img)
            y += 78
        else:
            tw = d.textlength(text, font=f_line)
            d.text((cx - tw / 2, y), text, font=f_line, fill=PAPER + (alpha,))
            y += 58

    # progress + captions
    track_w = 780
    tx = cx - track_w / 2
    img = over(img, lambda d: d.rounded_rectangle(
        (tx, h - 106, tx + track_w, h - 103), radius=2, fill=PAPER + (24,)))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((tx, h - 106, tx + track_w * 0.41, h - 103), radius=2,
                        fill=AMBER + (255,))
    dot = tx + track_w * 0.41
    d.ellipse((dot - 3, h - 109, dot + 4, h - 100), fill=PAPER + (255,))
    mono_t = font("JetBrainsMono-Regular.ttf", 17)
    d.text((tx, h - 88), "1:28 / 3:36", font=mono_t, fill=(124, 118, 106) + (255,))
    lw = d.textlength("LRCLIB", font=mono_t)
    d.text((tx + track_w - lw, h - 88), "LRCLIB", font=mono_t,
           fill=(96, 92, 83) + (255,))

    img.convert("RGB").save(path, optimize=True)
    print(f"wrote {path}")


# ---------------------------------------------------------------- badges


def build_badge(path: str, label: str, value: str) -> tuple[int, int]:
    """A pill badge in the brand palette, drawn at 2x and displayed at 1x.

    Rendered from the same mono font the rest of the artwork uses, so the badge
    cannot drift from the banner. PNG rather than SVG because it is checked by
    eye here: whatever ships has been looked at.
    """
    scale = 2
    h = 26 * scale
    pad = 12 * scale
    gap = 14 * scale
    f = font("JetBrainsMono-Regular.ttf", 11 * scale)
    d = ImageDraw.Draw(Image.new("RGB", (1, 1)))

    def tracked(text: str) -> float:
        return (sum(d.textlength(c, font=f) for c in text)
                + 0.9 * scale * (len(text) - 1))

    lw, vw = tracked(label.upper()), tracked(value.upper())
    total = int(pad + lw + gap + vw + pad)
    img = Image.new("RGBA", (total, h), (0, 0, 0, 0))

    img = over(img, lambda d: d.rounded_rectangle(
        (0, 0, total - 1, h - 1), radius=7 * scale, fill=INK_CARD + (255,),
        outline=PAPER + (42,), width=2))
    # amber eyebrow rule at the left edge
    img = over(img, lambda d: d.rounded_rectangle(
        (0, 5 * scale, 2 * scale, h - 5 * scale), radius=2 * scale,
        fill=AMBER + (255,)))

    d = ImageDraw.Draw(img)
    spaced_text(d, (pad, 8 * scale), label.upper(), f, (168, 161, 147) + (255,),
                0.9 * scale)
    spaced_text(d, (pad + lw + gap, 8 * scale), value.upper(), f,
                AMBER + (255,), 0.9 * scale)

    img.convert("RGB").save(path, optimize=True)
    print(f"wrote {path}")
    return (total // scale, h // scale)


# ---------------------------------------------------------------- flow figure


def build_flow(path: str, w: int = 1200, h: int = 208) -> None:
    """Four steps, hairline arrows, no clip art."""
    img = vertical_gradient((w, h), (16, 14, 21), (10, 9, 13))
    img = over(img, lambda d: d.rounded_rectangle(
        (10, 10, w - 10, h - 10), radius=12, outline=PAPER + (22,), width=1))

    steps = [
        ("01", "play something", "youtube music, spotify,\nanything with media keys"),
        ("02", "windows session", "title, artist, playhead.\nread locally, not recorded"),
        ("03", "match the lyrics", "LRCLIB + lyrics.ovh,\nvalidated before display"),
        ("04", "desktop wallpaper", "behind icons, click-through\none lit line follows along"),
    ]
    margin = 40
    col_w = (w - 2 * margin) / len(steps)
    mono_num = font("JetBrainsMono-Bold.ttf", 14)
    f_sub = font("JetBrainsMono-Regular.ttf", 13)
    f_title = font("SpaceGrotesk-Medium.ttf", 22)
    gap = 30

    d = ImageDraw.Draw(img)
    for i, (num, title, sub) in enumerate(steps):
        x = margin + i * col_w
        spaced_text(d, (x, 36), num, mono_num, AMBER, 1.6)
        d.text((x, 58), title, font=f_title, fill=PAPER)
        d.multiline_text((x, 96), sub, font=f_sub, fill=MUTED, spacing=7)
        if i < len(steps) - 1:
            ax = x + col_w - gap
            ay = 68
            img = over(img, lambda d, ax=ax, ay=ay: d.line(
                [(ax, ay), (ax + 22, ay)], fill=PAPER + (70,), width=1))
            d = ImageDraw.Draw(img)
            d.line([(ax + 16, ay - 4), (ax + 22, ay)], fill=PAPER + (70,), width=1)
            d.line([(ax + 16, ay + 4), (ax + 22, ay)], fill=PAPER + (70,), width=1)

    img.convert("RGB").save(path, optimize=True)
    print(f"wrote {path}")


# ------------------------------------------------------- interface preview art


def build_display(path: str, w: int = 2560, h: int = 1440) -> None:
    """The full-screen display, using the renderer's own constants
    (src/win/render.cc): base type h * 0.042, active line x1.22, column
    min(w * 0.80, 1400), active row anchored at 0.42 * h, 3 px progress track
    on the bottom edge."""
    img = vertical_gradient((w, h), (13, 12, 18), (23, 19, 29))
    # The same three-layer background the renderer caches: the selected still,
    # a theme-coloured haze, then a vignette and the readability scrims.
    # Framed like the app frames it (aspect fill, centred), then pushed down so
    # the skyline sits behind the lower half of the lyric stack instead of
    # directly behind the first line.
    still = Image.open(os.path.join("src", "win", "wallpapers", "mountain-night.jpg"))
    still = still.convert("RGB").resize((w, h), Image.LANCZOS)
    tall = still.crop((0, 0, w, h - int(h * 0.06)))
    still = Image.new("RGB", (w, h))
    still.paste(tall, (0, int(h * 0.06)))
    img = Image.blend(img.convert("RGB"), still, 0.82).convert("RGBA")
    img = Image.composite(img, img.convert("RGB").point(lambda v: int(v * 0.86)).convert("RGBA"),
                          Image.new("L", (w, h), 255))
    img.alpha_composite(radial_glow((w, h), (w * 0.18, h * 0.16), h * 0.55, AMBER, 0.13))
    img.alpha_composite(radial_glow((w, h), (w * 0.86, h * 0.88), h * 0.50, AMBER, 0.075))

    # Vignette: an ellipse of clear centre over a darkened surround.
    vig = Image.new("L", (w, h), 0)
    ImageDraw.Draw(vig).ellipse((-w * 0.18, -h * 0.28, w * 1.18, h * 1.28), fill=255)
    vig = vig.filter(ImageFilter.GaussianBlur(w / 26))
    shade = Image.new("RGBA", (w, h), (0, 0, 0, 160))
    img = Image.composite(img, Image.alpha_composite(img, shade), vig)

    # Readability scrims: the renderer darkens the top and bottom 15-16 % of the
    # frame so chrome text never sits on busy artwork.
    scrim = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    sd = ImageDraw.Draw(scrim)
    for y in range(int(h * 0.20)):
        sd.line((0, y, w, y), fill=(0, 0, 0, int(96 * (1 - y / (h * 0.20)))))
    for y in range(int(h * 0.84), h):
        sd.line((0, y, w, y), fill=(0, 0, 0, int(120 * (y - h * 0.84) / (h * 0.16))))
    # The art is still legible behind the words, so the scrim is built once and
    # then flattened, not laid over the lyric text.
    img = Image.alpha_composite(img, scrim)

    d = ImageDraw.Draw(img)

    # header
    f_artist = font("Inter-Regular.ttf", 30)
    f_track = font("Inter-SemiBold.ttf", 40)
    tw = d.textlength("Amber Fields", font=f_artist)
    d.text(((w - tw) / 2, h * 0.026), "Amber Fields", font=f_artist,
           fill=(168, 160, 146) + (255,))
    tw = d.textlength("The Long Way Round", font=f_track)
    d.text(((w - tw) / 2, h * 0.026 + 44), "The Long Way Round", font=f_track,
           fill=(232, 226, 214) + (255,))

    base = min(max(h * 0.042, 16.0), 88.0)      # 60 px at 1440
    act = base * 1.22
    box = base * 1.5
    col_w = min(w * 0.80, 1400.0)
    col_x = (w - col_w) / 2

    lines = [
        "every light along the motorway",
        "counts the minutes back to you",
        "and i keep the radio low",
        "just in case you call",
        "and the line i am on finally",
        "burns a little brighter now",
        "so sing it with me",
        "we are not lost, we are just far",
    ]
    active = 3
    f_line = font("Inter-Regular.ttf", int(base))
    f_active = font("Inter-SemiBold.ttf", int(act))

    anchor_y = h * 0.42 - box / 2
    first_top = anchor_y - active * box

    # Pacing: the renderer adds half a line of breathing room after a long gap,
    # which is why a real chorus is not an evenly spaced block of text.
    gap_after = {1: 0.55, 4: 0.55}
    offsets = []
    running = 0.0
    for i in range(len(lines)):
        offsets.append(running)
        running += box + box * gap_after.get(i, 0.0)

    for i, text in enumerate(lines):
        # The active row is anchored at 0.42 * h and the block grows around it;
        # rows that would collide with the header or the progress bar are simply
        # not drawn, exactly as the renderer skips off-screen rows.
        y = h * 0.42 - box / 2 + (offsets[i] - offsets[active])
        if y < h * 0.105 or y > h * 0.90:
            continue
        if i == active:
            tw = d.textlength(text, font=f_active)
            x = col_x + (col_w - tw) / 2
            img.alpha_composite(radial_glow((w, h), (w / 2, y + box / 2), 760, AMBER, 0.16))
            d = ImageDraw.Draw(img)
            # unsung remainder, then the sung part in amber
            d.text((x, y + (box - act) / 2), text, font=f_active, fill=PAPER + (102,))
            clipped = Image.new("RGBA", (w, h), (0, 0, 0, 0))
            ImageDraw.Draw(clipped).text((x, y + (box - act) / 2), text,
                                         font=f_active, fill=AMBER + (255,))
            mask = Image.new("L", (w, h), 0)
            ImageDraw.Draw(mask).rectangle(
                (x - 10, y - 40, x + tw * 0.58, y + box + 40), fill=255)
            clipped.putalpha(Image.composite(clipped.getchannel("A"),
                                             Image.new("L", (w, h), 0), mask))
            img.alpha_composite(clipped)
            d = ImageDraw.Draw(img)
        else:
            alpha = int(max(52, 208 - abs(i - active) * 30))
            tw = d.textlength(text, font=f_line)
            d.text((col_x + (col_w - tw) / 2, y + (box - base) / 2), text,
                   font=f_line, fill=PAPER + (alpha,))

    # progress bar: 3 px track on the very bottom edge, as the renderer draws it
    d = ImageDraw.Draw(img)
    d.rectangle((0, h - 4, w, h - 2), fill=PAPER + (46,))
    d.rectangle((0, h - 4, w * 0.41, h - 2), fill=AMBER)
    d.ellipse((w * 0.41 - 5, h - 7, w * 0.41 + 5, h + 2), fill=PAPER)

    mono = font("JetBrainsMono-Regular.ttf", 22)
    times = "1:28 / 3:36"
    d.text((w - 26 - d.textlength(times, font=mono), h - 54), times, font=mono,
           fill=PAPER + (144,))
    d.text((26, h - 54),
           "via chrome.exe  ·  lyrics: LRCLIB  ·  Enhanced LRC",
           font=mono, fill=PAPER + (92,))

    img.convert("RGB").save(path, optimize=True)
    print(f"wrote {path}")


# ----------------------------------------------------------------------- main


def read_version() -> str:
    return Path("VERSION").read_text(encoding="ascii").strip()


def read_download_size() -> str:
    """Size of the shipped executable, in KiB, read from the file itself.

    The download badge used to hard-code "1.4.0 · 842 KiB" and drifted the
    moment the binary changed. If the executable does not exist yet (a fresh
    checkout before the first build) the badge says so instead of lying.
    """
    exe = Path("dist/YoutubeLyrics.exe")
    if not exe.is_file():
        return "build pending"
    kib = exe.stat().st_size / 1024.0
    return f"{kib:.0f} KiB"


def main() -> int:
    os.makedirs(BRAND_DIR, exist_ok=True)
    os.makedirs(os.path.join("docs", "img"), exist_ok=True)
    build_vector_logos()

    icon_master = draw_mark(1024)
    icon_master.save(os.path.join(BRAND_DIR, "icon.png"), optimize=True)
    icon_master.save("icon.png", optimize=True)
    print(f"wrote {os.path.join(BRAND_DIR, 'icon.png')} and icon.png")

    # Same mark, square, for the GitHub repo avatar / About panel.
    draw_mark(460).save(os.path.join(BRAND_DIR, "avatar.png"), optimize=True)
    print(f"wrote {os.path.join(BRAND_DIR, 'avatar.png')}")

    # The exe icon is built from this by scripts/make_icon.py.
    draw_mark(512).save(".build/app-icon-512.png", optimize=True)

    build_banner(os.path.join(BRAND_DIR, "banner.png"))
    shutil.copyfile(os.path.join(BRAND_DIR, "banner.png"), "banner.png")
    build_badge(os.path.join(BRAND_DIR, "badge-download.png"),
                "download", f"{read_version()} \u00b7 {read_download_size()}")
    build_badge(os.path.join(BRAND_DIR, "badge-support.png"),
                "help me out", "paypal")
    build_badge(os.path.join(BRAND_DIR, "badge-platform.png"),
                "runs on", "windows 10 / 11")
    build_badge(os.path.join(BRAND_DIR, "badge-licence.png"), "licence", "MIT")
    build_flow(os.path.join(BRAND_DIR, "pipeline.png"))
    build_display(os.path.join("docs", "img", "display.png"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
