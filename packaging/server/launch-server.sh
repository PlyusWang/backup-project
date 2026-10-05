#!/usr/bin/env bash
# launch-server.sh —— 把 server.conf 翻译成 backup-server 的命令行参数。
#
# 它**不实现产品语义**：每个键一对一映射到 server/main.cpp 里已有的参数
# （见 backup-server --help）。未知键、缺字段、公网绑定却缺少安全前提，
# 全部在这里明确失败 —— 这层只做"配置 -> argv"，不做任何业务判断。
#
# 用法：
#   backup-project-server [--config <文件>] [其它 backup-server 参数...]
#   backup-project-server --check-config [--config <文件>]
#   backup-project-server --print-command [--config <文件>]
#
# 解析规则（刻意很小）：
#   * 每行 "key = value"，# 之后是注释；
#   * 键必须在白名单里，未知键直接报错（不忽略、不猜测、不"尽力而为"）；
#   * 值不做任何展开：不 eval、不 source、不跑 $( ) 或反引号；
#   * 传给 backup-server 的每个参数都是数组里的一个元素，从不拼成 shell 字符串。
#
# 退出码：0 正常 / 2 配置错误 / 1 环境或文件错误。

set -Eeuo pipefail

SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVER_BIN="${BACKUP_SERVER_BIN:-$SELF_DIR/backup-server}"
CERT_TOOL="${BACKUP_CERT_TOOL:-$SELF_DIR/backup-cert-tool}"
DEFAULT_CONF="${BACKUP_SERVER_CONF:-/etc/backup-project-server/server.conf}"

MODE=run
CONF="$DEFAULT_CONF"
EXTRA=()

usage() {
  sed -n '2,20p' "$0"
}

cfg_error() { printf '[config] ERROR: %s\n' "$*" >&2; exit 2; }
env_error() { printf '[config] ERROR: %s\n' "$*" >&2; exit 1; }
note()      { printf '[config] %s\n' "$*"; }

while [ $# -gt 0 ]; do
  case "$1" in
    --config)
      [ $# -ge 2 ] || cfg_error "--config 需要一个文件参数"
      CONF="$2"; shift 2 ;;
    --check-config) MODE=check; shift ;;
    --print-command) MODE=print; shift ;;
    -h|--help) usage; exit 0 ;;
    *) EXTRA+=("$1"); shift ;;
  esac
done

[ -f "$CONF" ] || env_error "找不到配置文件：$CONF"
[ -r "$CONF" ] || env_error "配置文件不可读：$CONF"

# ---- 1. 严格解析 ----
CONFIG_KEYS=" bind port data_root database secret_file transport_key certificate require_bpsec2 allow_public_bind public_bind_reason allow_public_bind_insecure log_file pid_file workers io_timeout max_upload_bytes max_login_failures login_lockout_seconds quiet "

declare -A CFG=()
line_no=0
while IFS= read -r raw || [ -n "$raw" ]; do
  line_no=$((line_no + 1))
  line="${raw%%#*}"
  line="${line#"${line%%[![:space:]]*}"}"
  line="${line%"${line##*[![:space:]]}"}"
  [ -n "$line" ] || continue
  case "$line" in
    *=*) ;;
    *) cfg_error "$CONF:$line_no: 不是 \"key = value\" 形式：$line" ;;
  esac
  key="${line%%=*}"
  value="${line#*=}"
  key="${key#"${key%%[![:space:]]*}"}"
  key="${key%"${key##*[![:space:]]}"}"
  value="${value#"${value%%[![:space:]]*}"}"
  value="${value%"${value##*[![:space:]]}"}"
  case "$key" in
    *[!a-z0-9_]*) cfg_error "$CONF:$line_no: 键名只允许小写字母 / 数字 / 下划线：'$key'" ;;
  esac
  case "$CONFIG_KEYS" in
    *" $key "*) ;;
    *) cfg_error "$CONF:$line_no: 未知配置键 '$key'（允许的键见 docs/install-server.md）" ;;
  esac
  CFG[$key]="$value"
done < "$CONF"

get() { printf '%s' "${CFG[$1]:-}"; }

bool_of() {
  local value
  value="$(get "$1")"
  [ -n "$value" ] || value="$2"
  case "$value" in
    true|yes|1|on)   printf 'true' ;;
    false|no|0|off)  printf 'false' ;;
    *) cfg_error "配置键 $1 只接受 true / false（收到 '$value'）" ;;
  esac
}

num_of() {
  local value
  value="$(get "$1")"
  [ -n "$value" ] || value="$2"
  case "$value" in
    ''|*[!0-9]*) cfg_error "配置键 $1 必须是十进制数字（收到 '$value'）" ;;
  esac
  if [ "$value" -lt "$3" ] || [ "$value" -gt "$4" ]; then
    cfg_error "配置键 $1 必须在 $3..$4 之间（收到 '$value'）"
  fi
  printf '%s' "$value"
}

for required in data_root database secret_file transport_key; do
  [ -n "$(get "$required")" ] || cfg_error "缺少必填键：$required（$CONF）"
done

BIND="$(get bind)";            [ -n "$BIND" ] || BIND=127.0.0.1
PORT="$(num_of port 18765 1 65535)"
DATA_ROOT="$(get data_root)"
DATABASE="$(get database)"
SECRET_FILE="$(get secret_file)"
TRANSPORT_KEY="$(get transport_key)"
CERTIFICATE="$(get certificate)"
LOG_FILE="$(get log_file)"
PID_FILE="$(get pid_file)";    [ -n "$PID_FILE" ] || PID_FILE=/run/backup-project-server/backup-server.pid
WORKERS="$(num_of workers 4 1 64)"
IO_TIMEOUT="$(num_of io_timeout 30 1 3600)"
MAX_UPLOAD="$(num_of max_upload_bytes 8589934592 1 1099511627776)"
MAX_FAILURES="$(num_of max_login_failures 5 0 1000000)"
LOCKOUT="$(num_of login_lockout_seconds 60 0 86400)"
REQUIRE_BPSEC2="$(bool_of require_bpsec2 false)"
ALLOW_PUBLIC="$(bool_of allow_public_bind false)"
PUBLIC_REASON="$(get public_bind_reason)"
INSECURE="$(bool_of allow_public_bind_insecure false)"
QUIET="$(bool_of quiet false)"

# ---- 2. 公网绑定：与产品同一条 fail-closed 合同，另加一条打包策略 ----
LOOPBACK=0
case "$BIND" in
  127.0.0.1|::1|localhost) LOOPBACK=1 ;;
esac
PUBLIC_BIND=0
if [ "$LOOPBACK" -eq 0 ]; then
  PUBLIC_BIND=1
  [ -n "$CERTIFICATE" ] || cfg_error "bind=$BIND 是非回环地址：必须同时配置 certificate（BPSEC2 签名身份），产品自身也会拒绝启动"
  [ -n "$PUBLIC_REASON" ] || cfg_error "bind=$BIND 需要 public_bind_reason（会写进启动日志与事后审计）"
  if [ "$REQUIRE_BPSEC2" != "true" ] && [ "$INSECURE" != "true" ]; then
    cfg_error "bind=$BIND 要求 require_bpsec2 = true（公网只接受签名身份客户端）。确实要用 BPSEC1 指纹模式请显式写 allow_public_bind_insecure = true"
  fi
elif [ "$ALLOW_PUBLIC" = "true" ]; then
  note "提示：allow_public_bind = true 但 bind=$BIND 是回环地址，公网开关不会生效"
fi

# ---- 3. 组装 argv（每个参数都是数组元素）----
ARGS=(--bind "$BIND" --port "$PORT" --root "$DATA_ROOT" --db "$DATABASE"
      --secret-file "$SECRET_FILE" --transport-key-file "$TRANSPORT_KEY")
[ -n "$CERTIFICATE" ] && ARGS+=(--bpsec2-cert-file "$CERTIFICATE")
[ "$REQUIRE_BPSEC2" = "true" ] && ARGS+=(--require-bpsec2)
[ "$PUBLIC_BIND" -eq 1 ] && ARGS+=(--allow-public-bind "$PUBLIC_REASON")
[ -n "$LOG_FILE" ] && ARGS+=(--log-file "$LOG_FILE")
[ -n "$PID_FILE" ] && ARGS+=(--pid-file "$PID_FILE")
ARGS+=(--workers "$WORKERS" --io-timeout "$IO_TIMEOUT" --max-upload-bytes "$MAX_UPLOAD")
ARGS+=(--max-login-failures "$MAX_FAILURES" --login-lockout-seconds "$LOCKOUT")
[ "$QUIET" = "true" ] && ARGS+=(--quiet)
ARGS+=(${EXTRA[@]+"${EXTRA[@]}"})

print_command() {
  local arg
  printf 'backup-server'
  for arg in "${ARGS[@]}"; do
    case "$arg" in
      *[!A-Za-z0-9_./=:@+-]*) printf " '%s'" "$arg" ;;
      *) printf ' %s' "$arg" ;;
    esac
  done
  printf '\n'
}

# ---- 4. 环境检查（只在 --check-config 时做，正常运行交给产品自己 fail closed）----
check_file() {
  local path="$1" label="$2" mode
  [ -e "$path" ] || env_error "$label 不存在：$path"
  mode="$(stat -c '%a' "$path")"
  printf '[config]   %-14s %s (mode %s, %s bytes)\n' "$label" "$path" "$mode" "$(stat -c %s "$path")"
}

run_checks() {
  printf '[config] 配置文件    %s\n' "$CONF"
  printf '[config] 监听          %s:%s%s\n' "$BIND" "$PORT" \
    "$( [ "$PUBLIC_BIND" -eq 1 ] && printf '  ← 公网绑定' || printf '  (仅回环)' )"
  printf '[config] 身份模式      %s\n' \
    "$( [ "$REQUIRE_BPSEC2" = "true" ] && printf 'BPSEC2 签名身份（拒绝 BPSEC1 降级）' || printf 'BPSEC1 指纹 + 可选 BPSEC2' )"
  [ -d "$DATA_ROOT" ] || env_error "数据目录不存在：$DATA_ROOT（先安装/初始化，或修正 data_root）"
  printf '[config]   %-14s %s (mode %s)\n' "data_root" "$DATA_ROOT" "$(stat -c '%a' "$DATA_ROOT")"
  check_file "$SECRET_FILE" "secret_file"
  if ! grep -q '^BACKUP_TOKEN_SECRET=' "$SECRET_FILE"; then
    env_error "secret_file 里没有 BACKUP_TOKEN_SECRET：$SECRET_FILE"
  fi
  local secret_len
  secret_len="$(sed -n 's/^BACKUP_TOKEN_SECRET=//p' "$SECRET_FILE" | head -1 | tr -d '\r\n' | wc -c)"
  if [ "$secret_len" -lt 16 ]; then
    env_error "BACKUP_TOKEN_SECRET 短于 16 字节（当前 $secret_len）"
  fi
  printf '[config]   %-14s BACKUP_TOKEN_SECRET 已存在（%s 字节，内容不显示）\n' "secret" "$secret_len"
  check_file "$TRANSPORT_KEY" "transport_key"
  local key_mode key_size
  key_mode="$(stat -c '%a' "$TRANSPORT_KEY")"
  key_size="$(stat -c %s "$TRANSPORT_KEY")"
  [ "$key_size" = "32" ] || env_error "transport_key 必须是 32 字节（当前 $key_size）：$TRANSPORT_KEY"
  case "$key_mode" in 600|400) ;; *) env_error "transport_key 权限必须是 0600（当前 $key_mode）：$TRANSPORT_KEY" ;; esac
  if [ -n "$CERTIFICATE" ]; then
    check_file "$CERTIFICATE" "certificate"
    if [ -x "$CERT_TOOL" ]; then
      if ! "$CERT_TOOL" inspect-server --cert "$CERTIFICATE" > /dev/null 2>&1; then
        env_error "certificate 无法解析（backup-cert-tool inspect-server 失败）：$CERTIFICATE"
      fi
      printf '[config]   %-14s backup-cert-tool inspect-server 通过\n' "certificate"
    fi
  else
    printf '[config]   %-14s 未配置（只支持 BPSEC1 指纹模式）\n' "certificate"
  fi
  [ -x "$SERVER_BIN" ] || env_error "找不到 backup-server：$SERVER_BIN"
  printf '[config] backup-server  %s\n' "$SERVER_BIN"
  printf '[config] 解析后的命令：\n  '
  print_command
  printf '[config] OK\n'
}

case "$MODE" in
  print)
    print_command
    exit 0 ;;
  check)
    run_checks
    exit 0 ;;
esac

[ -x "$SERVER_BIN" ] || env_error "找不到 backup-server：$SERVER_BIN"
exec "$SERVER_BIN" "${ARGS[@]}"
