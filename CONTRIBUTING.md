# Contributing to Youtube-Lyrics

Thanks for taking the time to improve Youtube-Lyrics. The project is intentionally small, native, and privacy-conscious; contributions should keep that character.

## Before opening a pull request

- Search existing issues and pull requests first. For a significant feature, open an issue to discuss the design before investing in a large change.
- Keep reports focused on Youtube-Lyrics. Do not include copyrighted lyrics, private track history, access tokens, or unredacted log files.
- Be clear about what you tested. A Linux test or Windows cross-build is not proof that the program ran correctly on Windows.

## Build and test

The portable C++17 core tests run on Linux or macOS with `g++`:

```sh
./scripts/test.sh
```

The test suite uses AddressSanitizer and UndefinedBehaviorSanitizer. To build the Windows x64 GUI executable from Linux or macOS, install Zig and Python 3 with Pillow, then run:

```sh
./scripts/build.sh
```

On Windows, use `scripts/build.ps1` with Zig and Python 3 + Pillow available. The build writes `dist/YoutubeLyrics.exe`; the checked-in public download lives at `docs/downloads/YoutubeLyrics.exe`.

## Project-specific guidelines

- Keep platform-independent parsing, matching, and layout logic in `src/core/`; put Windows APIs in `src/win/`.
- Do not add audio capture, telemetry, account requirements, bundled lyrics, or unnecessary background services.
- Preserve the distinction between source-synchronized lyric timing and estimated navigation. Never present estimated timing as genuine karaoke sync.
- Update the README, download page, `CHANGELOG.md`, and app version resources when a change affects users.
- If changing the executable, rebuild it from the same source, copy the result to `docs/downloads/YoutubeLyrics.exe`, and verify that the two distributable copies match. Mention the SHA-256 in release notes when publishing a release.
- Keep generated art labeled honestly. `docs/img/display.png` and `docs/preview.html` are illustrative/simulated, not screenshots of the running Windows application.
- Do not commit `.cache/`, `.build/`, font downloads, temporary logs, or local test output.

## Pull request checklist

- [ ] The change has a clear, focused purpose.
- [ ] `./scripts/test.sh` passes, or the PR explains why it could not be run.
- [ ] User-facing documentation and the changelog are updated where needed.
- [ ] New or changed Windows-only behavior is explicitly marked as untested unless exercised on Windows.
- [ ] No private track data, copyrighted lyrics, credentials, or generated build caches are included.
