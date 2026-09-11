#!/usr/bin/env bash
#
# Sprint 1 测试套件：基础 CLI backup / restore，外加路径健壮性检查。
#
# 分三个区：
#   A. round trip：backup -> restore -> diff -r 与 sha256 比对。
#   B. error paths：Sprint 1 已有的参数与文件系统错误，比如路径缺失、
#      目标非空、不支持的文件类型、权限不足、用法错误。
#   C. path topology：destination 不能等于 source，也不能落在 source
#      里面（ROB-02 / ROB-03）；同时 /tmp/a 与 /tmp/abc 这种只有字符串
#      前缀相同的路径，以及 "." ".." 规范化后才成为父子的路径，
#      两个方向都要判对。
#
# 每个 backupctl 调用都有硬超时兜底：万一递归复制出问题，
# 也不会把磁盘写满、把测试进程挂死。
#
# 测试数据生成在 <repo>/testdata（已 gitignore），全套通过后自动删除；
# 想保留现场就设置 KEEP_TESTDATA=1。
#
# 退出码：只有全部用例通过才是 0。

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BACKUPCTL="$ROOT_DIR/build/backupctl"
TEST_ROOT="$ROOT_DIR/testdata"
SOURCE="$TEST_ROOT/source"
REPOSITORY="$TEST_ROOT/repository"
RESTORED="$TEST_ROOT/restored"
ROBUST="$TEST_ROOT/robust"
OUT_FILE="$TEST_ROOT/last-output.txt"
TIMEOUT_SECONDS=30

PASS_COUNT=0
FAIL_COUNT=0
STATUS=0

echo "[test] Sprint 1 backup/restore suite (with path robustness checks)"

if [[ ! -x "$BACKUPCTL" ]]; then
  echo "[test] backupctl is missing; building first..."
  make -C "$ROOT_DIR"
fi

record_pass() {
  PASS_COUNT=$((PASS_COUNT + 1))
  echo "  PASS: $1"
}

record_fail() {
  FAIL_COUNT=$((FAIL_COUNT + 1))
  echo "  FAIL: $1 -- $2" >&2
}

# 带硬超时地运行 backupctl：合并后的输出写进 $OUT_FILE，
# 退出码留在 $STATUS（124 表示超时被杀）。
run_backupctl() {
  set +e
  timeout --signal=KILL "$TIMEOUT_SECONDS" "$BACKUPCTL" "$@" >"$OUT_FILE" 2>&1
  STATUS=$?
  set -e
  if [[ $STATUS -eq 124 || $STATUS -eq 137 ]]; then
    STATUS=124
  fi
}

first_line() {
  head -n 1 "$OUT_FILE" | cut -c1-160
}

# 命令必须以退出码 0 成功。
expect_success() {
  local name="$1"
  shift
  run_backupctl "$@"
  if [[ $STATUS -eq 124 ]]; then
    record_fail "$name" "timed out after ${TIMEOUT_SECONDS}s"
  elif [[ $STATUS -eq 0 ]]; then
    record_pass "$name"
  else
    record_fail "$name" "expected exit 0, got $STATUS: $(first_line)"
  fi
}

# 命令必须以 |expected_code| 失败，且报错信息里出现 |expected_text|。
expect_failure() {
  local name="$1"
  local expected_code="$2"
  local expected_text="$3"
  shift 3
  run_backupctl "$@"
  if [[ $STATUS -eq 124 ]]; then
    record_fail "$name" "timed out after ${TIMEOUT_SECONDS}s"
  elif [[ $STATUS -eq 0 ]]; then
    record_fail "$name" "expected exit $expected_code, got 0"
  elif [[ $STATUS -ge 128 ]]; then
    record_fail "$name" "crashed (exit code $STATUS)"
  elif [[ $STATUS -ne $expected_code ]]; then
    record_fail "$name" \
      "expected exit $expected_code, got $STATUS: $(first_line)"
  elif ! grep -qF -- "$expected_text" "$OUT_FILE"; then
    record_fail "$name" \
      "error text \"$expected_text\" is missing: $(first_line)"
  else
    record_pass "$name"
  fi
}

expect_path_exists() {
  if [[ -e "$2" ]]; then
    record_pass "$1"
  else
    record_fail "$1" "missing: $2"
  fi
}

expect_path_absent() {
  if [[ ! -e "$2" ]]; then
    record_pass "$1"
  else
    record_fail "$1" "unexpected: $2"
  fi
}

expect_file_content() {
  if [[ "$(cat "$2")" == "$3" ]]; then
    record_pass "$1"
  else
    record_fail "$1" "content of $2 does not match"
  fi
}

expect_same_tree() {
  if diff -r "$2" "$3" >/dev/null 2>&1; then
    record_pass "$1"
  else
    record_fail "$1" "diff -r reports differences"
  fi
}

expect_same_sha256() {
  local left right
  left="$(sha256sum "$2" | awk '{print $1}')"
  right="$(sha256sum "$3" | awk '{print $1}')"
  if [[ "$left" == "$right" ]]; then
    record_pass "$1"
  else
    record_fail "$1" "sha256 mismatch"
  fi
}

# ---- 测试数据 --------------------------------------------------------

rm -rf "$TEST_ROOT"
mkdir -p "$TEST_ROOT" \
  "$SOURCE/empty_dir" \
  "$SOURCE/directory with spaces" \
  "$SOURCE/中文目录" \
  "$SOURCE/level1/level2"

printf 'Hello, backup project!\n' > "$SOURCE/hello.txt"
: > "$SOURCE/empty.txt"
# 2 MiB 随机数据：二进制内容必须逐字节活下来。
head -c 2097152 /dev/urandom > "$SOURCE/binary.bin"
printf 'Space in file name.\n' > "$SOURCE/directory with spaces/space file.txt"
printf 'UTF-8 中文内容：备份与恢复。\n' > "$SOURCE/中文目录/中文文件.txt"
printf 'Nested file content.\n' > "$SOURCE/level1/level2/nested.txt"

# ---- A. 正常回环 -----------------------------------------------------

echo "[test] A. round trip"
expect_success "BR-01 backup succeeds" backup "$SOURCE" "$REPOSITORY"
expect_path_exists "BR-02 backup created <repository>/data" "$REPOSITORY/data"
expect_success "BR-03 restore succeeds" restore "$REPOSITORY" "$RESTORED"
expect_same_tree "BR-04 restored tree matches source (diff -r)" "$SOURCE" \
  "$RESTORED"
expect_same_sha256 "BR-05 binary.bin is byte-identical (sha256)" \
  "$SOURCE/binary.bin" "$RESTORED/binary.bin"

if [[ -f "$RESTORED/empty.txt" && ! -s "$RESTORED/empty.txt" ]]; then
  record_pass "BR-06 empty file stays empty"
else
  record_fail "BR-06 empty file stays empty" "missing or not empty"
fi

if [[ -d "$RESTORED/empty_dir" && -z "$(ls -A "$RESTORED/empty_dir")" ]]; then
  record_pass "BR-07 empty directory is restored"
else
  record_fail "BR-07 empty directory is restored" "missing or not empty"
fi

if [[ -f "$RESTORED/level1/level2/nested.txt" &&
      -f "$RESTORED/directory with spaces/space file.txt" &&
      -f "$RESTORED/中文目录/中文文件.txt" ]]; then
  record_pass "BR-08 nested, spaced and UTF-8 names are restored"
else
  record_fail "BR-08 nested, spaced and UTF-8 names are restored" \
    "some names are missing"
fi

# ---- B. 错误路径 -----------------------------------------------------

echo "[test] B. error paths"
expect_failure "ER-01 source directory does not exist" 1 "does not exist" \
  backup "$TEST_ROOT/no_such_source" "$TEST_ROOT/repo-er01"
expect_failure "ER-02 source is a regular file" 1 "not a directory" \
  backup "$SOURCE/hello.txt" "$TEST_ROOT/repo-er02"
expect_failure "ER-03 restore from a missing repository" 1 "does not exist" \
  restore "$TEST_ROOT/no_such_repo" "$TEST_ROOT/rest-er03"

mkdir -p "$TEST_ROOT/repo-no-data"
expect_failure "ER-04 repository without data directory" 1 \
  "data directory does not exist" \
  restore "$TEST_ROOT/repo-no-data" "$TEST_ROOT/rest-er04"

mkdir -p "$TEST_ROOT/busy-dest"
printf 'keep\n' > "$TEST_ROOT/busy-dest/keep.txt"
expect_failure "ER-05 destination exists and is not empty" 1 \
  "already exists and is not empty" \
  restore "$REPOSITORY" "$TEST_ROOT/busy-dest"
expect_failure "ER-06 repository data exists and is not empty" 1 \
  "already exists and is not empty" \
  backup "$SOURCE" "$REPOSITORY"

mkdir -p "$TEST_ROOT/symlink-src"
printf 'target\n' > "$TEST_ROOT/symlink-src/target.txt"
ln -s target.txt "$TEST_ROOT/symlink-src/link.txt"
expect_failure "ER-07 symlink is rejected" 1 "Unsupported file type" \
  backup "$TEST_ROOT/symlink-src" "$TEST_ROOT/repo-er07"

mkdir -p "$TEST_ROOT/fifo-src"
mkfifo "$TEST_ROOT/fifo-src/pipe"
expect_failure "ER-08 FIFO is rejected" 1 "Unsupported file type" \
  backup "$TEST_ROOT/fifo-src" "$TEST_ROOT/repo-er08"

mkdir -p "$TEST_ROOT/unreadable-src"
printf 'secret\n' > "$TEST_ROOT/unreadable-src/locked.txt"
chmod 000 "$TEST_ROOT/unreadable-src/locked.txt"
expect_failure "ER-09 unreadable file is reported" 1 "Permission denied" \
  backup "$TEST_ROOT/unreadable-src" "$TEST_ROOT/repo-er09"
chmod 644 "$TEST_ROOT/unreadable-src/locked.txt"

printf 'blocker\n' > "$TEST_ROOT/blocker"
expect_failure "ER-10 repository cannot be created (parent is a file)" 1 \
  "Failed to create destination directory" backup "$SOURCE" \
    "$TEST_ROOT/blocker/repo"
expect_failure "ER-11 destination cannot be created (parent is a file)" 1 \
  "Failed to create destination directory" restore "$REPOSITORY" \
    "$TEST_ROOT/blocker/dest"

expect_failure "ER-12 usage error returns 2" 2 "Usage:" backup "$SOURCE"
expect_failure "ER-13 unknown command returns 2" 2 "unknown command" \
  frobnicate a b

# ---- C. 路径拓扑 -----------------------------------------------------

echo "[test] C. path topology"
mkdir -p "$ROBUST"

# ROB-02：repository 被放在 source 目录里面。
mkdir -p "$ROBUST/repo-in-source"
printf 'keep\n' > "$ROBUST/repo-in-source/keep.txt"
expect_failure "RB-01 repository inside source is rejected" 1 \
  "inside the source directory" \
  backup "$ROBUST/repo-in-source" "$ROBUST/repo-in-source/repository"
# 拒绝之后必须一点痕迹都不留：不只是没有 repository/data，
# 连 repository 目录本身都不该被创建出来。
expect_path_absent "RB-02 rejected backup created no repository at all" \
  "$ROBUST/repo-in-source/repository"

# ROB-03：destination 被放在 <repository>/data 里面。
mkdir -p "$ROBUST/repo-outside/data"
printf 'payload\n' > "$ROBUST/repo-outside/data/file.txt"
expect_failure "RB-03 destination inside repository/data is rejected" 1 \
  "inside the source directory" \
  restore "$ROBUST/repo-outside" "$ROBUST/repo-outside/data/restored-inside"
expect_path_absent "RB-04 rejection left no restored tree behind" \
  "$ROBUST/repo-outside/data/restored-inside"

# destination == source。data 目录是空的，所以引擎会一路走到复制这一步，
# 最后由 FileSystem 的拓扑检查负责拒绝。
mkdir -p "$ROBUST/same-repository/data"
expect_failure "RB-05 destination equal to source is rejected" 1 \
  "same as the source directory" \
  restore "$ROBUST/same-repository" "$ROBUST/same-repository/data"

# 被拒绝的 restore 不能往它拒绝写入的目录树里添加任何东西。
if [[ -z "$(ls -A "$ROBUST/same-repository/data")" ]]; then
  record_pass "RB-05b rejected restore added no new content"
else
  record_fail "RB-05b rejected restore added no new content" \
    "data directory is not empty"
fi

# 两个路径只有字符串前缀相同，并不是父子关系，必须放行。
mkdir -p "$ROBUST/a"
printf 'value\n' > "$ROBUST/a/file.txt"
expect_success "RB-06 lookalike paths (/a vs /abc) are allowed" backup \
  "$ROBUST/a" "$ROBUST/abc"
expect_file_content "RB-07 lookalike backup copied the file" \
  "$ROBUST/abc/data/file.txt" "value"

# 相对路径按当前工作目录解析。
mkdir -p "$ROBUST/relative/a"
printf 'relative\n' > "$ROBUST/relative/a/file.txt"
pushd "$ROBUST/relative" >/dev/null
expect_success "RB-08 relative sibling paths are allowed" backup a abc
expect_failure "RB-09 relative descendant path is rejected" 1 \
  "inside the source directory" \
  backup a a/sub
popd >/dev/null
expect_file_content "RB-10 relative backup copied the file" \
  "$ROBUST/relative/abc/data/file.txt" "relative"

# "." 和 ".." 会先规范化，再做拓扑判断。
mkdir -p "$ROBUST/normalized/src"
printf 'normalized\n' > "$ROBUST/normalized/src/file.txt"
pushd "$ROBUST/normalized" >/dev/null
expect_failure "RB-11 dot-dot path into the source is rejected" 1 \
  "inside the source directory" \
  backup src tmp/../src/sub
expect_failure "RB-12 dot as source rejects a descendant destination" 1 \
  "inside the source directory" \
  backup . ./out
expect_success "RB-13 dot-dot path leaving the source is allowed" backup ./src \
  ./dest/../dest2
popd >/dev/null
expect_file_content "RB-14 normalized backup copied the file" \
  "$ROBUST/normalized/dest2/data/file.txt" "normalized"

# 父目录里带软链接时，必须先解析链接再做拓扑判断：只比原始字符串的话，
# 这两条路径看着毫不相干，复制会一头扎进自己里面；先解析链接，
# 才能看出 destination 其实就在 source 内部。
mkdir -p "$ROBUST/symlink-parent/real/src"
printf 'linked\n' > "$ROBUST/symlink-parent/real/src/file.txt"
ln -s real "$ROBUST/symlink-parent/alias"
expect_failure "RB-15 symlinked parent inside the source is rejected" 1 \
  "inside the source directory" \
  backup "$ROBUST/symlink-parent/alias/src" \
    "$ROBUST/symlink-parent/real/src/sub"
expect_path_absent "RB-16 symlinked rejection created no destination" \
  "$ROBUST/symlink-parent/real/src/sub"

# ---- CLI 约定 --------------------------------------------------------

expect_success "CLI-01 --help exits 0" --help

# ---- 清理与汇总 ------------------------------------------------------

if [[ $FAIL_COUNT -eq 0 && -z "${KEEP_TESTDATA:-}" ]]; then
  rm -rf "$TEST_ROOT"
else
  echo "[test] keeping test data in $TEST_ROOT"
fi

echo "[test] results: PASS=${PASS_COUNT} FAIL=${FAIL_COUNT}"
if [[ $FAIL_COUNT -ne 0 ]]; then
  echo "[test] suite FAILED" >&2
  exit 1
fi
echo "[test] all cases passed"
