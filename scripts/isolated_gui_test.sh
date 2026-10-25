#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$ROOT/scripts/config_manifest.sh"
ORIGINAL_HOME="${HOME:?HOME is not set}"
ORIGINAL_XDG_CONFIG_HOME="${XDG_CONFIG_HOME:-$ORIGINAL_HOME/.config}"
REAL_APP_CONFIG="$ORIGINAL_XDG_CONFIG_HOME/backup-project"
STAMP="$(date +%Y%m%d-%H%M%S)-$$"
W="$ROOT/tests/output/isolated-gui/$STAMP"
mkdir -p "$W/home" "$W/config" "$W/cache" "$W/data" "$W/state" "$W/tmp"
set +e
manifest_dir "$REAL_APP_CONFIG" "$W/real-config.before"
before_rc=$?
set -e
if [ "$before_rc" -eq 1 ]; then
  echo "[isolated-gui] REAL_CONFIG_MANIFEST_BEFORE=ERROR" >&2
  exit 1
fi
capture_after() {
  local rc=$?
  set +e
  manifest_dir "$REAL_APP_CONFIG" "$W/real-config.after"
  local after_rc=$?
  if [ "$after_rc" -eq 1 ]; then
    echo "[isolated-gui] REAL_CONFIG_UNCHANGED=0"
    rc=1
  elif cmp -s "$W/real-config.before" "$W/real-config.after"; then
    echo "[isolated-gui] REAL_CONFIG_UNCHANGED=1"
  else
    echo "[isolated-gui] REAL_CONFIG_UNCHANGED=0"
    rc=1
  fi
  echo "[isolated-gui] HOME=$HOME"
  echo "[isolated-gui] XDG_CONFIG_HOME=$XDG_CONFIG_HOME"
  echo "[isolated-gui] TMPDIR=$TMPDIR"
  exit "$rc"
}
trap capture_after EXIT
export HOME="$W/home" XDG_CONFIG_HOME="$W/config" XDG_CACHE_HOME="$W/cache"
export XDG_DATA_HOME="$W/data" XDG_STATE_HOME="$W/state" TMPDIR="$W/tmp"
export QT_QPA_PLATFORM=offscreen
"$@" 2>&1 | tee "$W/command.log"
