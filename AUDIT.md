# Youtube-Lyrics — engineering audit

**Reviewed:** 6 November 2026

**Version under review:** 1.4.0

**Repository:** `LIN4CRE/Youtube-Lyrics`

**Base commit:** `91b13c56f51e313ae1baa8c4d41d2b6994434a48`

## Summary

Youtube-Lyrics remains a portable native C++17/Win32 application. The portable core handles lyric parsing, source timing, metadata validation, matching, and layout; the Windows layer provides the tray, GDI+ renderer, Windows media-session listener, and HTTPS networking. The application does not capture audio, use telemetry, require an account, or run as a service.

Version 1.4.0 adds an Explorer WorkerW desktop-wallpaper mode, bundled original procedural scenes, saved wallpaper preferences, and a consent-based stable GitHub Release updater. The artwork is generated locally from code and does not download third-party photographs. Existing lyrics providers, cache and local files, Enhanced LRC, sync controls, themes, hotkeys, tray, and normal display mode remain in the application.

## Behavior and trust boundaries

- **Media sessions:** the listener prefers playing sessions and may retain only Windows' current known music session while it is explicitly paused. Paused position is frozen; stopped or missing sessions are not promoted to a track. A short SMTC withdrawal grace avoids flicker while Windows changes sessions, after which the old sheet is cleared.
- **Lyric timing:** Enhanced LRC inline timestamps and ordinary LRC line timestamps are preserved. Untimed lyrics can be paced for navigation only when explicitly labeled as estimated; they do not gain a fabricated karaoke wipe. Player-position fallback, where used, is not a source lyric timestamp.
- **Lyrics and privacy:** LRCLIB candidates are checked against track metadata; lyrics.ovh is a lower-confidence untimed fallback because its response has no track identity. Local `.lrc`/`.txt` files take priority. Lookups send title and artist, and LRCLIB's exact-match request also sends album and duration when Windows provides them. Logs, settings, cache, and cover art remain local.
- **Desktop layer:** the code asks Explorer for a WorkerW host and uses no-activate, transparent input, and child-window styles. It has a safe bottom-of-z-order fallback if WorkerW cannot be found. WorkerW behavior and click-through have **not** been exercised on Windows in this environment; the implementation and cross-build are not runtime proof.
- **Updates:** the app queries only the project's latest stable GitHub Release. It validates the repository, version/tag, channel, architecture, asset name and size; compares the release manifest and SHA-256 sidecar; verifies downloaded bytes and the x64 PE header; stages the update; and uses a local replacement helper with rollback. Checksums delivered by the same GitHub HTTPS origin provide integrity checking, not an independent digital signature. The user is asked before installation.
- **Distribution:** the executable is a portable x64 Windows GUI build, not code-signed. Windows SmartScreen may warn. The stable-release and GitHub Pages workflows publish only after their GitHub Actions runs succeed; a local build alone does not establish that a public release or site is live.

## Verification performed

- `./scripts/test.sh` — **3,363 C++ checks passed** with AddressSanitizer and UndefinedBehaviorSanitizer; four Python release-generator tests also passed.
- `ZIG="$PWD/.cache/pydeps/ziglang/zig" PYTHONPATH="$PWD/.cache/pydeps" ./scripts/build.sh` — successful Zig/MinGW x86-64 Windows GUI cross-build, including embedded resources and updater code.
- PE-header inspection — both distributable copies are valid x86-64 PE32+ executables, each **862,208 bytes**. The cross-build does not prove that the app launches on Windows.
- `dist/YoutubeLyrics.exe` and `docs/downloads/YoutubeLyrics.exe` are byte-identical. SHA-256: `1730585365d1607476a8416f61f1326fd4e7fea2775e51851d007ad0f7a13fd1`.
- `scripts/generate_release.py` produced a local `v1.4.0` manifest and sidecar from the built PE; the release-generator tests cover invalid tags, repositories, architecture, PE headers, size, and version strings.
- `python3 scripts/check_docs.py` — the local links and fragments in both static HTML pages resolve.

## Not verified / residual risks

- No Windows runtime or VM was available. Startup, SMTC behavior, the WorkerW layer and click-through, hotkeys, tray menus, DPI/display changes, PowerShell media access, and update replacement/rollback still require real Windows 10/11 testing.
- No genuine Windows application screenshot was captured. `docs/img/display.png` is generated project artwork, and `docs/preview.html` is an interactive browser simulation; neither is runtime evidence.
- No browser visual or assistive-technology audit, provider-availability test, or battery/CPU measurement was performed.
- GitHub Pages and stable-release publication must be verified from successful Actions runs and by opening the public endpoints. This audit treats publication as a separate step from source changes and cross-compilation.
- LRCLIB and lyrics.ovh are third-party services and may be unavailable or rate-limited. The lyrics.ovh result cannot be independently matched from response metadata.

## Recommended Windows checks

1. Run the executable on Windows 10 (1809+) and Windows 11. Exercise YouTube Music, another media player, paused-current and playing sessions, stopped/missing sessions, track changes, sleep/resume, and app exit during a slow lookup.
2. Test wallpaper on/off, WorkerW recovery after Explorer restarts, icon/taskbar/context-menu use, ordinary foreground windows, mixed-DPI monitors, monitor removal, and the safe fallback. Confirm no focus theft or click interception.
3. Test tray actions, global `Ctrl+Alt+W` / `Ctrl+Alt+L` hotkeys, wallpaper cycling/preferences, Enhanced LRC, local BOM-encoded lyric files, cache clearing, update consent/cancellation, successful replacement, and rollback.
4. Capture genuine screenshots only from the running Windows application; keep the simulated preview clearly labeled.
