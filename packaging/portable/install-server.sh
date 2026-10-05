#!/usr/bin/env bash
# install.sh —— 便携安装（不依赖 dpkg、也不假设一定有 systemd）。
#
#   sudo ./install.sh [--prefix /opt/backup-project-server]
#                     [--no-systemd] [--port 18765] [--bind 127.0.0.1]
#                     [--user backup-project] [--no-user]
#
# 与 .deb 同一套目录语义（data/ state/ etc/ bin/），只是根在 --prefix 下：
#
#   <prefix>/bin/            可执行文件 + 启动器
#   <prefix>/share/          公开材料（官方根公钥）
#   <prefix>/docs/           文档
#   <prefix>/etc/            server.conf、secrets.env
#   <prefix>/var/data/       备份 blob
#   <prefix>/var/state/      元数据库 + 传输身份私钥
#   <prefix>/lib/systemd/system/backup-project-server.service   （--no-systemd 时没有）
#
# 安全性质与 deb 完全一致：默认只监听 127.0.0.1；已存在的 secrets.env /
# transport.key / 数据库 / data 目录**绝不覆盖**；失败时清理自己的 staging。
set -Eeuo pipefail

PREFIX=/opt/backup-project-server
NO_SYSTEMD=0
PORT=18765
BIND=127.0.0.1
SVC_USER=backup-project
MAKE_USER=1
CONFIRM=0

while [ $# -gt 0 ]; do
  case "$1" in
    --prefix) PREFIX="$2"; shift 2 ;;
    --no-systemd) NO_SYSTEMD=1; shift ;;
    --port) PORT="$2"; shift 2 ;;
    --bind) BIND="$2"; shift 2 ;;
    --user) SVC_USER="$2"; shift 2 ;;
    --no-user) MAKE_USER=0; shift ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "未知参数：$1" >&2; exit 2 ;;
  esac
done

SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
die() { echo "[install] ERROR: $*" >&2; exit 1; }
log() { echo "[install] $*"; }

# ---- 0. 前置检查：空路径 / 危险路径一律拒绝 ----
case "$PREFIX" in
  ""|/|/usr|/etc|/var|/bin|/lib|/sbin|/boot|/dev|/proc|/sys) die "拒绝安装在 $PREFIX=$PREFIX" ;;
esac
[ -f "$SRC_DIR/MANIFEST.sha256" ] || die "找不到 MANIFEST.sha256（请在解包后的目录里运行 install.sh）"

if [ "$(id -u)" -ne 0 ] && [ "$NO_SYSTEMD" -eq 1 ] && [ "$MAKE_USER" -eq 0 ]; then
  log "非 root 且 --no-systemd --no-user：只解包到 $PREFIX（不做任何系统改动）"
fi

# ---- 1. 清单校验（先验内容，再动系统）----
log "校验包内容（MANIFEST.sha256）"
if ! ( cd "$SRC_DIR" && sha256sum -c --quiet MANIFEST.sha256 ); then
  die "包内容与 MANIFEST.sha256 不一致，拒绝安装"
fi

# ---- 2. 并发锁 + 原子 staging ----
STAGE="$(mktemp -d "${TMPDIR:-/tmp}/bp-server-install.XXXXXX")"
cleanup() { rm -rf "$STAGE"; }
trap cleanup EXIT

log "staging -> $STAGE"
mkdir -p "$STAGE/bin" "$STAGE/share/backup-project" "$STAGE/docs"
cp -a "$SRC_DIR"/bin/. "$STAGE/bin/"
[ -d "$SRC_DIR/share" ] && cp -a "$SRC_DIR"/share/. "$STAGE/share/"
[ -d "$SRC_DIR/docs" ] && cp -a "$SRC_DIR"/docs/. "$STAGE/docs/"

# 打包层附带的启动器 / 配置模板 / unit（在 tarball 的 packaging/ 子目录里）
[ -d "$SRC_DIR/packaging" ] && cp -a "$SRC_DIR"/packaging/. "$STAGE/packaging/"

# ---- 3. 建用户 ----
if [ "$MAKE_USER" -eq 1 ] && [ "$(id -u)" -eq 0 ]; then
  if ! getent group "$SVC_USER" >/dev/null 2>&1; then
    addgroup --system "$SVC_USER"
    log "已创建系统组 $SVC_USER"
  fi
  if ! getent passwd "$SVC_USER" >/dev/null 2>&1; then
    adduser --system --ingroup "$SVC_USER" --home "$PREFIX" --no-create-home \
      --shell /usr/sbin/nologin --gecos "Backup Project server" "$SVC_USER" >/dev/null
    log "已创建系统用户 $SVC_USER（nologin）"
  fi
fi

# ---- 4. 目录（已存在就保持不动，权限只收紧不放开）----
install -d -m 0755 "$PREFIX"
if [ "$MAKE_USER" -eq 1 ] && [ "$(id -u)" -eq 0 ]; then
  install -d -m 0750 -o "$SVC_USER" -g "$SVC_USER" "$PREFIX/bin" "$PREFIX/var" "$PREFIX/var/data" "$PREFIX/var/state"
  install -d -m 0750 -o root -g "$SVC_USER" "$PREFIX/etc"
else
  install -d -m 0755 "$PREFIX/bin" "$PREFIX/var" "$PREFIX/var/data" "$PREFIX/var/state" "$PREFIX/etc"
fi

# ---- 5. 发布（先拷到同文件系统临时目录，再原子替换 bin/）----
log "安装程序文件 -> $PREFIX/bin"
rm -rf "$PREFIX/.bin.staging.$$"
cp -a "$STAGE/bin" "$PREFIX/.bin.staging.$$"
rm -rf "$PREFIX/bin.old.$$"
[ -d "$PREFIX/bin" ] && mv "$PREFIX/bin" "$PREFIX/bin.old.$$"
mv "$PREFIX/.bin.staging.$$" "$PREFIX/bin"
rm -rf "$PREFIX/bin.old.$$"
mkdir -p "$PREFIX/share" "$PREFIX/docs"
cp -a "$STAGE/share/." "$PREFIX/share/" 2>/dev/null || true
cp -a "$STAGE/docs/." "$PREFIX/docs/" 2>/dev/null || true
if [ "$MAKE_USER" -eq 1 ] && [ "$(id -u)" -eq 0 ]; then
  chown -R "$SVC_USER:$SVC_USER" "$PREFIX/bin" "$PREFIX/share" 2>/dev/null || true
fi

# ---- 6. 配置（只在缺失时生成；已存在的一律 preserve）----
CONF="$PREFIX/etc/server.conf"
if [ ! -e "$CONF" ]; then
  sed -e "s|^bind = .*|bind = $BIND|" \
      -e "s|^port = .*|port = $PORT|" \
      -e "s|^data_root = .*|data_root = $PREFIX/var/data|" \
      -e "s|^database = .*|database = $PREFIX/var/state/metadata.sqlite3|" \
      -e "s|^secret_file = .*|secret_file = $PREFIX/etc/secrets.env|" \
      -e "s|^transport_key = .*|transport_key = $PREFIX/var/state/transport.key|" \
      -e "s|^pid_file = .*|pid_file = $PREFIX/var/state/backup-server.pid|" \
      "$STAGE/packaging/server/server.conf" > "$CONF"
  chmod 0644 "$CONF"
  [ "$(id -u)" -eq 0 ] && chown root:"$SVC_USER" "$CONF" 2>/dev/null || true
  log "已生成配置：$CONF（默认只监听 $BIND）"
else
  log "配置已存在，保持不变：$CONF"
fi

if [ ! -e "$PREFIX/etc/secrets.env" ]; then
  umask 007
  printf 'BACKUP_TOKEN_SECRET=%s\n' "$(head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n')" > "$PREFIX/etc/secrets.env"
  # 产品要求 secret 文件 0600（0400 也接受）：group/other 一律不允许。
  if [ "$(id -u)" -eq 0 ] && [ "$MAKE_USER" -eq 1 ]; then
    chown "$SVC_USER:$SVC_USER" "$PREFIX/etc/secrets.env" 2>/dev/null || true
  fi
  chmod 0600 "$PREFIX/etc/secrets.env"
  log "已生成随机 BACKUP_TOKEN_SECRET：$PREFIX/etc/secrets.env（0600）"
else
  log "secrets.env 已存在，保持不变"
fi

KEY="$PREFIX/var/state/transport.key"
if [ ! -e "$KEY" ]; then
  if [ "$(id -u)" -eq 0 ] && [ "$MAKE_USER" -eq 1 ] && command -v runuser >/dev/null 2>&1; then
    runuser -u "$SVC_USER" -- "$PREFIX/bin/backup-server-keygen" --output "$KEY" >/dev/null
  else
    "$PREFIX/bin/backup-server-keygen" --output "$KEY" >/dev/null
  fi
  chmod 0600 "$KEY"
  log "已生成传输身份私钥：$KEY（0600）"
else
  log "传输身份私钥已存在，保持不变：$KEY"
fi

# ---- 7. systemd（可选）----
if [ "$NO_SYSTEMD" -eq 0 ] && [ -d /run/systemd/system ] && [ "$(id -u)" -eq 0 ]; then
  UNIT=/etc/systemd/system/backup-project-server.service
  sed -e "s|^ExecStart=.*|ExecStart=$PREFIX/bin/launch-server.sh --config $CONF|" \
      -e "s|^User=.*|User=$SVC_USER|" \
      -e "s|^Group=.*|Group=$SVC_USER|" \
      -e "s|^ReadWritePaths=.*|ReadWritePaths=$PREFIX/var|" \
      -e "/^RuntimeDirectory=/d" \
      -e "s|^\[Install\]|[Install]|" \
      "$STAGE/packaging/server/backup-project-server.service" > "$UNIT"
  systemctl daemon-reload
  systemctl enable backup-project-server.service >/dev/null 2>&1 || true
  if "$PREFIX/bin/launch-server.sh" --check-config --config "$CONF" >/dev/null 2>&1; then
    systemctl restart backup-project-server.service && log "服务已启动（systemctl status backup-project-server）" \
      || log "WARN: 服务启动失败，看 journalctl -u backup-project-server"
  else
    log "WARN: 配置自检未通过，服务未启动：$PREFIX/bin/launch-server.sh --check-config --config $CONF"
  fi
else
  log "跳过 systemd（--no-systemd 或当前环境没有 systemd）"
  log "手工启动：$PREFIX/bin/launch-server.sh --config $CONF"
fi

log "安装完成。管理菜单：$PREFIX/bin/backup-server-admin.sh（需要 BACKUP_SERVER_ROOT=$PREFIX）"
log "卸载：$PREFIX/docs/uninstall.sh --prefix $PREFIX"
