#!/usr/bin/env bash
# purge-data.sh —— **显式**删除本机备份数据。
#
# 卸载包（哪怕 purge）**永远不会**调用它：数据是用户的，不是包的。只有管理员
# 在终端里手打确认短语才会真的删。默认保留传输身份私钥与证书（删掉它们等于
# 让所有客户端的 pin 失效），要删必须再加 --include-identity。
#
#   sudo backup-server-purge-data --dry-run
#   sudo backup-server-purge-data
#   sudo backup-server-purge-data --include-identity
set -Eeuo pipefail

SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_CONF="${BACKUP_SERVER_CONF:-/etc/backup-project-server/server.conf}"
CONF="$DEFAULT_CONF"
DRY_RUN=0
INCLUDE_IDENTITY=0
FORCE=0

while [ $# -gt 0 ]; do
  case "$1" in
    --config) CONF="$2"; shift 2 ;;
    --dry-run) DRY_RUN=1; shift ;;
    --include-identity) INCLUDE_IDENTITY=1; shift ;;
    --yes) FORCE=1; shift ;;
    -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
    *) echo "未知参数：$1" >&2; exit 2 ;;
  esac
done

[ -f "$CONF" ] || { echo "找不到配置文件：$CONF" >&2; exit 1; }
value_of() { sed -n "s|^[[:space:]]*$1[[:space:]]*=[[:space:]]*||p" "$CONF" | head -1; }
DATA_ROOT="$(value_of data_root)"
DATABASE="$(value_of database)"
TRANSPORT_KEY="$(value_of transport_key)"

[ -n "$DATA_ROOT" ] || { echo "配置里没有 data_root" >&2; exit 2; }
case "$DATA_ROOT" in
  /|/usr|/etc|/var|/home|/root|"") echo "拒绝：data_root 看起来不是一个数据目录（$DATA_ROOT）" >&2; exit 2 ;;
esac
[ "$DATA_ROOT" != "/var/lib/backup-project-server" ] || { echo "拒绝：data_root 指向实例根而不是数据目录" >&2; exit 2; }

echo "将要删除："
[ -d "$DATA_ROOT" ] && echo "  $DATA_ROOT        $(du -sh "$DATA_ROOT" 2>/dev/null | cut -f1) (所有云端备份 blob)"
[ -f "$DATABASE" ] && echo "  $DATABASE         $(du -h "$DATABASE" 2>/dev/null | cut -f1) (元数据库：用户与快照索引)"
if [ "$INCLUDE_IDENTITY" -eq 1 ]; then
  echo "  $TRANSPORT_KEY    (服务器传输身份私钥：删掉之后所有客户端的指纹/pin 全部失效)"
fi
echo "保留：/etc/backup-project-server/（配置与 token secret）$( [ "$INCLUDE_IDENTITY" -eq 0 ] && echo '、传输身份私钥、证书' )"

if [ "$DRY_RUN" -eq 1 ]; then
  echo "[dry-run] 什么都没删。"
  exit 0
fi

if [ "$FORCE" -ne 1 ]; then
  echo
  echo "这是不可逆操作。请输入下面这行以确认："
  echo "  DELETE ALL BACKUP DATA"
  printf '确认：'
  read -r answer || answer=""
  [ "$answer" = "DELETE ALL BACKUP DATA" ] || { echo "确认文字不匹配，已取消。" >&2; exit 2; }
fi

# 先让服务停下来，避免边删边写。
if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
  systemctl stop backup-project-server 2>/dev/null || true
fi

[ -d "$DATA_ROOT" ] && find "$DATA_ROOT" -mindepth 1 -maxdepth 1 -exec rm -rf -- {} +
rm -f "$DATABASE" "$DATABASE-journal" "$DATABASE-wal" "$DATABASE-shm"
if [ "$INCLUDE_IDENTITY" -eq 1 ]; then
  [ -n "$TRANSPORT_KEY" ] && rm -f "$TRANSPORT_KEY"
fi
echo "已删除备份数据与元数据库。"
echo "重新启动服务： sudo systemctl start backup-project-server"
