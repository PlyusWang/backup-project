#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$ROOT_DIR/packaging/lib/common.sh"
TEST_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/bp-package-maintainer.XXXXXX")"
trap 'rm -rf "$TEST_ROOT"' EXIT

record_pass() { printf 'PASS %s\n' "$1"; }
fail() { printf 'FAIL %s\n' "$1" >&2; exit 1; }

# Known byte lengths exercise per-file rounding and ensure DEBIAN is excluded.
STAGE="$TEST_ROOT/debroot"
mkdir -p "$STAGE/DEBIAN" "$STAGE/usr/share/nested"
truncate -s 1 "$STAGE/usr/share/one-byte"
truncate -s 1024 "$STAGE/usr/share/one-kib"
truncate -s 1025 "$STAGE/usr/share/rounds-up"
truncate -s 2049 "$STAGE/usr/share/nested/two-kib-rounds-up"
truncate -s 10485760 "$STAGE/DEBIAN/control"
ln -s one-byte "$STAGE/usr/share/link"
actual="$(installed_size_kib "$STAGE")"
[ "$actual" = 11 ] || fail "Installed-Size expected 11 KiB, got $actual"
record_pass "Installed-Size uses per-object KiB rounding and excludes DEBIAN control files"

actual="$(installed_size_kib "$STAGE/")"
[ "$actual" = 11 ] || fail "trailing slash counted DEBIAN contents: $actual KiB"
record_pass "Installed-Size accepts staging paths with a trailing slash"

ln "$STAGE/usr/share/one-byte" "$STAGE/usr/share/nested/hardlink"
ln -P "$STAGE/usr/share/link" "$STAGE/usr/share/nested/hardlinked-symlink"
: > "$STAGE/usr/share/empty"
actual="$(installed_size_kib "$STAGE")"
[ "$actual" = 11 ] || fail "hardlinks or empty files were counted twice: $actual KiB"
record_pass "Installed-Size deduplicates file and symlink hardlinks and handles empty files"

cat > "$STAGE/DEBIAN/control" <<EOF
Package: backup-project-installed-size-fixture
Version: 1.0-1
Architecture: all
Maintainer: Backup Project <noreply@example.invalid>
Description: Installed-Size regression fixture
Installed-Size: $actual
EOF
dpkg-deb --build --root-owner-group "$STAGE" "$TEST_ROOT/fixture.deb" > "$TEST_ROOT/dpkg-deb.log" 2>&1
actual="$(dpkg-deb -f "$TEST_ROOT/fixture.deb" Installed-Size)"
[ "$actual" = 11 ] || fail "built deb has incorrect Installed-Size: $actual"
record_pass "built deb contains the expected KiB Installed-Size field"

# Exercise the actual postinst functions with mocks. No service commands are run
# on the host and the full maintainer script is never executed.
POSTINST="$ROOT_DIR/packaging/server/deb/postinst"
FUNCTIONS="$TEST_ROOT/postinst-functions.sh"
: > "$FUNCTIONS"
for function_name in restart_and_check start_and_check record_rollback_state \
                     check_rolled_back_state fail_and_maybe_rollback; do
  sed -n "/^$function_name() {$/,/^}$/p" "$POSTINST" >> "$FUNCTIONS"
done
# The test intentionally sources extracted functions from a temporary file.
# shellcheck source=/dev/null
source "$FUNCTIONS"
for function_name in restart_and_check start_and_check record_rollback_state \
                     check_rolled_back_state fail_and_maybe_rollback; do
  declare -F "$function_name" > /dev/null || fail "could not extract $function_name"
done

CALLS="$TEST_ROOT/systemctl-calls"
ERRORS="$TEST_ROOT/errors"
PAYLOAD="$TEST_ROOT/restored-payload"
ROLLBACK_DIR="$TEST_ROOT/rollback"
mkdir -p "$ROLLBACK_DIR"
# Exported settings are consumed by the extracted postinst functions.
export SERVICE=backup-project-server OLD_VERSION=1.0-1
export CONF_DIR="$TEST_ROOT/config" INSTANCE_ROOT="$TEST_ROOT/instance"
export CURRENT_VERSION=2.0-1 ENABLE_FAILURE=1 MOCK_HEALTHY=1 MOCK_RESTORE_RC=0
: > "$CALLS"
: > "$ERRORS"

err() { printf '%s\n' "$*" >> "$ERRORS"; }
systemctl() {
  printf '%s\n' "$*" >> "$CALLS"
  [ "$1" != enable ] || [ "$ENABLE_FAILURE" -eq 0 ]
}
service_healthy() { [ "$MOCK_HEALTHY" -eq 1 ]; }
service_state() { printf 'active\n'; }
conf_value() { printf '18765\n'; }
dpkg_version() { printf '%s\n' "$CURRENT_VERSION"; }
rollback_available() { return 0; }
restore_payload() {
  [ -s "$ROLLBACK_DIR/rolled-back" ] || {
    printf 'restore-without-version-state\n' >> "$CALLS"
    return 1
  }
  printf 'restore-payload\n' >> "$CALLS"
  printf 'old payload\n' > "$PAYLOAD"
  return "$MOCK_RESTORE_RC"
}

if start_and_check; then
  fail "enable failure was treated as successful start"
fi
! grep -q '^restart ' "$CALLS" || fail "restart ran after enable failure"
grep -q '无法启用服务自启动' "$ERRORS" || fail "enable failure has no diagnostic"
record_pass "systemctl enable failure stops a new installation before restart"

: > "$CALLS"
if ( fail_and_maybe_rollback "simulated enable failure" ) > "$TEST_ROOT/rollback.log" 2>&1; then
  fail "failed upgrade was reported as successful after rollback"
fi
[ -f "$PAYLOAD" ] || fail "rollback did not restore the old payload"
grep -q '^restart ' "$CALLS" || fail "persistent enable failure blocked rollback restart"
! grep -q '^enable ' "$CALLS" || fail "rollback retried enable instead of restoring service"
! grep -q 'restore-without-version-state' "$CALLS" || fail "payload changed before version state was recorded"
IFS=$'\t' read -r restored_version failed_version < "$ROLLBACK_DIR/rolled-back"
[ "$restored_version" = "$OLD_VERSION" ] && [ "$failed_version" = "$CURRENT_VERSION" ] ||
  fail "rollback did not record old and failed package versions"
record_pass "rollback persists version state before restoring and restarts despite enable failure"

ENABLE_FAILURE=0
: > "$CALLS"
if ( check_rolled_back_state ) > "$TEST_ROOT/retry.log" 2>&1; then
  fail "configure retry accepted old payload as the failed new version"
fi
[ -s "$ROLLBACK_DIR/rolled-back" ] || fail "rejected retry discarded the rollback state"
[ ! -s "$CALLS" ] || fail "configure retry started service before rejecting mismatched payload"
record_pass "configure retry cannot commit restored old payload under new package metadata"

for failure_case in restore health; do
  MOCK_RESTORE_RC=0
  MOCK_HEALTHY=1
  case "$failure_case" in
    restore) MOCK_RESTORE_RC=1 ;;
    health) MOCK_HEALTHY=0 ;;
  esac
  rm -f "$PAYLOAD" "$ROLLBACK_DIR/rolled-back"
  : > "$CALLS"
  if ( fail_and_maybe_rollback "simulated rollback $failure_case failure" ) \
       > "$TEST_ROOT/rollback-$failure_case.log" 2>&1; then
    fail "rollback $failure_case failure was reported as success"
  fi
  [ -s "$ROLLBACK_DIR/rolled-back" ] || fail "$failure_case failure lost rollback version state"
  if ( check_rolled_back_state ) > "$TEST_ROOT/retry-$failure_case.log" 2>&1; then
    fail "configure retry accepted payload after rollback $failure_case failure"
  fi
  record_pass "rollback $failure_case failure retains state and blocks configure retry"
done

MOCK_RESTORE_RC=0
MOCK_HEALTHY=1
rm -f "$PAYLOAD"
ROLLBACK_DIR="$TEST_ROOT/absent/rollback"
: > "$CALLS"
if ( fail_and_maybe_rollback "simulated unwritable rollback state" ) \
     > "$TEST_ROOT/rollback-state-write.log" 2>&1; then
  fail "rollback without persistent version state was reported as success"
fi
[ ! -e "$PAYLOAD" ] || fail "rollback changed payload after failing to write version state"
! grep -q '^restart ' "$CALLS" || fail "rollback restarted service without recording version state"
record_pass "rollback state write failure leaves payload untouched"

ROLLBACK_DIR="$TEST_ROOT/rollback"
CURRENT_VERSION=""
rm -f "$PAYLOAD"
if ( fail_and_maybe_rollback "simulated unavailable dpkg version" ) \
     > "$TEST_ROOT/rollback-version-read.log" 2>&1; then
  fail "rollback with unknown package version was reported as success"
fi
[ ! -e "$PAYLOAD" ] || fail "rollback changed payload without knowing failed package version"
record_pass "unknown package version prevents an untracked payload rollback"

CURRENT_VERSION=2.0-1
: > "$ROLLBACK_DIR/rolled-back"
if ( check_rolled_back_state ) > "$TEST_ROOT/retry-malformed.log" 2>&1; then
  fail "incomplete rollback state was silently accepted"
fi
record_pass "incomplete rollback state fails closed"

printf '%s\t%s\n' "$OLD_VERSION" "$CURRENT_VERSION" > "$ROLLBACK_DIR/rolled-back"
CURRENT_VERSION="$OLD_VERSION"
check_rolled_back_state
[ ! -e "$ROLLBACK_DIR/rolled-back" ] || fail "explicit old-version reinstall did not clear stale mismatch state"
: > "$CALLS"
start_and_check || fail "successful install path failed with enable and healthy service"
grep -q '^enable ' "$CALLS" || fail "normal installation skipped enabling service"
grep -q '^restart ' "$CALLS" || fail "normal installation skipped restarting service"
record_pass "matching reinstalled package clears rollback state and normal startup still enables service"

# Read the real configuration accessor and health probe after the rollback
# scenarios, so no real systemd or network operation is needed.
PARSER_FUNCTIONS="$TEST_ROOT/postinst-parser-functions.sh"
: >"$PARSER_FUNCTIONS"
for function_name in conf_value service_healthy; do
  sed -n "/^$function_name() {$/,/^}$/p" "$POSTINST" >>"$PARSER_FUNCTIONS"
done
# shellcheck source=/dev/null
source "$PARSER_FUNCTIONS"
declare -F conf_value >/dev/null || fail 'could not extract configuration accessor'
declare -F service_healthy >/dev/null || fail 'could not extract actual health probe'
CONF_DIR="$TEST_ROOT/parser=config with spaces"
mkdir -p "$CONF_DIR"
cat >"$CONF_DIR/server.conf" <<'EOF'
bind = 127.0.0.1 # loopback
port = 1 # superseded
data_root = /not-the-active-root
port = 18768 # business port
data_root = /tmp/instance=a/data # literal equals sign
log_file = /tmp/old.log
log_file = # cleared by the last entry
EOF
[ "$(conf_value bind)" = 127.0.0.1 ] || fail 'postinst retained inline comment'
record_pass 'postinst strips inline comments and surrounding whitespace'
[ "$(conf_value data_root)" = /tmp/instance=a/data ] || fail 'postinst truncated equals-containing path'
record_pass 'postinst preserves equals signs after the first assignment separator'
[ "$(conf_value port)" = 18768 ] && [ -z "$(conf_value log_file)" ] ||
  fail 'postinst did not use the last repeated key, including an empty value'
record_pass 'postinst uses the final repeated key exactly as the launcher'
export HEALTH_TIMEOUT=1
HEALTH_PROBE="$TEST_ROOT/health-probe-port"
port_listening() { printf '%s\n' "$1" >>"$HEALTH_PROBE"; [ "$1" = 18768 ]; }
service_healthy || fail 'health probe used a stale or commented port'
[ "$(cat "$HEALTH_PROBE")" = 18768 ] || fail 'health probe queried the wrong port'
record_pass 'real postinst health probe checks the final uncommented port with isolated mocks'


# --skip-stage cannot relabel old or dirty binaries as a new clean release.
STAGE_INFO="$TEST_ROOT/stage BUILD-INFO.txt"
EXPECTED_STAGE_COMMIT=1111111111111111111111111111111111111111
EXPECTED_STAGE_VERSION=0.2.1-rc1
write_stage_info() {
  printf 'bundle = server\nversion = %s\ncommit = %s\nworking_tree = clean\n' \
    "$EXPECTED_STAGE_VERSION" "$EXPECTED_STAGE_COMMIT" >"$STAGE_INFO"
}
write_stage_info
validate_stage_build_info "$STAGE_INFO" "$EXPECTED_STAGE_COMMIT" "$EXPECTED_STAGE_VERSION" ||
  fail 'matching clean stage metadata was rejected'
record_pass 'skip-stage accepts exactly matching clean commit and version'
for metadata_case in missing old-commit old-version dirty missing-field duplicate-field; do
  write_stage_info
  case "$metadata_case" in
    missing) rm "$STAGE_INFO" ;;
    old-commit) sed -i 's/^commit = .*/commit = 2222222222222222222222222222222222222222/' "$STAGE_INFO" ;;
    old-version) sed -i 's/^version = .*/version = 0.2.0/' "$STAGE_INFO" ;;
    dirty) sed -i 's/^working_tree = .*/working_tree = dirty/' "$STAGE_INFO" ;;
    missing-field) sed -i '/^working_tree = /d' "$STAGE_INFO" ;;
    duplicate-field) printf 'commit = %s\n' "$EXPECTED_STAGE_COMMIT" >>"$STAGE_INFO" ;;
  esac
  if validate_stage_build_info "$STAGE_INFO" "$EXPECTED_STAGE_COMMIT" "$EXPECTED_STAGE_VERSION" \
      >"$TEST_ROOT/skip-stage-$metadata_case.log" 2>&1; then
    fail "skip-stage accepted $metadata_case identity"
  fi
  record_pass "skip-stage rejects $metadata_case identity before packaging"
done
