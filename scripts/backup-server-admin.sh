#!/usr/bin/env bash
#
# backup-server-admin.sh —— 自建服务器本机管理菜单。
#
# 用户和备份数据的操作调用共享管理工具；配置和证书更新使用停机锁与原子替换。
# 本脚本不直接修改数据库或备份文件。
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
for required_tool in ss flock; do
  command -v "$required_tool" >/dev/null 2>&1 || {
    printf '缺少 %s，无法安全管理实例；请安装 iproute2 与 util-linux。\n' "$required_tool" >&2
    exit 1
  }
done
config_value() {
  [ -f "$CONFIG_FILE" ] || return 0
  # Match launch-server.sh: strip comments, split only the first '=', and
  # let the last occurrence win. Never interpret a value as shell syntax.
  awk -v wanted="$1" '
    {
      line = $0
      sub(/#.*/, "", line)
      equals = index(line, "=")
      if (!equals) next
      key = substr(line, 1, equals - 1)
      value = substr(line, equals + 1)
      sub(/^[[:space:]]*/, "", key); sub(/[[:space:]]*$/, "", key)
      sub(/^[[:space:]]*/, "", value); sub(/[[:space:]]*$/, "", value)
      if (key == wanted) { result = value; found = 1 }
    }
    END { if (found) print result }
  ' "$CONFIG_FILE"
}
configured_port() { config_value port | head -1; }
configured_cert() { config_value certificate | head -1; }
configured_root_pub="${BACKUP_SERVER_ROOT_PUBLIC:-$SERVER_ROOT/private/root.key.pub}"
# Imported public materials live together in a versioned directory. Deriving
# the root from the committed certificate keeps the pair consistent on restart.
refresh_client_root() {
  local cert
  cert="$(configured_cert)"
  if [[ "$cert" == "$SERVER_ROOT"/state/client-identity.*/server.bpcert ]]; then
    configured_root_pub="$(dirname "$cert")/trusted-root.pub"
  fi
}
refresh_client_root
if [ -z "${BACKUP_SERVER_DATA:-}" ] && [ -n "$(config_value data_root)" ]; then
  DATA_ROOT="$(config_value data_root)"
fi
if [ -z "${BACKUP_SERVER_DB:-}" ] && [ -n "$(config_value database)" ]; then
  DB_PATH="$(config_value database)"
fi
if [ -z "${BACKUP_SERVER_PID_FILE:-}" ] && [ -n "$(config_value pid_file)" ]; then
  PID_FILE="$(config_value pid_file)"
fi
refresh_runtime_config() {
  local configured_log
  refresh_client_root
  configured_log="$(config_value log_file)"
  [ -z "$configured_log" ] || LOG_FILE="$configured_log"
}
refresh_runtime_config

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

# 0 = listening, 1 = free, 2 = cannot determine.
port_listener_state() {
  local port="$1" listeners
  case "$port" in ''|*[!0-9]*) return 2 ;; esac
  [ "$port" -ge 1 ] && [ "$port" -le 65535 ] || return 2
  listeners="$(ss -ltnH "sport = :$port" 2>/dev/null)" || return 2
  [ -n "$listeners" ]
}

port_listener_exists() {
  local state=0
  port_listener_state "$1" || state=$?
  if [ "$state" = 2 ]; then
    printf '无法检查端口 %s 的监听状态，按已占用处理。\n' "$1" >&2
  fi
  [ "$state" != 1 ]
}

pid_starttime() {
  local pid="$1" status rest
  local -a fields=()
  IFS= read -r status <"/proc/$pid/stat" 2>/dev/null || return 1
  rest="${status##*) }"
  read -r -a fields <<<"$rest"
  [ -n "${fields[19]:-}" ] || return 1
  printf '%s\n' "${fields[19]}"
}

pid_is_ours() {
  local pid="$1" expected_exe actual_exe before index
  local roots=0 databases=0 pid_files=0
  local -a args=()
  case "$pid" in ''|*[!0-9]*) return 1 ;; esac
  [ "$pid" -gt 1 ] 2>/dev/null || return 1
  kill -0 "$pid" 2>/dev/null || return 1
  before="$(pid_starttime "$pid")" || return 1
  expected_exe="$(readlink -f "$SERVER_BIN")" || return 1
  actual_exe="$(readlink "/proc/$pid/exe")" || return 1
  # dpkg can replace the payload while the old executable is still running.
  # Its original absolute path must still be exactly the configured server.
  case "$actual_exe" in "$expected_exe"|"$expected_exe (deleted)") ;; *) return 1 ;; esac
  mapfile -d '' -t args <"/proc/$pid/cmdline" 2>/dev/null || return 1
  for ((index=1; index<${#args[@]}; index++)); do
    case "${args[$index]}" in
      --root)
        index=$((index + 1))
        [ "${args[$index]:-}" = "$DATA_ROOT" ] || return 1
        roots=$((roots + 1)) ;;
      --db)
        index=$((index + 1))
        [ "${args[$index]:-}" = "$DB_PATH" ] || return 1
        databases=$((databases + 1)) ;;
      --pid-file)
        index=$((index + 1))
        [ "${args[$index]:-}" = "$PID_FILE" ] || return 1
        pid_files=$((pid_files + 1)) ;;
    esac
  done
  [ "$roots" = 1 ] && [ "$databases" = 1 ] && [ "$pid_files" = 1 ] &&
    [ "$before" = "$(pid_starttime "$pid")" ]
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
  instance_ready || { echo '服务器尚未初始化：请先选择“初始化服务器”。'; return 1; }
  if [ -n "${BACKUP_SERVER_SERVICE:-}" ]; then
    installed_instance_matches || return 1
    if [ "$(id -u)" = 0 ]; then
      command -v runuser >/dev/null 2>&1 || { printf '缺少 runuser，无法切换到安装版服务用户。\n'; return 1; }
      runuser -u backup-project -- "$ADMIN_BIN" --server-root "$SERVER_ROOT" \
        --root "$DATA_ROOT" --db "$DB_PATH" "$@"
      return "$?"
    fi
  fi
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
    print_client_connection_info
  else
    printf '状态：尚未初始化\n'
  fi
  if instance_initialized || instance_ready; then
    local listener_state=0
    port_listener_state "$(configured_port)" || listener_state=$?
    if pid="$(owned_pid 2>/dev/null)"; then
      if [ "$listener_state" = 0 ]; then
        printf '进程状态：运行中（PID %s，业务端口已监听）\n' "$pid"
      else
        printf '进程状态：已确认实例进程（PID %s），业务端口监听尚未确认\n' "$pid"
      fi
    elif [ "$listener_state" = 0 ]; then
      printf '进程状态：端口 %s 已监听，但进程不属于当前实例管理器\n' "$(configured_port)"
    elif [ "$listener_state" = 2 ]; then
      printf '进程状态：无法核实业务端口监听，请检查 ss 与系统权限。\n'
    else
      printf '进程状态：已初始化，当前未运行\n'
    fi
  fi
  printf '\n'
}

print_client_connection_info() {
  local bind port cert key
  bind="$(config_value bind)"; [ -n "$bind" ] || bind=127.0.0.1
  port="$(configured_port)"
  printf '服务器地址：%s\n业务端口：%s\n' "$bind" "$port"
  cert="$(configured_cert)"
  if [ -n "$cert" ]; then
    if [ "$(config_value require_bpsec2)" = true ]; then
      printf '认证模式：BPSEC2（仅接受证书身份）\n'
    else
      printf '认证模式：BPSEC1 指纹 + 可选 BPSEC2 证书\n'
    fi
    printf '客户端可信根公钥：%s\n服务器身份文件：%s\n' "$configured_root_pub" "$cert"
    return 0
  fi
  key="$(config_value transport_key)"
  printf '认证模式：BPSEC1 公钥指纹\n服务器身份私钥：%s（私钥内容不会显示）\n' "$key"
  if [ -x "$KEYGEN_BIN" ] && [ -n "$key" ] && [ -f "$key" ]; then
    "$KEYGEN_BIN" --show --key-file "$key" || return 1
  else
    printf '传输身份指纹：不可用（找不到已配置的身份私钥）\n'
  fi
}

# server.conf has no quoting syntax: comments and trailing whitespace cannot
# represent literal path bytes. Reject them before creating identity material.
initialization_path_supported() {
  local path="$1"
  case "$path" in /*) ;; *) return 1 ;; esac
  case "$path" in *'#'*|*$'\r'*|*$'\n'*) return 1 ;; esac
  [[ ! "$path" =~ [[:space:]]$ ]]
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
  local secret_file="$SERVER_ROOT/secrets.env" key_file="$SERVER_ROOT/state/transport.key"
  local root_key="$SERVER_ROOT/private/root.key" cert_path="$SERVER_ROOT/state/server.bpcert"
  local path
  for path in "$SERVER_ROOT" "$DATA_ROOT" "$DB_PATH" "$PID_FILE" "$LOG_FILE" \
      "$CONFIG_FILE" "$configured_root_pub"; do
    initialization_path_supported "$path" || {
      printf '实例路径必须是绝对路径，且不能包含 #、换行或结尾空白；尚未创建身份文件。\n'
      return 1
    }
  done
  printf '监听地址 [127.0.0.1]：'; read -r bind || return 0; [ -n "$bind" ] || bind=127.0.0.1
  printf '业务端口 [18765]：'; read -r port || return 0; [ -n "$port" ] || port=18765
  if [ "$bind" != "127.0.0.1" ]; then
    printf '当前初始化向导仅配置 127.0.0.1 回环监听；其他机器可使用 SSH 隧道，未生成配置。\n'
    return 1
  fi
  case "$port" in ''|*[!0-9]*) printf '端口必须是 1 到 65535 的整数。\n'; return 1 ;; esac
  if [ "$port" -lt 1 ] || [ "$port" -gt 65535 ] || port_listener_exists "$port"; then
    printf '端口无效或已被占用：%s\n' "$port"
    return 1
  fi
  printf '是否初始化 BPSEC2 证书身份（y/N）：'; read -r use_cert || return 0
  [ -x "$KEYGEN_BIN" ] || { printf '找不到 backup-server-keygen：%s\n' "$KEYGEN_BIN"; return 1; }
  [ -x "$LAUNCHER" ] || { printf '找不到安全启动器：%s\n' "$LAUNCHER"; return 1; }
  if [[ "$use_cert" =~ ^[Yy]$ ]]; then
    [ -x "$CERT_TOOL" ] || { printf '找不到 backup-cert-tool：%s\n' "$CERT_TOOL"; return 1; }
    printf 'server_id [self-hosted-%s]：' "$port"; read -r server_id || return 0
    [ -n "$server_id" ] || server_id="self-hosted-$port"
  fi
  # The persistent lock inode is never removed. A second initializer must not
  # bypass the first by opening a newly created file at the same pathname.
  (
    umask 077
    mkdir -p "$SERVER_ROOT" || exit 1
    local init_lock="$SERVER_ROOT/.backup-server-initialize.lock" init_fd
    [ ! -L "$init_lock" ] && { [ ! -e "$init_lock" ] || [ -f "$init_lock" ]; } || {
      printf '初始化锁不是普通文件，操作已拒绝。\n'; exit 1;
    }
    if [ ! -e "$init_lock" ]; then
      (set -o noclobber; : >"$init_lock") 2>/dev/null || true
    fi
    [ ! -L "$init_lock" ] && [ -f "$init_lock" ] &&
      [ "$(stat -c %u "$init_lock")" = "$(id -u)" ] &&
      [ "$(stat -c %h "$init_lock")" = 1 ] || {
      printf '初始化锁的类型或属主无效，操作已拒绝。\n'; exit 1;
    }
    exec {init_fd}>>"$init_lock" || exit 1
    [ ! -L "$init_lock" ] &&
      [ "$(stat -Lc '%d:%i' "/dev/fd/$init_fd")" = "$(stat -Lc '%d:%i' "$init_lock")" ] || exit 1
    flock -n "$init_fd" || { printf '另一初始化操作正在运行，请稍后重试。\n'; exit 1; }
    # Recheck after obtaining the lock; ln below also refuses any concurrent
    # external creation without replacing a single existing identity file.
    for path in "$DB_PATH" "$CONFIG_FILE" "$key_file" "$secret_file" \
        "$root_key" "${root_key}.pub" "$configured_root_pub" "$cert_path"; do
      if [ -e "$path" ] || [ -L "$path" ]; then
        printf '目标目录已有数据库、配置或身份材料，初始化已拒绝，不会覆盖。\n'; exit 1
      fi
    done
    if [ -d "$DATA_ROOT" ]; then
      local existing_data
      existing_data="$(find "$DATA_ROOT" -mindepth 1 -maxdepth 1 -print -quit)" || exit 1
      [ -z "$existing_data" ] || { printf '目标目录已有备份文件，初始化已拒绝。\n'; exit 1; }
    fi
    local init_dir keygen_output pubkey certificate='' require_bpsec2=false
    local -a published_paths=() published_refs=() temporary_files=()
    init_dir="$(mktemp -d "$SERVER_ROOT/.server-init.XXXXXXXX")" || exit 1
    cleanup_initialization() {
      local result="$?" index rollback_failed=0
      trap - EXIT INT TERM
      if [ "$result" != 0 ]; then
        for ((index=${#published_paths[@]}-1; index>=0; index--)); do
          # Only unlink the exact inode created by this transaction.
          if [ "${published_paths[$index]}" -ef "${published_refs[$index]}" ]; then
            rm -f -- "${published_paths[$index]}" || rollback_failed=1
          fi
        done
        if [ "$rollback_failed" = 0 ]; then
          printf '初始化未完成，本次创建的身份与配置已撤回，可修正问题后重试。\n' >&2
        else
          printf '初始化未完成，部分文件无法撤回；请检查目录权限，现有材料不会自动覆盖。\n' >&2
        fi
      fi
      for path in "${temporary_files[@]}"; do rm -f -- "$path"; done
      rm -rf -- "$init_dir"
      exit "$result"
    }
    trap cleanup_initialization EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM
    publish_initialization_file() {
      local source="$1" target="$2" staged
      mkdir -p "$(dirname "$target")" || return 1
      staged="$(mktemp "$(dirname "$target")/.server-init-file.XXXXXXXX")" || return 1
      temporary_files+=("$staged")
      cp -p -- "$source" "$staged" || return 1
      # A same-directory hard link is atomic, works across source filesystems,
      # and unlike mv never overwrites an identity created after the recheck.
      published_paths+=("$target")
      published_refs+=("$staged")
      ln -- "$staged" "$target" || return 1
    }
    mkdir -p "$init_dir/state" "$init_dir/private" || exit 1
    keygen_output="$("$KEYGEN_BIN" --output "$init_dir/state/transport.key")" || exit 1
    pubkey="$(printf '%s\n' "$keygen_output" | sed -n 's/.*公钥 (hex): //p' | head -1)"
    [ -n "$pubkey" ] || { printf '无法从 keygen 结果取得公钥，初始化停止。\n'; exit 1; }
    if [[ "$use_cert" =~ ^[Yy]$ ]]; then
      "$CERT_TOOL" root-init --root-key "$init_dir/private/root.key" --root-id "${server_id}-root" >/dev/null || exit 1
      "$CERT_TOOL" issue-server --root-key "$init_dir/private/root.key" --server-id "$server_id" \
        --server-pubkey "hex:$pubkey" --out "$init_dir/state/server.bpcert" >/dev/null || exit 1
      "$CERT_TOOL" verify-server --cert "$init_dir/state/server.bpcert" \
        --roots "$init_dir/private/root.key.pub" >/dev/null || exit 1
      certificate="$cert_path"
      require_bpsec2=true
    fi
    head -c 32 /dev/urandom | sha256sum | awk '{print "BACKUP_TOKEN_SECRET="$1}' > "$init_dir/secrets.env" || exit 1
    chmod 600 "$init_dir/secrets.env" || exit 1
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
    chmod 600 "$init_dir/server.conf" || exit 1
    mkdir -p "$DATA_ROOT" "$SERVER_ROOT/state" "$SERVER_ROOT/logs" "$SERVER_ROOT/private" || exit 1
    publish_initialization_file "$init_dir/state/transport.key" "$key_file" || exit 1
    publish_initialization_file "$init_dir/secrets.env" "$secret_file" || exit 1
    if [[ "$use_cert" =~ ^[Yy]$ ]]; then
      publish_initialization_file "$init_dir/private/root.key" "$root_key" || exit 1
      publish_initialization_file "$init_dir/private/root.key.pub" "$configured_root_pub" || exit 1
      publish_initialization_file "$init_dir/state/server.bpcert" "$cert_path" || exit 1
    fi
    BACKUP_SERVER_BIN="$SERVER_BIN" BACKUP_CERT_TOOL="$CERT_TOOL" \
      "$LAUNCHER" --check-config --config "$init_dir/server.conf" || exit 1
    # The configuration is the commit point: no launcher can see a new instance
    # until every identity file and the shared configuration check have passed.
    publish_initialization_file "$init_dir/server.conf" "$CONFIG_FILE" || exit 1
  ) || return 1
  printf '\n初始化完成。之后可选择启动服务器。\n'
  print_client_connection_info || return 1
}

# Installed instances belong to one fixed systemd unit, never a root-run child.
installed_instance_matches() {
  [ "${BACKUP_SERVER_SERVICE:-}" = backup-project-server ] &&
    [ "$SERVER_ROOT" = /var/lib/backup-project-server ] &&
    [ "$CONFIG_FILE" = /etc/backup-project-server/server.conf ] &&
    [ "$DATA_ROOT" = /var/lib/backup-project-server/data ] &&
    [ "$DB_PATH" = /var/lib/backup-project-server/state/metadata.sqlite3 ] &&
    [ "$PID_FILE" = /run/backup-project-server/backup-server.pid ] || {
      printf '安装版服务与实例路径不匹配，操作已拒绝。\n'; return 1;
    }
}

service_manager_action() {
  local action="$1"
  installed_instance_matches || return 1
  if [ ! -d /run/systemd/system ] || ! command -v systemctl >/dev/null 2>&1; then
    printf '当前环境没有可用的 systemd；请使用服务器便携包的 ./backup-server-manager 管理独立实例。\n'
    return 1
  fi
  case "$action" in start|stop) ;; *) return 1 ;; esac
  systemctl "$action" backup-project-server.service
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
  if [ -n "${BACKUP_SERVER_SERVICE:-}" ]; then
    service_manager_action start || return 1
    for _ in $(seq 1 20); do
      if pid="$(owned_pid 2>/dev/null)" && port_listener_state "$(configured_port)"; then
        printf 'systemd 服务器已启动（PID %s）。\n' "$pid"; return 0
      fi
      sleep 0.1
    done
    printf 'systemd 已返回，但尚未确认当前实例的进程与监听，请查看状态和日志。\n'
    return 1
  fi
  mkdir -p "$(dirname "$LOG_FILE")" "$(dirname "$PID_FILE")"
  [ -x "$LAUNCHER" ] || { printf '找不到安全启动器：%s\n' "$LAUNCHER"; return 1; }
  BACKUP_SERVER_BIN="$SERVER_BIN" BACKUP_CERT_TOOL="$CERT_TOOL" \
    BACKUP_SERVER_CONF="$CONFIG_FILE" "$LAUNCHER" --config "$CONFIG_FILE" >>"$LOG_FILE" 2>&1 &
  sleep 0.2
  if pid="$(owned_pid 2>/dev/null)" && port_listener_state "$(configured_port)"; then printf '服务器已启动（PID %s）。\n' "$pid"; else printf '服务器未能启动，请查看日志：%s\n' "$LOG_FILE"; return 1; fi
}

stop_server() {
  local pid start
  if [ -n "${BACKUP_SERVER_SERVICE:-}" ]; then
    service_manager_action stop || return 1
    if owned_pid >/dev/null 2>&1; then
      printf 'systemd 已返回，但当前实例进程尚未退出，请查看服务状态。\n'; return 1
    fi
    printf 'systemd 服务器已停止。\n'; return 0
  fi
  pid="$(owned_pid 2>/dev/null || true)"
  [ -n "$pid" ] || { printf '没有找到属于当前实例的运行进程。\n'; return 0; }
  start="$(pid_starttime "$pid")" || return 1
  pid_is_ours "$pid" && [ "$start" = "$(pid_starttime "$pid")" ] || {
    printf '进程身份已改变，拒绝发送停止信号。\n'; return 1;
  }
  kill -TERM "$pid" 2>/dev/null || { printf '停止信号发送失败。\n'; return 1; }
  for _ in $(seq 1 50); do
    if ! kill -0 "$pid" 2>/dev/null || [ "$start" != "$(pid_starttime "$pid" 2>/dev/null)" ]; then
      printf '服务器已停止。\n'; return 0
    fi
    sleep 0.1
  done
  printf '服务器未在 5 秒内退出，未向其他进程发送信号。\n'; return 1
}

logs_page() {
  clear_screen
  if [ -n "${BACKUP_SERVER_SERVICE:-}" ] && [ -z "$(config_value log_file)" ]; then
    installed_instance_matches || return 1
    command -v journalctl >/dev/null 2>&1 || { printf '缺少 journalctl，无法查看系统服务日志。\n'; return 1; }
    journalctl -u backup-project-server.service --no-pager -n 80
  else
    [ -f "$LOG_FILE" ] && tail -80 "$LOG_FILE" || printf '日志尚不存在：%s\n' "$LOG_FILE"
  fi
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
      check_certificate_pair "$cert" "$configured_root_pub" || true
    fi
  else
    printf 'BPSEC2：不适用（当前配置未启用服务器身份证书）。\n'
  fi
}

# Hold the same kernel lock as backup-server until the operation finishes.
# PID files alone cannot detect a service started outside this menu.
with_stopped_instance() (
  local lock_file manager_lock_fd lock_created=0 data_owner
  instance_initialized || { printf '请先初始化服务器。\n'; return 1; }
  if owned_pid >/dev/null 2>&1 || port_listener_exists "$(configured_port)"; then
    printf '请先停止服务器，再修改配置或证书。\n'; return 1
  fi
  command -v flock >/dev/null 2>&1 || { printf '缺少 flock，无法安全确认停机状态。\n'; return 1; }
  lock_file="$DATA_ROOT/.backup-server.lock"
  if [ -L "$lock_file" ] || { [ -e "$lock_file" ] && [ ! -f "$lock_file" ]; }; then
    printf '数据目录锁不是普通文件，操作已拒绝。\n'; return 1
  fi
  umask 077
  if [ ! -e "$lock_file" ]; then
    # noclobber distinguishes our new lock from one created concurrently.
    if (set -o noclobber; : >"$lock_file") 2>/dev/null; then lock_created=1; fi
  fi
  [ ! -L "$lock_file" ] && [ -f "$lock_file" ] || return 1
  data_owner="$(stat -c '%u:%g' "$DATA_ROOT")" || return 1
  if [ "$lock_created" = 1 ] && [ "$(stat -c '%u:%g' "$lock_file")" != "$data_owner" ]; then
    [ "$(id -u)" = 0 ] && chown --no-dereference --reference="$DATA_ROOT" "$lock_file" || {
      printf '无法让新锁文件归属实例数据用户，操作已拒绝。\n'; return 1;
    }
  fi
  [ ! -L "$lock_file" ] && [ -f "$lock_file" ] &&
    [ "$(stat -c '%u:%g' "$lock_file")" = "$data_owner" ] &&
    [ "$(stat -c '%h' "$lock_file")" = 1 ] || {
      printf '已有锁文件的属主或类型与数据目录不匹配，操作已拒绝，未修改其属主。\n'; return 1;
    }
  exec {manager_lock_fd}>>"$lock_file" || return 1
  # Opening in append mode writes nothing. Check the opened inode as well, so
  # replacement of the pathname cannot make us lock a different file.
  [ ! -L "$lock_file" ] &&
    [ "$(stat -Lc '%d:%i' "/dev/fd/$manager_lock_fd")" = "$(stat -Lc '%d:%i' "$lock_file")" ] || return 1
  flock -n "$manager_lock_fd" || { printf '实例仍在运行或另一管理操作持有数据目录锁，操作已拒绝。\n'; return 1; }
  "$@"
)

# Preserve unrelated keys and their formatting. Values are data, never shell.
write_config_candidate() {
  local output="$1" line key selected match
  shift
  local -a updates=("$@")
  cp -p "$CONFIG_FILE" "$output" || return 1
  {
    while IFS= read -r line || [ -n "$line" ]; do
      key="${line%%=*}"
      key="${key#"${key%%[![:space:]]*}"}"
      key="${key%"${key##*[![:space:]]}"}"
      match=0
      for ((selected=0; selected<${#updates[@]}; selected+=2)); do
        [ "$key" != "${updates[$selected]}" ] || match=1
      done
      [ "$match" = 1 ] || printf '%s\n' "$line"
    done <"$CONFIG_FILE"
    for ((selected=0; selected<${#updates[@]}; selected+=2)); do
      printf '%s = %s\n' "${updates[$selected]}" "${updates[$((selected + 1))]}"
    done
  } >"$output"
}

commit_config_candidate() {
  local candidate="$1" history
  BACKUP_SERVER_BIN="$SERVER_BIN" BACKUP_CERT_TOOL="$CERT_TOOL" \
    "$LAUNCHER" --check-config --config "$candidate" || return 1
  history="$(mktemp -d "$(dirname "$CONFIG_FILE")/.server-config-backup.XXXXXXXX")" || return 1
  chmod 700 "$history"
  cp -p "$CONFIG_FILE" "$history/server.conf" || return 1
  mv -f "$candidate" "$CONFIG_FILE" || return 1
  printf '配置已保存；旧配置保留于：%s/server.conf\n' "$history"
}

check_certificate_pair() {
  local certificate="$1" roots="$2" cert_public key_public
  "$CERT_TOOL" verify-server --cert "$certificate" --roots "$roots" || return 1
  cert_public="$("$CERT_TOOL" inspect-server --cert "$certificate" |
    sed -n 's/^server_public_key[[:space:]]*=[[:space:]]*//p')" || return 1
  key_public="$("$KEYGEN_BIN" --show --key-file "$(config_value transport_key)" |
    sed -n 's/.*公钥 (hex): //p')" || return 1
  [ -n "$key_public" ] && [ "$cert_public" = "$key_public" ] || {
    printf '证书公钥与当前服务器传输身份不匹配，操作已拒绝。\n'; return 1;
  }
}

import_certificate() (
  local roots certificate confirmation bundle candidate=''
  [ -x "$CERT_TOOL" ] && [ -x "$KEYGEN_BIN" ] || return 1
  printf '已有可信根公钥文件（绝对路径，不要提供根私钥）：'; read -r roots || return 1
  printf '已有服务器身份文件（绝对路径）：'; read -r certificate || return 1
  case "$roots:$certificate" in *$'\n'*|*$'\r'*) return 1 ;; esac
  [[ "$roots" == /* && "$certificate" == /* ]] &&
    [ -f "$roots" ] && [ -r "$roots" ] && [ -f "$certificate" ] && [ -r "$certificate" ] || {
    printf '必须提供可读文件的绝对路径。\n'; return 1;
  }
  [ "$(stat -c %s "$roots")" -le 65536 ] && [ "$(stat -c %s "$certificate")" -le 4096 ] || {
    printf '根公钥文件不得超过 65536 字节，证书不得超过 4096 字节。\n'; return 1;
  }
  mkdir -p "$SERVER_ROOT/state" || return 1
  bundle="$(mktemp -d "$SERVER_ROOT/state/client-identity.XXXXXXXX")" || return 1
  trap 'rm -rf "$bundle"; [ -z "$candidate" ] || rm -f "$candidate"' EXIT
  # Discard comments so even an accidental secret in a comment is not shared.
  head -c 65537 "$roots" >"$bundle/root-input" || return 1
  head -c 4097 "$certificate" >"$bundle/server.bpcert" || return 1
  [ "$(stat -c %s "$bundle/root-input")" -le 65536 ] &&
    [ "$(stat -c %s "$bundle/server.bpcert")" -le 4096 ] || return 1
  awk '{ sub(/#.*/, ""); if ($0 ~ /[^[:space:]]/) print }' "$bundle/root-input" >"$bundle/trusted-root.pub" || return 1
  rm -f "$bundle/root-input"
  chmod 755 "$bundle" && chmod 644 "$bundle/trusted-root.pub" "$bundle/server.bpcert" || return 1
  check_certificate_pair "$bundle/server.bpcert" "$bundle/trusted-root.pub" || return 1
  printf '根公钥文件 SHA-256：'
  sha256sum "$bundle/trusted-root.pub" | cut -d' ' -f1
  printf '请通过可信渠道核对以上根与服务器身份。导入会启用仅 BPSEC2 认证，不更换传输私钥。\n'
  printf '确认信任并导入请输入 IMPORT BPSEC2：'; read -r confirmation || return 1
  [ "$confirmation" = 'IMPORT BPSEC2' ] || { printf '已取消，原配置与材料未改变。\n'; return 1; }
  candidate="$(mktemp "$(dirname "$CONFIG_FILE")/.server.conf.XXXXXXXX")" || return 1
  write_config_candidate "$candidate" certificate "$bundle/server.bpcert" require_bpsec2 true || return 1
  commit_config_candidate "$candidate" || return 1
  candidate=''
  configured_root_pub="$bundle/trusted-root.pub"
  printf '证书导入完成。旧证书、根公钥和所有私钥均保留。\n'
  printf '客户端可信根公钥：%s\n服务器身份文件：%s\n' "$configured_root_pub" "$bundle/server.bpcert"
  bundle=''
  trap - EXIT
)

certificate_menu() {
  certificate_page
  printf '\n  1. 安全导入已有可信根公钥与服务器证书（需停机）\n  回车返回\n请选择：'
  local choice
  read -r choice || return 0
  case "$choice" in 1) with_stopped_instance import_certificate; pause ;; esac
}

edit_configuration() (
  local port workers io_timeout log_path mode confirmation candidate=''
  local require_bpsec2 cert
  printf '业务端口 [%s]：' "$(configured_port)"; read -r port || return 1
  [ -n "$port" ] || port="$(configured_port)"
  printf '工作线程数 [%s]：' "$(config_value workers)"; read -r workers || return 1
  [ -n "$workers" ] || workers="$(config_value workers)"
  printf 'I/O 超时秒数 [%s]：' "$(config_value io_timeout)"; read -r io_timeout || return 1
  [ -n "$io_timeout" ] || io_timeout="$(config_value io_timeout)"
  printf '日志文件绝对路径 [%s]：' "$(config_value log_file)"; read -r log_path || return 1
  [ -n "$log_path" ] || log_path="$(config_value log_file)"
  [[ "$port" =~ ^[0-9]{1,5}$ && "$workers" =~ ^[0-9]{1,2}$ && "$io_timeout" =~ ^[0-9]{1,4}$ ]] || {
    printf '端口、线程数和超时必须是有效范围内的整数。\n'; return 1;
  }
  if [ -n "$log_path" ]; then
    case "$log_path" in
      /*) ;;
      *) printf '日志路径必须是绝对路径。\n'; return 1 ;;
    esac
    case "$log_path" in *$'\n'*|*$'\r'*|*'#'*|*'='*|*'$('*|*'`'*|*';'*|*'|'*|*'&'*|*'<'*|*'>'*)
      printf '日志路径含不支持的配置字符，未保存。\n'; return 1 ;;
    esac
  fi
  if [ -n "$log_path" ]; then
    local resolved_log protected
    resolved_log="$(realpath -m "$log_path")" || return 1
    case "$resolved_log" in
      "$DATA_ROOT"|"$DATA_ROOT"/*|"$SERVER_ROOT/private"|"$SERVER_ROOT/private"/*)
        printf '日志不能写入备份数据或私钥目录。\n'; return 1 ;;
    esac
    for protected in "$CONFIG_FILE" "$DB_PATH" "$PID_FILE" "$configured_root_pub" \
        "$(config_value secret_file)" "$(config_value transport_key)" "$(configured_cert)"; do
      [ -n "$protected" ] || continue
      if [ "$resolved_log" = "$(realpath -m "$protected")" ] || [ "$log_path" -ef "$protected" ]; then
        printf '日志路径不能覆盖配置、数据库或身份文件。\n'; return 1
      fi
    done
  fi
  if [ "$port" != "$(configured_port)" ] && port_listener_exists "$port"; then
    printf '新端口已被占用，未保存。\n'; return 1
  fi
  require_bpsec2="$(config_value require_bpsec2)"
  cert="$(configured_cert)"
  printf '认证：回车保留；1 允许 BPSEC1（保留已有证书）；2 仅 BPSEC2：'; read -r mode || return 1
  case "$mode" in
    '') ;;
    1)
      case "$(config_value bind)" in
        127.0.0.1|::1|localhost) ;;
        *) printf '公网监听不允许在菜单中降级为 BPSEC1。\n'; return 1 ;;
      esac
      printf '允许 BPSEC1 后客户端可用指纹认证。确认请输入 ALLOW BPSEC1：'; read -r confirmation || return 1
      [ "$confirmation" = 'ALLOW BPSEC1' ] || { printf '已取消。\n'; return 1; }
      require_bpsec2=false
      ;;
    2)
      [ -n "$cert" ] && [ -f "$configured_root_pub" ] || {
        printf '请先通过证书菜单安全导入 BPSEC2 材料。\n'; return 1;
      }
      check_certificate_pair "$cert" "$configured_root_pub" || return 1
      require_bpsec2=true
      ;;
    *) printf '认证选项无效，未保存。\n'; return 1 ;;
  esac
  candidate="$(mktemp "$(dirname "$CONFIG_FILE")/.server.conf.XXXXXXXX")" || return 1
  trap '[ -z "$candidate" ] || rm -f "$candidate"' EXIT
  write_config_candidate "$candidate" port "$port" workers "$workers" io_timeout "$io_timeout" \
    log_file "$log_path" require_bpsec2 "$require_bpsec2" || return 1
  printf '确认保存以上设置请输入 SAVE：'; read -r confirmation || return 1
  [ "$confirmation" = SAVE ] || { printf '已取消，原配置未改变。\n'; return 1; }
  commit_config_candidate "$candidate" || return 1
  candidate=''
  trap - EXIT
)

configuration_menu() {
  clear_screen
  printf '服务器配置：%s\n数据目录：%s\n数据库：%s\n日志：%s\nPID 文件：%s\n' \
    "$CONFIG_FILE" "$DATA_ROOT" "$DB_PATH" "$LOG_FILE" "$PID_FILE"
  printf '\n  1. 修改端口、线程数、超时、日志与认证方式（需停机）\n  回车返回\n请选择：'
  local choice
  read -r choice || return 0
  case "$choice" in
    1)
      with_stopped_instance edit_configuration
      # The edit runs under a subshell lock; refresh values in the menu process.
      refresh_runtime_config
      pause
      ;;
  esac
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
  if ! instance_initialized; then
    printf '请先初始化服务器。\n'
    return 1
  fi
  if [ -n "$(configured_cert)" ]; then
    certificate_page
  else
    print_client_connection_info
  fi
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
  printf '  9. 查看与修改服务器配置\n'
  printf ' 10. 查看运行日志\n'
  printf ' 11. 存储概览\n'
  printf '  0. 退出\n\n'
  printf '请选择：'
  if ! read -r choice; then
    # A closed stdin is fine for an already configured instance, but an
    # uninitialized instance must not look like a successful no-op. This
    # keeps automation and service wrappers fail-closed without blocking the
    # interactive initialization path.
    if ! instance_initialized && ! instance_ready; then
      printf '%s\n' '数据目录不存在：未初始化的服务器需要通过菜单完成初始化。' >&2
      exit 1
    fi
    break
  fi
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
      certificate_menu
      refresh_runtime_config
      ;;
    9) configuration_menu ;;
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
