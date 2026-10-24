#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ORIGINAL_HOME="${HOME:?HOME is not set}"
ORIGINAL_XDG_CONFIG_HOME="${XDG_CONFIG_HOME:-$ORIGINAL_HOME/.config}"
REAL_APP_CONFIG="$ORIGINAL_XDG_CONFIG_HOME/backup-project"
STAMP="$(date +%Y%m%d-%H%M%S)-$$"
W="$ROOT/tests/output/cloud-retry-isolated/$STAMP"
mkdir -p "$W/home" "$W/config" "$W/cache" "$W/data" "$W/state" "$W/tmp"
manifest() {
  local dir="$1" out="$2"
  if [ ! -e "$dir" ]; then
    printf 'ABSENT\n' > "$out"
    return 0
  fi
  find "$dir" -printf '%y %P %l\n' 2>/dev/null | LC_ALL=C sort > "$out"
  find "$dir" -type f -print0 2>/dev/null | LC_ALL=C sort -z | \
    xargs -0 -r sha256sum >> "$out" 2>/dev/null || true
}
capture_after() {
  local rc=$?
  set +e
  manifest "$REAL_APP_CONFIG" "$W/real-config.after"
  if cmp -s "$W/real-config.before" "$W/real-config.after"; then
    echo "[isolation] REAL_CONFIG_UNCHANGED=1"
  else
    echo "[isolation] REAL_CONFIG_UNCHANGED=0"
    diff -u "$W/real-config.before" "$W/real-config.after" | head -20 >&2
    rc=1
  fi
  echo "[isolation] HOME=$HOME"
  echo "[isolation] XDG_CONFIG_HOME=$XDG_CONFIG_HOME"
  echo "[isolation] TMPDIR=$TMPDIR"
  echo "[isolation] ledger_files=$(find "$XDG_CONFIG_HOME" -name '*.ledger' 2>/dev/null | wc -l)"
  if grep -q "QtAppConfig=$XDG_CONFIG_HOME/" "$W/cloud-retry.log" 2>/dev/null; then
    echo "[isolation] QT_APP_CONFIG_ISOLATED=1"
  else
    echo "[isolation] QT_APP_CONFIG_ISOLATED=0"
    rc=1
  fi
  exit "$rc"
}
trap capture_after EXIT
manifest "$REAL_APP_CONFIG" "$W/real-config.before"
export HOME="$W/home"
export XDG_CONFIG_HOME="$W/config"
export XDG_CACHE_HOME="$W/cache"
export XDG_DATA_HOME="$W/data"
export XDG_STATE_HOME="$W/state"
export TMPDIR="$W/tmp"
export QT_QPA_PLATFORM=offscreen
"$ROOT/build/backup-gui-modern" --cloud-retry-test 2>&1 | tee "$W/cloud-retry.log"
