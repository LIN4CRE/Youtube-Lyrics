<div align="center">

<img src="docs/brand/banner.png" alt="Youtube-Lyrics — full-screen synced lyrics for Windows 10 and 11" width="100%">

<br>

**YouTube Music gives you a thin strip of small text at the side of the screen.**
**This gives you the whole screen, one lit line at a time.**

A single 640 KB executable that sits in the tray, listens to whatever Windows is
playing, and paints the lyrics full-screen — karaoke style.

[![Download](docs/brand/badge-download.png)][releases]
[![Windows 10 / 11](docs/brand/badge-platform.png)](#requirements)
[![MIT](docs/brand/badge-licence.png)](LICENSE)

[**Download**][releases] · [See it running](docs/preview.html) · [Shortcuts](#keyboard) · [Troubleshooting](#if-something-is-off)

</div>

<img src="docs/img/display.png" alt="The full-screen display: the current line large and lit in amber, the sung words filled in, surrounding lines fading out." width="100%">

<div align="center"><sub>The actual display, 2560 × 1440. No window chrome, no browser, no second screen required.</sub></div>

<br>

## Why this exists

YouTube Music's lyrics panel has three problems: it is a narrow column on the
right, the text is small, and **nothing tells you which line you are on**. If you
are singing along — or learning a song in another language, or playing live —
that is the only information you actually need.

Youtube-Lyrics is not a YouTube Music mod, extension, or wrapper. It reads the
Windows media session (the same metadata behind the volume-key overlay) and draws
its own display, so it works with any player that reports to Windows, and it
never touches the browser.

<br>

## What it does

| | |
| --- | --- |
| **The line you are on, lit** | The active line is 22 % larger, bright, and has a soft amber spotlight behind it. Lines fade with distance so the eye lands in the right place instantly. |
| **Karaoke wipe** | The sung part of each line fills in with colour, word by word, as the song plays — so you know where you are *inside* the line, not just which line. |
| **Type you can read from the sofa** | Base size is 4.2 % of screen height and the column is capped at 1400 px. If a line would overflow, the whole layout steps down to fit rather than clipping. |
| **Smooth, not twitchy** | Lines scroll on a spring, so a fast chorus glides instead of jolting. Gaps of 3 s or more get extra breathing room, so verses and choruses separate themselves. |
| **Four themes, one of them honest** | Midnight (ink and amber, the default), Aurora, Ember, Mono. `T` cycles; the choice is remembered. |
| **Cover art, if you want it** | The album cover, aspect-filled and heavily darkened behind the text. `C` toggles. |
| **Out of the way** | Runs in the tray, `Ctrl+Alt+L` from anywhere, always-on-top on a second monitor, `Esc` to get your screen back. |
| **Offline after the first play** | Lyrics are cached per track on disk. Replay a song on a plane and it still works. |

<br>

## Install

1. **[Download `YoutubeLyrics.exe`][releases]** — one file, about 640 KB.
2. Put it anywhere: Desktop, `%LOCALAPPDATA%`, a USB stick. It is portable and
   writes nothing outside its own folder and `%LOCALAPPDATA%\YouTubeLyrics`.
3. Double-click it, then play something.

Windows will show **"Windows protected your PC"** the first time, because the
file is not code-signed (a certificate costs more per year than this project
costs in total). Click **More info → Run anyway**. No administrator rights, no
installer, no services, no registry changes, no account.

<a id="requirements"></a>

**Requirements:** Windows 10 (1809 or newer) or Windows 11, 64-bit. Nothing else
— no .NET, no runtime, no Python. The lyric listener uses the PowerShell that
ships with Windows.

<br>

## Using it

`Esc` leaves full screen; `Esc` again hides to the tray. `Ctrl+Alt+L` brings it
back from any app. `F1` shows the shortcut sheet over the lyrics, along with the
raw state of the media session, which is the first thing to look at if
something seems stuck.

<a id="keyboard"></a>

| Key | Action | | Key | Action |
| :--- | :--- | :--- | :--- | :--- |
| `F11` | Full screen ⇄ windowed | | `F` | Reset text size |
| `Esc` | Leave full screen, then hide to tray | | `R` | Look the lyrics up again |
| `Ctrl`+`Alt`+`L` | Show / hide from any app | | `C` | Cover-art background |
| `A` | Always on top | | `K` | Karaoke wipe |
| `↑` `↓` | Bigger / smaller text | | `T` | Theme |
| `←` `→` | Shift lyric timing ±0.25 s | | `F1` `H` | Help overlay |
| | | | `Q` | Quit |

Double-clicking toggles full screen. Everything above is also in the tray
icon's right-click menu, and every setting is saved as you change it.

<br>

## Where the lyrics come from

[LRCLIB][lrclib] — a free, open, community-maintained database of time-synced
lyrics. When a track starts, the app sends its title, artist, album and duration
and gets back an LRC file with a timestamp for every line. That is the entire
network conversation: no account, no identifier, no telemetry, and nothing is
ever uploaded. Responses are cached on disk, so a track only costs one request,
ever.

If a track is not in the database you get a clear "no lyrics found" message
instead of a blank screen, and `R` retries.

<br>

<h2 align="center">How it works</h2>

<img src="docs/brand/pipeline.png" alt="Pipeline: you press play; Windows media session reports title, artist and playhead; LRCLIB returns synced LRC; the display draws one lit line." width="100%">

<div align="center"><sub>Nothing is recorded. The app never touches your audio — only the metadata Windows already publishes.</sub></div>

<br>

```
src/core/          portable logic: LRC parsing, JSON, track-name cleaning,
                   lyric layout and scroll maths  →  tests/core_test.cc
src/win/           Win32 window, tray, GDI+ renderer, WinHTTP, media listener
src/win/smtc.ps1   the PowerShell media-session listener (embedded in the exe)
scripts/build.sh   cross-compiles to dist/YoutubeLyrics.exe
scripts/brand.py   draws the icon, banner and preview art from code
scripts/test.sh    builds and runs the core tests under ASan/UBSan
docs/preview.html  a browser mock of the display, for looking at it here
```

The portable half is deliberately free of Windows headers and has its own test
suite — 3,192 assertions covering LRC parsing, timing, layout and scroll
behaviour — so lyric maths can be checked on any machine, not just a Windows
one.

```bash
./scripts/test.sh     # core tests (ASan + UBSan), no Windows needed
./scripts/build.sh    # -> dist/YoutubeLyrics.exe
```

The only build dependency is [Zig][zig]: it bundles the MinGW-w64 toolchain and
its resource compiler, so the exe cross-compiles from Linux or macOS exactly as
it does on Windows. Icon and artwork generation additionally need Pillow and
`fonttools` (`pip install pillow fonttools brotli`, then `scripts/fetch_fonts.sh`).

<br>

## If something is off

<details>
<summary><b>It says "waiting for music" while something is clearly playing</b></summary>

<br>

The app reads Windows' media session, so the player must publish one. Browsers do
this while a tab is actually producing audio — a paused tab does not count. Press
`F1`: it shows the raw state the listener reported, including whether any session
was found at all. Some players (a few portable builds, older foobar2000) never
publish a session and cannot be supported.

</details>

<details>
<summary><b>No lyrics for a track I can see in YouTube Music</b></summary>

<br>

Press `F1` to see the exact title and artist the app detected. YouTube Music
sometimes reports `Artist - Topic`, `Song (Official Video)` or a remaster suffix;
the app strips those, then tries a fuzzy search, then gives up rather than showing
the wrong words. `R` retries.

</details>

<details>
<summary><b>The words are consistently early or late</b></summary>

<br>

Some players report the playhead imprecisely, and some lyric files are just off
against a particular recording. Nudge with `←` `→`; the offset is saved per app
and can be reset from the tray menu. If a player publishes no playhead at all,
the app runs its own clock from the moment the track started, which is usually
within a second.

</details>

<details>
<summary><b>Nothing happens at all, or it exits immediately</b></summary>

<br>

Read `%LOCALAPPDATA%\YouTubeLyrics\ytlyrics.log`. Cached lyrics live beside it in
`cache\`; deleting that folder is always safe and costs you nothing but a
re-download.

</details>

<details>
<summary><b>I want to see exactly what the listener hears</b></summary>

<br>

Run this in a terminal while a song plays:

```bat
powershell -ExecutionPolicy Bypass -File "%LOCALAPPDATA%\YouTubeLyrics\smtc.ps1"
```

It prints one JSON line every 400 ms with status, title, artist and playhead. This
is also the fastest way to tell whether a playback problem is in the app or in
Windows' media session.

</details>

<details>
<summary><b>A corporate policy blocks PowerShell</b></summary>

<br>

The listener is a small PowerShell script using the WinRT media-session API.
There is no packaged alternative that works in a single unsigned executable
without bundling a runtime, so if PowerShell is disabled by policy, listening
will not work. The display itself still runs.

</details>

<br>

## Licence

MIT — do what you like with it. The lyrics themselves belong to their writers
and publishers: they are fetched from LRCLIB at runtime and never bundled here.
Typefaces in the artwork are under the SIL Open Font License (Instrument Serif,
Space Grotesk, JetBrains Mono, Inter).

<div align="center">
<br>
<sub>Built for people who would rather look at the song than at a column.</sub>
</div>

[releases]: https://github.com/LIN4CRE/Youtube-Lyrics/releases/latest
[lrclib]: https://lrclib.net
[zig]: https://ziglang.org
