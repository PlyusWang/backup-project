#!/usr/bin/env bash
#
# Sprint 1 quality regression suite.
#
# Purpose:
#   Turn the current backup/restore implementation into repeatable evidence for
#   usability, robustness, stability, and runtime safety. This supplements the
#   functional cases in scripts/test.sh; it does not replace them.
#
# Results are written under tests/output/quality/<UTC timestamp>/, which is
# already covered by the repository's test-output ignore rule.
#
# Exit status:
#   0  every quality check passed
#   1  one or more checks failed
#
# Optional environment variables:
#   QUALITY_REPEAT       repeated round trips for stability testing (default 10)
#   QUALITY_LARGE_MB     large binary size in MiB (default 16)
#   QUALITY_MANY_FILES   number of small files in stress dataset (default 500)
#   QUALITY_TIMEOUT      timeout per potentially dangerous command (default 20s)
#   QUALITY_RESULT_ROOT  result root directory

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BACKUPCTL="$ROOT_DIR/build/backupctl"
SANITIZED_BACKUPCTL="$ROOT_DIR/build-sanitize/backupctl"
RESULT_ROOT="${QUALITY_RESULT_ROOT:-$ROOT_DIR/tests/output/quality}"
REPEAT="${QUALITY_REPEAT:-10}"
LARGE_MB="${QUALITY_LARGE_MB:-16}"
MANY_FILES="${QUALITY_MANY_FILES:-500}"
COMMAND_TIMEOUT="${QUALITY_TIMEOUT:-20}"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
RESULT_DIR="$RESULT_ROOT/$STAMP"
WORK_DIR="$RESULT_DIR/work"
LOG_FILE="$RESULT_DIR/quality.log"
SUMMARY_FILE="$RESULT_DIR/summary.md"
ENV_FILE="$RESULT_DIR/environment.txt"
BASELINE_LOG="$RESULT_DIR/baseline.log"
SANITIZER_LOG="$RESULT_DIR/sanitizer.log"

mkdir -p "$WORK_DIR"
: > "$LOG_FILE"

PASS_COUNT=0
FAIL_COUNT=0
SKIP_COUNT=0
BASELINE_CASES=0
STABILITY_COMPLETED=0
STRESS_SECONDS="n/a"
declare -a FAILURES=()

log() {
  printf '%s\n' "$*" | tee -a "$LOG_FILE"
}

pass() {
  PASS_COUNT=$((PASS_COUNT + 1))
  log "  PASS: $1"
}

fail() {
  FAIL_COUNT=$((FAIL_COUNT + 1))
  FAILURES+=("$1")
  log "  FAIL: $1"
}

skip() {
  SKIP_COUNT=$((SKIP_COUNT + 1))
  log "  SKIP: $1"
}

command_status() {
  set +e
  "$@"
  local status=$?
  set -e 2>/dev/null || true
  return "$status"
}

run_timed() {
  timeout "${COMMAND_TIMEOUT}s" "$@"
}

require_tool() {
  if ! command -v "$1" >/dev/null 2>&1; then
    fail "ENV-$1: required tool is missing"
    return 1
  fi
  return 0
}

hash_tree() {
  local root="$1"
  (
    cd "$root" || exit 1
    find . -type f -print0 \
      | sort -z \
      | xargs -0 -r sha256sum
  )
}

round_trip() {
  local binary="$1"
  local source="$2"
  local repository="$3"
  local restored="$4"

  "$binary" backup "$source" "$repository" >/dev/null 2>&1 || return 1
  "$binary" restore "$repository" "$restored" >/dev/null 2>&1 || return 1
  diff -r "$source" "$restored" >/dev/null 2>&1 || return 1

  hash_tree "$source" > "$WORK_DIR/source.sha256" || return 1
  hash_tree "$restored" > "$WORK_DIR/restored.sha256" || return 1
  cmp -s "$WORK_DIR/source.sha256" "$WORK_DIR/restored.sha256" || return 1
  return 0
}

write_summary() {
  {
    echo "# Sprint 1 Quality Regression Result"
    echo
    echo "- UTC timestamp: \`$STAMP\`"
    echo "- Git commit: \`$(git -C "$ROOT_DIR" rev-parse HEAD 2>/dev/null || echo unknown)\`"
    echo "- Passed checks: **$PASS_COUNT**"
    echo "- Failed checks: **$FAIL_COUNT**"
    echo "- Skipped checks: **$SKIP_COUNT**"
    echo "- Baseline cases observed: **$BASELINE_CASES**"
    echo "- Stability round trips completed: **$STABILITY_COMPLETED / $REPEAT**"
    echo "- Stress dataset: **${MANY_FILES} small files + ${LARGE_MB} MiB binary**"
    echo "- Stress round-trip elapsed: **${STRESS_SECONDS}s**"
    echo
    echo "## Quality dimensions"
    echo
    echo "- Usability: CLI help, usage diagnostics, and exit-code behavior."
    echo "- Robustness: boundary datasets, unsupported/dangerous path topology, and failure containment."
    echo "- Stability: repeated backup -> restore -> byte-for-byte verification."
    echo "- Runtime safety: AddressSanitizer + UndefinedBehaviorSanitizer smoke round trip."
    echo
    echo "## Failures"
    echo
    if [[ ${#FAILURES[@]} -eq 0 ]]; then
      echo "None."
    else
      local item
      for item in "${FAILURES[@]}"; do
        echo "- $item"
      done
    fi
    echo
    echo "## Evidence files"
    echo
    echo "- \`quality.log\`: quality-suite execution record"
    echo "- \`baseline.log\`: existing Sprint 1 functional suite output"
    echo "- \`sanitizer.log\`: sanitizer build/runtime output"
    echo "- \`environment.txt\`: compiler/OS/toolchain snapshot"
  } > "$SUMMARY_FILE"
}

trap write_summary EXIT

log "[quality] Sprint 1 quality regression suite"
log "[quality] results: $RESULT_DIR"

# ----------------------------------------------------------------------
# Environment evidence
# ----------------------------------------------------------------------
{
  echo "timestamp_utc=$STAMP"
  echo "git_commit=$(git -C "$ROOT_DIR" rev-parse HEAD 2>/dev/null || echo unknown)"
  echo "kernel=$(uname -srvmo 2>/dev/null || uname -a)"
  echo "g++=$(g++ --version 2>/dev/null | head -n 1 || true)"
  echo "make=$(make --version 2>/dev/null | head -n 1 || true)"
  echo "bash=$BASH_VERSION"
  echo "ulimit_n=$(ulimit -n 2>/dev/null || echo unknown)"
} > "$ENV_FILE"

for tool in make g++ diff cmp sha256sum timeout find sort xargs; do
  require_tool "$tool" || true
done

if [[ $FAIL_COUNT -ne 0 ]]; then
  log "[quality] environment prerequisites are incomplete; continuing where possible"
fi

# ----------------------------------------------------------------------
# Build and existing Sprint 1 regression suite
# ----------------------------------------------------------------------
log "[quality] BUILD-01: clean release build"
if make -C "$ROOT_DIR" clean >/dev/null 2>&1 && make -C "$ROOT_DIR" all >>"$LOG_FILE" 2>&1; then
  pass "BUILD-01: release build succeeds with repository warning flags"
else
  fail "BUILD-01: release build failed"
fi

log "[quality] BASE-01: existing Sprint 1 functional suite"
set +e
bash "$ROOT_DIR/scripts/test.sh" >"$BASELINE_LOG" 2>&1
baseline_status=$?
set -e 2>/dev/null || true
BASELINE_CASES="$(grep -c 'PASS:' "$BASELINE_LOG" 2>/dev/null || true)"
if [[ $baseline_status -eq 0 ]]; then
  pass "BASE-01: scripts/test.sh passes ($BASELINE_CASES PASS records)"
else
  fail "BASE-01: scripts/test.sh failed with exit code $baseline_status"
fi

# ----------------------------------------------------------------------
# Usability: predictable CLI contract and readable diagnostics
# ----------------------------------------------------------------------
log "[quality] USE-01: --help contract"
set +e
help_output="$("$BACKUPCTL" --help 2>&1)"
help_status=$?
set -e 2>/dev/null || true
if [[ $help_status -eq 0 && "$help_output" == *"Usage:"* && \
      "$help_output" == *"backup"* && "$help_output" == *"restore"* ]]; then
  pass "USE-01: help is discoverable and exits 0"
else
  fail "USE-01: help output or exit code is inconsistent"
fi

log "[quality] USE-02: missing-argument diagnostics"
set +e
usage_output="$("$BACKUPCTL" backup "$WORK_DIR/no-source" 2>&1)"
usage_status=$?
set -e 2>/dev/null || true
if [[ $usage_status -eq 2 && "$usage_output" == *"expects"* && \
      "$usage_output" == *"Usage:"* ]]; then
  pass "USE-02: usage errors return 2 with actionable diagnostics"
else
  fail "USE-02: missing-argument behavior is not CLI-contract compliant"
fi

# ----------------------------------------------------------------------
# Robustness: high-volume/boundary data without corruption
# ----------------------------------------------------------------------
log "[quality] ROB-01: many files + large binary + deep tree round trip"
stress="$WORK_DIR/stress"
stress_source="$stress/source"
stress_repo="$stress/repository"
stress_restored="$stress/restored"
mkdir -p "$stress_source/many" "$stress_source/deep"

i=1
while [[ $i -le $MANY_FILES ]]; do
  printf 'small-file-%06d\n' "$i" > "$stress_source/many/file-$(printf '%06d' "$i").txt"
  i=$((i + 1))
done
head -c "$((LARGE_MB * 1024 * 1024))" /dev/urandom > "$stress_source/large.bin"
printf 'space path\n' > "$stress_source/file with spaces.txt"
printf 'UTF-8 中文路径\n' > "$stress_source/中文文件.txt"

deep="$stress_source/deep"
for i in $(seq 1 24); do
  deep="$deep/level-$(printf '%02d' "$i")"
  mkdir -p "$deep"
done
printf 'deep leaf\n' > "$deep/leaf.txt"

stress_start="$(date +%s)"
if round_trip "$BACKUPCTL" "$stress_source" "$stress_repo" "$stress_restored"; then
  STRESS_SECONDS="$(( $(date +%s) - stress_start ))"
  pass "ROB-01: stress round trip is byte-identical"
else
  STRESS_SECONDS="$(( $(date +%s) - stress_start ))"
  fail "ROB-01: stress round trip failed or data differed"
fi

# ----------------------------------------------------------------------
# Stability: repeat the same operation many times with fresh repositories
# ----------------------------------------------------------------------
log "[quality] STB-01: repeated round trips ($REPEAT iterations)"
stable_source="$WORK_DIR/stability/source"
mkdir -p "$stable_source/a/b" "$stable_source/empty"
printf 'stable text\n' > "$stable_source/a/text.txt"
head -c 1048576 /dev/urandom > "$stable_source/a/b/random.bin"

stability_ok=1
for i in $(seq 1 "$REPEAT"); do
  repo="$WORK_DIR/stability/repository-$i"
  restored="$WORK_DIR/stability/restored-$i"
  if round_trip "$BACKUPCTL" "$stable_source" "$repo" "$restored"; then
    STABILITY_COMPLETED=$i
  else
    stability_ok=0
    break
  fi
done
if [[ $stability_ok -eq 1 ]]; then
  pass "STB-01: $REPEAT/$REPEAT repeated round trips succeeded"
else
  fail "STB-01: repeated round trip failed at iteration $((STABILITY_COMPLETED + 1))"
fi

# ----------------------------------------------------------------------
# Robustness: dangerous topology must be rejected before recursive copying
# ----------------------------------------------------------------------
log "[quality] ROB-02: repository located inside source directory"
self_root="$WORK_DIR/self-backup"
self_source="$self_root/source"
self_repo="$self_source/repository"
mkdir -p "$self_source"
printf 'self recursion guard\n' > "$self_source/file.txt"

set +e
run_timed "$BACKUPCTL" backup "$self_source" "$self_repo" \
  >"$RESULT_DIR/self-backup.stdout" 2>"$RESULT_DIR/self-backup.stderr"
self_status=$?
set -e 2>/dev/null || true

if [[ $self_status -eq 124 ]]; then
  fail "ROB-02: self-nested backup timed out instead of being rejected"
elif [[ $self_status -eq 0 ]]; then
  fail "ROB-02: self-nested backup incorrectly reported success"
elif [[ $self_status -ge 128 ]]; then
  fail "ROB-02: self-nested backup crashed (exit $self_status)"
elif [[ -e "$self_repo/data/repository/data" ]]; then
  fail "ROB-02: self-nested backup recursed into its own repository before failing"
else
  pass "ROB-02: self-nested backup is rejected without recursive side effects"
fi

log "[quality] ROB-03: restore destination located inside repository data"
restore_root="$WORK_DIR/self-restore"
restore_source="$restore_root/source"
restore_repo="$restore_root/repository"
restore_dest="$restore_repo/data/restored-inside-data"
mkdir -p "$restore_source"
printf 'restore recursion guard\n' > "$restore_source/file.txt"

if "$BACKUPCTL" backup "$restore_source" "$restore_repo" >/dev/null 2>&1; then
  set +e
  run_timed "$BACKUPCTL" restore "$restore_repo" "$restore_dest" \
    >"$RESULT_DIR/self-restore.stdout" 2>"$RESULT_DIR/self-restore.stderr"
  restore_status=$?
  set -e 2>/dev/null || true

  if [[ $restore_status -eq 124 ]]; then
    fail "ROB-03: self-nested restore timed out instead of being rejected"
  elif [[ $restore_status -eq 0 ]]; then
    fail "ROB-03: self-nested restore incorrectly reported success"
  elif [[ $restore_status -ge 128 ]]; then
    fail "ROB-03: self-nested restore crashed (exit $restore_status)"
  elif [[ -e "$restore_dest/restored-inside-data" ]]; then
    fail "ROB-03: restore recursed into its own destination before failing"
  else
    pass "ROB-03: self-nested restore is rejected without recursive side effects"
  fi
else
  fail "ROB-03: setup backup failed, restore topology check could not run"
fi

# ----------------------------------------------------------------------
# Runtime safety: sanitizer build + representative round trip
# ----------------------------------------------------------------------
log "[quality] SAN-01: AddressSanitizer + UndefinedBehaviorSanitizer build"
set +e
make -C "$ROOT_DIR" sanitize >"$SANITIZER_LOG" 2>&1
sanitize_build_status=$?
set -e 2>/dev/null || true
if [[ $sanitize_build_status -eq 0 && -x "$SANITIZED_BACKUPCTL" ]]; then
  pass "SAN-01: sanitizer build succeeds"
else
  fail "SAN-01: sanitizer build failed"
fi

log "[quality] SAN-02: sanitizer runtime smoke round trip"
if [[ -x "$SANITIZED_BACKUPCTL" ]]; then
  san_source="$WORK_DIR/sanitizer/source"
  san_repo="$WORK_DIR/sanitizer/repository"
  san_restored="$WORK_DIR/sanitizer/restored"
  mkdir -p "$san_source/nested" "$san_source/empty"
  printf 'sanitizer text\n' > "$san_source/nested/text.txt"
  head -c 2097152 /dev/urandom > "$san_source/random.bin"

  set +e
  ASAN_OPTIONS="detect_leaks=1:halt_on_error=1" \
  UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
    "$SANITIZED_BACKUPCTL" backup "$san_source" "$san_repo" \
      >>"$SANITIZER_LOG" 2>&1
  san_backup_status=$?
  if [[ $san_backup_status -eq 0 ]]; then
    ASAN_OPTIONS="detect_leaks=1:halt_on_error=1" \
    UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
      "$SANITIZED_BACKUPCTL" restore "$san_repo" "$san_restored" \
        >>"$SANITIZER_LOG" 2>&1
    san_restore_status=$?
  else
    san_restore_status=1
  fi
  set -e 2>/dev/null || true

  if [[ $san_backup_status -eq 0 && $san_restore_status -eq 0 ]] && \
      diff -r "$san_source" "$san_restored" >/dev/null 2>&1 && \
      ! grep -Eq 'ERROR: AddressSanitizer|runtime error:' "$SANITIZER_LOG"; then
    pass "SAN-02: sanitizer smoke round trip reports no ASan/UBSan error"
  else
    fail "SAN-02: sanitizer runtime failed or reported a memory/UB error"
  fi
else
  skip "SAN-02: sanitizer binary unavailable"
fi

log "[quality] completed: pass=$PASS_COUNT fail=$FAIL_COUNT skip=$SKIP_COUNT"

if [[ $FAIL_COUNT -eq 0 ]]; then
  exit 0
fi
exit 1
