#!/usr/bin/env bash
# packaging/ci-lib.sh —— 打包验收脚本共用的断言小工具。
# 输出是稳定的 "  PASS/FAIL  <标签>" 行，便于在 CI 日志里逐条核对。
set -uo pipefail

CI_FAILED=0
CI_PASSED=0

ci_section() { printf '\n== %s ==\n' "$*"; }
ci_pass() { CI_PASSED=$((CI_PASSED + 1)); printf '  PASS  %s\n' "$*"; }
ci_fail() { CI_FAILED=$((CI_FAILED + 1)); printf '  FAIL  %s\n' "$*" >&2; }

expect_ok() {   # expect_ok <标签> <命令...>
  local label="$1"; shift
  if "$@" > /tmp/ci-lib.out 2>&1; then ci_pass "$label"; else
    ci_fail "$label"; sed 's/^/        /' /tmp/ci-lib.out | tail -8 >&2; fi
}

expect_fail() { # expect_fail <标签> <命令...>
  local label="$1"; shift
  if "$@" > /tmp/ci-lib.out 2>&1; then
    ci_fail "$label（本应失败却成功了）"; sed 's/^/        /' /tmp/ci-lib.out | tail -8 >&2
  else ci_pass "$label"; fi
}

expect_contains() { # expect_contains <标签> <文件> <正则>
  local label="$1" file="$2" pattern="$3"
  if grep -qE "$pattern" "$file" 2>/dev/null; then ci_pass "$label"; else
    ci_fail "$label（$file 里没有匹配 $pattern 的内容）"; tail -5 "$file" >&2; fi
}

expect_eq() { # expect_eq <标签> <期望> <实际>
  if [ "$2" = "$3" ]; then ci_pass "$1"; else ci_fail "$1（期望 '$2'，实际 '$3'）"; fi
}

expect_file() { # expect_file <标签> <路径>
  if [ -e "$2" ]; then ci_pass "$1"; else ci_fail "$1（缺少 $2）"; fi
}

expect_mode() { # expect_mode <标签> <路径> <期望权限>
  local mode; mode="$(stat -c '%a' "$2" 2>/dev/null || echo missing)"
  if [ "$mode" = "$3" ]; then ci_pass "$1"; else ci_fail "$1（$2 权限 $mode，期望 $3）"; fi
}

ci_finish() { # ci_finish <套件名>
  printf '\n[%s] passed=%d failed=%d\n' "$1" "$CI_PASSED" "$CI_FAILED"
  [ "$CI_FAILED" -eq 0 ]
}
