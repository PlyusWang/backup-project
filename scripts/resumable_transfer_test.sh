#!/usr/bin/env bash
#
# resumable_transfer_test.sh —— 上传 / 下载断点续传的端到端回归。
#
#   bash scripts/resumable_transfer_test.sh
#
# 覆盖“真实进程被杀”之后的恢复，而不是同一次连接里的重试：
#   A. 上传中断 -> 客户端失败但保留状态 -> 新进程从服务端检查点继续
#   B. 下载中断 -> 半成品与状态保留 -> 新进程从 offset 继续
#   C. 半成品被篡改 -> 内容校验失败 -> 状态被丢弃 -> 下一次从头成功
#
# 退出码：0 = 全部通过，非 0 = 有 FAIL。

set -uo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR" || exit 1

PASS=0
FAIL=0
record_pass() { PASS=$((PASS + 1)); echo "  PASS  $1"; }
record_fail() { FAIL=$((FAIL + 1)); echo "  FAIL  $1 -- $2"; }
finish() {
  echo "[resumable-transfer] passed=$PASS failed=$FAIL"
  [ "$FAIL" -eq 0 ] || exit 1
}

if [ ! -x ./build/backupctl ] || [ ! -x ./build/backup-server ]; then
  make -j4 all server >/tmp/resume-build.log 2>&1 || {
    echo "构建失败"; tail -5 /tmp/resume-build.log; exit 1;
  }
fi

W=$(mktemp -d /tmp/resume-e2e-XXXXXX)
export HOME="$W/home"
mkdir -p "$W/data" "$W/state" "$W/logs" "$W/home/.config" "$W/src" "$W/out" "$W/out2"
SP=""
cleanup() {
  if [ -n "$SP" ] && [ "$(cat "/proc/$SP/comm" 2>/dev/null)" = "backup-server" ]; then
    kill -KILL "$SP" 2>/dev/null
  fi
  rm -rf "$W"
}
trap cleanup EXIT

head -c 32 /dev/urandom | sha256sum | cut -c1-64 >"$W/secret"
echo "BACKUP_TOKEN_SECRET=$(cat "$W/secret")" >"$W/secrets.env"
chmod 600 "$W/secrets.env"
./build/backup-server-keygen --output "$W/key" >"$W/key.out" 2>&1 || { echo keygen 失败; exit 1; }
export BACKUP_REMOTE_SERVER_KEY="$(grep -oE 'sha256:[0-9a-f]{64}' "$W/key.out" | head -1)"
BACKUP_REMOTE_PASSWORD="$(head -c 18 /dev/urandom | base64 | tr -d '\n')"
export BACKUP_REMOTE_PASSWORD

PORT=0
for p in $(seq 25300 25450); do
  if ! ss -ltn 2>/dev/null | grep -q ":$p "; then PORT=$p; break; fi
done
[ "$PORT" != "0" ] || { echo 没有空闲端口; exit 1; }
REMOTE="--host 127.0.0.1 --port $PORT --user resume_user"

start() {
  ./build/backup-server --bind 127.0.0.1 --port "$PORT" --root "$W/data" \
    --db "$W/state/db" --secret-file "$W/secrets.env" \
    --transport-key-file "$W/key" --log-file "$W/logs/s.log" --quiet \
    >>"$W/logs/out" 2>&1 &
  SP=$!
  for _ in $(seq 1 100); do
    ss -ltn 2>/dev/null | grep -q ":$PORT " && return 0
    sleep 0.05
  done
  return 1
}
kill_server() {
  if [ -n "$SP" ] && [ "$(cat "/proc/$SP/comm" 2>/dev/null)" = "backup-server" ]; then
    kill -KILL "$SP" 2>/dev/null
  fi
  wait "$SP" 2>/dev/null
  SP=""
}
wait_part() {
  local dir="$1" target="$2" size=0 part=""
  for _ in $(seq 1 5000); do
    part=$(find "$dir" -name '*.part' -type f 2>/dev/null | head -1)
    [ -n "$part" ] && size=$(stat -c %s "$part" 2>/dev/null || echo 0)
    [ "$size" -ge "$target" ] && { echo "$size"; return 0; }
    sleep 0.01
  done
  echo "$size"
  return 1
}

echo "[resumable-transfer] A. 上传中断与续传"
start || { echo 服务端启动失败; finish; }
./build/backupctl remote register $REMOTE >"$W/reg.log" 2>&1
./build/backupctl remote login $REMOTE >"$W/login.log" 2>&1 || { echo 登录失败; finish; }
head -c 67108864 /dev/urandom >"$W/src/big.bak"
LOCAL_SHA=$(sha256sum "$W/src/big.bak" | cut -d' ' -f1)
( ./build/backupctl remote upload "$W/src/big.bak" --name big.bak $REMOTE >"$W/u1.log" 2>&1 ) &
UP=$!
if ! S=$(wait_part "$W/data" 12582912); then
  record_fail "A.1 上传产生至少 12 MiB 临时数据" "size=$S"
  finish
fi
kill_server
wait $UP 2>/dev/null
U1=$?
[ "$U1" -ne 0 ] && record_pass "A.2 中断后客户端非 0 退出" || record_fail "A.2 中断后客户端非 0 退出" "exit=$U1"
N=$(find "$HOME/.config" -name '*.upload' | wc -l)
[ "$N" -ge 1 ] && record_pass "A.3 客户端保留续传状态" || record_fail "A.3 客户端保留续传状态" "state=$N"
start || { echo 服务端重启失败; finish; }
./build/backupctl remote upload "$W/src/big.bak" --name big.bak $REMOTE >"$W/u2.log" 2>&1
U2=$?
[ "$U2" -eq 0 ] && record_pass "A.4 续传上传成功" || record_fail "A.4 续传上传成功" "$(tail -2 "$W/u2.log" | tr '\n' ' ')"
N=$(grep -c 'upload resumed at offset' "$W/logs/s.log" || true)
[ "$N" -ge 1 ] && record_pass "A.5 服务端确认从检查点继续" || record_fail "A.5 服务端确认从检查点继续" "log=$N"
BLOB=$(find "$W/data/users" -name '*.bak' -type f | head -1)
BLOB_SHA=$(sha256sum "$BLOB" | cut -d' ' -f1)
[ "$LOCAL_SHA" = "$BLOB_SHA" ] && record_pass "A.6 云端 blob 与本地逐字节一致" || record_fail "A.6 云端 blob 与本地逐字节一致" "sha mismatch"
SNAP=$(grep -oE '快照 ID: [0-9a-f]{32}' "$W/u2.log" | grep -oE '[0-9a-f]{32}' | head -1)
[ -n "$SNAP" ] && record_pass "A.7 取得快照 id" || { record_fail "A.7 取得快照 id" "missing"; finish; }

echo "[resumable-transfer] B. 下载中断与续传"
( ./build/backupctl remote download "$SNAP" "$W/out/big.bak" $REMOTE >"$W/d1.log" 2>&1 ) &
DP=$!
if ! S=$(wait_part "$W/out" 12582912); then
  record_fail "B.1 下载产生至少 12 MiB 半成品" "size=$S"
  finish
fi
kill_server
wait $DP 2>/dev/null
D1=$?
[ "$D1" -ne 0 ] && record_pass "B.2 中断后客户端非 0 退出" || record_fail "B.2 中断后客户端非 0 退出" "exit=$D1"
N=$(find "$HOME/.config" -name '*.download' | wc -l)
[ "$N" -ge 1 ] && record_pass "B.3 客户端保留下载状态" || record_fail "B.3 客户端保留下载状态" "state=$N"
start || { echo 服务端重启失败; finish; }
./build/backupctl remote download "$SNAP" "$W/out/big.bak" $REMOTE >"$W/d2.log" 2>&1
D2=$?
[ "$D2" -eq 0 ] && record_pass "B.4 续传下载成功" || record_fail "B.4 续传下载成功" "$(tail -2 "$W/d2.log" | tr '\n' ' ')"
N=$(grep -c 'download resumed at offset' "$W/logs/s.log" || true)
[ "$N" -ge 1 ] && record_pass "B.5 服务端确认从 offset 继续" || record_fail "B.5 服务端确认从 offset 继续" "log=$N"
cmp -s "$W/src/big.bak" "$W/out/big.bak" && record_pass "B.6 下载结果逐字节一致" || record_fail "B.6 下载结果逐字节一致" "cmp 不一致"

echo "[resumable-transfer] C. 半成品损坏的清理"
( ./build/backupctl remote download "$SNAP" "$W/out2/big.bak" $REMOTE >"$W/d3.log" 2>&1 ) &
DP=$!
if ! S=$(wait_part "$W/out2" 12582912); then
  record_fail "C.1 第三次下载产生半成品" "size=$S"
  finish
fi
kill_server
wait $DP 2>/dev/null
PART=$(find "$W/out2" -name '*.part' -type f | head -1)
printf 'CORRUPTED' | dd of="$PART" bs=1 seek=4096 conv=notrunc status=none
start || { echo 服务端重启失败; finish; }
./build/backupctl remote download "$SNAP" "$W/out2/big.bak" $REMOTE >"$W/d4.log" 2>&1
D4=$?
[ "$D4" -ne 0 ] && record_pass "C.2 损坏的半成品被完整性校验拒绝" || record_fail "C.2 损坏的半成品被完整性校验拒绝" "exit=0"
N=$(find "$HOME/.config" -name '*.download' | wc -l)
[ "$N" -eq 0 ] && record_pass "C.3 损坏后状态被丢弃" || record_fail "C.3 损坏后状态被丢弃" "state=$N"
./build/backupctl remote download "$SNAP" "$W/out2/big.bak" $REMOTE >"$W/d5.log" 2>&1
D5=$?
[ "$D5" -eq 0 ] && record_pass "C.4 下一次从头下载成功" || record_fail "C.4 下一次从头下载成功" "$(tail -2 "$W/d5.log" | tr '\n' ' ')"
cmp -s "$W/src/big.bak" "$W/out2/big.bak" && record_pass "C.5 重下结果逐字节一致" || record_fail "C.5 重下结果逐字节一致" "cmp 不一致"

finish
