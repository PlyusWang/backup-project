#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUT_DIR="$ROOT_DIR/tests/output"
BIN="$OUT_DIR/config_manager_test"

mkdir -p "$OUT_DIR"
echo "[config-manager] compiling unit test"
g++ -std=c++17 -Wall -Wextra -Wpedantic -g -I"$ROOT_DIR/include" \
  "$ROOT_DIR/src/config/config_manager.cpp" \
  "$ROOT_DIR/tests/unit/config_manager_test.cpp" -o "$BIN"
echo "[config-manager] running unit test"
"$BIN"
