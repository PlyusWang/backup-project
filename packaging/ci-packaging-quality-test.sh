#!/usr/bin/env bash
# packaging/ci-packaging-quality-test.sh —— 发行检查自身的"缺陷检出"测试。
#
#   bash packaging/ci-packaging-quality-test.sh <release 目录>
#
# 用**真实制品**（传进来的 release 目录）当正对照，再用它的隔离副本做负对照：
# 每一件被故意做坏的制品/清单都必须让对应的检查**失败**。旧的检查逻辑在这些用例
# 下会假通过（解包失败仍报 0 命中 / 被简化版覆盖的声明仍算齐全 / 漏登记的插件仍算
# 覆盖 / 缺 Qt 核心库时退化成扫当前目录 / 缺制品时循环直接跳过），所以这组用例
# 既是回归，也是"检查真的在检查"的证据。
#
# 只读正式制品：所有"做坏"的操作都发生在 mktemp 出来的隔离副本里；注入的内容全部
# 是合成数据（假的 PEM 文本、假的十六进制 token、纯 A 的 32 字节文件），不含任何
# 真实密钥。
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
VERSION="$(grep -m1 '^version' "$REL/RELEASE-INFO.txt" 2>/dev/null | awk '{print $3}')"
[ -n "$VERSION" ] || VERSION="0.0.0"

# ---- 夹具助手 -------------------------------------------------------------
# 复制正式发行目录（只带指定后缀的文件），可再执行一段修正片段。
make_rel_dir() {  # make_rel_dir <目标目录> [--only-client]
  local dest="$1" only="${2:-}"
  rm -rf "$dest"; mkdir -p "$dest"
  if [ "$only" = "--only-client" ]; then
    cp -a "$REL"/*client*.deb "$REL"/*client*.tar.xz "$REL"/*.AppImage "$dest/" 2>/dev/null || true
  else
    cp -a "$REL"/*.deb "$REL"/*.tar.xz "$REL"/*.AppImage "$dest/" 2>/dev/null || true
  fi
  [ -f "$REL/RELEASE-INFO.txt" ] && cp -a "$REL/RELEASE-INFO.txt" "$dest/"
  ( cd "$dest" && rm -f SHA256SUMS && { find . -maxdepth 1 -type f ! -name SHA256SUMS -printf '%P\n' | LC_ALL=C sort | xargs -r sha256sum > SHA256SUMS; } )
}

# 重打包客户端 tar，附带一段在解包树根目录执行的片段（注入合成数据）。
make_client_tar_variant() {  # make_client_tar_variant <输出 tar> <片段>
  local out="$1" snippet="$2" d top
  d="$(mktemp -d "$WORK/tv.XXXXXX")"
  tar -xJf "$CLIENT_TAR" -C "$d"
  top="$(find "$d" -maxdepth 1 -mindepth 1 -type d | sed -n '1p')"
  ( cd "$top" && eval "$snippet" )
  ( cd "$d" && tar -cJf "$out" "$(basename "$top")" )
  rm -rf "$d"
}

# 重打包客户端 deb，附带一段在解包树根目录执行的片段。
make_client_deb_variant() {  # make_client_deb_variant <输出 deb> <片段>
  local out="$1" snippet="$2" d
  d="$(mktemp -d "$WORK/dv.XXXXXX")"
  dpkg-deb -R "$CLIENT_DEB" "$d"
  ( cd "$d" && eval "$snippet" )
  dpkg-deb -b "$d" "$out" > /dev/null
  rm -rf "$d"
}

# 只看退出码不够：还要确认失败原因里出现了该规则的标志，证明打中的是目标分支。
expect_scan_reason() {  # expect_scan_reason <标签> <脚本> <目录> <正则>
  local label="$1" script="$2" dir="$3" pattern="$4" out
  if out="$(bash "$script" "$dir" 2>&1)"; then
    ci_fail "$label（本应失败却成功了）"
    return
  fi
  if printf '%s' "$out" | grep -qE -- "$pattern"; then
    ci_pass "$label 命中正确规则"
  else
    ci_fail "$label 失败原因里没有该规则的标志（$pattern）"
    printf '%s\n' "$out" | tail -5 | sed 's/^/        /' >&2
  fi
}

# 两套安全检查入口必须给出一致结论。
expect_both_scans_fail() {  # expect_both_scans_fail <标签> <目录>
  local label="$1" dir="$2"
  expect_fail "$label（构建期自查）" bash "$HERE/ci-artifact-selfscan.sh" "$dir"
  expect_fail "$label（CI 扫描）" bash "$HERE/ci-secret-scan.sh" "$dir"
}

# ---- 0) 正对照：真品必须通过全部检查 ----
ci_section "正对照：正式制品必须通过全部检查"
expect_ok "正对照 私钥扫描通过" bash "$HERE/ci-secret-scan.sh" "$REL"
expect_ok "正对照 许可覆盖通过" bash "$HERE/ci-license-coverage.sh" "$REL"
expect_ok "正对照 制品自查通过（客户端 + 服务端）" \
  bash "$HERE/ci-artifact-selfscan.sh" "$REL"
expect_ok "正对照 完整清单通过（family=all）" \
  bash "$HERE/ci-release-manifest.sh" "$REL" --family all
expect_ok "家族模式：只要求客户端（family=client）" \
  bash "$HERE/ci-release-manifest.sh" "$REL" --family client
expect_ok "家族模式：只要求服务端（family=server）" \
  bash "$HERE/ci-release-manifest.sh" "$REL" --family server

# ---- 1) P1-01：解包 fail-closed ----
ci_section "P1-01 解包与扫描必须 fail-closed"
if [ -n "$APPIMAGE" ]; then
  base="$(basename "$APPIMAGE")"
  d="$WORK/truncated"; mkdir -p "$d"; head -c 4096 "$APPIMAGE" > "$d/$base"
  expect_both_scans_fail "P1-01 截断的 AppImage" "$d"

  d="$WORK/empty"; mkdir -p "$d"; : > "$d/$base"
  expect_both_scans_fail "P1-01 空文件冒充 AppImage" "$d"
else
  ci_fail "P1-01 找不到 AppImage：无法验证解包 fail-closed"
fi
d="$WORK/unknown"; mkdir -p "$d"; printf 'not an artifact' > "$d/weird.zip"
expect_both_scans_fail "P1-01 不认识的发行文件" "$d"

# ---- 2) P1-02：完整声明被简化版覆盖 ----
ci_section "P1-02 第三方声明不许被简化版覆盖"
if [ -n "$CLIENT_DEB" ]; then
  d="$WORK/deb-overwritten"; mkdir -p "$d/raw" "$d/rel"
  dpkg-deb -R "$CLIENT_DEB" "$d/raw"
  doc="$d/raw/usr/share/doc/backup-project-client"
  if [ -f "$doc/THIRD-PARTY-NOTICES.txt" ]; then
    printf '%s\n' '本发行包动态链接 Qt（LGPL-3.0）。' > "$doc/THIRD-PARTY-NOTICES.txt"
    dpkg-deb -b "$d/raw" "$d/rel/$(basename "$CLIENT_DEB")" > /dev/null
    expect_fail "P1-02 声明被简化版覆盖" \
      bash "$HERE/ci-license-coverage.sh" "$d/rel"
  else
    ci_fail "P1-02 找不到 deb 里的 THIRD-PARTY-NOTICES.txt"
  fi
fi

# ---- 3) P1-03：QML / 插件对象必须双向登记 ----
ci_section "P1-03 插件与 QML 对象必须双向登记"
if [ -n "$CLIENT_TAR" ]; then
  mkdir -p "$WORK/tar-src"
  tar -xJf "$CLIENT_TAR" -C "$WORK/tar-src"
  top="$(find "$WORK/tar-src" -maxdepth 1 -mindepth 1 -type d | sed -n '1p')"
  d="$WORK/tar-missing-row"; mkdir -p "$d/src" "$d/rel"
  cp -a "$top" "$d/src/"; t="$d/src/$(basename "$top")"
  victim="$(awk -F'\t' 'NR>1 && ($1 ~ /^plugins\// || $1 ~ /^qml\//) {print $1; exit}' "$t/licenses/shipped-objects.tsv")"
  if [ -n "$victim" ]; then
    grep -vF "$victim" "$t/licenses/shipped-objects.tsv" > "$t/licenses/shipped-objects.tsv.tmp"
    mv "$t/licenses/shipped-objects.tsv.tmp" "$t/licenses/shipped-objects.tsv"
    ( cd "$d/src" && tar -cJf "$d/rel/$(basename "$CLIENT_TAR")" "$(basename "$top")" )
    expect_fail "P1-03 漏登记 $victim" bash "$HERE/ci-license-coverage.sh" "$d/rel"
  else
    ci_fail "P1-03 清单里没有任何 plugins/ 或 qml/ 行"
  fi
  d="$WORK/tar-extra-row"; mkdir -p "$d/src" "$d/rel"
  cp -a "$top" "$d/src/"; t="$d/src/$(basename "$top")"
  printf 'qml/Nonexistent/libnotrealplugin.so\tfile\tlibnotrealplugin.so\n' >> "$t/licenses/shipped-objects.tsv"
  ( cd "$d/src" && tar -cJf "$d/rel/$(basename "$CLIENT_TAR")" "$(basename "$top")" )
  expect_fail "P1-03 多登记不存在的对象" bash "$HERE/ci-license-coverage.sh" "$d/rel"
fi

# ---- 4) A：缺 Qt 核心库 / 断链必须明确失败 ----
ci_section "A 随包核心库定位（不许退化成当前目录）"
if [ -n "$CLIENT_DEB" ]; then
  d="$WORK/deb-no-qtcore"; mkdir -p "$d/rel"
  make_client_deb_variant "$d/rel/$(basename "$CLIENT_DEB")" \
    'rm -f usr/lib/backup-project-client/lib/libQt6Core.so.6'
  out="$(bash "$HERE/ci-license-coverage.sh" "$d/rel" 2>&1 || true)"
  if printf '%s' "$out" | grep -q "找不到随包核心库 libQt6Core.so.6"; then
    ci_pass "A 缺 libQt6Core.so.6 时命中正确的失败分支"
  else
    ci_fail "A 缺 libQt6Core.so.6 时没有给出预期原因"
    printf '%s\n' "$out" | tail -5 | sed 's/^/        /' >&2
  fi
  expect_fail "A 缺 libQt6Core.so.6 -> 覆盖检查失败" \
    bash "$HERE/ci-license-coverage.sh" "$d/rel"

  d="$WORK/deb-qtcore-dangling"; mkdir -p "$d/rel"
  make_client_deb_variant "$d/rel/$(basename "$CLIENT_DEB")" \
    'rm -f usr/lib/backup-project-client/lib/libQt6Core.so.6 && ln -s /nonexistent/libQt6Core.so.6 usr/lib/backup-project-client/lib/libQt6Core.so.6'
  out="$(bash "$HERE/ci-license-coverage.sh" "$d/rel" 2>&1 || true)"
  if printf '%s' "$out" | grep -q "断链"; then
    ci_pass "A libQt6Core.so.6 是断链时命中正确的失败分支"
  else
    ci_fail "A 断链时没有给出预期原因"
    printf '%s\n' "$out" | tail -5 | sed 's/^/        /' >&2
  fi
  expect_fail "A libQt6Core.so.6 断链 -> 覆盖检查失败" \
    bash "$HERE/ci-license-coverage.sh" "$d/rel"
else
  ci_fail "A 找不到客户端 .deb：无法验证核心库定位"
fi

# ---- 5) B：两套扫描入口的规则一致性矩阵 ----
ci_section "B 构建期自查与 CI 扫描必须给出一致结论"
if [ -n "$CLIENT_TAR" ]; then
  mkdir -p "$WORK/b"
  make_client_tar_variant "$WORK/b/with-key.tar.xz" \
    'mkdir -p extra && printf "%s\n" synthetic > extra/test.key'
  d="$WORK/b-key"; mkdir -p "$d"; cp -a "$WORK/b/with-key.tar.xz" "$d/$(basename "$CLIENT_TAR")"
  expect_both_scans_fail "B 含测试用 .key" "$d"
  expect_scan_reason "B .key 构建期自查" "$HERE/ci-artifact-selfscan.sh" "$d" "\.key"
  expect_scan_reason "B .key CI 扫描" "$HERE/ci-secret-scan.sh" "$d" "\.key"

  make_client_tar_variant "$WORK/b/with-bpcert.tar.xz" \
    'mkdir -p extra && printf "%s\n" synthetic > extra/test.bpcert'
  d="$WORK/b-bpcert"; mkdir -p "$d"; cp -a "$WORK/b/with-bpcert.tar.xz" "$d/$(basename "$CLIENT_TAR")"
  expect_both_scans_fail "B 含测试用 .bpcert" "$d"
  expect_scan_reason "B .bpcert 构建期自查" "$HERE/ci-artifact-selfscan.sh" "$d" "bpcert"
  expect_scan_reason "B .bpcert CI 扫描" "$HERE/ci-secret-scan.sh" "$d" "bpcert"

  make_client_tar_variant "$WORK/b/with-pem.tar.xz" \
    'mkdir -p extra && printf -- "-----BEGIN RSA PRIVATE KEY-----\nFAKE-NOT-A-KEY\n-----END RSA PRIVATE KEY-----\n" > extra/fake.pem'
  d="$WORK/b-pem"; mkdir -p "$d"; cp -a "$WORK/b/with-pem.tar.xz" "$d/$(basename "$CLIENT_TAR")"
  expect_both_scans_fail "B 含模拟 PEM 私钥" "$d"
  expect_scan_reason "B PEM 构建期自查" "$HERE/ci-artifact-selfscan.sh" "$d" "私钥"
  expect_scan_reason "B PEM CI 扫描" "$HERE/ci-secret-scan.sh" "$d" "私钥"

  make_client_tar_variant "$WORK/b/with-token.tar.xz" \
    'mkdir -p extra && printf "BACKUP_TOKEN_SECRET=deadbeefdeadbeefdeadbeefdeadbeef\n" > extra/secrets-sample.txt'
  d="$WORK/b-token"; mkdir -p "$d"; cp -a "$WORK/b/with-token.tar.xz" "$d/$(basename "$CLIENT_TAR")"
  expect_both_scans_fail "B 含模拟 token secret" "$d"
  expect_scan_reason "B token 构建期自查" "$HERE/ci-artifact-selfscan.sh" "$d" "私钥|口令"
  expect_scan_reason "B token CI 扫描" "$HERE/ci-secret-scan.sh" "$d" "私钥|口令"

  # 32 字节"原始密钥形状"：放在**排除目录之外**必须拦下
  make_client_tar_variant "$WORK/b/with-raw.tar.xz" \
    'mkdir -p extra && printf "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" > extra/rawkey.bin'
  d="$WORK/b-raw"; mkdir -p "$d"; cp -a "$WORK/b/with-raw.tar.xz" "$d/$(basename "$CLIENT_TAR")"
  expect_both_scans_fail "B 含 32 字节原始密钥形状（排除目录之外）" "$d"
  expect_scan_reason "B 32 字节 构建期自查" "$HERE/ci-artifact-selfscan.sh" "$d" "32 字节"
  expect_scan_reason "B 32 字节 CI 扫描" "$HERE/ci-secret-scan.sh" "$d" "32 字节"
else
  ci_fail "B 找不到客户端 tar.xz：无法验证扫描一致性"
fi

if [ -n "$CLIENT_DEB" ]; then
  # 排除目录**之内**的 32 字节文件：按 common.sh 里写明的工程依据放行（这是已知的
  # 假阴性边界，两个入口必须一致地放行，才说明它们用的是同一套规则）。
  d="$WORK/b-excluded"; mkdir -p "$d/rel"
  make_client_deb_variant "$d/rel/$(basename "$CLIENT_DEB")" \
    'mkdir -p usr/share/backup-project && printf "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" > usr/share/backup-project/rawkey.bin'
  expect_ok "B 排除目录内的 32 字节文件：构建期自查一致放行" \
    bash "$HERE/ci-artifact-selfscan.sh" "$d/rel"
  expect_ok "B 排除目录内的 32 字节文件：CI 扫描一致放行" \
    bash "$HERE/ci-secret-scan.sh" "$d/rel"
fi

# ---- 6) C：完整发行清单 ----
ci_section "C 完整清单与哈希验收"
make_rel_dir "$WORK/c-ok"
expect_ok "C 完整发行目录通过" bash "$HERE/ci-release-manifest.sh" "$WORK/c-ok" --family all

for spec in "AppImage:*.AppImage" "客户端 deb:backup-project-client_*.deb" "服务端 tar:backup-project-server-*.tar.xz" "RELEASE-INFO.txt:RELEASE-INFO.txt" "SHA256SUMS:SHA256SUMS"; do
  label="${spec%%:*}"; pat="${spec#*:}"
  d="$WORK/c-missing-$(printf '%s' "$label" | tr -c 'A-Za-z0-9' '_')"
  make_rel_dir "$d"
  # shellcheck disable=SC2086
  victim="$(find "$d" -maxdepth 1 -name "$pat" -printf '%f\n' 2>/dev/null | sed -n '1p')"
  rm -f "$d/$victim"
  out="$(bash "$HERE/ci-release-manifest.sh" "$d" --family all 2>&1 || true)"
  if printf '%s' "$out" | grep -q "$victim"; then
    ci_pass "C 缺 $label 时报告了具体缺哪个文件"
  else
    ci_fail "C 缺 $label 时没有点名缺失文件"
  fi
  expect_fail "C 缺 $label -> 清单检查失败" \
    bash "$HERE/ci-release-manifest.sh" "$d" --family all
done

d="$WORK/c-bad-hash"; make_rel_dir "$d"
printf 'x' >> "$d/$(find "$d" -maxdepth 1 -name '*.tar.xz' -printf '%f\n' | sed -n '1p')"
expect_fail "C SHA256 与实际文件不符 -> 失败" \
  bash "$HERE/ci-release-manifest.sh" "$d" --family all

d="$WORK/c-wrong-version"; make_rel_dir "$d"
mv "$d/$(find "$d" -maxdepth 1 -name '*.AppImage' -printf '%f\n' | sed -n '1p')" "$d/Backup-Project-Client-9.9.9-x86_64.AppImage"
expect_fail "C 版本与正式版本不一致 -> 失败" \
  bash "$HERE/ci-release-manifest.sh" "$d" --family all

d="$WORK/c-duplicate"; make_rel_dir "$d"
cp -a "$d/$(find "$d" -maxdepth 1 -name '*.AppImage' -printf '%f\n' | sed -n '1p')" "$d/Backup-Project-Client-9.9.9-x86_64.AppImage"
expect_fail "C 同类重复制品 -> 失败" \
  bash "$HERE/ci-release-manifest.sh" "$d" --family all

d="$WORK/c-client-only"; make_rel_dir "$d" --only-client
expect_ok "C 只有客户端时 family=client 通过" \
  bash "$HERE/ci-release-manifest.sh" "$d" --family client
expect_fail "C 只有客户端时 family=all 失败（服务端缺失）" \
  bash "$HERE/ci-release-manifest.sh" "$d" --family all

# ---- 7) D：BUILD-INFO 契约 ----
ci_section "D BUILD-INFO / VERSION 契约（各产品族路径不同）"
if [ -n "$CLIENT_TAR" ]; then
  make_client_tar_variant "$WORK/b/no-buildinfo.tar.xz" 'rm -f BUILD-INFO.txt'
  d="$WORK/d-no-buildinfo"; mkdir -p "$d"; cp -a "$WORK/b/no-buildinfo.tar.xz" "$d/$(basename "$CLIENT_TAR")"
  out="$(bash "$HERE/ci-artifact-selfscan.sh" "$d" 2>&1 || true)"
  if printf '%s' "$out" | grep -q "找不到 BUILD-INFO.txt"; then
    ci_pass "D 缺 BUILD-INFO 时报告了具体原因"
  else
    ci_fail "D 缺 BUILD-INFO 时没有点名 BUILD-INFO"
  fi
  expect_fail "D 缺 BUILD-INFO -> 自查失败" bash "$HERE/ci-artifact-selfscan.sh" "$d"
fi
if [ -n "$APPIMAGE" ]; then
  d="$WORK/d-appimage"; mkdir -p "$d"; cp -a "$APPIMAGE" "$d/"
  expect_fail "D AppImage 的 BUILD-INFO 契约被改坏（注入错误版本）-> 失败" \
    bash "$HERE/ci-artifact-selfscan.sh" "$d" --expect-version 9.9.9
fi

# ---- 8) P2-01：生成器不得触发反引号命令替换 ----
ci_section "P2-01 生成器不得触发反引号命令替换"
d="$WORK/mats"; mkdir -p "$d/lib" "$d/plugins"
cp -a "$(find /usr/lib -name 'libz.so.1' -print -quit)" "$d/lib/"
cp -a "$d/lib/libz.so.1" "$d/plugins/libfakeplugin.so"
bash "$HERE/licenses/build-materials.sh" "$d" "$WORK/mats-out" "test-product" \
  > "$WORK/mats.log" 2>&1 || ci_fail "P2-01 生成器在合成输入上失败"
expect_eq "P2-01 生成器 stderr 没有 command not found" "0" \
  "$(grep -c 'command not found' "$WORK/mats.log" || true)"
# shellcheck disable=SC2016  # 单引号是有意的：这是交给 grep -E 的字面模式
expect_contains "P2-01 声明里保留了字面量反引号 .so" \
  "$WORK/mats-out/THIRD-PARTY-NOTICES.txt" '每个 `\.so` 一行'

ci_finish "packaging-quality"
