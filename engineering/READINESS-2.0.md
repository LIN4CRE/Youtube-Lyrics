# Youtube-Lyrics 2.0.0 — readiness report

**Verdict: not ready. 2.0.0 is not complete and must not be tagged, released or
promoted.** The essential acceptance criteria all depend on behaviour on
Windows, and no Windows run has been made.

Date: 14 November 2026. Development builds are numbered 1.9.0.
`RELEASE_CHANNEL` is `development`.

Evidence available: source on this branch; `scripts/build.sh` (cross-build,
no warnings); `scripts/test.sh` (all suites pass). There are no screenshots and
no Windows reports, so none are linked.

| Acceptance criterion | Status | Evidence |
| --- | --- | --- |
| Stable channel serves only a verified release | **Partially verified** | Fixed in the repository and tested (`tests/build_test.py`, `tests/release_test.py`); production still served 1.8.0 when this was written (`FINDINGS-2.0.md` F1) |
| Runs unnoticed; no window on start | Not verified | compiled only |
| Lyrics appear automatically when music plays | Not verified | compiled only |
| Lyrics are part of the wallpaper, behind icons | Not verified | compiled only; `--desktop-test` never run |
| Behind every application window | Not verified | compiled only |
| Never takes focus; clicks pass through; icons usable | Not verified | compiled only |
| User's wallpaper preserved; nothing substituted on failure | Partially verified | opaque fallback removed; state-machine behaviour tested (`tests/core_test.cc`); not seen on Windows |
| Attachment state is real, not a setting | Partially verified | state machine tested; Win32 verification code not run |
| Recovers from Explorer restart, sleep, display change | Not verified | transitions tested in the state machine only |
| Multiple displays, mixed DPI | Not verified | selection logic tested; scaled-copy limitation L1 |
| Correct, synchronised lyrics; no stale or wrong-song words | Not verified | portable matching/timing tests pass; no real playback |
| Pause behaves naturally; stop fades to a clean desktop | Not verified | compiled only |
| Visual controls listed in the brief | Failed (incomplete) | fade speed, text shadow, caption, pause dimming, theme, size, darkness exist; position, alignment, font, colours, spacing, emphasis do not |
| Start with Windows | Not verified | compiled only |
| Settings, lyrics, corrections preserved | Partially verified | config keys only added; never run |
| Performance measured | Not verified | instrumentation added; no measurements |
| One-command local verification | Not verified | `scripts/verify.ps1` written, never executed |
| Windows 10 support claim | Not verified | never run |
| Release metadata consistent | Verified (repository) | `check_docs.py`, `release_facts.py --check`, `tests/build_test.py` |
| Updater cannot offer a development build | Verified (repository) / Not verified (production) | as first row |
| Final independent review | Not done | cannot precede Windows validation |

What has to happen next, in order: merge or roll back to fix production (F1);
run `scripts\verify.ps1` on Windows 11 and return the report; fix what it
finds; work through `WINDOWS_TEST_MATRIX.md`; finish the open items in
`FINDINGS-2.0.md`; only then switch `RELEASE_CHANNEL` to `stable`.
