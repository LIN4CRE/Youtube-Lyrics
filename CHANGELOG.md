# Changelog

User-visible changes to Youtube-Lyrics are recorded here. The executable's version is shown in Windows file properties and in the About item in the tray menu.

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
