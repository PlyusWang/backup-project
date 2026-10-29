#!/usr/bin/env bash
# 会话解锁、共享云端凭据及配置不落密码的回归入口。
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"
WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/session-credentials-test-XXXXXX")"
trap 'rm -rf "$WORK_DIR"' EXIT

make -j4 all >"$WORK_DIR/product-build.log" 2>&1 || {
  cat "$WORK_DIR/product-build.log"
  exit 1
}
mapfile -t CORE_OBJECTS < <(find build/src -name '*.o' | sort)
g++ -std=c++17 -Wall -Wextra -Wpedantic -Iinclude -Itests/unit \
  tests/unit/session_credentials_test.cpp "${CORE_OBJECTS[@]}" -pthread \
  -o "$WORK_DIR/session_credentials_test" >"$WORK_DIR/build.log" 2>&1 || {
  cat "$WORK_DIR/build.log"
  exit 1
}
if grep -q 'warning:' "$WORK_DIR/build.log"; then
  cat "$WORK_DIR/build.log"
  exit 1
fi
"$WORK_DIR/session_credentials_test"
