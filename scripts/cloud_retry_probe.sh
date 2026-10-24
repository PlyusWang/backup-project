#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STAMP="$(date +%Y%m%d-%H%M%S)-$$"
W="$ROOT/tests/output/cloud-retry-isolated/$STAMP"
mkdir -p "$W/home" "$W/config" "$W/cache" "$W/data" "$W/state" "$W/tmp"
export HOME="$W/home"
export XDG_CONFIG_HOME="$W/config"
export XDG_CACHE_HOME="$W/cache"
export XDG_DATA_HOME="$W/data"
export XDG_STATE_HOME="$W/state"
export TMPDIR="$W/tmp"
export QT_QPA_PLATFORM=offscreen
REAL_CONFIG="$HOME/.config"
# 记录真实用户配置（若存在）用于事后人工对比；断言只依赖本次隔离目录。
find "$REAL_CONFIG" -type f 2>/dev/null | sort > "$W/real-config-files.before" || true
"$ROOT/build/backup-gui-modern" --cloud-retry-test | tee "$W/cloud-retry.log"
find "$REAL_CONFIG" -type f 2>/dev/null | sort > "$W/real-config-files.after" || true
if ! cmp -s "$W/real-config-files.before" "$W/real-config-files.after"; then
  echo "FAIL: real user config changed" >&2
  exit 1
fi
echo "[isolation] HOME=$HOME"
echo "[isolation] XDG_CONFIG_HOME=$XDG_CONFIG_HOME"
echo "[isolation] TMPDIR=$TMPDIR"
echo "[isolation] ledger files: $(find "$XDG_CONFIG_HOME" -name '*.ledger' | wc -l)"
