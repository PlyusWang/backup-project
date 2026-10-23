#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
mkdir -p tests/output
g++ -std=c++17 -O2 -Wall -Wextra -Iinclude tests/unit/ownership_ledger_test.cpp src/core/ownership_ledger.cpp src/platform/app_paths.cpp -o tests/output/ownership-ledger-test 2>&1 | tee tests/output/ownership-ledger-build.log
./tests/output/ownership-ledger-test 2>&1 | tee tests/output/ownership-ledger-test.log
