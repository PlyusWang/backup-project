#!/usr/bin/env bash
#
# file_io 与流水线安全 / 权限测试的编译 + 运行入口。
#
# 覆盖：
#   * file_io_test：FileSink 的清理状态机（含 fsync / close 注入失败）、
#     PublishNoReplace、TempDirectoryGuard、CheckFreeSpace；
#   * pipeline_security_test：0600 产物、失败后不留最终 .bak / 不留目标目录、
#     父目录里没有临时残余、以及"运行中观察"私有工作目录权限。
#
# mutation 扫描不在这里跑：它仍然由 scripts/archive_pipeline_test.sh 负责。
#
# 为什么要自己编整份 src/**：被测的是真实产品代码，不做任何链接替身。
# 产物一律落在 mktemp 目录里，退出时清理，不写 tests/output。
#
# 环境变量 FILE_IO_TEST_EXTRA_FLAGS 可以追加编译参数（例如 sanitizer）：
#   FILE_IO_TEST_EXTRA_FLAGS="-g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer" \
#       bash scripts/file_io_test.sh
#
# 退出码：0 = 全绿。编译错误、编译警告、断言失败都非零退出。
# 末行固定打印 "file-io: N/M checks passed"。

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

CXX="${CXX:-g++}"
EXTRA_FLAGS="${FILE_IO_TEST_EXTRA_FLAGS:-}"

WORK_DIR="$(mktemp -d /tmp/file-io-test-XXXXXX)"
trap 'rm -rf "$WORK_DIR"' EXIT

OBJ_DIR="$WORK_DIR/obj"
LIB="$WORK_DIR/libarchivecore.a"
mkdir -p "$OBJ_DIR"

say() { echo "== $* =="; }

# 编译整份产品代码。警告按失败处理：这个项目里 -Wall -Wextra -Wpedantic 是
# 硬要求，警告混在日志里比直接失败更危险。
say "编译产品代码 src/**/*.cpp（$WORK_DIR）"
mapfile -t SOURCES < <(find src -name '*.cpp' | sort)
if [[ ${#SOURCES[@]} -eq 0 ]]; then
    echo "[file-io] 找不到任何产品源文件" >&2
    echo "file-io: 0/0 checks passed"
    exit 1
fi
BUILD_LOG="$WORK_DIR/product-build.log"
if ! printf '%s\n' "${SOURCES[@]}" | xargs -P "$(nproc)" -I{} bash -c '
    src="$1"; objdir="$2"; cxx="$3"; flags="$4"
    out="$objdir/$(echo "$src" | tr "/" "_").o"
    # shellcheck disable=SC2086
    "$cxx" -Iinclude -std=c++17 -Wall -Wextra -Wpedantic -O1 $flags -c "$src" -o "$out"
' _ {} "$OBJ_DIR" "$CXX" "$EXTRA_FLAGS" >"$BUILD_LOG" 2>&1; then
    echo "[file-io] 产品代码编译失败：" >&2
    cat "$BUILD_LOG" >&2
    echo "file-io: 0/0 checks passed"
    exit 1
fi
if grep -q "warning:" "$BUILD_LOG"; then
    echo "[file-io] 产品代码编译出现警告，按规定视为失败：" >&2
    cat "$BUILD_LOG" >&2
    echo "file-io: 0/0 checks passed"
    exit 1
fi
rm -f "$LIB"
ar rcs "$LIB" "$OBJ_DIR"/*.o

FAILURES=0
TOTAL_CHECKS=0
TOTAL_PASSED=0

compile_and_run() {
    local name="$1"
    local source="$2"
    say "编译并运行 $name"
    local binary="$WORK_DIR/$name"
    local build_log="$WORK_DIR/$name.build.log"
    local run_log="$WORK_DIR/$name.run.log"
    # shellcheck disable=SC2086
    if ! "$CXX" -Iinclude -Itests/unit -std=c++17 -Wall -Wextra -Wpedantic \
        -O1 $EXTRA_FLAGS "$source" "$LIB" -o "$binary" >"$build_log" 2>&1; then
        echo "[file-io] $name 编译失败：" >&2
        cat "$build_log" >&2
        FAILURES=$((FAILURES + 1))
        return 1
    fi
    if grep -q "warning:" "$build_log"; then
        echo "[file-io] $name 编译出现警告，按规定视为失败：" >&2
        cat "$build_log" >&2
        FAILURES=$((FAILURES + 1))
        return 1
    fi

    "$binary" >"$run_log" 2>&1
    local status=$?
    cat "$run_log"

    local summary
    summary="$(grep -E "^${name}: [0-9]+/[0-9]+ checks passed\$" "$run_log" | tail -n 1)"
    if [[ -n "$summary" ]]; then
        local numbers="${summary#*: }"
        local passed="${numbers%%/*}"
        local total="${numbers#*/}"
        total="${total%% *}"
        TOTAL_CHECKS=$((TOTAL_CHECKS + total))
        TOTAL_PASSED=$((TOTAL_PASSED + passed))
    else
        echo "[file-io] $name 没有打印汇总行" >&2
        FAILURES=$((FAILURES + 1))
    fi
    if [[ $status -ne 0 ]]; then
        echo "[file-io] $name 存在失败项（exit=$status）" >&2
        FAILURES=$((FAILURES + 1))
    fi
    return 0
}

compile_and_run file_io_test tests/unit/file_io_test.cpp
compile_and_run pipeline_security_test tests/unit/pipeline_security_test.cpp

# 静态检查：PublishNoReplace 里不许再出现普通 rename()。
# "lstat(final) 确认不存在 + 普通 rename" 有真实 TOCTOU：检查与 rename 之间
# 另一个进程可以创建 final，而普通 rename 会直接覆盖它。这条断言把"绝不再
# 退回非原子发布"钉在源码上，而不是只钉在行为测试上。
publish_body="$(awk '/^bool PublishNoReplace[(]/{inside=1} inside{print} inside && /^}$/{exit}' "$ROOT_DIR/src/core/file_io.cpp")"
if [[ -z "$publish_body" ]]; then
    echo "[file-io] 静态检查失败：找不到 PublishNoReplace 函数体" >&2
    FAILURES=$((FAILURES + 1))
elif printf '%s' "$publish_body" | grep -q '::rename('; then
    echo "[file-io] 静态检查失败：PublishNoReplace 仍在使用普通 rename()" >&2
    FAILURES=$((FAILURES + 1))
else
    TOTAL_CHECKS=$((TOTAL_CHECKS + 1))
    TOTAL_PASSED=$((TOTAL_PASSED + 1))
    echo "[file-io] PASS: PublishNoReplace 里没有普通 rename() 回退"
fi

echo
echo "file-io: $TOTAL_PASSED/$TOTAL_CHECKS checks passed"
if [[ $FAILURES -ne 0 || $TOTAL_CHECKS -eq 0 || $TOTAL_PASSED -ne $TOTAL_CHECKS ]]; then
    echo "[file-io] 失败 $FAILURES 项" >&2
    exit 1
fi
exit 0
