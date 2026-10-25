#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$ROOT/scripts/config_manifest.sh"
ORIGINAL_HOME="${HOME:?HOME is not set}"
ORIGINAL_XDG_CONFIG_HOME="${XDG_CONFIG_HOME:-$ORIGINAL_HOME/.config}"
REAL_APP_CONFIG="$ORIGINAL_XDG_CONFIG_HOME/backup-project"
STAMP="$(date +%Y%m%d-%H%M%S)-$$"
W="$ROOT/tests/output/cloud-retry-isolated/$STAMP"
mkdir -p "$W/home" "$W/config" "$W/cache" "$W/data" "$W/state" "$W/tmp"
set +e
manifest_dir "$REAL_APP_CONFIG" "$W/real-config.before"
before_rc=$?
set -e
if [ "$before_rc" -eq 1 ]; then
  echo "[isolation] REAL_CONFIG_MANIFEST_BEFORE=ERROR" >&2
  exit 1
fi
capture_after() {
  local rc=$?
  set +e
  manifest_dir "$REAL_APP_CONFIG" "$W/real-config.after"
  local after_rc=$?
  if [ "$after_rc" -eq 1 ]; then
    echo "[isolation] REAL_CONFIG_UNCHANGED=0"
    echo "[isolation] REAL_CONFIG_MANIFEST_AFTER=ERROR"
    rc=1
  elif cmp -s "$W/real-config.before" "$W/real-config.after"; then
    echo "[isolation] REAL_CONFIG_UNCHANGED=1"
  else
    echo "[isolation] REAL_CONFIG_UNCHANGED=0"
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
export HOME="$W/home" XDG_CONFIG_HOME="$W/config" XDG_CACHE_HOME="$W/cache"
export XDG_DATA_HOME="$W/data" XDG_STATE_HOME="$W/state" TMPDIR="$W/tmp"
export QT_QPA_PLATFORM=offscreen
"$ROOT/build/backup-gui-modern" --cloud-retry-test 2>&1 | tee "$W/cloud-retry.log"
