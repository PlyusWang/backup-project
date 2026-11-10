#!/usr/bin/env bash
# Real certificate import and three sequential GUI processes sharing a public connection profile.
# Requires built server/cert-tool/modern GUI and openssh-server. No user SSH
# configuration, existing daemon, agent, personal key or non-loopback endpoint
# is used. Missing sshd capability is BLOCKED (77), never a passing skip.
set -euo pipefail

TASK_REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mkdir -p "$TASK_REPO/tests/output"
TASK_OUTPUT="${BACKUP_CERTIFICATE_RESTART_OUTPUT:-$(mktemp -d "$TASK_REPO/tests/output/certificate-restart-XXXXXX")}"
mkdir -p "$TASK_OUTPUT"
TASK_OUTPUT="$(cd "$TASK_OUTPUT" && pwd)"
TASK_RUNTIME="$(mktemp -d "$TASK_OUTPUT/runtime-XXXXXX")"
TASK_SERVER_PID=""
TASK_BPSEC1_SERVER_PID=""
TASK_SSHD_PID=""
cleanup() {
  local code=$?
  trap - EXIT
  if [ -n "$TASK_SERVER_PID" ]; then
    kill "$TASK_SERVER_PID" 2>/dev/null || true
    wait "$TASK_SERVER_PID" 2>/dev/null || true
  fi
  if [ -n "$TASK_BPSEC1_SERVER_PID" ]; then
    kill "$TASK_BPSEC1_SERVER_PID" 2>/dev/null || true
    wait "$TASK_BPSEC1_SERVER_PID" 2>/dev/null || true
  fi
  if [ -n "$TASK_SSHD_PID" ]; then
    kill "$TASK_SSHD_PID" 2>/dev/null || true
    wait "$TASK_SSHD_PID" 2>/dev/null || true
  fi
  for log in server.out server.log bpsec1-server.out bpsec1-server.log sshd.log sshd.out; do
    if [ -f "$TASK_RUNTIME/$log" ]; then
      cp "$TASK_RUNTIME/$log" "$TASK_OUTPUT/$log"
    fi
  done
  # The unique runtime is created by this invocation and contains all private
  # test keys. Only screenshots and process logs survive the fixture.
  rm -rf -- "$TASK_RUNTIME"
  exit "$code"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
umask 077

TASK_SSHD="$(command -v sshd || true)"
if [ -z "$TASK_SSHD" ] && [ -x /usr/sbin/sshd ]; then TASK_SSHD=/usr/sbin/sshd; fi
if [ -z "$TASK_SSHD" ] || [ ! -d /run/sshd ]; then
  echo "NETWORK BLOCKED: isolated sshd requires openssh-server and existing /run/sshd" >&2
  exit 77
fi
for tool in python3 ssh ssh-keygen; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "NETWORK BLOCKED: missing fixture dependency $tool" >&2
    exit 77
  fi
done
for binary in backup-server backup-server-keygen backup-cert-tool backup-gui-modern; do
  if [ ! -x "$TASK_REPO/build/$binary" ]; then
    echo "NETWORK FAIL: build/$binary is missing; build server, cert-tool and gui-modern first" >&2
    exit 1
  fi
done

TASK_GUI_SHA="$(sha256sum "$TASK_REPO/build/backup-gui-modern" | cut -d ' ' -f 1)"
"$TASK_REPO/build/backup-cert-tool" root-init --root-key "$TASK_RUNTIME/root.key" \
  --root-id acceptance-loopback-root > "$TASK_RUNTIME/root-init.log"
"$TASK_REPO/build/backup-cert-tool" root-init --root-key "$TASK_RUNTIME/wrong-root.key" \
  --root-id acceptance-untrusted-root > "$TASK_RUNTIME/wrong-root-init.log"
"$TASK_REPO/build/backup-server-keygen" --output "$TASK_RUNTIME/transport.key" \
  > "$TASK_RUNTIME/keygen.log"
TASK_SERVER_PUBLIC="$("$TASK_REPO/build/backup-server-keygen" --show \
  --key-file "$TASK_RUNTIME/transport.key" | sed -n 's/.*--server-key hex:\([0-9a-f]*\)$/\1/p')"
if [ "${#TASK_SERVER_PUBLIC}" -ne 64 ]; then
  echo "NETWORK FAIL: cannot obtain generated server public key" >&2
  exit 1
fi
"$TASK_REPO/build/backup-server-keygen" --output "$TASK_RUNTIME/bpsec1-transport.key" \
  > "$TASK_RUNTIME/bpsec1-keygen.log"
TASK_BPSEC1_PUBLIC="$("$TASK_REPO/build/backup-server-keygen" --show \
  --key-file "$TASK_RUNTIME/bpsec1-transport.key" | sed -n 's/.*--server-key sha256:\([0-9a-f]*\)$/sha256:\1/p')"
if [[ ! "$TASK_BPSEC1_PUBLIC" =~ ^sha256:[0-9a-f]{64}$ ]]; then
  echo "NETWORK FAIL: cannot obtain BPSEC1 server key pin" >&2
  exit 1
fi
printf '%s\n' "$TASK_BPSEC1_PUBLIC" > "$TASK_RUNTIME/bpsec1.server-key"
"$TASK_REPO/build/backup-cert-tool" issue-server --root-key "$TASK_RUNTIME/root.key" \
  --server-id acceptance-loopback-server --server-pubkey "$TASK_SERVER_PUBLIC" \
  --out "$TASK_RUNTIME/server.bpcert" > "$TASK_RUNTIME/issue.log"
"$TASK_REPO/build/backup-cert-tool" verify-server --cert "$TASK_RUNTIME/server.bpcert" \
  --roots "$TASK_RUNTIME/root.key.pub" > "$TASK_RUNTIME/verify.log"

ssh-keygen -q -t ed25519 -N '' -f "$TASK_RUNTIME/ssh-host"
ssh-keygen -q -t ed25519 -N '' -f "$TASK_RUNTIME/ssh-client"
cp "$TASK_RUNTIME/ssh-client.pub" "$TASK_RUNTIME/authorized_keys"
TASK_SSH="$(command -v ssh)"
TASK_USER="$(id -un)"
python3 - "$TASK_RUNTIME" "$TASK_SSH" "$TASK_USER" <<'PY'
import os
from pathlib import Path
import pwd
import secrets
import shlex
import socket
import sys

runtime = Path(sys.argv[1])
ssh = sys.argv[2]
user = sys.argv[3]
listeners = [socket.socket(), socket.socket(), socket.socket()]
for listener in listeners:
    listener.bind(('127.0.0.1', 0))
backup_port, bpsec1_port, ssh_port = [listener.getsockname()[1] for listener in listeners]
for listener in listeners:
    listener.close()
runtime.joinpath('backup.port').write_text(str(backup_port))
runtime.joinpath('bpsec1.port').write_text(str(bpsec1_port))
runtime.joinpath('ssh.target').write_text(f'{user}@127.0.0.1')
while True:
    missing_user = 'bp-no-user-' + secrets.token_hex(8)
    try:
        pwd.getpwnam(missing_user)
    except KeyError:
        break
runtime.joinpath('ssh.invalid-target').write_text(f'{missing_user}@127.0.0.1')
runtime.joinpath('secrets.env').write_text(f'BACKUP_TOKEN_SECRET={secrets.token_hex(32)}\n')
runtime.joinpath('bpsec1-secrets.env').write_text(f'BACKUP_TOKEN_SECRET={secrets.token_hex(32)}\n')
host_type, host_public = runtime.joinpath('ssh-host.pub').read_text().split()[:2]
runtime.joinpath('known_hosts').write_text(f'[127.0.0.1]:{ssh_port} {host_type} {host_public}\n')
runtime.joinpath('empty_known_hosts').write_text('')
config = f'''Port {ssh_port}
ListenAddress 127.0.0.1
HostKey "{runtime / 'ssh-host'}"
PidFile "{runtime / 'sshd.pid'}"
AuthorizedKeysFile "{runtime / 'authorized_keys'}"
AllowUsers {user}
AuthenticationMethods publickey
PubkeyAuthentication yes
PasswordAuthentication no
KbdInteractiveAuthentication no
UsePAM no
PermitRootLogin prohibit-password
StrictModes yes
AllowTcpForwarding local
PermitOpen 127.0.0.1:{backup_port} 127.0.0.1:{bpsec1_port}
GatewayPorts no
X11Forwarding no
PrintMotd no
LogLevel VERBOSE
'''
runtime.joinpath('sshd_config').write_text(config)
for name, hosts in [('fixture-ssh', 'known_hosts'),
                    ('fixture-ssh-untrusted', 'empty_known_hosts')]:
    arguments = [ssh, '-F', '/dev/null', '-p', str(ssh_port), '-i', str(runtime / 'ssh-client'),
                 '-o', f'UserKnownHostsFile={runtime / hosts}',
                 '-o', f'GlobalKnownHostsFile={runtime / hosts}',
                 '-o', 'StrictHostKeyChecking=yes', '-o', 'IdentitiesOnly=yes',
                 '-o', 'IdentityAgent=none']
    wrapper = runtime / name
    wrapper.write_text('#!/bin/sh\nexec ' + ' '.join(shlex.quote(x) for x in arguments) + ' "$@"\n')
    wrapper.chmod(0o700)
for name in ['secrets.env', 'bpsec1-secrets.env', 'sshd_config', 'known_hosts',
             'empty_known_hosts', 'authorized_keys']:
    runtime.joinpath(name).chmod(0o600)
PY

if ! "$TASK_SSHD" -t -f "$TASK_RUNTIME/sshd_config" > "$TASK_RUNTIME/sshd.out" 2>&1; then
  echo "NETWORK BLOCKED: current user cannot validate the isolated sshd configuration" >&2
  cat "$TASK_RUNTIME/sshd.out" >&2
  exit 77
fi
"$TASK_SSHD" -D -f "$TASK_RUNTIME/sshd_config" -E "$TASK_RUNTIME/sshd.log" \
  >> "$TASK_RUNTIME/sshd.out" 2>&1 &
TASK_SSHD_PID=$!
mkdir -p "$TASK_RUNTIME/blobs"
TASK_BACKUP_PORT="$(cat "$TASK_RUNTIME/backup.port")"
"$TASK_REPO/build/backup-server" --bind 127.0.0.1 --port "$TASK_BACKUP_PORT" \
  --root "$TASK_RUNTIME/blobs" --db "$TASK_RUNTIME/metadata.sqlite3" \
  --secret-file "$TASK_RUNTIME/secrets.env" --transport-key-file "$TASK_RUNTIME/transport.key" \
  --bpsec2-cert-file "$TASK_RUNTIME/server.bpcert" --require-bpsec2 \
  --pid-file "$TASK_RUNTIME/server.pid" --log-file "$TASK_RUNTIME/server.log" \
  > "$TASK_RUNTIME/server.out" 2>&1 &
TASK_SERVER_PID=$!
TASK_BPSEC1_PORT="$(cat "$TASK_RUNTIME/bpsec1.port")"
mkdir -p "$TASK_RUNTIME/bpsec1-blobs"
"$TASK_REPO/build/backup-server" --bind 127.0.0.1 --port "$TASK_BPSEC1_PORT" \
  --root "$TASK_RUNTIME/bpsec1-blobs" --db "$TASK_RUNTIME/bpsec1-metadata.sqlite3" \
  --secret-file "$TASK_RUNTIME/bpsec1-secrets.env" \
  --transport-key-file "$TASK_RUNTIME/bpsec1-transport.key" \
  --pid-file "$TASK_RUNTIME/bpsec1-server.pid" --log-file "$TASK_RUNTIME/bpsec1-server.log" \
  > "$TASK_RUNTIME/bpsec1-server.out" 2>&1 &
TASK_BPSEC1_SERVER_PID=$!
if ! python3 - "$TASK_RUNTIME" <<'PY'
from pathlib import Path
import socket
import sys
import time

runtime = Path(sys.argv[1])
ports = [int(runtime.joinpath('backup.port').read_text()),
         int(runtime.joinpath('bpsec1.port').read_text())]
ports.append(int(next(line.split()[1] for line in runtime.joinpath('sshd_config').read_text().splitlines()
                      if line.startswith('Port '))))
deadline = time.monotonic() + 10
while time.monotonic() < deadline:
    try:
        for port in ports:
            with socket.create_connection(('127.0.0.1', port), timeout=0.2):
                pass
        sys.exit(0)
    except OSError:
        time.sleep(0.05)
sys.exit(1)
PY
then
  echo "NETWORK BLOCKED: isolated sshd/server did not become reachable" >&2
  exit 77
fi

# All durable client state, SSH identities and test credentials stay inside the
# owned runtime. Each GUI exits (and releases the normal single-instance lock)
# before the next one starts. No lock bypass or in-process controller recreation.
mkdir -p "$TASK_RUNTIME/home" "$TASK_RUNTIME/config" "$TASK_RUNTIME/cache" \
  "$TASK_RUNTIME/data" "$TASK_RUNTIME/tmp" "$TASK_RUNTIME/profile" "$TASK_RUNTIME/xdg-runtime"
export XDG_CONFIG_HOME="$TASK_RUNTIME/config"
export XDG_CACHE_HOME="$TASK_RUNTIME/cache"
export XDG_DATA_HOME="$TASK_RUNTIME/data"
export XDG_RUNTIME_DIR="$TASK_RUNTIME/xdg-runtime"
export TMPDIR="$TASK_RUNTIME/tmp"
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}"
export BACKUP_CERTIFICATE_RESTART_FIXTURE="$TASK_RUNTIME"
export BACKUP_CERTIFICATE_RESTART_PROFILE="$TASK_RUNTIME/profile/config.json.remote.json"
python3 - "$TASK_RUNTIME" <<'PY'
from pathlib import Path
import secrets
import sys
runtime = Path(sys.argv[1])
runtime.joinpath('restart.password').write_text(secrets.token_hex(24))
certificate = runtime.joinpath('server.bpcert').read_bytes()
runtime.joinpath('renamed-not-the-server-id.bpcert').write_bytes(certificate)
runtime.joinpath('tampered.bpcert').write_bytes(certificate[:-1] + bytes([certificate[-1] ^ 1]))
PY
"$TASK_REPO/build/backup-cert-tool" issue-server --root-key "$TASK_RUNTIME/root.key" \
  --server-id acceptance-loopback-server --server-pubkey "$TASK_SERVER_PUBLIC" \
  --not-before 1000000000 --days 1 --out "$TASK_RUNTIME/expired.bpcert" \
  > "$TASK_RUNTIME/expired-issue.log"
for stage in prepare reopen-direct reopen-ssh; do
  export BACKUP_CERTIFICATE_RESTART_STAGE="$stage"
  env HOME="$TASK_RUNTIME/home" "$TASK_REPO/build/backup-gui-modern" \
    --config-file "$TASK_RUNTIME/profile/config.json" \
    --schedule-file "$TASK_RUNTIME/profile/schedule.json" \
    --realtime-file "$TASK_RUNTIME/profile/realtime.json" \
    --credential-ui-test "$TASK_OUTPUT/screenshots" 2>&1 | tee "$TASK_OUTPUT/$stage.log"
done
python3 - "$TASK_RUNTIME" "$TASK_OUTPUT" <<'PY'
import hashlib
import json
from pathlib import Path
import re
import sys
runtime, output = map(Path, sys.argv[1:])
profile = runtime / 'profile/config.json.remote.json'
raw = profile.read_bytes()
data = json.loads(raw)
allowed = {'version', 'connection_mode', 'auth_mode', 'host', 'port', 'server_key_pin',
           'trusted_roots_file', 'expected_server_id', 'ssh_host', 'ssh_local_port'}
if set(data) != allowed or runtime.joinpath('restart.password').read_bytes() in raw:
    raise SystemExit('CERTIFICATE_RESTART FAIL: profile schema or credential exclusion')
pids = []
for stage in ('prepare', 'reopen-direct', 'reopen-ssh'):
    log = output.joinpath(stage + '.log').read_text()
    found = re.search(r'stage=' + stage + r' pid=(\d+) passed=(\d+) failed=0', log)
    if not found:
        raise SystemExit('CERTIFICATE_RESTART FAIL: missing stage result')
    pids.append(int(found[1]))
if len(set(pids)) != 3:
    raise SystemExit('CERTIFICATE_RESTART FAIL: GUI processes were not distinct')
evidence = {'passed': False, 'process_count': 3, 'distinct_gui_pids': pids,
            'final_transport': data['connection_mode'], 'auth_mode': data['auth_mode'],
            'server_id': data['expected_server_id'], 'profile_sha256': hashlib.sha256(raw).hexdigest(),
            'profile_fields': sorted(data), 'private_permissions': oct(profile.stat().st_mode & 0o777)}
output.joinpath('restart-evidence.json').write_text(json.dumps(evidence, indent=2) + '\n')
runtime.joinpath('valid-profile.json').write_bytes(raw)
PY
export BACKUP_CERTIFICATE_RESTART_STAGE=invalid-profile
for case_name in unknown-field fifo symlink oversize ssh-target server-id control pin-space; do
  python3 - "$TASK_RUNTIME" "$case_name" <<'PY'
import json
import os
import hashlib
import stat
from pathlib import Path
import sys
runtime, case = Path(sys.argv[1]), sys.argv[2]
profile = runtime / 'profile/config.json.remote.json'
profile.unlink()
if case == 'fifo':
    os.mkfifo(profile, 0o600)
elif case == 'symlink':
    profile.symlink_to(runtime / 'valid-profile.json')
elif case == 'oversize':
    profile.write_bytes(b'x' * 65537)
else:
    data = json.loads(runtime.joinpath('valid-profile.json').read_text())
    if case == 'unknown-field':
        data['password'] = runtime.joinpath('restart.password').read_text()
    elif case == 'ssh-target':
        data['ssh_host'] = '-oProxyCommand=unwanted'
    elif case == 'server-id':
        data['expected_server_id'] = 'x' * 129
    elif case == 'control':
        data['trusted_roots_file'] += '\n'
    elif case == 'pin-space':
        data['server_key_pin'] = ' ' + data['server_key_pin']
    else:
        raise SystemExit('unknown invalid-profile case')
    profile.write_text(json.dumps(data))
# Snapshot before the GUI starts; do not open FIFO or follow a symlink.
info = profile.lstat()
fingerprint = {'device': info.st_dev, 'inode': info.st_ino,
               'mode': info.st_mode, 'size': info.st_size, 'mtime_ns': info.st_mtime_ns}
if stat.S_ISREG(info.st_mode):
    fingerprint['sha256'] = hashlib.sha256(profile.read_bytes()).hexdigest()
elif stat.S_ISLNK(info.st_mode):
    fingerprint['target'] = os.readlink(profile)
runtime.joinpath('invalid-before.json').write_text(json.dumps(fingerprint, sort_keys=True))
PY
  timeout --foreground 60 env HOME="$TASK_RUNTIME/home" "$TASK_REPO/build/backup-gui-modern" \
    --config-file "$TASK_RUNTIME/profile/config.json" \
    --schedule-file "$TASK_RUNTIME/profile/schedule.json" \
    --realtime-file "$TASK_RUNTIME/profile/realtime.json" \
    --credential-ui-test "$TASK_OUTPUT/screenshots" 2>&1 | tee "$TASK_OUTPUT/invalid-$case_name.log"
  python3 - "$TASK_RUNTIME" "$TASK_OUTPUT" "$case_name" <<'PY'
import hashlib
import json
import os
from pathlib import Path
import stat
import sys
runtime, output, case = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
profile = runtime / 'profile/config.json.remote.json'
info = profile.lstat()
fingerprint = {'device': info.st_dev, 'inode': info.st_ino,
               'mode': info.st_mode, 'size': info.st_size, 'mtime_ns': info.st_mtime_ns}
if stat.S_ISREG(info.st_mode):
    fingerprint['sha256'] = hashlib.sha256(profile.read_bytes()).hexdigest()
elif stat.S_ISLNK(info.st_mode):
    fingerprint['target'] = os.readlink(profile)
before = json.loads(runtime.joinpath('invalid-before.json').read_text())
if fingerprint != before:
    raise SystemExit('CERTIFICATE_RESTART FAIL: startup rewrote malformed profile ' + case)
evidence_path = output / 'invalid-profile-integrity.json'
evidence = json.loads(evidence_path.read_text()) if evidence_path.exists() else {}
evidence[case] = {'unchanged_since_before_gui_start': True, **fingerprint}
evidence_path.write_text(json.dumps(evidence, indent=2) + '\n')
print('CERTIFICATE_RESTART EXTERNAL_STARTUP_PROFILE_UNCHANGED_' + case + '=PASS')
PY
done
python3 - "$TASK_OUTPUT" "$TASK_REPO/build/backup-gui-modern" "$TASK_GUI_SHA" <<'PY'
import hashlib
import json
from pathlib import Path
import re
import sys
output = Path(sys.argv[1])
binary = Path(sys.argv[2])
if hashlib.sha256(binary.read_bytes()).hexdigest() != sys.argv[3]:
    raise SystemExit('CERTIFICATE_RESTART FAIL: GUI binary changed during sequential process test')
path = output / 'restart-evidence.json'
evidence = json.loads(path.read_text())
cases = ('unknown-field', 'fifo', 'symlink', 'oversize', 'ssh-target', 'server-id', 'control', 'pin-space')
integrity = json.loads(output.joinpath('invalid-profile-integrity.json').read_text())
if set(integrity) != set(cases) or not all(x['unchanged_since_before_gui_start'] for x in integrity.values()):
    raise SystemExit('CERTIFICATE_RESTART FAIL: missing external startup integrity')
for case in cases:
    log = output.joinpath('invalid-' + case + '.log').read_text()
    found = re.search(r'stage=invalid-profile pid=(\d+) passed=(\d+) failed=0', log)
    if not found or int(found[1]) in evidence['distinct_gui_pids']:
        raise SystemExit('CERTIFICATE_RESTART FAIL: malformed-profile process result')
    evidence['distinct_gui_pids'].append(int(found[1]))
evidence['rejected_profile_cases'] = list(cases)
evidence['external_before_after_integrity_passed'] = True
evidence['gui_sha256'] = sys.argv[3]
evidence['process_count'] = len(evidence['distinct_gui_pids'])
evidence['passed'] = True
path.write_text(json.dumps(evidence, indent=2) + '\n')
PY
echo 'CERTIFICATE_RESTART all sequential GUI stages passed; private fixture cleanup follows'
