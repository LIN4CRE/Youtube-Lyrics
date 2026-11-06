# Youtube-Lyrics

Big, readable, **synced** lyrics for whatever is playing — including YouTube
Music in a browser — on a full-screen display of their own.

YouTube Music shows lyrics in a narrow column on the right, in small type, with
no indication of where you are in the song. This is a tiny native Windows app
that sits in the background, listens to the Windows "now playing" session, and
paints the lyrics full-screen, karaoke style, with the current line lit up and
the sung words filling in as the song plays.

```
dist/YoutubeLyrics.exe      <- single file, no installer, no dependencies
```

## What it does

- **Listens, doesn't record.** It reads the Windows media session (the same
  metadata that appears on the volume-key overlay). Nothing is captured from
  your audio output, no microphone, no browser extension, no account.
- **Follows any player.** YouTube Music in Chrome/Edge/Brave/Firefox, Spotify,
  VLC, Plex… anything that appears in Windows' media controls.
- **Karaoke highlighting.** The active line is drawn larger and brighter, and
  fills in word-by-word as it is sung. Everything else fades back.
- **Readable from across the room.** Auto-sized type that always fits the width,
  smooth scrolling, stanza breathing room, and a choice of four themes.
- **Background friendly.** Lives in the tray, hides with `Ctrl+Alt+L`, can float
  always-on-top on a second monitor, and remembers your settings.
- **Offline after the first play.** Lyrics are cached on disk per track.

## Running it

1. Copy `YoutubeLyrics.exe` anywhere you like (Desktop, `%LOCALAPPDATA%`, a USB
   stick — it is fully portable) and double-click it.
2. Start a song in YouTube Music (or any player). The window fills in by itself,
   usually within a second.
3. `Esc` leaves full screen, `Esc` again hides to the tray, `Ctrl+Alt+L` brings
   it back from anywhere, `F1` lists the shortcuts.

Windows may show a SmartScreen prompt the first time ("Windows protected your
PC") because the file is unsigned. Choose **More info → Run anyway**. It needs
no administrator rights and makes no system changes.

## Keyboard shortcuts

| Key | Action |
| --- | --- |
| `F11` | Full screen ⇄ windowed |
| `Esc` | Leave full screen; if windowed, hide to tray |
| `Ctrl+Alt+L` | Show / hide from any app (global) |
| `A` | Always on top |
| `↑` / `↓` | Bigger / smaller text |
| `←` / `→` | Nudge lyric timing by 0.25 s |
| `F` | Reset text size |
| `R` | Look the lyrics up again |
| `C` | Cover-art background on/off |
| `K` | Karaoke wipe on/off |
| `T` | Theme: Midnight → Aurora → Ember → Mono |
| `F1` / `H` | Help overlay (also shows what the media helper sees) |
| `Q` | Quit |

Double-click the window to toggle full screen. The tray icon has the same
essentials in a right-click menu.

## Where the lyrics come from

[LRCLIB](https://lrclib.net) — a free, open, community-maintained database of
time-synced lyrics. The app sends the track title, artist, album and duration,
and gets back an LRC file with per-line timestamps. Only the metadata is sent,
only when a new track starts. Nothing about you is transmitted or stored, and
nothing is uploaded.

If a track is missing from the database you get a clear "no lyrics found"
message rather than a blank screen, and `R` retries.

## If something is off

**No lyrics for a track I can see in YouTube Music.**
Press `F1` to see exactly which title/artist the app detected — YouTube Music
sometimes reports things like `Artist - Topic` or `Song (Official Video)`, which
the app cleans up, but odd spellings can miss. The lookup also tries fuzzy
search before giving up. Press `R` to retry after starting a different track.

**Words are consistently early or late.**
Some players report the playhead imprecisely. Nudge with `←` / `→`; the offset
is saved. If a player reports no playhead at all, the app runs its own clock from
the moment the track started, which is usually within a second or so.

**"Waiting for music…" while something is playing.**
The app reads Windows' media session, so the player has to publish one. Browsers
do this while a tab is actually playing audio. If you are using a player that
does not, check `F1` — it shows the raw state the helper reports.

**Nothing happens at all / it exits immediately.**
Check `%LOCALAPPDATA%\YouTubeLyrics\ytlyrics.log`. Lyrics are cached in the
`cache` subfolder next to it; deleting that folder is always safe.

**Seeing what the listener hears.**
To watch the raw media-session feed yourself, run in a terminal while a song
plays:

```bat
powershell -ExecutionPolicy Bypass -File "%LOCALAPPDATA%\YouTubeLyrics\smtc.ps1"
```

It prints one JSON line every 400 ms with the detected status, title, artist and
playhead. `F1` inside the app shows the most recent line too.

**It needs PowerShell.**
The background listener is a small Windows PowerShell script (present on every
Windows 10/11 machine) that talks to the WinRT media-session API. The app writes
it to `%LOCALAPPDATA%\YouTubeLyrics\smtc.ps1` and runs it hidden; no console
window appears. If a corporate policy blocks PowerShell, listening will not work.

## How it is built

Nothing exotic: a native Win32/GDI+ application in C++17, cross-compiled to a
single Windows executable. The portable logic (LRC parsing, JSON, track-name
cleaning, lyric layout and scroll maths) is deliberately separated from the
Windows layer and covered by unit tests that run on any platform.

```
src/core/       portable logic + its test suite (tests/core_test.cc)
src/win/        window, tray, painting, WinHTTP, media-session helper
src/win/smtc.ps1  the PowerShell listener (embedded into the exe at build time)
scripts/build.sh  cross-compile to dist/YoutubeLyrics.exe
scripts/test.sh   build & run the core tests with ASan/UBSan
docs/preview.html browser mock of the full-screen display (design preview)
```

```bash
./scripts/test.sh     # core tests (ASan + UBSan)
./scripts/build.sh    # -> dist/YoutubeLyrics.exe
```

The build needs [Zig](https://ziglang.org) only — it bundles the MinGW-w64
toolchain and resource compiler, so it works on Linux, macOS or Windows without
Visual Studio. Icon generation additionally uses Pillow (`pip install pillow`).

## Licence

MIT.
