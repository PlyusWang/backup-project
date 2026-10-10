#!/usr/bin/env bash
#
# 本脚本对**明确列出的** Shell 脚本跑 ShellCheck（静态检查门禁）。
#
#   bash scripts/shellcheck.sh
#
# 为什么要有它：PR #34 新增的可靠性脚本带了 4 条 ShellCheck 告警，而完整门禁
# 一条都没报——不是告警检测失败，是门禁**从来没有**这个工具。这个脚本把它接进
# 长期验收流程。
#
# 设计取舍（刻意的）：
#   * **工具不在就失败**，绝不静默跳过：跳过等于把"没检查"记成 PASS。
#   * 版本与严重级别都打印出来，日志里能看清是什么在把关。
#   * 只检查**显式清单**里的脚本。仓库里还有大量历史脚本带告警（例如
#     scripts/test.sh 有 9 条），一次性全量清理属于与本轮无关的大重构，也会
#     让这个门禁一上线就是红的。清单可以按需增补，增补时把该脚本的告警修干净。
#   * 不允许用 shellcheck disable 消音：清单内脚本必须真的干净。
#
# 测试用覆盖：设置 SHELLCHECK_FILES 可以只检查指定文件（用于负向自检），
# 例如  SHELLCHECK_FILES=/tmp/broken.sh bash scripts/shellcheck.sh
#
# 退出码：0 = 清单内全部干净；非 0 = 有告警，或依赖缺失。

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)" || exit 1
cd "$ROOT_DIR" || exit 1

if ! command -v shellcheck >/dev/null 2>&1; then
  echo "shellcheck is NOT installed - this check cannot be skipped." >&2
  echo "install it (apt-get install shellcheck) or run it in the CI image." >&2
  echo "the gate must not report PASS for a check that never ran." >&2
  exit 1
fi

SEVERITY="${SHELLCHECK_SEVERITY:-warning}"
VERSION="$(shellcheck --version | awk '/^version:/ {print $2}')"

if [ -n "${SHELLCHECK_FILES:-}" ]; then
  # shellcheck disable=SC2206
  FILES=(${SHELLCHECK_FILES})
  echo "[shellcheck] override file list from SHELLCHECK_FILES"
else
  FILES=(
    scripts/final_gate.sh
    scripts/remote_reliability_test.sh
    scripts/shellcheck.sh
  )
fi

echo "[shellcheck] version=${VERSION} severity=${SEVERITY} files=${#FILES[@]}"

status=0
for f in "${FILES[@]}"; do
  if [ ! -f "$f" ]; then
    echo "  FAIL  $f (listed but missing)" >&2
    status=1
    continue
  fi
  output="$(shellcheck -S "$SEVERITY" -f gcc "$f" 2>&1)"
  code=$?
  if [ "$code" -eq 0 ]; then
    echo "  PASS  $f"
  else
    echo "  FAIL  $f (shellcheck exit=$code)" >&2
    printf '%s\n' "$output" >&2
    status=1
  fi
done

if [ "$status" -ne 0 ]; then
  echo "[shellcheck] 有告警未清理（severity=${SEVERITY}）" >&2
  exit 1
fi
echo "[shellcheck] 清单内脚本全部干净（severity=${SEVERITY}, version=${VERSION}）"
exit 0
