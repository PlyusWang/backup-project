#!/usr/bin/env bash
# Isolated filesystem/menu tests. Key generation, certificates, execution and
# privilege changes are mocks; no server, GUI or installed service is run.
set -uo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_ROOT="${BACKUP_SERVER_MANAGER_INIT_TEST_ROOT:-$(mktemp -d "${TMPDIR:-/tmp}/bp-manager-init.XXXXXXXX")}"
mkdir -p "$TEST_ROOT/tools"
TEST_ROOT="$(cd "$TEST_ROOT" && pwd)"
PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); printf 'PASS %s\n' "$1"; }
fail() { FAIL=$((FAIL + 1)); printf 'FAIL %s\n' "$1" >&2; }
assert() { local label="$1"; shift; if "$@"; then pass "$label"; else fail "$label"; fi; }

# Load only production initialization and configuration parsing definitions.
# shellcheck source=/dev/null
source <(sed -n '/^initialization_path_supported() {/,/^}/p; /^init_server() {/,/^}/p; /^config_value() {/,/^}/p' "$ROOT_DIR/scripts/backup-server-admin.sh")
cat >"$TEST_ROOT/tools/keygen" <<'MOCK'
#!/usr/bin/env bash
if [ "${1:-}" = --show ]; then printf '指纹: mock-only\n'; exit 0; fi
if [ -n "${TEST_KEYGEN_GATE:-}" ]; then
  : >"$TEST_KEYGEN_GATE.entered"
  for ((attempt=0; attempt<150; attempt++)); do
    [ ! -e "$TEST_KEYGEN_GATE.release" ] || break
    sleep 0.05
  done
  [ -e "$TEST_KEYGEN_GATE.release" ] || exit 1
fi
[ "${1:-}" = --output ] || exit 1
head -c 32 /dev/zero >"$2" || exit 1
chmod 600 "$2"
printf '公钥 (hex): mock-public-key\n'
MOCK
cat >"$TEST_ROOT/tools/cert-tool" <<'MOCK'
#!/usr/bin/env bash
command="$1"; shift
key=''; output=''
while [ "$#" -gt 0 ]; do
  case "$1" in --root-key) key="$2";; --out) output="$2";; esac
  shift
  [ "$#" -eq 0 ] || shift
done
case "$command" in
  root-init) printf 'mock root private\n' >"$key"; chmod 600 "$key"; printf 'mock root public\n' >"$key.pub"; chmod 644 "$key.pub" ;;
  issue-server) printf 'mock server certificate\n' >"$output"; chmod 644 "$output" ;;
  verify-server) exit 0 ;;
  inspect-server) printf 'server_public_key = mock-public-key\n' ;;
  *) exit 1 ;;
esac
MOCK
cat >"$TEST_ROOT/tools/server" <<'MOCK'
#!/bin/sh
exit 0
MOCK
cat >"$TEST_ROOT/tools/checker" <<'MOCK'
#!/bin/bash
if [ "${TEST_CHECK_FAIL:-0}" = 1 ]; then echo 'injected shared check failure' >&2; exit 1; fi
if [ "${TEST_CHECK_TERM:-0}" = 1 ]; then kill -TERM "$PPID"; exit 1; fi
exec "$TEST_REAL_LAUNCHER" "$@"
MOCK
cat >"$TEST_ROOT/tools/ln" <<'MOCK'
#!/bin/bash
target="${!#}"
if [ "$target" = "${TEST_RACE_CREATE_TARGET:-}" ]; then
  if [ "${TEST_RACE_CREATE_TYPE:-}" = symlink ]; then
    /usr/bin/ln -s "$TEST_RACE_DIRECTORY" "$target" || exit 1
  else
    mkdir "$target" || exit 1
  fi
fi
if [ "$target" = "${TEST_FAIL_LINK_TARGET:-}" ]; then
  if [ "${TEST_EXTERNAL_CREATE:-0}" = 1 ]; then printf 'external-owned-sentinel\n' >"$target"; fi
  echo 'injected no-clobber link failure' >&2
  exit 1
fi
exec /usr/bin/ln "$@"
MOCK
chmod +x "$TEST_ROOT/tools/"*
export TEST_REAL_LAUNCHER="$ROOT_DIR/packaging/server/launch-server.sh"
export PATH="$TEST_ROOT/tools:$PATH"

setup_instance() {
  unset BACKUP_SERVER_DATA BACKUP_SERVER_DB BACKUP_SERVER_PID_FILE BACKUP_SERVER_LOG
  unset BACKUP_SERVER_CONFIG BACKUP_SERVER_ROOT_PUBLIC
  SERVER_ROOT="$1"; DATA_ROOT="$SERVER_ROOT/data"; DB_PATH="$SERVER_ROOT/state/metadata.sqlite3"
  CONFIG_FILE="$SERVER_ROOT/server.conf"; LOG_FILE="$SERVER_ROOT/logs/server.log"
  PID_FILE="$SERVER_ROOT/state/backup-server.pid"; configured_root_pub="$SERVER_ROOT/private/root.key.pub"
  MANAGER_DEFAULT_DATA=1; MANAGER_DEFAULT_DB=1; MANAGER_DEFAULT_PID_FILE=1
  MANAGER_DEFAULT_LOG=1; MANAGER_DEFAULT_CONFIG=1; MANAGER_DEFAULT_ROOT_PUBLIC=1
  KEYGEN_BIN="$TEST_ROOT/tools/keygen"; CERT_TOOL="$TEST_ROOT/tools/cert-tool"
  SERVER_BIN="$TEST_ROOT/tools/server"; LAUNCHER="$TEST_ROOT/tools/checker"
  export SERVER_ROOT DATA_ROOT DB_PATH CONFIG_FILE LOG_FILE PID_FILE configured_root_pub
  export MANAGER_DEFAULT_DATA MANAGER_DEFAULT_DB MANAGER_DEFAULT_PID_FILE
  export MANAGER_DEFAULT_LOG MANAGER_DEFAULT_CONFIG MANAGER_DEFAULT_ROOT_PUBLIC
  export KEYGEN_BIN CERT_TOOL SERVER_BIN LAUNCHER
}
instance_initialized() { [ -f "$CONFIG_FILE" ]; }
instance_ready() { [ -f "$DB_PATH" ]; }
port_listener_exists() { return 1; }
print_client_connection_info() { printf 'MOCK client configuration ready: %s\n' "$CONFIG_FILE"; }
no_partial_identity() {
  local root="$1"
  [ ! -e "$root/server.conf" ] && [ ! -e "$root/secrets.env" ] &&
    [ ! -e "$root/state/transport.key" ] && [ ! -e "$root/private/root.key" ] &&
    [ ! -e "$root/private/root.key.pub" ] && [ ! -e "$root/state/server.bpcert" ] &&
    [ -z "$(find "$root" -name '.server-init.*' -o -name '.server-init-file.*')" ]
}
run_init() (
  setup_instance "$1"
  init_server <<<"${2:-$'\n\n18769\nn\n\n'}"
)

GOOD_ROOT="$TEST_ROOT/space = instance"
if run_init "$GOOD_ROOT" >"$TEST_ROOT/valid.log" 2>&1; then pass 'spaces and = remain valid initialization paths'; else fail 'valid initialization'; fi
(
  CONFIG_FILE="$GOOD_ROOT/server.conf"
  [ "$(config_value data_root)" = "$GOOD_ROOT/data" ] &&
    [ "$(config_value transport_key)" = "$GOOD_ROOT/state/transport.key" ] &&
    [ "$(stat -c %a "$GOOD_ROOT/state/transport.key")" = 600 ] &&
    [ "$(stat -c %h "$GOOD_ROOT/state/transport.key")" = 1 ] &&
    grep -q '初始化完成' "$TEST_ROOT/valid.log"
) && pass 'committed configuration paths, private mode and link count' || fail 'committed configuration paths, private mode and link count'
BEFORE="$(sha256sum "$GOOD_ROOT/state/transport.key" "$GOOD_ROOT/secrets.env" "$GOOD_ROOT/server.conf")"
if run_init "$GOOD_ROOT" >"$TEST_ROOT/repeated.log" 2>&1; then fail 'repeat initialization refused'; else pass 'repeat initialization refused'; fi
assert 'repeat preserves all existing identities and configuration' test "$BEFORE" = "$(sha256sum "$GOOD_ROOT/state/transport.key" "$GOOD_ROOT/secrets.env" "$GOOD_ROOT/server.conf")"

for suffix in 'bad#root' $'bad\rroot' 'bad trailing '; do
  root="$TEST_ROOT/$suffix"
  if run_init "$root" >"$TEST_ROOT/unsupported-${PASS}.log" 2>&1; then fail 'unsupported path rejected';
  elif [ ! -e "$root" ]; then pass 'unsupported path rejected before filesystem initialization';
  else fail 'unsupported path created a directory'; fi
done
(
  setup_instance "$TEST_ROOT/unsupported-config"
  MANAGER_DEFAULT_CONFIG=0
  export BACKUP_SERVER_CONFIG="$TEST_ROOT/bad#config"
  if init_server <<<' '; then exit 1; fi
  [ ! -e "$SERVER_ROOT" ]
) >"$TEST_ROOT/unsupported-config.log" 2>&1 && pass 'external unencodable configuration path rejected' || fail 'external unencodable configuration path rejected'

CERT_ROOT="$TEST_ROOT/certificate instance"
if run_init "$CERT_ROOT" $'\n\n18770\ny\n\n\nmock-server\n' >"$TEST_ROOT/certificate.log" 2>&1 &&
    [ -f "$CERT_ROOT/state/server.bpcert" ] && [ -f "$CERT_ROOT/private/root.key.pub" ] &&
    grep -q 'require_bpsec2 = true' "$CERT_ROOT/server.conf"; then
  pass 'BPSEC2 initialization commits verified mock certificate materials'
else fail 'BPSEC2 mock initialization'; fi

FAIL_ROOT="$TEST_ROOT/fail-publication"
if TEST_FAIL_LINK_TARGET="$FAIL_ROOT/secrets.env" run_init "$FAIL_ROOT" >"$TEST_ROOT/fail-publication.log" 2>&1; then fail 'publication failure rejected'; else pass 'publication failure rejected'; fi
assert 'publication failure rolls back only created identity files' no_partial_identity "$FAIL_ROOT"
if run_init "$FAIL_ROOT" >"$TEST_ROOT/retry-publication.log" 2>&1; then pass 'failed initialization can be retried'; else fail 'failed initialization can be retried'; fi

CHECK_ROOT="$TEST_ROOT/fail-check"
if TEST_CHECK_FAIL=1 run_init "$CHECK_ROOT" >"$TEST_ROOT/fail-check.log" 2>&1; then fail 'shared checker failure rejected'; else pass 'shared checker failure rejected'; fi
assert 'shared checker failure leaves no published configuration or identities' no_partial_identity "$CHECK_ROOT"
TERM_ROOT="$TEST_ROOT/terminated"
if TEST_CHECK_TERM=1 run_init "$TERM_ROOT" >"$TEST_ROOT/terminated.log" 2>&1; then fail 'transaction termination rejected'; else pass 'transaction termination rejected'; fi
assert 'transaction termination rolls back published identity files' no_partial_identity "$TERM_ROOT"
if run_init "$TERM_ROOT" >"$TEST_ROOT/retry-termination.log" 2>&1; then pass 'termination permits retry with the same instance directory'; else fail 'termination permits retry with the same instance directory'; fi

FINAL_ROOT="$TEST_ROOT/fail-final-config"
if TEST_FAIL_LINK_TARGET="$FINAL_ROOT/server.conf" run_init "$FINAL_ROOT" $'\n\n18771\ny\n\n\nmock-server\n' >"$TEST_ROOT/fail-final-config.log" 2>&1; then
  fail 'final configuration publication failure rejected'
else pass 'final configuration publication failure rejected'; fi
assert 'failed final commit rolls back BPSEC2 identities' no_partial_identity "$FINAL_ROOT"

EXTERNAL_ROOT="$TEST_ROOT/external-race"
if TEST_FAIL_LINK_TARGET="$EXTERNAL_ROOT/secrets.env" TEST_EXTERNAL_CREATE=1 run_init "$EXTERNAL_ROOT" >"$TEST_ROOT/external-race.log" 2>&1; then
  fail 'external publication conflict rejected'
else pass 'external publication conflict rejected'; fi
if [ "$(cat "$EXTERNAL_ROOT/secrets.env")" = external-owned-sentinel ] &&
    [ ! -e "$EXTERNAL_ROOT/state/transport.key" ] && [ ! -e "$EXTERNAL_ROOT/server.conf" ]; then
  pass 'rollback preserves a concurrently created external file'
else fail 'rollback preserves a concurrently created external file'; fi

SYMLINK_ROOT="$TEST_ROOT/existing-symlink"
mkdir -p "$SYMLINK_ROOT"
/usr/bin/ln -s "$SYMLINK_ROOT/nonexistent" "$SYMLINK_ROOT/server.conf"
if run_init "$SYMLINK_ROOT" >"$TEST_ROOT/existing-symlink.log" 2>&1; then fail 'dangling configuration symlink refused';
elif [ -L "$SYMLINK_ROOT/server.conf" ] && [ ! -e "$SYMLINK_ROOT/state/transport.key" ]; then pass 'dangling configuration symlink preserved';
else fail 'dangling configuration symlink preserved'; fi

LOCK_ROOT="$TEST_ROOT/symlink-lock"
mkdir -p "$LOCK_ROOT"
printf 'protected lock target\n' >"$TEST_ROOT/external-lock-target"
/usr/bin/ln -s "$TEST_ROOT/external-lock-target" "$LOCK_ROOT/.backup-server-initialize.lock"
if run_init "$LOCK_ROOT" >"$TEST_ROOT/symlink-lock.log" 2>&1; then fail 'initialization lock symlink refused';
elif [ "$(cat "$TEST_ROOT/external-lock-target")" = 'protected lock target' ] && [ ! -e "$LOCK_ROOT/state/transport.key" ]; then pass 'initialization lock symlink refused without changing target';
else fail 'initialization lock symlink target preserved'; fi
HARDLINK_ROOT="$TEST_ROOT/hardlinked-lock"
mkdir -p "$HARDLINK_ROOT"
/usr/bin/ln "$TEST_ROOT/external-lock-target" "$HARDLINK_ROOT/.backup-server-initialize.lock"
if run_init "$HARDLINK_ROOT" >"$TEST_ROOT/hardlinked-lock.log" 2>&1; then fail 'hardlinked initialization lock refused';
elif [ "$(cat "$TEST_ROOT/external-lock-target")" = 'protected lock target' ] && [ ! -e "$HARDLINK_ROOT/state/transport.key" ]; then pass 'hardlinked initialization lock refused without changing inode';
else fail 'hardlinked initialization lock protected'; fi

CONCURRENT_ROOT="$TEST_ROOT/concurrent"
GATE="$TEST_ROOT/keygen-gate"
TEST_KEYGEN_GATE="$GATE" run_init "$CONCURRENT_ROOT" >"$TEST_ROOT/concurrent-first.log" 2>&1 &
FIRST_PID=$!
for ((attempt=0; attempt<100; attempt++)); do [ ! -e "$GATE.entered" ] || break; sleep 0.05; done
if [ ! -e "$GATE.entered" ]; then
  fail 'first initializer reached controlled key generation'
elif run_init "$CONCURRENT_ROOT" >"$TEST_ROOT/concurrent-second.log" 2>&1; then
  fail 'concurrent second initializer refused'
elif grep -q '另一初始化操作正在运行' "$TEST_ROOT/concurrent-second.log"; then
  pass 'concurrent second initializer refused by persistent lock'
else fail 'concurrent second initializer lock diagnostic'; fi
: >"$GATE.release"
if wait "$FIRST_PID" && [ -f "$CONCURRENT_ROOT/server.conf" ]; then pass 'first initializer completes after competing attempt'; else fail 'first concurrent initializer'; fi

# A concurrent directory must never turn an exact publication target into a
# destination directory, including a symlink to a directory outside the instance.
for race_type in directory symlink; do
  race_root="$TEST_ROOT/publication-$race_type-race"
  race_directory="$race_root/state/transport.key"
  if [ "$race_type" = symlink ]; then
    race_directory="$TEST_ROOT/external-publication-directory"
    mkdir -p "$race_directory"
  fi
  if TEST_RACE_CREATE_TARGET="$race_root/state/transport.key" \
      TEST_RACE_CREATE_TYPE="$race_type" TEST_RACE_DIRECTORY="$race_directory" \
      run_init "$race_root" >"$TEST_ROOT/publication-$race_type-race.log" 2>&1; then
    fail "concurrent $race_type publication refused"
  elif [ -d "$race_directory" ] &&
      [ -z "$(find "$race_directory" -mindepth 1 -print -quit)" ] &&
      [ ! -e "$race_root/server.conf" ] && [ ! -e "$race_root/secrets.env" ]; then
    pass "concurrent $race_type publication refused without leaking an identity link"
  else fail "concurrent $race_type publication left identity material"; fi
done

# Intercept exec while sourcing the unmodified POSIX wrapper in a Bash child.
# No actual runuser or installed backend is invoked by these routing tests.
cat >"$TEST_ROOT/tools/id" <<'MOCK'
#!/bin/sh
printf '%s\n' "${TEST_UID:-0}"
MOCK
chmod +x "$TEST_ROOT/tools/id"
WRAPPER="$ROOT_DIR/packaging/server/wrappers/backup-server-admin"
route_wrapper() (
  local log="$1"; shift
  exec() { printf 'ARG=%q\n' "$@" >"$log"; exit 0; }
  set -- "$@"
  # shellcheck source=/dev/null
  source "$WRAPPER"
)
route_wrapper "$TEST_ROOT/wrapper-root.actual" status
printf 'ARG=%q\n' runuser -u backup-project -- /usr/lib/backup-project-server/bin/backup-server-admin \
  --server-root /var/lib/backup-project-server --root /var/lib/backup-project-server/data \
  --db /var/lib/backup-project-server/state/metadata.sqlite3 status >"$TEST_ROOT/wrapper-root.expected"
assert 'installed default root CLI routes to fixed service user' cmp -s "$TEST_ROOT/wrapper-root.actual" "$TEST_ROOT/wrapper-root.expected"
TEST_UID=1000 route_wrapper "$TEST_ROOT/wrapper-user.actual" list-users
printf 'ARG=%q\n' /usr/lib/backup-project-server/bin/backup-server-admin \
  --server-root /var/lib/backup-project-server --root /var/lib/backup-project-server/data \
  --db /var/lib/backup-project-server/state/metadata.sqlite3 list-users >"$TEST_ROOT/wrapper-user.expected"
assert 'non-root installed CLI preserves current user' cmp -s "$TEST_ROOT/wrapper-user.actual" "$TEST_ROOT/wrapper-user.expected"
for option in --root --db --server-root; do
  route_wrapper "$TEST_ROOT/wrapper-custom.actual" "$option" "$TEST_ROOT/custom value" status
  printf 'ARG=%q\n' /usr/lib/backup-project-server/bin/backup-server-admin "$option" "$TEST_ROOT/custom value" status >"$TEST_ROOT/wrapper-custom.expected"
  assert "explicit $option keeps custom instance semantics" cmp -s "$TEST_ROOT/wrapper-custom.actual" "$TEST_ROOT/wrapper-custom.expected"
done
mkdir -p "$TEST_ROOT/no-runuser"
/usr/bin/ln -s "$TEST_ROOT/tools/id" "$TEST_ROOT/no-runuser/id"
if PATH="$TEST_ROOT/no-runuser" route_wrapper "$TEST_ROOT/wrapper-missing.actual" status >"$TEST_ROOT/wrapper-missing.log" 2>&1; then
  fail 'default root wrapper refuses missing runuser'
elif [ ! -e "$TEST_ROOT/wrapper-missing.actual" ] && grep -q '缺少 runuser' "$TEST_ROOT/wrapper-missing.log"; then
  pass 'default root wrapper refuses missing runuser without direct backend fallback'
else fail 'missing runuser diagnostic'; fi
printf '\n[manager-init] %s PASS / %s FAIL (mock tools only)\nEvidence: %s\n' "$PASS" "$FAIL" "$TEST_ROOT"
[ "$FAIL" = 0 ]
