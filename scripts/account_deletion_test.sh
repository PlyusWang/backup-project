#!/usr/bin/env bash
#
# PR #20（closure）账户注销的 CLI 端到端测试。
#
# 跑的是**真实链路**：真实 backup-server 进程、真实 SQLite、真实磁盘，客户端是
# 产品自己的 backupctl remote（与 Modern GUI 共用同一个 RemoteArchiveClient）。
# 断言不看服务端的自述：元数据用 ECS 本地管理工具查，blob 与目录直接看磁盘。
#
# 退出码：只有全部用例通过才是 0。

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "$BASH_SOURCE")/.." && pwd)"
cd "$ROOT_DIR"
TEST_ROOT="$ROOT_DIR/testdata/account-deletion"
rm -rf "$TEST_ROOT"
mkdir -p "$TEST_ROOT/data" "$TEST_ROOT/state" "$TEST_ROOT/logs" \
  "$TEST_ROOT/repo" "$TEST_ROOT/src" "$TEST_ROOT/out"

PASS=0
FAIL=0
record_pass() { PASS=$((PASS + 1)); echo "  PASS  $1"; }
record_fail() { FAIL=$((FAIL + 1)); echo "  FAIL  $1 -- $2" >&2; }

echo "[account-deletion] PR #20 账户注销端到端"

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
  echo "[account-deletion] 合计: $PASS passed, $FAIL failed"
  [ "$FAIL" = "0" ] || exit 1
  echo "ACCOUNT_DELETION_ALL_PASS"
}

DATA="$TEST_ROOT/data"
DB="$TEST_ROOT/state/metadata.sqlite3"
SECRET="$TEST_ROOT/secrets.env"
LOG="$TEST_ROOT/logs/server.log"

if ! make -j4 all server > "$TEST_ROOT/build.log" 2>&1; then
  record_fail "make all server" "$(tail -3 "$TEST_ROOT/build.log" | tr '\n' ' ')"
  finish
fi
record_pass "make all server 成功（warning $(grep -ci warning "$TEST_ROOT/build.log" || true) 条）"

# 随机 secret，只落在 0600 文件里，脚本不打印内容。
head -c 32 /dev/urandom | sha256sum | cut -c1-64 \
  | sed 's/^/BACKUP_TOKEN_SECRET=/' > "$SECRET"
chmod 600 "$SECRET"

PORT=""
for candidate in $(seq 20100 20130); do
  if ! ss -ltn 2>/dev/null | grep -q ":$candidate "; then PORT="$candidate"; break; fi
done
[ -n "$PORT" ] || { record_fail "端口" "20100-20130 都被占用"; finish; }

./build/backup-server --bind 127.0.0.1 --port "$PORT" --root "$DATA" --db "$DB" \
  --secret-file "$SECRET" --pid-file "$TEST_ROOT/state/server.pid" \
  --log-file "$LOG" --quiet > "$TEST_ROOT/logs/server-stderr.log" 2>&1 &
SERVER_PID=$!

ready=0
for _ in $(seq 1 50); do
  if ss -ltn 2>/dev/null | grep -q "127.0.0.1:$PORT "; then ready=1; break; fi
  sleep 0.1
done
if [ "$ready" = "1" ]; then
  record_pass "服务端监听 127.0.0.1:$PORT"
else
  record_fail "服务端监听" "没有起来"
  finish
fi

UA="acc-cli-a-$$"
UB="acc-cli-b-$$"
PW_A="$(head -c 24 /dev/urandom | sha256sum | cut -c1-24)"
PW_B="$(head -c 24 /dev/urandom | sha256sum | cut -c1-24)"
REMOTE_OPTS="--host 127.0.0.1 --port $PORT"

# run_remote <口令> <参数...>：口令只从环境变量进，绝不进 argv。
run_remote() {
  local password="$1"
  shift
  BACKUP_REMOTE_PASSWORD="$password" timeout --signal=KILL 180 \
    ./build/backupctl remote "$@" $REMOTE_OPTS >"$TEST_ROOT/last.txt" 2>&1
  echo $?
}

run_admin() {
  timeout --signal=KILL 120 ./build/backup-server-admin --root "$DATA" --db "$DB" "$@" \
    >"$TEST_ROOT/admin.txt" 2>&1
  echo $?
}

check_remote_ok() {
  local label="$1"
  local password="$2"
  shift 2
  local code
  code="$(run_remote "$password" "$@")"
  if [ "$code" = "0" ]; then
    record_pass "$label"
  else
    record_fail "$label" "退出码 $code: $(head -1 "$TEST_ROOT/last.txt")"
  fi
}

# ---- 准备：两个用户各一份真实归档 ----

printf 'alpha\n' > "$TEST_ROOT/src/a.txt"
head -c 8192 /dev/urandom > "$TEST_ROOT/src/blob.bin"
./build/backupctl config repository set "$TEST_ROOT/repo" \
  --config-file "$TEST_ROOT/config.json" >"$TEST_ROOT/last.txt" 2>&1
./build/backupctl backup "$TEST_ROOT/src" --config-file "$TEST_ROOT/config.json" \
  >"$TEST_ROOT/backup.txt" 2>&1
ARCHIVE="$(ls -1 "$TEST_ROOT/repo"/*.bak 2>/dev/null | head -1)"
if [ -n "$ARCHIVE" ] && [ -s "$ARCHIVE" ]; then
  record_pass "用产品 CLI 生成真实归档（$(stat -c%s "$ARCHIVE") 字节）"
else
  record_fail "用产品 CLI 生成真实归档" "$(head -1 "$TEST_ROOT/backup.txt")"
  finish
fi
ARCHIVE_SHA="$(sha256sum "$ARCHIVE" | cut -d' ' -f1)"

check_remote_ok "注册用户 A" "$PW_A" register --user "$UA"
check_remote_ok "登录用户 A" "$PW_A" login --user "$UA"
check_remote_ok "用户 A 上传" "$PW_A" upload "$ARCHIVE" --user "$UA" \
  --repository "$TEST_ROOT/repo"
SNAP_A="$(grep -m1 '快照 ID:' "$TEST_ROOT/last.txt" | awk '{print $3}')"
check_remote_ok "注册用户 B" "$PW_B" register --user "$UB"
check_remote_ok "登录用户 B" "$PW_B" login --user "$UB"
check_remote_ok "用户 B 上传" "$PW_B" upload "$ARCHIVE" --user "$UB" \
  --repository "$TEST_ROOT/repo"
SNAP_B="$(grep -m1 '快照 ID:' "$TEST_ROOT/last.txt" | awk '{print $3}')"
if [ -n "$SNAP_A" ] && [ -n "$SNAP_B" ]; then
  record_pass "两次上传都返回了快照 ID"
else
  record_fail "两次上传的返回" "$(head -1 "$TEST_ROOT/last.txt")"
  finish
fi

# 用户 id 从管理工具读（只读操作在服务端运行时也允许）。
run_admin list-users >/dev/null
USER_A_ID="$(awk -v name="$UA" '$2 == name { print $1 }' "$TEST_ROOT/admin.txt")"
USER_B_ID="$(awk -v name="$UB" '$2 == name { print $1 }' "$TEST_ROOT/admin.txt")"
if [ -n "$USER_A_ID" ] && [ -n "$USER_B_ID" ]; then
  record_pass "管理工具读到两个用户（id=$USER_A_ID/$USER_B_ID）"
else
  record_fail "管理工具读到两个用户" "$(head -3 "$TEST_ROOT/admin.txt" | tr '\n' ' ')"
  finish
fi

BLOB_A="$DATA/users/$USER_A_ID/$SNAP_A.bak"
BLOB_B="$DATA/users/$USER_B_ID/$SNAP_B.bak"
if [ -f "$BLOB_A" ] && [ -f "$BLOB_B" ] \
   && [ "$(sha256sum "$BLOB_A" | cut -d' ' -f1)" = "$ARCHIVE_SHA" ]; then
  record_pass "两份 blob 都真的落在磁盘上，且与本地归档同哈希"
else
  record_fail "两份 blob" "文件不存在或哈希不一致"
fi

# ---- 1. 二次确认：没有 --confirm / 确认字符串不对，都必须拒绝 ----
NO_CONFIRM="$(run_remote "$PW_A" delete-account --user "$UA")"
if [ "$NO_CONFIRM" != "0" ]; then
  record_pass "缺少 --confirm 时注销被拒绝（用法错误）"
else
  record_fail "缺少 --confirm" "本应失败却成功了"
fi
WRONG_CONFIRM="$(run_remote "$PW_A" delete-account --user "$UA" --confirm "$UB")"
if [ "$WRONG_CONFIRM" != "0" ]; then
  record_pass "--confirm 给的是另一个账户名时被拒绝"
else
  record_fail "--confirm 名字不对" "本应失败却成功了"
fi
check_remote_ok "两次拒绝之后账户 A 仍然可以登录" "$PW_A" login --user "$UA"

# ---- 2. 口令不对：服务端必须拒绝（token 有效也不够） ----
BAD_PW="$(run_remote "$PW_A-wrong" delete-account --user "$UA" --confirm "$UA")"
if [ "$BAD_PW" != "0" ]; then
  record_pass "口令不对时服务端拒绝注销"
else
  record_fail "口令不对" "本应失败却成功了"
fi
check_remote_ok "拒绝之后账户 A 仍然能列出自己的备份" "$PW_A" list --user "$UA"
if grep -q "$SNAP_A" "$TEST_ROOT/last.txt"; then
  record_pass "拒绝之后 A 的快照还在"
else
  record_fail "拒绝之后 A 的快照" "列表里没有 $SNAP_A"
fi

# ---- 3. 正确口令 + 正确确认：注销成功 ----
check_remote_ok "正确口令 + 正确确认：注销成功" "$PW_A" \
  delete-account --user "$UA" --confirm "$UA"

# ---- 4. 注销之后：登录失败、数据为空、磁盘上什么都不剩 ----
# 注意：run_remote 用 echo 输出退出码，所以它的**函数**退出状态永远是 0；
# 这里必须把码取出来比较，不能写成 if run_remote ...; then（那永远为真）。
LOGIN_AFTER_DELETE="$(run_remote "$PW_A" login --user "$UA")"
if [ "$LOGIN_AFTER_DELETE" != "0" ]; then
  record_pass "注销之后原账户无法再登录（退出码 $LOGIN_AFTER_DELETE）"
else
  record_fail "注销之后原账户登录" "本应失败却成功了"
fi

run_admin list-users >/dev/null
if awk -v name="$UA" '$2 == name { found = 1 } END { exit found ? 0 : 1 }' \
     "$TEST_ROOT/admin.txt"; then
  record_fail "注销之后 list-users" "用户 A 还在列表里"
else
  record_pass "注销之后 list-users 里没有用户 A"
fi

run_admin list-snapshots "$UB" >/dev/null
if grep -q "$SNAP_B" "$TEST_ROOT/admin.txt"; then
  record_pass "注销之后用户 B 的快照仍然在（跨用户隔离）"
else
  record_fail "注销之后用户 B 的快照" "管理工具读不到 $SNAP_B"
fi
if grep -q "$SNAP_A" "$TEST_ROOT/admin.txt"; then
  record_fail "注销之后用户 A 的快照" "B 的列表里出现了 A 的快照"
else
  record_pass "注销之后用户 A 的快照不再出现在任何列表里"
fi

if [ ! -f "$BLOB_A" ] && [ ! -d "$DATA/users/$USER_A_ID" ]; then
  record_pass "注销之后 A 的 blob 与目录都从磁盘上消失"
else
  record_fail "注销之后 A 的磁盘数据" "文件或目录还在"
fi
if [ -f "$BLOB_B" ] \
   && [ "$(sha256sum "$BLOB_B" | cut -d' ' -f1)" = "$ARCHIVE_SHA" ]; then
  record_pass "注销 A 之后 B 的 blob 逐字节未变"
else
  record_fail "注销 A 之后 B 的 blob" "文件不见了或哈希变了"
fi
if ls "$DATA/trash" 2>/dev/null | grep -q '^account-'; then
  record_fail "trash 里没有隔离残留" "发现了 account-* 目录"
else
  record_pass "trash 里没有隔离残留"
fi

run_admin overview >/dev/null
if grep -q "用户数：1" "$TEST_ROOT/admin.txt"; then
  record_pass "存储概览：注销之后只剩 1 个用户"
else
  record_fail "存储概览" "$(head -3 "$TEST_ROOT/admin.txt" | tr '\n' ' ')"
fi

# ---- 5. 口令不写日志 ----
if grep -qF "$PW_A" "$LOG" 2>/dev/null || grep -qF "$PW_B" "$LOG" 2>/dev/null; then
  record_fail "服务端日志里没有口令" "日志里出现了口令"
else
  record_pass "服务端日志里没有出现过任何口令"
fi
if grep -q "account deleted: user id=" "$LOG" 2>/dev/null; then
  record_pass "服务端日志记录了注销（只记 user id 与数量）"
else
  record_fail "服务端日志记录了注销" "日志里没有这条记录"
fi

finish
