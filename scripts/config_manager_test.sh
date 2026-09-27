#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUT_DIR="$ROOT_DIR/tests/output"
BIN="$OUT_DIR/config_manager_test"

mkdir -p "$OUT_DIR"
echo "[config-manager] compiling unit test"
# ConfigManager 的原子保存走的是共享的 file_io 原语（与 ScheduleStore 同一个
# 函数），所以这个测试现在也必须链接它——少了它会在链接期报未定义引用，而不是
# 悄悄退化成另一套临时文件写法。
g++ -std=c++17 -Wall -Wextra -Wpedantic -g -I"$ROOT_DIR/include" \
  "$ROOT_DIR/src/config/config_manager.cpp" \
  "$ROOT_DIR/src/core/file_io.cpp" \
  "$ROOT_DIR/tests/unit/config_manager_test.cpp" -o "$BIN"
echo "[config-manager] running unit test"
"$BIN"
