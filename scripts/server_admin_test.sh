#!/usr/bin/env bash
#
# PR #20（closure）ECS 本地管理工具 backup-server-admin 的专项测试。
#
# 被测对象是"只能在服务器本机运行"的管理工具：它不监听端口、不说 BPNET1、
# 不在 GUI / CLI 的调用路径上。13 项覆盖与交付报告一一对应：
#
#   1. 普通 Ubuntu 客户端无法调用管理员功能
#   2. BPNET1 里不存在 admin 操作码
#   3. 管理工具不新增任何 TCP listener
#   4. 管理工具可以在本机运行（--help / status）
#   5. list-users 正确（id / 用户名 / 备份数 / 占用）
#   6. list-snapshots 正确
#   7. 删除单个 snapshot：blob 与元数据一致
#   8. 删除 user：该用户全部数据删除
#   9. 删除 User A：User B 完全不受影响
#  10. 危险输入：路径穿越不被接受
#  11. 用户输入不会直接变成文件系统路径
#  12. 输出里不出现 password hash / salt / token secret
#  13. backup-server 正在运行时，破坏性操作被拒绝（内核锁，不是 pgrep 猜的）

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "$BASH_SOURCE")/.." && pwd)"
cd "$ROOT_DIR"
TEST_ROOT="$ROOT_DIR/testdata/server-admin"
rm -rf "$TEST_ROOT"
mkdir -p "$TEST_ROOT/data" "$TEST_ROOT/state" "$TEST_ROOT/logs" \
  "$TEST_ROOT/repo" "$TEST_ROOT/src"

PASS=0
FAIL=0
record_pass() { PASS=$((PASS + 1)); echo "  PASS  $1"; }
record_fail() { FAIL=$((FAIL + 1)); echo "  FAIL  $1 -- $2" >&2; }

echo "[server-admin] PR #20 ECS 本地管理工具"

DATA="$TEST_ROOT/data"
DB="$TEST_ROOT/state/metadata.sqlite3"
SECRET="$TEST_ROOT/secrets.env"
LOG="$TEST_ROOT/logs/server.log"
ADMIN="./build/backup-server-admin"
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
  echo "[server-admin] 合计: $PASS passed, $FAIL failed"
  [ "$FAIL" = "0" ] || exit 1
  echo "SERVER_ADMIN_ALL_PASS"
}

if ! make -j4 all server > "$TEST_ROOT/build.log" 2>&1; then
  record_fail "make all server" "$(tail -3 "$TEST_ROOT/build.log" | tr '\n' ' ')"
  finish
fi
if [ ! -x "$ADMIN" ]; then
  record_fail "管理工具已构建" "$ADMIN 不存在"
  finish
fi
record_pass "make all server 成功（warning $(grep -ci warning "$TEST_ROOT/build.log" || true) 条）"

run_admin() {
  timeout --signal=KILL 120 "$ADMIN" --root "$DATA" --db "$DB" "$@" \
    >"$TEST_ROOT/admin.txt" 2>&1
  echo $?
}

run_remote() {
  local password="$1"
  shift
  BACKUP_REMOTE_PASSWORD="$password" timeout --signal=KILL 180 \
    ./build/backupctl remote "$@" --host 127.0.0.1 --port "$PORT" \
    >"$TEST_ROOT/last.txt" 2>&1
  echo $?
}

# ---- 1. 普通客户端无法调用管理员功能 ----
if grep -qE 'backup-server-admin|RemoteMaintenance|remote_maintenance|admin_main' \
     src/cli/remote_commands.cpp src/network/remote_backup_client.cpp \
     ui/modern/remote_controller.cpp ui/modern/qml/pages/RemotePage.qml 2>/dev/null; then
  record_fail "客户端里没有管理员入口" "客户端源码里出现了管理工具的引用"
else
  record_pass "客户端（CLI / 共享客户端 / GUI）里没有任何管理员入口"
fi
ADMIN_AS_SUBCOMMAND="$(BACKUP_REMOTE_PASSWORD=x timeout 60 ./build/backupctl remote \
  admin list-users --host 127.0.0.1 --port 1 2>&1)"
if printf '%s' "$ADMIN_AS_SUBCOMMAND" | grep -q "未知的 remote 子命令"; then
  record_pass "backupctl remote 不认识 admin 子命令"
else
  record_fail "backupctl remote admin" "居然被当成合法子命令"
fi

# ---- 2. BPNET1 里没有 admin 操作码（运行时由 network_protocol_test 断言，
#         这里做静态复核：协议源码里没有任何 ADMIN_* 名字） ----
if grep -nE 'ADMIN|kAdmin|admin' include/network_protocol.h \
     src/network/network_protocol.cpp | grep -vE '^[^:]+:[0-9]+:[[:space:]]*//' | grep -q .; then
  record_fail "协议里没有管理员操作码" "协议源码里出现了 admin 字样"
else
  record_pass "协议里没有管理员操作码（BPNET1 没有 ADMIN_*）"
fi

# ---- 3. 管理工具不新增任何 TCP listener ----
if grep -nE 'socket\(|::bind\(|listen\(|accept\(' server/admin_main.cpp \
     | grep -vE '^[0-9]+:[[:space:]]*//' | grep -q .; then
  record_fail "管理工具没有监听代码" "admin_main.cpp 里出现了 socket/bind/listen/accept"
else
  record_pass "管理工具源码里没有 socket() / bind() / listen() / accept()"
fi
LISTEN_BEFORE="$(ss -ltn 2>/dev/null | awk '{print $4}' | sort)"
run_admin status >/dev/null
run_admin overview >/dev/null
run_admin list-users >/dev/null
LISTEN_AFTER="$(ss -ltn 2>/dev/null | awk '{print $4}' | sort)"
if [ "$LISTEN_BEFORE" = "$LISTEN_AFTER" ]; then
  record_pass "运行管理命令前后，系统里的监听端口集合完全一致"
else
  record_fail "运行管理命令前后的监听端口" "出现了新的 listener"
fi

# ---- 4. 管理工具可以在本机运行 ----
HELP_CODE="$(timeout 60 "$ADMIN" --help >/dev/null 2>&1; echo $?)"
if [ "$HELP_CODE" = "0" ]; then
  record_pass "backup-server-admin --help 正常"
else
  record_fail "backup-server-admin --help" "退出码 $HELP_CODE"
fi

# ---- 准备数据：真实服务端 + 两个用户 + 三份归档 ----
head -c 32 /dev/urandom | sha256sum | cut -c1-64 \
  | sed 's/^/BACKUP_TOKEN_SECRET=/' > "$SECRET"
chmod 600 "$SECRET"

PORT=""
for candidate in $(seq 20150 20180); do
  if ! ss -ltn 2>/dev/null | grep -q ":$candidate "; then PORT="$candidate"; break; fi
done
[ -n "$PORT" ] || { record_fail "端口" "20150-20180 都被占用"; finish; }

./build/backup-server --bind 127.0.0.1 --port "$PORT" --root "$DATA" --db "$DB" \
  --secret-file "$SECRET" --pid-file "$TEST_ROOT/state/server.pid" \
  --log-file "$LOG" --quiet > "$TEST_ROOT/logs/server-stderr.log" 2>&1 &
SERVER_PID=$!
ready=0
for _ in $(seq 1 50); do
  if ss -ltn 2>/dev/null | grep -q "127.0.0.1:$PORT "; then ready=1; break; fi
  sleep 0.1
done
[ "$ready" = "1" ] || { record_fail "服务端监听" "没有起来"; finish; }

USER_A="sadm-a-$$"
USER_B="sadm-b-$$"
PW_A="$(head -c 24 /dev/urandom | sha256sum | cut -c1-24)"
PW_B="$(head -c 24 /dev/urandom | sha256sum | cut -c1-24)"

printf 'admin-test\n' > "$TEST_ROOT/src/a.txt"
head -c 4096 /dev/urandom > "$TEST_ROOT/src/blob.bin"
./build/backupctl config repository set "$TEST_ROOT/repo" \
  --config-file "$TEST_ROOT/config.json" >"$TEST_ROOT/last.txt" 2>&1
./build/backupctl backup "$TEST_ROOT/src" --config-file "$TEST_ROOT/config.json" \
  >"$TEST_ROOT/backup.txt" 2>&1
ARCHIVE="$(ls -1 "$TEST_ROOT/repo"/*.bak 2>/dev/null | head -1)"
[ -n "$ARCHIVE" ] || { record_fail "准备归档" "$(head -1 "$TEST_ROOT/backup.txt")"; finish; }
ARCHIVE_SHA="$(sha256sum "$ARCHIVE" | cut -d' ' -f1)"

setup_ok=1
[ "$(run_remote "$PW_A" register --user "$USER_A")" = "0" ] || setup_ok=0
[ "$(run_remote "$PW_A" upload "$ARCHIVE" --user "$USER_A" --repository "$TEST_ROOT/repo")" = "0" ] || setup_ok=0
SNAP_A="$(grep -m1 '快照 ID:' "$TEST_ROOT/last.txt" | awk '{print $3}')"
[ "$(run_remote "$PW_B" register --user "$USER_B")" = "0" ] || setup_ok=0
[ "$(run_remote "$PW_B" upload "$ARCHIVE" --user "$USER_B" --repository "$TEST_ROOT/repo")" = "0" ] || setup_ok=0
SNAP_B1="$(grep -m1 '快照 ID:' "$TEST_ROOT/last.txt" | awk '{print $3}')"
[ "$(run_remote "$PW_B" upload "$ARCHIVE" --user "$USER_B" --repository "$TEST_ROOT/repo")" = "0" ] || setup_ok=0
SNAP_B2="$(grep -m1 '快照 ID:' "$TEST_ROOT/last.txt" | awk '{print $3}')"
if [ "$setup_ok" = "1" ] && [ -n "$SNAP_A" ] && [ -n "$SNAP_B1" ] && [ -n "$SNAP_B2" ]; then
  record_pass "准备完成：两个用户、三份云端快照"
else
  record_fail "准备数据" "$(head -1 "$TEST_ROOT/last.txt")"
  finish
fi

# ---- 5. list-users 正确 ----
run_admin list-users >/dev/null
USER_A_ID="$(awk -v name="$USER_A" '$2 == name { print $1 }' "$TEST_ROOT/admin.txt")"
USER_B_ID="$(awk -v name="$USER_B" '$2 == name { print $1 }' "$TEST_ROOT/admin.txt")"
COUNT_B="$(awk -v name="$USER_B" '$2 == name { print $5 }' "$TEST_ROOT/admin.txt")"
if [ -n "$USER_A_ID" ] && [ -n "$USER_B_ID" ] && [ "$COUNT_B" = "2" ]; then
  record_pass "list-users 显示 id / 用户名 / 备份数（B 有 2 份）"
else
  record_fail "list-users" "$(head -4 "$TEST_ROOT/admin.txt" | tr '\n' ' ')"
fi

# ---- 6. list-snapshots 正确 ----
run_admin list-snapshots "$USER_B" >/dev/null
if grep -q "$SNAP_B1" "$TEST_ROOT/admin.txt" && grep -q "$SNAP_B2" "$TEST_ROOT/admin.txt"; then
  record_pass "list-snapshots 按用户列出两份快照"
else
  record_fail "list-snapshots" "$(head -4 "$TEST_ROOT/admin.txt" | tr '\n' ' ')"
fi
run_admin show-snapshot "$SNAP_B1" >/dev/null
if grep -q "$ARCHIVE_SHA" "$TEST_ROOT/admin.txt"; then
  record_pass "show-snapshot 详情里有完整 SHA-256（与本地归档一致）"
else
  record_fail "show-snapshot 详情" "$(head -6 "$TEST_ROOT/admin.txt" | tr '\n' ' ')"
fi

# ---- 12（运行时部分）：输出里没有口令字段、也没有 secret ----
SECRET_VALUE="$(cut -d= -f2 "$SECRET")"
LEAK=0
for command in "status" "list-users" "overview" "show-user $USER_A" "list-snapshots $USER_A"; do
  # shellcheck disable=SC2086
  run_admin $command >/dev/null
  if grep -qiE 'password|salt|iterations|token|secret' "$TEST_ROOT/admin.txt" \
     || grep -qF "$SECRET_VALUE" "$TEST_ROOT/admin.txt"; then
    LEAK=1
  fi
done
if [ "$LEAK" = "0" ]; then
  record_pass "管理工具的输出里没有口令字段、token 或 secret"
else
  record_fail "管理工具的输出" "输出里出现了 password / salt / token / secret"
fi
if grep -nE 'password_salt|password_hash|password_iterations|BACKUP_TOKEN_SECRET' \
     server/admin_main.cpp | grep -vE '^[0-9]+:[[:space:]]*//' | grep -q .; then
  record_fail "管理工具源码不碰口令字段" "admin_main.cpp 里出现了口令字段名"
else
  record_pass "管理工具源码里一次都没有出现 salt / hash / secret 的字段名"
fi

# ---- 13. 服务端运行时，破坏性操作被拒绝 ----
REFUSE="$(run_admin delete-user "$USER_B" --confirm "DELETE $USER_B")"
if [ "$REFUSE" != "0" ] && grep -q "请先在服务器上停止 backup-server" "$TEST_ROOT/admin.txt"; then
  record_pass "backup-server 运行时 delete-user 被拒绝并提示先停服务"
else
  record_fail "服务端运行时的 delete-user" "$(head -2 "$TEST_ROOT/admin.txt" | tr '\n' ' ')"
fi
REFUSE_SNAP="$(run_admin delete-snapshot "$SNAP_B1" --user "$USER_B" --confirm "$SNAP_B1")"
if [ "$REFUSE_SNAP" != "0" ]; then
  record_pass "backup-server 运行时 delete-snapshot 也被拒绝"
else
  record_fail "服务端运行时的 delete-snapshot" "本应失败却成功了"
fi
run_admin list-snapshots "$USER_B" >/dev/null
if grep -q "$SNAP_B1" "$TEST_ROOT/admin.txt"; then
  record_pass "被拒绝之后数据完好（两份快照都还在）"
else
  record_fail "被拒绝之后的数据" "快照不见了"
fi

# 数据目录锁：同一个 root 上不允许起第二个服务端。
SECOND_PORT=$((PORT + 1))
./build/backup-server --bind 127.0.0.1 --port "$SECOND_PORT" --root "$DATA" --db "$DB" \
  --secret-file "$SECRET" --quiet > "$TEST_ROOT/logs/second.log" 2>&1 &
SECOND_PID=$!
sleep 1
if kill -0 "$SECOND_PID" 2>/dev/null; then
  record_fail "数据目录锁" "第二个服务端居然还在运行"
  kill -TERM "$SECOND_PID" 2>/dev/null
  wait "$SECOND_PID" 2>/dev/null
else
  wait "$SECOND_PID" 2>/dev/null
  if grep -q "already using the data directory" "$TEST_ROOT/logs/second.log"; then
    record_pass "同一个数据目录上的第二个服务端拒绝启动（flock，不是 pgrep）"
  else
    record_fail "数据目录锁" "$(head -2 "$TEST_ROOT/logs/second.log" | tr '\n' ' ')"
  fi
fi

# ---- 停服务，开始做破坏性操作 ----
kill -TERM "$SERVER_PID" 2>/dev/null
wait "$SERVER_PID" 2>/dev/null
SERVER_PID=""
record_pass "服务端已优雅停止"

BLOB_A="$DATA/users/$USER_A_ID/$SNAP_A.bak"
BLOB_B1="$DATA/users/$USER_B_ID/$SNAP_B1.bak"
BLOB_B2="$DATA/users/$USER_B_ID/$SNAP_B2.bak"

# ---- 7. 删除单个 snapshot：blob 与元数据一致 ----
DELETE_SNAP="$(run_admin delete-snapshot "$SNAP_B1" --user "$USER_B" --confirm "$SNAP_B1")"
if [ "$DELETE_SNAP" = "0" ]; then
  record_pass "delete-snapshot 成功"
else
  record_fail "delete-snapshot" "$(head -2 "$TEST_ROOT/admin.txt" | tr '\n' ' ')"
fi
if [ ! -f "$BLOB_B1" ] && [ -f "$BLOB_B2" ] && [ -f "$BLOB_A" ]; then
  record_pass "被删的那一份 blob 消失，别的 blob 一个都没动"
else
  record_fail "删除单个快照之后的磁盘状态" "文件状态不对"
fi
run_admin list-snapshots "$USER_B" >/dev/null
if ! grep -q "$SNAP_B1" "$TEST_ROOT/admin.txt" && grep -q "$SNAP_B2" "$TEST_ROOT/admin.txt"; then
  record_pass "元数据与磁盘一致：只剩没被删的那一份"
else
  record_fail "删除单个快照之后的元数据" "$(head -4 "$TEST_ROOT/admin.txt" | tr '\n' ' ')"
fi
if ls "$DATA/users/$USER_B_ID/trash" 2>/dev/null | grep -q '.deleted'; then
  record_fail "删除之后 trash 里没有残留" "还有 .deleted 文件"
else
  record_pass "删除之后 trash 里没有残留"
fi

# ---- 10. 危险输入：路径穿越不被接受 ----
TRAVERSAL_1="$(run_admin show-user '../../etc/passwd')"
TRAVERSAL_2="$(run_admin show-snapshot '../../../etc/passwd')"
TRAVERSAL_3="$(run_admin list-snapshots '../../etc')"
if [ "$TRAVERSAL_1" != "0" ] && [ "$TRAVERSAL_2" != "0" ] && [ "$TRAVERSAL_3" != "0" ]; then
  record_pass "路径穿越形式的用户 / 快照参数一律被拒绝"
else
  record_fail "路径穿越输入" "居然有一个成功了"
fi
if [ ! -e "$TEST_ROOT/etc" ] && [ ! -e "$DATA/users/../../etc" ]; then
  record_pass "穿越输入没有被当成路径使用（没有产生任何路径效应）"
else
  record_fail "穿越输入的路径效应" "出现了预料之外的文件"
fi

# ---- 11. 用户输入不会直接变成文件系统路径 ----
if grep -nE 'root_directory_? *\+ *(username|selector|user_input)|\+ *selector *\+' \
     server/admin_main.cpp server/remote_maintenance.cpp | grep -q .; then
  record_fail "用户输入不拼路径" "出现了把用户输入拼进路径的代码"
else
  record_pass "磁盘路径只由数字 user id + 服务端生成的 storage_name 拼成"
fi
if grep -q 'std::to_string(user_id)' server/remote_maintenance.cpp; then
  record_pass "维护层用数字 user id 拼路径（用户名不参与）"
else
  record_fail "维护层的路径构造" "没有找到基于数字 id 的路径构造"
fi

# ---- 8/9. 删除 user：A 全部数据删除，B 完全不受影响 ----
DELETE_USER="$(run_admin delete-user "$USER_A" --confirm "DELETE $USER_A")"
if [ "$DELETE_USER" = "0" ]; then
  record_pass "delete-user 成功"
else
  record_fail "delete-user" "$(head -2 "$TEST_ROOT/admin.txt" | tr '\n' ' ')"
fi
run_admin list-users >/dev/null
if ! awk -v name="$USER_A" '$2 == name { found = 1 } END { exit found ? 0 : 1 }' \
     "$TEST_ROOT/admin.txt"; then
  record_pass "被删用户从 list-users 里消失"
else
  record_fail "被删用户" "还在列表里"
fi
if [ ! -f "$BLOB_A" ] && [ ! -d "$DATA/users/$USER_A_ID" ]; then
  record_pass "被删用户的 blob 与目录都从磁盘上消失"
else
  record_fail "被删用户的磁盘数据" "文件或目录还在"
fi
if awk -v name="$USER_B" '$2 == name { found = 1 } END { exit found ? 0 : 1 }' \
     "$TEST_ROOT/admin.txt" && [ -f "$BLOB_B2" ] \
   && [ "$(sha256sum "$BLOB_B2" | cut -d' ' -f1)" = "$ARCHIVE_SHA" ] \
   && [ -d "$DATA/users/$USER_B_ID" ]; then
  record_pass "删除 User A 之后 User B 的账户、目录与 blob 逐字节未变"
else
  record_fail "跨用户隔离" "User B 受到了影响"
fi
if ls "$DATA/trash" 2>/dev/null | grep -q '^account-'; then
  record_fail "账户注销之后 trash 里没有残留" "还有 account-* 目录"
else
  record_pass "账户注销之后 trash 里没有残留"
fi

# ---- 12（补充）：删除操作之后，日志里也没有 secret ----
if grep -qF "$SECRET_VALUE" "$LOG" 2>/dev/null; then
  record_fail "服务端日志里没有 secret" "日志里出现了 secret"
else
  record_pass "服务端日志里没有出现过 token secret"
fi

finish
