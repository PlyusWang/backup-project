#!/usr/bin/env bash
#
# FormatByteSize 的边界测试入口（展示层格式只有一份实现）。
#
# 被测的是 src/core/format_bytes.cpp：它被 CLI、服务端管理工具与 Modern GUI 的
# 四个页面共用，所以"整数省略 .0、非整数一位小数、不进成 1024 单位"这几条必须在
# 一处钉死 —— 否则同一个归档又会在不同页面显示成不同面貌。
#
# 编译整份 src/** 里的 core 部分：被测的是真实产品代码，不做链接替身。
# 退出码：0 = 全绿。编译错误、编译警告、断言失败都非零退出。
# 末行固定打印 "format-bytes: N/M checks passed"。

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

CXX="${CXX:-g++}"
EXTRA_FLAGS="${FORMAT_BYTES_TEST_EXTRA_FLAGS:-}"

WORK_DIR="$(mktemp -d /tmp/format-bytes-XXXXXX)"
trap 'rm -rf "$WORK_DIR"' EXIT

BIN="$WORK_DIR/format_bytes_test"
BUILD_LOG="$WORK_DIR/build.log"
RUN_LOG="$WORK_DIR/run.log"

echo "[format-bytes] 编译（$WORK_DIR）"
if ! "$CXX" -Iinclude -Itests/unit -std=c++17 -Wall -Wextra -Wpedantic -O1 \
    $EXTRA_FLAGS src/core/format_bytes.cpp tests/unit/format_bytes_test.cpp \
    -o "$BIN" > "$BUILD_LOG" 2>&1; then
    echo "[format-bytes] 编译失败：" >&2
    cat "$BUILD_LOG" >&2
    echo "format-bytes: 0/0 checks passed"
    exit 1
fi
if grep -q "warning:" "$BUILD_LOG"; then
    echo "[format-bytes] 编译出现警告，按规定视为失败：" >&2
    cat "$BUILD_LOG" >&2
    echo "format-bytes: 0/0 checks passed"
    exit 1
fi

"$BIN" > "$RUN_LOG" 2>&1
RUN_CODE=$?
cat "$RUN_LOG"

SUMMARY="$(grep -E '^format-bytes: [0-9]+/[0-9]+ checks passed$' "$RUN_LOG" | tail -n 1)"
if [ -z "$SUMMARY" ]; then
    echo "[format-bytes] 缺少汇总行" >&2
    echo "format-bytes: 0/0 checks passed"
    exit 1
fi
NUMBERS="${SUMMARY#format-bytes: }"
PASSED="${NUMBERS%%/*}"
TOTAL="${NUMBERS#*/}"
TOTAL="${TOTAL%% *}"
if [ "$RUN_CODE" -ne 0 ] || [ "$PASSED" != "$TOTAL" ]; then
    echo "[format-bytes] 用例失败：$SUMMARY" >&2
    exit 1
fi
echo "[format-bytes] 全部通过（$SUMMARY）"
exit 0
