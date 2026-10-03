#!/usr/bin/env bash
#
# raw_archive_restore_test.sh —— 原始归档（lineage 为空）"单独恢复"的专项 suite。
#
# 被测路径是**产品路径**，不是 CLI 包装，也不是本脚本自己拼出来的命令：
#
#   QML -> RemoteController -> RunRemoteRawRestore -> 既有本地恢复核心
#                                 (BackupEngine / RunRestorePipeline /
#                                  ArchiveReader：与 backupctl restore、
#                                  GUI 本地恢复同一份实现)
#
# A 段（默认）：跑 Modern GUI 的 --remote-test。它自己起一个真的 backup-server，
#   用 RemoteController 把下面五条真的走一遍，本脚本只断言"这些条目确实跑了、
#   而且全部通过"：
#     RAW-R01 可独立恢复的完整 .bak：下载 -> SHA-256 校验 -> 本地核心恢复 -> diff
#     RAW-R02 被篡改的归档：失败，目标目录为空（没有半成品）
#     RAW-R03 任意文件（显示名还是 .bak）：按内容识别 -> 明确失败，目标目录为空
#     RAW-R04 单独的 delta：明确"不能脱离依赖链单独恢复"，目标目录为空
#     RAW-R05 加密归档：没填密码 -> 明确要求密码；错密码 -> 失败；对密码 -> 成功
#
# B 段（RAW_RESTORE_SANITIZE=1）：同一个入口在 ASan + UBSan 下跑**边界输入**。
#   tests/review/raw_restore_robustness.cpp 是 Qt-free 的 harness，链 build-sanitize
#   的产品目标文件，对着一个真的 sanitize 版 backup-server 跑：截断 / 任意字节 /
#   两种离谱的声明长度 / 加密归档的三种情况 / 单独的 delta / 不存在的快照 id，
#   每一条都要求 fail-closed（目标目录不被创建）并且消毒剂报告为 0。
#
# 退出码：0 = 全部通过。

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

PASS=0
FAIL=0
record_pass() { PASS=$((PASS + 1)); echo "  PASS  $1"; }
record_fail() { FAIL=$((FAIL + 1)); echo "  FAIL  $1 -- $2" >&2; }
finish() {
  echo
  echo "[raw-restore] PASS=$PASS FAIL=$FAIL"
  if [ "$FAIL" -ne 0 ]; then
    exit 1
  fi
  echo "[raw-restore] 全部通过"
  exit 0
}

OUT_DIR="$ROOT_DIR/tests/output"
mkdir -p "$OUT_DIR"

# ---- B 段：消毒剂维度 -------------------------------------------------------

sanitize_section() {
  echo "[raw-restore] B 段：ASan + UBSan 下的边界输入（raw_restore_robustness）"
  local build_dir=build-sanitize
  local source=tests/review/raw_restore_robustness.cpp
  local tmp
  tmp="$(mktemp -d /tmp/raw-restore-sanitize-XXXXXX)"
  local server_pid=""

  cleanup_san() {
    if [ -n "$server_pid" ] && kill -0 "$server_pid" 2>/dev/null; then
      kill -TERM "$server_pid" 2>/dev/null
      wait "$server_pid" 2>/dev/null
    fi
  }
  trap cleanup_san EXIT

  if ! make -j2 sanitize >"$tmp/make.log" 2>&1; then
    record_fail "make sanitize" "$(tail -3 "$tmp/make.log" | tr '\n' ' ')"
    finish
  fi
  record_pass "build-sanitize 产品目标文件已构建（$(grep -ci warning "$tmp/make.log" || true) 条 warning）"

  mkdir -p "$tmp/state" "$tmp/logs" "$tmp/work"
  if ! "$build_dir/backup-server-keygen" --output "$tmp/state/transport.key" \
      >"$tmp/state/keygen.out" 2>&1; then
    record_fail "生成传输身份密钥" "$(head -2 "$tmp/state/keygen.out" | tr '\n' ' ')"
    finish
  fi
  local pin
  pin="$(grep -oE 'sha256:[0-9a-f]{64}' "$tmp/state/keygen.out" | head -1)"
  if [ -n "$pin" ]; then
    record_pass "pin 已从 keygen 输出取到（$pin）"
  else
    record_fail "pin" "keygen 输出里没有 sha256:<64 位十六进制>"
    finish
  fi
  head -c 32 /dev/urandom | sha256sum | cut -c1-64 >"$tmp/state/secret.value"
  echo "BACKUP_TOKEN_SECRET=$(cat "$tmp/state/secret.value")" >"$tmp/state/secrets.env"
  chmod 600 "$tmp/state/secrets.env"

  local port=0 candidate
  for candidate in $(seq 20400 20480); do
    if ! ss -ltn 2>/dev/null | grep -q ":$candidate "; then
      port="$candidate"
      break
    fi
  done
  if [ "$port" = "0" ]; then
    record_fail "端口" "20400-20480 都被占用"
    finish
  fi
  # 服务端自己不是被测对象（另一个进程、由信号终止）：只给它关掉 leak 检测。
  ASAN_OPTIONS=detect_leaks=0 "$build_dir/backup-server" \
    --bind 127.0.0.1 --port "$port" \
    --root "$tmp/data" --db "$tmp/state/metadata.sqlite3" \
    --secret-file "$tmp/state/secrets.env" \
    --transport-key-file "$tmp/state/transport.key" \
    --pid-file "$tmp/state/server.pid" --log-file "$tmp/logs/server.log" \
    --quiet &
  server_pid=$!
  local ready=0
  for _ in $(seq 1 100); do
    kill -0 "$server_pid" 2>/dev/null || break
    if ss -ltn 2>/dev/null | grep -q "127.0.0.1:$port "; then
      ready=1
      break
    fi
    sleep 0.1
  done
  if [ "$ready" = "1" ]; then
    record_pass "sanitize 版服务端已监听 127.0.0.1:$port（pid $server_pid）"
  else
    record_fail "sanitize 服务端监听" "$(tail -3 "$tmp/logs/server.log" | tr '\n' ' ')"
    finish
  fi

  local objects bin harness_log
  objects="$(find "$build_dir/src" -name '*.o' 2>/dev/null | sort | tr '\n' ' ')"
  if [ -z "$objects" ]; then
    record_fail "目标文件" "$build_dir/src 下没有 .o"
    finish
  fi
  bin="$tmp/raw_restore_robustness"
  # shellcheck disable=SC2086
  if ! g++ -std=c++17 -Wall -Wextra -Wpedantic -g -O1 \
      -fsanitize=address,undefined -fno-omit-frame-pointer \
      -Iinclude "$source" $objects -pthread -o "$bin" >"$tmp/compile.log" 2>&1; then
    record_fail "编译 raw_restore_robustness" "$(tail -5 "$tmp/compile.log" | tr '\n' ' ')"
    finish
  fi
  if [ -s "$tmp/compile.log" ]; then
    record_fail "编译输出必须为空（零警告）" "$(head -3 "$tmp/compile.log" | tr '\n' ' ')"
  else
    record_pass "raw_restore_robustness 零警告编译 + 链接（ASan+UBSan）"
  fi

  harness_log="$tmp/harness.log"
  local status=0
  ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=0 \
    timeout --signal=KILL 3600 "$bin" 127.0.0.1 "$port" "$pin" "$tmp/work" \
    >"$harness_log" 2>&1 || status=$?
  sed 's/^/      /' "$harness_log"

  cp "$harness_log" "$OUT_DIR/raw-restore-sanitize.log"
  if [ "$status" = "0" ]; then
    record_pass "harness 退出码 0"
  else
    record_fail "harness 退出码" "$status"
  fi
  local summary counts passed_n total_n
  summary="$(grep -oE 'raw-restore-robustness: [0-9]+/[0-9]+ checks passed' "$harness_log" | tail -1)"
  counts="$(printf '%s' "$summary" | grep -oE '[0-9]+/[0-9]+' | head -1)"
  passed_n="$(printf '%s' "$counts" | cut -d/ -f1)"
  total_n="$(printf '%s' "$counts" | cut -d/ -f2)"
  if [ -n "$counts" ] && [ "$passed_n" = "$total_n" ]; then
    record_pass "边界输入用例 $summary"
  else
    record_fail "边界输入用例" "汇总行：$summary"
  fi
  local reports
  reports="$(grep -cE 'ERROR: (AddressSanitizer|LeakSanitizer|UndefinedBehaviorSanitizer)|runtime error:' "$harness_log" || true)"
  if [ "$reports" = "0" ]; then
    record_pass "ASan / UBSan / LeakSanitizer 报告 0 条"
  else
    record_fail "消毒剂报告" "$reports 条（见 tests/output/raw-restore-sanitize.log）"
  fi
  rm -rf "$tmp"
  finish
}

if [ "$(printenv RAW_RESTORE_SANITIZE 2>/dev/null || true)" = "1" ]; then
  sanitize_section
fi

# ---- A 段：产品路径（GUI --remote-test）-------------------------------------

echo "[raw-restore] A 段：Qt/GUI 产品路径（--remote-test 里的 RAW-R01..R05）"

BUILD_LOG="/tmp/raw-restore-build.log"
GUI="./build/backup-gui-modern"
if [ ! -x "$GUI" ]; then
  if ! make -j4 gui-modern >"$BUILD_LOG" 2>&1; then
    record_fail "make gui-modern" "$(tail -3 "$BUILD_LOG" | tr '\n' ' ')"
    finish
  fi
fi
if [ ! -x "$GUI" ]; then
  record_fail "Modern GUI 已构建" "$GUI 不存在"
  finish
fi
record_pass "Modern GUI 已构建（$GUI）"

# 真实用户配置隔离：AppTheme 的 QSettings 与 QStandardPaths 以 XDG_CONFIG_HOME
# 为根，指向临时目录之后，这一次自检既不读也不写 ~/.config —— 远端缓存的
# 下载目录就在那下面，隔离之后"临时文件有没有清干净"这件事才可判定。
STATE_DIR="$(mktemp -d)"
cleanup_state() { rm -rf "$STATE_DIR"; }
trap cleanup_state EXIT
export XDG_CONFIG_HOME="$STATE_DIR/xdg"
mkdir -p "$XDG_CONFIG_HOME"

LOG_FILE="$STATE_DIR/remote-test.log"
set +e
QT_QPA_PLATFORM=offscreen QSG_RHI_BACKEND=software timeout 2400 \
  "$GUI" --remote-test \
  --config-file "$STATE_DIR/config.json" \
  --schedule-file "$STATE_DIR/schedule.json" \
  --realtime-file "$STATE_DIR/realtime.json" \
  >"$LOG_FILE" 2>&1
test_status=$?
set -e

if [ "$test_status" != "0" ]; then
  record_fail "GUI --remote-test 退出码" "exit=$test_status"
  tail -30 "$LOG_FILE" | sed 's/^/    /' >&2
else
  record_pass "GUI --remote-test 退出码 0"
fi

summary="$(grep -oE 'passed=[0-9]+ failed=[0-9]+' "$LOG_FILE" | tail -1)"
case "$summary" in
  *"failed=0")
    record_pass "GUI 自检汇总 $summary"
    ;;
  *)
    record_fail "GUI 自检汇总" "汇总行：$summary"
    ;;
esac

# 五条 RAW-R 必须**真的跑过**：逐条数 ok 行，并把每一条的原文打在屏幕上。
check_raw() {
  local tag="$1"
  local label="$2"
  local expected="$3"
  local ok_count
  ok_count="$(grep -c "ok   $tag" "$LOG_FILE" || true)"
  if [ "$ok_count" -ge "$expected" ]; then
    record_pass "$tag（$ok_count 条）：$label"
  else
    record_fail "$tag" "只看到 $ok_count 条 ok（期望至少 $expected）：$label"
  fi
  grep "ok   $tag" "$LOG_FILE" | sed 's/^/      /' || true
}
check_raw "RAW-R01" "独立完整 .bak 的下载 -> 校验 -> 本地核心恢复" 5
check_raw "RAW-R02" "被篡改的归档：失败且目标目录为空" 3
check_raw "RAW-R03" "任意文件（.bak 显示名）：按内容识别后明确失败" 4
check_raw "RAW-R04" "单独的 delta：不能脱离依赖链单独恢复" 3
check_raw "RAW-R05" "加密归档：没密码 / 错密码 / 对密码三条路径" 6

if grep -q "FAIL RAW-R" "$LOG_FILE"; then
  record_fail "RAW-R 条目" "$(grep -m3 'FAIL RAW-R' "$LOG_FILE" | tr '\n' ' ')"
else
  record_pass "没有任何 RAW-R 条目失败"
fi

# 界面类型契约也在同一个自检里（三种类型三个词）。
for line in "GUI-P08 完整备份：badge=完整备份" "GUI-P08 增量备份：badge=增量备份"; do
  if grep -q "ok   $line" "$LOG_FILE"; then
    record_pass "类型契约：$line"
  else
    record_fail "类型契约" "缺少：$line"
  fi
done

cp "$LOG_FILE" "$OUT_DIR/raw-restore.log"

echo
echo "---- RAW-R 逐条结果 ----"
grep "RAW-R" "$LOG_FILE" | sed 's/^/  /' || true
finish
