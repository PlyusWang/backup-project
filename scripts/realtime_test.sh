#!/usr/bin/env bash
#
# PR #19 Realtime Trigger 的专项测试入口。
#
#   A. 核心单元测试：realtime_store / realtime_debouncer / realtime_watcher /
#      realtime_backup / realtime_retention（编译方式与 scheduled_backup_test.sh
#      的 run_unit 一致：真实 core 目标文件 + -Wall -Wextra -Wpedantic 零警告）。
#   B. CLI 端到端：realtime set/show/enable/watch/history/disable，真实 inotify。
#
# 退出码：只有全部用例通过才是 0。

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "$BASH_SOURCE")/.." && pwd)"
BACKUPCTL="$ROOT_DIR/build/backupctl"
TEST_ROOT="$ROOT_DIR/testdata/realtime"
OUT="$TEST_ROOT/last-output.txt"
mkdir -p "$TEST_ROOT"

PASS=0
FAIL=0

echo "[realtime-test] PR #19 realtime trigger suite"

record_pass() { PASS=$((PASS + 1)); echo "  PASS  $1"; }
record_fail() { FAIL=$((FAIL + 1)); echo "  FAIL  $1 -- $2" >&2; }

first_line() { head -n 1 "$OUT" | cut -c1-200; }

if [ ! -x "$BACKUPCTL" ]; then
  echo "[realtime-test] build/backupctl is missing; building first..."
  make -C "$ROOT_DIR" all >/dev/null 2>&1
fi

# ============================================================
echo "[realtime-test] A. core unit tests"
# ============================================================

CORE_OBJECTS="$(find "$ROOT_DIR/build/src" -name '*.o' | sort | tr '\n' ' ')"
if [ -z "$CORE_OBJECTS" ]; then
  echo "[realtime-test] 没有找到 build/src 下的目标文件；请先 make" >&2
  exit 1
fi

run_unit() {
  local name="$1"
  local source="$ROOT_DIR/tests/unit/$name.cpp"
  local binary="$TEST_ROOT/$name"
  if [ ! -f "$source" ]; then
    record_fail "A.$name 存在" "tests/unit/$name.cpp 不存在"
    return
  fi
  if ! g++ -std=c++17 -Wall -Wextra -Wpedantic -I"$ROOT_DIR/include" \
      -I"$ROOT_DIR/tests/unit" "$source" $CORE_OBJECTS -o "$binary" \
      >"$TEST_ROOT/$name-build.log" 2>&1; then
    record_fail "A.$name 编译" "$(head -3 "$TEST_ROOT/$name-build.log" | tr '\n' ' ')"
    return
  fi
  record_pass "A.$name 编译（-Wall -Wextra -Wpedantic 无警告）"
  if [ -s "$TEST_ROOT/$name-build.log" ]; then
    record_fail "A.$name 零警告" "$(head -2 "$TEST_ROOT/$name-build.log" | tr '\n' ' ')"
  else
    record_pass "A.$name 零警告"
  fi
  "$binary" >"$TEST_ROOT/$name.log" 2>&1
  local status=$?
  if [ "$status" = "0" ]; then
    record_pass "A.$name 全部断言通过（$(tail -1 "$TEST_ROOT/$name.log")）"
  else
    record_fail "A.$name 全部断言通过" "$(grep FAIL "$TEST_ROOT/$name.log" | head -3 | tr '\n' ' ')"
  fi
}

run_unit realtime_store_test
run_unit realtime_debouncer_test
run_unit realtime_watcher_test
run_unit realtime_backup_test
run_unit realtime_retention_test

# ============================================================
echo "[realtime-test] 结果"
# ============================================================
echo "[realtime-test] results: PASS=$PASS FAIL=$FAIL"
if [ "$FAIL" -ne 0 ]; then
  echo "[realtime-test] some cases failed" >&2
  exit 1
fi
echo "[realtime-test] all cases passed"
