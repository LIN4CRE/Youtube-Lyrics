# Youtube-Lyrics — engineering and product audit

**Reviewed:** 6 November 2026

**Repository:** `LIN4CRE/Youtube-Lyrics`

**Working branch:** `arena/dd3d7a18-youtube-lyrics`

**Base commit:** `bd3330de3e6d8e5400c2b4f4fe87148032f2df54`

## Executive summary

Youtube-Lyrics remains a small native Windows desktop application: portable C++17 core logic, a Win32/GDI+ UI, WinHTTP lyric lookups, and a bundled Windows PowerShell 5.1 helper for the Windows media-session API. It does not use a browser wrapper, audio capture, a server, an account, or telemetry.

This audit focused on truthful lyric timing, malformed input and overflow handling, local-file/cache safety, worker shutdown, media-session selection, idle redraw behavior, and download-page accuracy. It adds Enhanced LRC timing preservation/rendering, UTF-8-aware wrapping, safer bounded I/O and cancellation, local/cache controls, clearer timing labels, and more accurate product/download documentation.

The portable tests pass and a Windows x64 GUI executable cross-builds with its icon and version resource. **The executable has not been run on Windows in this environment.** SMTC, Win32 rendering/input, PowerShell behavior, SmartScreen, runtime performance, and actual Windows compatibility therefore remain unverified. No real before/after screenshot is available; the existing large display image is generated concept art, now labeled as such.

## Architecture and feature audit

- **Core:** `src/core/core.cc` is platform-independent C++17. The sanitizer-backed suite exercises JSON, UTF-8, timestamp/LRC parsing, track matching, layout/wrapping, interpolation, local imports, and related behavior.
- **Windows UI:** `src/win/app.cc`, `render.cc`, and `app.h` implement the native window, tray/hotkeys, layout state, Win32 lifecycle, DPI/display/resume handling, and GDI+ drawing. `media.cc` consumes newline-delimited JSON from the bundled SMTC helper.
- **Media selection:** `src/win/smtc.ps1` prefers playing sessions and falls back only to Windows' current known music session when explicitly paused. Stopped sessions are ignored. The C++ listener confirms a candidate over multiple polls before switching tracks.
- **Lyrics and timing:** standard LRC remains line-timed; Enhanced LRC inline markers are retained and can drive segment-level progress. Untimed lyrics may be paced for navigation, but keep `synced == false`, are labeled **estimated scroll**, and never get a fabricated karaoke wipe. `lyrics.ovh` is a lower-confidence untimed fallback because its response contains no track identity metadata; LRCLIB candidates are scored against track metadata.
- **Local use and privacy:** a local `.lrc`/`.txt` override takes priority and is not stored as an automatic cache entry. The tray can open the overrides folder and clear the automatic lyric cache. No audio is recorded. Lookups send title and artist; LRCLIB's exact-match request also sends album and duration when Windows provides them. Logs, lyric cache, configuration, and cover art are local application data.
- **Timing adjustment:** the nudge is one saved global offset, not per-app or per-track, and the tray can reset it.
- **Distribution:** the app is a standalone x64 Windows executable and requires no installer, administrator rights, or terminal use. It is unsigned; Windows may show an unrecognized-app/SmartScreen warning.

## Changes in this audit

- Hardened the portable JSON and UTF-8 paths, rejected malformed/overflowing timestamps, supported hour-form timestamps, and preserved genuine Enhanced LRC segments through local-cache serialization.
- Wrapped long unbroken and multibyte text at UTF-8 boundaries, carried timing ranges through layout, and advanced the active wrapped row using source segment timestamps.
- Kept untimed lyrics explicitly untimed while providing optional estimated line navigation; only source timestamps can drive a wipe.
- Bounded media duration, file reads and HTTP responses; made config-number parsing strict; made atomic writes check short writes and flush before replacement.
- Added cancellation-aware WinHTTP shutdown, cache synchronization/clear controls, local-file BOM handling, paused-current-session behavior, and worker shutdown protection.
- Reduced idle UI timer frequency, added display/DPI/resume handling, resettable sync offset, and improved cleanup/error handling for extracted cover art.
- Reordered lyric fallback so filtered LRCLIB title-only candidates are tried before the unverified lyrics.ovh fallback.
- Corrected privacy/timing claims and explicitly labeled the generated display image and browser mock as illustrations rather than runtime evidence.

## Verification performed

- `./scripts/test.sh` — **3,333 checks, 0 failures** under AddressSanitizer and UndefinedBehaviorSanitizer.
- `PATH="$PWD/.cache/ziglang/ziglang:$PATH" PYTHONPATH="$PWD/.cache/pydeps" ./scripts/build.sh` — successful Zig/MinGW cross-build with Pillow available; the generated icon and version resource are embedded.
- PE inspection: x86-64 PE32+, Windows GUI subsystem, icon/group-icon and version resource entries present; version strings report `1.3.0.0`.
- Static docs check: Python's HTML parser accepted `docs/index.html` and `docs/preview.html`; `node --check` accepted the preview's inline JavaScript. This is not a browser visual test.
- Artifact: `567,808` bytes (about 555 KiB); `dist/YoutubeLyrics.exe` and `docs/downloads/YoutubeLyrics.exe` are byte-identical. SHA-256: `51722e0c9a93ac38214b01b558e9722d8345dff9d5d308cb6eb5f66147059bfd`.
- Repository comparisons: PRs #1–#4 are merged. PR #1 merged as `4810ccb60c611224eba2ada6d1b22d0c1b79dbc2`. The older branch `arena/e4ef4e7f-youtube-lyrics` is now **0 ahead, 7 behind** `main`, with no changed files.
- GitHub API check: `main` is the default branch; Pages is disabled (`has_pages: false`), and there are no GitHub Releases. The README is configured to download from GitHub's raw `main` path; this branch's new binary is not available through that URL until the branch is merged. No Pages deployment or release is claimed.

## Not verified / residual risks

- No Windows runtime or VM was available. The cross-build proves compilation/linking, **not** that the program launches or behaves correctly on Windows 10/11.
- The bundled PowerShell 5.1/WinRT SMTC logic, player-specific session metadata, pause/resume behavior, click-through wallpaper integration, tray/hotkeys, sleep/wake, multi-monitor DPI transitions, local overrides, cache clearing, and HTTP cancellation need Windows runtime tests.
- No real screenshot, before/after comparison, browser visual test, accessibility audit with assistive technology, CPU/battery measurement, or provider-availability test was performed. `docs/img/display.png` is generated by `scripts/brand.py`; `docs/preview.html` is a hand-built simulation with sample track/lyrics/timing.
- LRCLIB and lyrics.ovh are third-party services and may be unavailable or rate-limited. lyrics.ovh cannot independently confirm returned track identity. There is no `Retry-After` backoff implementation.
- The executable is not code-signed. Users should download it from the published repository path only if they trust the source.
- The repository has no active GitHub Pages site or GitHub Release at audit time. Enabling/publishing either requires repository permissions and a separate release/deployment step.

## Recommended next verification steps

1. Run the built executable on Windows 10 (1809+) and Windows 11. Exercise YouTube Music, a desktop player, a paused current session, a second playing session, stopped sessions, missing metadata, track changes, sleep/resume, and app exit during a slow lookup.
2. Test windowed/full-screen/wallpaper modes, tray actions and global hotkeys, mixed-DPI monitors, monitor removal, cover art, cache clearing, local UTF-8/UTF-16 BOM files, Enhanced LRC, and long multilingual lines. Capture genuine screenshots from the running executable only after these tests.
3. Perform desktop/mobile browser checks and keyboard/contrast/accessibility checks for the static download page and simulated preview; do not treat the preview as proof of Windows rendering.
4. If publishing a new release, verify the uploaded artifact hash, include the unsigned-app warning and source link, and create a GitHub Release only when release permissions are available. Enable Pages separately if a hosted landing page is desired.
