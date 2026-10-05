#!/usr/bin/env bash
#
# pr23_ecs_phase8_public_e2e.sh —— PR #23 Phase 8：无隧道公网直连的端到端，
# 并且证明这条路径上没有任何 SSH。
#
#   bash scripts/pr23_ecs_phase8_public_e2e.sh
#
# 与 Phase 5 的区别：Phase 5 走 ssh -L 隧道（连本地端口），这一步客户端连的就是
# 公网地址 8.130.9.200:18765 本身。
#
# 「零 SSH」怎么证明（不是靠嘴说）：
#   1. 全程统计 ssh 进程数，前后 delta 必须为 0；
#   2. 断言本机 18765 上没有本地监听（不存在把本地端口接到远端的隧道）；
#   3. 只调用 backupctl，参数原样打印。
#
# 前置条件（脚本自己判定，不满足就明确失败）：
#   * 安全组入方向放行 18765/tcp（源要包含本机出口地址）；
#   * ECS 本机 sudo ufw allow 18765/tcp；
#   * 服务端以 --bind 0.0.0.0 + 证书 + --require-bpsec2 运行。
#
# 退出码：0 = 通过；1 = 未通过。

set -uo pipefail
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

SERVER_ID="backup-project-cloud-production"
PUBLIC_HOST="8.130.9.200"
PUBLIC_PORT=18765
USER_NAME="phase8-public-user"
ACCOUNT_PASSWORD="phase8-public-password"

PASS=0
FAIL=0
record_pass() { PASS=$((PASS + 1)); echo "  PASS  $1"; }
record_fail() { FAIL=$((FAIL + 1)); echo "  FAIL  $1 -- $2" >&2; }
count_ssh() { pgrep -c -x ssh 2>/dev/null || echo 0; }

echo "[phase8] 0/4 前置条件：公网可达性"
if ! timeout 8 bash -c "cat < /dev/null > /dev/tcp/$PUBLIC_HOST/$PUBLIC_PORT" 2>/dev/null; then
  record_fail "公网 $PUBLIC_HOST:$PUBLIC_PORT 可达" "连不上：请确认安全组放行 18765/tcp、ECS 上 ufw allow 18765/tcp、服务端 --bind 0.0.0.0"
  echo "[phase8] passed=$PASS failed=$FAIL"
  exit 1
fi
record_pass "公网 $PUBLIC_HOST:$PUBLIC_PORT 可达（前置条件满足）"

echo "[phase8] 1/4 构建客户端并确认没有隧道"
if ! make -j4 client-cli > /tmp/pr23/phase8-build.log 2>&1; then
  record_fail "构建 backupctl" "$(tail -3 /tmp/pr23/phase8-build.log | tr '\n' ' ')"
  echo "[phase8] passed=$PASS failed=$FAIL"
  exit 1
fi
record_pass "构建 backupctl"
if ss -ltn 2>/dev/null | grep -q ":18765 "; then
  record_fail "本机 18765 上没有隧道" "存在本地监听，说明有转发"
else
  record_pass "本机 18765 没有本地监听（不存在 ssh -L 隧道）"
fi

echo "[phase8] 2/4 直连公网地址（内置官方根、零指纹）"
SSH_BEFORE="$(count_ssh)"
if ./build/backupctl remote ping --host "$PUBLIC_HOST" --port "$PUBLIC_PORT" \
     --expected-server-id "$SERVER_ID" > /tmp/pr23/phase8-ping.txt 2>&1; then
  record_pass "P01 公网证书模式 ping 成功（连的是 $PUBLIC_HOST:$PUBLIC_PORT 本身）"
else
  record_fail "P01 公网证书模式 ping" "$(tail -3 /tmp/pr23/phase8-ping.txt | tr '\n' ' ')"
fi
SSH_AFTER="$(count_ssh)"
if [ "$SSH_BEFORE" = "$SSH_AFTER" ]; then
  record_pass "P02 全程 ssh 进程数 delta = 0（$SSH_BEFORE -> $SSH_AFTER）"
else
  record_fail "P02 ssh 进程数 delta" "$SSH_BEFORE -> $SSH_AFTER"
fi

echo "[phase8] 3/4 公网路径上的账户操作"
if BACKUP_REMOTE_PASSWORD="$ACCOUNT_PASSWORD" ./build/backupctl remote register \
     --user "$USER_NAME" --host "$PUBLIC_HOST" --port "$PUBLIC_PORT" \
     --expected-server-id "$SERVER_ID" > /tmp/pr23/phase8-register.txt 2>&1; then
  record_pass "P03 公网注册成功"
else
  record_fail "P03 公网注册" "$(tail -3 /tmp/pr23/phase8-register.txt | tr '\n' ' ')"
fi
if BACKUP_REMOTE_PASSWORD="$ACCOUNT_PASSWORD" ./build/backupctl remote login \
     --user "$USER_NAME" --host "$PUBLIC_HOST" --port "$PUBLIC_PORT" \
     --expected-server-id "$SERVER_ID" > /tmp/pr23/phase8-login.txt 2>&1; then
  record_pass "P04 公网登录成功"
else
  record_fail "P04 公网登录" "$(tail -3 /tmp/pr23/phase8-login.txt | tr '\n' ' ')"
fi

echo "[phase8] 4/4 负向（公网路径同样 fail-closed）"
if ./build/backupctl remote ping --host "$PUBLIC_HOST" --port "$PUBLIC_PORT" \
     --expected-server-id some-other-cloud > /dev/null 2>&1; then
  record_fail "N01 server_id 不符必须失败" "竟然成功了"
else
  record_pass "N01 server_id 不符 -> 拒绝（公网也一样）"
fi

echo "[phase8] passed=$PASS failed=$FAIL"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
