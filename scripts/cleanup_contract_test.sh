#!/usr/bin/env bash
#
# 失败路径清理合同的专项入口（src/core 与 src/realtime 里"写临时文件 -> fsync ->
# close -> rename"那几段）。
#
# 覆盖：
#   * fsync 失败后 close 仍然必须执行（`fsync(fd) != 0 || close(fd) != 0` 这类
#     短路会在 fsync 失败时把 close 整个跳掉，每失败一次泄漏一个 fd）；
#   * 多个 fd 的清理必须逐个尝试；
#   * 诊断保留**第一次**失败的原因，后续清理不得改写它；
#   * 失败后不留半成品，成功路径行为不变。
#
# 注入方式：tests/review/cleanup_fault_interposer.cpp 在链接期定义 fsync() /
# close()（放在 libc 之前，所以产品代码里的同名调用解析到它），可以指定"第几次
# 调用失败"与失败的 errno —— 确定性，不 sleep 撞运气，不用 LD_PRELOAD。
#
# 编译整份 src/**：被测的是真实产品代码，不做链接替身。
# 产物一律落在 mktemp 目录里，退出时清理，不写 tests/output。
#
# 环境变量 CLEANUP_CONTRACT_TEST_EXTRA_FLAGS 可以追加编译参数（sanitizer 构建）：
#   CLEANUP_CONTRACT_TEST_EXTRA_FLAGS="-g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer" \
#       bash scripts/cleanup_contract_test.sh
#
# 退出码：0 = 全绿。编译错误、编译警告、断言失败都非零退出。
# 末行固定打印 "cleanup-short-circuit: N/M checks passed"。

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

CXX="${CXX:-g++}"
EXTRA_FLAGS="${CLEANUP_CONTRACT_TEST_EXTRA_FLAGS:-}"

WORK_DIR="$(mktemp -d /tmp/cleanup-contract-XXXXXX)"
trap 'rm -rf "$WORK_DIR"' EXIT

OBJ_DIR="$WORK_DIR/obj"
LIB="$WORK_DIR/libproduct.a"
mkdir -p "$OBJ_DIR"

say() { echo "== $* =="; }

say "编译产品代码 src/**/*.cpp（$WORK_DIR）"
mapfile -t SOURCES < <(find src -name '*.cpp' | sort)
if [[ ${#SOURCES[@]} -eq 0 ]]; then
    echo "[cleanup-contract] 找不到任何产品源文件" >&2
    echo "cleanup-short-circuit: 0/0 checks passed"
    exit 1
fi
BUILD_LOG="$WORK_DIR/product-build.log"
if ! printf '%s\n' "${SOURCES[@]}" | xargs -P "$(nproc)" -I{} bash -c '
    src="$1"; objdir="$2"; cxx="$3"; flags="$4"
    out="$objdir/$(echo "$src" | tr "/" "_").o"
    # shellcheck disable=SC2086
    "$cxx" -Iinclude -std=c++17 -Wall -Wextra -Wpedantic -O1 $flags -c "$src" -o "$out"
' _ {} "$OBJ_DIR" "$CXX" "$EXTRA_FLAGS" >"$BUILD_LOG" 2>&1; then
    echo "[cleanup-contract] 产品代码编译失败：" >&2
    cat "$BUILD_LOG" >&2
    echo "cleanup-short-circuit: 0/0 checks passed"
    exit 1
fi
if grep -q "warning:" "$BUILD_LOG"; then
    echo "[cleanup-contract] 产品代码编译出现警告，按规定视为失败：" >&2
    cat "$BUILD_LOG" >&2
    echo "cleanup-short-circuit: 0/0 checks passed"
    exit 1
fi
rm -f "$LIB"
ar rcs "$LIB" "$OBJ_DIR"/*.o

say "编译并运行 cleanup-short-circuit"
BIN="$WORK_DIR/cleanup_short_circuit_test"
RUN_LOG="$WORK_DIR/run.log"
TEST_BUILD_LOG="$WORK_DIR/test-build.log"
if ! "$CXX" -Iinclude -Itests/unit -std=c++17 -Wall -Wextra -Wpedantic -O1 \
    $EXTRA_FLAGS \
    tests/unit/cleanup_short_circuit_test.cpp \
    tests/review/cleanup_fault_interposer.cpp \
    "$LIB" -o "$BIN" >"$TEST_BUILD_LOG" 2>&1; then
    echo "[cleanup-contract] 测试编译/链接失败：" >&2
    cat "$TEST_BUILD_LOG" >&2
    echo "cleanup-short-circuit: 0/0 checks passed"
    exit 1
fi
if grep -q "warning:" "$TEST_BUILD_LOG"; then
    echo "[cleanup-contract] 测试编译出现警告，按规定视为失败：" >&2
    cat "$TEST_BUILD_LOG" >&2
    echo "cleanup-short-circuit: 0/0 checks passed"
    exit 1
fi

"$BIN" >"$RUN_LOG" 2>&1
RUN_CODE=$?
cat "$RUN_LOG"

SUMMARY="$(grep -E '^cleanup-short-circuit: [0-9]+/[0-9]+ checks passed$' "$RUN_LOG" | tail -n 1)"
if [[ -z "$SUMMARY" ]]; then
    echo "[cleanup-contract] 缺少汇总行" >&2
    echo "cleanup-short-circuit: 0/0 checks passed"
    exit 1
fi
NUMBERS="${SUMMARY#cleanup-short-circuit: }"
PASSED="${NUMBERS%%/*}"
TOTAL="${NUMBERS#*/}"
TOTAL="${TOTAL%% *}"
if [[ "$RUN_CODE" -ne 0 || "$PASSED" != "$TOTAL" ]]; then
    echo "[cleanup-contract] 用例失败：$SUMMARY" >&2
    exit 1
fi
echo "[cleanup-contract] 全部通过（$SUMMARY）"
exit 0
