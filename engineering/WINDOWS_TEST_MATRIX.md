# Youtube-Lyrics — manual Windows test matrix

**Status: not started. Nobody has run any of this.** Every row is blank on
purpose. Fill it in on a real Windows PC with real playback; do not fill it in
from a simulator, Wine, the design preview, or memory.

## Before you start

1. Get `dist\YoutubeLyrics.exe` from the branch (or build it:
   `powershell -ExecutionPolicy Bypass -File scripts\build.ps1`).
2. Minimise everything (Win+D) and run
   `powershell -ExecutionPolicy Bypass -File scripts\verify.ps1 -SkipBuild`
   (drop `-SkipBuild` if you have Zig and Python and want the full run).
3. Keep `.build\verify\verification-report.txt`, `desktop-test.txt` and
   `desktop-test.png`.

## Record

| Field | Value |
| --- | --- |
| Date | |
| Tester | |
| Windows edition and build (`winver`) | |
| App version and SHA-256 of the exe tested | |
| Displays (resolution, scaling %, arrangement, which is main) | |
| Players used | |
| `verify.ps1` result | |

Result column: **Pass**, **Fail** (say what happened), or **Not run**. Add a
screenshot or the diagnostic report (tray → *Save a diagnostic report*) for
anything that fails.

## A. Start-up and presence

| # | Check | How | Pass when | Result |
| --- | --- | --- | --- | --- |
| A1 | Fresh launch | Rename `%LOCALAPPDATA%\YouTubeLyrics\config.ini`, start the exe | No window appears; one tray notice | |
| A2 | Tray icon | Look in the tray (and overflow) | Icon present; right-click menu opens | |
| A3 | Attachment | Tray menu or Settings (Ctrl+Alt+S) | Status reads "Attached beneath the desktop icons" | |
| A4 | Idle desktop | No music playing | Nothing from the app is visible on the desktop | |
| A5 | Single instance | Start the exe again | No second tray icon; Settings opens | |

## B. Desktop integration

| # | Check | How | Pass when | Result |
| --- | --- | --- | --- | --- |
| B1 | Behind icons | Play a song with lyrics, show the desktop | Lyrics are drawn under the icons and their labels | |
| B2 | Icons usable | Click, double-click, drag and right-click an icon that overlaps lyrics; right-click empty desktop | Everything behaves as without the app | |
| B3 | Behind windows | Open and move Explorer, a browser, a maximised and a borderless full-screen app | Every window covers the lyrics | |
| B4 | Focus | Type in Notepad through a song change, a pause, a stop | No keystroke is lost; Notepad stays active | |
| B5 | No chrome | Alt+Tab, taskbar, Task View | The app has no entry in any of them | |
| B6 | Wallpaper untouched | Compare the wallpaper before, during and after a song | Same wallpaper; no scene replaces it | |
| B7 | Wallpaper change | Change the Windows wallpaper while lyrics show; try a slideshow | Lyrics stay, under the icons | |

## C. Music and lyrics (real playback)

| # | Check | How | Pass when | Result |
| --- | --- | --- | --- | --- |
| C1 | Start | Play in YouTube Music in a browser | Lyrics fade in without any action | |
| C2 | Track and artist | Compare caption with the player | Correct | |
| C3 | Sync | Sing along through a verse and chorus | Highlight follows the voice | |
| C4 | Song change | Next track | Old words fade out, new ones fade in; never both; never old words under the new song | |
| C5 | Rapid skipping | Press next five times quickly | Only the final song's lyrics ever appear | |
| C6 | Pause / resume | Pause 10 s, resume | Position holds (dimmed if enabled), then continues in sync | |
| C7 | Seek | Jump forward and back | Highlight follows within about a second | |
| C8 | Stop / close player | Stop; then close the tab or app | Lyrics fade out; desktop clean | |
| C9 | Two players | Play in a second app while the first is paused | The playing one is shown | |
| C10 | No lyrics | Play an instrumental or obscure track | Nothing is shown | |
| C11 | Untimed lyrics | A song with only plain lyrics | Shown and labelled as estimated in window mode; behaviour on the desktop noted | |
| C12 | Network off | Disconnect, play an uncached song, reconnect | Nothing shown while offline; no stale words; recovers | |
| C13 | Local file | Put a matching `.lrc` in the lyrics folder | It is used | |
| C14 | Per-song sync | Window mode: nudge with arrows, skip away and back, restart | Correction returns for that song only | |

## D. Interruptions

| # | Check | How | Pass when | Result |
| --- | --- | --- | --- | --- |
| D1 | Explorer restart | Task Manager → restart Windows Explorer, during a song | Tray icon and lyrics return within seconds; status mentions the restore | |
| D2 | Explorer killed | End `explorer.exe`, wait 20 s, start it again | App still running; lyrics return | |
| D3 | Sleep / resume | Sleep during playback, wake | Layer attached; lyrics in sync after playback resumes | |
| D4 | Lock / unlock | Win+L, sign back in | As before | |
| D5 | Resolution change | Change resolution and scaling while lyrics show | Layer fills the display exactly; text sharp | |
| D6 | Quit | Tray → Quit during a lookup | Exits within seconds; log ends `stopped (clean)` | |

## E. Displays (needs two monitors)

| # | Check | How | Pass when | Result |
| --- | --- | --- | --- | --- |
| E1 | All displays | Default setting | Lyrics on each display, each filling its own screen, none spanning the gap | |
| E2 | Mixed scaling | 100% + 150% | Correct size and position on both | |
| E3 | Different resolutions | e.g. 4K + 1080p | Correct on both; note how the scaled copy looks | |
| E4 | Main only / one display | Settings → *Show on* | Only the chosen display shows lyrics | |
| E5 | Unplug / replug | Remove the second display during a song, then reconnect | Lyrics stay on the remaining one, return to the second | |
| E6 | Rearrange / change main | Display settings | Lyrics follow the new arrangement | |

## F. Modes, settings, start-up

| # | Check | How | Pass when | Result |
| --- | --- | --- | --- | --- |
| F1 | Mode switching | Desktop ↔ full screen ↔ window from tray, Settings, Ctrl+Alt+W / F | Each switch works and is reversible | |
| F2 | Settings persist | Change several settings, quit, start | All kept; mode kept | |
| F3 | Existing config | Start with a config that has `wallpaper=0` | Opens as a window, not on the desktop | |
| F4 | Start with Windows | Tick it, sign out and in | App starts silently in the tray; untick removes it | |
| F5 | Moved exe | With F4 on, move the exe, start it once, sign out and in | Still starts | |
| F6 | Shutdown | Shut Windows down while a song plays | No "app is preventing shutdown" | |

## G. Performance (read from *Save a diagnostic report* and Task Manager)

| # | Measure | 1080p | 1440p | 4K | Two displays |
| --- | --- | --- | --- | --- | --- |
| G1 | Idle CPU % / memory | | | | |
| G2 | Playing CPU % / memory | | | | |
| G3 | Frame time avg / max (report: *Frame time*) | | | | |
| G4 | During rapid skipping | | | | |

## H. Update safety

| # | Check | How | Pass when | Result |
| --- | --- | --- | --- | --- |
| H1 | No unwanted update | Run this development build with the site live | It is not replaced by an older stable build | |
| H2 | Update and rollback | Per `AUDIT.md` M12, when a real newer release exists | New version starts, or the old one keeps working | |

## Sign-off

A stable release needs `release/acceptance.json` for the exact binary tested:

```json
{
  "version": "2.0.0",
  "sha256": "<sha256 of the exe that was tested>",
  "windows_build": "<edition and build>",
  "tested_by": "<name>",
  "date": "<yyyy-mm-dd>",
  "desktop_test": "pass",
  "manual_matrix": "pass"
}
```

`desktop_test` is `pass` only if `verify.ps1` exited 0. `manual_matrix` is
`pass` only if no row above is Fail and none in A–D, F is Not run.
`scripts/verify_release.py` refuses to release without this file.
