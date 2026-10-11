#!/usr/bin/env bash
#
# 只读核对 ECS 上 backup-project-server-ecs 的实际部署。
#
#   bash packaging/ecs/verify-ecs-deployment.sh [模板路径]
#
# 在 **ECS 主机上**运行（或在开发机上通过 ssh 把本目录带过去运行）。
# 只调用 systemctl is-enabled/is-active/show、ss、sha256sum、diff——
# **不做任何写入**：不安装、不重启、不改配置、不碰数据目录内容。
#
# 为什么要有它：unit 曾经只存在于主机和一份证据包里，任何人 clone 仓库都
# 无法复现或核对这次部署。这个脚本把"模板 vs 现场"的比较变成可执行的一步。
#
# 退出码：0 = 全部 PASS；非 0 = 有 FAIL。

set -uo pipefail

UNIT_NAME="backup-project-server-ecs"
UNIT_PATH="/etc/systemd/system/${UNIT_NAME}.service"
SERVICE_ROOT="/home/ubuntu/backup-project-server"
EXPECTED_PORT="18765"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)" || exit 1
TEMPLATE="${1:-${SCRIPT_DIR}/${UNIT_NAME}.service}"

PASS=0
FAIL=0
NOTE=0
ok()   { PASS=$((PASS + 1)); echo "  PASS  $1"; }
bad()  { FAIL=$((FAIL + 1)); echo "  FAIL  $1 -- $2"; }
note() { NOTE=$((NOTE + 1)); echo "  NOTE  $1"; }

echo "[ecs-verify] $(date '+%Y-%m-%d %H:%M:%S %Z')  unit=${UNIT_NAME}"

if [ -f "$UNIT_PATH" ]; then
  ok "unit 文件存在：${UNIT_PATH}"
else
  bad "unit 文件存在" "找不到 ${UNIT_PATH}"
  echo "[ecs-verify] PASS=${PASS} FAIL=${FAIL} NOTE=${NOTE}"
  exit 1
fi

enabled="$(systemctl is-enabled "$UNIT_NAME" 2>&1)"
if [ "$enabled" = "enabled" ]; then
  ok "开机自启已启用（is-enabled=enabled）"
else
  bad "开机自启已启用" "is-enabled=${enabled}"
fi

active="$(systemctl is-active "$UNIT_NAME" 2>&1)"
if [ "$active" = "active" ]; then
  ok "服务正在运行（is-active=active）"
else
  bad "服务正在运行" "is-active=${active}"
fi

main_pid="$(systemctl show "$UNIT_NAME" -p MainPID --value 2>/dev/null)"
restarts="$(systemctl show "$UNIT_NAME" -p NRestarts --value 2>/dev/null)"
echo "  NOTE  MainPID=${main_pid} NRestarts=${restarts} Restart=$(systemctl show "$UNIT_NAME" -p Restart --value 2>/dev/null)"
NOTE=$((NOTE + 1))

instances="$(pgrep -c -f 'bin/backup-server --bind' 2>/dev/null || true)"
if [ "${instances:-0}" = "1" ]; then
  ok "只有一个 backup-server 实例"
else
  bad "只有一个 backup-server 实例" "pgrep -c 得到 ${instances:-0}"
fi

nohup_left="$(pgrep -f 'nohup.*backup-server' 2>/dev/null | wc -l)"
if [ "${nohup_left:-1}" = "0" ]; then
  ok "没有遗留的 nohup 实例（迁移已收口）"
else
  bad "没有遗留的 nohup 实例" "还找到 ${nohup_left} 个"
fi

if systemctl show "$UNIT_NAME" -p ExecStart --value 2>/dev/null | grep -q '\-\-require-bpsec2'; then
  ok "ExecStart 带 --require-bpsec2（不接受 BPSEC1 降级）"
else
  bad "ExecStart 带 --require-bpsec2" "实际 ExecStart 里没有这个开关"
fi

if ss -ltn 2>/dev/null | grep -q ":${EXPECTED_PORT} "; then
  ok "端口 ${EXPECTED_PORT} 正在监听"
else
  bad "端口 ${EXPECTED_PORT} 正在监听" "ss 里没有它"
fi

BIN="${SERVICE_ROOT}/bin/backup-server"
CERT="${SERVICE_ROOT}/state/server-identity.bpcert"
KEY="${SERVICE_ROOT}/state/transport.key"
for pair in "二进制:${BIN}" "证书:${CERT}"; do
  label="${pair%%:*}"
  path="${pair#*:}"
  if [ -f "$path" ]; then
    echo "  NOTE  ${label} sha256=$(sha256sum "$path" | cut -d' ' -f1)"
    NOTE=$((NOTE + 1))
  else
    bad "${label}存在" "找不到 ${path}"
  fi
done
if [ -f "$KEY" ]; then
  mode="$(stat -c '%a' "$KEY" 2>/dev/null)"
  if [ "$mode" = "600" ]; then
    ok "传输身份私钥权限为 0600"
  else
    bad "传输身份私钥权限为 0600" "实际 ${mode}"
  fi
else
  bad "传输身份私钥存在" "找不到 ${KEY}"
fi

if [ -f "$TEMPLATE" ]; then
  # 归一化比较：去掉注释与空行，只比真正生效的指令。文档改动不算 drift。
  normalized_live="$(grep -vE '^[[:space:]]*(#|$)' "$UNIT_PATH" | sed 's/[[:space:]]*$//')"
  normalized_tpl="$(grep -vE '^[[:space:]]*(#|$)' "$TEMPLATE" | sed 's/[[:space:]]*$//')"
  if [ "$normalized_live" = "$normalized_tpl" ]; then
    ok "现场 unit 与仓库模板一致（忽略注释与空行）"
  else
    bad "现场 unit 与仓库模板一致" "归一化后仍有差异，见下方 diff"
    diff <(printf '%s\n' "$normalized_tpl") <(printf '%s\n' "$normalized_live") | head -20
  fi
else
  note "没有提供模板（${TEMPLATE}），跳过模板比对——这一项**没有**被验证"
fi

echo "[ecs-verify] PASS=${PASS} FAIL=${FAIL} NOTE=${NOTE}"
if [ "$FAIL" -ne 0 ]; then
  echo "[ecs-verify] 有 FAIL 项" >&2
  exit 1
fi
echo "[ecs-verify] 全部只读检查通过"
exit 0
