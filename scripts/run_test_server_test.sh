#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
W="$(mktemp -d)"
LOG="$W/server.log"
PORT=26433
PID=""
cleanup() {
  if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then kill -INT "$PID" 2>/dev/null || true; wait "$PID" 2>/dev/null || true; fi
  rm -rf "$W"
}
trap cleanup EXIT
cd "$ROOT"
BACKUP_TEST_PORT="$PORT" bash scripts/run_test_server.sh >"$LOG" 2>&1 &
PID=$!
for i in $(seq 1 100); do ss -ltn | grep -q ":$PORT " && break; sleep 0.1; done
ss -ltn | grep -q ":$PORT " || { echo "FAIL: server not listening"; cat "$LOG"; exit 1; }
grep -F "[test-server]" "$LOG"
FP="$(awk '/指纹: sha256:/ {print $NF}' "$LOG" | cut -d: -f2 | tail -1)"
[ -n "$FP" ] || { echo "FAIL: fingerprint missing"; cat "$LOG"; exit 1; }
./build/backupctl remote ping --host 127.0.0.1 --port "$PORT" --server-key "sha256:$FP" >"$W/ping.log" 2>&1 || { echo "FAIL: ping"; cat "$W/ping.log"; exit 1; }
USER="accept_test_$$"
BACKUP_REMOTE_PASSWORD="acceptance-only-password" ./build/backupctl remote register --user "$USER" --host 127.0.0.1 --port "$PORT" --server-key "sha256:$FP" >"$W/register.log" 2>&1 || { echo "FAIL: register"; cat "$W/register.log"; exit 1; }
BACKUP_REMOTE_PASSWORD="acceptance-only-password" ./build/backupctl remote login --user "$USER" --host 127.0.0.1 --port "$PORT" --server-key "sha256:$FP" >"$W/login.log" 2>&1 || { echo "FAIL: login"; cat "$W/login.log"; exit 1; }
kill -INT "$PID"
wait "$PID" || true
PID=""
grep -F "[test-server]" "$LOG"

echo "[run-test-server] PASS port=$PORT fingerprint=$FP account=$USER"
