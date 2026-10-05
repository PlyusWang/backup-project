#!/usr/bin/env bash
#
# server_profile_test.sh —— ServerProfile / .bpserver 的测试入口（PR #23）。
#
#   bash scripts/server_profile_test.sh
#
# 除了单元测试，还会核对一件只有在这台机器上才能核对的事：编进二进制的
# 官方云端主机名与 ssh -G aliyun-ecs 的 HostName 是否一致（有 ssh 配置才查）。
#
# 退出码：0 = 全部通过。

set -uo pipefail
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

set +u
EXTRA_FLAGS="$SERVER_PROFILE_TEST_EXTRA_FLAGS"
set -u

WORK_DIR="$(mktemp -d /tmp/server-profile-test-XXXXXX)"
trap 'rm -rf "$WORK_DIR"' EXIT

PASS=0
FAIL=0
record_pass() { PASS=$((PASS + 1)); echo "  PASS  $1"; }
record_fail() { FAIL=$((FAIL + 1)); echo "  FAIL  $1 -- $2" >&2; }

CRYPTO_SOURCES="src/crypto/sha256.cpp src/crypto/hmac.cpp src/crypto/pbkdf2.cpp \
src/crypto/aes.cpp src/crypto/random.cpp src/crypto/x25519.cpp src/crypto/hkdf.cpp \
src/crypto/sha512.cpp src/crypto/ed25519.cpp src/crypto/bpcert.cpp \
src/crypto/trusted_root_store.cpp"

# shellcheck disable=SC2086
if ! g++ $EXTRA_FLAGS -std=c++17 -Wall -Wextra -Wpedantic -Iinclude $CRYPTO_SOURCES \
      src/network/network_protocol.cpp src/network/secure_transport.cpp \
      src/network/remote_backup_client.cpp src/network/server_profile.cpp \
      src/core/file_io.cpp src/platform/app_paths.cpp src/core/simple_json.cpp \
      tests/unit/server_profile_test.cpp -o "$WORK_DIR/profile_test" \
      > "$WORK_DIR/build.log" 2>&1; then
  record_fail "编译 server_profile_test" "$(tail -8 "$WORK_DIR/build.log" | tr '\n' ' ')"
  echo "[profile] passed=$PASS failed=$FAIL"
  exit 1
fi
if [ -s "$WORK_DIR/build.log" ]; then
  record_fail "编译必须零警告" "$(head -5 "$WORK_DIR/build.log" | tr '\n' ' ')"
else
  record_pass "零警告编译（-Wall -Wextra -Wpedantic）"
fi

if "$WORK_DIR/profile_test" > "$WORK_DIR/run.log" 2>&1; then
  record_pass "$(grep -oE 'passed=[0-9]+ failed=[0-9]+' "$WORK_DIR/run.log" | tail -1)（官方云端零配置 / 三种身份往返 / 九条严格解析 / 端点翻译）"
else
  record_fail "server_profile_test 退出码" "$(grep -m5 FAIL "$WORK_DIR/run.log" | tr '\n' ' ')"
fi
if grep -qE 'ERROR: (AddressSanitizer|LeakSanitizer|UndefinedBehaviorSanitizer)|runtime error:' "$WORK_DIR/run.log"; then
  record_fail "消毒剂报告" "有消毒剂报告"
else
  record_pass "消毒剂报告 0 条"
fi

if ssh -G aliyun-ecs >/dev/null 2>&1; then
  DEPLOYED_HOST="$(ssh -G aliyun-ecs 2>/dev/null | awk '/^hostname /{print $2}')"
  COMPILED_HOST="$(grep -oE 'kOfficialCloudHost = "[^"]+"' include/server_profile.h | cut -d'"' -f2)"
  if [ "$DEPLOYED_HOST" = "$COMPILED_HOST" ]; then
    record_pass "编进二进制的官方主机名与 ssh -G aliyun-ecs 的 HostName 一致（$DEPLOYED_HOST）"
  else
    record_fail "官方主机名与部署事实一致" "编进去的是 $COMPILED_HOST，ssh -G 给的是 $DEPLOYED_HOST"
  fi
else
  record_pass "跳过 ssh -G 核对（本机没有 aliyun-ecs 配置）"
fi

echo "[profile] passed=$PASS failed=$FAIL"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1