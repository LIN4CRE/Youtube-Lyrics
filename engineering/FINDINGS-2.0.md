# Youtube-Lyrics 2.0 — audit findings

Baseline: `main` at `80d70cc` (1.8.0). Reviewed 14 November 2026 in a Linux
sandbox with the Zig cross toolchain. **No Windows machine was available:
nothing here is a runtime observation unless it says so.**

Scope of this pass, stated honestly:

- Read in full for this pass: `src/win/desktop.cc`, the display-mode, timer,
  tray, config and start-up paths of `src/win/app.cc`, `src/win/app.h`, the
  desktop path of `src/win/render.cc`, `src/win/settings.cc`, the desktop
  section of `src/win/diag.cc`, every script in `scripts/`, the tests, both
  workflows, `docs/downloads`, README, CHANGELOG, AUDIT, VERSION.
- Not re-read line by line in this pass: `src/win/media.cc`,
  `src/win/lyrics.cc`, `src/win/update.cc`, most of `src/core/core.cc`. They
  were audited for 1.7.0 and 1.7.1 (see `AUDIT.md`) and were not changed by
  1.8.0; their tests still pass. Findings for them below come from tracing the
  call sites, not from a fresh full review.

Status: **Fixed** = changed on this branch and covered by a test that runs
here. **Changed, unverified** = changed on this branch but only provable on
Windows. **Open** = not done.

| # | Severity | Finding | Evidence | Affected | Action / status |
| --- | --- | --- | --- | --- | --- |
| F1 | Critical | The production site serves the unverified 1.8.0 build as the **stable** release, and the in-app updater installs from it. | Live `downloads/release.json` on 14 Nov: `"channel": "stable", "version": "1.8.0"`; Vercel production deployment `dpl_6u2n8eTT596t9EAnepRfiKifj1bg` built from `80d70cc`; `CHANGELOG.md`/`AUDIT.md` call the same build unreleased and unverified. | `docs/downloads/*`, production | **Fixed on this branch, not yet in production.** `docs/downloads` is restored byte-for-byte to the 1.7.1 release (SHA-256 `fcba7110…cf3b`). Production changes only when this branch is merged or the Vercel deployment is rolled back; see the PR description. |
| F2 | Critical | Root cause of F1: every build overwrote `docs/downloads`, so *any* merge to `main` published whatever was built. The only gate was a sentence in a PR description. | `scripts/build.sh` step 5, `scripts/build.ps1` step 5 | build scripts, tests, CI | **Fixed.** New `RELEASE_CHANNEL` file (`development` or `stable`). Builds touch `docs/downloads` only when it says `stable`. `tests/build_test.py` fails if the stable channel advertises the version under development. |
| F3 | High | Nothing recorded that a release had been run on Windows; the release gate checked bytes and metadata only. | `scripts/verify_release.py` | release gate | **Fixed.** The gate now refuses unless `RELEASE_CHANNEL` is `stable` and `release/acceptance.json` names this exact binary (SHA-256) with `desktop_test` and `manual_matrix` both `pass`, a Windows build and a tester. Tested in `tests/release_test.py`. |
| F4 | High | Users who already took the 1.8.0 update stay on it: the updater only moves forward. | `core.cc` release comparison (`parsed.channel != "stable"`, newer-only) | updater | **Open by design.** Not downgraded automatically. Development builds are numbered 1.9.x so those users are offered the real 2.0.0 when it exists. Noted in CHANGELOG. |
| F5 | High | When Windows refused per-pixel drawing, the layer became an opaque child painting a bundled scene over the user's wallpaper. | `desktop.cc` (1.8.0) `d.opaque` path | `desktop.cc`, `render.cc` | **Fixed (logic tested; Windows behaviour unverified).** Removed. Five consecutive refusals raise `DrawRefused`: the layer is destroyed, nothing is shown, the reason is reported. |
| F6 | High | Attachment state was a set of flags written from several places (`state`, `lost`, `attempts`, `failure_notified`), including directly from `app.cc` and `settings.cc`. It had no "attaching" or "recovering" state and could not be tested. | `app.cc` 609–612, 916–918; `settings.cc` 829–834 (1.8.0) | `core`, `desktop.cc`, callers | **Fixed.** Pure state machine in `src/core` (`DesktopMachine`): off / attaching / attached / restoring / recovering / failed. Only `AttachSucceeded`, sent after the parent and styles are read back, reaches *attached*. 300+ directed checks and a 20,000-step random walk in `tests/core_test.cc`. |
| F7 | High | Primary monitor only. | `target_monitor()` (1.8.0) | `desktop.cc`, settings | **Changed, unverified.** One layer window per chosen display; choice is *all* (default), *main only* or one display, stored as `desktop_monitors`. Selection logic is tested; placement is not. See limitation L1. |
| F8 | Medium | A display change only moved the existing window; a removed or re-ordered monitor was not handled, and a change that arrived without `WM_DISPLAYCHANGE` was missed. | `desktop_layer_tick` (1.8.0) | `desktop.cc` | **Changed, unverified.** The arrangement of displays is fingerprinted; any difference (message or 2-second poll) rebuilds every layer through the *recovering* state. |
| F9 | Medium | A layer that ended up *not* beneath the icons was still reported as attached ("icon order not confirmed"). | `create_layer` (1.8.0) | `desktop.cc` | **Changed, unverified.** In the shared-parent layouts that is now an attachment failure. |
| F10 | Medium | On a song change the old words vanished in one frame while the veil faded, then the new ones faded in. | `reset_document` + presence code | `app.cc`, `desktop.cc` | **Changed, unverified.** The last frame of the old song is faded out as a still (never re-rendered, so it cannot pick up the new song's position); the new song starts fading in only after it has gone. No cross-fade: two songs are never on screen together. |
| F11 | Medium | `--desktop-test` reported `PASS` for facts it had not measured (for example "visible" when a window covered the desktop) and had no way to say "not checked". Screenshot failure was a note. | `run_desktop_test` (1.8.0) | `desktop.cc` | **Changed, unverified.** PASS / FAIL / SKIP per check; exit code 0 / 1 / 2; screen-pixel check of transparent drawing that is SKIPped when the desktop is covered; screenshot failure is a FAIL with the reason; the report lists what the test cannot show. |
| F12 | Medium | No launch-at-startup option. | — | `app.cc`, settings | **Changed, unverified.** *Start with Windows* writes a per-user `Run` entry; the path is corrected on launch if the portable exe was moved. |
| F13 | Medium | No single local verification command on Windows. | — | `scripts/verify.ps1` | **Written, never executed.** |
| F14 | Low | README, site and CHANGELOG described different things as "current". | see F1 | docs | **Fixed** for the files; the site copy is the 1.7.1 page again. `check_docs.py` now compares the site and README with the published `release.json`, not with `VERSION`. |
| F15 | Low | CI compared `dist` with `docs/downloads` unconditionally. | `ci.yml` | CI | **Fixed** (channel-aware). CI itself still cannot run (account billing lock). |

## Not done in this pass

These parts of the 2.0 brief are **open**. None is claimed.

| # | Item | Why |
| --- | --- | --- |
| O1 | Any Windows run: start-up, attachment, real playback, Explorer restart, sleep, displays, autostart (brief phases 8–9) | No Windows machine. Needs the owner; see `WINDOWS_TEST_MATRIX.md`. |
| O2 | Performance measurements at 1080p / 1440p / 4K (phase 7) | Needs Windows. The report now records average / maximum / last frame time and frames shown, so the numbers can be read off a diagnostic report. |
| O3 | Native per-display layout (limitation L1) | Deliberately deferred until the single-display layer is proven on Windows. |
| O4 | Text position, alignment, font, text and highlight colour, line spacing, active-line emphasis controls (phase 5) | Not started. Fade speed and text shadow were added; theme (colours), text size and background darkness already existed. |
| O5 | Fresh line-by-line review of `media.cc`, `lyrics.cc`, `update.cc` | Not repeated in this pass. |
| O6 | Installer | Not started; the portable exe is kept. |
| O7 | Windows 10 | Still claimed by the manifest and README, never run. Either test it or drop the claim before 2.0.0. |

## Known limitations of what was built

- **L1 — displays of different sizes.** Lyrics are laid out once, for the
  largest chosen display. A display of the same pixel size shows the identical
  frame. A display of a different size shows that frame scaled down to fit,
  centred. Text there is slightly softer than a native layout would be, and the
  scaling costs time on every frame (unmeasured).
- **L2 — one sheet on every display.** The same lyrics appear on each chosen
  display; there is no per-display content.
- **L3 — classic shell layout.** With icons and wallpaper in separate
  top-level windows, "beneath the icons" is inferred from which `WorkerW` was
  found, not re-measured.
