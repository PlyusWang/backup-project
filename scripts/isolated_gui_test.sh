#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ORIGINAL_HOME="${HOME:?HOME is not set}"
ORIGINAL_XDG_CONFIG_HOME="${XDG_CONFIG_HOME:-$ORIGINAL_HOME/.config}"
REAL_APP_CONFIG="$ORIGINAL_XDG_CONFIG_HOME/backup-project"
STAMP="$(date +%Y%m%d-%H%M%S)-$$"
W="$ROOT/tests/output/isolated-gui/$STAMP"
mkdir -p "$W/home" "$W/config" "$W/cache" "$W/data" "$W/state" "$W/tmp"
manifest() {
  local dir="$1" out="$2"
  if [ ! -e "$dir" ]; then printf 'ABSENT\n' > "$out"; return 0; fi
  find "$dir" -printf '%y %P %l\n' 2>/dev/null | LC_ALL=C sort > "$out"
  find "$dir" -type f -print0 2>/dev/null | LC_ALL=C sort -z | xargs -0 -r sha256sum >> "$out" 2>/dev/null || true
}
capture_after() {
  local rc=$?
  set +e
  manifest "$REAL_APP_CONFIG" "$W/real-config.after"
  if cmp -s "$W/real-config.before" "$W/real-config.after"; then
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
manifest "$REAL_APP_CONFIG" "$W/real-config.before"
export HOME="$W/home" XDG_CONFIG_HOME="$W/config" XDG_CACHE_HOME="$W/cache"
export XDG_DATA_HOME="$W/data" XDG_STATE_HOME="$W/state" TMPDIR="$W/tmp"
export QT_QPA_PLATFORM=offscreen
"$@" 2>&1 | tee "$W/command.log"
