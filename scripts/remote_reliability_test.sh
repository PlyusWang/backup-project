#!/usr/bin/env bash
#
# remote_reliability_test.sh —— 远端可靠性 + **下载路径契约**回归。
#
#   bash scripts/remote_reliability_test.sh
#
# 为什么有这个脚本：上一轮的可靠性测试是临时脚本，它假设 remote download 会把
# 结果写成 "<快照ID>.bak"，于是把一次**完全正确**的下载判成 "FAIL 下载结果缺失"。
# 产品契约其实是"内容写到调用方给定的目标路径"。这里把该契约固化成仓库内的回归
# 测试，并且让"比较逻辑本身"也有自检 —— 一个永远返回成功的比较函数必须自己被
# 判失败，否则测试就是一个橡皮图章。
#
# 覆盖：
#   A. 自检：比较函数能发现 缺失 / 同长度不同内容 / 不同长度
#   B. 真实 backup-server（BPSEC1 pin 模式）+ 真实 backupctl：注册 / 备份 / 列表
#   C. 下载路径契约：写在给定路径、不是 .bak；大小与列表一致；两次下载逐字节相同
#   D. 恢复契约：remote restore 出来的文件与源文件逐字节一致
#   E. 可靠性：重复 ping / 重复 login / 错口令后正确口令仍可用 / 服务端 PID 不变
#   F. 优雅停止：SIGTERM 后连接必须失败
#
# 退出码：0 = 全部通过，非 0 = 有 FAIL。

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR" || exit 1

PASS=0
FAIL=0
record_pass() { PASS=$((PASS + 1)); echo "  PASS  $1"; }
record_fail() { FAIL=$((FAIL + 1)); echo "  FAIL  $1 -- $2"; }

TEST_ROOT="$(mktemp -d /tmp/remote-reliability-test-XXXXXX)"
SERVER_PID=""
cleanup() {
  if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill -TERM "$SERVER_PID" 2>/dev/null
    wait "$SERVER_PID" 2>/dev/null
  fi
  rm -rf "$TEST_ROOT"
}
trap cleanup EXIT

# 逐字节比较。**唯一**的"下载/恢复结果正确吗"判定入口。
# 返回 0 = 一致；1 = 不一致（含任一文件缺失、大小不同、内容不同）。
files_identical() {
  local expected="$1" actual="$2"
  [ -f "$expected" ] || return 1
  [ -f "$actual" ] || return 1
  local size_expected size_actual
  size_expected="$(stat -c '%s' "$expected" 2>/dev/null)" || return 1
  size_actual="$(stat -c '%s' "$actual" 2>/dev/null)" || return 1
  [ "$size_expected" = "$size_actual" ] || return 1
  cmp -s "$expected" "$actual"
}

echo "[remote-reliability] A. 自检：比较函数必须真的能发现缺陷"
mkdir -p "$TEST_ROOT/selfcheck"
printf 'same\n'   >"$TEST_ROOT/selfcheck/a"
printf 'same\n'   >"$TEST_ROOT/selfcheck/b"
printf 'diff\n'   >"$TEST_ROOT/selfcheck/c"
printf 'longer\n' >"$TEST_ROOT/selfcheck/d"
if files_identical "$TEST_ROOT/selfcheck/a" "$TEST_ROOT/selfcheck/b"; then
  record_pass "A.1 内容一致 -> 判定一致"
else
  record_fail "A.1 内容一致 -> 判定一致" "同样的内容被判成不一致"
fi
if files_identical "$TEST_ROOT/selfcheck/a" "$TEST_ROOT/selfcheck/missing"; then
  record_fail "A.2 目标文件缺失 -> 判定不一致" "缺失文件竟然判成一致"
else
  record_pass "A.2 目标文件缺失 -> 判定不一致"
fi
if files_identical "$TEST_ROOT/selfcheck/a" "$TEST_ROOT/selfcheck/c"; then
  record_fail "A.3 同长度不同内容 -> 判定不一致" "内容不同竟然判成一致"
else
  record_pass "A.3 同长度不同内容 -> 判定不一致"
fi
if files_identical "$TEST_ROOT/selfcheck/a" "$TEST_ROOT/selfcheck/d"; then
  record_fail "A.4 不同长度 -> 判定不一致" "长度不同竟然判成一致"
else
  record_pass "A.4 不同长度 -> 判定不一致"
fi

echo "[remote-reliability] B. 真实服务端 + 真实客户端（BPSEC1 pin 模式）"
if [ ! -x ./build/backupctl ] || [ ! -x ./build/backup-server ]; then
  if ! make -j4 all server >"$TEST_ROOT/build.log" 2>&1; then
    record_fail "产品构建" "$(tail -3 "$TEST_ROOT/build.log" | tr '\n' ' ')"
    echo "[remote-reliability] 失败项：$FAIL"
    exit 1
  fi
  record_pass "产品构建"
else
  record_pass "产品二进制已存在（build/backupctl, build/backup-server）"
fi

WORK="$TEST_ROOT/e2e"
mkdir -p "$WORK/data" "$WORK/state" "$WORK/logs" "$WORK/src" "$WORK/home/.config" "$WORK/downloads"
export HOME="$WORK/home"

head -c 32 /dev/urandom | sha256sum | cut -c1-64 >"$WORK/secret.value"
echo "BACKUP_TOKEN_SECRET=$(cat "$WORK/secret.value")" >"$WORK/secrets.env"
chmod 600 "$WORK/secrets.env"

if ./build/backup-server-keygen --output "$WORK/transport.key" >"$WORK/keygen.out" 2>&1; then
  record_pass "B.1 生成传输身份密钥"
else
  record_fail "B.1 生成传输身份密钥" "$(tail -1 "$WORK/keygen.out")"
  echo "[remote-reliability] 失败项：$FAIL"
  exit 1
fi
BACKUP_REMOTE_SERVER_KEY="$(grep -oE 'sha256:[0-9a-f]{64}' "$WORK/keygen.out" | head -1)"
export BACKUP_REMOTE_SERVER_KEY

PORT=0
for candidate in $(seq 23100 23180); do
  if ! ss -ltn 2>/dev/null | grep -q ":$candidate "; then PORT=$candidate; break; fi
done
if [ "$PORT" = "0" ]; then
  record_fail "B.2 找空闲端口" "23100-23180 都被占用"
  echo "[remote-reliability] 失败项：$FAIL"
  exit 1
fi

./build/backup-server --bind 127.0.0.1 --port "$PORT" --root "$WORK/data" \
  --db "$WORK/state/metadata.sqlite3" --secret-file "$WORK/secrets.env" \
  --transport-key-file "$WORK/transport.key" --pid-file "$WORK/state/server.pid" \
  --log-file "$WORK/logs/server.log" --quiet >>"$WORK/logs/server.out" 2>&1 &
SERVER_PID=$!
ready=0
for _ in $(seq 1 50); do
  if ss -ltn 2>/dev/null | grep -q "127.0.0.1:$PORT "; then ready=1; break; fi
  sleep 0.1
done
if [ "$ready" = "1" ]; then
  record_pass "B.2 服务端监听 127.0.0.1:$PORT"
else
  record_fail "B.2 服务端监听" "$(tail -2 "$WORK/logs/server.log" | tr '\n' ' ')"
  echo "[remote-reliability] 失败项：$FAIL"
  exit 1
fi

BACKUP_REMOTE_PASSWORD="pw-$(head -c 16 /dev/urandom | sha256sum | cut -c1-24)"
export BACKUP_REMOTE_PASSWORD
USER_NAME="remote-reliability-$$"
REMOTE="--user $USER_NAME --host 127.0.0.1 --port $PORT"
run_remote() { ./build/backupctl remote "$@" 2>&1; }

if run_remote register $REMOTE >"$WORK/register.log"; then
  record_pass "B.3 注册"
else
  record_fail "B.3 注册" "$(tail -1 "$WORK/register.log")"
fi

head -c 65536 /dev/urandom >"$WORK/src/big.bin"
printf 'payload-v1\n' >"$WORK/src/note.txt"
mkdir -p "$WORK/src/nested dir"
printf 'nested\n' >"$WORK/src/nested dir/child file.txt"
SRC_SHA_BIG="$(sha256sum "$WORK/src/big.bin" | cut -d' ' -f1)"
SRC_SHA_NOTE="$(sha256sum "$WORK/src/note.txt" | cut -d' ' -f1)"

if run_remote backup "$WORK/src" --strategy full $REMOTE --name R1 >"$WORK/backup.log"; then
  record_pass "B.4 完整备份"
else
  record_fail "B.4 完整备份" "$(tail -2 "$WORK/backup.log" | tr '\n' ' ')"
fi

run_remote list $REMOTE >"$WORK/list.log" 2>&1
SNAP_ID="$(awk 'NR>2 && NF>0 {print $1; exit}' "$WORK/list.log")"
SNAP_SIZE="$(awk 'NR>2 && NF>0 {print $2; exit}' "$WORK/list.log")"
if [ -n "$SNAP_ID" ]; then
  record_pass "B.5 列表返回快照 $SNAP_ID（$SNAP_SIZE 字节）"
else
  record_fail "B.5 列表返回快照" "$(tail -2 "$WORK/list.log" | tr '\n' ' ')"
  echo "[remote-reliability] 失败项：$FAIL"
  exit 1
fi

echo "[remote-reliability] C. 下载路径契约（上一轮 L12 误判的根因）"
DL_ONE="$WORK/downloads/explicit-target.bin"
run_remote download "$SNAP_ID" "$DL_ONE" $REMOTE --force >"$WORK/dl1.log" 2>&1
if [ "$?" = "0" ]; then
  record_pass "C.1 remote download 退出码 0"
else
  record_fail "C.1 remote download 退出码 0" "$(tail -2 "$WORK/dl1.log" | tr '\n' ' ')"
fi
if [ -f "$DL_ONE" ]; then
  record_pass "C.2 内容写在调用方给定的目标路径上"
else
  record_fail "C.2 内容写在调用方给定的目标路径上" "给定路径上什么都没有"
fi
if [ -e "$WORK/downloads/$SNAP_ID.bak" ]; then
  record_fail "C.3 没有凭空生成 <快照ID>.bak" "出现了 $SNAP_ID.bak"
else
  record_pass "C.3 没有凭空生成 <快照ID>.bak（旧脚本的错误假设）"
fi
DL_SIZE="$(stat -c '%s' "$DL_ONE" 2>/dev/null)"
if [ -n "$SNAP_SIZE" ] && [ "$DL_SIZE" = "$SNAP_SIZE" ]; then
  record_pass "C.4 下载大小与列表一致（$DL_SIZE 字节）"
else
  record_fail "C.4 下载大小与列表一致" "列表=$SNAP_SIZE 实际=$DL_SIZE"
fi
DL_ONE_SHA="$(sha256sum "$DL_ONE" 2>/dev/null | cut -d' ' -f1)"
DL_ONE_SHA_SHORT="$(printf '%s' "$DL_ONE_SHA" | cut -c1-16)"

DL_TWO="$WORK/downloads/second-target.bin"
run_remote download "$SNAP_ID" "$DL_TWO" $REMOTE --force >"$WORK/dl2.log" 2>&1
if files_identical "$DL_ONE" "$DL_TWO"; then
  record_pass "C.5 两次下载逐字节一致（sha256 $DL_ONE_SHA_SHORT…）"
else
  record_fail "C.5 两次下载逐字节一致" "两次下载的内容不同"
fi

echo "[remote-reliability] D. 恢复契约"
RESTORE_DIR="$WORK/restored"
run_remote restore "$SNAP_ID" "$RESTORE_DIR" $REMOTE >"$WORK/restore.log" 2>&1
if [ "$?" = "0" ]; then
  record_pass "D.1 remote restore 退出码 0"
else
  record_fail "D.1 remote restore 退出码 0" "$(tail -2 "$WORK/restore.log" | tr '\n' ' ')"
fi
if files_identical "$WORK/src/big.bin" "$RESTORE_DIR/big.bin"; then
  record_pass "D.2 恢复出来的 big.bin 与源文件逐字节一致"
else
  record_fail "D.2 恢复出来的 big.bin 与源文件逐字节一致" "内容不一致或缺失"
fi
if files_identical "$WORK/src/note.txt" "$RESTORE_DIR/note.txt"; then
  record_pass "D.3 恢复出来的 note.txt 与源文件逐字节一致"
else
  record_fail "D.3 恢复出来的 note.txt 与源文件逐字节一致" "内容不一致或缺失"
fi
if files_identical "$WORK/src/nested dir/child file.txt" "$RESTORE_DIR/nested dir/child file.txt"; then
  record_pass "D.4 含空格路径的嵌套文件逐字节一致"
else
  record_fail "D.4 含空格路径的嵌套文件逐字节一致" "内容不一致或缺失"
fi
if [ "$(sha256sum "$RESTORE_DIR/big.bin" 2>/dev/null | cut -d' ' -f1)" = "$SRC_SHA_BIG" ] &&
   [ "$(sha256sum "$RESTORE_DIR/note.txt" 2>/dev/null | cut -d' ' -f1)" = "$SRC_SHA_NOTE" ]; then
  record_pass "D.5 恢复结果 SHA256 与源一致"
else
  record_fail "D.5 恢复结果 SHA256 与源一致" "SHA256 不同"
fi

echo "[remote-reliability] E. 可靠性"
PING_OK=0
for _ in $(seq 1 10); do
  if run_remote ping --host 127.0.0.1 --port "$PORT" >/dev/null 2>&1; then PING_OK=$((PING_OK + 1)); fi
done
if [ "$PING_OK" = "10" ]; then
  record_pass "E.1 连续 10 次 ping 全部成功"
else
  record_fail "E.1 连续 10 次 ping 全部成功" "成功 $PING_OK/10"
fi

LOGIN_OK=0
for _ in $(seq 1 3); do
  if run_remote login $REMOTE >/dev/null 2>&1; then LOGIN_OK=$((LOGIN_OK + 1)); fi
done
if [ "$LOGIN_OK" = "3" ]; then
  record_pass "E.2 连续 3 次 login 全部成功"
else
  record_fail "E.2 连续 3 次 login 全部成功" "成功 $LOGIN_OK/3"
fi

LIST_OK=0
for _ in $(seq 1 5); do
  if run_remote list $REMOTE >/dev/null 2>&1; then LIST_OK=$((LIST_OK + 1)); fi
done
if [ "$LIST_OK" = "5" ]; then
  record_pass "E.3 连续 5 次 list 全部成功"
else
  record_fail "E.3 连续 5 次 list 全部成功" "成功 $LIST_OK/5"
fi

if BACKUP_REMOTE_PASSWORD="definitely-wrong-password" run_remote login $REMOTE >"$WORK/wrong-pw.log" 2>&1; then
  record_fail "E.4 错误口令被拒绝" "竟然登录成功"
else
  record_pass "E.4 错误口令被拒绝"
fi
if run_remote login $REMOTE >/dev/null 2>&1; then
  record_pass "E.5 错误口令之后正确口令仍然可用"
else
  record_fail "E.5 错误口令之后正确口令仍然可用" "正确口令也登不上了"
fi

if kill -0 "$SERVER_PID" 2>/dev/null; then
  record_pass "E.6 全程服务端进程没有退出（pid $SERVER_PID）"
else
  record_fail "E.6 全程服务端进程没有退出" "服务端已经不在"
fi

echo "[remote-reliability] F. 优雅停止"
kill -TERM "$SERVER_PID" 2>/dev/null
STOPPED=0
for _ in $(seq 1 50); do
  if ! kill -0 "$SERVER_PID" 2>/dev/null; then STOPPED=1; break; fi
  sleep 0.1
done
if [ "$STOPPED" = "1" ]; then
  record_pass "F.1 SIGTERM 后服务端退出"
else
  record_fail "F.1 SIGTERM 后服务端退出" "进程还在"
fi
# "backup-server stopped." 走的是服务端 stdout，不一定写进 --log-file；
# 两处都查，避免把一次正确的优雅停止判成失败（同 L12 的教训）。
if grep -q 'backup-server stopped\.' "$WORK/logs/server.log" "$WORK/logs/server.out" 2>/dev/null; then
  record_pass "F.2 日志里有优雅停止记录"
else
  record_fail "F.2 日志里有优雅停止记录" "$(tail -1 "$WORK/logs/server.out" 2>/dev/null)"
fi
if run_remote ping --host 127.0.0.1 --port "$PORT" >/dev/null 2>&1; then
  record_fail "F.3 停止后连接必须失败" "竟然还能连上"
else
  record_pass "F.3 停止后连接必须失败"
fi
SERVER_PID=""

echo
echo "[remote-reliability] PASS=$PASS FAIL=$FAIL"
if [ "$FAIL" != "0" ]; then
  echo "[remote-reliability] 失败项：$FAIL"
  exit 1
fi
echo "[remote-reliability] 全部通过"
exit 0
