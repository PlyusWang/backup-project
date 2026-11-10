#!/usr/bin/env bash
# Validate portable desktop integration with isolated mock executables.
set -Eeuo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$ROOT/tests/output/client-desktop-integration}"
command -v desktop-file-validate >/dev/null || { echo "需要 desktop-file-utils" >&2; exit 1; }
python3 "$ROOT/tests/integration/test_client_desktop_integration.py" --repo-root "$ROOT" --out "$OUT"
