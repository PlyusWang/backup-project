#!/usr/bin/env bash
# packaging/ci-license-coverage.sh —— 第三方许可材料的**覆盖检查**。
#
#   bash packaging/ci-license-coverage.sh <release 目录>
#
# 不是"看一眼目录在不在"，而是做映射检查：
#
#   client：制品里每个随包 .so  ->  coverage.tsv 里的一行
#                              ->  licenses/<提供包>/copyright 在制品里存在
#           并且 coverage.tsv 里的每一行都能在制品里找到对应的 .so（反向也要成立）
#           必需的许可长文本必须都在
#   server：制品里不得出现随包第三方 .so，且必须带一份说明这件事的 NOTICES
#
# 任何缺口都判 FAIL（不是 warning）。
set -Eeuo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/ci-lib.sh"

REL="${1:?用法: ci-license-coverage.sh <release 目录>}"
REL="$(cd "$REL" && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

LONG_TEXTS="LGPL-3.txt LGPL-2.1.txt GPL-3.txt CC0-1.0.txt AFL-2.1.txt GCC-Runtime-Library-Exception-3.1.txt"

unpack() {  # $1 = 制品路径, $2 = 目标目录
  local artifact="$1" dest="$2"
  mkdir -p "$dest"
  case "$artifact" in
    *.deb) dpkg-deb -x "$artifact" "$dest" ;;
    *.tar.xz) tar -xf "$artifact" -C "$dest" ;;
    *.AppImage) ( cd "$dest" && chmod 0755 "$artifact" && "$artifact" --appimage-extract >/dev/null 2>&1 ) ;;
    *) return 1 ;;
  esac
}

check_client_material() {  # $1 = 解包根, $2 = 材料根（相对）, $3 = 标签
  local root="$1" mat="$2" label="$3"
  ci_section "$label：材料是否齐全"
  expect_file "$label THIRD-PARTY-NOTICES.txt" "$root/$mat/THIRD-PARTY-NOTICES.txt"
  expect_file "$label LICENSE-INVENTORY.md" "$root/$mat/LICENSE-INVENTORY.md"
  expect_file "$label licenses/coverage.tsv" "$root/$mat/licenses/coverage.tsv"
  local t
  for t in $LONG_TEXTS; do
    expect_file "$label licenses/$t" "$root/$mat/licenses/$t"
  done

  ci_section "$label：随包 .so -> inventory -> 许可材料"
  local coverage="$root/$mat/licenses/coverage.tsv"
  local libdir
  libdir="$(dirname "$(find "$root" -name 'libQt6Core.so.6' -print -quit 2>/dev/null || true)")"
  if [ -z "$libdir" ] || [ "$libdir" = "." ]; then
    ci_fail "$label 在制品里找不到随包库目录（libQt6Core.so.6 不在）"
    return
  fi
  local total=0 mapped=0 missing_cpr=0 so pkg
  while IFS=$'\t' read -r so pkg ver cpr lic; do
    [ "$so" = "soname" ] && continue
    total=$((total + 1))
    if [ -e "$libdir/$so" ]; then
      mapped=$((mapped + 1))
    else
      ci_fail "$label coverage 里的 $so 在制品里不存在（反向映射不成立）"
    fi
    if [ ! -e "$root/$mat/licenses/$pkg/copyright" ]; then
      missing_cpr=$((missing_cpr + 1))
      ci_fail "$label $so 的提供包 $pkg 没有随包 copyright"
    fi
  done < "$coverage"
  expect_eq "$label coverage 行数与随包库数一致（正向）" "$(find "$libdir" -maxdepth 1 -name '*.so*' | wc -l)" "$total"
  expect_eq "$label coverage 全部能在制品里找到（反向）" "$total" "$mapped"
  expect_eq "$label 每个提供包都有随包 copyright" "0" "$missing_cpr"
  ci_pass "$label 覆盖检查完成：$total 个随包组件，全部映射到 inventory 与许可材料"
}

check_server_material() {  # $1 = 解包根, $2 = 标签
  local root="$1" label="$2"
  ci_section "$label：不重新分发第三方二进制"
  local notice
  notice="$(find "$root" -name 'THIRD-PARTY-NOTICES.txt' -print -quit 2>/dev/null || true)"
  if [ -z "$notice" ]; then
    ci_fail "$label 缺少 THIRD-PARTY-NOTICES.txt"
    return
  fi
  ci_pass "$label 带 THIRD-PARTY-NOTICES.txt"
  expect_contains "$label 明确声明不重新分发第三方二进制" "$notice" '不重新分发任何第三方二进制'
  local extra_so
  extra_so="$( { find "$root" -name '*.so*' 2>/dev/null || true; } | wc -l)"
  expect_eq "$label 制品里没有随包第三方 .so" "0" "$extra_so"
}

for artifact in "$REL"/*.AppImage; do
  [ -e "$artifact" ] || continue
  unpack "$artifact" "$WORK/appimage"
  check_client_material "$WORK/appimage/squashfs-root" "usr/share/doc/backup-project-client" "AppImage"
done

for artifact in "$REL"/backup-project-client_*.deb; do
  [ -e "$artifact" ] || continue
  unpack "$artifact" "$WORK/clientdeb"
  check_client_material "$WORK/clientdeb" "usr/share/doc/backup-project-client" "client deb"
done

for artifact in "$REL"/backup-project-client-*-linux-x86_64.tar.xz; do
  [ -e "$artifact" ] || continue
  unpack "$artifact" "$WORK/clienttar"
  local_top="$(find "$WORK/clienttar" -maxdepth 1 -mindepth 1 -type d | sed -n '1p')"
  check_client_material "$local_top" "." "client tar.xz"
done

for artifact in "$REL"/backup-project-server_*.deb; do
  [ -e "$artifact" ] || continue
  unpack "$artifact" "$WORK/serverdeb"
  check_server_material "$WORK/serverdeb" "server deb"
done

for artifact in "$REL"/backup-project-server-*-linux-x86_64.tar.xz; do
  [ -e "$artifact" ] || continue
  unpack "$artifact" "$WORK/servertar"
  top="$(find "$WORK/servertar" -maxdepth 1 -mindepth 1 -type d | sed -n '1p')"
  check_server_material "$top" "server tar.xz"
done

ci_finish "license-coverage"
