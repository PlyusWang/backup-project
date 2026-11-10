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


# The portable uninstaller must prove ownership before touching a global unit.
# Only its actual functions are sourced; all paths and systemctl calls below are
# synthetic. Never execute the full uninstaller against the host systemd tree.
PORTABLE_FUNCTIONS="$TEST_ROOT/portable-uninstall-functions.sh"
: >"$PORTABLE_FUNCTIONS"
for function_name in portable_prefix_path portable_check_removal_paths \
                     portable_unit_matches_prefix portable_owned_unit_fingerprint \
                     portable_systemd_install_preflight portable_remove_systemd_unit; do
  sed -n "/^$function_name() {$/,/^}$/p" \
    "$ROOT_DIR/packaging/portable/uninstall.sh" >>"$PORTABLE_FUNCTIONS"
done
# shellcheck source=/dev/null
source "$PORTABLE_FUNCTIONS"
for function_name in portable_prefix_path portable_check_removal_paths \
                     portable_unit_matches_prefix portable_owned_unit_fingerprint \
                     portable_systemd_install_preflight portable_remove_systemd_unit; do
  declare -F "$function_name" >/dev/null || fail "could not extract $function_name"
done
PORTABLE_ROOT="$TEST_ROOT/portable"
PORTABLE_A="$PORTABLE_ROOT/instance-a"
PORTABLE_B="$PORTABLE_ROOT/instance-b"
PORTABLE_SPACES="$PORTABLE_ROOT/instance with spaces"
PORTABLE_RUNTIME="$PORTABLE_ROOT/run/systemd/system"
PORTABLE_UNIT="$PORTABLE_ROOT/etc/systemd/system/backup-project-server.service"
PORTABLE_CALLS="$PORTABLE_ROOT/systemctl-calls"
PORTABLE_LOG="$PORTABLE_ROOT/uninstall.log"
mkdir -p "$(dirname "$PORTABLE_UNIT")" "$PORTABLE_RUNTIME"
for prefix in "$PORTABLE_A" "$PORTABLE_B" "$PORTABLE_SPACES"; do
  mkdir -p "$prefix/bin" "$prefix/etc" "$prefix/var" "$prefix/share" "$prefix/docs"
  printf '#!/bin/sh\nexit 0\n' >"$prefix/bin/launch-server.sh"
  chmod +x "$prefix/bin/launch-server.sh"
  printf 'synthetic config\n' >"$prefix/etc/server.conf"
done
portable_log() { printf '%s\n' "$*" >>"$PORTABLE_LOG"; }
write_portable_unit() {
  local prefix="$1" quote="${2:-plain}"
  awk -v prefix="$prefix" -v quote="$quote" '
    /^ExecStart=/ {
      if (quote == "quoted") print "ExecStart=\"" prefix "/bin/launch-server.sh\" --config \"" prefix "/etc/server.conf\""
      else print "ExecStart=" prefix "/bin/launch-server.sh --config " prefix "/etc/server.conf"
      next
    }
    /^ReadWritePaths=/ {
      if (quote == "quoted") print "ReadWritePaths=\"" prefix "/var\""
      else print "ReadWritePaths=" prefix "/var"
      next
    }
    /^RuntimeDirectory=/ { next }
    { print }
  ' "$ROOT_DIR/packaging/server/backup-project-server.service" >"$PORTABLE_UNIT"
}
reset_portable_mock() {
  : >"$PORTABLE_CALLS"
  PORTABLE_LOAD_STATE=loaded
  PORTABLE_FRAGMENT="$PORTABLE_UNIT"
  PORTABLE_DROPINS=""
  PORTABLE_RELOAD=no
  PORTABLE_FAILURE=""
}
systemctl() {
  printf '%s\n' "$*" >>"$PORTABLE_CALLS"
  [ "$PORTABLE_FAILURE" != "$1" ] || return 1
  case "$1" in
    show)
      case "$2" in
        --property=LoadState) printf '%s\n' "$PORTABLE_LOAD_STATE" ;;
        --property=FragmentPath) printf '%s\n' "$PORTABLE_FRAGMENT" ;;
        --property=DropInPaths) printf '%s\n' "$PORTABLE_DROPINS" ;;
        --property=NeedDaemonReload) printf '%s\n' "$PORTABLE_RELOAD" ;;
        *) return 1 ;;
      esac ;;
    stop)
      if [ "$PORTABLE_FAILURE" = change-after-stop ]; then
        printf '\n# concurrent modification\n' >>"$PORTABLE_UNIT"
      fi ;;
    disable|daemon-reload|enable|restart) ;;
    *) return 1 ;;
  esac
}
no_portable_service_actions() {
  ! grep -Eq '^(stop|disable|daemon-reload)( |$)' "$PORTABLE_CALLS"
}
assert_portable_retained() {
  [ -f "$PORTABLE_UNIT" ] && [ -f "$PORTABLE_A/bin/launch-server.sh" ] &&
    [ -f "$PORTABLE_B/bin/launch-server.sh" ] || fail 'a refused uninstall removed another instance'
}
reset_portable_mock
write_portable_unit "$PORTABLE_A"
if portable_remove_systemd_unit "$PORTABLE_B" "$PORTABLE_UNIT" "$PORTABLE_RUNTIME"; then
  fail 'instance B accepted the global unit of instance A'
fi
assert_portable_retained
[ ! -s "$PORTABLE_CALLS" ] || fail 'foreign-prefix unit reached systemctl'
record_pass 'portable uninstall refuses another prefix and preserves both installations and its unit'

reset_portable_mock
portable_remove_systemd_unit "$PORTABLE_A" "$PORTABLE_UNIT" "$PORTABLE_RUNTIME" ||
  fail 'own installer-generated unit was rejected'
[ ! -e "$PORTABLE_UNIT" ] || fail 'verified own unit was not removed'
[ "$(grep -c '^stop backup-project-server.service$' "$PORTABLE_CALLS")" = 1 ] &&
  [ "$(grep -c '^disable backup-project-server.service$' "$PORTABLE_CALLS")" = 1 ] &&
  [ "$(grep -c '^daemon-reload$' "$PORTABLE_CALLS")" = 1 ] || fail 'own service lifecycle was not exact'
record_pass 'portable uninstall stops/disables/removes exactly its verified installer-generated unit'

reset_portable_mock
portable_remove_systemd_unit "$PORTABLE_B" "$PORTABLE_UNIT" "$PORTABLE_RUNTIME" ||
  fail 'a prefix without a system unit cannot be uninstalled'
[ ! -s "$PORTABLE_CALLS" ] || fail 'absent system unit reached systemctl'
record_pass 'portable uninstall without a global unit never calls systemctl'

for unit_case in wrong-launcher wrong-config wrong-data exec-stop duplicate-exec environment; do
  reset_portable_mock
  write_portable_unit "$PORTABLE_A"
  case "$unit_case" in
    wrong-launcher) sed -i "s|^ExecStart=.*|ExecStart=/usr/bin/other-server --config $PORTABLE_A/etc/server.conf|" "$PORTABLE_UNIT" ;;
    wrong-config) sed -i "s| --config .*| --config $PORTABLE_B/etc/server.conf|" "$PORTABLE_UNIT" ;;
    wrong-data) sed -i "s|^ReadWritePaths=.*|ReadWritePaths=$PORTABLE_B/var|" "$PORTABLE_UNIT" ;;
    exec-stop) sed -i '/^Type=/a ExecStop=/usr/bin/foreign-stop' "$PORTABLE_UNIT" ;;
    duplicate-exec) sed -i "/^Type=/a ExecStart=$PORTABLE_A/bin/launch-server.sh --config $PORTABLE_A/etc/server.conf" "$PORTABLE_UNIT" ;;
    environment) sed -i '/^Type=/a Environment=BACKUP_SERVER_BIN=/other/server' "$PORTABLE_UNIT" ;;
  esac
  before="$(sha256sum "$PORTABLE_UNIT")"
  if portable_remove_systemd_unit "$PORTABLE_A" "$PORTABLE_UNIT" "$PORTABLE_RUNTIME"; then
    fail "portable uninstall accepted $unit_case"
  fi
  [ "$before" = "$(sha256sum "$PORTABLE_UNIT")" ] || fail "$unit_case unit was changed"
  [ ! -s "$PORTABLE_CALLS" ] || fail "$unit_case reached systemctl"
  record_pass "portable uninstall rejects $unit_case before service operations"
done

for loaded_case in fragment dropin stale show-failure; do
  reset_portable_mock
  write_portable_unit "$PORTABLE_A"
  case "$loaded_case" in
    fragment) PORTABLE_FRAGMENT="$PORTABLE_ROOT/unrelated.service" ;;
    dropin) PORTABLE_DROPINS="$PORTABLE_ROOT/override.conf" ;;
    stale) PORTABLE_RELOAD=yes ;;
    show-failure) PORTABLE_FAILURE=show ;;
  esac
  if portable_remove_systemd_unit "$PORTABLE_A" "$PORTABLE_UNIT" "$PORTABLE_RUNTIME"; then
    fail "portable uninstall accepted loaded $loaded_case"
  fi
  assert_portable_retained
  no_portable_service_actions || fail "$loaded_case caused a service operation"
  record_pass "portable uninstall preserves service and programs on loaded $loaded_case"
done

for stop_case in stop disable change-after-stop; do
  reset_portable_mock
  write_portable_unit "$PORTABLE_A"
  PORTABLE_FAILURE="$stop_case"
  if portable_remove_systemd_unit "$PORTABLE_A" "$PORTABLE_UNIT" "$PORTABLE_RUNTIME"; then
    fail "portable uninstall ignored $stop_case"
  fi
  assert_portable_retained
  ! grep -q '^daemon-reload' "$PORTABLE_CALLS" || fail "$stop_case removed/reloaded the unit"
  if [ "$stop_case" != disable ]; then
    ! grep -q '^disable ' "$PORTABLE_CALLS" || fail "$stop_case continued to disable"
  fi
  record_pass "portable uninstall retains unit and programs after $stop_case"
done

reset_portable_mock
write_portable_unit "$PORTABLE_SPACES" quoted
[ "$(portable_prefix_path "$PORTABLE_SPACES/")" = "$PORTABLE_SPACES" ] || fail 'prefix spaces were lost'
portable_remove_systemd_unit "$PORTABLE_SPACES" "$PORTABLE_UNIT" "$PORTABLE_RUNTIME" ||
  fail 'valid quoted unit for a prefix with spaces was rejected'
[ ! -e "$PORTABLE_UNIT" ] || fail 'quoted own unit was retained'
record_pass 'portable uninstall preserves spaces and accepts exactly quoted systemd instance paths'

reset_portable_mock
write_portable_unit "$PORTABLE_SPACES"
if portable_remove_systemd_unit "$PORTABLE_SPACES" "$PORTABLE_UNIT" "$PORTABLE_RUNTIME"; then
  fail 'ambiguous unquoted systemd prefix with spaces was accepted'
fi
[ -f "$PORTABLE_UNIT" ] && [ ! -s "$PORTABLE_CALLS" ] || fail 'ambiguous space prefix changed service'
record_pass 'portable uninstall refuses unquoted systemd paths with spaces without guessing'

reset_portable_mock
write_portable_unit "$PORTABLE_A"
mv "$PORTABLE_UNIT" "$PORTABLE_ROOT/foreign-unit"
ln -s "$PORTABLE_ROOT/foreign-unit" "$PORTABLE_UNIT"
before="$(sha256sum "$PORTABLE_ROOT/foreign-unit")"
if portable_remove_systemd_unit "$PORTABLE_A" "$PORTABLE_UNIT" "$PORTABLE_RUNTIME"; then
  fail 'symlink unit was accepted'
fi
[ -L "$PORTABLE_UNIT" ] && [ "$before" = "$(sha256sum "$PORTABLE_ROOT/foreign-unit")" ] &&
  [ ! -s "$PORTABLE_CALLS" ] || fail 'symlink unit affected its target'
rm "$PORTABLE_UNIT"
record_pass 'portable uninstall preserves symlink units and their unrelated targets'

mkdir -p "$PORTABLE_ROOT/unit-link-parent"
ln -s "$(dirname "$PORTABLE_UNIT")" "$PORTABLE_ROOT/unit-link-parent/alias"
write_portable_unit "$PORTABLE_A"
if portable_remove_systemd_unit "$PORTABLE_A" \
     "$PORTABLE_ROOT/unit-link-parent/alias/backup-project-server.service" "$PORTABLE_RUNTIME"; then
  fail 'unit path through a symlink ancestor was accepted'
fi
[ -f "$PORTABLE_UNIT" ] && [ ! -s "$PORTABLE_CALLS" ] || fail 'unit ancestor symlink affected service'
record_pass 'portable uninstall refuses system-unit symlink ancestors'

ln -s "$PORTABLE_A" "$PORTABLE_ROOT/prefix-alias"
ln -s "$PORTABLE_ROOT" "$PORTABLE_ROOT/ancestor-alias"
for invalid_prefix in "$PORTABLE_ROOT/prefix-alias" "$PORTABLE_ROOT/ancestor-alias/instance-a" \
                      "$PORTABLE_A/../instance-b" /etc //etc/; do
  if portable_prefix_path "$invalid_prefix" >"$PORTABLE_ROOT/prefix-result" 2>"$PORTABLE_ROOT/prefix-error"; then
    fail "unsafe prefix was accepted: $invalid_prefix"
  fi
  [ -f "$PORTABLE_A/bin/launch-server.sh" ] && [ -f "$PORTABLE_B/bin/launch-server.sh" ] ||
    fail 'unsafe prefix check removed an instance'
  record_pass "portable uninstall rejects unsafe prefix $invalid_prefix"
done

for target in bin share docs var etc; do
  mv "$PORTABLE_B/$target" "$PORTABLE_B/$target.real"
  ln -s "$PORTABLE_B/$target.real" "$PORTABLE_B/$target"
  if portable_check_removal_paths "$PORTABLE_B" 1 >"$PORTABLE_ROOT/path-$target.log" 2>&1; then
    fail "symlink removal target $target was accepted"
  fi
  [ -L "$PORTABLE_B/$target" ] && [ -d "$PORTABLE_B/$target.real" ] || fail "$target symlink target changed"
  rm "$PORTABLE_B/$target"
  mv "$PORTABLE_B/$target.real" "$PORTABLE_B/$target"
  record_pass "portable uninstall refuses symlink $target removal targets"
done

for private_path in bin/launch-server.sh etc/server.conf; do
  reset_portable_mock
  write_portable_unit "$PORTABLE_A"
  mv "$PORTABLE_A/$private_path" "$PORTABLE_A/$private_path.real"
  ln -s "$PORTABLE_A/$private_path.real" "$PORTABLE_A/$private_path"
  if portable_remove_systemd_unit "$PORTABLE_A" "$PORTABLE_UNIT" "$PORTABLE_RUNTIME"; then
    fail "symlink identity path $private_path was accepted"
  fi
  [ ! -s "$PORTABLE_CALLS" ] || fail "$private_path symlink reached systemctl"
  rm "$PORTABLE_A/$private_path"
  mv "$PORTABLE_A/$private_path.real" "$PORTABLE_A/$private_path"
  record_pass "portable uninstall refuses symlink $private_path ownership paths"
done

reset_portable_mock
write_portable_unit "$PORTABLE_A"
if portable_remove_systemd_unit "$PORTABLE_A" "$PORTABLE_UNIT" "$PORTABLE_ROOT/absent-runtime"; then
  fail 'system unit was removed without a running system manager'
fi
[ -f "$PORTABLE_UNIT" ] && [ ! -s "$PORTABLE_CALLS" ] || fail 'offline system unit was changed'
record_pass 'portable uninstall preserves an offline system unit and the installation'


# Run complete installer/uninstaller copies in a synthetic package. Redirect the
# two fixed systemd paths to this fixture; export only mock service/root helpers.
# All product launch/keygen tools are stubs, so no real server or key is created.
PORTABLE_PACKAGE="$PORTABLE_ROOT/mock-package"
PORTABLE_STAGING="$PORTABLE_ROOT/mock-staging"
mkdir -p "$PORTABLE_PACKAGE/bin" "$PORTABLE_PACKAGE/packaging/server" "$PORTABLE_STAGING"
awk -v unit="$PORTABLE_UNIT" -v runtime="$PORTABLE_RUNTIME" '
  function replace_literal(line, old, replacement, position, result) {
    result=""
    while ((position=index(line, old))) {
      result=result substr(line, 1, position-1) replacement
      line=substr(line, position+length(old))
    }
    return result line
  }
  { print replace_literal(replace_literal($0, "/etc/systemd/system/backup-project-server.service", unit), "/run/systemd/system", runtime) }
' "$ROOT_DIR/packaging/portable/install-server.sh" >"$PORTABLE_PACKAGE/install.sh"
awk -v unit="$PORTABLE_UNIT" -v runtime="$PORTABLE_RUNTIME" '
  function replace_literal(line, old, replacement, position, result) {
    result=""
    while ((position=index(line, old))) {
      result=result substr(line, 1, position-1) replacement
      line=substr(line, position+length(old))
    }
    return result line
  }
  { print replace_literal(replace_literal($0, "/etc/systemd/system/backup-project-server.service", unit), "/run/systemd/system", runtime) }
' "$ROOT_DIR/packaging/portable/uninstall.sh" >"$PORTABLE_PACKAGE/uninstall.sh"
cp "$ROOT_DIR/packaging/server/server.conf" "$ROOT_DIR/packaging/server/backup-project-server.service" \
  "$PORTABLE_PACKAGE/packaging/server/"
printf '#!/usr/bin/env bash\nexit 0\n' >"$PORTABLE_PACKAGE/bin/launch-server.sh"
printf '#!/usr/bin/env bash\n[ "$1" = --output ] || exit 2\nprintf "synthetic non-key\\n" >"$2"\n' \
  >"$PORTABLE_PACKAGE/bin/backup-server-keygen"
printf '#!/usr/bin/env bash\nexit 0\n' >"$PORTABLE_PACKAGE/backup-server-manager"
chmod +x "$PORTABLE_PACKAGE/bin/launch-server.sh" "$PORTABLE_PACKAGE/bin/backup-server-keygen" "$PORTABLE_PACKAGE/uninstall.sh"
(cd "$PORTABLE_PACKAGE" && find . -type f ! -name MANIFEST.sha256 -print0 |
  sort -z | xargs -0 sha256sum >MANIFEST.sha256)
# Only the copied installer sees these exported functions. They cannot operate
# host accounts or change ownership; the mock systemctl only records commands.
id() { [ "$1" = -u ] && printf '0\n'; }
chown() { printf 'mock chown\n' >>"$PORTABLE_LOG"; }
deluser() { printf 'unexpected deluser\n' >>"$PORTABLE_CALLS"; return 1; }
export -f id chown deluser systemctl
export PORTABLE_CALLS PORTABLE_FAILURE PORTABLE_LOAD_STATE PORTABLE_FRAGMENT PORTABLE_DROPINS PORTABLE_RELOAD PORTABLE_UNIT PORTABLE_LOG

for install_case in other-prefix deb-unit symlink-unit dropin stale; do
  reset_portable_mock
  write_portable_unit "$PORTABLE_A"
  case "$install_case" in
    other-prefix) ;;
    deb-unit) cp "$ROOT_DIR/packaging/server/backup-project-server.service" "$PORTABLE_UNIT" ;;
    symlink-unit) mv "$PORTABLE_UNIT" "$PORTABLE_ROOT/install-unit-target"; ln -s "$PORTABLE_ROOT/install-unit-target" "$PORTABLE_UNIT" ;;
    dropin) PORTABLE_DROPINS="$PORTABLE_ROOT/install-override.conf" ;;
    stale) PORTABLE_RELOAD=yes ;;
  esac
  prefix="$PORTABLE_B"
  if [ "$install_case" = dropin ] || [ "$install_case" = stale ]; then prefix="$PORTABLE_A"; fi
  before="$(find "$prefix" -type f -exec sha256sum {} + | sort)"
  unit_before="$(sha256sum "$PORTABLE_UNIT")"
  if TMPDIR="$PORTABLE_STAGING" bash "$PORTABLE_PACKAGE/install.sh" --prefix "$prefix" --no-user \
      >"$PORTABLE_ROOT/full-install-$install_case.log" 2>&1; then
    fail "full portable installer accepted $install_case"
  fi
  [ "$before" = "$(find "$prefix" -type f -exec sha256sum {} + | sort)" ] &&
    [ "$unit_before" = "$(sha256sum "$PORTABLE_UNIT")" ] || fail "$install_case installer changed existing files"
  no_portable_service_actions && ! grep -Eq '^(enable|restart) ' "$PORTABLE_CALLS" ||
    fail "$install_case installer changed another service"
  [ -z "$(find "$PORTABLE_STAGING" -mindepth 1 -print -quit)" ] || fail "$install_case failed after staging began"
  if [ "$install_case" = symlink-unit ]; then rm "$PORTABLE_UNIT"; fi
  record_pass "full portable installer rejects $install_case before program/config/identity/staging writes"
done

reset_portable_mock
rm "$PORTABLE_UNIT"
PORTABLE_NEW_SPACES="$PORTABLE_ROOT/new systemd prefix with spaces"
if TMPDIR="$PORTABLE_STAGING" bash "$PORTABLE_PACKAGE/install.sh" --prefix "$PORTABLE_NEW_SPACES" --no-user \
    >"$PORTABLE_ROOT/full-install-systemd-spaces.log" 2>&1; then
  fail 'systemd install accepted an unencodable prefix with spaces'
fi
[ ! -e "$PORTABLE_NEW_SPACES" ] && [ ! -s "$PORTABLE_CALLS" ] || fail 'rejected systemd space prefix was written'
record_pass 'full portable systemd installer rejects spaces before creating any prefix or identity'

reset_portable_mock
TMPDIR="$PORTABLE_STAGING" bash "$PORTABLE_PACKAGE/install.sh" --prefix "$PORTABLE_NEW_SPACES" --no-systemd --no-user \
  >"$PORTABLE_ROOT/full-install-portable-spaces.log" 2>&1 || fail 'no-systemd install lost support for spaces'
[ -x "$PORTABLE_NEW_SPACES/bin/backup-server-manager" ] && [ -f "$PORTABLE_NEW_SPACES/etc/server.conf" ] &&
  [ -f "$PORTABLE_NEW_SPACES/var/state/transport.key" ] && [ ! -s "$PORTABLE_CALLS" ] ||
  fail 'no-systemd space install did not publish synthetic files in the requested prefix'
record_pass 'full no-systemd portable installer still handles a new prefix with spaces without service calls'

python3 - "$PORTABLE_ROOT/full-install-portable-spaces.log" "$PORTABLE_PACKAGE" "$PORTABLE_NEW_SPACES" <<'PY'
import os
from pathlib import Path
import shlex
import subprocess
import sys
log, package, prefix = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
lines = log.read_text().splitlines()
for label, expected in (
        ('手工启动：', [str(prefix/'bin/launch-server.sh'), '--config', str(prefix/'etc/server.conf')]),
        ('管理菜单：', [str(prefix/'bin/backup-server-manager')]),
        ('卸载：', [str(package/'uninstall.sh'), '--prefix', str(prefix)])):
    matching = [line.split(label, 1)[1] for line in lines if label in line]
    if len(matching) != 1 or shlex.split(matching[0]) != expected or not os.access(expected[0], os.X_OK):
        raise RuntimeError('Printed installer command does not resolve: ' + label)
subprocess.run([str(package/'uninstall.sh'), '--prefix', str(prefix), '--help'],
               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=True)
PY
record_pass 'portable installer prints runnable exact launch/menu/uninstall commands for paths with spaces'

bash "$PORTABLE_PACKAGE/uninstall.sh" --prefix "$PORTABLE_NEW_SPACES" \
  >"$PORTABLE_ROOT/uninstall-spaces-command.log" 2>&1 || fail 'space-prefix uninstall failed'
python3 - "$PORTABLE_ROOT/uninstall-spaces-command.log" "$PORTABLE_PACKAGE/uninstall.sh" "$PORTABLE_NEW_SPACES" <<'PY'
from pathlib import Path
import shlex
import subprocess
import sys
log, script, prefix = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
matches = [line.split('要删除它们：', 1)[1] for line in log.read_text().splitlines() if '要删除它们：' in line]
expected = [str(script.resolve()), '--prefix', str(prefix), '--purge-data']
if len(matches) != 1 or shlex.split(matches[0]) != expected:
    raise RuntimeError('Printed data-removal command does not preserve its exact path and arguments')
subprocess.run(expected + ['--help'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=True)
if not (prefix/'etc/server.conf').is_file() or not (prefix/'var/state/transport.key').is_file():
    raise RuntimeError('Ordinary uninstall or help removed retained configuration or identity')
PY
record_pass 'space-prefix uninstall prints an exact purge command while preserving configuration and identity'

reset_portable_mock
write_portable_unit "$PORTABLE_A"
printf 'preserve secret\n' >"$PORTABLE_A/etc/secrets.env"
printf 'preserve transport identity\n' >"$PORTABLE_A/var/transport-sentinel"
mkdir -p "$PORTABLE_A/var/state"
printf 'preserve private identity\n' >"$PORTABLE_A/var/state/transport.key"
private_before="$(sha256sum "$PORTABLE_A/etc/server.conf" "$PORTABLE_A/etc/secrets.env" "$PORTABLE_A/var/state/transport.key")"
TMPDIR="$PORTABLE_STAGING" bash "$PORTABLE_PACKAGE/install.sh" --prefix "$PORTABLE_A" --no-user \
  >"$PORTABLE_ROOT/full-install-owned.log" 2>&1 || fail 'verified own-prefix upgrade was rejected'
[ "$private_before" = "$(sha256sum "$PORTABLE_A/etc/server.conf" "$PORTABLE_A/etc/secrets.env" "$PORTABLE_A/var/state/transport.key")" ] ||
  fail 'verified own-prefix upgrade changed existing config or identity'
grep -q '^restart backup-project-server.service$' "$PORTABLE_CALLS" || fail 'verified own-prefix upgrade did not restart its service'
record_pass 'full portable systemd upgrade accepts only its owned unit and preserves config/secret/private identity'

reset_portable_mock
before="$(find "$PORTABLE_A" -type f -exec sha256sum {} + | sort)"
unit_before="$(sha256sum "$PORTABLE_UNIT")"
if bash "$PORTABLE_PACKAGE/uninstall.sh" --prefix "$PORTABLE_A" --purge-data --remove-user \
    >"$PORTABLE_ROOT/full-uninstall-remove-user.log" 2>&1; then
  fail 'uninstaller accepted automatic deletion of a potentially shared account'
fi
[ "$before" = "$(find "$PORTABLE_A" -type f -exec sha256sum {} + | sort)" ] &&
  [ "$unit_before" = "$(sha256sum "$PORTABLE_UNIT")" ] && [ ! -s "$PORTABLE_CALLS" ] ||
  fail 'remove-user refusal occurred after a program/data/account/service mutation'
record_pass 'full portable uninstall rejects remove-user before deleting anything and never invokes deluser'

reset_portable_mock
bash "$PORTABLE_PACKAGE/uninstall.sh" --prefix "$PORTABLE_A" \
  >"$PORTABLE_ROOT/full-uninstall-owned.log" 2>&1 || fail 'full owned-prefix uninstall failed'
[ ! -d "$PORTABLE_A/bin" ] && [ ! -e "$PORTABLE_UNIT" ] &&
  [ "$private_before" = "$(sha256sum "$PORTABLE_A/etc/server.conf" "$PORTABLE_A/etc/secrets.env" "$PORTABLE_A/var/state/transport.key")" ] &&
  [ -f "$PORTABLE_B/bin/launch-server.sh" ] || fail 'full own uninstall removed data or another prefix'
record_pass 'full portable uninstall removes its own service/programs and retains exact identity/data plus the other prefix'
for absent_unit_case in vendor-lib-unit query-failure notfound-with-dropin; do
  reset_portable_mock
  PORTABLE_LOAD_STATE=not-found
  PORTABLE_FRAGMENT=""
  case "$absent_unit_case" in
    vendor-lib-unit) PORTABLE_LOAD_STATE=loaded; PORTABLE_FRAGMENT=/lib/systemd/system/backup-project-server.service ;;
    query-failure) PORTABLE_FAILURE=show ;;
    notfound-with-dropin) PORTABLE_DROPINS="$PORTABLE_ROOT/vendor-dropin.conf" ;;
  esac
  prefix="$PORTABLE_ROOT/new-$absent_unit_case"
  if TMPDIR="$PORTABLE_STAGING" bash "$PORTABLE_PACKAGE/install.sh" --prefix "$prefix" --no-user \
      >"$PORTABLE_ROOT/full-install-absent-$absent_unit_case.log" 2>&1; then
    fail "absent /etc unit bypassed $absent_unit_case ownership checks"
  fi
  [ ! -e "$PORTABLE_UNIT" ] && [ ! -e "$prefix" ] &&
    [ -z "$(find "$PORTABLE_STAGING" -mindepth 1 -print -quit)" ] ||
    fail "$absent_unit_case changed an instance or created staging before refusal"
  ! grep -Eq '^(stop|disable|daemon-reload|enable|restart)( |$)' "$PORTABLE_CALLS" ||
    fail "$absent_unit_case changed the loaded vendor service"
  record_pass "full portable installer without /etc unit rejects $absent_unit_case before any writes"
done

reset_portable_mock
PORTABLE_LOAD_STATE=not-found
PORTABLE_FRAGMENT=""
PORTABLE_NEW_SYSTEMD="$PORTABLE_ROOT/new-systemd-instance"
TMPDIR="$PORTABLE_STAGING" bash "$PORTABLE_PACKAGE/install.sh" --prefix "$PORTABLE_NEW_SYSTEMD" --no-user \
  >"$PORTABLE_ROOT/full-install-fresh-systemd.log" 2>&1 || fail 'fresh systemd fixture install failed'
[ -f "$PORTABLE_UNIT" ] && [ -x "$PORTABLE_NEW_SYSTEMD/bin/backup-server-manager" ] &&
  grep -q '^restart backup-project-server.service$' "$PORTABLE_CALLS" || fail 'fresh systemd fixture was not published'
portable_unit_matches_prefix "$PORTABLE_NEW_SYSTEMD" "$PORTABLE_UNIT" || fail 'fresh installer violated shared unit ownership contract'
record_pass 'full fresh portable systemd install requires explicit not-found with no fragment/dropins and creates an owned unit'

reset_portable_mock
before="$(find "$PORTABLE_B" -type f -exec sha256sum {} + | sort)"
unit_before="$(sha256sum "$PORTABLE_UNIT")"
if bash "$PORTABLE_PACKAGE/uninstall.sh" --prefix "$PORTABLE_B" \
    >"$PORTABLE_ROOT/full-uninstall-other-prefix.log" 2>&1; then
  fail 'full uninstaller accepted a unit belonging to another prefix'
fi
[ "$before" = "$(find "$PORTABLE_B" -type f -exec sha256sum {} + | sort)" ] &&
  [ "$unit_before" = "$(sha256sum "$PORTABLE_UNIT")" ] && [ ! -s "$PORTABLE_CALLS" ] ||
  fail 'full uninstaller changed programs/data/unit/service belonging to an uncertain instance'
record_pass 'full portable uninstall of another prefix refuses before any program/data/service mutation'

unset -f id chown deluser
