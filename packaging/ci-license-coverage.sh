#!/usr/bin/env bash
# packaging/ci-license-coverage.sh —— 第三方许可材料的**覆盖检查**。
#
#   bash packaging/ci-license-coverage.sh <release 目录>
#
# 不是"看一眼目录在不在"，而是做映射检查（client）：
#
#   制品里的**每个**随包共享对象（lib/ + plugins/ + qml/ …，按路径）
#     -> licenses/shipped-objects.tsv 的一行
#     -> licenses/coverage.tsv 的一个组件（同一库的 soname 软链接不重复计）
#     -> licenses/<提供包>/copyright 在制品里存在
#   反向同样成立：清单里写的路径与组件都必须真的在制品里。
#   必需的许可长文本必须都在。
#   THIRD-PARTY-NOTICES.txt 必须是**完整声明**：含"构建期工具"小节，并且逐个列出
#   全部组件 —— 防的是"完整声明被一份简短说明覆盖"（v0.1.0 的 .deb 就是这样）。
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
# 查不到提供包的组件白名单（仓库文件；空表 = 一个都不许放行）
ALLOWLIST="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/licenses/UNMAPPED-COMPONENTS.txt"

# 同一个目录里可能有多个制品：这里逐个解包并逐个检查，绝不复用上一件的解包结果。
check_client_material() {  # $1 = 解包根, $2 = 材料根（相对）, $3 = 标签
  local root="$1" mat="$2" label="$3"
  local matdir="$root/$mat"
  ci_section "$label：材料是否齐全"
  expect_file "$label THIRD-PARTY-NOTICES.txt" "$matdir/THIRD-PARTY-NOTICES.txt"
  expect_file "$label LICENSE-INVENTORY.md" "$matdir/LICENSE-INVENTORY.md"
  expect_file "$label licenses/coverage.tsv" "$matdir/licenses/coverage.tsv"
  expect_file "$label licenses/shipped-objects.tsv" \
    "$matdir/licenses/shipped-objects.tsv"
  local t
  for t in $LONG_TEXTS; do
    expect_file "$label licenses/$t" "$matdir/licenses/$t"
  done

  ci_section "$label：随包共享对象 -> inventory -> 许可材料"
  local libdir objectroot
  libdir="$(dirname "$(find "$root" -name 'libQt6Core.so.6' -print -quit 2>/dev/null || true)")"
  if [ ! -d "$libdir" ]; then
    ci_fail "$label 在制品里找不到随包库目录（libQt6Core.so.6 不在）"
    return
  fi
  objectroot="$(dirname "$libdir")"

  # ---- 1) 路径级双向核对：制品 <-> shipped-objects.tsv ----
  local -a actual declared
  mapfile -t actual < <(cd "$objectroot" && find . -name '*.so*' -printf '%P\n' | LC_ALL=C sort)
  mapfile -t declared < <(awk -F'\t' 'NR>1 && $1!="" {print $1}' \
    "$matdir/licenses/shipped-objects.tsv" | LC_ALL=C sort)
  expect_eq "$label 制品里的共享对象数（路径级，含 plugins/ 与 qml/）" \
    "${#actual[@]}" "${#declared[@]}"
  local diff_out
  diff_out="$(diff <(printf '%s\n' "${actual[@]:-}") \
                  <(printf '%s\n' "${declared[@]:-}") 2>&1 || true)"
  if [ -n "$diff_out" ]; then
    ci_fail "$label 制品与 shipped-objects.tsv 不一致（< 只在制品里，> 只在清单里）"
    printf '%s\n' "$diff_out" | sed -n '1,8p' | sed 's/^/        /' >&2
  else
    ci_pass "$label 路径级双向一致（${#actual[@]} 个对象）"
  fi

  # ---- 2) 组件级双向核对：shipped-objects.tsv <-> coverage.tsv ----
  local -a comps rows
  mapfile -t comps < <(awk -F'\t' 'NR>1 && $3!="" {print $3}' \
    "$matdir/licenses/shipped-objects.tsv" | LC_ALL=C sort -u)
  mapfile -t rows < <(awk -F'\t' 'NR>1 && $1!="" {print $1}' \
    "$matdir/licenses/coverage.tsv" | LC_ALL=C sort -u)
  expect_eq "$label 组件数（去重后）与 coverage 行数一致" \
    "${#comps[@]}" "${#rows[@]}"
  diff_out="$(diff <(printf '%s\n' "${comps[@]:-}") \
                  <(printf '%s\n' "${rows[@]:-}") 2>&1 || true)"
  if [ -n "$diff_out" ]; then
    ci_fail "$label 组件集合与 coverage.tsv 不一致"
    printf '%s\n' "$diff_out" | sed -n '1,8p' | sed 's/^/        /' >&2
  else
    ci_pass "$label 组件级双向一致（${#comps[@]} 个组件）"
  fi

  # ---- 3) 每个组件的许可材料必须真的在制品里 ----
  local so pkg ver cpr lic missing_cpr=0 unmapped=0
  while IFS=$'\t' read -r so pkg ver cpr lic; do
    [ "$so" = "soname" ] && continue
    [ -n "$so" ] || continue
    if [ "$pkg" = "NOT_PROVIDED_BY_A_PACKAGE" ]; then
      if ! grep -qE "^${so}([[:space:]]|$)" "$ALLOWLIST" 2>/dev/null; then
        unmapped=$((unmapped + 1))
        ci_fail "$label $so 查不到提供包，且没有登记在 UNMAPPED-COMPONENTS.txt"
      fi
      continue
    fi
    if [ ! -e "$matdir/licenses/$pkg/copyright" ]; then
      missing_cpr=$((missing_cpr + 1))
      ci_fail "$label $so 的提供包 $pkg 没有随包 copyright"
    fi
  done < "$matdir/licenses/coverage.tsv"
  expect_eq "$label 每个提供包都有随包 copyright" "0" "$missing_cpr"
  expect_eq "$label 没有未登记的组件" "0" "$unmapped"

  # ---- 4) 完整声明检查（防"被简化版覆盖"）----
  local notices="$matdir/THIRD-PARTY-NOTICES.txt"
  expect_contains "$label 声明含随包组件清单小节" "$notices" '随包组件清单'
  expect_contains "$label 声明含构建期工具小节" "$notices" '构建期工具'
  local not_listed=0
  for so in "${comps[@]}"; do
    grep -qF -- "$so" "$notices" || not_listed=$((not_listed + 1))
  done
  expect_eq "$label 声明逐个列出了全部组件" "0" "$not_listed"

  ci_pass "$label 覆盖检查完成：${#actual[@]} 个对象 / ${#comps[@]} 个组件，全部映射到 inventory 与许可材料"
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

# 解包一律走 ci_unpack（解包失败即失败，且核对解出来的真实内容）。
for artifact in "$REL"/*.AppImage; do
  [ -e "$artifact" ] || continue
  if ci_unpack "AppImage 解包" "$artifact" "$WORK/appimage" \
      "squashfs-root/usr/bin/backupctl" "squashfs-root/usr/bin/backup-gui-modern"; then
    check_client_material "$WORK/appimage/squashfs-root" \
      "usr/share/doc/backup-project-client" "AppImage"
  fi
done

for artifact in "$REL"/backup-project-client_*.deb; do
  [ -e "$artifact" ] || continue
  if ci_unpack "client deb 解包" "$artifact" "$WORK/clientdeb" "usr"; then
    check_client_material "$WORK/clientdeb" \
      "usr/share/doc/backup-project-client" "client deb"
  fi
done

for artifact in "$REL"/backup-project-client-*-linux-x86_64.tar.xz; do
  [ -e "$artifact" ] || continue
  if ci_unpack "client tar 解包" "$artifact" "$WORK/clienttar" \
      "glob:*/bin/backupctl"; then
    local_top="$(find "$WORK/clienttar" -maxdepth 1 -mindepth 1 -type d | sed -n '1p')"
    check_client_material "$local_top" "." "client tar.xz"
  fi
done

for artifact in "$REL"/backup-project-server_*.deb; do
  [ -e "$artifact" ] || continue
  if ci_unpack "server deb 解包" "$artifact" "$WORK/serverdeb" "usr"; then
    check_server_material "$WORK/serverdeb" "server deb"
  fi
done

for artifact in "$REL"/backup-project-server-*-linux-x86_64.tar.xz; do
  [ -e "$artifact" ] || continue
  if ci_unpack "server tar 解包" "$artifact" "$WORK/servertar" \
      "glob:*/bin/backup-server"; then
    top="$(find "$WORK/servertar" -maxdepth 1 -mindepth 1 -type d | sed -n '1p')"
    check_server_material "$top" "server tar.xz"
  fi
done

ci_finish "license-coverage"
