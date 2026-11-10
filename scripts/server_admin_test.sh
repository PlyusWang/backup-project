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
#  14. 用户选择器：id:<编号> / name:<用户名> 明确指定；裸输入同时命中 id 与
#      用户名时拒绝并给出两种写法；删除操作必须显式指定（绝不猜）
#  15. 只读命令（status / list-users / show-user / list-snapshots / overview）
#      是**真的**只读：服务端运行时可用，而且不产生写事务（连接是 READONLY）
#  16. 源码树里的 backup-server-admin.sh 不再猜实例：没有 BACKUP_SERVER_ROOT
#      就 fail closed，不会去读另一个实例

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR" || exit 1
TEST_ROOT="${BACKUP_SERVER_ADMIN_TEST_ROOT:-$(mktemp -d "${TMPDIR:-/tmp}/bp-server-admin-test.XXXXXX")}"
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

# ---- PR #21：BPSEC1 服务端身份密钥 + 客户端 pin ----
# 服务端必须带 --transport-key-file 才能启动；客户端必须先知道服务端公钥指纹
# （pin），没有就直接用法错误退出（不做 TOFU）。密钥只生成在测试自己的临时
# 目录里，私钥内容不打印；pin 从 keygen 输出里读进环境变量，不写明文常量。
# 这一段必须在**任何** backupctl remote 调用之前：pin 缺失时 remote 会以
# 用法错误退出，那样下面"不认识 admin 子命令"之类的断言就会被 pin 错误顶掉。
KEYGEN="./build/backup-server-keygen"
# 与部署布局一致：身份私钥在 <server-root>/state/ 下（deploy_aliyun_server.sh
# 就是把它装在 state/transport.key）。管理工具的"部署约定路径"因此也指向这里。
TRANSPORT_KEY="$TEST_ROOT/state/transport.key"
KEYGEN_OUT="$TEST_ROOT/keygen.out"
if [ ! -x "$KEYGEN" ]; then
  record_fail "backup-server-keygen 已构建" "$KEYGEN 不存在（make server 应该产出它）"
  finish
fi
if ! "$KEYGEN" --output "$TRANSPORT_KEY" > "$KEYGEN_OUT" 2>&1; then
  record_fail "生成服务端传输身份密钥" "$(head -2 "$KEYGEN_OUT" | tr '\n' ' ')"
  finish
fi
KEY_MODE="$(stat -c '%a' "$TRANSPORT_KEY" 2>/dev/null || echo missing)"
if [ "$KEY_MODE" = "600" ]; then
  record_pass "服务端传输身份私钥已生成且权限 0600"
else
  record_fail "传输身份私钥权限" "期望 0600，实际 $KEY_MODE"
  finish
fi
BACKUP_REMOTE_SERVER_KEY="$(grep -oE 'sha256:[0-9a-f]{64}' "$KEYGEN_OUT" | head -1)"
export BACKUP_REMOTE_SERVER_KEY
if [ -n "$BACKUP_REMOTE_SERVER_KEY" ]; then
  record_pass "客户端 pin 已从 keygen 输出读入环境变量 BACKUP_REMOTE_SERVER_KEY（不打印、不写常量）"
else
  record_fail "客户端 pin" "keygen 输出里没有 sha256:<64 位十六进制> 指纹"
  finish
fi

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
# PR #21：pin 是**必填项**。上面那条断言能继续成立，靠的正是它带了 pin
# （环境变量），所以这里再用 env -u 摘掉 pin 证明：没有 pin 时命令在连接
# 之前就以用法错误（退出码 2）失败，错误信息里能看到 --server-key。
NO_PIN_CODE=0
env -u BACKUP_REMOTE_SERVER_KEY timeout 60 ./build/backupctl remote \
  ping --host 127.0.0.1 --port 1 > "$TEST_ROOT/no-pin.txt" 2>&1 \
  || NO_PIN_CODE=$?
if [ "$NO_PIN_CODE" = "2" ] && grep -q -- "--server-key" "$TEST_ROOT/no-pin.txt"; then
  record_pass "没有 pin 时 remote 以用法错误退出（退出码 2，提示 --server-key）"
else
  record_fail "没有 pin 时的 remote" \
    "退出码 $NO_PIN_CODE $(head -2 "$TEST_ROOT/no-pin.txt" | tr '\n' ' ')"
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
  --secret-file "$SECRET" --transport-key-file "$TRANSPORT_KEY" \
  --pid-file "$TEST_ROOT/state/server.pid" \
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

# ---- 4b. 传输身份指纹：首页就能抄到（PR #21 UI closure，问题 A）----
#
# 客户端的"服务器身份指纹（server-key）"是必填项，所以服务器本机的管理工具
# 必须在**首页**把它显示出来，用户才抄得走。这里钉死三件事：
#
#   1. 首页（status，也就是菜单 banner 调用的那条命令）里就有
#      "传输身份指纹: sha256:<64 位十六进制>"；
#   2. 它和客户端用来连接**这个正在运行的服务端**的 pin 完全相同——这就是
#      "显示的指纹 == 服务端在用的身份"的运行时证据（管理工具只读
#      /proc/<pid>/cmdline 找到服务端实际加载的那把私钥）；
#   3. 详细页（transport-identity）给出 Fingerprint 与 Public key，但**永远
#      不打印私钥**：私钥文件的十六进制在这里单独算一份，必须一个字都搜不到。
ADMIN_STATUS_CODE="$(run_admin status)"
STATUS_FP="$(grep -oE 'sha256:[0-9a-f]{64}' "$TEST_ROOT/admin.txt" | head -1)"
if [ "$ADMIN_STATUS_CODE" = "0" ] && [ "$STATUS_FP" = "$BACKUP_REMOTE_SERVER_KEY" ] \
   && grep -q '^传输身份指纹: sha256:' "$TEST_ROOT/admin.txt"; then
  record_pass "管理工具首页显示传输身份指纹，且与客户端连接该服务端用的 pin 完全一致"
else
  record_fail "管理工具首页的传输身份指纹" \
    "exit=$ADMIN_STATUS_CODE 首页=$STATUS_FP pin=$BACKUP_REMOTE_SERVER_KEY"
fi
ADMIN_ID_CODE="$(run_admin transport-identity)"
ADMIN_PUB="$(grep -oE 'hex:[0-9a-f]{64}' "$TEST_ROOT/admin.txt" | head -1)"
KEYGEN_PUB="$(grep -oE 'hex:[0-9a-f]{64}' "$KEYGEN_OUT" | head -1)"
if [ "$ADMIN_ID_CODE" = "0" ] && [ "$ADMIN_PUB" = "$KEYGEN_PUB" ] \
   && grep -q '^Fingerprint:' "$TEST_ROOT/admin.txt" \
   && grep -q '^Public key:' "$TEST_ROOT/admin.txt" \
   && grep -qF "$BACKUP_REMOTE_SERVER_KEY" "$TEST_ROOT/admin.txt"; then
  record_pass "服务器身份信息页：Fingerprint 与 Public key 都与 keygen 一致"
else
  record_fail "服务器身份信息页" \
    "exit=$ADMIN_ID_CODE pub=$ADMIN_PUB keygen=$KEYGEN_PUB"
fi
PRIVATE_HEX="$(od -An -tx1 -v "$TRANSPORT_KEY" | tr -d ' \n')"
if [ -n "$PRIVATE_HEX" ] \
   && ! grep -qF "$PRIVATE_HEX" "$TEST_ROOT/admin.txt" \
   && ! grep -qiE 'private key|BEGIN [A-Z ]*PRIVATE' "$TEST_ROOT/admin.txt"; then
  record_pass "身份信息页只给公钥与指纹：私钥的 32 字节一次都没有出现"
else
  record_fail "身份信息页的私钥边界" "输出里出现了私钥材料或 private key 字样"
fi
# 读不到指纹不是"管理工具坏了"：明确说不可用 + 原因，其余输出照常，退出码仍是 0。
UNREADABLE_CODE="$(run_admin --transport-key-file "$TEST_ROOT/state/missing.key" status)"
if [ "$UNREADABLE_CODE" = "0" ] \
   && grep -q '^传输身份指纹: 不可用' "$TEST_ROOT/admin.txt" \
   && grep -q '原因:' "$TEST_ROOT/admin.txt" \
   && grep -q '用户数：' "$TEST_ROOT/admin.txt"; then
  record_pass '身份私钥读不到时：显示“不可用 + 原因”，菜单其余输出照常（退出码 0）'
else
  record_fail "身份私钥读不到时" \
    "exit=$UNREADABLE_CODE $(head -3 "$TEST_ROOT/admin.txt" | tr '\n' ' ')"
fi
# 菜单首页（wrapper 的 banner）里也能看到这一行——用户看的就是这一屏。
WRAPPER_HOME="$TEST_ROOT/wrapper-home.txt"
BACKUP_SERVER_ROOT="$TEST_ROOT" timeout --signal=KILL 60 \
  bash scripts/backup-server-admin.sh </dev/null > "$WRAPPER_HOME" 2>&1 || true
if grep -q "^传输身份指纹: $BACKUP_REMOTE_SERVER_KEY" "$WRAPPER_HOME" \
   && grep -q '5. 服务器身份与连接信息' "$WRAPPER_HOME"; then
  record_pass '菜单首页直接显示指纹，并且第 5 项就是“服务器身份信息”'
else
  record_fail "菜单首页" "$(head -8 "$WRAPPER_HOME" | tr '\n' ' ')"
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
REFUSE_SNAP="$(run_admin delete-snapshot "$SNAP_B1" --user "name:$USER_B" --confirm "$SNAP_B1")"
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

# ---- 15. 只读命令必须**真的**只读：服务端在跑也安全，而且不写库 ----
#
# 这一版修掉的 review finding：status / list-users / show-user / overview 以前走
# 的是带 SQLITE_OPEN_CREATE 的"能读也能写"的连接，打开时还会 EnsureSchema
# （BEGIN IMMEDIATE + CREATE TABLE + PRAGMA user_version）——一次货真价实的写事务。
# 现在只读命令用 SQLITE_OPEN_READONLY 的连接（外加 PRAGMA query_only），
# 破坏性命令才用可写连接。
#
# 运行时证据：这些命令在服务端**正在运行**时全部成功，而主库与 WAL 一个字节都
# 没有变。
DB_SHA_BEFORE="$(sha256sum "$DB" | cut -d' ' -f1)"
WAL_SIZE_BEFORE="$(stat -c %s "$DB-wal" 2>/dev/null || echo 0)"
RO_OK=1
for command in "status" "list-users" "overview" "show-user name:$USER_A" \
               "list-snapshots name:$USER_B"; do
  # shellcheck disable=SC2086
  [ "$(run_admin $command)" = "0" ] || RO_OK=0
done
if [ "$RO_OK" = "1" ]; then
  record_pass "服务端运行时 status / list-users / overview / show-user / list-snapshots 全部成功"
else
  record_fail "服务端运行时的只读命令" "$(head -3 "$TEST_ROOT/admin.txt" | tr '\n' ' ')"
fi
DB_SHA_AFTER="$(sha256sum "$DB" | cut -d' ' -f1)"
WAL_SIZE_AFTER="$(stat -c %s "$DB-wal" 2>/dev/null || echo 0)"
if [ "$DB_SHA_AFTER" = "$DB_SHA_BEFORE" ] \
   && [ "$WAL_SIZE_AFTER" = "$WAL_SIZE_BEFORE" ]; then
  record_pass "只读命令之后主库与 WAL 都没有变化（没有写事务）"
else
  record_fail "只读命令的写效应" \
    "db $DB_SHA_BEFORE->$DB_SHA_AFTER wal $WAL_SIZE_BEFORE->$WAL_SIZE_AFTER"
fi
# 一条只读命令的输出前后一致：不是"某几条其实读的是别的东西"。
run_admin list-users >/dev/null
RO_LIST_A="$(cat "$TEST_ROOT/admin.txt")"
run_admin list-users >/dev/null
RO_LIST_B="$(cat "$TEST_ROOT/admin.txt")"
if [ -n "$RO_LIST_A" ] && [ "$RO_LIST_A" = "$RO_LIST_B" ]; then
  record_pass "重复执行 list-users 的输出完全一致（只读视图稳定）"
else
  record_fail "只读视图的稳定性" "两次输出不同"
fi

# 源码级契约：两种打开方式是两个入口；共用的打开实现只**校验** schema。
RO_BODY="$(awk '/^bool RemoteMetadataStore::OpenExistingWithMode[(]/{inside=1} inside{print} inside && /^}$/{exit}' server/remote_metadata_store.cpp)"
RO_STATIC=1
printf '%s' "$RO_BODY" | grep -q "SQLITE_OPEN_READONLY" || RO_STATIC=0
printf '%s' "$RO_BODY" | grep -q "VerifyExistingSchema" || RO_STATIC=0
if printf '%s' "$RO_BODY" | grep -qE "EnsureSchema|BEGIN IMMEDIATE|CREATE TABLE"; then
  RO_STATIC=0
fi
grep -q "OpenExistingReadOnly" server/admin_main.cpp || RO_STATIC=0
grep -q "OpenExistingReadWrite" server/admin_main.cpp || RO_STATIC=0
if grep -n "OpenExisting(" server/admin_main.cpp server/remote_metadata_store.cpp \
     2>/dev/null | grep -vE ':[0-9]+:[[:space:]]*//' | grep -q .; then
  RO_STATIC=0
fi
if [ "$RO_STATIC" = "1" ]; then
  record_pass "管理工具只用 OpenExistingReadOnly / OpenExistingReadWrite，且打开路径不建表"
else
  record_fail "管理工具的打开方式" "只读 / 可写没有分开，或者打开路径仍会改 schema"
fi

# 数据目录锁：同一个 root 上不允许起第二个服务端。
# 第二个实例同样带上 --transport-key-file：PR #21 起缺这个参数会直接以用法
# 错误退出，那样下面就会因为"参数不全"而不是"目录锁"退出——断言虽然还是
# 检查 "already using the data directory"，但测的就不是锁了。
SECOND_PORT=$((PORT + 1))
./build/backup-server --bind 127.0.0.1 --port "$SECOND_PORT" --root "$DATA" --db "$DB" \
  --secret-file "$SECRET" --transport-key-file "$TRANSPORT_KEY" \
  --quiet > "$TEST_ROOT/logs/second.log" 2>&1 &
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

# ---- 14. 用户选择器：id: / name: 明确指定，裸输入永不猜 ----
#
# 人工验收里的问题：`show-user 23` 到底是 "id=23" 还是 "用户名叫 23"？旧实现直接
# 按数字当 id 解析，于是可能显示成另一个账户，而且没有任何提示。
#
# 要真的造出"两个候选同时存在"，就得让一个数字**既**是某个账户的 id、**又**是
# 另一个账户的用户名。用户名最少 3 个字节，所以用带前导零的 "001"：它作为编号
# 解析就是 id=1（USER_A），作为用户名解析则是另一个账户。用户名允许纯数字是
# **产品规则**，不改；这里正是要证明两种解释各自都能被明确指定。
NUM_NAME="001"
PW_C="$(head -c 24 /dev/urandom | sha256sum | cut -c1-24)"
if [ "$(run_remote "$PW_C" register --user "$NUM_NAME")" = "0" ]; then
  record_pass "注册了一个用户名为纯数字（$NUM_NAME）的账户：纯数字用户名仍然合法"
else
  record_fail "纯数字用户名" "$(head -3 "$TEST_ROOT/last.txt" | tr '\n' ' ')"
fi
run_admin list-users >/dev/null
NUM_ID="$(awk -v name="$NUM_NAME" '$2 == name { print $1 }' "$TEST_ROOT/admin.txt")"
if [ -n "$NUM_ID" ] && [ "$NUM_ID" != "$USER_A_ID" ]; then
  record_pass "数字用户名与 id=$USER_A_ID 是两个不同的账户（name=$NUM_NAME id=$NUM_ID）"
else
  record_fail "纯数字用户名" "解析出的 id 不对：num_id=$NUM_ID"
fi

# 裸输入同时命中 id 与用户名 -> 拒绝，而且把两种写法都写给用户
AMBIG="$(run_admin show-user "$NUM_NAME")"
AMBIG_TEXT="$(cat "$TEST_ROOT/admin.txt")"
if [ "$AMBIG" != "0" ] \
   && printf '%s' "$AMBIG_TEXT" | grep -q "存在歧义" \
   && printf '%s' "$AMBIG_TEXT" | grep -q "id:$NUM_NAME" \
   && printf '%s' "$AMBIG_TEXT" | grep -q "name:$NUM_NAME"; then
  record_pass "裸输入同时命中 id 与用户名：拒绝，并给出 id: / name: 两种写法"
else
  record_fail "歧义输入" "$(printf '%s' "$AMBIG_TEXT" | head -3 | tr '\n' ' ')"
fi

# 显式写法各自解析到**不同**的账户：这是"不猜"的正面证据
run_admin show-user "id:$NUM_NAME" >/dev/null
ID_TEXT="$(cat "$TEST_ROOT/admin.txt")"
ID_OK=1
printf '%s' "$ID_TEXT" | grep -q "用户 ID：$USER_A_ID" || ID_OK=0
printf '%s' "$ID_TEXT" | grep -q "用户名：$USER_A" || ID_OK=0
run_admin show-user "name:$NUM_NAME" >/dev/null
NAME_TEXT="$(cat "$TEST_ROOT/admin.txt")"
NAME_OK=1
printf '%s' "$NAME_TEXT" | grep -q "用户 ID：$NUM_ID" || NAME_OK=0
printf '%s' "$NAME_TEXT" | grep -q "用户名：$NUM_NAME" || NAME_OK=0
if [ "$ID_OK" = "1" ] && [ "$NAME_OK" = "1" ]; then
  record_pass "id:$NUM_NAME 解析到 $USER_A；name:$NUM_NAME 解析到 id=$NUM_ID（两条路各走各的）"
else
  record_fail "显式选择器" "id_ok=$ID_OK name_ok=$NAME_OK"
fi

# 只有一个候选时裸输入仍然可用，但会在 stderr 上说明这次按哪一种解析
BARE_ID="$(run_admin show-user "$USER_B_ID")"
BARE_ID_TEXT="$(cat "$TEST_ROOT/admin.txt")"
BARE_NAME="$(run_admin show-user "$USER_A")"
BARE_NAME_TEXT="$(cat "$TEST_ROOT/admin.txt")"
if [ "$BARE_ID" = "0" ] \
   && printf '%s' "$BARE_ID_TEXT" | grep -q "按编号解析" \
   && printf '%s' "$BARE_ID_TEXT" | grep -q "用户名：$USER_B" \
   && [ "$BARE_NAME" = "0" ] \
   && printf '%s' "$BARE_NAME_TEXT" | grep -q "按用户名解析" \
   && printf '%s' "$BARE_NAME_TEXT" | grep -q "用户名：$USER_A"; then
  record_pass "不产生歧义的裸输入仍然可用，并说明这次按哪一种解析"
else
  record_fail "裸输入的唯一候选" "id=$BARE_ID name=$BARE_NAME"
fi
run_admin show-user "name:$USER_A" >/dev/null
if ! grep -q "提示：" "$TEST_ROOT/admin.txt"; then
  record_pass "显式 name: 写法不打印提示（用户已经把话说清楚了）"
else
  record_fail "显式写法" "不该出现提示行"
fi

# 找不到的时候，id: 与 name: 各说各的话，不会让人误会成另一种解释
NOT_FOUND_ID="$(run_admin show-user id:99999)"
NOT_FOUND_ID_TEXT="$(cat "$TEST_ROOT/admin.txt")"
NOT_FOUND_NAME="$(run_admin show-user "name:nobody-$$")"
NOT_FOUND_NAME_TEXT="$(cat "$TEST_ROOT/admin.txt")"
if [ "$NOT_FOUND_ID" != "0" ] \
   && printf '%s' "$NOT_FOUND_ID_TEXT" | grep -q "没有 id=99999 的用户" \
   && [ "$NOT_FOUND_NAME" != "0" ] \
   && printf '%s' "$NOT_FOUND_NAME_TEXT" | grep -q "没有叫 nobody-$$ 的用户"; then
  record_pass "id: 与 name: 找不到时各说各的话"
else
  record_fail "找不到的用户" "id=[$(printf '%s' "$NOT_FOUND_ID_TEXT" | head -1)] name=[$(printf '%s' "$NOT_FOUND_NAME_TEXT" | head -1)]"
fi

# ---- 停服务，开始做破坏性操作 ----
kill -TERM "$SERVER_PID" 2>/dev/null
wait "$SERVER_PID" 2>/dev/null
SERVER_PID=""
record_pass "服务端已优雅停止"

# ---- 14（续）：删除操作必须明确指定选择器 ----
# 删除不可逆：不能由一个"这个数字到底是编号还是用户名"的疑问来决定删掉谁。
BARE_DELETE="$(run_admin delete-user "$USER_B" --confirm "DELETE $USER_B#$USER_B_ID")"
if [ "$BARE_DELETE" != "0" ] && grep -q "必须明确指定用户" "$TEST_ROOT/admin.txt"; then
  record_pass "delete-user 拒绝裸输入，并说明要写成 id: 或 name:"
else
  record_fail "delete-user 的裸输入" "$(head -3 "$TEST_ROOT/admin.txt" | tr '\n' ' ')"
fi
BARE_SNAP="$(run_admin delete-snapshot "$SNAP_B2" --user "$USER_B" --confirm "$SNAP_B2")"
if [ "$BARE_SNAP" != "0" ] && grep -q "必须明确指定用户" "$TEST_ROOT/admin.txt"; then
  record_pass "delete-snapshot 同样拒绝裸用户选择器"
else
  record_fail "delete-snapshot 的裸输入" "$(head -3 "$TEST_ROOT/admin.txt" | tr '\n' ' ')"
fi
# 确认串必须同时写出用户名与编号：同名不同号的两个账户在这一行就能分清
WRONG_CONFIRM="$(run_admin delete-user "name:$USER_B" --confirm "DELETE $USER_B")"
if [ "$WRONG_CONFIRM" != "0" ] \
   && grep -q "DELETE $USER_B#$USER_B_ID" "$TEST_ROOT/admin.txt" \
   && grep -q "id=$USER_B_ID" "$TEST_ROOT/admin.txt"; then
  record_pass "确认串要写成 DELETE <用户名>#<编号>，错误提示里两个都给出"
else
  record_fail "确认串" "$(head -3 "$TEST_ROOT/admin.txt" | tr '\n' ' ')"
fi
# 快照 id 允许 snapshot:<32 位十六进制>，与裸写法等价
run_admin show-snapshot "snapshot:$SNAP_B2" >/dev/null
if grep -q "$SNAP_B2" "$TEST_ROOT/admin.txt"; then
  record_pass "show-snapshot 接受 snapshot:<32 位十六进制>，与裸 id 等价"
else
  record_fail "snapshot 前缀" "$(head -3 "$TEST_ROOT/admin.txt" | tr '\n' ' ')"
fi

BLOB_A="$DATA/users/$USER_A_ID/$SNAP_A.bak"
BLOB_B1="$DATA/users/$USER_B_ID/$SNAP_B1.bak"
BLOB_B2="$DATA/users/$USER_B_ID/$SNAP_B2.bak"

# ---- 7. 删除单个 snapshot：blob 与元数据一致 ----
DELETE_SNAP="$(run_admin delete-snapshot "$SNAP_B1" --user "name:$USER_B" --confirm "$SNAP_B1")"
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
# 破坏性操作会持有数据目录锁，于是"试锁"一定是 busy：身份块必须说清楚这一点，
# 不能把"锁在本次操作手里"显示成"backup-server 正在运行"（服务端明明已经停了）。
if ! grep -q "backup-server 正在运行" "$TEST_ROOT/admin.txt" \
   && grep -q "数据目录锁由本次管理操作持有" "$TEST_ROOT/admin.txt"; then
  record_pass "破坏性操作的实例身份块不谎称服务端在运行（锁是本次操作持有的）"
else
  record_fail "破坏性操作的实例身份块" "$(grep -m1 'Service:' "$TEST_ROOT/admin.txt")"
fi
run_admin list-snapshots "$USER_B" >/dev/null
if ! grep -q "$SNAP_B1" "$TEST_ROOT/admin.txt" && grep -q "$SNAP_B2" "$TEST_ROOT/admin.txt"; then
  record_pass "元数据与磁盘一致：只剩没被删的那一份"
else
  record_fail "删除单个快照之后的元数据" "$(head -4 "$TEST_ROOT/admin.txt" | tr '\n' ' ')"
fi
if find "$DATA/users/$USER_B_ID/trash" -maxdepth 1 -name "*.deleted*" -print -quit 2>/dev/null | grep -q .; then
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
DELETE_USER="$(run_admin delete-user "name:$USER_A" --confirm "DELETE $USER_A#$USER_A_ID")"
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
if find "$DATA/trash" -maxdepth 1 -name "account-*" -print -quit 2>/dev/null | grep -q .; then
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

# ---- 16. 源码树里的 wrapper 不再猜实例（fail closed）----
SRC_WRAP_OUT="$TEST_ROOT/wrapper-src.txt"
SRC_WRAP_CODE=0
( cd "$ROOT_DIR" && env -u BACKUP_SERVER_ROOT -u BACKUP_SERVER_DATA \
    -u BACKUP_SERVER_DB bash scripts/backup-server-admin.sh </dev/null ) \
  > "$SRC_WRAP_OUT" 2>&1 || SRC_WRAP_CODE=$?
if [ "$SRC_WRAP_CODE" != "0" ] && grep -q "源码树" "$SRC_WRAP_OUT"; then
  record_pass "源码树里的 wrapper 在没有 BACKUP_SERVER_ROOT 时 fail closed"
else
  record_fail "源码树 wrapper" \
    "exit=$SRC_WRAP_CODE $(head -2 "$SRC_WRAP_OUT" | tr '\n' ' ')"
fi
# 显式给出实例根之后，同一条路径必须正常工作（逃生口不是死的）。
SRC_WRAP_OK=0
BACKUP_SERVER_ROOT="$TEST_ROOT" timeout --signal=KILL 60 \
  bash "$ROOT_DIR/scripts/backup-server-admin.sh" </dev/null \
  > "$TEST_ROOT/wrapper-src-ok.txt" 2>&1 && SRC_WRAP_OK=1
if [ "$SRC_WRAP_OK" = "1" ] \
   && grep -q "已退出管理工具" "$TEST_ROOT/wrapper-src-ok.txt"; then
  record_pass "显式给出 BACKUP_SERVER_ROOT 之后，源码树 wrapper 正常工作"
else
  record_fail "源码树 wrapper 的逃生口" \
    "$(head -3 "$TEST_ROOT/wrapper-src-ok.txt" | tr '\n' ' ')"
fi
# 服务端已经停掉的情况下，指纹仍然看得到（退到部署约定路径），而且首页那一行
# 与客户端 pin 是同一串——"抄指纹"这条流程不依赖服务端是否在运行。
if grep -q "^传输身份指纹: $BACKUP_REMOTE_SERVER_KEY" \
     "$TEST_ROOT/wrapper-src-ok.txt"; then
  record_pass "服务端停止后：首页指纹仍然等于客户端 pin（部署约定路径）"
else
  record_fail "服务端停止后的首页指纹" \
    "$(grep -m1 '传输身份指纹' "$TEST_ROOT/wrapper-src-ok.txt" | tr '\n' ' ')"
fi

# ---- 17. 路径含空格时仍能初始化 BPSEC2 身份 ----
MANAGER_ROOT="$TEST_ROOT/manager instance with spaces"
MANAGER_TOOLS="$TEST_ROOT/manager tools with spaces"
mkdir -p "$MANAGER_TOOLS"
ln -s "$ROOT_DIR/build/backup-server" "$MANAGER_TOOLS/backup-server"
ln -s "$ROOT_DIR/build/backup-server-keygen" "$MANAGER_TOOLS/backup-server-keygen"
ln -s "$ROOT_DIR/build/backup-cert-tool" "$MANAGER_TOOLS/backup-cert-tool"
MANAGER_PORT=""
for candidate in $(seq 20700 20750); do
  if ! ss -ltnH "sport = :$candidate" 2>/dev/null | grep -q .; then
    MANAGER_PORT="$candidate"
    break
  fi
done
MANAGER_OUTPUT="$TEST_ROOT/manager-bpsec2-space.log"
MANAGER_REPEAT_OUTPUT="$TEST_ROOT/manager-repeat-init.log"
if [ -n "$MANAGER_PORT" ]; then
  printf '1\n\n\n%s\ny\nmanager-space-test-%s\n\n8\n\n0\n' \
    "$MANAGER_PORT" "$MANAGER_PORT" | \
    env BACKUP_SERVER_ROOT="$MANAGER_ROOT" \
        BACKUP_SERVER_CONFIG="$MANAGER_ROOT/server.conf" \
        BACKUP_SERVER_DATA="$MANAGER_ROOT/data" \
        BACKUP_SERVER_DB="$MANAGER_ROOT/state/metadata.sqlite3" \
        BACKUP_SERVER_PID_FILE="$MANAGER_ROOT/state/server.pid" \
        BACKUP_SERVER_LOG="$MANAGER_ROOT/logs/server.log" \
        BACKUP_SERVER_ADMIN="$ADMIN" \
        BACKUP_SERVER_BIN="$MANAGER_TOOLS/backup-server" \
        BACKUP_SERVER_KEYGEN="$MANAGER_TOOLS/backup-server-keygen" \
        BACKUP_CERT_TOOL="$MANAGER_TOOLS/backup-cert-tool" \
        timeout --signal=KILL 60 bash "$ROOT_DIR/scripts/backup-server-admin.sh" \
          > "$MANAGER_OUTPUT" 2>&1
  if grep -q 'verdict *= *TRUSTED' "$MANAGER_OUTPUT" \
     && grep -Fq "$MANAGER_ROOT/private/root.key.pub" "$MANAGER_OUTPUT" \
     && grep -Fq "$MANAGER_ROOT/state/server.bpcert" "$MANAGER_OUTPUT" \
     && grep -q "manager-space-test-$MANAGER_PORT" "$MANAGER_OUTPUT" \
     && grep -q '^require_bpsec2 = true$' "$MANAGER_ROOT/server.conf" \
     && [ "$(stat -c '%a' "$MANAGER_ROOT/private/root.key")" = "600" ] \
     && [ "$(stat -c '%a' "$MANAGER_ROOT/private/root.key.pub")" = "644" ] \
     && [ "$(stat -c '%a' "$MANAGER_ROOT/state/transport.key")" = "600" ] \
     && [ "$(stat -c '%a' "$MANAGER_ROOT/secrets.env")" = "600" ] \
     && ! find "$MANAGER_ROOT" -maxdepth 1 -name '.server-init.*' -print -quit | grep -q .; then
    record_pass "路径含空格时 BPSEC2 初始化、可信根与身份路径显示正常"
  else
    record_fail "路径含空格的 BPSEC2 初始化" \
      "$(tail -8 "$MANAGER_OUTPUT" | tr '\n' ' ')"
  fi
  ROOT_KEY_BEFORE="$(sha256sum "$MANAGER_ROOT/private/root.key" | cut -d' ' -f1)"
  printf '1\n\n0\n' | \
    env BACKUP_SERVER_ROOT="$MANAGER_ROOT" \
        BACKUP_SERVER_CONFIG="$MANAGER_ROOT/server.conf" \
        BACKUP_SERVER_DATA="$MANAGER_ROOT/data" \
        BACKUP_SERVER_DB="$MANAGER_ROOT/state/metadata.sqlite3" \
        BACKUP_SERVER_PID_FILE="$MANAGER_ROOT/state/server.pid" \
        BACKUP_SERVER_LOG="$MANAGER_ROOT/logs/server.log" \
        BACKUP_SERVER_ADMIN="$ADMIN" \
        BACKUP_SERVER_BIN="$MANAGER_TOOLS/backup-server" \
        BACKUP_SERVER_KEYGEN="$MANAGER_TOOLS/backup-server-keygen" \
        BACKUP_CERT_TOOL="$MANAGER_TOOLS/backup-cert-tool" \
        timeout --signal=KILL 60 bash "$ROOT_DIR/scripts/backup-server-admin.sh" \
          > "$MANAGER_REPEAT_OUTPUT" 2>&1
  ROOT_KEY_AFTER="$(sha256sum "$MANAGER_ROOT/private/root.key" | cut -d' ' -f1)"
  if grep -q '初始化已拒绝，不会覆盖已有内容' "$MANAGER_REPEAT_OUTPUT" \
     && [ "$ROOT_KEY_BEFORE" = "$ROOT_KEY_AFTER" ]; then
    record_pass "重复 BPSEC2 初始化被拒绝，根私钥保持不变"
  else
    record_fail "重复 BPSEC2 初始化保护" "已有根私钥变化或未明确拒绝"
  fi
else
  record_fail "路径含空格的 BPSEC2 初始化" "20700-20750 端口均已占用"
fi

# BPSEC1 must expose the exact client pin immediately after initialization,
# before the server creates its metadata database.
BPSEC1_ROOT="$TEST_ROOT/manager-bpsec1"
BPSEC1_PORT=""
for candidate in $(seq 20751 20800); do
  if ! ss -ltnH "sport = :$candidate" 2>/dev/null | grep -q .; then
    BPSEC1_PORT="$candidate"
    break
  fi
done
BPSEC1_OUTPUT="$TEST_ROOT/manager-bpsec1-init.log"
if [ -n "$BPSEC1_PORT" ]; then
  printf '1\n\n\n%s\nn\n\n0\n' "$BPSEC1_PORT" | \
    env BACKUP_SERVER_ROOT="$BPSEC1_ROOT" \
        BACKUP_SERVER_CONFIG="$BPSEC1_ROOT/server.conf" \
        BACKUP_SERVER_DATA="$BPSEC1_ROOT/data" \
        BACKUP_SERVER_DB="$BPSEC1_ROOT/state/metadata.sqlite3" \
        BACKUP_SERVER_PID_FILE="$BPSEC1_ROOT/state/server.pid" \
        BACKUP_SERVER_LOG="$BPSEC1_ROOT/logs/server.log" \
        BACKUP_SERVER_ADMIN="$ADMIN" \
        BACKUP_SERVER_BIN="$MANAGER_TOOLS/backup-server" \
        BACKUP_SERVER_KEYGEN="$MANAGER_TOOLS/backup-server-keygen" \
        BACKUP_CERT_TOOL="$MANAGER_TOOLS/backup-cert-tool" \
        timeout --signal=KILL 60 bash "$ROOT_DIR/scripts/backup-server-admin.sh" \
          > "$BPSEC1_OUTPUT" 2>&1
  if grep -q '^认证模式：BPSEC1 公钥指纹$' "$BPSEC1_OUTPUT" \
     && grep -q "^服务器地址：127.0.0.1$" "$BPSEC1_OUTPUT" \
     && grep -q "^业务端口：$BPSEC1_PORT$" "$BPSEC1_OUTPUT" \
     && grep -Eq 'sha256:[0-9a-f]{64}' "$BPSEC1_OUTPUT" \
     && [ -f "$BPSEC1_ROOT/state/transport.key" ] \
     && [ "$(stat -c '%a' "$BPSEC1_ROOT/state/transport.key")" = 600 ] \
     && [ ! -e "$BPSEC1_ROOT/state/metadata.sqlite3" ] \
     && ! grep -q '\.bpcert' "$BPSEC1_OUTPUT"; then
    record_pass "BPSEC1 初始化后在数据库创建前显示地址与指纹且不伪造证书"
  else
    record_fail "BPSEC1 初始化连接信息" \
      "$(grep -E '认证模式|服务器地址|业务端口|sha256:|不适用' "$BPSEC1_OUTPUT" | tr '\n' ' ')"
  fi
else
  record_fail "BPSEC1 初始化连接信息" "20751-20800 端口均已占用"
fi

# ---- 18. Stopped-instance certificate import and configuration updates ----
run_manager() {
  local input="$1" output="$2"
  printf '%s' "$input" |
    env BACKUP_SERVER_ROOT="$MANAGER_ROOT" \
        BACKUP_SERVER_CONFIG="$MANAGER_ROOT/server.conf" \
        BACKUP_SERVER_DATA="$MANAGER_ROOT/data" \
        BACKUP_SERVER_DB="$MANAGER_ROOT/state/metadata.sqlite3" \
        BACKUP_SERVER_PID_FILE="$MANAGER_ROOT/state/server.pid" \
        BACKUP_SERVER_ADMIN="$ADMIN" \
        BACKUP_SERVER_BIN="$MANAGER_TOOLS/backup-server" \
        BACKUP_SERVER_KEYGEN="$MANAGER_TOOLS/backup-server-keygen" \
        BACKUP_CERT_TOOL="$MANAGER_TOOLS/backup-cert-tool" \
        timeout --signal=KILL 90 bash "$ROOT_DIR/scripts/backup-server-admin.sh" >"$output" 2>&1
}
manager_config_sha() { sha256sum "$MANAGER_ROOT/server.conf" | cut -d' ' -f1; }
manager_import_input() { printf '8\n1\n%s\n%s\n%s\n\n0\n' "$1" "$2" "$3"; }

if [ -f "$MANAGER_ROOT/server.conf" ]; then
  IMPORT_SOURCE="$TEST_ROOT/import source with spaces"
  mkdir -p "$IMPORT_SOURCE"
  ./build/backup-cert-tool root-init --root-key "$IMPORT_SOURCE/root.key" --root-id manager-import-root \
    >"$IMPORT_SOURCE/root-init.log" 2>&1
  MANAGER_PUBLIC="$("$KEYGEN" --show --key-file "$MANAGER_ROOT/state/transport.key" |
    sed -n 's/.*公钥 (hex): //p')"
  ./build/backup-cert-tool issue-server --root-key "$IMPORT_SOURCE/root.key" \
    --server-id manager-import-valid --server-pubkey "hex:$MANAGER_PUBLIC" \
    --out "$IMPORT_SOURCE/valid.bpcert" >"$IMPORT_SOURCE/issue-valid.log" 2>&1
  ./build/backup-cert-tool issue-server --root-key "$IMPORT_SOURCE/root.key" \
    --server-id manager-import-expired --server-pubkey "hex:$MANAGER_PUBLIC" \
    --not-before 1 --days 1 --out "$IMPORT_SOURCE/expired.bpcert" >"$IMPORT_SOURCE/issue-expired.log" 2>&1
  "$KEYGEN" --output "$IMPORT_SOURCE/other-transport.key" >"$IMPORT_SOURCE/other-keygen.log" 2>&1
  OTHER_PUBLIC="$("$KEYGEN" --show --key-file "$IMPORT_SOURCE/other-transport.key" |
    sed -n 's/.*公钥 (hex): //p')"
  ./build/backup-cert-tool issue-server --root-key "$IMPORT_SOURCE/root.key" \
    --server-id manager-import-wrong-key --server-pubkey "hex:$OTHER_PUBLIC" \
    --out "$IMPORT_SOURCE/wrong-key.bpcert" >"$IMPORT_SOURCE/issue-wrong-key.log" 2>&1
  cp "$IMPORT_SOURCE/valid.bpcert" "$IMPORT_SOURCE/tampered.bpcert"
  python3 - "$IMPORT_SOURCE/tampered.bpcert" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
b = bytearray(p.read_bytes())
b[-1] ^= 1
p.write_bytes(b)
PY
  TRANSPORT_BEFORE="$(sha256sum "$MANAGER_ROOT/state/transport.key" | cut -d' ' -f1)"
  ROOT_KEY_BEFORE="$(sha256sum "$MANAGER_ROOT/private/root.key" | cut -d' ' -f1)"
  IMPORT_ROOT_PRIVATE_BEFORE="$(sha256sum "$IMPORT_SOURCE/root.key" | cut -d' ' -f1)"
  BEFORE="$(manager_config_sha)"
  truncate -s 65537 "$IMPORT_SOURCE/oversized-root.pub"
  truncate -s 4097 "$IMPORT_SOURCE/oversized.bpcert"
  for failure in wrong-root expired wrong-key tampered cancelled oversized-root oversized-cert root-private; do
    ROOT_INPUT="$IMPORT_SOURCE/root.key.pub"
    CERT_INPUT="$IMPORT_SOURCE/valid.bpcert"
    CONFIRMATION='IMPORT BPSEC2'
    case "$failure" in
      wrong-root) ROOT_INPUT="$MANAGER_ROOT/private/root.key.pub" ;;
      expired) CERT_INPUT="$IMPORT_SOURCE/expired.bpcert" ;;
      wrong-key) CERT_INPUT="$IMPORT_SOURCE/wrong-key.bpcert" ;;
      tampered) CERT_INPUT="$IMPORT_SOURCE/tampered.bpcert" ;;
      cancelled) CONFIRMATION=no ;;
      oversized-root) ROOT_INPUT="$IMPORT_SOURCE/oversized-root.pub" ;;
      oversized-cert) CERT_INPUT="$IMPORT_SOURCE/oversized.bpcert" ;;
      root-private) ROOT_INPUT="$IMPORT_SOURCE/root.key" ;;
    esac
    run_manager "$(manager_import_input "$ROOT_INPUT" "$CERT_INPUT" "$CONFIRMATION")"$'\n' \
      "$TEST_ROOT/manager-import-$failure.log"
    if [ "$BEFORE" = "$(manager_config_sha)" ] &&
        ! find "$MANAGER_ROOT/state" -maxdepth 1 -name 'client-identity.*' -print -quit | grep -q .; then
      record_pass "证书导入拒绝 $failure，原配置和材料保持不变"
    else
      record_fail "证书导入拒绝 $failure" "配置变化或留下未提交的公钥材料"
    fi
  done
  printf '# synthetic-private-comment-must-not-be-shared\n' >>"$IMPORT_SOURCE/root.key.pub"
  run_manager "$(printf '8\n1\n%s\n%s\nIMPORT BPSEC2\n\n5\n\n8\n\n0\n' "$IMPORT_SOURCE/root.key.pub" "$IMPORT_SOURCE/valid.bpcert")"$'\n' \
    "$TEST_ROOT/manager-import-valid.log"
  IMPORTED_CERT="$(sed -n 's/^certificate = //p' "$MANAGER_ROOT/server.conf")"
  IMPORTED_ROOT="$(dirname "$IMPORTED_CERT")/trusted-root.pub"
  if grep -q '证书导入完成' "$TEST_ROOT/manager-import-valid.log" &&
      [ -f "$IMPORTED_CERT" ] && [ -f "$IMPORTED_ROOT" ] &&
      grep -q '^require_bpsec2 = true$' "$MANAGER_ROOT/server.conf" &&
      [ -f "$MANAGER_ROOT/state/server.bpcert" ] &&
      [ "$TRANSPORT_BEFORE" = "$(sha256sum "$MANAGER_ROOT/state/transport.key" | cut -d' ' -f1)" ] &&
      [ "$ROOT_KEY_BEFORE" = "$(sha256sum "$MANAGER_ROOT/private/root.key" | cut -d' ' -f1)" ] &&
      [ "$IMPORT_ROOT_PRIVATE_BEFORE" = "$(sha256sum "$IMPORT_SOURCE/root.key" | cut -d' ' -f1)" ] &&
      ! grep -q 'synthetic-private-comment' "$IMPORTED_ROOT" &&
      ./build/backup-cert-tool verify-server --cert "$IMPORTED_CERT" --roots "$IMPORTED_ROOT" \
        >"$TEST_ROOT/manager-import-verify.log" 2>&1; then
    record_pass '安全导入启用 BPSEC2，保持旧材料与私钥，公钥副本不携带源注释'
  else
    record_fail '安全导入已有证书' "$(tail -6 "$TEST_ROOT/manager-import-valid.log" | tr '\n' ' ')"
  fi
  if [ "$(grep -Fc "$IMPORTED_ROOT" "$TEST_ROOT/manager-import-valid.log")" -ge 3 ]; then
    record_pass '同一菜单会话导入后身份页与证书页同步使用新根路径'
  else
    record_fail '同会话身份刷新' '仍显示旧根或未重新验证'
  fi
  run_manager $'8\n\n0\n' "$TEST_ROOT/manager-import-reopen.log"
  if grep -Fq "$IMPORTED_ROOT" "$TEST_ROOT/manager-import-reopen.log" &&
      grep -q 'verdict *= *TRUSTED' "$TEST_ROOT/manager-import-reopen.log"; then
    record_pass '重启菜单后仍显示已提交的可信根绝对路径并验证配对'
  else
    record_fail '重新打开已导入证书' '根路径未持久化或验证失败'
  fi

  BEFORE="$(manager_config_sha)"
  run_manager $'2\n\n8\n1\n\n9\n1\n\n3\n\n0\n' "$TEST_ROOT/manager-running-edits.log"
  if [ "$BEFORE" = "$(manager_config_sha)" ] &&
      [ "$(grep -c '请先停止服务器，再修改配置或证书' "$TEST_ROOT/manager-running-edits.log")" -ge 2 ] &&
      ! ss -ltnH "sport = :$MANAGER_PORT" | grep -q .; then
    record_pass '实际运行的实例拒绝证书与配置编辑，停止后端口释放'
  else
    record_fail '运行中修改保护' "$(tail -6 "$TEST_ROOT/manager-running-edits.log" | tr '\n' ' ')"
  fi
  # A server using the data root on a different port still holds this lock.
  flock -F "$MANAGER_ROOT/data/.backup-server.lock" sleep 15 &
  MANAGER_LOCK_HOLDER=$!
  sleep 0.1
  run_manager $'9\n1\n\n0\n' "$TEST_ROOT/manager-data-lock-edit.log"
  kill -TERM "$MANAGER_LOCK_HOLDER" 2>/dev/null || true
  wait "$MANAGER_LOCK_HOLDER" 2>/dev/null || true
  if [ "$BEFORE" = "$(manager_config_sha)" ] &&
      grep -q '持有数据目录锁' "$TEST_ROOT/manager-data-lock-edit.log"; then
    record_pass '无 PID/监听提示时仍通过数据目录内核锁拒绝并发修改'
  else
    record_fail '数据目录锁保护' '配置变化或未拒绝'
  fi

  UPDATE_PORT=""
  for candidate in $(seq 20801 20850); do
    if ! ss -ltnH "sport = :$candidate" 2>/dev/null | grep -q .; then UPDATE_PORT="$candidate"; break; fi
  done
  NEW_LOG="$MANAGER_ROOT/logs/updated server.log"
  printf 'same-session-log-sentinel\n' >"$NEW_LOG"
  run_manager "$(printf '9\n1\n%s\n7\n45\n%s\n\nSAVE\n\n10\n\n9\n\n0\n' "$UPDATE_PORT" "$NEW_LOG")"$'\n' \
    "$TEST_ROOT/manager-config-valid.log"
  if grep -q '配置已保存' "$TEST_ROOT/manager-config-valid.log" &&
      grep -q "^port = $UPDATE_PORT$" "$MANAGER_ROOT/server.conf" &&
      grep -q '^workers = 7$' "$MANAGER_ROOT/server.conf" &&
      grep -q '^io_timeout = 45$' "$MANAGER_ROOT/server.conf" &&
      grep -Fxq "log_file = $NEW_LOG" "$MANAGER_ROOT/server.conf" &&
      grep -Fxq "transport_key = $MANAGER_ROOT/state/transport.key" "$MANAGER_ROOT/server.conf" &&
      grep -Fxq "certificate = $IMPORTED_CERT" "$MANAGER_ROOT/server.conf" &&
      grep -q 'same-session-log-sentinel' "$TEST_ROOT/manager-config-valid.log"; then
    record_pass '停机配置编辑保存端口/线程/超时/含空格日志路径，身份路径保持不变'
  else
    record_fail '合法配置保存' "$(tail -6 "$TEST_ROOT/manager-config-valid.log" | tr '\n' ' ')"
  fi
  BEFORE="$(manager_config_sha)"
  run_manager $'9\n1\n\n99\n\n\n\nSAVE\n\n0\n' "$TEST_ROOT/manager-config-invalid.log"
  if [ "$BEFORE" = "$(manager_config_sha)" ] &&
      grep -q 'workers 必须在' "$TEST_ROOT/manager-config-invalid.log"; then
    record_pass '复用启动器范围校验，非法配置不发布'
  else
    record_fail '非法配置拒绝' '原配置变化或未执行共享校验'
  fi
  INJECTION_LOG="$MANAGER_ROOT/logs/"'$(touch '"$TEST_ROOT"'/INJECTED)'
  run_manager "$(printf '9\n1\n\n\n\n%s\n\n0\n' "$INJECTION_LOG")"$'\n' "$TEST_ROOT/manager-config-injection.log"
  if [ "$BEFORE" = "$(manager_config_sha)" ] && [ ! -e "$TEST_ROOT/INJECTED" ] &&
      grep -q '不支持的配置字符' "$TEST_ROOT/manager-config-injection.log"; then
    record_pass '配置输入中的 shell 表达式被拒绝且没有执行'
  else
    record_fail '配置输入注入拒绝' '配置变化或产生表达式副作用'
  fi
  run_manager "$(printf '9\n1\n\n\n\n%s\n\n0\n' "$MANAGER_ROOT/state/transport.key")"$'\n' \
    "$TEST_ROOT/manager-config-protected-log.log"
  if [ "$BEFORE" = "$(manager_config_sha)" ] &&
      grep -q '日志路径不能覆盖' "$TEST_ROOT/manager-config-protected-log.log" &&
      [ "$TRANSPORT_BEFORE" = "$(sha256sum "$MANAGER_ROOT/state/transport.key" | cut -d' ' -f1)" ]; then
    record_pass '日志路径不能覆盖传输私钥或其他受保护文件'
  else
    record_fail '日志覆盖身份保护' '配置或传输私钥发生改变'
  fi
  run_manager $'9\n1\n\n\n\n\n1\nno\n\n0\n' "$TEST_ROOT/manager-mode-cancel.log"
  if [ "$BEFORE" = "$(manager_config_sha)" ]; then
    record_pass '未明确确认时不允许 BPSEC1 降级'
  else
    record_fail '降级确认' '取消后配置发生改变'
  fi
  run_manager $'9\n1\n\n\n\n\n1\nALLOW BPSEC1\nSAVE\n\n0\n' "$TEST_ROOT/manager-mode-bpsec1.log"
  MODE_B1_OK=0
  grep -q '^require_bpsec2 = false$' "$MANAGER_ROOT/server.conf" && MODE_B1_OK=1
  run_manager $'9\n1\n\n\n\n\n2\nSAVE\n\n0\n' "$TEST_ROOT/manager-mode-bpsec2.log"
  if [ "$MODE_B1_OK" = 1 ] && grep -q '^require_bpsec2 = true$' "$MANAGER_ROOT/server.conf" &&
      grep -q 'verdict *= *TRUSTED' "$TEST_ROOT/manager-mode-bpsec2.log" &&
      [ "$TRANSPORT_BEFORE" = "$(sha256sum "$MANAGER_ROOT/state/transport.key" | cut -d' ' -f1)" ]; then
    record_pass '明确确认后 BPSEC1/BPSEC2 切换，重验配对且传输私钥保持不变'
  else
    record_fail '认证模式切换' "$(tail -5 "$TEST_ROOT/manager-mode-bpsec2.log" | tr '\n' ' ')"
  fi
  cp -p "$MANAGER_ROOT/server.conf" "$TEST_ROOT/manager-public-restore.conf"
  sed -i 's/^bind = .*/bind = 0.0.0.0/' "$MANAGER_ROOT/server.conf"
  BEFORE="$(manager_config_sha)"
  run_manager $'9\n1\n\n\n\n\n1\n\n0\n' "$TEST_ROOT/manager-mode-public-refused.log"
  if [ "$BEFORE" = "$(manager_config_sha)" ] &&
      grep -q '公网监听不允许' "$TEST_ROOT/manager-mode-public-refused.log"; then
    record_pass '菜单保持公网监听必须使用 BPSEC2 的安全要求'
  else
    record_fail '公网降级拒绝' '配置变化或未拒绝'
  fi
  cp -p "$TEST_ROOT/manager-public-restore.conf" "$MANAGER_ROOT/server.conf"
  if find "$MANAGER_ROOT" -maxdepth 2 -path '*/.server-config-backup.*/server.conf' -print -quit | grep -q . &&
      ! find "$MANAGER_ROOT" -maxdepth 1 -name '.server.conf.*' -print -quit | grep -q .; then
    record_pass '旧配置保留且失败/取消操作不留下候选配置'
  else
    record_fail '原子配置与备份' '备份缺失或候选配置残留'
  fi
  # A PID file must not authorize a different executable or a sibling instance.
  python3 -c 'import time; time.sleep(30)' backup-server --root "$MANAGER_ROOT/data" \
    --db "$MANAGER_ROOT/state/metadata.sqlite3" --pid-file "$MANAGER_ROOT/state/server.pid" &
  FAKE_SERVER_PID=$!
  printf '%s\n' "$FAKE_SERVER_PID" >"$MANAGER_ROOT/state/server.pid"
  run_manager $'3\n\n0\n' "$TEST_ROOT/manager-stop-fake-pid.log"
  FAKE_STILL_ALIVE=0
  kill -0 "$FAKE_SERVER_PID" 2>/dev/null && FAKE_STILL_ALIVE=1
  kill -TERM "$FAKE_SERVER_PID" 2>/dev/null || true
  wait "$FAKE_SERVER_PID" 2>/dev/null || true
  rm -f "$MANAGER_ROOT/state/server.pid"
  if [ "$FAKE_STILL_ALIVE" = 1 ] && grep -q '没有找到属于当前实例' "$TEST_ROOT/manager-stop-fake-pid.log"; then
    record_pass '伪造服务器参数和 PID 文件不能授权停止其他可执行程序'
  else
    record_fail '假 PID 停止保护' '伪装进程受到停止信号或未明确拒绝'
  fi
  OTHER_PORT="$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')"
  mkdir -p "$MANAGER_ROOT/data-other"
  "$MANAGER_TOOLS/backup-server" --bind 127.0.0.1 --port "$OTHER_PORT" \
    --root "$MANAGER_ROOT/data-other" --db "$MANAGER_ROOT/state/metadata.sqlite3-other" \
    --pid-file "$MANAGER_ROOT/state/server.pid-other" --secret-file "$MANAGER_ROOT/secrets.env" \
    --transport-key-file "$MANAGER_ROOT/state/transport.key" >"$TEST_ROOT/prefix-server.log" 2>&1 &
  PREFIX_SERVER_PID=$!
  for _ in $(seq 1 30); do
    ss -ltnH "sport = :$OTHER_PORT" | grep -q . && break
    sleep 0.1
  done
  printf '%s\n' "$PREFIX_SERVER_PID" >"$MANAGER_ROOT/state/server.pid"
  run_manager $'3\n\n0\n' "$TEST_ROOT/manager-stop-prefix-pid.log"
  PREFIX_STILL_ALIVE=0
  if kill -0 "$PREFIX_SERVER_PID" 2>/dev/null && ss -ltnH "sport = :$OTHER_PORT" | grep -q .; then
    PREFIX_STILL_ALIVE=1
  fi
  kill -TERM "$PREFIX_SERVER_PID" 2>/dev/null || true
  wait "$PREFIX_SERVER_PID" 2>/dev/null || true
  rm -f "$MANAGER_ROOT/state/server.pid"
  if [ "$PREFIX_STILL_ALIVE" = 1 ] && grep -q '没有找到属于当前实例' "$TEST_ROOT/manager-stop-prefix-pid.log"; then
    record_pass '数据/数据库/PID 路径前缀相同的真实其他实例不会被停止'
  else
    record_fail '相似前缀实例停止保护' '其他实例受到影响或未明确拒绝'
  fi

  # Replacing a running payload leaves the old /proc/PID/exe marked deleted.
  ORIGINAL_MANAGER_TOOLS="$MANAGER_TOOLS"
  MANAGER_TOOLS="$TEST_ROOT/upgrade tools"
  mkdir -p "$MANAGER_TOOLS"
  cp "$ROOT_DIR/build/backup-server" "$MANAGER_TOOLS/backup-server"
  ln -s "$ROOT_DIR/build/backup-server-keygen" "$MANAGER_TOOLS/backup-server-keygen"
  ln -s "$ROOT_DIR/build/backup-cert-tool" "$MANAGER_TOOLS/backup-cert-tool"
  run_manager $'2\n\n0\n' "$TEST_ROOT/manager-start-before-payload-replace.log"
  UPGRADE_SERVER_PID="$(cat "$MANAGER_ROOT/state/server.pid" 2>/dev/null || true)"
  DELETED_EXE=''
  case "$UPGRADE_SERVER_PID" in
    ''|*[!0-9]*) ;;
    *)
      cp "$MANAGER_TOOLS/backup-server" "$MANAGER_TOOLS/backup-server.new"
      mv "$MANAGER_TOOLS/backup-server.new" "$MANAGER_TOOLS/backup-server"
      DELETED_EXE="$(readlink "/proc/$UPGRADE_SERVER_PID/exe" 2>/dev/null || true)"
      ;;
  esac
  run_manager $'3\n\n0\n' "$TEST_ROOT/manager-stop-replaced-payload.log"
  if [ "$DELETED_EXE" = "$MANAGER_TOOLS/backup-server (deleted)" ] &&
      grep -q '服务器已停止' "$TEST_ROOT/manager-stop-replaced-payload.log" &&
      ! ss -ltnH "sport = :$UPDATE_PORT" | grep -q .; then
    record_pass '合法载荷升级后仍可精确识别并停止原实例（deleted exe）'
  else
    record_fail '升级载荷停止兼容' '原实例未被精确识别或端口仍在监听'
  fi
  MANAGER_TOOLS="$ORIGINAL_MANAGER_TOOLS"

  NO_TOOLS_PATH="$TEST_ROOT/no-management-tools"
  mkdir -p "$NO_TOOLS_PATH"
  ln -s "$(command -v dirname)" "$NO_TOOLS_PATH/dirname"
  MISSING_TOOL_CODE=0
  env PATH="$NO_TOOLS_PATH" BACKUP_SERVER_ROOT="$MANAGER_ROOT" BACKUP_SERVER_ADMIN="$ADMIN" \
    /bin/bash "$ROOT_DIR/scripts/backup-server-admin.sh" </dev/null \
      >"$TEST_ROOT/manager-missing-ss.log" 2>&1 || MISSING_TOOL_CODE=$?
  if [ "$MISSING_TOOL_CODE" != 0 ] && grep -q '缺少 ss' "$TEST_ROOT/manager-missing-ss.log"; then
    record_pass '缺少 ss 时明确失败，不把不可检测的端口当作空闲'
  else
    record_fail 'ss 运行依赖拒绝' '缺少 ss 时未安全失败'
  fi
  ln -s "$(command -v ss)" "$NO_TOOLS_PATH/ss"
  MISSING_TOOL_CODE=0
  env PATH="$NO_TOOLS_PATH" BACKUP_SERVER_ROOT="$MANAGER_ROOT" BACKUP_SERVER_ADMIN="$ADMIN" \
    /bin/bash "$ROOT_DIR/scripts/backup-server-admin.sh" </dev/null \
      >"$TEST_ROOT/manager-missing-flock.log" 2>&1 || MISSING_TOOL_CODE=$?
  if [ "$MISSING_TOOL_CODE" != 0 ] && grep -q '缺少 flock' "$TEST_ROOT/manager-missing-flock.log"; then
    record_pass '缺少 flock 时明确失败，不跳过实例锁'
  else
    record_fail 'flock 运行依赖拒绝' '缺少 flock 时未安全失败'
  fi


  MOCK_TOOLS="$TEST_ROOT/service mocks"
  mkdir -p "$MOCK_TOOLS"
  cat >"$MOCK_TOOLS/systemctl" <<'MOCK'
#!/bin/sh
printf '%s\n' "$*" >>"$BACKUP_TEST_SYSTEMCTL_LOG"
MOCK
  chmod +x "$MOCK_TOOLS/systemctl"
  (
    # shellcheck source=/dev/null
    source <(sed -n '/^installed_instance_matches() {/,/^}/p; /^service_manager_action() {/,/^}/p' "$ROOT_DIR/scripts/backup-server-admin.sh")
    export PATH="$MOCK_TOOLS:$PATH" BACKUP_TEST_SYSTEMCTL_LOG="$TEST_ROOT/systemctl-actions.log"
    export BACKUP_SERVER_SERVICE=backup-project-server
    export SERVER_ROOT=/var/lib/backup-project-server
    export CONFIG_FILE=/etc/backup-project-server/server.conf
    export DATA_ROOT=/var/lib/backup-project-server/data
    export DB_PATH=/var/lib/backup-project-server/state/metadata.sqlite3
    export PID_FILE=/run/backup-project-server/backup-server.pid
    service_manager_action start && service_manager_action stop
  ) >"$TEST_ROOT/systemctl-routing.log" 2>&1
  printf 'start backup-project-server.service\nstop backup-project-server.service\n' >"$TEST_ROOT/systemctl-actions.expected"
  if cmp -s "$TEST_ROOT/systemctl-actions.expected" "$TEST_ROOT/systemctl-actions.log"; then
    record_pass '安装版启停只调用固定 systemd unit（本地 mock，不操作宿主服务）'
  else
    record_fail 'systemd 路由' 'mock 未收到精确的固定 unit 操作'
  fi
  (
    # shellcheck source=/dev/null
    source <(sed -n '/^installed_instance_matches() {/,/^}/p; /^service_manager_action() {/,/^}/p' "$ROOT_DIR/scripts/backup-server-admin.sh")
    export PATH="$MOCK_TOOLS:$PATH" BACKUP_TEST_SYSTEMCTL_LOG="$TEST_ROOT/systemctl-actions.log"
    export BACKUP_SERVER_SERVICE=ssh
    export SERVER_ROOT=/var/lib/backup-project-server
    export CONFIG_FILE=/etc/backup-project-server/server.conf
    export DATA_ROOT=/var/lib/backup-project-server/data
    export DB_PATH=/var/lib/backup-project-server/state/metadata.sqlite3
    export PID_FILE=/run/backup-project-server/backup-server.pid
    if service_manager_action stop; then exit 1; fi
    export BACKUP_SERVER_SERVICE=backup-project-server
    export DATA_ROOT="$TEST_ROOT/another-instance/data"
    if service_manager_action start; then exit 1; fi
  ) >"$TEST_ROOT/systemctl-mismatch.log" 2>&1
  if [ "$?" = 0 ] && cmp -s "$TEST_ROOT/systemctl-actions.expected" "$TEST_ROOT/systemctl-actions.log"; then
    record_pass '非固定 unit 或实例路径不符时拒绝 systemd 操作'
  else
    record_fail 'systemd 实例隔离' '错误 unit/实例触发了 mock 操作'
  fi
  (
    # shellcheck source=/dev/null
    source <(sed -n '/^installed_instance_matches() {/,/^}/p; /^service_manager_action() {/,/^}/p' "$ROOT_DIR/scripts/backup-server-admin.sh")
    export BACKUP_SERVER_SERVICE=backup-project-server
    export SERVER_ROOT=/var/lib/backup-project-server
    export CONFIG_FILE=/etc/backup-project-server/server.conf
    export DATA_ROOT=/var/lib/backup-project-server/data
    export DB_PATH=/var/lib/backup-project-server/state/metadata.sqlite3
    export PID_FILE=/run/backup-project-server/backup-server.pid
    PATH="$NO_TOOLS_PATH"
    if service_manager_action start; then exit 1; fi
  ) >"$TEST_ROOT/systemctl-unavailable.log" 2>&1
  if grep -q '请使用服务器便携包' "$TEST_ROOT/systemctl-unavailable.log"; then
    record_pass '无可用 systemd 时明确要求便携入口，不回退为 root 子进程'
  else
    record_fail '无 systemd 的安装版启动' '未明确拒绝并指向便携入口'
  fi

  # Simulate root/other-owner metadata to cover the ownership branch without
  # claiming that an unprivileged test proves real root permissions.
  OWNER_MOCKS="$TEST_ROOT/owner mocks"
  OWNER_DATA="$TEST_ROOT/owner mock data"
  mkdir -p "$OWNER_MOCKS" "$OWNER_DATA"
  export BACKUP_TEST_REAL_STAT
  BACKUP_TEST_REAL_STAT="$(command -v stat)"
  export BACKUP_TEST_REAL_ID
  BACKUP_TEST_REAL_ID="$(command -v id)"
  export BACKUP_TEST_OWNER_DATA="$OWNER_DATA"
  export BACKUP_TEST_OWNER_CHANGED="$TEST_ROOT/mock-owner-changed"
  export BACKUP_TEST_CHOWN_LOG="$TEST_ROOT/mock-chown-actions.log"
  cat >"$OWNER_MOCKS/stat" <<'MOCK'
#!/bin/sh
if [ "$1" = -c ] && [ "$2" = '%u:%g' ]; then
  if [ "$3" = "$BACKUP_TEST_OWNER_DATA" ]; then printf '2000:2000\n'; exit 0; fi
  if [ "$3" = "$BACKUP_TEST_OWNER_DATA/.backup-server.lock" ] && [ -e "$BACKUP_TEST_OWNER_CHANGED" ]; then
    printf '2000:2000\n'; exit 0
  fi
fi
exec "$BACKUP_TEST_REAL_STAT" "$@"
MOCK
  cat >"$OWNER_MOCKS/chown" <<'MOCK'
#!/bin/sh
printf '%s\n' "$*" >>"$BACKUP_TEST_CHOWN_LOG"
: >"$BACKUP_TEST_OWNER_CHANGED"
MOCK
  cat >"$OWNER_MOCKS/id" <<'MOCK'
#!/bin/sh
if [ "$1" = -u ]; then printf '0\n'; exit 0; fi
exec "$BACKUP_TEST_REAL_ID" "$@"
MOCK
  chmod +x "$OWNER_MOCKS/stat" "$OWNER_MOCKS/chown" "$OWNER_MOCKS/id"
  (
    # shellcheck source=/dev/null
    source <(sed -n '/^with_stopped_instance() (/,/^)/p' "$ROOT_DIR/scripts/backup-server-admin.sh")
    instance_initialized() { return 0; }
    owned_pid() { return 1; }
    port_listener_exists() { return 1; }
    configured_port() { printf '18765\n'; }
    export DATA_ROOT="$OWNER_DATA"
    PATH="$OWNER_MOCKS:$PATH"
    with_stopped_instance true
  ) >"$TEST_ROOT/lock-new-owner-mock.log" 2>&1
  OWNER_NEW_RESULT=$?
  printf '%s\n' "--no-dereference --reference=$OWNER_DATA $OWNER_DATA/.backup-server.lock" >"$TEST_ROOT/mock-chown-actions.expected"
  if [ "$OWNER_NEW_RESULT" = 0 ] &&
      cmp -s "$TEST_ROOT/mock-chown-actions.expected" "$BACKUP_TEST_CHOWN_LOG" &&
      [ "$(stat -c %a "$OWNER_DATA/.backup-server.lock")" = 600 ]; then
    record_pass '模拟 sudo 新建锁路径只同步新锁的实例属主/组（真实 root 权限未验）'
  else
    record_fail '新锁属主分支' 'mock chown 参数或新锁模式不正确'
  fi
  rm -f "$BACKUP_TEST_OWNER_CHANGED"
  (
    # shellcheck source=/dev/null
    source <(sed -n '/^with_stopped_instance() (/,/^)/p' "$ROOT_DIR/scripts/backup-server-admin.sh")
    instance_initialized() { return 0; }
    owned_pid() { return 1; }
    port_listener_exists() { return 1; }
    configured_port() { printf '18765\n'; }
    export DATA_ROOT="$OWNER_DATA"
    PATH="$OWNER_MOCKS:$PATH"
    if with_stopped_instance true; then exit 1; fi
  ) >"$TEST_ROOT/lock-existing-owner-mock.log" 2>&1
  if [ "$?" = 0 ] && grep -q '未修改其属主' "$TEST_ROOT/lock-existing-owner-mock.log" &&
      cmp -s "$TEST_ROOT/mock-chown-actions.expected" "$BACKUP_TEST_CHOWN_LOG"; then
    record_pass '已有锁属主不一致时拒绝，绝不盲目 chown 既有文件'
  else
    record_fail '旧锁属主拒绝' '未拒绝或调用了 chown'
  fi


  export ADMIN_BIN="$ADMIN"
  ADMIN_MOCKS="$TEST_ROOT/admin routing mocks"
  mkdir -p "$ADMIN_MOCKS"
  cat >"$ADMIN_MOCKS/runuser" <<'MOCK'
#!/bin/bash
printf 'ARG=%q\n' "$@" >>"$BACKUP_TEST_RUNUSER_LOG"
MOCK
  cat >"$ADMIN_MOCKS/backend" <<'MOCK'
#!/bin/bash
printf 'ARG=%q\n' "$@" >>"$BACKUP_TEST_DIRECT_ADMIN_LOG"
MOCK
  chmod +x "$ADMIN_MOCKS/runuser" "$ADMIN_MOCKS/backend"
  (
    # shellcheck source=/dev/null
    source <(sed -n '/^installed_instance_matches() {/,/^}/p; /^admin() {/,/^}/p' "$ROOT_DIR/scripts/backup-server-admin.sh")
    instance_ready() { return 0; }
    export PATH="$ADMIN_MOCKS:$OWNER_MOCKS:$PATH"
    export BACKUP_TEST_RUNUSER_LOG="$TEST_ROOT/runuser-actions.log"
    export BACKUP_SERVER_SERVICE=backup-project-server
    export SERVER_ROOT=/var/lib/backup-project-server
    export CONFIG_FILE=/etc/backup-project-server/server.conf
    export DATA_ROOT=/var/lib/backup-project-server/data
    export DB_PATH=/var/lib/backup-project-server/state/metadata.sqlite3
    export PID_FILE=/run/backup-project-server/backup-server.pid
    admin status
  ) >"$TEST_ROOT/admin-installed-routing.log" 2>&1
  printf 'ARG=%q\n' -u backup-project -- "$ADMIN" --server-root /var/lib/backup-project-server \
    --root /var/lib/backup-project-server/data --db /var/lib/backup-project-server/state/metadata.sqlite3 \
    status >"$TEST_ROOT/runuser-actions.expected"
  if cmp -s "$TEST_ROOT/runuser-actions.expected" "$TEST_ROOT/runuser-actions.log"; then
    record_pass '安装版 root 管理操作固定通过 runuser -u backup-project 调用共享后台（mock）'
  else
    record_fail '安装版管理用户路由' '未使用固定服务用户或参数错误'
  fi
  (
    # shellcheck source=/dev/null
    source <(sed -n '/^installed_instance_matches() {/,/^}/p; /^admin() {/,/^}/p' "$ROOT_DIR/scripts/backup-server-admin.sh")
    instance_ready() { return 0; }
    export PATH="$ADMIN_MOCKS:$OWNER_MOCKS:$PATH"
    export BACKUP_TEST_RUNUSER_LOG="$TEST_ROOT/runuser-actions.log"
    export BACKUP_SERVER_SERVICE=ssh
    export SERVER_ROOT=/var/lib/backup-project-server
    export CONFIG_FILE=/etc/backup-project-server/server.conf
    export DATA_ROOT=/var/lib/backup-project-server/data
    export DB_PATH=/var/lib/backup-project-server/state/metadata.sqlite3
    export PID_FILE=/run/backup-project-server/backup-server.pid
    if admin status; then exit 1; fi
  ) >"$TEST_ROOT/admin-installed-mismatch.log" 2>&1
  if [ "$?" = 0 ] && cmp -s "$TEST_ROOT/runuser-actions.expected" "$TEST_ROOT/runuser-actions.log"; then
    record_pass '非固定安装实例不能利用 root 菜单切换用户调用后台'
  else
    record_fail '管理路由实例保护' '错误实例触发了 runuser'
  fi
  (
    # shellcheck source=/dev/null
    source <(sed -n '/^admin() {/,/^}/p' "$ROOT_DIR/scripts/backup-server-admin.sh")
    instance_ready() { return 0; }
    export BACKUP_SERVER_SERVICE=''
    export SERVER_ROOT="$TEST_ROOT/portable"
    export DATA_ROOT="$TEST_ROOT/portable/data"
    export DB_PATH="$TEST_ROOT/portable/state/metadata.sqlite3"
    export BACKUP_TEST_DIRECT_ADMIN_LOG="$TEST_ROOT/direct-admin-actions.log"
    export ADMIN_BIN="$ADMIN_MOCKS/backend"
    admin status
  ) >"$TEST_ROOT/admin-portable-routing.log" 2>&1
  printf 'ARG=%q\n' --server-root "$TEST_ROOT/portable" --root "$TEST_ROOT/portable/data" \
    --db "$TEST_ROOT/portable/state/metadata.sqlite3" status >"$TEST_ROOT/direct-admin-actions.expected"
  if cmp -s "$TEST_ROOT/direct-admin-actions.expected" "$TEST_ROOT/direct-admin-actions.log"; then
    record_pass '便携实例保留当前普通用户直接调用共享管理后台'
  else
    record_fail '便携管理路由' '未直接调用共享后台或参数丢失'
  fi


  PARSER_ROOT="$TEST_ROOT/parser=instance with spaces"
  PARSER_PORT="$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')"
  run_parser_manager() {
    printf '%s' "$1" |
      env -u BACKUP_SERVER_DATA -u BACKUP_SERVER_DB -u BACKUP_SERVER_PID_FILE -u BACKUP_SERVER_CONFIG \
        -u BACKUP_SERVER_LOG BACKUP_SERVER_ROOT="$PARSER_ROOT" BACKUP_SERVER_ADMIN="$ADMIN" \
        BACKUP_SERVER_BIN="$MANAGER_TOOLS/backup-server" \
        BACKUP_SERVER_KEYGEN="$MANAGER_TOOLS/backup-server-keygen" \
        BACKUP_CERT_TOOL="$MANAGER_TOOLS/backup-cert-tool" \
        timeout --signal=KILL 60 bash "$ROOT_DIR/scripts/backup-server-admin.sh" >"$2" 2>&1
  }
  run_parser_manager "$(printf '1\n\n\n%s\nn\n\n0\n' "$PARSER_PORT")"$'\n' \
    "$TEST_ROOT/parser-init.log"
  if [ -f "$PARSER_ROOT/server.conf" ]; then
    sed -i 's/^port = .*/port = 1/; s|^data_root = .*|data_root = /invalid-first-root|; s|^database = .*|database = /invalid-first-db|; s|^pid_file = .*|pid_file = /invalid-first-pid|' "$PARSER_ROOT/server.conf"
    {
      printf '\n# duplicate keys use the last value, including inline comments\n'
      printf 'port = %s # business port\n' "$PARSER_PORT"
      printf 'data_root = %s/data # active root\n' "$PARSER_ROOT"
      printf 'database = %s/state/metadata.sqlite3 # active database\n' "$PARSER_ROOT"
      printf 'pid_file = %s/state/backup-server.pid # active PID\n' "$PARSER_ROOT"
    } >>"$PARSER_ROOT/server.conf"
    run_parser_manager $'2\n\n4\n\n3\n\n0\n' "$TEST_ROOT/parser-start-stop.log"
    if grep -q '服务器已启动' "$TEST_ROOT/parser-start-stop.log" &&
        grep -q '服务器已停止' "$TEST_ROOT/parser-start-stop.log" &&
        grep -Fq "Data root:   $PARSER_ROOT/data" "$TEST_ROOT/parser-start-stop.log" &&
        [ -f "$PARSER_ROOT/state/metadata.sqlite3" ] &&
        ! ss -ltnH "sport = :$PARSER_PORT" | grep -q .; then
      record_pass '注释/等号路径/最后重复键与启动器一致，真实 PID/停止匹配不截断路径'
    else
      record_fail '配置解析同一合同' "$(tail -6 "$TEST_ROOT/parser-start-stop.log" | tr '\n' ' ')"
    fi
  else
    record_fail '配置解析同一合同' '等号路径实例初始化失败'
  fi

  cat >"$MOCK_TOOLS/journalctl" <<'MOCK'
#!/bin/bash
printf 'ARG=%q\n' "$@" >>"$BACKUP_TEST_JOURNAL_LOG"
printf 'task-local-journal-sentinel\n'
MOCK
  chmod +x "$MOCK_TOOLS/journalctl"
  (
    # shellcheck source=/dev/null
    source <(sed -n '/^installed_instance_matches() {/,/^}/p; /^logs_page() {/,/^}/p' "$ROOT_DIR/scripts/backup-server-admin.sh")
    clear_screen() { return 0; }
    config_value() { printf '\n'; }
    export PATH="$MOCK_TOOLS:$PATH" BACKUP_TEST_JOURNAL_LOG="$TEST_ROOT/journal-actions.log"
    export BACKUP_SERVER_SERVICE=backup-project-server
    export SERVER_ROOT=/var/lib/backup-project-server
    export CONFIG_FILE=/etc/backup-project-server/server.conf
    export DATA_ROOT=/var/lib/backup-project-server/data
    export DB_PATH=/var/lib/backup-project-server/state/metadata.sqlite3
    export PID_FILE=/run/backup-project-server/backup-server.pid
    logs_page
  ) >"$TEST_ROOT/manager-journal-routing.log" 2>&1
  printf 'ARG=%q\n' -u backup-project-server.service --no-pager -n 80 >"$TEST_ROOT/journal-actions.expected"
  if cmp -s "$TEST_ROOT/journal-actions.expected" "$TEST_ROOT/journal-actions.log" &&
      grep -q 'task-local-journal-sentinel' "$TEST_ROOT/manager-journal-routing.log"; then
    record_pass '安装版空 log_file 从固定 unit 读取 journald（mock，无宿主日志访问）'
  else
    record_fail 'journald 菜单路由' '未调用固定 unit 或参数不正确'
  fi


fi



finish
