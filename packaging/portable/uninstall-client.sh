#!/usr/bin/env bash
# uninstall-client.sh —— 只删客户端程序文件。
# 用户数据（~/.config、备份、缓存）一律不动。
set -Eeuo pipefail
PREFIX="${HOME}/.local"
while [ $# -gt 0 ]; do
  case "$1" in
    --prefix) PREFIX="$2"; shift 2 ;;
    -h|--help) sed -n '2,4p' "$0"; exit 0 ;;
    *) echo "未知参数：$1" >&2; exit 2 ;;
  esac
done
case "$PREFIX" in ""|/) echo "[uninstall] 拒绝无效安装路径" >&2; exit 1 ;; esac
[[ ! "$PREFIX" =~ [[:cntrl:]] ]] || { echo "[uninstall] 路径不能包含控制字符" >&2; exit 1; }
PREFIX="$(realpath -m -- "$PREFIX")"
[ "$PREFIX" != / ] || exit 1
LIB_ROOT="$PREFIX/lib/backup-project"
DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"
APP_ID="backup-project-$(printf '%s' "$LIB_ROOT" | sha256sum | cut -c1-16)"
DESKTOP_FILE="$DATA_HOME/applications/$APP_ID.desktop"
ICON_FILE="$DATA_HOME/icons/hicolor/256x256/apps/$APP_ID.png"
ICON_SOURCE="$LIB_ROOT/share/icons/hicolor/256x256/apps/backup-project.png"
PREFIX_VALUE="${LIB_ROOT//\\/\\\\}"
if [ -f "$DESKTOP_FILE" ] && [ ! -L "$DESKTOP_FILE" ] &&
    grep -Fxq "X-Backup-Project-Prefix=$PREFIX_VALUE" "$DESKTOP_FILE"; then
  rm -f "$DESKTOP_FILE"
fi
# Only remove the icon if it still matches this installation's own resource.
if [ -f "$ICON_SOURCE" ] && [ -f "$ICON_FILE" ] && [ ! -L "$ICON_FILE" ] &&
    cmp -s "$ICON_SOURCE" "$ICON_FILE"; then
  rm -f "$ICON_FILE"
fi
for tool in backupctl backup-gui-modern backup-project; do
  entry="$PREFIX/bin/$tool"
  if [ -L "$entry" ] && [ "$(readlink "$entry")" = "$LIB_ROOT/bin/$tool" ]; then
    rm -f "$entry"
  fi
done
rm -rf "$LIB_ROOT" "$PREFIX/share/backup-project"
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$DATA_HOME/applications" >/dev/null 2>&1 || true
command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -f -t "$DATA_HOME/icons/hicolor" >/dev/null 2>&1 || true
echo "[uninstall] 已删除当前安装的程序与桌面入口；用户数据（备份 / 配置 / 缓存）未改动。"
