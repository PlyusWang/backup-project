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
[[ ! "$PREFIX" =~ [[:cntrl:]] ]] || die "安装路径不能包含控制字符"
PREFIX="$(realpath -m -- "$PREFIX")"
[ "$PREFIX" != / ] || die "拒绝安装在根目录"
if [ "$NO_DESKTOP" -eq 0 ]; then
  [[ "$PREFIX" != *%* ]] || die "桌面入口不支持包含 % 的安装路径；请更换路径，或添加 --no-desktop 仅安装程序"
  DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"
  [[ "$DATA_HOME" = /* && ! "$DATA_HOME" =~ [[:cntrl:]] ]] || die "桌面资源目录必须是无控制字符的绝对路径"
  DATA_HOME="$(realpath -m -- "$DATA_HOME")"
fi
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

# 第三方许可材料：随 portable 安装一起保留（发布二进制时必须随包提供给用户）
if [ -f "$SRC_DIR/THIRD-PARTY-NOTICES.txt" ]; then
  install -d -m 0755 "$LIB_ROOT/doc"
  install -m 0644 "$SRC_DIR/THIRD-PARTY-NOTICES.txt" "$LIB_ROOT/doc/"
  [ -f "$SRC_DIR/LICENSE-INVENTORY.md" ] && install -m 0644 "$SRC_DIR/LICENSE-INVENTORY.md" "$LIB_ROOT/doc/"
  [ -d "$SRC_DIR/licenses" ] && { rm -rf "$LIB_ROOT/doc/licenses"; cp -a "$SRC_DIR/licenses" "$LIB_ROOT/doc/"; }
  log "已随安装保留第三方许可材料：$LIB_ROOT/doc/"
fi

if [ "$NO_DESKTOP" -eq 0 ] && [ -f "$SRC_DIR/packaging/client/backup-project.desktop" ]; then
  APPDIR="$DATA_HOME/applications"
  ICONDIR="$DATA_HOME/icons/hicolor/256x256/apps"
  APP_ID="backup-project-$(printf '%s' "$LIB_ROOT" | sha256sum | cut -c1-16)"
  ICON_SOURCE="$LIB_ROOT/share/icons/hicolor/256x256/apps/backup-project.png"
  [ -f "$ICON_SOURCE" ] || die "包内缺少正式图标：$ICON_SOURCE"
  mkdir -p "$APPDIR" "$ICONDIR"
  install -m 0644 "$ICON_SOURCE" "$ICONDIR/$APP_ID.png"
  # Escape the Exec argument first, then the desktop string layer. TryExec
  # and Icon are plain string paths, so they only need the latter.
  desktop_value() { DESKTOP_VALUE="${1//\\/\\\\}"; }
  desktop_exec() {
    local value="$1" escaped='' character index
    for ((index=0; index<${#value}; index++)); do
      character="${value:index:1}"
      case "$character" in
        \\|'"'|'$'|'`') escaped+="\\$character" ;;
        '%') escaped+='%%' ;;
        *) escaped+="$character" ;;
      esac
    done
    desktop_value "$escaped"
    printf '"%s"' "$DESKTOP_VALUE"
  }

  DESKTOP_FILE="$APPDIR/$APP_ID.desktop"
  DESKTOP_TMP="$(mktemp "$APPDIR/.$APP_ID.XXXXXXXX")"
  while IFS= read -r line || [ -n "$line" ]; do
    case "$line" in
      Exec=*) printf 'Exec=%s\n' "$(desktop_exec "$PREFIX/bin/backup-gui-modern")" ;;
      TryExec=*) desktop_value "$PREFIX/bin/backup-gui-modern"; printf 'TryExec=%s\n' "$DESKTOP_VALUE" ;;
      Icon=*) desktop_value "$ICONDIR/$APP_ID.png"; printf 'Icon=%s\n' "$DESKTOP_VALUE" ;;
      *) printf '%s\n' "$line" ;;
    esac
  done <"$SRC_DIR/packaging/client/backup-project.desktop" >"$DESKTOP_TMP"
  desktop_value "$LIB_ROOT"
  printf 'X-Backup-Project-Prefix=%s\n' "$DESKTOP_VALUE" >>"$DESKTOP_TMP"
  chmod 0644 "$DESKTOP_TMP"
  mv -f "$DESKTOP_TMP" "$DESKTOP_FILE"
  command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$APPDIR" >/dev/null 2>&1 || true
  command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -f -t "$DATA_HOME/icons/hicolor" >/dev/null 2>&1 || true
  log "已安装桌面入口：$DESKTOP_FILE"
fi

log "安装完成：$PREFIX/bin/backupctl、$PREFIX/bin/backup-gui-modern"
log "卸载：$SRC_DIR/packaging/portable/uninstall-client.sh --prefix $PREFIX"
