#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cd "$ROOT_DIR"

if ! command -v clang-format >/dev/null 2>&1; then
    echo "clang-format is not installed."
    exit 1
fi

# 手写 C++ 源码所在目录。ui/desktop 是 Qt 桌面 GUI：它必须走同一个 lint 入口，
# 否则 GUI 文件就算格式不合规，./scripts/lint.sh 也照样报 PASS。
# ui/desktop 不存在时跳过，保证没有 GUI 的环境下 lint 依然可用。
LINT_DIRS="app src include"
if [[ -d ui/desktop ]]; then
    LINT_DIRS="$LINT_DIRS ui/desktop"
fi
# 现代 QML GUI 的 C++ 部分（moc 桥 + 主题）同样走这个入口；
# QML 本身由 scripts/modern_gui_check.sh 用 qmllint 与冒烟测试检查。
if [[ -d ui/modern ]]; then
    LINT_DIRS="$LINT_DIRS ui/modern"
fi

mapfile -d '' FILES < <(
    # $LINT_DIRS 有意不加引号：它就是一组空格分隔的目录名。
    find $LINT_DIRS \
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