#!/usr/bin/env bash
#
# cert_tool_test.sh —— backup-cert-tool + TrustedRootStore 的端到端测试（PR #23）。
#
#   bash scripts/cert_tool_test.sh
#
# 覆盖：
#   1. root-init：私钥 0600、位置在仓库之外、拒绝覆盖、输出里没有私钥内容；
#   2. root-info：只输出公钥材料；
#   3. issue-server：为一把**真实生成的** X25519 服务器公钥签发证书；
#   4. inspect-server：字段与布局；
#   5. verify-server：可信根通过；空存储 / 陌生根 / 内置官方根 -> 一律拒绝；
#      变异、截断、过期、换根公钥 -> 各自具名失败；
#   6. 私钥命令行入口不存在（--root-key-hex 必须被拒）；
#   7. 内置官方根常量与 resources/security/official-root-ed25519.pub 一致；
#   8. 工具全部输出里都不含私钥内容（可核对的硬证据）。
#
# 退出码：0 = 全部通过。

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

WORK_DIR="$(mktemp -d /tmp/cert-tool-test-XXXXXX)"
trap 'rm -rf "$WORK_DIR"' EXIT

PASS=0
FAIL=0
record_pass() { PASS=$((PASS + 1)); echo "  PASS  $1"; }
record_fail() { FAIL=$((FAIL + 1)); echo "  FAIL  $1 -- $2" >&2; }

check() {
  local label="$1"
  shift
  if "$@" >/dev/null 2>&1; then
    record_pass "$label"
  else
    record_fail "$label" "断言失败"
  fi
}

strlen() { printf %s "$1" | wc -c; }

# ---- 0. 构建 ----
if ! make server cert-tool > "$WORK_DIR/build.log" 2>&1; then
  record_fail "构建 server + cert-tool" "$(tail -5 "$WORK_DIR/build.log" | tr '\n' ' ')"
  echo "[cert-tool] passed=$PASS failed=$FAIL"
  exit 1
fi
TOOL="$ROOT_DIR/build/backup-cert-tool"
KEYGEN="$ROOT_DIR/build/backup-server-keygen"
[ -x "$TOOL" ] || { record_fail "找不到 backup-cert-tool" "$TOOL"; echo "[cert-tool] passed=$PASS failed=$FAIL"; exit 1; }
record_pass "构建 server + cert-tool（含 backup-server-keygen）"

ALL_OUTPUT="$WORK_DIR/all-output.txt"
: > "$ALL_OUTPUT"
run_capture() {
  local out="$1"
  shift
  "$@" > "$out" 2>&1
  local code=$?
  cat "$out" >> "$ALL_OUTPUT"
  return $code
}

# ---- 1. root-init ----
KEY="$WORK_DIR/root-a.key"
if run_capture "$WORK_DIR/root-init.out" "$TOOL" root-init --root-key "$KEY" --root-id test-root-a; then
  record_pass "root-init 退出码 0"
else
  record_fail "root-init 退出码 0" "$(tail -2 "$WORK_DIR/root-init.out" | tr '\n' ' ')"
fi
check "私钥文件权限是 0600" test "$(stat -c %a "$KEY")" = "600"
check "公钥文件已生成" test -s "$KEY.pub"
if grep -q "^test-root-a ed25519:[0-9a-f][0-9a-f]* unlimited$" "$KEY.pub"; then
  record_pass "公钥文件格式可直接当可信根用"
else
  record_fail "公钥文件格式可直接当可信根用" "$(cat "$KEY.pub")"
fi
if grep -q "content_printed    = NO" "$WORK_DIR/root-init.out"; then
  record_pass "root-init 报告 content_printed = NO"
else
  record_fail "root-init 报告 content_printed = NO" "缺少该行"
fi
BEFORE_HASH="$(sha256sum "$KEY" | cut -d' ' -f1)"
if run_capture "$WORK_DIR/root-init-again.out" "$TOOL" root-init --root-key "$KEY" --root-id test-root-b; then
  record_fail "root-init 拒绝覆盖已有私钥" "第二次竟然成功了"
else
  record_pass "root-init 拒绝覆盖已有私钥（退出码非 0）"
fi
check "拒绝覆盖时原私钥未被改动" test "$(sha256sum "$KEY" | cut -d' ' -f1)" = "$BEFORE_HASH"

# ---- 2. root-info ----
if run_capture "$WORK_DIR/root-info.out" "$TOOL" root-info --root-key "$KEY"; then
  record_pass "root-info 退出码 0"
else
  record_fail "root-info 退出码 0" "$(tail -2 "$WORK_DIR/root-info.out" | tr '\n' ' ')"
fi
PUBKEY_HEX="$(sed -n 's/^test-root-a ed25519:\([0-9a-f]*\).*$/\1/p' "$KEY.pub")"
if grep -q "root_public_key    = ed25519:$PUBKEY_HEX" "$WORK_DIR/root-info.out"; then
  record_pass "root-info 输出的公钥与 .pub 一致"
else
  record_fail "root-info 输出的公钥与 .pub 一致" "$(grep root_public_key "$WORK_DIR/root-info.out")"
fi
if grep -q "content_printed    = NO" "$WORK_DIR/root-info.out"; then
  record_pass "root-info 报告 content_printed = NO"
else
  record_fail "root-info 报告 content_printed = NO" "缺少该行"
fi

# ---- 3. issue-server（用真实生成的 X25519 服务器密钥）----
TRANSPORT="$WORK_DIR/transport.key"
if "$KEYGEN" --output "$TRANSPORT" > "$WORK_DIR/keygen.out" 2>&1; then
  record_pass "backup-server-keygen 生成服务器传输身份密钥"
else
  record_fail "backup-server-keygen 生成服务器传输身份密钥" "$(tail -2 "$WORK_DIR/keygen.out" | tr '\n' ' ')"
fi
SERVER_PUB_HEX="$("$KEYGEN" --show --key-file "$TRANSPORT" | sed -n 's/.*--server-key hex:\([0-9a-f]*\)$/\1/p' | head -1)"
check "取到服务器公钥（hex，64 字符）" test "$(strlen "$SERVER_PUB_HEX")" = "64"
CERT="$WORK_DIR/server.bpcert"
if run_capture "$WORK_DIR/issue.out" "$TOOL" issue-server --root-key "$KEY" \
      --server-id backup-project-cloud-production --server-pubkey "$SERVER_PUB_HEX" \
      --out "$CERT"; then
  record_pass "issue-server 签发成功"
else
  record_fail "issue-server 签发成功" "$(tail -2 "$WORK_DIR/issue.out" | tr '\n' ' ')"
fi
check "证书文件已生成" test -s "$CERT"
if grep -q "content_printed    = NO" "$WORK_DIR/issue.out"; then
  record_pass "issue-server 报告 content_printed = NO"
else
  record_fail "issue-server 报告 content_printed = NO" "缺少该行"
fi

# ---- 4. inspect-server ----
if run_capture "$WORK_DIR/inspect.out" "$TOOL" inspect-server --cert "$CERT"; then
  record_pass "inspect-server 退出码 0"
else
  record_fail "inspect-server 退出码 0" "$(tail -2 "$WORK_DIR/inspect.out" | tr '\n' ' ')"
fi
if grep -q "^format             = BPCERT1$" "$WORK_DIR/inspect.out"; then
  record_pass "inspect-server 认出 BPCERT1"
else
  record_fail "inspect-server 认出 BPCERT1" "无 format 行"
fi
if grep -q "^server_id          = backup-project-cloud-production$" "$WORK_DIR/inspect.out"; then
  record_pass "inspect-server 的 server_id 正确"
else
  record_fail "inspect-server 的 server_id 正确" "无 server_id 行"
fi
if grep -q "^validity_days      = 180.00$" "$WORK_DIR/inspect.out"; then
  record_pass "有效期恰好 180 天"
else
  record_fail "有效期恰好 180 天" "$(grep validity_days "$WORK_DIR/inspect.out")"
fi
if grep -q "^server_public_key  = $SERVER_PUB_HEX$" "$WORK_DIR/inspect.out"; then
  record_pass "证书里的服务器公钥与密钥文件一致"
else
  record_fail "证书里的服务器公钥与密钥文件一致" "$(grep server_public_key "$WORK_DIR/inspect.out")"
fi

# ---- 5. verify-server ----
if run_capture "$WORK_DIR/verify-ok.out" "$TOOL" verify-server --cert "$CERT" --roots "$KEY.pub"; then
  record_pass "verify-server 用自签根 -> 可信"
else
  record_fail "verify-server 用自签根 -> 可信" "$(tail -2 "$WORK_DIR/verify-ok.out" | tr '\n' ' ')"
fi
if grep -q "^verdict            = TRUSTED$" "$WORK_DIR/verify-ok.out"; then
  record_pass "verdict = TRUSTED"
else
  record_fail "verdict = TRUSTED" "无 verdict 行"
fi

: > "$WORK_DIR/empty.roots"
if run_capture "$WORK_DIR/verify-empty.out" "$TOOL" verify-server --cert "$CERT" --roots "$WORK_DIR/empty.roots"; then
  record_fail "空根存储必须拒绝一切" "竟然通过了"
else
  record_pass "空根存储必须拒绝一切"
fi
if grep -q "^trust_result       = untrusted-issuer$" "$WORK_DIR/verify-empty.out"; then
  record_pass "空存储的原因是 untrusted-issuer"
else
  record_fail "空存储的原因是 untrusted-issuer" "$(grep trust_result "$WORK_DIR/verify-empty.out")"
fi

if run_capture "$WORK_DIR/verify-official.out" "$TOOL" verify-server --cert "$CERT"; then
  record_fail "陌生根不能靠内置官方根通过" "竟然通过了"
else
  record_pass "陌生根不能靠内置官方根通过（fail closed）"
fi

if run_capture "$WORK_DIR/verify-wrongroot.out" "$TOOL" verify-server --cert "$CERT" \
      --issuer-pub ed25519:3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c; then
  record_fail "换一把根公钥必须失败" "竟然通过了"
else
  record_pass "换一把根公钥必须失败"
fi

python3 - "$CERT" "$WORK_DIR/mutated.bpcert" <<'PY'
import sys
raw = bytearray(open(sys.argv[1], 'rb').read())
raw[len(raw) // 2] ^= 0x20
open(sys.argv[2], 'wb').write(bytes(raw))
PY
if run_capture "$WORK_DIR/verify-mutated.out" "$TOOL" verify-server --cert "$WORK_DIR/mutated.bpcert" --roots "$KEY.pub"; then
  record_fail "变异证书必须被拒" "竟然通过了"
else
  record_pass "变异证书必须被拒"
fi

head -c 100 "$CERT" > "$WORK_DIR/truncated.bpcert"
if run_capture "$WORK_DIR/verify-truncated.out" "$TOOL" verify-server --cert "$WORK_DIR/truncated.bpcert" --roots "$KEY.pub"; then
  record_fail "截断证书必须被拒" "竟然通过了"
else
  record_pass "截断证书必须被拒"
fi
if grep -q "^parse_result       = truncated$" "$WORK_DIR/verify-truncated.out"; then
  record_pass "截断的原因是 truncated"
else
  record_fail "截断的原因是 truncated" "$(grep parse_result "$WORK_DIR/verify-truncated.out")"
fi

if run_capture "$WORK_DIR/verify-expired.out" "$TOOL" verify-server --cert "$CERT" --roots "$KEY.pub" --now 2000000000; then
  record_fail "过期证书必须被拒" "竟然通过了"
else
  record_pass "过期证书必须被拒"
fi

# ---- 6. 私钥命令行入口必须不存在 ----
if "$TOOL" issue-server --root-key-hex 00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff \
      --server-id x --server-pubkey "$SERVER_PUB_HEX" --out "$WORK_DIR/nope.bpcert" >/dev/null 2>&1; then
  record_fail "--root-key-hex 必须被拒绝" "竟然接受了"
else
  record_pass "--root-key-hex 必须被拒绝"
fi
if "$TOOL" root-info --seed-hex 00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff >/dev/null 2>&1; then
  record_fail "--seed-hex 必须被拒绝" "竟然接受了"
else
  record_pass "--seed-hex 必须被拒绝"
fi
check "被拒时没有生成任何证书文件" test ! -e "$WORK_DIR/nope.bpcert"

# ---- 7. 内置官方根常量与资源文件一致 ----
cat > "$WORK_DIR/rootdump.cpp" <<'CPP'
#include <cstdio>
#include "ed25519.h"
#include "trusted_root_store.h"
int main() {
  const backupproject::crypto::TrustedRootStore store =
      backupproject::crypto::TrustedRootStore::OfficialCloudStore();
  std::printf("size=%zu\n", store.size());
  for (const auto& root : store.roots()) {
    std::printf("root_id=%s\n", root.root_id.c_str());
    std::printf("root_public_key=%s\n",
                backupproject::crypto::Ed25519FormatPublicKeyHex(root.public_key).c_str());
  }
  return 0;
}
CPP
if g++ -std=c++17 -Wall -Wextra -Iinclude "$WORK_DIR/rootdump.cpp" \
      src/crypto/trusted_root_store.cpp src/crypto/bpcert.cpp src/crypto/ed25519.cpp \
      src/crypto/sha512.cpp src/crypto/sha256.cpp src/crypto/random.cpp \
      -o "$WORK_DIR/rootdump" > "$WORK_DIR/rootdump-build.log" 2>&1; then
  "$WORK_DIR/rootdump" > "$WORK_DIR/rootdump.out" 2>&1
  cat "$WORK_DIR/rootdump.out" >> "$ALL_OUTPUT"
  BUILTIN_ID="$(sed -n 's/^root_id=//p' "$WORK_DIR/rootdump.out")"
  BUILTIN_KEY="$(sed -n 's/^root_public_key=//p' "$WORK_DIR/rootdump.out")"
  FILE_LINE="$(grep -v '^#' resources/security/official-root-ed25519.pub | grep -v '^$' | head -1)"
  FILE_ID="$(printf %s "$FILE_LINE" | cut -d' ' -f1)"
  FILE_KEY="$(printf %s "$FILE_LINE" | awk '{print $2}')"
  check "内置官方根只有一把" test "$(sed -n 's/^size=//p' "$WORK_DIR/rootdump.out")" = "1"
  check "内置根标识与资源文件一致" test "$BUILTIN_ID" = "$FILE_ID"
  check "内置根公钥与资源文件一致" test "$BUILTIN_KEY" = "$FILE_KEY"
else
  record_fail "编译内置根检查程序" "$(tail -3 "$WORK_DIR/rootdump-build.log" | tr '\n' ' ')"
fi

# ---- 7b. 独立红队发现的四类问题的回归测试 ----
# (a) 用法错误不得回显参数原值：--root-key-hex=<种子> 被整串回显，就等于把
#     私钥写进了 stderr 与日志（这是红队唯一抓到的、对"私钥永不进日志"的
#     字面反例）。这里用单 token 形式 + 两个位置各试一次。
DUMMY_HEX="00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"
"$TOOL" root-info "--root-key-hex=$DUMMY_HEX" > "$WORK_DIR/echo1.out" 2>&1
"$TOOL" "--root-key-hex=$DUMMY_HEX" > "$WORK_DIR/echo2.out" 2>&1
if grep -q -F "$DUMMY_HEX" "$WORK_DIR/echo1.out" "$WORK_DIR/echo2.out"; then
  record_fail "用法错误不回显参数原值" "种子被回显进了 stderr"
else
  record_pass "用法错误不回显参数原值（--root-key-hex=<值> 单 token 也不回显）"
fi

# (b) 根文件的时间戳必须严格是整数，且"不限期"必须显式写 unlimited：
#     红队用 "not-a-number" 一个笔误就把"根 2033 年过期"变成了"永久有效"。
KEY_SPEC="ed25519:$PUBKEY_HEX"
printf 'test-root-a %s 1700000000 2000000000\n' "$KEY_SPEC" > "$WORK_DIR/root-window.roots"
printf 'test-root-a %s 1700000000 not-a-number\n' "$KEY_SPEC" > "$WORK_DIR/root-typo.roots"
printf 'test-root-a %s 1700000000\n' "$KEY_SPEC" > "$WORK_DIR/root-one.roots"
printf 'test-root-a %s unlimited\n' "$KEY_SPEC" > "$WORK_DIR/root-unlimited.roots"
if run_capture "$WORK_DIR/verify-window.out" "$TOOL" verify-server --cert "$CERT" --roots "$WORK_DIR/root-window.roots"; then
  record_pass "根文件写两个整数 -> 正常参与信任判断"
else
  record_fail "根文件写两个整数 -> 正常参与信任判断" "$(tail -2 "$WORK_DIR/verify-window.out" | tr '\n' ' ')"
fi
if "$TOOL" verify-server --cert "$CERT" --roots "$WORK_DIR/root-typo.roots" > /dev/null 2>&1; then
  record_fail "根文件时间戳写错必须直接报错" "竟然被当成不限期接受了"
else
  record_pass "根文件时间戳写错必须直接报错（不再静默变成不限期）"
fi
if "$TOOL" verify-server --cert "$CERT" --roots "$WORK_DIR/root-one.roots" > /dev/null 2>&1; then
  record_fail "根文件只写一个时间戳必须报错" "竟然被当成不限期接受了"
else
  record_pass "根文件只写一个时间戳必须报错（不限期要显式写 unlimited）"
fi
if run_capture "$WORK_DIR/verify-unlimited.out" "$TOOL" verify-server --cert "$CERT" --roots "$WORK_DIR/root-unlimited.roots"; then
  record_pass "根文件显式写 unlimited -> 接受"
else
  record_fail "根文件显式写 unlimited -> 接受" "$(tail -2 "$WORK_DIR/verify-unlimited.out" | tr '\n' ' ')"
fi

# (c) 内联的 32 个十六进制字符不许被当成 32 字节 ASCII 原样签进证书
if "$TOOL" issue-server --root-key "$KEY" --server-id x \
      --server-pubkey abababababababababababababababab --out "$WORK_DIR/ambig.bpcert" > /dev/null 2>&1; then
  record_fail "32 字符的内联公钥必须被拒" "被当成 ASCII 原始字节接受并签进了证书"
else
  record_pass "32 字符的内联公钥必须被拒（消除同一输入两种读法）"
fi

# (d) 硬链接目标必须拒绝（O_NOFOLLOW 只挡符号链接）
printf 'original-content\n' > "$WORK_DIR/hardlink-target"
ln -f "$WORK_DIR/hardlink-target" "$WORK_DIR/hardlink-out"
if "$TOOL" issue-server --root-key "$KEY" --server-id x \
      --server-pubkey "$SERVER_PUB_HEX" --out "$WORK_DIR/hardlink-out" > /dev/null 2>&1; then
  record_fail "硬链接目标必须拒绝" "竟然写了进去"
else
  record_pass "硬链接目标必须拒绝"
fi
check "拒绝硬链接时原文件未被改动" grep -q "^original-content$" "$WORK_DIR/hardlink-target"

# ---- 8. 全部输出里都不含私钥内容 ----
SEED="$(sed -n 's/^seed-hex: //p' "$KEY" | tr -d '[:space:]')"
if [ "$(strlen "$SEED")" = "64" ] && ! grep -q -F "$SEED" "$ALL_OUTPUT"; then
  record_pass "工具全部输出里都不含根私钥内容（全部输出合并后检索）"
else
  record_fail "工具全部输出里都不含根私钥内容" "私钥长度异常，或私钥出现在输出里"
fi

echo "[cert-tool] passed=$PASS failed=$FAIL"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
