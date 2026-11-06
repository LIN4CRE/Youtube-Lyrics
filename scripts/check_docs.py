#!/usr/bin/env python3
"""Check the static HTML pages for parse errors and broken local links."""
from __future__ import annotations

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

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        values = dict(attrs)
        if values.get("id"):
            self.ids.add(values["id"] or "")
        for name in ("href", "src"):
            value = values.get(name)
            if value:
                self.references.append((name, value))
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

    if errors:
        print("Documentation link check failed:")
        for error in errors:
            print(f"  - {error}")
        return 1

    for page in PAGES:
        print(f"checked {page.relative_to(ROOT)} ({len(parsed[page].references)} local/external refs)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
