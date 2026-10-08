#!/usr/bin/env bash
# packaging/ci-secret-scan.sh —— 对**最终制品**做私钥/口令扫描。
#
#   bash packaging/ci-secret-scan.sh <release 目录>
#
# 结构规则（不是"grep 字段名"）：PEM 私钥块、seed-hex 行、secrets.env 行、
# 32/64 字节裸十六进制文件。把每个制品解开再扫，而不是只扫压缩包本身。
#
# fail-closed：**解包失败 = 扫描失败**。以前这里是 "mkdir -p 目标目录" +
# "--appimage-extract ... || true"，再用"目录存在"当成功证据 —— 解包失败时扫描
# 会退化成"扫一个空目录"，照样报 0 命中。现在解包走 ci_unpack（见 ci-lib.sh）：
# 命令非零即失败，而且必须核对解出来的真实内容。
set -Eeuo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/ci-lib.sh"

REL="${1:?用法: ci-secret-scan.sh <release 目录>}"
REL="$(cd "$REL" && pwd)"   # 后面会 cd 到别处，先用绝对路径钉住
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

ci_section "解包所有制品（解包失败即失败）"
UNPACK_FAILED=0
for artifact in "$REL"/*; do
  [ -e "$artifact" ] || continue
  case "$artifact" in *SHA256SUMS|*RELEASE-INFO.txt) continue ;; esac
  base="$(basename "$artifact")"
  case "$artifact" in
    backup-project-client_*.deb)
      ci_unpack "解包 $base" "$artifact" "$WORK/$base" \
        "usr/bin/backupctl" || UNPACK_FAILED=1 ;;
    backup-project-server_*.deb)
      ci_unpack "解包 $base" "$artifact" "$WORK/$base" \
        "usr/bin/backup-project-server" || UNPACK_FAILED=1 ;;
    *.deb)
      ci_unpack "解包 $base" "$artifact" "$WORK/$base" "usr" || UNPACK_FAILED=1 ;;
    backup-project-client-*.tar.xz)
      ci_unpack "解包 $base" "$artifact" "$WORK/$base" \
        "glob:*/bin/backupctl" "glob:*/bin/backup-gui-modern" || UNPACK_FAILED=1 ;;
    backup-project-server-*.tar.xz)
      ci_unpack "解包 $base" "$artifact" "$WORK/$base" \
        "glob:*/bin/backup-server" "glob:*/bin/backup-server-keygen" || UNPACK_FAILED=1 ;;
    *.tar.xz)
      ci_unpack "解包 $base" "$artifact" "$WORK/$base" "glob:*/bin/*" || UNPACK_FAILED=1 ;;
    *.AppImage)
      ci_unpack "解包 $base" "$artifact" "$WORK/$base" \
        "squashfs-root/usr/bin/backupctl" \
        "squashfs-root/usr/bin/backup-gui-modern" \
        "squashfs-root/usr/share/backup-project/VERSION" || UNPACK_FAILED=1 ;;
    *)
      ci_fail "不认识的发行文件（不允许跳过扫描）：$base"
      UNPACK_FAILED=1 ;;
  esac
done

if [ "$UNPACK_FAILED" -ne 0 ]; then
  # 有任何一件没解出来，就不允许给出"没有私钥"的结论：那正是假通过的来源。
  ci_fail "解包不完整：拒绝报告扫描结果（先修解包，再谈扫描）"
else
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
fi

ci_finish "secret-scan"
