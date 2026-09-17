#!/usr/bin/env bash
#
# Sprint 1 + Sprint 2 阶段性质量回归入口。
#
# 与 scripts/test.sh 的分工：
#   * scripts/test.sh  -> 功能正确性（round-trip、错误路径、拓扑、安全、元数据）。
#   * 本脚本           -> 阶段收尾的四个质量维度，回答"这套东西能不能交付"。
#
# 四个维度：
#   维度 1 可用性 (US) CLI 契约、帮助文本、退出码约定、错误可诊断性、成功反馈。
#   维度 2 鲁棒性 (RB) 边界与损坏输入：空/深/大/中文路径、截断、位翻转、
#                      路径穿越、非空目标、拒绝覆盖、特殊文件。
#   维度 3 稳定性 (ST) 连续 10 轮 backup -> restore 往返，每轮都做
#                      diff -r + sha256sum 全树比对，记录 X/10。
#   维度 4 健壮性 (SN) ASan + UBSan 构建下重跑代表性操作与模糊输入，
#                      要求 sanitizer 零报告。
#
# 判定约定：
#   * 成功用例必须真的核对磁盘结果（diff -r 与 sha256sum 双证）；
#   * 失败用例要求退出码 1（用法错误 2），关键原因出现在输出里；
#   * 被信号打死（>=128）或超时（124）一律算失败，绝不放过。
#
# 测试数据放在 testdata/quality（testdata/ 已 gitignore），
# 全套通过后自动删除；想保留现场就设置 KEEP_TESTDATA=1。
#
# 退出码：四个维度全部通过才是 0。

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BACKUPCTL="$ROOT_DIR/build/backupctl"
SAN_BACKUPCTL="$ROOT_DIR/build-sanitize/backupctl"
WORK_DIR="$ROOT_DIR/testdata/quality"
LOGDIR="/tmp/backup-project-quality"
LOG="$LOGDIR/last-output.txt"
SAN_LOG="$LOGDIR/sanitizer-output.txt"
FUNC_LOG="$LOGDIR/functional-suite.txt"
BUILD_LOG="$LOGDIR/build.txt"
TIMEOUT_SECONDS=120
ROUNDS=10
FUZZ_ROUNDS=64
SAN_FUZZ_ROUNDS=24

PASS_COUNT=0
FAIL_COUNT=0
STABILITY_PASS=0
SAN_RUNS=0
SAN_REPORTS=0
FUNC_PASS=0
FUNC_FAIL=0
BUILD_WARNINGS=0
START_TS=$(date +%s)

echo "[quality] Sprint 1+2 阶段性质量回归（可用性 / 鲁棒性 / 稳定性 / 健壮性）"
echo "[quality] 仓库: $ROOT_DIR"
mkdir -p "$LOGDIR"

pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo "  PASS: $1"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo "  FAIL: $1 -- $2" >&2; }

# ---- 通用执行器 ------------------------------------------------------
invoke() {
  timeout --signal=KILL "$TIMEOUT_SECONDS" "$@" > "$LOG" 2>&1
}

expect_ok() {
  local desc="$1"; shift
  invoke "$@"
  local code=$?
  if [[ $code -eq 0 ]]; then
    pass "$desc"
  else
    fail "$desc" "期望 exit=0 实际 $code: $(head -2 "$LOG" | tr '\n' ' ')"
  fi
}

expect_ok_msg() {
  local desc="$1" needle="$2"; shift 2
  invoke "$@"
  local code=$?
  if [[ $code -ne 0 ]]; then fail "$desc" "期望 exit=0 实际 $code"; return; fi
  if ! grep -qF -- "$needle" "$LOG"; then fail "$desc" "输出里缺少关键字: $needle"; return; fi
  pass "$desc"
}

expect_fail() {
  local desc="$1" want="$2" needle="$3"; shift 3
  invoke "$@"
  local code=$?
  if [[ $code -eq 124 ]]; then fail "$desc" "超时被杀（疑似挂死）"; return; fi
  if [[ $code -ge 128 ]]; then fail "$desc" "被信号打死 exit=$code"; return; fi
  if [[ $code -ne $want ]]; then fail "$desc" "期望 exit=$want 实际 $code: $(head -2 "$LOG" | tr '\n' ' ')"; return; fi
  if [[ -n "$needle" ]] && ! grep -qF -- "$needle" "$LOG"; then
    fail "$desc" "错误信息缺少关键原因: $needle"; return
  fi
  pass "$desc"
}

# ---- 数据一致性：diff -r + sha256sum 双证 ----------------------------
tree_hash() {
  ( cd "$1" && find . -type f -print0 | sort -z | xargs -0 sha256sum 2> /dev/null )
}

consistent() {
  diff -r "$1" "$2" > "$LOG" 2>&1 || return 1
  local a b
  a="$(tree_hash "$1")"
  b="$(tree_hash "$2")"
  [[ "$a" == "$b" ]] || return 1
  return 0
}

check_consistency() {
  local desc="$1" src="$2" dst="$3"
  if consistent "$src" "$dst"; then
    pass "$desc（$(find "$src" -type f | wc -l) 个文件 diff -r 与 sha256sum 一致）"
  else
    fail "$desc" "diff -r 或 sha256sum 不一致: $(head -3 "$LOG" | tr '\n' ' ')"
  fi
}

# ======================================================================
# 步骤 0：从零构建（同时统计编译警告）
# ======================================================================
echo
echo "[quality] 步骤 0：从零构建 CLI 与 sanitizer 版本"
make -C "$ROOT_DIR" clean > "$BUILD_LOG" 2>&1
rm -rf "$ROOT_DIR/build-sanitize"
make -C "$ROOT_DIR" >> "$BUILD_LOG" 2>&1
BUILD_CODE=$?
make -C "$ROOT_DIR" sanitize >> "$BUILD_LOG" 2>&1
SAN_BUILD_CODE=$?
BUILD_WARNINGS=$(grep -c "warning:" "$BUILD_LOG")
echo "  make 退出码 = $BUILD_CODE, make sanitize 退出码 = $SAN_BUILD_CODE, 编译警告 = $BUILD_WARNINGS"
if [[ $BUILD_CODE -eq 0 && $SAN_BUILD_CODE -eq 0 ]]; then
  pass "BLD-01 从零构建成功（make 与 make sanitize 均为 0）"
else
  fail "BLD-01 从零构建成功" "make=$BUILD_CODE sanitize=$SAN_BUILD_CODE: $(tail -3 "$BUILD_LOG" | tr '\n' ' ')"
fi
if [[ $BUILD_WARNINGS -eq 0 ]]; then
  pass "BLD-02 -Wall -Wextra -Wpedantic 下零编译警告"
else
  fail "BLD-02 -Wall -Wextra -Wpedantic 下零编译警告" "$BUILD_WARNINGS 条警告"
fi
if [[ -x "$BACKUPCTL" ]]; then pass "BLD-03 产物 build/backupctl 可执行"; else fail "BLD-03 产物 build/backupctl 可执行" "缺失"; fi
if [[ -x "$SAN_BACKUPCTL" ]]; then pass "BLD-04 产物 build-sanitize/backupctl 可执行"; else fail "BLD-04 产物 build-sanitize/backupctl 可执行" "缺失"; fi

# ======================================================================
# 步骤 1：功能基线（scripts/test.sh）
# ======================================================================
echo
echo "[quality] 步骤 1：功能基线 scripts/test.sh"
FUNC_START=$(date +%s)
bash "$ROOT_DIR/scripts/test.sh" > "$FUNC_LOG" 2>&1
FUNC_CODE=$?
FUNC_END=$(date +%s)
FUNC_LINE="$(grep -E 'results: PASS=' "$FUNC_LOG" | tail -1)"
FUNC_PASS=$(printf '%s' "$FUNC_LINE" | sed -E 's/.*PASS=([0-9]+).*/\1/')
FUNC_FAIL=$(printf '%s' "$FUNC_LINE" | sed -E 's/.*FAIL=([0-9]+).*/\1/')
if [[ -z "$FUNC_PASS" ]]; then FUNC_PASS=0; fi
if [[ -z "$FUNC_FAIL" ]]; then FUNC_FAIL=0; fi
echo "  $FUNC_LINE（$((FUNC_END - FUNC_START))s, exit=$FUNC_CODE）"
if [[ $FUNC_CODE -eq 0 && $FUNC_FAIL -eq 0 ]]; then
  pass "FUNC-01 功能基线全部通过（PASS=$FUNC_PASS FAIL=$FUNC_FAIL）"
else
  fail "FUNC-01 功能基线全部通过" "exit=$FUNC_CODE $FUNC_LINE"
fi

# ======================================================================
# 步骤 2：构造测试数据
# ======================================================================
mkdir -p "$WORK_DIR"
SRC_SMALL="$WORK_DIR/src_small"
SRC_EMPTY="$WORK_DIR/src_empty"
SRC_DEEP="$WORK_DIR/src_deep"
SRC_FIFO="$WORK_DIR/src_fifo"
GOOD_ARCHIVE="$WORK_DIR/good.bak"

mktree() {
  local base="$1"
  mkdir -p "$base/empty_dir" "$base/docs/2026" "$base/中文目录/空格 目录"
  : > "$base/empty.txt"
  printf 'a' > "$base/one-byte.txt"
  printf 'hello backup\n' > "$base/docs/readme.txt"
  printf 'nested\n' > "$base/docs/2026/note.md"
  printf 'utf8 content\n' > "$base/中文目录/空格 目录/带 空格#%%的文件.txt"
  head -c 524288 /dev/urandom > "$base/binary.bin"
  chmod 0640 "$base/docs/readme.txt"
}

mktree "$SRC_SMALL"
mkdir -p "$SRC_EMPTY/a/b/c"
mkdir -p "$SRC_DEEP"
DEEP_PTR="$SRC_DEEP"
DEEP_I=0
while [[ $DEEP_I -lt 50 ]]; do
  DEEP_PTR="$DEEP_PTR/level$DEEP_I"
  mkdir -p "$DEEP_PTR"
  DEEP_I=$((DEEP_I + 1))
done
printf 'deep\n' > "$DEEP_PTR/bottom.txt"
head -c 4194304 /dev/urandom > "$WORK_DIR/big.bin"
SOURCE_FILES=$(find "$SRC_SMALL" -type f | wc -l)
SOURCE_DIRS=$(find "$SRC_SMALL" -mindepth 1 -type d | wc -l)
echo "  混合树: $SOURCE_FILES 个文件 / $SOURCE_DIRS 个目录；深树 51 层；大文件 4 MiB"

if expect_ok "SETUP-01 生成参考归档" "$BACKUPCTL" backup "$SRC_SMALL" "$GOOD_ARCHIVE"; then :; fi

# ======================================================================
# 维度 1：可用性
# ======================================================================
echo
echo "[quality] 维度 1/4 可用性 (usability)"
expect_ok_msg "US-01 --help 退出 0 并给出用法" "Usage" "$BACKUPCTL" --help
expect_ok_msg "US-02 --help 说明归档文件参数" "backup_file" "$BACKUPCTL" --help
expect_fail "US-03 无参数返回用法错误 2" 2 "Usage:" "$BACKUPCTL"
expect_fail "US-04 未知子命令返回 2" 2 "" "$BACKUPCTL" frobnicate a b
expect_fail "US-05 backup 缺参数返回 2" 2 "Usage:" "$BACKUPCTL" backup
expect_fail "US-06 restore 缺参数返回 2" 2 "Usage:" "$BACKUPCTL" restore
MISSING_SRC="$WORK_DIR/missing-source"
expect_fail "US-07 源不存在时报错含源路径" 1 "$MISSING_SRC" "$BACKUPCTL" backup "$MISSING_SRC" "$WORK_DIR/us07.bak"
expect_fail "US-08 归档不存在时报错说明原因" 1 "Backup file does not exist" "$BACKUPCTL" restore "$WORK_DIR/no-such.bak" "$WORK_DIR/us08-dest"
invoke "$BACKUPCTL" backup "$SRC_SMALL" "$WORK_DIR/us09.bak"
US09_CODE=$?
if [[ $US09_CODE -eq 0 ]] && grep -qF "Backup completed successfully." "$LOG"; then
  pass "US-09 backup 成功时给出可读的成功反馈"
else
  fail "US-09 backup 成功时给出可读的成功反馈" "exit=$US09_CODE 输出: $(head -1 "$LOG")"
fi
expect_ok_msg "US-10 restore 成功时给出可读的成功反馈" "Restore completed successfully." "$BACKUPCTL" restore "$WORK_DIR/us09.bak" "$WORK_DIR/us10-dest"

# ======================================================================
# 维度 2：鲁棒性
# ======================================================================
echo
echo "[quality] 维度 2/4 鲁棒性 (robustness)"
rm -rf "$WORK_DIR/rb01-dest"
expect_ok "RB-01 只含空目录的树可备份" "$BACKUPCTL" backup "$SRC_EMPTY" "$WORK_DIR/rb01.bak"
expect_ok "RB-01b 只含空目录的树可恢复" "$BACKUPCTL" restore "$WORK_DIR/rb01.bak" "$WORK_DIR/rb01-dest"
check_consistency "RB-01c 空目录骨架往返一致" "$SRC_EMPTY" "$WORK_DIR/rb01-dest"

rm -rf "$WORK_DIR/rb02-dest"
expect_ok "RB-02 50 层深目录可备份" "$BACKUPCTL" backup "$SRC_DEEP" "$WORK_DIR/rb02.bak"
expect_ok "RB-02b 50 层深目录可恢复" "$BACKUPCTL" restore "$WORK_DIR/rb02.bak" "$WORK_DIR/rb02-dest"
check_consistency "RB-02c 深目录往返一致" "$SRC_DEEP" "$WORK_DIR/rb02-dest"

rm -rf "$WORK_DIR/rb03-dest"
expect_ok "RB-03 混合树（空文件/单字节/中文/空格/#%/二进制/0640）可备份" "$BACKUPCTL" backup "$SRC_SMALL" "$WORK_DIR/rb03.bak"
expect_ok "RB-03b 混合树可恢复" "$BACKUPCTL" restore "$WORK_DIR/rb03.bak" "$WORK_DIR/rb03-dest"
check_consistency "RB-03c 混合树往返一致" "$SRC_SMALL" "$WORK_DIR/rb03-dest"

rm -rf "$WORK_DIR/rb04-src" "$WORK_DIR/rb04-dest"
mkdir -p "$WORK_DIR/rb04-src"
cp "$WORK_DIR/big.bin" "$WORK_DIR/rb04-src/big.bin"
expect_ok "RB-04 4 MiB 随机二进制可备份" "$BACKUPCTL" backup "$WORK_DIR/rb04-src" "$WORK_DIR/rb04.bak"
expect_ok "RB-04b 4 MiB 随机二进制可恢复" "$BACKUPCTL" restore "$WORK_DIR/rb04.bak" "$WORK_DIR/rb04-dest"
if cmp -s "$WORK_DIR/big.bin" "$WORK_DIR/rb04-dest/big.bin"; then
  pass "RB-04c 大文件字节级一致（cmp）"
else
  fail "RB-04c 大文件字节级一致（cmp）" "内容不同"
fi

expect_fail "RB-05 源目录不存在返回 1" 1 "$MISSING_SRC" "$BACKUPCTL" backup "$MISSING_SRC" "$WORK_DIR/rb05.bak"
expect_fail "RB-06 源是普通文件返回 1" 1 "" "$BACKUPCTL" backup "$WORK_DIR/big.bin" "$WORK_DIR/rb06.bak"
rm -rf "$WORK_DIR/missing-parent"
expect_ok "RB-07 归档父目录不存在时自动补建" "$BACKUPCTL" backup "$SRC_SMALL" "$WORK_DIR/missing-parent/deep/rb07.bak"
if [[ -f "$WORK_DIR/missing-parent/deep/rb07.bak" ]]; then
  pass "RB-07b 自动补建的归档文件确实生成"
else
  fail "RB-07b 自动补建的归档文件确实生成" "文件不存在"
fi

printf 'sentinel-content\n' > "$WORK_DIR/rb08.bak"
RB08_BEFORE=$(sha256sum "$WORK_DIR/rb08.bak" | cut -d' ' -f1)
expect_fail "RB-08 归档已存在时拒绝覆盖" 1 "" "$BACKUPCTL" backup "$SRC_SMALL" "$WORK_DIR/rb08.bak"
RB08_AFTER=$(sha256sum "$WORK_DIR/rb08.bak" | cut -d' ' -f1)
if [[ "$RB08_BEFORE" == "$RB08_AFTER" ]]; then
  pass "RB-09 拒绝覆盖后原归档字节未变（sha256 相同）"
else
  fail "RB-09 拒绝覆盖后原归档字节未变（sha256 相同）" "文件被改动"
fi

mkdir -p "$WORK_DIR/rb10-dest"
printf 'keep-me\n' > "$WORK_DIR/rb10-dest/keep.txt"
expect_fail "RB-10 restore 拒绝非空目标" 1 "" "$BACKUPCTL" restore "$GOOD_ARCHIVE" "$WORK_DIR/rb10-dest"
if [[ -f "$WORK_DIR/rb10-dest/keep.txt" ]] && [[ "$(cat "$WORK_DIR/rb10-dest/keep.txt")" == "keep-me" ]]; then
  pass "RB-11 拒绝后原有内容未被破坏"
else
  fail "RB-11 拒绝后原有内容未被破坏" "keep.txt 丢失或被改写"
fi

rm -rf "$SRC_FIFO"
mkdir -p "$SRC_FIFO"
mkfifo "$SRC_FIFO/pipe" 2> /dev/null || true
rm -f "$WORK_DIR/rb12.bak"
expect_fail "RB-12 FIFO 让整次备份失败" 1 "" "$BACKUPCTL" backup "$SRC_FIFO" "$WORK_DIR/rb12.bak"
if [[ ! -e "$WORK_DIR/rb12.bak" ]]; then
  pass "RB-13 失败时不留下半成品归档"
else
  fail "RB-13 失败时不留下半成品归档" "残留 $WORK_DIR/rb12.bak"
fi

: > "$WORK_DIR/rb14.bak"
expect_fail "RB-14 0 字节归档被拒绝" 1 "" "$BACKUPCTL" restore "$WORK_DIR/rb14.bak" "$WORK_DIR/rb14-dest"

head -c 200 "$GOOD_ARCHIVE" > "$WORK_DIR/rb15.bak"
expect_fail "RB-15 截断归档被拒绝" 1 "" "$BACKUPCTL" restore "$WORK_DIR/rb15.bak" "$WORK_DIR/rb15-dest"

cp "$GOOD_ARCHIVE" "$WORK_DIR/rb16.bak"
printf 'X' | dd of="$WORK_DIR/rb16.bak" bs=1 seek=0 conv=notrunc status=none
expect_fail "RB-16 破坏 magic 的归档被拒绝" 1 "" "$BACKUPCTL" restore "$WORK_DIR/rb16.bak" "$WORK_DIR/rb16-dest"

cp "$GOOD_ARCHIVE" "$WORK_DIR/rb17.bak"
printf '\377\377\000\000' | dd of="$WORK_DIR/rb17.bak" bs=1 seek=16 conv=notrunc status=none
expect_fail "RB-17 篡改 entry_count 的归档被拒绝" 1 "" "$BACKUPCTL" restore "$WORK_DIR/rb17.bak" "$WORK_DIR/rb17-dest"

build_traversal_archive() {
  local out="$1"
  local src="$WORK_DIR/trav_src"
  local base="$WORK_DIR/trav_base.bak"
  rm -rf "$src"
  rm -f "$base" "$out"
  mkdir -p "$src/d"
  printf 'boom\n' > "$src/d/aaaaaaaa.txt"
  invoke "$BACKUPCTL" backup "$src" "$base"
  if [[ $? -ne 0 ]]; then return 1; fi
  cp "$base" "$out"
  local off
  off="$(grep -abo -F 'd/aaaaaaaa.txt' "$out" | head -n 1 | cut -d: -f1)"
  if [[ -z "$off" ]]; then return 1; fi
  printf '../pwned00.txt' | dd of="$out" bs=1 seek="$off" conv=notrunc status=none
  return 0
}

rm -f "$WORK_DIR/pwned00.txt"
if build_traversal_archive "$WORK_DIR/rb18.bak"; then
  rm -rf "$WORK_DIR/rb18-dest"
  expect_fail "RB-18 归档中 ../ 路径被拒绝" 1 "" "$BACKUPCTL" restore "$WORK_DIR/rb18.bak" "$WORK_DIR/rb18-dest"
  if [[ ! -e "$WORK_DIR/pwned00.txt" ]]; then
    pass "RB-19 路径穿越未逃逸出目标目录"
  else
    fail "RB-19 路径穿越未逃逸出目标目录" "文件逃逸到 $WORK_DIR/pwned00.txt"
  fi
else
  fail "RB-18 归档中 ../ 路径被拒绝" "构造穿越归档失败"
  fail "RB-19 路径穿越未逃逸出目标目录" "前置构造失败"
fi

FUZZ_CRASH=0
FUZZ_HANG=0
FUZZ_OTHER=0
FUZZ_I=1
while [[ $FUZZ_I -le $FUZZ_ROUNDS ]]; do
  cp "$GOOD_ARCHIVE" "$WORK_DIR/fuzz.bak"
  FUZZ_SIZE=$(stat -c %s "$WORK_DIR/fuzz.bak")
  FUZZ_OFF=$((RANDOM % FUZZ_SIZE))
  dd if=/dev/urandom of="$WORK_DIR/fuzz.bak" bs=1 seek="$FUZZ_OFF" count=1 conv=notrunc status=none
  rm -rf "$WORK_DIR/fuzz-dest"
  invoke "$BACKUPCTL" restore "$WORK_DIR/fuzz.bak" "$WORK_DIR/fuzz-dest"
  FUZZ_CODE=$?
  if [[ $FUZZ_CODE -eq 124 ]]; then FUZZ_HANG=$((FUZZ_HANG + 1)); fi
  if [[ $FUZZ_CODE -ge 128 ]]; then FUZZ_CRASH=$((FUZZ_CRASH + 1)); fi
  if [[ $FUZZ_CODE -ne 0 && $FUZZ_CODE -ne 1 && $FUZZ_CODE -ne 2 ]]; then FUZZ_OTHER=$((FUZZ_OTHER + 1)); fi
  FUZZ_I=$((FUZZ_I + 1))
done
if [[ $FUZZ_CRASH -eq 0 && $FUZZ_HANG -eq 0 && $FUZZ_OTHER -eq 0 ]]; then
  pass "RB-20 $FUZZ_ROUNDS 次随机单字节破坏：无崩溃、无挂死、退出码可判定"
else
  fail "RB-20 $FUZZ_ROUNDS 次随机单字节破坏" "崩溃=$FUZZ_CRASH 挂死=$FUZZ_HANG 异常退出码=$FUZZ_OTHER"
fi

# ======================================================================
# 维度 3：稳定性
# ======================================================================
echo
echo "[quality] 维度 3/4 稳定性 (stability)：$ROUNDS 轮 backup -> restore 往返"
STAB_I=1
while [[ $STAB_I -le $ROUNDS ]]; do
  STAB_ARC="$WORK_DIR/stab-$STAB_I.bak"
  STAB_DST="$WORK_DIR/stab-$STAB_I-dest"
  rm -f "$STAB_ARC"
  rm -rf "$STAB_DST"
  STAB_START=$(date +%s%N)
  invoke "$BACKUPCTL" backup "$SRC_SMALL" "$STAB_ARC"
  STAB_CODE=$?
  STAB_OK=1
  STAB_WHY="ok"
  if [[ $STAB_CODE -ne 0 ]]; then STAB_OK=0; STAB_WHY="backup exit=$STAB_CODE"; fi
  if [[ $STAB_OK -eq 1 ]]; then
    invoke "$BACKUPCTL" restore "$STAB_ARC" "$STAB_DST"
    STAB_CODE=$?
    if [[ $STAB_CODE -ne 0 ]]; then STAB_OK=0; STAB_WHY="restore exit=$STAB_CODE"; fi
  fi
  if [[ $STAB_OK -eq 1 ]]; then
    if ! consistent "$SRC_SMALL" "$STAB_DST"; then STAB_OK=0; STAB_WHY="diff -r / sha256sum 不一致"; fi
  fi
  STAB_END=$(date +%s%N)
  STAB_MS=$(((STAB_END - STAB_START) / 1000000))
  if [[ $STAB_OK -eq 1 ]]; then
    STABILITY_PASS=$((STABILITY_PASS + 1))
    echo "  round $STAB_I/$ROUNDS: OK（$STAB_MS ms, $SOURCE_FILES 个文件）"
  else
    echo "  round $STAB_I/$ROUNDS: FAIL（$STAB_WHY）" >&2
  fi
  STAB_I=$((STAB_I + 1))
done
if [[ $STABILITY_PASS -eq $ROUNDS ]]; then
  pass "ST-01 稳定性 $STABILITY_PASS/$ROUNDS 轮往返全部 diff -r + sha256sum 一致"
else
  fail "ST-01 稳定性往返" "$STABILITY_PASS/$ROUNDS 轮通过"
fi

# ======================================================================
# 维度 4：健壮性（ASan + UBSan）
# ======================================================================
echo
echo "[quality] 维度 4/4 健壮性 (sanitizer: AddressSanitizer + UndefinedBehaviorSanitizer)"
san_run() {
  local desc="$1" want="$2"; shift 2
  SAN_RUNS=$((SAN_RUNS + 1))
  timeout --signal=KILL "$TIMEOUT_SECONDS" "$@" > "$SAN_LOG" 2>&1
  local code=$?
  if grep -qE 'AddressSanitizer|LeakSanitizer|runtime error:|SUMMARY: ' "$SAN_LOG"; then
    SAN_REPORTS=$((SAN_REPORTS + 1))
    fail "$desc" "sanitizer 报告: $(grep -m1 -E 'AddressSanitizer|runtime error:|SUMMARY: ' "$SAN_LOG")"
    return
  fi
  if [[ $code -eq 124 ]]; then fail "$desc" "超时被杀"; return; fi
  if [[ $code -ge 128 ]]; then fail "$desc" "被信号打死 exit=$code"; return; fi
  if [[ $code -ne $want ]]; then fail "$desc" "期望 exit=$want 实际 $code: $(head -2 "$SAN_LOG" | tr '\n' ' ')"; return; fi
  pass "$desc"
}

if [[ ! -x "$SAN_BACKUPCTL" ]]; then
  fail "SN-00 sanitizer 产物可用" "build-sanitize/backupctl 缺失，维度 4 无法执行"
else
  SAN_RUNS=$((SAN_RUNS + 1))
  rm -rf "$WORK_DIR/sn01-dest"
  timeout --signal=KILL "$TIMEOUT_SECONDS" "$SAN_BACKUPCTL" backup "$SRC_SMALL" "$WORK_DIR/sn01.bak" > "$SAN_LOG" 2>&1
  SAN_CODE=$?
  timeout --signal=KILL "$TIMEOUT_SECONDS" "$SAN_BACKUPCTL" restore "$WORK_DIR/sn01.bak" "$WORK_DIR/sn01-dest" >> "$SAN_LOG" 2>&1
  SAN_CODE2=$?
  SAN_RUNS=$((SAN_RUNS + 1))
  if grep -qE 'AddressSanitizer|LeakSanitizer|runtime error:|SUMMARY: ' "$SAN_LOG"; then
    SAN_REPORTS=$((SAN_REPORTS + 1))
    fail "SN-01 sanitizer 下正常往返" "sanitizer 报告: $(grep -m1 -E 'AddressSanitizer|runtime error:|SUMMARY: ' "$SAN_LOG")"
  elif [[ $SAN_CODE -eq 0 && $SAN_CODE2 -eq 0 ]] && consistent "$SRC_SMALL" "$WORK_DIR/sn01-dest"; then
    pass "SN-01 sanitizer 下正常往返退出 0 且 diff -r + sha256sum 一致"
  else
    fail "SN-01 sanitizer 下正常往返" "backup=$SAN_CODE restore=$SAN_CODE2"
  fi

  rm -rf "$WORK_DIR/sn02-dest"
  san_run "SN-02 sanitizer 下空目录树往返" 0 "$SAN_BACKUPCTL" backup "$SRC_EMPTY" "$WORK_DIR/sn02.bak"
  san_run "SN-02b sanitizer 下空目录树恢复" 0 "$SAN_BACKUPCTL" restore "$WORK_DIR/sn02.bak" "$WORK_DIR/sn02-dest"
  rm -rf "$WORK_DIR/sn03-dest"
  san_run "SN-03 sanitizer 下 50 层深目录往返（备份）" 0 "$SAN_BACKUPCTL" backup "$SRC_DEEP" "$WORK_DIR/sn03.bak"
  san_run "SN-03b sanitizer 下 50 层深目录往返（恢复）" 0 "$SAN_BACKUPCTL" restore "$WORK_DIR/sn03.bak" "$WORK_DIR/sn03-dest"
  rm -rf "$WORK_DIR/sn04-dest"
  san_run "SN-04 sanitizer 下 4 MiB 文件备份" 0 "$SAN_BACKUPCTL" backup "$WORK_DIR/rb04-src" "$WORK_DIR/sn04.bak"
  san_run "SN-04b sanitizer 下 4 MiB 文件恢复" 0 "$SAN_BACKUPCTL" restore "$WORK_DIR/sn04.bak" "$WORK_DIR/sn04-dest"
  san_run "SN-05 sanitizer 下 --help" 0 "$SAN_BACKUPCTL" --help
  san_run "SN-06 sanitizer 下源不存在报错" 1 "$SAN_BACKUPCTL" backup "$MISSING_SRC" "$WORK_DIR/sn06.bak"
  san_run "SN-07 sanitizer 下截断归档被拒" 1 "$SAN_BACKUPCTL" restore "$WORK_DIR/rb15.bak" "$WORK_DIR/sn07-dest"
  san_run "SN-08 sanitizer 下 magic 破坏被拒" 1 "$SAN_BACKUPCTL" restore "$WORK_DIR/rb16.bak" "$WORK_DIR/sn08-dest"
  san_run "SN-09 sanitizer 下 entry_count 篡改被拒" 1 "$SAN_BACKUPCTL" restore "$WORK_DIR/rb17.bak" "$WORK_DIR/sn09-dest"
  san_run "SN-10 sanitizer 下路径穿越被拒" 1 "$SAN_BACKUPCTL" restore "$WORK_DIR/rb18.bak" "$WORK_DIR/sn10-dest"
  san_run "SN-11 sanitizer 下 FIFO 备份失败" 1 "$SAN_BACKUPCTL" backup "$SRC_FIFO" "$WORK_DIR/sn11.bak"
  san_run "SN-12 sanitizer 下非空目标被拒" 1 "$SAN_BACKUPCTL" restore "$GOOD_ARCHIVE" "$WORK_DIR/rb10-dest"
  san_run "SN-13 sanitizer 下 0 字节归档被拒" 1 "$SAN_BACKUPCTL" restore "$WORK_DIR/rb14.bak" "$WORK_DIR/sn13-dest"

  SANFUZZ_CRASH=0
  SANFUZZ_REPORT=0
  SANFUZZ_I=1
  while [[ $SANFUZZ_I -le $SAN_FUZZ_ROUNDS ]]; do
    cp "$GOOD_ARCHIVE" "$WORK_DIR/sanfuzz.bak"
    SANFUZZ_SIZE=$(stat -c %s "$WORK_DIR/sanfuzz.bak")
    SANFUZZ_OFF=$((RANDOM % SANFUZZ_SIZE))
    dd if=/dev/urandom of="$WORK_DIR/sanfuzz.bak" bs=1 seek="$SANFUZZ_OFF" count=1 conv=notrunc status=none
    rm -rf "$WORK_DIR/sanfuzz-dest"
    SAN_RUNS=$((SAN_RUNS + 1))
    timeout --signal=KILL "$TIMEOUT_SECONDS" "$SAN_BACKUPCTL" restore "$WORK_DIR/sanfuzz.bak" "$WORK_DIR/sanfuzz-dest" > "$SAN_LOG" 2>&1
    SANFUZZ_CODE=$?
    if [[ $SANFUZZ_CODE -eq 124 || $SANFUZZ_CODE -ge 128 ]]; then SANFUZZ_CRASH=$((SANFUZZ_CRASH + 1)); fi
    if grep -qE 'AddressSanitizer|LeakSanitizer|runtime error:|SUMMARY: ' "$SAN_LOG"; then
      SANFUZZ_REPORT=$((SANFUZZ_REPORT + 1))
      SAN_REPORTS=$((SAN_REPORTS + 1))
    fi
    SANFUZZ_I=$((SANFUZZ_I + 1))
  done
  if [[ $SANFUZZ_CRASH -eq 0 && $SANFUZZ_REPORT -eq 0 ]]; then
    pass "SN-14 sanitizer 下 $SAN_FUZZ_ROUNDS 次随机位翻转：零报告、零崩溃"
  else
    fail "SN-14 sanitizer 下 $SAN_FUZZ_ROUNDS 次随机位翻转" "崩溃=$SANFUZZ_CRASH sanitizer 报告=$SANFUZZ_REPORT"
  fi
fi

# ======================================================================
# 汇总
# ======================================================================
END_TS=$(date +%s)
echo
echo "[quality] ================= 汇总 =================="
echo "[quality] 功能基线 scripts/test.sh : PASS=$FUNC_PASS FAIL=$FUNC_FAIL"
echo "[quality] 质量维度用例           : PASS=$PASS_COUNT FAIL=$FAIL_COUNT"
echo "[quality] 稳定性往返             : $STABILITY_PASS/$ROUNDS 轮 diff -r + sha256sum 一致"
echo "[quality] sanitizer 调用         : $SAN_RUNS 次，报告 $SAN_REPORTS 次"
echo "[quality] 编译警告               : $BUILD_WARNINGS 条"
echo "[quality] 总耗时                 : $((END_TS - START_TS))s"

KEEP_FLAG=0
if [[ -n "$(printenv KEEP_TESTDATA || true)" ]]; then KEEP_FLAG=1; fi

if [[ $FAIL_COUNT -eq 0 && $STABILITY_PASS -eq $ROUNDS && $SAN_REPORTS -eq 0 && $FUNC_FAIL -eq 0 ]]; then
  echo "[quality] 结论：四个维度全部通过"
  if [[ $KEEP_FLAG -eq 0 ]]; then
    chmod -R u+rwX "$WORK_DIR" 2> /dev/null || true
    rm -rf "$WORK_DIR" || true
  fi
  exit 0
fi

echo "[quality] 结论：存在未通过项，现场保留在 $WORK_DIR" >&2
exit 1
