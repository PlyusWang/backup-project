#!/usr/bin/env bash
#
# pr23_ecs_phase7_public_bind.sh —— PR #23 Phase 7：把官方云端的 18765
# 开放到公网（**只在 Phase 6 审计之后**）。
#
#   bash scripts/pr23_ecs_phase7_public_bind.sh
#
# 两半：
#   A. 先在本地用**真实进程**验证新加的公网绑定规则（三条：不给开关必须拒绝、
#      给开关但不给证书必须拒绝、两个都给才允许监听 0.0.0.0）；
#   B. 再对 ECS 做同样的事：新取一个回滚点、带 --allow-public-bind 重启、
#      然后**从外部真的去连** —— 能连上才算 Phase 7 完成；连不上就说明
#      安全组还没放行，如实记录成 SECURITY_GROUP_AUTOMATION = UNAVAILABLE
#      并给出需要人工做的那一步。
#
# 退出码：0 = 公网已经真的可达；1 = 未达（附带原因）。

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

ECS="aliyun-ecs"
SERVER_ROOT="/home/ubuntu/backup-project-server"
SERVER_ID="backup-project-cloud-production"
WORK_DIR="$(mktemp -d /tmp/pr23-phase7-XXXXXX)"
trap 'rm -rf "$WORK_DIR"' EXIT

PASS=0
FAIL=0
record_pass() { PASS=$((PASS + 1)); echo "  PASS  $1"; }
record_fail() { FAIL=$((FAIL + 1)); echo "  FAIL  $1 -- $2" >&2; }

echo "[phase7] A. 本地验证公网绑定规则（真实进程）"
if ! make -j4 server cert-tool > "$WORK_DIR/build.log" 2>&1; then
  record_fail "构建" "$(tail -3 "$WORK_DIR/build.log" | tr '\n' ' ')"
  exit 1
fi
record_pass "构建 backup-server + backup-cert-tool"

./build/backup-cert-tool root-init --root-key "$WORK_DIR/root.key" --root-id test-phase7-root \
  > "$WORK_DIR/root-init.txt" 2>&1
./build/backup-server-keygen --output "$WORK_DIR/transport.key" > "$WORK_DIR/keygen.txt" 2>&1
SERVER_PUB="$(./build/backup-server-keygen --show --key-file "$WORK_DIR/transport.key" \
  | sed -n 's/.*--server-key hex:\([0-9a-f]*\)$/\1/p' | head -1)"
./build/backup-cert-tool issue-server --root-key "$WORK_DIR/root.key" \
  --server-id "$SERVER_ID" --server-pubkey "$SERVER_PUB" --out "$WORK_DIR/server.bpcert" \
  > "$WORK_DIR/issue.txt" 2>&1

mkdir -p "$WORK_DIR/data" "$WORK_DIR/state"
printf 'BACKUP_TOKEN_SECRET=%s\n' "$(head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n')" > "$WORK_DIR/secrets.env"
chmod 600 "$WORK_DIR/secrets.env"

try_start() {  # try_start <端口> <附加参数...>
  local port="$1"
  shift
  ./build/backup-server --bind 0.0.0.0 --port "$port" \
    --root "$WORK_DIR/data" --db "$WORK_DIR/state/meta-$port.sqlite3" \
    --secret-file "$WORK_DIR/secrets.env" \
    --transport-key-file "$WORK_DIR/transport.key" "$@" > "$WORK_DIR/start-$port.log" 2>&1 &
  local pid=$!
  sleep 1.5
  if kill -0 "$pid" 2>/dev/null; then
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    return 0
  fi
  return 1
}

if try_start 18801; then
  record_fail "A01 不给 --allow-public-bind 时必须拒绝 0.0.0.0" "竟然起来了"
else
  record_pass "A01 不给 --allow-public-bind -> 拒绝监听 0.0.0.0（既有规则不变）"
fi
if try_start 18802 --allow-public-bind "phase7 自测"; then
  record_fail "A02 给了开关但没给证书时必须拒绝" "竟然起来了"
else
  record_pass "A02 有开关但没证书 -> 拒绝（公网监听必须有签名身份）"
fi
if try_start 18803 --allow-public-bind "phase7 自测" --bpsec2-cert-file "$WORK_DIR/server.bpcert" --require-bpsec2; then
  record_pass "A03 开关 + 证书 -> 允许监听 0.0.0.0"
else
  record_fail "A03 开关 + 证书 -> 允许监听 0.0.0.0" "$(tail -2 "$WORK_DIR/start-18803.log" | tr '\n' ' ')"
fi

echo "[phase7] B. 对 ECS 执行（新回滚点 + 公网绑定重启）"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
# payload 必须先更新：ECS 上那份可能是上一次部署时上传的旧版本，而
# "绑定地址可参数化"恰好是这一步需要的（第一版就是拿着旧 payload 反复
# deploy-public，每次都仍然绑回环）。
scp -q scripts/pr23_ecs_phase3_payload.sh "$ECS:$SERVER_ROOT/deploy/incoming/pr23_ecs_phase3_payload.sh" \
  || { record_fail "上传最新 payload" "scp 失败"; exit 1; }
if ! ssh "$ECS" "bash $SERVER_ROOT/deploy/incoming/pr23_ecs_phase3_payload.sh backup $SERVER_ROOT $STAMP"; then
  record_fail "取新的回滚点" "backup 阶段失败"
  echo "[phase7] passed=$PASS failed=$FAIL"
  exit 1
fi
record_pass "在 ECS 上取了新的回滚点 pr23-before-$STAMP"

# 目标机上的二进制必须**先**带上 --allow-public-bind 才会认这个参数：
# 第一版直接 deploy-public，结果服务端打了一屏用法就退了（unknown option），
# 脚本却因为 nohup 只看得到"启动命令发出去了"。所以这里先重建。
echo "[phase7] 在目标机上重建（把 --allow-public-bind 带上去）"
if ! bash scripts/deploy_aliyun_server.sh "$ECS" backup-project-server > /tmp/pr23/phase7-build.log 2>&1; then
  record_fail "目标机重建" "$(tail -5 /tmp/pr23/phase7-build.log | tr '
' ' ')"
  ssh "$ECS" "bash $SERVER_ROOT/deploy/incoming/pr23_ecs_phase3_payload.sh rollback $SERVER_ROOT $STAMP" || true
  exit 1
fi
record_pass "在 ECS 上重建出带 --allow-public-bind 的服务端"

PUBLIC_REASON="PR23-Phase7-official-cloud-direct: no tunnel, identity by BPSEC2 certificate"
# 切到公网：走 payload 的 deploy-public —— 停机等待、启动、报错处理都在那一个
# 文件里，脚本外不再自己拼 kill/start（第一版就是在外面手搓重启，结果旧进程
# 没退干净、新进程起不来，看起来像"公网起不来"）。
if ssh "$ECS" "bash $SERVER_ROOT/deploy/incoming/pr23_ecs_phase3_payload.sh deploy-public $SERVER_ROOT $STAMP '$PUBLIC_REASON'"      > "$WORK_DIR/deploy-public.txt" 2>&1; then
  sleep 2
  # 必须**精确**匹配 0.0.0.0:18765。第一版这里只 grep 了 "18765"，于是
  # 上一轮遗留的 127.0.0.1 监听也让它"通过"了 —— 一个只验证"有东西在听"
  # 的断言，会把"根本没切过去"报成成功。
  if ssh "$ECS" "ss -ltn | grep -q '0.0.0.0:18765'"; then
    record_pass "B01 ECS 服务端已在 0.0.0.0:18765 监听"
  else
    record_fail "B01 ECS 服务端公网监听" "$(ssh "$ECS" "ss -ltn | grep 18765 || echo '没有任何 18765 监听'" 2>&1 | tr '\n' ' ')"
  fi
else
  record_fail "B01 ECS 服务端公网监听" "$(ssh "$ECS" "tail -3 $SERVER_ROOT/logs/server.log" 2>&1 | tr '\n' ' ')"
fi

if ssh "$ECS" "grep -q 'listening on a PUBLIC address' $SERVER_ROOT/logs/server.log"; then
  record_pass "B02 启动日志留痕（含理由）"
else
  record_fail "B02 启动日志留痕" "没有找到 PUBLIC address 记录"
fi

echo "[phase7] C. 外部探测（这才是 Phase 7 的判据）"
ECS_HOST="$(ssh -G "$ECS" 2>/dev/null | awk '/^hostname /{print $2}')"
if timeout 8 bash -c "cat < /dev/null > /dev/tcp/$ECS_HOST/18765" 2>/dev/null; then
  record_pass "C01 公网 18765 真的可达（Phase 7 完成）"
  echo "[phase7] passed=$PASS failed=$FAIL"
  exit 0
fi
record_fail "C01 公网 18765 可达" "TCP 连不上"
{
  echo "SECURITY_GROUP_AUTOMATION = UNAVAILABLE"
  echo "  服务端已经在 0.0.0.0:18765 监听（B01 通过），但从外部连不上 ——"
  echo "  说明阿里云安全组入方向还没有放行 18765/tcp。"
  echo "  本项目不允许在仓库/制品里放 AccessKey，所以这一步必须人工做："
  echo "    阿里云控制台 -> ECS -> 安全组 -> 入方向 -> 添加 18765/tcp（源 0.0.0.0/0）"
  echo "  做完之后重跑本脚本即可判定。"
} | tee /tmp/pr23/phase7-security-group.txt
echo "[phase7] passed=$PASS failed=$FAIL"
exit 1
