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
  # -- 是必须的：pattern 可能以 - 开头（例如 --reinstall），否则 grep 会把它当成选项。
  if grep -qE -- "$pattern" "$file" 2>/dev/null; then ci_pass "$label"; else
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

# ---- 制品解包（fail-closed）------------------------------------------------
#
# 为什么必须"解包 + 校验内容"而不是"先 mkdir，再检查目录在不在"：预建目录会让
# **解包失败**看起来像成功，于是后面的私钥扫描会去扫一个空目录并报告 0 命中 ——
# 那是一条假通过。这里的三条规则：
#   * 目标目录由本函数创建，调用方不许先建；
#   * 解包命令非零 = 失败；
#   * 必须逐个核对"解出来真的有什么"（支持 glob: 前缀的通配）。
ci_unpack() {  # ci_unpack <标签> <制品> <目标目录> <期望相对路径...>
  local label="$1" artifact="$2" dest="$3"
  shift 3
  local rel pattern missing=0
  rm -rf "$dest"
  mkdir -p "$dest"
  case "$artifact" in
    *.deb)
      dpkg-deb -x "$artifact" "$dest" || { ci_fail "$label 解包失败（dpkg-deb -x）"; return 1; } ;;
    *.tar.xz)
      tar -xJf "$artifact" -C "$dest" || { ci_fail "$label 解包失败（tar -xJf）"; return 1; } ;;
    *.AppImage)
      # 不改动发行目录里的制品：拿一份临时副本解包，解完删掉。
      cp -f "$artifact" "$dest/.extract.AppImage" || { ci_fail "$label 无法复制制品"; return 1; }
      chmod 0755 "$dest/.extract.AppImage"
      if ! ( cd "$dest" && ./.extract.AppImage --appimage-extract ) > /dev/null 2>&1; then
        rm -f "$dest/.extract.AppImage"
        ci_fail "$label 解包失败（--appimage-extract）"
        return 1
      fi
      rm -f "$dest/.extract.AppImage" ;;
    *)
      ci_fail "$label 不认识的发行文件类型（不允许跳过扫描）：$(basename "$artifact")"
      return 1 ;;
  esac
  for rel in "$@"; do
    case "$rel" in
      glob:*)
        pattern="${rel#glob:}"
        if ! compgen -G "$dest/$pattern" > /dev/null 2>&1; then
          ci_fail "$label 解包结果缺少匹配 $pattern 的文件"; missing=1
        fi ;;
      *)
        if [ ! -e "$dest/$rel" ]; then
          ci_fail "$label 解包结果缺少 $rel"; missing=1
        fi ;;
    esac
  done
  [ "$missing" -eq 0 ] || return 1
  ci_pass "$label 解包成功且内容已核对（$# 项）"
  return 0
}

ci_finish() { # ci_finish <套件名>
  printf '\n[%s] passed=%d failed=%d\n' "$1" "$CI_PASSED" "$CI_FAILED"
  [ "$CI_FAILED" -eq 0 ]
}
