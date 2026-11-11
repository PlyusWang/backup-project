#!/usr/bin/env bash
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"
OBJ_DIR="${BP_INCREMENTAL_ENCRYPTION_BUILD_DIR:-build}"
make BUILD_DIR="$OBJ_DIR" all
TEST_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/bp-incremental-encryption.XXXXXX")"
trap 'rm -rf "$TEST_ROOT"' EXIT
mapfile -t CORE_OBJECTS < <(find "$OBJ_DIR/src" -name '*.o' | sort)
g++ -std=c++17 -Wall -Wextra -Wpedantic -Iinclude -Itests/unit \
  tests/unit/incremental_encryption_test.cpp "${CORE_OBJECTS[@]}" \
  -o "$TEST_ROOT/incremental_encryption_test"
"$TEST_ROOT/incremental_encryption_test" "$@"
