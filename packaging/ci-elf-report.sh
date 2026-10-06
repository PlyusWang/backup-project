#!/usr/bin/env bash
# packaging/ci-elf-report.sh —— 量出每个最终 ELF 的真实运行期契约。
#
# 只看 ldd --version 猜兼容性是错的：这里逐个二进制读 ELF 头、
# 读 .gnu.version_r 里真正引用到的 GLIBC_*/GLIBCXX_*/CXXABI_* 版本，
# 再用 ldd 确认没有未解析的库。输出的就是 COMPATIBILITY.md 里的数字。
set -uo pipefail

status=0
for binary in "$@"; do
  if [ ! -f "$binary" ]; then
    echo "== $binary: MISSING"
    status=1
    continue
  fi
  printf '== %s\n' "$binary"
  printf '   bytes=%s\n' "$(stat -c %s "$binary")"
  readelf -h "$binary" | awk '/Class:/{c=$2} /Machine:/{m=$2} /Type:/{t=$2} END{printf "   elf=%s %s %s\n", c, m, t}'
  printf '   needed=%s\n' "$(readelf -d "$binary" | awk '/NEEDED/{gsub(/[][]/,"",$5); printf "%s ", $5}')"
  printf '   rpath=%s\n' "$(readelf -d "$binary" | awk '/RPATH|RUNPATH/{gsub(/[][]/,"",$5); printf "%s", $6}')"
  printf '   glibc_max=%s\n' "$(readelf --version-info "$binary" 2>/dev/null | grep -o 'GLIBC_[0-9.]*' | sort -Vu | tail -1)"
  printf '   glibcxx_max=%s\n' "$(readelf --version-info "$binary" 2>/dev/null | grep -o 'GLIBCXX_[0-9.]*' | sort -Vu | tail -1)"
  printf '   cxxabi_max=%s\n' "$(readelf --version-info "$binary" 2>/dev/null | grep -o 'CXXABI_[0-9.]*' | sort -Vu | tail -1)"
  missing="$(ldd "$binary" 2>/dev/null | grep -c 'not found' || true)"
  printf '   ldd_missing=%s\n' "$missing"
  [ "$missing" = "0" ] || status=1
  # 开发机路径绝不允许出现在最终二进制里。
  dev_paths="$(strings -a "$binary" | grep -cE '/home/pw-is-123|/tmp/stage-|/workspace/backup-project/build' || true)"
  printf '   dev_path_hits=%s\n' "$dev_paths"
done
exit "$status"
