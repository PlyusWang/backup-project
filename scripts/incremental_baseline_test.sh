#!/usr/bin/env bash
# A03 baseline discovery, explicit branching and asynchronous GUI controller.
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"
OBJ_DIR="${BP_BASELINE_BUILD_DIR:-build}"
make BUILD_DIR="$OBJ_DIR" CXXFLAGS="${BP_BASELINE_CXXFLAGS:--std=c++17 -Wall -Wextra -Wpedantic}" all gui-modern
TEST_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/bp-baseline-test.XXXXXX")"
trap 'rm -rf "$TEST_ROOT"' EXIT
mapfile -t CORE_OBJECTS < <(find "$OBJ_DIR/src" -name '*.o' | sort)
g++ -std=c++17 -Wall -Wextra -Wpedantic -Iinclude -Itests/unit \
  tests/unit/incremental_baseline_test.cpp "${CORE_OBJECTS[@]}" \
  -o "$TEST_ROOT/incremental_baseline_test"
"$TEST_ROOT/incremental_baseline_test"
mapfile -t QT_FLAGS < <(pkg-config --cflags --libs Qt6Core Qt6Concurrent | xargs -n1)
g++ -std=c++17 -Wall -Wextra -Wpedantic -fPIC -Iinclude -Iui/modern \
  -Itests/unit tests/unit/baseline_controller_test.cpp \
  "$OBJ_DIR"/ui/modern/backup_controller.o "$OBJ_DIR"/ui/modern/moc_backup_controller.o \
  "$OBJ_DIR"/ui/modern/operation_gate.o "$OBJ_DIR"/ui/modern/user_error.o \
  "${CORE_OBJECTS[@]}" "${QT_FLAGS[@]}" -o "$TEST_ROOT/baseline_controller_test"
"$TEST_ROOT/baseline_controller_test"
