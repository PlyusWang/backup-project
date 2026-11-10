#!/usr/bin/env bash
# uninstall.sh —— 卸载便携安装。
#
#   sudo ./uninstall.sh --prefix /opt/backup-project-server [--purge-data]
#
# 默认**只删程序文件**：配置、secrets.env、传输身份私钥、数据库、备份数据全部
# 保留。真要删数据必须显式 --purge-data，并且手打确认短语。
set -Eeuo pipefail

portable_log() { echo "[portable] $*"; }

portable_prefix_path() {
  local prefix remaining component current=""
  case "$1" in
    ""|*[$'\r\n\t']*) echo '安装前缀不能为空或包含控制字符。' >&2; return 1 ;;
  esac
  case "/$1/" in
    */../*) echo '安装前缀不能包含 ..，请使用直接路径。' >&2; return 1 ;;
  esac
  prefix="$(realpath -ms -- "$1")" || return 1
  case "$prefix" in
    /|/usr|/etc|/var|/bin|/lib|/lib64|/sbin|/boot|/dev|/proc|/sys|/run|/home|/root|/tmp|/opt|/usr/local)
      echo "拒绝操作系统目录：$prefix" >&2; return 1 ;;
  esac
  remaining="${prefix#/}"
  while [ -n "$remaining" ]; do
    component="${remaining%%/*}"
    if [ "$remaining" = "$component" ]; then remaining=""; else remaining="${remaining#*/}"; fi
    current="$current/$component"
    [ ! -L "$current" ] || {
      echo "安装前缀不能经过符号链接：$current" >&2; return 1;
    }
  done
  if [ "${2:-0}" = 1 ]; then
    [ ! -e "$prefix" ] || [ -d "$prefix" ] || {
      echo "安装前缀不是目录：$prefix" >&2; return 1;
    }
  else
    [ -d "$prefix" ] || { echo "安装前缀不存在：$prefix" >&2; return 1; }
  fi
  printf '%s\n' "$prefix"
}

portable_check_removal_paths() {
  local prefix="$1" purge="$2" target
  for target in bin share docs; do
    [ ! -L "$prefix/$target" ] || {
      echo "程序目录是符号链接，保留安装：$prefix/$target" >&2; return 1;
    }
  done
  if [ "$purge" -eq 1 ]; then
    for target in var etc; do
      [ ! -L "$prefix/$target" ] || {
        echo "数据目录是符号链接，保留安装：$prefix/$target" >&2; return 1;
      }
    done
  fi
}

portable_unit_matches_prefix() {
  local prefix="$1" unit="$2" unit_dir plain=1
  [ -f "$unit" ] && [ ! -L "$unit" ] || return 1
  [ -x "$prefix/bin/launch-server.sh" ] && [ ! -L "$prefix/bin/launch-server.sh" ] &&
    [ -f "$prefix/etc/server.conf" ] && [ ! -L "$prefix/etc/server.conf" ] || return 1
  portable_prefix_path "$prefix/bin" >/dev/null &&
    portable_prefix_path "$prefix/etc" >/dev/null || return 1
  unit_dir="$(portable_prefix_path "$(dirname -- "$unit")")" || return 1
  [ "$unit_dir/$(basename -- "$unit")" = "$unit" ] || return 1
  # These characters have expansion/escaping meanings in systemd, even when
  # quoted. Unknown encodings must never be guessed during a destructive action.
  case "$prefix" in *\\*|*\"*|*\'*|*'$'*|*'%'*) return 1 ;; esac
  [[ "$prefix" != *[[:space:]]* ]] || plain=0
  awk -v prefix="$prefix" -v plain="$plain" '
    BEGIN {
      allowed=" Type User Group RuntimeDirectoryMode ExecStart ExecReload Restart RestartSec StartLimitIntervalSec StartLimitBurst TimeoutStopSec KillSignal NoNewPrivileges PrivateTmp ProtectSystem ProtectHome ReadWritePaths ProtectKernelTunables ProtectKernelModules ProtectKernelLogs ProtectControlGroups ProtectClock ProtectHostname ProtectProc RestrictSUIDSGID RestrictRealtime RestrictNamespaces LockPersonality MemoryDenyWriteExecute RestrictAddressFamilies SystemCallArchitectures UMask "
      good=1
    }
    /^[ \t]*($|[#;])/ { next }
    {
      line=$0
      sub(/^[ \t]+/, "", line); sub(/[ \t\r]+$/, "", line)
      if (line ~ /^\[/) {
        if (line == "[Unit]") section="Unit"
        else if (line == "[Service]") { section="Service"; services++ }
        else if (line == "[Install]") section="Install"
        else good=0
        next
      }
      separator=index(line, "=")
      if (!separator) { good=0; next }
      key=substr(line, 1, separator-1); sub(/[ \t]+$/, "", key)
      value=substr(line, separator+1); sub(/^[ \t]+/, "", value)
      if (seen[section SUBSEP key]++) good=0
      if (section == "Unit") {
        if (key != "Description" && key != "Documentation" && key != "After" && key != "Wants") good=0
        if ((key == "After" || key == "Wants") && value != "network.target") good=0
      } else if (section == "Install") {
        if (key != "WantedBy" || value != "multi-user.target") good=0
      } else if (section == "Service") {
        if (!index(allowed, " " key " ")) good=0
        if (key == "Type") { types++; if (value != "simple") good=0 }
        if (key == "ExecStart") {
          starts++
          expected=prefix "/bin/launch-server.sh --config " prefix "/etc/server.conf"
          quoted="\"" prefix "/bin/launch-server.sh\" --config \"" prefix "/etc/server.conf\""
          if (value != quoted && !(plain && value == expected)) good=0
        }
        if (key == "ReadWritePaths") {
          roots++
          if (value != "\"" prefix "/var\"" && !(plain && value == prefix "/var")) good=0
        }
      } else good=0
    }
    END { exit !(good && services == 1 && types == 1 && starts == 1 && roots == 1) }
  ' "$unit"
}

portable_owned_unit_fingerprint() {
  local prefix="$1" unit="$2" service=backup-project-server.service
  local state fragment dropins reload fingerprint
  portable_unit_matches_prefix "$prefix" "$unit" || return 1
  fingerprint="$(sha256sum -- "$unit")" || return 1
  # The loaded service must agree with the file, without overriding drop-ins or
  # a pending definition change. Both install and uninstall use this contract.
  state="$(systemctl show --property=LoadState --value "$service")" &&
    fragment="$(systemctl show --property=FragmentPath --value "$service")" &&
    dropins="$(systemctl show --property=DropInPaths --value "$service")" &&
    reload="$(systemctl show --property=NeedDaemonReload --value "$service")" || return 1
  [ "$state" = loaded ] && [ "$fragment" = "$unit" ] && [ -z "$dropins" ] && [ "$reload" = no ] &&
    [ ! -L "$unit" ] && [ "$(sha256sum -- "$unit")" = "$fingerprint" ] || return 1
  printf '%s\n' "$fingerprint"
}

portable_systemd_install_preflight() {
  local prefix="$1" unit="$2" service=backup-project-server.service state fragment dropins
  # The current installer emits unquoted systemd paths. Reject unsupported
  # systemd prefixes before writing programs, keys or configuration; portable
  # --no-systemd installs continue to support spaces and non-ASCII paths.
  if [[ ! "$prefix" =~ ^/[-A-Za-z0-9_./=]+$ ]]; then
    echo 'systemd 安装前缀仅支持 ASCII 字母、数字及 / . _ - =；请换路径或使用 --no-systemd。' >&2
    return 1
  fi
  if [ ! -e "$unit" ] && [ ! -L "$unit" ]; then
    # Absence of an /etc override does not exclude a vendor /lib or generated
    # service. Only systemd can establish that this name is truly unused.
    if ! state="$(systemctl show --property=LoadState --value "$service")" ||
       ! fragment="$(systemctl show --property=FragmentPath --value "$service")" ||
       ! dropins="$(systemctl show --property=DropInPaths --value "$service")" ||
       [ "$state" != not-found ] || [ -n "$fragment" ] || [ -n "$dropins" ]; then
      echo 'systemd 同名服务已存在或状态无法确认，拒绝创建覆盖 unit 或修改安装。' >&2
      return 1
    fi
    return 0
  fi
  if ! portable_owned_unit_fingerprint "$prefix" "$unit" >/dev/null; then
    echo '已有 systemd unit 无法确认属于当前前缀，拒绝覆盖、重启或修改安装。' >&2
    return 1
  fi
}

portable_remove_systemd_unit() {
  local prefix="$1" unit="$2" runtime="$3" service=backup-project-server.service fingerprint
  [ -e "$unit" ] || [ -L "$unit" ] || return 0
  if [ ! -d "$runtime" ] || ! fingerprint="$(portable_owned_unit_fingerprint "$prefix" "$unit")"; then
    portable_log "无法确认 systemd unit 属于当前安装，已保留服务和程序：$unit"
    return 1
  fi
  if [ -L "$unit" ] || [ "$(sha256sum -- "$unit")" != "$fingerprint" ]; then
    portable_log 'unit 在验证期间变化，已保留服务和程序。'
    return 1
  fi
  if ! systemctl stop "$service"; then
    portable_log '停止服务失败，已保留 unit 和程序。'
    return 1
  fi
  if [ -L "$unit" ] || [ "$(sha256sum -- "$unit")" != "$fingerprint" ] ||
     ! systemctl disable "$service"; then
    portable_log 'unit 已变化或禁用失败，已保留 unit 和程序。'
    return 1
  fi
  if [ -L "$unit" ] || [ "$(sha256sum -- "$unit")" != "$fingerprint" ]; then
    portable_log 'unit 在卸载期间变化，已保留 unit 和程序。'
    return 1
  fi
  rm -f -- "$unit"
  systemctl daemon-reload || { portable_log '重新加载 systemd 失败，已保留程序。'; return 1; }
  portable_log '已移除当前安装的 systemd unit'
}

# install.sh sources only these shared checks, after validating its manifest.
if [ "${BASH_SOURCE[0]}" != "$0" ]; then return 0; fi

UNINSTALL_SCRIPT="$(realpath -e -- "${BASH_SOURCE[0]}")"
PREFIX=""
PURGE_DATA=0

while [ $# -gt 0 ]; do
  case "$1" in
    --prefix) PREFIX="$2"; shift 2 ;;
    --purge-data) PURGE_DATA=1; shift ;;
    --remove-user)
      echo '无法证明系统账号由此实例独占，已保留程序、数据和账号；请由管理员独立管理账号。' >&2
      exit 2 ;;
    -h|--help) sed -n '2,7p' "$0"; exit 0 ;;
    *) echo "未知参数：$1" >&2; exit 2 ;;
  esac
done

PREFIX="$(portable_prefix_path "$PREFIX")" || exit 2
portable_check_removal_paths "$PREFIX" "$PURGE_DATA" || exit 2
portable_remove_systemd_unit "$PREFIX" \
  /etc/systemd/system/backup-project-server.service /run/systemd/system || exit 2

rm -rf -- "${PREFIX:?}/bin" "${PREFIX:?}/share" "${PREFIX:?}/docs"
portable_log "已删除程序文件（$PREFIX/bin, share, docs）"

if [ "$PURGE_DATA" -eq 1 ]; then
  echo
  echo "这会永久删除：$PREFIX/var（备份数据、元数据库、传输身份私钥）与 $PREFIX/etc（配置与 token secret）。"
  printf '请输入 DELETE ALL BACKUP DATA 以确认：'
  read -r answer || answer=""
  [ "$answer" = "DELETE ALL BACKUP DATA" ] || { echo "确认文字不匹配：只删了程序文件。" >&2; exit 2; }
  rm -rf -- "${PREFIX:?}/var" "${PREFIX:?}/etc"
  portable_log "已删除数据与配置"
else
  portable_log "保留：$PREFIX/var（数据/数据库/私钥）与 $PREFIX/etc（配置/secret）"
  portable_log "要删除它们：$(printf '%q' "$UNINSTALL_SCRIPT") --prefix $(printf '%q' "$PREFIX") --purge-data"
fi
