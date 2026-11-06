#!/usr/bin/env python3
"""Generate C++ and Windows resource version headers from the single VERSION file."""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VERSION_FILE = ROOT / "VERSION"
BUILD_DIR = ROOT / ".build"


def main() -> int:
    raw = VERSION_FILE.read_text(encoding="ascii").strip()
    match = re.fullmatch(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", raw)
    if not match:
        raise SystemExit(f"VERSION must be a stable semantic version (MAJOR.MINOR.PATCH), got {raw!r}")
    major, minor, patch = map(int, match.groups())
    if any(part > 65535 for part in (major, minor, patch)):
        raise SystemExit("Windows version components must fit in 16 bits")

    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    (BUILD_DIR / "version.h").write_text(
        "// Generated from VERSION by scripts/generate_version.py. Do not edit.\n"
        "#pragma once\n"
        f"#define YTL_VERSION_MAJOR {major}\n"
        f"#define YTL_VERSION_MINOR {minor}\n"
        f"#define YTL_VERSION_PATCH {patch}\n"
        f'#define YTL_VERSION_STRING "{raw}"\n'
        f'#define YTL_VERSION_WIDE L"{raw}"\n',
        encoding="utf-8",
    )
    (BUILD_DIR / "version.rcinc").write_text(
        "// Generated from VERSION by scripts/generate_version.py. Do not edit.\n"
        f"#define YTL_FILE_VERSION {major},{minor},{patch},0\n"
        f'#define YTL_FILE_VERSION_STRING "{raw}.0"\n',
        encoding="utf-8",
    )
    print(f"generated Windows version headers for {raw}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
