#!/usr/bin/env bash
# packaging/ci-appimage-test.sh —— AppImage 必须在**没有 Qt 开发包**的干净机器上跑起来。
#
#   bash packaging/ci-appimage-test.sh <release 目录>
#
# 覆盖：CLI 入口 / offscreen 启动 / X11(xcb) 真渲染 / 整页截图模式 /
#       QML 运行期告警 = 0 / 不含开发机路径。
set -Eeuo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/ci-lib.sh"

REL="${1:?用法: ci-appimage-test.sh <release 目录>}"
REL="$(cd "$REL" && pwd)"   # 后面会 cd 到别处，先用绝对路径钉住
APPIMAGE="$(ls "$REL"/Backup-Project-Client-*-x86_64.AppImage)"
# upload-artifact / download-artifact 不保留可执行位，所以这里补一次；真实用户
# 拿到的 AppImage 也是 "chmod +x 之后再运行"，与 docs/install-client.md 一致。
chmod 0755 "$APPIMAGE"
export APPIMAGE_EXTRACT_AND_RUN=1     # CI 里通常没有 FUSE，用解包运行同一条路径
export NO_AT_BRIDGE=1

ci_section "环境：确认这台机器没有 Qt 开发包"
qt_dev="$(dpkg -l 2>/dev/null | awk '/^ii/ && /qt6-.*dev/ {print $2}' | wc -l)" 
expect_eq "机器上没有 qt6-*-dev（AppImage 不该依赖它们）" "0" "$qt_dev"
qt_any="$(dpkg -l 2>/dev/null | awk '/^ii/ && /(libqt6|qml6-module)/ {print $2}' | wc -l)"
expect_eq "机器上没有任何 Qt 运行时包（AppImage 自带 Qt）" "0" "$qt_any"
expect_file "AppImage 存在：$(basename "$APPIMAGE")" "$APPIMAGE"

ci_section "1. CLI 入口（AppImage 里的 backupctl）"
expect_ok "backupctl --help" "$APPIMAGE" backupctl --help

ci_section "2. offscreen 启动（无显示环境）"
set +e
QT_QPA_PLATFORM=offscreen timeout 15 "$APPIMAGE" > /tmp/appimage-offscreen.log 2>&1
code=$?
set -e
if [ "$code" = "0" ] || [ "$code" = "124" ]; then ci_pass "GUI 在 offscreen 下正常（退出码 $code）";
else ci_fail "GUI offscreen 退出码 $code"; tail -20 /tmp/appimage-offscreen.log >&2; fi
qml_hits="$(grep -cE '\.qml:[0-9]+:|is not installed|Type .* unavailable' /tmp/appimage-offscreen.log || true)"
expect_eq "offscreen 运行期 QML 告警 = 0" "0" "$qml_hits"

ci_section "3. X11 / xcb 真渲染（xvfb）"
set +e
xvfb-run -a --server-args="-screen 0 1280x1024x24" timeout 90 "$APPIMAGE" --screenshot /tmp/appimage-shots \
  > /tmp/appimage-xvfb.log 2>&1
code=$?
set -e
if [ "$code" = "0" ] || [ "$code" = "124" ]; then ci_pass "GUI 在 X11(xvfb) 下正常（退出码 $code）";
else ci_fail "GUI 在 X11 下退出码 $code"; tail -30 /tmp/appimage-xvfb.log >&2; fi
shots="$(ls /tmp/appimage-shots/*.png 2>/dev/null | wc -l)"
if [ "$shots" -ge 8 ]; then ci_pass "整页截图模式产出 $shots 张 PNG（登录页 / 官方云端等页面真的渲染了）";
else ci_fail "截图只有 $shots 张"; tail -20 /tmp/appimage-xvfb.log >&2; fi
qml_hits2="$(grep -cE '\.qml:[0-9]+:|is not installed|Type .* unavailable' /tmp/appimage-xvfb.log || true)"
expect_eq "X11 运行期 QML 告警 = 0" "0" "$qml_hits2"

ci_section "4. 平台插件与自带 Qt"
rm -rf /tmp/appimage-extract
mkdir -p /tmp/appimage-extract
( cd /tmp/appimage-extract && "$APPIMAGE" --appimage-extract > /dev/null 2>&1 )
expect_file "libqxcb.so（X11 平台插件）" "/tmp/appimage-extract/squashfs-root/usr/plugins/platforms/libqxcb.so"
expect_file "自带 libQt6Core.so.6" "/tmp/appimage-extract/squashfs-root/usr/lib/libQt6Core.so.6"
expect_file "自带 QML 模块 QtQuick" "/tmp/appimage-extract/squashfs-root/usr/qml/QtQuick"
expect_file "官方根公钥（公开材料）" "/tmp/appimage-extract/squashfs-root/usr/share/backup-project/official-root-ed25519.pub"

ci_section "5. 二进制里不许出现开发机路径"
dev_hits="$(grep -rIl '/home/pw-is-123\|/tmp/stage-\|/workspace/backup-project/build' /tmp/appimage-extract/squashfs-root/usr/bin 2>/dev/null | wc -l)"
expect_eq "二进制内没有开发机路径" "0" "$dev_hits"

ci_finish "appimage"