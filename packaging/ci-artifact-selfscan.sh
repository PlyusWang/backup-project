#!/usr/bin/env bash
# packaging/ci-artifact-selfscan.sh —— 对**最终制品**做解包 + 私钥/口令自查。
#
#   bash packaging/ci-artifact-selfscan.sh <release 目录>
#
# 这是构建期自查（build-release.sh 打完包立刻调用），与 CI 的 ci-secret-scan.sh
# 用**同一套判据**。之所以抽成一个脚本：v0.1.1 的第一次发行 CI 撞过一次
# "两处期望值各自漂移" —— 构建期那处把客户端 tar 才有的 bin/backupctl 写成了对
# **所有** tar 的要求，服务端 tar 因此被误判成"解包结果异常"。现在解包与核对只有
# 一份实现，而且核对项**与产品族无关**（tar 只要求"顶层目录下有 bin/"）。
set -Eeuo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/lib/common.sh"

REL="${1:?用法: ci-artifact-selfscan.sh <release 目录>}"
REL="$(cd "$REL" && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

unpack_one() {  # $1 = 制品路径, $2 = 目标目录
  local artifact="$1" dest="$2" base
  base="$(basename "$artifact")"
  rm -rf "$dest"; mkdir -p "$dest"
  case "$artifact" in
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
      die "不认识的发行文件（不允许跳过扫描）：$base" ;;
  esac
}

log "== 制品自查：解包 + 私钥扫描 =="
SCAN_TARGET="$WORK/scan"
mkdir -p "$SCAN_TARGET"
for artifact in "$REL"/*; do
  [ -e "$artifact" ] || continue
  case "$artifact" in *SHA256SUMS|*RELEASE-INFO.txt) continue ;; esac
  unpack_one "$artifact" "$SCAN_TARGET/$(basename "$artifact")"
done
hits="$(scan_for_secrets "$SCAN_TARGET")"
if [ "$hits" != "0" ]; then
  die "制品里发现了疑似私钥/口令内容（$hits 处）"
fi
log "  私钥扫描：0 命中"
if [ -n "$(find "$SCAN_TARGET" \( -name '*.key' -o -name 'secrets.env' \) -print -quit)" ]; then
  die "制品里出现了 .key / secrets.env 文件"
fi
log "  文件级检查：无 .key / secrets.env"
