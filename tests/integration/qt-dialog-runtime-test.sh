#!/usr/bin/env bash
# 使用包内 Qt 实际打开、点击、确认和取消文件与目录选择器。
set -euo pipefail
TASK_SOURCE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TASK_CPP="$TASK_SOURCE/qt_dialog_runtime_probe.cpp"

build_probe() {
  local target="$1"
  local -a flags=()
  command -v pkg-config >/dev/null
  command -v "${CXX:-c++}" >/dev/null
  read -r -a flags <<<"$(pkg-config --cflags --libs \
    Qt6Quick Qt6Qml Qt6QuickControls2)"
  mkdir -p "$(dirname "$target")"
  # 防止静态元对象的 COPY relocation 破坏 Qt 跨库类型转换。
  "${CXX:-c++}" -std=c++17 -fPIC -O2 -Wall -Wextra -Werror \
    "$TASK_CPP" -o "$target" "${flags[@]}"
}

if [[ "${1:-}" == --build-only ]]; then
  [[ $# == 2 ]] || { echo "usage: $0 --build-only <probe>" >&2; exit 2; }
  build_probe "$2"
  exit 0
fi
if (( $# < 2 || $# > 3 )); then
  echo "usage: $0 <AppDir> <evidence-dir> [prebuilt-probe]" >&2
  exit 2
fi

TASK_APPDIR="$(cd "$1" && pwd -P)"
mkdir -p "$2"
TASK_OUTPUT="$(cd "$2" && pwd -P)"
for result in positive negative summary.json; do
  if [[ -e "$TASK_OUTPUT/$result" || -L "$TASK_OUTPUT/$result" ]]; then
    echo "Refusing to overwrite evidence: $TASK_OUTPUT/$result" >&2
    exit 2
  fi
done
[[ -f "$TASK_APPDIR/usr/lib/libQt6Core.so.6" ]]
[[ -d "$TASK_APPDIR/usr/qml/QtQuick/Dialogs" ]]
[[ -d "$TASK_APPDIR/usr/plugins/platforms" ]]
if [[ $# == 3 ]]; then
  TASK_PROBE="$(realpath "$3")"
  [[ -x "$TASK_PROBE" ]]
else
  TASK_PROBE="$TASK_OUTPUT/qt-dialog-runtime-probe"
  build_probe "$TASK_PROBE"
fi

mkdir -p "$TASK_OUTPUT"/{positive,negative,config,cache,data,runtime}
chmod 0700 "$TASK_OUTPUT/runtime"
ulimit -c 0
export LD_LIBRARY_PATH="$TASK_APPDIR/usr/lib"
export QT_PLUGIN_PATH="$TASK_APPDIR/usr/plugins"
export QT_QPA_PLATFORM_PLUGIN_PATH="$TASK_APPDIR/usr/plugins/platforms"
export QML_IMPORT_PATH="$TASK_APPDIR/usr/qml"
export QML2_IMPORT_PATH="$TASK_APPDIR/usr/qml"
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}"
export QT_QPA_PLATFORMTHEME=
export QT_QUICK_CONTROLS_STYLE=Basic
export QT_QUICK_BACKEND=software
export QSG_RENDER_LOOP=basic
export XDG_CONFIG_HOME="$TASK_OUTPUT/config"
export XDG_CACHE_HOME="$TASK_OUTPUT/cache"
export XDG_DATA_HOME="$TASK_OUTPUT/data"
export XDG_RUNTIME_DIR="$TASK_OUTPUT/runtime"
export NO_AT_BRIDGE=1
unset QT_FATAL_WARNINGS QML_IMPORT_TRACE QT_DEBUG_PLUGINS LD_PRELOAD
sha256sum "$TASK_CPP" "$TASK_PROBE" > "$TASK_OUTPUT/probe-sha256.txt"

# 正向运行只使用包内 QML 搜索路径，并检查进程实际加载的 Qt 库。
if timeout --signal=TERM --kill-after=3s 35s "$TASK_PROBE" \
    --appdir "$TASK_APPDIR" \
    --evidence-dir "$TASK_OUTPUT/positive" \
    > "$TASK_OUTPUT/positive/report.json" \
    2> "$TASK_OUTPUT/positive/stderr.log"; then
  :
else
  status=$?
  cat "$TASK_OUTPUT/positive/stderr.log" >&2
  cat "$TASK_OUTPUT/positive/report.json" >&2
  echo "Qt dialog positive probe failed: $status" >&2
  exit "$status"
fi

# 仅在独立搜索路径隐藏待检模块，不修改原 AppDir 的内容。
python3 - "$TASK_APPDIR/usr/qml" "$TASK_OUTPUT/negative/qml" <<'PY'
from pathlib import Path
import sys

source = Path(sys.argv[1]).resolve()
target = Path(sys.argv[2])
excluded = Path("Qt/labs/folderlistmodel")


def overlay(relative=Path()):
    destination = target / relative
    destination.mkdir()
    for child in (source / relative).iterdir():
        path = relative / child.name
        if path == excluded:
            continue
        if path in excluded.parents:
            overlay(path)
        else:
            (target / path).symlink_to(child, target_is_directory=child.is_dir())


overlay()
PY
set +e
timeout --signal=TERM --kill-after=3s 20s "$TASK_PROBE" \
  --appdir "$TASK_APPDIR" --qml-root "$TASK_OUTPUT/negative/qml" \
  > "$TASK_OUTPUT/negative/report.json" \
  2> "$TASK_OUTPUT/negative/stderr.log"
TASK_NEGATIVE_STATUS=$?
set -e
python3 - "$TASK_OUTPUT" "$TASK_NEGATIVE_STATUS" <<'PY'
import json
from pathlib import Path
import re
import sys

output = Path(sys.argv[1])
status = int(sys.argv[2])
positive = json.loads((output / "positive/report.json").read_text())
negative_path = output / "negative/report.json"
try:
    negative = json.loads(negative_path.read_text())
except (json.JSONDecodeError, OSError):
    negative = {}
stderr = (output / "negative/stderr.log").read_text(errors="replace")
message = 'module "Qt.labs.folderlistmodel" is not installed'
detected = message in stderr
blocks = re.split(r"(?m)^\[qt-dialog-probe\] ", stderr)[1:]
unrelated = [block for block in blocks if message not in block]
passed = (positive.get("passed") is True and status in (1, 139)
          and negative.get("passed") is not True and detected and not unrelated)
summary = {
    "passed": passed,
    "positive_passed": positive.get("passed"),
    "missing_module_control_exit": status,
    "missing_module_control_detected": detected,
    "negative_report_available": bool(negative),
    "negative_signal": 11 if status == 139 else None,
    "product_executed": False,
    "appdir": positive.get("appdir"),
    "platform": positive.get("platform"),
}
(output / "summary.json").write_text(
    json.dumps(summary, indent=2) + "\n", encoding="utf-8")
print(json.dumps(summary, indent=2))
if not passed:
    raise SystemExit(1)
PY
