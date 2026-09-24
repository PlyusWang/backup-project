#!/usr/bin/env bash
#
# ustar 编解码 / 两个写入器 / 读取器的测试入口。
#
# 四件事：
#   1. 严格编译（-std=c++17 -Wall -Wextra -Wpedantic，零 warning）并跑单元测试；
#   2. ASan + UBSan 再跑一遍单元测试，确认 0 报告；
#   3. 与 GNU tar 的 interoperability（A / B / C / D 四组）；
#   4. 三个 corpus 的 baseline vs Fast 性能对比，打印真实墙钟时间。
#
# 系统 tar 只在这个脚本里当 oracle：产品代码里没有任何地方调用它。
# 临时数据全部落在 mktemp -d 出来的目录里（退出时删掉，包括 128 MiB 的
# corpus），不写 tests/output，也不碰仓库里的任何文件。
#
# 环境变量：
#   USTAR_TEST_WORK=<目录>   用指定目录当工作目录（默认 mktemp）
#   USTAR_KEEP_WORK=1        测试结束后保留工作目录（排查用）
#   USTAR_SKIP_PERF=1        跳过性能 corpus（只跑正确性）
#
# 退出码：0 = 全部通过。

set -uo pipefail

export LC_ALL=C

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

PASS_COUNT=0
FAIL_COUNT=0

pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo "PASS: $1"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo "FAIL: $1"; }

# check <名字> <命令...>：成功算 PASS，失败打印命令与被捕获输出的尾部。
check() {
  local name="$1"
  shift
  local output
  if output=$("$@" 2>&1); then
    pass "$name"
  else
    fail "$name"
    echo "      命令: $*"
    printf '%s\n' "$output" | tail -5 | sed 's/^/      /'
  fi
}

# check_true <名字> <命令...>：只关心退出码，不打印输出。
check_true() {
  local name="$1"
  shift
  if "$@" > /dev/null 2>&1; then
    pass "$name"
  else
    fail "$name"
    echo "      命令: $*"
  fi
}

check_eq() {
  local name="$1"
  local actual="$2"
  local expected="$3"
  if [ "$actual" = "$expected" ]; then
    pass "$name"
  else
    fail "$name（期望 $expected，实际 $actual）"
  fi
}

# 下面几个专用检查都先确认"文件真的存在、类型真的对"，再做比较：
# 否则两个都失败（比如两边都不存在）会得到空字符串等于空字符串的假 PASS。
check_fifo() {
  local name="$1"
  local path="$2"
  if [ -p "$path" ]; then
    pass "$name"
  else
    fail "$name（$path 不是 FIFO 或不存在）"
  fi
}

check_same_inode() {
  local name="$1"
  local left="$2"
  local right="$3"
  if [ -f "$left" ] && [ -f "$right" ] &&
    [ "$(stat -c %i "$left")" = "$(stat -c %i "$right")" ]; then
    pass "$name"
  else
    fail "$name（$left 与 $right 不是同一个 inode 或不存在）"
  fi
}

check_symlink_target() {
  local name="$1"
  local link="$2"
  local expected="$3"
  if [ -L "$link" ] && [ "$(readlink "$link")" = "$expected" ]; then
    pass "$name"
  else
    fail "$name（$link 不是指向 $expected 的软链接）"
  fi
}

check_mode() {
  local name="$1"
  local path="$2"
  local expected="$3"
  local actual=""
  if [ -f "$path" ]; then
    actual="$(stat -c %a "$path")"
  fi
  check_eq "$name" "$actual" "$expected"
}

WORK_DIR=""
OWN_WORK=0
if [ -n "${USTAR_TEST_WORK:-}" ]; then
  WORK_DIR="$USTAR_TEST_WORK"
  mkdir -p "$WORK_DIR"
else
  WORK_DIR="$(mktemp -d /tmp/ustar-test.XXXXXX)"
  OWN_WORK=1
fi

cleanup() {
  # corpus 里有 128 MiB 级别的文件，无论成功失败都要删掉。
  if [ "$OWN_WORK" = "1" ] && [ "${USTAR_KEEP_WORK:-0}" != "1" ]; then
    rm -rf "$WORK_DIR"
  fi
}
trap cleanup EXIT

echo "==================================================================="
echo "[ustar] 工作目录: $WORK_DIR"
echo "[ustar] 仓库:     $ROOT_DIR"
echo "==================================================================="

BIN="$WORK_DIR/ustar_test"
BUILD_LOG="$WORK_DIR/build.log"
UNIT_LOG="$WORK_DIR/unit.log"

# ---------------------------------------------------------------- 1. 编译
echo "[ustar] 1/4 严格编译"
g++ -std=c++17 -Wall -Wextra -Wpedantic -Iinclude \
  src/archive/ustar.cpp src/core/archive_entry.cpp tests/unit/ustar_test.cpp \
  -o "$BIN" > "$BUILD_LOG" 2>&1
BUILD_CODE=$?
if [ "$BUILD_CODE" -ne 0 ]; then
  echo "[ustar] 编译失败，日志尾部："
  tail -20 "$BUILD_LOG"
  echo "ustar: 0/1 checks passed"
  exit 1
fi
WARNING_COUNT="$(grep -c "warning:" "$BUILD_LOG" || true)"
check_eq "编译零 warning（-Wall -Wextra -Wpedantic）" "$WARNING_COUNT" "0"
if [ "$WARNING_COUNT" != "0" ]; then
  grep "warning:" "$BUILD_LOG" | head -10 | sed 's/^/      /'
fi

# ------------------------------------------------------------ 2. 单元测试
echo "[ustar] 2/4 单元测试（含 100 万条目上限测试）"
UNIT_DIR="$WORK_DIR/unit"
mkdir -p "$UNIT_DIR"
if "$BIN" --unit "$UNIT_DIR" > "$UNIT_LOG" 2>&1; then
  pass "单元测试退出码为 0（$(grep -c '^PASS' "$UNIT_LOG") 项检查）"
else
  fail "单元测试退出码非 0"
  grep '^FAIL' "$UNIT_LOG" | head -20 | sed 's/^/      /'
fi
echo "[ustar] 单元测试摘要: $(tail -1 "$UNIT_LOG")"

# ------------------------------------------------- 3. ASan + UBSan 再跑一遍
echo "[ustar] 3/4 ASan + UBSan"
ASAN_BIN="$WORK_DIR/ustar_test_asan"
ASAN_BUILD_LOG="$WORK_DIR/asan-build.log"
ASAN_RUN_LOG="$WORK_DIR/asan-run.log"
if g++ -std=c++17 -Wall -Wextra -Wpedantic -g -O1 \
  -fsanitize=address,undefined -fno-omit-frame-pointer -Iinclude \
  src/archive/ustar.cpp src/core/archive_entry.cpp tests/unit/ustar_test.cpp \
  -o "$ASAN_BIN" > "$ASAN_BUILD_LOG" 2>&1; then
  pass "ASan + UBSan 构建成功"
  mkdir -p "$WORK_DIR/unit-asan"
  # 100 万条目那条会占几百 MB 内存，sanitizer 下跳过（正常构建里已经跑过）。
  if ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 \
    "$ASAN_BIN" --unit "$WORK_DIR/unit-asan" --skip-big > "$ASAN_RUN_LOG" 2>&1; then
    pass "ASan + UBSan 运行退出码为 0（$(grep -c '^PASS' "$ASAN_RUN_LOG") 项检查）"
  else
    fail "ASan + UBSan 运行退出码非 0"
    tail -20 "$ASAN_RUN_LOG" | sed 's/^/      /'
  fi
  SANITIZER_REPORTS="$(grep -c -E "AddressSanitizer|runtime error:|LeakSanitizer|UndefinedBehaviorSanitizer" "$ASAN_RUN_LOG" || true)"
  check_eq "ASan + UBSan 0 报告" "$SANITIZER_REPORTS" "0"
  if [ "$SANITIZER_REPORTS" != "0" ]; then
    grep -E "AddressSanitizer|runtime error:|LeakSanitizer" "$ASAN_RUN_LOG" | head -10 | sed 's/^/      /'
  fi
else
  fail "ASan + UBSan 构建失败"
  tail -10 "$ASAN_BUILD_LOG" | sed 's/^/      /'
fi

# ------------------------------------------- 4. GNU tar interoperability
HAVE_TAR=0
if command -v tar > /dev/null 2>&1; then
  if tar --version 2> /dev/null | head -1 | grep -q "GNU tar"; then
    HAVE_TAR=1
  fi
fi

echo "[ustar] 4/4 interoperability 与性能"
SRC="$WORK_DIR/src"
mkdir -p "$SRC/sub/deep" "$SRC/emptydir"
printf 'hello world\n' > "$SRC/file.txt"
: > "$SRC/zero.bin"
head -c 5000 /dev/urandom > "$SRC/big.bin"
printf 'abc' > "$SRC/sub/inner.txt"
printf 'deep\n' > "$SRC/sub/deep/leaf.txt"
printf 'unicode\n' > "$SRC/空 格 ünïcode.txt"
ln -s file.txt "$SRC/link.txt"
ln "$SRC/file.txt" "$SRC/hard.txt"
mkfifo "$SRC/pipe.fifo"
chmod 0755 "$SRC/sub"
chmod 0600 "$SRC/zero.bin"
chmod 0640 "$SRC/file.txt"

BASE_TAR="$WORK_DIR/base.tar"
FAST_TAR="$WORK_DIR/fast.tar"
check "写 baseline 归档（真实目录树）" "$BIN" --pack "$SRC" "$BASE_TAR" baseline
check "写 Fast 归档（真实目录树）" "$BIN" --pack "$SRC" "$FAST_TAR" fast

OUR_COUNT="$("$BIN" --list "$BASE_TAR" | wc -l)"
# 独立计数：源树自身（对应归档里的 "."）+ 全部子项。两边都是 0 时下面的
# "条目数一致"会假通过，所以这一条必须先立住。
check_eq "写出的条目数等于源树节点数（find 计数）" \
  "$OUR_COUNT" "$(find "$SRC" | wc -l)"
echo "[ustar] 我们写出的归档条目数: $OUR_COUNT"
echo "[ustar] 我们的 Scan 看到的条目："
"$BIN" --list "$BASE_TAR" | sed 's/^/      /'

if [ "$HAVE_TAR" = "1" ]; then
  # A. 我们的 baseline 归档能被 GNU tar 列出，名字 / 大小 / 链接都对。
  check_eq "interop A: tar -tf 的条目数与我们一致" \
    "$(tar -tf "$BASE_TAR" | wc -l)" "$OUR_COUNT"
  check_eq "interop A: tar -tvf 里 big.bin 的大小是 5000" \
    "$(tar -tvf "$BASE_TAR" | awk '$6 == "big.bin" {print $3}')" "5000"
  check_eq "interop A: tar -tvf 里 file.txt 的大小是 12" \
    "$(tar -tvf "$BASE_TAR" | awk '$6 == "file.txt" {print $3}')" "12"
  check_true "interop A: tar 认出软链接 link.txt -> file.txt" \
    bash -c "tar -tvf '$BASE_TAR' | grep -q 'link.txt -> file.txt'"
  check_true "interop A: tar 认出硬链接 hard.txt link to file.txt" \
    bash -c "tar -tvf '$BASE_TAR' | grep -q 'hard.txt link to file.txt'"
  check_true "interop A: tar 认出目录条目（drwx 开头）" \
    bash -c "tar -tvf '$BASE_TAR' | grep -q '^d.* sub$'"
  check_true "interop A: tar 认出 FIFO 条目（prw 开头）" \
    bash -c "tar -tvf '$BASE_TAR' | grep -q '^p.* pipe.fifo$'"

  # B. Fast 归档同样能被列出，而且条目数与 baseline 一致。
  check_eq "interop B: Fast 归档 tar -tf 条目数与 baseline 一致" \
    "$(tar -tf "$FAST_TAR" | wc -l)" "$(tar -tf "$BASE_TAR" | wc -l)"

  # C. tar -xf 解我们的两个归档，与源目录 diff -r 一致。
  DEST_BASE="$WORK_DIR/dest-base"
  DEST_FAST="$WORK_DIR/dest-fast"
  mkdir -p "$DEST_BASE" "$DEST_FAST"
  check "interop C: tar -xf 解开 baseline 归档" tar -xf "$BASE_TAR" -C "$DEST_BASE"
  check "interop C: tar -xf 解开 Fast 归档" tar -xf "$FAST_TAR" -C "$DEST_FAST"
  # diff -r 对 FIFO 会直接报"是先进先出文件"（即使两边都是 FIFO），所以
  # FIFO 用 -x 排除掉，再单独用 test -p 检查类型。
  check_true "interop C: baseline 解出的树与源目录 diff -r 一致" \
    diff -r -x '*.fifo' "$SRC" "$DEST_BASE"
  check_true "interop C: Fast 解出的树与源目录 diff -r 一致" \
    diff -r -x '*.fifo' "$SRC" "$DEST_FAST"
  check_fifo "interop C: baseline 解出的 FIFO 仍是 FIFO" "$DEST_BASE/pipe.fifo"
  check_fifo "interop C: Fast 解出的 FIFO 仍是 FIFO" "$DEST_FAST/pipe.fifo"
  check_same_inode "interop C: baseline 解出的硬链接共享 inode" \
    "$DEST_BASE/file.txt" "$DEST_BASE/hard.txt"
  check_same_inode "interop C: Fast 解出的硬链接共享 inode" \
    "$DEST_FAST/file.txt" "$DEST_FAST/hard.txt"
  check_symlink_target "interop C: baseline 解出的软链接目标正确" \
    "$DEST_BASE/link.txt" "file.txt"
  check_symlink_target "interop C: Fast 解出的软链接目标正确" \
    "$DEST_FAST/link.txt" "file.txt"
  check_mode "interop C: baseline 解出的文件权限是 640" "$DEST_BASE/file.txt" "640"
  check_mode "interop C: Fast 解出的文件权限是 640" "$DEST_FAST/file.txt" "640"

  # D. GNU tar 写的标准 ustar，我们的 Scan / ExtractData 要能读回来。
  GNU_TAR="$WORK_DIR/gnu.tar"
  check "interop D: GNU tar --format=ustar 造标准归档" \
    tar -C "$SRC" --format=ustar -cf "$GNU_TAR" .
  GNU_LISTED="$(tar -tf "$GNU_TAR" | wc -l)"
  check_eq "interop D: 我们的 Scan 读出的条目数与 tar -tf 一致" \
    "$("$BIN" --list "$GNU_TAR" | wc -l)" "$GNU_LISTED"
  DEST_GNU="$WORK_DIR/dest-gnu"
  mkdir -p "$DEST_GNU"
  check "interop D: 用 Scan + ExtractData 还原 GNU 归档" \
    "$BIN" --restore "$GNU_TAR" "$DEST_GNU"
  check_true "interop D: 还原结果与源目录 diff -r 一致" \
    diff -r -x '*.fifo' "$SRC" "$DEST_GNU"
  check_fifo "interop D: 还原出的 FIFO 仍是 FIFO" "$DEST_GNU/pipe.fifo"
  check_same_inode "interop D: 还原出的硬链接共享 inode" \
    "$DEST_GNU/file.txt" "$DEST_GNU/hard.txt"
  check_symlink_target "interop D: 还原出的软链接目标正确" "$DEST_GNU/link.txt" "file.txt"
  check_mode "interop D: 还原出的文件权限是 640" "$DEST_GNU/file.txt" "640"
else
  echo "SKIP: 系统里没有 GNU tar，A / B / C / D 四组 interoperability 测试跳过"
fi

# ------------------------------------------------------------ 性能对比
# 单次测量：写一个归档，返回毫秒（失败返回 -1）。
time_one() {
  local writer="$1"
  local srcdir="$2"
  local archive="$3"
  local t0=0
  local t1=0
  local rc=0
  rm -f "$archive"
  t0=$(date +%s%N)
  "$BIN" --pack "$srcdir" "$archive" "$writer" > /dev/null 2>&1
  rc=$?
  t1=$(date +%s%N)
  if [ "$rc" -ne 0 ]; then
    echo "-1"
    return
  fi
  echo $(( (t1 - t0) / 1000000 ))
}

# 交替测量三轮，各自取最小值。交替而不是"先跑完 baseline 再跑 Fast"，是为了
# 抵消机器负载随时间的漂移——这台 VM 上还有别的任务在跑，分组测量会把负载
# 变化整个算到其中一个写入器头上。取最小值而不是平均值：我们要的是"这套
# I/O 策略本身要多久"，平均值会被页缓存与调度抖动带偏。
measure_corpus() {
  local srcdir="$1"
  local base_archive="$2"
  local fast_archive="$3"
  local round=0
  local base_best=""
  local fast_best=""
  local ms=""
  for round in 1 2 3; do
    ms=$(time_one baseline "$srcdir" "$base_archive")
    if [ "$ms" = "-1" ]; then
      echo "-1 -1"
      return
    fi
    if [ -z "$base_best" ] || [ "$ms" -lt "$base_best" ]; then
      base_best="$ms"
    fi
    ms=$(time_one fast "$srcdir" "$fast_archive")
    if [ "$ms" = "-1" ]; then
      echo "-1 -1"
      return
    fi
    if [ -z "$fast_best" ] || [ "$ms" -lt "$fast_best" ]; then
      fast_best="$ms"
    fi
  done
  echo "$base_best $fast_best"
}

run_corpus() {
  local name="$1"
  local srcdir="$2"
  local base_archive="$WORK_DIR/perf-$name-baseline.tar"
  local fast_archive="$WORK_DIR/perf-$name-fast.tar"
  local bytes=0
  local files=0
  local base_ms=0
  local fast_ms=0
  local base_size=0
  local fast_size=0
  local speedup=0
  local base_rate=0
  local fast_rate=0

  bytes=$(du -sb "$srcdir" | awk '{print $1}')
  files=$(find "$srcdir" -type f | wc -l)
  local measured=""
  measured=$(measure_corpus "$srcdir" "$base_archive" "$fast_archive")
  base_ms=$(printf '%s' "$measured" | awk '{print $1}')
  fast_ms=$(printf '%s' "$measured" | awk '{print $2}')
  if [ "$base_ms" = "-1" ] || [ "$fast_ms" = "-1" ]; then
    fail "性能 corpus $name：写入失败"
    return
  fi
  base_size=$(stat -c %s "$base_archive")
  fast_size=$(stat -c %s "$fast_archive")
  speedup=$(awk -v b="$base_ms" -v f="$fast_ms" 'BEGIN { if (f <= 0) f = 1; printf "%.2f", b / f }')
  base_rate=$(awk -v n="$bytes" -v ms="$base_ms" 'BEGIN { if (ms <= 0) ms = 1; printf "%.1f", n / (ms / 1000.0) / 1048576.0 }')
  fast_rate=$(awk -v n="$bytes" -v ms="$fast_ms" 'BEGIN { if (ms <= 0) ms = 1; printf "%.1f", n / (ms / 1000.0) / 1048576.0 }')

  printf '%-9s %12s %6s %13s %10s %8s %11s %11s %13s %13s\n' \
    "$name" "$bytes" "$files" "$base_ms" "$fast_ms" "$speedup" \
    "$base_rate" "$fast_rate" "$base_size" "$fast_size"
  if [ "$fast_ms" -lt "$base_ms" ]; then
    echo "          Fast 更快: $speedup x（baseline $base_ms ms，Fast $fast_ms ms）"
  else
    echo "          INFO: 这个 corpus 上 Fast 没有更快（baseline $base_ms ms，Fast $fast_ms ms），如实记录"
  fi
  if [ "$base_size" = "$fast_size" ]; then
    echo "          两个归档字节数相同（$base_size）：Fast 只改了 I/O 策略，没有改格式"
  else
    echo "          归档字节数：baseline $base_size，Fast $fast_size"
  fi

  if [ "$HAVE_TAR" = "1" ]; then
    local expected=""
    expected=$("$BIN" --list "$base_archive" | wc -l)
    check_eq "性能 $name: baseline 归档能被 tar -tf 列出（$expected 条）" \
      "$(tar -tf "$base_archive" | wc -l)" "$expected"
    check_eq "性能 $name: Fast 归档能被 tar -tf 列出（$expected 条）" \
      "$(tar -tf "$fast_archive" | wc -l)" "$expected"
  fi
}

if [ "${USTAR_SKIP_PERF:-0}" = "1" ]; then
  echo "SKIP: 性能对比（USTAR_SKIP_PERF=1）"
else
  echo "[ustar] 生成性能 corpus（A: 1 x 128 MiB；B: 10000 x 1-8 KiB；C: 约 100 MiB 混合）"
  head -c 8192 /dev/urandom > "$WORK_DIR/seed.bin"
  for k in 1 2 3 4 5 6 7 8; do
    head -c $((k * 1024)) "$WORK_DIR/seed.bin" > "$WORK_DIR/master-$k.bin"
  done

  CORPUS_A="$WORK_DIR/corpus-a"
  mkdir -p "$CORPUS_A"
  head -c 134217728 /dev/urandom > "$CORPUS_A/big.bin"

  CORPUS_B="$WORK_DIR/corpus-b"
  mkdir -p "$CORPUS_B"
  i=0
  while [ "$i" -lt 10000 ]; do
    i=$((i + 1))
    k=$(( (RANDOM % 8) + 1 ))
    cp "$WORK_DIR/master-$k.bin" "$CORPUS_B/f$i.bin"
  done

  CORPUS_C="$WORK_DIR/corpus-c"
  mkdir -p "$CORPUS_C/large"
  for i in 1 2 3 4 5; do
    head -c $((16 * 1024 * 1024)) /dev/urandom > "$CORPUS_C/large/big$i.bin"
  done
  for d in $(seq 1 20); do
    mkdir -p "$CORPUS_C/dirs/d$d"
    for f in $(seq 1 250); do
      cp "$WORK_DIR/master-4.bin" "$CORPUS_C/dirs/d$d/f$f.bin"
    done
  done

  echo
  echo "corpus        bytes   files   baseline_ms   fast_ms  speedup  base_MiB/s  fast_MiB/s  base_archive  fast_archive"
  run_corpus "A-128MiB" "$CORPUS_A"
  run_corpus "B-10000" "$CORPUS_B"
  run_corpus "C-mixed" "$CORPUS_C"

  # 大文件用完就删，别占着磁盘。
  rm -rf "$CORPUS_A" "$CORPUS_B" "$CORPUS_C"
fi

# ------------------------------------------------------------------ 汇总
TOTAL_COUNT=$((PASS_COUNT + FAIL_COUNT))
echo "==================================================================="
echo "ustar: $PASS_COUNT/$TOTAL_COUNT checks passed"
if [ "$FAIL_COUNT" -ne 0 ]; then
  echo "[ustar] 失败 $FAIL_COUNT 项" >&2
  exit 1
fi
exit 0
