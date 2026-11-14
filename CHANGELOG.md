# Changelog

User-visible changes to Youtube-Lyrics are recorded here. The executable's version is shown in Windows file properties and in the About item in the tray menu.

## 1.8.0 — unreleased

Desktop mode is rebuilt to be what the program was meant to be: lyrics that are
part of the desktop while music plays, and nothing at all when it does not.
**This version has been compiled and its portable logic tested, but the new
desktop layer has not been run on Windows.** It is not published as a stable
release until `--desktop-test` and the manual checks in `AUDIT.md` have passed
on a real PC.

### Why desktop mode could look like an ordinary window

- When Explorer's wallpaper window could not be found or would not take the
  window, the app silently used a top-level window at the bottom of the window
  stack and still called it desktop mode. That window sits in front of the
  desktop icons, on one monitor, and is not a wallpaper. On current Windows 11
  builds, where the desktop is laid out differently, the old lookup had no
  window to find.
- Even when attached, the app painted its own opaque scene over the whole
  desktop permanently, including when nothing was playing.
- Leaving desktop mode (`Ctrl+Alt+W`, `F11`, a double-click, the tray) was
  saved, as it should be, but nothing said how to get back.

### Changed

- **A separate desktop layer.** Lyrics in desktop mode are drawn by a
  transparent, click-through child window created inside Explorer's desktop,
  beneath the icon layer. Three Explorer layouts are recognised, including the
  one used by Windows 11 24H2 and later. The main window is no longer moved
  into Explorer, so an Explorer restart cannot close the program.
- **No pretend wallpaper.** If the layer cannot be attached, nothing is drawn.
  The state is retried, reported in Settings, the tray menu and the diagnostic
  report, and announced once from the tray.
- **Your wallpaper stays.** The layer has per-pixel transparency; lyrics are
  drawn over the existing Windows wallpaper with a soft dark edge for contrast.
  The bundled scenes are now an option (*Cinematic scene instead of my
  wallpaper*).
- **Clean when idle.** With no music, or no lyrics for the song, the desktop
  shows nothing from this program: no "looking for lyrics", hint, toast,
  progress bar or status line. Lyrics fade in when ready, dim slightly while
  paused (optional), and fade out on stop or when the session closes. A pause
  longer than two minutes also fades out.
- **Settings.** Three explicit modes (Desktop wallpaper / Full screen /
  Ordinary window), a live attachment status line, a *Restore desktop mode* /
  *Try attaching again* button, and switches for the artist/title caption,
  pause dimming and the scene background.
- **Recovery.** The layer is re-attached after Explorer restarts, and checked
  again on display changes, DPI changes and resume from sleep.
- **Diagnostics.** New `--desktop-test [file]` command and a *DESKTOP
  INTEGRATION* section in the diagnostic report.

Existing settings, caches, lyric files and sync corrections are untouched. A
saved choice of window or full-screen mode is respected.

## 1.7.1 — 2026-11-14

A second, independent review of the previous release: reliability fixes in the
lookup, cache, updater and shutdown paths, stricter privacy in diagnostic
reports, and a release process that refuses to publish anything that is not
byte-for-byte the verified build. No features were added or removed. It was
cross-compiled and its portable logic is tested, but **it still has not been run
on a Windows machine**; `AUDIT.md` separates what was tested from what was only
compiled.

### Fixed

- **Skipping to a new song could show "provider unavailable" for the new song.** Cancelling the previous song's request also cancelled the new song's first request, which was then treated as an outage and skipped LRCLIB's exact match. A cancellation now only ever applies to requests of older lookups.
- **Cached lyrics could be misread.** A lyric line that happened to look like the cache's internal separator split the entry in the wrong place, a title containing a line break could inject cache header fields, and an entry without its timed/untimed marker was accepted. The cache reader and writer were rewritten in the tested core; malformed entries are discarded and fetched again.
- **"Remembered for this song" was shown even when the correction could not be saved** (read-only or full data folder). The app now says the correction applies until it closes, and logs the reason. Failed settings saves are logged too.
- **Diagnostic report export reported success or one generic failure.** It now distinguishes "saved", "could not save the file, but the report is on the clipboard" and "failed", and the clipboard is retried when another program is holding it.
- **Diagnostic reports could leak more than intended.** Redaction now also covers the account name in forward-slash, URL-encoded and differently-cased paths, other users' profile folders, `Authorization`/`Cookie`/API-key headers, bearer tokens, credentials inside URLs and key=value secrets, and applying it twice no longer damages the text.
- **Shutdown.** Stopping the media listener could close an event handle its thread was still using; stopping the updater waited without limit for a stuck download. Every worker now has a bounded stop, the log records whether the stop was clean, and the process no longer runs normal teardown underneath a thread that is still alive.
- **Updater file check.** A downloaded update must now be an x64 Windows *application*: a DLL, a console program or a truncated header is refused before it can replace the running copy (size and SHA-256 were already checked).
- **Media listener.** A failed helper start logged the wrong error code and could flood the log; an over-long profile path could produce a truncated helper command line. Both fixed.

### Changed

- The diagnostic report opens with a plain statement of what it contains (song titles and artists, player name, lookup activity, Windows version, monitors, settings, log) and what it does not, and now includes the media listener's health.
- `YoutubeLyrics.exe --self-test [file]` and `YoutubeLyrics.exe --quit` were added for automated checks; see the README.

### Release process

- A GitHub Release can be published only by `scripts/verify_release.py` passing: the tag must equal `VERSION`, the commit must be on `main`, a rebuild must leave every tracked file unchanged, and the rebuilt executable must be identical to the site's download and agree with its checksum file and every field of `release.json`. The three release assets are generated from that verified file. Publishing goes draft → upload → download and compare → public, and can be re-run safely; a published release is never overwritten.
- GitHub Actions still cannot run on this repository (account billing lock), so these workflows have not been executed there; the scripts behind them are covered by tests that were run locally.

## 1.7.0 — 2026-11-14

An audit release: fewer wrong or missing lyric matches, sync corrections that
stay with the song, a diagnostic report, a sturdier updater and a download
that can be reproduced from source. Like 1.6.0 it was cross-compiled and its
portable logic is tested, but **it has not been run on a Windows machine**;
`AUDIT.md` lists exactly what was and was not verified.

### New

- **Sync correction per song.** `←` / `→` now correct the song on screen, and the correction is remembered for the next time that song plays (up to 400 songs, in `offsets.txt`). Previously the arrows moved one global offset, so fixing one badly timed sheet put every other song out. The value in Settings is still the all-songs baseline; an offset saved by an older version carries over as that baseline. *Reset sync correction* clears both.
- **Diagnostic report.** `D` in the display, *Save a diagnostic report* in the tray menu, or the button in Settings writes one text file with the app and Windows versions, monitors and DPI, the display mode, what the media session reports, the track on screen, every provider request of recent lookups and why each result was accepted or refused, the cache size, the updater's last activity and the end of the log. It is saved under `%LOCALAPPDATA%\YouTubeLyrics\diagnostics\`, copied to the clipboard and shown in Explorer. Nothing is uploaded; the Windows account name and anything that looks like a credential are replaced before the file is written.
- **`YoutubeLyrics.exe --diagnostics [file]`** writes the same report without opening a window and additionally checks that LRCLIB, lyrics.ovh, the project site and GitHub can be reached.

### Fixed

- **Ordinary titles treated as live or remix versions.** Words such as *live*, *session*, *flip* and *reverb* anywhere in a title marked the track as a special edition, so "Live Forever", "Obsession" or "Flip Flop" were matched against the wrong set of results or refused. They now count only inside brackets or as a trailing "- Live" style segment.
- **Bracketed subtitles removed from titles.** A short bracket was dropped when it merely contained a tag word as part of another word ("(Alive)", "(After Hours)", "(Lost)"), which changed the title being searched. Whole words only.
- **Untimed lyrics stuck for ever.** Once a song had been cached with untimed words it was never looked up again, even after timed lyrics became available. Untimed cache entries are re-checked after seven days; if nothing better is found the cached words are kept.
- **"Not found" shown for a network failure.** When one provider failed and the other simply had no entry, the screen reported the song as missing and did not retry. The failure is now what is reported, and the automatic retry applies.
- **The fallback provider could be skipped.** A long chain of LRCLIB searches could use up every request before lyrics.ovh was asked. One request is always kept for it, and an identical request is never sent twice.
- **Interrupted updates left files behind.** A partial download, an unused staged copy or the replacement helper's working files stayed on disk indefinitely. They are removed at the next start.
- **The update helper could wait for ever.** If the running copy never exited, a hidden PowerShell process waited indefinitely. It gives up after a minute without touching the installed file.
- Automatic update-check failures are written to the log instead of vanishing.

### Changed

- **The download is reproducible.** The link timestamp and the build identifier derived from it are normalised after linking (`scripts/normalize_pe.py`), so building the same commit with the pinned toolchain gives the same SHA-256 as the published file. Both build scripts also regenerate the release manifest and the figures on the site from the bytes they publish.
- The shortcut list names `[` correctly (it cycles the background darkness) and lists `D`.

## 1.6.0 — 2026-11-14

A reliability release. It fixes full screen, the wrong-song lyric sheets, the
unreadably small text and several ways the display could stop following the
music. It was cross-compiled and its portable logic is tested, but it has not
been run on a Windows machine as part of this release; see `AUDIT.md`.

### Fixed

- **Full screen did nothing.** With the desktop layer switched on (the default), `F11`, double-click and the settings tick changed a saved flag but not the window, so nothing visible happened. There are now three explicit modes, *on the desktop*, *full screen* and *window*, applied in one place. `F11`, double-click and the new global `Ctrl+Alt+F` go to full screen from any mode and back to a window; the tray and settings show the three as one choice.
- **`Esc` made the display disappear.** It left full screen and then hid the window to the tray, which looked like a crash. `Esc` now closes help, then leaves full screen, and stops there. Hiding to the tray shows a one-time notice saying where it went.
- **Long, wrong lyrics that played to the end.** A search result was accepted on a loose title match alone, so another song with a similar name (or a longer live cut) could be shown and timed against the wrong recording. A result now needs the exact title or a length within 3 s; anything over a minute out is refused; a 12–60 s difference shows the words without pretending to time them; a timed sheet must fit the track's length; and a lyrics.ovh answer is refused when its own credit line names another song or it is far longer than a song. Cached sheets from older versions are discarded.
- **Titles lost their last word.** Clean-up removed words such as "Radio" or "Clean" from the end of a title ("Car Radio" was searched as "Car"). Only quality tags such as `4K` and `HD` are removed now.
- **Tiny lyrics.** Text was shrunk until the longest line of the song fitted on one row, and untimed sheets until the whole song fitted the screen, down to 13 px. Size now comes from the window, long lines wrap, and there is a floor. One oversized setting from an older config is brought back into range once.
- **Lyrics lost in the background.** Distant lines faded to near-invisible and the scene behind the text was not darkened. Lines keep a readable minimum opacity, every line has a soft shadow, the middle of the scene is dimmed slightly, and the line being sung is larger.
- **A new song opening on its last lines.** Browsers change the title before the playback position, so a new track briefly reported the end of the previous one. The position is now counted from when the track appeared until the player reports a fresh one.
- **The display stopping after a while.** The media helper could wait for ever on a player that never answered; it was restarted after 12 s of silence even while PowerShell was still starting on a slow machine, so it could loop without ever running; and its script was only written once per run. Calls are bounded, start-up gets 45 s, and the script is checked on every start. A paused tab in another app no longer replaces the song being listened to, and players that report no "last updated" time no longer freeze the lyrics.
- **Stutter when the desktop layer is unavailable.** Attaching to it was retried every two seconds for ever, blocking the display each time. It is tried five times and then once a minute.
- **Failed lookups stayed failed.** A network or provider error is retried up to three times; a lookup for a song that has already been skipped is abandoned instead of holding up the next one.
- **The settings window was tiny on high-DPI screens**, and the mouse pointer could vanish over it while the display was full screen.
- **Smaller things:** seeking jumps to the new line instead of scrolling through the whole song; a second launch brings up the running copy (and opens Settings when it is on the desktop); the log file is rotated at 1 MB; *Reset display settings* also resets the mode and scene; the text-size button in Settings wraps round instead of stopping.

### Changed

- **The download page serves its own file.** The button used GitHub's "latest release" link, which was still handing out 1.4.0. It now points at the copy published with the site, so the page, its checksum and the file always match.
- **Updates no longer depend on GitHub Releases.** The updater checks the release published with the project site (`youtube-lyrics-seven.vercel.app`) first and GitHub Releases second, with the same size, checksum and x64 checks for both. Copies older than 1.6.0 only know about GitHub Releases, so they have to be replaced by hand once.
- An application manifest declares Windows 10/11 support and per-monitor DPI awareness.

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
