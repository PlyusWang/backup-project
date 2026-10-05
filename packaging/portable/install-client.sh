#!/usr/bin/env bash
# install.sh —— 客户端便携安装（不需要 root）。
#
#   ./install.sh [--prefix ~/.local] [--no-desktop]
#
# 包里的 app/ 是自带 Qt 运行时的完整树（bin/ lib/ plugins/ qml/），安装后放在
# <prefix>/lib/backup-project/ 下，并在 <prefix>/bin/ 放两个入口：
#
#   <prefix>/bin/backupctl             客户端 CLI
#   <prefix>/bin/backup-gui-modern     现代 GUI（自带 Qt，目标机不需要装 Qt）
#
# 绝不碰用户数据：备份、配置、缓存都不动（卸载同理）。
set -Eeuo pipefail

PREFIX="${HOME}/.local"
NO_DESKTOP=0
while [ $# -gt 0 ]; do
  case "$1" in
    --prefix) PREFIX="$2"; shift 2 ;;
    --no-desktop) NO_DESKTOP=1; shift ;;
    -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
    *) echo "未知参数：$1" >&2; exit 2 ;;
  esac
done

SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
die() { echo "[install] ERROR: $*" >&2; exit 1; }
log() { echo "[install] $*"; }

case "$PREFIX" in ""|/) die "拒绝安装在 \$PREFIX=$PREFIX" ;; esac
[ -f "$SRC_DIR/MANIFEST.sha256" ] || die "找不到 MANIFEST.sha256"
( cd "$SRC_DIR" && sha256sum -c --quiet MANIFEST.sha256 ) || die "包内容与 MANIFEST.sha256 不一致"

LIB_ROOT="$PREFIX/lib/backup-project"
STAGE="$PREFIX/lib/.backup-project.staging.$$"
rm -rf "$STAGE"
mkdir -p "$STAGE" "$PREFIX/bin"
for part in bin lib plugins qml share; do
  [ -d "$SRC_DIR/$part" ] && cp -a "$SRC_DIR/$part" "$STAGE/"
done
mkdir -p "$STAGE/docs"; [ -d "$SRC_DIR/docs" ] && cp -a "$SRC_DIR"/docs/. "$STAGE/docs/"

# 原子替换：先把新树就位，再换名，最后删旧的。
PREV="$PREFIX/lib/.backup-project.prev.$$"
[ -d "$LIB_ROOT" ] && mv "$LIB_ROOT" "$PREV"
mv "$STAGE" "$LIB_ROOT"
rm -rf "$PREV"

for tool in backupctl backup-gui-modern; do
  [ -e "$LIB_ROOT/bin/$tool" ] || continue
  ln -sf "$LIB_ROOT/bin/$tool" "$PREFIX/bin/$tool"
done

if [ "$NO_DESKTOP" -eq 0 ] && [ -f "$SRC_DIR/packaging/client/backup-project.desktop" ]; then
  APPDIR="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
  mkdir -p "$APPDIR"
  sed -e "s|^Exec=.*|Exec=$PREFIX/bin/backup-gui-modern|" \
      -e "s|^TryExec=.*|TryExec=$PREFIX/bin/backup-gui-modern|" \
      "$SRC_DIR/packaging/client/backup-project.desktop" > "$APPDIR/backup-project.desktop"
  command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$APPDIR" >/dev/null 2>&1 || true
  log "已安装桌面入口：$APPDIR/backup-project.desktop"
fi

log "安装完成：$PREFIX/bin/backupctl、$PREFIX/bin/backup-gui-modern"
log "卸载：$SRC_DIR/packaging/portable/uninstall-client.sh --prefix $PREFIX"
