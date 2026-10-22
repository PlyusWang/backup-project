#!/usr/bin/env bash
#
# resumable_transfer_test.sh —— 上传 / 下载断点续传的端到端回归。
#
#   bash scripts/resumable_transfer_test.sh
#
# 覆盖的是“真实进程被杀”之后的恢复，而不是同一次连接里的重试：
#   A. 上传进行中杀掉服务端 -> 客户端失败但保留续传状态
#   B. 服务端重启 -> 新客户端进程从服务端已验证的检查点继续，最终 blob 的
#      SHA-256 与本地文件一致
#   C. 下载进行中杀掉服务端 -> 半成品与状态保留
#   D. 服务端重启 -> 新客户端进程从半成品继续，最终文件逐字节一致
#   E. 前端损坏的半成品必须被拒绝，客户端从头重传后仍然成功
#
# 退出码：0 = 全部通过，非 0 = 有 FAIL。

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR" || exit 1

PASS=0
FAIL=0
record_pass() { PASS=$((PASS + 1)); echo "  PASS  $1"; }
record_fail() { FAIL=$((FAIL + 1)); echo "  FAIL  $1 -- $2"; }
finish() {
  echo "[resumable-transfer] passed=$PASS failed=$FAIL"
  [ "$FAIL" -eq 0 ] || exit 1
}

TEST_ROOT="$(mktemp -d /tmp/resumable-transfer-test-XXXXXX)"
SERVER_PID=""
cleanup() {
  if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill -KILL "$SERVER_PID" 2>/dev/null
    wait "$SERVER_PID" 2>/dev/null
  fi
  rm -rf "$TEST_ROOT"
}
trap cleanup EXIT

if [ ! -x ./build/backupctl ] || [ ! -x ./build/backup-server ]; then
  make -j4 all server >"$TEST_ROOT/build.log" 2>&1 || {
    echo "构建失败"; tail -5 "$TEST_ROOT/build.log"; exit 1;
  }
fi

WORK="$TEST_ROOT/e2e"
mkdir -p "$WORK/data" "$WORK/state" "$WORK/logs" "$WORK/src" "$WORK/home/.config" "$WORK/downloads"
export HOME="$WORK/home"
export BACKUP_REMOTE_PASSWORD="resume-test-password"

head -c 32 /dev/urandom | sha256sum | cut -c1-64 >"$WORK/secret.value"
echo "BACKUP_TOKEN_SECRET=$(cat "$WORK/secret.value")" >"$WORK/secrets.env"
chmod 600 "$WORK/secrets.env"
./build/backup-server-keygen --output "$WORK/transport.key" >"$WORK/keygen.out" 2>&1 || {
  echo "keygen 失败"; cat "$WORK/keygen.out"; exit 1;
}
BACKUP_REMOTE_SERVER_KEY="$(grep -oE 'sha256:[0-9a-f]{64}' "$WORK/keygen.out" | head -1)"
export BACKUP_REMOTE_SERVER_KEY

PORT=0
for candidate in $(seq 24100 24200); do
  if ! ss -ltn 2>/dev/null | grep -q ":$candidate "; then PORT=$candidate; break; fi
done
if [ "$PORT" = "0" ]; then echo "没有空闲端口"; exit 1; fi

start_server() {
  ./build/backup-server --bind 127.0.0.1 --port "$PORT" --root "$WORK/data" \
    --db "$WORK/state/metadata.sqlite3" --secret-file "$WORK/secrets.env" \
    --transport-key-file "$WORK/transport.key" --pid-file "$WORK/state/server.pid" \
    --log-file "$WORK/logs/server.log" --quiet >>"$WORK/logs/server.out" 2>&1 &
  SERVER_PID=$!
  for _ in $(seq 1 80); do
    if ss -ltn 2>/dev/null | grep -q "127.0.0.1:$PORT "; then return 0; fi
    sleep 0.1
  done
  return 1
}
stop_server_hard() {
  # 只允许停掉我们自己启动的 backup-server：PID 为空、1 或换成了别的进程
  # 一律不动。测试脚本没有资格杀任何其它东西。
  if [ -z "${SERVER_PID:-}" ] || [ "$SERVER_PID" -le 1 ]; then
    return 0
  fi
  if [ ! -r "/proc/$SERVER_PID/comm" ] ||
     [ "$(cat "/proc/$SERVER_PID/comm" 2>/dev/null)" != "backup-server" ]; then
    echo "  WARN 拒绝停止非 backup-server 进程（pid=$SERVER_PID）"
    SERVER_PID=""
    return 0
  fi
  kill -TERM "$SERVER_PID" 2>/dev/null
  for _ in $(seq 1 30); do
    kill -0 "$SERVER_PID" 2>/dev/null || break
    sleep 0.1
  done
  kill -KILL "$SERVER_PID" 2>/dev/null
  wait "$SERVER_PID" 2>/dev/null
  SERVER_PID=""
}

start_server || { echo "服务端未监听"; exit 1; }
REMOTE="--host 127.0.0.1 --port $PORT --user resume_user"
./build/backupctl remote register $REMOTE >"$WORK/register.log" 2>&1 || {
  echo "注册失败"; tail -3 "$WORK/register.log"; exit 1;
}
./build/backupctl remote login $REMOTE >"$WORK/login.log" 2>&1 || {
  echo "登录失败"; tail -3 "$WORK/login.log"; exit 1;
}

# 64 MiB：足够跨过 4 MiB 检查点，又不会让测试跑太久。
head -c 67108864 /dev/urandom >"$WORK/src/big.bak"
LOCAL_SHA="$(sha256sum "$WORK/src/big.bak" | cut -d' ' -f1)"

part_size() {
  local part
  part="$(find "$1" -name '*.part' -type f 2>/dev/null | head -1)"
  [ -n "$part" ] && stat -c '%s' "$part" 2>/dev/null || echo 0
}
upload_state_count() {
  find "$HOME/.config" -name '*.upload' -type f 2>/dev/null | wc -l
}
download_state_count() {
  find "$HOME/.config" -name '*.download' -type f 2>/dev/null | wc -l
}

echo "[resumable-transfer] A. 上传中断"
( ./build/backupctl remote upload "$WORK/src/big.bak" --name resume-big.bak $REMOTE >"$WORK/upload1.log" 2>&1 ) &
UPLOAD_PID=$!
# 等到服务端真的写进去 8 MiB，再杀服务端：这样一定不是“还没 BEGIN”的空失败。
SIZE=0
for _ in $(seq 1 300); do
  SIZE=$(part_size "$WORK/data")
  [ "$SIZE" -ge 8388608 ] && break
  sleep 0.1
done
if [ "$SIZE" -lt 8388608 ]; then
  record_fail "A.1 上传中途终止" "没等到 8 MiB 临时数据（size=$SIZE）"
  finish
fi
stop_server_hard
wait "$UPLOAD_PID" 2>/dev/null
UPLOAD1_CODE=$?
if [ "$UPLOAD1_CODE" -ne 0 ]; then
  record_pass "A.1 上传中途终止（客户端非 0 退出）"
else
  record_fail "A.1 上传中途终止" "服务端被杀之后客户端仍然返回 0"
fi
if [ "$(upload_state_count)" -ge 1 ]; then
  record_pass "A.2 续传状态已落盘"
else
  record_fail "A.2 续传状态已落盘" "找不到 *.upload 状态文件"
fi

echo "[resumable-transfer] B. 上传从检查点继续"
start_server || { echo "服务端重启失败"; finish; }
./build/backupctl remote upload "$WORK/src/big.bak" --name resume-big.bak $REMOTE >"$WORK/upload2.log" 2>&1
UPLOAD2_CODE=$?
if [ "$UPLOAD2_CODE" -eq 0 ]; then
  record_pass "B.1 续传上传成功"
else
  record_fail "B.1 续传上传成功" "$(tail -2 "$WORK/upload2.log" | tr '\n' ' ')"
fi
if grep -q "upload resumed at offset" "$WORK/logs/server.log"; then
  record_pass "B.2 服务端确认从检查点继续"
else
  record_fail "B.2 服务端确认从检查点继续" "服务端日志里没有 resume 记录"
fi
./build/backupctl remote list $REMOTE >"$WORK/list.log" 2>&1
REMOTE_SHA="$(grep -A3 'resume-big.bak' "$WORK/list.log" | grep -oE '[0-9a-f]{64}' | head -1)"
if [ "$REMOTE_SHA" = "$LOCAL_SHA" ]; then
  record_pass "B.3 云端 blob 的 SHA-256 与本地一致"
else
  record_fail "B.3 云端 blob 的 SHA-256 与本地一致" "cloud=$REMOTE_SHA local=$LOCAL_SHA"
fi
SNAPSHOT_ID="$(grep -B1 'resume-big.bak' "$WORK/list.log" | grep -oE '[0-9a-f]{32}' | head -1)"
if [ -z "$SNAPSHOT_ID" ]; then
  SNAPSHOT_ID="$(grep -oE '[0-9a-f]{32}' "$WORK/list.log" | head -1)"
fi
[ -n "$SNAPSHOT_ID" ] || { record_fail "B.4 取得快照 id" "列表里没有 id"; finish; }
record_pass "B.4 取得快照 id"

echo "[resumable-transfer] C. 下载中断"
( timeout 120 ./build/backupctl remote download "$SNAPSHOT_ID" "$WORK/downloads/big.bak" $REMOTE >"$WORK/download1.log" 2>&1 ) &
DOWNLOAD_PID=$!
SIZE=0
for _ in $(seq 1 300); do
  SIZE=$(part_size "$WORK/downloads")
  [ "$SIZE" -ge 8388608 ] && break
  sleep 0.1
done
if [ "$SIZE" -lt 8388608 ]; then
  record_fail "C.1 下载中途终止" "没等到 8 MiB 半成品（size=$SIZE）"
  finish
fi
stop_server_hard
wait "$DOWNLOAD_PID" 2>/dev/null
if [ "$(download_state_count)" -ge 1 ]; then
  record_pass "C.1 下载状态已落盘"
else
  record_fail "C.1 下载状态已落盘" "找不到 *.download 状态文件"
fi

echo "[resumable-transfer] D. 下载从半成品继续"
start_server || { echo "服务端重启失败"; finish; }
./build/backupctl remote download "$SNAPSHOT_ID" "$WORK/downloads/big.bak" $REMOTE >"$WORK/download2.log" 2>&1
DOWNLOAD2_CODE=$?
if [ "$DOWNLOAD2_CODE" -eq 0 ]; then
  record_pass "D.1 续传下载成功"
else
  record_fail "D.1 续传下载成功" "$(tail -2 "$WORK/download2.log" | tr '\n' ' ')"
fi
if grep -q "download resumed at offset" "$WORK/logs/server.log"; then
  record_pass "D.2 服务端确认从 offset 继续"
else
  record_fail "D.2 服务端确认从 offset 继续" "服务端日志里没有 download resume 记录"
fi
if cmp -s "$WORK/src/big.bak" "$WORK/downloads/big.bak"; then
  record_pass "D.3 下载结果逐字节一致"
else
  record_fail "D.3 下载结果逐字节一致" "cmp 不一致"
fi

echo "[resumable-transfer] E. 损坏的半成品必须被拒绝"
rm -f "$WORK/downloads/big.bak"
# 再制造一次中断，然后篡改半成品的前缀。
( timeout 120 ./build/backupctl remote download "$SNAPSHOT_ID" "$WORK/downloads/big.bak" $REMOTE >"$WORK/download3.log" 2>&1 ) &
DOWNLOAD_PID=$!
for _ in $(seq 1 300); do
  SIZE=$(part_size "$WORK/downloads")
  [ "$SIZE" -ge 8388608 ] && break
  sleep 0.1
done
stop_server_hard
wait "$DOWNLOAD_PID" 2>/dev/null
PART="$(find "$WORK/downloads" -name '*.part' -type f | head -1)"
if [ -n "$PART" ]; then
  printf 'CORRUPTED' | dd of="$PART" bs=1 seek=4096 conv=notrunc status=none
fi
start_server || { echo "服务端重启失败"; finish; }
./build/backupctl remote download "$SNAPSHOT_ID" "$WORK/downloads/big.bak" $REMOTE >"$WORK/download4.log" 2>&1
if cmp -s "$WORK/src/big.bak" "$WORK/downloads/big.bak"; then
  record_pass "E.1 前缀损坏后重新下载仍得到正确文件"
else
  record_fail "E.1 前缀损坏后重新下载仍得到正确文件" "$(tail -2 "$WORK/download4.log" | tr '\n' ' ')"
fi

echo "[resumable-transfer] F. 不留半成品"
if find "$WORK/downloads" -name '*.part' -type f | grep -q .; then
  record_fail "F.1 成功发布后不留半成品" "还有 .part 文件"
else
  record_pass "F.1 成功发布后不留半成品"
fi

finish
