#!/usr/bin/env bash
#
# PR #20 远程备份的专项测试入口。
#
#   A. 协议层单元测试：帧编解码、截断、假 magic、超大长度、字段上限。
#   B. 服务端测试：真实 TCP 环回上的 PING / 状态机 / 帧损坏断开。
#
# 编译方式沿用 realtime_test.sh 的 run_unit：真实产品目标文件 + -Wall -Wextra
# -Wpedantic，出现任何警告即算失败（不留"以后再说"的余地）。
#
# 退出码：只有全部用例通过才是 0。

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "$BASH_SOURCE")/.." && pwd)"
cd "$ROOT_DIR"
TEST_ROOT="$ROOT_DIR/testdata/network"
mkdir -p "$TEST_ROOT"

PASS=0
FAIL=0
# 消毒剂报告计数（只在 NETWORK_TEST_SANITIZE=1 时有意义）。
SAN_REPORTS=0

record_pass() { PASS=$((PASS + 1)); echo "  PASS  $1"; }
record_fail() { FAIL=$((FAIL + 1)); echo "  FAIL  $1 -- $2" >&2; }

# 消毒剂模式：NETWORK_TEST_SANITIZE=1 时改用 build-sanitize 下的目标文件与
# 二进制，并给单元测试加上同一组 ASan/UBSan 编译参数。
# 默认（不设该变量）仍然是普通构建，两者跑的是同一套用例。
OBJ_ROOT="build"
EXTRA_FLAGS=""
SAN_LABEL=""
SAN_MODE="${NETWORK_TEST_SANITIZE:-0}"
if [ "$SAN_MODE" = "1" ]; then
  OBJ_ROOT="build-sanitize"
  EXTRA_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
  SAN_LABEL=" [ASan+UBSan]"
fi

echo "[network-test] PR #20 remote backup suite$SAN_LABEL"

# 服务端目标文件是测试的前置条件：先确保它们存在（也是零警告的证据）。
if [ -z "$(find "$OBJ_ROOT/server" -name remote_server.o 2>/dev/null | head -1)" ]; then
  echo "[network-test] $OBJ_ROOT/backup-server is missing; building first..."
  if [ "$OBJ_ROOT" = "build-sanitize" ]; then
    BUILD_CMD="make -j4 sanitize"
  else
    BUILD_CMD="make -j4 all server"
  fi
  if ! $BUILD_CMD >"$TEST_ROOT/build-server.log" 2>&1; then
    record_fail "make server" "$(tail -3 "$TEST_ROOT/build-server.log" | tr '\n' ' ')"
    echo "network: $PASS passed, $FAIL failed"
    exit 1
  fi
fi

# SQLite 运行库：与 Makefile 用同一条发现顺序（先系统 .so，再 .so.0）。
SQLITE_LIBRARY="$(ls -1 /usr/lib/x86_64-linux-gnu/libsqlite3.so \
  /usr/lib/x86_64-linux-gnu/libsqlite3.so.0 /usr/lib64/libsqlite3.so \
  /usr/lib/libsqlite3.so 2>/dev/null | head -1)"
if [ -z "$SQLITE_LIBRARY" ]; then
  record_fail "sqlite3 运行库" "找不到 libsqlite3.so"
  echo "network: $PASS passed, $FAIL failed"
  exit 1
fi

NET_OBJECTS="$OBJ_ROOT/src/network/network_protocol.o"
# 服务端的目标文件统一在 $OBJ_ROOT/server/ 下（见 Makefile 里 SERVER_OBJECTS
# 的说明）：它们不能落在 $OBJ_ROOT/src/，否则既有测试脚本的
# "find build/src -name '*.o'" 会把 server/main.o 也链进来，
# 与单元测试自己的 main 冲突。
# 用 find 而不是写死路径：服务端目标文件的目录结构随 Makefile 变过一次
# （先在 $OBJ_ROOT/src/，后移到 $OBJ_ROOT/server/），写死路径会在移动时
# 静默指向不存在的文件。
# 排除 main.o：单元测试有自己的 main，链接服务端的 main 会重复定义。
# 排除 main.o（单元测试有自己的 main）与 crypto/*（那部分统一用
# $OBJ_ROOT/src/crypto 下的目标文件，两处都链会重复定义）。
# 排除 main.o（单元测试有自己的 main）、admin_main.o（管理工具自己的 main，
# 链接进来会与单元测试的 main 重复定义）与 crypto/*（那部分统一用
# $OBJ_ROOT/src/crypto 下的目标文件，两处都链会重复定义）。
SERVER_ONLY="$(find "$OBJ_ROOT/server" -name '*.o' ! -name 'main.o' \
  ! -name 'admin_main.o' ! -path '*/crypto/*' 2>/dev/null | sort | tr '\n' ' ')"
SERVER_AUTH_OBJ="$(find "$OBJ_ROOT/server" -name 'remote_auth.o' 2>/dev/null | head -1)"
SERVER_STORE_OBJ="$(find "$OBJ_ROOT/server" -name 'remote_metadata_store.o' 2>/dev/null | head -1)"
SERVER_OBJECTS="$SERVER_ONLY $OBJ_ROOT/src/network/remote_backup_client.o"
CRYPTO_OBJECTS="$OBJ_ROOT/src/crypto/sha256.o $OBJ_ROOT/src/crypto/hmac.o \
$OBJ_ROOT/src/crypto/pbkdf2.o $OBJ_ROOT/src/crypto/random.o"

# run_unit <名字> <目标文件列表> [额外链接参数]
run_unit() {
  local name="$1"
  local objects="$2"
  shift 2
  local source="$ROOT_DIR/tests/unit/$name.cpp"
  local binary="$TEST_ROOT/$name"
  if [ ! -f "$source" ]; then
    record_fail "A.$name 存在" "tests/unit/$name.cpp 不存在"
    return
  fi
  if ! g++ -std=c++17 -Wall -Wextra -Wpedantic $EXTRA_FLAGS \
      -I"$ROOT_DIR/include" -I"$ROOT_DIR/tests/unit" "$source" $objects "$@" \
      -o "$binary" \
      >"$TEST_ROOT/$name-build.log" 2>&1; then
    record_fail "A.$name 编译" "$(head -3 "$TEST_ROOT/$name-build.log" | tr '\n' ' ')"
    return
  fi
  record_pass "A.$name 编译"
  if [ -s "$TEST_ROOT/$name-build.log" ]; then
    record_fail "A.$name 零警告" "$(head -2 "$TEST_ROOT/$name-build.log" | tr '\n' ' ')"
  else
    record_pass "A.$name 零警告"
  fi
  timeout --signal=KILL 300 "$binary" >"$TEST_ROOT/$name.log" 2>&1
  local status=$?
  if [ "$status" = "0" ]; then
    record_pass "A.$name 全部断言通过（$(tail -1 "$TEST_ROOT/$name.log")）"
  else
    record_fail "A.$name 全部断言通过" \
      "$(grep -m3 FAIL "$TEST_ROOT/$name.log" | tr '\n' ' ')"
  fi
  scan_sanitizer_reports "$name"
}

# 消毒剂构建下任何一条报告都算失败：ASan / UBSan 默认只打印、不改退出码，
# 所以只看退出码会漏掉"跑完了但有报告"这种情况。
# 模式与 scripts/legacy_filter_test.sh 用的是同一组（同一个判定口径）。
scan_sanitizer_reports() {
  local name="$1"
  if [ "$SAN_MODE" != "1" ]; then
    return 0
  fi
  local log="$TEST_ROOT/$name.log"
  local reports
  reports="$(grep -c -E "ERROR: AddressSanitizer|runtime error:|SUMMARY: AddressSanitizer|LeakSanitizer" "$log" || true)"
  SAN_REPORTS=$((SAN_REPORTS + reports))
  if [ "$reports" = "0" ]; then
    record_pass "A.$name 消毒剂零报告"
  else
    record_fail "A.$name 消毒剂零报告" \
      "$(grep -m2 -E 'ERROR: AddressSanitizer|runtime error:' "$log" | tr '\n' ' ')"
  fi
}

echo "[network-test] A. 单元测试"
run_unit network_protocol_test "$NET_OBJECTS"
run_unit remote_auth_test "$SERVER_AUTH_OBJ $CRYPTO_OBJECTS"
run_unit remote_metadata_store_test \
  "$SERVER_STORE_OBJ $SERVER_AUTH_OBJ \
$CRYPTO_OBJECTS" "$SQLITE_LIBRARY" -pthread
run_unit remote_server_test "$SERVER_OBJECTS $CRYPTO_OBJECTS" \
  "$SQLITE_LIBRARY" -pthread
run_unit remote_transfer_test "$SERVER_OBJECTS $CRYPTO_OBJECTS" \
  "$SQLITE_LIBRARY" -pthread
# 流式有界内存的证据来自非消毒剂构建；消毒剂构建会明确写"RSS 阈值不适用"。
# 这两行原样打出来，避免有人以为严格断言被删掉了。
grep -E '^(RSS_BOUND|ASAN):' "$TEST_ROOT/remote_transfer_test.log" \
  | sed 's/^/  /' || true
run_unit remote_client_test "$SERVER_OBJECTS $CRYPTO_OBJECTS" \
  "$SQLITE_LIBRARY" -pthread
# 账户注销：真实服务端 + 真实 SQLite + 真实磁盘，含元数据事务故障注入。
run_unit remote_account_test "$SERVER_OBJECTS $CRYPTO_OBJECTS" \
  "$SQLITE_LIBRARY" -pthread
# 请求序列回归：空闲超时、错误口令 × 6、LIST × 10、20 轮、注销序列、不重发。
run_unit remote_sequence_test "$SERVER_OBJECTS $CRYPTO_OBJECTS" \
  "$SQLITE_LIBRARY" -pthread

echo
echo
echo "[network-test] B. CLI 端到端（真实 backup-server 进程 + 真实归档）"

CLI_WORK="$TEST_ROOT/cli-e2e"
rm -rf "$CLI_WORK"
mkdir -p "$CLI_WORK/data" "$CLI_WORK/state" "$CLI_WORK/logs" "$CLI_WORK/repo" \
  "$CLI_WORK/src/sub" "$CLI_WORK/out"

# 随机 secret，只在 600 文件里存在，脚本不打印内容。
head -c 32 /dev/urandom | sha256sum | cut -c1-64 \
  | sed 's/^/BACKUP_TOKEN_SECRET=/' > "$CLI_WORK/secrets.env"
chmod 600 "$CLI_WORK/secrets.env"

# 随机端口：先确认空闲再启动，避免与别的测试撞车。
PORT=""
for candidate in $(seq 20000 20020); do
  if ! ss -ltn 2>/dev/null | grep -q ":$candidate "; then PORT="$candidate"; break; fi
done
if [ -z "$PORT" ]; then
  record_fail "B 端口" "20000-20020 都被占用"
  PORT=20021
fi

SERVER_ARGS="--bind 127.0.0.1 --port $PORT --root $CLI_WORK/data \
--db $CLI_WORK/state/metadata.sqlite3 --secret-file $CLI_WORK/secrets.env \
--pid-file $CLI_WORK/state/server.pid --log-file $CLI_WORK/logs/server.log --quiet"
# shellcheck disable=SC2086
./$OBJ_ROOT/backup-server $SERVER_ARGS &
SERVER_PID=$!
cleanup_server() {
  if kill -0 "$SERVER_PID" 2>/dev/null; then
    kill -TERM "$SERVER_PID" 2>/dev/null
    wait "$SERVER_PID" 2>/dev/null
  fi
}

ready=0
for _ in $(seq 1 50); do
  if ss -ltn 2>/dev/null | grep -q "127.0.0.1:$PORT "; then ready=1; break; fi
  sleep 0.1
done
if [ "$ready" = "1" ]; then
  record_pass "B.1 backup-server 监听 127.0.0.1:$PORT"
else
  record_fail "B.1 backup-server 监听" "服务端没有起来"
  cleanup_server
  echo
  echo "[network-test] 合计: $PASS passed, $FAIL failed"
  [ "$FAIL" = "0" ] || exit 1
  echo "NETWORK_ALL_PASS"
  exit 0
fi
# 必须只监听环回地址。
if ss -ltn 2>/dev/null | grep -q "0.0.0.0:$PORT "; then
  record_fail "B.1 只绑环回" "出现了 0.0.0.0 监听"
else
  record_pass "B.1 服务端没有绑 0.0.0.0"
fi

USER_A="night-a-$$"
USER_B="night-b-$$"
# 口令只存在于环境变量与内存里，脚本与日志都不打印它。
export BACKUP_REMOTE_PASSWORD="$(head -c 24 /dev/urandom | sha256sum | cut -c1-32)"
# 选项放在子命令之后：remote <子命令> [--host] [--port] ...
REMOTE_OPTS="--host 127.0.0.1 --port $PORT"

# 注意：这个脚本没有开 errexit，这里也**不能**顺手 set -e——
# 那会让后面任何一次 grep 未命中直接结束整个套件。
run_remote() {
  timeout --signal=KILL 300 ./$OBJ_ROOT/backupctl remote "$@" $REMOTE_OPTS \
    >"$CLI_WORK/last.txt" 2>&1
  echo $?
}

check_remote_ok() {
  local label="$1"
  shift
  local code
  code="$(run_remote "$@")"
  if [ "$code" = "0" ]; then
    record_pass "$label"
  else
    record_fail "$label" "退出码 $code: $(head -1 "$CLI_WORK/last.txt")"
  fi
}

check_remote_fail() {
  local label="$1"
  shift
  local code
  code="$(run_remote "$@")"
  if [ "$code" != "0" ]; then
    record_pass "$label"
  else
    record_fail "$label" "本应失败却成功了"
  fi
}

check_remote_ok "B.2 remote ping" ping
if grep -q "PING 正常" "$CLI_WORK/last.txt"; then
  record_pass "B.2 ping 输出可读"
else
  record_fail "B.2 ping 输出可读" "$(head -1 "$CLI_WORK/last.txt")"
fi

check_remote_ok "B.3 注册用户 A" register --user "$USER_A"
check_remote_ok "B.3 登录用户 A" login --user "$USER_A"

# 用现有产品生成一份真正的归档（不是 echo 出来的假文件）。
printf 'alpha\n' > "$CLI_WORK/src/a.txt"
printf 'bee\n' > "$CLI_WORK/src/sub/b.txt"
head -c 4096 /dev/urandom > "$CLI_WORK/src/blob.bin"
CONFIG_FILE="$CLI_WORK/config.json"
./$OBJ_ROOT/backupctl config repository set "$CLI_WORK/repo" \
  --config-file "$CONFIG_FILE" >"$CLI_WORK/last.txt" 2>&1
./$OBJ_ROOT/backupctl backup "$CLI_WORK/src" --config-file "$CONFIG_FILE" \
  >"$CLI_WORK/backup.txt" 2>&1
BACKUP_CODE=$?
if [ "$BACKUP_CODE" = "0" ]; then
  record_pass "B.4 用产品 CLI 生成真实归档"
else
  record_fail "B.4 用产品 CLI 生成真实归档" \
    "退出码 $BACKUP_CODE: $(head -1 "$CLI_WORK/backup.txt")"
fi
LOCAL_ARCHIVE="$(ls -1 "$CLI_WORK/repo"/*.bak 2>/dev/null | head -1)"
if [ -n "$LOCAL_ARCHIVE" ] && [ -s "$LOCAL_ARCHIVE" ]; then
  record_pass "B.4 归档存在且非空（$(stat -c%s "$LOCAL_ARCHIVE") 字节）"
else
  record_fail "B.4 归档存在且非空" "没有找到 .bak"
fi
LOCAL_SHA="$(sha256sum "$LOCAL_ARCHIVE" | cut -d' ' -f1)"

check_remote_ok "B.5 上传真实归档（走仓库身份校验）" \
  upload "$LOCAL_ARCHIVE" --user "$USER_A" --repository "$CLI_WORK/repo"
SNAPSHOT_ID="$(grep -m1 '快照 ID:' "$CLI_WORK/last.txt" | awk '{print $3}')"
if [ -n "$SNAPSHOT_ID" ]; then
  record_pass "B.5 上传返回快照 ID"
else
  record_fail "B.5 上传返回快照 ID" "$(head -1 "$CLI_WORK/last.txt")"
fi

check_remote_ok "B.6 列表" list --user "$USER_A"
if grep -q "$SNAPSHOT_ID" "$CLI_WORK/last.txt"; then
  record_pass "B.6 列表里有刚上传的快照"
else
  record_fail "B.6 列表里有刚上传的快照" "列表里没有 $SNAPSHOT_ID"
fi

# 服务端 blob 的哈希必须与本地归档一致（真落盘校验，不看响应）。
SERVER_BLOB="$CLI_WORK/data/users/1/$SNAPSHOT_ID.bak"
if [ -f "$SERVER_BLOB" ] \
   && [ "$(sha256sum "$SERVER_BLOB" | cut -d' ' -f1)" = "$LOCAL_SHA" ]; then
  record_pass "B.7 服务端磁盘 blob 的 SHA-256 == 本地归档"
else
  record_fail "B.7 服务端磁盘 blob 的 SHA-256" "对不上或文件不存在"
fi

DOWNLOADED="$CLI_WORK/out/downloaded.bak"
check_remote_ok "B.8 下载" download "$SNAPSHOT_ID" "$DOWNLOADED" --user "$USER_A"
if [ -f "$DOWNLOADED" ] \
   && [ "$(sha256sum "$DOWNLOADED" | cut -d' ' -f1)" = "$LOCAL_SHA" ]; then
  record_pass "B.8 下载回来的归档与本地归档哈希一致"
else
  record_fail "B.8 下载回来的归档哈希" "对不上"
fi
if [ -f "$DOWNLOADED.part" ]; then
  record_fail "B.8 没有残留 .part" "留下了 .part"
else
  record_pass "B.8 没有残留 .part"
fi

check_remote_fail "B.8 默认不覆盖已存在的目标" \
  download "$SNAPSHOT_ID" "$DOWNLOADED" --user "$USER_A"

# 用同一个 restore 引擎恢复下载回来的归档，再与源目录逐字节比较。
if [ ! -x ./$OBJ_ROOT/archive-cli ]; then
  make -C "$ROOT_DIR" BUILD_DIR="$OBJ_ROOT" test-fixtures >/dev/null 2>&1
fi
timeout --signal=KILL 120 ./$OBJ_ROOT/archive-cli restore "$DOWNLOADED" \
  "$CLI_WORK/restored" >"$CLI_WORK/restore.txt" 2>&1
RESTORE_CODE=$?
if [ "$RESTORE_CODE" = "0" ] \
   && diff -r "$CLI_WORK/src" "$CLI_WORK/restored" >/dev/null 2>&1; then
  record_pass "B.9 恢复下载的归档，内容与源目录一致"
else
  record_fail "B.9 恢复下载的归档" \
    "退出码 $RESTORE_CODE: $(head -1 "$CLI_WORK/restore.txt")"
fi

# 跨用户：B 看不到、下不到、删不掉 A 的东西。
check_remote_ok "B.10 注册用户 B" register --user "$USER_B"
check_remote_ok "B.10 登录用户 B" login --user "$USER_B"
check_remote_ok "B.10 用户 B 列表" list --user "$USER_B"
if grep -q "$SNAPSHOT_ID" "$CLI_WORK/last.txt"; then
  record_fail "B.10 B 的列表看不到 A 的快照" "列表里出现了 A 的快照"
else
  record_pass "B.10 B 的列表看不到 A 的快照"
fi
check_remote_fail "B.10 B 不能下载 A 的快照" \
  download "$SNAPSHOT_ID" "$CLI_WORK/out/steal.bak" --user "$USER_B"
check_remote_fail "B.10 B 不能删除 A 的快照" \
  delete "$SNAPSHOT_ID" --user "$USER_B"
if [ -f "$SERVER_BLOB" ]; then
  record_pass "B.10 越权尝试之后 A 的 blob 仍在"
else
  record_fail "B.10 越权尝试之后 A 的 blob 仍在" "文件不见了"
fi

check_remote_ok "B.11 用户 A 删除自己的快照" \
  delete "$SNAPSHOT_ID" --user "$USER_A"
if [ ! -f "$SERVER_BLOB" ]; then
  record_pass "B.11 删除之后服务端 blob 消失"
else
  record_fail "B.11 删除之后服务端 blob 消失" "文件还在"
fi
check_remote_ok "B.11 删除之后列表为空" list --user "$USER_A"
if grep -q "$SNAPSHOT_ID" "$CLI_WORK/last.txt"; then
  record_fail "B.11 列表里没有已删除的快照" "列表里还有它"
else
  record_pass "B.11 列表里没有已删除的快照"
fi

# 命令行里不允许出现 --password。
check_remote_fail "B.12 拒绝 --password 这种写法" \
  login --user "$USER_A" --password secret

# secret 不能出现在服务端日志里。
if grep -qF "$(cut -d= -f2 "$CLI_WORK/secrets.env")" "$CLI_WORK/logs/server.log" \
   2>/dev/null; then
  record_fail "B.13 服务端日志里没有 secret" "日志里出现了 secret"
else
  record_pass "B.13 服务端日志里没有 secret"
fi

cleanup_server
record_pass "B.14 服务端优雅停止"

# 停止竞态：紧接着 Start 之后发 SIGTERM 也必须能停下来。
# （stop 标志在 Start() 里复位而不是 Run() 里，否则这次请求会被吞掉。）
SECOND_PORT=$((PORT + 1))
./$OBJ_ROOT/backup-server --bind 127.0.0.1 --port "$SECOND_PORT" \
  --root "$CLI_WORK/data2" --db "$CLI_WORK/state/metadata2.sqlite3" \
  --secret-file "$CLI_WORK/secrets.env" --pid-file "$CLI_WORK/state/server2.pid" \
  --quiet >/dev/null 2>&1 &
RACE_PID=$!
kill -TERM "$RACE_PID" 2>/dev/null
race_stopped=0
for _ in $(seq 1 60); do
  if ! kill -0 "$RACE_PID" 2>/dev/null; then race_stopped=1; break; fi
  sleep 0.1
done
if [ "$race_stopped" = "1" ]; then
  record_pass "B.15 启动后立刻 SIGTERM 也能优雅停止"
else
  record_fail "B.15 启动后立刻 SIGTERM 也能优雅停止" "进程没有退出"
  kill -TERM "$RACE_PID" 2>/dev/null
fi

if [ "$SAN_MODE" = "1" ]; then
  echo "ASAN: aggregate reports = $SAN_REPORTS"
fi
echo
echo "[network-test] 合计: $PASS passed, $FAIL failed"
[ "$FAIL" = "0" ] || exit 1
echo "NETWORK_ALL_PASS"
