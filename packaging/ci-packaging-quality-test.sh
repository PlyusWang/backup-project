#!/usr/bin/env bash
# packaging/ci-packaging-quality-test.sh —— 发行检查自身的"缺陷检出"测试。
#
#   bash packaging/ci-packaging-quality-test.sh <release 目录>
#
# 用**真实制品**（传进来的 release 目录）当正对照，再用它的隔离副本做负对照：
# 每一件被故意做坏的制品都必须让对应的检查**失败**。旧的检查逻辑在这套用例下会
# 假通过（解包失败仍报 0 命中 / 被简化版覆盖的声明仍算"齐全" / 漏登记的 QML 与
# 插件仍算"覆盖"），所以这组用例既是回归，也是"检查真的在检查"的证据。
#
# 只读正式制品：所有"做坏"的操作都发生在 mktemp 出来的隔离副本里。
set -Eeuo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/ci-lib.sh"

REL="${1:?用法: ci-packaging-quality-test.sh <release 目录>}"
REL="$(cd "$REL" && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

APPIMAGE="$(find "$REL" -maxdepth 1 -name '*.AppImage' -print -quit 2>/dev/null || true)"
CLIENT_DEB="$(find "$REL" -maxdepth 1 -name 'backup-project-client_*.deb' -print -quit 2>/dev/null || true)"
CLIENT_TAR="$(find "$REL" -maxdepth 1 -name 'backup-project-client-*-linux-x86_64.tar.xz' -print -quit 2>/dev/null || true)"

# ---- 0) 正对照：真品必须通过 ----
ci_section "正对照：正式制品必须通过全部检查"
expect_ok "正对照 私钥扫描通过" bash "$HERE/ci-secret-scan.sh" "$REL"
expect_ok "正对照 许可覆盖通过" bash "$HERE/ci-license-coverage.sh" "$REL"

# ---- 1) P1-01：解包失败 / 内容缺失 / 不认识的制品，都必须让扫描失败 ----
ci_section "P1-01 私钥扫描必须 fail-closed"
if [ -n "$APPIMAGE" ]; then
  base="$(basename "$APPIMAGE")"

  d="$WORK/truncated"; mkdir -p "$d"
  head -c 4096 "$APPIMAGE" > "$d/$base"
  expect_fail "P1-01 截断的 AppImage -> 扫描失败" bash "$HERE/ci-secret-scan.sh" "$d"

  d="$WORK/empty"; mkdir -p "$d"
  : > "$d/$base"
  expect_fail "P1-01 空文件冒充 AppImage -> 扫描失败" bash "$HERE/ci-secret-scan.sh" "$d"

  d="$WORK/missing-content"; mkdir -p "$d"
  # 能跑起来、但解出来的树里没有我们核对的内容：用真实 AppImage 前 1 MiB
  # 不构成完整 squashfs，解包必然失败或内容缺失。
  head -c 1048576 "$APPIMAGE" > "$d/$base"
  expect_fail "P1-01 内容不完整的 AppImage -> 扫描失败" bash "$HERE/ci-secret-scan.sh" "$d"
else
  ci_fail "P1-01 找不到 AppImage：无法验证解包 fail-closed"
fi

d="$WORK/unknown-type"; mkdir -p "$d"
printf 'not an artifact' > "$d/weird.zip"
expect_fail "P1-01 不认识的发行文件 -> 扫描失败（不许跳过）" \
  bash "$HERE/ci-secret-scan.sh" "$d"

# ---- 2) P1-02：完整第三方声明被简化版覆盖，必须被发现 ----
ci_section "P1-02 第三方声明不许被简化版覆盖"
if [ -n "$CLIENT_DEB" ]; then
  d="$WORK/deb-overwritten"; mkdir -p "$d/raw" "$d/rel"
  dpkg-deb -R "$CLIENT_DEB" "$d/raw"
  doc="$d/raw/usr/share/doc/backup-project-client"
  if [ -f "$doc/THIRD-PARTY-NOTICES.txt" ]; then
    cat > "$doc/THIRD-PARTY-NOTICES.txt" <<'EOF'
本发行包动态链接 Qt（版本 6.4.2，来自构建基线的发行版软件包，LGPL-3.0）。
Qt 以动态库形式随包提供，可以被替换（也可被移除，改用系统 Qt）。
EOF
    dpkg-deb -b "$d/raw" "$d/rel/$(basename "$CLIENT_DEB")" > /dev/null
    expect_fail "P1-02 声明被简化版覆盖 -> 覆盖检查失败" \
      bash "$HERE/ci-license-coverage.sh" "$d/rel"
  else
    ci_fail "P1-02 找不到 $doc/THIRD-PARTY-NOTICES.txt"
  fi
else
  ci_fail "P1-02 找不到客户端 .deb：无法验证声明覆盖"
fi

# ---- 3) P1-03：QML / 插件的共享对象漏登记或多登记，必须被发现 ----
ci_section "P1-03 插件与 QML 对象必须双向登记"
if [ -n "$CLIENT_TAR" ]; then
  mkdir -p "$WORK/tar-src"
  tar -xJf "$CLIENT_TAR" -C "$WORK/tar-src"
  top="$(find "$WORK/tar-src" -maxdepth 1 -mindepth 1 -type d | sed -n '1p')"

  # 3a) 删掉一条 plugins/ 或 qml/ 的登记 -> 制品里有、清单里没有
  d="$WORK/tar-missing-row"; mkdir -p "$d/src" "$d/rel"
  cp -a "$top" "$d/src/"
  t="$d/src/$(basename "$top")"
  victim="$(awk -F'\t' 'NR>1 && ($1 ~ /^plugins\// || $1 ~ /^qml\//) {print $1; exit}' \
    "$t/licenses/shipped-objects.tsv")"
  if [ -n "$victim" ]; then
    grep -vF "$victim" "$t/licenses/shipped-objects.tsv" > "$t/licenses/shipped-objects.tsv.tmp"
    mv "$t/licenses/shipped-objects.tsv.tmp" "$t/licenses/shipped-objects.tsv"
    ( cd "$d/src" && tar -cJf "$d/rel/$(basename "$CLIENT_TAR")" "$(basename "$top")" )
    expect_fail "P1-03 漏登记 $victim -> 覆盖检查失败" \
      bash "$HERE/ci-license-coverage.sh" "$d/rel"
  else
    ci_fail "P1-03 清单里没有任何 plugins/ 或 qml/ 行（正是旧版的盲区）"
  fi

  # 3b) 多登记一条制品里不存在的对象 -> 清单里有、制品里没有
  d="$WORK/tar-extra-row"; mkdir -p "$d/src" "$d/rel"
  cp -a "$top" "$d/src/"
  t="$d/src/$(basename "$top")"
  printf 'qml/Nonexistent/libnotrealplugin.so\tfile\tlibnotrealplugin.so\n' >> "$t/licenses/shipped-objects.tsv"
  ( cd "$d/src" && tar -cJf "$d/rel/$(basename "$CLIENT_TAR")" "$(basename "$top")" )
  expect_fail "P1-03 多登记不存在的对象 -> 覆盖检查失败" \
    bash "$HERE/ci-license-coverage.sh" "$d/rel"
else
  ci_fail "P1-03 找不到客户端 tar.xz：无法验证对象登记"
fi

# ---- 4) P2-01：许可材料生成器不许触发命令替换 ----
ci_section "P2-01 生成器不得触发反引号命令替换"
d="$WORK/mats"
mkdir -p "$d/lib" "$d/plugins"
if [ -e /usr/lib/x86_64-linux-gnu/libz.so.1 ]; then
  cp -a /usr/lib/x86_64-linux-gnu/libz.so.1 "$d/lib/"
else
  cp -a "$(find /usr/lib -name 'libz.so.1' -print -quit)" "$d/lib/"
fi
cp -a "$(find /usr/lib/x86_64-linux-gnu -maxdepth 1 -name 'libbz2.so.1*' -print -quit)" \
  "$d/plugins/" 2>/dev/null || cp -a "$d/lib/libz.so.1" "$d/plugins/libfakeplugin.so"
bash "$HERE/licenses/build-materials.sh" "$d" "$WORK/mats-out" "test-product" \
  > "$WORK/mats.log" 2>&1 || ci_fail "P2-01 生成器在合成输入上失败（见日志）"
expect_eq "P2-01 生成器 stderr 没有 command not found" "0" \
  "$(grep -c 'command not found' "$WORK/mats.log" || true)"
expect_contains "P2-01 声明里保留了字面量 \`.so\`" \
  "$WORK/mats-out/THIRD-PARTY-NOTICES.txt" '每个 `\.so` 一行'
expect_contains "P2-01 声明里带构建期工具小节" \
  "$WORK/mats-out/THIRD-PARTY-NOTICES.txt" '构建期工具'
expect_file "P2-01 逐个路径清单已生成" "$WORK/mats-out/licenses/shipped-objects.tsv"
expect_eq "P2-01 路径级清单行数 = 对象数 + 表头" "3" \
  "$(grep -c '' "$WORK/mats-out/licenses/shipped-objects.tsv" || true)"

ci_finish "packaging-quality"
