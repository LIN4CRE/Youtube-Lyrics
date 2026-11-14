# Youtube-Lyrics — engineering audit

**Reviewed:** 14 November 2026

**Version under review:** 1.7.1

**Repository:** `LIN4CRE/Youtube-Lyrics`

**Baseline reviewed:** `main` at `6bc6525efa2f5cfbba53a00323674717b6c16bfe` (the published 1.7.0). The 1.7.0 audit, which started from `4f70a0c` (1.6.0), is kept below.

**Changes made on:** branch `arena/1a165e35-youtube-lyrics`, branched from that commit; the Git history of that branch is the record of each change.

> **Scope of this audit.** Everything below was measured in a Linux sandbox with
> the Zig/MinGW-w64 cross toolchain. There was no Windows machine, no VM and no
> Wine in that environment, and its network could reach only GitHub and the
> package registries. **No Windows runtime claim and no live-provider claim is
> made anywhere in this document.** The status table below separates what was
> tested from what was only compiled.

## 1.7.1: second independent review

1.7.1 is a second pass over the merged 1.7.0 source by fresh reading of the
code, not a repeat of the first audit. It changes no architecture and no
feature. Status words mean the same as in the table further down: **Tested** =
executed here and passed; **Compiled only** = built for Windows, never run.

### Defects found and fixed

| # | Severity | Defect | Fix | Evidence |
| --- | --- | --- | --- | --- |
| A1 | High | Superseding a lookup aborted whatever request was on the wire, including the *new* song's first request if the worker had already moved on. The new song then saw a "transport error", LRCLIB's exact match was skipped and the result could be shown as an outage. | Each request records the lookup that owns it; an abort applies only to older lookups. | Compiled only |
| A2 | High | Cache file format: a lyric line equal to the internal separator split the entry early; a line break in a title could inject header fields; an entry with no `synced` marker was accepted as timed. | Reader/writer moved into the portable core and rewritten; malformed entries are rejected and refetched. | Tested (core) |
| A3 | High | The stable-release workflow published whatever it had just built. It never compared the build with the site's download, did not check the manifest's fields, and created the release in one step, so an upload failure could leave a public release without its executable (this is what happened to the manual 1.6.0 attempt). | `scripts/verify_release.py` + `scripts/publish_release.sh`, see "Release process" below. | Tested (scripts, 13 tests); workflow never executed |
| A4 | Medium | Report redaction missed: forward-slash and URL-encoded profile paths, case differences, other users' folders, `Authorization`/`Cookie`/`X-Api-Key` headers, bearer tokens, `user:password@` in URLs, `key=value` secrets. Redacting twice corrupted the text. | Rewritten; idempotent. | Tested (core) |
| A5 | Medium | `media_stop` closed the stop event after a 3 s wait even if the thread was still running and would use the handle again. `update_stop` waited without limit. A stuck worker then had its globals destroyed underneath it by normal process exit. | Bounded waits everywhere; handles are kept if a thread is still alive; the process ends with `ExitProcess` in that case and the log says so. | Compiled only |
| A6 | Medium | The updater's file check accepted any x64 PE: a DLL or console program with the right checksum in a tampered manifest would have been staged. | Requires an executable, non-DLL, PE32+, GUI-subsystem image; logic moved to the core. | Tested (core, incl. every header truncation) |
| A7 | Medium | Saving a per-song correction or the settings could fail silently; the toast still said "remembered". | Result checked; honest toast; logged once. | Compiled only |
| A8 | Medium | Diagnostic export: file failure and clipboard failure were not told apart, the clipboard was tried once, and the user was not told the report names songs. | Three outcomes, clipboard retried, privacy statement at the top of the report and in the toast. | Compiled only |
| A9 | Low | Helper start failure logged `GetLastError()` after `CloseHandle` had overwritten it, and logged on every retry; `swprintf` of the helper command line was unchecked; worker threads posted messages to a null window handle in headless runs. | Fixed. | Compiled only |

### Reviewed and found sound (no change)

Log rotation; atomic file replacement; the read-size cap; the single-instance
mutex; update download, checksum and staging; provider classification of
outages versus misses; and **diagnostic report generation's reads of shared
state** — the report is built on the UI thread, which owns everything it reads
except the media snapshot (copied under `app.mu`), the lookup trail
(`g_trace_mu`), the cache listing (`g_cache_mu`) and the listener counters
(atomics).

### Release process (stable-release.yml)

A tag push (or manual dispatch) now runs, in this order, and stops at the first
failure with nothing published:

1. `VERSION` is a stable version and the tag is `v<VERSION>`.
2. The commit is on `main`, and an existing tag of that name points at it.
3. `./scripts/test.sh` on the committed files.
4. `./scripts/build.sh` with the pinned toolchain (which itself gates on icon,
   PE, resource and version-resource verification).
5. `git diff --exit-code`: the rebuild must not change one tracked byte.
6. `scripts/verify_release.py`: structural verification again; the rebuilt
   executable must equal `docs/downloads/YoutubeLyrics.exe` **as committed**
   (read with `git show`, because the build overwrites the working copy); its
   SHA-256 must equal the sidecar; `release.json` must state exactly this
   schema, repository, tag, version, channel, architecture, asset name, size
   and checksum, with no extra field. The three assets are then generated from
   the verified file and compared byte-for-byte with the committed ones.
7. `scripts/publish_release.sh`: create a **draft**, upload, download the
   assets back and compare, then make it public. Re-running reuses a leftover
   draft; an already-public release with identical assets is a no-op; one with
   different assets is refused.

`ci.yml` runs step 6 as a dry run on every push, so a commit that could not be
released is caught before anyone tags it.

### GitHub Actions status and the owner action

Re-checked on 14 November 2026: runs 43585063304 (CI) and 43585063261 (Publish
documentation site) for `6bc6525` failed with jobs that have **zero steps** and
the annotation *"The job was not started because your account is locked due to
a billing issue."* No runner is ever allocated. This cannot be fixed, worked
around or hidden from the repository, and no workflow was changed to ignore it.

**Action for the account owner:** GitHub → profile picture → *Settings* →
*Billing and licensing* → resolve the outstanding payment or spending-limit
problem shown there. Then re-run the failed CI run on `main` (Actions → CI →
*Re-run all jobs*). Only once CI is green: `git tag v1.7.1 <commit on main>`
and `git push origin v1.7.1`, which starts the release workflow.

Until then **no v1.7.1 tag or GitHub Release exists or should be created**; the
site download (served by Vercel, which does not depend on Actions) is the
distribution channel, and the in-app updater reads it first.

### Verification performed for 1.7.1

Run on 14 November 2026 in the Linux sandbox:

- `./scripts/test.sh`, exit status 0:
  - **4,783 portable-core checks, 0 failures** under
    `-fsanitize=address,undefined`, `-Wall -Wextra` (3,483 before); new groups:
    redaction hardening, cache format, lookup policy, offset persistence,
    update-image validation;
  - **17 release tests** (4 before): 7 for the release gate (every manifest
    field wrong and missing, checksum-file variants, a one-bit different
    rebuild, tag/version/repository mismatches, step order in the workflow) and
    6 that run `publish_release.sh` against a stand-in `gh` (draft-first order,
    idempotent re-run, corrupted upload stays a draft and can be retried,
    published release never replaced, inconsistent asset folder refused);
  - 15 build-tooling tests, 5 settings-layout tests, `check_docs.py`,
    `release_facts.py --check`.
- `./scripts/build.sh` (Zig 0.17.1, MinGW-w64, x86-64 GUI), no compiler
  warnings: **1,099,264 bytes (1074 KiB)**, SHA-256
  `fcba7110414ac6f40cc8e033310d8af06f052cf711cc6fde9292acd5c715cf3b`,
  `0x8664 windows-gui`, version `1.7.1`, icon frames
  `[16, 20, 24, 32, 40, 48, 64, 96, 128, 256]`.
- **Reproducibility:** `.build` and the compiler cache were deleted and the
  build repeated; byte-identical.
- `scripts/verify_release.py --tag v1.7.1` against the commit: see the pull
  request for the recorded output.

**Not executed:** anything on Windows. The new `--self-test`, `--quit`, the
three-way export result, the owner-aware abort and the shutdown changes are
compiled and linked only.

### Automated Windows checks (written, never executed)

The `windows-native` CI job will, once Actions can run: build natively; load
the image; run `--diagnostics` (version, redaction, provider reachability,
Windows version and monitor detection); run `--self-test` (data-folder
write/replace/read/delete, settings in range, media listener starts, reports
and stops, worker stops during a lyric request, redaction of a planted
credential); then start the real program, confirm it stays up for 8 s, that a
second copy exits, that `--quit` ends it within 20 s with exit code 0 and a
`stopped (clean)` log line and no process left. These prove process-level
behaviour only. **They do not show that anything is drawn correctly**, and
until a run exists they are evidence of nothing.

### Manual Windows test matrix (all outstanding)

None of these has been done by anyone. Each needs a person at a Windows PC.

| # | Area | Steps | Pass when |
| --- | --- | --- | --- |
| M1 | Windows 10 (1809+) and Windows 11 | Run `--self-test r.txt`, then start normally on each | `RESULT: PASS`; tray icon and display appear |
| M2 | Players | YouTube Music in a browser, then one other player (Spotify, Media Player) | Track, artist and position are picked up from each |
| M3 | Transport | Pause, resume, seek forwards and back, skip mid-lookup, skip quickly several times | Lines follow within about a second; never the previous song's lyrics; no "provider unavailable" after a skip (A1) |
| M4 | Desktop mode | Lyrics behind icons; click and drag icons through the display | Clicks pass through; focus never taken |
| M5 | Modes | `Ctrl+Alt+F`, `F11`, double-click, `Esc` across desktop / full screen / window | Each switch is immediate and reversible |
| M6 | Monitors | Two monitors at different scaling; move, then unplug one | Text stays sharp and on a live monitor |
| M7 | Explorer restart | End `explorer.exe` in Task Manager and start it again | Desktop display and tray icon return |
| M8 | Sleep / resume | Sleep during playback, wake | Listener recovers within 45 s; position correct |
| M9 | Per-song sync | Nudge with arrows, skip away and back, restart the app; then make the data folder read-only and nudge | Correction returns for that song only; read-only case says it is *not* remembered (A7) |
| M10 | Diagnostics | `D`; then with the diagnostics folder read-only | File + clipboard; second case says clipboard only (A8); no account name in the text |
| M11 | Quit under load | Quit from the tray during a lookup on a slow connection; shut Windows down while it runs | Exits within seconds; log ends `stopped (clean)` |
| M12 | Updater | Older build beside a newer site release: accept; cancel mid-download; pull the network mid-download; make the exe folder read-only | New version starts; otherwise the old one keeps working and no `.part` files remain after the next start |

## Summary

Youtube-Lyrics is a portable native C++17/Win32 application with no framework,
no web view, no installer, no account, no telemetry and no audio capture. The
portable core (`src/core`) handles LRC parsing, timing, metadata clean-up,
matching and layout and is unit-tested under sanitizers on Linux; the Windows
layer (`src/win`) provides the tray, the GDI+ renderer, the media-session
listener, HTTPS, the updater and the settings window.

1.7.0 is the result of a forensic pass over the 1.6.0 source. It keeps the
architecture and every feature, fixes the defects listed below, adds a
diagnostic report and per-song sync correction, and makes the published
executable reproducible from source.

## History: findings and plan of the 1.7.0 audit

Ordered by priority. "Done" means implemented in this release; the evidence
column says how far each was verified.

| # | Priority | Finding | Outcome | Evidence |
| --- | --- | --- | --- | --- |
| 1 | Critical (process) | Both GitHub Actions workflows fail before running any step. Cause: every job is refused with *"The job was not started because your account is locked due to a billing issue."* It is an account-level lock, not a source or workflow fault. | Cannot be fixed from the repository. Documented with a local route (below). CI was extended so that it proves more once it can run. | GitHub check-run annotations on `main`; jobs show 0 steps |
| 2 | High | The published executable could not be reproduced: every link embedded a new timestamp and build GUID, so no rebuild ever matched the published SHA-256. | Done: `scripts/normalize_pe.py`, called by both build scripts. | Two builds, one with an emptied compiler cache, byte-identical; 4 tests |
| 3 | High | Titles containing *live*, *session*, *flip*, *rework*, *medley*, *slowed* or *reverb* as ordinary words were classed as special editions and mismatched or refused. | Done: these count only in brackets or a trailing segment. | Portable tests |
| 4 | High | Short bracketed subtitles were deleted when a tag word occurred inside another word. | Done: whole-word matching. | Portable tests |
| 5 | High | One global sync offset: correcting one song mistimed all others. | Done: per-song corrections, stored atomically, capped at 400 songs. | Store logic tested; UI wiring compiled only |
| 6 | High | No way to see why a lookup failed or what the app detected, short of reading the log. | Done: diagnostic report with a per-request lookup trail; redaction of the account name and credential-like values. | Redaction tested; report generation compiled only |
| 7 | Medium | An untimed cache entry was permanent, so timed lyrics added later were never found. | Done: re-checked after 7 days, cached words kept if nothing better. | Compiled only |
| 8 | Medium | A provider outage followed by an ordinary miss was reported as "not found" and not retried. | Done. | Compiled only |
| 9 | Medium | LRCLIB searches could use all 12 requests before lyrics.ovh was asked; identical URLs could be requested twice. | Done. | Compiled only |
| 10 | Medium | Interrupted updates left `.part`, staged and helper files behind; the replacement helper waited without limit for the old process. | Done: clean-up at start, 60 s bound. | Compiled only |
| 11 | Medium | Build scripts did not regenerate `release.json` and the site figures, so a build could leave them stale. | Done. | `tests/build_test.py`, `release_facts.py --check` |
| 12 | Low | Help listed `[` as "quieter / brighter"; it cycles darkness. Automatic update-check failures were not logged. | Done. | Compiled only |
| 13 | Low | In desktop mode a failed lookup shows only the scene, with no message. | Kept deliberately (an error banner across the desktop was judged worse); the reason is now in the diagnostic report. | — |
| 14 | Low | A title with no artist is split as "Title - Artist"; the other order is tried by the lookup chain, and every candidate still has to pass title, length and edition checks. | Left as is: no wrong lyrics result, only extra requests. | Read-through |
| 15 | Low | The updater validates size, SHA-256 and x64 PE headers, but the file is not code-signed. | Not addressable without a certificate; stated in the README. | — |

### GitHub Actions: the exact limitation and the local route

Workflow runs on this repository are created and then fail in about a second
with no steps and no runner. The annotation on each job reads: *"The job was
not started because your account is locked due to a billing issue."* Only the
account owner can clear this (GitHub → Settings → Billing and plans). No change
to the workflows or the source can make a job start, and the workflows
themselves were not the cause. Consequences:

- no Linux or Windows CI result exists for 1.6.0 or 1.7.0;
- the `windows-native` job, the only automated step that would execute the
  program on Windows, has never run;
- the *Stable release* workflow cannot publish, so the newest GitHub Release is
  still 1.4.0; the site (deployed by Vercel from `docs/`) is the release channel
  that works, and the updater reads it first.

Everything the Linux CI job does can be run locally, with the same pinned tools:

```sh
python3 -m venv .venv && . .venv/bin/activate
pip install ziglang==0.17.1 Pillow==13.0.0
export ZIG="$(python -c 'import pathlib, ziglang; print(pathlib.Path(ziglang.__file__).with_name("zig"))')"
./scripts/build.sh          # gated build; normalises and publishes to docs/downloads
./scripts/test.sh           # sanitizer core tests, tooling tests, docs and release facts
python scripts/verify_build.py dist/YoutubeLyrics.exe --ico docs/brand/YoutubeLyrics.ico
git diff --exit-code -- docs/downloads dist/YoutubeLyrics.exe   # the committed download is this build
```

When Actions is available again the CI workflow additionally (a) fails if a
fresh build differs from the committed download and (b) runs
`YoutubeLyrics.exe --diagnostics` on a Windows runner, which executes the
program for real and checks the report's version and redaction. Those two
steps were written but, for the reason above, have never been executed.

## Verification status

This table was written for 1.7.0 and still holds for 1.7.1, with these
changes: cache read/write format and the updater's image check are now
**Tested** (moved into the core); everything added in 1.7.1 on the Windows side
is **Unverified**.

**Tested** = executed here and passed. **Partly tested** = the decision logic
is covered by portable tests, the Windows code that calls it was only compiled.
**Unverified** = compiled and linked, never executed.

| Area | Status | What that rests on |
| --- | --- | --- |
| LRC / Enhanced LRC parsing, timestamps, active-line and word interpolation | Tested | portable tests, ASan/UBSan |
| Title and artist clean-up, edition detection, candidate scoring, duration rules, wrong-song rejection | Tested | portable tests, including the 1.7.0 regressions |
| lyrics.ovh payload validation, untimed pacing, local-file parsing | Tested | portable tests |
| Release-metadata parsing and version comparison used by the updater | Tested | portable tests, `tests/release_test.py` |
| Per-song offset store (parse, serialise, limits, eviction) | Tested | portable tests |
| Report redaction | Tested | portable tests |
| Build gate, PE/icon/version-resource verification, reproducibility, release facts, site links | Tested | `tests/build_test.py`, `verify_build.py`, `check_docs.py`, two identical builds |
| Settings window geometry | Tested (static) | `tests/settings_layout_test.py` re-runs the layout from the source table |
| Lookup chain, cache read/write/invalidation, retries, cancellation on track change | Partly tested | rules tested; WinHTTP and file code compiled only |
| Updater: detection, download, checksum, staging, replacement, rollback, clean-up | Partly tested | manifest/version logic tested; download, helper script and rollback never executed |
| Per-song sync keys, toast, status line; diagnostic report contents and export | Unverified | compiled only |
| Seek / pause / resume tracking, track transitions, media-session changes | Unverified | compiled only; needs real players |
| Desktop layer (WorkerW), Explorer restart, click-through, no-focus | Unverified | compiled only |
| Full screen / window switching, multi-monitor, mixed DPI, monitor removal | Unverified | compiled only |
| Tray icon and menu, global hotkeys, settings window appearance and keyboard use | Unverified | compiled only |
| Sleep/resume, shutdown during a network request | Unverified | compiled only |
| Windows 10 and Windows 11 compatibility | Unverified | manifest declares both; never launched |
| LRCLIB and lyrics.ovh live behaviour | Unverified | the sandbox network cannot reach either; no request was made |
| Rendering quality, contrast and typography on a real display | Unverified | no screenshot of the running app exists |

## History: verification performed for 1.7.0

Run on 14 November 2026 in this sandbox, on the 1.7.0 source:

- `./scripts/test.sh`, exit status 0:
  - **3,483 portable-core checks, 0 failures**, compiled with
    `-fsanitize=address,undefined` and `-Wall -Wextra`;
  - 4 release-metadata tests, 15 build-tooling tests (including 5 new ones for
    reproducibility and build-script consistency), 5 settings-layout tests;
  - `scripts/check_docs.py` and `scripts/release_facts.py --check`.
- `./scripts/build.sh`, Zig 0.17.1 / MinGW-w64 x86-64 Windows GUI cross-build,
  gated: icon → icon verification → resources → compile to a temporary path →
  normalise → `verify_build.py` → publish → re-verify → regenerate release
  manifest and site figures.
  - **Distributed build: 1,074,688 bytes (1050 KiB)**, SHA-256
    `23f775f63bc518d0f5c0303adcd9ba76841ece68dc7c8a87ee841aa270b3d4e3`.
  - `verify_build.py`: `0x8664 windows-gui`, version `1.7.0`, icon frames
    `[16, 20, 24, 32, 40, 48, 64, 96, 128, 256]`, wallpapers
    `city-rain.jpg, forest-mist.jpg, mountain-night.jpg`.
- **Reproducibility:** the build directory and the compiler cache were deleted
  and the build repeated; the result was byte-identical to the first.
- `dist/YoutubeLyrics.exe`, `docs/downloads/YoutubeLyrics.exe`, the `.sha256`
  sidecar and `release.json` agree.

One expectation written during this work was wrong and was corrected rather
than forced: a test assumed "Song (Stereo Hearts)" keeps its bracket. "Stereo"
is a genuine audio tag ("(Stereo)", "(Stereo Mix)"), a short bracket containing
it is still removed, and that case is now recorded as a known limitation below.

## Not verified / residual risks

- **No Windows runtime.** The executable has never been launched. Every row
  marked *Unverified* above is in that state, including all of the 1.6.0
  display-mode work and the 1.7.0 interface additions.
- **No CI.** See the Actions section; nothing has been checked by a second
  machine.
- **Providers untested live.** Availability, rate limits and result quality of
  LRCLIB and lyrics.ovh were not exercised.
- **No GitHub Release for 1.5.0–1.7.0.** An attempt to publish 1.6.0 by hand
  failed during the asset upload and left nothing behind; the release workflow
  is blocked by the Actions lock. Copies older than 1.6.0 therefore do not see
  an update and need one manual download.
- **Unsigned executable.** Windows SmartScreen will warn on first run. The
  checksum proves the download matches the site, not who built it.
- **Known matching limitation.** A short bracketed subtitle that contains a
  real tag word as a whole word (for example "(Stereo Hearts)", "(Radio
  Silence)") is still removed from the search title. The result is still
  validated against artist and length, so the risk is a missed match, not a
  wrong one.
- **No genuine screenshot.** `docs/img/display.png` is artwork generated by
  `scripts/brand.py` from the renderer's constants and `docs/preview.html` is a
  simulation; both are labelled as illustrative. 
- **No performance measurement.** No CPU, memory or start-up figures are quoted
  because none were measured.
- **No assistive-technology audit** of the site or the settings window.

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

The full matrix is in "Manual Windows test matrix" near the top; the shorter
list below is the 1.7.0 version, kept for reference.

The quickest first step on any Windows machine is
`YoutubeLyrics.exe --diagnostics report.txt`: it proves the program starts,
shows the detected Windows version and monitors, and tests each provider.

1. Windows 10 (1809+) and Windows 11: start, play music in YouTube Music and
   one other player, pause, seek, skip, close the player, sleep and resume, and
   quit during a slow lookup.
2. Desktop mode: lyrics behind the icons, clicks pass through, focus is never
   taken; restart Explorer and confirm the display and tray icon return.
3. `Ctrl+Alt+F`, `F11`, double-click and `Esc` across desktop / full screen /
   window; repeat with a second monitor at a different scale, then unplug it.
4. Sync: nudge a song with the arrows, skip away and back, restart the app, and
   confirm the correction returns for that song only.
5. Settings (`Ctrl+Alt+S`): every theme, Tab/Shift+Tab reach every control with
   a visible focus ring, Escape closes; save a diagnostic report and read it.
6. Updater: with an older build beside a newer published release, accept the
   update, cancel one mid-download, and disconnect the network during another.
7. Capture genuine screenshots only from the running application and keep the
   simulated preview labelled.

## History: the 1.5.0 review (changes since 1.4.0)

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

## History: 1.6.0

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

