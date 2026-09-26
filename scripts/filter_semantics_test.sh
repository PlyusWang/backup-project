#!/usr/bin/env bash
#
# Filter 语义一致性测试入口：编译并运行 tests/unit/filter_semantics_test.cpp，
# 再对 src/core/user_directory.cpp 做静态检查。
#
# 静态检查为什么放在这里：group 用 group 的 sysconf hint（_SC_GETGR_R_SIZE_MAX）
# 与 ERANGE 翻倍重试这两条，运行期没法人为制造（NSS 返回多大的记录不由我们决定），
# 只能把源码形状钉住。
#
# 输出目录走 /tmp，不写 tests/output/，可用环境变量覆盖：
#   FILTER_SEMANTICS_OUT_DIR   构建与日志目录
#   FILTER_SEMANTICS_SANITIZE  置 1 时用 -fsanitize=address,undefined 构建并运行
#
# 最后一行固定打印 "filter-semantics: N/M checks passed"；任何失败一律非零退出。

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

OUT_DIR="${FILTER_SEMANTICS_OUT_DIR:-$(mktemp -d /tmp/filter-semantics.XXXXXX)}"
mkdir -p "$OUT_DIR"
BIN="$OUT_DIR/filter_semantics_test"
BUILD_LOG="$OUT_DIR/build.log"
RUN_LOG="$OUT_DIR/run.log"

EXTRA_FLAGS=""
if [ "${FILTER_SEMANTICS_SANITIZE:-0}" = "1" ]; then
  EXTRA_FLAGS="-g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer"
  echo "[filter-semantics] ASan + UBSan 构建"
fi

SOURCES="tests/unit/filter_semantics_test.cpp src/filter/filter.cpp \
  src/core/user_directory.cpp src/core/tree_scanner.cpp src/core/archive_entry.cpp \
  src/archive/archive_path.cpp src/filesystem/file_system.cpp"

echo "[filter-semantics] 编译：g++ -std=c++17 -Wall -Wextra -Wpedantic -Iinclude $EXTRA_FLAGS"
# shellcheck disable=SC2086
g++ -std=c++17 -Wall -Wextra -Wpedantic -Iinclude $EXTRA_FLAGS $SOURCES -o "$BIN" > "$BUILD_LOG" 2>&1
BUILD_CODE=$?
if [ $BUILD_CODE -ne 0 ]; then
  echo "[filter-semantics] 编译失败（exit=$BUILD_CODE），日志：$BUILD_LOG"
  tail -30 "$BUILD_LOG"
  echo "filter-semantics: 0/1 checks passed"
  exit 1
fi

WARNINGS=$(grep -c "warning:" "$BUILD_LOG")
echo "[filter-semantics] 编译警告数：$WARNINGS"
if [ "$WARNINGS" != "0" ]; then
  grep "warning:" "$BUILD_LOG" | head -10
  echo "filter-semantics: 0/1 checks passed"
  exit 1
fi

"$BIN" > "$RUN_LOG" 2>&1
RUN_CODE=$?
# 汇总行统一由脚本最后打印一次，日志里先过滤掉它，避免重复。
grep -v -E "^filter-semantics: [0-9]+/[0-9]+ checks passed$" "$RUN_LOG"
SUMMARY=$(grep -E "^filter-semantics: [0-9]+/[0-9]+ checks passed$" "$RUN_LOG" | tail -1)
if [ -z "$SUMMARY" ]; then
  echo "[filter-semantics] 测试没有打印汇总行（exit=$RUN_CODE），日志：$RUN_LOG"
  echo "filter-semantics: 0/1 checks passed"
  exit 1
fi

PASSED="${SUMMARY#filter-semantics: }"
PASSED="${PASSED%%/*}"
TOTAL="${SUMMARY##*/}"
TOTAL="${TOTAL%% *}"

FAIL_CODE=0
if [ "$RUN_CODE" -ne 0 ]; then
  echo "[filter-semantics] 存在失败断言（exit=$RUN_CODE），日志：$RUN_LOG" >&2
  FAIL_CODE=1
fi

# ---- 静态检查：user_directory.cpp 的 group / user 解析 ----------------------
#
# 用 awk 截函数体，而不是整文件 grep：同一个常量在文件里出现两次也算过，
# 那样就查不出"group 误用 passwd 的 hint"这类错。
USER_DIR="src/core/user_directory.cpp"
USER_BODY="$(awk '/^bool LookupUserName/{inside=1} inside{print} inside && /^}/{exit}' "$USER_DIR")"
GROUP_BODY="$(awk '/^bool LookupGroupName/{inside=1} inside{print} inside && /^}/{exit}' "$USER_DIR")"

STATIC_PASSED=0
STATIC_TOTAL=0

static_pass() {
  STATIC_TOTAL=$((STATIC_TOTAL + 1))
  STATIC_PASSED=$((STATIC_PASSED + 1))
  echo "[filter-semantics] static OK: $1"
}

static_fail() {
  STATIC_TOTAL=$((STATIC_TOTAL + 1))
  FAIL_CODE=1
  echo "[filter-semantics] static FAIL: $1"
}

expect_in() {
  if printf '%s\n' "$1" | grep -qF -- "$2"; then
    static_pass "$3"
  else
    static_fail "$3（函数体里找不到 $2）"
  fi
}

expect_not_in() {
  if printf '%s\n' "$1" | grep -qF -- "$2"; then
    static_fail "$3（函数体里出现了 $2）"
  else
    static_pass "$3"
  fi
}

if [ -z "$USER_BODY" ] || [ -z "$GROUP_BODY" ]; then
  static_fail "能截到 LookupUserName / LookupGroupName 的函数体"
else
  static_pass "能截到 LookupUserName / LookupGroupName 的函数体"
  expect_in "$GROUP_BODY" "_SC_GETGR_R_SIZE_MAX" "LookupGroupName 用 group 自己的 sysconf hint"
  expect_not_in "$GROUP_BODY" "_SC_GETPW_R_SIZE_MAX" "LookupGroupName 不用 passwd 的 sysconf hint"
  expect_in "$USER_BODY" "_SC_GETPW_R_SIZE_MAX" "LookupUserName 用 passwd 自己的 sysconf hint"
  expect_not_in "$USER_BODY" "_SC_GETGR_R_SIZE_MAX" "LookupUserName 不用 group 的 sysconf hint"
  expect_in "$GROUP_BODY" "ERANGE" "LookupGroupName 处理 ERANGE（缓冲不足要重试）"
  expect_in "$USER_BODY" "ERANGE" "LookupUserName 处理 ERANGE（缓冲不足要重试）"
  expect_in "$GROUP_BODY" "kMaxBufferSize" "LookupGroupName 的重试有上限"
  expect_in "$USER_BODY" "kMaxBufferSize" "LookupUserName 的重试有上限"
  if grep -qF -- "1024 * 1024" "$USER_DIR"; then
    static_pass "重试上限常量有具体取值（1024 * 1024）"
  else
    static_fail "重试上限常量有具体取值（1024 * 1024）"
  fi
fi

echo "filter-semantics: $((PASSED + STATIC_PASSED))/$((TOTAL + STATIC_TOTAL)) checks passed"
exit $FAIL_CODE
