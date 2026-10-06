#!/usr/bin/env bash
# uninstall.sh —— 卸载便携安装。
#
#   sudo ./uninstall.sh --prefix /opt/backup-project-server [--purge-data]
#
# 默认**只删程序文件**：配置、secrets.env、传输身份私钥、数据库、备份数据全部
# 保留。真要删数据必须显式 --purge-data，并且手打确认短语。
set -Eeuo pipefail

PREFIX=""
PURGE_DATA=0
KEEP_USER=1

while [ $# -gt 0 ]; do
  case "$1" in
    --prefix) PREFIX="$2"; shift 2 ;;
    --purge-data) PURGE_DATA=1; shift ;;
    --remove-user) KEEP_USER=0; shift ;;
    -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
    *) echo "未知参数：$1" >&2; exit 2 ;;
  esac
done
[ -n "$PREFIX" ] || { echo "必须给 --prefix" >&2; exit 2; }
case "$PREFIX" in ""|/|/usr|/etc|/var|/bin|/lib|/sbin) echo "拒绝操作 \$PREFIX=$PREFIX" >&2; exit 2 ;; esac

log() { echo "[uninstall] $*"; }

if [ -f /etc/systemd/system/backup-project-server.service ] && [ -d /run/systemd/system ]; then
  systemctl stop backup-project-server.service 2>/dev/null || true
  systemctl disable backup-project-server.service >/dev/null 2>&1 || true
  rm -f /etc/systemd/system/backup-project-server.service
  systemctl daemon-reload || true
  log "已移除 systemd unit"
fi

rm -rf "$PREFIX/bin" "$PREFIX/share" "$PREFIX/docs"
log "已删除程序文件（$PREFIX/bin, share, docs）"

if [ "$PURGE_DATA" -eq 1 ]; then
  echo
  echo "这会永久删除：$PREFIX/var（备份数据、元数据库、传输身份私钥）与 $PREFIX/etc（配置与 token secret）。"
  printf '请输入 DELETE ALL BACKUP DATA 以确认：'
  read -r answer || answer=""
  [ "$answer" = "DELETE ALL BACKUP DATA" ] || { echo "确认文字不匹配：只删了程序文件。" >&2; exit 2; }
  rm -rf "$PREFIX/var" "$PREFIX/etc"
  log "已删除数据与配置"
  if [ "$KEEP_USER" -eq 0 ] && getent passwd backup-project >/dev/null 2>&1; then
    deluser --system backup-project >/dev/null 2>&1 || true
    log "已删除系统用户 backup-project"
  fi
else
  log "保留：$PREFIX/var（数据/数据库/私钥）与 $PREFIX/etc（配置/secret）"
  log "要删除它们：$0 --prefix $PREFIX --purge-data"
fi
