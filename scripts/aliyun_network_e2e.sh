#!/usr/bin/env bash
#
# PR #20：阿里云真实端到端（SSH tunnel + 真实归档 + 真实恢复）。
#
#   bash scripts/aliyun_network_e2e.sh [ssh-alias]
#
# 前置条件：ECS 上的 backup-server 已在运行且**只**绑 127.0.0.1:18765；
# 本机能以 BatchMode 免密登录该 alias；secrets.env 在 ECS 上存在可读。
#
# 隧道只绑本机环回：127.0.0.1:18765 -> ECS 127.0.0.1:18765。
# 不改安全组，不把 18765 暴露到公网。
#
# 口令随机生成，只经 BACKUP_REMOTE_PASSWORD 传给 backupctl；
# 输出里没有口令、token 或 secret。

set -uo pipefail

ALIAS="$1"
[ -n "$ALIAS" ] || ALIAS=aliyun-ecs
REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
CLI="$REPO_DIR/build/backupctl"
ARCHIVE_CLI="$REPO_DIR/build/archive-cli"
WORK_DIR="$PR20_E2E_DIR"
[ -n "$WORK_DIR" ] || WORK_DIR="$(mktemp -d /tmp/pr20-e2e-XXXXXX)"
PORT=18765
PASS=0
FAIL=0

record_pass() { PASS=$((PASS + 1)); echo "  PASS  $1"; }
record_fail() { FAIL=$((FAIL + 1)); echo "  FAIL  $1 -- $2" >&2; }

run() { timeout 300 "$CLI" remote "$@" --host 127.0.0.1 --port "$PORT"; }

echo "[e2e] 前置检查"
if ! ssh -o BatchMode=yes "$ALIAS" 'hostname' > "$WORK_DIR/host.txt" 2>&1; then
  echo "[e2e] 无法免密登录 $ALIAS" >&2
  exit 1
fi
echo "[e2e] ECS hostname: $(cat "$WORK_DIR/host.txt")"

echo "[e2e] 建立 SSH tunnel（只绑环回）"
ssh -o BatchMode=yes -o ExitOnForwardFailure=yes -o ServerAliveInterval=30 \
  -N -L "127.0.0.1:$PORT:127.0.0.1:$PORT" "$ALIAS" \
  > "$WORK_DIR/tunnel.log" 2>&1 &
TUNNEL_PID=$!
cleanup() {
  if kill -0 "$TUNNEL_PID" 2>/dev/null; then
    kill -TERM "$TUNNEL_PID" 2>/dev/null
    wait "$TUNNEL_PID" 2>/dev/null
  fi
}
sleep 2
if kill -0 "$TUNNEL_PID" 2>/dev/null; then
  record_pass "SSH tunnel 建立（pid $TUNNEL_PID）"
else
  record_fail "SSH tunnel 建立" "$(head -2 "$WORK_DIR/tunnel.log" | tr '\n' ' ')"
  exit 1
fi
if ss -ltn 2>/dev/null | grep -q "0.0.0.0:$PORT"; then
  record_fail "隧道只绑环回" "出现了 0.0.0.0 监听"
else
  record_pass "隧道只绑环回"
fi

mkdir -p "$WORK_DIR/src/sub" "$WORK_DIR/repo" "$WORK_DIR/out"
printf 'alpha\n' > "$WORK_DIR/src/a.txt"
printf 'bee\n' > "$WORK_DIR/src/sub/b.txt"
head -c 8192 /dev/urandom > "$WORK_DIR/src/payload.bin"
CONFIG="$WORK_DIR/config.json"
export BACKUP_REMOTE_PASSWORD="$(head -c 24 /dev/urandom | sha256sum | cut -c1-32)"
STAMP="$(date +%s)"
USER_A="e2e-a-$STAMP"
USER_B="e2e-b-$STAMP"

echo "[e2e] A. ping"
if run ping | grep -q "PING 正常"; then
  record_pass "A. ping 经隧道到达 ECS"
else
  record_fail "A. ping" "没有拿到 PING 响应"
fi

echo "[e2e] B. register / login"
run register --user "$USER_A" >/dev/null 2>&1 \
  && record_pass "B. 注册用户 A" \
  || record_fail "B. 注册用户 A" "失败"
run login --user "$USER_A" >/dev/null 2>&1 \
  && record_pass "B. 登录用户 A" \
  || record_fail "B. 登录用户 A" "失败"

echo "[e2e] C. 用产品 CLI 生成真实归档并上传"
"$CLI" config repository set "$WORK_DIR/repo" --config-file "$CONFIG" >/dev/null 2>&1
"$CLI" backup "$WORK_DIR/src" --config-file "$CONFIG" >/dev/null 2>&1
LOCAL_ARCHIVE="$(ls -1 "$WORK_DIR/repo"/*.bak 2>/dev/null | head -1)"
if [ -z "$LOCAL_ARCHIVE" ]; then
  record_fail "C. 生成真实归档" "没有产出 .bak"
  cleanup
  exit 1
fi
LOCAL_SHA="$(sha256sum "$LOCAL_ARCHIVE" | cut -d' ' -f1)"
LOCAL_SIZE="$(stat -c%s "$LOCAL_ARCHIVE")"
LOCAL_SHA_SHORT="$(echo "$LOCAL_SHA" | cut -c1-16)"
record_pass "C. 真实归档 $LOCAL_SIZE 字节 sha256=$LOCAL_SHA_SHORT..."

UPLOAD_OUT="$(run upload "$LOCAL_ARCHIVE" --user "$USER_A" --repository "$WORK_DIR/repo" 2>&1)"
SNAP="$(echo "$UPLOAD_OUT" | grep -m1 '快照 ID:' | awk '{print $3}')"
if [ -n "$SNAP" ]; then
  record_pass "C. 上传成功 snapshot=$SNAP"
else
  record_fail "C. 上传" "$(echo "$UPLOAD_OUT" | head -2 | tr '\n' ' ')"
fi

echo "[e2e] D. list"
run list --user "$USER_A" 2>/dev/null | grep -q "$SNAP" \
  && record_pass "D. 列表里有刚上传的快照" \
  || record_fail "D. 列表" "列表里没有 $SNAP"

echo "[e2e] E. download + restore"
run download "$SNAP" "$WORK_DIR/out/roundtrip.bak" --user "$USER_A" >/dev/null 2>&1 \
  && record_pass "E. 下载成功" \
  || record_fail "E. 下载" "失败"
DL_SHA="$(sha256sum "$WORK_DIR/out/roundtrip.bak" 2>/dev/null | cut -d' ' -f1)"
if [ "$DL_SHA" = "$LOCAL_SHA" ]; then
  record_pass "E. 下载回来的归档与本地归档 SHA-256 一致"
else
  record_fail "E. 下载哈希" "$DL_SHA != $LOCAL_SHA"
fi
if [ ! -x "$ARCHIVE_CLI" ]; then
  make -C "$REPO_DIR" test-fixtures >/dev/null 2>&1
fi
if "$ARCHIVE_CLI" restore "$WORK_DIR/out/roundtrip.bak" "$WORK_DIR/out/restored" \
     >/dev/null 2>&1 \
   && diff -r "$WORK_DIR/src" "$WORK_DIR/out/restored" >/dev/null 2>&1; then
  record_pass "E. 恢复下载的归档，内容与源目录一致"
else
  record_fail "E. 恢复" "恢复结果与源不一致"
fi

echo "[e2e] ECS 侧证据"
if ssh -o BatchMode=yes "$ALIAS" "bash -s" <<REMOTE > "$WORK_DIR/evidence.txt" 2>&1
SRV="$HOME/backup-project-server"
echo "ecs_hostname=$(hostname)"
echo "server_pid=$(cat $SRV/state/server.pid)"
echo "loopback_listeners=$(ss -ltn | grep -c '127.0.0.1:18765')"
echo "blob_sha256=$(sha256sum $SRV/data/users/1/$SNAP.bak | cut -d' ' -f1)"
echo "blob_size=$(stat -c%s $SRV/data/users/1/$SNAP.bak)"
echo "db_row=$(sqlite3 $SRV/state/metadata.sqlite3 \"select id||'|'||user_id||'|'||size_bytes||'|'||sha256 from snapshots where id='$SNAP';\")"
REMOTE
then
  sed 's/^/  /' "$WORK_DIR/evidence.txt"
  if grep -q "$LOCAL_SHA" "$WORK_DIR/evidence.txt"; then
    record_pass "ECS 上的 blob SHA-256 == 本地归档 SHA-256"
  else
    record_fail "ECS 上的 blob SHA-256" "对不上"
  fi
else
  record_fail "ECS 侧证据" "无法读取"
fi

echo "[e2e] F. 跨用户"
run register --user "$USER_B" >/dev/null 2>&1
run login --user "$USER_B" >/dev/null 2>&1
run list --user "$USER_B" 2>/dev/null | grep -q "$SNAP" \
  && record_fail "F. B 的列表看不到 A 的快照" "泄漏了" \
  || record_pass "F. B 的列表看不到 A 的快照"
run download "$SNAP" "$WORK_DIR/out/steal.bak" --user "$USER_B" >/dev/null 2>&1 \
  && record_fail "F. B 不能下载 A 的快照" "竟然成功了" \
  || record_pass "F. B 不能下载 A 的快照"
run delete "$SNAP" --user "$USER_B" >/dev/null 2>&1 \
  && record_fail "F. B 不能删除 A 的快照" "竟然成功了" \
  || record_pass "F. B 不能删除 A 的快照"

echo "[e2e] G. 删除自己的快照"
run delete "$SNAP" --user "$USER_A" >/dev/null 2>&1 \
  && record_pass "G. A 删除自己的快照" \
  || record_fail "G. 删除" "失败"
REMOTE_CHECK="$(ssh -o BatchMode=yes "$ALIAS" "bash -s" <<REMOTE
SRV="$HOME/backup-project-server"
if [ -f "$SRV/data/users/1/$SNAP.bak" ]; then echo BLOB_STILL_THERE; else echo BLOB_GONE; fi
echo "rows=$(sqlite3 $SRV/state/metadata.sqlite3 \"select count(*) from snapshots where id='$SNAP';\")"
REMOTE
)"
echo "$REMOTE_CHECK" | sed 's/^/  /'
echo "$REMOTE_CHECK" | grep -q BLOB_GONE \
  && record_pass "G. ECS 上的 blob 已消失" \
  || record_fail "G. ECS 上的 blob 已消失" "文件还在"

cleanup
record_pass "隧道已关闭"

echo
echo "[e2e] 合计: $PASS passed, $FAIL failed  (工作目录 $WORK_DIR)"
[ "$FAIL" = "0" ] || exit 1
echo "ALIYUN_NETWORK_E2E_ALL_PASS"
