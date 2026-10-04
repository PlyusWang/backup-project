#!/usr/bin/env bash
#
# bpcert_test.sh —— BPCERT1 证书格式的测试入口（PR #23）。
#
#   bash scripts/bpcert_test.sh
#
# 两层：
#   A. 编译 tests/unit/bpcert_test.cpp + src/crypto/bpcert.cpp，跑正向 / 时间窗 /
#      六类畸形 / 字段语义 / 单字节扫描 / 20000 例 fuzz；
#   B. **独立 oracle**：python 侧按格式规范**自己重新拼一遍字节**，再用
#      cryptography（走 OpenSSL）的 Ed25519 对同一个 body 签名 —— Ed25519 是
#      确定性的，所以两边的完整证书必须逐字节相同；再双向验签 + 变异必拒。
#      这一步不是"再跑一遍我自己"：格式和签名各有一个独立实现对照。
#
# 环境变量 BPCERT_TEST_EXTRA_FLAGS 可以追加编译参数（例如 sanitizer）。
# 退出码：0 = 全部通过。

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

set +u
EXTRA="$BPCERT_TEST_EXTRA_FLAGS"
set -u

WORK_DIR="$(mktemp -d /tmp/bpcert-test-XXXXXX)"
trap 'rm -rf "$WORK_DIR"' EXIT

BUILD_LOG="$WORK_DIR/build.log"
PASS=0
FAIL=0
record_pass() { PASS=$((PASS + 1)); echo "  PASS  $1"; }
record_fail() { FAIL=$((FAIL + 1)); echo "  FAIL  $1 -- $2" >&2; }

# ---- A. 单元测试 ----
# shellcheck disable=SC2086
if ! g++ -std=c++17 -Wall -Wextra -Wpedantic -Iinclude $EXTRA \
      tests/unit/bpcert_test.cpp src/crypto/bpcert.cpp src/crypto/ed25519.cpp \
      src/crypto/sha512.cpp src/crypto/sha256.cpp src/crypto/random.cpp \
      -o "$WORK_DIR/bpcert_test" >"$BUILD_LOG" 2>&1; then
  record_fail "编译 bpcert_test" "$(tail -8 "$BUILD_LOG" | tr '\n' ' ')"
  echo "[bpcert] passed=$PASS failed=$FAIL"
  exit 1
fi
if [ -s "$BUILD_LOG" ]; then
  record_fail "编译必须零警告" "$(head -5 "$BUILD_LOG" | tr '\n' ' ')"
else
  record_pass "零警告编译（-Wall -Wextra -Wpedantic）"
fi

if "$WORK_DIR/bpcert_test" > "$WORK_DIR/run.log" 2>&1; then
  record_pass "$(grep -oE 'passed=[0-9]+ failed=[0-9]+' "$WORK_DIR/run.log" | tail -1)（正向 / 时间窗 / 畸形 / 字段 / 变异 / fuzz）"
else
  record_fail "bpcert_test 退出码" "$(grep -m5 FAIL "$WORK_DIR/run.log" | tr '\n' ' ')"
fi
if grep -qE 'ERROR: (AddressSanitizer|LeakSanitizer|UndefinedBehaviorSanitizer)|runtime error:' "$WORK_DIR/run.log"; then
  record_fail "消毒剂报告" "$(grep -m2 -E 'ERROR|runtime error' "$WORK_DIR/run.log" | tr '\n' ' ')"
else
  record_pass "消毒剂报告 0 条"
fi

# ---- 供 oracle 使用的 driver ----
# 这里的参数都很短（证书 ~200 字节），走 argv 不会撞 E2BIG。
cat > "$WORK_DIR/driver.cpp" <<'CPP'
#include <cstdio>
#include <cstdlib>
#include <string>
#include "bpcert.h"
#include "crypto.h"
#include "ed25519.h"

using backupproject::crypto::Bpcert1;
using backupproject::crypto::Bpcert1Error;
using backupproject::crypto::Bpcert1ErrorName;

static std::string Unhex(const char* text) {
  std::string out;
  if (!backupproject::crypto::FromHex(text, &out)) { std::exit(2); }
  return out;
}

static std::string Hex(const std::string& raw) {
  return backupproject::crypto::ToHex(
      reinterpret_cast<const unsigned char*>(raw.data()), raw.size());
}

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  const std::string cmd = argv[1];
  if (cmd == "issue" && argc == 9) {
    Bpcert1 cert;
    cert.server_id = argv[3];
    cert.server_public_key = Unhex(argv[4]);
    cert.issuer_id = argv[5];
    cert.serial_number = std::strtoull(argv[6], nullptr, 10);
    cert.not_before = std::strtoll(argv[7], nullptr, 10);
    cert.not_after = std::strtoll(argv[8], nullptr, 10);
    std::string out;
    std::string error;
    if (!backupproject::crypto::Bpcert1Issue(cert, Unhex(argv[2]), &out, &error)) {
      std::fprintf(stderr, "%s\n", error.c_str());
      return 1;
    }
    std::printf("%s\n", Hex(out).c_str());
    return 0;
  }
  if (cmd == "verify" && argc == 4) {
    const Bpcert1Error r =
        backupproject::crypto::Bpcert1VerifySignature(Unhex(argv[2]), Unhex(argv[3]));
    std::printf("%s\n", Bpcert1ErrorName(r));
    return r == Bpcert1Error::kOk ? 0 : 1;
  }
  if (cmd == "parse" && argc == 3) {
    Bpcert1 cert;
    const Bpcert1Error r = backupproject::crypto::Bpcert1Parse(Unhex(argv[2]), &cert);
    if (r != Bpcert1Error::kOk) {
      std::printf("error=%s\n", Bpcert1ErrorName(r));
      return 1;
    }
    std::printf("server_id=%s\n", cert.server_id.c_str());
    std::printf("server_public_key=%s\n", Hex(cert.server_public_key).c_str());
    std::printf("issuer_id=%s\n", cert.issuer_id.c_str());
    std::printf("serial_number=%llu\n",
                static_cast<unsigned long long>(cert.serial_number));
    std::printf("not_before=%lld\n", static_cast<long long>(cert.not_before));
    std::printf("not_after=%lld\n", static_cast<long long>(cert.not_after));
    std::printf("signature=%s\n", Hex(cert.signature).c_str());
    return 0;
  }
  return 2;
}
CPP
# shellcheck disable=SC2086
if ! g++ -std=c++17 -Wall -Wextra -Iinclude $EXTRA \
      "$WORK_DIR/driver.cpp" src/crypto/bpcert.cpp src/crypto/ed25519.cpp \
      src/crypto/sha512.cpp src/crypto/sha256.cpp src/crypto/random.cpp \
      -o "$WORK_DIR/driver" >>"$BUILD_LOG" 2>&1; then
  record_fail "编译 oracle driver" "$(tail -5 "$BUILD_LOG" | tr '\n' ' ')"
  echo "[bpcert] passed=$PASS failed=$FAIL"
  exit 1
fi

# ---- B. 独立 oracle ----
if ! python3 -c "import cryptography" >/dev/null 2>&1; then
  record_fail "独立 oracle" "缺少 python cryptography"
else
  cat > "$WORK_DIR/oracle.py" <<'PY'
import binascii, struct, subprocess, sys
from cryptography.hazmat.primitives.asymmetric.ed25519 import (
    Ed25519PrivateKey, Ed25519PublicKey)

driver = sys.argv[1]
SEED = binascii.unhexlify(
    "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")
PUB = Ed25519PrivateKey.from_private_bytes(SEED).public_key().public_bytes_raw()
SERVER_PUB = bytes(range(1, 33))
SERVER_ID = b"backup-project-cloud-production"
ISSUER_ID = b"backup-project-official-root-a"
SERIAL = 20261005001
NOT_BEFORE = 1790000000
NOT_AFTER = NOT_BEFORE + 180 * 24 * 60 * 60

def build_body(server_id, server_pub, serial, not_before, not_after, issuer_id):
    """按 include/bpcert.h 里的规范布局独立拼一遍（大端、长度前缀）。"""
    b = bytearray()
    b += b"BPCERT1"
    b += struct.pack(">H", 1)                  # format_version
    b += struct.pack(">H", len(server_id)) + server_id
    b += bytes([1])                            # public_key_algorithm = X25519
    b += server_pub
    b += struct.pack(">Q", serial)
    b += struct.pack(">q", not_before)
    b += struct.pack(">q", not_after)
    b += struct.pack(">H", len(issuer_id)) + issuer_id
    b += bytes([1])                            # key_usage = SERVER_AUTH
    b += bytes([1])                            # signature_algorithm = Ed25519
    return bytes(b)

def run(args):
    p = subprocess.run([driver] + list(args), capture_output=True, text=True)
    return p.returncode, p.stdout.strip(), p.stderr.strip()

mismatch = 0

body = build_body(SERVER_ID, SERVER_PUB, SERIAL, NOT_BEFORE, NOT_AFTER, ISSUER_ID)
# Ed25519 是确定性的：独立实现对同一个 body 签名必须得到同样的 64 字节。
sig = Ed25519PrivateKey.from_private_bytes(SEED).sign(body)
expected = body + sig

rc, got, err = run(["issue", binascii.hexlify(SEED).decode(), SERVER_ID.decode(),
                    binascii.hexlify(SERVER_PUB).decode(), ISSUER_ID.decode(),
                    str(SERIAL), str(NOT_BEFORE), str(NOT_AFTER)])
if rc != 0:
    print("driver issue failed:", err); mismatch += 1
elif got != binascii.hexlify(expected).decode():
    print("证书字节与独立实现不一致")
    print("  我们:", got[:120])
    print("  独立:", binascii.hexlify(expected).decode()[:120])
    mismatch += 1
else:
    print("FORMAT_MATCH=1 bytes=%d" % len(expected))

# 独立实现验我们的签名
try:
    Ed25519PublicKey.from_public_bytes(PUB).verify(sig, body)
except Exception as exc:
    print("独立实现拒绝了我们的证书签名:", exc); mismatch += 1

# 独立实现签的证书，我们必须接受
rc, name, _ = run(["verify", binascii.hexlify(expected).decode(),
                   binascii.hexlify(PUB).decode()])
if rc != 0:
    print("我们拒绝了独立实现签发的证书:", name); mismatch += 1

# 我们自己的 parse 必须还原出全部字段
rc, out, err = run(["parse", binascii.hexlify(expected).decode()])
if rc != 0:
    print("我们无法解析独立实现构造的证书:", err); mismatch += 1
else:
    fields = dict(line.split("=", 1) for line in out.splitlines())
    checks = {
        "server_id": SERVER_ID.decode(),
        "server_public_key": binascii.hexlify(SERVER_PUB).decode(),
        "issuer_id": ISSUER_ID.decode(),
        "serial_number": str(SERIAL),
        "not_before": str(NOT_BEFORE),
        "not_after": str(NOT_AFTER),
        "signature": binascii.hexlify(sig).decode(),
    }
    for key, want in checks.items():
        if fields.get(key) != want:
            print("字段 %s 不一致: %r vs %r" % (key, fields.get(key), want))
            mismatch += 1

# 变异 40 处，必须全部被拒（含 body 与签名两段）
import random
random.seed(20261005)
for i in range(40):
    bad = bytearray(expected)
    pos = random.randrange(len(bad))
    bad[pos] ^= 1 << random.randrange(8)
    rc, name, _ = run(["verify", binascii.hexlify(bytes(bad)).decode(),
                       binascii.hexlify(PUB).decode()])
    if rc == 0:
        print("变异证书被接受: pos=%d" % pos); mismatch += 1

# 截断到每一个长度都必须被拒
for length in range(len(expected)):
    rc, name, _ = run(["verify", binascii.hexlify(expected[:length]).decode(),
                       binascii.hexlify(PUB).decode()])
    if rc == 0:
        print("截断到 %d 字节被接受" % length); mismatch += 1

print("ORACLE_MISMATCH=%d" % mismatch)
sys.exit(0 if mismatch == 0 else 1)
PY
  if python3 "$WORK_DIR/oracle.py" "$WORK_DIR/driver" > "$WORK_DIR/oracle.log" 2>&1; then
    record_pass "独立 oracle（python 独立拼字节 + cryptography 双向验签 + 变异/截断必拒）$(grep -oE 'bytes=[0-9]+' "$WORK_DIR/oracle.log" | tail -1)"
  else
    record_fail "独立 oracle" "$(tail -4 "$WORK_DIR/oracle.log" | tr '\n' ' ')"
  fi
fi

echo "[bpcert] passed=$PASS failed=$FAIL"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
