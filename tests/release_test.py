#!/usr/bin/env python3
"""Unit tests for stable-release asset and checksum generation."""
from __future__ import annotations

import hashlib
import importlib.util
import json
import struct
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


if __name__ == "__main__":
    unittest.main(verbosity=2)
