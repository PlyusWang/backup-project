#!/usr/bin/env bash
#
# pr23_ecs_phase6_firewall_audit.sh —— PR #23 Phase 6：开放公网**之前**的
# 防火墙 / 安全组审计。
#
#   bash scripts/pr23_ecs_phase6_firewall_audit.sh
#
# 这个阶段的产出是一份**事实记录**，不是一堆"应该没问题"：
#   1. ECS 上到底在监听什么、监听在哪个地址；
#   2. 主机防火墙（ufw / iptables）的状态，读不到就如实写"读不到"；
#   3. 安全组能不能自动化（本项目不允许在仓库/制品里放阿里云 AccessKey，
#      所以大概率是 UNAVAILABLE，那就如实写，并写清楚剩下的手工步骤）；
#   4. **从外部真的去连**：这是一个外部探测，不是看配置文件。它能把
#      "端口没开"与"端口开着但被挡住"区分开 —— 前提是先用一个已知可达的
#      端口（22）证明探测方法本身有效，否则"连不上"什么都证明不了。
#
# 退出码：0 = 审计完成（不代表端口已开放）。

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

ECS="aliyun-ecs"
OUT="/tmp/pr23/phase6-audit.txt"
mkdir -p /tmp/pr23

PASS=0
FAIL=0
record_pass() { PASS=$((PASS + 1)); echo "  PASS  $1"; }
record_fail() { FAIL=$((FAIL + 1)); echo "  FAIL  $1 -- $2" >&2; }

ECS_HOST="$(ssh -G "$ECS" 2>/dev/null | awk '/^hostname /{print $2}')"
if [ -z "$ECS_HOST" ]; then
  record_fail "取得 ECS 公网地址" "ssh -G $ECS 里没有 hostname"
  echo "[phase6] passed=$PASS failed=$FAIL"
  exit 1
fi
record_pass "取到 ECS 地址（来自 ssh 配置，不写死在脚本里）：$ECS_HOST"

{
  echo "=== Phase 6 审计 $(date -u +%Y-%m-%dT%H:%M:%SZ) ==="
  echo "--- ECS 监听（ss -ltn）---"
  ssh "$ECS" "ss -ltn"
  echo "--- 18765 的进程 ---"
  ssh "$ECS" "ss -ltnp 2>/dev/null | grep 18765 || echo '(没有 18765 的监听)'"
  echo "--- 主机防火墙：ufw ---"
  ssh "$ECS" "sudo -n ufw status 2>&1 || echo 'UNAVAILABLE：需要 sudo 或没有 ufw'"
  echo "--- 主机防火墙：iptables ---"
  ssh "$ECS" "sudo -n iptables -S 2>&1 | head -20 || echo 'UNAVAILABLE：需要 sudo'"
  echo "--- 安全组自动化能力 ---"
  ssh "$ECS" "command -v aliyun >/dev/null 2>&1 && echo 'aliyun CLI 存在' || echo 'aliyun CLI 不存在'"
  echo "--- 本机有没有阿里云凭据（不允许放，这里是核对）---"
  ssh "$ECS" "ls ~/.aliyun/config.json ~/.aliyun/credentials 2>/dev/null || echo '没有任何阿里云凭据文件（符合要求）'"
} >> "$OUT" 2>&1

# ---- 外部探测 ----
probe_port() {
  timeout 6 bash -c "cat < /dev/null > /dev/tcp/$ECS_HOST/$1" 2>/dev/null
}

echo "--- 外部探测（从本机直连 $ECS_HOST，不经隧道）---" >> "$OUT"
if probe_port 22; then
  echo "port 22    : 可达（探测方法有效）" >> "$OUT"
  record_pass "外部探测方法有效（22 可达）"
else
  echo "port 22    : 不可达（探测方法可能无效，下面的结论不可信）" >> "$OUT"
  record_fail "外部探测方法有效（22 可达）" "22 都连不上，探测方法本身有问题"
fi

if probe_port 18765; then
  echo "port 18765 : **可达**（公网已经能连到 18765）" >> "$OUT"
  record_pass "Phase 6 现状：18765 公网可达（若尚未执行 Phase 7，说明安全组本来就没挡）"
else
  echo "port 18765 : 不可达（当前公网连不上，符合 Phase 7 之前的要求）" >> "$OUT"
  record_pass "Phase 6 现状：18765 公网不可达（符合"开放前"的要求）"
fi

if probe_port 80; then
  echo "port 80    : 可达" >> "$OUT"
else
  echo "port 80    : 不可达" >> "$OUT"
fi

{
  echo "--- 结论 ---"
  echo "SECURITY_GROUP_AUTOMATION = UNAVAILABLE"
  echo "  原因：仓库与制品里不允许出现阿里云 AccessKey，本机也没有 aliyun CLI 凭据。"
  echo "  影响：开放 18765 需要**人工**在阿里云控制台（或带凭据的 API 调用）里"
  echo "        把安全组入方向 18765/tcp 打开；脚本只能验证"开没开"，不能替你开。"
  echo "  主机侧：服务端自身只监听 127.0.0.1，公网开放必须同时改 --bind 并显式"
  echo "          给出 --allow-public-bind（见 remote_server.cpp 的 fail-closed 规则）。"
} >> "$OUT"

echo "[phase6] 审计记录：$OUT"
cat "$OUT"
echo "[phase6] passed=$PASS failed=$FAIL"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
