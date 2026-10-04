#!/usr/bin/env python3
# tests/review/x25519_oracle.py
#
# **REVIEW-ONLY 独立 oracle，不属于产品构建。**
#
# 目的：用一个与产品实现**完全不同**的表示（Python 任意精度整数）独立复现
# RFC 7748 的数学定义，然后拿它去对拍 src/crypto/x25519.cpp：
#   * 域运算：add / sub / mul / sq / inv
#   * X25519 整体：随机 scalar/u + 边界 u 坐标
#
# 只用 Python 内建整数与 hashlib（没有 cryptography / PyNaCl / OpenSSL / libsodium，
# 也没有任何第三方曲线代码）。oracle 先自我校验 RFC 7748 的官方向量，通过之后
# 才拿来当基准。
#
# 用法: python3 x25519_oracle.py <harness-binary> [cases]

import hashlib
import os
import random
import subprocess
import sys

P = 2 ** 255 - 19
A24 = 121665
MASK255 = (1 << 255) - 1

# ---- RFC 7748 §5 的定义（独立实现：用 Python 整数，没有 limb / 进位概念）----


def clamp(scalar: bytes) -> int:
    k = bytearray(scalar)
    k[0] &= 248
    k[31] &= 127
    k[31] |= 64
    return int.from_bytes(k, "little")


def decode_u(u: bytes) -> int:
    # decodeUCoordinate：屏蔽最高位，再按整数解释（mod p 语义由运算保证）
    return int.from_bytes(u, "little") & MASK255


def x25519(scalar: bytes, u_bytes: bytes) -> bytes:
    k = clamp(scalar)
    x1 = decode_u(u_bytes) % P
    x2, z2, x3, z3 = 1, 0, x1 % P, 1
    swap = 0
    for t in range(254, -1, -1):
        kt = (k >> t) & 1
        swap ^= kt
        if swap:
            x2, x3 = x3, x2
            z2, z3 = z3, z2
        swap = kt
        a = (x2 + z2) % P
        aa = (a * a) % P
        b = (x2 - z2) % P
        bb = (b * b) % P
        e = (aa - bb) % P
        c = (x3 + z3) % P
        d = (x3 - z3) % P
        da = (d * a) % P
        cb = (c * b) % P
        x3 = ((da + cb) % P) ** 2 % P
        z3 = (x1 * (((da - cb) % P) ** 2 % P)) % P
        x2 = (aa * bb) % P
        z2 = (e * ((aa + A24 * e) % P)) % P
    if swap:
        x2, x3 = x3, x2
        z2, z3 = z3, z2
    return ((x2 * pow(z2, P - 2, P)) % P).to_bytes(32, "little")


def fe(hex_text: str) -> int:
    return int.from_bytes(bytes.fromhex(hex_text), "little") % P


def to_hex(value: int) -> str:
    return value.to_bytes(32, "little").hex()


# ---- oracle 自我校验（先证明 oracle 自己是对的）----

SELF_TESTS = [
    ("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4",
     "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c",
     "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552"),
    ("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d",
     "e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493",
     "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957"),
]


def self_check() -> bool:
    ok = True
    for scalar_hex, u_hex, expect in SELF_TESTS:
        got = x25519(bytes.fromhex(scalar_hex), bytes.fromhex(u_hex)).hex()
        if got != expect:
            print("ORACLE SELF-CHECK FAILED: %s != %s" % (got, expect))
            ok = False
    k = bytes([9] + [0] * 31)
    u = k
    for _ in range(1000):
        k, u = x25519(k, u), k
    if k.hex() != "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51":
        print("ORACLE SELF-CHECK FAILED at 1000 iterations: %s" % k.hex())
        ok = False
    return ok


# ---- 对拍驱动 ----


def run_harness(binary: str, lines):
    proc = subprocess.run([binary], input="\n".join(lines) + "\n",
                          capture_output=True, text=True, check=True)
    return proc.stdout.strip().split("\n")


def main() -> int:
    binary = sys.argv[1]
    cases = int(sys.argv[2]) if len(sys.argv) > 2 else 1200
    rng = random.Random(20261003)
    if not self_check():
        return 1
    print("oracle self-check: PASS（RFC 7748 两条向量 + 1000 次迭代）")

    failures = 0
    checks = 0

    # 1) 域运算
    ops = ["add", "sub", "mul", "sq", "inv"]
    for op in ops:
        lines = []
        expected = []
        for _ in range(cases):
            a = rng.randrange(0, P)
            b = rng.randrange(0, P)
            if op == "add":
                expected.append(to_hex((a + b) % P))
                lines.append("add %s %s" % (to_hex(a), to_hex(b)))
            elif op == "sub":
                expected.append(to_hex((a - b) % P))
                lines.append("sub %s %s" % (to_hex(a), to_hex(b)))
            elif op == "mul":
                expected.append(to_hex((a * b) % P))
                lines.append("mul %s %s" % (to_hex(a), to_hex(b)))
            elif op == "sq":
                expected.append(to_hex((a * a) % P))
                lines.append("sq %s" % to_hex(a))
            else:
                expected.append(to_hex(0 if a % P == 0 else pow(a % P, P - 2, P)))
                lines.append("inv %s" % to_hex(a))
        got = run_harness(binary, lines)
        bad = 0
        for index, (g, e) in enumerate(zip(got, expected)):
            checks += 1
            if g != e:
                bad += 1
                if bad <= 3:
                    print("  MISMATCH %s case %d: got %s want %s" % (op, index, g, e))
        failures += bad
        print("field %-4s : %d cases, %d mismatches" % (op, cases, bad))

    # 2) 边界 / 特殊值（含 p、p+1、2^255-1、最高位被置位）
    boundary = {
        "zero": 0,
        "one": 1,
        "p-1": P - 1,
        "p": P,
        "p+1": P + 1,
        "2^255-1": (1 << 255) - 1,
        "2^255-19+19": P + 19,
        "high-bit-only": 1 << 255,
        "max-32byte": (1 << 256) - 1,
    }
    lines = []
    expected = []
    labels = []
    for name, value in boundary.items():
        for op in ["add", "sub", "mul", "sq", "inv"]:
            a = value % (1 << 256)
            b = rng.randrange(0, P)
            raw_a = a.to_bytes(32, "little")
            raw_b = b.to_bytes(32, "little")
            # 产品侧 FeFromBytes 会屏蔽最高位并 mod p；oracle 用同样的语义
            a_eff = int.from_bytes(raw_a, "little") & MASK255
            if op == "add":
                expected.append(to_hex((a_eff + b) % P))
                lines.append("add %s %s" % (raw_a.hex(), raw_b.hex()))
            elif op == "sub":
                expected.append(to_hex((a_eff - b) % P))
                lines.append("sub %s %s" % (raw_a.hex(), raw_b.hex()))
            elif op == "mul":
                expected.append(to_hex((a_eff * b) % P))
                lines.append("mul %s %s" % (raw_a.hex(), raw_b.hex()))
            elif op == "sq":
                expected.append(to_hex((a_eff * a_eff) % P))
                lines.append("sq %s" % raw_a.hex())
            else:
                expected.append(to_hex(0 if a_eff % P == 0 else pow(a_eff % P, P - 2, P)))
                lines.append("inv %s" % raw_a.hex())
            labels.append("%s/%s" % (name, op))
    got = run_harness(binary, lines)
    bad = 0
    for index, (g, e) in enumerate(zip(got, expected)):
        checks += 1
        if g != e:
            bad += 1
            print("  BOUNDARY MISMATCH %s: got %s want %s" % (labels[index], g, e))
    failures += bad
    print("field boundary: %d cases, %d mismatches" % (len(lines), bad))

    # 3) X25519 整体：随机
    lines = []
    expected = []
    zero_outputs = 0
    for _ in range(cases):
        scalar = bytes(rng.randrange(0, 256) for _ in range(32))
        u = bytes(rng.randrange(0, 256) for _ in range(32))
        out = x25519(scalar, u)
        if out == bytes(32):
            zero_outputs += 1
        expected.append(out.hex())
        lines.append("x25519 %s %s" % (scalar.hex(), u.hex()))
    got = run_harness(binary, lines)
    bad = 0
    for index, (g, e) in enumerate(zip(got, expected)):
        checks += 1
        if g != e:
            bad += 1
            if bad <= 3:
                print("  X25519 MISMATCH case %d: got %s want %s" % (index, g, e))
    failures += bad
    print("x25519 random : %d cases, %d mismatches（其中全零输出 %d 例）"
          % (cases, bad, zero_outputs))

    # 4) X25519 边界 u 坐标（按 RFC 7748 的 decodeUCoordinate 语义）
    lines = []
    expected = []
    labels = []
    scalar = bytes.fromhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a")
    for name, value in boundary.items():
        u = (value % (1 << 256)).to_bytes(32, "little")
        expected.append(x25519(scalar, u).hex())
        lines.append("x25519 %s %s" % (scalar.hex(), u.hex()))
        labels.append(name)
    got = run_harness(binary, lines)
    bad = 0
    for index, (g, e) in enumerate(zip(got, expected)):
        checks += 1
        if g != e:
            bad += 1
            print("  BOUNDARY X25519 MISMATCH %s: got %s want %s" % (labels[index], g, e))
        else:
            print("  boundary u %-14s -> %s%s" % (labels[index], g[:16],
                  "（全零：低阶点，产品用 X25519SharedSecret 时必须拒绝）"
                  if g == "0" * 64 else ""))
    failures += bad
    print("x25519 boundary: %d cases, %d mismatches" % (len(lines), bad))

    print("TOTAL: %d comparisons, %d mismatches" % (checks, failures))
    print("X25519_ORACLE_PASS" if failures == 0 else "X25519_ORACLE_FAIL")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
