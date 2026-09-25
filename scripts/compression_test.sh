#!/usr/bin/env bash
#
# 压缩模块的编译 + 测试 + 基准入口。
#
# 只做两件事：
#   1. 用 -Wall -Wextra -Wpedantic 编译 src/compression/*.cpp + 它们依赖的 src/core/file_io.cpp；
#   2. 跑单元测试与 benchmark，最后打印 "compression: N/M checks passed"。
#
# 产物一律放在临时目录（默认 /tmp），不写 tests/output，避免和别的任务抢文件。
# 额外编译参数走环境变量 EXTRA_CXXFLAGS，例如 ASan/UBSan 那一轮：
#   EXTRA_CXXFLAGS="-g -O1 -fsanitize=address,undefined" bash scripts/compression_test.sh

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="${COMPRESSION_TEST_OUT_DIR:-${TMPDIR:-/tmp}/compression-test}"

CXX="${CXX:-g++}"
CXXFLAGS=(-std=c++17 -Wall -Wextra -Wpedantic -O2 -I"$ROOT_DIR/include")
# shellcheck disable=SC2206
if [[ -n "${EXTRA_CXXFLAGS:-}" ]]; then
    CXXFLAGS+=(${EXTRA_CXXFLAGS})
fi

# 编解码器现在只有一份实现，字符串接口与文件接口共用同一套比特语义，
# 因此也依赖 file_io 的 FileSink / FileSource / ByteSinkAdapter。
SOURCES=(
    "$ROOT_DIR/src/compression/codec_io.cpp"
    "$ROOT_DIR/src/compression/huffman.cpp"
    "$ROOT_DIR/src/compression/lzss.cpp"
    "$ROOT_DIR/src/core/file_io.cpp"
)

mkdir -p "$OUT_DIR"

echo "[compression] 编译单元测试 -> $OUT_DIR/compression_test"
if ! "$CXX" "${CXXFLAGS[@]}" "${SOURCES[@]}" \
    "$ROOT_DIR/tests/unit/compression_test.cpp" -o "$OUT_DIR/compression_test"; then
    echo "compression: 0/1 checks passed (单元测试编译失败)"
    exit 1
fi

echo "[compression] 编译 benchmark -> $OUT_DIR/compression_bench"
if ! "$CXX" "${CXXFLAGS[@]}" "${SOURCES[@]}" \
    "$ROOT_DIR/tests/unit/compression_bench.cpp" -o "$OUT_DIR/compression_bench"; then
    echo "compression: 0/1 checks passed (benchmark 编译失败)"
    exit 1
fi

echo "[compression] 运行单元测试"
TEST_OUTPUT="$("$OUT_DIR/compression_test" 2>&1)"
TEST_STATUS=$?
printf '%s\n' "$TEST_OUTPUT"

TEST_SUMMARY="$(printf '%s\n' "$TEST_OUTPUT" |
    grep -E '^compression_test: [0-9]+/[0-9]+ checks passed' | tail -n 1)"
TEST_PASSED=0
TEST_TOTAL=0
if [[ -n "$TEST_SUMMARY" ]]; then
    TEST_PASSED="$(printf '%s' "$TEST_SUMMARY" | sed -E 's#^compression_test: ([0-9]+)/([0-9]+).*#\1#')"
    TEST_TOTAL="$(printf '%s' "$TEST_SUMMARY" | sed -E 's#^compression_test: ([0-9]+)/([0-9]+).*#\2#')"
fi

echo
echo "[compression] 运行 benchmark"
"$OUT_DIR/compression_bench" "$ROOT_DIR"
BENCH_STATUS=$?

# benchmark 的往返校验算第 (TEST_TOTAL + 1) 项检查。
TOTAL=$((TEST_TOTAL + 1))
PASSED=$TEST_PASSED
if [[ $BENCH_STATUS -eq 0 ]]; then
    PASSED=$((PASSED + 1))
fi

echo
echo "compression: $PASSED/$TOTAL checks passed"

if [[ $TEST_STATUS -ne 0 || $BENCH_STATUS -ne 0 || -z "$TEST_SUMMARY" ]]; then
    echo "compression: FAILED (unit test exit $TEST_STATUS, benchmark exit $BENCH_STATUS)"
    exit 1
fi
exit 0
