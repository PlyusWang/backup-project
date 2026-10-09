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
# 顶层条目另有**严格白名单**（第3轮 P3-01）：只允许五件制品 + SHA256SUMS +
# RELEASE-INFO.txt，多一个 debug.log、一个隐藏文件、一个目录都算失败。
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
  # 用 awk 而不是 grep|awk：报告缺字段时，grep 的"无匹配"退出码 1 会在 pipefail 下
  # 直接把脚本打断，看不到下面那条清楚的 ci_fail 诊断。
  [ -n "$EXPECT_VERSION" ] || EXPECT_VERSION="$(awk '/^version/ { print $3; exit }' "$INFO")"
  [ -n "$EXPECT_COMMIT" ] || EXPECT_COMMIT="$(awk '/^commit/ { print $3; exit }' "$INFO")"
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
DEB_V="$(awk '/^deb_version/ { print $3; exit }' "$INFO" 2>/dev/null)"
[ -n "$DEB_V" ] || DEB_V="$V"

# 行数统计：空行不算。用 awk 而不是 grep -c —— grep 在"0 行"时退出码是 1，旧代码
# 只能靠 `|| true` 压住，那正是本轮要收敛的"吞掉错误状态"写法（第3轮 P3-01）。
count_lines() { awk 'NF { n++ } END { print n + 0 }'; }

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
  matches="$(find "$REL" -maxdepth 1 -name "$pat" -printf '%f\n' | LC_ALL=C sort)"
  count="$(printf '%s' "$matches" | count_lines)"
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

# 本族必须齐备的制品
want_artifacts="$(for key in "${KEYS[@]}"; do printf '%s\n' "${EXPECTED[$key]}"; done | LC_ALL=C sort)"
# 允许出现的制品 = 正式发行包的全部五件。家族模式**只要求本族齐备**，但不允许出现
# 五件之外的东西（例如另一族的旧版本残留）——这样单独构建一族（CI 的 client / server
# 作业）不会因为"没有另一族"而失败，而正式 bundle 目录（--family all）仍是严格全检。
allowed_artifacts="$(for key in appimage client_deb client_tar server_deb server_tar; do printf '%s\n' "${EXPECTED[$key]}"; done | LC_ALL=C sort)"

# ---- 顶层条目白名单（第3轮 P3-01）------------------------------------------
# 官方发行目录的顶层**只允许**这 7 个名字：五件制品 + SHA256SUMS + RELEASE-INFO.txt。
# 旧实现只按 *.deb / *.AppImage / *.tar.xz 找"多余的发行文件"，于是 debug.log、
# .unexpected、意外目录这类东西根本不进检查 —— 白名单写成了黑名单。
# 这里枚举顶层的**全部**条目（含隐藏文件、目录、符号链接），逐个核对名字与类型。
allowed_top="$(printf '%s\n' "${EXPECTED[appimage]}" "${EXPECTED[client_deb]}" "${EXPECTED[client_tar]}" "${EXPECTED[server_deb]}" "${EXPECTED[server_tar]}" 'SHA256SUMS' 'RELEASE-INFO.txt' | LC_ALL=C sort -u)"
[ "$(printf '%s\n' "$allowed_top" | count_lines)" -eq 7 ] \
  || ci_fail "白名单条目数不是 7（脚本自身的错误）"

top_bad=0
while IFS=$'\t' read -r tname ttype; do
  [ -n "$tname" ] || continue
  if ! printf '%s\n' "$allowed_top" | grep -qxF -- "$tname"; then
    top_bad=$((top_bad + 1))
    case "$tname" in
      .*) ci_fail "发行目录顶层出现未预期的隐藏文件：$tname（类型 $ttype）" ;;
      *)  ci_fail "发行目录顶层出现未预期条目：$tname（类型 $ttype）" ;;
    esac
    continue
  fi
  if [ "$ttype" != "f" ]; then
    top_bad=$((top_bad + 1))
    ci_fail "发行目录里的 $tname 不是普通文件（类型 $ttype；d=目录 l=符号链接）"
  fi
done < <(find "$REL" -mindepth 1 -maxdepth 1 -printf '%f\t%y\n' | LC_ALL=C sort)
if [ "$top_bad" -eq 0 ]; then
  ci_pass "顶层条目全部在白名单内（五件制品 + SHA256SUMS + RELEASE-INFO.txt）"
fi

if [ -f "$INFO" ]; then
  ci_pass "存在 RELEASE-INFO.txt"
  got_version="$(awk '/^version/ { print $3; exit }' "$INFO")"
  expect_eq "RELEASE-INFO 的 version 与期望一致" "$V" "$got_version"
  got_arch="$(awk '/^architecture/ { print $3; exit }' "$INFO")"
  case "$got_arch" in
    *amd64*) ci_pass "RELEASE-INFO 记录 amd64（$got_arch）" ;;
    *) ci_fail "RELEASE-INFO 的 architecture 不是 amd64：$got_arch" ;;
  esac
  got_commit_any="$(awk '/^commit/ { print $3; exit }' "$INFO")"
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
  debv=""
  deba=""
  if ! debv="$(dpkg-deb -f "$REL/${EXPECTED[$key]}" Version 2>/dev/null)"; then
    ci_fail "$key 的 deb 元数据读取失败（dpkg-deb -f Version）：${EXPECTED[$key]}"
  fi
  if ! deba="$(dpkg-deb -f "$REL/${EXPECTED[$key]}" Architecture 2>/dev/null)"; then
    ci_fail "$key 的 deb 元数据读取失败（dpkg-deb -f Architecture）：${EXPECTED[$key]}"
  fi
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
  listed_no_info="$(printf '%s\n' "$listed" | awk '$0 != "RELEASE-INFO.txt"')"
  listed_artifacts="$(printf '%s\n' "$listed_no_info" | count_lines)"
  missing_in_sums="$(LC_ALL=C comm -23 <(printf '%s\n' "$want_artifacts") <(printf '%s\n' "$listed") | count_lines)"
  unknown_in_sums="$(LC_ALL=C comm -13 <(printf '%s\n' "$allowed_artifacts") <(printf '%s\n' "$listed_no_info") | count_lines)"
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
