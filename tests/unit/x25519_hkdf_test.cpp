// tests/unit/x25519_hkdf_test.cpp
//
// PR #21：手写 X25519（RFC 7748）与 HKDF-SHA256（RFC 5869）的单元测试。
//
// 这个文件只测**密码学原语本身**：官方测试向量、随机对称性、退化输入拒绝、
// 长度边界。握手与记录层在 tests/unit/secure_transport_test.cpp 里测。
//
// 测试向量来源（输入与期望输出都来自标准文档，不是从任何实现里抄的代码）：
//   * RFC 7748 §5.2  X25519 标量乘法向量（两条）+ 迭代 1000 次向量
//   * RFC 7748 §6.1  Diffie-Hellman 向量（Alice/Bob 公钥与共享秘密）
//   * RFC 5869 附录 A Test Case 1 / 2 / 3（SHA-256）
//
// 退出码：0 = 全部通过。

#include <cstdio>
#include <string>

#include "crypto.h"
#include "hkdf.h"
#include "x25519.h"

using backupproject::crypto::FromHex;
using backupproject::crypto::HkdfExpand;
using backupproject::crypto::HkdfExtract;
using backupproject::crypto::RandomBytes;
using backupproject::crypto::ToHex;
using backupproject::crypto::X25519;
using backupproject::crypto::X25519GenerateKeyPair;
using backupproject::crypto::X25519PublicKeyFromPrivate;
using backupproject::crypto::X25519SharedSecret;

namespace {

int g_checks = 0;
int g_failures = 0;

std::string Hex(const std::string& text) {
  std::string out;
  if (!FromHex(text, &out)) {
    std::printf("  FAIL 测试自身的十六进制常量写错了: %s\n", text.c_str());
    g_failures += 1;
  }
  return out;
}

std::string HexOf(const std::string& raw) {
  return ToHex(reinterpret_cast<const unsigned char*>(raw.data()), raw.size());
}

void Check(bool ok, const std::string& name, const std::string& detail = "") {
  g_checks += 1;
  if (!ok) {
    g_failures += 1;
    std::printf("  FAIL %s%s\n", name.c_str(),
                detail.empty() ? "" : (" -- " + detail).c_str());
  }
}

void CheckBytes(const char* name, const std::string& got,
                const std::string& want) {
  Check(got == want, name,
        got == want ? "" : ("got " + HexOf(got) + " want " + HexOf(want)));
}

std::string Repeated(const std::string& unit, int times) {
  std::string out;
  for (int i = 0; i < times; ++i) {
    out += unit;
  }
  return out;
}

std::string SequentialBytes(unsigned char first, unsigned char last) {
  std::string out;
  for (int value = first; value <= last; ++value) {
    out.push_back(static_cast<char>(value));
  }
  return out;
}

void TestX25519KnownAnswers() {
  std::printf("[x25519] RFC 7748 官方向量\n");
  std::string out;
  std::string error;

  const std::string scalar1 =
      Hex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4");
  const std::string u1 =
      Hex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c");
  Check(X25519(scalar1, u1, &out, &error), "vec1 计算成功", error);
  CheckBytes("RFC 7748 §5.2 向量 1", out,
             Hex("c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552"));

  const std::string scalar2 =
      Hex("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d");
  const std::string u2 =
      Hex("e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493");
  Check(X25519(scalar2, u2, &out, &error), "vec2 计算成功", error);
  CheckBytes("RFC 7748 §5.2 向量 2", out,
             Hex("95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957"));

  // 迭代向量：k = u = 9，每轮 (k, u) = (X25519(k, u), k)。
  std::string k = Hex("0900000000000000000000000000000000000000000000000000000000000000");
  std::string u = k;
  for (int i = 0; i < 1000; ++i) {
    std::string next;
    if (!X25519(k, u, &next, &error)) {
      Check(false, "迭代向量中途失败", error);
      return;
    }
    u = k;
    k = next;
    if (i == 0) {
      CheckBytes("RFC 7748 §5.2 迭代 1 次", k,
                 Hex("422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079"));
    }
  }
  CheckBytes("RFC 7748 §5.2 迭代 1000 次", k,
             Hex("684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51"));

  // §6.1 Diffie-Hellman
  const std::string alice_private =
      Hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
  const std::string bob_private =
      Hex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
  std::string alice_public;
  std::string bob_public;
  Check(X25519PublicKeyFromPrivate(alice_private, &alice_public, &error),
        "Alice 公钥推导成功", error);
  CheckBytes("RFC 7748 §6.1 Alice 公钥", alice_public,
             Hex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a"));
  Check(X25519PublicKeyFromPrivate(bob_private, &bob_public, &error),
        "Bob 公钥推导成功", error);
  CheckBytes("RFC 7748 §6.1 Bob 公钥", bob_public,
             Hex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f"));

  std::string shared_a;
  std::string shared_b;
  Check(X25519(alice_private, bob_public, &shared_a, &error), "Alice 侧共享秘密",
        error);
  Check(X25519(bob_private, alice_public, &shared_b, &error), "Bob 侧共享秘密",
        error);
  CheckBytes("RFC 7748 §6.1 共享秘密", shared_a,
             Hex("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742"));
  Check(shared_a == shared_b, "双方共享秘密一致");
}

void TestX25519Symmetry() {
  std::printf("[x25519] 随机对称性与退化输入\n");
  int symmetric = 0;
  for (int i = 0; i < 128; ++i) {
    std::string a_private;
    std::string a_public;
    std::string b_private;
    std::string b_public;
    std::string error;
    if (!X25519GenerateKeyPair(&a_private, &a_public, &error) ||
        !X25519GenerateKeyPair(&b_private, &b_public, &error)) {
      Check(false, "生成临时密钥对", error);
      return;
    }
    std::string shared_ab;
    std::string shared_ba;
    if (X25519SharedSecret(a_private, b_public, &shared_ab, &error) &&
        X25519SharedSecret(b_private, a_public, &shared_ba, &error) &&
        shared_ab == shared_ba && shared_ab.size() == 32) {
      symmetric += 1;
    }
  }
  Check(symmetric == 128,
        "128 组随机 X25519(a,B) == X25519(b,A)",
        std::to_string(symmetric) + "/128");

  // 低阶点：u = 0 让共享秘密退化成全零。
  std::string private_key;
  std::string public_key;
  std::string error;
  Check(X25519GenerateKeyPair(&private_key, &public_key, &error), "生成密钥对",
        error);
  std::string zero_u(32, '\0');
  std::string raw;
  Check(X25519(private_key, zero_u, &raw, &error), "原始接口接受低阶点输入",
        error);
  Check(raw == std::string(32, '\0'), "低阶点的原始输出是全零");
  std::string rejected;
  const bool ok = X25519SharedSecret(private_key, zero_u, &rejected, &error);
  Check(!ok, "X25519SharedSecret 拒绝全零共享秘密");
  Check(rejected.empty(), "被拒绝时不留任何输出");

  // 长度不合法必须显式失败。
  Check(!X25519(std::string(31, 'a'), public_key, &raw, &error),
        "标量长度 31 字节被拒绝");
  Check(!X25519(private_key, std::string(33, 'a'), &raw, &error),
        "u 坐标长度 33 字节被拒绝");

  // clamp 是幂等的：同一个私钥反复推导公钥结果一致。
  std::string public_again;
  Check(X25519PublicKeyFromPrivate(private_key, &public_again, &error) &&
            public_again == public_key,
        "私钥推导公钥可重复");
}

void TestX25519Parsing() {
  std::printf("[x25519] 文本解析与指纹\n");
  std::string private_key;
  std::string public_key;
  std::string error;
  Check(X25519GenerateKeyPair(&private_key, &public_key, &error), "生成密钥对",
        error);
  const std::string hex = backupproject::crypto::X25519FormatKeyHex(public_key);
  Check(hex.size() == 64, "公钥十六进制长度 64");
  std::string parsed;
  Check(backupproject::crypto::X25519ParseKeyText(hex, &parsed, &error) &&
            parsed == public_key,
        "裸十六进制可以解析回公钥");
  Check(backupproject::crypto::X25519ParseKeyText("hex:" + hex, &parsed, &error) &&
            parsed == public_key,
        "hex: 前缀可以解析回公钥");
  Check(!backupproject::crypto::X25519ParseKeyText("base64:" + hex, &parsed, &error),
        "未知前缀被拒绝");
  Check(!backupproject::crypto::X25519ParseKeyText(hex.substr(1), &parsed, &error),
        "长度不对的十六进制被拒绝");
  std::string bad = hex;
  bad[0] = 'z';
  Check(!backupproject::crypto::X25519ParseKeyText(bad, &parsed, &error),
        "非十六进制字符被拒绝");
  const std::string fingerprint =
      backupproject::crypto::X25519Fingerprint(public_key);
  Check(fingerprint.size() == 64, "指纹是 64 个十六进制字符");
  Check(fingerprint != hex, "指纹与公钥不是同一个字符串");
  Check(backupproject::crypto::X25519Fingerprint(std::string(31, 'x')).empty(),
        "长度不对时指纹为空");
}

void TestHkdfKnownAnswers() {
  std::printf("[hkdf] RFC 5869 附录 A 向量\n");
  std::string prk;
  std::string okm;
  std::string error;

  // Test Case 1
  Check(HkdfExtract(Hex("000102030405060708090a0b0c"), Repeated(Hex("0b"), 22),
                    &prk, &error),
        "TC1 Extract", error);
  CheckBytes("RFC 5869 TC1 PRK", prk,
             Hex("077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5"));
  Check(HkdfExpand(prk, Hex("f0f1f2f3f4f5f6f7f8f9"), 42, &okm, &error),
        "TC1 Expand", error);
  CheckBytes("RFC 5869 TC1 OKM", okm,
             Hex("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865"));

  // Test Case 2（长输入）
  const std::string ikm2 = SequentialBytes(0x00, 0x4f);
  const std::string salt2 = SequentialBytes(0x60, 0xaf);
  const std::string info2 = SequentialBytes(0xb0, 0xff);
  Check(HkdfExtract(salt2, ikm2, &prk, &error), "TC2 Extract", error);
  CheckBytes("RFC 5869 TC2 PRK", prk,
             Hex("06a6b88c5853361a06104c9ceb35b45cef760014904671014a193f40c15fc244"));
  Check(HkdfExpand(prk, info2, 82, &okm, &error), "TC2 Expand", error);
  CheckBytes("RFC 5869 TC2 OKM", okm,
             Hex("b11e398dc80327a1c8e7f78c596a49344f012eda2d4efad8a050cc4c19afa97c59045a99cac7827271cb41c65e590e09da3275600c2f09b8367793a9aca3db71cc30c58179ec3e87c14c01d5c1f3434f1d87"));

  // Test Case 3（salt 与 info 都是空串）
  Check(HkdfExtract("", Repeated(Hex("0b"), 22), &prk, &error), "TC3 Extract",
        error);
  CheckBytes("RFC 5869 TC3 PRK", prk,
             Hex("19ef24a32c717b167f33a91d6f648bdf96596776afdb6377ac434c1c293ccb04"));
  Check(HkdfExpand(prk, "", 42, &okm, &error), "TC3 Expand", error);
  CheckBytes("RFC 5869 TC3 OKM", okm,
             Hex("8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8"));
}

void TestHkdfProperties() {
  std::printf("[hkdf] 长度边界与标签分离\n");
  std::string prk;
  std::string okm;
  std::string error;
  Check(HkdfExtract("salt", "ikm", &prk, &error), "Extract", error);
  Check(HkdfExpand(prk, "info", 0, &okm, &error) && okm.empty(),
        "长度为 0 时输出空串");
  Check(HkdfExpand(prk, "info", 8160, &okm, &error) && okm.size() == 8160,
        "255 * HashLen 是允许的最大长度");
  Check(!HkdfExpand(prk, "info", 8161, &okm, &error) && okm.empty(),
        "超过 255 * HashLen 被拒绝且不留输出");

  // 不同 info 标签必须给出不相交的密钥材料（方向/用途分离的前提）。
  std::string a;
  std::string b;
  Check(HkdfExpand(prk, "BPSEC1 c2s enc", 32, &a, &error) &&
            HkdfExpand(prk, "BPSEC1 s2c enc", 32, &b, &error) && a != b,
        "不同标签给出不同密钥");
  std::string a2;
  Check(HkdfExpand(prk, "BPSEC1 c2s enc", 32, &a2, &error) && a2 == a,
        "同一标签可重复得到同一密钥");
  Check(HkdfExpand(prk, "BPSEC1 c2s enc", 32, &a, &error) &&
            HkdfExpand(prk, "BPSEC1 c2s mac", 32, &b, &error) && a != b,
        "加密与 MAC 标签给出不同密钥");

  // 前缀一致：HKDF-Expand 的输出必须是"T(1) || T(2) || ..."，
  // 所以长度 32 的输出必须是长度 64 输出的前缀。
  std::string short_out;
  std::string long_out;
  Check(HkdfExpand(prk, "prefix-test", 32, &short_out, &error) &&
            HkdfExpand(prk, "prefix-test", 64, &long_out, &error) &&
            long_out.compare(0, 32, short_out) == 0,
        "更长的输出以更短的输出为前缀");

  // salt 为空与 salt = 32 个 0 字节必须等价（RFC 5869 §2.2）。
  std::string prk_empty;
  std::string prk_zeros;
  Check(HkdfExtract("", "ikm", &prk_empty, &error) &&
            HkdfExtract(std::string(32, '\0'), "ikm", &prk_zeros, &error) &&
            prk_empty == prk_zeros,
        "空 salt 等价于 HashLen 个 0 字节");

  // Extract 的输出长度恒为 32 字节，且与 IKM 无关地只依赖 HMAC。
  std::string random_ikm;
  Check(RandomBytes(1000, &random_ikm, &error), "OS CSPRNG 可用", error);
  Check(HkdfExtract("s", random_ikm, &prk, &error) && prk.size() == 32,
        "PRK 恒为 32 字节");
}

}  // namespace

int main() {
  std::printf("x25519 / hkdf 单元测试\n");
  TestX25519KnownAnswers();
  TestX25519Symmetry();
  TestX25519Parsing();
  TestHkdfKnownAnswers();
  TestHkdfProperties();
  const int passed = g_checks - g_failures;
  std::printf("x25519-test: %d/%d checks passed\n", passed, g_checks);
  return g_failures == 0 ? 0 : 1;
}
