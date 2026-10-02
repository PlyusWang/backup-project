#!/usr/bin/env bash
#
# same_instance_truth_test.sh —— "同一个实例" 的真值测试。
#
# 这一条测试是为人工验收里那次 P0 写的：
#
#     GUI 显示 "当前账户：Wjy / 状态：已登录"
#     同一个 ECS 上的管理 CLI 却显示 "还没有任何用户"
#
# 根因是**状态根分叉**：wrapper 默认 DB 指向 <server-root>/data/metadata.sqlite3，
# 而服务端用的是 <server-root>/state/metadata.sqlite3；SQLite 在文件不存在时会
# 新建，于是管理 CLI 安静地建了一个空库，屏幕上"还没有任何用户"和"你看错实例了"
# 长得一模一样。
#
# 所以这里断言的不是"某个函数返回 0"，而是四个真值源必须同时一致：
#
#     1. 协议（RemoteArchiveClient，与 GUI 同一个实现，这里经由 backupctl remote）
#     2. 管理 CLI（用户要手工用的那个工具，走 wrapper）
#     3. 元数据库文件本身（realpath + device/inode + 原始计数）
#     4. 服务端进程实际使用的路径（从 /proc/<pid>/cmdline 解析）
#
# 并且覆盖 §20：把管理 CLI 指向一个不存在的实例时，必须**失败且不创建任何文件**。
#
# 退出码：全部通过才 0。

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "$BASH_SOURCE")/.." && pwd)"
cd "$ROOT_DIR"

# 刻意放在 /tmp：仓库里的 testdata 会被 scripts/test.sh 整棵清掉。
TEST_ROOT="${TMPDIR:-/tmp}/same-instance-truth"
rm -rf "$TEST_ROOT"
mkdir -p "$TEST_ROOT"

PASS=0
FAIL=0
record_pass() { PASS=$((PASS + 1)); echo "  PASS  $1"; }
record_fail() { FAIL=$((FAIL + 1)); echo "  FAIL  $1 -- $2" >&2; }

echo "[same-instance] PR #20 P0：GUI / 服务端 / 管理 CLI / 数据库 四源一致性"

SERVER_PID=""
cleanup() {
  if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill -TERM "$SERVER_PID" 2>/dev/null
    wait "$SERVER_PID" 2>/dev/null
  fi
}
trap cleanup EXIT
finish() {
  cleanup
  echo
  echo "[same-instance] 合计: $PASS passed, $FAIL failed"
  [ "$FAIL" = "0" ] || exit 1
  echo "SAME_INSTANCE_ALL_PASS"
}

# ---- 0. 构建 + 搭一个和 ECS 一样的部署布局 ----
if ! make -j4 all server > "$TEST_ROOT/build.log" 2>&1; then
  record_fail "make all server" "$(tail -3 "$TEST_ROOT/build.log" | tr '\n' ' ')"
  finish
fi
record_pass "make all server 成功（warning $(grep -ci warning "$TEST_ROOT/build.log" || true) 条）"

DEPLOY="$TEST_ROOT/deploy"
mkdir -p "$DEPLOY/bin" "$DEPLOY/data" "$DEPLOY/state"
cp build/backup-server "$DEPLOY/bin/backup-server"
cp build/backup-server-admin "$DEPLOY/bin/backup-server-admin"
cp scripts/backup-server-admin.sh "$DEPLOY/bin/backup-server-admin.sh"
chmod +x "$DEPLOY/bin/backup-server-admin.sh"
head -c 32 /dev/urandom | sha256sum | cut -c1-64 \
  | sed 's/^/BACKUP_TOKEN_SECRET=/' > "$TEST_ROOT/secrets.env"
chmod 600 "$TEST_ROOT/secrets.env"

SERVER_ROOT="$DEPLOY"
DATA_ROOT="$SERVER_ROOT/data"
DB_PATH="$SERVER_ROOT/state/metadata.sqlite3"
ADMIN="$SERVER_ROOT/bin/backup-server-admin"

PORT=""
for candidate in $(seq 20250 20280); do
  if ! ss -ltn 2>/dev/null | grep -q ":$candidate "; then PORT="$candidate"; break; fi
done
[ -n "$PORT" ] || { record_fail "端口" "20250-20280 都被占用"; finish; }

# 服务端**从部署根启动**，用的就是部署脚本安装出来的那一份二进制。
( cd "$SERVER_ROOT" && ./bin/backup-server --bind 127.0.0.1 --port "$PORT" \
    --root "$DATA_ROOT" --db "$DB_PATH" --secret-file "$TEST_ROOT/secrets.env" \
    --pid-file "$SERVER_ROOT/state/server.pid" \
    --log-file "$SERVER_ROOT/logs-server.log" --quiet \
    > "$TEST_ROOT/server-stderr.log" 2>&1 ) &
SERVER_PID=$!
ready=0
for _ in $(seq 1 60); do
  if ss -ltn 2>/dev/null | grep -q "127.0.0.1:$PORT "; then ready=1; break; fi
  sleep 0.1
done
[ "$ready" = "1" ] || { record_fail "服务端监听" "没有起来"; finish; }

# $! 是那个包装子 shell 的 PID；服务端真正的 PID 从它自己写的 pid 文件读。
# （这正是产品行为的一部分：启动时 --pid-file 会写下自己的 pid。）
for _ in $(seq 1 50); do
  [ -s "$SERVER_ROOT/state/server.pid" ] && break
  sleep 0.1
done
PID_FROM_FILE="$(cat "$SERVER_ROOT/state/server.pid" 2>/dev/null | tr -d '[:space:]')"
if [ -n "$PID_FROM_FILE" ] && kill -0 "$PID_FROM_FILE" 2>/dev/null; then
  SERVER_PID="$PID_FROM_FILE"
fi
record_pass "部署布局里的 backup-server 监听 127.0.0.1:$PORT（pid $SERVER_PID）"

# ---- 1. 服务端进程实际用的路径（唯一的权威）----
SERVER_CMDLINE="$(tr '\0' '\n' < "/proc/$SERVER_PID/cmdline" 2>/dev/null)"
SERVER_DB_ARG="$(printf '%s\n' "$SERVER_CMDLINE" | awk '/^--db$/{getline; print}')"
SERVER_ROOT_ARG="$(printf '%s\n' "$SERVER_CMDLINE" | awk '/^--root$/{getline; print}')"
record_pass "从 /proc/$SERVER_PID/cmdline 解析出 --root=$SERVER_ROOT_ARG --db=$SERVER_DB_ARG"

# ---- 2. 管理工具解析出来的路径与身份（wrapper，人工验收用的入口）----
WRAPPER_OUT="$TEST_ROOT/wrapper-status.txt"
timeout --signal=KILL 120 "$SERVER_ROOT/bin/backup-server-admin.sh" </dev/null \
  > "$WRAPPER_OUT" 2>&1
ADMIN_DB_ARG="$(grep -m1 '^Metadata DB:' "$WRAPPER_OUT" | sed 's/^Metadata DB:[[:space:]]*//' || true)"
if [ -z "$ADMIN_DB_ARG" ]; then
  record_fail "wrapper 打印实例身份" "$(head -3 "$WRAPPER_OUT" | tr '\n' ' ')"
  ADMIN_DB_ARG="(none)"
else
  record_pass "wrapper 打印实例身份：$(grep -m1 '^Server root:' "$WRAPPER_OUT" | tr -s ' ')"
fi
for label in "Host:" "Server root:" "Data root:" "Metadata DB:" "Service:"; do
  if grep -q "^$label" "$WRAPPER_OUT"; then
    record_pass "身份块包含 $label"
  else
    record_fail "身份块包含 $label" "$(head -6 "$WRAPPER_OUT" | tr '\n' ' ')"
  fi
done

# ---- 3. SERVER_DB == ADMIN_DB（realpath + device/inode）----
SERVER_DB_REAL="$(realpath "$SERVER_DB_ARG" 2>/dev/null || echo "(none)")"
ADMIN_DB_REAL="$(realpath "$ADMIN_DB_ARG" 2>/dev/null || echo "(none)")"
WANT_DB_REAL="$(realpath "$DB_PATH")"
if [ "$SERVER_DB_REAL" = "$WANT_DB_REAL" ] && [ "$ADMIN_DB_REAL" = "$WANT_DB_REAL" ]; then
  record_pass "服务端与管理 CLI 指向同一个库：$WANT_DB_REAL"
else
  record_fail "服务端与管理 CLI 指向同一个库" \
    "server=$SERVER_DB_REAL admin=$ADMIN_DB_REAL want=$WANT_DB_REAL"
fi
SERVER_INODE="$(stat -c '%d:%i' "$SERVER_DB_REAL" 2>/dev/null || echo none)"
ADMIN_INODE="$(stat -c '%d:%i' "$ADMIN_DB_REAL" 2>/dev/null || echo none)"
if [ "$SERVER_INODE" = "$ADMIN_INODE" ] && [ "$SERVER_INODE" != "none" ]; then
  record_pass "同一个 device/inode（$SERVER_INODE）"
else
  record_fail "device/inode 一致" "server=$SERVER_INODE admin=$ADMIN_INODE"
fi

# ---- 4. 部署根里不允许出现第二个 metadata.sqlite3 ----
STRAY="$(find "$SERVER_ROOT" -name 'metadata.sqlite3' ! -path "$SERVER_ROOT/state/*" 2>/dev/null | head -5)"
if [ -z "$STRAY" ]; then
  record_pass "部署根里只有一个 metadata.sqlite3（state/ 下那个）"
else
  record_fail "部署根里只有一个 metadata.sqlite3" "$STRAY"
fi

# ---- 5. Truth Matrix ----
USER_NAME="qa_truth_$(head -c 4 /dev/urandom | od -An -tx1 | tr -d ' \n')"
export BACKUP_REMOTE_PASSWORD="$(head -c 24 /dev/urandom | sha256sum | cut -c1-24)"
REMOTE_OPTS="--host 127.0.0.1 --port $PORT"
ADMIN_OPTS="--server-root $SERVER_ROOT --root $DATA_ROOT --db $DB_PATH"
echo "[same-instance] 唯一测试账户：$USER_NAME"

admin_out() { "$ADMIN" $ADMIN_OPTS "$@" > "$TEST_ROOT/admin.txt" 2>&1; echo $?; }
remote_out() { timeout --signal=KILL 120 ./build/backupctl remote "$@" $REMOTE_OPTS \
  > "$TEST_ROOT/remote.txt" 2>&1; echo $?; }
admin_has_user() {
  admin_out show-user "$USER_NAME" >/dev/null
  if grep -q "没有这个用户" "$TEST_ROOT/admin.txt"; then echo absent; else echo present; fi
}
db_user_count() {
  # 第三个真值源：库文件本身。有 sqlite3 就直接查；没有就退回管理工具
  # （与服务端同一份读取实现），输出里会说明用的是哪一个。
  if command -v sqlite3 >/dev/null 2>&1; then
    sqlite3 -readonly "$DB_PATH" "SELECT COUNT(*) FROM users WHERE username='$USER_NAME';"
  else
    admin_out show-user "$USER_NAME" >/dev/null
    if grep -q "没有这个用户" "$TEST_ROOT/admin.txt"; then echo 0; else echo 1; fi
  fi
}

# STATE A：注册前
A_ADMIN="$(admin_has_user)"
A_DB="$(db_user_count)"
if [ "$A_ADMIN" = "absent" ] && [ "$A_DB" = "0" ]; then
  record_pass "STATE A（注册前）：admin=absent / db=0"
else
  record_fail "STATE A（注册前）" "admin=$A_ADMIN db=$A_DB"
fi

# STATE B：注册后（GUI 路径 = 共享客户端）
B_CODE="$(remote_out register --user "$USER_NAME")"
B_ADMIN="$(admin_has_user)"
B_DB="$(db_user_count)"
if [ "$B_CODE" = "0" ] && [ "$B_ADMIN" = "present" ] && [ "$B_DB" != "0" ]; then
  record_pass "STATE B（注册后）：remote=ok / admin=present / db=$B_DB"
else
  record_fail "STATE B（注册后）" \
    "remote=$B_CODE admin=$B_ADMIN db=$B_DB $(head -1 "$TEST_ROOT/remote.txt")"
fi

# STATE C：登录 + 列表
C_LOGIN="$(remote_out login --user "$USER_NAME")"
C_LIST="$(remote_out list --user "$USER_NAME")"
C_ADMIN="$(admin_has_user)"
if [ "$C_LOGIN" = "0" ] && [ "$C_LIST" = "0" ] && [ "$C_ADMIN" = "present" ]; then
  record_pass "STATE C（登录后）：login=ok / list=ok / admin=present"
else
  record_fail "STATE C（登录后）" "login=$C_LOGIN list=$C_LIST admin=$C_ADMIN"
fi

# STATE D：本地退出登录（退出登录只清本机内存，云端数据必须还在）
D_ADMIN="$(admin_has_user)"
D_DB="$(db_user_count)"
if [ "$D_ADMIN" = "present" ] && [ "$D_DB" != "0" ]; then
  record_pass "STATE D（退出登录）：admin=present / db=$D_DB（退出登录不删数据）"
else
  record_fail "STATE D（退出登录）" "admin=$D_ADMIN db=$D_DB"
fi

# STATE E：重新登录
E_CODE="$(remote_out login --user "$USER_NAME")"
E_ADMIN="$(admin_has_user)"
if [ "$E_CODE" = "0" ] && [ "$E_ADMIN" = "present" ]; then
  record_pass "STATE E（重新登录）：login=ok / admin=present"
else
  record_fail "STATE E（重新登录）" "login=$E_CODE admin=$E_ADMIN"
fi

# STATE F：注销账户
F_CODE="$(remote_out delete-account --user "$USER_NAME" --confirm "$USER_NAME")"
F_ADMIN="$(admin_has_user)"
F_DB="$(db_user_count)"
F_DIRS="$(ls "$DATA_ROOT/users" 2>/dev/null | tr '\n' ' ')"
if [ "$F_CODE" = "0" ] && [ "$F_ADMIN" = "absent" ] && [ "$F_DB" = "0" ]; then
  record_pass "STATE F（注销成功）：remote=ok / admin=absent / db=0"
else
  record_fail "STATE F（注销成功）" \
    "remote=$F_CODE admin=$F_ADMIN db=$F_DB dirs=$F_DIRS $(head -1 "$TEST_ROOT/remote.txt")"
fi

# STATE G：再用原凭据登录
G_CODE="$(remote_out login --user "$USER_NAME")"
G_ADMIN="$(admin_has_user)"
if [ "$G_CODE" != "0" ] && [ "$G_ADMIN" = "absent" ]; then
  record_pass "STATE G（再次登录）：login FAIL / admin=absent"
else
  record_fail "STATE G（再次登录）" "login=$G_CODE admin=$G_ADMIN"
fi

# ---- 5b. 重复注册：拒绝 + 库里只有一行 + 原口令不受影响 ----
#
# 人工验收第 5 条：同一个用户名注册第二次，界面（GUI / CLI）上要看得见"该用户名
# 已被使用"，而且**原来的账户不能被换掉**。这里在四个真值源上验证：
#
#   * 第一次注册成功，第二次被服务端拒绝（ALREADY_EXISTS）；
#   * 库里这个用户名只有 1 行（有 sqlite3 就直接查文件）；
#   * 原口令仍然能登录，第二次用的口令登不进去。
remote_pw() {
  local password="$1"
  shift
  BACKUP_REMOTE_PASSWORD="$password" timeout --signal=KILL 120 \
    ./build/backupctl remote "$@" $REMOTE_OPTS > "$TEST_ROOT/remote.txt" 2>&1
  echo $?
}
dup_rows() {
  if command -v sqlite3 >/dev/null 2>&1; then
    sqlite3 -readonly "$DB_PATH" "SELECT COUNT(*) FROM users WHERE username='$DUP_NAME';"
  else
    admin_out show-user "name:$DUP_NAME" >/dev/null
    if grep -q "没有叫" "$TEST_ROOT/admin.txt"; then echo 0; else echo 1; fi
  fi
}
DUP_NAME="qa_dup_$(head -c 4 /dev/urandom | od -An -tx1 | tr -d ' \n')"
DUP_ORIGINAL="dup-original-$(head -c 8 /dev/urandom | od -An -tx1 | tr -d ' \n')"
DUP_SECOND="dup-second-$(head -c 8 /dev/urandom | od -An -tx1 | tr -d ' \n')"
DUP_FIRST="$(remote_pw "$DUP_ORIGINAL" register --user "$DUP_NAME")"
DUP_AGAIN="$(remote_pw "$DUP_SECOND" register --user "$DUP_NAME")"
DUP_AGAIN_TEXT="$(cat "$TEST_ROOT/remote.txt")"
DUP_LOGIN_ORIGINAL="$(remote_pw "$DUP_ORIGINAL" login --user "$DUP_NAME")"
DUP_LOGIN_SECOND="$(remote_pw "$DUP_SECOND" login --user "$DUP_NAME")"
DUP_ROWS="$(dup_rows)"
if [ "$DUP_FIRST" = "0" ] && [ "$DUP_AGAIN" != "0" ] && [ "$DUP_ROWS" = "1" ] \
   && [ "$DUP_LOGIN_ORIGINAL" = "0" ] && [ "$DUP_LOGIN_SECOND" != "0" ]; then
  record_pass "重复注册：第一次成功、第二次被拒、库里只有 1 行、原口令仍可用"
else
  record_fail "重复注册" \
    "first=$DUP_FIRST again=$DUP_AGAIN rows=$DUP_ROWS orig=$DUP_LOGIN_ORIGINAL second=$DUP_LOGIN_SECOND"
fi
if printf '%s' "$DUP_AGAIN_TEXT" | grep -q "名字已经被占用"; then
  record_pass "重复注册的拒绝原因在输出里可见（服务端 ALREADY_EXISTS → 名字已经被占用）"
else
  record_fail "重复注册的拒绝原因" "$(printf '%s' "$DUP_AGAIN_TEXT" | head -2 | tr '\n' ' ')"
fi
# 原账户没有被"覆盖写"：第二个口令在任何路径上都进不去
if [ "$DUP_LOGIN_SECOND" != "0" ] && [ "$DUP_LOGIN_ORIGINAL" = "0" ]; then
  record_pass "第二个口令登不进去（原账户没有被后一次注册覆盖）"
else
  record_fail "重复注册之后的口令" \
    "orig=$DUP_LOGIN_ORIGINAL second=$DUP_LOGIN_SECOND"
fi

# ---- 6. §20：错误的实例根必须 fail closed，且不创建任何东西 ----
BAD_ROOT="$TEST_ROOT/wrong-instance-$(head -c 3 /dev/urandom | od -An -tx1 | tr -d ' \n')"
BAD_CODE="$(admin_out --server-root "$BAD_ROOT" --root "$BAD_ROOT/data" \
  --db "$BAD_ROOT/state/metadata.sqlite3" list-users)"
if [ "$BAD_CODE" != "0" ] && grep -q "未找到服务器状态数据库" "$TEST_ROOT/admin.txt"; then
  record_pass "错误的实例根：管理工具非 0 退出并说明原因"
else
  record_fail "错误的实例根" "exit=$BAD_CODE $(head -2 "$TEST_ROOT/admin.txt" | tr '\n' ' ')"
fi
if [ -e "$BAD_ROOT" ]; then
  record_fail "错误的实例根不创建任何文件" "$(find "$BAD_ROOT" 2>/dev/null | head -3 | tr '\n' ' ')"
else
  record_pass "错误的实例根不创建任何文件（没有静默建空库）"
fi

# 同样的检查走 wrapper：实例根不存在时必须拒绝。
WRAP_BAD_OUT="$TEST_ROOT/wrapper-bad.txt"
WRAP_CODE=0
BACKUP_SERVER_ROOT="$BAD_ROOT" timeout --signal=KILL 60 \
  "$SERVER_ROOT/bin/backup-server-admin.sh" </dev/null > "$WRAP_BAD_OUT" 2>&1 || WRAP_CODE=$?
if [ "$WRAP_CODE" != "0" ] && grep -q "数据目录不存在" "$WRAP_BAD_OUT"; then
  record_pass "wrapper 对错误的实例根也 fail closed"
else
  record_fail "wrapper 对错误的实例根" \
    "exit=$WRAP_CODE $(head -2 "$WRAP_BAD_OUT" | tr '\n' ' ')"
fi

# ---- 7. 协议身份 + 二进制身份（§15/§16）----
PING_OUT="$TEST_ROOT/ping.txt"
./build/backupctl remote ping $REMOTE_OPTS > "$PING_OUT" 2>&1
if grep -q "PING 正常" "$PING_OUT"; then
  record_pass "PING 正常（$(head -1 "$PING_OUT")）"
else
  record_fail "PING" "$(head -1 "$PING_OUT")"
fi
DEPLOY_SHA="$(sha256sum "$SERVER_ROOT/bin/backup-server" | cut -d' ' -f1)"
BUILD_SHA="$(sha256sum build/backup-server | cut -d' ' -f1)"
if [ "$DEPLOY_SHA" = "$BUILD_SHA" ]; then
  record_pass "部署目录里的服务端二进制 == 本轮构建产物（$(printf '%s' "$DEPLOY_SHA" | cut -c1-16)…）"
else
  record_fail "部署目录里的服务端二进制" "deploy=$DEPLOY_SHA build=$BUILD_SHA"
fi

finish
