#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cd "$ROOT_DIR"

if ! command -v clang-format >/dev/null 2>&1; then
    echo "clang-format is not installed."
    exit 1
fi

mapfile -d '' FILES < <(
    find app src include \
        -type f \
        \( -name "*.cpp" -o -name "*.cc" -o -name "*.h" \) \
        -print0
)

if [[ ${#FILES[@]} -eq 0 ]]; then
    echo "No C++ source files found."
    exit 0
fi

clang-format --dry-run --Werror "${FILES[@]}"

echo "clang-format check passed."