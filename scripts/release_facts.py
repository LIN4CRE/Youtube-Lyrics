#!/usr/bin/env python3
"""Rewrite generated release facts in the docs and README from the real build.

The version, file size and SHA-256 printed on the website and in the README are
not editorial copy: they describe a specific executable. Hand-editing them is
how a page ends up advertising a download that no longer exists, so they live
between markers and are rewritten from VERSION plus the built binary.

    python3 scripts/release_facts.py            # rewrite in place
    python3 scripts/release_facts.py --check    # exit 1 if anything is stale
"""
from __future__ import annotations

import argparse
import hashlib
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MARKER = re.compile(r"(<!-- release-facts:start -->)(.*?)(<!-- release-facts:end -->)", re.S)
# The version appears as an HTML span in the site and as a bare number in the
# README, so it is written as data, not as markup.
VERSION_SPAN = re.compile(r'(<span data-version>)([^<]*)(</span>)')
README_VERSION = re.compile(r"(current )\d+\.\d+\.\d+( download)")


def facts() -> dict[str, str]:
    # The site and README describe the published stable download, which is not
    # necessarily the version under development in VERSION.
    import json as _json
    version = _json.loads(
        (ROOT / "docs" / "downloads" / "release.json").read_text(encoding="utf-8"))["version"]
    exe = ROOT / "docs" / "downloads" / "YoutubeLyrics.exe"
    if not exe.is_file():
        raise SystemExit("docs/downloads/YoutubeLyrics.exe is missing; run scripts/build.sh first")
    image = exe.read_bytes()
    digest = hashlib.sha256(image).hexdigest()
    sidecar = ROOT / "docs" / "downloads" / "YoutubeLyrics.exe.sha256"
    if sidecar.is_file():
        recorded = sidecar.read_text(encoding="ascii").split()[0].strip().lower()
        if recorded != digest:
            raise SystemExit(
                f"{sidecar.relative_to(ROOT)} records {recorded} but the executable is "
                f"{digest}; rebuild instead of editing the sidecar"
            )
    return {
        "version": version,
        "kib": f"{len(image) / 1024.0:.0f}",
        "bytes": f"{len(image):,}",
        "sha256": digest,
    }


def render(page: Path, text: str, f: dict[str, str]) -> str:
    replacement = (
        f'<p class="file-note"><strong>YoutubeLyrics.exe</strong> · 64-bit · about '
        f'{f["kib"]} KiB ({f["bytes"]} bytes) · portable Windows x64 build</p>\n'
        f'          <p class="file-note">No installer, account, or administrator rights '
        f'required. The download is served from this site.</p>\n'
        f'          <p class="file-note">SHA-256: <code>{f["sha256"]}</code> · '
        f'<a href="downloads/YoutubeLyrics.exe.sha256">checksum file</a></p>'
    )
    text = MARKER.sub(
        lambda m: m.group(1) + "\n          " + replacement + "\n          " + m.group(3),
        text,
    )
    text = VERSION_SPAN.sub(lambda m: m.group(1) + f["version"] + m.group(3), text)
    text = README_VERSION.sub(lambda m: m.group(1) + f["version"] + m.group(2), text)
    if page.suffix == ".md":
        # Keep the README's prose description of the build in step too.
        text = re.sub(r"about \d+ KiB", f'about {f["kib"]} KiB', text)
    return text


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="report drift without writing")
    args = parser.parse_args()

    f = facts()
    stale: list[str] = []
    for relative in ("docs/index.html", "README.md"):
        path = ROOT / relative
        original = path.read_text(encoding="utf-8")
        updated = render(path, original, f)
        if updated == original:
            print(f"{relative}: release facts up to date")
            continue
        stale.append(relative)
        if args.check:
            print(f"{relative}: release facts are stale")
        else:
            path.write_text(updated, encoding="utf-8")
            print(f"{relative}: updated to {f['version']} · {f['kib']} KiB · {f['sha256'][:12]}…")

    if args.check and stale:
        print("run scripts/release_facts.py to refresh generated release facts", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
