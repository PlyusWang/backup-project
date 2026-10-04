#!/usr/bin/env bash
#
# pr23_ecs_phase3_payload.sh —— **在 ECS 本机执行**的 Phase 3 动作。
#
#   pr23_ecs_phase3_payload.sh backup   <server-root> <stamp>
#   pr23_ecs_phase3_payload.sh deploy   <server-root> <stamp>
#   pr23_ecs_phase3_payload.sh verify   <server-root> <stamp>
#   pr23_ecs_phase3_payload.sh rollback <server-root> <stamp>
#
# 单独拆成一个脚本是有意的：编排脚本在本机跑，真正动服务器的事只有这一个
# 脚本能干，回滚路径与部署路径在同一个文件里，改的时候不会只改一半。
#
# 退出码：0 = 该阶段成功。

set -uo pipefail

set +u
PHASE="$1"
ROOT="$2"
STAMP="$3"
set -u
if [ -z "$PHASE" ] || [ -z "$ROOT" ] || [ -z "$STAMP" ]; then
  echo "用法：$0 <backup|deploy|verify|rollback> <server-root> <stamp>" >&2
  exit 2
fi

BIN="$ROOT/bin"
INCOMING="$ROOT/deploy/incoming"
BACKUP="$ROOT/deploy-backups/pr23-before-$STAMP"
CERT_DEST="$ROOT/state/server-identity.bpcert"
PID_FILE="$ROOT/state/server.pid"
LOG_FILE="$ROOT/logs/server.log"
BINARIES="backup-server backup-server-admin backup-server-keygen"

running_pid() {
  if [ -f "$PID_FILE" ]; then
    local pid
    pid="$(cat "$PID_FILE" 2>/dev/null)"
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
      echo "$pid"
      return 0
    fi
  fi
  pgrep -f "$ROOT/bin/backup-server --bind" 2>/dev/null | head -1
}

stop_server() {
  local pid
  pid="$(running_pid)"
  if [ -z "$pid" ]; then
    return 0
  fi
  kill "$pid" 2>/dev/null || true
  for _ in $(seq 1 40); do
    kill -0 "$pid" 2>/dev/null || return 0
    sleep 0.5
  done
  echo "服务端没有在 20 秒内退出，改用 SIGKILL（pid=$pid）" >&2
  kill -9 "$pid" 2>/dev/null || true
  sleep 1
  return 0
}

start_server() {
  local extra="$1"
  cd "$ROOT" || return 1
  # shellcheck disable=SC2086
  nohup "$BIN/backup-server" --bind 127.0.0.1 --port 18765 \
    --root "$ROOT/data" --db "$ROOT/state/metadata.sqlite3" \
    --secret-file "$HOME/.config/backup-project-server/secrets.env" \
    --transport-key-file "$ROOT/state/transport.key" \
    --log-file "$LOG_FILE" --pid-file "$PID_FILE" $extra \
    >> "$ROOT/logs/nohup.out" 2>&1 &
  echo $!
}

case "$PHASE" in
  backup)
    mkdir -p "$BACKUP"
    for name in $BINARIES; do
      if [ -f "$BIN/$name" ]; then
        cp -p "$BIN/$name" "$BACKUP/$name"
      fi
    done
    if [ -f "$PID_FILE" ]; then
      PID="$(cat "$PID_FILE")"
      tr '\0' ' ' < "/proc/$PID/cmdline" > "$BACKUP/run-command.txt" 2>/dev/null || true
    fi
    ( cd "$BACKUP" && sha256sum $BINARIES > MANIFEST.sha256 2>/dev/null )
    echo "BACKUP_DIR=$BACKUP"
    ls -la "$BACKUP"
    ;;

  deploy)
    # 二进制**不在这里装**：本机（Ubuntu 24.04 / glibc 2.39）编译出来的
    # 二进制在 ECS（22.04 / glibc 2.35）上会直接报 "GLIBC_2.38 not found"。
    # 所以二进制由 scripts/deploy_aliyun_server.sh 在目标机就地构建并安装
    # （那条路径是 PR #20 起就在用的），这里只做证书 + 重启这两件事。
    # 第一版把本机二进制 scp 过来装，结果新进程起不来 —— 回滚因此被真实触发
    # 了一次，也顺带证明了回滚确实有效。
    if [ ! -f "$INCOMING/server-identity.bpcert" ]; then
      echo "缺少待部署的证书（$INCOMING/server-identity.bpcert）" >&2
      exit 1
    fi
    install -m 644 "$INCOMING/server-identity.bpcert" "$CERT_DEST"
    stop_server
    PID="$(start_server "--bpsec2-cert-file $CERT_DEST --require-bpsec2")"
    echo "STARTED_PID=$PID"
    ;;

  verify)
    for _ in $(seq 1 60); do
      if ss -ltn 2>/dev/null | grep -q "127.0.0.1:18765 "; then
        break
      fi
      sleep 0.5
    done
    FAILED=0
    if ss -ltn 2>/dev/null | grep -q "127.0.0.1:18765 "; then
      echo "VERIFY listening=yes"
    else
      echo "VERIFY listening=NO" >&2
      FAILED=1
    fi
    if ss -ltn 2>/dev/null | grep -q "0.0.0.0:18765 "; then
      echo "VERIFY public_bind=YES（不应该！）" >&2
      FAILED=1
    else
      echo "VERIFY public_bind=no"
    fi
    if tail -40 "$LOG_FILE" 2>/dev/null | grep -q "BPSEC2 identity certificate loaded"; then
      echo "VERIFY certificate_loaded=yes"
    else
      echo "VERIFY certificate_loaded=NO" >&2
      FAILED=1
    fi
    echo "VERIFY pid=$(cat "$PID_FILE" 2>/dev/null)"
    exit "$FAILED"
    ;;

  rollback)
    if [ ! -d "$BACKUP" ]; then
      echo "找不到回滚备份目录 $BACKUP" >&2
      exit 1
    fi
    for name in $BINARIES; do
      if [ -f "$BACKUP/$name" ]; then
        install -m 755 "$BACKUP/$name" "$BIN/$name"
      fi
    done
    rm -f "$CERT_DEST"
    stop_server
    PID="$(start_server "")"
    echo "ROLLED_BACK_PID=$PID"
    for _ in $(seq 1 60); do
      if ss -ltn 2>/dev/null | grep -q "127.0.0.1:18765 "; then
        echo "ROLLBACK listening=yes"
        exit 0
      fi
      sleep 0.5
    done
    echo "回滚之后服务端没有起来（pid=$PID）" >&2
    exit 1
    ;;

  *)
    echo "未知阶段：$PHASE" >&2
    exit 2
    ;;
esac
