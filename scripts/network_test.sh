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

record_pass() { PASS=$((PASS + 1)); echo "  PASS  $1"; }
record_fail() { FAIL=$((FAIL + 1)); echo "  FAIL  $1 -- $2" >&2; }

echo "[network-test] PR #20 remote backup suite"

# 服务端目标文件是测试的前置条件：先确保它们存在（也是零警告的证据）。
if [ ! -f build/src/network/remote_server.o ]; then
  echo "[network-test] build/backup-server is missing; building first..."
  if ! make -j4 server >"$TEST_ROOT/build-server.log" 2>&1; then
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

NET_OBJECTS="build/src/network/network_protocol.o"
SERVER_OBJECTS="build/src/network/network_protocol.o build/src/network/remote_auth.o \
build/src/network/remote_metadata_store.o build/src/network/remote_server.o"
CRYPTO_OBJECTS="build/src/crypto/sha256.o build/src/crypto/hmac.o \
build/src/crypto/pbkdf2.o build/src/crypto/random.o"

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
  if ! g++ -std=c++17 -Wall -Wextra -Wpedantic -I"$ROOT_DIR/include" \
      -I"$ROOT_DIR/tests/unit" "$source" $objects "$@" -o "$binary" \
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
}

echo "[network-test] A. 单元测试"
run_unit network_protocol_test "$NET_OBJECTS"
run_unit remote_auth_test "build/src/network/remote_auth.o $CRYPTO_OBJECTS"
run_unit remote_metadata_store_test \
  "build/src/network/remote_metadata_store.o build/src/network/remote_auth.o \
$CRYPTO_OBJECTS" "$SQLITE_LIBRARY" -pthread
run_unit remote_server_test "$SERVER_OBJECTS $CRYPTO_OBJECTS" \
  "$SQLITE_LIBRARY" -pthread
run_unit remote_transfer_test "$SERVER_OBJECTS $CRYPTO_OBJECTS" \
  "$SQLITE_LIBRARY" -pthread

echo
echo "[network-test] 合计: $PASS passed, $FAIL failed"
[ "$FAIL" = "0" ] || exit 1
echo "NETWORK_ALL_PASS"
