# Changelog

User-visible changes to Youtube-Lyrics are recorded here. The executable's version is shown in Windows file properties and in the About item in the tray menu.

## 1.5.0 — 2026-11-12

A usability and polish release. Everything that was already working still works
the same way; what changed is that the app can now be configured without the tray
menu, the artwork travels with the build, and a broken build can no longer be
published.

### New

- **Native settings window.** `Ctrl+Alt+S` (or **Settings…** in the tray, or `S` in the display) opens a compact owner-drawn Win32 window: display mode, full screen, always-on-top, theme, album-art tint, text size, sync offset (nudge either way and reset), karaoke wipe, wallpaper choice, rotation and interval, random scene per launch, background darkness, plus open-lyrics-folder, clear-cache and check-for-updates actions. It is a plain window with no framework, no web view and no new dependency, and keyboard navigation works through it.
- **A fourth background, *Drift*.** A procedural aurora-over-water scene drawn with GDI+ when the background cache is built. It costs no bytes in the download and scales to any resolution without resampling.
- **Text size from the keyboard** with `+`/`-` as well as `↑`/`↓`, and `[` to make the background quieter or brighter, because the brightness control was previously only reachable from the tray.
- **A grouped help overlay**: shortcuts are now laid out in WINDOW / LYRICS / LOOK columns instead of one long list.
- **First-run orientation**: a fresh install with no config file gets a one-line explanation of what the app is waiting for, instead of a bare "Listening".
- **`AUDIT.md`** rewritten for this release with the actual measurements and the remaining unknowns.

### Fixed

- **Settings and display could disagree.** The settings window kept its own copy of the theme list, the wallpaper list and the source order, in a different order from the renderer, with its own modulo — so it showed the wrong scene name and could not represent a fourth choice at all. It now asks the renderer for names and reads the live config, and every tray/hotkey change refreshes an open window.
- **The settings layout was broken.** Labels were positioned by a second hand-counted sequence from the controls, and the DISPLAY column ran past the footer. Rows are now driven by one table, the window height is derived from that table, and a test re-runs the geometry so a new row cannot silently collide.
- **Settings could not be used from the keyboard.** Nothing gave Tab, arrows or Escape to the owner-drawn controls. The message loop now routes them through `IsDialogMessage`, and focused controls draw a focus ring.
- **Wallpapers had hard seams.** *Stillwood*'s mist bands were flat rectangles with a visible hard edge across the frame, and *Rainfall District*'s road cut the composition in half. Both are feathered now, and a regression check looks for rows whose vertical step is far above their neighbours.
- **Icon legibility.** The quiet lyric lines were too pale, so the mark read as grey-on-cream at large sizes instead of a lyric sheet. Contrast raised in the artwork and in the in-app mark.
- **Version, size and checksum drift on the website.** The version, size and SHA-256 printed on the download page and in the README were maintained by hand and no longer matched the binary. They are now generated from `VERSION` and the actual executable, and `scripts/check_docs.py` fails if they drift again.
- **An iconless executable could be published.** On Windows, `build.ps1` swallowed a resource-compiler failure and produced a build with no icon. It now enforces the same gate as `build.sh`: mandatory icon, mandatory resources, verification before publishing, and rollback of `docs/downloads` if anything fails. `verify_build.py` grew an explicit throwaway-build escape hatch instead.
- **Single instance, tray recovery and suspend handling** were tightened: a stale mutex no longer blocks startup, the tray icon is re-added after an Explorer restart, and the config is saved on suspend and on session end.

### Changed

- The site palette now uses the application's own palette (background `#0b0a0f`, lyric paper `#f3efe6`, one amber `#f5a524`), and the illustrative preview was redrawn on a real bundled scene with scrims, line pacing and a status line.
- CI gained a Windows-native job (real Windows build, resource check, version-resource report and an actual PE load), and the existing job now verifies the executable, the assets and the downloadable copy instead of only the PE header.

### Verification for this build

- 3,400 portable-core checks under AddressSanitizer and UndefinedBehaviorSanitizer, plus 19 Python tests for release metadata, build verification, documentation facts and the settings geometry.
- `./scripts/build.sh` cross-builds, verifies the PE (x64, GUI subsystem, version resource, 16–256 px icon frames, three embedded scenes) and only then publishes to `docs/downloads`.
- Distributed x64 build: 910,848 bytes (889 KiB); SHA-256 `55de6cd3a4bcbd08f6bef8482f16ce79c17416acede9dea6231e3d034a2c0631`.
- Windows runtime behaviour — WorkerW parenting, tray recovery, DPI changes and the settings window itself — is still **not** verified in this environment; see `AUDIT.md`.

## 1.4.0 — 2026-11-06

- Added desktop-wallpaper mode using the Explorer WorkerW layer, click-through/no-activate window styles, and the existing global wallpaper/hide hotkeys.
- Bundled three original procedural 1600×900 wallpapers and added next/random selection, rotation intervals, dimming, and saved preferences.
- Improved paused/current media-session handling and kept lyric timing tied to source timestamps; no audio capture or invented karaoke timing was added.
- Added consent-based stable GitHub Release updates with strict release metadata, SHA-256 sidecar and download verification, x64 PE checks, staged replacement, and rollback handling.
- Added release-metadata tests, a stable-release publishing workflow, and a GitHub Pages deployment workflow.
- Refreshed the README, public download page, About details, help links, and optional PayPal support link.
- Windows runtime and WorkerW behavior remain unverified outside a real Windows 10/11 session.
- Distributed x64 build: 862,208 bytes (842 KiB); SHA-256 `1730585365d1607476a8416f61f1326fd4e7fea2775e51851d007ad0f7a13fd1`.

## 1.3.1 — 2026-11-06

- Added a complete in-app About panel with project, privacy, licence, and support information.
- Added a tray-menu action that opens the optional PayPal support page.
- Refreshed the public brand assets and made the download/support actions easier to find.
- Added repository contribution, security, support, and community guidance.
- Distributed x64 build: 702,464 bytes; SHA-256 `fe53060b920b149b67290317d7897e4bd9619944fd3acda9529502f8e0f65c17`.

## 1.3.0 — 2026-11-06

- Preserved Enhanced LRC inline timing and use genuine word/segment timestamps when available.
- Kept untimed lyrics explicitly untimed; estimated navigation is labeled and does not fabricate karaoke progress.
- Improved UTF-8 wrapping, track matching, cache and local-file handling, and bounded media/HTTP input.
- Added safer cancellation and shutdown behavior, improved Windows media-session selection, and reduced idle redraw frequency.
- Clarified privacy, download, source-confidence, and Windows verification limits in the documentation.

See the repository history for implementation details and tests. Windows runtime behavior still needs verification on a real Windows 10/11 system; a cross-build is not a runtime test.
