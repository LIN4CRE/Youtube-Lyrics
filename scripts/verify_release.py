#!/usr/bin/env python3
"""Gate for publishing a stable release. Exits non-zero unless everything agrees.

A GitHub Release must be the very file the project site already serves, built
from the source being tagged. This script proves that before anything is
uploaded, and it is the only thing that writes the release assets:

  1. VERSION is a stable MAJOR.MINOR.PATCH version and the tag is v<VERSION>.
  2. The freshly built executable passes the structural checks: x86-64 PE32+
     Windows GUI image, icon group with every shell size, embedded scenes and a
     version resource equal to VERSION (scripts/verify_build.py).
  3. The build is byte-for-byte identical to the committed
     docs/downloads/YoutubeLyrics.exe.
  4. Its SHA-256 equals the committed docs/downloads/YoutubeLyrics.exe.sha256.
  5. The committed docs/downloads/release.json states exactly this version,
     tag, repository, channel, architecture, asset name, size and checksum,
     and nothing else.
  6. The three release assets are then generated from the verified build and
     compared, byte for byte, with the committed copies.

The committed files are read from Git (``git show <ref>:path``), not from the
working tree, because scripts/build.sh overwrites the working-tree copies with
whatever it has just built; comparing a build with itself would prove nothing.

    python3 scripts/verify_release.py --tag v1.7.1
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REPOSITORY = "LIN4CRE/Youtube-Lyrics"
ASSET = "YoutubeLyrics.exe"
SIDECAR = "YoutubeLyrics.exe.sha256"
MANIFEST = "release.json"
VERSION_RE = re.compile(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\Z")
SIDECAR_RE = re.compile(r"([0-9a-f]{64})  YoutubeLyrics\.exe\n\Z")


class ReleaseError(Exception):
    """A reason this source must not be published."""


def _load(name: str):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / f"{name}.py")
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    sys.modules.setdefault(name, module)
    spec.loader.exec_module(module)
    return module


def check_version_and_tag(version: str, tag: str) -> None:
    if not VERSION_RE.fullmatch(version):
        raise ReleaseError(f"VERSION is not a stable MAJOR.MINOR.PATCH version: {version!r}")
    if tag != f"v{version}":
        raise ReleaseError(f"tag {tag!r} does not match VERSION {version!r} (expected 'v{version}')")


def expected_manifest(version: str, tag: str, repository: str, image: bytes) -> dict[str, object]:
    return {
        "schema_version": 1,
        "repository": repository,
        "tag": tag,
        "version": version,
        "channel": "stable",
        "architecture": "x64",
        "asset": ASSET,
        "size_bytes": len(image),
        "sha256": hashlib.sha256(image).hexdigest(),
    }


def check_consistency(
    *,
    version: str,
    tag: str,
    repository: str,
    built: bytes,
    published_exe: bytes,
    published_sidecar: str,
    published_manifest: str,
) -> dict[str, object]:
    """Checks 1 and 3-5. Returns the manifest every copy must equal."""
    check_version_and_tag(version, tag)
    if repository != REPOSITORY:
        raise ReleaseError(f"unexpected repository {repository!r}; releases come from {REPOSITORY}")

    built_hash = hashlib.sha256(built).hexdigest()
    published_hash = hashlib.sha256(published_exe).hexdigest()
    if built != published_exe:
        raise ReleaseError(
            "the build is not the file the site serves: built "
            f"{len(built):,} bytes sha256 {built_hash}, committed docs/downloads/{ASSET} "
            f"{len(published_exe):,} bytes sha256 {published_hash}. Run ./scripts/build.sh with "
            "the pinned toolchain, commit the result, and tag that commit."
        )

    match = SIDECAR_RE.fullmatch(published_sidecar)
    if not match:
        raise ReleaseError(
            f"docs/downloads/{SIDECAR} is not exactly '<64 lowercase hex>  {ASSET}\\n'"
        )
    if match.group(1) != built_hash:
        raise ReleaseError(
            f"docs/downloads/{SIDECAR} records {match.group(1)}, the executable is {built_hash}"
        )

    try:
        manifest = json.loads(published_manifest)
    except json.JSONDecodeError as exc:
        raise ReleaseError(f"docs/downloads/{MANIFEST} is not valid JSON: {exc}") from exc
    if not isinstance(manifest, dict):
        raise ReleaseError(f"docs/downloads/{MANIFEST} is not a JSON object")
    expected = expected_manifest(version, tag, repository, built)
    for key, value in expected.items():
        if key not in manifest:
            raise ReleaseError(f"docs/downloads/{MANIFEST} has no {key!r}")
        # bool is an int in Python; a manifest saying true for a size is wrong.
        if type(manifest[key]) is not type(value) or manifest[key] != value:
            raise ReleaseError(
                f"docs/downloads/{MANIFEST} says {key} = {manifest[key]!r}, "
                f"the release is {value!r}"
            )
    extra = sorted(set(manifest) - set(expected))
    if extra:
        raise ReleaseError(f"docs/downloads/{MANIFEST} has unexpected fields: {', '.join(extra)}")
    return expected


def write_assets(
    built_path: Path, output_dir: Path, version: str, tag: str, repository: str
) -> dict[str, Path]:
    """Check 6, first half: assets come from the verified build, nothing else."""
    generate_release = _load("generate_release")
    output_dir.mkdir(parents=True, exist_ok=True)
    for name in (ASSET, SIDECAR, MANIFEST):
        stale = output_dir / name
        if stale.exists():
            stale.unlink()
    try:
        generate_release.generate(built_path, output_dir, repository, version, tag)
    except (OSError, ValueError) as exc:
        raise ReleaseError(f"could not generate the release metadata: {exc}") from exc
    (output_dir / ASSET).write_bytes(built_path.read_bytes())
    return {name: output_dir / name for name in (ASSET, SIDECAR, MANIFEST)}


def check_assets(
    assets: dict[str, Path], published_exe: bytes, published_sidecar: str, published_manifest: str
) -> None:
    """Check 6, second half: what will be uploaded equals what the site serves."""
    committed = {
        ASSET: published_exe,
        SIDECAR: published_sidecar.encode("utf-8"),
        MANIFEST: published_manifest.encode("utf-8"),
    }
    for name, path in assets.items():
        if path.read_bytes() != committed[name]:
            raise ReleaseError(
                f"the release asset {name} differs from the committed docs/downloads/{name}"
            )


def git_show(ref: str, path: str) -> bytes:
    result = subprocess.run(
        ["git", "-C", str(ROOT), "show", f"{ref}:{path}"], capture_output=True, check=False
    )
    if result.returncode != 0:
        raise ReleaseError(
            f"{path} is not committed at {ref}: {result.stderr.decode('utf-8', 'replace').strip()}"
        )
    return result.stdout


ACCEPTANCE_CHECKS = ("desktop_test", "manual_matrix")


def check_acceptance(text: str, version: str, sha256: str) -> dict:
    """The human sign-off: this exact binary was run on a real Windows PC.

    release/acceptance.json is written by a person after running
    scripts/verify.ps1 and the manual matrix in engineering/WINDOWS_TEST_MATRIX.md.
    It names the binary by hash, so it cannot be carried over to another build.
    """
    try:
        record = json.loads(text)
    except json.JSONDecodeError as exc:
        raise ReleaseError(f"release/acceptance.json is not valid JSON: {exc}") from exc
    if not isinstance(record, dict):
        raise ReleaseError("release/acceptance.json is not a JSON object")
    if record.get("version") != version:
        raise ReleaseError(
            f"release/acceptance.json is for version {record.get('version')!r}, not {version!r}")
    if record.get("sha256") != sha256:
        raise ReleaseError(
            "release/acceptance.json was recorded for a different binary "
            f"({record.get('sha256')!r}); the Windows checks must be repeated on this build")
    for key in ("windows_build", "tested_by", "date"):
        if not isinstance(record.get(key), str) or not record[key].strip():
            raise ReleaseError(f"release/acceptance.json has no {key!r}")
    for key in ACCEPTANCE_CHECKS:
        if record.get(key) != "pass":
            raise ReleaseError(
                f"release/acceptance.json: {key} is {record.get(key)!r}, not 'pass'")
    return record


def check_channel(text: str) -> None:
    if text.strip() != "stable":
        raise ReleaseError(
            f"RELEASE_CHANNEL is {text.strip()!r}: this commit is a development build "
            "and cannot be published as a stable release")


def run(tag: str, built_path: Path, ref: str, output_dir: Path, repository: str) -> dict[str, object]:
    check_channel(git_show(ref, "RELEASE_CHANNEL").decode("ascii", "replace"))
    version = git_show(ref, "VERSION").decode("ascii", "replace").strip()
    working_version = (ROOT / "VERSION").read_text(encoding="ascii").strip()
    if working_version != version:
        raise ReleaseError(
            f"VERSION in the working tree ({working_version!r}) differs from {ref} ({version!r})"
        )
    check_version_and_tag(version, tag)

    if not built_path.is_file():
        raise ReleaseError(f"{built_path} does not exist; run ./scripts/build.sh first")
    verify_build = _load("verify_build")
    try:
        report = verify_build.validate(built_path)
    except verify_build.VerifyError as exc:
        raise ReleaseError(f"the build failed structural verification: {exc}") from exc
    if report.get("version") != version:
        raise ReleaseError(
            f"the executable's version resource is {report.get('version')!r}, VERSION is {version!r}"
        )

    built = built_path.read_bytes()
    check_acceptance(git_show(ref, "release/acceptance.json").decode("utf-8", "replace"),
                     version, hashlib.sha256(built).hexdigest())
    published_exe = git_show(ref, f"docs/downloads/{ASSET}")
    published_sidecar = git_show(ref, f"docs/downloads/{SIDECAR}").decode("utf-8", "replace")
    published_manifest = git_show(ref, f"docs/downloads/{MANIFEST}").decode("utf-8", "replace")
    manifest = check_consistency(
        version=version,
        tag=tag,
        repository=repository,
        built=built,
        published_exe=published_exe,
        published_sidecar=published_sidecar,
        published_manifest=published_manifest,
    )
    assets = write_assets(built_path, output_dir, version, tag, repository)
    check_assets(assets, published_exe, published_sidecar, published_manifest)
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--tag", required=True, help="the stable tag being published, e.g. v1.7.1")
    parser.add_argument("--built", type=Path, default=ROOT / "dist" / ASSET)
    parser.add_argument("--ref", default="HEAD", help="commit whose docs/downloads is authoritative")
    parser.add_argument("--output-dir", type=Path, default=ROOT / ".build" / "release")
    parser.add_argument("--repository", default=REPOSITORY)
    args = parser.parse_args()
    try:
        manifest = run(args.tag, args.built, args.ref, args.output_dir, args.repository)
    except ReleaseError as exc:
        print(f"RELEASE BLOCKED: {exc}", file=sys.stderr)
        return 1
    print(
        f"release {manifest['tag']} verified: {manifest['size_bytes']:,} bytes, "
        f"sha256 {manifest['sha256']}; assets written to {args.output_dir}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
