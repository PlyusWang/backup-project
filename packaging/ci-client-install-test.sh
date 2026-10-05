#!/usr/bin/env bash
# packaging/ci-client-install-test.sh —— 客户端三个制品的干净机器安装验收。
#
#   bash packaging/ci-client-install-test.sh <release 目录>
#
# 覆盖：.deb 安装 / CLI / GUI（无 Qt 开发包）/ 桌面入口 / 卸载不动用户数据 /
#       portable tar.xz 安装与卸载 / 重复安装（同版本再装一次）。
set -Eeuo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/ci-lib.sh"

REL="${1:?用法: ci-client-install-test.sh <release 目录>}"
DEB="$(ls "$REL"/backup-project-client_*.deb)"
TARBALL="$(ls "$REL"/backup-project-client-*-linux-x86_64.tar.xz)"
export QT_QPA_PLATFORM=offscreen

# 用户数据哨兵：安装/升级/卸载都不许碰它。
USER_HOME=/root
mkdir -p "$USER_HOME/.config/backup-project" "$USER_HOME/.local/share/backup-project"
echo "user-config-sentinel" > "$USER_HOME/.config/backup-project/sentinel"
echo "user-data-sentinel" > "$USER_HOME/.local/share/backup-project/sentinel"

ci_section "1. 干净机器：确认没有 Qt 开发包"
expect_eq "没有 qt6-*-dev" "0" "$(dpkg -l 2>/dev/null | awk '/^ii/ && /qt6-.*dev/ {print $2}' | wc -l)"

ci_section "2. .deb 安装"
expect_ok "dpkg -i $(basename "$DEB")" dpkg -i "$DEB"
expect_file "/usr/bin/backupctl" /usr/bin/backupctl
expect_file "/usr/bin/backup-project" /usr/bin/backup-project
expect_file "/usr/share/applications/backup-project.desktop" /usr/share/applications/backup-project.desktop
expect_file "自带 Qt（/usr/lib/backup-project-client/lib/libQt6Core.so.6）" /usr/lib/backup-project-client/lib/libQt6Core.so.6
expect_file "自带 QML 模块" /usr/lib/backup-project-client/qml/QtQuick
dpkg-deb --info "$DEB" > /tmp/client-deb-info.txt
expect_contains "control 里有 Depends" /tmp/client-deb-info.txt '^ Depends:'
expect_contains "desktop 文件没有假图标（不含 Icon=）" /usr/share/applications/backup-project.desktop '^Exec=/usr/bin/backup-project'
if grep -q '^Icon=' /usr/share/applications/backup-project.desktop; then ci_fail "desktop 文件出现了 Icon=（仓库没有正式图标资源）"; else ci_pass "desktop 文件没有伪造 Icon="; fi

ci_section "3. 安装后真的能跑（机器上没有 Qt 开发包）"
expect_ok "backupctl --help" backupctl --help
set +e
timeout 20 backup-project > /tmp/client-gui.log 2>&1
code=$?
set -e
if [ "$code" = "0" ] || [ "$code" = "124" ]; then ci_pass "GUI 启动正常（退出码 $code）"; else ci_fail "GUI 退出码 $code"; tail -20 /tmp/client-gui.log >&2; fi
expect_eq "GUI 运行期 QML 告警 = 0" "0" "$(grep -cE '\.qml:[0-9]+:|is not installed' /tmp/client-gui.log || true)"
expect_ok "非 root 用户也能跑 backupctl" runuser -u nobody -- /usr/bin/backupctl --help

ci_section "4. 同版本重复安装（幂等）"
expect_ok "dpkg -i 再装一次" dpkg -i "$DEB"
expect_eq "用户配置哨兵还在" "user-config-sentinel" "$(cat "$USER_HOME/.config/backup-project/sentinel")"

ci_section "5. 卸载：只删程序文件"
expect_ok "dpkg -r backup-project-client" dpkg -r backup-project-client
if [ -e /usr/bin/backup-project ]; then ci_fail "/usr/bin/backup-project 仍在"; else ci_pass "程序文件已删除"; fi
expect_eq "用户配置没被动过" "user-config-sentinel" "$(cat "$USER_HOME/.config/backup-project/sentinel")"
expect_eq "用户数据没被动过" "user-data-sentinel" "$(cat "$USER_HOME/.local/share/backup-project/sentinel")"

ci_section "6. 重新安装"
expect_ok "卸载后再装一次" dpkg -i "$DEB"
expect_ok "重装后 backupctl --help" backupctl --help

ci_section "7. portable tar.xz（不需要 root，装到含空格的路径）"
PREFIX="/tmp/client prefix/opt"
rm -rf "/tmp/client prefix"
mkdir -p "/tmp/client prefix"
expect_ok "解包 tar.xz" tar -xf "$TARBALL" -C "/tmp/client prefix"
TOP="$(ls -d "/tmp/client prefix"/*/ | head -1)"
expect_file "install.sh" "${TOP}install.sh"
expect_ok "install.sh --prefix（路径含空格）" "${TOP}install.sh" --prefix "$PREFIX" --no-desktop
expect_file "安装出的 backupctl" "$PREFIX/bin/backupctl"
expect_ok "portable backupctl --help" "$PREFIX/bin/backupctl" --help
set +e
timeout 20 "$PREFIX/bin/backup-gui-modern" > /tmp/client-portable-gui.log 2>&1
code=$?
set -e
if [ "$code" = "0" ] || [ "$code" = "124" ]; then ci_pass "portable GUI 启动正常（退出码 $code）"; else ci_fail "portable GUI 退出码 $code"; tail -20 /tmp/client-portable-gui.log >&2; fi
expect_eq "portable GUI QML 告警 = 0" "0" "$(grep -cE '\.qml:[0-9]+:|is not installed' /tmp/client-portable-gui.log || true)"

ci_section "8. portable 卸载"
expect_ok "uninstall.sh --prefix" "${TOP}uninstall.sh" --prefix "$PREFIX"
if [ -e "$PREFIX/bin/backupctl" ]; then ci_fail "portable 程序文件仍在"; else ci_pass "portable 程序文件已删除"; fi
expect_eq "卸载后用户数据仍在" "user-data-sentinel" "$(cat "$USER_HOME/.local/share/backup-project/sentinel")"

ci_finish "client-install"
