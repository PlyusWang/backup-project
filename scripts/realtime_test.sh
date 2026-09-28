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
echo "[realtime-test] B. CLI 端到端（真实 inotify）"
# ============================================================

CLI_WORK="$TEST_ROOT/cli"
rm -rf "$CLI_WORK"
mkdir -p "$CLI_WORK/src/sub" "$CLI_WORK/repo"
printf 'alpha\n' > "$CLI_WORK/src/a.txt"
printf 'bee\n' > "$CLI_WORK/src/sub/b.txt"

CONFIG_FILE="$CLI_WORK/config.json"
REALTIME_FILE="$CLI_WORK/realtime.json"

cli() {
  "$BACKUPCTL" --config-file "$CONFIG_FILE" --realtime-file "$REALTIME_FILE" "$@"
}

if cli config repository set "$CLI_WORK/repo" >"$OUT" 2>&1; then
  record_pass "B.1 配置仓库"
else
  record_fail "B.1 配置仓库" "$(first_line)"
fi

# 自动触发不接受加密：组合校验必须直接拒绝，绝不静默降级。
if cli realtime set --source "$CLI_WORK/src" \
    --encryption aes-256-ctr-hmac-sha256 >"$OUT" 2>&1; then
  record_fail "B.2 realtime 拒绝加密" "命令居然成功了"
elif grep -q '不启用加密' "$OUT"; then
  record_pass "B.2 realtime 拒绝加密并说明原因"
else
  record_fail "B.2 realtime 拒绝加密并说明原因" "$(first_line)"
fi

if cli realtime set --source "$CLI_WORK/src" --debounce-ms 200 --max-wait-ms 1000 \
    --retain 2 --strategy full --pack mypack --compression none \
    --include 'ext:txt' >"$OUT" 2>&1; then
  record_pass "B.3 realtime set 保存成功"
else
  record_fail "B.3 realtime set 保存成功" "$(first_line)"
fi

if cli realtime show >"$OUT" 2>&1 && grep -q 'Enabled:        no' "$OUT" &&
    grep -q 'ext:txt' "$OUT"; then
  record_pass "B.4 realtime show 读出配置"
else
  record_fail "B.4 realtime show 读出配置" "$(first_line)"
fi

# 没 enable 时 watch 必须拒绝启动，而不是偷偷跑起来。
if cli realtime watch >"$OUT" 2>&1; then
  record_fail "B.5 未启用时 watch 拒绝启动" "命令居然成功了"
else
  record_pass "B.5 未启用时 watch 拒绝启动"
fi

if cli realtime enable >"$OUT" 2>&1 && grep -q 'Enabled:        yes' "$OUT"; then
  record_pass "B.6 enable"
else
  record_fail "B.6 enable" "$(first_line)"
fi

WATCH_LOG="$CLI_WORK/watch.log"
# 注意：这里刻意不走 cli() 函数——函数会在子 shell 里跑，$! 拿到的是子 shell 的
# pid，SIGINT 传不到 backupctl，watch 会留下来。
"$BACKUPCTL" --config-file "$CONFIG_FILE" --realtime-file "$REALTIME_FILE" \
  realtime watch >"$WATCH_LOG" 2>&1 &
WATCH_PID=$!
sleep 2
printf 'beta\n' > "$CLI_WORK/src/c.txt"
sleep 2
printf 'gamma\n' > "$CLI_WORK/src/sub/d.txt"
sleep 3
kill -INT "$WATCH_PID" 2>/dev/null
wait "$WATCH_PID" 2>/dev/null
WATCH_STATUS=$?

if grep -q 'outcome=full' "$WATCH_LOG" && grep -q 'stopped' "$WATCH_LOG"; then
  record_pass "B.7 真实 inotify：settled generation 建了快照并干净退出"
else
  record_fail "B.7 真实 inotify 触发" "$(head -3 "$WATCH_LOG" | tr '\n' ' ')"
fi
if [ "$WATCH_STATUS" = "0" ]; then
  record_pass "B.8 SIGINT 退出码 0"
else
  record_fail "B.8 SIGINT 退出码 0" "exit=$WATCH_STATUS"
fi
if pgrep -x backupctl >/dev/null 2>&1; then
  record_fail "B.9 没有残留 watch 进程" "pgrep 仍然有匹配"
else
  record_pass "B.9 没有残留 watch 进程"
fi

if cli realtime history >"$OUT" 2>&1; then
  COUNT="$(grep -c 'strategy=' "$OUT" || true)"
  if [ "$COUNT" = "2" ]; then
    record_pass "B.10 history：retain=2 生效（快照数=2）"
  else
    record_fail "B.10 history：retain=2 生效" "快照数=$COUNT"
  fi
else
  record_fail "B.10 history" "$(first_line)"
fi

TMP_COUNT="$(find "$CLI_WORK/repo" -name '*.tmp' | wc -l)"
BAK_COUNT="$(find "$CLI_WORK/repo" -name '*.bak' | wc -l)"
MARKER_COUNT="$(find "$CLI_WORK/repo" -name '*.bak.realtime' | wc -l)"
if [ "$TMP_COUNT" = "0" ] && [ "$BAK_COUNT" = "$MARKER_COUNT" ] &&
    [ "$BAK_COUNT" != "0" ]; then
  record_pass "B.11 仓库干净：无 temp、每份归档都有 marker"
else
  record_fail "B.11 仓库干净" "tmp=$TMP_COUNT bak=$BAK_COUNT marker=$MARKER_COUNT"
fi

if cli realtime disable >"$OUT" 2>&1 && grep -q 'disabled' "$OUT"; then
  record_pass "B.12 disable"
else
  record_fail "B.12 disable" "$(first_line)"
fi

# 进程重启 catch-up：监听没在跑的时候改文件，下一次 attach 必须合成 resync。
printf 'delta\n' > "$CLI_WORK/src/e.txt"
cli realtime enable >/dev/null 2>&1
WATCH_LOG2="$CLI_WORK/watch2.log"
# 注意：这里刻意不走 cli() 函数——函数会在子 shell 里跑，$! 拿到的是子 shell 的
# pid，SIGINT 传不到 backupctl，watch 会留下来。
"$BACKUPCTL" --config-file "$CONFIG_FILE" --realtime-file "$REALTIME_FILE" \
  realtime watch >"$WATCH_LOG2" 2>&1 &
WATCH_PID=$!
sleep 4
kill -INT "$WATCH_PID" 2>/dev/null
wait "$WATCH_PID" 2>/dev/null
if grep -q 'resync trigger' "$WATCH_LOG2"; then
  record_pass "B.13 重新 attach 后先合成一次 resync（重启 catch-up）"
else
  record_fail "B.13 重启 catch-up" "$(head -3 "$WATCH_LOG2" | tr '\n' ' ')"
fi
if cli realtime history >"$OUT" 2>&1 && grep -q 'resync' "$OUT"; then
  record_pass "B.14 history 里能看到 resync 快照"
else
  record_fail "B.14 history 里的 resync 快照" "$(first_line)"
fi
cli realtime disable >/dev/null 2>&1

# ============================================================
echo "[realtime-test] 结果"
# ============================================================
echo "[realtime-test] results: PASS=$PASS FAIL=$FAIL"
if [ "$FAIL" -ne 0 ]; then
  echo "[realtime-test] some cases failed" >&2
  exit 1
fi
echo "[realtime-test] all cases passed"
