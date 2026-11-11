#!/usr/bin/env bash
# packaging/ci-elf-report.sh —— 量出每个最终 ELF 的真实运行期契约。
#
# 只看 ldd --version 猜兼容性是错的：这里逐个二进制读 ELF 头、
# 读 .gnu.version_r 里真正引用到的 GLIBC_*/GLIBCXX_*/CXXABI_* 版本，
# 再用 ldd 确认没有未解析的库。输出的就是 COMPATIBILITY.md 里的数字。
set -uo pipefail
export LC_ALL=C

work="$(mktemp -d)" || exit 1
trap 'rm -rf "$work"' EXIT
status=0
[ "$#" -gt 0 ] || exit 1
for binary in "$@"; do
  if [ ! -f "$binary" ]; then
    echo "== $binary: MISSING"
    status=1
    continue
  fi
  printf '== %s\n' "$binary"
  if ! bytes="$(stat -c %s "$binary")"; then status=1; continue; fi
  printf '   bytes=%s\n' "$bytes"
  # readelf 和 ldd 的失败必须保留；“没有 not found”不代表检查已完成。
  if ! elf="$(readelf -W -h -l -d --version-info "$binary" 2>"$work/readelf.err")" || [ -s "$work/readelf.err" ]; then
    cat "$work/readelf.err" >&2
    printf '   elf_error=metadata-inspection-failed\n'
    status=1
    continue
  fi
  if ! printf '%s\n' "$elf" | awk '
      /Class:/{class=$2} /Machine:/{machine=$2} /Type:/{type=$2}
      /^[[:space:]]*LOAD[[:space:]]/{load=1}
      END{exit !(class ~ /^ELF(32|64)$/ && machine != "" && (type == "EXEC" || type == "DYN") && load)}'; then
    printf '   elf_error=invalid-loadable-header\n'
    status=1
    continue
  fi
  printf '%s\n' "$elf" | awk '/Class:/{c=$2} /Machine:/{m=$2} /Type:/{t=$2} END{printf "   elf=%s %s %s\n", c, m, t}'
  printf '   needed=%s\n' "$(printf '%s\n' "$elf" | awk '/\(NEEDED\)/{gsub(/[][]/,"",$5); printf "%s ", $5}')"
  printf '   rpath=%s\n' "$(printf '%s\n' "$elf" | awk '/\(RPATH\)|\(RUNPATH\)/{sub(/^.*\[/, ""); sub(/\].*$/, ""); printf "%s", $0}')"
  printf '   glibc_max=%s\n' "$(printf '%s\n' "$elf" | awk 'match($0,/GLIBC_[0-9.]+/){print substr($0,RSTART,RLENGTH)}' | sort -Vu | tail -1)"
  printf '   glibcxx_max=%s\n' "$(printf '%s\n' "$elf" | awk 'match($0,/GLIBCXX_[0-9.]+/){print substr($0,RSTART,RLENGTH)}' | sort -Vu | tail -1)"
  printf '   cxxabi_max=%s\n' "$(printf '%s\n' "$elf" | awk 'match($0,/CXXABI_[0-9.]+/){print substr($0,RSTART,RLENGTH)}' | sort -Vu | tail -1)"
  if printf '%s\n' "$elf" | awk '/^[[:space:]]*INTERP[[:space:]]|\(NEEDED\)/{dynamic=1} END{exit !dynamic}'; then
    if ! deps="$(ldd "$binary" 2>"$work/ldd.err")" || [ -s "$work/ldd.err" ] || [ -z "$deps" ]; then
      cat "$work/ldd.err" >&2
      printf '   ldd_error=dependency-inspection-failed\n'
      status=1
      continue
    fi
    printf '%s\n' "$deps" | awk '/not found/{n++} END{printf "   ldd_missing=%d\n", n}'
    if printf '%s\n' "$deps" | awk '/not found|not a dynamic executable|statically linked/{bad=1} END{exit !bad}'; then
      status=1
    fi
    printf '   dependency_class=dynamic-ldd-checked\n'
  else
    # 合法静态 ELF 无需用 ldd 的“非动态可执行文件”诊断判错。
    printf '   ldd_missing=0\n   dependency_class=no-interpreter-or-needed\n'
  fi
  if ! printable="$(strings -a "$binary")"; then status=1; continue; fi
  dev_paths="$(printf '%s\n' "$printable" | awk '/\/home\/pw-is-123|\/tmp\/stage-|\/workspace\/backup-project\/build/{n++} END{print n+0}')"
  printf '   dev_path_hits=%s\n' "$dev_paths"
  [ "$dev_paths" = 0 ] || status=1
done
exit "$status"
