#!/usr/bin/env bash
set -uo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR" || exit 1
OUT=/tmp/copy-staging-test
rm -rf "$OUT"; mkdir -p "$OUT"
if ! g++ -std=c++17 -Wall -Wextra -Wpedantic -Iui/modern \
    tests/unit/copy_staging_test.cpp ui/modern/copy_staging.cpp \
    -pthread -o "$OUT/copy_staging_test" >"$OUT/build.log" 2>&1; then
  echo "  FAIL 编译 copy_staging_test"; tail -20 "$OUT/build.log"; exit 1
fi
"$OUT/copy_staging_test" >"$OUT/run.log" 2>&1
CODE=$?
cat "$OUT/run.log"
echo "[copy-staging] exit=$CODE"
exit $CODE
