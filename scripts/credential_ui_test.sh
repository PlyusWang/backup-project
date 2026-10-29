#!/usr/bin/env bash
# 自动任务解锁、云端会话与密码对话框的真实 QML 控件专项。
set -euo pipefail
TASK_REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mkdir -p "$TASK_REPO/tests/output"
TASK_OUTPUT="${BACKUP_CREDENTIAL_OUTPUT:-$(mktemp -d "$TASK_REPO/tests/output/credential-ui-XXXXXX")}"
mkdir -p "$TASK_OUTPUT"
TASK_OUTPUT="$(cd "$TASK_OUTPUT" && pwd)"
TASK_RUNTIME="$(mktemp -d "$TASK_OUTPUT/runtime-XXXXXX")"

# GUI 异常退出时 C++ 析构未必运行；只回收这个唯一目录中登记的测试服务器。
owned_server() {
  local pid="$1" pid_file="$2" index
  local -a command=()
  [[ "$pid" =~ ^[1-9][0-9]*$ ]] || return 1
  [[ -r "/proc/$pid/cmdline" ]] || return 1
  if ! mapfile -d '' -t command < "/proc/$pid/cmdline"; then
    return 1
  fi
  [[ "${command[0]:-}" = "$TASK_REPO/build/backup-server" ]] || return 1
  for ((index = 1; index + 1 < ${#command[@]}; ++index)); do
    if [[ "${command[index]}" = --pid-file &&
          "${command[index + 1]}" = "$pid_file" ]]; then
      return 0
    fi
  done
  return 1
}

cleanup() {
  local status=$? pid_file pid remaining log_file log_index=0
  trap - EXIT INT TERM
  set +e
  while IFS= read -r -d '' pid_file; do
    pid="$(< "$pid_file")"
    if owned_server "$pid" "$pid_file"; then
      kill -TERM -- "$pid" 2>/dev/null
      remaining=30
      while ((remaining > 0)) && owned_server "$pid" "$pid_file"; do
        sleep 0.1
        remaining=$((remaining - 1))
      done
      if owned_server "$pid" "$pid_file"; then
        echo "[credential-ui] 测试服务器未在等待期内退出：$pid" >&2
        if ((status == 0)); then status=1; fi
      fi
    fi
  done < <(find "$TASK_RUNTIME" -type f -path '*/server-state/pid' -print0)
  while IFS= read -r -d '' log_file; do
    log_index=$((log_index + 1))
    cp -- "$log_file" "$TASK_OUTPUT/${TASK_RUNTIME##*/}-server-$log_index.log"
  done < <(find "$TASK_RUNTIME" -type f -path '*/server-state/server.log' -print0)
  # 只删除本次 mktemp 创建的子目录；密钥、口令与临时配置不进入测试交付物。
  if [[ "$TASK_RUNTIME" = "$TASK_OUTPUT"/runtime-* &&
        -d "$TASK_RUNTIME" && ! -L "$TASK_RUNTIME" ]]; then
    if ! rm -rf -- "$TASK_RUNTIME"; then
      echo "[credential-ui] 本次临时目录未能清理：$TASK_RUNTIME" >&2
      if ((status == 0)); then status=1; fi
    fi
  fi
  exit "$status"
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$TASK_RUNTIME/config" "$TASK_RUNTIME/cache" "$TASK_RUNTIME/data"
export XDG_CONFIG_HOME="$TASK_RUNTIME/config"
export XDG_CACHE_HOME="$TASK_RUNTIME/cache"
export XDG_DATA_HOME="$TASK_RUNTIME/data"
export TMPDIR="$TASK_RUNTIME"
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}"
"$TASK_REPO/build/backup-gui-modern" --credential-ui-test "$TASK_OUTPUT/screenshots"
