#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cd "$ROOT_DIR"

echo "[test] Running current smoke test..."

if [[ ! -x build/backupctl ]]; then
    echo "[test] backupctl is missing. Building first..."
    make
fi

./build/backupctl --help

echo "[test] Smoke test passed."
echo "[test] Real Backup / Restore tests will be implemented in Sprint 1."