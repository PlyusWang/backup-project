#!/usr/bin/env bash
#
# Sprint 1 (v0.1) test suite: basic CLI backup / restore.
#
# The suite verifies:
#   * the full round trip (backup -> restore -> diff -r identical);
#   * byte-level equality of the random binary test file (sha256);
#   * the main error paths (missing paths, non-directory source, existing
#     non-empty targets, unsupported file types, permission errors).
#
# All test data is generated inside <repo>/testdata, which is gitignored
# and removed again when the suite passes (set KEEP_TESTDATA=1 to keep it
# for manual inspection).
#
# Exit status: 0 only when every case passes.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BACKUPCTL="$ROOT_DIR/build/backupctl"
TEST_ROOT="$ROOT_DIR/testdata"
SOURCE="$TEST_ROOT/source"
REPOSITORY="$TEST_ROOT/repository"
RESTORED="$TEST_ROOT/restored"

echo "[test] Sprint 1 backup/restore test suite"

if [[ ! -x "$BACKUPCTL" ]]; then
  echo "[test] backupctl is missing; building first..."
  make -C "$ROOT_DIR"
fi

fail() {
  echo "  FAIL: $1" >&2
  exit 1
}

pass() {
  echo "  PASS: $1"
}

# Runs a command that must fail.  Fails the suite when the command
# succeeds (exit code 0) or crashes (exit code >= 128, i.e. a signal).
expect_failure() {
  local name="$1"
  shift
  set +e
  "$@" >/dev/null 2>&1
  local status=$?
  set -e
  if [[ $status -eq 0 ]]; then
    fail "$name: expected non-zero exit code, got 0"
  fi
  if [[ $status -ge 128 ]]; then
    fail "$name: command crashed (exit code $status)"
  fi
  pass "$name"
}

# ---- Test data ---------------------------------------------------------

rm -rf "$TEST_ROOT"
mkdir -p \
  "$SOURCE/empty_dir" \
  "$SOURCE/directory with spaces" \
  "$SOURCE/中文目录" \
  "$SOURCE/level1/level2"

printf 'Hello, backup project!\n' > "$SOURCE/hello.txt"
: > "$SOURCE/empty.txt"
# 2 MiB of random binary data (requirement: 1 MiB or more).
head -c 2097152 /dev/urandom > "$SOURCE/binary.bin"
printf 'Space in file name.\n' > "$SOURCE/directory with spaces/space file.txt"
printf 'UTF-8 中文内容：备份与恢复。\n' > "$SOURCE/中文目录/中文文件.txt"
printf 'Nested file content.\n' > "$SOURCE/level1/level2/nested.txt"

# ---- Round trip --------------------------------------------------------

echo "[test] TC-BR-01: backup creates <repository>/data"
"$BACKUPCTL" backup "$SOURCE" "$REPOSITORY" \
  || fail "TC-BR-01: backup command failed"
[[ -d "$REPOSITORY/data" ]] || fail "TC-BR-01: repository/data was not created"
pass "TC-BR-01"

echo "[test] TC-BR-02: restore reproduces the source tree"
"$BACKUPCTL" restore "$REPOSITORY" "$RESTORED" \
  || fail "TC-BR-02: restore command failed"
diff -r "$SOURCE" "$RESTORED" \
  || fail "TC-BR-02: diff -r source restored reports differences"
pass "TC-BR-02"

echo "[test] TC-BR-03: random binary file is byte-identical"
cmp -s "$SOURCE/binary.bin" "$RESTORED/binary.bin" \
  || fail "TC-BR-03: binary.bin differs"
src_sum="$(sha256sum "$SOURCE/binary.bin" | awk '{print $1}')"
rest_sum="$(sha256sum "$RESTORED/binary.bin" | awk '{print $1}')"
[[ "$src_sum" == "$rest_sum" ]] || fail "TC-BR-03: sha256 mismatch"
pass "TC-BR-03"

echo "[test] TC-BR-04: empty file stays empty"
[[ -f "$RESTORED/empty.txt" && ! -s "$RESTORED/empty.txt" ]] \
  || fail "TC-BR-04: empty.txt is missing or not empty"
pass "TC-BR-04"

echo "[test] TC-BR-05: empty directory is restored"
[[ -d "$RESTORED/empty_dir" && -z "$(ls -A "$RESTORED/empty_dir")" ]] \
  || fail "TC-BR-05: empty_dir is missing or not empty"
pass "TC-BR-05"

echo "[test] TC-BR-06: nested directories, spaces and UTF-8 names"
[[ -f "$RESTORED/level1/level2/nested.txt" ]] \
  || fail "TC-BR-06: nested file missing"
[[ -f "$RESTORED/directory with spaces/space file.txt" ]] \
  || fail "TC-BR-06: file with spaces missing"
[[ -f "$RESTORED/中文目录/中文文件.txt" ]] \
  || fail "TC-BR-06: UTF-8 file missing"
pass "TC-BR-06"

# ---- Error paths -------------------------------------------------------

echo "[test] TC-ER-01: source directory does not exist"
expect_failure "TC-ER-01" "$BACKUPCTL" backup "$TEST_ROOT/no_such_source" "$TEST_ROOT/repo-er01"

echo "[test] TC-ER-02: source is a regular file"
expect_failure "TC-ER-02" "$BACKUPCTL" backup "$SOURCE/hello.txt" "$TEST_ROOT/repo-er02"

echo "[test] TC-ER-03: restore from a repository that does not exist"
expect_failure "TC-ER-03" "$BACKUPCTL" restore "$TEST_ROOT/no_such_repo" "$TEST_ROOT/rest-er03"

echo "[test] TC-ER-04: repository without data directory"
mkdir -p "$TEST_ROOT/repo-no-data"
expect_failure "TC-ER-04" "$BACKUPCTL" restore "$TEST_ROOT/repo-no-data" "$TEST_ROOT/rest-er04"

echo "[test] TC-ER-05: destination already exists and is not empty"
mkdir -p "$TEST_ROOT/busy-dest"
printf 'keep\n' > "$TEST_ROOT/busy-dest/keep.txt"
expect_failure "TC-ER-05" "$BACKUPCTL" restore "$REPOSITORY" "$TEST_ROOT/busy-dest"

echo "[test] TC-ER-06: repository data already exists and is not empty"
expect_failure "TC-ER-06" "$BACKUPCTL" backup "$SOURCE" "$REPOSITORY"

echo "[test] TC-ER-07: unsupported file type (symlink)"
mkdir -p "$TEST_ROOT/symlink-src"
printf 'target\n' > "$TEST_ROOT/symlink-src/target.txt"
ln -s target.txt "$TEST_ROOT/symlink-src/link.txt"
expect_failure "TC-ER-07" "$BACKUPCTL" backup "$TEST_ROOT/symlink-src" "$TEST_ROOT/repo-er07"

echo "[test] TC-ER-08: unsupported file type (FIFO)"
mkdir -p "$TEST_ROOT/fifo-src"
mkfifo "$TEST_ROOT/fifo-src/pipe"
expect_failure "TC-ER-08" "$BACKUPCTL" backup "$TEST_ROOT/fifo-src" "$TEST_ROOT/repo-er08"

echo "[test] TC-ER-09: unreadable regular file"
mkdir -p "$TEST_ROOT/unreadable-src"
printf 'secret\n' > "$TEST_ROOT/unreadable-src/locked.txt"
chmod 000 "$TEST_ROOT/unreadable-src/locked.txt"
expect_failure "TC-ER-09" "$BACKUPCTL" backup "$TEST_ROOT/unreadable-src" "$TEST_ROOT/repo-er09"
chmod 644 "$TEST_ROOT/unreadable-src/locked.txt"

echo "[test] TC-ER-10: cannot create repository (parent is a file)"
printf 'blocker\n' > "$TEST_ROOT/blocker"
expect_failure "TC-ER-10" "$BACKUPCTL" backup "$SOURCE" "$TEST_ROOT/blocker/repo"

echo "[test] TC-ER-11: cannot create destination (parent is a file)"
expect_failure "TC-ER-11" "$BACKUPCTL" restore "$REPOSITORY" "$TEST_ROOT/blocker/dest"

echo "[test] TC-ER-12: usage error (missing argument)"
expect_failure "TC-ER-12" "$BACKUPCTL" backup "$SOURCE"

echo "[test] TC-ER-13: unknown command"
expect_failure "TC-ER-13" "$BACKUPCTL" frobnicate a b

echo "[test] TC-CLI-01: --help exits 0"
"$BACKUPCTL" --help >/dev/null || fail "TC-CLI-01: --help failed"
pass "TC-CLI-01"

# ---- Cleanup -----------------------------------------------------------

if [[ -n "${KEEP_TESTDATA:-}" ]]; then
  echo "[test] Keeping test data in $TEST_ROOT (KEEP_TESTDATA is set)."
else
  rm -rf "$TEST_ROOT"
fi

echo "[test] All test cases passed."
