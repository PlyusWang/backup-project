#!/usr/bin/env bash
# Sprint 1 quality regression suite for backupctl.
# Scope: usability, robustness, stability, and runtime safety.
# Results are written to tests/output/quality/<UTC timestamp>/.
# This suite supplements scripts/test.sh instead of duplicating it.
# Exit 0 means every required check passed; exit 1 means at least one failed.
# Tunables are exposed only through QUALITY_* environment variables.

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BACKUPCTL="$ROOT_DIR/build/backupctl"
SANITIZED_BACKUPCTL="$ROOT_DIR/build-sanitize/backupctl"
RESULT_ROOT="${QUALITY_RESULT_ROOT:-$ROOT_DIR/tests/output/quality}"
REPEAT="${QUALITY_REPEAT:-10}"
LARGE_MB="${QUALITY_LARGE_MB:-16}"
MANY_FILES="${QUALITY_MANY_FILES:-500}"
COMMAND_TIMEOUT="${QUALITY_TIMEOUT:-20}"
COMMENT_MIN=20
COMMENT_MAX=23
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

# Every PASS/FAIL line has a stable case identifier supplied by its caller.
# Counters are updated only through pass(), fail(), and skip().
# A failure does not abort immediately because later checks may provide more evidence.
# The final process status still becomes non-zero if any required check fails.
# Keep all visible test output in one log so a failed run remains auditable.
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

# timeout is used only where recursion or filesystem topology could make progress unbounded.
# A timeout is treated as a test failure, never as an acceptable operational outcome.
# Potentially recursive path-topology cases must never hang the whole suite.
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

# find emits only regular files because Sprint 1 does not preserve metadata.
# NUL separators keep spaces, tabs, and UTF-8 path bytes safe through the pipeline.
# sort -z removes filesystem enumeration order from the comparison.
# sha256sum gives content evidence independent of diff's textual presentation.
# Hash files in a deterministic path order; directory timestamps are irrelevant.
hash_tree() {
  local root="$1"
  (
    cd "$root" || exit 1
    find . -type f -print0 | sort -z | xargs -0 -r sha256sum
  )
}

# The source, repository, and destination must be fresh for each invocation.
# backup and restore must both return success before content comparison is attempted.
# diff -r detects missing files, extra files, and empty-directory structure changes.
# Hash comparison adds an explicit byte-integrity check over all regular files.
# One round trip checks both tree shape and byte-level file content.
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
}

# STYLE-01 exists to keep this script itself reviewable as it grows across Sprints.
# Blank lines are excluded so formatting changes cannot manipulate the ratio.
# The shebang is executable metadata rather than explanatory documentation.
# Only full-line comments count; inline comments are excluded to avoid encouraging clutter.
# Integer basis points avoid a dependency on bc or Python.
# The accepted interval is intentionally narrow: 20.00% through 23.00%.
# Comment ratio is measured on non-empty lines; shebang and inline comments do not count.
check_comment_ratio() {
  local script_path="$ROOT_DIR/scripts/quality_test.sh"
  local nonempty comments ratio

  nonempty="$(awk 'NF {count++} END {print count+0}' "$script_path")"
  comments="$(awk 'NF && NR != 1 && $0 ~ /^[[:space:]]*#/ {count++} END {print count+0}' "$script_path")"
  ratio=$((comments * 10000 / (nonempty - 1)))

  if (( ratio < COMMENT_MIN * 100 || ratio > COMMENT_MAX * 100 )); then
    fail "STYLE-01: comment ratio is $((ratio / 100)).$((ratio % 100))%, expected ${COMMENT_MIN}%-${COMMENT_MAX}%"
    return 1
  fi
  pass "STYLE-01: comment ratio is $((ratio / 100)).$((ratio % 100))%"
}

# The Markdown summary is designed for direct archival in a release or course report.
# It records counts instead of claiming broad quality properties from a single run.
# Git SHA ties the evidence to an exact source revision.
# Stress parameters are recorded because changing them changes the strength of the evidence.
# The summary is generated even on failure through the EXIT trap.
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
    echo "## Evidence"
    echo
    echo "- \`quality.log\`: complete suite output"
    echo "- \`baseline.log\`: existing Sprint 1 functional suite"
    echo "- \`sanitizer.log\`: sanitizer build and runtime output"
    echo "- \`environment.txt\`: toolchain and host snapshot"
  } > "$SUMMARY_FILE"
}

trap write_summary EXIT

log "[quality] Sprint 1 quality regression suite"
log "[quality] results: $RESULT_DIR"

# Environment capture is diagnostic evidence, not a pass/fail criterion by itself.
# Compiler and Make versions explain build differences between local VM and CI.
# Kernel information matters because the implementation uses POSIX filesystem primitives.
# The file-descriptor limit is recorded for future large-tree regressions.
# No hostname, username, home path, or other unnecessary personal information is archived.
# Record enough environment data to make later reruns comparable.
{
  echo "timestamp_utc=$STAMP"
  echo "git_commit=$(git -C "$ROOT_DIR" rev-parse HEAD 2>/dev/null || echo unknown)"
  echo "kernel=$(uname -srvmo 2>/dev/null || uname -a)"
  echo "g++=$(g++ --version 2>/dev/null | head -n 1 || true)"
  echo "make=$(make --version 2>/dev/null | head -n 1 || true)"
  echo "bash=$BASH_VERSION"
  echo "ulimit_n=$(ulimit -n 2>/dev/null || echo unknown)"
} > "$ENV_FILE"

for tool in make g++ diff cmp sha256sum timeout find sort xargs awk; do
  require_tool "$tool" || true
done

check_comment_ratio || true

# The clean step prevents stale object files from hiding dependency or compile problems.
# This uses the repository Makefile rather than a second build definition.
# Compiler warnings remain enabled through the project's existing CXXFLAGS.
# Build stdout/stderr is retained in the quality log for diagnosis.
# BUILD-01 verifies the repository's normal warning-enabled build path.
log "[quality] BUILD-01: clean release build"
if make -C "$ROOT_DIR" clean >/dev/null 2>&1 &&
   make -C "$ROOT_DIR" all >>"$LOG_FILE" 2>&1; then
  pass "BUILD-01: release build succeeds"
else
  fail "BUILD-01: release build failed"
fi

# Reusing scripts/test.sh avoids duplicating its normal and error-path cases here.
# Its own exit status is authoritative; PASS count is recorded only as evidence metadata.
# A baseline failure does not suppress later quality probes.
# BASE-01 preserves Sprint 1's existing regression suite as the functional baseline.
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

# Usability here is limited to the current CLI contract, not subjective interface scoring.
# Help must mention both supported operations so a user can discover the core workflow.
# Help is expected to use exit code 0 because requesting help is not an error.
# The test ignores exact whitespace to avoid brittle presentation-only failures.
# USE-01 requires discoverable help and a successful help exit status.
log "[quality] USE-01: --help contract"
set +e
help_output="$("$BACKUPCTL" --help 2>&1)"
help_status=$?
set -e 2>/dev/null || true
if [[ $help_status -eq 0 &&
      "$help_output" == *"Usage:"* &&
      "$help_output" == *"backup"* &&
      "$help_output" == *"restore"* ]]; then
  pass "USE-01: help is discoverable and exits 0"
else
  fail "USE-01: help output or exit code is inconsistent"
fi

# Exit code 2 is part of backupctl's documented distinction for usage errors.
# The diagnostic must state what is missing and also show the usage form.
# The intentionally nonexistent source path must never be inspected in this branch.
# USE-02 checks that malformed commands fail predictably instead of reaching file I/O.
log "[quality] USE-02: missing-argument diagnostics"
set +e
usage_output="$("$BACKUPCTL" backup "$WORK_DIR/no-source" 2>&1)"
usage_status=$?
set -e 2>/dev/null || true
if [[ $usage_status -eq 2 &&
      "$usage_output" == *"expects"* &&
      "$usage_output" == *"Usage:"* ]]; then
  pass "USE-02: usage errors return 2 with actionable diagnostics"
else
  fail "USE-02: missing-argument behavior violates the CLI contract"
fi

# This is a moderate stress case suitable for routine execution on a course VM.
# Many small files exercise directory traversal and repeated open/close paths.
# The large binary crosses the 64 KiB copy buffer many times.
# Deep nesting exercises recursive path construction without approaching OS path limits.
# Spaces and UTF-8 names verify that shell quoting and raw path handling stay correct.
# The empty directory verifies structure preservation beyond file-content hashing.
# ROB-01 combines file-count, file-size, depth, spaces, UTF-8, and empty directories.
log "[quality] ROB-01: stress round trip"
stress="$WORK_DIR/stress"
stress_source="$stress/source"
stress_repo="$stress/repository"
stress_restored="$stress/restored"
mkdir -p "$stress_source/many" "$stress_source/deep" "$stress_source/empty"

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

# Each iteration uses a new repository and destination to avoid intentional overwrite refusal.
# The source is held constant so failures can be compared across iterations.
# A random binary file keeps the test representative of non-text data.
# Ten repetitions are a regression signal, not a statistical reliability guarantee.
# The completed iteration count is written into the summary on first failure.
# STB-01 repeats fresh round trips to expose state leakage and nondeterministic failure.
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
  fail "STB-01: failed at iteration $((STABILITY_COMPLETED + 1))"
fi

# Without an ancestor/descendant guard, recursive copy can consume its own output.
# The expected behavior is fast rejection before nested repository/data trees appear.
# A timeout catches unbounded recursion even if the process does not crash.
# Signal exits are distinguished from ordinary non-zero operational failures.
# Side-effect inspection checks that rejection happened before recursive damage accumulated.
# ROB-02 probes a dangerous topology: repository nested below its own source.
log "[quality] ROB-02: repository inside source"
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
  fail "ROB-02: self-nested backup timed out"
elif [[ $self_status -eq 0 ]]; then
  fail "ROB-02: self-nested backup incorrectly reported success"
elif [[ $self_status -ge 128 ]]; then
  fail "ROB-02: self-nested backup crashed (exit $self_status)"
elif [[ -e "$self_repo/data/repository/data" ]]; then
  fail "ROB-02: recursive side effects appeared before rejection"
else
  pass "ROB-02: self-nested backup is rejected safely"
fi

# Restore has the symmetric risk of copying a destination that becomes part of its source tree.
# Setup first creates a valid repository through the public backupctl interface.
# The expected behavior is again rejection before recursive destination nesting appears.
# A setup failure is reported separately because it invalidates the topology probe.
# This case is intentionally black-box and does not depend on private C++ APIs.
# ROB-03 mirrors the topology check on restore, where destination sits in repository/data.
log "[quality] ROB-03: destination inside repository data"
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
    fail "ROB-03: self-nested restore timed out"
  elif [[ $restore_status -eq 0 ]]; then
    fail "ROB-03: self-nested restore incorrectly reported success"
  elif [[ $restore_status -ge 128 ]]; then
    fail "ROB-03: self-nested restore crashed (exit $restore_status)"
  elif [[ -e "$restore_dest/restored-inside-data" ]]; then
    fail "ROB-03: recursive side effects appeared before rejection"
  else
    pass "ROB-03: self-nested restore is rejected safely"
  fi
else
  fail "ROB-03: setup backup failed"
fi

# Sanitizers complement functional checks by observing memory and undefined-behavior faults.
# The project Makefile owns the sanitizer flags, keeping this script free of compiler duplication.
# A successful build alone is not treated as runtime evidence.
# The dedicated build directory prevents instrumented objects from replacing release objects.
# SAN-01 proves the same sources compile with ASan and UBSan instrumentation.
log "[quality] SAN-01: sanitizer build"
set +e
make -C "$ROOT_DIR" sanitize >"$SANITIZER_LOG" 2>&1
sanitize_build_status=$?
set -e 2>/dev/null || true
if [[ $sanitize_build_status -eq 0 && -x "$SANITIZED_BACKUPCTL" ]]; then
  pass "SAN-01: sanitizer build succeeds"
else
  fail "SAN-01: sanitizer build failed"
fi

# The smoke dataset includes nesting, an empty directory, text, and a multi-buffer binary file.
# halt_on_error makes sanitizer findings propagate into an observable non-zero status.
# Leak detection is enabled because file-descriptor RAII is a stated implementation concern.
# The restored tree must still pass diff; sanitizer cleanliness alone is insufficient.
# Log scanning catches diagnostic text even if a runtime configuration returns unexpectedly.
# SAN-02 runs representative I/O through instrumentation and scans diagnostics.
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
    "$SANITIZED_BACKUPCTL" backup "$san_source" "$san_repo" >>"$SANITIZER_LOG" 2>&1
  san_backup_status=$?
  if [[ $san_backup_status -eq 0 ]]; then
    ASAN_OPTIONS="detect_leaks=1:halt_on_error=1" \
    UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
      "$SANITIZED_BACKUPCTL" restore "$san_repo" "$san_restored" >>"$SANITIZER_LOG" 2>&1
    san_restore_status=$?
  else
    san_restore_status=1
  fi
  set -e 2>/dev/null || true

  if [[ $san_backup_status -eq 0 &&
        $san_restore_status -eq 0 ]] &&
     diff -r "$san_source" "$san_restored" >/dev/null 2>&1 &&
     ! grep -Eq 'ERROR: AddressSanitizer|runtime error:' "$SANITIZER_LOG"; then
    pass "SAN-02: no ASan/UBSan error detected"
  else
    fail "SAN-02: sanitizer runtime failed or reported memory/UB error"
  fi
else
  skip "SAN-02: sanitizer binary unavailable"
fi

log "[quality] completed: pass=$PASS_COUNT fail=$FAIL_COUNT skip=$SKIP_COUNT"

if [[ $FAIL_COUNT -eq 0 ]]; then
  exit 0
fi
exit 1
