#!/usr/bin/env bash
#
# bpsec2_test.sh —— BPSEC2（服务器签名身份）握手测试入口（PR #23 Phase 1）。
#
#   bash scripts/bpsec2_test.sh
#
# 两层：
#   A. tests/unit/bpsec2_test.cpp：正向握手、九类失败路径、拒绝降级、
#      10000 条畸形 ServerHello/ServerCertificate 的 fuzz；
#   B. 与 BPSEC1 的关系：本脚本另外跑一遍 secure_transport_test.sh，
#      证明"加了 BPSEC2 之后 BPSEC1 一点没变"（那 15 个用例是 BPSEC1 的
#      行为基线，必须原样通过）。
#
# 环境变量 BPSEC2_TEST_EXTRA_FLAGS 可以追加编译参数（sanitizer 构建）。
# 退出码：0 = 全部通过。

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

WORK_DIR="$(mktemp -d /tmp/bpsec2-test-XXXXXX)"
trap 'rm -rf "$WORK_DIR"' EXIT

set +u
EXTRA_FLAGS="$BPSEC2_TEST_EXTRA_FLAGS"
set -u

PASS=0
FAIL=0
record_pass() { PASS=$((PASS + 1)); echo "  PASS  $1"; }
record_fail() { FAIL=$((FAIL + 1)); echo "  FAIL  $1 -- $2" >&2; }

CRYPTO_SOURCES="src/crypto/sha256.cpp src/crypto/hmac.cpp src/crypto/pbkdf2.cpp \
src/crypto/aes.cpp src/crypto/random.cpp src/crypto/x25519.cpp src/crypto/hkdf.cpp \
src/crypto/sha512.cpp src/crypto/ed25519.cpp src/crypto/bpcert.cpp \
src/crypto/trusted_root_store.cpp"
NET_SOURCES="src/network/network_protocol.cpp src/network/secure_transport.cpp"

BUILD_LOG="$WORK_DIR/build.log"
# shellcheck disable=SC2086
if ! g++ $EXTRA_FLAGS -std=c++17 -Wall -Wextra -Wpedantic -Iinclude -pthread \
      $CRYPTO_SOURCES $NET_SOURCES tests/unit/bpsec2_test.cpp \
      -o "$WORK_DIR/bpsec2_test" > "$BUILD_LOG" 2>&1; then
  record_fail "编译 bpsec2_test" "$(tail -8 "$BUILD_LOG" | tr '\n' ' ')"
  echo "[bpsec2] passed=$PASS failed=$FAIL"
  exit 1
fi
if [ -s "$BUILD_LOG" ]; then
  record_fail "编译必须零警告" "$(head -5 "$BUILD_LOG" | tr '\n' ' ')"
else
  record_pass "零警告编译（-Wall -Wextra -Wpedantic）"
fi

if timeout 900 "$WORK_DIR/bpsec2_test" > "$WORK_DIR/run.log" 2>&1; then
  record_pass "$(grep -oE 'passed=[0-9]+ failed=[0-9]+' "$WORK_DIR/run.log" | tail -1)（正向 / 失败路径 / 拒绝降级 / 10000 例 fuzz）"
else
  record_fail "bpsec2_test 退出码" "$(grep -m5 FAIL "$WORK_DIR/run.log" | tr '\n' ' ')"
fi
if grep -qE 'ERROR: (AddressSanitizer|LeakSanitizer|UndefinedBehaviorSanitizer)|runtime error:' "$WORK_DIR/run.log"; then
  record_fail "消毒剂报告" "$(grep -m2 -E 'ERROR|runtime error' "$WORK_DIR/run.log" | tr '\n' ' ')"
else
  record_pass "消毒剂报告 0 条"
fi

# B. BPSEC1 行为基线：加了 BPSEC2 之后必须一点没变。
if timeout 2400 bash scripts/secure_transport_test.sh > "$WORK_DIR/bpsec1.log" 2>&1; then
  record_pass "BPSEC1 行为基线原样通过（15 个用例，$(grep -c "checks passed" "$WORK_DIR/bpsec1.log" || true) 组）"
else
  record_fail "BPSEC1 行为基线" "$(tail -3 "$WORK_DIR/bpsec1.log" | tr '\n' ' ')"
fi

echo "[bpsec2] passed=$PASS failed=$FAIL"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
