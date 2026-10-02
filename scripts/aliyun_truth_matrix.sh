#!/usr/bin/env bash
#
# ECS 真机：客户端（与 GUI 同一条路径）↔ 管理 CLI ↔ SQLite 三方真值矩阵。
#
#   bash scripts/aliyun_truth_matrix.sh [ssh-alias] [host] [port]
#
# 前置条件（脚本自己不做端口转发、不改任何网络配置）：
#   * ECS 上的 backup-server 正在 127.0.0.1:<port> 监听；
#   * 本机已经有一条 SSH 隧道把 127.0.0.1:<port> 转到 ECS 的同一个端口。
#
# 它同时是那次 P0 的复验：管理 CLI 必须读到**服务端正在用的那个库**，并且每一步
# 都与客户端、与库文件本身对得上。口令随机生成，只从环境变量传，不打印。
#
# 退出码：全部通过才 0。

set -uo pipefail
cd "$(dirname "$BASH_SOURCE")/.."

ALIAS="${1:-aliyun-ecs}"
HOST="${2:-127.0.0.1}"
PORT="${3:-18765}"
REMOTE_OPTS="--host $HOST --port $PORT"

PASS=0
FAIL=0
record_pass() { PASS=$((PASS + 1)); echo "  PASS  $1"; }
record_fail() { FAIL=$((FAIL + 1)); echo "  FAIL  $1 -- $2" >&2; }

echo "[ecs-truth] ECS 真机三方真值矩阵（客户端 / 管理 CLI / SQLite）"

ADMIN_STATE_FILE="${TMPDIR:-/tmp}/ecs-truth-admin-state.txt"

# ---- 0. 实例身份：服务端进程实际用的路径 ----
SERVER_INFO="$(ssh -o BatchMode=yes "$ALIAS" '
  SP=$(pgrep -f "bin/backup-server --bind" | head -1)
  echo "pid=$SP"
  echo "host=$(hostname)"
  echo "exe=$(readlink -f /proc/$SP/exe)"
  echo "cwd=$(readlink -f /proc/$SP/cwd)"
  echo "root=$(tr "\0" "\n" < /proc/$SP/cmdline | awk "/^--root$/{getline; print}")"
  echo "db=$(tr "\0" "\n" < /proc/$SP/cmdline | awk "/^--db$/{getline; print}")"
  echo "sha=$(sha256sum /proc/$SP/exe | cut -d" " -f1)"')"
echo "$SERVER_INFO" | sed 's/^/    /'
SERVER_PID="$(printf '%s\n' "$SERVER_INFO" | sed -n 's/^pid=//p')"
SERVER_DB="$(printf '%s\n' "$SERVER_INFO" | sed -n 's/^db=//p')"
SERVER_SHA="$(printf '%s\n' "$SERVER_INFO" | sed -n 's/^sha=//p')"
if [ -n "$SERVER_PID" ] && [ -n "$SERVER_DB" ]; then
  record_pass "服务端实例：pid=$SERVER_PID db=$SERVER_DB"
else
  record_fail "解析服务端实例" "$SERVER_INFO"
fi

# ---- 1. 管理 CLI（wrapper）必须指向同一个库，并且能看到真实用户数 ----
WRAP_OUT="$(ssh -o BatchMode=yes "$ALIAS" 'cd ~/backup-project-server && \
  TERM=dumb ./bin/backup-server-admin.sh </dev/null 2>&1 | head -24')"
WRAP_DB="$(printf '%s\n' "$WRAP_OUT" | grep -m1 '^Metadata DB:' | sed 's/^Metadata DB:[[:space:]]*//')"
WRAP_USERS="$(printf '%s\n' "$WRAP_OUT" | grep -m1 '^用户数：' | sed 's/^用户数：//')"
if [ "$WRAP_DB" = "$SERVER_DB" ]; then
  record_pass "管理 CLI（wrapper 默认值）指向服务端的库：$WRAP_DB"
else
  record_fail "管理 CLI 指向服务端的库" "wrapper=$WRAP_DB server=$SERVER_DB"
fi
if [ -n "$WRAP_USERS" ] && [ "$WRAP_USERS" != "0" ]; then
  record_pass "管理 CLI 的 wrapper 首页就能看到真实用户数（用户数：$WRAP_USERS）"
else
  record_fail "wrapper 首页的用户数" "$(printf '%s\n' "$WRAP_OUT" | head -8 | tr '\n' ' ')"
fi
for label in "Host:" "Server root:" "Data root:" "Metadata DB:" "Service:"; do
  if printf '%s\n' "$WRAP_OUT" | grep -q "^$label"; then
    record_pass "ECS 管理 CLI 身份块包含 $label"
  else
    record_fail "ECS 管理 CLI 身份块包含 $label" "$(printf '%s\n' "$WRAP_OUT" | head -8 | tr '\n' ' ')"
  fi
done

admin_show() {
  ssh -o BatchMode=yes "$ALIAS" "cd ~/backup-project-server && \
    ./bin/backup-server-admin --root data --db state/metadata.sqlite3 show-user '$1' 2>&1"
}
admin_state() {
  # show-user 对"用户不存在"会以退出码 1 结束，而本脚本开了 pipefail：
  # 直接写 admin_show | grep -q 会因为远程的退出码把"匹配成功"也判成失败。
  # 先把输出落盘，再只看 grep 的结果。
  admin_show "$1" > "$ADMIN_STATE_FILE" 2>&1 || true
  if grep -q "没有这个用户" "$ADMIN_STATE_FILE"; then echo absent; else echo present; fi
}
db_count() {
  ssh -o BatchMode=yes "$ALIAS" "sqlite3 -readonly $SERVER_DB \"SELECT COUNT(*) FROM users WHERE username='$1';\" 2>/dev/null || echo '?'"
}
remote_out() {
  timeout --signal=KILL 180 ./build/backupctl remote "$@" $REMOTE_OPTS > /tmp/ecs-truth-remote.txt 2>&1
  echo $?
}

# ---- 2. 唯一测试账户 + 真值矩阵 ----
USER_NAME="qa_truth_$(head -c 4 /dev/urandom | od -An -tx1 | tr -d ' \n')"
export BACKUP_REMOTE_PASSWORD="$(head -c 24 /dev/urandom | sha256sum | cut -c1-24)"
echo "[ecs-truth] 唯一测试账户：$USER_NAME（口令随机生成，不打印）"

A_ADMIN="$(admin_state "$USER_NAME")"
A_DB="$(db_count "$USER_NAME")"
[ "$A_ADMIN" = "absent" ] && [ "$A_DB" = "0" ] \
  && record_pass "STATE A（注册前）：admin=absent / sqlite=0" \
  || record_fail "STATE A（注册前）" "admin=$A_ADMIN sqlite=$A_DB"

B_CODE="$(remote_out register --user "$USER_NAME")"
B_ADMIN="$(admin_state "$USER_NAME")"
B_DB="$(db_count "$USER_NAME")"
[ "$B_CODE" = "0" ] && [ "$B_ADMIN" = "present" ] && [ "$B_DB" = "1" ] \
  && record_pass "STATE B（注册后）：remote=ok / admin=present / sqlite=1" \
  || record_fail "STATE B（注册后）" \
     "remote=$B_CODE admin=$B_ADMIN sqlite=$B_DB $(head -1 /tmp/ecs-truth-remote.txt)"

C_LOGIN="$(remote_out login --user "$USER_NAME")"
C_LIST="$(remote_out list --user "$USER_NAME")"
C_ADMIN="$(admin_state "$USER_NAME")"
[ "$C_LOGIN" = "0" ] && [ "$C_LIST" = "0" ] && [ "$C_ADMIN" = "present" ] \
  && record_pass "STATE C（登录 + 列表）：login=ok / list=ok / admin=present" \
  || record_fail "STATE C（登录 + 列表）" "login=$C_LOGIN list=$C_LIST admin=$C_ADMIN"

D_ADMIN="$(admin_state "$USER_NAME")"
D_DB="$(db_count "$USER_NAME")"
[ "$D_ADMIN" = "present" ] && [ "$D_DB" = "1" ] \
  && record_pass "STATE D（本机退出登录之后）：admin=present / sqlite=1" \
  || record_fail "STATE D" "admin=$D_ADMIN sqlite=$D_DB"

E_CODE="$(remote_out login --user "$USER_NAME")"
E_ADMIN="$(admin_state "$USER_NAME")"
[ "$E_CODE" = "0" ] && [ "$E_ADMIN" = "present" ] \
  && record_pass "STATE E（重新登录）：login=ok / admin=present" \
  || record_fail "STATE E（重新登录）" "login=$E_CODE admin=$E_ADMIN"

F_CODE="$(remote_out delete-account --user "$USER_NAME" --confirm "$USER_NAME")"
F_ADMIN="$(admin_state "$USER_NAME")"
F_DB="$(db_count "$USER_NAME")"
F_DIR="$(ssh -o BatchMode=yes "$ALIAS" 'ls ~/backup-project-server/data/users 2>/dev/null | tr "\n" " "')"
[ "$F_CODE" = "0" ] && [ "$F_ADMIN" = "absent" ] && [ "$F_DB" = "0" ] \
  && record_pass "STATE F（注销成功）：remote=ok / admin=absent / sqlite=0" \
  || record_fail "STATE F（注销成功）" \
     "remote=$F_CODE admin=$F_ADMIN sqlite=$F_DB dirs=$F_DIR $(head -1 /tmp/ecs-truth-remote.txt)"

G_CODE="$(remote_out login --user "$USER_NAME")"
G_ADMIN="$(admin_state "$USER_NAME")"
G_DB="$(db_count "$USER_NAME")"
[ "$G_CODE" != "0" ] && [ "$G_ADMIN" = "absent" ] && [ "$G_DB" = "0" ] \
  && record_pass "STATE G（再次登录）：login FAIL / admin=absent / sqlite=0" \
  || record_fail "STATE G（再次登录）" "login=$G_CODE admin=$G_ADMIN sqlite=$G_DB"

# ---- 3. 重启服务端之后再确认一次（持久化 / 无内存缓存）----
NEW_PID="$(ssh -o BatchMode=yes "$ALIAS" "cd ~/backup-project-server && \
  kill -TERM $SERVER_PID && sleep 2 && \
  setsid nohup ./bin/backup-server --bind 127.0.0.1 --port $PORT \
    --root \$PWD/data --db \$PWD/state/metadata.sqlite3 \
    --secret-file /home/ubuntu/.config/backup-project-server/secrets.env \
    --log-file \$PWD/logs/server.log --pid-file \$PWD/state/server.pid \
    > \$PWD/logs/server.stdout 2>&1 < /dev/null & sleep 3 ; \
  pgrep -f 'bin/backup-server --bind' | head -1")"
sleep 2
R_CODE="$(remote_out login --user "$USER_NAME")"
R_ADMIN="$(admin_state "$USER_NAME")"
[ -n "$NEW_PID" ] && [ "$R_CODE" != "0" ] && [ "$R_ADMIN" = "absent" ] \
  && record_pass "重启服务端（新 pid $NEW_PID）之后再登录仍然失败，admin 仍然 absent" \
  || record_fail "重启之后复验" "pid=$NEW_PID login=$R_CODE admin=$R_ADMIN"

echo
echo "[ecs-truth] 服务端二进制 sha256=$SERVER_SHA"
echo "[ecs-truth] 本机 FINAL_HEAD=$(git rev-parse HEAD)"
echo "[ecs-truth] 合计: $PASS passed, $FAIL failed"
[ "$FAIL" = "0" ] || exit 1
echo "ECS_TRUTH_ALL_PASS"
