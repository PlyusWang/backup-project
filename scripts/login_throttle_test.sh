#!/usr/bin/env bash
#
# login_throttle_test.sh —— §33 登录失败节流的端到端测试（PR #23）。
#
#   bash scripts/login_throttle_test.sh
#
# 用真实 backup-server + 真实 backupctl 验证三件事：
#   1. 连续错口令达到阈值之后，**正确口令也会被拒**（这是节流的意义所在：
#      攻击者不能在锁定窗口里碰对一次就绕过）；
#   2. 窗口过去之后，正确口令恢复正常；
#   3. 服务端日志里留下明确的限速记录（而不是伪装成口令错误）。
#
# 退出码：0 = 全部通过。

set -uo pipefail
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

WORK_DIR="$(mktemp -d /tmp/login-throttle-XXXXXX)"
SERVER_PID=""
cleanup() {
  if [ -n "$SERVER_PID" ]; then kill "$SERVER_PID" 2>/dev/null || true; wait "$SERVER_PID" 2>/dev/null || true; fi
  rm -rf "$WORK_DIR"
}
trap cleanup EXIT

PASS=0
FAIL=0
record_pass() { PASS=$((PASS + 1)); echo "  PASS  $1"; }
record_fail() { FAIL=$((FAIL + 1)); echo "  FAIL  $1 -- $2" >&2; }

PORT=$((20000 + (RANDOM % 20000)))
SERVER_ID="login-throttle-test"
USER_NAME="throttle-user"
GOOD="correct-horse-battery"
BAD="wrong-password-1"
THRESHOLD=3
WINDOW=5

if ! make -j4 all server cert-tool > "$WORK_DIR/build.log" 2>&1; then
  record_fail "构建" "$(tail -3 "$WORK_DIR/build.log" | tr '\n' ' ')"; exit 1
fi
record_pass "构建 backupctl + backup-server + backup-cert-tool"

./build/backup-cert-tool root-init --root-key "$WORK_DIR/root.key" --root-id test-throttle-root > /dev/null 2>&1
./build/backup-server-keygen --output "$WORK_DIR/transport.key" > /dev/null 2>&1
SERVER_PUB="$(./build/backup-server-keygen --show --key-file "$WORK_DIR/transport.key" | sed -n 's/.*--server-key hex:\([0-9a-f]*\)$/\1/p' | head -1)"
./build/backup-cert-tool issue-server --root-key "$WORK_DIR/root.key" --server-id "$SERVER_ID" --server-pubkey "$SERVER_PUB" --out "$WORK_DIR/server.bpcert" > /dev/null 2>&1
ROOT_PUB="$WORK_DIR/root.key.pub"
mkdir -p "$WORK_DIR/data" "$WORK_DIR/state"
printf 'BACKUP_TOKEN_SECRET=%s\n' "$(head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n')" > "$WORK_DIR/secrets.env"
chmod 600 "$WORK_DIR/secrets.env"

./build/backup-server --bind 127.0.0.1 --port "$PORT" \
  --root "$WORK_DIR/data" --db "$WORK_DIR/state/meta.sqlite3" \
  --secret-file "$WORK_DIR/secrets.env" --transport-key-file "$WORK_DIR/transport.key" \
  --bpsec2-cert-file "$WORK_DIR/server.bpcert" --require-bpsec2 \
  --max-login-failures "$THRESHOLD" --login-lockout-seconds "$WINDOW" \
  --log-file "$WORK_DIR/server.log" > "$WORK_DIR/server.out" 2>&1 &
SERVER_PID=$!
for _ in $(seq 1 100); do
  ss -ltn 2>/dev/null | grep -q "127.0.0.1:$PORT " && break
  sleep 0.2
done
if ss -ltn 2>/dev/null | grep -q "127.0.0.1:$PORT "; then
  record_pass "服务端起来了（证书模式 + 限速阈值 $THRESHOLD / 窗口 ${WINDOW}s）"
else
  record_fail "服务端启动" "$(tail -3 "$WORK_DIR/server.out" | tr '\n' ' ')"; exit 1
fi

CLIENT_BASE="./build/backupctl remote"
COMMON="--host 127.0.0.1 --port $PORT --expected-server-id $SERVER_ID --trusted-roots $ROOT_PUB"

# 先把账户建好（注册不受限速影响）。
if BACKUP_REMOTE_PASSWORD="$GOOD" ./build/backupctl remote register --user "$USER_NAME" $COMMON > "$WORK_DIR/register.txt" 2>&1; then
  record_pass "注册账户成功（注册路径不应被限速影响）"
else
  record_fail "注册账户" "$(tail -3 "$WORK_DIR/register.txt" | tr '\n' ' ')"
fi

# 连续错口令到阈值。
for i in $(seq 1 "$THRESHOLD"); do
  if BACKUP_REMOTE_PASSWORD="$BAD" ./build/backupctl remote login --user "$USER_NAME" $COMMON > /dev/null 2>&1; then
    record_fail "第 $i 次错口令必须失败" "竟然成功了"
  fi
done
record_pass "连续 $THRESHOLD 次错口令全部被拒"

# 关键断言：锁定期间**正确口令也被拒**。
if BACKUP_REMOTE_PASSWORD="$GOOD" ./build/backupctl remote login --user "$USER_NAME" $COMMON > "$WORK_DIR/locked.txt" 2>&1; then
  record_fail "锁定期间正确口令也必须被拒" "竟然成功了 —— 节流可被绕过"
else
  record_pass "锁定期间正确口令也被拒（攻击者无法在窗口内碰对一次就绕过）"
fi

if grep -q "login throttle engaged" "$WORK_DIR/server.log" 2>/dev/null; then
  record_pass "服务端日志明确记录限速（不伪装成口令错误）"
else
  record_fail "服务端日志记录限速" "日志里没有 login throttle engaged"
fi

# 等窗口过去，正确口令应当恢复正常。
sleep $((WINDOW + 2))
if BACKUP_REMOTE_PASSWORD="$GOOD" ./build/backupctl remote login --user "$USER_NAME" $COMMON > "$WORK_DIR/after.txt" 2>&1; then
  record_pass "窗口过去之后正确口令恢复（阈值 ${THRESHOLD} / 窗口 ${WINDOW}s）"
else
  record_fail "窗口过后正确口令应恢复" "$(tail -3 "$WORK_DIR/after.txt" | tr '\n' ' ')"
fi

echo "[login-throttle] passed=$PASS failed=$FAIL"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1