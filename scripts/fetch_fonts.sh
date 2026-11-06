#!/usr/bin/env bash
# Fetches the four typefaces used by scripts/brand.py into .build/fonts.
#
# They are not committed: they are build inputs, not source. All four are
# licensed under the SIL Open Font License 1.1, which permits redistribution
# inside this repository's generated artwork.
#
# Requires: node + npm (only for @fontsource), python3 + fonttools + brotli,
# both installable with `pip install fonttools brotli`.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/.build/fonts"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$OUT"

echo "== fetching @fontsource packages =="
cd "$TMP"
npm init -y >/dev/null 2>&1
npm install --silent --no-audit --no-fund \
  @fontsource/instrument-serif \
  @fontsource/space-grotesk \
  @fontsource/jetbrains-mono \
  @fontsource/inter

PYTHON="${PYTHON:-python3}"
"$PYTHON" - "$TMP" "$OUT" <<'PY'
import os, sys
from fontTools.ttLib import TTFont

src, out = sys.argv[1], sys.argv[2]
mods = os.path.join(src, "node_modules", "@fontsource")

# (package, family file stem, weight, style, output name)
WANTED = [
    ("instrument-serif", "instrument-serif-latin-{weight}-{style}", 400, "normal",
     "InstrumentSerif-Regular.ttf"),
    ("instrument-serif", "instrument-serif-latin-{weight}-{style}", 400, "italic",
     "InstrumentSerif-Italic.ttf"),
    ("space-grotesk", "space-grotesk-latin-{weight}-{style}", 500, "normal",
     "SpaceGrotesk-Medium.ttf"),
    ("space-grotesk", "space-grotesk-latin-{weight}-{style}", 700, "normal",
     "SpaceGrotesk-Bold.ttf"),
    ("jetbrains-mono", "jetbrains-mono-latin-{weight}-{style}", 400, "normal",
     "JetBrainsMono-Regular.ttf"),
    ("jetbrains-mono", "jetbrains-mono-latin-{weight}-{style}", 700, "normal",
     "JetBrainsMono-Bold.ttf"),
    ("inter", "inter-latin-{weight}-{style}", 400, "normal", "Inter-Regular.ttf"),
    ("inter", "inter-latin-{weight}-{style}", 600, "normal", "Inter-SemiBold.ttf"),
]

for pkg, pattern, weight, style, name in WANTED:
    stem = pattern.format(weight=weight, style=style)
    woff2 = os.path.join(mods, pkg, "files", stem + ".woff2")
    if not os.path.exists(woff2):
        raise SystemExit(f"missing {woff2}")
    font = TTFont(woff2)
    font.flavor = None            # woff2 -> plain sfnt
    font.save(os.path.join(out, name))
    print(f"  {name}")
PY

# One copy of the licence text, covering all four families.
cp "$TMP/node_modules/@fontsource/instrument-serif/LICENSE" "$OUT/LICENSE" 2>/dev/null \
  || cp "$TMP/node_modules/@fontsource/inter/LICENSE" "$OUT/LICENSE"

echo "== fonts in .build/fonts =="
ls -1 "$OUT"
