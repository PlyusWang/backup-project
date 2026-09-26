#!/usr/bin/env bash
#
# legacy v0.1 路径的 Filter 元数据投影测试入口。
#
# 编译整份 src/**/*.cpp（被测的就是产品代码本身）以及
# tests/unit/legacy_filter_test.cpp，链接成一个二进制后运行；测试自带 main()，
# 不依赖任何测试框架。
#
# 为什么整份编译：这条路径的行为分布在 archive.cpp（元数据投影）、filter.cpp
# （规则匹配）、user_directory.cpp（uid/gid -> 名字）和 backup_engine.cpp
# （旧签名入口）里，只挑几个 .cpp 编译会漏掉"改一个头文件把别的模块带崩"。
# 所有源文件都用 -Wall -Wextra -Wpedantic 编译，出现任何 warning 直接判失败。
#
# 输出目录走 /tmp，不写 tests/output/，可用环境变量覆盖：
#   LEGACY_FILTER_OUT_DIR   构建与日志目录
#   LEGACY_FILTER_SANITIZE  置 1 时用 -fsanitize=address,undefined 构建并运行
#
# 最后一行固定打印 "legacy-filter: N/M checks passed"；失败（编译失败、出现警告、
# 断言失败、sanitizer 报告、缺汇总行）一律非零退出。

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

CXX="${CXX:-g++}"
OUT_DIR="${LEGACY_FILTER_OUT_DIR:-$(mktemp -d /tmp/legacy-filter.XXXXXX)}"
mkdir -p "$OUT_DIR"
OBJ_DIR="$OUT_DIR/obj"
mkdir -p "$OBJ_DIR"
BIN="$OUT_DIR/legacy_filter_test"
BUILD_LOG="$OUT_DIR/build.log"
RUN_LOG="$OUT_DIR/run.log"

EXTRA_FLAGS=""
if [ "${LEGACY_FILTER_SANITIZE:-0}" = "1" ]; then
  EXTRA_FLAGS="-g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer"
  echo "[legacy-filter] ASan + UBSan 构建"
fi

: > "$BUILD_LOG"

fail_build() {
  echo "[legacy-filter] $1（日志：$BUILD_LOG）"
  tail -30 "$BUILD_LOG"
  echo "legacy-filter: 0/1 checks passed"
  exit 1
}

mapfile -t SOURCES < <(find src -name '*.cpp' | LC_ALL=C sort)
if [ "${#SOURCES[@]}" -eq 0 ]; then
  fail_build "src 下找不到任何产品源文件"
fi

echo "[legacy-filter] 编译产品代码 ${#SOURCES[@]} 个 .cpp（-Wall -Wextra -Wpedantic）"
OBJECTS=()
for source in "${SOURCES[@]}"; do
  object="$OBJ_DIR/$(echo "$source" | tr '/' '_').o"
  # shellcheck disable=SC2086
  if ! "$CXX" -std=c++17 -Wall -Wextra -Wpedantic -Iinclude $EXTRA_FLAGS -O1 -c "$source" -o "$object" >> "$BUILD_LOG" 2>&1; then
    fail_build "编译失败：$source"
  fi
  OBJECTS+=("$object")
done

echo "[legacy-filter] 编译并链接测试 tests/unit/legacy_filter_test.cpp"
# shellcheck disable=SC2086
if ! "$CXX" -std=c++17 -Wall -Wextra -Wpedantic -Iinclude -Itests/unit $EXTRA_FLAGS -O1 tests/unit/legacy_filter_test.cpp "${OBJECTS[@]}" -o "$BIN" >> "$BUILD_LOG" 2>&1; then
  fail_build "测试编译 / 链接失败"
fi

WARNINGS=$(grep -c "warning:" "$BUILD_LOG")
echo "[legacy-filter] 编译警告数：$WARNINGS"
if [ "$WARNINGS" != "0" ]; then
  grep "warning:" "$BUILD_LOG" | head -10
  echo "legacy-filter: 0/1 checks passed"
  exit 1
fi

"$BIN" > "$RUN_LOG" 2>&1
RUN_CODE=$?

# sanitizer 构建下任何一条报告都算失败：ASan / UBSan 默认只打印、不改退出码，
# 只看 $? 会把"带报告通过"当成绿灯。
if [ -n "$EXTRA_FLAGS" ]; then
  REPORTS=$(grep -c -E "ERROR: AddressSanitizer|runtime error:|SUMMARY: AddressSanitizer|LeakSanitizer" "$RUN_LOG")
  echo "[legacy-filter] sanitizer 报告数：$REPORTS"
  if [ "$REPORTS" != "0" ]; then
    grep -E "ERROR: AddressSanitizer|runtime error:|SUMMARY: AddressSanitizer|LeakSanitizer" "$RUN_LOG" | head -10
    echo "legacy-filter: 0/1 checks passed"
    exit 1
  fi
fi

# 汇总行统一由脚本最后打印一次，日志里先过滤掉它，避免重复。
grep -v -E "^legacy-filter: [0-9]+/[0-9]+ checks passed$" "$RUN_LOG"
SUMMARY=$(grep -E "^legacy-filter: [0-9]+/[0-9]+ checks passed$" "$RUN_LOG" | tail -1)
if [ -z "$SUMMARY" ]; then
  echo "[legacy-filter] 测试没有打印汇总行（exit=$RUN_CODE），日志：$RUN_LOG"
  echo "legacy-filter: 0/1 checks passed"
  exit 1
fi
if [ "$RUN_CODE" -ne 0 ]; then
  echo "[legacy-filter] 存在失败断言（exit=$RUN_CODE），日志：$RUN_LOG" >&2
  echo "$SUMMARY"
  exit 1
fi

echo "$SUMMARY"
exit 0
