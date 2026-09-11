#!/usr/bin/env bash
#
# 桌面 GUI 冒烟测试：构建 backup-gui，然后在无显示环境下启动一次。
# 只验证“能构建、能起来、不崩”，界面行为本身要靠人工点。
#
# 退出码：0 表示构建与启动都正常。

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$BASH_SOURCE")/.." && pwd)"
cd "$ROOT_DIR"

# 缺 Qt 时直接失败并给出能照做的安装命令，
# 而不是让 make 抛出一屏找不到头文件的错误。
if ! pkg-config --exists Qt6Widgets Qt6Concurrent; then
  echo "[gui-smoke] 缺少 Qt 6 开发包。"
  echo "[gui-smoke] Ubuntu 上可执行: sudo apt-get install -y qt6-base-dev qt6-base-dev-tools"
  exit 1
fi

# 复用 Makefile 的 gui 目标，脚本不自己拼编译参数，
# 避免编译选项在两处各写一份、日后改一处漏一处。
echo "[gui-smoke] 构建 GUI..."
make gui

echo "[gui-smoke] offscreen 启动自检..."
LOG_FILE="$ROOT_DIR/tests/output/gui-smoke.log"
mkdir -p "$(dirname "$LOG_FILE")"
# offscreen 平台插件让无显示环境（纯 SSH、CI）也能真正构造出窗口；
# timeout 兜底，界面万一卡住也不会把测试挂死在这里。
set +e
QT_QPA_PLATFORM=offscreen timeout 30 ./build/backup-gui --smoke-test > "$LOG_FILE" 2>&1
status=$?
set -e

if [[ "$status" -ne 0 ]]; then
  echo "[gui-smoke] 启动失败（exit=$status），输出如下："
  cat "$LOG_FILE"
  exit 1
fi

# 有输出不等于失败（Qt 可能打印无害提示），但要显式打出来，
# 让人能自己判断是不是真的有问题。
if [[ -s "$LOG_FILE" ]]; then
  echo "[gui-smoke] 运行期有输出，需要人工确认是否只是无害提示："
  cat "$LOG_FILE"
fi

echo "[gui-smoke] PASS"
