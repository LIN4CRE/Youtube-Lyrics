#!/usr/bin/env bash
# Publishes the three verified release assets to GitHub Releases.
#
#   scripts/publish_release.sh <tag> <assets-dir> <target-commit>
#
# <assets-dir> must be the directory written by scripts/verify_release.py; this
# script uploads nothing else. It is safe to run again after any failure:
#
#   * The release is created as a DRAFT, the assets are uploaded, downloaded
#     back and compared byte for byte, and only then is the draft published.
#     An interrupted upload therefore never leaves a public release with a
#     missing or truncated executable.
#   * A draft left by an earlier attempt is reused and its assets replaced.
#   * A release that is already public is never modified. If its assets are the
#     ones being published the script succeeds without doing anything; if they
#     differ it fails, because a published version must not change.
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: $0 <tag> <assets-dir> <target-commit>" >&2
  exit 2
fi
TAG="$1"
ASSETS="$2"
TARGET="$3"
NAMES=(YoutubeLyrics.exe release.json YoutubeLyrics.exe.sha256)

if [[ ! "$TAG" =~ ^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]]; then
  echo "RELEASE BLOCKED: '$TAG' is not a stable vMAJOR.MINOR.PATCH tag" >&2
  exit 1
fi
for name in "${NAMES[@]}"; do
  if [[ ! -s "$ASSETS/$name" ]]; then
    echo "RELEASE BLOCKED: $ASSETS/$name is missing; run scripts/verify_release.py first" >&2
    exit 1
  fi
done
# The sidecar must describe the executable beside it (verify_release.py wrote
# both; this catches a directory that was edited or mixed up afterwards).
recorded="$(cut -d' ' -f1 "$ASSETS/YoutubeLyrics.exe.sha256")"
actual="$(sha256sum "$ASSETS/YoutubeLyrics.exe" | cut -d' ' -f1)"
if [[ "$recorded" != "$actual" ]]; then
  echo "RELEASE BLOCKED: the checksum file does not match the executable in $ASSETS" >&2
  exit 1
fi

scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT

# Downloads the release's assets and compares them with the verified ones.
remote_matches() {
  rm -rf "$scratch/remote"
  mkdir -p "$scratch/remote"
  if ! gh release download "$TAG" --dir "$scratch/remote" >/dev/null 2>&1; then
    return 1
  fi
  local name
  for name in "${NAMES[@]}"; do
    cmp -s "$ASSETS/$name" "$scratch/remote/$name" || return 1
  done
  # Nothing beyond the three expected files.
  [[ "$(find "$scratch/remote" -type f | wc -l)" -eq "${#NAMES[@]}" ]]
}

state="absent"
if is_draft="$(gh release view "$TAG" --json isDraft --jq .isDraft 2>/dev/null)"; then
  if [[ "$is_draft" == "true" ]]; then state="draft"; else state="published"; fi
fi

if [[ "$state" == "published" ]]; then
  if remote_matches; then
    echo "$TAG is already published with exactly these assets; nothing to do"
    exit 0
  fi
  echo "RELEASE BLOCKED: $TAG is already published with different assets." >&2
  echo "A published version is never replaced; fix the problem and release a new version." >&2
  exit 1
fi

if [[ "$state" == "absent" ]]; then
  gh release create "$TAG" --draft --title "Youtube-Lyrics ${TAG#v}" \
    --generate-notes --target "$TARGET"
  echo "created draft release $TAG"
else
  echo "reusing the draft release $TAG left by an earlier attempt"
fi

gh release upload "$TAG" "${NAMES[@]/#/$ASSETS/}" --clobber

if ! remote_matches; then
  echo "RELEASE BLOCKED: the uploaded assets do not match the verified build." >&2
  echo "The release has been left as a draft; run this again to retry." >&2
  exit 1
fi

gh release edit "$TAG" --draft=false --latest
echo "published $TAG ($actual)"
