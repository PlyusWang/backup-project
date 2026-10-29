#!/usr/bin/env bash
# Real loopback BPSEC2 + an independent, unprivileged OpenSSH fixture.
# Requires built server/cert-tool/modern GUI and openssh-server. No user SSH
# configuration, existing daemon, agent, personal key or non-loopback endpoint
# is used. Missing sshd capability is BLOCKED (77), never a passing skip.
set -euo pipefail

TASK_REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mkdir -p "$TASK_REPO/tests/output"
TASK_OUTPUT="${BACKUP_NETWORK_OUTPUT:-$(mktemp -d "$TASK_REPO/tests/output/network-acceptance-XXXXXX")}"
mkdir -p "$TASK_OUTPUT"
TASK_OUTPUT="$(cd "$TASK_OUTPUT" && pwd)"
TASK_RUNTIME="$(mktemp -d "$TASK_OUTPUT/runtime-XXXXXX")"
TASK_SERVER_PID=""
TASK_SSHD_PID=""
cleanup() {
  local code=$?
  trap - EXIT
  if [ -n "$TASK_SERVER_PID" ]; then
    kill "$TASK_SERVER_PID" 2>/dev/null || true
    wait "$TASK_SERVER_PID" 2>/dev/null || true
  fi
  if [ -n "$TASK_SSHD_PID" ]; then
    kill "$TASK_SSHD_PID" 2>/dev/null || true
    wait "$TASK_SSHD_PID" 2>/dev/null || true
  fi
  for log in server.out server.log sshd.log sshd.out; do
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
import secrets
import shlex
import socket
import sys

runtime = Path(sys.argv[1])
ssh = sys.argv[2]
user = sys.argv[3]
listeners = [socket.socket(), socket.socket()]
for listener in listeners:
    listener.bind(('127.0.0.1', 0))
backup_port, ssh_port = [listener.getsockname()[1] for listener in listeners]
for listener in listeners:
    listener.close()
runtime.joinpath('backup.port').write_text(str(backup_port))
runtime.joinpath('ssh.target').write_text(f'{user}@127.0.0.1')
runtime.joinpath('secrets.env').write_text(f'BACKUP_TOKEN_SECRET={secrets.token_hex(32)}\n')
host_type, host_public = runtime.joinpath('ssh-host.pub').read_text().split()[:2]
runtime.joinpath('known_hosts').write_text(f'[127.0.0.1]:{ssh_port} {host_type} {host_public}\n')
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
PermitOpen 127.0.0.1:{backup_port}
GatewayPorts no
X11Forwarding no
PrintMotd no
LogLevel VERBOSE
'''
runtime.joinpath('sshd_config').write_text(config)
arguments = [ssh, '-F', '/dev/null', '-p', str(ssh_port), '-i', str(runtime / 'ssh-client'),
             '-o', f'UserKnownHostsFile={runtime / "known_hosts"}',
             '-o', f'GlobalKnownHostsFile={runtime / "known_hosts"}',
             '-o', 'StrictHostKeyChecking=yes', '-o', 'IdentitiesOnly=yes',
             '-o', 'IdentityAgent=none']
wrapper = runtime / 'fixture-ssh'
wrapper.write_text('#!/bin/sh\nexec ' + ' '.join(shlex.quote(x) for x in arguments) + ' "$@"\n')
wrapper.chmod(0o700)
for name in ['secrets.env', 'sshd_config', 'known_hosts', 'authorized_keys']:
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
if ! python3 - "$TASK_RUNTIME" <<'PY'
from pathlib import Path
import socket
import sys
import time

runtime = Path(sys.argv[1])
ports = [int(runtime.joinpath('backup.port').read_text())]
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
mkdir -p "$TASK_OUTPUT/config" "$TASK_OUTPUT/cache" "$TASK_OUTPUT/data" "$TASK_OUTPUT/tmp"
export XDG_CONFIG_HOME="$TASK_OUTPUT/config"
export XDG_CACHE_HOME="$TASK_OUTPUT/cache"
export XDG_DATA_HOME="$TASK_OUTPUT/data"
export TMPDIR="$TASK_OUTPUT/tmp"
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}"
echo "NETWORK isolated fixture ready; output=$TASK_OUTPUT platform=$QT_QPA_PLATFORM"
"$TASK_REPO/build/backup-gui-modern" --network-acceptance "$TASK_RUNTIME" \
  "$TASK_OUTPUT/screenshots" 2>&1 | tee "$TASK_OUTPUT/network.log"
