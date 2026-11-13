#!/usr/bin/env bash
# Builds and runs the portable core unit tests with sanitizers.
set -euo pipefail
cd "$(dirname "$0")/.."
CXX="${CXX:-g++}"
mkdir -p .build
"$CXX" -std=c++17 -O1 -g -Wall -Wextra \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  src/core/core.cc tests/core_test.cc -o .build/core_test
./.build/core_test
python3 tests/release_test.py
python3 tests/build_test.py
python3 tests/settings_layout_test.py
python3 scripts/check_docs.py
python3 scripts/release_facts.py --check
