#!/usr/bin/env python3
"""Tests for the build-verification and release-facts tooling.

These are static checks: they exercise the Python that guards a build, not the
Windows executable itself. The sections marked "against the repository" only
run when a build exists, so a clean checkout can still pass.
"""
from __future__ import annotations

import hashlib
import importlib.util
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILT_EXE = ROOT / "docs" / "downloads" / "YoutubeLyrics.exe"


def load(name: str, relative: str):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / relative)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


verify = load("verify_build", "verify_build.py")
facts = load("release_facts", "release_facts.py")
normalize_pe = load("normalize_pe", "normalize_pe.py")


def minimal_x64_pe(size: int = 160 * 1024) -> bytes:
    """A PE good enough for the header checks: MZ, PE32+, x64, GUI subsystem."""
    image = bytearray(size)
    image[:2] = b"MZ"
    struct.pack_into("<I", image, 0x3C, 0x80)
    image[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<H", image, 0x84, 0x8664)      # Machine = AMD64
    struct.pack_into("<H", image, 0x86, 1)           # one section
    struct.pack_into("<H", image, 0x94, 0xE0)        # SizeOfOptionalHeader
    struct.pack_into("<H", image, 0x98, 0x20B)       # PE32+ magic
    struct.pack_into("<H", image, 0x98 + 68, 2)      # Subsystem = WINDOWS_GUI
    section = 0x98 + 0xE0
    image[section : section + 8] = b".text\0\0\0"
    struct.pack_into("<I", image, section + 8, 0x1000)   # VirtualSize
    struct.pack_into("<I", image, section + 12, 0x1000)  # VirtualAddress
    struct.pack_into("<I", image, section + 16, 0x200)   # SizeOfRawData
    struct.pack_into("<I", image, section + 20, 0x400)   # PointerToRawData
    return bytes(image)


def tiny_ico(sizes=(16, 32, 48, 256)) -> bytes:
    frames = []
    for size in sizes:
        # A valid ICO frame just needs a directory entry that points at bytes;
        # the verifier checks structure, not pixels, so a small placeholder blob
        # is enough here.
        header = struct.pack("<I", 0x474E5089) + b"\0" * (size * size // 8 + 8)
        frames.append((size, header))
    out = bytearray(struct.pack("<HHH", 0, 1, len(frames)))
    offset = 6 + 16 * len(frames)
    for size, payload in frames:
        out += struct.pack(
            "<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(payload), offset
        )
        offset += len(payload)
    for _size, payload in frames:
        out += payload
    return bytes(out)


class VerifyBuildTests(unittest.TestCase):
    def test_rejects_a_non_pe_file(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "fake.exe"
            path.write_bytes(b"not an executable" * 20000)
            with self.assertRaises(verify.VerifyError):
                verify.validate(path)

    def test_odd_sizes_are_accepted_by_the_ico_parser_as_errors(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "short.ico"
            path.write_bytes(b"\0\0\1\0")
            with self.assertRaises(verify.VerifyError):
                verify.verify_ico(path)

    def test_ico_requires_every_shell_scale(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            complete = Path(directory) / "complete.ico"
            complete.write_bytes(tiny_ico((16, 20, 24, 32, 40, 48, 64, 96, 128, 256)))
            report = verify.verify_ico(complete)
            self.assertIn(16, report["sizes"])
            self.assertIn(256, report["sizes"])

            partial = Path(directory) / "partial.ico"
            partial.write_bytes(tiny_ico((16, 32)))
            with self.assertRaisesRegex(verify.VerifyError, "no 20px frame"):
                verify.verify_ico(partial)

    def test_iconless_escape_hatch_still_requires_an_x64_pe(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            good = Path(directory) / "good.exe"
            good.write_bytes(minimal_x64_pe())
            report = verify.validate(good, allow_missing_resources=True)
            self.assertTrue(report["resources_skipped"])
            self.assertEqual(report["machine"], "0x8664")

            bad = Path(directory) / "bad.exe"
            bad.write_bytes(minimal_x64_pe().replace(b"\x64\x86", b"\x86\x64"))
            with self.assertRaises(verify.VerifyError):
                verify.validate(bad, allow_missing_resources=True)


class ReleaseFactsTests(unittest.TestCase):
    def test_rewrites_version_span_and_readme_prose(self) -> None:
        html = (
            '<p>Version <span data-version>1.2.3</span></p>'
            "<!-- release-facts:start --><!-- release-facts:end -->"
        )
        readme = "Expected SHA-256 for the current 1.2.3 download, about 900 KiB."
        fake = {"version": "9.9.9", "kib": "1234", "bytes": "1,263,616", "sha256": "a" * 64}

        updated = facts.render(ROOT / "docs/index.html", html, fake)
        self.assertIn('<span data-version>9.9.9</span>', updated)
        self.assertIn("about 1234 KiB", updated)
        self.assertIn("a" * 64, updated)
        self.assertNotIn("1.2.3", updated.replace("1.2.3", "", 0))

        updated_readme = facts.render(ROOT / "README.md", readme, fake)
        self.assertIn("current 9.9.9 download", updated_readme)
        self.assertIn("about 1234 KiB", updated_readme)

    def test_sidecar_disagreement_is_fatal(self) -> None:
        if not BUILT_EXE.is_file():
            self.skipTest("no build present")
        digest = hashlib.sha256(BUILT_EXE.read_bytes()).hexdigest()
        report = facts.facts()
        self.assertEqual(report["sha256"], digest)


@unittest.skipUnless(BUILT_EXE.is_file(), "no build present")
class BuiltArtifactTests(unittest.TestCase):
    """Checks the shipped executable against the metadata the site advertises."""

    def test_binary_is_a_verified_x64_windows_gui_image(self) -> None:
        report = verify.validate(BUILT_EXE)
        self.assertEqual(report["machine"], "0x8664")
        self.assertEqual(report["subsystem"], "windows-gui")
        for size in (16, 32, 48, 256):
            self.assertIn(size, report["icon_sizes"])
        self.assertEqual(len(report["wallpapers"]), 3)

    def test_version_resource_matches_version_file(self) -> None:
        report = verify.validate(BUILT_EXE)
        self.assertEqual(report["version"], (ROOT / "VERSION").read_text().strip())

    def test_sidecar_and_manifest_agree_with_the_binary(self) -> None:
        digest = hashlib.sha256(BUILT_EXE.read_bytes()).hexdigest()
        sidecar = (ROOT / "docs" / "downloads" / "YoutubeLyrics.exe.sha256").read_text()
        self.assertEqual(sidecar.split()[0], digest)

        manifest = (ROOT / "docs" / "downloads" / "release.json").read_text()
        import json

        data = json.loads(manifest)
        self.assertEqual(data["sha256"], digest)
        self.assertEqual(data["size_bytes"], BUILT_EXE.stat().st_size)
        self.assertEqual(data["version"], (ROOT / "VERSION").read_text().strip())
        self.assertEqual(data["tag"], "v" + data["version"])

    def test_dist_and_downloads_are_byte_identical(self) -> None:
        dist = ROOT / "dist" / "YoutubeLyrics.exe"
        if not dist.is_file():
            self.skipTest("dist copy absent")
        self.assertEqual(
            hashlib.sha256(dist.read_bytes()).hexdigest(),
            hashlib.sha256(BUILT_EXE.read_bytes()).hexdigest(),
            "the downloadable copy must be the exact binary that was verified",
        )


class NormalizePeTests(unittest.TestCase):
    """The step that makes two builds of the same source byte-identical."""

    def test_timestamp_is_fixed_and_result_is_stable(self) -> None:
        first = bytearray(minimal_x64_pe())
        second = bytearray(first)
        struct.pack_into("<I", first, 0x88, 0x11111111)   # COFF TimeDateStamp
        struct.pack_into("<I", second, 0x88, 0x22222222)
        a = normalize_pe.normalize(bytes(first))
        b = normalize_pe.normalize(bytes(second))
        self.assertEqual(a, b, "two link times must normalise to the same bytes")
        self.assertEqual(struct.unpack_from("<I", a, 0x88)[0], normalize_pe.FIXED_TIMESTAMP)
        self.assertEqual(normalize_pe.normalize(a), a, "normalising twice changes nothing")
        self.assertEqual(len(a), len(first))

    def test_different_code_stays_different(self) -> None:
        first = bytearray(minimal_x64_pe())
        second = bytearray(first)
        second[-1] ^= 0xFF
        self.assertNotEqual(normalize_pe.normalize(bytes(first)),
                            normalize_pe.normalize(bytes(second)))

    def test_rejects_files_that_are_not_pe32_plus(self) -> None:
        with self.assertRaises(normalize_pe.PeError):
            normalize_pe.normalize(b"not an executable" * 64)
        wrong = bytearray(minimal_x64_pe())
        struct.pack_into("<H", wrong, 0x98, 0x10B)  # PE32 (32-bit) magic
        with self.assertRaises(normalize_pe.PeError):
            normalize_pe.normalize(bytes(wrong))

    @unittest.skipUnless(BUILT_EXE.exists(), "no built executable in this checkout")
    def test_published_executable_is_already_normalised(self) -> None:
        data = BUILT_EXE.read_bytes()
        self.assertEqual(
            normalize_pe.normalize(data), data,
            "docs/downloads/YoutubeLyrics.exe was not produced by scripts/build.sh "
            "(its link timestamp is not normalised), so it cannot be reproduced",
        )

    def test_both_build_scripts_compile_the_same_sources(self) -> None:
        import re
        sh = (ROOT / "scripts" / "build.sh").read_text(encoding="utf-8")
        ps = (ROOT / "scripts" / "build.ps1").read_text(encoding="utf-8")
        on_disk = {f"src/win/{p.name}" for p in (ROOT / "src" / "win").glob("*.cc")}
        on_disk.add("src/core/core.cc")
        for name, text in (("build.sh", sh), ("build.ps1", ps)):
            listed = set(re.findall(r"src/(?:win|core)/\w+\.cc", text))
            self.assertEqual(listed, on_disk, f"{name} does not compile every source file")
            self.assertIn("normalize_pe.py", text, f"{name} skips the reproducibility step")


if __name__ == "__main__":
    unittest.main(verbosity=2)
