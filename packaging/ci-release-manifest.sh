#!/usr/bin/env bash
# packaging/ci-release-manifest.sh —— 发行目录的**完整清单**与哈希验收。
#
#   bash packaging/ci-release-manifest.sh <release 目录> \
#        [--family all|client|server] [--expect-version <版本>] [--expect-commit <sha>]
#
# 为什么需要它：其它检查都是"遍历目录里现有的文件、逐个检查"，缺一件制品时那个
# 循环直接跳过、整体照样 PASS。这个脚本反过来从**应有清单**出发：逐项要求存在、
# 唯一、命名/版本/平台正确，逐项重算哈希，并核对 SHA256SUMS 的集合与
# RELEASE-INFO 的版本 / commit。
#
# 家族（--family）：
#   all     正式发行目录：AppImage + 客户端 deb/tar + 服务端 deb/tar + RELEASE-INFO + SHA256SUMS
#   client  只要求客户端三件 + RELEASE-INFO + SHA256SUMS
#   server  只要求服务端两件 + RELEASE-INFO + SHA256SUMS
# 单独构建一族（build-release.sh --only client|server）时**不能**因为没有另一族的
# 制品而失败，所以家族必须由调用方显式给出。
set -Eeuo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/ci-lib.sh"

REL=""
FAMILY="all"
EXPECT_VERSION=""
EXPECT_COMMIT=""
while [ $# -gt 0 ]; do
  case "$1" in
    --family) FAMILY="${2:-}"; shift 2 ;;
    --expect-version) EXPECT_VERSION="${2:-}"; shift 2 ;;
    --expect-commit) EXPECT_COMMIT="${2:-}"; shift 2 ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    *) [ -z "$REL" ] || { echo "多余的参数：$1" >&2; exit 2; }; REL="$1"; shift ;;
  esac
done
[ -n "$REL" ] || { echo "用法: ci-release-manifest.sh <release 目录> [--family all|client|server]" >&2; exit 2; }
case "$FAMILY" in all|client|server) ;; *) echo "--family 只接受 all / client / server" >&2; exit 2 ;; esac
REL="$(cd "$REL" && pwd)"

INFO="$REL/RELEASE-INFO.txt"
if [ -f "$INFO" ]; then
  [ -n "$EXPECT_VERSION" ] || EXPECT_VERSION="$(grep -m1 '^version' "$INFO" | awk '{print $3}')"
  [ -n "$EXPECT_COMMIT" ] || EXPECT_COMMIT="$(grep -m1 '^commit' "$INFO" | awk '{print $3}')"
fi
if [ -z "$EXPECT_VERSION" ]; then
  ci_fail "无法确定期望版本（既没给 --expect-version，RELEASE-INFO.txt 也读不到）"
  ci_finish "release-manifest"
  exit 1
fi
V="$EXPECT_VERSION"
# .deb 的文件名与 Version 字段用的是 **Debian 版本**：semver 里 "-" 之后的预发布段在
# deb 里写成 "~"（见 lib/common.sh 的 deb_version_of）。单族构建用 --only server 配
# 带短横线的版本号时，两者会不一样 —— 这个差别是本地单族构建实测出来的。
DEB_V="$(grep -m1 '^deb_version' "$INFO" 2>/dev/null | awk '{print $3}')"
[ -n "$DEB_V" ] || DEB_V="$V"

ci_section "发行清单：家族 $FAMILY，版本 $V（deb: $DEB_V）"

declare -A PATTERN=(
  [appimage]='Backup-Project-Client-*-x86_64.AppImage'
  [client_deb]='backup-project-client_*_amd64.deb'
  [client_tar]='backup-project-client-*-linux-x86_64.tar.xz'
  [server_deb]='backup-project-server_*_amd64.deb'
  [server_tar]='backup-project-server-*-linux-x86_64.tar.xz'
)
declare -A EXPECTED=(
  [appimage]="Backup-Project-Client-$V-x86_64.AppImage"
  [client_deb]="backup-project-client_${DEB_V}_amd64.deb"
  [client_tar]="backup-project-client-$V-linux-x86_64.tar.xz"
  [server_deb]="backup-project-server_${DEB_V}_amd64.deb"
  [server_tar]="backup-project-server-$V-linux-x86_64.tar.xz"
)

case "$FAMILY" in
  all)    KEYS=(appimage client_deb client_tar server_deb server_tar) ;;
  client) KEYS=(appimage client_deb client_tar) ;;
  server) KEYS=(server_deb server_tar) ;;
esac

missing=0
dup=0
for key in "${KEYS[@]}"; do
  name="${EXPECTED[$key]}"
  pat="${PATTERN[$key]}"
  # shellcheck disable=SC2086
  matches="$(find "$REL" -maxdepth 1 -name "$pat" -printf '%f\n' 2>/dev/null | LC_ALL=C sort || true)"
  count="$(printf '%s' "$matches" | grep -c . || true)"
  if [ ! -f "$REL/$name" ]; then
    missing=$((missing + 1))
    if [ "$count" != "0" ]; then
      ci_fail "缺少制品 $name（同族另有：$(printf '%s' "$matches" | tr '\n' ' ')）"
    else
      ci_fail "缺少制品 $name"
    fi
  elif [ "$count" != "1" ]; then
    dup=$((dup + 1))
    ci_fail "制品 $name 的同类文件不是恰好 1 个（$count 个：$(printf '%s' "$matches" | tr '\n' ' ')）"
  else
    ci_pass "存在且唯一：$name"
  fi
done

actual_artifacts="$(find "$REL" -maxdepth 1 -type f \( -name '*.deb' -o -name '*.AppImage' -o -name '*.tar.xz' \) -printf '%f\n' 2>/dev/null | LC_ALL=C sort)"
# 本族必须齐备的制品
want_artifacts="$(for key in "${KEYS[@]}"; do printf '%s\n' "${EXPECTED[$key]}"; done | LC_ALL=C sort)"
# 允许出现的制品 = 正式发行包的全部五件。家族模式**只要求本族齐备**，但不允许出现
# 五件之外的东西（例如另一族的旧版本残留）——这样单独构建一族（CI 的 client / server
# 作业）不会因为"没有另一族"而失败，而正式 bundle 目录（--family all）仍是严格全检。
allowed_artifacts="$(for key in appimage client_deb client_tar server_deb server_tar; do printf '%s\n' "${EXPECTED[$key]}"; done | LC_ALL=C sort)"
extra="$(LC_ALL=C comm -13 <(printf '%s\n' "$allowed_artifacts") <(printf '%s\n' "$actual_artifacts") | grep -c . || true)"
if [ "$extra" != "0" ]; then
  ci_fail "发行目录里有未预期的发行文件（$extra 个，不在正式发行包里）："
  LC_ALL=C comm -13 <(printf '%s\n' "$allowed_artifacts") <(printf '%s\n' "$actual_artifacts") | sed 's/^/        /' >&2
else
  ci_pass "没有多余的发行文件（旧版本残留 / 重复制品）"
fi

if [ -f "$INFO" ]; then
  ci_pass "存在 RELEASE-INFO.txt"
  got_version="$(grep -m1 '^version' "$INFO" | awk '{print $3}')"
  expect_eq "RELEASE-INFO 的 version 与期望一致" "$V" "$got_version"
  got_arch="$(grep -m1 '^architecture' "$INFO" | awk '{print $3}')"
  case "$got_arch" in
    *amd64*) ci_pass "RELEASE-INFO 记录 amd64（$got_arch）" ;;
    *) ci_fail "RELEASE-INFO 的 architecture 不是 amd64：$got_arch" ;;
  esac
  got_commit_any="$(grep -m1 '^commit' "$INFO" | awk '{print $3}')"
  if printf '%s' "$got_commit_any" | grep -qE '^[0-9a-f]{40}$'; then
    ci_pass "RELEASE-INFO 的 commit 是完整的 40 位 SHA"
  else
    ci_fail "RELEASE-INFO 的 commit 形状不对：$got_commit_any"
  fi
  if [ -n "$EXPECT_COMMIT" ]; then
    expect_eq "RELEASE-INFO 的 commit 与期望一致" "$EXPECT_COMMIT" "$got_commit_any"
  fi
else
  ci_fail "缺少 RELEASE-INFO.txt"
fi

for key in client_deb server_deb; do
  [ -f "$REL/${EXPECTED[$key]}" ] || continue
  debv="$(dpkg-deb -f "$REL/${EXPECTED[$key]}" Version 2>/dev/null || true)"
  deba="$(dpkg-deb -f "$REL/${EXPECTED[$key]}" Architecture 2>/dev/null || true)"
  expect_eq "$key 的 deb Version 与期望一致" "$DEB_V" "$debv"
  expect_eq "$key 的 deb Architecture = amd64" "amd64" "$deba"
done

SUM="$REL/SHA256SUMS"
if [ ! -f "$SUM" ]; then
  ci_fail "缺少 SHA256SUMS"
else
  ci_pass "存在 SHA256SUMS"
  listed="$(awk '{ $1=""; sub(/^[ \t]+/, ""); sub(/^\*/, ""); if ($0 != "") print }' "$SUM" | LC_ALL=C sort)"
  # 制品必须**全部**在清单里 —— 这条不打折。
  # RELEASE-INFO.txt 则可有可无：构建期在单族目录里生成清单时它还没写出来（清单在
  # RELEASE-INFO 之前生成），而 CI bundle 阶段重算清单时它已经在了；两种都接受。
  listed_artifacts="$(printf '%s\n' "$listed" | grep -vx 'RELEASE-INFO.txt' | grep -c . || true)"
  missing_in_sums="$(LC_ALL=C comm -23 <(printf '%s\n' "$want_artifacts") <(printf '%s\n' "$listed") | grep -c . || true)"
  unknown_in_sums="$(LC_ALL=C comm -13 <(printf '%s\n' "$allowed_artifacts") <(printf '%s\n' "$listed" | grep -vx 'RELEASE-INFO.txt') | grep -c . || true)"
  expect_eq "SHA256SUMS 列出了本族全部制品（共 $listed_artifacts 个制品行）" "0" "$missing_in_sums"
  expect_eq "SHA256SUMS 没有列入发行包之外的文件" "0" "$unknown_in_sums"
  if [ "$missing_in_sums" != "0" ]; then
    LC_ALL=C comm -23 <(printf '%s\n' "$want_artifacts") <(printf '%s\n' "$listed") | sed 's/^/        /' >&2
  fi
  bad=0
  while read -r want_hash file; do
    [ -n "$file" ] || continue
    file="${file#\*}"
    if [ ! -f "$REL/$file" ]; then
      ci_fail "SHA256SUMS 列出的 $file 在目录里不存在"
      bad=$((bad + 1))
      continue
    fi
    got_hash="$(sha256sum "$REL/$file" | awk '{print $1}')"
    if [ "$got_hash" != "$want_hash" ]; then
      bad=$((bad + 1))
      ci_fail "$file 的实际 SHA256 与清单不符（清单 ${want_hash:0:12}… 实际 ${got_hash:0:12}…）"
    fi
  done < <(awk 'NF>=2 {print $1, $2}' "$SUM")
  expect_eq "SHA256SUMS 里每一项都实际重算通过" "0" "$bad"
fi

ci_finish "release-manifest"
