#!/usr/bin/env bash
#
# 可视化规则编辑器中间层的单元测试入口。
#
# 编译 tests/unit/filter_rule_builder_test.cpp + src/filter/filter_rule_builder.cpp，
# 链接真实 Filter 核心（src/filter/filter.cpp），跑断言。
# 装了 GoogleTest 就用它（-lgtest -pthread，main 由测试自己提供），否则用内置 harness。
#
# 退出码：0 = 全部通过。

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

OUT_DIR="$ROOT_DIR/tests/output"
BIN="$OUT_DIR/filter_rule_builder_test"
LOG="$OUT_DIR/filter-rule-builder-test.log"
mkdir -p "$OUT_DIR"

echo "[rule-builder] 编译中间层与单元测试"

GTEST_LIBS=""
GTEST_CFLAGS=""
GTEST_FORCE=""
printf '#include <gtest/gtest.h>\nTEST(Probe, Ok) { EXPECT_EQ(1, 1); }\nint main(int argc, char** argv) { testing::InitGoogleTest(&argc, argv); return RUN_ALL_TESTS(); }\n' > "$OUT_DIR/gtest_probe.cpp"
if pkg-config --exists gtest 2> /dev/null; then
  GTEST_CFLAGS="$(pkg-config --cflags gtest 2> /dev/null)"
  GTEST_LIBS="$(pkg-config --libs gtest 2> /dev/null)"
fi
if [ -z "$GTEST_LIBS" ]; then GTEST_LIBS="-lgtest"; fi
case "$GTEST_LIBS" in
  *-pthread*) : ;;
  *) GTEST_LIBS="$GTEST_LIBS -pthread" ;;
esac
if g++ -std=c++17 $GTEST_CFLAGS "$OUT_DIR/gtest_probe.cpp" -o "$OUT_DIR/gtest_probe" $GTEST_LIBS > "$LOG" 2>&1 \
   && "$OUT_DIR/gtest_probe" >> "$LOG" 2>&1; then
  echo "[rule-builder] GTEST_AVAILABLE=YES（libs: $GTEST_LIBS）"
else
  echo "[rule-builder] GTEST_AVAILABLE=NO，使用内置 harness"
  GTEST_LIBS=""
  GTEST_CFLAGS=""
  GTEST_FORCE="-DL3_FORCE_NO_GTEST"
fi

# 顺序：先编译各自的 .o（产品核心在前、测试在后），最后链接，库放最后。
g++ -std=c++17 -Wall -Wextra -Wpedantic -g -Iinclude -c src/filter/filter.cpp -o "$OUT_DIR/frb_filter.o" >> "$LOG" 2>&1
g++ -std=c++17 -Wall -Wextra -Wpedantic -g -Iinclude -c src/filter/filter_rule_builder.cpp -o "$OUT_DIR/frb_builder.o" >> "$LOG" 2>&1
g++ -std=c++17 -Wall -Wextra -Wpedantic -g $GTEST_FORCE $GTEST_CFLAGS -Iinclude -c tests/unit/filter_rule_builder_test.cpp -o "$OUT_DIR/frb_test.o" >> "$LOG" 2>&1
echo "[rule-builder] 链接: g++ frb_test.o frb_builder.o frb_filter.o -o filter_rule_builder_test $GTEST_LIBS"
g++ "$OUT_DIR/frb_test.o" "$OUT_DIR/frb_builder.o" "$OUT_DIR/frb_filter.o" -o "$BIN" $GTEST_LIBS >> "$LOG" 2>&1
BUILD_CODE=$?
if [ $BUILD_CODE -ne 0 ]; then
  echo "[rule-builder] 编译失败（exit=$BUILD_CODE），日志：$LOG"
  tail -5 "$LOG"
  exit 1
fi
grep -c "warning:" "$LOG" | sed 's/^/[rule-builder] 编译警告: /'

"$BIN" > "$OUT_DIR/filter-rule-builder-run.log" 2>&1
RUN_CODE=$?
tail -6 "$OUT_DIR/filter-rule-builder-run.log" | sed 's/^/[rule-builder] /'

echo "[rule-builder] 场景 -> DSL（集成测试用的同一批规则）"
for scenario in ext_txt_md exclude_build include_txt_exclude_secret multi_clause_folder_cache; do
  "$BIN" --emit "$scenario" | sed "s/^/[rule-builder]   $scenario: /"
done
echo "[rule-builder] 非法场景必须被前端拦下："
if "$BIN" --emit invalid_empty_ext > /dev/null 2>&1; then
  echo "[rule-builder]   FAIL: invalid_empty_ext 竟然通过了校验"
  RUN_CODE=1
else
  echo "[rule-builder]   OK: invalid_empty_ext 被拒绝（exit=$?）"
fi

echo "[rule-builder] 运行日志：$OUT_DIR/filter-rule-builder-run.log"
if [ $RUN_CODE -eq 0 ]; then
  echo "[rule-builder] 结论：单元测试全部通过"
  exit 0
fi
echo "[rule-builder] 结论：存在失败" >&2
exit 1
