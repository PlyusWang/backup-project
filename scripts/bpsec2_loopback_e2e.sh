#!/usr/bin/env bash
#
# bpsec2_loopback_e2e.sh —— PR #23 Phase 2：**回环上的 BPSEC2 端到端**。
#
#   bash scripts/bpsec2_loopback_e2e.sh
#
# 全程用真实进程：真的 backup-server（带证书、--require-bpsec2）+ 真的
# backupctl（证书模式）。不是把库函数拼在一起跑一遍。
#
# 覆盖：
#   A. 离线测试根 -> 服务器传输身份密钥 -> 为它签发证书（临时目录，用完即删）
#   B. 正向：证书模式下 ping / register / login / list 全部成功
#   C. 负向：server_id 不符 / 不受信任的根 / 内置官方根 / BPSEC1 降级 全部失败
#   D. 全程没有 ssh 进程（Phase 8 会用同样口径再证明一次"零 SSH"）
#
# 退出码：0 = 全部通过。

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

WORK_DIR="$(mktemp -d /tmp/bpsec2-loopback-XXXXXX)"
SERVER_PID=""
cleanup() {
  if [ -n "$SERVER_PID" ]; then
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
  rm -rf "$WORK_DIR"
}
trap cleanup EXIT

PASS=0
FAIL=0
record_pass() { PASS=$((PASS + 1)); echo "  PASS  $1"; }
record_fail() { FAIL=$((FAIL + 1)); echo "  FAIL  $1 -- $2" >&2; }

SERVER_ID="backup-project-cloud-production"
PORT=$((20000 + (RANDOM % 20000)))
USER_NAME="bpsec2-user"
ACCOUNT_PASSWORD="loopback-account-password"

echo "[phase2] 构建（真实进程要用真二进制）"
if ! make -j4 all cert-tool > "$WORK_DIR/build.log" 2>&1; then
  record_fail "构建" "$(tail -3 "$WORK_DIR/build.log" | tr '\n' ' ')"
  echo "[phase2] passed=$PASS failed=$FAIL"
  exit 1
fi
record_pass "构建 backupctl + backup-server + backup-cert-tool"

# ---- A. 离线测试根与证书 ----
if ./build/backup-cert-tool root-init --root-key "$WORK_DIR/root.key" \
      --root-id test-loopback-root-a > "$WORK_DIR/root-init.txt" 2>&1; then
  record_pass "A01 生成离线测试根（0600）"
else
  record_fail "A01 生成离线测试根" "$(tail -2 "$WORK_DIR/root-init.txt" | tr '\n' ' ')"
fi
ROOT_PUB="$WORK_DIR/root.key.pub"

if ./build/backup-server-keygen --output "$WORK_DIR/transport.key" \
      > "$WORK_DIR/keygen.txt" 2>&1; then
  record_pass "A02 生成服务器传输身份密钥"
else
  record_fail "A02 生成服务器传输身份密钥" "$(tail -2 "$WORK_DIR/keygen.txt" | tr '\n' ' ')"
fi
SERVER_PUB="$(./build/backup-server-keygen --show --key-file "$WORK_DIR/transport.key" \
  | sed -n 's/.*--server-key hex:\([0-9a-f]*\)$/\1/p' | head -1)"
if [ "$(printf %s "$SERVER_PUB" | wc -c)" = "64" ]; then
  record_pass "A03 取到服务器公钥（64 位十六进制）"
else
  record_fail "A03 取到服务器公钥" "拿到的是 '$SERVER_PUB'"
fi

if ./build/backup-cert-tool issue-server --root-key "$WORK_DIR/root.key" \
      --server-id "$SERVER_ID" --server-pubkey "$SERVER_PUB" \
      --out "$WORK_DIR/server.bpcert" > "$WORK_DIR/issue.txt" 2>&1; then
  record_pass "A04 为这把服务器公钥签发身份证书"
else
  record_fail "A04 签发身份证书" "$(tail -2 "$WORK_DIR/issue.txt" | tr '\n' ' ')"
fi
if ./build/backup-cert-tool verify-server --cert "$WORK_DIR/server.bpcert" \
      --roots "$ROOT_PUB" > "$WORK_DIR/verify.txt" 2>&1; then
  record_pass "A05 证书用自签根验过（verdict = TRUSTED）"
else
  record_fail "A05 证书自验" "$(tail -2 "$WORK_DIR/verify.txt" | tr '\n' ' ')"
fi

# ---- B. 真实服务端 + 真实客户端（全在回环上）----
mkdir -p "$WORK_DIR/data" "$WORK_DIR/state"
printf 'BACKUP_TOKEN_SECRET=%s\n' \
  "$(head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n')" > "$WORK_DIR/secrets.env"
chmod 600 "$WORK_DIR/secrets.env"

SSH_BEFORE="$(pgrep -c -x ssh 2>/dev/null || echo 0)"

./build/backup-server --bind 127.0.0.1 --port "$PORT" \
  --root "$WORK_DIR/data" --db "$WORK_DIR/state/metadata.sqlite3" \
  --secret-file "$WORK_DIR/secrets.env" \
  --transport-key-file "$WORK_DIR/transport.key" \
  --bpsec2-cert-file "$WORK_DIR/server.bpcert" --require-bpsec2 \
  --log-file "$WORK_DIR/server.log" > "$WORK_DIR/server.out" 2>&1 &
SERVER_PID=$!

LISTENING=0
for _ in $(seq 1 100); do
  if ss -ltn 2>/dev/null | grep -q "127.0.0.1:$PORT "; then
    LISTENING=1
    break
  fi
  sleep 0.2
done
if [ "$LISTENING" = "1" ]; then
  record_pass "B01 backup-server 在 127.0.0.1:$PORT 监听（只回环）"
else
  record_fail "B01 服务端监听" "$(tail -3 "$WORK_DIR/server.log" 2>/dev/null | tr '\n' ' ')"
  echo "[phase2] passed=$PASS failed=$FAIL"
  exit 1
fi
if grep -q "BPSEC2 identity certificate loaded" "$WORK_DIR/server.log" 2>/dev/null; then
  record_pass "B02 服务端启动日志确认加载了身份证书"
else
  record_fail "B02 服务端加载证书的日志" "$(grep -c . "$WORK_DIR/server.log" 2>/dev/null || echo 0) 行日志"
fi

PING_OK=0
if ./build/backupctl remote ping --host 127.0.0.1 --port "$PORT" \
      --expected-server-id "$SERVER_ID" --trusted-roots "$ROOT_PUB" \
      > "$WORK_DIR/ping.txt" 2>&1; then
  PING_OK=1
  record_pass "B03 证书模式 ping 成功（没有用任何指纹）"
else
  record_fail "B03 证书模式 ping" "$(tail -3 "$WORK_DIR/ping.txt" | tr '\n' ' ')"
fi

if [ "$PING_OK" = "1" ]; then
  if BACKUP_REMOTE_PASSWORD="$ACCOUNT_PASSWORD" ./build/backupctl remote register \
        --user "$USER_NAME" --host 127.0.0.1 --port "$PORT" \
        --expected-server-id "$SERVER_ID" --trusted-roots "$ROOT_PUB" \
        > "$WORK_DIR/register.txt" 2>&1; then
    record_pass "B04 证书模式下注册账户成功"
  else
    record_fail "B04 证书模式下注册账户" "$(tail -3 "$WORK_DIR/register.txt" | tr '\n' ' ')"
  fi
  if BACKUP_REMOTE_PASSWORD="$ACCOUNT_PASSWORD" ./build/backupctl remote login \
        --user "$USER_NAME" --host 127.0.0.1 --port "$PORT" \
        --expected-server-id "$SERVER_ID" --trusted-roots "$ROOT_PUB" \
        > "$WORK_DIR/login.txt" 2>&1; then
    record_pass "B05 证书模式下登录成功"
  else
    record_fail "B05 证书模式下登录" "$(tail -3 "$WORK_DIR/login.txt" | tr '\n' ' ')"
  fi
  if BACKUP_REMOTE_PASSWORD="$ACCOUNT_PASSWORD" ./build/backupctl remote list \
        --user "$USER_NAME" --host 127.0.0.1 --port "$PORT" \
        --expected-server-id "$SERVER_ID" --trusted-roots "$ROOT_PUB" \
        > "$WORK_DIR/list.txt" 2>&1; then
    record_pass "B06 证书模式下读取备份列表成功"
  else
    record_fail "B06 证书模式下读取列表" "$(tail -3 "$WORK_DIR/list.txt" | tr '\n' ' ')"
  fi
fi

# ---- C. 负向：每一条都必须失败 ----
if ./build/backupctl remote ping --host 127.0.0.1 --port "$PORT" \
      --expected-server-id "some-other-cloud" --trusted-roots "$ROOT_PUB" \
      > "$WORK_DIR/wrong-id.txt" 2>&1; then
  record_fail "C01 server_id 不符必须失败" "竟然成功了"
else
  record_pass "C01 server_id 不符 -> 拒绝"
fi
if grep -qi "server_id\|名字" "$WORK_DIR/wrong-id.txt"; then
  record_pass "C02 失败原因说的是 server_id 不符（用户看得懂）"
else
  record_fail "C02 失败原因" "$(tail -2 "$WORK_DIR/wrong-id.txt" | tr '\n' ' ')"
fi

if ./build/backupctl remote ping --host 127.0.0.1 --port "$PORT" \
      --expected-server-id "$SERVER_ID" > "$WORK_DIR/no-roots.txt" 2>&1; then
  record_fail "C03 不给 --trusted-roots 时用内置官方根，陌生根必须失败" "竟然成功了"
else
  record_pass "C03 陌生根（内置官方根里没有它）-> 拒绝"
fi

SERVER_PIN="$(./build/backupctl remote ping --help >/dev/null 2>&1; true)"
PIN="$(./build/backup-server-keygen --show --key-file "$WORK_DIR/transport.key" \
  | sed -n 's/.*--server-key \(sha256:[0-9a-f]*\)$/\1/p' | head -1)"
if ./build/backupctl remote ping --host 127.0.0.1 --port "$PORT" \
      --server-key "$PIN" > "$WORK_DIR/downgrade.txt" 2>&1; then
  record_fail "C04 只接受 BPSEC2 的服务端必须拒绝 BPSEC1 客户端" "竟然成功了"
else
  record_pass "C04 BPSEC1 客户端被拒（拒绝降级）"
fi

# ---- D. 零 SSH ----
SSH_AFTER="$(pgrep -c -x ssh 2>/dev/null || echo 0)"
if [ "$SSH_BEFORE" = "$SSH_AFTER" ]; then
  record_pass "D01 全程没有新增 ssh 进程（delta = 0）"
else
  record_fail "D01 ssh 进程数" "$SSH_BEFORE -> $SSH_AFTER"
fi

echo "[phase2] passed=$PASS failed=$FAIL"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
