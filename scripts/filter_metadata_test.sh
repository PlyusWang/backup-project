#!/usr/bin/env bash
#
# filter 元数据字段（uid / gid / user / group 与 type 的细分取值）单元测试入口。
#
# 编译 tests/unit/filter_metadata_test.cpp + src/filter/filter.cpp +
# src/filter/filter_rule_builder.cpp 后运行。测试自带 main()，不依赖任何测试框架。
#
# 输出目录走 /tmp，不写 tests/output/，可用环境变量覆盖：
#   FILTER_METADATA_OUT_DIR   构建与日志目录
#   FILTER_METADATA_SANITIZE  置 1 时用 -fsanitize=address,undefined 重新构建并运行
#
# 最后一行固定打印 "filter-metadata: N/M checks passed"；失败（编译失败、出现警告、
# 断言失败、缺汇总行）一律非零退出。

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

OUT_DIR="${FILTER_METADATA_OUT_DIR:-$(mktemp -d /tmp/filter-metadata.XXXXXX)}"
mkdir -p "$OUT_DIR"
BIN="$OUT_DIR/filter_metadata_test"
BUILD_LOG="$OUT_DIR/build.log"
RUN_LOG="$OUT_DIR/run.log"

EXTRA_FLAGS=""
if [ "${FILTER_METADATA_SANITIZE:-0}" = "1" ]; then
  EXTRA_FLAGS="-g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer"
  echo "[filter-metadata] ASan + UBSan 构建"
fi

echo "[filter-metadata] 编译：g++ -std=c++17 -Wall -Wextra -Wpedantic -Iinclude $EXTRA_FLAGS"
# shellcheck disable=SC2086
g++ -std=c++17 -Wall -Wextra -Wpedantic -Iinclude $EXTRA_FLAGS \
  src/filter/filter.cpp \
  src/filter/filter_rule_builder.cpp \
  tests/unit/filter_metadata_test.cpp \
  -o "$BIN" > "$BUILD_LOG" 2>&1
BUILD_CODE=$?
if [ $BUILD_CODE -ne 0 ]; then
  echo "[filter-metadata] 编译失败（exit=$BUILD_CODE），日志：$BUILD_LOG"
  tail -20 "$BUILD_LOG"
  echo "filter-metadata: 0/1 checks passed"
  exit 1
fi

WARNINGS=$(grep -c "warning:" "$BUILD_LOG")
echo "[filter-metadata] 编译警告数：$WARNINGS"
if [ "$WARNINGS" != "0" ]; then
  grep "warning:" "$BUILD_LOG" | head -10
  echo "filter-metadata: 0/1 checks passed"
  exit 1
fi

"$BIN" > "$RUN_LOG" 2>&1
RUN_CODE=$?
# 汇总行统一由脚本最后打印一次，日志里先过滤掉它，避免重复。
grep -v -E "^filter-metadata: [0-9]+/[0-9]+ checks passed$" "$RUN_LOG"

SUMMARY=$(grep -E "^filter-metadata: [0-9]+/[0-9]+ checks passed$" "$RUN_LOG" | tail -1)
if [ -z "$SUMMARY" ]; then
  echo "[filter-metadata] 测试没有打印汇总行（exit=$RUN_CODE），日志：$RUN_LOG"
  echo "filter-metadata: 0/1 checks passed"
  exit 1
fi

if [ $RUN_CODE -ne 0 ]; then
  echo "[filter-metadata] 存在失败断言（exit=$RUN_CODE），日志：$RUN_LOG" >&2
  echo "$SUMMARY"
  exit 1
fi

echo "$SUMMARY"
exit 0
