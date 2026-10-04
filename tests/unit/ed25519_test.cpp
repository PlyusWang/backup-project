// tests/unit/ed25519_test.cpp
//
// Ed25519（RFC 8032）的官方向量、变异与畸形输入测试。
//
// 层次：
//   V. RFC 8032 §7.1 官方向量：由种子导出公钥 + 对给定消息签名，逐字节比对；
//   R. 往返：生成密钥 -> 签名 -> 验签通过；换消息/换公钥/改一个 bit 必须失败；
//   M. 变异：消息 bit 翻转、签名两个半区 bit 翻转、公钥 bit 翻转；
//   X. 畸形：长度不对、S >= L（非规范标量）、y >= p（非规范编码）、
//      不在曲线上的公钥、小阶公钥 —— 每一种都必须是**具名的失败原因**，
//      不是笼统的 false。
//
// 独立实现（python cryptography / OpenSSL）的交叉验证放在
// scripts/ed25519_test.sh：那一步不是"再跑一遍我自己"。
//
// 退出码 0 = 全部通过。

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "crypto.h"
#include "ed25519.h"

namespace {

using backupproject::crypto::Ed25519VerifyDetailed;
using backupproject::crypto::Ed25519VerifyResult;

int g_passed = 0;
int g_failed = 0;

void Check(bool ok, const std::string& label, const std::string& detail = "") {
  if (ok) {
    ++g_passed;
    std::printf("[ed25519]   ok   %s\n", label.c_str());
    return;
  }
  ++g_failed;
  if (detail.empty()) {
    std::printf("[ed25519]   FAIL %s\n", label.c_str());
  } else {
    std::printf("[ed25519]   FAIL %s（%s）\n", label.c_str(), detail.c_str());
  }
}

std::string FromHexOrDie(const std::string& hex) {
  std::string out;
  if (!backupproject::crypto::FromHex(hex, &out)) {
    std::printf("[ed25519]   FATAL 测试自身的十六进制常量不合法\n");
    std::exit(2);
  }
  return out;
}

std::string Hex(const std::string& raw) {
  return backupproject::crypto::ToHex(
      reinterpret_cast<const unsigned char*>(raw.data()), raw.size());
}

const char* ResultName(Ed25519VerifyResult r) {
  return backupproject::crypto::Ed25519VerifyResultName(r);
}

struct RfcVector {
  const char* label;
  const char* seed_hex;
  const char* public_hex;
  const char* message_hex;   // "" 表示空消息
  const char* signature_hex;
};

// RFC 8032 §7.1 的 TEST 1 / TEST 2 / TEST 3 / TEST SHA(abc)
const RfcVector kVectors[] = {
    {"RFC8032-1", "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
     "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
     "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb88215"
     "90a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
    {"RFC8032-2", "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
     "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
     "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e4"
     "3e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
    {"RFC8032-3", "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
     "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
     "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b53"
     "8d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"},
    {"RFC8032-SHA(abc)",
     "833fe62409237b9d62ec77587520911e9a759cec1d19755b7da901b96dca3d42",
     "ec172b93ad5e563bf4932c70e1245034c35467ef2efd4d64ebf819683467e2bf",
     "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a"
     "274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
     "dc2a4459e7369633a52b1bf277839a00201009a3efbf3ecb69bea2186c26b58909351fc9"
     "ac90b3ecfdfbc7c66431e0303dca179c138ac17ad9bef1177331a704"},
};

}  // namespace

int main() {
  using backupproject::crypto::Ed25519FormatPublicKeyHex;
  using backupproject::crypto::Ed25519GenerateKeyPair;
  using backupproject::crypto::Ed25519ParsePublicKeyText;
  using backupproject::crypto::Ed25519PublicKeyFromSeed;
  using backupproject::crypto::Ed25519Sign;
  using backupproject::crypto::Ed25519Verify;
  using backupproject::crypto::kEd25519PublicKeySize;
  using backupproject::crypto::kEd25519SeedSize;
  using backupproject::crypto::kEd25519SignatureSize;

  // ---- V. RFC 8032 官方向量 ----
  for (const RfcVector& v : kVectors) {
    const std::string seed = FromHexOrDie(v.seed_hex);
    const std::string expect_public = FromHexOrDie(v.public_hex);
    const std::string message = FromHexOrDie(v.message_hex);
    const std::string expect_signature = FromHexOrDie(v.signature_hex);

    std::string public_key;
    std::string error;
    const bool pk_ok = Ed25519PublicKeyFromSeed(seed, &public_key, &error);
    Check(pk_ok && public_key == expect_public,
          std::string(v.label) + " 由种子导出公钥与官方一致",
          pk_ok ? Hex(public_key).substr(0, 16) : error);

    std::string signature;
    const bool sign_ok =
        Ed25519Sign(seed, message.data(), message.size(), &signature, &error);
    Check(sign_ok && signature == expect_signature,
          std::string(v.label) + " 签名与官方逐字节一致",
          sign_ok ? Hex(signature).substr(0, 16) : error);

    Check(Ed25519VerifyDetailed(expect_public, message.data(), message.size(),
                                expect_signature) == Ed25519VerifyResult::kOk,
          std::string(v.label) + " 官方签名验签通过");
  }

  // ---- R. 往返 ----
  std::string seed;
  std::string public_key;
  std::string error;
  Check(Ed25519GenerateKeyPair(&seed, &public_key, &error) &&
            seed.size() == kEd25519SeedSize &&
            public_key.size() == kEd25519PublicKeySize,
        "ED-R01 生成密钥对（32 字节种子 / 32 字节公钥）", error);
  {
    const std::string message = "backup-project ed25519 round trip";
    std::string signature;
    const bool ok = Ed25519Sign(seed, message.data(), message.size(),
                                &signature, &error) &&
                    signature.size() == kEd25519SignatureSize;
    Check(ok, "ED-R02 签名成功且长度 64 字节", error);
    Check(Ed25519Verify(public_key, message.data(), message.size(), signature,
                        &error),
          "ED-R03 正确验签通过", error);
    // 空消息
    std::string empty_sig;
    Check(Ed25519Sign(seed, nullptr, 0, &empty_sig, &error) &&
              Ed25519Verify(public_key, nullptr, 0, empty_sig, &error),
          "ED-R04 空消息签名 / 验签通过", error);
    // 换消息必须失败
    const std::string other = message + "!";
    Check(!Ed25519Verify(public_key, other.data(), other.size(), signature,
                         &error),
          "ED-R05 换一条消息验签失败");
  }

  // ---- M. 变异 ----
  {
    const std::string message = "mutation target";
    std::string signature;
    Ed25519Sign(seed, message.data(), message.size(), &signature, &error);

    // 消息 bit 翻转
    {
      std::string mutated = message;
      mutated[0] = static_cast<char>(mutated[0] ^ 0x01);
      Check(Ed25519VerifyDetailed(public_key, mutated.data(), mutated.size(),
                                  signature) ==
                Ed25519VerifyResult::kInvalidSignature,
            "ED-M01 消息翻一个 bit -> invalid-signature");
    }
    // 签名两个半区各翻一个 bit
    for (int half = 0; half < 2; ++half) {
      std::string mutated = signature;
      mutated[half * 32] = static_cast<char>(mutated[half * 32] ^ 0x01);
      Check(Ed25519VerifyDetailed(public_key, message.data(), message.size(),
                                  mutated) ==
                Ed25519VerifyResult::kInvalidSignature,
            half == 0 ? "ED-M02 R 半区翻一个 bit -> invalid-signature"
                      : "ED-M03 S 半区翻一个 bit -> invalid-signature");
    }
    // 公钥 bit 翻转：可能是另一个合法点（那就该验签失败），也可能是非法编码
    {
      std::string mutated = public_key;
      mutated[0] = static_cast<char>(mutated[0] ^ 0x01);
      const Ed25519VerifyResult r = Ed25519VerifyDetailed(
          mutated, message.data(), message.size(), signature);
      Check(r != Ed25519VerifyResult::kOk,
            "ED-M04 公钥翻一个 bit -> 绝不通过",
            ResultName(r));
    }
  }

  // ---- X. 畸形输入 ----
  {
    const std::string message = "malformed input target";
    std::string signature;
    Ed25519Sign(seed, message.data(), message.size(), &signature, &error);

    Check(Ed25519VerifyDetailed(public_key, message.data(), message.size(),
                                signature.substr(0, 63)) ==
              Ed25519VerifyResult::kBadSignatureSize,
          "ED-X01 截断到 63 字节的签名 -> bad-signature-size");
    Check(Ed25519VerifyDetailed(public_key, message.data(), message.size(),
                                signature + std::string(1, '\0')) ==
              Ed25519VerifyResult::kBadSignatureSize,
          "ED-X02 多 1 字节的签名 -> bad-signature-size");
    Check(Ed25519VerifyDetailed(public_key.substr(0, 31), message.data(),
                                message.size(), signature) ==
              Ed25519VerifyResult::kBadPublicKeySize,
          "ED-X03 31 字节公钥 -> bad-public-key-size");

    // S = L（非规范标量）
    {
      static const char* kLHex =
          "edd3f55c1a631258d69cf7a2def9de1400000000000000000000000000000010";
      std::string s_equal_l = FromHexOrDie(kLHex);
      std::string mutated = signature.substr(0, 32) + s_equal_l;
      Check(Ed25519VerifyDetailed(public_key, message.data(), message.size(),
                                  mutated) ==
                Ed25519VerifyResult::kNonCanonicalScalar,
            "ED-X04 S = L -> non-canonical-scalar");
      // S = L - 1 是规范的，应当走到验签方程并失败（而不是被长度/标量挡住）
      std::string s_l_minus_1 = s_equal_l;
      s_l_minus_1[0] = static_cast<char>(s_l_minus_1[0] - 1);
      std::string mutated2 = signature.substr(0, 32) + s_l_minus_1;
      Check(Ed25519VerifyDetailed(public_key, message.data(), message.size(),
                                  mutated2) ==
                Ed25519VerifyResult::kInvalidSignature,
            "ED-X05 S = L-1（规范但错误）-> invalid-signature");
    }

    // y >= p：全 0xFF 的最高位会被掩掉，得到 0x7F..FF = 2^255 - 1 = p + 18，
    // 仍然 >= p，所以必须按"非规范编码"拒绝。
    // （别把 0x7F..FF 写成 p-1：p-1 的编码是 ecff..ff7f，那个才是合法的。）
    // 所以下面用 y = p（= 2^255-19）的编码：低 255 位是 p，最高位 0。
    {
      static const char* kPHex =
          "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f";
      std::string y_equal_p = FromHexOrDie(kPHex);
      Check(Ed25519VerifyDetailed(y_equal_p, message.data(), message.size(),
                                  signature) ==
                Ed25519VerifyResult::kNonCanonicalPublicKey,
            "ED-X06 公钥 y = p -> non-canonical-public-key");
      static const char* kAllFf =
          "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
      std::string all_ff = FromHexOrDie(kAllFf);
      Check(Ed25519VerifyDetailed(all_ff, message.data(), message.size(),
                                  signature) ==
                Ed25519VerifyResult::kNonCanonicalPublicKey,
            "ED-X07 公钥全 0xFF（y = 2^255-1 >= p）-> non-canonical-public-key");
    }

    // 小阶公钥：identity 的编码是 y = 1（32 字节 01 00 ...）
    {
      std::string identity(32, '\0');
      identity[0] = 1;
      Check(Ed25519VerifyDetailed(identity, message.data(), message.size(),
                                  signature) ==
                Ed25519VerifyResult::kSmallOrderPublicKey,
            "ED-X08 identity 编码（小阶）-> small-order-public-key");
      // y = -1（order 2）
      std::string minus_one(32, '\0');
      minus_one[0] = static_cast<char>(0xEC);
      for (int i = 1; i < 31; ++i) {
        minus_one[i] = static_cast<char>(0xFF);
      }
      minus_one[31] = static_cast<char>(0x7F);
      const Ed25519VerifyResult r = Ed25519VerifyDetailed(
          minus_one, message.data(), message.size(), signature);
      Check(r == Ed25519VerifyResult::kSmallOrderPublicKey ||
                r == Ed25519VerifyResult::kInvalidPublicKey,
            "ED-X09 y = -1（order 2）被拒", ResultName(r));
    }

    // 不在曲线上的公钥：扫一批 y 值，必须至少命中一个 invalid-public-key
    {
      int invalid_hits = 0;
      int small_order_hits = 0;
      for (int y = 2; y < 40; ++y) {
        std::string candidate(32, '\0');
        candidate[0] = static_cast<char>(y);
        const Ed25519VerifyResult r = Ed25519VerifyDetailed(
            candidate, message.data(), message.size(), signature);
        if (r == Ed25519VerifyResult::kInvalidPublicKey) {
          ++invalid_hits;
        } else if (r == Ed25519VerifyResult::kSmallOrderPublicKey) {
          ++small_order_hits;
        }
      }
      Check(invalid_hits > 0, "ED-X10 扫 y=2..39 命中 invalid-public-key",
            "hits=" + std::to_string(invalid_hits) +
                " small_order=" + std::to_string(small_order_hits));
    }

    // 随机字节的公钥绝不允许"通过"
    {
      int accepted = 0;
      for (int i = 0; i < 64; ++i) {
        std::string candidate(32, '\0');
        for (int j = 0; j < 32; ++j) {
          candidate[j] = static_cast<char>((i * 31 + j * 17) & 0xff);
        }
        if (Ed25519VerifyDetailed(candidate, message.data(), message.size(),
                                  signature) == Ed25519VerifyResult::kOk) {
          ++accepted;
        }
      }
      Check(accepted == 0, "ED-X11 64 组构造字节的公钥没有一个通过",
            "accepted=" + std::to_string(accepted));
    }
  }

  // ---- 参数卫生 ----
  {
    std::string pk;
    Check(!Ed25519PublicKeyFromSeed(std::string(31, 'x'), &pk, &error),
          "ED-P01 31 字节种子被拒");
    Check(!Ed25519PublicKeyFromSeed(std::string(33, 'x'), &pk, &error),
          "ED-P02 33 字节种子被拒");
    std::string sig;
    Check(!Ed25519Sign(std::string(31, 'x'), "m", 1, &sig, &error),
          "ED-P03 31 字节种子签名被拒");
    Check(!Ed25519Sign(seed, nullptr, 5, &sig, &error),
          "ED-P04 长度非零但消息指针为空被拒");
  }

  // ---- 文本工具 ----
  {
    const std::string text = Ed25519FormatPublicKeyHex(public_key);
    std::string parsed;
    Check(text.compare(0, 8, "ed25519:") == 0 &&
              Ed25519ParsePublicKeyText(text, &parsed, &error) &&
              parsed == public_key,
          "ED-T01 公钥文本往返一致");
    Check(!Ed25519ParsePublicKeyText("ed25519:zz", &parsed, &error),
          "ED-T02 非法十六进制被拒");
    Check(!backupproject::crypto::Ed25519Fingerprint("short").empty() == false,
          "ED-T03 长度不对的指纹返回空串");
  }

  std::printf("[ed25519] passed=%d failed=%d\n", g_passed, g_failed);
  return g_failed == 0 ? 0 : 1;
}
