#!/usr/bin/env python3
"""Check the static HTML pages for parse errors and broken local links.

Beyond links, this enforces the release facts that are easy to let drift:
  * the version and download size printed on the page must match VERSION and
    the real docs/downloads/YoutubeLyrics.exe,
  * the SHA-256 printed on the page must match the sidecar and the executable,
  * every <img> must carry an alt attribute, and
  * every "simulated"/"illustrative" preview must say so in its caption.
"""
from __future__ import annotations

import hashlib
import re
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parent.parent
PAGES = (ROOT / "docs/index.html", ROOT / "docs/preview.html")


class Links(HTMLParser):
    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.ids: set[str] = set()
        self.references: list[tuple[str, str]] = []
        self.title_text = ""
        self.in_title = False
        self.images: list[tuple[str, str | None]] = []

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        values = dict(attrs)
        if values.get("id"):
            self.ids.add(values["id"] or "")
        for name in ("href", "src"):
            value = values.get(name)
            if value:
                self.references.append((name, value))
        if tag.lower() == "img":
            self.images.append((values.get("src") or "", values.get("alt")))
        if tag.lower() == "title":
            self.in_title = True

    def handle_endtag(self, tag: str) -> None:
        if tag.lower() == "title":
            self.in_title = False

    def handle_data(self, data: str) -> None:
        if self.in_title:
            self.title_text += data


def parse(path: Path) -> Links:
    parser = Links()
    parser.feed(path.read_text(encoding="utf-8"))
    parser.close()
    if not parser.title_text.strip():
        raise ValueError(f"{path.relative_to(ROOT)} has no non-empty <title>")
    return parser


def release_facts() -> tuple[str, int, str]:
    version = (ROOT / "VERSION").read_text(encoding="ascii").strip()
    exe = ROOT / "docs" / "downloads" / "YoutubeLyrics.exe"
    if not exe.is_file():
        raise SystemExit("docs/downloads/YoutubeLyrics.exe is missing; run scripts/build.sh")
    digest = hashlib.sha256(exe.read_bytes()).hexdigest()
    sidecar = ROOT / "docs" / "downloads" / "YoutubeLyrics.exe.sha256"
    sidecar_hash = sidecar.read_text(encoding="ascii").split()[0].strip().lower()
    if sidecar_hash != digest:
        raise SystemExit(
            "docs/downloads/YoutubeLyrics.exe.sha256 does not match the executable "
            f"({sidecar_hash} != {digest}); rebuild rather than editing the sidecar"
        )
    return version, exe.stat().st_size, digest


def check_release_facts(path: Path, text: str, facts: tuple[str, int, str]) -> list[str]:
    version, size, digest = facts
    errors: list[str] = []

    for found in set(re.findall(r"\b\d+\.\d+\.\d+\b", text)):
        if found != version:
            errors.append(f"{path.name}: mentions version {found}, VERSION is {version}")

    for found in re.findall(r"about (\d+) KiB", text):
        # Allow a 1 KiB rounding window for how the page describes the file.
        if abs(int(found) - size / 1024.0) > 1.0:
            errors.append(
                f"{path.name}: says about {found} KiB, the executable is "
                f"{size / 1024.0:.0f} KiB"
            )

    for found in re.findall(r"\b[0-9a-f]{64}\b", text):
        if found != digest:
            errors.append(f"{path.name}: prints SHA-256 {found[:12]}…, build is {digest[:12]}…")

    return errors


def main() -> int:
    parsed = {page: parse(page) for page in PAGES}
    errors: list[str] = []

    for page, parser in parsed.items():
        for _attribute, reference in parser.references:
            parts = urlsplit(reference)
            if parts.scheme or parts.netloc or reference.startswith(("mailto:", "data:")):
                continue

            target = (page.parent / unquote(parts.path)).resolve() if parts.path else page
            if not target.exists():
                errors.append(f"{page.relative_to(ROOT)}: missing local target {reference!r}")
                continue

            if parts.fragment and target in parsed and parts.fragment not in parsed[target].ids:
                errors.append(
                    f"{page.relative_to(ROOT)}: missing fragment #{parts.fragment} in "
                    f"{target.relative_to(ROOT)}"
                )

    facts = release_facts()
    for page in PAGES:
        text = page.read_text(encoding="utf-8")
        errors.extend(check_release_facts(page, text, facts))
        for src, alt in parsed[page].images:
            if alt is None:
                errors.append(f"{page.relative_to(ROOT)}: <img src={src!r}> has no alt attribute")
        if "Illustrative" in text or "illustrative" in text:
            if "not a screenshot" not in text:
                errors.append(
                    f"{page.relative_to(ROOT)}: contains a simulated preview but does not "
                    "say it is not a screenshot from the running app"
                )

    for document in (ROOT / "README.md",):
        text = document.read_text(encoding="utf-8")
        version = facts[0]
        for found in set(re.findall(r"\b\d+\.\d+\.\d+\b", text)):
            if found != version:
                errors.append(f"{document.name}: mentions version {found}, VERSION is {version}")

    if errors:
        print("Documentation check failed:")
        for error in errors:
            print(f"  - {error}")
        return 1

    for page in PAGES:
        print(f"checked {page.relative_to(ROOT)} ({len(parsed[page].references)} local/external refs)")
    print(f"release facts: version {facts[0]}, {facts[1] / 1024.0:.0f} KiB, sha256 {facts[2][:12]}…")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
