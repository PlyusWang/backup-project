#!/usr/bin/env bash
#
# ArchiveReader::InspectHeader 与 BackupCatalog 的单元测试入口。
#
# 编译 tests/unit/backup_catalog_test.cpp + 真实核心（src/archive、src/catalog、
# src/core、src/filesystem、src/filter）。测试里每一份"合法归档"都是用真实
# BackupEngine 写出来的，不是测试自己拼的字节。
#
# 装了 GoogleTest 就用它（-lgtest -pthread，main 由测试自己提供），否则用内置
# harness——没有 GoogleTest 的环境照样必须能跑。
#
# TZ 固定成 UTC：BuildArchivePath 用 localtime_r，只有时区固定了，文件名里的
# 时间戳才是确定的。
#
# 临时数据全部落在 mkdtemp 出来的目录里（默认 /tmp），不碰真实 HOME。
#
# 退出码：0 = 全部通过。

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

export TZ=UTC

OUT_DIR="$ROOT_DIR/tests/output"
BIN="$OUT_DIR/backup_catalog_test"
LOG="$OUT_DIR/backup-catalog-test-build.log"
RUN_LOG="$OUT_DIR/backup-catalog-test-run.log"
mkdir -p "$OUT_DIR"

echo "[catalog] 编译核心与单元测试"

# 探测 GoogleTest 是否真的可用：只信"能编过并且能跑起来"的探测结果。
# 探测文件用独立名字，避免和 filter_rule_builder 那个脚本互相覆盖。
GTEST_LIBS=""
GTEST_CFLAGS=""
GTEST_FORCE=""
if [ "${BP_FORCE_NO_GTEST:-0}" = "1" ]; then
  # 用来验证"没有 GoogleTest 也能跑"这条承诺：强制走内置 harness。
  GTEST_FORCE="-DL3_FORCE_NO_GTEST"
  echo "[catalog] GTEST_AVAILABLE=NO（BP_FORCE_NO_GTEST=1 强制），使用内置 harness"
else
  printf '#include <gtest/gtest.h>\nTEST(Probe, Ok) { EXPECT_EQ(1, 1); }\nint main(int argc, char** argv) { testing::InitGoogleTest(&argc, argv); return RUN_ALL_TESTS(); }\n' > "$OUT_DIR/gtest_probe_catalog.cpp"
  if pkg-config --exists gtest 2> /dev/null; then
    GTEST_CFLAGS="$(pkg-config --cflags gtest 2> /dev/null)"
    GTEST_LIBS="$(pkg-config --libs gtest 2> /dev/null)"
  fi
  if [ -z "$GTEST_LIBS" ]; then GTEST_LIBS="-lgtest"; fi
  case "$GTEST_LIBS" in
    *-pthread*) : ;;
    *) GTEST_LIBS="$GTEST_LIBS -pthread" ;;
  esac
  if g++ -std=c++17 $GTEST_CFLAGS "$OUT_DIR/gtest_probe_catalog.cpp" -o "$OUT_DIR/gtest_probe_catalog" $GTEST_LIBS > "$LOG" 2>&1 \
     && "$OUT_DIR/gtest_probe_catalog" >> "$LOG" 2>&1; then
    echo "[catalog] GTEST_AVAILABLE=YES（libs: $GTEST_LIBS）"
  else
    echo "[catalog] GTEST_AVAILABLE=NO，使用内置 harness"
    GTEST_LIBS=""
    GTEST_CFLAGS=""
    GTEST_FORCE="-DL3_FORCE_NO_GTEST"
  fi
fi

# 顺序：先编译各自的 .o（产品核心在前、测试在后），最后链接，库放最后。
g++ -std=c++17 -Wall -Wextra -Wpedantic -g -Iinclude -c src/archive/archive.cpp -o "$OUT_DIR/bpcat_archive.o" >> "$LOG" 2>&1
g++ -std=c++17 -Wall -Wextra -Wpedantic -g -Iinclude -c src/catalog/backup_catalog.cpp -o "$OUT_DIR/bpcat_catalog.o" >> "$LOG" 2>&1
g++ -std=c++17 -Wall -Wextra -Wpedantic -g -Iinclude -c src/core/backup_engine.cpp -o "$OUT_DIR/bpcat_engine.o" >> "$LOG" 2>&1
g++ -std=c++17 -Wall -Wextra -Wpedantic -g -Iinclude -c src/filesystem/file_system.cpp -o "$OUT_DIR/bpcat_filesystem.o" >> "$LOG" 2>&1
g++ -std=c++17 -Wall -Wextra -Wpedantic -g -Iinclude -c src/filter/filter.cpp -o "$OUT_DIR/bpcat_filter.o" >> "$LOG" 2>&1
g++ -std=c++17 -Wall -Wextra -Wpedantic -g $GTEST_FORCE $GTEST_CFLAGS -Iinclude -c tests/unit/backup_catalog_test.cpp -o "$OUT_DIR/bpcat_test.o" >> "$LOG" 2>&1
echo "[catalog] 链接: bpcat_test.o bpcat_catalog.o bpcat_archive.o bpcat_engine.o bpcat_filesystem.o bpcat_filter.o"
g++ "$OUT_DIR/bpcat_test.o" "$OUT_DIR/bpcat_catalog.o" "$OUT_DIR/bpcat_archive.o" "$OUT_DIR/bpcat_engine.o" "$OUT_DIR/bpcat_filesystem.o" "$OUT_DIR/bpcat_filter.o" -o "$BIN" $GTEST_LIBS >> "$LOG" 2>&1
BUILD_CODE=$?
if [ $BUILD_CODE -ne 0 ]; then
  echo "[catalog] 编译失败（exit=$BUILD_CODE），日志：$LOG"
  tail -25 "$LOG"
  exit 1
fi
grep -c "warning:" "$LOG" | sed 's/^/[catalog] 编译警告: /'

"$BIN" > "$RUN_LOG" 2>&1
RUN_CODE=$?
echo "[catalog] 测试二进制: $BIN"
tail -6 "$RUN_LOG" | sed 's/^/[catalog] /'
echo "[catalog] 运行日志：$RUN_LOG"
if [ $RUN_CODE -eq 0 ]; then
  echo "[catalog] 结论：单元测试全部通过"
  exit 0
fi
echo "[catalog] 结论：存在失败" >&2
exit 1
