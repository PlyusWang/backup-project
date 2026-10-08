#!/usr/bin/env bash
# packaging/licenses/build-materials.sh —— 在**构建容器里**生成随包的第三方许可材料。
#
#   bash packaging/licenses/build-materials.sh <随包对象根> <输出目录> [产品名]
#
# 为什么在构建容器里生成：随包的 .so 是从这台机器的发行版包里拷出来的，只有这台
# 机器的 dpkg 数据库能**逐个文件**回答"它是哪个包提供的、什么版本、它的版权文件
# 在哪"。判定全部来自包自带的机器可读 copyright（DEP-5）的 License: 字段 ——
# 不按库名猜许可，也不复制任何实现代码。
#
# 清点范围（对象根之下的**全部**共享对象，不只 lib/ 顶层）：旧版本只数
# "<库目录>/*.so*"，于是随包的 Qt 平台插件（plugins/）与 QML 插件（qml/）从来没
# 进过清单 —— 那些同样是随包分发的第三方二进制。现在按"对象根"递归清点：
#   AppImage : <appdir>/usr     （usr/lib、usr/plugins、usr/qml）
#   tar.xz   : <top>            （lib、plugins、qml）
#   .deb     : usr/lib/backup-project-client
# 软链接与真实文件分开记录：同一个共享库的 soname 软链接只算**一个组件**，
# 但它出现的每个路径都写进 shipped-objects.tsv，双向都能核对。
#
# 产出（<输出目录> 下）：
#   THIRD-PARTY-NOTICES.txt    给最终用户看的声明（模板 + 生成的分节 + 构建期工具）
#   LICENSE-INVENTORY.md       逐组件：soname / 提供包 / 版本 / copyright 路径 / 许可 / 上游 / 要求
#   licenses/<提供包>/copyright 每个提供包自己的版权与许可声明
#   licenses/*.txt             必需的长文本（LGPL-2.1 / LGPL-3 / GPL-3 / CC0-1.0 / AFL-2.1 / GCC 例外）
#   licenses/coverage.tsv      逐个**组件**的映射表（给覆盖检查用）
#   licenses/shipped-objects.tsv  逐个**路径**的清单：relative_path / kind / component
set -Eeuo pipefail

OBJECT_ROOT="${1:?用法: build-materials.sh <随包对象根> <输出目录> [产品名]}"
DEST="${2:?缺少输出目录}"
PRODUCT="${3:-backup-project-client}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

[ -d "$OBJECT_ROOT" ] || { echo "找不到随包对象根：$OBJECT_ROOT" >&2; exit 1; }
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
SHIPPED="$DEST/licenses/shipped-objects.tsv"
printf 'soname\tprovider_package\tprovider_version\tcopyright_in_package\tlicense_field\n' > "$COVERAGE"
printf 'relative_path\tkind\tcomponent\n' > "$SHIPPED"

# ---- 1) 清点对象根之下的全部共享对象 -------------------------------------
mapfile -t OBJECT_RELS < <(cd "$OBJECT_ROOT" && find . -name '*.so*' -printf '%P\n' | LC_ALL=C sort)
if [ "${#OBJECT_RELS[@]}" -eq 0 ]; then
  # 0 个对象几乎一定是"材料生成得太早"（AppDir 还没被 linuxdeploy 填满），
  # 与其产出一份空材料，不如当场失败。
  echo "对象根 $OBJECT_ROOT 下没有任何 *.so*：材料生成得太早或路径传错了" >&2
  exit 1
fi

declare -A COMPONENT_SEEN=()
for rel in "${OBJECT_RELS[@]}"; do
  full="$OBJECT_ROOT/$rel"
  if [ -L "$full" ]; then
    kind="symlink"
    resolved="$(readlink -f "$full" 2>/dev/null || true)"
    case "$resolved" in
      "$OBJECT_ROOT"/*) comp="$(basename "$resolved")" ;;
      *) comp="$(basename "$full")" ;;
    esac
  else
    kind="file"
    comp="$(basename "$full")"
  fi
  printf '%s\t%s\t%s\n' "$rel" "$kind" "$comp" >> "$SHIPPED"
  COMPONENT_SEEN["$comp"]=1
done
mapfile -t SONAMES < <(printf '%s\n' "${!COMPONENT_SEEN[@]}" | LC_ALL=C sort)

{
  echo "# LICENSE-INVENTORY —— 随 ${PRODUCT} 分发的第三方共享库"
  echo
  echo "生成方式：在**构建容器内部**逐个文件查 dpkg 数据库（\`dpkg -S\`），再读提供包"
  echo "自带的机器可读 copyright（DEP-5）里的 \`License:\` 字段。不按库名猜许可。"
  echo
  echo "对象根：\`$OBJECT_ROOT\`；清点到的共享对象 **${#OBJECT_RELS[@]} 个路径 / ${#SONAMES[@]} 个组件**"
  echo "（含 lib/、plugins/、qml/ 等目录；同一个库的 soname 软链接只计一个组件，"
  echo "逐个路径的对照见 licenses/shipped-objects.tsv）。"
  echo
  echo "| soname | 提供包 | 版本 | 许可（来自该包 copyright） | 上游 | 再分发要求 | 随包材料 |"
  echo "|---|---|---|---|---|---|---|"
} > "$DEST/LICENSE-INVENTORY.md"

for so in "${SONAMES[@]}"; do
  path="$(find /usr/lib /lib -maxdepth 3 -name "$so" -print -quit 2>/dev/null || true)"
  if [ -z "$path" ]; then
    # Qt 的平台插件与 QML 模块落在 /usr/lib/<triplet>/qt6/qml/... 这类深路径里，
    # maxdepth 3 根本看不见它们。找不到就放宽一次；判定仍然只以 dpkg 数据库为准。
    path="$(find /usr/lib /lib -name "$so" -print -quit 2>/dev/null || true)"
  fi
  pkg=""; ver=""; lic=""; cpr=""
  if [ -n "$path" ]; then
    # dpkg 的文件清单用的是 /usr/lib/... 这种规范路径，而 find 可能先从 /lib
    # （merged-usr 的符号链接）命中，直接查会查不到（第一次就是这样漏了 4 个库：
    # libcap / libdbus-1 / libkeyutils / liblzma）。三种写法依次试，最后一个用
    # 通配模式匹配 basename，跨 usrmerge 也能落到正确的包。
    pkg="$( { dpkg -S "$path" 2>/dev/null || true; } | sed -n 's/:.*//p' | sed -n '1p')"
    if [ -z "$pkg" ]; then
      pkg="$( { dpkg -S "$(readlink -f "$path")" 2>/dev/null || true; } | sed -n 's/:.*//p' | sed -n '1p')"
    fi
  fi
  # 无论上面的 find 有没有命中，都再问一次 dpkg 数据库（按 basename 通配）：
  # 深路径对象、merged-usr 的两种写法都在这里兜住。
  if [ -z "$pkg" ]; then
    pkg="$( { dpkg -S "*/$so" 2>/dev/null || true; } | sed -n 's/:.*//p' | sed -n '1p')"
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

# 路径 → 组件对照（人看的部分；机器核对用 shipped-objects.tsv）
{
  echo
  echo "## 随包对象路径 → 组件"
  echo
  echo "| 相对路径 | 类型 | 组件 |"
  echo "|---|---|---|"
  awk -F'\t' 'NR>1 { printf "| %s | %s | %s |\n", $1, $2, $3 }' "$SHIPPED"
} >> "$DEST/LICENSE-INVENTORY.md"

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

# 给最终用户的声明：模板 + 自动生成的分节（组件 -> 材料）+ 构建期工具说明。
# 反引号在这里必须转义：整个块是双引号字符串，未转义的 \`\` 会被 shell 当成命令
# 替换执行（v0.1.0 的构建日志里真的出现过 ".so: command not found"，而且生成的
# 文件里那句话因此少了个词）。
{
  cat "$HEADER"
  echo
  echo "----------------------------------------------------------------------"
  echo "随包组件清单（由构建容器自动生成；每个 \`.so\` 一行）"
  echo
  awk -F'|' 'NR>0 && /^\| lib/ {gsub(/^ +| +$/,"",$2); printf "  %s\n", $2 }' "$DEST/LICENSE-INVENTORY.md" | sort -u
  echo
  echo "----------------------------------------------------------------------"
  echo "构建期工具（**不随包分发**）"
  echo
  echo "linuxdeploy / linuxdeploy-plugin-qt / appimagetool 只在构建容器里使用，"
  echo "不会进入任何发行制品；它们的版本、来源 URL、sha256 与许可证见仓库里的"
  echo "packaging/tools.lock。随包的 Qt 以动态库形式提供，用户可以替换（也可以"
  echo "移除它、改用系统 Qt）——这一条对全部客户端制品都成立。"
} > "$DEST/THIRD-PARTY-NOTICES.txt"

echo "[licenses] 随包共享对象：${#OBJECT_RELS[@]} 个路径 / ${#SONAMES[@]} 个组件"
echo "[licenses] 材料写到 $DEST"
echo "[licenses]   $(find "$DEST/licenses" -type f | wc -l) 个许可/版权文件"
echo "[licenses]   coverage.tsv 行数：$(grep -c '' "$COVERAGE")"
