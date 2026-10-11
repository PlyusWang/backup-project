#!/usr/bin/env bash
#
# 认领 / 身份 / 回滚路径的**确定性注入**检查（PR #34 收口）。
#
# 为什么需要它：tests/unit/incremental_restore_test.cpp 里 T24/T25/T26 那几组
# 用例要确定性命中"排他认领失败"与"认领成功但身份取不到"，纯文件系统事实做
# 不到（产品先查一遍同名冲突，之后才 mkdirat），只能靠 LD_PRELOAD 注入
# （tests/integration/claim_inject.c，不加产品侧测试后门）。没有注入器时那几条
# 用例是**显式 SKIP**，而 SKIP 不等于通过 —— 以后有人把同样的缺陷改回来，普通
# 门禁不一定抓得到。本脚本把 SKIP 直接判成失败，并接进 canonical gate。
#
# 职责（只有这些）：
#   1. 在受控临时目录里编译注入器（0 warning）；
#   2. 从**当前源码**编译增量恢复单元测试（不依赖 build/ 里的旧目标文件，
#      quality_test.sh 会 make clean，所以本脚本自己把需要的东西编出来）；
#   3. 只对被测进程设置 LD_PRELOAD，不导出到整轮构建 / GUI / 网络进程；
#   4. 跑 staging / overlay / 父目录竞态几种注入场景；
#   5. 验证注入确实命中、哨兵未变、结果是 GREEN；
#   6. 清掉本轮自己的临时目录。
#
# 下面任何一条成立即失败：注入器编译失败或有警告、LD_PRELOAD 加载失败、注入
# 没有命中、哨兵不一致、出现 SKIP、出现 FAIL、被测程序非零退出或超时、
# 缺汇总行。绝不允许静默退化成"普通未注入测试"。
#
# 退出码：0 = 全绿。

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)" || exit 1
cd "$ROOT_DIR" || exit 1

# 允许用环境变量覆盖编译器；set -u 之下不能直接引用可能未定义的变量，
# 所以先问环境再给默认值。
CXX="$(printenv CXX || true)"
CC="$(printenv CC || true)"
if [ -z "$CXX" ]; then CXX=g++; fi
if [ -z "$CC" ]; then CC=gcc; fi

WORK_DIR="$(mktemp -d /tmp/claim-injection-XXXXXX)" || exit 1
cleanup() { rm -rf "$WORK_DIR"; }
trap cleanup EXIT

OBJECT_DIR="$WORK_DIR/obj"
PRODUCT_LIB="$WORK_DIR/libproduct.a"
INJECTOR="$WORK_DIR/claiminject.so"
INJECT_TARGET="$WORK_DIR/inject.txt"
BINARY="$WORK_DIR/incremental_restore_test"
RUN_LOG="$WORK_DIR/run.log"
INJECTOR_BUILD_LOG="$WORK_DIR/injector-build.log"
PRODUCT_BUILD_LOG="$WORK_DIR/product-build.log"
TEST_BUILD_LOG="$WORK_DIR/test-build.log"

mkdir -p "$OBJECT_DIR"

fail() {
    echo "[claim-injection] FAIL: $*" >&2
    exit 1
}

say() { echo "== $* =="; }

# ---- 1. 注入器 ------------------------------------------------------------
say "编译注入器 tests/integration/claim_inject.c（0 warning）"
if ! "$CC" -shared -fPIC -O2 -std=c11 -Wall -Wextra -Wpedantic -Wshadow \
    -Wformat=2 -o "$INJECTOR" tests/integration/claim_inject.c -ldl \
    >"$INJECTOR_BUILD_LOG" 2>&1; then
    cat "$INJECTOR_BUILD_LOG" >&2
    fail "注入器编译失败"
fi
if grep -q "warning:" "$INJECTOR_BUILD_LOG"; then
    cat "$INJECTOR_BUILD_LOG" >&2
    fail "注入器编译出现警告，按规定视为失败"
fi
if [ ! -f "$INJECTOR" ]; then
    fail "注入器没有生成（$INJECTOR）"
fi

# ---- 2. 从当前源码编译产品核心 --------------------------------------------
say "从当前源码编译 src/**（$WORK_DIR）"
if [ -z "$(find src -name '*.cpp' -print -quit)" ]; then
    fail "找不到任何产品源文件"
fi
COMPILE_ONE="$WORK_DIR/compile-one.sh"
cat >"$COMPILE_ONE" <<'COMPILE_ONE_SCRIPT'
#!/usr/bin/env bash
# 编译一个产品源文件。参数：源文件 目标目录 CXX
set -u
src="$1"
objdir="$2"
cxx="$3"
out="$objdir/$(printf '%s' "$src" | tr '/' '_').o"
"$cxx" -Iinclude -std=c++17 -Wall -Wextra -Wpedantic -O1 -c "$src" -o "$out"
COMPILE_ONE_SCRIPT
chmod +x "$COMPILE_ONE"
if ! find src -name '*.cpp' -print0 | sort -z |
    xargs -0 -P "$(nproc)" -I{} "$COMPILE_ONE" {} "$OBJECT_DIR" "$CXX" \
        >"$PRODUCT_BUILD_LOG" 2>&1; then
    cat "$PRODUCT_BUILD_LOG" >&2
    fail "产品代码编译失败"
fi
if grep -q "warning:" "$PRODUCT_BUILD_LOG"; then
    cat "$PRODUCT_BUILD_LOG" >&2
    fail "产品代码编译出现警告，按规定视为失败"
fi
rm -f "$PRODUCT_LIB"
if ! ar rcs "$PRODUCT_LIB" "$OBJECT_DIR"/*.o; then
    fail "打包产品目标文件失败"
fi

# ---- 3. 编译被测单元测试（用最终源码，不用 build/ 里的残留）--------------
say "编译并链接 tests/unit/incremental_restore_test.cpp"
if ! "$CXX" -Iinclude -Itests/unit -std=c++17 -Wall -Wextra -Wpedantic -O1 \
    tests/unit/incremental_restore_test.cpp "$PRODUCT_LIB" \
    -o "$BINARY" >"$TEST_BUILD_LOG" 2>&1; then
    cat "$TEST_BUILD_LOG" >&2
    fail "单元测试编译/链接失败"
fi
if grep -q "warning:" "$TEST_BUILD_LOG"; then
    cat "$TEST_BUILD_LOG" >&2
    fail "单元测试编译出现警告，按规定视为失败"
fi

# ---- 4. 只在被测进程上设 LD_PRELOAD ---------------------------------------
say "在 LD_PRELOAD 注入器下运行增量恢复测试"
timeout --signal=KILL 900 env LD_PRELOAD="$INJECTOR" \
    R9_CLAIM_INJECT_FILE="$INJECT_TARGET" "$BINARY" >"$RUN_LOG" 2>&1
RUN_CODE=$?

# 诊断用（可选）：设了 CLAIM_INJECTION_LOG_DIR 就把本轮的原始日志留一份，
# 否则临时目录连同日志一起清掉。默认不设，免得往仓库里掉文件。
KEEP_DIR="$(printenv CLAIM_INJECTION_LOG_DIR || true)"
if [ -n "$KEEP_DIR" ]; then
    mkdir -p "$KEEP_DIR" && cp "$RUN_LOG" "$KEEP_DIR/unit-run.log"
    cp "$PRODUCT_BUILD_LOG" "$KEEP_DIR/product-build.log" 2>/dev/null
    cp "$INJECTOR_BUILD_LOG" "$KEEP_DIR/injector-build.log" 2>/dev/null
    cp "$TEST_BUILD_LOG" "$KEEP_DIR/test-build.log" 2>/dev/null
fi

# ---- 5. 判定 ---------------------------------------------------------------
if grep -q "cannot be preloaded" "$RUN_LOG"; then
    tail -n 20 "$RUN_LOG" >&2
    fail "LD_PRELOAD 加载失败（注入器没有被装进被测进程）"
fi
if [ "$RUN_CODE" -eq 137 ]; then
    tail -n 40 "$RUN_LOG" >&2
    fail "被测进程超时被强杀"
fi
if [ "$RUN_CODE" -ne 0 ]; then
    tail -n 60 "$RUN_LOG" >&2
    fail "被测进程非零退出（exit=$RUN_CODE）"
fi

count_fired() {
    grep -c "\[claiminject\] FIRED: $1" "$RUN_LOG" || true
}
fired_eexist="$(count_fired mkdirat-eexist)"
fired_swap="$(count_fired mkdirat-swap)"
fired_probe="$(count_fired fstatat-fail)"
fired_race="$(count_fired mkdir-race)"

echo "[claim-injection] 注入命中：mkdirat-eexist=$fired_eexist mkdirat-swap=$fired_swap fstatat-fail=$fired_probe mkdir-race=$fired_race"

if [ "$fired_eexist" -lt 2 ]; then
    fail "mkdirat-eexist 注入没有命中（期望 >= 2，实际 $fired_eexist）"
fi
if [ "$fired_swap" -lt 4 ]; then
    fail "mkdirat-swap 注入没有命中（期望 >= 4，实际 $fired_swap）"
fi
if [ "$fired_probe" -lt 4 ]; then
    fail "身份探测失败注入没有命中（期望 >= 4，实际 $fired_probe）"
fi
if [ "$fired_race" -lt 2 ]; then
    fail "父目录竞态注入没有命中（期望 >= 2，实际 $fired_race）"
fi

# SKIP 不是通过：有注入器在场还出现 SKIP，说明用例根本没跑。
SKIPS="$(grep -c 'SKIP' "$RUN_LOG" || true)"
if [ "$SKIPS" -ne 0 ]; then
    grep 'SKIP' "$RUN_LOG" >&2
    fail "出现 $SKIPS 条 SKIP —— 注入用例没有被真正执行"
fi
FAILS="$(grep -c '^  FAIL' "$RUN_LOG" || true)"
if [ "$FAILS" -ne 0 ]; then
    grep '^  FAIL' "$RUN_LOG" >&2
    fail "出现 $FAILS 条断言失败"
fi

# 每条场景都必须留下**通过**记录，防止"用例被删掉"也算绿。
for label in \
    "INC-R T24 A" "INC-R T24 B" "INC-R T25 A" "INC-R T25 B" "INC-R T25 C" \
    "INC-R T25 D" "INC-R T26 D" "INC-R T26 E"; do
    if ! grep -q "^  PASS  $label" "$RUN_LOG"; then
        fail "缺少通过的用例：$label"
    fi
done

SENTINELS="$(grep -c '受保护内容一个字节都没少' "$RUN_LOG" || true)"
if [ "$SENTINELS" -lt 7 ]; then
    fail "受保护哨兵校验少于 7 条（实际 $SENTINELS）"
fi

SUMMARY="$(grep -E '^incremental_restore_test: [0-9]+/[0-9]+ checks passed$' \
    "$RUN_LOG" | tail -n 1)"
if [ -z "$SUMMARY" ]; then
    tail -n 20 "$RUN_LOG" >&2
    fail "缺少汇总行"
fi
NUMBERS="$(printf '%s' "$SUMMARY" | sed 's/^incremental_restore_test: //')"
PASSED="$(printf '%s' "$NUMBERS" | cut -d/ -f1)"
TOTAL="$(printf '%s' "$NUMBERS" | cut -d/ -f2 | cut -d' ' -f1)"
if [ "$PASSED" != "$TOTAL" ]; then
    fail "用例没有全过：$SUMMARY"
fi

echo "[claim-injection] 注入命中且全部通过（$SUMMARY）"
exit 0
