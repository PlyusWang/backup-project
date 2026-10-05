#!/usr/bin/env bash
#
# ed25519_test.sh —— 手写 Ed25519 的单元测试入口（PR #23）。
#
#   bash scripts/ed25519_test.sh
#
# 三层：
#   A. 编译 tests/unit/ed25519_test.cpp + src/crypto/ed25519.cpp + sha512/sha256，
#      跑 RFC 8032 官方向量、往返、变异、畸形输入（每一条都要求具名的失败原因）；
#   B. **独立 oracle 交叉验证**：随机密钥/消息，与本机两个**互相独立**的实现
#      逐条比对 —— python cryptography（走 OpenSSL）与 PyNaCl（libsodium）：
#        * 公钥导出一致
#        * 本实现签名 -> 两个 oracle 都验签通过
#        * 两个 oracle 签名 -> 本实现验签通过
#        * 变异后的签名 -> 本实现必须拒绝
#   C. 与 OpenSSL CLI 再对一次（第三个实现）。
#
# 这些 oracle 只出现在测试脚本里，绝不进入产品运行路径。
#
# 环境变量 ED25519_TEST_EXTRA_FLAGS 可以追加编译参数（例如 sanitizer），
# ED25519_ORACLE_CASES 可以改随机用例数（默认 300）。
# 退出码：0 = 全部通过。

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

set +u
EXTRA="$ED25519_TEST_EXTRA_FLAGS"
CASES="$ED25519_ORACLE_CASES"
set -u
[ -n "$CASES" ] || CASES=300

WORK_DIR="$(mktemp -d /tmp/ed25519-test-XXXXXX)"
trap 'rm -rf "$WORK_DIR"' EXIT

BUILD_LOG="$WORK_DIR/build.log"
PASS=0
FAIL=0
record_pass() { PASS=$((PASS + 1)); echo "  PASS  $1"; }
record_fail() { FAIL=$((FAIL + 1)); echo "  FAIL  $1 -- $2" >&2; }

# ---- A. 单元测试 ----
# shellcheck disable=SC2086
if ! g++ -std=c++17 -Wall -Wextra -Wpedantic -Iinclude $EXTRA \
      tests/unit/ed25519_test.cpp src/crypto/ed25519.cpp src/crypto/sha512.cpp \
      src/crypto/sha256.cpp src/crypto/random.cpp \
      -o "$WORK_DIR/ed25519_test" >"$BUILD_LOG" 2>&1; then
  record_fail "编译 ed25519_test" "$(tail -8 "$BUILD_LOG" | tr '\n' ' ')"
  echo "[ed25519] passed=$PASS failed=$FAIL"
  exit 1
fi
if [ -s "$BUILD_LOG" ]; then
  record_fail "编译必须零警告" "$(head -5 "$BUILD_LOG" | tr '\n' ' ')"
else
  record_pass "零警告编译（-Wall -Wextra -Wpedantic）"
fi

if "$WORK_DIR/ed25519_test" > "$WORK_DIR/run.log" 2>&1; then
  record_pass "RFC 8032 向量 / 往返 / 变异 / 畸形（$(grep -oE 'passed=[0-9]+ failed=[0-9]+' "$WORK_DIR/run.log" | tail -1)）"
else
  record_fail "ed25519_test 退出码" "$(grep -m5 FAIL "$WORK_DIR/run.log" | tr '\n' ' ')"
fi
if grep -qE 'ERROR: (AddressSanitizer|LeakSanitizer|UndefinedBehaviorSanitizer)|runtime error:' "$WORK_DIR/run.log"; then
  record_fail "消毒剂报告" "$(grep -m2 -E 'ERROR|runtime error' "$WORK_DIR/run.log" | tr '\n' ' ')"
else
  record_pass "消毒剂报告 0 条"
fi

# ---- 供 oracle 使用的小 driver ----
cat > "$WORK_DIR/driver.cpp" <<'CPP'
#include <cstdio>
#include <cstring>
#include <string>
#include "crypto.h"
#include "ed25519.h"
static std::string Unhex(const char* h) {
  std::string out;
  if (!backupproject::crypto::FromHex(h, &out)) { std::exit(2); }
  return out;
}
// 消息最长覆盖到 70 KB，十六进制就是 140 KB —— 走 argv 会直接撞 E2BIG
// （Argument list too long），所以消息一律从 stdin 读。
static std::string ReadStdinHex() {
  std::string hex;
  char buf[4096];
  std::size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof(buf), stdin)) > 0) {
    hex.append(buf, n);
  }
  while (!hex.empty() &&
         (hex.back() == '\n' || hex.back() == '\r' || hex.back() == ' ')) {
    hex.pop_back();
  }
  std::string out;
  if (!backupproject::crypto::FromHex(hex, &out)) { std::exit(2); }
  return out;
}
int main(int argc, char** argv) {
  if (argc < 3) return 2;
  const std::string cmd = argv[1];
  std::string error;
  if (cmd == "pub") {
    std::string pk;
    if (!backupproject::crypto::Ed25519PublicKeyFromSeed(Unhex(argv[2]), &pk, &error)) {
      std::fprintf(stderr, "%s\n", error.c_str());
      return 1;
    }
    std::printf("%s\n", backupproject::crypto::ToHex(
        reinterpret_cast<const unsigned char*>(pk.data()), pk.size()).c_str());
    return 0;
  }
  if (cmd == "sign") {
    const std::string seed = Unhex(argv[2]);
    const std::string msg = ReadStdinHex();
    std::string sig;
    if (!backupproject::crypto::Ed25519Sign(seed, msg.data(), msg.size(), &sig, &error)) {
      std::fprintf(stderr, "%s\n", error.c_str());
      return 1;
    }
    std::printf("%s\n", backupproject::crypto::ToHex(
        reinterpret_cast<const unsigned char*>(sig.data()), sig.size()).c_str());
    return 0;
  }
  if (cmd == "verify") {
    const std::string pk = Unhex(argv[2]);
    const std::string msg = ReadStdinHex();
    const std::string sig = Unhex(argv[3]);
    const backupproject::crypto::Ed25519VerifyResult r =
        backupproject::crypto::Ed25519VerifyDetailed(pk, msg.data(), msg.size(), sig);
    std::printf("%s\n", backupproject::crypto::Ed25519VerifyResultName(r));
    return r == backupproject::crypto::Ed25519VerifyResult::kOk ? 0 : 1;
  }
  return 2;
}
CPP
# shellcheck disable=SC2086
if ! g++ -std=c++17 -Wall -Wextra -Iinclude $EXTRA \
      "$WORK_DIR/driver.cpp" src/crypto/ed25519.cpp src/crypto/sha512.cpp \
      src/crypto/sha256.cpp src/crypto/random.cpp \
      -o "$WORK_DIR/driver" >>"$BUILD_LOG" 2>&1; then
  record_fail "编译 oracle driver" "$(tail -5 "$BUILD_LOG" | tr '\n' ' ')"
  echo "[ed25519] passed=$PASS failed=$FAIL"
  exit 1
fi

# ---- B. 独立 oracle ----
if ! python3 -c "import cryptography, nacl.signing" >/dev/null 2>&1; then
  record_fail "独立 oracle" "缺少 python cryptography / PyNaCl"
else
  cat > "$WORK_DIR/oracle.py" <<'PY'
import binascii, os, subprocess, sys
from cryptography.hazmat.primitives.asymmetric.ed25519 import (
    Ed25519PrivateKey, Ed25519PublicKey)
from cryptography.hazmat.primitives import serialization
from nacl.signing import SigningKey, VerifyKey

driver, cases = sys.argv[1], int(sys.argv[2])
mismatch = 0
checked = 0

# 消息走 stdin：最长 70 KB（十六进制 140 KB）用 argv 传会撞 E2BIG。
def run(args, stdin_hex=""):
    p = subprocess.run([driver] + list(args), input=stdin_hex,
                       capture_output=True, text=True)
    return p.returncode, p.stdout.strip(), p.stderr.strip()

for i in range(cases):
    seed = os.urandom(32)
    # 长度覆盖：0、小、大
    if i % 5 == 0:
        msg = b""
    elif i % 5 == 1:
        msg = os.urandom(1 + i % 7)
    elif i % 5 == 2:
        msg = os.urandom(64)
    elif i % 5 == 3:
        msg = os.urandom(1000 + i % 500)
    else:
        msg = os.urandom(70000)
    seed_hex = binascii.hexlify(seed).decode()
    msg_hex = binascii.hexlify(msg).decode()

    # 1) 公钥导出：本实现 vs cryptography vs nacl
    rc, mine_pk, err = run(["pub", seed_hex])
    if rc != 0:
        print("driver pub failed:", err); mismatch += 1; continue
    sk = Ed25519PrivateKey.from_private_bytes(seed)
    ref_pk = sk.public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    nacl_pk = bytes(SigningKey(seed).verify_key)
    checked += 1
    if mine_pk != binascii.hexlify(ref_pk).decode() or ref_pk != nacl_pk:
        print("public key mismatch at case", i); mismatch += 1; continue

    # 2) 本实现签名 -> 两个 oracle 都验签
    rc, my_sig_hex, err = run(["sign", seed_hex], msg_hex)
    if rc != 0:
        print("driver sign failed:", err); mismatch += 1; continue
    my_sig = binascii.unhexlify(my_sig_hex)
    try:
        Ed25519PublicKey.from_public_bytes(ref_pk).verify(my_sig, msg)
    except Exception as exc:
        print("cryptography rejected our signature at case", i, exc)
        mismatch += 1; continue
    try:
        VerifyKey(ref_pk).verify(msg, my_sig)
    except Exception as exc:
        print("nacl rejected our signature at case", i, exc)
        mismatch += 1; continue

    # 3) oracle 签名 -> 本实现验签
    ref_sig = sk.sign(msg)
    rc, name, _ = run(["verify", binascii.hexlify(ref_pk).decode(),
                       binascii.hexlify(ref_sig).decode()], msg_hex)
    if rc != 0:
        print("we rejected oracle signature at case", i, name)
        mismatch += 1; continue
    nacl_sig = SigningKey(seed).sign(msg).signature
    rc, name, _ = run(["verify", binascii.hexlify(ref_pk).decode(),
                       binascii.hexlify(nacl_sig).decode()], msg_hex)
    if rc != 0:
        print("we rejected nacl signature at case", i, name)
        mismatch += 1; continue

    # 4) 变异必须被我们拒绝
    bad = bytearray(ref_sig)
    bad[i % 64] ^= 0x01
    rc, name, _ = run(["verify", binascii.hexlify(ref_pk).decode(),
                       binascii.hexlify(bytes(bad)).decode()], msg_hex)
    if rc == 0:
        print("mutated signature ACCEPTED at case", i); mismatch += 1; continue

print("ORACLE_CASES=%d" % checked)
print("ORACLE_MISMATCH=%d" % mismatch)
sys.exit(0 if mismatch == 0 else 1)
PY
  if python3 "$WORK_DIR/oracle.py" "$WORK_DIR/driver" "$CASES" > "$WORK_DIR/oracle.log" 2>&1; then
    record_pass "独立 oracle 双实现（cryptography + PyNaCl）$(grep -oE 'ORACLE_CASES=[0-9]+' "$WORK_DIR/oracle.log" | tail -1)，mismatch = 0"
  else
    record_fail "独立 oracle" "$(tail -4 "$WORK_DIR/oracle.log" | tr '\n' ' ')"
  fi
fi

# ---- C. OpenSSL CLI 作为第三个实现 ----
if ! command -v openssl >/dev/null 2>&1; then
  record_fail "OpenSSL oracle" "本机没有 openssl"
else
  OK=1
  for i in 1 2 3 4 5; do
    # 用固定序列生成种子与消息，避免依赖随机数工具
    SEED_HEX="$(printf '%064x' "$((i * 7919))")"
    MSG_HEX="$(printf '%032x' "$((i * 104729))")"
    printf '%s' "$SEED_HEX" | xxd -r -p > "$WORK_DIR/seed.bin"
    printf '%s' "$MSG_HEX" | xxd -r -p > "$WORK_DIR/msg.bin"
    # 由种子构造 PKCS#8 DER：302e020100300506032b657004220420 || seed
    printf '302e020100300506032b657004220420%s' "$SEED_HEX" | xxd -r -p > "$WORK_DIR/key.der"
    openssl pkey -inform DER -in "$WORK_DIR/key.der" -out "$WORK_DIR/key.pem" 2>/dev/null || { OK=0; break; }
    openssl pkey -in "$WORK_DIR/key.pem" -pubout -outform DER 2>/dev/null | tail -c 32 | xxd -p | tr -d '\n' > "$WORK_DIR/ossl.pub"
    MINE_PUB="$("$WORK_DIR/driver" pub "$SEED_HEX")"
    if [ "$MINE_PUB" != "$(cat "$WORK_DIR/ossl.pub")" ]; then
      echo "openssl 公钥不一致 case $i: $MINE_PUB vs $(cat "$WORK_DIR/ossl.pub")" >&2
      OK=0; break
    fi
    openssl pkeyutl -sign -rawin -inkey "$WORK_DIR/key.pem" -in "$WORK_DIR/msg.bin" -out "$WORK_DIR/ossl.sig" 2>/dev/null || { OK=0; break; }
    OSSL_SIG="$(xxd -p "$WORK_DIR/ossl.sig" | tr -d '\n')"
    if ! printf '%s' "$MSG_HEX" | "$WORK_DIR/driver" verify "$MINE_PUB" "$OSSL_SIG" >/dev/null; then
      echo "我们拒绝了 openssl 的签名 case $i" >&2
      OK=0; break
    fi
    MY_SIG="$(printf '%s' "$MSG_HEX" | "$WORK_DIR/driver" sign "$SEED_HEX")"
    printf '%s' "$MY_SIG" | xxd -r -p > "$WORK_DIR/my.sig"
    # 先把公钥导成 PEM 再验签。key.pem 里是**私钥**，拿它直接配 -pubin 读
    # 一定失败，还会往 stderr 吐一串 ossl_store 噪声（早期版本就是这么写的，
    # 日志里看着像出错，其实只是走错了分支）。
    openssl pkey -in "$WORK_DIR/key.pem" -pubout -out "$WORK_DIR/pub.pem" 2>/dev/null
    if ! openssl pkeyutl -verify -rawin -pubin -inkey "$WORK_DIR/pub.pem" \
           -in "$WORK_DIR/msg.bin" -sigfile "$WORK_DIR/my.sig" >/dev/null 2>&1; then
      echo "openssl 拒绝了我们的签名 case $i" >&2
      OK=0; break
    fi
  done
  if [ "$OK" -eq 1 ]; then
    record_pass "OpenSSL 3.0 CLI 交叉验证 5 例（公钥一致 + 双向验签）"
  else
    record_fail "OpenSSL oracle" "见上面的不一致行"
  fi
fi

echo "[ed25519] passed=$PASS failed=$FAIL"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
