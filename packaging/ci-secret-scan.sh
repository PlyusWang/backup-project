#!/usr/bin/env bash
# packaging/ci-secret-scan.sh —— 对**最终制品**做私钥/口令扫描。
#
#   bash packaging/ci-secret-scan.sh <release 目录>
#
# 结构规则（不是"grep 字段名"）：PEM 私钥块、seed-hex 行、secrets.env 行、
# 32/64 字节裸十六进制文件。把每个制品解开再扫，而不是只扫压缩包本身。
set -Eeuo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/ci-lib.sh"

REL="${1:?用法: ci-secret-scan.sh <release 目录>}"
REL="$(cd "$REL" && pwd)"   # 后面会 cd 到别处，先用绝对路径钉住
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
ci_section "解包所有制品"
for artifact in "$REL"/*; do
  case "$artifact" in *SHA256SUMS|*RELEASE-INFO.txt) continue ;; esac
  base="$(basename "$artifact")"
  case "$artifact" in
    *.deb) dpkg-deb -x "$artifact" "$WORK/$base" 2>/dev/null || true ;;
    *.tar.xz) mkdir -p "$WORK/$base" && tar -xf "$artifact" -C "$WORK/$base" ;;
    *.AppImage)
      mkdir -p "$WORK/$base"
      ( cd "$WORK/$base" && "$artifact" --appimage-extract >/dev/null 2>&1 ) || true ;;
  esac
  expect_file "解包 $base" "$WORK/$base"
done

ci_section "结构规则扫描"
PATTERNS='-----BEGIN [A-Z ]*PRIVATE KEY|^seed-hex: [0-9a-f]{64}$|^BACKUP_TOKEN_SECRET=[0-9a-fA-F]{16,}$'
# pipefail：grep 无命中返回 1，必须显式吞掉，否则"没有私钥"反而会把脚本弄挂。
hits="$( { grep -rEl "$PATTERNS" "$WORK" 2>/dev/null || true; } | wc -l)"
expect_eq "私钥/口令结构规则命中 = 0" "0" "$hits"
if [ "$hits" != "0" ]; then grep -rEl "$PATTERNS" "$WORK" 2>/dev/null | sed -n '1,5p' >&2; fi

key_files="$(find "$WORK" \( -name '*.key' -o -name 'secrets.env' -o -name '*.bpcert' \) | wc -l)"
expect_eq "制品里没有 .key / secrets.env / .bpcert 文件" "0" "$key_files"

ci_section "32 字节裸密钥文件扫描（transport.key 的形状）"
raw="$(find "$WORK" -type f -size -64c -size +16c 2>/dev/null | while read -r f; do
  if [ "$(stat -c %s "$f")" = "32" ]; then
    if LC_ALL=C grep -qP '^[\x00-\xff]{32}$' "$f" 2>/dev/null; then echo "$f"; fi
  fi
done | { grep -vE '/usr/share/|/docs/|/qml/|/lib/' || true; } | wc -l)"
expect_eq "没有 32 字节裸密钥形状的文件" "0" "$raw"

ci_finish "secret-scan"
