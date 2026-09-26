#!/usr/bin/env bash
#
# 手写密码学原语模块的单元测试入口。
#
# 编译 src/crypto/*.cpp + tests/unit/crypto_test.cpp，跑官方向量、流式一致性、
# 边界与负路径检查。
#
# 产物一律放在 /tmp 下的临时目录：tests/output 是别的测试脚本的目录，不在这里
# 抢地盘，退出时自动清理。
#
# 环境变量 CRYPTO_TEST_EXTRA_FLAGS 可以追加编译参数（例如 sanitizer 构建），
# 默认不追加任何东西。
#
# 退出码：0 = 全部通过。编译错误、编译警告、断言失败都非零退出。

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

WORK_DIR="$(mktemp -d /tmp/crypto-test-XXXXXX)"
trap 'rm -rf "$WORK_DIR"' EXIT

BIN="$WORK_DIR/crypto_test"
BUILD_LOG="$WORK_DIR/build.log"
RUN_LOG="$WORK_DIR/run.log"
EXTRA_FLAGS="${CRYPTO_TEST_EXTRA_FLAGS:-}"

SOURCES="src/crypto/sha256.cpp src/crypto/hmac.cpp src/crypto/pbkdf2.cpp src/crypto/des.cpp src/crypto/aes.cpp src/crypto/random.cpp"

echo "[crypto] 编译模块与单元测试（${WORK_DIR}）"
g++ $EXTRA_FLAGS -std=c++17 -Wall -Wextra -Wpedantic -Iinclude $SOURCES tests/unit/crypto_test.cpp -o "$BIN" > "$BUILD_LOG" 2>&1
BUILD_CODE=$?
if [ $BUILD_CODE -ne 0 ]; then
    echo "[crypto] 编译失败（exit=$BUILD_CODE）："
    cat "$BUILD_LOG"
    echo "crypto: 0/0 checks passed"
    exit 1
fi
if grep -q "warning:" "$BUILD_LOG"; then
    echo "[crypto] 编译出现警告，按规定视为失败："
    cat "$BUILD_LOG"
    echo "crypto: 0/0 checks passed"
    exit 1
fi
echo "[crypto] 编译通过，0 warning"

echo "[crypto] 运行单元测试"
"$BIN" > "$RUN_LOG" 2>&1
RUN_CODE=$?
cat "$RUN_LOG"

SUMMARY="$(grep -E '^crypto-test: [0-9]+/[0-9]+ checks passed$' "$RUN_LOG" | tail -n 1)"
PASSED=0
TOTAL=0
if [ -n "$SUMMARY" ]; then
    NUMBERS="${SUMMARY#crypto-test: }"
    PASSED="${NUMBERS%%/*}"
    TOTAL="${NUMBERS#*/}"
    TOTAL="${TOTAL%% *}"
fi

echo "crypto: $PASSED/$TOTAL checks passed"

if [ $RUN_CODE -ne 0 ]; then
    echo "[crypto] 存在失败项（exit=$RUN_CODE）" >&2
    exit 1
fi
if [ "$TOTAL" = "0" ] || [ "$PASSED" != "$TOTAL" ]; then
    echo "[crypto] 汇总行缺失或计数不一致：$SUMMARY" >&2
    exit 1
fi
exit 0
