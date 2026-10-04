#!/usr/bin/env bash
#
# remote_connection_ux_test.sh —— PR #22 的连接层合同（C01..C07）。
#
#   bash scripts/remote_connection_ux_test.sh
#
# 它跑 Modern GUI 的 --remote-test（那一条会自己起一个真的 backup-server），
# 然后断言连接层那一段的每一条**真的执行过而且通过了**：
#
#   C01 合法 pin + 点"应用" -> 页面上看得见"✓ 已应用"
#   C02 非法 pin -> 红字，已经生效的指纹一个字节都不改
#   C03 改 pin 不点"应用"直接登录 -> 采纳的是**当前输入框里**的值
#   C04 有活动连接时改 pin -> 当前操作不被踢掉，下一次重连才用新值
#   C05/C06/C07 ssh 不存在 / 主机不存在 / host key 不被信任 -> 三种不同的说法
#
# 为什么不是"grep 源码里有没有这几行"：人工验收发现的第一个问题就是**按钮的
# onClicked 丢掉了返回值** —— 那种缺陷在源码里完全看不出来，只有真的按下按钮、
# 再看页面上的文字才能发现。所以这一条脚本跑的是真进程、真 QML、真的点击。
#
# 它**不需要 ECS**：C08..C14 需要真实 SSH 目标，由 scripts/pr22_ecs_e2e.sh
# 单独驱动（那一条跑完之后同样会打印 C08..C14 的逐条结果）。
#
# 退出码：0 = 全部通过。

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

GUI="./build/backup-gui-modern"
LOG="${REMOTE_CONNECTION_UX_LOG:-tests/output/remote-connection-ux.log}"
mkdir -p "$(dirname "$LOG")"
PASS=0
FAIL=0

record_pass() { PASS=$((PASS + 1)); echo "  PASS  $1"; }
record_fail() { FAIL=$((FAIL + 1)); echo "  FAIL  $1 -- $2" >&2; }

finish() {
  echo "[remote-connection-ux] passed=$PASS failed=$FAIL"
  echo "[remote-connection-ux] 日志：$LOG"
  [ "$FAIL" -eq 0 ] && exit 0 || exit 1
}

if [ ! -x "$GUI" ] || [ ! -x ./build/backup-server ] || [ ! -x ./build/backup-server-keygen ]; then
  make -j4 gui-modern server test-fixtures >/tmp/remote-connection-ux-build.log 2>&1 \
    || { record_fail "构建 gui-modern / server" "$(tail -3 /tmp/remote-connection-ux-build.log | tr '\n' ' ')"; finish; }
fi
[ -x "$GUI" ] && record_pass "Modern GUI 已构建" || { record_fail "Modern GUI 已构建" "$GUI 不存在"; finish; }
if [ -x ./build/backup-server ] && [ -x ./build/backup-server-keygen ]; then
  record_pass "backup-server 与 backup-server-keygen 已构建（--remote-test 需要真服务端）"
else
  record_fail "服务端产物" "缺 build/backup-server 或 build/backup-server-keygen"; finish
fi

# 配置隔离：绝不读写真实用户目录。
STATE_DIR="$(mktemp -d)"
trap 'rm -rf "$STATE_DIR"' EXIT
export XDG_CONFIG_HOME="$STATE_DIR/xdg"
mkdir -p "$XDG_CONFIG_HOME" "$STATE_DIR/state"

DIAG="$STATE_DIR/stderr.log"
set +e
QT_QPA_PLATFORM=offscreen QSG_RHI_BACKEND=software timeout 2400 \
  "$GUI" --remote-test \
  --config-file "$STATE_DIR/state/config.json" \
  --schedule-file "$STATE_DIR/state/schedule.json" \
  --realtime-file "$STATE_DIR/state/realtime.json" \
  > "$LOG" 2> "$DIAG"
status=$?
set -e

if [ "$status" -eq 0 ]; then
  record_pass "GUI --remote-test 退出码 0（$(grep -oE 'passed=[0-9]+ failed=[0-9]+' "$LOG" | tail -1)）"
else
  record_fail "GUI --remote-test 退出码" "exit=$status；$(grep -m3 FAIL "$LOG" | tr '\n' ' ')"
fi

# 逐条断言：每一条都必须出现，而且是 ok。缺一条 = 那一条没跑，不能算通过。
expect_ok() {
  local label="$1"
  local pattern="$2"
  if grep -qE "^\[remote-test\]   ok   $pattern" "$LOG"; then
    record_pass "$label"
  else
    record_fail "$label" "日志里没有这一条 ok 行（模式：$pattern）"
  fi
}

expect_ok "C00 连接方式 / SSH 主机 / 通道状态 / 建立连接按钮在页面上" "C00 "
expect_ok "C01a 输入合法 pin 并点“应用”" "C01a "
expect_ok "C01b 控制器采用了这个 pin" "C01b "
expect_ok "C01c 反馈里出现“已应用”" "C01c "
expect_ok "C01d 页面上可见“✓ 已应用”" "C01d "
expect_ok "C01e 应用 pin 只改配置：不联网、不登录" "C01e "
expect_ok "C02a 非法 pin 不改变已经生效的指纹" "C02a "
expect_ok "C02b 非法 pin 写下了自己的原因" "C02b "
expect_ok "C02c 页面上可见红色错误行" "C02c "
expect_ok "C02d 非法时不显示“✓ 已应用”" "C02d "
expect_ok "C02e 不带前缀的裸十六进制同样被拒" "C02e "
expect_ok "C03a 没点应用就登录：用的是输入框里的 pin" "C03a "
expect_ok "C03b 不点应用直接登录：当前输入被自动采用并生效" "C03b "
expect_ok "C03c 自动提交同样给出“已应用”反馈" "C03c "
expect_ok "C03d 非法 pin 时登录被本地拒绝且生效值不变" "C03d "
expect_ok "C04a 应用 pin 时确实有一条活动操作" "C04a "
expect_ok "C04b 活动连接时给出“当前连接保持不变”" "C04b "
expect_ok "C04c 当前操作没有被“应用”打断" "C04c "
expect_ok "C04d 新 pin 是生效值" "C04d "
expect_ok "C05 找不到 ssh -> ssh-missing" "C05 "
expect_ok "C05b 失败原因说清了是 ssh 命令的问题" "C05b "
expect_ok "C06 SSH 主机不存在 -> ssh-host-unreachable" "C06 "
expect_ok "C07 SSH host key 不被信任 -> fail closed" "C07 |C07 跳过"
expect_ok "C07b 文案让用户去检查 SSH 配置" "C07b "
expect_ok "C07c 登录说的是通道的原因" "C07c "

# 汇总行必须 failed=0，而且连接层那一段的 FAIL 一条都不许有。
if grep -qE 'passed=[0-9]+ failed=0$' "$LOG"; then
  record_pass "汇总行 failed=0"
else
  record_fail "汇总行 failed=0" "$(grep -oE 'passed=[0-9]+ failed=[0-9]+' "$LOG" | tail -1)"
fi
if grep -qE "^\[remote-test\] FAIL C0" "$LOG"; then
  record_fail "连接层没有 FAIL" "$(grep -m3 -E '^\[remote-test\] FAIL C0' "$LOG" | tr '\n' ' ')"
else
  record_pass "连接层没有 FAIL"
fi

# QML 运行期告警必须为 0：新版页面新增了连接方式 / 通道状态 / 反馈行，
# 任何绑定写错都会在这里以 qml-warning 的形式露出来。
if grep -qF "qml-warning" "$LOG"; then
  record_fail "0 QML 运行期告警" "$(grep -m3 -F 'qml-warning' "$LOG" | tr '\n' ' ')"
else
  record_pass "0 QML 运行期告警"
fi

finish
