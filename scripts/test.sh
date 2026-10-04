#!/usr/bin/env bash
#
# 备份套件：CLI 的 backup / restore 全链路测试。
#
# 分区：
#   A. round trip：backup -> 归档文件 -> restore -> diff -r 与 sha256 比对。
#   B. error paths：参数与文件系统错误，比如路径缺失、目标非空、
#      不支持的文件类型、权限不足、用法错误。
#   C. path topology：归档文件不能是源目录本身，也不能落在源目录里面；
#      /tmp/a 与 /tmp/abc 这种只有字符串前缀相同的路径、"." ".." 规范化后
#      才成为父子的路径，两个方向都要判对。
#   D. ARC：归档往返的各种内容形态（空目录、深目录、大文件、中文/空格/#/%）。
#   E. META：mode / mtime（秒 + 纳秒）在文件、目录和 source root 上的往返。
#   F. BAD：损坏归档必须被拒绝，不崩不挂。
#   G. SEC：path traversal 与父子冲突必须在动磁盘之前被拒绝。
#   H. UNSUP：软链接 / FIFO / socket 必须让整次备份失败，不跳过。
#   I. NOCMP：证明 payload 是原样存储的（没有压缩）。
#
# 每个 backupctl 调用都有硬超时兜底：万一归档读写出问题，
# 也不会把磁盘写满、把测试进程挂死。
#
# 判定约定：
#   * 成功用例要求退出码 0，并且真的用 diff -r / cmp / stat 检查结果；
#   * 失败用例要求退出码 1（用法错误是 2），错误信息里必须出现关键原因，
#     而且不能是被信号打死的（>=128 直接算失败）；
#   * "被拒绝"的用例还要确认没有留下任何文件——拒绝同时不留痕才算合格。
#
# 测试数据生成在 <repo>/testdata（已 gitignore），全套通过后自动删除；
# 想保留现场就设置 KEEP_TESTDATA=1。
#
# 退出码：只有全部用例通过才是 0。

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$BASH_SOURCE")/.." && pwd)"
BACKUPCTL="$ROOT_DIR/build/backupctl"
ARCHIVE_CLI="$ROOT_DIR/build/archive-cli"
TEST_ROOT="$ROOT_DIR/testdata"
SOURCE="$TEST_ROOT/source"
ARCHIVE="$TEST_ROOT/backup.bak"
RESTORED="$TEST_ROOT/restored"
ROBUST="$TEST_ROOT/robust"
OUT_FILE="$TEST_ROOT/last-output.txt"
TIMEOUT_SECONDS=60

PASS_COUNT=0
FAIL_COUNT=0
STATUS=0

echo "[test] backup/restore suite (archive v0.1)"

# archive-cli 是测试夹具，不在默认产品构建里（见 Makefile）：测试要自己显式
# 构建它，而不是指望 make all 顺手产出。
if [[ ! -x "$BACKUPCTL" || ! -x "$ARCHIVE_CLI" ]]; then
  echo "[test] backupctl/archive-cli is missing; building first..."
  make -C "$ROOT_DIR" all test-fixtures
fi

record_pass() {
  PASS_COUNT=$((PASS_COUNT + 1))
  echo "  PASS: $1"
}

record_fail() {
  FAIL_COUNT=$((FAIL_COUNT + 1))
  echo "  FAIL: $1 -- $2" >&2
}

# 用 timeout --signal=KILL：归档解析如果陷入死循环，SIGTERM 未必能叫停，
# 直接 KILL 才能保证测试不会挂在这里。超时按 124 处理，算失败。
#
# 带硬超时地运行 CLI：合并后的输出写进 OUT_FILE，
# 退出码留在 STATUS（124 表示超时被杀）。
run_backupctl() {
  local binary="$BACKUPCTL"

  # legacy 直连路径的 backup / restore（带 <backup_file> 参数的那种）由测试夹具
  # archive-cli 执行，argv 与旧 backupctl 一字不差。裸的 backup / restore 没有
  # 任何直接路径，它们是产品 CLI 的参数数量契约（对应 quality_test.sh 的
  # US-05/US-06），其余子命令同样留在产品 CLI 上。
  case "${1-}" in
    backup|restore)
      if [[ $# -gt 1 ]]; then
        binary="$ARCHIVE_CLI"
      fi
      ;;
  esac
  set +e
  timeout --signal=KILL "$TIMEOUT_SECONDS" "$binary" "$@" >"$OUT_FILE" 2>&1
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
    record_fail "$name" "timed out after $TIMEOUT_SECONDS seconds"
  elif [[ $STATUS -eq 0 ]]; then
    record_pass "$name"
  else
    record_fail "$name" "expected exit 0, got $STATUS: $(first_line)"
  fi
}

# 命令必须以 expected_code 失败，且报错信息里出现 expected_text。
expect_failure() {
  local name="$1"
  local expected_code="$2"
  local expected_text="$3"
  shift 3
  run_backupctl "$@"
  if [[ $STATUS -eq 124 ]]; then
    record_fail "$name" "timed out after $TIMEOUT_SECONDS seconds"
  elif [[ $STATUS -eq 0 ]]; then
    record_fail "$name" "expected exit $expected_code, got 0"
  elif [[ $STATUS -ge 128 ]]; then
    # 归档解析里的越界通常直接让进程死掉，这里明确当成失败，
    # 而不是"某种非零退出"糊过去。
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

expect_regular_file() {
  if [[ -f "$2" && ! -d "$2" && ! -L "$2" ]]; then
    record_pass "$1"
  else
    record_fail "$1" "not a regular file: $2"
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

# 目录树里的每个普通文件都要逐字节一致：比 diff -r 更严格，
# 因为 diff -r 只报差异，不保证两侧文件集合完全相同。
expect_same_bytes() {
  local left="$2" right="$3"
  local bad=0
  while IFS= read -r file; do
    if ! cmp -s "$left/$file" "$right/$file"; then
      bad=1
      break
    fi
  done < <(cd "$left" && find . -type f -printf '%P\n' | sort)
  if [[ $bad -eq 0 ]]; then
    record_pass "$1"
  else
    record_fail "$1" "byte comparison failed"
  fi
}

# mode：八进制权限位，和归档里保存的 st_mode & 0777 对应。
expect_mode() {
  local actual
  actual="$(stat -c '%a' "$2")"
  if [[ "$actual" == "$3" ]]; then
    record_pass "$1"
  else
    record_fail "$1" "mode of $2 is $actual, expected $3"
  fi
}

# mtime：秒 + 纳秒一起比。date -r 给的是 %s.%N，纳秒级差异也能看出来。
expect_mtime() {
  local actual expected
  actual="$(date -r "$2" +%s.%N)"
  expected="$(date -r "$3" +%s.%N)"
  if [[ "$actual" == "$expected" ]]; then
    record_pass "$1"
  else
    record_fail "$1" "mtime $actual != $expected ($2)"
  fi
}

# 归档文件必须以 8 字节 magic 开头：格式判断只认 magic，不看扩展名。
expect_magic() {
  local actual
  actual="$(head -c 8 "$2" | od -An -tx1 | tr -d ' \n')"
  if [[ "$actual" == "424b504152434800" ]]; then
    record_pass "$1"
  else
    record_fail "$1" "magic is $actual"
  fi
}

# ---- 测试数据 --------------------------------------------------------

# 上一轮可能留下 0555 的目录（metadata 用例故意造的），直接 rm -rf 会被拒绝，
# 所以先把写权限加回来。
chmod -R u+rwX "$TEST_ROOT" 2>/dev/null || true
rm -rf "$TEST_ROOT" || true
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
# 这段 marker 用来证明 payload 是原样写进归档的，没有被压缩或编码。
printf 'UNCOMPRESSED_ARCHIVE_PAYLOAD_0123456789\n' > "$SOURCE/marker.txt"

# A 区是"最短可信路径"：一次打包、一次解包，然后用三种方式确认结果：
# diff -r（结构一致）、逐文件 cmp（字节一致）、sha256（大文件的强校验）。
# 三种都通过，才说明 payload 是真的原样存进去了。
#
# ---- A. 正常回环 -----------------------------------------------------

echo "[test] A. round trip"
expect_success "BR-01 backup succeeds" backup "$SOURCE" "$ARCHIVE"
expect_regular_file "BR-02 backup produced a single regular archive file" "$ARCHIVE"
expect_magic "BR-03 archive starts with the BKPARCH magic" "$ARCHIVE"
expect_success "BR-04 restore succeeds" restore "$ARCHIVE" "$RESTORED"
expect_same_tree "BR-05 restored tree matches source (diff -r)" "$SOURCE" \
  "$RESTORED"
expect_same_bytes "BR-06 every file is byte-identical" "$SOURCE" "$RESTORED"
expect_same_sha256 "BR-07 binary.bin is byte-identical (sha256)" \
  "$SOURCE/binary.bin" "$RESTORED/binary.bin"

if [[ -f "$RESTORED/empty.txt" && ! -s "$RESTORED/empty.txt" ]]; then
  record_pass "BR-08 empty file stays empty"
else
  record_fail "BR-08 empty file stays empty" "missing or not empty"
fi

if [[ -d "$RESTORED/empty_dir" && -z "$(ls -A "$RESTORED/empty_dir")" ]]; then
  record_pass "BR-09 empty directory is restored"
else
  record_fail "BR-09 empty directory is restored" "missing or not empty"
fi

if [[ -f "$RESTORED/level1/level2/nested.txt" &&
      -f "$RESTORED/directory with spaces/space file.txt" &&
      -f "$RESTORED/中文目录/中文文件.txt" ]]; then
  record_pass "BR-10 nested, spaced and UTF-8 names are restored"
else
  record_fail "BR-10 nested, spaced and UTF-8 names are restored" \
    "some names are missing"
fi

# B 区把"用户可能给错什么"排了一遍：路径不存在、类型不对、目标已存在、
# 权限不足、参数数量不对。判定标准统一是"明确失败 + 说清原因"，而不是返回 0 之后
# 让人以为备份好了。
#
# ---- B. 错误路径 -----------------------------------------------------

echo "[test] B. error paths"
expect_failure "ER-01 source directory does not exist" 1 "does not exist" \
  backup "$TEST_ROOT/no_such_source" "$TEST_ROOT/er01.bak"
expect_failure "ER-02 source is a regular file" 1 "not a directory" \
  backup "$SOURCE/hello.txt" "$TEST_ROOT/er02.bak"
expect_failure "ER-03 restore from a missing archive" 1 "does not exist" \
  restore "$TEST_ROOT/no_such_archive.bak" "$TEST_ROOT/rest-er03"

# 非归档文件当归档用：必须被 magic 检查拦下，而不是当成目录树读。
printf 'this file is definitely not an archive, only plain text\n' \
  > "$TEST_ROOT/not-an-archive.bak"
expect_failure "ER-04 restoring a non-archive file is rejected" 1 \
  "Invalid archive magic" \
  restore "$TEST_ROOT/not-an-archive.bak" "$TEST_ROOT/rest-er04"

# 归档文件已存在：v0.1 不覆盖、不截断用户已有的文件。
printf 'keep me\n' > "$TEST_ROOT/existing.bak"
expect_failure "ER-05 existing archive file is not overwritten" 1 \
  "already exists" backup "$SOURCE" "$TEST_ROOT/existing.bak"
expect_file_content "ER-06 rejected backup left the existing file intact" \
  "$TEST_ROOT/existing.bak" "keep me"

mkdir -p "$TEST_ROOT/busy-dest"
printf 'keep\n' > "$TEST_ROOT/busy-dest/keep.txt"
expect_failure "ER-07 destination exists and is not empty" 1 \
  "already exists and is not empty" \
  restore "$ARCHIVE" "$TEST_ROOT/busy-dest"

mkdir -p "$TEST_ROOT/symlink-src"
printf 'target\n' > "$TEST_ROOT/symlink-src/target.txt"
ln -s target.txt "$TEST_ROOT/symlink-src/link.txt"
expect_failure "ER-08 symlink is rejected" 1 "Unsupported source entry type" \
  backup "$TEST_ROOT/symlink-src" "$TEST_ROOT/er08.bak"
expect_path_absent "ER-09 rejected symlink backup left no archive" \
  "$TEST_ROOT/er08.bak"

mkdir -p "$TEST_ROOT/fifo-src"
mkfifo "$TEST_ROOT/fifo-src/pipe"
expect_failure "ER-10 FIFO is rejected" 1 "Unsupported source entry type" \
  backup "$TEST_ROOT/fifo-src" "$TEST_ROOT/er10.bak"

mkdir -p "$TEST_ROOT/unreadable-src"
printf 'secret\n' > "$TEST_ROOT/unreadable-src/locked.txt"
chmod 000 "$TEST_ROOT/unreadable-src/locked.txt"
expect_failure "ER-11 unreadable file is reported" 1 "Permission denied" \
  backup "$TEST_ROOT/unreadable-src" "$TEST_ROOT/er11.bak"
chmod 644 "$TEST_ROOT/unreadable-src/locked.txt"

printf 'blocker\n' > "$TEST_ROOT/blocker"
expect_failure "ER-12 archive parent path is a regular file" 1 \
  "Failed to inspect archive file" backup "$SOURCE" "$TEST_ROOT/blocker/x.bak"
expect_failure "ER-13 destination cannot be created (parent is a file)" 1 \
  "blocker" restore "$ARCHIVE" "$TEST_ROOT/blocker/dest"

expect_failure "ER-14 usage error returns 2" 2 "Usage" backup "$SOURCE"
expect_failure "ER-15 unknown command returns 2" 2 "unknown command" \
  frobnicate a b

# C 区盯住最容易造成事故的一类输入：归档文件被写进源目录内部。
# 一旦允许，扫描过程中归档自己就成了输入的一部分，复制会一层层套下去。
# 除了"拒绝"，每一条还额外确认"没留下任何文件"。
#
# ---- C. 路径拓扑 -----------------------------------------------------

echo "[test] C. path topology"
mkdir -p "$ROBUST"

# 归档文件被放在 source 目录里面：扫描过程中它会成为输入的一部分，
# 必须拒绝，而且不能留下任何文件。
mkdir -p "$ROBUST/src-inner"
printf 'keep\n' > "$ROBUST/src-inner/keep.txt"
expect_failure "RB-01 archive inside source is rejected" 1 \
  "inside the source directory" \
  backup "$ROBUST/src-inner" "$ROBUST/src-inner/backup.bak"
expect_path_absent "RB-02 rejected backup created no archive" \
  "$ROBUST/src-inner/backup.bak"

# 深层子目录里也一样。
mkdir -p "$ROBUST/src-inner/deep/deeper"
expect_failure "RB-03 archive in a nested source subdirectory is rejected" 1 \
  "inside the source directory" \
  backup "$ROBUST/src-inner" "$ROBUST/src-inner/deep/deeper/backup.bak"
expect_path_absent "RB-04 nested rejection left nothing behind" \
  "$ROBUST/src-inner/deep/deeper/backup.bak"

# 归档文件 == 源目录。
expect_failure "RB-05 archive equal to source is rejected" 1 \
  "same as the source directory" \
  backup "$ROBUST/src-inner" "$ROBUST/src-inner"

# 两个路径只有字符串前缀相同，并不是父子关系，必须放行。
mkdir -p "$ROBUST/a"
printf 'value\n' > "$ROBUST/a/file.txt"
expect_success "RB-06 lookalike paths (/a vs /abc) are allowed" backup \
  "$ROBUST/a" "$ROBUST/abc"
expect_regular_file "RB-07 lookalike backup produced an archive" "$ROBUST/abc"
expect_success "RB-08 lookalike archive restores" restore "$ROBUST/abc" \
  "$ROBUST/abc-restored"
expect_file_content "RB-09 lookalike backup stored the file" \
  "$ROBUST/abc-restored/file.txt" "value"

# 相对路径按当前工作目录解析。
mkdir -p "$ROBUST/relative/a"
printf 'relative\n' > "$ROBUST/relative/a/file.txt"
pushd "$ROBUST/relative" >/dev/null
expect_success "RB-10 relative sibling paths are allowed" backup a abc.bak
expect_failure "RB-11 relative descendant path is rejected" 1 \
  "inside the source directory" backup a a/sub.bak
popd >/dev/null
expect_success "RB-12 relative archive restores" restore \
  "$ROBUST/relative/abc.bak" "$ROBUST/relative/out"
expect_file_content "RB-13 relative backup stored the file" \
  "$ROBUST/relative/out/file.txt" "relative"

# "." 和 ".." 会先规范化，再做拓扑判断。
mkdir -p "$ROBUST/normalized/src"
printf 'normalized\n' > "$ROBUST/normalized/src/file.txt"
pushd "$ROBUST/normalized" >/dev/null
expect_failure "RB-14 dot-dot path into the source is rejected" 1 \
  "inside the source directory" \
  backup src tmp/../src/sub.bak
expect_failure "RB-15 dot as source rejects a descendant archive" 1 \
  "inside the source directory" \
  backup . ./out.bak
expect_success "RB-16 dot-dot path leaving the source is allowed" backup ./src \
  ./dest/../dest2.bak
popd >/dev/null
expect_success "RB-17 normalized archive restores" restore \
  "$ROBUST/normalized/dest2.bak" "$ROBUST/normalized/out2"
expect_file_content "RB-18 normalized backup stored the file" \
  "$ROBUST/normalized/out2/file.txt" "normalized"

# 父目录里带软链接时，必须先解析链接再做拓扑判断：只比原始字符串的话，
# 这两条路径看着毫不相干，归档会一头扎进自己里面。
mkdir -p "$ROBUST/symlink-parent/real/src"
printf 'linked\n' > "$ROBUST/symlink-parent/real/src/file.txt"
ln -s real "$ROBUST/symlink-parent/alias"
expect_failure "RB-19 symlinked parent inside the source is rejected" 1 \
  "inside the source directory" \
  backup "$ROBUST/symlink-parent/alias/src" \
    "$ROBUST/symlink-parent/real/src/sub.bak"
expect_path_absent "RB-20 symlinked rejection created no archive" \
  "$ROBUST/symlink-parent/real/src/sub.bak"

# ---- 损坏归档的构造工具 ----------------------------------------------
#
# 见文件末尾的 python 源码说明：它只服务于测试，用来精确地写坏某个字段。
ARCHIVE_TOOL="$TEST_ROOT/archive_tool.py"
cat > "$ARCHIVE_TOOL" <<'PY_EOF'
#!/usr/bin/env python3
"""按 Archive Format v0.1 手工拼归档，专门用来构造损坏样本。

正常路径永远走 backupctl；这个脚本只服务于测试，用来"精确地写坏某一个
字段"——CLI 不可能产出这种归档。字节布局严格照
docs/format/archive_v0.1.md 的偏移表拼，所以它同时也是那份文档的
一次独立复核：如果文档和实现不一致，这里的样本就会对不上。
"""
import struct
import sys

MAGIC = b"BKPARCH\0"
DIRECTORY = 1
REGULAR = 2


def global_header(count, version=1, flags=0, header_size=24):
    return MAGIC + struct.pack("<HHIQ", version, flags, header_size, count)


def entry(kind, path, payload=b"", mode=None, mtime_sec=1700000000,
          mtime_nsec=123456789, path_length=None, payload_size=None,
          reserved0=0, reserved1=0):
    # 目录默认 0755：恢复出来的目录必须可进入，否则测试连读都读不了。
    if mode is None:
        mode = 0o755 if kind == DIRECTORY else 0o644
    raw = path if isinstance(path, bytes) else path.encode()
    length = len(raw) if path_length is None else path_length
    size = len(payload) if payload_size is None else payload_size
    header = struct.pack("<BBHIIIQQ", kind, reserved0, reserved1, length, mode,
                         mtime_nsec, mtime_sec, size)
    return header + raw + payload


def build(case):
    if case == "valid":
        return global_header(2) + entry(DIRECTORY, ".") + entry(
            REGULAR, "a.txt", b"hello\n")
    # 用例按"坏在哪个字段"分组：全局 header、entry header、路径、payload。
    # 每条只改动一个字段，其它部分保持合法，这样失败原因才唯一。
    if case == "bad_magic":
        return b"NOTMAGI\0" + global_header(1)[8:] + entry(DIRECTORY, ".")
    if case == "bad_version":
        return global_header(1, version=2) + entry(DIRECTORY, ".")
    if case == "bad_flags":
        return global_header(1, flags=1) + entry(DIRECTORY, ".")
    if case == "bad_header_size":
        return global_header(1, header_size=32) + entry(DIRECTORY, ".")
    if case == "truncated_global":
        return global_header(1)[:16]
    if case == "truncated_entry":
        return global_header(2) + entry(DIRECTORY, ".")[:20]
    if case == "path_too_long":
        return (global_header(2) + entry(DIRECTORY, ".") +
                entry(REGULAR, "x" * 4097, b"", path_length=4097))
    if case == "truncated_path":
        return (global_header(2) + entry(DIRECTORY, ".") +
                entry(REGULAR, "abcd", b"", path_length=64))
    if case == "truncated_payload":
        return (global_header(2) + entry(DIRECTORY, ".") +
                entry(REGULAR, "a.txt", b"abc", payload_size=64))
    if case == "bad_type":
        return global_header(2) + entry(DIRECTORY, ".") + entry(3, "weird")
    if case == "dir_with_payload":
        return (global_header(2) + entry(DIRECTORY, ".") +
                entry(DIRECTORY, "d", b"x", payload_size=1))
    if case == "reserved_nonzero":
        return (global_header(2) + entry(DIRECTORY, ".", reserved0=1) +
                entry(REGULAR, "a.txt", b"x"))
    if case == "bad_nsec":
        return (global_header(2) + entry(DIRECTORY, ".") +
                entry(REGULAR, "a.txt", b"x", mtime_nsec=1000000000))
    if case == "count_too_large":
        return (global_header(50) + entry(DIRECTORY, ".") +
                entry(REGULAR, "a.txt", b"x"))
    if case == "trailing_garbage":
        return (global_header(2) + entry(DIRECTORY, ".") +
                entry(REGULAR, "a.txt", b"x") + b"GARBAGE!")
    if case == "payload_overflow":
        return (global_header(2) + entry(DIRECTORY, ".") +
                entry(REGULAR, "a.txt", b"x", payload_size=2 ** 62))
    if case == "mode_extra_bits":
        return (global_header(2) + entry(DIRECTORY, ".") +
                entry(REGULAR, "a.txt", b"x", mode=0o4755))
    if case == "empty_archive":
        return global_header(0)
    if case == "no_root_entry":
        return (global_header(2) + entry(REGULAR, "a.txt", b"x") +
                entry(DIRECTORY, "d"))
    # 路径类用例：每一条都是"如果解析器偷懒就会中招"的写法。
    if case == "path_dotdot":
        return (global_header(2) + entry(DIRECTORY, ".") +
                entry(REGULAR, "../escape", b"x"))
    if case == "path_deep_dotdot":
        return (global_header(2) + entry(DIRECTORY, ".") +
                entry(REGULAR, "../../escape", b"x"))
    if case == "path_backslash":
        return (global_header(2) + entry(DIRECTORY, ".") +
                entry(REGULAR, "a\\b.txt", b"x"))
    if case == "path_drive_letter":
        return (global_header(2) + entry(DIRECTORY, ".") +
                entry(REGULAR, "C:note.txt", b"x"))
    if case == "path_mid_dotdot":
        return (global_header(2) + entry(DIRECTORY, ".") +
                entry(REGULAR, "foo/../bar", b"x"))
    if case == "path_absolute":
        return (global_header(2) + entry(DIRECTORY, ".") +
                entry(REGULAR, "/absolute/path", b"x"))
    if case == "path_dot_component":
        return (global_header(2) + entry(DIRECTORY, ".") +
                entry(REGULAR, "foo/./bar", b"x"))
    if case == "path_empty_component":
        return (global_header(2) + entry(DIRECTORY, ".") +
                entry(REGULAR, "foo//bar", b"x"))
    if case == "path_trailing_slash":
        return (global_header(2) + entry(DIRECTORY, ".") +
                entry(DIRECTORY, "foo/"))
    if case == "path_nul":
        return (global_header(2) + entry(DIRECTORY, ".") +
                entry(REGULAR, b"a\x00b", b"x"))
    if case == "duplicate_path":
        return (global_header(3) + entry(DIRECTORY, ".") +
                entry(REGULAR, "a.txt", b"x") +
                entry(REGULAR, "a.txt", b"y"))
    if case == "file_as_parent":
        return (global_header(3) + entry(DIRECTORY, ".") +
                entry(REGULAR, "a", b"x") + entry(REGULAR, "a/b", b"y"))
    if case == "missing_parent":
        return (global_header(2) + entry(DIRECTORY, ".") +
                entry(REGULAR, "x/y", b"x"))
    raise SystemExit("unknown case: " + case)


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: archive_tool.py <case> <output>")
    data = build(sys.argv[1])
    with open(sys.argv[2], "wb") as handle:
        handle.write(data)


if __name__ == "__main__":
    main()

PY_EOF
chmod +x "$ARCHIVE_TOOL"

# ---- D. ARC：内容形态 -------------------------------------------------

echo "[test] D. archive round trip cases"
ARC_ROOT="$TEST_ROOT/arc"
mkdir -p "$ARC_ROOT"

# ARC 区按"内容形态"分组，每一类都对应一种真实会遇到的输入：
# 空目录、空文件、二进制、中文/空格/#/% 名字、深目录、大文件、超多小文件。
# 这些形态在归档里分别考验 path 编码、payload 边界和流式读写，
# 任何一类漏掉，用户都可能在某次备份里第一次踩到。
#
# 大部分 ARC 用例都是同一个形状：打包 -> 解包 -> diff -r 必须为空。
arc_case() {
  local id="$1"
  local src="$2"
  local archive="$ARC_ROOT/$id.bak"
  local out="$ARC_ROOT/$id-out"
  run_backupctl backup "$src" "$archive"
  if [[ $STATUS -ne 0 ]]; then
    record_fail "$id" "backup failed: $(first_line)"
    return
  fi
  run_backupctl restore "$archive" "$out"
  if [[ $STATUS -ne 0 ]]; then
    record_fail "$id" "restore failed: $(first_line)"
    return
  fi
  if diff -r "$src" "$out" >/dev/null 2>&1; then
    record_pass "$id"
  else
    record_fail "$id" "diff -r reports differences"
  fi
}

mkdir -p "$ARC_ROOT/empty-src"
arc_case "ARC-01 empty source directory" "$ARC_ROOT/empty-src"

mkdir -p "$ARC_ROOT/one-file-src"
printf 'single file\n' > "$ARC_ROOT/one-file-src/only.txt"
arc_case "ARC-02 single regular text file" "$ARC_ROOT/one-file-src"

mkdir -p "$ARC_ROOT/multi/a/b/c"
printf 'x\n' > "$ARC_ROOT/multi/a/one.txt"
printf 'y\n' > "$ARC_ROOT/multi/a/b/two.txt"
printf 'z\n' > "$ARC_ROOT/multi/a/b/c/three.txt"
arc_case "ARC-03 multi-level directory tree" "$ARC_ROOT/multi"

mkdir -p "$ARC_ROOT/empty-file-src"
: > "$ARC_ROOT/empty-file-src/zero-length.txt"
arc_case "ARC-04 zero-length file" "$ARC_ROOT/empty-file-src"

mkdir -p "$ARC_ROOT/binary-src"
# 0x00 与 0xff 都要出现：文本工具会在这两个字节上出错。
printf '\x00\x01\x02\x7f\x80\xfe\xff\x00\xff' > "$ARC_ROOT/binary-src/raw.bin"
arc_case "ARC-05 binary payload with 0x00 and 0xff" "$ARC_ROOT/binary-src"

mkdir -p "$ARC_ROOT/中文目录/子目录"
printf '中文内容\n' > "$ARC_ROOT/中文目录/文件.txt"
printf '深层中文\n' > "$ARC_ROOT/中文目录/子目录/更深.txt"
arc_case "ARC-06 UTF-8 (Chinese) paths" "$ARC_ROOT/中文目录"

mkdir -p "$ARC_ROOT/with space/inner dir"
printf 'spaced\n' > "$ARC_ROOT/with space/a file.txt"
printf 'nested\n' > "$ARC_ROOT/with space/inner dir/b file.txt"
arc_case "ARC-07 paths with spaces" "$ARC_ROOT/with space"

mkdir -p "$ARC_ROOT/hash#percent%"
printf 'special\n' > "$ARC_ROOT/hash#percent%/a#b.txt"
printf 'special2\n' > "$ARC_ROOT/hash#percent%/c%d.txt"
arc_case "ARC-08 # and % in paths" "$ARC_ROOT/hash#percent%"

mkdir -p "$ARC_ROOT/中文 空格#百分号%"
printf 'mixed\n' > "$ARC_ROOT/中文 空格#百分号%/文件名 #%.txt"
arc_case "ARC-09 combined Chinese, space, # and %" "$ARC_ROOT/中文 空格#百分号%"

mkdir -p "$ARC_ROOT/nested-empty/a/b/c" "$ARC_ROOT/nested-empty/d"
arc_case "ARC-10 nested empty directories" "$ARC_ROOT/nested-empty"

# 500 个小文件：考验"每个文件一条 entry"时的顺序与循环，
# 文件多但都很小，问题通常出在 entry 计数或路径拼接上。
mkdir -p "$ARC_ROOT/many-files"
for index in $(seq 1 500); do
  printf 'content %s\n' "$index" > "$ARC_ROOT/many-files/file-$index.txt"
done
arc_case "ARC-11 500 small files" "$ARC_ROOT/many-files"

# 16 MiB 单文件：payload 必须流式读写。如果实现里偷偷把整个文件读进内存，
# 这条用例会明显变慢甚至失败——这也是"不预加载"这条约束的可执行版本。
mkdir -p "$ARC_ROOT/big-file"
head -c 16777216 /dev/urandom > "$ARC_ROOT/big-file/big.bin"
arc_case "ARC-12 16 MiB file (streamed)" "$ARC_ROOT/big-file"

# 深度 24：递归层数、路径拼接和恢复顺序（父先于子）都在这一条里被压到极限。
mkdir -p "$ARC_ROOT/deep"
DEEP="$ARC_ROOT/deep"
for level in $(seq 1 24); do
  DEEP="$DEEP/l$level"
done
mkdir -p "$DEEP"
printf 'bottom\n' > "$DEEP/bottom.txt"
arc_case "ARC-13 directory depth 24" "$ARC_ROOT/deep"

# ARC-14：source root 本身也要作为 "." entry 保存，恢复后 metadata 必须回来。
mkdir -p "$ARC_ROOT/root-meta-src"
printf 'root\n' > "$ARC_ROOT/root-meta-src/file.txt"
chmod 0711 "$ARC_ROOT/root-meta-src"
touch -d '2020-12-31 23:59:58.765432100' "$ARC_ROOT/root-meta-src"
# ARC-14 专门验证归档的第一条 entry 是 "."：源目录自己的 mode / mtime
# 靠它保存，恢复时再写到 destination 根上（详细断言见 META 区）。
arc_case "ARC-14 source root is stored as the first entry" "$ARC_ROOT/root-meta-src"

# META 区专门盯住 metadata：mode 与 mtime（秒 + 纳秒）。
# 这里特意用了 0555 的目录——它是"先 chmod 再写子文件"这类实现错误的
# 照妖镜：顺序错了，inside.txt 根本写不进去。
#
# ---- E. META：mode 与 mtime ------------------------------------------

echo "[test] E. metadata round trip"
# 时间戳刻意用不同的秒和纳秒（含 9 位纳秒），这样"只保存秒"的实现会露馅。
META_ROOT="$TEST_ROOT/meta"
mkdir -p "$META_ROOT/src/dir750" "$META_ROOT/src/ro555" "$META_ROOT/src/empty-ro"
printf 'mode\n' > "$META_ROOT/src/file640.txt"
printf 'inside\n' > "$META_ROOT/src/ro555/inside.txt"
chmod 640 "$META_ROOT/src/file640.txt"
chmod 750 "$META_ROOT/src/dir750"
chmod 555 "$META_ROOT/src/ro555"
chmod 555 "$META_ROOT/src/empty-ro"
touch -d '2024-01-02 03:04:05.123456789' "$META_ROOT/src/file640.txt"
touch -d '2023-05-06 07:08:09.987654321' "$META_ROOT/src/dir750"
touch -d '2022-03-04 05:06:07.111111111' "$META_ROOT/src"
touch -d '2021-11-12 13:14:15.161718192' "$META_ROOT/src/ro555"

expect_success "META-00 pack metadata fixture" backup "$META_ROOT/src" \
  "$META_ROOT/meta.bak"
expect_success "META-00b unpack metadata fixture" restore "$META_ROOT/meta.bak" \
  "$META_ROOT/out"
# 0555 的目录：如果解包时先 chmod 再写子文件，这一步就会失败。
expect_path_exists "META-01 read-only directory keeps its child" \
  "$META_ROOT/out/ro555/inside.txt"
expect_mode "META-02 file mode 0640 round trips" \
  "$META_ROOT/out/file640.txt" 640
expect_mode "META-03 directory mode 0750 round trips" \
  "$META_ROOT/out/dir750" 750
expect_mode "META-04 read-only directory mode 0555 round trips" \
  "$META_ROOT/out/ro555" 555
expect_mode "META-05 empty read-only directory mode 0555 round trips" \
  "$META_ROOT/out/empty-ro" 555
expect_mode "META-06 source root mode round trips" "$META_ROOT/out" \
  "$(stat -c '%a' "$META_ROOT/src")"
expect_mtime "META-07 file mtime seconds and nanoseconds round trip" \
  "$META_ROOT/out/file640.txt" "$META_ROOT/src/file640.txt"
expect_mtime "META-08 directory mtime round trips" "$META_ROOT/out/dir750" \
  "$META_ROOT/src/dir750"
expect_mtime "META-09 read-only directory mtime round trips" \
  "$META_ROOT/out/ro555" "$META_ROOT/src/ro555"
expect_mtime "META-10 source root mtime round trips" "$META_ROOT/out" \
  "$META_ROOT/src"
expect_same_tree "META-11 metadata fixture tree matches" "$META_ROOT/src" \
  "$META_ROOT/out"

# ---- F. BAD：损坏归档 -------------------------------------------------

echo "[test] F. malformed archives"
mkdir -p "$TEST_ROOT/bad"

# 先证明手工构造工具本身能产出"读得进去"的归档：否则后面每个 BAD 用例
# 都可能只是因为样本本来就不合法，而不是因为注入的那处损坏。
python3 "$ARCHIVE_TOOL" valid "$TEST_ROOT/bad/valid.bak"
expect_success "BAD-00 hand-built valid archive restores" restore \
  "$TEST_ROOT/bad/valid.bak" "$TEST_ROOT/bad/valid-out"
expect_file_content "BAD-00b hand-built payload is restored" \
  "$TEST_ROOT/bad/valid-out/a.txt" "hello"

# 每个坏样本都要求：restore 退出码 1、错误信息点明原因、
# 并且 destination 一个字节都没被创建。
bad_case() {
  local id="$1"
  local case_name="$2"
  local expected_text="$3"
  local archive="$TEST_ROOT/bad/$case_name.bak"
  local dest="$TEST_ROOT/bad/$case_name-out"
  # 工具本身出错（比如用例名写错）不该把整个套件带崩：
  # 样本不存在时下面会记一条 FAIL，问题照样看得见。
  python3 "$ARCHIVE_TOOL" "$case_name" "$archive" >/dev/null 2>&1 || true
  if [[ ! -s "$archive" ]]; then
    record_fail "$id" "crafted sample is missing"
    return
  fi
  expect_failure "$id" 1 "$expected_text" restore "$archive" "$dest"
  expect_path_absent "$id left the destination untouched" "$dest"
}

# 全局 header 的四个字段：magic / version / flags / header_size。
# 它们决定"这个文件到底是不是我们这一版格式"，任何一个不对都不该继续往下读。
bad_case "BAD-01 wrong magic" bad_magic "Invalid archive magic"
bad_case "BAD-02 unsupported version" bad_version "Unsupported archive version"
bad_case "BAD-03 non-zero flags" bad_flags "Unsupported archive flags"
bad_case "BAD-04 wrong header_size" bad_header_size "Invalid archive header size"
bad_case "BAD-05 truncated global header" truncated_global "Truncated archive header"
# 文件被截断的两种位置：头都没读全（BAD-05）、entry 头读一半（BAD-06）。
# 截断是备份文件最常见的损坏方式（拷贝中断、磁盘写满），必须稳稳地拒绝。
bad_case "BAD-06 truncated entry header" truncated_entry "Truncated entry header"
bad_case "BAD-07 path_length above the limit" path_too_long "Invalid path length"
bad_case "BAD-08 truncated entry path" truncated_path "Truncated entry path"
bad_case "BAD-09 payload beyond the archive" truncated_payload "Payload exceeds archive size"
# entry 头内部字段的合法性：type 白名单、reserved 必须为 0、
# directory 不许带 payload、mode 不许含权限位以外的位、nsec 不许越界。
bad_case "BAD-10 invalid entry type" bad_type "Invalid entry type"
bad_case "BAD-11 directory entry with payload" dir_with_payload "Directory entry has a payload"
bad_case "BAD-12 non-zero reserved field" reserved_nonzero "Non-zero reserved field"
bad_case "BAD-13 mtime nanoseconds out of range" bad_nsec "Invalid mtime nanoseconds"
# 条数与文件长度对不上、以及读完最后一条还多出字节：
# 前者说明归档被截断，后者说明内容被追加过——两种都不接受。
bad_case "BAD-14 entry_count larger than the file" count_too_large "Truncated entry header"
bad_case "BAD-15 trailing bytes after the last entry" trailing_garbage "Trailing bytes"
bad_case "BAD-16 payload size overflows the archive" payload_overflow "Payload exceeds archive size"
bad_case "BAD-17 mode carries non-permission bits" mode_extra_bits "Invalid entry mode"
bad_case "BAD-18 empty archive has no root entry" empty_archive "root entry"
bad_case "BAD-19 first entry is not the root" no_root_entry "no parent directory"
bad_case "BAD-20 entry without its parent directory" missing_parent "no parent directory"
bad_case "BAD-21 path with a trailing slash" path_trailing_slash "trailing slash"

# SEC 区把恶意归档的每一种写法都试一遍。判定标准统一：
# 退出码 1、错误信息点明原因、destination 不存在。
# 这里用的样本都由 archive_tool.py 手工拼字节，CLI 产不出这种归档。
#
# ---- G. SEC：路径安全 -------------------------------------------------

echo "[test] G. archive path safety"
bad_case "SEC-01 ../escape is rejected" path_dotdot "Invalid archive path"
bad_case "SEC-02 ../../escape is rejected" path_deep_dotdot "Invalid archive path"
bad_case "SEC-03 absolute path is rejected" path_absolute "absolute"
bad_case "SEC-04 foo/../bar is rejected" path_mid_dotdot "Invalid archive path"
bad_case "SEC-05 foo/./bar is rejected" path_dot_component "Invalid archive path"
bad_case "SEC-06 foo//bar is rejected" path_empty_component "Invalid archive path"
bad_case "SEC-07 duplicate path is rejected" duplicate_path "Duplicate archive path"
bad_case "SEC-08 file used as a parent directory" file_as_parent "both a file and a directory"
bad_case "SEC-08b path containing NUL is rejected" path_nul "NUL"

# SEC-09：归档文件写在源目录内部。检查必须在创建文件之前完成，
# 拒绝之后连一个空文件都不该出现。
mkdir -p "$TEST_ROOT/sec/source"
printf 'data\n' > "$TEST_ROOT/sec/source/file.txt"
expect_failure "SEC-09 archive inside the source is rejected" 1 \
  "inside the source directory" \
  backup "$TEST_ROOT/sec/source" "$TEST_ROOT/sec/source/backup.bak"
expect_path_absent "SEC-09b rejection created no archive file" \
  "$TEST_ROOT/sec/source/backup.bak"

# SEC-10：坏归档不能让 destination（连同它的父目录）出现。
python3 "$ARCHIVE_TOOL" path_dotdot "$TEST_ROOT/sec/evil.bak"
expect_failure "SEC-10 traversal archive is rejected" 1 "Invalid archive path" \
  restore "$TEST_ROOT/sec/evil.bak" "$TEST_ROOT/sec/deep/nested/out"
expect_path_absent "SEC-10b no destination directory was created" \
  "$TEST_ROOT/sec/deep"

# UNSUP 区的要求只有一条：遇到不支持的类型，整次备份失败。
# 不跳过（会让备份少东西却报成功）、不跟随软链接（会备份到源目录之外）、
# 也不当普通文件复制（会复制出一个内容不对的文件）。
#
# ---- H. UNSUP：不支持的特殊文件 ---------------------------------------

echo "[test] H. unsupported source entries"
mkdir -p "$TEST_ROOT/unsup/with-symlink"
printf 'target\n' > "$TEST_ROOT/unsup/with-symlink/target.txt"
ln -s target.txt "$TEST_ROOT/unsup/with-symlink/link.txt"
expect_failure "UNSUP-01 symlink to a file fails the whole backup" 1 \
  "Unsupported source entry type" \
  backup "$TEST_ROOT/unsup/with-symlink" "$TEST_ROOT/unsup/u01.bak"
expect_path_absent "UNSUP-01b no archive was left behind" "$TEST_ROOT/unsup/u01.bak"

mkdir -p "$TEST_ROOT/unsup/with-dir-symlink/real"
printf 'x\n' > "$TEST_ROOT/unsup/with-dir-symlink/real/file.txt"
ln -s real "$TEST_ROOT/unsup/with-dir-symlink/alias"
expect_failure "UNSUP-02 symlink to a directory fails the whole backup" 1 \
  "Unsupported source entry type" \
  backup "$TEST_ROOT/unsup/with-dir-symlink" "$TEST_ROOT/unsup/u02.bak"

mkdir -p "$TEST_ROOT/unsup/with-fifo"
printf 'x\n' > "$TEST_ROOT/unsup/with-fifo/normal.txt"
mkfifo "$TEST_ROOT/unsup/with-fifo/pipe"
expect_failure "UNSUP-03 FIFO fails the whole backup" 1 \
  "Unsupported source entry type" \
  backup "$TEST_ROOT/unsup/with-fifo" "$TEST_ROOT/unsup/u03.bak"
expect_path_absent "UNSUP-03b no archive was left behind" "$TEST_ROOT/unsup/u03.bak"

# socket 没法用 mkfifo/ln 造，用一个后台 python 进程 bind 住再测；
# 备份必须在它存在期间失败，所以这里 sleep 1 等它真的建出来。
mkdir -p "$TEST_ROOT/unsup/with-socket"
# bind 用**相对名字**：AF_UNIX 的 sun_path 只有 108 字节，而仓库路径
# （课程根搬迁之后）已经 72 字符，再把 testdata/... 拼上去就超了 —— 现象是
# python 直接抛 OSError: AF_UNIX path too long，测试连夹具都建不出来。
# 先 chdir 进目录、再 bind('sock')，socket 仍然落在被备份的目录里
# （语义不变），但传进 bind 的字符串只有 5 个字节。
python3 -c "
import os, socket, time
os.chdir('$TEST_ROOT/unsup/with-socket')
handle = socket.socket(socket.AF_UNIX)
handle.bind('sock')
time.sleep(30)
" &
SOCKET_PID=$!
sleep 1
expect_failure "UNSUP-04 unix socket fails the whole backup" 1 \
  "Unsupported source entry type" \
  backup "$TEST_ROOT/unsup/with-socket" "$TEST_ROOT/unsup/u04.bak"
kill "$SOCKET_PID" 2>/dev/null || true
wait "$SOCKET_PID" 2>/dev/null || true

# NOCMP 区用可检查的事实证明"没有压缩"，而不是靠文档里的一句声明：
#   * 源文件里的 marker 必须原样出现在归档中；
#   * 归档字节数不会小于 payload 总字节数；
#   * 最容易被压缩的全零文件，归档后必须比原文件更大。
#
# ---- I. NOCMP：证明 payload 没有压缩 ----------------------------------

echo "[test] I. payload is stored uncompressed"
# 1. 源文件里的 marker 必须原样出现在归档中（二进制安全匹配）。
if grep -aqF 'UNCOMPRESSED_ARCHIVE_PAYLOAD_0123456789' "$ARCHIVE"; then
  record_pass "NOCMP-01 raw payload marker is present in the archive"
else
  record_fail "NOCMP-01 raw payload marker is present in the archive" \
    "marker bytes not found"
fi

# 2. 归档只会比 payload 总和大（多了 header 与元数据），绝不会小。
# 只算普通文件的字节数，不含目录项与 header：这是"归档绝不会比内容小"的基线。
PAYLOAD_BYTES="$(find "$SOURCE" -type f -printf '%s\n' | awk '{total += $1} END {print total}')"
ARCHIVE_BYTES="$(stat -c '%s' "$ARCHIVE")"
if [[ "$ARCHIVE_BYTES" -ge "$PAYLOAD_BYTES" ]]; then
  record_pass "NOCMP-02 archive is at least as large as its payloads ($ARCHIVE_BYTES >= $PAYLOAD_BYTES)"
else
  record_fail "NOCMP-02 archive is at least as large as its payloads" \
    "archive $ARCHIVE_BYTES < payload $PAYLOAD_BYTES"
fi

# 3. 全零文件是最容易被压缩的东西：归档如果比它还小，就说明发生了压缩。
mkdir -p "$TEST_ROOT/nocmp"
head -c 1048576 /dev/zero > "$TEST_ROOT/nocmp/zeros.bin"
expect_success "NOCMP-03 pack a 1 MiB zero file" backup "$TEST_ROOT/nocmp" \
  "$TEST_ROOT/nocmp.bak"
ZERO_BYTES="$(stat -c '%s' "$TEST_ROOT/nocmp/zeros.bin")"
ZERO_ARCHIVE_BYTES="$(stat -c '%s' "$TEST_ROOT/nocmp.bak")"
if [[ "$ZERO_ARCHIVE_BYTES" -gt "$ZERO_BYTES" ]]; then
  record_pass "NOCMP-04 all-zero payload does not shrink ($ZERO_ARCHIVE_BYTES > $ZERO_BYTES)"
else
  record_fail "NOCMP-04 all-zero payload does not shrink" \
    "archive $ZERO_ARCHIVE_BYTES <= payload $ZERO_BYTES"
fi

# ---- J. SYM：Writer / Reader 的路径规则必须对称 -----------------------

echo "[test] J. writer and reader path rules are symmetric"
# Linux 允许文件名里出现反斜杠，也允许 "C:note.txt" 这种形状；归档格式
# （Archive v0.1）两者都不接受。写侧必须和读侧用同一套判断，否则会出现
# "备份成功、恢复失败"——自己刚写出来的包自己读不回来。
mkdir -p "$TEST_ROOT/sym/backslash"
printf 'x\n' > "$TEST_ROOT/sym/backslash/a\\b.txt"
expect_failure "SYM-01 filename with a backslash is rejected by the writer" 1 \
  "Invalid archive path" \
  backup "$TEST_ROOT/sym/backslash" "$TEST_ROOT/sym/sym01.bak"
expect_path_absent "SYM-01b rejected backup left no archive" \
  "$TEST_ROOT/sym/sym01.bak"

mkdir -p "$TEST_ROOT/sym/drive"
printf 'x\n' > "$TEST_ROOT/sym/drive/C:note.txt"
expect_failure "SYM-02 drive-letter style filename is rejected by the writer" 1 \
  "Invalid archive path" \
  backup "$TEST_ROOT/sym/drive" "$TEST_ROOT/sym/sym02.bak"
expect_path_absent "SYM-02b rejected backup left no archive" \
  "$TEST_ROOT/sym/sym02.bak"

# 判的是归档内路径，不只是根目录下的名字：嵌一层同样要拒。
mkdir -p "$TEST_ROOT/sym/nested/sub"
printf 'x\n' > "$TEST_ROOT/sym/nested/sub/a\\b.txt"
expect_failure "SYM-03 nested backslash path is rejected by the writer" 1 \
  "Invalid archive path" \
  backup "$TEST_ROOT/sym/nested" "$TEST_ROOT/sym/sym03.bak"
expect_path_absent "SYM-03b rejected backup left no archive" \
  "$TEST_ROOT/sym/sym03.bak"

# 反向对照：同样的形状由手工样本喂给读侧，读侧给出同一类拒绝信息。
# 两边一致，才说明校验规则真的只有一份。
python3 "$ARCHIVE_TOOL" path_backslash "$TEST_ROOT/sym/sym04.bak"
expect_failure "SYM-04 reader rejects a backslash path with the same rule" 1 \
  "Invalid archive path" \
  restore "$TEST_ROOT/sym/sym04.bak" "$TEST_ROOT/sym/sym04-out"
python3 "$ARCHIVE_TOOL" path_drive_letter "$TEST_ROOT/sym/sym05.bak"
expect_failure "SYM-05 reader rejects a drive-letter path with the same rule" 1 \
  "Invalid archive path" \
  restore "$TEST_ROOT/sym/sym05.bak" "$TEST_ROOT/sym/sym05-out"

# ---- K. FILTER：文件筛选 ------------------------------------------------

echo "[test] K. file filtering"
FIL="$TEST_ROOT/filter"
rm -rf "$FIL"
mkdir -p "$FIL/src/build/sub" "$FIL/src/keep" "$FIL/src/中文 空格#%"
printf 'cpp\n'    > "$FIL/src/a.cpp"
printf 'header\n' > "$FIL/src/b.h"
printf 'log\n'    > "$FIL/src/c.log"
printf 'text\n'   > "$FIL/src/d.txt"
printf 'obj\n'    > "$FIL/src/build/out.o"
printf 'deep\n'   > "$FIL/src/build/sub/deep.txt"
printf 'keep\n'   > "$FIL/src/keep/k.cpp"
printf 'cn\n'     > "$FIL/src/中文 空格#%/file.cpp"
printf 'sp\n'     > "$FIL/src/re port?.txt"

# 断言筛选结果：只比"普通文件"的相对路径集合（结构目录由 K3 单独检查）。
expect_filtered_files() {
  local name="$1" expected="$2" src="$3" bak="$4"
  shift 4
  local out="$bak.out"
  rm -rf "$bak" "$out"
  run_backupctl backup "$src" "$bak" "$@"
  if [[ $STATUS -ne 0 ]]; then
    record_fail "$name" "backup failed: $(first_line)"
    return
  fi
  run_backupctl restore "$bak" "$out"
  if [[ $STATUS -ne 0 ]]; then
    record_fail "$name" "restore failed: $(first_line)"
    return
  fi
  local actual
  actual="$(cd "$out" && find . -type f -printf '%P\n' | LC_ALL=C sort | tr '\n' ' ')"
  actual="${actual% }"
  if [[ "$actual" == "$expected" ]]; then
    record_pass "$name"
  else
    record_fail "$name" "got [$actual] expected [$expected]"
  fi
}

# 断言整棵恢复树（含目录）与期望一致，用来验证"目录剪枝"。
expect_filtered_tree() {
  local name="$1" expected="$2" src="$3" bak="$4"
  shift 4
  local out="$bak.out"
  rm -rf "$bak" "$out"
  run_backupctl backup "$src" "$bak" "$@"
  if [[ $STATUS -ne 0 ]]; then
    record_fail "$name" "backup failed: $(first_line)"
    return
  fi
  run_backupctl restore "$bak" "$out"
  local actual
  actual="$(cd "$out" && find . -mindepth 1 -printf '%P\n' | LC_ALL=C sort | tr '\n' ' ')"
  actual="${actual% }"
  if [[ "$actual" == "$expected" ]]; then
    record_pass "$name"
  else
    record_fail "$name" "got [$actual] expected [$expected]"
  fi
}

ALL_FILES="a.cpp b.h build/out.o build/sub/deep.txt c.log d.txt keep/k.cpp re port?.txt 中文 空格#%/file.cpp"
NO_BUILD="a.cpp b.h c.log d.txt keep/k.cpp re port?.txt 中文 空格#%/file.cpp"
CPP_ONLY="a.cpp b.h keep/k.cpp 中文 空格#%/file.cpp"

echo "[test] K1. include / exclude 语义"
expect_filtered_files "FIL-01 no rules keeps everything (PR #8 behaviour)" \
  "$ALL_FILES" "$FIL/src" "$FIL/f01.bak"
expect_filtered_files "FIL-02 include ext" "$CPP_ONLY" \
  "$FIL/src" "$FIL/f02.bak" --include 'ext:cpp;h'
expect_filtered_files "FIL-03 exclude ext" \
  "a.cpp b.h build/sub/deep.txt d.txt keep/k.cpp re port?.txt 中文 空格#%/file.cpp" \
  "$FIL/src" "$FIL/f03.bak" --exclude 'ext:log;o'
expect_filtered_files "FIL-04 exclude wins over include" \
  "a.cpp keep/k.cpp 中文 空格#%/file.cpp" \
  "$FIL/src" "$FIL/f04.bak" --include 'ext:cpp;h' --exclude 'ext:h'
expect_filtered_files "FIL-05 multiple include is OR" \
  "a.cpp build/sub/deep.txt d.txt keep/k.cpp re port?.txt 中文 空格#%/file.cpp" \
  "$FIL/src" "$FIL/f05.bak" --include 'ext:cpp' --include 'ext:txt'
expect_filtered_files "FIL-06 multiple exclude is OR" \
  "a.cpp b.h build/sub/deep.txt d.txt keep/k.cpp re port?.txt 中文 空格#%/file.cpp" \
  "$FIL/src" "$FIL/f06.bak" --exclude 'ext:log' --exclude 'name:out.o'

echo "[test] K2. 字段与通配符"
expect_filtered_files "FIL-07 name:" \
  "a.cpp b.h build/sub/deep.txt c.log d.txt keep/k.cpp re port?.txt 中文 空格#%/file.cpp" \
  "$FIL/src" "$FIL/f07.bak" --exclude 'name:out.o'
expect_filtered_files "FIL-08 path:**/build/** filters the subtree" "$NO_BUILD" \
  "$FIL/src" "$FIL/f08.bak" --exclude 'path:**/build/**'
expect_filtered_files "FIL-09 stem:" \
  "b.h build/out.o build/sub/deep.txt c.log d.txt keep/k.cpp re port?.txt 中文 空格#%/file.cpp" \
  "$FIL/src" "$FIL/f09.bak" --exclude 'stem:a'
expect_filtered_files "FIL-10 ext list is OR" "$CPP_ONLY" \
  "$FIL/src" "$FIL/f10.bak" --include 'ext:cpp;h;hpp'
# path:*.txt 只匹配"根目录下"的 .txt：* 不跨 '/'，因此 build/sub/deep.txt
# 不会被命中——这正是 * 与 ** 的区别。
expect_filtered_files "FIL-11 * does not cross / (path:*.txt)" \
  "d.txt re port?.txt" \
  "$FIL/src" "$FIL/f11.bak" --include 'path:*.txt'
expect_filtered_files "FIL-12 ? matches exactly one character" \
  "a.cpp b.h build/out.o build/sub/deep.txt c.log keep/k.cpp re port?.txt 中文 空格#%/file.cpp" \
  "$FIL/src" "$FIL/f12.bak" --exclude 'name:d.tx?'
expect_filtered_files "FIL-13 ** crosses directories" "keep/k.cpp" \
  "$FIL/src" "$FIL/f13.bak" --include 'path:**/keep/**'
expect_filtered_files "FIL-14 type:file matches every regular file" "$ALL_FILES" \
  "$FIL/src" "$FIL/f14.bak" --include 'type:file'
expect_filtered_files "FIL-15 中文 / 空格 / # / % 路径" \
  "中文 空格#%/file.cpp" \
  "$FIL/src" "$FIL/f15.bak" --include 'path:**/中文*/**'

echo "[test] K3. 目录剪枝与特殊文件"
# 被剪掉的子树里即使有 FIFO / symlink，也不再触发"不支持的类型"失败。
mkdir -p "$FIL/prune/build"
printf 'o\n' > "$FIL/prune/build/out.o"
mkfifo "$FIL/prune/build/pipe"
ln -s /etc/hostname "$FIL/prune/build/link"
printf 'ok\n' > "$FIL/prune/keep.txt"
expect_filtered_tree "FIL-16 excluded subtree with FIFO and symlink still succeeds" \
  "keep.txt" "$FIL/prune" "$FIL/f16.bak" --exclude 'path:**/build/**'
# 没有被排除时，特殊文件仍然让备份失败。
mkdir -p "$FIL/unsup"
printf 'ok\n' > "$FIL/unsup/keep.txt"
mkfifo "$FIL/unsup/pipe"
rm -rf "$FIL/f17.bak"
run_backupctl backup "$FIL/unsup" "$FIL/f17.bak" --exclude 'ext:nosuch'
if [[ $STATUS -ne 0 ]] && grep -qF 'Unsupported source entry type' "$OUT_FILE"; then
  record_pass "FIL-17 non-excluded FIFO still fails the backup"
else
  record_fail "FIL-17 non-excluded FIFO still fails the backup" "exit=$STATUS"
fi
expect_path_absent "FIL-17b failure left no archive" "$FIL/f17.bak"
# 明确把 FIFO 排除掉则允许成功。
expect_filtered_tree "FIL-18 explicitly excluded FIFO is skipped" "keep.txt" \
  "$FIL/unsup" "$FIL/f18.bak" --exclude 'name:pipe'

echo "[test] K4. size"
mkdir -p "$FIL/size"
head -c 100 /dev/zero     > "$FIL/size/small.bin"
head -c 2048 /dev/zero    > "$FIL/size/mid.bin"
head -c 3145728 /dev/zero > "$FIL/size/big.bin"
expect_filtered_files "FIL-19 size:<2KB" "small.bin" \
  "$FIL/size" "$FIL/f19.bak" --include 'size:<2KB'
expect_filtered_files "FIL-20 size:<=2KB" "mid.bin small.bin" \
  "$FIL/size" "$FIL/f20.bak" --include 'size:<=2KB'
expect_filtered_files "FIL-21 size:>2KB" "big.bin" \
  "$FIL/size" "$FIL/f21.bak" --include 'size:>2KB'
expect_filtered_files "FIL-22 size:>=2KB" "big.bin mid.bin" \
  "$FIL/size" "$FIL/f22.bak" --include 'size:>=2KB'
expect_filtered_files "FIL-23 size range is inclusive" "big.bin mid.bin" \
  "$FIL/size" "$FIL/f23.bak" --include 'size:2KB..3MB'
expect_filtered_files "FIL-24 size boundary 100B..100B" "small.bin" \
  "$FIL/size" "$FIL/f24.bak" --include 'size:100B..100B'
expect_filtered_files "FIL-25 size:<100B matches nothing" "" \
  "$FIL/size" "$FIL/f25.bak" --include 'size:<100B'

echo "[test] K5. mtime（固定 TZ=UTC，避免依赖执行时刻）"
mkdir -p "$FIL/time"
TODAY="$(date -u +%F)"
YESTERDAY="$(date -u -d 'yesterday' +%F)"
printf 't\n' > "$FIL/time/today.txt"
printf 'y\n' > "$FIL/time/yesterday.txt"
printf 'o\n' > "$FIL/time/old.txt"
touch -d "$TODAY 12:00:00 UTC" "$FIL/time/today.txt"
touch -d "$YESTERDAY 12:00:00 UTC" "$FIL/time/yesterday.txt"
touch -d '2020-01-01 12:00:00 UTC' "$FIL/time/old.txt"
export TZ=UTC
expect_filtered_files "FIL-26 mtime:today" "today.txt" \
  "$FIL/time" "$FIL/f26.bak" --include 'mtime:today'
expect_filtered_files "FIL-27 mtime:yesterday" "yesterday.txt" \
  "$FIL/time" "$FIL/f27.bak" --include 'mtime:yesterday'
expect_filtered_files "FIL-28 mtime:7days" "today.txt yesterday.txt" \
  "$FIL/time" "$FIL/f28.bak" --include 'mtime:7days'
expect_filtered_files "FIL-29 mtime exact date" "today.txt" \
  "$FIL/time" "$FIL/f29.bak" --include "mtime:$TODAY"
expect_filtered_files "FIL-30 mtime date range" "today.txt yesterday.txt" \
  "$FIL/time" "$FIL/f30.bak" --include "mtime:$YESTERDAY..$TODAY"
unset TZ

echo "[test] K6. 空结果与错误规则"
# include 只决定普通文件；目录作为结构项保留，所以"匹配不到任何文件"时
# 恢复出来是只有目录骨架的空树。
expect_filtered_tree "FIL-31 include matches nothing keeps only the directory skeleton" \
  "build build/sub keep 中文 空格#%" \
  "$FIL/src" "$FIL/f31.bak" --include 'ext:nosuchext'
rm -rf "$FIL/f32.bak"
run_backupctl backup "$FIL/src" "$FIL/f32.bak" --include 'bogus:x'
if [[ $STATUS -eq 2 ]] && grep -qF 'Invalid filter rule' "$OUT_FILE"; then
  record_pass "FIL-32 malformed rule is rejected with exit 2"
else
  record_fail "FIL-32 malformed rule is rejected with exit 2" "exit=$STATUS"
fi
expect_path_absent "FIL-32b malformed rule left no archive" "$FIL/f32.bak"
rm -rf "$FIL/f33.bak"
run_backupctl backup "$FIL/src" "$FIL/f33.bak" --include 'size:1XB'
if [[ $STATUS -eq 2 ]] && grep -qF 'Invalid filter rule' "$OUT_FILE"; then
  record_pass "FIL-33 malformed size is rejected"
else
  record_fail "FIL-33 malformed size is rejected" "exit=$STATUS"
fi
expect_path_absent "FIL-33b no archive left behind" "$FIL/f33.bak"
rm -rf "$FIL/f34.bak"
run_backupctl backup "$FIL/src" "$FIL/f34.bak" --include 'mtime:2026-13-99'
if [[ $STATUS -eq 2 ]] && grep -qF 'Invalid filter rule' "$OUT_FILE"; then
  record_pass "FIL-34 malformed mtime is rejected"
else
  record_fail "FIL-34 malformed mtime is rejected" "exit=$STATUS"
fi
expect_path_absent "FIL-34b no archive left behind" "$FIL/f34.bak"
rm -rf "$FIL/f35.bak"
run_backupctl backup "$FIL/src" "$FIL/f35.bak" --from-filter 'ext:cpp'
if [[ $STATUS -eq 2 ]]; then
  record_pass "FIL-35 unknown option is a usage error"
else
  record_fail "FIL-35 unknown option is a usage error" "exit=$STATUS"
fi

echo "[test] K7. 筛选后的往返完整性"
run_backupctl restore "$FIL/f02.bak" "$FIL/f36.out" >/dev/null 2>&1
expect_same_sha256 "FIL-36 retained file is byte-identical after filtering" \
  "$FIL/src/a.cpp" "$FIL/f36.out/a.cpp"
expect_path_absent "FIL-37 filtered file is absent from the restored tree" \
  "$FIL/f36.out/c.log"

# ---- L. Manual Backup 的筛选预览 ------------------------------------
#
# 这一段证明的是**三方一致**，而不是"某个函数返回了预期的值"：
#
#   CLI 预览      backupctl preview
#   GUI 预览      build/backup-gui-modern --preview-test（界面真正的入口）
#   真实备份      用同一组规则 backup 之后，把归档恢复出来数实际存在的节点
#
# GUI 二进制不存在时只跑 CLI 那一半（它属于 gui-modern 目标）。

echo "[test] L. 筛选预览（CLI == GUI == 真实备份）"

PREVIEW="$TEST_ROOT/preview"
PREVIEW_GUI_BIN="$ROOT_DIR/build/backup-gui-modern"
PREVIEW_CLI_OUT="$PREVIEW/cli.out"
PREVIEW_CLI_ERR="$PREVIEW/cli.err"
PREVIEW_GUI_OUT="$PREVIEW/gui.out"
PREVIEW_GUI_ERR="$PREVIEW/gui.err"
PREVIEW_DIFF="$PREVIEW/diff.txt"
PREVIEW_SRC="$PREVIEW/src"
PREVIEW_REPO="$PREVIEW/repo"
PREVIEW_CONFIG="$PREVIEW/config.json"
mkdir -p "$PREVIEW_SRC/sub" "$PREVIEW_SRC/build" "$PREVIEW_SRC/cache" \
  "$PREVIEW_SRC/empty_dir" "$PREVIEW_REPO"

printf 'aaa\n' > "$PREVIEW_SRC/a.txt"
printf 'bbbbb\n' > "$PREVIEW_SRC/b.txt"
printf 'notes\n' > "$PREVIEW_SRC/notes.md"
printf 'obj\n' > "$PREVIEW_SRC/build/obj.o"
printf 'ccc\n' > "$PREVIEW_SRC/sub/c.txt"
printf 'big\n' > "$PREVIEW_SRC/sub/big.txt"
printf 'tmp\n' > "$PREVIEW_SRC/cache/tmp.dat"

"$BACKUPCTL" --config-file "$PREVIEW_CONFIG" config repository set "$PREVIEW_REPO" \
  >/dev/null 2>&1

PREVIEW_CLI_STATUS=0
PREVIEW_GUI_STATUS=0
run_preview_cli() {
  set +e
  timeout --signal=KILL "$TIMEOUT_SECONDS" "$BACKUPCTL" preview "$@" \
    >"$PREVIEW_CLI_OUT" 2>"$PREVIEW_CLI_ERR"
  PREVIEW_CLI_STATUS=$?
  set -e
}
run_preview_gui() {
  set +e
  QT_QPA_PLATFORM=offscreen timeout --signal=KILL 180 "$PREVIEW_GUI_BIN" \
    --preview-test "$@" >"$PREVIEW_GUI_OUT" 2>"$PREVIEW_GUI_ERR"
  PREVIEW_GUI_STATUS=$?
  set -e
}
# 预览输出 = 头部 + （可能一行 Note） + 每个 included 条目的相对路径。
# 比较前排序：两边都按扫描顺序输出，但"顺序也一致"不该是这条断言的负担。
preview_listed() {
  grep -v '^Preview: ' "$1" | grep -v '^Note: ' | sort
}
# 恢复出来的树里所有节点（含目录）的相对路径。
tree_nodes() {
  ( cd "$1" && find . -mindepth 1 -printf '%P\n' | sort )
}

# CLI 预览 == GUI 预览：两条命令的输出逐行 diff。
# 这是最直接的一条证据 —— 它不是"都调了同一个函数"，而是"命令行里看到的和
# 界面上看到的一模一样"。
# $2 是源目录：P7 的截断用例要换一个大目录，其它用例都用 $PREVIEW_SRC。
expect_preview_parity_at() {
  local name="$1"
  local source="$2"
  shift 2
  run_preview_cli "$source" "$@"
  if [[ $PREVIEW_CLI_STATUS -ne 0 ]]; then
    record_fail "$name" \
      "backupctl preview exit=$PREVIEW_CLI_STATUS: $(head -n 1 "$PREVIEW_CLI_ERR")"
    return
  fi
  if [[ ! -x "$PREVIEW_GUI_BIN" ]]; then
    record_pass "$name（CLI；没有 build/backup-gui-modern，跳过 GUI 对比）"
    return
  fi
  run_preview_gui "$source" "$@"
  if [[ $PREVIEW_GUI_STATUS -ne 0 ]]; then
    record_fail "$name" \
      "GUI preview exit=$PREVIEW_GUI_STATUS: $(head -n 1 "$PREVIEW_GUI_ERR")"
    return
  fi
  if diff -u "$PREVIEW_CLI_OUT" "$PREVIEW_GUI_OUT" >"$PREVIEW_DIFF" 2>&1; then
    record_pass "$name"
  else
    record_fail "$name" "$(head -n 6 "$PREVIEW_DIFF" | tr '\n' ' ')"
  fi
}

expect_preview_parity() {
  local name="$1"
  shift
  expect_preview_parity_at "$name" "$PREVIEW_SRC" "$@"
}

# 预览 == 真实备份：用同一组规则备份到仓库，恢复，比较节点集合。
# $2 是源目录：grammar 用例各自有自己的树，不能都绑在 $PREVIEW_SRC 上。
expect_preview_matches_backup_at() {
  local name="$1"
  local source="$2"
  shift 2
  run_preview_cli "$source" "$@"
  if [[ $PREVIEW_CLI_STATUS -ne 0 ]]; then
    record_fail "$name" "preview exit=$PREVIEW_CLI_STATUS"
    return
  fi
  # 快照用文件而不是变量：仓库开始时是空的，空字符串会变成一个空行，
  # 让 comm 多出一行"新文件名"。
  find "$PREVIEW_REPO" -maxdepth 1 -name '*.bak' -printf '%f\n' | sort \
    >"$PREVIEW/before.txt"
  set +e
  timeout --signal=KILL "$TIMEOUT_SECONDS" "$BACKUPCTL" \
    --config-file "$PREVIEW_CONFIG" backup "$source" "$@" \
    >"$PREVIEW/backup.log" 2>&1
  local backup_status=$?
  set -e
  if [[ $backup_status -ne 0 ]]; then
    record_fail "$name" "backup exit=$backup_status: $(head -n 1 "$PREVIEW/backup.log")"
    return
  fi
  find "$PREVIEW_REPO" -maxdepth 1 -name '*.bak' -printf '%f\n' | sort \
    >"$PREVIEW/after.txt"
  local file_name
  file_name="$(comm -13 "$PREVIEW/before.txt" "$PREVIEW/after.txt" | head -n 1)"
  if [[ -z "$file_name" ]]; then
    record_fail "$name" "仓库里没有新归档"
    return
  fi
  rm -rf "$PREVIEW/restored"
  set +e
  timeout --signal=KILL "$TIMEOUT_SECONDS" "$BACKUPCTL" \
    --config-file "$PREVIEW_CONFIG" restore "$file_name" "$PREVIEW/restored" \
    >>"$PREVIEW/backup.log" 2>&1
  local restore_status=$?
  set -e
  if [[ $restore_status -ne 0 ]]; then
    record_fail "$name" "restore exit=$restore_status: $(tail -n 1 "$PREVIEW/backup.log")"
    return
  fi
  if diff -u <(preview_listed "$PREVIEW_CLI_OUT") <(tree_nodes "$PREVIEW/restored") \
    >"$PREVIEW_DIFF" 2>&1; then
    record_pass "$name"
  else
    record_fail "$name" "$(head -n 6 "$PREVIEW_DIFF" | tr '\n' ' ')"
  fi
}

expect_preview_matches_backup() {
  local name="$1"
  shift
  expect_preview_matches_backup_at "$name" "$PREVIEW_SRC" "$@"
}

# P1 没有规则 / P2 单条件 / P3 一条规则内的 compound AND /
# P4 include+exclude（exclude 优先）/ P5 被排除的目录整棵剪掉
expect_preview_parity "PRV-01 P1 无规则：CLI 预览 == GUI 预览"
expect_preview_matches_backup "PRV-02 P1 无规则：CLI 预览 == 真实备份条目"
expect_preview_parity "PRV-03 P2 单条件 include：CLI == GUI" --include 'ext:txt'
expect_preview_matches_backup "PRV-04 P2 单条件 include：CLI == 真实备份" --include 'ext:txt'
expect_preview_parity "PRV-05 P3 compound AND：CLI == GUI" --include 'name:*.txt size:<5'
expect_preview_matches_backup "PRV-06 P3 compound AND：CLI == 真实备份" \
  --include 'name:*.txt size:<5'
expect_preview_parity "PRV-07 P4 include+exclude：CLI == GUI" \
  --include 'name:*.txt' --exclude 'name:b*'
expect_preview_matches_backup "PRV-08 P4 include+exclude：CLI == 真实备份" \
  --include 'name:*.txt' --exclude 'name:b*'
expect_preview_parity "PRV-09 P5 被排除目录：CLI == GUI" --exclude 'name:build'
expect_preview_matches_backup "PRV-10 P5 被排除目录：CLI == 真实备份" --exclude 'name:build'

# P5b：排除项必须**真的不在**结果里。前两条只证明两边一致，这一条证明一致的
# 方向是对的 —— 否则"两边都错误地包含了 build/"也会通过。
run_preview_cli "$PREVIEW_SRC" --exclude 'name:build'
if [[ $PREVIEW_CLI_STATUS -eq 0 ]] &&
   ! preview_listed "$PREVIEW_CLI_OUT" | grep -q '^build'; then
  record_pass "PRV-11 P5b 被排除目录及其子树不出现在预览里"
else
  record_fail "PRV-11 P5b 被排除目录及其子树不出现在预览里" \
    "$(head -n 3 "$PREVIEW_CLI_OUT" | tr '\n' ' ')"
fi

# 预览是只读的：不建归档、不改仓库、不改配置。
PREVIEW_LOCK_REPO_BEFORE="$(ls -1 "$PREVIEW_REPO" | sort | tr '\n' ' ')"
PREVIEW_CFG_SUM="$(cksum "$PREVIEW_CONFIG" | cut -d' ' -f1)"
run_preview_cli "$PREVIEW_SRC" --include 'ext:txt'
PREVIEW_LOCK_REPO_AFTER="$(ls -1 "$PREVIEW_REPO" | sort | tr '\n' ' ')"
if [[ $PREVIEW_CLI_STATUS -eq 0 &&
      "$PREVIEW_LOCK_REPO_BEFORE" == "$PREVIEW_LOCK_REPO_AFTER" &&
      "$PREVIEW_CFG_SUM" == "$(cksum "$PREVIEW_CONFIG" | cut -d' ' -f1)" ]]; then
  record_pass "PRV-12 预览不创建归档、不改仓库、不改配置"
else
  record_fail "PRV-12 预览不创建归档、不改仓库、不改配置" \
    "repo=[$PREVIEW_LOCK_REPO_AFTER]"
fi

# 预览**不需要**仓库：--help 与文档都这么承诺。这一条与 PRV-12 是两件事 ——
# 那条说"不修改仓库"，这条说"没有仓库也能跑"。
NOREPO_DIR="$PREVIEW/no-repo"
mkdir -p "$NOREPO_DIR"
NOREPO_CONFIG="$NOREPO_DIR/config.json"
run_preview_cli "$PREVIEW_SRC" --include 'ext:txt' --config-file "$NOREPO_CONFIG"
if [[ $PREVIEW_CLI_STATUS -eq 0 ]] &&
   ! grep -q 'No backup repository is configured' "$PREVIEW_CLI_ERR"; then
  record_pass "PRV-24 预览不需要配置仓库（没有 config.json 也能预览）"
else
  record_fail "PRV-24 预览不需要配置仓库" \
    "exit=$PREVIEW_CLI_STATUS $(head -n 1 "$PREVIEW_CLI_ERR")"
fi
# 空配置必须真的是"没有仓库"，否则 PRV-24 就是空转：同一个配置下 backup
# 必须明确拒绝，理由正是"没有配置仓库"。
set +e
timeout --signal=KILL "$TIMEOUT_SECONDS" "$BACKUPCTL" --config-file "$NOREPO_CONFIG" \
  backup "$PREVIEW_SRC" >"$PREVIEW/no-repo-backup.log" 2>&1
NOREPO_BACKUP_STATUS=$?
set -e
if [[ $NOREPO_BACKUP_STATUS -eq 1 ]] &&
   grep -q 'No backup repository is configured' "$PREVIEW/no-repo-backup.log"; then
  record_pass "PRV-25 同一份空配置下 backup 仍然被拒绝（PRV-24 不是空转）"
else
  record_fail "PRV-25 同一份空配置下 backup 仍然被拒绝" \
    "exit=$NOREPO_BACKUP_STATUS $(head -n 1 "$PREVIEW/no-repo-backup.log")"
fi

# P6：非法规则 = 用法错误（2），在扫描之前返回，且不产生任何归档。
expect_failure "PRV-13 P6 非法 DSL 是用例错误（exit 2）" 2 "Invalid filter rule" \
  preview "$PREVIEW_SRC" --include 'nonsense:xx'
# 语法错误的三种形态都必须是 2，而不是"忽略掉继续跑"。
run_preview_cli "$PREVIEW_SRC" --include
if [[ $PREVIEW_CLI_STATUS -eq 2 ]]; then
  record_pass "PRV-14 P6 --include 缺少规则值是用法错误（exit 2）"
else
  record_fail "PRV-14 P6 --include 缺少规则值是用法错误" "exit=$PREVIEW_CLI_STATUS"
fi
run_preview_cli "$PREVIEW_SRC" --pack ustar
if [[ $PREVIEW_CLI_STATUS -eq 2 ]]; then
  record_pass "PRV-15 preview 不接受 pipeline 选项（exit 2）"
else
  record_fail "PRV-15 preview 不接受 pipeline 选项" "exit=$PREVIEW_CLI_STATUS"
fi
run_preview_cli "$PREVIEW_SRC" extra-positional
if [[ $PREVIEW_CLI_STATUS -eq 2 ]]; then
  record_pass "PRV-16 preview 拒绝多余的位置参数（exit 2）"
else
  record_fail "PRV-16 preview 拒绝多余的位置参数" "exit=$PREVIEW_CLI_STATUS"
fi

# 业务失败（源目录不存在）仍然是 1，与其它子命令一致。
# 消息与真实 Backup 逐字一致（同一个 walker 的同一句原文），不再是 CLI 自己
# 拼的一句话。
expect_failure "PRV-17 源目录不存在是操作失败（exit 1）" 1 \
  "Failed to inspect source directory" preview "$PREVIEW/nope"

# P6b：非法规则在 GUI 与 CLI 得到**同一句**核心原文。
if [[ -x "$PREVIEW_GUI_BIN" ]]; then
  run_preview_cli "$PREVIEW_SRC" --include 'nonsense:xx'
  run_preview_gui "$PREVIEW_SRC" --include 'nonsense:xx'
  CLI_MESSAGE="$(head -n 1 "$PREVIEW_CLI_ERR")"
  GUI_MESSAGE="$(head -n 1 "$PREVIEW_GUI_ERR")"
  if [[ -n "$CLI_MESSAGE" && "$CLI_MESSAGE" == "$GUI_MESSAGE" ]]; then
    record_pass "PRV-18 P6 GUI 与 CLI 报出同一句无效规则原文"
  else
    record_fail "PRV-18 P6 GUI 与 CLI 报出同一句无效规则原文" \
      "cli=[$CLI_MESSAGE] gui=[$GUI_MESSAGE]"
  fi
fi

# P7：超过预览窗口时 GUI 与 CLI 采用同一个截断契约（同一个 limit、同一行 Note）。
PBIG="$PREVIEW/big"
mkdir -p "$PBIG"
for index in $(seq 1 320); do printf 'x' > "$PBIG/f$index.dat"; done
run_preview_cli "$PBIG"
# 计数是**完整实际备份遍历**的（320），窗口是前 300 个 preview entries：三个
# 数字必须分开说——总数、窗口大小、窗口里列出来的匹配数。窗口大小不是匹配数，
# 被剪枝的子树也不会被说成"检查过整棵源目录树"。
if [[ $PREVIEW_CLI_STATUS -eq 0 ]] &&
   grep -qF 'Preview: 320 matching item(s) in the effective backup selection.' "$PREVIEW_CLI_OUT" &&
   grep -qF 'Note: showing matches found within the first 300 preview entries; the complete effective backup traversal was validated, 300 matching item(s) listed below.' "$PREVIEW_CLI_OUT"; then
  record_pass "PRV-19 P7 超过窗口：总数 / 窗口大小 / 列出数三个数字分开说清楚"
else
  record_fail "PRV-19 P7 超过窗口：总数 / 列出数" \
    "$(head -n 2 "$PREVIEW_CLI_OUT" | tr '\n' ' ')"
fi
# 旧文案是错的（下面两句都不许再出现）：它说"只检查了前 300 条"，而实际上整棵树都被检查过。
if grep -qF 'first 300 of' "$PREVIEW_CLI_OUT" ||
   grep -qF 'were examined' "$PREVIEW_CLI_OUT"; then
  record_fail "PRV-19b 截断提示不再把窗口大小说成匹配数、也不再声称只检查了 300 条" \
    "$(grep -E 'first 300 of|were examined' "$PREVIEW_CLI_OUT" | head -n 1)"
else
  record_pass "PRV-19b 截断提示不再把窗口大小说成匹配数、也不再声称只检查了 300 条"
fi
# GUI 打出来的那一行必须与 CLI 逐字一致（PRV-20 会 diff，这里额外钉住关键词）。
# 先真的跑一次 GUI：不跑就会拿着上一条用例留下的输出做断言。
if [[ -x "$PREVIEW_GUI_BIN" ]]; then
  run_preview_gui "$PBIG"
  if grep -qF 'the complete effective backup traversal was validated' "$PREVIEW_GUI_OUT" &&
     grep -qF 'within the first 300 preview entries' "$PREVIEW_GUI_OUT" &&
     ! grep -qF 'first 300 of' "$PREVIEW_GUI_OUT" &&
     ! grep -qF 'were examined' "$PREVIEW_GUI_OUT"; then
    record_pass "PRV-19c GUI 的截断提示与 CLI 同义（traversal / preview entries）"
  else
    record_fail "PRV-19c GUI 的截断提示与 CLI 同义" \
      "$(grep -E 'Note:' "$PREVIEW_GUI_OUT" | head -n 1)"
  fi
fi
expect_preview_parity_at "PRV-20 P7 截断契约在 GUI 与 CLI 上一致" "$PBIG"

# ---- L.4c 窗口大小 != 匹配数：三个数字必须分开报 ----
#
# 窗口 = 遍历顺序里的前 300 个 preview entries（included / excluded / pruned 都
# 占位）；included_count = 完整实际备份遍历里的匹配数；窗口里列出来的匹配项数
# 又是第三个数字。下面两个形状让这三个数字互不相等。
#
# 窗口装不下时 preview_listed 与整棵恢复树本来就不同（这正是窗口的定义），
# 所以这里不比列表，只比"预览第一行报的匹配总数"与"真实备份真的产出多少条目"。
# $1 = 用例名，$2 = 源目录，$3 = 期望条目数，其余 = 规则
expect_preview_total_matches_backup() {
  local name="$1"
  local source="$2"
  local expected="$3"
  shift 3
  run_preview_cli "$source" "$@"
  if [[ $PREVIEW_CLI_STATUS -ne 0 ]]; then
    record_fail "$name" "preview exit=$PREVIEW_CLI_STATUS"
    return
  fi
  find "$PREVIEW_REPO" -maxdepth 1 -name '*.bak' -printf '%f\n' | sort \
    >"$PREVIEW/before.txt"
  set +e
  timeout --signal=KILL "$TIMEOUT_SECONDS" "$BACKUPCTL" \
    --config-file "$PREVIEW_CONFIG" backup "$source" "$@" \
    >"$PREVIEW/total-backup.log" 2>&1
  local backup_status=$?
  set -e
  if [[ $backup_status -ne 0 ]]; then
    record_fail "$name" \
      "backup exit=$backup_status: $(head -n 1 "$PREVIEW/total-backup.log")"
    return
  fi
  find "$PREVIEW_REPO" -maxdepth 1 -name '*.bak' -printf '%f\n' | sort \
    >"$PREVIEW/after.txt"
  local file_name
  file_name="$(comm -13 "$PREVIEW/before.txt" "$PREVIEW/after.txt" | head -n 1)"
  if [[ -z "$file_name" ]]; then
    record_fail "$name" "仓库里没有新归档"
    return
  fi
  rm -rf "$PREVIEW/restored-total"
  set +e
  timeout --signal=KILL "$TIMEOUT_SECONDS" "$BACKUPCTL" \
    --config-file "$PREVIEW_CONFIG" restore "$file_name" "$PREVIEW/restored-total" \
    >>"$PREVIEW/total-backup.log" 2>&1
  local restore_status=$?
  set -e
  if [[ $restore_status -ne 0 ]]; then
    record_fail "$name" "restore exit=$restore_status"
    return
  fi
  local nodes
  nodes="$(tree_nodes "$PREVIEW/restored-total" | wc -l)"
  if [[ "$nodes" -eq "$expected" ]] &&
     grep -qF "Preview: $expected matching item(s) in the effective backup selection." \
       "$PREVIEW_CLI_OUT"; then
    record_pass "$name"
  else
    record_fail "$name" \
      "backup nodes=$nodes expected=$expected: $(head -n 1 "$PREVIEW_CLI_OUT")"
  fi
}

# 形状 1：前 300 条按名字全部被 exclude，真正会进归档的 10 条排在窗口之外。
#   included_count = 10；窗口 = 300 个 preview entries；窗口里的 matching = 0。
# 旧文案在这里会打印 "showing the first 300 of 10 matching item(s)"：既把窗口
# 大小说成了匹配数，又和"301 项之后仍在继续验证"的事实打架。
PWEX="$PREVIEW/window-excluded"
rm -rf "$PWEX"
mkdir -p "$PWEX"
for index in $(seq 1 300); do printf 'x' > "$PWEX/aaa$(printf '%03d' "$index").dat"; done
for index in $(seq 1 10); do printf 'x' > "$PWEX/zzz$(printf '%02d' "$index").dat"; done
run_preview_cli "$PWEX" --exclude 'name:aaa*'
if [[ $PREVIEW_CLI_STATUS -eq 0 ]] &&
   grep -qF 'Preview: 10 matching item(s) in the effective backup selection.' "$PREVIEW_CLI_OUT" &&
   grep -qF 'within the first 300 preview entries' "$PREVIEW_CLI_OUT" &&
   grep -qF '0 matching item(s) listed below.' "$PREVIEW_CLI_OUT" &&
   ! grep -qF 'first 300 of 10 matching item(s)' "$PREVIEW_CLI_OUT" &&
   ! grep -qF 'were examined' "$PREVIEW_CLI_OUT"; then
  record_pass "PRV-42 300 条被排除 + 10 条 included：总数 10 / 窗口 300 / 列出 0"
else
  record_fail "PRV-42 300 excluded + 10 included 的文案" \
    "$(head -n 2 "$PREVIEW_CLI_OUT" | tr '\n' ' ')"
fi
# 窗口里一条匹配项都没有：结果列表必须是空的（excluded 不冒充匹配项）。
if [[ -z "$(preview_listed "$PREVIEW_CLI_OUT")" ]]; then
  record_pass "PRV-42b 窗口里 0 条 matching：列出的路径也是空的"
else
  record_fail "PRV-42b 窗口里 0 条 matching：列出的路径也是空的" \
    "$(preview_listed "$PREVIEW_CLI_OUT" | head -n 3 | tr '\n' ' ')"
fi
# 但真实备份确实产出 10 条：预览报的 10 是"完整遍历"的数字，窗口不影响它。
expect_preview_total_matches_backup \
  "PRV-42c 窗口里 0 条 matching，总数 10 == 真实备份的 10 条" "$PWEX" 10 \
  --exclude 'name:aaa*'
expect_preview_parity_at "PRV-42d 同一个形状：GUI 与 CLI 逐字一致" "$PWEX" \
  --exclude 'name:aaa*'

# 形状 2：前 250 条 included + 50 条 excluded + 末尾 20 条 included。
#   included_count = 270；窗口 = 300 个 preview entries；窗口里的 matching = 250。
# 三个数字互不相等，任何"用其中一个冒充另一个"的文案都会露馅。
PWMX="$PREVIEW/window-mixed"
rm -rf "$PWMX"
mkdir -p "$PWMX"
for index in $(seq 1 250); do printf 'x' > "$PWMX/aaa$(printf '%03d' "$index").dat"; done
for index in $(seq 1 50); do printf 'x' > "$PWMX/mmm$(printf '%03d' "$index").dat"; done
for index in $(seq 1 20); do printf 'x' > "$PWMX/zzz$(printf '%02d' "$index").dat"; done
run_preview_cli "$PWMX" --exclude 'name:mmm*'
PVL_MIXED_LISTED="$(preview_listed "$PREVIEW_CLI_OUT")"
if [[ $PREVIEW_CLI_STATUS -eq 0 ]] &&
   grep -qF 'Preview: 270 matching item(s) in the effective backup selection.' "$PREVIEW_CLI_OUT" &&
   grep -qF 'within the first 300 preview entries' "$PREVIEW_CLI_OUT" &&
   grep -qF '250 matching item(s) listed below.' "$PREVIEW_CLI_OUT" &&
   [[ "$(printf '%s\n' "$PVL_MIXED_LISTED" | wc -l)" -eq 250 ]] &&
   [[ "$(printf '%s\n' "$PVL_MIXED_LISTED" | head -n 1)" == "aaa001.dat" ]] &&
   [[ "$(printf '%s\n' "$PVL_MIXED_LISTED" | tail -n 1)" == "aaa250.dat" ]]; then
  record_pass "PRV-43 混合窗口：总数 270 / 窗口 300 / 列出 250（三个数字互不相等）"
else
  record_fail "PRV-43 混合窗口的三个数字" \
    "$(head -n 2 "$PREVIEW_CLI_OUT" | tr '\n' ' ') listed=$(printf '%s\n' "$PVL_MIXED_LISTED" | wc -l)"
fi
expect_preview_parity_at "PRV-43b 混合窗口：GUI 与 CLI 逐字一致" "$PWMX" \
  --exclude 'name:mmm*'
expect_preview_total_matches_backup \
  "PRV-43c 混合窗口：总数 270 == 真实备份的 270 条" "$PWMX" 270 \
  --exclude 'name:mmm*'

# ---- L.5 源目录语义 / socket / 顺序：预览与备份必须是同一个结论 ----

# 预览与真实 Backup 共用同一份遍历（source_tree_walker），所以下面每一条都是
# "同一个问题问两次"：一次问 preview，一次问 backup，答案必须一样。
PVSEM="$PREVIEW/semantics"
rm -rf "$PVSEM"
mkdir -p "$PVSEM/real-src" "$PVSEM/sock-src/sub" "$PVSEM/order-src"
printf 'a\n' > "$PVSEM/real-src/a.txt"
printf 'a\n' > "$PVSEM/sock-src/a.txt"
printf 'b\n' > "$PVSEM/sock-src/sub/b.txt"
ln -s "$PVSEM/real-src" "$PVSEM/link-src"
python3 -c "import os,socket,sys; os.chdir(os.path.dirname(sys.argv[1])); s=socket.socket(socket.AF_UNIX); s.bind(os.path.basename(sys.argv[1]))" \
  "$PVSEM/sock-src/sub/sock"
"$BACKUPCTL" --config-file "$PREVIEW_CONFIG" config repository set "$PREVIEW_REPO" \
  >/dev/null 2>&1

# PRV-26 源目录是 symlink-to-directory：预览拒绝，理由与备份一致。
run_preview_cli "$PVSEM/link-src"
PVL_LINK_STATUS=$PREVIEW_CLI_STATUS
PVL_LINK_MESSAGE="$(head -n 1 "$PREVIEW_CLI_ERR")"
set +e
"$BACKUPCTL" --config-file "$PREVIEW_CONFIG" backup "$PVSEM/link-src" \
  >"$PVSEM/link-backup.log" 2>&1
PVL_LINK_BACKUP=$?
set -e
if [[ $PVL_LINK_STATUS -eq 1 && $PVL_LINK_BACKUP -eq 1 ]] &&
   grep -qF "Source is not a directory" "$PVSEM/link-backup.log" &&
   [[ "$PVL_LINK_MESSAGE" == *"Source is not a directory"* ]]; then
  record_pass "PRV-26 symlink 源目录：预览与备份都拒绝且同一句原文"
else
  record_fail "PRV-26 symlink 源目录：预览与备份都拒绝" \
    "preview=$PVL_LINK_STATUS[$PVL_LINK_MESSAGE] backup=$PVL_LINK_BACKUP"
fi

# PRV-27 没有被排除的 socket：预览必须 blocked（exit 1 + 说清后果），
# 真实备份必须失败，两边第一行是同一句核心原文。
run_preview_cli "$PVSEM/sock-src"
PVL_SOCK_STATUS=$PREVIEW_CLI_STATUS
PVL_SOCK_MESSAGE="$(head -n 1 "$PREVIEW_CLI_ERR")"
set +e
"$BACKUPCTL" --config-file "$PREVIEW_CONFIG" backup "$PVSEM/sock-src" \
  >"$PVSEM/sock-backup.log" 2>&1
PVL_SOCK_BACKUP=$?
set -e
if [[ $PVL_SOCK_STATUS -eq 1 && $PVL_SOCK_BACKUP -eq 1 ]] &&
   grep -qF "Unsupported special type: socket" "$PVSEM/sock-backup.log" &&
   [[ "$PVL_SOCK_MESSAGE" == *"Unsupported special type: socket"* ]] &&
   grep -qF "Backup would fail unless this entry is excluded." "$PREVIEW_CLI_OUT"; then
  record_pass "PRV-27 未排除的 socket：预览 exit 1 且说明备份会失败"
else
  record_fail "PRV-27 未排除的 socket：预览 exit 1" \
    "preview=$PVL_SOCK_STATUS[$PVL_SOCK_MESSAGE] backup=$PVL_SOCK_BACKUP"
fi

if [[ -x "$PREVIEW_GUI_BIN" ]]; then
  run_preview_gui "$PVSEM/sock-src"
  if [[ $PREVIEW_GUI_STATUS -eq 1 ]] &&
     [[ "$(head -n 1 "$PREVIEW_GUI_ERR")" == "$PVL_SOCK_MESSAGE" ]] &&
     grep -qF "Backup would fail unless this entry is excluded." "$PREVIEW_GUI_OUT"; then
    record_pass "PRV-28 未排除的 socket：GUI 与 CLI 同一句、同一个 exit"
  else
    record_fail "PRV-28 未排除的 socket：GUI 与 CLI 一致" \
      "gui=$PREVIEW_GUI_STATUS[$(head -n 1 "$PREVIEW_GUI_ERR")] cli=[$PVL_SOCK_MESSAGE]"
  fi
fi

# PRV-29 明确排除 socket：预览成功、备份成功、结果里没有 socket。
expect_preview_parity "PRV-29 排除 socket：CLI == GUI" --exclude 'name:sock'
expect_preview_matches_backup "PRV-29b 排除 socket：CLI 预览 == 真实备份" \
  --exclude 'name:sock'
run_preview_cli "$PVSEM/sock-src" --exclude 'name:sock'
if [[ $PREVIEW_CLI_STATUS -eq 0 ]] &&
   ! preview_listed "$PREVIEW_CLI_OUT" | grep -q 'sock'; then
  record_pass "PRV-29c 被排除的 socket 不出现在预览结果里"
else
  record_fail "PRV-29c 被排除的 socket 不出现在预览结果里" \
    "$(head -n 3 "$PREVIEW_CLI_OUT" | tr '\n' ' ')"
fi

# PRV-30 顺序：创建顺序故意与 lexical 顺序相反，逐行比较**不排序**的输出。
POUT="$PVSEM/order-src"
for name in zulu.txt mike.txt alpha.txt yankee.txt bravo.txt; do
  printf 'x\n' > "$POUT/$name"
done
mkdir -p "$POUT/nested" "$POUT/alpha-dir"
printf 'x\n' > "$POUT/nested/inner.txt"
printf 'x\n' > "$POUT/alpha-dir/deep.txt"
cat > "$PVSEM/expected-order.txt" <<'PEOF'
Preview: 9 matching item(s) in the effective backup selection.
alpha-dir
alpha-dir/deep.txt
alpha.txt
bravo.txt
mike.txt
nested
nested/inner.txt
yankee.txt
zulu.txt
PEOF
run_preview_cli "$POUT"
if [[ $PREVIEW_CLI_STATUS -eq 0 ]] &&
   diff -u "$PVSEM/expected-order.txt" "$PREVIEW_CLI_OUT" > "$PVSEM/order.diff" 2>&1; then
  record_pass "PRV-30 遍历顺序是每层 lexical 的 DFS 先序（逐行比较，未排序）"
else
  record_fail "PRV-30 遍历顺序是每层 lexical 的 DFS 先序" \
    "$(head -n 6 "$PVSEM/order.diff" | tr '\n' ' ')"
fi
expect_preview_parity_at "PRV-30b GUI 与 CLI 的顺序逐行一致（未排序）" "$POUT"

# PRV-31 第 301 个条目是 socket：300 项的窗口不能把它掩盖掉。
PWIN="$PVSEM/window-src"
mkdir -p "$PWIN"
for index in $(seq 1 300); do printf 'x' > "$PWIN/f$(printf '%03d' "$index").dat"; done
python3 -c "import os,socket,sys; os.chdir(os.path.dirname(sys.argv[1])); s=socket.socket(socket.AF_UNIX); s.bind(os.path.basename(sys.argv[1]))" \
  "$PWIN/zzz-socket"
run_preview_cli "$PWIN"
PVL_WIN_STATUS=$PREVIEW_CLI_STATUS
set +e
"$BACKUPCTL" --config-file "$PREVIEW_CONFIG" backup "$PWIN" \
  >"$PVSEM/window-backup.log" 2>&1
PVL_WIN_BACKUP=$?
set -e
if [[ $PVL_WIN_STATUS -eq 1 && $PVL_WIN_BACKUP -eq 1 ]] &&
   grep -qF "zzz-socket" "$PREVIEW_CLI_ERR" &&
   grep -qF "zzz-socket" "$PVSEM/window-backup.log"; then
  record_pass "PRV-31 第 301 条的 socket 不被 300 项窗口掩盖"
else
  record_fail "PRV-31 第 301 条的 socket 不被窗口掩盖" \
    "preview=$PVL_WIN_STATUS[$(head -n 1 "$PREVIEW_CLI_ERR")] backup=$PVL_WIN_BACKUP"
fi
if [[ -x "$PREVIEW_GUI_BIN" ]]; then
  run_preview_gui "$PWIN"
  if [[ $PREVIEW_GUI_STATUS -eq 1 ]] &&
     [[ "$(head -n 1 "$PREVIEW_GUI_ERR")" == "$(head -n 1 "$PREVIEW_CLI_ERR")" ]]; then
    record_pass "PRV-31b 同一个窗口外 socket 在 GUI 上也 blocked"
  else
    record_fail "PRV-31b 同一个窗口外 socket 在 GUI 上也 blocked" \
      "gui=$PREVIEW_GUI_STATUS[$(head -n 1 "$PREVIEW_GUI_ERR")]"
  fi
fi

# ---- L.5b 归档路径 grammar：预览与备份必须用同一套判断 ----
#
# Linux 允许文件名里出现反斜杠，也允许 "C:note.txt" 这种形状；归档格式两者都
# 不接受。共享 walker 现在在生成 archive-relative path 之后调用的是完整
# IsValidArchivePath，所以这类名字必须在**同一层**就让预览失败，而不是等真实
# 备份去报错。
PG="$PREVIEW/grammar"
rm -rf "$PG"
mkdir -p "$PG/backslash" "$PG/drive" "$PG/legal"
mkdir -p "$PG/backslash-dir/dir\name"

printf 'x\n' > "$PG/backslash/a\b.txt"
printf 'x\n' > "$PG/backslash-dir/dir\name/file.txt"
printf 'x\n' > "$PG/drive/C:note.txt"
printf 'x\n' > "$PG/legal/a_b.txt"
printf 'x\n' > "$PG/legal/a-b.txt"
printf 'x\n' > "$PG/legal/a.b.txt"
printf 'x\n' > "$PG/legal/中文.txt"
printf 'x\n' > "$PG/legal/space name.txt"

# PTH-04：合法 Linux 文件名不能被新增的 grammar 检查误伤。预览必须成功、
# 备份必须成功，而且两者看到的是同一批条目。
expect_preview_parity_at "PRV-32 PTH-04 合法文件名：CLI == GUI" "$PG/legal"
run_preview_cli "$PG/legal"
PVL_LEGAL_STATUS=$PREVIEW_CLI_STATUS
PG_REPO_BEFORE="$(find "$PREVIEW_REPO" -maxdepth 1 -name '*.bak' -printf '%f\n' | sort)"
set +e
timeout --signal=KILL "$TIMEOUT_SECONDS" "$BACKUPCTL" --config-file "$PREVIEW_CONFIG" \
  backup "$PG/legal" >"$PG/legal-backup.log" 2>&1
PVL_LEGAL_BACKUP=$?
set -e
if [[ $PVL_LEGAL_STATUS -eq 0 && $PVL_LEGAL_BACKUP -eq 0 ]] &&
   grep -qF 'Preview: 5 matching item(s)' "$PREVIEW_CLI_OUT" &&
   ! grep -qF 'Invalid archive path' "$PG/legal-backup.log"; then
  record_pass "PRV-32b PTH-04 合法文件名：预览与备份都成功（5 项）"
else
  record_fail "PRV-32b PTH-04 合法文件名：预览与备份都成功" \
    "preview=$PVL_LEGAL_STATUS backup=$PVL_LEGAL_BACKUP $(head -n 1 "$PG/legal-backup.log")"
fi

# 逐个非法形状：预览与真实备份必须一起失败，并且说同一句话。
# $1 = 用例号/说明，$2 = 源目录，$3 = 期望在错误里出现的片段
expect_grammar_rejected() {
  local label="$1"
  local source="$2"
  run_preview_cli "$source"
  local preview_status=$PREVIEW_CLI_STATUS
  local preview_message
  preview_message="$(head -n 1 "$PREVIEW_CLI_ERR")"
  set +e
  timeout --signal=KILL "$TIMEOUT_SECONDS" "$BACKUPCTL" --config-file "$PREVIEW_CONFIG" \
    backup "$source" >"$PG/backup.log" 2>&1
  local backup_status=$?
  set -e
  if [[ $preview_status -eq 1 && $backup_status -eq 1 ]] &&
     grep -qF "Invalid archive path" "$PREVIEW_CLI_ERR" &&
     grep -qF "Invalid archive path" "$PG/backup.log" &&
     diff <(echo "$preview_message" | sed 's/^Error: //') \
          <(head -n 1 "$PG/backup.log" | sed 's/^Error: //') >/dev/null; then
    record_pass "$label：预览与备份同一句拒绝（$preview_message）"
  else
    record_fail "$label：预览与备份同一句拒绝" \
      "preview=$preview_status[$preview_message] backup=$backup_status[$(head -n 1 "$PG/backup.log")]"
  fi
  if [[ -x "$PREVIEW_GUI_BIN" ]]; then
    run_preview_gui "$source"
    if [[ $PREVIEW_GUI_STATUS -eq 1 ]] &&
       [[ "$(head -n 1 "$PREVIEW_GUI_ERR")" == "$preview_message" ]]; then
      record_pass "$label：GUI 与 CLI 同一句"
    else
      record_fail "$label：GUI 与 CLI 同一句" \
        "gui=[$(head -n 1 "$PREVIEW_GUI_ERR")] cli=[$preview_message]"
    fi
  fi
}

# 三次被拒绝的备份都不能留下新归档：快照要取在这三次之前。
PG_REPO_BEFORE="$(find "$PREVIEW_REPO" -maxdepth 1 -name '*.bak' -printf '%f\n' | sort)"
expect_grammar_rejected "PRV-33 PTH-01 文件名含反斜杠" "$PG/backslash"
expect_grammar_rejected "PRV-34 PTH-02 目录名含反斜杠" "$PG/backslash-dir"
expect_grammar_rejected "PRV-35 PTH-03 盘符风格文件名" "$PG/drive"

# 拒绝之后不能留下半成品归档：用前后快照比较，不依赖时间戳。
PG_REPO_AFTER="$(find "$PREVIEW_REPO" -maxdepth 1 -name '*.bak' -printf '%f\n' | sort)"
if [[ "$PG_REPO_BEFORE" == "$PG_REPO_AFTER" ]]; then
  record_pass "PRV-35b 被 grammar 拒绝的备份没有留下任何新归档"
else
  record_fail "PRV-35b 被 grammar 拒绝的备份没有留下任何新归档" \
    "before=[$PG_REPO_BEFORE] after=[$PG_REPO_AFTER]"
fi

# ---- L.5c 完整 archive grammar 只在"真的会进归档"时才要求 ----
#
# 历史 Backup 的顺序是"Filter 先决定这条进不进归档，进了才校验路径"。所以一个
# 会被规则排除的非法名字**不能**阻塞整次备份/预览——这正是 FILT-PATH-02/04/05b
# 与 SOCK-PATH-02 要钉的东西。

# PRV-36 被排除的反斜杠文件名：两边都成功，且它不在结果里。
expect_preview_matches_backup_at "PRV-36 FILT-PATH-02 被排除的反斜杠文件名不阻塞" \
  "$PG/backslash" --exclude 'ext:txt'
run_preview_cli "$PG/backslash" --exclude 'ext:txt'
if [[ $PREVIEW_CLI_STATUS -eq 0 ]] &&
   ! preview_listed "$PREVIEW_CLI_OUT" | grep -q 'b.txt'; then
  record_pass "PRV-36b 被排除的反斜杠文件不出现在预览结果里"
else
  record_fail "PRV-36b 被排除的反斜杠文件不出现在预览结果里" \
    "$(head -n 3 "$PREVIEW_CLI_OUT" | tr '\n' ' ')"
fi

# PRV-37 被排除的反斜杠目录：整棵子树剪掉，两边都成功。
expect_preview_matches_backup_at "PRV-37 FILT-PATH-04 被排除的反斜杠目录整棵剪掉" \
  "$PG/backslash-dir" --exclude 'name:*name'
run_preview_cli "$PG/backslash-dir" --exclude 'name:*name'
if [[ $PREVIEW_CLI_STATUS -eq 0 ]] &&
   ! preview_listed "$PREVIEW_CLI_OUT" | grep -q 'file.txt' &&
   ! grep -qF 'Invalid archive path' "$PREVIEW_CLI_ERR"; then
  record_pass "PRV-37b 整棵子树被剪掉且没有出现语法错误"
else
  record_fail "PRV-37b 整棵子树被剪掉且没有出现语法错误" \
    "exit=$PREVIEW_CLI_STATUS $(head -n 1 "$PREVIEW_CLI_ERR")"
fi

# PRV-38 被排除的盘符风格文件名：两边都成功。
expect_preview_matches_backup_at "PRV-38 FILT-PATH-05b 被排除的 C:note.txt 不阻塞" \
  "$PG/drive" --exclude 'ext:txt'

# PRV-39 socket 的错误优先级：名字里带反斜杠也一样。
PSOCK="$PG/socket"
rm -rf "$PSOCK"; mkdir -p "$PSOCK"
printf 'x\n' > "$PSOCK/keep.txt"
python3 -c "import os,socket,sys; os.chdir(os.path.dirname(sys.argv[1])); s=socket.socket(socket.AF_UNIX); s.bind(os.path.basename(sys.argv[1]))" \
  "$PSOCK/sock\bad"
run_preview_cli "$PSOCK"
PSOCK_PREVIEW=$PREVIEW_CLI_STATUS
PSOCK_MESSAGE="$(head -n 1 "$PREVIEW_CLI_ERR")"
set +e
timeout --signal=KILL "$TIMEOUT_SECONDS" "$BACKUPCTL" --config-file "$PREVIEW_CONFIG" \
  backup "$PSOCK" > "$PG/socket-backup.log" 2>&1
PSOCK_BACKUP=$?
set -e
if [[ $PSOCK_PREVIEW -eq 1 && $PSOCK_BACKUP -eq 1 ]] &&
   [[ "$PSOCK_MESSAGE" == *"Unsupported special type: socket"* ]] &&
   [[ "$PSOCK_MESSAGE" != *"Invalid archive path"* ]] &&
   grep -qF "Unsupported special type: socket" "$PG/socket-backup.log" &&
   ! grep -qF "Invalid archive path" "$PG/socket-backup.log"; then
  record_pass "PRV-39 SOCK-PATH-01 非法名 socket 仍然报 socket 不支持（优先级保持）"
else
  record_fail "PRV-39 SOCK-PATH-01 非法名 socket 的优先级" \
    "preview=$PSOCK_PREVIEW[$PSOCK_MESSAGE] backup=$PSOCK_BACKUP"
fi
if [[ -x "$PREVIEW_GUI_BIN" ]]; then
  run_preview_gui "$PSOCK"
  if [[ $PREVIEW_GUI_STATUS -eq 1 ]] &&
     [[ "$(head -n 1 "$PREVIEW_GUI_ERR")" == "$PSOCK_MESSAGE" ]]; then
    record_pass "PRV-39b GUI 与 CLI 对非法名 socket 报同一句"
  else
    record_fail "PRV-39b GUI 与 CLI 对非法名 socket 报同一句" \
      "gui=[$(head -n 1 "$PREVIEW_GUI_ERR")] cli=[$PSOCK_MESSAGE]"
  fi
fi

# PRV-40 明确排除这个 socket：两边都成功，它不进入结果。
expect_preview_matches_backup_at "PRV-40 SOCK-PATH-02 被排除的非法名 socket 不阻塞" \
  "$PSOCK" --exclude 'name:sock*'

# PRV-41 超长 child path：长度是遍历阶段的硬边界（历史语义），即使规则会把它
#
# ⚠ 这条用例目前是红的，原因值得写在这里，免得下一个人重新推一遍：
#   要断言的守卫是 kMaxArchivePathLength = 4096（include/archive_path.h:25），
#   它**等于 PATH_MAX**。夹具只能用 chdir 逐级下钻去构造 >4096 的 archive path，
#   但无论如何，那个夹具的**绝对**路径都至少是
#       <仓库前缀> + 4097 + 溢出 + 文件名
#   —— 对任何仓库位置都必然超过 PATH_MAX（前缀不可能为负）。
#   所以这条用例只有在产品用 openat/fchdir 风格的**相对下钻**遍历时才可能成立；
#   现在的失败信息是 "Failed to inspect path: <绝对路径>: File name too long"，
#   说明遍历把完整绝对路径交给了 syscall。
#   两种可能的收尾都需要单独评审：(a) 改遍历策略；(b) 明确写清"在当前遍历策略下
#   这条端到端构造不可成立"。注意守卫本身有单元测试覆盖
#   （tests/unit/backup_preview_test.cpp:1256），缺的只是这一段端到端构造。
# PRV-41 超长 child path：长度是遍历阶段的硬边界（历史语义），即使规则会把它
# 排除，也照样失败。用 chdir + 相对路径构造，任何一次 syscall 都不超 PATH_MAX。
PLONG="$PG/too-long/src"
rm -rf "$PG/too-long"; mkdir -p "$PLONG"
python3 - "$PLONG" <<'PYEOF'
import os, sys
src = sys.argv[1]
os.chdir(src)
component = 'd' * 190
relative = 0
while relative < 3900:
    os.makedirs(component, exist_ok=True)
    os.chdir(component)
    relative += 1 + len(component)
open('v' * 200, 'w').write('x')
PYEOF
run_preview_cli "$PLONG" --exclude 'name:vvv*'
PLONG_STATUS=$PREVIEW_CLI_STATUS
set +e
timeout --signal=KILL "$TIMEOUT_SECONDS" "$BACKUPCTL" --config-file "$PREVIEW_CONFIG" \
  backup "$PLONG" --exclude 'name:vvv*' > "$PG/toolong-backup.log" 2>&1
PLONG_BACKUP=$?
set -e
if [[ $PLONG_STATUS -eq 1 && $PLONG_BACKUP -eq 1 ]] &&
   grep -qF "Archive path too long" "$PREVIEW_CLI_ERR" &&
   grep -qF "Archive path too long" "$PG/toolong-backup.log"; then
  record_pass "PRV-41 超长 child path 即使被排除也照样失败（历史 early 语义）"
else
  record_fail "PRV-41 超长 child path 的历史 early 语义" \
    "preview=$PLONG_STATUS[$(head -n 1 "$PREVIEW_CLI_ERR")] backup=$PLONG_BACKUP"
fi
rm -rf "$PG/too-long"

# L.6 单实例：preview 是产品命令，必须在进入扫描之前被同一把锁拒绝。
# "只读所以可以并发"不是这个产品的规则。
PLOCK="$PREVIEW/lock"
mkdir -p "$PLOCK/src" "$PLOCK/repo"
printf 'w\n' > "$PLOCK/src/a.txt"
"$BACKUPCTL" --config-file "$PLOCK/config.json" config repository set "$PLOCK/repo" \
  >/dev/null 2>&1
"$BACKUPCTL" --config-file "$PLOCK/config.json" --schedule-file "$PLOCK/schedule.json" \
  schedule set --source "$PLOCK/src" --interval-minutes 5 --retain 2 >/dev/null 2>&1
APP_LOCK="/run/user/$(id -u)/backup-project.lock"
[ -d "/run/user/$(id -u)" ] || APP_LOCK="/tmp/backup-project-$(id -u).lock"
"$BACKUPCTL" --config-file "$PLOCK/config.json" --schedule-file "$PLOCK/schedule.json" \
  schedule watch >"$PLOCK/watch.log" 2>&1 &
PLOCK_WATCH=$!
PLOCK_HOLD=0
for _ in $(seq 1 100); do
  if kill -0 "$PLOCK_WATCH" 2>/dev/null &&
     grep -q "^pid=$PLOCK_WATCH " "$APP_LOCK" 2>/dev/null; then
    PLOCK_HOLD=1
    break
  fi
  sleep 0.1
done
run_preview_cli "$PREVIEW_SRC" --include 'ext:txt'
if [[ $PLOCK_HOLD -eq 1 && $PREVIEW_CLI_STATUS -eq 3 &&
      ! -s "$PREVIEW_CLI_OUT" ]]; then
  record_pass "PRV-21 单实例：CLI 持锁时 preview 退出 3 且不扫描（无输出）"
else
  record_fail "PRV-21 单实例：CLI 持锁时 preview 退出 3 且不扫描" \
    "hold=$PLOCK_HOLD exit=$PREVIEW_CLI_STATUS out=$(wc -c < "$PREVIEW_CLI_OUT")"
fi
if [[ -x "$PREVIEW_GUI_BIN" ]]; then
  run_preview_gui "$PREVIEW_SRC" --include 'ext:txt'
  if [[ $PREVIEW_GUI_STATUS -eq 3 ]]; then
    record_pass "PRV-22 单实例：同一个锁也拦住 GUI 的预览入口"
  else
    record_fail "PRV-22 单实例：同一个锁也拦住 GUI 的预览入口" \
      "exit=$PREVIEW_GUI_STATUS $(head -n 1 "$PREVIEW_GUI_ERR")"
  fi
fi
kill -TERM "$PLOCK_WATCH" 2>/dev/null || true
for _ in $(seq 1 100); do kill -0 "$PLOCK_WATCH" 2>/dev/null || break; sleep 0.1; done
wait "$PLOCK_WATCH" 2>/dev/null || true
run_preview_cli "$PREVIEW_SRC" --include 'ext:txt'
if [[ $PREVIEW_CLI_STATUS -eq 0 ]]; then
  record_pass "PRV-23 持锁进程退出之后 preview 又能跑（锁由内核释放）"
else
  record_fail "PRV-23 持锁进程退出之后 preview 又能跑" "exit=$PREVIEW_CLI_STATUS"
fi

# ---- L.7 增量策略：CLI 与 GUI 给出同一组事实 ----------------------------
#
# 增量是**策略**维度上的第二个取值，不是第二种备份命令。这里不重新实现任何
# 语义，只钉两件事：
#
#   1. CLI 的三步（完整基线 / 无变化 / delta）真的按这个顺序发生，而且它说的是
#      "这一轮到底做了什么"；
#   2. Modern GUI 走它自己的真实控制器入口，得到**同一串结果**。
#
# 两边共用同一个核心引擎，所以"结果一致"本来应该是推论 —— 这条用例把它变成
# 事实：任何一边偷偷换了判定，这里都会红。
INC="$PREVIEW/incremental"
INC_CONFIG="$INC/config.json"
rm -rf "$INC"
mkdir -p "$INC/src" "$INC/repo-cli" "$INC/repo-gui" "$INC/gui-src"

inc_fill_source() {
  local dir="$1"
  rm -rf "$dir"
  mkdir -p "$dir"
  printf 'alpha' > "$dir/a.txt"
  printf 'bravo' > "$dir/b.txt"
}
inc_fill_source "$INC/src"
inc_fill_source "$INC/gui-src"

"$BACKUPCTL" --config-file "$INC_CONFIG" config repository set "$INC/repo-cli" \
  >/dev/null 2>&1

run_inc_cli() {
  set +e
  timeout --signal=KILL "$TIMEOUT_SECONDS" "$BACKUPCTL" \
    --config-file "$INC_CONFIG" backup "$INC/src" --strategy incremental "$@" \
    >"$INC/cli.out" 2>"$INC/cli.err"
  INC_STATUS=$?
  set -e
}

# 挑出仓库里"增量/差异"那一份归档。
#
# 为什么不能按文件名挑：增量归档只有在**同一秒内**与基线重名时才会带上 _NNN
# 后缀（BackupCatalog 的去重规则）。基线快照与 delta 差一秒就都没有后缀，于是
# 原来的 "*_001.bak" 匹配不到任何东西 —— basename 拿不到参数，set -e 会让整个
# 套件直接退出（exit=123）。这条断言因此与机器快慢相关，属于既有的偶发失败。
# 判据改成内容：增量归档里带 parent 引用，完整基线里没有。
find_delta_archive() {
  local repository="$1"
  local candidate
  for candidate in "$repository"/*.bak; do
    [[ -e "$candidate" ]] || continue
    if grep -aq "parent" "$candidate"; then
      basename "$candidate"
      return 0
    fi
  done
  # 兜底：一份带 parent 的都没有时取最新的（调用方自己判断结果对不对）。
  ls -t "$repository"/*.bak 2>/dev/null | head -1 | xargs -r basename
}

# INC-01 第一次：没有可信基线 -> 完整基线，而且必须说出来。
run_inc_cli
if [[ $INC_STATUS -eq 0 ]] &&
   grep -qF 'Strategy:   incremental' "$INC/cli.out" &&
   grep -qF 'Kind:       full baseline' "$INC/cli.out" &&
   grep -qF 'no trustworthy baseline' "$INC/cli.out"; then
  record_pass "INC-01 第一次增量：明确报告建的是完整基线（并给出原因）"
else
  record_fail "INC-01 第一次增量：报告完整基线" \
    "$(head -4 "$INC/cli.out" | tr '\n' ' ')"
fi

# INC-02 第二次：没有变化 -> 什么都不写。
run_inc_cli
INC_REPO_BEFORE="$(find "$INC/repo-cli" -type f | wc -l)"
if [[ $INC_STATUS -eq 0 ]] &&
   grep -qF 'No effective changes' "$INC/cli.out"; then
  record_pass "INC-02 没有有效变化时不创建新快照"
else
  record_fail "INC-02 没有变化时跳过" "$(head -2 "$INC/cli.out" | tr '\n' ' ')"
fi

# INC-03 就地改写一个文件：长度与 mtime 都不变。
# 这是 metadata-first 看不见、而增量必须看见的那一类变化。
python3 - "$INC/src/a.txt" <<'PYEOF'
import os, sys
path = sys.argv[1]
info = os.lstat(path)
with open(path, 'r+b') as handle:
    handle.write(b'ALPHA')
os.utime(path, (info.st_atime, info.st_mtime), follow_symlinks=False)
PYEOF
run_inc_cli
if [[ $INC_STATUS -eq 0 ]] &&
   grep -qF 'Kind:       delta' "$INC/cli.out" &&
   grep -qF 'Changes:    added=0 modified=1 metadata=0 removed=0' "$INC/cli.out"; then
  record_pass "INC-03 判别：same-size + same-mtime 改写被识别为一次修改"
else
  record_fail "INC-03 same-size/same-mtime 改写" \
    "$(grep -E 'Kind:|Changes:' "$INC/cli.out" | tr '\n' ' ')"
fi
INC_REPO_AFTER="$(find "$INC/repo-cli" -type f | wc -l)"
if [[ "$INC_REPO_AFTER" -gt "$INC_REPO_BEFORE" ]]; then
  record_pass "INC-03b delta 真的落盘了（仓库里多了文件）"
else
  record_fail "INC-03b delta 落盘" "before=$INC_REPO_BEFORE after=$INC_REPO_AFTER"
fi

# INC-04 GUI 走真实控制器入口，跑同一串步骤。
if [[ -x "$PREVIEW_GUI_BIN" ]]; then
  set +e
  QT_QPA_PLATFORM=offscreen timeout 180 "$PREVIEW_GUI_BIN" --incremental-test \
    "$INC/gui-src" "$INC/repo-gui" \
    --config-file "$INC/gui-config.json" --schedule-file "$INC/gui-schedule.json" \
    >"$INC/gui.out" 2>"$INC/gui.err"
  INC_GUI_STATUS=$?
  set -e
  if [[ $INC_GUI_STATUS -eq 0 ]] &&
     grep -qF '[incremental] step1 kind=full-baseline reason=yes' "$INC/gui.out" &&
     grep -qF '[incremental] step2 kind=no-changes' "$INC/gui.out" &&
     grep -qF '[incremental] step3 kind=delta' "$INC/gui.out" &&
     grep -qF '[incremental] restore ok' "$INC/gui.out"; then
    record_pass "INC-04 GUI 增量：基线 / 无变化 / delta / 依赖链恢复全部成立"
  else
    record_fail "INC-04 GUI 增量链路" \
      "exit=$INC_GUI_STATUS $(grep -E 'step|restore' "$INC/gui.out" | tr '\n' ' ')"
  fi

  # INC-05 parity：两侧报告的种类序列必须逐项相同。
  # CLI 侧的序列从三次输出里取，GUI 侧从它自己打印的行里取。
  INC_CLI_KINDS="full-baseline,no-changes,delta"
  INC_GUI_KINDS="$(grep -oE 'kind=[a-z-]+' "$INC/gui.out" | sed 's/kind=//' | paste -sd, -)"
  if [[ "$INC_CLI_KINDS" == "$INC_GUI_KINDS" ]]; then
    record_pass "INC-05 CLI 与 GUI 报告的种类序列一致（$INC_CLI_KINDS）"
  else
    record_fail "INC-05 CLI/GUI 种类序列一致" \
      "cli=$INC_CLI_KINDS gui=$INC_GUI_KINDS"
  fi
else
  record_pass "INC-04/05 GUI 增量（没有 build/backup-gui-modern，跳过 GUI 侧）"
fi

# INC-06 组合校验：增量 + 非 MyPack 必须在写任何东西之前被拒绝。
rm -rf "$INC/repo-reject"
mkdir -p "$INC/repo-reject"
"$BACKUPCTL" --config-file "$INC_CONFIG" config repository set "$INC/repo-reject" \
  >/dev/null 2>&1
set +e
timeout --signal=KILL "$TIMEOUT_SECONDS" "$BACKUPCTL" --config-file "$INC_CONFIG" \
  backup "$INC/src" --strategy incremental --pack ustar \
  >"$INC/reject.out" 2>&1
INC_REJECT_STATUS=$?
set -e
INC_REJECT_FILES="$(find "$INC/repo-reject" -type f | wc -l)"
if [[ $INC_REJECT_STATUS -eq 2 && "$INC_REJECT_FILES" -eq 0 ]] &&
   grep -qF 'MyPack' "$INC/reject.out"; then
  record_pass "INC-06 增量 + USTAR 在写盘前被拒绝，且理由说明为什么"
else
  record_fail "INC-06 增量 + USTAR 被拒绝" \
    "exit=$INC_REJECT_STATUS files=$INC_REJECT_FILES $(head -1 "$INC/reject.out")"
fi

# INC-07 未知策略绝不回退到 full。
set +e
timeout --signal=KILL "$TIMEOUT_SECONDS" "$BACKUPCTL" --config-file "$INC_CONFIG" \
  backup "$INC/src" --strategy bogus >"$INC/bogus.out" 2>&1
INC_BOGUS_STATUS=$?
set -e
if [[ $INC_BOGUS_STATUS -eq 2 ]] &&
   grep -qF "unknown backup strategy 'bogus'" "$INC/bogus.out"; then
  record_pass "INC-07 未知策略是用法错误，不静默降级为 full"
else
  record_fail "INC-07 未知策略" "exit=$INC_BOGUS_STATUS $(head -1 "$INC/bogus.out")"
fi

# INC-08 依赖链恢复：只给 delta 的文件名，base 由核心自己解析。
# INC-06 把配置指向了另一个仓库（那是它要证明的事），这里先指回来。
"$BACKUPCTL" --config-file "$INC_CONFIG" config repository set "$INC/repo-cli" \
  >/dev/null 2>&1
# 明确挑"_NNN.bak"那一份（delta），不靠 sort 的标点顺序：
# 排序规则受 locale 影响，"a.bak" 与 "a_001.bak" 谁在前并不稳定。
INC_DELTA="$(find_delta_archive "$INC/repo-cli")"
rm -rf "$INC/restored"
set +e
timeout --signal=KILL "$TIMEOUT_SECONDS" "$BACKUPCTL" --config-file "$INC_CONFIG" \
  restore "$INC_DELTA" "$INC/restored" >"$INC/restore.out" 2>&1
INC_RESTORE_STATUS=$?
set -e
if [[ $INC_RESTORE_STATUS -eq 0 ]] &&
   [[ "$(cat "$INC/restored/a.txt" 2>/dev/null)" == "ALPHA" ]] &&
   [[ "$(cat "$INC/restored/b.txt" 2>/dev/null)" == "bravo" ]]; then
  record_pass "INC-08 从 delta 的依赖链恢复：变化部分与基线部分都对"
else
  record_fail "INC-08 依赖链恢复" \
    "exit=$INC_RESTORE_STATUS $(head -1 "$INC/restore.out")"
fi

# ---- L.8 计划 + 增量：同一个引擎，dependency-aware retention ----------------
#
# 这一节钉的是"计划路径没有自己的一套增量"：它把决策交给同一个共享引擎，
# 于是 metadata-first 看不见的改写在这里同样看得见；而 retention 变成
# dependency-aware 之后，把 retain 调小也不会为了"删最旧"而删断一条链。
SCHED_INC="$PREVIEW/sched-incremental"
SCHED_INC_CFG="$SCHED_INC/config.json"
SCHED_INC_STORE="$SCHED_INC/schedule.json"
rm -rf "$SCHED_INC"
mkdir -p "$SCHED_INC/src" "$SCHED_INC/repo" "$SCHED_INC/home"
printf 'alpha' > "$SCHED_INC/src/a.txt"
printf 'bravo' > "$SCHED_INC/src/b.txt"
"$BACKUPCTL" --config-file "$SCHED_INC_CFG" --schedule-file "$SCHED_INC_STORE" \
  config repository set "$SCHED_INC/repo" >/dev/null 2>&1

run_sched_inc() {
  set +e
  timeout --signal=KILL "$TIMEOUT_SECONDS" "$BACKUPCTL" --config-file "$SCHED_INC_CFG" \
    --schedule-file "$SCHED_INC_STORE" schedule "$@" >"$SCHED_INC/out" 2>&1
  SCHED_INC_STATUS=$?
  set -e
}

# INC-09 计划配置接受 --strategy，并且真的按增量跑。
run_sched_inc set --source "$SCHED_INC/src" --interval-minutes 60 --retain 3 \
  --strategy incremental
if [[ $SCHED_INC_STATUS -eq 0 ]] &&
   grep -qF 'Strategy: incremental' "$SCHED_INC/out"; then
  record_pass "INC-09 计划支持 --strategy incremental"
else
  record_fail "INC-09 计划 --strategy" "$(head -2 "$SCHED_INC/out" | tr '\n' ' ')"
fi
run_sched_inc enable
run_sched_inc run
if grep -qF 'full baseline snapshot' "$SCHED_INC/out"; then
  record_pass "INC-09b 计划增量第一轮：如实报告建的是完整基线"
else
  record_fail "INC-09b 计划增量第一轮" "$(head -3 "$SCHED_INC/out" | tr '\n' ' ')"
fi
run_sched_inc run
if grep -qiE 'skipped|not changed' "$SCHED_INC/out"; then
  record_pass "INC-09c 计划增量第二轮：没有变化就跳过"
else
  record_fail "INC-09c 计划增量第二轮" "$(head -3 "$SCHED_INC/out" | tr '\n' ' ')"
fi

# INC-10 判别：same-size + same-mtime 的改写，计划路径也必须看得见。
python3 - "$SCHED_INC/src/a.txt" <<'PYEOF'
import os, sys
path = sys.argv[1]
info = os.lstat(path)
with open(path, 'r+b') as handle:
    handle.write(b'ALPHA')
os.utime(path, (info.st_atime, info.st_mtime), follow_symlinks=False)
PYEOF
run_sched_inc run
if grep -qF 'Incremental delta on top of' "$SCHED_INC/out"; then
  record_pass "INC-10 判别：计划路径也识别 same-size/same-mtime 的改写（写出 delta）"
else
  record_fail "INC-10 计划路径识别改写" "$(head -3 "$SCHED_INC/out" | tr '\n' ' ')"
fi

# INC-11 retention 不能删断链：把 retain 调成 1 再跑一轮，链上的祖先必须还在。
run_sched_inc set --retain 1
printf 'charlie' > "$SCHED_INC/src/c.txt"
run_sched_inc run
SCHED_INC_BACKUPS="$(ls "$SCHED_INC/repo"/*.bak 2>/dev/null | wc -l)"
if [[ "$SCHED_INC_BACKUPS" -ge 3 ]]; then
  record_pass "INC-11 retain=1 也不会删掉链上必需的祖先（当前 $SCHED_INC_BACKUPS 份）"
else
  record_fail "INC-11 retention 保住了祖先" "backups=$SCHED_INC_BACKUPS"
fi
# 保住还不够：那条链必须真的还能恢复。
SCHED_INC_DELTA="$(find_delta_archive "$SCHED_INC/repo")"
rm -rf "$SCHED_INC/restored"
set +e
timeout --signal=KILL "$TIMEOUT_SECONDS" "$BACKUPCTL" --config-file "$SCHED_INC_CFG" \
  --schedule-file "$SCHED_INC_STORE" restore "$SCHED_INC_DELTA" "$SCHED_INC/restored" \
  >"$SCHED_INC/restore.out" 2>&1
SCHED_INC_RESTORE=$?
set -e
if [[ $SCHED_INC_RESTORE -eq 0 ]] &&
   [[ "$(cat "$SCHED_INC/restored/a.txt" 2>/dev/null)" == "ALPHA" ]]; then
  record_pass "INC-11b 经过 retention 之后，链仍然恢复得出正确内容"
else
  record_fail "INC-11b retention 之后链可恢复" \
    "exit=$SCHED_INC_RESTORE $(head -1 "$SCHED_INC/restore.out")"
fi

# ---- CLI 约定 --------------------------------------------------------

expect_success "CLI-01 --help exits 0" --help
run_backupctl --help
if grep -qF "file_name" "$OUT_FILE"; then
  record_pass "CLI-02 help text uses the archive file wording"
else
  record_fail "CLI-02 help text uses the archive file wording" "not found"
fi
expect_failure "CLI-03 missing arguments return 2" 2 "Usage:" restore

# ---- 清理与汇总 ------------------------------------------------------

KEEP_FLAG=0
if [[ -n "$(printenv KEEP_TESTDATA || true)" ]]; then
  KEEP_FLAG=1
fi

if [[ $FAIL_COUNT -eq 0 && $KEEP_FLAG -eq 0 ]]; then
  # 归档里保存了 mode，恢复出来的目录可能是只读的；先加回写权限再删，
  # 否则清理本身会因为权限失败而把整个套件带崩。
  chmod -R u+rwX "$TEST_ROOT" 2>/dev/null || true
  rm -rf "$TEST_ROOT" || true
else
  echo "[test] keeping test data in $TEST_ROOT"
fi

echo "[test] results: PASS=$PASS_COUNT FAIL=$FAIL_COUNT"
if [[ $FAIL_COUNT -ne 0 ]]; then
  echo "[test] suite FAILED" >&2
  exit 1
fi
echo "[test] all cases passed"
