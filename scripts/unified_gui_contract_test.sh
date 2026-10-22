#!/usr/bin/env bash
#
# unified_gui_contract_test.sh —— 新信息架构的契约回归。
#
# 它替代旧 modern_gui_check.sh 中“七页 / 三卡 / 设置页”那组静态断言，
# 验证本轮实际交付的结构：五页语义导航、四入口首页、统一备份管理、
# 三页共享加密/仓库配置、统一错误对话框、Widgets 退出产品构建。
#
# 退出码：0 = 全部通过。

set -uo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR" || exit 1
PASS=0
FAIL=0
ok() { PASS=$((PASS + 1)); echo "  PASS  $1"; }
bad() { FAIL=$((FAIL + 1)); echo "  FAIL  $1 -- $2"; }
finish() { echo "[unified-gui] passed=$PASS failed=$FAIL"; [ "$FAIL" -eq 0 ] || exit 1; }

check() {
  local name="$1" pattern="$2" file="$3"
  if grep -qF -- "$pattern" "$file"; then ok "$name"; else bad "$name" "missing in $file"; fi
}
reject() {
  local name="$1" pattern="$2" file="$3"
  if grep -qF -- "$pattern" "$file"; then bad "$name" "unexpected in $file"; else ok "$name"; fi
}

MAIN="ui/modern/qml/Main.qml"
HOME="ui/modern/qml/pages/HomePage.qml"
MGMT="ui/modern/qml/pages/BackupManagementPage.qml"
QUICK="ui/modern/qml/pages/BackupPage.qml"
SCHED="ui/modern/qml/pages/SchedulePage.qml"
REAL="ui/modern/qml/pages/RealtimePage.qml"
QRC="ui/modern/resources.qrc"
MAKE="Makefile"

check "五页语义 key" 'pageKeys: ["home", "quick", "scheduled"' "$MAIN"
for key in quick scheduled realtime management; do
  if grep -qF -- "\"key\": \"$key\"" "$HOME"; then ok "首页入口 $key"; else bad "首页入口 $key" "missing"; fi
done
[ "$(grep -c 'homeAction' "$HOME")" -ge 1 ] && ok "首页入口按钮模板" || bad "首页入口按钮模板" "missing"
check "首页响应式列数" "readonly property int columns" "$HOME"
check "备份管理本地/云端标签" '"key": "cloud", "text": "云端仓库"' "$MGMT"
check "备份管理本地仓库设置" 'localRepositorySettings' "$MGMT"
check "本地记录可复制到云端" 'copyLocalArchiveToCloud' "$MGMT"
check "云端可下载到本地仓库" 'copySnapshotToLocalRepository' "$(dirname "$MGMT")/RemotePage.qml"
check "快速备份共享选项面板" 'BackupOptionsPanel' "$QUICK"
check "定时备份统一加密字段" 'backupOptions.validate' "$SCHED"
check "实时备份统一加密字段" 'backupOptions.validate' "$REAL"
check "定时备份口令文件" 'scheduleEncryptionCombo' "$SCHED"
check "实时备份口令文件" 'realtimeEncryptionCombo' "$REAL"
check "统一错误对话框组件" 'UserErrorDialog.qml' "$QRC"
reject "SettingsPage 已从资源清单移除" 'SettingsPage.qml' "$QRC"
check "Widgets 不再进入 gui-all" 'gui-all: gui-modern' "$MAKE"
reject "Widgets 源码不再参与产品构建" 'ui/desktop/*.cpp' "$MAKE"
check "QML 错误对话框已接入主窗口" 'userErrorDialog' "$MAIN"

TMP=$(mktemp -d /tmp/unified-gui-XXXXXX)
QT_QPA_PLATFORM=offscreen timeout 90 ./build/backup-gui-modern --smoke-test \
  --config-file "$TMP/config.json" --schedule-file "$TMP/schedule.json" \
  --realtime-file "$TMP/realtime.json" >"$TMP/smoke.log" 2>&1
if [ $? -eq 0 ]; then ok "五页 smoke（offscreen）"; else bad "五页 smoke（offscreen）" "$(tail -2 "$TMP/smoke.log")"; fi
rm -rf "$TMP"
finish
