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

# portable tar 的顶层目录名两端不一致（客户端大写 / 服务端小写），所以用 find 定位，
# 但要求**恰好一个**，并且必须落在本次解包的目录里。
resolve_tar_file() {  # $1 = 解包目录, $2 = 文件名
  local dest="$1" name="$2" found
  # 必须**递归**找：portable tar 里 BUILD-INFO.txt 在树根（深度 2），而 VERSION 在
  # <top>/share/backup-project/VERSION（深度 4）—— 上界写死 maxdepth 会在不同产品族
  # 上各漏一次。这里用"恰好一个 + 必须落在解包目录内"两条约束兜住，不靠深度。
  found="$(find "$dest" -name "$name" -print 2>/dev/null | LC_ALL=C sort)"
  [ -n "$found" ] || { printf '%s' ''; return; }
  [ "$(printf '%s\n' "$found" | wc -l)" = "1" ] || { printf '%s' 'AMBIGUOUS'; return; }
  case "$found" in "$dest"/*) printf '%s' "$found" ;; *) printf '%s' 'OUTSIDE' ;; esac
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

  # ---- BUILD-INFO / VERSION 契约 ----
  rel="$(build_info_rel "$base")"
  [ -n "$rel" ] || die "不认识的发行文件形状（无法核对 BUILD-INFO）：$base"
  info=""
  case "$base" in
    *.tar.xz) info="$(resolve_tar_file "$dest" 'BUILD-INFO.txt')" ;;
    *)        info="$dest/$rel" ;;
  esac
  case "$info" in
    '') die "$base 里找不到 BUILD-INFO.txt（按产品族路径 $rel 也没找到）" ;;
    AMBIGUOUS) die "$base 里有多个 BUILD-INFO.txt" ;;
    OUTSIDE) die "$base 的 BUILD-INFO.txt 不在解包目录内" ;;
  esac
  [ -f "$info" ] || die "$base 的 BUILD-INFO.txt 不是普通文件：$info"
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

  vrel="$(version_rel "$base")"
  case "$base" in
    *.tar.xz) vfile="$(resolve_tar_file "$dest" 'VERSION')" ;;
    *)        vfile="$dest/$vrel" ;;
  esac
  case "$vfile" in
    ''|AMBIGUOUS|OUTSIDE) die "$base 里找不到 share/backup-project/VERSION（$vrel）" ;;
  esac
  [ -f "$vfile" ] || die "$base 的 VERSION 不是普通文件：$vfile"
  if [ -n "$EXPECT_VERSION" ]; then
    got_v="$(head -1 "$vfile")"
    [ "$got_v" = "$EXPECT_VERSION" ] \
      || die "$base 的 VERSION=$got_v，期望 $EXPECT_VERSION"
  fi
  log "  $base：BUILD-INFO/VERSION 契约通过（version=${EXPECT_VERSION:-未指定}）"
done
[ "$checked" -gt 0 ] || die "发行目录里没有任何制品：$REL"

# ---- 统一的安全检查（与 ci-secret-scan.sh 同一份规则）----
content_files="$(secret_scan_content_files "$SCAN_TARGET")"
if [ -n "$content_files" ]; then
  printf '%s\n' "$content_files" >&2
  die "制品里发现了疑似私钥/口令内容（PEM / seed-hex / secret 行）"
fi
key_files="$(secret_scan_key_files "$SCAN_TARGET")"
if [ -n "$key_files" ]; then
  printf '%s\n' "$key_files" >&2
  die "制品里出现了 .key / secrets.env / .bpcert 文件"
fi
raw_files="$(secret_scan_raw_key_files "$SCAN_TARGET")"
if [ -n "$raw_files" ]; then
  printf '%s\n' "$raw_files" >&2
  die "制品里出现了 32 字节裸密钥形状的文件"
fi
log "  安全检查：0 命中（内容规则 + 敏感文件名 + 32 字节形状，规则见 lib/common.sh）"
log "  自查完成：$checked 件制品"
