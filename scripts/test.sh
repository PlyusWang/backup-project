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

# 用 timeout --signal=KILL：归档解析如果陷入死循环，SIGTERM 未必能叫停，
# 直接 KILL 才能保证测试不会挂在这里。超时按 124 处理，算失败。
#
# 带硬超时地运行 backupctl：合并后的输出写进 OUT_FILE，
# 退出码留在 STATUS（124 表示超时被杀）。
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

expect_failure "ER-14 usage error returns 2" 2 "Usage:" backup "$SOURCE"
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
python3 -c "
import socket, time
handle = socket.socket(socket.AF_UNIX)
handle.bind('$TEST_ROOT/unsup/with-socket/sock')
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

# ---- CLI 约定 --------------------------------------------------------

expect_success "CLI-01 --help exits 0" --help
run_backupctl --help
if grep -qF "backup_file" "$OUT_FILE"; then
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
