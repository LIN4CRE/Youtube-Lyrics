# Built-in wallpapers

`city-rain.jpg`, `mountain-night.jpg`, and `forest-mist.jpg` are original procedural illustrations made specifically for Youtube-Lyrics by `scripts/wallpapers.py`. They are generated locally from code; the script downloads no imagery and uses no third-party photographs. The assets are distributed under the repository's MIT licence.

The 1600×900 JPEGs are intentionally dark and restrained so lyric text and desktop icons remain legible. The executable embeds the compressed files and decodes only the selected wallpaper. It retains one size-limited background cache rather than keeping a decoded copy of every wallpaper in memory.

Regenerate with Python 3 and Pillow:

```sh
PYTHONPATH=.cache/pydeps python3 scripts/wallpapers.py
```
