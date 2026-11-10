#!/usr/bin/env bash
# Real dpkg lifecycle in an explicitly marked disposable PRoot filesystem.
set -Eeuo pipefail
ROOTFS='' PROOT='' CLIENT_DEB='' SERVER_DEB='' OUT=''
UPGRADE_CLIENT_DEB='' UPGRADE_SERVER_DEB=''
usage() {
  echo 'Usage: deb_rootfs_lifecycle_test.sh --rootfs DIR --proot FILE --client-deb FILE --server-deb FILE --out DIR [--upgrade-client-deb FILE --upgrade-server-deb FILE]' >&2
}
while [ "$#" -gt 0 ]; do
  [ "$#" -ge 2 ] || { usage; exit 2; }
  case "$1" in
    --rootfs) ROOTFS="$2"; shift 2 ;;
    --proot) PROOT="$2"; shift 2 ;;
    --client-deb) CLIENT_DEB="$2"; shift 2 ;;
    --server-deb) SERVER_DEB="$2"; shift 2 ;;
    --upgrade-client-deb) UPGRADE_CLIENT_DEB="$2"; shift 2 ;;
    --upgrade-server-deb) UPGRADE_SERVER_DEB="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    *) echo "Unknown argument: $1" >&2; exit 2 ;;
  esac
done
for value in "$ROOTFS" "$PROOT" "$CLIENT_DEB" "$SERVER_DEB" "$OUT"; do
  [ -n "$value" ] || { echo 'Require --rootfs --proot --client-deb --server-deb --out' >&2; exit 2; }
done
ROOTFS="$(realpath "$ROOTFS")"
[ "$ROOTFS" != / ] && [ -d "$ROOTFS/var/lib/dpkg" ] || exit 2
grep -qx 'backup-project-deb-lifecycle-rootfs-v1' "$ROOTFS/.backup-project-deb-test-rootfs" ||
  { echo 'Rootfs must have explicit disposable-test marker.' >&2; exit 2; }
[ -x "$PROOT" ] && [ -f "$CLIENT_DEB" ] && [ -f "$SERVER_DEB" ] || exit 2
PROOT="$(realpath "$PROOT")"
[ "$(dpkg-deb -f "$CLIENT_DEB" Package)" = backup-project-client ] &&
  [ "$(dpkg-deb -f "$SERVER_DEB" Package)" = backup-project-server ] ||
  { echo 'Input Debian package names do not match client/server roles.' >&2; exit 2; }
client_version="$(dpkg-deb -f "$CLIENT_DEB" Version)"
server_version="$(dpkg-deb -f "$SERVER_DEB" Version)"
if [ -n "$UPGRADE_CLIENT_DEB" ] || [ -n "$UPGRADE_SERVER_DEB" ]; then
  [ -n "$UPGRADE_CLIENT_DEB" ] && [ -n "$UPGRADE_SERVER_DEB" ] &&
    [ -f "$UPGRADE_CLIENT_DEB" ] && [ -f "$UPGRADE_SERVER_DEB" ] ||
    { echo 'Provide both existing upgrade Debian packages.' >&2; exit 2; }
  [ "$(dpkg-deb -f "$UPGRADE_CLIENT_DEB" Package)" = backup-project-client ] &&
    [ "$(dpkg-deb -f "$UPGRADE_SERVER_DEB" Package)" = backup-project-server ] ||
    { echo 'Upgrade Debian package names do not match client/server roles.' >&2; exit 2; }
  upgrade_client_version="$(dpkg-deb -f "$UPGRADE_CLIENT_DEB" Version)"
  upgrade_server_version="$(dpkg-deb -f "$UPGRADE_SERVER_DEB" Version)"
  dpkg --compare-versions "$upgrade_client_version" gt "$client_version" &&
    dpkg --compare-versions "$upgrade_server_version" gt "$server_version" ||
    { echo 'Upgrade versions must be strictly newer than installed input versions.' >&2; exit 2; }
fi
mkdir -p "$OUT"
OUT="$(realpath "$OUT")"
[ -z "$(ls -A "$OUT")" ] || { echo 'Output directory must be empty.' >&2; exit 2; }
chmod 0700 "$OUT"
exec > >(tee "$OUT/summary.log") 2>&1
passed=0
supervisor_pid=
server_pid=
server_start=
pass() { passed=$((passed + 1)); printf 'PASS %s\n' "$1"; }
fail() { printf 'FAIL %s\n' "$1" >&2; exit 1; }
guest() {
  "$PROOT" -0 -r "$ROOTFS" -b /dev -b /proc -w /root \
    /usr/bin/env -i HOME=/root PATH=/usr/sbin:/usr/bin:/sbin:/bin \
    LANG=C.UTF-8 DEBIAN_FRONTEND=noninteractive "$@"
}
owned_server() {
  [ -n "$server_pid" ] && [ -r "/proc/$server_pid/stat" ] &&
    grep -Fq "$ROOTFS/usr/lib/backup-project-server/bin/backup-server" "/proc/$server_pid/maps" &&
    [ "$(readlink -f "/proc/$(awk '/TracerPid:/ {print $2}' "/proc/$server_pid/status")/exe")" = "$PROOT" ] &&
    [ "$(awk '{print $22}' "/proc/$server_pid/stat")" = "$server_start" ]
}
stop_server() {
  if [ -n "$server_pid" ]; then
    owned_server || fail "Refuse to signal unowned or reused PID $server_pid"
    printf 'OWNED_STOP pid=%s start=%s exe=%s\n' "$server_pid" "$server_start" "$(readlink -f "/proc/$server_pid/exe")"
    kill -TERM "$server_pid"
    for _ in $(seq 1 100); do
      [ ! -e "/proc/$server_pid" ] && break
      sleep 0.1
    done
    [ ! -e "/proc/$server_pid" ] || fail 'Owned server did not stop within 10 seconds'
    wait "$supervisor_pid" || true
    server_pid=
    supervisor_pid=
    pass 'Only captured server PID stopped; no broad process kill'
  fi
}
cleanup() {
  rc=$?
  trap - EXIT
  if [ -n "$server_pid" ] && owned_server; then
    kill -TERM "$server_pid"
    wait "$supervisor_pid" || true
  fi
  current="$(sha256sum /var/lib/dpkg/status | cut -d' ' -f1)"
  printf '%s\n' "$current" >"$OUT/host-dpkg-status.after.sha256"
  if [ "$current" != "$host_dpkg_before" ]; then
    echo 'FAIL Host dpkg status changed (investigate external activity).' >&2
    rc=1
  else
    echo 'PASS Host dpkg status SHA-256 unchanged'
  fi
  printf 'RESULT passed=%s exit=%s\n' "$passed" "$rc"
  exit "$rc"
}
host_dpkg_before="$(sha256sum /var/lib/dpkg/status | cut -d' ' -f1)"
printf '%s\n' "$host_dpkg_before" >"$OUT/host-dpkg-status.before.sha256"
trap cleanup EXIT
echo 'SCOPE actual dpkg in PRoot; no PID/network namespace; non-systemd; no GUI'
printf 'ROOTFS=%s\n' "$ROOTFS"
mkdir -m 0700 "$OUT/host-only"
printf '#!/bin/sh\nexit 0\n' >"$OUT/host-only/visibility-sentinel"
chmod 0700 "$OUT/host-only/visibility-sentinel"
guest /bin/bash -ec 'test ! -d /run/systemd/system; test ! -e "$1"; test ! -x "$1"; dpkg --audit' bash "$OUT/host-only/visibility-sentinel"
pass 'Non-systemd rootfs, host-only path absent, dependency database audit clean'
mkdir -p "$ROOTFS/packages" "$ROOTFS/root/lifecycle-source/subdir" "$ROOTFS/root/lifecycle-client-data"
cp "$CLIENT_DEB" "$ROOTFS/packages/client.deb"
cp "$SERVER_DEB" "$ROOTFS/packages/server.deb"
sha256sum "$CLIENT_DEB" "$SERVER_DEB" >"$OUT/input-artifacts.sha256"
if [ -n "$UPGRADE_CLIENT_DEB" ]; then
  cp "$UPGRADE_CLIENT_DEB" "$ROOTFS/packages/upgrade-client.deb"
  cp "$UPGRADE_SERVER_DEB" "$ROOTFS/packages/upgrade-server.deb"
  sha256sum "$UPGRADE_CLIENT_DEB" "$UPGRADE_SERVER_DEB" >>"$OUT/input-artifacts.sha256"
fi
guest dpkg -i /packages/client.deb /packages/server.deb >"$OUT/dpkg-install.log" 2>&1
guest dpkg-query -W '-f=${Package} ${Status} ${Version}\n' backup-project-client backup-project-server >"$OUT/dpkg-status-installed.log"
grep -q 'backup-project-client install ok installed' "$OUT/dpkg-status-installed.log"
grep -q 'backup-project-server install ok installed' "$OUT/dpkg-status-installed.log"
pass 'Real dpkg install/configure completed for both original Debian packages'
guest backupctl --help >"$OUT/client-help.log" 2>&1
guest /usr/lib/backup-project-server/bin/backup-server --help >"$OUT/server-help.log" 2>&1
pass 'Installed client CLI and server binaries execute'
PORT="$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')"
printf 'LOOPBACK_PORT=%s\n' "$PORT"
guest /bin/bash -ec 'sed -i "s/^port = .*/port = $1/" /etc/backup-project-server/server.conf; mkdir -p /run/backup-project-server; printf "keep client settings\n" >/root/lifecycle-client-data/settings.txt; printf "source v1\n" >/root/lifecycle-source/a.txt; printf "nested\n" >/root/lifecycle-source/subdir/nested.txt; : >/root/lifecycle-source/empty.txt' bash "$PORT"
printf 'synthetic-%s\n' "$(head -c 16 /dev/urandom | od -An -tx1 | tr -d ' \n')" >"$ROOTFS/root/lifecycle-account-password"
chmod 0600 "$ROOTFS/root/lifecycle-account-password"
pin="$(guest /usr/lib/backup-project-server/bin/backup-server-keygen --show --key-file /var/lib/backup-project-server/state/transport.key 2>/dev/null | sed -n 's/.*--server-key \(sha256:[0-9a-f]*\).*/\1/p' | head -1)"
[ "${#pin}" -eq 71 ] || fail 'Could not derive installed server pin'
remote() {
  guest /bin/bash -c 'export BACKUP_REMOTE_PASSWORD="$(cat /root/lifecycle-account-password)"; exec backupctl remote "$@"' bash "$@" --host 127.0.0.1 --port "$PORT" --server-key "$pin" --user package-lifecycle
}
start_server() {
  tag="$1"
  rm -f "$ROOTFS/run/backup-project-server/backup-server.pid"
  guest /usr/lib/backup-project-server/bin/launch-server.sh --config /etc/backup-project-server/server.conf >"$OUT/server-$tag.log" 2>&1 &
  supervisor_pid=$!
  for _ in $(seq 1 100); do
    [ -s "$ROOTFS/run/backup-project-server/backup-server.pid" ] && break
    kill -0 "$supervisor_pid" 2>/dev/null || break
    sleep 0.1
  done
  [ -s "$ROOTFS/run/backup-project-server/backup-server.pid" ] || fail "No PID file after start $tag"
  server_pid="$(cat "$ROOTFS/run/backup-project-server/backup-server.pid")"
  case "$server_pid" in ''|*[!0-9]*) fail 'Invalid server PID file';; esac
  server_start="$(awk '{print $22}' "/proc/$server_pid/stat")"
  owned_server || fail 'Server PID/executable ownership validation failed'
  printf 'OWNED_START tag=%s pid=%s start=%s exe=%s\n' "$tag" "$server_pid" "$server_start" "$(readlink -f "/proc/$server_pid/exe")"
  remote ping >"$OUT/ping-$tag.log" 2>&1
  pass "Installed launch-server starts and BPSEC1 ping succeeds ($tag)"
}
state_manifest() {
  (
    cd "$ROOTFS"
    find etc/backup-project-server var/lib/backup-project-server root/lifecycle-client-data -type f -print0 |
      LC_ALL=C sort -z | xargs -0 sha256sum
  ) >"$OUT/$1"
}
start_server initial
remote register >"$OUT/register.log" 2>&1
remote login >"$OUT/login-initial.log" 2>&1
pass 'Installed client registers and authenticates synthetic account'
remote backup /root/lifecycle-source --strategy full --name package-lifecycle-full >"$OUT/remote-backup.log" 2>&1
snapshot_id="$(grep -oE '快照 ID: +[0-9a-f]{32}' "$OUT/remote-backup.log" | grep -oE '[0-9a-f]{32}')"
[ "${#snapshot_id}" -eq 32 ] || fail 'Backup did not return snapshot ID'
remote restore "$snapshot_id" /root/lifecycle-restored-initial >"$OUT/remote-restore-initial.log" 2>&1
guest diff -qr /root/lifecycle-source /root/lifecycle-restored-initial >"$OUT/restore-diff-initial.log"
pass 'Package-installed client/server full backup and exact restore'
stop_server
state_manifest state-before-transition.sha256
if [ -n "$UPGRADE_CLIENT_DEB" ]; then
  transition_tag=upgraded
  guest dpkg --force-confold -i /packages/upgrade-client.deb /packages/upgrade-server.deb >"$OUT/dpkg-upgrade.log" 2>&1
  [ "$(guest dpkg-query -W '-f=${Version}' backup-project-client)" = "$upgrade_client_version" ] ||
    fail 'Installed client version differs from upgrade package'
  [ "$(guest dpkg-query -W '-f=${Version}' backup-project-server)" = "$upgrade_server_version" ] ||
    fail 'Installed server version differs from upgrade package'
  printf 'UPGRADE client=%s->%s server=%s->%s\n' "$client_version" "$upgrade_client_version" "$server_version" "$upgrade_server_version"
else
  transition_tag=reinstalled
  guest dpkg --force-confold -i /packages/client.deb /packages/server.deb >"$OUT/dpkg-same-version-reinstall.log" 2>&1
fi
state_manifest state-after-transition.sha256
cmp "$OUT/state-before-transition.sha256" "$OUT/state-after-transition.sha256"
guest dpkg-query -W '-f=${Package} ${Status} ${Version}\n' backup-project-client backup-project-server >"$OUT/dpkg-status-$transition_tag.log"
grep -q 'backup-project-client install ok installed' "$OUT/dpkg-status-$transition_tag.log"
grep -q 'backup-project-server install ok installed' "$OUT/dpkg-status-$transition_tag.log"
guest backupctl --help >"$OUT/client-help-$transition_tag.log" 2>&1
guest /usr/lib/backup-project-server/bin/backup-server --help >"$OUT/server-help-$transition_tag.log" 2>&1
pass "dpkg $transition_tag preserves config, secret, identity, database, blobs and client data"
start_server "$transition_tag"
remote login >"$OUT/login-after-$transition_tag.log" 2>&1
remote list >"$OUT/list-after-$transition_tag.log" 2>&1
grep -q "$snapshot_id" "$OUT/list-after-$transition_tag.log"
remote restore "$snapshot_id" "/root/lifecycle-restored-$transition_tag" >"$OUT/remote-restore-$transition_tag.log" 2>&1
guest diff -qr /root/lifecycle-source "/root/lifecycle-restored-$transition_tag" >"$OUT/restore-diff-$transition_tag.log"
pass "After $transition_tag, old account/backup remain usable and restore matches"
stop_server
state_manifest state-before-remove.sha256
guest dpkg -r backup-project-client backup-project-server >"$OUT/dpkg-remove.log" 2>&1
state_manifest state-after-remove.sha256
cmp "$OUT/state-before-remove.sha256" "$OUT/state-after-remove.sha256"
guest /bin/bash -c 'test ! -e /usr/bin/backupctl; test ! -e /usr/lib/backup-project-server/bin/backup-server'
pass 'dpkg remove removes binaries and preserves data/config/identity/client data'
guest dpkg --purge backup-project-client backup-project-server >"$OUT/dpkg-purge.log" 2>&1
state_manifest state-after-purge.sha256
# dpkg removes declared conffiles on purge. The generated secret and server
# state are not packaged conffiles and must remain unchanged.
grep -v '  etc/backup-project-server/server.conf$' "$OUT/state-before-remove.sha256" >"$OUT/state-before-purge-preserved.sha256"
grep -v '  etc/backup-project-server/server.conf$' "$OUT/state-after-purge.sha256" >"$OUT/state-after-purge-preserved.sha256"
cmp "$OUT/state-before-purge-preserved.sha256" "$OUT/state-after-purge-preserved.sha256"
guest dpkg --audit >"$OUT/dpkg-audit-final.log"
guest dpkg-query -W '-f=${Package} ${Status} ${Version}\n' backup-project-client backup-project-server >"$OUT/dpkg-status-final.log" 2>&1 || true
pass 'dpkg purge preserves generated secret, identity, database, blobs and client data'
if [ -e "$ROOTFS/etc/backup-project-server/server.conf" ]; then
  echo 'PURGE_SERVER_CONF=preserved'
else
  echo 'PURGE_SERVER_CONF=removed-by-dpkg-conffiles-policy'
fi
echo 'LIMIT systemd auto-start/stop and enable failure rollback not tested.'
if [ -z "$UPGRADE_CLIENT_DEB" ]; then
  echo 'LIMIT True cross-version upgrade not tested (same-version reinstall only).'
fi
echo 'LIMIT PRoot fake root does not prove real ownership or user privilege isolation.'
