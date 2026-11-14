#!/usr/bin/env python3
"""Unit tests for stable-release asset and checksum generation."""
from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location(
    "generate_release", ROOT / "scripts" / "generate_release.py"
)
assert SPEC and SPEC.loader
release = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(release)

GATE_SPEC = importlib.util.spec_from_file_location(
    "verify_release", ROOT / "scripts" / "verify_release.py"
)
assert GATE_SPEC and GATE_SPEC.loader
gate = importlib.util.module_from_spec(GATE_SPEC)
sys.modules["verify_release"] = gate
GATE_SPEC.loader.exec_module(gate)


def fake_x64_pe(size: int = 160 * 1024) -> bytes:
    image = bytearray(size)
    image[:2] = b"MZ"
    struct.pack_into("<I", image, 0x3C, 0x80)
    image[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<H", image, 0x84, 0x8664)
    return bytes(image)


class ReleaseMetadataTests(unittest.TestCase):
    def test_generate_manifest_and_sidecar_for_x64_pe(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            executable = root / "YoutubeLyrics.exe"
            executable.write_bytes(fake_x64_pe())
            output = root / "release"

            metadata = release.generate(
                executable,
                output,
                "LIN4CRE/Youtube-Lyrics",
                "1.4.0",
                "v1.4.0",
            )
            digest = hashlib.sha256(executable.read_bytes()).hexdigest()
            parsed = json.loads((output / "release.json").read_text(encoding="utf-8"))
            sidecar = (output / "YoutubeLyrics.exe.sha256").read_text(encoding="ascii")

            self.assertEqual(metadata, parsed)
            self.assertEqual(parsed["schema_version"], 1)
            self.assertEqual(parsed["repository"], "LIN4CRE/Youtube-Lyrics")
            self.assertEqual(parsed["tag"], "v1.4.0")
            self.assertEqual(parsed["version"], "1.4.0")
            self.assertEqual(parsed["channel"], "stable")
            self.assertEqual(parsed["architecture"], "x64")
            self.assertEqual(parsed["asset"], "YoutubeLyrics.exe")
            self.assertEqual(parsed["size_bytes"], len(fake_x64_pe()))
            self.assertEqual(parsed["sha256"], digest)
            self.assertEqual(sidecar, f"{digest}  YoutubeLyrics.exe\n")

    def test_rejects_tag_repository_and_architecture_mismatches(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "YoutubeLyrics.exe"
            executable.write_bytes(fake_x64_pe())
            output = Path(directory) / "release"
            common = (executable, output)
            with self.assertRaisesRegex(ValueError, "must match VERSION"):
                release.generate(*common, "LIN4CRE/Youtube-Lyrics", "1.4.0", "v1.4.1")
            with self.assertRaisesRegex(ValueError, "unexpected release repository"):
                release.generate(*common, "other/repository", "1.4.0", "v1.4.0")

            image = bytearray(executable.read_bytes())
            struct.pack_into("<H", image, 0x84, 0x014C)
            executable.write_bytes(image)
            with self.assertRaisesRegex(ValueError, "x86-64 PE"):
                release.generate(*common, "LIN4CRE/Youtube-Lyrics", "1.4.0", "v1.4.0")

    def test_rejects_invalid_pe_and_size(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "YoutubeLyrics.exe"
            output = Path(directory) / "release"
            executable.write_bytes(b"not an executable")
            with self.assertRaisesRegex(ValueError, "size must be between"):
                release.generate(executable, output, "LIN4CRE/Youtube-Lyrics", "1.4.0", "v1.4.0")

            executable.write_bytes(b"\0" * (160 * 1024))
            with self.assertRaisesRegex(ValueError, "DOS header"):
                release.generate(executable, output, "LIN4CRE/Youtube-Lyrics", "1.4.0", "v1.4.0")

    def test_rejects_unstable_or_malformed_versions(self) -> None:
        for version in ("1.4", "1.4.0-rc.1", "01.4.0", "1.4.0+build"):
            with self.subTest(version=version), self.assertRaises(ValueError):
                release.validate_version(version)


def consistent_set(version: str = "1.7.1") -> dict:
    """A build and the three committed files that correctly describe it."""
    image = fake_x64_pe()
    digest = hashlib.sha256(image).hexdigest()
    manifest = gate.expected_manifest(version, f"v{version}", gate.REPOSITORY, image)
    return {
        "version": version,
        "tag": f"v{version}",
        "repository": gate.REPOSITORY,
        "built": image,
        "published_exe": image,
        "published_sidecar": f"{digest}  YoutubeLyrics.exe\n",
        "published_manifest": json.dumps(manifest, indent=2, sort_keys=True) + "\n",
    }


class ReleaseGateTests(unittest.TestCase):
    """scripts/verify_release.py: every inconsistency must block the release."""

    def blocked(self, pattern: str, **changes) -> None:
        values = consistent_set()
        values.update(changes)
        with self.assertRaisesRegex(gate.ReleaseError, pattern):
            gate.check_consistency(**values)

    def test_a_consistent_release_passes(self) -> None:
        values = consistent_set()
        manifest = gate.check_consistency(**values)
        self.assertEqual(manifest["tag"], "v1.7.1")
        self.assertEqual(manifest["size_bytes"], len(values["built"]))

    def test_tag_and_version_must_agree_and_be_stable(self) -> None:
        self.blocked("does not match VERSION", tag="v1.7.0")
        self.blocked("does not match VERSION", tag="1.7.1")
        self.blocked("not a stable", version="1.7.1-rc.1", tag="v1.7.1-rc.1")
        self.blocked("not a stable", version="1.7", tag="v1.7")
        self.blocked("unexpected repository", repository="someone/fork")

    def test_a_rebuild_that_differs_from_the_site_copy_is_refused(self) -> None:
        other = bytearray(fake_x64_pe())
        other[-1] ^= 1   # one bit, same size
        self.blocked("not the file the site serves", built=bytes(other))
        self.blocked("not the file the site serves", published_exe=bytes(other))

    def test_checksum_file_must_match_exactly(self) -> None:
        digest = hashlib.sha256(fake_x64_pe()).hexdigest()
        self.blocked("records", published_sidecar=f"{'0' * 64}  YoutubeLyrics.exe\n")
        self.blocked("is not exactly", published_sidecar=f"{digest.upper()}  YoutubeLyrics.exe\n")
        self.blocked("is not exactly", published_sidecar=f"{digest}  Other.exe\n")
        self.blocked("is not exactly", published_sidecar=f"{digest}\n")
        self.blocked("is not exactly", published_sidecar="")

    def test_every_manifest_field_is_checked(self) -> None:
        good = json.loads(consistent_set()["published_manifest"])
        wrong = {
            "schema_version": 2,
            "repository": "someone/fork",
            "tag": "v1.7.0",
            "version": "1.7.0",
            "channel": "preview",
            "architecture": "arm64",
            "asset": "Other.exe",
            "size_bytes": good["size_bytes"] + 1,
            "sha256": "0" * 64,
        }
        self.assertEqual(set(wrong), set(good), "a manifest field has no mismatch test")
        for key, value in wrong.items():
            with self.subTest(field=key):
                changed = dict(good, **{key: value})
                self.blocked(f"says {key}", published_manifest=json.dumps(changed))
                missing = {k: v for k, v in good.items() if k != key}
                self.blocked(f"has no '{key}'", published_manifest=json.dumps(missing))
        self.blocked("unexpected fields", published_manifest=json.dumps(dict(good, extra=1)))
        self.blocked("says size_bytes", published_manifest=json.dumps(dict(good, size_bytes=True)))
        self.blocked("says size_bytes",
                     published_manifest=json.dumps(dict(good, size_bytes=str(good["size_bytes"]))))
        self.blocked("not valid JSON", published_manifest="{")
        self.blocked("not a JSON object", published_manifest="[]")

    def test_assets_are_written_from_the_build_and_compared(self) -> None:
        values = consistent_set()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            built = root / "YoutubeLyrics.exe"
            built.write_bytes(values["built"])
            output = root / "release"
            output.mkdir()
            (output / "release.json").write_text("stale", encoding="utf-8")
            assets = gate.write_assets(built, output, "1.7.1", "v1.7.1", gate.REPOSITORY)
            self.assertEqual(sorted(p.name for p in output.iterdir()),
                             ["YoutubeLyrics.exe", "YoutubeLyrics.exe.sha256", "release.json"])
            gate.check_assets(assets, values["published_exe"], values["published_sidecar"],
                              values["published_manifest"])
            # A committed manifest that is equivalent JSON but not the same
            # bytes is still a difference between the release and the site.
            compact = json.dumps(json.loads(values["published_manifest"]))
            with self.assertRaisesRegex(gate.ReleaseError, "release.json differs"):
                gate.check_assets(assets, values["published_exe"], values["published_sidecar"],
                                  compact)

    def test_the_workflow_runs_the_gate_before_publishing(self) -> None:
        text = (ROOT / ".github" / "workflows" / "stable-release.yml").read_text(encoding="utf-8")
        order = [text.index(marker) for marker in (
            "check_version_and_tag",
            "merge-base --is-ancestor",
            "./scripts/test.sh",
            "./scripts/build.sh",
            "git diff --exit-code",
            "scripts/verify_release.py --tag",
            "./scripts/publish_release.sh",
        )]
        self.assertEqual(order, sorted(order), "release steps are out of order")
        self.assertEqual(text.count("gh release"), 0,
                         "publishing must go through scripts/publish_release.sh only")
        self.assertNotIn("continue-on-error", text)
        self.assertNotIn("|| true", text)


FAKE_GH = r"""#!/usr/bin/env python3
import json, os, shutil, sys
from pathlib import Path
state_dir = Path(os.environ['FAKE_GH_STATE'])
state_file = state_dir / 'release.json'
state = json.loads(state_file.read_text()) if state_file.exists() else None
args = sys.argv[1:]
with open(state_dir / 'calls.log', 'a') as log:
    log.write(' '.join(args) + '\n')
def save(value):
    state_file.write_text(json.dumps(value))
if args[:2] == ['release', 'view']:
    if state is None:
        sys.exit(1)
    print('true' if state['draft'] else 'false')
elif args[:2] == ['release', 'create']:
    if state is not None:
        sys.exit(1)
    assert '--draft' in args, 'a release must be created as a draft'
    save({'draft': True, 'assets': []})
elif args[:2] == ['release', 'upload']:
    assert state is not None and state['draft'], 'uploads only go to a draft'
    assert '--clobber' in args
    (state_dir / 'assets').mkdir(exist_ok=True)
    for source in args[3:]:
        if source.startswith('--'):
            continue
        target = state_dir / 'assets' / Path(source).name
        shutil.copyfile(source, target)
        if os.environ.get('FAKE_GH_CORRUPT') == target.name:
            target.write_bytes(target.read_bytes()[:-7])
        if target.name not in state['assets']:
            state['assets'].append(target.name)
    save(state)
elif args[:2] == ['release', 'download']:
    if state is None:
        sys.exit(1)
    target = Path(args[args.index('--dir') + 1])
    for name in state['assets']:
        shutil.copyfile(state_dir / 'assets' / name, target / name)
elif args[:2] == ['release', 'edit']:
    assert state is not None and '--draft=false' in args
    state['draft'] = False
    save(state)
else:
    sys.exit(f'unexpected gh call: {args}')
"""


@unittest.skipUnless(shutil.which("bash") and shutil.which("sha256sum"), "needs bash and coreutils")
class AcceptanceGateTests(unittest.TestCase):
    """A stable release needs a human Windows sign-off for the exact binary."""

    SHA = "a" * 64

    def record(self, **changes) -> str:
        data = {
            "version": "2.0.0",
            "sha256": self.SHA,
            "windows_build": "Windows 11 26100.1",
            "tested_by": "someone",
            "date": "2026-12-01",
            "desktop_test": "pass",
            "manual_matrix": "pass",
        }
        data.update(changes)
        return json.dumps({k: v for k, v in data.items() if v is not None})

    def test_a_complete_record_for_this_binary_passes(self) -> None:
        self.assertEqual(gate.check_acceptance(self.record(), "2.0.0", self.SHA)["tested_by"],
                         "someone")

    def test_everything_else_is_refused(self) -> None:
        cases = [
            ("not valid JSON", "{", {}),
            ("different binary", None, {"sha256": "b" * 64}),
            ("is for version", None, {"version": "1.9.0"}),
            ("no 'windows_build'", None, {"windows_build": None}),
            ("no 'tested_by'", None, {"tested_by": "  "}),
            ("desktop_test is 'incomplete'", None, {"desktop_test": "incomplete"}),
            ("manual_matrix is None", None, {"manual_matrix": None}),
        ]
        for pattern, raw, changes in cases:
            with self.subTest(pattern=pattern):
                text = raw if raw is not None else self.record(**changes)
                with self.assertRaisesRegex(gate.ReleaseError, pattern):
                    gate.check_acceptance(text, "2.0.0", self.SHA)

    def test_a_development_commit_cannot_be_released(self) -> None:
        gate.check_channel("stable\n")
        for text in ("development\n", "", "Stable"):
            with self.assertRaisesRegex(gate.ReleaseError, "development build"):
                gate.check_channel(text)


class PublishScriptTests(unittest.TestCase):
    """scripts/publish_release.sh against a stand-in for the gh command."""

    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.state = self.root / "state"
        self.state.mkdir()
        bin_dir = self.root / "bin"
        bin_dir.mkdir()
        gh = bin_dir / "gh"
        gh.write_text(FAKE_GH, encoding="utf-8")
        gh.chmod(gh.stat().st_mode | stat.S_IEXEC)
        self.assets = self.root / "release"
        built = self.root / "YoutubeLyrics.exe"
        built.write_bytes(fake_x64_pe())
        gate.write_assets(built, self.assets, "1.7.1", "v1.7.1", gate.REPOSITORY)
        self.env = dict(os.environ, FAKE_GH_STATE=str(self.state),
                        PATH=f"{bin_dir}{os.pathsep}{os.environ['PATH']}")

    def tearDown(self) -> None:
        self.temp.cleanup()

    def publish(self, tag: str = "v1.7.1", **env) -> subprocess.CompletedProcess:
        return subprocess.run(
            ["bash", str(ROOT / "scripts" / "publish_release.sh"), tag, str(self.assets), "abc123"],
            env=dict(self.env, **env), capture_output=True, text=True, check=False)

    def remote(self) -> dict | None:
        path = self.state / "release.json"
        return json.loads(path.read_text()) if path.exists() else None

    def calls(self) -> str:
        path = self.state / "calls.log"
        return path.read_text() if path.exists() else ""

    def test_publishes_as_draft_then_makes_it_public(self) -> None:
        result = self.publish()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.remote()["draft"])
        self.assertEqual(sorted(self.remote()["assets"]),
                         ["YoutubeLyrics.exe", "YoutubeLyrics.exe.sha256", "release.json"])
        calls = self.calls()
        self.assertLess(calls.index("release create"), calls.index("release upload"))
        self.assertLess(calls.index("release upload"), calls.index("release download"))
        self.assertLess(calls.index("release download"), calls.index("release edit"))

    def test_running_again_after_success_changes_nothing(self) -> None:
        self.assertEqual(self.publish().returncode, 0)
        before = self.calls()
        again = self.publish()
        self.assertEqual(again.returncode, 0, again.stderr)
        self.assertIn("already published", again.stdout)
        added = self.calls()[len(before):]
        self.assertNotIn("upload", added)
        self.assertNotIn("edit", added)
        self.assertNotIn("create", added)

    def test_a_corrupted_upload_is_never_made_public_and_can_be_retried(self) -> None:
        failed = self.publish(FAKE_GH_CORRUPT="YoutubeLyrics.exe")
        self.assertNotEqual(failed.returncode, 0)
        self.assertIn("RELEASE BLOCKED", failed.stderr)
        self.assertTrue(self.remote()["draft"], "a bad upload must stay a draft")
        self.assertNotIn("release edit", self.calls())
        retried = self.publish()
        self.assertEqual(retried.returncode, 0, retried.stderr)
        self.assertIn("reusing the draft", retried.stdout)
        self.assertFalse(self.remote()["draft"])
        published = (self.state / "assets" / "YoutubeLyrics.exe").read_bytes()
        self.assertEqual(published, (self.assets / "YoutubeLyrics.exe").read_bytes())

    def test_a_published_release_is_never_replaced(self) -> None:
        self.assertEqual(self.publish().returncode, 0)
        changed = bytearray((self.assets / "YoutubeLyrics.exe").read_bytes())
        changed[-1] ^= 1
        (self.assets / "YoutubeLyrics.exe").write_bytes(changed)
        digest = hashlib.sha256(changed).hexdigest()
        (self.assets / "YoutubeLyrics.exe.sha256").write_text(f"{digest}  YoutubeLyrics.exe\n")
        before = self.calls()
        result = self.publish()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("already published with different assets", result.stderr)
        self.assertNotIn("upload", self.calls()[len(before):])

    def test_refuses_incomplete_or_inconsistent_asset_directories(self) -> None:
        (self.assets / "YoutubeLyrics.exe.sha256").write_text(f"{'0' * 64}  YoutubeLyrics.exe\n")
        result = self.publish()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does not match the executable", result.stderr)
        (self.assets / "release.json").unlink()
        result = self.publish()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("release.json is missing", result.stderr)
        self.assertEqual(self.calls(), "", "gh must not be called for a blocked release")

    def test_refuses_tags_that_are_not_stable_versions(self) -> None:
        for tag in ("1.7.1", "v1.7", "v1.7.1-rc1", "latest"):
            with self.subTest(tag=tag):
                result = self.publish(tag=tag)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("not a stable", result.stderr)
        self.assertEqual(self.calls(), "")


if __name__ == "__main__":
    unittest.main(verbosity=2)
