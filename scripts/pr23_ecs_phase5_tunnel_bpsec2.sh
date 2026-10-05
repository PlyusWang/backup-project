#!/usr/bin/env bash
#
# pr23_ecs_phase5_tunnel_bpsec2.sh —— PR #23 Phase 5：**隧道内**的 BPSEC2。
#
#   bash scripts/pr23_ecs_phase5_tunnel_bpsec2.sh
#
# 从本机（Ubuntu VM）经 SSH 隧道连到 ECS 的 127.0.0.1:18765，用证书模式
# 完成握手与一次真实请求。这一阶段证明的是"应用层协议本身在真实链路上
# 可用"，还不涉及开放公网（Phase 7 才做那件事）。
#
# 客户端仍然**不给任何指纹**：用编译进二进制的内置官方根验服务端证书。
#
# 退出码：0 = 全部通过。

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

ECS="aliyun-ecs"
SERVER_ID="backup-project-cloud-production"
LOCAL_PORT=28765
TUNNEL_PID=""
cleanup() {
  if [ -n "$TUNNEL_PID" ]; then
    kill "$TUNNEL_PID" 2>/dev/null || true
    wait "$TUNNEL_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

PASS=0
FAIL=0
record_pass() { PASS=$((PASS + 1)); echo "  PASS  $1"; }
record_fail() { FAIL=$((FAIL + 1)); echo "  FAIL  $1 -- $2" >&2; }

echo "[phase5] 1/4 构建本机客户端"
if make -j4 client-cli > /tmp/pr23/phase5-build.log 2>&1; then
  record_pass "构建 backupctl（本机）"
else
  record_fail "构建 backupctl" "$(tail -3 /tmp/pr23/phase5-build.log | tr '\n' ' ')"
  echo "[phase5] passed=$PASS failed=$FAIL"
  exit 1
fi
CLIENT="$ROOT_DIR/build/backupctl"

echo "[phase5] 2/4 建立 SSH 隧道 127.0.0.1:$LOCAL_PORT -> ECS 127.0.0.1:18765"
ssh -N -o ExitOnForwardFailure=yes -L "$LOCAL_PORT:127.0.0.1:18765" "$ECS" &
TUNNEL_PID=$!
TUNNEL_READY=0
for _ in $(seq 1 50); do
  if ss -ltn 2>/dev/null | grep -q "127.0.0.1:$LOCAL_PORT "; then
    TUNNEL_READY=1
    break
  fi
  sleep 0.2
done
if [ "$TUNNEL_READY" = "1" ]; then
  record_pass "T01 SSH 隧道就绪（本地 $LOCAL_PORT）"
else
  record_fail "T01 SSH 隧道" "本地端口没有起来"
  echo "[phase5] passed=$PASS failed=$FAIL"
  exit 1
fi

echo "[phase5] 3/4 隧道内证书模式请求"
if "$CLIENT" remote ping --host 127.0.0.1 --port "$LOCAL_PORT" \
     --expected-server-id "$SERVER_ID" > /tmp/pr23/phase5-ping.txt 2>&1; then
  record_pass "T02 隧道内 BPSEC2 握手 + 请求成功（内置官方根，零指纹）"
else
  record_fail "T02 隧道内 BPSEC2" "$(tail -3 /tmp/pr23/phase5-ping.txt | tr '\n' ' ')"
fi

echo "[phase5] 4/4 负向与隧道清理"
if "$CLIENT" remote ping --host 127.0.0.1 --port "$LOCAL_PORT" \
     --expected-server-id some-other-cloud > /dev/null 2>&1; then
  record_fail "T03 server_id 不符必须失败" "竟然成功了"
else
  record_pass "T03 server_id 不符 -> 拒绝"
fi
PIN="$(ssh "$ECS" "/home/ubuntu/backup-project-server/bin/backup-server-keygen --show --key-file /home/ubuntu/backup-project-server/state/transport.key" \
  | sed -n 's/.*--server-key \(sha256:[0-9a-f]*\)$/\1/p' | head -1)"
if "$CLIENT" remote ping --host 127.0.0.1 --port "$LOCAL_PORT" --server-key "$PIN" \
     > /dev/null 2>&1; then
  record_fail "T04 BPSEC1 客户端必须被拒（服务端 require-bpsec2）" "竟然成功了"
else
  record_pass "T04 BPSEC1 降级被拒（隧道里也一样）"
fi

kill "$TUNNEL_PID" 2>/dev/null || true
wait "$TUNNEL_PID" 2>/dev/null || true
TUNNEL_PID=""
sleep 1
if ss -ltn 2>/dev/null | grep -q "127.0.0.1:$LOCAL_PORT "; then
  record_fail "T05 隧道必须被清理掉" "本地端口还在监听"
else
  record_pass "T05 隧道已清理"
fi

echo "[phase5] passed=$PASS failed=$FAIL"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
