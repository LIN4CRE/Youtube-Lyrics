#!/usr/bin/env python3
"""Create the stable-release manifest and SHA-256 sidecar for Youtube-Lyrics.

The updater accepts only this small, strict manifest beside a stable x64
GitHub Release. The executable is bounded and checked as an x86-64 PE before
any release metadata is written.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import struct
from pathlib import Path

MAX_EXE_BYTES = 64 * 1024 * 1024
MIN_EXE_BYTES = 128 * 1024
DEFAULT_REPOSITORY = "LIN4CRE/Youtube-Lyrics"
VERSION_RE = re.compile(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\Z")


def validate_version(version: str) -> None:
    if not VERSION_RE.fullmatch(version):
        raise ValueError(f"not a stable MAJOR.MINOR.PATCH version: {version!r}")


def validate_x64_pe(data: bytes) -> None:
    if len(data) < MIN_EXE_BYTES or len(data) > MAX_EXE_BYTES:
        raise ValueError(f"executable size must be between {MIN_EXE_BYTES} and {MAX_EXE_BYTES} bytes")
    if data[:2] != b"MZ" or len(data) < 0x40:
        raise ValueError("executable has no valid DOS header")
    pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
    if pe_offset < 0x40 or pe_offset > len(data) - 6 or pe_offset > (1 << 20):
        raise ValueError("executable has an invalid PE header offset")
    if data[pe_offset : pe_offset + 4] != b"PE\0\0":
        raise ValueError("executable has no valid PE signature")
    machine = struct.unpack_from("<H", data, pe_offset + 4)[0]
    if machine != 0x8664:
        raise ValueError(f"expected an x86-64 PE executable, found machine 0x{machine:04x}")


def generate(exe: Path, output_dir: Path, repository: str, version: str, tag: str) -> dict[str, object]:
    validate_version(version)
    if tag != f"v{version}":
        raise ValueError(f"tag {tag!r} must match VERSION as v{version}")
    if repository != DEFAULT_REPOSITORY:
        raise ValueError(f"unexpected release repository {repository!r}")

    image = exe.read_bytes()
    validate_x64_pe(image)
    digest = hashlib.sha256(image).hexdigest()
    metadata: dict[str, object] = {
        "schema_version": 1,
        "repository": repository,
        "tag": tag,
        "version": version,
        "channel": "stable",
        "architecture": "x64",
        "asset": "YoutubeLyrics.exe",
        "size_bytes": len(image),
        "sha256": digest,
    }

    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "release.json").write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    (output_dir / "YoutubeLyrics.exe.sha256").write_text(
        f"{digest}  YoutubeLyrics.exe\n", encoding="ascii"
    )
    return metadata


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=Path("dist/YoutubeLyrics.exe"))
    parser.add_argument("--output-dir", type=Path, default=Path(".build/release"))
    parser.add_argument("--repository", default=os.environ.get("GITHUB_REPOSITORY", DEFAULT_REPOSITORY))
    parser.add_argument("--version-file", type=Path, default=Path("VERSION"))
    parser.add_argument("--tag", default=None, help="expected release tag; defaults to v<VERSION>")
    args = parser.parse_args()

    version = args.version_file.read_text(encoding="ascii").strip()
    tag = args.tag or f"v{version}"
    try:
        result = generate(args.exe, args.output_dir, args.repository, version, tag)
    except (OSError, ValueError, struct.error) as exc:
        parser.error(str(exc))
    print(
        f"prepared {result['tag']} release metadata: {result['size_bytes']:,} bytes, "
        f"SHA-256 {result['sha256']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
