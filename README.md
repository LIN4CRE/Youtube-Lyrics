<div align="center">

<img src="docs/brand/banner.png" alt="Youtube-Lyrics — full-screen desktop lyrics for Windows 10 and 11" width="100%">

<br>

**YouTube Music gives you a thin strip of small text at the side of the screen.**
**This gives you the whole screen, one lit line at a time.**

A single Windows executable that lives behind your desktop icons and follows
Windows' current music session, prioritizing what is playing while retaining its
current paused track. It never takes focus or catches clicks; the desktop stays
usable while lyrics sit quietly in the background. LRCLIB search results are
matched before display; lyrics.ovh is an untimed fallback without response-side
track metadata.

[![Download](docs/brand/badge-download.png)][download]
[![Windows 10 / 11](docs/brand/badge-platform.png)](#requirements)
[![MIT](docs/brand/badge-licence.png)](LICENSE)

[**Download YoutubeLyrics.exe**][download] · [Interactive design preview (simulated)](docs/preview.html) · [Shortcuts](#keyboard) · [Troubleshooting](#if-something-is-off)

</div>

<img src="docs/img/display.png" alt="Illustrative render of the lyrics layout: one amber-highlighted line with surrounding lines fading out." width="100%">

<div align="center"><sub>Illustrative design render generated from the project's artwork — not a screenshot of the running Windows application. No runtime screenshot is claimed.</sub></div>

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
| **Timing shown honestly** | Genuine Enhanced LRC timestamps fill timed word/lyric segments. Ordinary LRC gets a continuous line-level wipe. Untimed text can use estimated line navigation, clearly labeled as estimated, with no fake karaoke wipe. |
| **Type you can read from the sofa** | Base size is 4.2 % of screen height and the column is capped at 1400 px. If a line would overflow, the whole layout steps down to fit rather than clipping. |
| **Smooth, not twitchy** | Lines scroll on a spring, so a fast chorus glides instead of jolting. Gaps of 3 s or more get extra breathing room, so verses and choruses separate themselves. |
| **Four themes, one of them honest** | Midnight (ink and amber, the default), Aurora, Ember, Mono. `T` cycles; the choice is remembered. |
| **Cover art, if you want it** | The album cover, aspect-filled and heavily darkened behind the text. `C` toggles. |
| **A real desktop layer** | Sits behind desktop icons and ordinary windows, without taking focus or intercepting clicks. `Ctrl+Alt+W` switches wallpaper mode; `Ctrl+Alt+L` hides or restores it. |
| **The right media session** | A playing session takes priority; Windows' current known music session remains visible while paused, with its position frozen. Stopped sessions are ignored, track changes are confirmed across polls, and lyrics are checked against title, artist and duration. |
| **Out of the way** | Runs in the tray; use the global hotkeys or the tray menu. Turn wallpaper mode off to use the normal full-screen/windowed display. |
| **Offline after the first play** | Lyrics are cached per track on disk. Replay a song on a plane and it still works. |

<br>

## Install

1. **[Download `YoutubeLyrics.exe`][download]** — one file, about 555 KiB.
2. Put it anywhere: Desktop, `%LOCALAPPDATA%`, a USB stick. It is portable and
   writes nothing outside its own folder and `%LOCALAPPDATA%\YouTubeLyrics`.
3. Double-click it, then play something. No terminal or installer is needed.

Windows may show **"Windows protected your PC"** because the file is not
code-signed. The source is public; continue only if you trust the download. If
you choose to proceed, select **More info → Run anyway**. No administrator
rights, services, or registry changes are required.

<a id="requirements"></a>

**Requirements:** Windows 10 (1809 or newer) or Windows 11, 64-bit. Nothing else
— no .NET, no runtime, no Python. The lyric listener uses the PowerShell that
ships with Windows.

<br>

## Using it

The default is desktop-wallpaper mode: the display stays behind desktop icons,
never takes focus, and passes clicks through. `Ctrl+Alt+W` switches between that
and the normal full-screen display; `Ctrl+Alt+L` hides or restores it from any
app. In normal display mode, `F11` toggles full screen/windowed and `Esc` leaves
full screen, then hides to the tray. `F1` shows shortcuts and the raw media-session
state when the window is focused.

<a id="keyboard"></a>

| Key | Action | | Key | Action |
| :--- | :--- | :--- | :--- | :--- |
| `Ctrl`+`Alt`+`W` | Toggle desktop wallpaper mode | | `F` | Reset text size |
| `Ctrl`+`Alt`+`L` | Show / hide from any app | | `R` | Look the lyrics up again |
| `F11` | Full screen ⇄ windowed (normal mode) | | `C` | Cover-art background |
| `Esc` | Leave full screen, then hide to tray | | `K` | Karaoke wipe |
| `A` | Always on top (normal mode) | | `Q` | Quit |
| `↑` `↓` | Bigger / smaller text | | `T` | Theme |
| `←` `→` | Shift lyric timing ±0.25 s | | `F1` `H` | Help overlay |

Double-clicking toggles full screen. Everything above is also in the tray
icon's right-click menu, and every setting is saved as you change it.

<br>

## Where the lyrics come from

The app tries [LRCLIB][lrclib] for an exact match, then broader and title-only
searches; those candidate results are scored before display. It then asks
lyrics.ovh for an untimed fallback. Standard LRC
provides line timestamps, so its active line gets a continuous line-level wipe.
When a source supplies Enhanced LRC inline timestamps, those genuine segment
times drive the finer wipe. Untimed lyrics may be paced across the reported song
length to aid navigation; the display labels that as **estimated scroll** and
does not represent it as source synchronization.

LRCLIB search results are scored against the detected title, artist and
available duration; weak matches are rejected. The lyrics.ovh fallback is
queried with the detected artist/title, but its response contains no track
identity metadata, so it cannot be independently verified and is a lower-confidence,
untimed source. Automatic results are cached on disk. For songs neither source
has (or when a result is wrong), add an
`.lrc` or `.txt` file under `%LOCALAPPDATA%\YouTubeLyrics\lyrics\` named
`Artist - Title.lrc` (or `.txt`). The tray menu can open that folder or clear the
automatic lyrics cache; local sheets take priority and are not copied into the
automatic cache. No account, audio capture, or telemetry is used. Lookups send
the detected title and artist; LRCLIB's exact-match request also includes album
and duration when Windows provides them. If no source has the song, `R` retries.

<br>

<h2 align="center">How it works</h2>

<img src="docs/brand/pipeline.png" alt="Pipeline: Windows reports the playing track; LRCLIB, lyrics.ovh, or a local sheet supplies lyrics; the display shows one lit line." width="100%">

<div align="center"><sub>Nothing is recorded. The app never touches your audio — only the metadata Windows already publishes.</sub></div>

<br>

```
src/core/          portable logic: LRC parsing, JSON, track-name cleaning,
                   lyric layout and scroll maths  →  tests/core_test.cc
src/win/           Win32 window, tray, GDI+ renderer, WinHTTP, media listener
src/win/smtc.ps1   the PowerShell media-session listener (embedded in the exe)
scripts/build.sh   cross-compiles to dist/YoutubeLyrics.exe
docs/index.html    responsive no-terminal download page for GitHub Pages
AUDIT.md           engineering audit, verification, release status and next steps
docs/downloads/    direct download of the current Windows executable
scripts/brand.py   draws the icon, banner and preview art from code
scripts/test.sh    builds and runs the core tests under ASan/UBSan
docs/preview.html  an interactive browser mock of the display
```

The portable half is deliberately free of Windows headers and has its own test
suite covering LRC parsing, track matching, wrong-song rejection, lyric timing,
layout and scroll behaviour — so lyric maths can be checked on any machine, not
just a Windows one.

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

The app reads Windows' media session. Playing sessions take priority; when
nothing is playing, the current known music session can remain visible while
paused so lyrics do not disappear immediately. A stopped/closed session is
ignored, and a paused non-current tab cannot mask another playing session. Press
`F1` in normal display mode to see the raw state the listener reported. Some
players (a few portable builds, older foobar2000) never publish a session and
cannot be supported.

</details>

<details>
<summary><b>No lyrics for a track I can see in YouTube Music</b></summary>

<br>

Press `F1` in normal display mode to see the exact title and artist the app
detected. YouTube Music sometimes reports `Artist - Topic` or a video suffix;
the app cleans the noise and scores LRCLIB results against the track metadata.
The lyrics.ovh fallback does not return identity metadata, so it cannot be
independently checked; if its result is wrong, add a local `.lrc`/`.txt`
override under `%LOCALAPPDATA%\YouTubeLyrics\lyrics\`. `R` retries.

</details>

<details>
<summary><b>The words are consistently early or late</b></summary>

<br>

Some players report the playhead imprecisely, and some lyric files are just off
against a particular recording. Nudge with `←` `→`; the single sync offset is
saved globally (it is not per-app or per-track) and can be reset from the tray
menu. If a player publishes no playhead at all, the app runs its own clock from
the moment the track started, which is usually within a second. Untimed lyric
scroll is separately labeled as an estimate.

</details>

<details>
<summary><b>Nothing happens at all, or it exits immediately</b></summary>

<br>

Read `%LOCALAPPDATA%\YouTubeLyrics\ytlyrics.log`. Automatic cached lyrics live
beside it in `cache\`; local overrides go in `lyrics\`. Use the tray menu to
open the overrides folder or clear the automatic cache; deleting that cache
costs nothing but a re-download.

</details>

<details>
<summary><b>I want to see exactly what the listener hears</b></summary>

<br>

Run this in a terminal while a song plays:

```bat
powershell -ExecutionPolicy Bypass -File "%LOCALAPPDATA%\YouTubeLyrics\smtc.ps1"
```

It prints one JSON line every 400 ms with the selected playing session, title,
artist and playhead. This
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

[download]: https://raw.githubusercontent.com/LIN4CRE/Youtube-Lyrics/refs/heads/main/docs/downloads/YoutubeLyrics.exe
[lrclib]: https://lrclib.net
[zig]: https://ziglang.org
