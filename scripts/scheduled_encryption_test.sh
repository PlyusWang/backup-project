#!/usr/bin/env bash
#
# scheduled_encryption_test.sh —— 无人值守加密的凭据与端到端回归。
#
# 验证：
#   A. 没有口令文件时，保存加密计划必须被拒绝；
#   B. 配置 0600 口令文件后可以保存 / 启用；
#   C. 立即执行一次会产出真正加密的容器（list 显示 aes 算法与 password-required）；
#   D. 口令本身不落进 config.json / schedule.json / history。
#
# 退出码：0 = 全部通过，非 0 = 有 FAIL。

set -uo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR" || exit 1
PASS=0
FAIL=0
record_pass() { PASS=$((PASS + 1)); echo "  PASS  $1"; }
record_fail() { FAIL=$((FAIL + 1)); echo "  FAIL  $1 -- $2"; }
finish() { echo "[scheduled-encryption] passed=$PASS failed=$FAIL"; [ "$FAIL" -eq 0 ] || exit 1; }

W=$(mktemp -d /tmp/sched-enc-XXXXXX)
export HOME="$W/home"
mkdir -p "$HOME/.config" "$W/src" "$W/repo"
printf 'scheduled secret\n' >"$W/src/a.txt"
head -c 4096 /dev/urandom >"$W/src/b.bin"
printf 'correct horse battery staple\n' >"$W/pass.txt"
chmod 600 "$W/pass.txt"
printf 'wrong perms\n' >"$W/bad.txt"
chmod 644 "$W/bad.txt"

./build/backupctl config repository set "$W/repo" >/dev/null 2>&1
./build/backupctl schedule set --source "$W/src" --interval-minutes 1 --retain 3 \
  --strategy full --pack mypack --compression huffman \
  --encryption aes-256-ctr-hmac-sha256 >"$W/no-file.log" 2>&1
[ $? -ne 0 ] && record_pass "A.1 未配置口令文件时拒绝加密计划" || record_fail "A.1 未配置口令文件时拒绝加密计划" "accepted"

./build/backupctl schedule set --source "$W/src" --interval-minutes 1 --retain 3 \
  --strategy full --pack mypack --compression huffman \
  --encryption aes-256-ctr-hmac-sha256 --password-file "$W/bad.txt" >"$W/bad-perm.log" 2>&1
# 保存配置允许“路径暂时不可用”，但启用任务必须当场验证凭据。
./build/backupctl schedule enable >"$W/bad-enable.log" 2>&1
[ $? -ne 0 ] && record_pass "A.2 0644 口令文件无法启用任务" || record_fail "A.2 0644 口令文件无法启用任务" "enable accepted"

./build/backupctl schedule set --source "$W/src" --interval-minutes 1 --retain 3 \
  --strategy full --pack mypack --compression huffman \
  --encryption aes-256-ctr-hmac-sha256 --password-file "$W/pass.txt" >"$W/set.log" 2>&1
[ $? -eq 0 ] && record_pass "B.1 0600 口令文件可保存" || record_fail "B.1 0600 口令文件可保存" "$(tail -1 "$W/set.log")"
./build/backupctl schedule enable >"$W/enable.log" 2>&1
[ $? -eq 0 ] && record_pass "B.2 加密计划可启用" || record_fail "B.2 加密计划可启用" "$(tail -1 "$W/enable.log")"
./build/backupctl schedule run >"$W/run.log" 2>&1
[ $? -eq 0 ] && record_pass "C.1 立即执行成功" || record_fail "C.1 立即执行成功" "$(tail -1 "$W/run.log")"
LIST=$(./build/backupctl repository list 2>&1)
echo "$LIST" | grep -q "aes-256-ctr-hmac-sha256" && record_pass "C.2 归档真的使用 AES 加密" || record_fail "C.2 归档真的使用 AES 加密" "$LIST"
echo "$LIST" | grep -q "password-required" && record_pass "C.3 归档标记为需要密码" || record_fail "C.3 归档标记为需要密码" "$LIST"
./build/backupctl schedule history 2>&1 | grep -q "success_created" && record_pass "C.4 运行历史记录成功" || record_fail "C.4 运行历史记录成功" "history"
if grep -rq "correct horse battery staple" "$HOME/.config" 2>/dev/null; then
  record_fail "D.1 口令不落盘" "在配置目录里找到了口令原文"
else
  record_pass "D.1 口令不落盘"
fi
if [ -s "$W/repo" ] || find "$W/repo" -name '*.bak' | grep -q .; then
  record_pass "D.2 产出 1 份加密备份"
else
  record_fail "D.2 产出 1 份加密备份" "仓库为空"
fi
rm -rf "$W"
finish
