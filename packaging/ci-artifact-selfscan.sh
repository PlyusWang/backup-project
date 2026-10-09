#!/usr/bin/env bash
# packaging/ci-artifact-selfscan.sh —— 构建期与 CI 共用的制品自查。
#
#   bash packaging/ci-artifact-selfscan.sh <release 目录> \
#        [--expect-commit <sha>] [--expect-version <版本>]
#
# 做三件事：
#   1) 把每件制品**解包**，并核对解出来的真实内容（解包失败即失败，不许跳过未知类型）；
#   2) 用 packaging/lib/common.sh 里的统一规则做私钥/口令扫描 —— 与 CI 的
#      ci-secret-scan.sh 是**同一份实现**（v0.1.1 的教训：两处各写一份必然漂移）；
#   3) 逐个产品族核对 BUILD-INFO.txt / VERSION 的存在与内容（见下面的"真实路径"）。
#
# 为什么 BUILD-INFO 的路径要按产品族写死：五种制品的打包结构本来就不一样 ——
#   AppImage            usr/share/backup-project/BUILD-INFO.txt
#   client .deb         usr/share/doc/backup-project-client/BUILD-INFO.txt
#   client portable tar BUILD-INFO.txt（树根）
#   server .deb         usr/share/doc/backup-project-server/BUILD-INFO.txt
#   server portable tar BUILD-INFO.txt（树根）
# v0.1.1 的验尸命令想当然地按 .deb 的路径去找 AppImage，得到"找不到"，却没有据此
# 失败 —— 那正是本条检查要消灭的假通过。这些路径由测试钉住。
set -Eeuo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/lib/common.sh"
source "$(dirname "${BASH_SOURCE[0]}")/ci-lib.sh"

REL=""
EXPECT_COMMIT=""
EXPECT_VERSION=""
while [ $# -gt 0 ]; do
  case "$1" in
    --expect-commit) EXPECT_COMMIT="${2:-}"; shift 2 ;;
    --expect-version) EXPECT_VERSION="${2:-}"; shift 2 ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    *) [ -z "$REL" ] || die "多余的参数：$1"; REL="$1"; shift ;;
  esac
done
[ -n "$REL" ] || die "用法: ci-artifact-selfscan.sh <release 目录> [--expect-commit <sha>] [--expect-version <版本>]"
REL="$(cd "$REL" && pwd)"

# 没显式给期望值时，从 RELEASE-INFO.txt 里读（构建期与 CI 都有这个文件）。
if [ -f "$REL/RELEASE-INFO.txt" ]; then
  [ -n "$EXPECT_COMMIT" ] || EXPECT_COMMIT="$(grep -m1 '^commit' "$REL/RELEASE-INFO.txt" | awk '{print $3}')"
  [ -n "$EXPECT_VERSION" ] || EXPECT_VERSION="$(grep -m1 '^version' "$REL/RELEASE-INFO.txt" | awk '{print $3}')"
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# 解包一件制品并核对内容。核对项一律与产品族无关（谁有什么就查什么）。
unpack_one() {  # $1 = 制品路径, $2 = 目标目录
  local artifact="$1" dest="$2" base
  base="$(basename "$artifact")"
  rm -rf "$dest"; mkdir -p "$dest"
  case "$artifact" in
    backup-project-client_*.deb|backup-project-server_*.deb)
      dpkg-deb -x "$artifact" "$dest" || die "解包失败（dpkg-deb -x）：$base"
      compgen -G "$dest/usr/*" > /dev/null || die "deb 解包结果异常：$base" ;;
    *.deb)
      dpkg-deb -x "$artifact" "$dest" || die "解包失败（dpkg-deb -x）：$base"
      compgen -G "$dest/usr/*" > /dev/null || die "deb 解包结果异常：$base" ;;
    *.tar.xz)
      tar -xJf "$artifact" -C "$dest" || die "解包失败（tar -xJf）：$base"
      compgen -G "$dest/*/bin/*" > /dev/null || die "tar 解包结果异常：$base" ;;
    *.AppImage)
      cp -f "$artifact" "$dest/.extract.AppImage"
      chmod 0755 "$dest/.extract.AppImage"
      if ! ( cd "$dest" && ./.extract.AppImage --appimage-extract ) > /dev/null 2>&1; then
        rm -f "$dest/.extract.AppImage"
        die "AppImage 解包失败：$base"
      fi
      rm -f "$dest/.extract.AppImage"
      compgen -G "$dest/squashfs-root/usr/bin/*" > /dev/null \
        || die "AppImage 解包结果异常：$base" ;;
    *)
      die "不认识的发行文件（不允许跳过检查）：$base" ;;
  esac
}

# 该产品族的 BUILD-INFO 路径（相对解包目录）。打印空串 = 不认识的形状。
build_info_rel() {  # $1 = 制品基名
  case "$1" in
    *.AppImage) printf '%s' 'squashfs-root/usr/share/backup-project/BUILD-INFO.txt' ;;
    backup-project-client_*.deb) printf '%s' 'usr/share/doc/backup-project-client/BUILD-INFO.txt' ;;
    backup-project-server_*.deb) printf '%s' 'usr/share/doc/backup-project-server/BUILD-INFO.txt' ;;
    backup-project-client-*.tar.xz|backup-project-server-*.tar.xz) printf '%s' 'BUILD-INFO.txt' ;;
    *) printf '%s' '' ;;
  esac
}

# 该产品族的 VERSION 路径（AppImage 与 .deb 有 share/backup-project/VERSION；
# portable tar 也有；.deb 的在 usr/lib/<pkg>/share/backup-project/VERSION）。
version_rel() {  # $1 = 制品基名
  case "$1" in
    *.AppImage) printf '%s' 'squashfs-root/usr/share/backup-project/VERSION' ;;
    backup-project-client_*.deb) printf '%s' 'usr/lib/backup-project-client/share/backup-project/VERSION' ;;
    backup-project-server_*.deb) printf '%s' 'usr/lib/backup-project-server/share/backup-project/VERSION' ;;
    backup-project-client-*.tar.xz|backup-project-server-*.tar.xz) printf '%s' 'share/backup-project/VERSION' ;;
    *) printf '%s' '' ;;
  esac
}

# ---- portable tar 的严格布局（第3轮 P2-02）---------------------------------
#
# 契约（客户端 / 服务端一致，只有顶层目录名的大小写不同）：
#   <唯一顶层目录>/BUILD-INFO.txt
#   <唯一顶层目录>/share/backup-project/VERSION
# 上一轮用"递归 find 找同名文件、恰好一个就通过"，那只保证**名字唯一**，不保证
# **位置正确**：BUILD-INFO.txt 被挪到 random-place/ 也照样通过。这一轮改成先钉住
# 唯一的顶层目录，再按固定相对路径取文件，并核对文件类型与真实路径归属。
tar_top_dir() {  # $1 = 解包目录；打印唯一顶层目录（不合法即 die）
  local dest="$1" entry top="" count=0
  for entry in "$dest"/* "$dest"/.[!.]* "$dest"/..?*; do
    [ -e "$entry" ] || [ -L "$entry" ] || continue
    count=$((count + 1))
    [ -n "$top" ] || top="$entry"
  done
  [ "$count" -gt 0 ] || die "tar 解包结果为空（顶层没有任何条目）：$dest"
  [ "$count" -eq 1 ] || die "portable tar 顶层内容不唯一（$count 项；契约要求恰好一个顶层目录）：$dest"
  [ ! -L "$top" ] || die "portable tar 的顶层条目是符号链接（契约要求真实目录）：$top"
  [ -d "$top" ] || die "portable tar 的顶层条目不是目录：$top"
  printf '%s' "$top"
}

# 严格取一个契约文件：必须是根目录下的**普通文件**（不许符号链接），真实路径不得
# 逃出根目录。$1 = 根目录, $2 = 相对路径, $3 = 制品基名（诊断用）
require_contract_file() {
  local root="$1" rel="$2" base="$3" path real
  path="$root/$rel"
  [ -e "$path" ] || [ -L "$path" ] || die "$base 里找不到 $rel（按发行布局的固定路径查找，不做递归搜索）"
  [ ! -L "$path" ] || die "$base 的 $rel 是符号链接，拒绝（发行契约要求普通文件）"
  [ -f "$path" ] || die "$base 的 $rel 不是普通文件"
  real="$(readlink -f -- "$path")" || die "$base 的 $rel 无法解析真实路径"
  case "$real" in
    "$root"/*) : ;;
    *) die "$base 的 $rel 真实路径逃出 $root" ;;
  esac
  printf '%s' "$path"
}

log "== 制品自查：解包 + 安全检查 + BUILD-INFO 契约 =="
SCAN_TARGET="$WORK/scan"
mkdir -p "$SCAN_TARGET"
checked=0
for artifact in "$REL"/*; do
  [ -e "$artifact" ] || continue
  case "$artifact" in *SHA256SUMS|*RELEASE-INFO.txt) continue ;; esac
  base="$(basename "$artifact")"
  dest="$SCAN_TARGET/$base"
  unpack_one "$artifact" "$dest"
  checked=$((checked + 1))

  # ---- BUILD-INFO / VERSION 契约（按产品族固定路径，严格核对）----
  rel="$(build_info_rel "$base")"
  [ -n "$rel" ] || die "不认识的发行文件形状（无法核对 BUILD-INFO）：$base"
  vrel="$(version_rel "$base")"
  [ -n "$vrel" ] || die "不认识的发行文件形状（无法核对 VERSION）：$base"
  case "$base" in
    *.tar.xz)
      # portable tar：先钉住唯一顶层目录，再按 <top>/... 取文件（不递归找同名文件）。
      top="$(tar_top_dir "$dest")"
      info="$(require_contract_file "$top" "$rel" "$base")"
      vfile="$(require_contract_file "$top" "$vrel" "$base")" ;;
    *)
      info="$(require_contract_file "$dest" "$rel" "$base")"
      vfile="$(require_contract_file "$dest" "$vrel" "$base")" ;;
  esac
  if [ -n "$EXPECT_VERSION" ]; then
    got_version="$(grep -m1 '^version' "$info" | awk '{print $3}')"
    [ "$got_version" = "$EXPECT_VERSION" ] \
      || die "$base 的 BUILD-INFO version=$got_version，期望 $EXPECT_VERSION"
  fi
  if [ -n "$EXPECT_COMMIT" ]; then
    got_commit="$(grep -m1 '^commit' "$info" | awk '{print $3}')"
    [ "$got_commit" = "$EXPECT_COMMIT" ] \
      || die "$base 的 BUILD-INFO commit=$got_commit，期望 $EXPECT_COMMIT"
  fi
  if [ -n "$EXPECT_VERSION" ]; then
    got_v="$(head -1 "$vfile")"
    [ "$got_v" = "$EXPECT_VERSION" ] \
      || die "$base 的 VERSION=$got_v，期望 $EXPECT_VERSION"
  fi
  log "  $base：BUILD-INFO/VERSION 契约通过（version=${EXPECT_VERSION:-未指定}）"
done
[ "$checked" -gt 0 ] || die "发行目录里没有任何制品：$REL"

# ---- 统一的安全检查（与 ci-secret-scan.sh 同一份规则）----
# 三个扫描函数返回 2 = 扫描本身出错（根目录不存在 / 不可读、grep / find 报错）。
# 出错时必须让自查失败：把"没扫成"说成"没有私钥"就是假通过（第3轮 P2-01）。
SCAN_FAILED=0
content_files="$(secret_scan_content_files "$SCAN_TARGET")" || SCAN_FAILED=1
key_files="$(secret_scan_key_files "$SCAN_TARGET")" || SCAN_FAILED=1
raw_files="$(secret_scan_raw_key_files "$SCAN_TARGET")" || SCAN_FAILED=1
if [ "$SCAN_FAILED" -ne 0 ]; then
  die "安全检查失败：扫描出错，拒绝给出没有私钥的结论（见上方 [release] ERROR 诊断）"
fi
if [ -n "$content_files" ]; then
  printf '%s\n' "$content_files" >&2
  die "制品里发现了疑似私钥/口令内容（PEM / seed-hex / secret 行）"
fi
if [ -n "$key_files" ]; then
  printf '%s\n' "$key_files" >&2
  die "制品里出现了 .key / secrets.env / .bpcert 文件"
fi
if [ -n "$raw_files" ]; then
  printf '%s\n' "$raw_files" >&2
  die "制品里出现了 32 字节裸密钥形状的文件"
fi
log "  安全检查：0 命中（内容规则 + 敏感文件名 + 32 字节形状，规则见 lib/common.sh）"
log "  自查完成：$checked 件制品"
