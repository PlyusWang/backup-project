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
rm -f "$PREFIX/bin/backupctl" "$PREFIX/bin/backup-gui-modern" "$PREFIX/bin/backup-project"
rm -rf "$PREFIX/lib/backup-project" "$PREFIX/share/backup-project"
rm -f "${XDG_DATA_HOME:-$HOME/.local/share}/applications/backup-project.desktop"
echo "[uninstall] 已删除程序文件；用户数据（备份 / 配置 / 缓存）未改动。"
