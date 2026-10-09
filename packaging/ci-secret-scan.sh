#!/usr/bin/env bash
# packaging/ci-secret-scan.sh —— 对**最终制品**做私钥/口令扫描（CI 入口）。
#
#   bash packaging/ci-secret-scan.sh <release 目录>
#
# 规则本身只有一份：packaging/lib/common.sh 里的 secret_scan_* 三个函数
# （内容规则 / 敏感文件名 / 32 字节原始密钥形状）。构建期入口
# packaging/ci-artifact-selfscan.sh 调用的也是同一份实现，两个入口不允许再各写一套。
#
# fail-closed：**解包失败 = 扫描失败**。解包走 ci_unpack（见 ci-lib.sh）：命令非零
# 即失败，而且必须核对解出来的真实内容；解包不完整时不会给出"没有私钥"的结论。
set -Eeuo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/ci-lib.sh"
source "$(dirname "${BASH_SOURCE[0]}")/lib/common.sh"

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

# 行数统计：空行不算。用 awk 而不是 grep -c —— grep 在"0 行"时退出码是 1，
# 旧代码只能靠 || true 压住，那正是本轮要收敛的"吞掉错误状态"写法（第3轮 P2-01）。
count_hits() { awk 'NF { n++ } END { print n + 0 }'; }

if [ "$UNPACK_FAILED" -ne 0 ]; then
  # 有任何一件没解出来，就不允许给出"没有私钥"的结论：那正是假通过的来源。
  ci_fail "解包不完整：拒绝报告扫描结果（先修解包，再谈扫描）"
else
  ci_section "结构规则扫描（规则见 packaging/lib/common.sh）"
  # 三个扫描函数用 0/2 区分"扫完了"（可能 0 命中）与"扫描本身出错"（见 common.sh
  # 的 P2-01 段）。出错时**不给结论**、只记一条 FAIL：把"没扫成"说成"没有私钥"，
  # 正是这一轮要消灭的假通过。
  SCAN_FAILED=0
  content_files="$(secret_scan_content_files "$WORK")" || SCAN_FAILED=1
  key_files="$(secret_scan_key_files "$WORK")" || SCAN_FAILED=1
  raw_files="$(secret_scan_raw_key_files "$WORK")" || SCAN_FAILED=1
  if [ "$SCAN_FAILED" -ne 0 ]; then
    ci_fail "安全检查没有跑完：扫描出错，拒绝报告扫描结果（见上方 [release] ERROR 诊断）"
  else
    hits="$(printf '%s' "$content_files" | count_hits)"
    expect_eq "私钥/口令结构规则命中 = 0" "0" "$hits"
    if [ "$hits" != "0" ]; then printf '%s\n' "$content_files" | sed -n '1,5p' >&2; fi

    key_count="$(printf '%s' "$key_files" | count_hits)"
    expect_eq "制品里没有 .key / secrets.env / .bpcert 文件" "0" "$key_count"

    ci_section "32 字节裸密钥形状扫描（transport.key 的形状；排除目录见 common.sh）"
    raw_count="$(printf '%s' "$raw_files" | count_hits)"
    expect_eq "没有 32 字节裸密钥形状的文件" "0" "$raw_count"
  fi
fi

ci_finish "secret-scan"
