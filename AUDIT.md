# Youtube-Lyrics — engineering audit

**Reviewed:** 14 November 2026

**Version under review:** 1.6.0

**Repository:** `LIN4CRE/Youtube-Lyrics`

**Branch:** `arena/75852981-youtube-lyrics`, from `e0048124e4b965d7fcca48d7edcbb1b8f9fbf02f`

> **Scope of this audit.** Everything below was measured in a Linux sandbox with
> the Zig/MinGW-w64 cross toolchain. There was no Windows machine, no VM and no
> Wine in that environment, so **no Windows runtime claim is made anywhere in this
> document**. Where a statement depends on runtime behaviour it is marked as
> unverified rather than asserted.

## Summary

Youtube-Lyrics is still a portable native C++17/Win32 application with no
framework, no web view, no installer, no account, no telemetry and no audio
capture. The portable core handles LRC parsing, source timing, metadata
validation, matching and layout; the Windows layer provides the tray, the GDI+
renderer, the Windows media-session listener and HTTPS networking.

1.5.0 is a usability and integrity release rather than a rewrite:

- a native settings window now exposes everything that used to be reachable only
  through the tray menu or a hotkey, and it reads and writes the same `Config` the
  rest of the app uses;
- a fourth, procedural background was added that costs no bytes;
- the build pipeline became a gate on both platforms: a build that cannot embed
  its icon and version resource now fails instead of quietly shipping an iconless
  executable, and nothing is published to `docs/downloads` before verification;
- the version, size and checksum printed on the website are generated from the
  binary instead of being maintained by hand.

Two defects found during this review were *cosmetic on Linux but real on
Windows*, which is worth stating plainly: the settings dialog's layout had
overlapping and clipped controls, and the dialog could not be driven from the
keyboard at all. Both are fixed, and a test now re-runs the layout geometry.

## What changed since the 1.4.0 review

### Defects found and fixed

| # | Defect | Where | Evidence |
| --- | --- | --- | --- |
| 1 | Settings window listed themes and wallpapers in a different order from the renderer, with its own `% 3` — it displayed the wrong scene name and could not represent a fourth wallpaper | `src/win/settings.cc` | a second, divergent copy of `kThemes`/`kScenes` in the settings file |
| 2 | Settings controls and their painted labels were positioned by two independent hand-counted sequences; the DISPLAY column overflowed the footer and the sync buttons ran into the other column | `src/win/settings.cc` | offline geometry replay found 6 collisions and 5 clipped controls |
| 3 | The settings window could not be operated from the keyboard: nothing forwarded Tab/Escape/arrows to the owner-drawn controls | `src/win/app.cc` message loop | no `IsDialogMessage` call anywhere |
| 4 | A resource-compiler failure produced an iconless executable on Windows, because `build.ps1` caught the failure and continued | `scripts/build.ps1` | catch-and-continue around `zig rc` |
| 5 | The download page and README advertised a version, size and SHA-256 that no longer matched the binary | `docs/index.html`, `README.md` | page said `842 KiB` / `173058…`, build was `875,008` / `be9f632…` |
| 6 | `Stillwood`'s mist was three flat rectangles with hard edges across the whole frame; `Rainfall District`'s road cut the composition in half | `scripts/wallpapers.py` | 9.77-step luma discontinuity at the band edge |
| 7 | The quiet lyric lines in the app mark were too pale to read at large sizes | `scripts/brand.py`, `src/win/app.cc` | visual comparison against the dark and light backgrounds |
| 8 | An unused `destroy_settings` field had been added to `App` | `src/win/app.h` | leftover from an earlier patch |
| 9 | The settings window could show stale values after a tray/hotkey change | `src/win/app.cc` | `settings_refresh` added at 12 call sites |
| 10 | `Ctrl+Alt+S` did not exist; the tray menu had no settings entry | `src/win/app.cc` | tray ids stopped at 18 |

### Features added

- **Settings window** (`src/win/settings.cc`, 1 new file): owner-drawn Win32
  window, four palettes mirroring the renderer, custom control class, painted
  labels/sections/brand mark, `DwmSetWindowAttribute(20)` dark title bar, keyboard
  traversal and focus rings, and a height derived from its own row table. Native
  `BUTTON`/`STATIC` controls were tried first and rejected: with a dark painted
  background their text is illegible or invisible, because `WM_CTLCOLOR*` theming
  does not cover checkbox and push-button drawing reliably.
- **Procedural background** (`paint_procedural_background` in `render.cc`): a
  star field on a hashed lattice, three aurora ribbons, a Bézier horizon and
  gradient water, drawn once into the existing background cache.
- **Text-size and background keys** (`+`, `-`, `[`) and a grouped help overlay.
- **First-run line** in the idle state, keyed off `config.ini` being absent.
- **`scripts/verify_build.py`**: PE32+ / x64 / GUI-subsystem check, resource walk,
  icon group cross-check (every group entry must reference an existing RT_ICON of
  the declared size), wallpaper JPEG integrity, and VERSIONINFO comparison against
  `VERSION`.
- **`scripts/release_facts.py`**: rewrites the generated version/size/checksum
  block from `VERSION` and the built binary, with a `--check` mode for CI.
- **`tests/build_test.py`** and **`tests/settings_layout_test.py`**.
- **A Windows-native CI job** that builds with `build.ps1`, reports the embedded
  version resource and loads the executable with `LoadLibraryEx`.
- **`Config::version`** written to `config.ini` for future migrations.

## 1.6.0 addendum

1.6.0 rewrites display-mode handling (desktop / full screen / window), the
playback-position tracker, the lyric-candidate acceptance rules and the text
sizing; `CHANGELOG.md` lists each change. What was and was not verified:

- **Verified here:** the Windows build compiles and links cleanly with
  `-Wall -Wextra`; the PE passes `verify_build.py`; the matching, title clean-up
  and sheet-length rules are covered by 28 new portable checks.
- **Not verified here:** everything that needs a running Windows desktop. In
  particular the full-screen/desktop/window switching, the DPI-virtualised
  settings window, the new application manifest, text size and contrast on a
  real display, and the media-helper timeouts have been compiled but never
  executed. They should be checked on Windows 10 and 11 before this release is
  treated as proven.
- GitHub Actions did not run for this release (the account's Actions are
  blocked), so CI results are the local runs listed below, and the site is
  deployed from `docs/` by Vercel.

## Verification performed

Run on 14 November 2026 in this sandbox:

- `./scripts/test.sh` — **3,428 portable-core checks, 0 failures**, compiled with
  `-fsanitize=address,undefined` and `-Wall -Wextra`. Plus:
  - 4 release-metadata tests,
  - 10 build-verification/documentation tests,
  - 5 settings-layout geometry tests,
  - `scripts/check_docs.py` and `scripts/release_facts.py --check` (links,
    fragments, `alt` text, simulated-preview labelling, version/size/checksum).
- `./scripts/build.sh` — Zig/MinGW x86-64 Windows GUI cross-build. The build is
  gated: icon generation → icon verification → resource compilation → compile to a
  temporary path → `verify_build.py` → publish → re-verify the published copy.
  - **Distributed build: 1,018,368 bytes (994 KiB)**, SHA-256
    `e9eca38575f313b63e68ef226bbc0e11a3b1db7736959b54e870333c1aebe7aa`.
  - `verify_build.py`: `0x8664 windows-gui`, version `1.6.0`, icon frames
    `[16, 20, 24, 32, 40, 48, 64, 96, 128, 256]`, wallpapers
    `city-rain.jpg, forest-mist.jpg, mountain-night.jpg`.
- `dist/YoutubeLyrics.exe` and `docs/downloads/YoutubeLyrics.exe` are
  byte-identical; the sidecar and `release.json` are regenerated from the same
  bytes by `scripts/generate_release.py`.
- `scripts/make_icon.py --verify` — 10 frames, 16–256 px, every size present.
- Import-table inspection of the built PE: `gdiplus`, `gdi32`, `user32`, `ole32`,
  `shell32`, `dwmapi`, `winhttp`, `bcrypt`, `comctl32` and `kernel32`. `dwmapi` is
  linked for the dark title bar; the call is best-effort and its failure is
  ignored on Windows 10 builds that predate attribute 20.
- Wallpaper regression check: the maximum row-to-row luma step in each scene is
  **0.90 / 0.46 / 0.54** (previously 9.77 in *Stillwood*), and no row is flagged
  as a hard edge.

## Not verified / residual risks

- **No Windows runtime.** The executable has never been launched on Windows in
  this environment. Startup, shutdown, SMTC behaviour, WorkerW parenting,
  click-through, tray recovery after an Explorer restart, global hotkeys, the
  settings window, DPI changes and the updater's replacement/rollback path all
  remain unverified at runtime. The new `windows-native` CI job is the first
  automated step that actually loads the image on Windows; it does not replace
  interactive testing.
- **No genuine screenshot.** `docs/img/display.png` is artwork generated by
  `scripts/brand.py` from the renderer's real constants, and `docs/preview.html` is
  an interactive simulation. Both are labelled as such in the repository. No
  screenshot from a running Windows build exists, and none is claimed.
- **No performance measurement.** No CPU, memory, GDI-handle or startup-time
  numbers are quoted, because none were measured. The only performance statements
  in the README are design facts (background cache capped at ~2 MP, one composited
  back buffer, 16 ms frame timer used only while the display is animating).
- **Providers are untested live.** LRCLIB and lyrics.ovh availability, rate limits
  and result quality were not exercised; no network call was made from the app.
- **No browser accessibility audit.** The site's contrast, focus states, skip
  link and reduced-motion handling were reviewed by reading the CSS, not with an
  assistive-technology browser.
- **Publication is a separate step.** A tag push runs the stable-release workflow
  and Pages deploys from `main`. Neither was executed here; a local build is not
  evidence that a release exists.

## Deliberate design decisions

- **Owner-drawn settings controls.** The alternative — native `BUTTON`/`STATIC`
  controls with `WM_CTLCOLOR*` theming — renders unreadable text against a painted
  dark background. Owner drawing costs ~900 lines and keeps every theme exact.
- **Settings height derived from content.** A fixed 500 px client height could
  never fit eleven LYRICS rows at a comfortable row height; deriving the height
  from the row table keeps the two in step, and the geometry test enforces it.
- **The fourth background is procedural.** Adding a fourth JPEG would have grown
  the download for no new idea. The procedural scene reuses the existing cached
  background path and scales to any resolution.
- **Generated release facts.** Version, size and checksum are data about a
  specific binary; making the site cite them by hand guarantees drift.

## Recommended Windows checks

1. Run the executable on Windows 10 (1809+) and Windows 11. Exercise YouTube
   Music, another media player, paused-current and playing sessions,
   stopped/missing sessions, track changes, sleep/resume, and exiting during a
   slow lookup.
2. Open settings with `Ctrl+Alt+S`, `S` and the tray item on each theme; check the
   dark title bar, that Tab/Shift+Tab reach every control with a visible focus
   ring, that Escape closes, and that changing a value from the tray while the
   dialog is open updates it.
3. Test wallpaper on/off, WorkerW recovery after an Explorer restart, mixed-DPI
   monitors, monitor removal and the safe fallback. Confirm no focus theft and no
   click interception.
4. Confirm the redistributed copy still verifies:
   `python scripts/verify_build.py docs/downloads/YoutubeLyrics.exe`.
5. Capture genuine screenshots only from the running Windows application and keep
   the simulated preview labelled.
