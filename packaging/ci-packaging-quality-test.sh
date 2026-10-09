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
SERVER_TAR="$(find "$REL" -maxdepth 1 -name 'backup-project-server-*-linux-x86_64.tar.xz' -print -quit 2>/dev/null || true)"
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

# 通用重打包：把任意 portable tar 解开、执行片段、再打回去（保持单一顶层目录）。
make_tar_variant() {  # make_tar_variant <源 tar> <输出 tar> <片段>
  local src="$1" out="$2" snippet="$3" d top
  d="$(mktemp -d "$WORK/tv.XXXXXX")"
  tar -xJf "$src" -C "$d"
  top="$(find "$d" -maxdepth 1 -mindepth 1 -type d | sed -n '1p')"
  ( cd "$top" && eval "$snippet" )
  ( cd "$d" && tar -cJf "$out" "$(basename "$top")" )
  rm -rf "$d"
}

# 直接把共享函数当被测对象：打印退出码 / 合并输出（不依赖文件权限，root 下也成立）。
scan_fn_rc() {   # scan_fn_rc <函数名> <扫描根>
  bash -c 'source "$1"; rc=0; "$2" "$3" >/dev/null 2>&1 || rc=$?; printf "%s" "$rc"' \
    _ "$HERE/lib/common.sh" "$1" "$2"
}
scan_fn_out() {  # scan_fn_out <函数名> <扫描根>
  bash -c 'source "$1"; rc=0; "$2" "$3" 2>&1 || rc=$?; exit 0' \
    _ "$HERE/lib/common.sh" "$1" "$2"
}

# 运行一个检查脚本，打印 "rc|合并输出" 交给调用方断言（不吞任何状态）。
run_capture() {  # run_capture <命令...>
  local rc=0 out
  out="$("$@" 2>&1)" || rc=$?
  printf '%s|%s' "$rc" "$out"
}

# 清单检查必须以非零退出、并且**点名**那个非法条目、并给出对应原因。
expect_top_reject() {  # expect_top_reject <标签> <目录> <应点名> <原因正则>
  local label="$1" dir="$2" name="$3" pattern="$4" res rc out
  res="$(run_capture bash "$HERE/ci-release-manifest.sh" "$dir" --family all)"
  rc="${res%%|*}"; out="${res#*|}"
  if [ "$rc" -eq 0 ]; then ci_fail "$label（本应失败却成功了）"; return; fi
  if ! printf '%s' "$out" | grep -qF -- "$name"; then
    ci_fail "$label（失败信息里没有点名 $name）"
    printf '%s\n' "$out" | tail -6 | sed 's/^/        /' >&2
    return
  fi
  if ! printf '%s' "$out" | grep -qE -- "$pattern"; then
    ci_fail "$label（原因里没有 $pattern）"
    printf '%s\n' "$out" | tail -6 | sed 's/^/        /' >&2
    return
  fi
  ci_pass "$label（点名 $name 且原因正确）"
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


# ---- 9) 第3轮 P2-01：扫描出错必须 fail-closed ----
ci_section "第3轮 P2-01 扫描的错误状态必须传播（共享函数 + 两个入口）"
for fn in secret_scan_content_files secret_scan_key_files secret_scan_raw_key_files; do
  expect_eq "P2-01 $fn：扫描根不存在 -> 返回 2" "2" "$(scan_fn_rc "$fn" "$WORK/r3-missing-root")"
done
out="$(scan_fn_out secret_scan_content_files "$WORK/r3-missing-root")"
if printf '%s' "$out" | grep -q "扫描根目录不存在"; then
  ci_pass "P2-01 扫描根不存在时给出明确诊断"
else
  ci_fail "P2-01 扫描根不存在时没有给出诊断"
  printf '%s\n' "$out" | sed 's/^/        /' >&2
fi
printf 'x' > "$WORK/r3-not-a-dir"
expect_eq "P2-01 扫描根不是目录 -> 返回 2" "2" "$(scan_fn_rc secret_scan_content_files "$WORK/r3-not-a-dir")"
out="$(scan_fn_out secret_scan_key_files "$WORK/r3-not-a-dir")"
if printf '%s' "$out" | grep -q "不是目录"; then
  ci_pass "P2-01 扫描根不是目录时给出明确诊断"
else
  ci_fail "P2-01 扫描根不是目录时没有给出诊断"
fi
expect_eq "P2-01 正对照：正常根上内容规则扫描成功" "0" "$(scan_fn_rc secret_scan_content_files "$REL")"

# 确定性故障注入：让 grep 在内容扫描那一次调用上退出 2（不依赖权限，root 下同样成立）
REAL_GREP="$(command -v grep)"
d="$WORK/r3-inject"; fakebin="$WORK/r3-inject-bin"
mkdir -p "$d" "$fakebin"
cp -a "$CLIENT_TAR" "$d/$(basename "$CLIENT_TAR")"
{
  printf '%s\n' '#!/usr/bin/env bash'
  printf '%s\n' 'for a in "$@"; do'
  printf '%s\n' '  if [ "$a" = "-rIlE" ]; then printf "grep: 注入的读取错误\n" >&2; exit 2; fi'
  printf '%s\n' 'done'
  printf 'exec %s "$@"\n' "$REAL_GREP"
} > "$fakebin/grep"
chmod 0755 "$fakebin/grep"
res="$(PATH="$fakebin:$PATH" run_capture bash "$HERE/ci-artifact-selfscan.sh" "$d")"
rc="${res%%|*}"; out="${res#*|}"
if [ "$rc" -ne 0 ] && printf '%s' "$out" | grep -q "扫描出错"; then
  ci_pass "P2-01 注入 grep 退出 2 -> 构建期自查失败且原因正确"
else
  ci_fail "P2-01 注入 grep 退出 2 -> 构建期自查本应失败（rc=$rc）"
  printf '%s\n' "$out" | tail -6 | sed 's/^/        /' >&2
fi
res="$(PATH="$fakebin:$PATH" run_capture bash "$HERE/ci-secret-scan.sh" "$d")"
rc="${res%%|*}"; out="${res#*|}"
if [ "$rc" -ne 0 ] && printf '%s' "$out" | grep -qE "扫描出错|没有跑完"; then
  ci_pass "P2-01 注入 grep 退出 2 -> CI 扫描失败且原因正确"
else
  ci_fail "P2-01 注入 grep 退出 2 -> CI 扫描本应失败（rc=$rc）"
  printf '%s\n' "$out" | tail -6 | sed 's/^/        /' >&2
fi

# 读取错误：制品里放一个 0000 权限的文件。root 会绕过文件权限，那种情况下不做假测试。
if [ "$(id -u)" -eq 0 ]; then
  printf '  SKIP  不可读文件注入：当前是 root（文件权限不生效），不构造假测试\n'
else
  make_tar_variant "$CLIENT_TAR" "$WORK/r3-locked-src.tar.xz"     'mkdir -p extra && printf "synthetic\n" > extra/locked.txt'
  d="$WORK/r3-locked"; mkdir -p "$d"
  python3 - "$WORK/r3-locked-src.tar.xz" "$d/$(basename "$CLIENT_TAR")" <<'PYEOF'
import io, sys, tarfile
src, dst = sys.argv[1], sys.argv[2]
with tarfile.open(src) as t, tarfile.open(dst, "w:xz") as o:
    for m in t.getmembers():
        if m.name.endswith("extra/locked.txt"):
            m.mode = 0
        o.addfile(m, t.extractfile(m) if m.isfile() else None)
PYEOF
  chk="$WORK/r3-locked-check"; mkdir -p "$chk"
  tar -xJf "$d/$(basename "$CLIENT_TAR")" -C "$chk"
  locked="$(find "$chk" -name locked.txt -print -quit)"
  if [ -n "$locked" ] && [ ! -r "$locked" ]; then
    ci_pass "P2-01 夹具确实造出不可读文件（注入有效）"
  else
    ci_fail "P2-01 夹具没有造出不可读文件，这条测试无意义"
  fi
  res="$(run_capture bash "$HERE/ci-artifact-selfscan.sh" "$d")"
  rc="${res%%|*}"; out="${res#*|}"
  if [ "$rc" -ne 0 ] && printf '%s' "$out" | grep -q "扫描出错"; then
    ci_pass "P2-01 读不了制品里的文件 -> 构建期自查失败（不再报告 0 命中）"
  else
    ci_fail "P2-01 读不了制品里的文件 -> 构建期自查本应失败（rc=$rc）"
    printf '%s\n' "$out" | tail -6 | sed 's/^/        /' >&2
  fi
  res="$(run_capture bash "$HERE/ci-secret-scan.sh" "$d")"
  rc="${res%%|*}"; out="${res#*|}"
  if [ "$rc" -ne 0 ] && printf '%s' "$out" | grep -qE "扫描出错|没有跑完"; then
    ci_pass "P2-01 读不了制品里的文件 -> CI 扫描失败（不再报告 0 命中）"
  else
    ci_fail "P2-01 读不了制品里的文件 -> CI 扫描本应失败（rc=$rc）"
    printf '%s\n' "$out" | tail -6 | sed 's/^/        /' >&2
  fi
fi

# 反向：第三方二进制里的 PEM 字符串常量（grep -I 视为二进制）不得误报
make_client_tar_variant "$WORK/r3-binary-pem.tar.xz"   'mkdir -p extra && printf "x\000-----BEGIN RSA PRIVATE KEY-----\000y" > extra/libtls-fake.so'
d="$WORK/r3-binary-pem"; mkdir -p "$d"; cp -a "$WORK/r3-binary-pem.tar.xz" "$d/$(basename "$CLIENT_TAR")"
expect_ok "P2-01 二进制里的 PEM 字符串常量不误报（构建期自查）" bash "$HERE/ci-artifact-selfscan.sh" "$d"
expect_ok "P2-01 二进制里的 PEM 字符串常量不误报（CI 扫描）" bash "$HERE/ci-secret-scan.sh" "$d"

# 入口层：发行目录不存在也必须失败（不许"扫了个寂寞还报成功"）
expect_fail "P2-01 入口：发行目录不存在 -> 构建期自查失败" bash "$HERE/ci-artifact-selfscan.sh" "$WORK/r3-no-such-release"
expect_fail "P2-01 入口：发行目录不存在 -> CI 扫描失败" bash "$HERE/ci-secret-scan.sh" "$WORK/r3-no-such-release"

# ---- 10) 第3轮 P2-02：portable tar 的严格路径 ----
ci_section "第3轮 P2-02 portable tar 的 BUILD-INFO / VERSION 必须严格在 <top>/ 下"
if [ -z "$CLIENT_TAR" ]; then
  ci_fail "P2-02 找不到客户端 portable tar：无法验证严格路径"
else
  mkdir -p "$WORK/r3"
  d="$WORK/r3-tar-ok"; mkdir -p "$d"; cp -a "$CLIENT_TAR" "$d/"
  expect_ok "P2-02 正对照：正常客户端 portable tar 通过" bash "$HERE/ci-artifact-selfscan.sh" "$d"

  make_client_tar_variant "$WORK/r3/move-info.tar.xz"     'mkdir -p random-place && mv BUILD-INFO.txt random-place/'
  d="$WORK/r3-move-info"; mkdir -p "$d"; cp -a "$WORK/r3/move-info.tar.xz" "$d/$(basename "$CLIENT_TAR")"
  expect_scan_reason "P2-02 BUILD-INFO 被挪到 random-place/ -> 失败"     "$HERE/ci-artifact-selfscan.sh" "$d" "找不到 BUILD-INFO.txt"

  make_client_tar_variant "$WORK/r3/move-version.tar.xz"     'mkdir -p random-place && mv share/backup-project/VERSION random-place/'
  d="$WORK/r3-move-version"; mkdir -p "$d"; cp -a "$WORK/r3/move-version.tar.xz" "$d/$(basename "$CLIENT_TAR")"
  expect_scan_reason "P2-02 VERSION 被挪到 random-place/ -> 失败"     "$HERE/ci-artifact-selfscan.sh" "$d" "找不到 share/backup-project/VERSION"

  make_client_tar_variant "$WORK/r3/no-version.tar.xz" 'rm -f share/backup-project/VERSION'
  d="$WORK/r3-no-version"; mkdir -p "$d"; cp -a "$WORK/r3/no-version.tar.xz" "$d/$(basename "$CLIENT_TAR")"
  expect_scan_reason "P2-02 VERSION 缺失 -> 失败"     "$HERE/ci-artifact-selfscan.sh" "$d" "找不到 share/backup-project/VERSION"

  mkdir -p "$WORK/r3-two-tops/src" "$WORK/r3-two-tops/rel"
  tar -xJf "$CLIENT_TAR" -C "$WORK/r3-two-tops/src"
  top="$(find "$WORK/r3-two-tops/src" -maxdepth 1 -mindepth 1 -type d | sed -n '1p')"
  mkdir -p "$WORK/r3-two-tops/src/extra-top"
  ( cd "$WORK/r3-two-tops/src" && tar -cJf "$WORK/r3-two-tops/rel/$(basename "$CLIENT_TAR")" "$(basename "$top")" extra-top )
  expect_scan_reason "P2-02 顶层目录不唯一 -> 失败"     "$HERE/ci-artifact-selfscan.sh" "$WORK/r3-two-tops/rel" "顶层内容不唯一"

  make_client_tar_variant "$WORK/r3/symlink-info.tar.xz"     'rm -f BUILD-INFO.txt && ln -s /etc/hostname BUILD-INFO.txt'
  d="$WORK/r3-symlink-info"; mkdir -p "$d"; cp -a "$WORK/r3/symlink-info.tar.xz" "$d/$(basename "$CLIENT_TAR")"
  expect_scan_reason "P2-02 BUILD-INFO 是符号链接（指向树外）-> 失败"     "$HERE/ci-artifact-selfscan.sh" "$d" "是符号链接"

  outside="$WORK/r3-outside"; mkdir -p "$outside/backup-project"; printf '0.1.1\n' > "$outside/backup-project/VERSION"
  make_client_tar_variant "$WORK/r3/symlink-share.tar.xz" "rm -rf share && ln -s $outside share"
  d="$WORK/r3-symlink-share"; mkdir -p "$d"; cp -a "$WORK/r3/symlink-share.tar.xz" "$d/$(basename "$CLIENT_TAR")"
  expect_scan_reason "P2-02 share 是指向树外的符号链接（真实路径逃逸）-> 失败"     "$HERE/ci-artifact-selfscan.sh" "$d" "真实路径逃出"

  d="$WORK/r3-tar-ok"
  expect_fail "P2-02 VERSION 与期望版本不一致 -> 失败"     bash "$HERE/ci-artifact-selfscan.sh" "$d" --expect-version 9.9.9
  expect_fail "P2-02 BUILD-INFO commit 与期望不一致 -> 失败"     bash "$HERE/ci-artifact-selfscan.sh" "$d" --expect-commit 0000000000000000000000000000000000000000

  # 解包阶段本身的路径安全：GNU tar 必须拒绝 ".." 成员与"穿过符号链接写入"
  python3 - "$WORK/r3-dots.tar.xz" <<'PYEOF'
import io, sys, tarfile
with tarfile.open(sys.argv[1], "w:xz") as t:
    ti = tarfile.TarInfo("../escaped.txt"); ti.size = 3
    t.addfile(ti, io.BytesIO(b"bad"))
PYEOF
  d="$WORK/r3-dots"; mkdir -p "$d"; cp -a "$WORK/r3-dots.tar.xz" "$d/$(basename "$CLIENT_TAR")"
  expect_scan_reason "P2-02 tar 成员名含 .. -> 解包即失败"     "$HERE/ci-artifact-selfscan.sh" "$d" "解包失败"
  expect_fail "P2-02 tar 成员名含 .. -> CI 扫描同样失败" bash "$HERE/ci-secret-scan.sh" "$d"

  rm -rf /tmp/r3-escape-target
  python3 - "$WORK/r3-symlink-write.tar.xz" <<'PYEOF'
import io, sys, tarfile
with tarfile.open(sys.argv[1], "w:xz") as t:
    link = tarfile.TarInfo("top/link"); link.type = tarfile.SYMTYPE
    link.linkname = "/tmp/r3-escape-target"; t.addfile(link)
    data = b"pwned\n"
    member = tarfile.TarInfo("top/link/pwned.txt"); member.size = len(data)
    t.addfile(member, io.BytesIO(data))
PYEOF
  d="$WORK/r3-symlink-write"; mkdir -p "$d"; cp -a "$WORK/r3-symlink-write.tar.xz" "$d/$(basename "$CLIENT_TAR")"
  expect_fail "P2-02 tar 试图穿过符号链接写入 -> 解包失败" bash "$HERE/ci-artifact-selfscan.sh" "$d"
  if [ -e /tmp/r3-escape-target/pwned.txt ]; then
    ci_fail "P2-02 解包发生了路径逃逸（/tmp/r3-escape-target/pwned.txt 被写出）"
  else
    ci_pass "P2-02 解包没有逃逸（树外没有被写入）"
  fi
fi
if [ -z "$SERVER_TAR" ]; then
  printf '  SKIP  找不到服务端 portable tar：跳过服务端正对照\n'
else
  d="$WORK/r3-server-tar"; mkdir -p "$d"; cp -a "$SERVER_TAR" "$d/"
  expect_ok "P2-02 正对照：正常服务端 portable tar 通过" bash "$HERE/ci-artifact-selfscan.sh" "$d"
fi

# ---- 11) 第3轮 P3-01：发行目录顶层严格白名单 ----
ci_section "第3轮 P3-01 发行目录顶层只允许七件套"
d="$WORK/r3-top-ok"; make_rel_dir "$d"
expect_ok "P3-01 正对照：七件套通过" bash "$HERE/ci-release-manifest.sh" "$d" --family all

d="$WORK/r3-top-log"; make_rel_dir "$d"; printf 'noise\n' > "$d/debug.log"
expect_top_reject "P3-01 多出 debug.log" "$d" "debug.log" "未预期条目"

d="$WORK/r3-top-hidden"; make_rel_dir "$d"; printf 'x\n' > "$d/.unexpected"
expect_top_reject "P3-01 多出隐藏文件 .unexpected" "$d" ".unexpected" "隐藏文件"

d="$WORK/r3-top-dir"; make_rel_dir "$d"; mkdir -p "$d/extra-dir"; printf 'x\n' > "$d/extra-dir/f.txt"
expect_top_reject "P3-01 多出意外目录" "$d" "extra-dir" "未预期条目"

d="$WORK/r3-top-zip"; make_rel_dir "$d"; printf 'x\n' > "$d/weird.zip"
expect_top_reject "P3-01 多出未知格式文件" "$d" "weird.zip" "未预期条目"

d="$WORK/r3-top-symlink"; make_rel_dir "$d"; rm -f "$d/SHA256SUMS"; ln -s /etc/hostname "$d/SHA256SUMS"
expect_top_reject "P3-01 SHA256SUMS 被符号链接冒充" "$d" "SHA256SUMS" "不是普通文件"

# 兼容性：单族目录里出现另一族的**合法**制品不得误伤（CI 的 client / server 作业）
d="$WORK/r3-top-cross"; make_rel_dir "$d" --only-client
cp -a "$REL"/backup-project-server_*.deb "$REL"/backup-project-server-*-linux-x86_64.tar.xz "$d/" 2>/dev/null || true
expect_ok "P3-01 客户端目录里带另一族合法制品：family=client 通过"   bash "$HERE/ci-release-manifest.sh" "$d" --family client
expect_fail "P3-01 客户端目录里带另一族合法制品：family=all 仍失败（本族不齐）"   bash "$HERE/ci-release-manifest.sh" "$d" --family all

ci_finish "packaging-quality"
