#!/usr/bin/env bash
# 本地进度、加密增量、恢复和身份导入的真实控制器专项。
set -euo pipefail
TASK_REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TASK_OUTPUT="$(mktemp -d "$TASK_REPO/tests/output/workflow-acceptance-XXXXXX")"
mkdir -p "$TASK_OUTPUT/config" "$TASK_OUTPUT/cache" "$TASK_OUTPUT/data" "$TASK_OUTPUT/tmp"
export XDG_CONFIG_HOME="$TASK_OUTPUT/config"
export XDG_CACHE_HOME="$TASK_OUTPUT/cache"
export XDG_DATA_HOME="$TASK_OUTPUT/data"
export TMPDIR="$TASK_OUTPUT/tmp"
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}"
"$TASK_REPO/build/backup-gui-modern" --workflow-acceptance "$TASK_OUTPUT/screenshots"
