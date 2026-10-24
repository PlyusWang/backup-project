#!/usr/bin/env bash
#
# 桌面 GUI 冒烟测试：构建 **Qt Quick/QML** 产品 GUI，然后在无显示环境下启动一次。
# 旧 Qt Widgets GUI 已停用：它不再有构建目标，也不属于正式产品。
#
# 退出码：0 表示构建与启动都正常。

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

if ! pkg-config --exists Qt6Quick Qt6QuickControls2 Qt6Concurrent Qt6Network; then
  echo "[gui-smoke] 缺少 Qt 6 Quick 开发包。"
  echo "[gui-smoke] Ubuntu 上可执行: sudo apt-get install -y qt6-declarative-dev qt6-base-dev"
  exit 1
fi

echo "[gui-smoke] 构建 QML GUI..."
make gui-modern

echo "[gui-smoke] offscreen 启动自检..."
LOG_FILE="$ROOT_DIR/tests/output/gui-smoke.log"
mkdir -p "$(dirname "$LOG_FILE")"
set +e
"$ROOT_DIR/scripts/isolated_gui_test.sh" "$ROOT_DIR/build/backup-gui-modern" --smoke-test >"$LOG_FILE" 2>&1
status=$?
set -e

if [[ "$status" -ne 0 ]]; then
  echo "[gui-smoke] 启动失败（exit=$status），输出如下："
  cat "$LOG_FILE"
  exit 1
fi

if [[ -s "$LOG_FILE" ]]; then
  echo "[gui-smoke] 运行期有输出："
  cat "$LOG_FILE"
fi

echo "[gui-smoke] PASS"
