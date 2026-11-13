#!/usr/bin/env bash
# Builds dist/YoutubeLyrics.exe.
#
# Runs on Linux (or macOS) using Zig's bundled MinGW-w64 toolchain + resource
# compiler, so no Windows machine and no Visual Studio are needed.
#   - Linux/macOS: ZIG=/path/to/zig PYTHON=/path/to/python ./scripts/build.sh
#   - Windows:     build.ps1 (see README) or the same command with zig on PATH
#
# The build is a gate, not a suggestion:
#   * the icon and version resources are mandatory; if they cannot be generated
#     or compiled the build fails instead of producing an iconless executable
#   * the finished PE is verified (x64, GUI subsystem, icon frames, embedded
#     wallpapers, version resource) before anything is copied to docs/downloads
#   * docs/downloads is only replaced after that verification passes, and the
#     previous executable is restored if the copy fails
#
# Set YTL_ALLOW_ICONLESS_BUILD=1 only for throwaway experiments; it is never
# used by CI or the release workflow.
set -euo pipefail
cd "$(dirname "$0")/.."

ZIG="${ZIG:-zig}"
PYTHON="${PYTHON:-python3}"
OUT_DIR="dist"
BUILD_DIR=".build"
ALLOW_ICONLESS="${YTL_ALLOW_ICONLESS_BUILD:-0}"

fail() { printf 'build failed: %s\n' "$*" >&2; exit 1; }

command -v "$PYTHON" >/dev/null 2>&1 || fail "python3 not found (set PYTHON=/path/to/python)"

ZIG_BIN="$ZIG"
if ! command -v "$ZIG_BIN" >/dev/null 2>&1; then
  # `python3 -m ziglang` is a supported way to run the pinned pip package.
  if "$PYTHON" -c 'import ziglang' >/dev/null 2>&1; then
    ZIG_BIN="$("$PYTHON" -c 'import pathlib, ziglang; print(pathlib.Path(ziglang.__file__).with_name("zig"))')"
  fi
fi
[ -x "$ZIG_BIN" ] || fail "zig not found (set ZIG=/path/to/zig, or pip install ziglang)"

mkdir -p "$OUT_DIR" "$BUILD_DIR"
"$PYTHON" scripts/generate_version.py || fail "version header generation failed"

# 1. Embed the PowerShell media helper into the binary as a raw string literal.
"$PYTHON" scripts/generate_smtc.py || fail "media-helper script generation failed"

# 2. Application icon + version info -> COFF .res object.
RES_ARG=""
if ! "$PYTHON" -c 'import PIL' >/dev/null 2>&1; then
  if [ "$ALLOW_ICONLESS" = "1" ]; then
    echo "note: Pillow not installed; YTL_ALLOW_ICONLESS_BUILD=1 set, continuing without resources" >&2
  else
    fail "Pillow is required to embed the application icon and version resource.
       Install it with 'pip install Pillow', or set YTL_ALLOW_ICONLESS_BUILD=1 for a
       throwaway build with no icon (never for a release)."
  fi
else
  "$PYTHON" scripts/make_icon.py "$BUILD_DIR" || fail "icon generation failed"
  "$PYTHON" scripts/make_icon.py --verify "$BUILD_DIR/app.ico" || fail "generated icon failed verification"
  "$ZIG_BIN" rc /i "$BUILD_DIR" /i src/win src/win/app.rc "$BUILD_DIR/app.res" \
    || fail "resource compilation failed (icon and version resource are mandatory)"
  [ -s "$BUILD_DIR/app.res" ] || fail "resource compiler produced an empty object"
  RES_ARG="$BUILD_DIR/app.res"
fi

# 3. Cross compile. Written to a temporary name first so a failed link can
#    never truncate a working dist/YoutubeLyrics.exe.
TEMP_EXE="$BUILD_DIR/YoutubeLyrics.exe"
rm -f "$TEMP_EXE"
"$ZIG_BIN" c++ -target x86_64-windows-gnu -std=c++17 -O2 \
  -DUNICODE -D_UNICODE \
  -municode -Wl,--subsystem,windows -Wl,--gc-sections \
  -fno-exceptions -fno-rtti -fno-strict-aliasing \
  -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type \
  -I"$BUILD_DIR" \
  -o "$TEMP_EXE" \
  src/core/core.cc \
  src/win/app.cc \
  src/win/render.cc \
  src/win/media.cc \
  src/win/lyrics.cc \
  src/win/update.cc \
  src/win/settings.cc \
  $RES_ARG \
  -lgdiplus -lgdi32 -luser32 -lole32 -luuid -lshell32 -lshlwapi -ldwmapi \
  -lwinhttp -lcomdlg32 -ladvapi32 -lbcrypt -lcomctl32 \
  || fail "compile or link failed"

# Some Zig/MinGW versions leave an unused import archive beside the executable.
rm -f "$BUILD_DIR/core.lib" "$BUILD_DIR/YoutubeLyrics.pdb"

# 4. Verify the actual image, not the build log.
if [ -n "$RES_ARG" ]; then
  "$PYTHON" scripts/verify_build.py "$TEMP_EXE" --ico "$BUILD_DIR/app.ico" \
    || fail "the built executable failed verification"
else
  echo "warning: building without resources (YTL_ALLOW_ICONLESS_BUILD=1)" >&2
  "$PYTHON" - "$TEMP_EXE" <<'PY' || fail "the built executable is not a usable x64 PE"
import sys
from pathlib import Path
image = Path(sys.argv[1]).read_bytes()
assert image[:2] == b"MZ", "missing DOS header"
pe = int.from_bytes(image[0x3C:0x40], "little")
assert image[pe:pe + 4] == b"PE\0\0", "invalid PE signature"
assert int.from_bytes(image[pe + 4:pe + 6], "little") == 0x8664, "not x86-64"
print(f"validated unverified x86-64 executable ({len(image):,} bytes)")
PY
fi

# 5. Publish. The public copy is replaced only now, and rolled back if the copy
#    or the checksum sidecar cannot be written.
HASH=""
if command -v sha256sum >/dev/null 2>&1; then
  HASH="$(sha256sum "$TEMP_EXE" | awk '{print $1}')"
else
  HASH="$(shasum -a 256 "$TEMP_EXE" | awk '{print $1}')"
fi
[ -n "$HASH" ] || fail "could not compute a SHA-256 checksum"

PUBLISH_DIR="$OUT_DIR/YoutubeLyrics.exe"
DOWNLOADS="docs/downloads/YoutubeLyrics.exe"
DOWNLOADS_BACKUP=""
if [ -f "$DOWNLOADS" ]; then
  DOWNLOADS_BACKUP="$BUILD_DIR/YoutubeLyrics.exe.previous"
  cp "$DOWNLOADS" "$DOWNLOADS_BACKUP"
fi

restore_previous() {
  if [ -n "$DOWNLOADS_BACKUP" ] && [ -f "$DOWNLOADS_BACKUP" ]; then
    cp "$DOWNLOADS_BACKUP" "$DOWNLOADS" || true
    echo "build failed: restored the previous docs/downloads copy" >&2
  fi
  exit 1
}
trap restore_previous ERR

# The distributable and the public copy must be byte-identical.
mv "$TEMP_EXE" "$PUBLISH_DIR"
cp "$PUBLISH_DIR" "$DOWNLOADS"
printf '%s  YoutubeLyrics.exe\n' "$HASH" > docs/downloads/YoutubeLyrics.exe.sha256
trap - ERR

# Re-verify the published copy so "downloadable == verified" is a checked fact.
if [ -n "$RES_ARG" ]; then
  "$PYTHON" scripts/verify_build.py "$DOWNLOADS" >/dev/null \
    || fail "the published copy does not match the verified build"
else
  "$PYTHON" scripts/verify_build.py "$DOWNLOADS" --allow-missing-resources >/dev/null \
    || fail "the published copy does not match the verified build"
fi

echo "built $PUBLISH_DIR ($(wc -c < "$PUBLISH_DIR") bytes)"
echo "SHA-256: $HASH"
echo "published $DOWNLOADS (byte-identical)"
