#!/usr/bin/env bash
# packaging/licenses/build-materials.sh —— 在**构建容器里**生成随包的第三方许可材料。
#
#   bash packaging/licenses/build-materials.sh <随包 lib 目录> <输出目录> [产品名]
#
# 为什么在构建容器里生成：随包的 .so 是从这台机器的发行版包里拷出来的，只有这台
# 机器的 dpkg 数据库能**逐个文件**回答"它是哪个包提供的、什么版本、它的版权文件
# 在哪"。判定全部来自包自带的机器可读 copyright（DEP-5）的 License: 字段 ——
# 不按库名猜许可，也不复制任何实现代码。
#
# 产出（<输出目录> 下）：
#   THIRD-PARTY-NOTICES.txt    给最终用户看的声明（模板 + 生成的分节）
#   LICENSE-INVENTORY.md       逐文件：soname / 提供包 / 版本 / copyright 路径 / 许可 / 上游 / 要求
#   licenses/<提供包>/copyright 每个提供包自己的版权与许可声明
#   licenses/*.txt             必需的长文本（LGPL-2.1 / LGPL-3 / GPL-3 / CC0-1.0 / AFL-2.1 / GCC 例外）
#   licenses/coverage.tsv      给自动覆盖检查用的映射表：soname -> 提供包 -> 材料路径
#
# 只处理**真正随包**的 .so：调用方传进来的目录里有什么，就只记录什么。
set -Eeuo pipefail

LIB_DIR="${1:?用法: build-materials.sh <随包 lib 目录> <输出目录> [产品名]}"
DEST="${2:?缺少输出目录}"
PRODUCT="${3:-backup-project-client}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

[ -d "$LIB_DIR" ] || { echo "找不到随包库目录：$LIB_DIR" >&2; exit 1; }
mkdir -p "$DEST/licenses"

TSV="$HERE/COMPONENT-LICENSES.tsv"
HEADER="$HERE/NOTICES-HEADER.txt"
[ -f "$TSV" ] || { echo "缺少 $TSV" >&2; exit 1; }
[ -f "$HEADER" ] || { echo "缺少 $HEADER" >&2; exit 1; }

# 上游 / 许可摘要 / 再分发要求：按 soname 前缀查表（跨发行版稳定）
lookup() {  # $1 = soname, $2 = 字段号(2..4)
  local so="$1" field="$2" line
  line="$(awk -F'|' -v s="$so" '!/^#/ && NF>=4 { if (index(s, $1) == 1) print; }' "$TSV" | sort -t'|' -k1,1 | awk 'length($1)>=m {m=length($1); best=$0} END{print best}' )"
  [ -n "$line" ] || { printf '%s' "（未在对照表中登记）"; return; }
  printf '%s' "$(printf '%s' "$line" | cut -d'|' -f"$field")"
}

COVERAGE="$DEST/licenses/coverage.tsv"
printf 'soname\tprovider_package\tprovider_version\tcopyright_in_package\tlicense_field\n' > "$COVERAGE"

mapfile -t SONAMES < <(find "$LIB_DIR" -maxdepth 1 -name '*.so*' -printf '%f\n' | LC_ALL=C sort -u)
{
  echo "# LICENSE-INVENTORY —— 随 ${PRODUCT} 分发的第三方共享库"
  echo
  echo "生成方式：在**构建容器内部**逐个文件查 dpkg 数据库（\`dpkg -S\`），再读提供包"
  echo "自带的机器可读 copyright（DEP-5）里的 \`License:\` 字段。不按库名猜许可。"
  echo
  echo "随包库目录：\`$LIB_DIR\`（共 ${#SONAMES[@]} 个 \`.so\`）。"
  echo
  echo "| soname | 提供包 | 版本 | 许可（来自该包 copyright） | 上游 | 再分发要求 | 随包材料 |"
  echo "|---|---|---|---|---|---|---|"
} > "$DEST/LICENSE-INVENTORY.md"

for so in "${SONAMES[@]}"; do
  path="$(find /usr/lib /lib -maxdepth 3 -name "$so" -print -quit 2>/dev/null || true)"
  pkg=""; ver=""; lic=""; cpr=""
  if [ -n "$path" ]; then
    pkg="$( { dpkg -S "$path" 2>/dev/null || true; } | sed -n 's/:.*//p' | sed -n '1p')"
  fi
  if [ -n "$pkg" ]; then
    ver="$(dpkg-query -W -f='${Version}' "$pkg" 2>/dev/null || echo '?')"
    src="$(dpkg-query -W -f='${source:Package}' "$pkg" 2>/dev/null || true)"
    [ -n "$src" ] || src="$pkg"
    for cand in "/usr/share/doc/$src/copyright" "/usr/share/doc/$pkg/copyright"; do
      [ -f "$cand" ] && { cpr="$cand"; break; }
    done
    if [ -n "$cpr" ]; then
      lic="$(grep -m1 '^License:' "$cpr" | sed 's/^License:[[:space:]]*//' || true)"
      [ -n "$lic" ] || lic="（该包的 copyright 不是 DEP-5：按文件整体声明处理）"
      mkdir -p "$DEST/licenses/$pkg"
      cp -f "$cpr" "$DEST/licenses/$pkg/copyright"
    else
      lic="（找不到该包的 copyright 文件）"
    fi
  else
    pkg="NOT_PROVIDED_BY_A_PACKAGE"; ver="-"; lic="（不来自发行版包，需要人工核对）"
  fi
  upstream="$(lookup "$so" 2)"
  summary="$(lookup "$so" 3)"
  requirement="$(lookup "$so" 4)"
  printf '| %s | %s | %s | %s | %s | %s | licenses/%s/ |\n' \
    "$so" "$pkg" "$ver" "$lic" "$upstream" "$requirement" "$pkg" >> "$DEST/LICENSE-INVENTORY.md"
  printf '%s\t%s\t%s\t%s\t%s\n' "$so" "$pkg" "$ver" "$cpr" "$lic" >> "$COVERAGE"
done

# 必需的长文本：从这台机器的 common-licenses 取（权威副本），例外文本从对应包取
copy_common() { [ -f "/usr/share/common-licenses/$1" ] && cp -f "/usr/share/common-licenses/$1" "$DEST/licenses/$1.txt" || true; }
copy_common LGPL-3
copy_common LGPL-2.1
copy_common GPL-3
copy_common CC0-1.0
# AFL-2.1：在提供 libdbus 的那个包的 copyright 里（发行版不同，路径也不同）
AFL_SRC=""
for cand in /usr/share/doc/dbus/copyright /usr/share/doc/libdbus-1-3/copyright; do
  [ -f "$cand" ] && { AFL_SRC="$cand"; break; }
done
if [ -n "$AFL_SRC" ]; then
  t="$(grep -n 'Academic Free License' "$AFL_SRC" | sed -n '1p' | cut -d: -f1 || true)"
  [ -n "$t" ] && sed -n "${t},$((t+95))p" "$AFL_SRC" > "$DEST/licenses/AFL-2.1.txt" || true
fi
# GCC Runtime Library Exception v3.1：gcc 的 copyright 里有全文（包名随版本变）
GCC_CPR=""
for cand in /usr/share/doc/gcc-*/copyright /usr/share/doc/gcc-*-base/copyright; do
  # shellcheck disable=SC2086
  set -- $cand
  [ -f "$1" ] && { GCC_CPR="$1"; break; }
done
if [ -n "$GCC_CPR" ]; then
  t="$(grep -n 'GCC RUNTIME LIBRARY EXCEPTION' "$GCC_CPR" | sed -n '1p' | cut -d: -f1 || true)"
  [ -n "$t" ] && sed -n "${t},$((t+30))p" "$GCC_CPR" > "$DEST/licenses/GCC-Runtime-Library-Exception-3.1.txt" || true
fi
# 生成了哪些长文本，明确打出来（缺哪个，覆盖检查会硬失败）
for t in LGPL-3 LGPL-2.1 GPL-3 CC0-1.0 AFL-2.1 GCC-Runtime-Library-Exception-3.1; do
  if [ -s "$DEST/licenses/$t.txt" ]; then
    echo "[licenses]   $t.txt  $(wc -c < "$DEST/licenses/$t.txt") 字节"
  else
    echo "[licenses]   WARN 缺少 $t.txt（覆盖检查会失败）" >&2
  fi
done

# 给最终用户的声明：模板 + 自动生成的分节（组件 -> 材料）
{
  cat "$HEADER"
  echo
  echo "----------------------------------------------------------------------"
  echo "随包组件清单（由构建容器自动生成；每个 `.so` 一行）"
  echo
  awk -F'|' 'NR>0 && /^\| lib/ {gsub(/^ +| +$/,"",$2); printf "  %s\n", $2 }' "$DEST/LICENSE-INVENTORY.md" | sort -u
} > "$DEST/THIRD-PARTY-NOTICES.txt"

echo "[licenses] 随包 .so：${#SONAMES[@]} 个；材料写到 $DEST"
echo "[licenses]   $(find "$DEST/licenses" -type f | wc -l) 个许可/版权文件"
echo "[licenses]   coverage.tsv 行数：$(grep -c '' "$COVERAGE")"
