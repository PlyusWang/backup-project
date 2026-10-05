#!/usr/bin/env bash
#
# pr23_ecs_phase4_local_bpsec2.sh —— PR #23 Phase 4：**在 ECS 本机**验证
# 部署上去的服务端真的在用签名身份（BPSEC2）。
#
#   bash scripts/pr23_ecs_phase4_local_bpsec2.sh
#
# 关键点：客户端**不给任何指纹、也不给根文件**，直接用编译进二进制的
# 内置官方根去验服务端证书。这条路径就是官方云端用户将来走的那条：
# 用户不需要知道、也不需要核对任何东西。
#
# 客户端二进制在 ECS 上就地构建（本机 glibc 2.39 / ECS 2.35，拷过去跑不了）。
#
# 退出码：0 = 全部通过。

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

ECS="aliyun-ecs"
SERVER_ROOT="/home/ubuntu/backup-project-server"
SERVER_ID="backup-project-cloud-production"
STAGE="/tmp/pr23-phase4"
mkdir -p "$STAGE"

PASS=0
FAIL=0
record_pass() { PASS=$((PASS + 1)); echo "  PASS  $1"; }
record_fail() { FAIL=$((FAIL + 1)); echo "  FAIL  $1 -- $2" >&2; }

echo "[phase4] 1/4 打包客户端源码子集并上传（在 ECS 上就地构建）"
if ! tar -czf "$STAGE/client-src.tar.gz" Makefile include app src third_party; then
  record_fail "打包客户端源码" "tar 失败"
  echo "[phase4] passed=$PASS failed=$FAIL"
  exit 1
fi
scp -q "$STAGE/client-src.tar.gz" "$ECS:$SERVER_ROOT/deploy/" \
  || { record_fail "上传客户端源码" "scp 失败"; echo "[phase4] passed=$PASS failed=$FAIL"; exit 1; }

echo "[phase4] 2/4 在 ECS 上构建 backupctl"
if ssh "$ECS" "cd $SERVER_ROOT/deploy && rm -rf client-src && mkdir -p client-src && tar -xzf client-src.tar.gz -C client-src && cd client-src && make -j2 client-cli > build.log 2>&1 && echo BUILT"; then
  record_pass "在 ECS 上就地构建 backupctl"
else
  record_fail "在 ECS 上构建 backupctl" "$(ssh "$ECS" "tail -5 $SERVER_ROOT/deploy/client-src/build.log" 2>&1 | tr '\n' ' ')"
  echo "[phase4] passed=$PASS failed=$FAIL"
  exit 1
fi

CLIENT="$SERVER_ROOT/deploy/client-src/build/backupctl"

echo "[phase4] 3/4 证书模式 ping（只给 server_id，用内置官方根，零指纹）"
if ssh "$ECS" "$CLIENT remote ping --host 127.0.0.1 --port 18765 --expected-server-id $SERVER_ID" \
     > "$STAGE/ping.txt" 2>&1; then
  record_pass "P01 ECS 本机用内置官方根验签成功（没有任何指纹）"
else
  record_fail "P01 ECS 本机证书模式 ping" "$(tail -3 "$STAGE/ping.txt" | tr '\n' ' ')"
fi

echo "[phase4] 4/4 负向"
if ssh "$ECS" "$CLIENT remote ping --host 127.0.0.1 --port 18765 --expected-server-id some-other-cloud" \
     > "$STAGE/wrong-id.txt" 2>&1; then
  record_fail "P02 server_id 不符必须失败" "竟然成功了"
else
  record_pass "P02 server_id 不符 -> 拒绝"
fi

PIN="$(ssh "$ECS" "$SERVER_ROOT/bin/backup-server-keygen --show --key-file $SERVER_ROOT/state/transport.key" \
  | sed -n 's/.*--server-key \(sha256:[0-9a-f]*\)$/\1/p' | head -1)"
if ssh "$ECS" "$CLIENT remote ping --host 127.0.0.1 --port 18765 --server-key $PIN" \
     > "$STAGE/downgrade.txt" 2>&1; then
  record_fail "P03 只接受 BPSEC2 的服务端必须拒绝 BPSEC1 客户端" "竟然成功了"
else
  record_pass "P03 BPSEC1 客户端被拒（拒绝降级）"
fi

# 服务端日志里必须留下"证书已加载"的痕迹，并且公网端口仍然没开。
if ssh "$ECS" "grep -q 'BPSEC2 identity certificate loaded' $SERVER_ROOT/logs/server.log"; then
  record_pass "P04 服务端日志确认加载了身份证书"
else
  record_fail "P04 服务端日志" "没有找到证书加载记录"
fi
if ssh "$ECS" "ss -ltn | grep -q '0.0.0.0:18765'"; then
  record_fail "P05 公网端口必须仍然关闭（Phase 7 之前）" "0.0.0.0:18765 已经在监听"
else
  record_pass "P05 公网端口仍然关闭（只在 127.0.0.1:18765）"
fi

echo "[phase4] passed=$PASS failed=$FAIL"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
