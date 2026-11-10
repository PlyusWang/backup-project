#!/usr/bin/env bash
#
# backup-server-admin.sh —— 备份服务器本机管理菜单（只在 ECS 本机使用）。
#
# 它只做四件事：清屏、显示菜单、读选择、调用同目录下的 backup-server-admin。
# 真正危险的数据操作**一个都不在这里做**：没有 SQL，没有 rm，没有直接改目录。
#
# ---- 实例身份（这一版的重点）----
#
# 部署布局（scripts/deploy_aliyun_server.sh 安装出来的样子）：
#
#   <server-root>/bin/backup-server-admin        管理工具
#   <server-root>/bin/backup-server-admin.sh     本脚本
#   <server-root>/data                           数据根
#   <server-root>/state/metadata.sqlite3         元数据库
#
# 脚本从**自己所在的目录**推出这个布局，因此天然与同一个部署里的 backup-server
# 指向同一个状态根。上一版硬编码成 "$HOME/backup-project-server/data/metadata.sqlite3"，
# 而服务端用的是 state/metadata.sqlite3 —— 路径分叉加上 SQLite 的"文件不存在就
# 新建"，让管理工具读到了一个**刚被自己创建出来的空库**，屏幕上显示"还没有任何
# 用户"，人工验收因此以为 ECS 上没有任何用户（其实用户都在正确的库里）。
#
# 现在：路径错了就明确失败（fail closed），绝不创建空库；而且每次运行都会先打印
# Host / Server root / Data root / Metadata DB / Service，让人一眼看出在看哪个实例。
#
# ---- 传输身份指纹（PR #21 UI closure）----
#
# 首页（banner 调用的 status）现在还会打印一行：
#
#   传输身份指纹: sha256:<64 位十六进制>
#
# 它就是客户端"服务器身份指纹（server-key）"要填的那一串，用户不用再去翻日志、
# 翻部署脚本或者找 keygen。菜单第 5 项"服务器身份信息"会显示完整的 Fingerprint
# 与 Public key（私钥不显示）。指纹一律由管理工具算，本脚本只负责显示。
#
# 用法（先 SSH 登录到 ECS，再在本机执行）：
#
#   ssh aliyun-ecs
#   cd ~/backup-project-server
#   ./bin/backup-server-admin.sh
#
# 环境变量（覆盖自动推导；都是可选）：
#
#   BACKUP_SERVER_ROOT   部署根，默认 = 本脚本所在目录的上一级；
#                        在源码树里运行时**必须**显式给出，否则 fail closed
#   BACKUP_SERVER_DATA   数据根，默认 = $BACKUP_SERVER_ROOT/data
#   BACKUP_SERVER_DB     元数据库，默认 = $BACKUP_SERVER_ROOT/state/metadata.sqlite3
#   BACKUP_SERVER_ADMIN  管理工具，默认 = 与本脚本同目录的 backup-server-admin
#
# 本脚本不读、不打印 token secret：管理工具不需要它。

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# 部署布局：管理工具与本脚本同在 bin/ 下，部署根就是它的上一级——这一条只对
# **部署后**的脚本成立，因为那个 bin/.. 就是被管理的那个实例。
if [ -x "$SCRIPT_DIR/backup-server-admin" ]; then
  ADMIN_BIN="${BACKUP_SERVER_ADMIN:-$SCRIPT_DIR/backup-server-admin}"
  SERVER_ROOT_DEFAULT="$(cd "$SCRIPT_DIR/.." && pwd)"
  SERVER_ROOT="${BACKUP_SERVER_ROOT:-$SERVER_ROOT_DEFAULT}"
else
  # 源码树里直接跑（<repo>/scripts/backup-server-admin.sh）：产物在 build/ 下，
  # 这里**推不出**任何真实实例。以前它猜 $HOME/backup-project-server——猜错了就会
  # 去读另一个实例（SQLite 还会顺手把不存在的文件建成空库），人工验收因此得出过
  # 完全错误的结论。现在 fail closed：要么用部署后的脚本，要么显式给出
  # BACKUP_SERVER_ROOT。
  ADMIN_BIN="${BACKUP_SERVER_ADMIN:-$SCRIPT_DIR/../build/backup-server-admin}"
  if [ -z "${BACKUP_SERVER_ROOT:-}" ]; then
    echo "ERROR: 这是源码树里的脚本，无法推断要管理哪个实例。" >&2
    echo "  请 SSH 到服务器，使用部署后的 <server-root>/bin/backup-server-admin.sh；" >&2
    echo "  或者显式设置 BACKUP_SERVER_ROOT（可选 BACKUP_SERVER_DATA / BACKUP_SERVER_DB）。" >&2
    exit 1
  fi
  SERVER_ROOT="$BACKUP_SERVER_ROOT"
fi

DATA_ROOT="${BACKUP_SERVER_DATA:-$SERVER_ROOT/data}"
DB_PATH="${BACKUP_SERVER_DB:-$SERVER_ROOT/state/metadata.sqlite3}"
if [ -x "$SCRIPT_DIR/backup-server" ]; then
  SERVER_BIN_DEFAULT="$SCRIPT_DIR/backup-server"
  KEYGEN_BIN_DEFAULT="$SCRIPT_DIR/backup-server-keygen"
  if [ -x "$SCRIPT_DIR/backup-cert-tool" ]; then
    CERT_TOOL_DEFAULT="$SCRIPT_DIR/backup-cert-tool"
  else
    CERT_TOOL_DEFAULT="$SCRIPT_DIR/../tools/backup-cert-tool"
  fi
else
  SERVER_BIN_DEFAULT="$SCRIPT_DIR/../build/backup-server"
  KEYGEN_BIN_DEFAULT="$SCRIPT_DIR/../build/backup-server-keygen"
  CERT_TOOL_DEFAULT="$SCRIPT_DIR/../build/backup-cert-tool"
fi
SERVER_BIN="${BACKUP_SERVER_BIN:-$SERVER_BIN_DEFAULT}"
KEYGEN_BIN="${BACKUP_SERVER_KEYGEN:-$KEYGEN_BIN_DEFAULT}"
CERT_TOOL="${BACKUP_CERT_TOOL:-$CERT_TOOL_DEFAULT}"
if [ -x "$SCRIPT_DIR/launch-server.sh" ]; then
  LAUNCHER="$SCRIPT_DIR/launch-server.sh"
else
  LAUNCHER="$SCRIPT_DIR/../packaging/server/launch-server.sh"
fi
CONFIG_FILE="${BACKUP_SERVER_CONFIG:-$SERVER_ROOT/server.conf}"
PID_FILE="${BACKUP_SERVER_PID_FILE:-$SERVER_ROOT/state/backup-server.pid}"
LOG_FILE="${BACKUP_SERVER_LOG:-$SERVER_ROOT/logs/server.log}"

if [ ! -x "$ADMIN_BIN" ]; then
  echo "找不到管理工具：$ADMIN_BIN" >&2
  echo "先构建：make server（产物在 build/backup-server-admin）" >&2
  exit 1
fi
config_value() {
  [ -f "$CONFIG_FILE" ] || return 0
  awk -F= -v key="$1" '$1 ~ "^[[:space:]]*" key "[[:space:]]*$" { sub(/^[[:space:]]*/, "", $2); sub(/[[:space:]]*$/, "", $2); print $2; exit }' "$CONFIG_FILE"
}
configured_port() { config_value port | head -1; }
configured_cert() { config_value certificate | head -1; }
configured_root_pub="${BACKUP_SERVER_ROOT_PUBLIC:-$SERVER_ROOT/private/root.key.pub}"
if [ -z "${BACKUP_SERVER_DATA:-}" ] && [ -n "$(config_value data_root)" ]; then
  DATA_ROOT="$(config_value data_root)"
fi
if [ -z "${BACKUP_SERVER_DB:-}" ] && [ -n "$(config_value database)" ]; then
  DB_PATH="$(config_value database)"
fi
if [ -z "${BACKUP_SERVER_PID_FILE:-}" ] && [ -n "$(config_value pid_file)" ]; then
  PID_FILE="$(config_value pid_file)"
fi
if [ -z "${BACKUP_SERVER_LOG:-}" ]; then
  configured_log="$(config_value log_file)"
  [ -z "$configured_log" ] || LOG_FILE="$configured_log"
fi

# Paths derived by the portable wrapper must follow the instance root entered
# during initialization. Caller-supplied BACKUP_SERVER_* paths stay fixed.
MANAGER_DEFAULT_CONFIG="${BACKUP_SERVER_CONFIG_DEFAULT:-0}"
MANAGER_DEFAULT_ROOT_PUBLIC="${BACKUP_SERVER_ROOT_PUBLIC_DEFAULT:-0}"
MANAGER_DEFAULT_DATA="${BACKUP_SERVER_DATA_DEFAULT:-0}"
MANAGER_DEFAULT_DB="${BACKUP_SERVER_DB_DEFAULT:-0}"
MANAGER_DEFAULT_PID_FILE="${BACKUP_SERVER_PID_FILE_DEFAULT:-0}"
MANAGER_DEFAULT_LOG="${BACKUP_SERVER_LOG_DEFAULT:-0}"

instance_initialized() {
  local secret_file key_file
  secret_file="$(config_value secret_file)"
  key_file="$(config_value transport_key)"
  [ -f "$CONFIG_FILE" ] && [ -n "$secret_file" ] &&
    [ -f "$secret_file" ] && [ -n "$key_file" ] &&
    [ -f "$key_file" ] && [ -d "$DATA_ROOT" ]
}
instance_ready() { [ -d "$DATA_ROOT" ] && [ -f "$DB_PATH" ]; }

port_listener_exists() {
  local port="$1"
  case "$port" in ''|*[!0-9]*) return 1 ;; esac
  [ "$port" -ge 1 ] && [ "$port" -le 65535 ] || return 1
  ss -ltnH "sport = :$port" 2>/dev/null | grep -q .
}

pid_is_ours() {
  local pid="$1" cmd
  case "$pid" in ''|*[!0-9]*) return 1 ;; esac
  [ "$pid" -gt 1 ] 2>/dev/null || return 1
  kill -0 "$pid" 2>/dev/null || return 1
  [ -r "/proc/$pid/cmdline" ] || return 1
  cmd="$(tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null || true)"
  case "$cmd" in
    *backup-server*"--root"*"$DATA_ROOT"*"--db"*"$DB_PATH"*) return 0 ;;
    *) return 1 ;;
  esac
}

owned_pid() {
  [ -f "$PID_FILE" ] || return 1
  local pid
  pid="$(tr -d '[:space:]' < "$PID_FILE" 2>/dev/null || true)"
  pid_is_ours "$pid" && printf '%s\n' "$pid"
}

clear_screen() {
  [ -t 1 ] || return 0
  if [ -n "${TERM:-}" ] && command -v clear >/dev/null 2>&1; then
    clear
  else
    printf '\033[H\033[2J'
  fi
}

admin() {
  instance_ready || { echo "服务器尚未初始化：请先选择“初始化服务器”。"; return 1; }
  "$ADMIN_BIN" --server-root "$SERVER_ROOT" --root "$DATA_ROOT" \
    --db "$DB_PATH" "$@"
}

pause() {
  printf '\n按回车返回...'
  read -r _ || true
}

banner() {
  clear_screen
  printf '========================================\n'
  printf '      Backup Project Server Admin\n'
  printf '========================================\n\n'
  printf '实例根目录：%s\n' "$SERVER_ROOT"
  if instance_ready; then
    admin status || true
  elif instance_initialized; then
    printf '状态：已初始化，数据库尚未创建\n'
  else
    printf '状态：尚未初始化\n'
  fi
  if instance_initialized || instance_ready; then
    if pid="$(owned_pid 2>/dev/null)"; then
      printf '进程状态：运行中（PID %s）\n' "$pid"
    elif port_listener_exists "$(configured_port)"; then
      printf '进程状态：端口 %s 已监听，但进程不属于当前实例管理器\n' "$(configured_port)"
    else
      printf '进程状态：已初始化，当前未运行\n'
    fi
  fi
  printf '\n'
}

init_server() {
  if instance_initialized || instance_ready; then
    printf '已检测到现有实例配置或数据，初始化已拒绝，不会覆盖已有内容。\n'
    return 1
  fi
  printf '实例根目录 [%s]：' "$SERVER_ROOT"; read -r entered || return 0
  if [ -n "$entered" ]; then SERVER_ROOT="$entered"; fi
  if [ "$MANAGER_DEFAULT_DATA" = 1 ]; then
    DATA_ROOT="$SERVER_ROOT/data"
  else
    DATA_ROOT="${BACKUP_SERVER_DATA:-$SERVER_ROOT/data}"
  fi
  if [ "$MANAGER_DEFAULT_DB" = 1 ]; then
    DB_PATH="$SERVER_ROOT/state/metadata.sqlite3"
  else
    DB_PATH="${BACKUP_SERVER_DB:-$SERVER_ROOT/state/metadata.sqlite3}"
  fi
  if [ "$MANAGER_DEFAULT_PID_FILE" = 1 ]; then
    PID_FILE="$SERVER_ROOT/state/backup-server.pid"
  else
    PID_FILE="${BACKUP_SERVER_PID_FILE:-$SERVER_ROOT/state/backup-server.pid}"
  fi
  if [ "$MANAGER_DEFAULT_LOG" = 1 ]; then
    LOG_FILE="$SERVER_ROOT/logs/server.log"
  else
    LOG_FILE="${BACKUP_SERVER_LOG:-$SERVER_ROOT/logs/server.log}"
  fi
  if [ "$MANAGER_DEFAULT_CONFIG" = 1 ]; then
    CONFIG_FILE="$SERVER_ROOT/server.conf"
  else
    CONFIG_FILE="${BACKUP_SERVER_CONFIG:-$SERVER_ROOT/server.conf}"
  fi
  if [ "$MANAGER_DEFAULT_ROOT_PUBLIC" = 1 ]; then
    configured_root_pub="$SERVER_ROOT/private/root.key.pub"
  else
    configured_root_pub="${BACKUP_SERVER_ROOT_PUBLIC:-$SERVER_ROOT/private/root.key.pub}"
  fi
  case "$SERVER_ROOT" in /*) ;; *) printf '实例目录必须是绝对路径。\n'; return 1 ;; esac
  local secret_file="$SERVER_ROOT/secrets.env" key_file="$SERVER_ROOT/state/transport.key"
  local root_key="$SERVER_ROOT/private/root.key" cert_path="$SERVER_ROOT/state/server.bpcert"
  if [ -e "$DB_PATH" ] || [ -e "$CONFIG_FILE" ] || [ -e "$key_file" ] || [ -e "$secret_file" ] ||
      [ -e "$root_key" ] || [ -e "${root_key}.pub" ] || [ -e "$configured_root_pub" ] || [ -e "$cert_path" ] ||
      { [ -d "$DATA_ROOT" ] && find "$DATA_ROOT" -mindepth 1 -print -quit | grep -q .; }; then
    printf '目标目录已有数据库、配置、密钥或备份文件，初始化已拒绝，不会覆盖。\n'
    return 1
  fi
  printf '监听地址 [127.0.0.1]：'; read -r bind || return 0; [ -n "$bind" ] || bind=127.0.0.1
  printf '业务端口 [18765]：'; read -r port || return 0; [ -n "$port" ] || port=18765
  if [ "$bind" != "127.0.0.1" ]; then
    printf '当前服务端只支持 127.0.0.1 回环监听，未生成配置。\n'
    return 1
  fi
  case "$port" in ''|*[!0-9]*) printf '端口必须是 1 到 65535 的整数。\n'; return 1 ;; esac
  if [ "$port" -lt 1 ] || [ "$port" -gt 65535 ] || port_listener_exists "$port"; then
    printf '端口无效或已被占用：%s\n' "$port"
    return 1
  fi
  printf '是否初始化 BPSEC2 证书身份（y/N）：'; read -r use_cert || return 0
  [ -x "$KEYGEN_BIN" ] || { printf '找不到 backup-server-keygen：%s\n' "$KEYGEN_BIN"; return 1; }
  if [[ "$use_cert" =~ ^[Yy]$ ]]; then
    [ -x "$CERT_TOOL" ] || { printf '找不到 backup-cert-tool：%s\n' "$CERT_TOOL"; return 1; }
    printf 'server_id [self-hosted-%s]：' "$port"; read -r server_id || return 0
    [ -n "$server_id" ] || server_id="self-hosted-$port"
  fi
  mkdir -p "$SERVER_ROOT" || return 1
  local keygen_output pubkey certificate="" require_bpsec2=false
  local init_dir config_tmp
  init_dir="$(mktemp -d "$SERVER_ROOT/.server-init.XXXXXXXX")" || return 1
  trap 'rm -rf "$init_dir"' RETURN
  mkdir -p "$init_dir/state" "$init_dir/private" || return 1
  keygen_output="$($KEYGEN_BIN --output "$init_dir/state/transport.key")" || return 1
  pubkey="$(printf '%s\n' "$keygen_output" | sed -n 's/.*公钥 (hex): //p' | head -1)"
  [ -n "$pubkey" ] || { printf '无法从 keygen 结果取得公钥，初始化停止。\n'; return 1; }
  if [[ "$use_cert" =~ ^[Yy]$ ]]; then
    [ -x "$CERT_TOOL" ] || { printf '找不到 backup-cert-tool：%s\n' "$CERT_TOOL"; return 1; }
    root_key="$SERVER_ROOT/private/root.key"
    cert_path="$SERVER_ROOT/state/server.bpcert"
    "$CERT_TOOL" root-init --root-key "$init_dir/private/root.key" --root-id "${server_id}-root" >/dev/null || return 1
    "$CERT_TOOL" issue-server --root-key "$init_dir/private/root.key" --server-id "$server_id" \
      --server-pubkey "hex:$pubkey" --out "$init_dir/state/server.bpcert" >/dev/null || return 1
    certificate="$cert_path"
    require_bpsec2=true
  fi
  head -c 32 /dev/urandom | sha256sum | awk '{print "BACKUP_TOKEN_SECRET="$1}' > "$init_dir/secrets.env"
  chmod 600 "$init_dir/secrets.env"
  cat > "$init_dir/server.conf" <<EOF
bind = $bind
port = $port
data_root = $DATA_ROOT
database = $DB_PATH
secret_file = $secret_file
transport_key = $key_file
certificate = $certificate
require_bpsec2 = $require_bpsec2
allow_public_bind = false
public_bind_reason =
allow_public_bind_insecure = false
log_file = $LOG_FILE
pid_file = $PID_FILE
workers = 4
io_timeout = 30
max_upload_bytes = 8589934592
max_login_failures = 5
login_lockout_seconds = 60
quiet = false
EOF
  chmod 600 "$init_dir/server.conf"
  mkdir -p "$DATA_ROOT" "$SERVER_ROOT/state" "$SERVER_ROOT/logs" "$SERVER_ROOT/private" \
    "$(dirname "$CONFIG_FILE")" || return 1
  mv "$init_dir/state/transport.key" "$key_file" || return 1
  mv "$init_dir/secrets.env" "$secret_file" || return 1
  if [[ "$use_cert" =~ ^[Yy]$ ]]; then
    mv "$init_dir/private/root.key" "$root_key" || return 1
    mkdir -p "$(dirname "$configured_root_pub")" || return 1
    mv "$init_dir/private/root.key.pub" "$configured_root_pub" || return 1
    mv "$init_dir/state/server.bpcert" "$cert_path" || return 1
  fi
  config_tmp="$(mktemp "$(dirname "$CONFIG_FILE")/.server.conf.XXXXXXXX")" || return 1
  cp "$init_dir/server.conf" "$config_tmp" && chmod 600 "$config_tmp" && mv "$config_tmp" "$CONFIG_FILE" || {
    rm -f "$config_tmp"
    return 1
  }
  trap - RETURN
  rm -rf "$init_dir"
  printf '\n初始化完成。之后可选择启动服务器。\n'
  if [ -n "$certificate" ]; then
    printf '客户端可信根公钥：%s\n服务器身份文件：%s\n' "$configured_root_pub" "$certificate"
  fi
}

start_server() {
  if ! instance_initialized; then printf '请先初始化服务器。\n'; return 1; fi
  local pid
  if pid="$(owned_pid 2>/dev/null)"; then printf '实例已经运行（PID %s）。\n' "$pid"; return 1; fi
  if port_listener_exists "$(configured_port)"; then
    printf '端口 %s 已被监听，拒绝启动第二个实例。\n' "$(configured_port)"
    return 1
  fi
  [ -x "$SERVER_BIN" ] || { printf '找不到服务端：%s\n' "$SERVER_BIN"; return 1; }
  [ -f "$CONFIG_FILE" ] || { printf '找不到配置：%s\n' "$CONFIG_FILE"; return 1; }
  mkdir -p "$(dirname "$LOG_FILE")" "$(dirname "$PID_FILE")"
  [ -x "$LAUNCHER" ] || { printf '找不到安全启动器：%s\n' "$LAUNCHER"; return 1; }
  BACKUP_SERVER_BIN="$SERVER_BIN" BACKUP_CERT_TOOL="$CERT_TOOL" \
    BACKUP_SERVER_CONF="$CONFIG_FILE" "$LAUNCHER" --config "$CONFIG_FILE" >>"$LOG_FILE" 2>&1 &
  sleep 0.2
  if pid="$(owned_pid 2>/dev/null)"; then printf '服务器已启动（PID %s）。\n' "$pid"; else printf '服务器未能启动，请查看日志：%s\n' "$LOG_FILE"; return 1; fi
}

stop_server() {
  local pid
  pid="$(owned_pid 2>/dev/null || true)"
  [ -n "$pid" ] || { printf '没有找到属于当前实例的运行进程。\n'; return 0; }
  kill -TERM "$pid" 2>/dev/null || { printf '停止信号发送失败。\n'; return 1; }
  for _ in $(seq 1 50); do
    kill -0 "$pid" 2>/dev/null || { printf '服务器已停止。\n'; return 0; }
    sleep 0.1
  done
  printf '服务器未在 5 秒内退出，未向其他进程发送信号。\n'; return 1
}

logs_page() {
  clear_screen
  [ -f "$LOG_FILE" ] && tail -80 "$LOG_FILE" || printf '日志尚不存在：%s\n' "$LOG_FILE"
}

certificate_page() {
  clear_screen
  printf '证书与可信根\n\n'
  local cert; cert="$(configured_cert)"
  if [ -n "$cert" ] && [ -f "$cert" ] && [ -x "$CERT_TOOL" ]; then
    printf '客户端可信根公钥：%s\n' "$configured_root_pub"
    printf '服务器身份文件：%s\n\n' "$cert"
    "$CERT_TOOL" inspect-server --cert "$cert" || true
    if [ -f "$configured_root_pub" ]; then
      printf '\n证书信任检查：\n'
      "$CERT_TOOL" verify-server --cert "$cert" --roots "$configured_root_pub" || true
    fi
  else
    printf 'BPSEC2：不适用（当前配置未启用服务器身份证书）。\n'
  fi
}

# ---- 用户管理 ----

user_menu() {
  while true; do
    banner
    printf '用户管理\n'
    printf '  1. 用户列表\n'
    printf '  2. 查看用户详情\n'
    printf '  3. 删除用户及其全部备份（危险）\n'
    printf '  0. 返回\n\n'
    printf '请选择：'
    read -r choice || return 0
    case "$choice" in
      1)
        clear_screen
        admin list-users
        pause
        ;;
      2)
        printf '用户选择器（id:<编号> 或 name:<用户名>）：'
        read -r selector || return 0
        clear_screen
        admin show-user "$selector"
        pause
        ;;
      3)
        # 删除是不可逆的：这里**要求**用户写成 id:<编号> 或 name:<用户名>。
        # 裸输入可能是编号也可能是用户名，管理工具不会替用户猜。
        printf '要删除的用户（必须写成 id:<编号> 或 name:<用户名>）：'
        read -r selector || return 0
        [ -n "$selector" ] || continue
        case "$selector" in
          id:*|name:*) ;;
          *)
            printf '\n删除操作必须明确指定用户：id:<编号> 或 name:<用户名>。\n'
            printf '（裸输入可能是编号也可能是用户名，管理工具不会替你猜。）\n'
            pause
            continue
            ;;
        esac
        clear_screen
        local detail target_name target_id
        detail="$(admin show-user "$selector" 2>&1)"
        printf '%s\n' "$detail"
        target_name="$(printf '%s\n' "$detail" | awk -F'：' '/^用户名：/ { print $2; exit }')"
        target_id="$(printf '%s\n' "$detail" | awk -F'：' '/^用户 ID：/ { print $2; exit }')"
        if [ -z "$target_name" ] || [ -z "$target_id" ]; then
          pause
          continue
        fi
        printf '\n'
        printf '警告：这会永久删除账户 "%s"（id=%s）以及它的全部云端备份，且无法撤销。\n' \
          "$target_name" "$target_id"
        printf '请输入 DELETE %s#%s 以确认（直接回车取消）：' \
          "$target_name" "$target_id"
        read -r confirmation || return 0
        [ -n "$confirmation" ] || continue
        admin delete-user "$selector" --confirm "$confirmation"
        pause
        ;;
      0) return 0 ;;
      *) ;;
    esac
  done
}

# ---- 备份文件管理 ----

snapshot_menu() {
  while true; do
    banner
    printf '备份文件管理\n'
    printf '  1. 列出某个用户的备份\n'
    printf '  2. 查看备份详情（含 SHA-256）\n'
    printf '  3. 删除单个备份（危险）\n'
    printf '  0. 返回\n\n'
    printf '请选择：'
    read -r choice || return 0
    case "$choice" in
      1)
        printf '用户选择器（id:<编号> 或 name:<用户名>）：'
        read -r selector || return 0
        clear_screen
        admin list-snapshots "$selector"
        pause
        ;;
      2)
        printf '快照 id：'
        read -r snapshot_id || return 0
        clear_screen
        admin show-snapshot "$snapshot_id"
        pause
        ;;
      3)
        # 与删除用户同一条规则：破坏性操作必须显式指定选择器。
        printf '用户选择器（必须写成 id:<编号> 或 name:<用户名>）：'
        read -r selector || return 0
        case "$selector" in
          id:*|name:*) ;;
          *)
            printf '\n删除操作必须明确指定用户：id:<编号> 或 name:<用户名>。\n'
            printf '（裸输入可能是编号也可能是用户名，管理工具不会替你猜。）\n'
            pause
            continue
            ;;
        esac
        printf '快照 id：'
        read -r snapshot_id || return 0
        [ -n "$selector" ] && [ -n "$snapshot_id" ] || continue
        clear_screen
        admin show-snapshot "$snapshot_id" || { pause; continue; }
        printf '\n'
        printf '警告：这会永久删除这一份云端备份，且无法撤销。\n'
        printf '请再次输入快照 id 以确认（直接回车取消）：'
        read -r confirmation || return 0
        [ -n "$confirmation" ] || continue
        admin delete-snapshot "$snapshot_id" --user "$selector" \
          --confirm "$confirmation"
        pause
        ;;
      0) return 0 ;;
      *) ;;
    esac
  done
}

# ---- 服务器身份（只显示，不算指纹）----
#
# 指纹由 backup-server-admin / 共享的 LoadTransportIdentity + X25519Fingerprint
# 算出来——shell 这一层**不重新实现**任何编码，也不读私钥文件的内容。它只把
# 管理工具打印的那一页原样显示出来，"显示的指纹 == 服务端在用的身份"这条性质
# 因此只有一份实现。
identity_page() {
  clear_screen
  admin transport-identity
}

# ---- 主循环 ----

while true; do
  banner
  printf '  1. 初始化服务器\n'
  printf '  2. 启动服务器\n'
  printf '  3. 停止服务器\n'
  printf '  4. 查看服务器状态\n'
  printf '  5. 服务器身份与连接信息\n'
  printf '  6. 用户管理\n'
  printf '  7. 备份文件管理\n'
  printf '  8. 管理证书与可信根\n'
  printf '  9. 查看服务器配置路径\n'
  printf ' 10. 查看运行日志\n'
  printf ' 11. 存储概览\n'
  printf '  0. 退出\n\n'
  printf '请选择：'
  read -r choice || break
  case "$choice" in
    1) init_server; pause ;;
    2) start_server; pause ;;
    3) stop_server; pause ;;
    4) banner; pause ;;
    5)
      identity_page
      printf '\n监听地址：%s:%s\n' "$(config_value bind)" "$(configured_port)"
      printf 'BPSEC2 身份文件：%s\n' "$(configured_cert | sed '/^$/d' | head -1)"
      pause
      ;;
    6) user_menu ;;
    7) snapshot_menu ;;
    8)
      certificate_page
      pause
      ;;
    9)
      clear_screen
      printf '服务器配置：%s\n数据目录：%s\n数据库：%s\n日志：%s\nPID 文件：%s\n' \
        "$CONFIG_FILE" "$DATA_ROOT" "$DB_PATH" "$LOG_FILE" "$PID_FILE"
      pause
      ;;
    10)
      logs_page
      pause
      ;;
    11)
      clear_screen
      admin overview
      pause
      ;;
    0) break ;;
    *) ;;
  esac
done

clear_screen
echo "已退出管理工具。"
