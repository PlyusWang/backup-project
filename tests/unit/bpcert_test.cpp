// tests/unit/bpcert_test.cpp
//
// BPCERT1 的格式测试：正向、六类畸形、字段语义、变异与随机 fuzz。
//
// 层次：
//   P. 正向：签发 -> 解析 -> 逐字段相等；编码可复现；body/签名切分；
//   T. 时间窗：生效前 / 生效后 / 到期 / 过期 / ±5 分钟容差 / 时钟提示文案；
//   X. 六类畸形（**每一类都要求具名原因**）：
//        截断、超长、未知版本、未知算法、长度不合法、尾部多余字节；
//   F. 字段语义：序列号 0、时间窗反向、超长窗口、标识符含控制字符；
//   M. 变异：单字节扫描（每个位置 × 3 种掩码）后**完整验签必须失败**；
//   Z. fuzz：10000 条随机字节串 + 10000 条随机单字节变异。
//
// 退出码 0 = 全部通过。

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "bpcert.h"
#include "crypto.h"

namespace {

using backupproject::crypto::Bpcert1;
using backupproject::crypto::Bpcert1Error;
using backupproject::crypto::Bpcert1ErrorName;

int g_passed = 0;
int g_failed = 0;

void Check(bool ok, const std::string& label, const std::string& detail = "") {
  if (ok) {
    ++g_passed;
    std::printf("[bpcert]   ok   %s\n", label.c_str());
    return;
  }
  ++g_failed;
  if (detail.empty()) {
    std::printf("[bpcert]   FAIL %s\n", label.c_str());
  } else {
    std::printf("[bpcert]   FAIL %s（%s）\n", label.c_str(), detail.c_str());
  }
}

std::string FromHexOrDie(const std::string& hex) {
  std::string out;
  if (!backupproject::crypto::FromHex(hex, &out)) {
    std::printf("[bpcert]   FATAL 测试自身的十六进制常量不合法\n");
    std::exit(2);
  }
  return out;
}

// RFC 8032 TEST1 的密钥对：只用于测试，不是任何真实身份的密钥。
const char* kIssuerSeedHex =
    "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";
const char* kIssuerPubHex =
    "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";
const char* kOtherPubHex =
    "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c";
const char* kServerPubHex =
    "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20";

const char* kServerId = "backup-project-cloud-production";
const char* kIssuerId = "backup-project-official-root-a";
const std::uint64_t kSerial = 20261005001ULL;
const std::int64_t kNotBefore = 1790000000;  // 固定时间：编码必须可复现

Bpcert1 MakeCert() {
  Bpcert1 cert;
  cert.server_id = kServerId;
  cert.server_public_key = FromHexOrDie(kServerPubHex);
  cert.issuer_id = kIssuerId;
  cert.serial_number = kSerial;
  cert.not_before = kNotBefore;
  cert.not_after = kNotBefore + 180LL * 24 * 60 * 60;
  return cert;
}

std::string IssueFixed() {
  std::string cert;
  std::string error;
  if (!backupproject::crypto::Bpcert1Issue(MakeCert(), FromHexOrDie(kIssuerSeedHex),
                                           &cert, &error)) {
    std::printf("[bpcert]   FATAL 签发失败：%s\n", error.c_str());
    std::exit(2);
  }
  return cert;
}

// 布局偏移。标识符长度可变，所以按同样的规则现场算出来。
struct Offsets {
  std::size_t server_id = 11;
  std::size_t key_alg = 0;
  std::size_t public_key = 0;
  std::size_t serial = 0;
  std::size_t not_before = 0;
  std::size_t not_after = 0;
  std::size_t issuer_len = 0;
  std::size_t issuer = 0;
  std::size_t key_usage = 0;
  std::size_t sig_alg = 0;
  std::size_t signature = 0;
};

Offsets LayoutOf(const Bpcert1& cert) {
  Offsets o;
  o.key_alg = o.server_id + cert.server_id.size();
  o.public_key = o.key_alg + 1;
  o.serial = o.public_key + 32;
  o.not_before = o.serial + 8;
  o.not_after = o.not_before + 8;
  o.issuer_len = o.not_after + 8;
  o.issuer = o.issuer_len + 2;
  o.key_usage = o.issuer + cert.issuer_id.size();
  o.sig_alg = o.key_usage + 1;
  o.signature = o.sig_alg + 1;
  return o;
}

std::string Mutate(const std::string& base, std::size_t offset,
                   unsigned char value) {
  std::string copy = base;
  if (offset < copy.size()) {
    copy[offset] = static_cast<char>(value);
  }
  return copy;
}

void PutU16(std::string* raw, std::size_t offset, std::uint16_t value) {
  (*raw)[offset] = static_cast<char>((value >> 8) & 0xFF);
  (*raw)[offset + 1] = static_cast<char>(value & 0xFF);
}

void PutU64(std::string* raw, std::size_t offset, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    (*raw)[offset + static_cast<std::size_t>(i)] =
        static_cast<char>((value >> (56 - 8 * i)) & 0xFF);
  }
}

// 一条断言的通用形状：解析必须给出**指定的**原因。
void CheckParseError(const std::string& raw, Bpcert1Error expected,
                     const std::string& label) {
  Bpcert1 parsed;
  const Bpcert1Error actual = backupproject::crypto::Bpcert1Parse(raw, &parsed);
  Check(actual == expected, label,
        std::string("期望 ") + Bpcert1ErrorName(expected) + "，实际 " +
            Bpcert1ErrorName(actual));
}

std::uint64_t g_random_state = 0x9E3779B97F4A7C15ULL;
std::uint64_t NextRandom() {
  g_random_state = g_random_state * 6364136223846793005ULL +
                   1442695040888963407ULL;
  return g_random_state >> 17;
}

}  // namespace

int main() {
  const std::string issuer_pub = FromHexOrDie(kIssuerPubHex);
  const std::string other_pub = FromHexOrDie(kOtherPubHex);
  const Bpcert1 cert = MakeCert();
  const Offsets off = LayoutOf(cert);
  const std::string raw = IssueFixed();

  // ---- P. 正向 ----
  {
    Bpcert1 parsed;
    Check(backupproject::crypto::Bpcert1Parse(raw, &parsed) == Bpcert1Error::kOk,
          "P01 解析自己签发的证书成功");
    Check(parsed.server_id == cert.server_id &&
              parsed.server_public_key == cert.server_public_key &&
              parsed.issuer_id == cert.issuer_id &&
              parsed.serial_number == cert.serial_number &&
              parsed.not_before == cert.not_before &&
              parsed.not_after == cert.not_after &&
              parsed.signature.size() == 64,
          "P02 逐字段与签发输入一致");
    Check(off.signature + 64 == raw.size(), "P03 证书长度等于布局推导值",
          std::to_string(raw.size()) + " vs " +
              std::to_string(off.signature + 64));
    Check(backupproject::crypto::Bpcert1VerifySignature(raw, issuer_pub) ==
              Bpcert1Error::kOk,
          "P04 根公钥验签通过");
    std::string reencoded;
    std::string error;
    Bpcert1 again;
    backupproject::crypto::Bpcert1Parse(raw, &again);
    Check(backupproject::crypto::Bpcert1Encode(again, &reencoded, &error) &&
              reencoded == raw,
          "P05 解析后再编码逐字节相同（规范表示唯一）");
    const std::string body = backupproject::crypto::Bpcert1Body(raw);
    Check(body.size() + 64 == raw.size() &&
              body == raw.substr(0, raw.size() - 64) &&
              raw.substr(raw.size() - 64) == again.signature,
          "P06 body 与签名的切分正确");
    const std::string issued_again = IssueFixed();
    Check(issued_again == raw, "P07 相同输入重复签发逐字节相同（Ed25519 确定性）");
    Check(backupproject::crypto::Bpcert1Fingerprint(raw).size() == 64 &&
              backupproject::crypto::Bpcert1Fingerprint(raw) ==
                  backupproject::crypto::Sha256Hex(raw),
          "P08 指纹 = sha256(整张证书)");
    Check(backupproject::crypto::Bpcert1Describe(again).find(kServerId) !=
              std::string::npos,
          "P09 摘要包含 server_id");
  }

  // ---- T. 时间窗 ----
  {
    Bpcert1 parsed;
    backupproject::crypto::Bpcert1Parse(raw, &parsed);
    Bpcert1Error reason = Bpcert1Error::kOk;
    std::string message;
    const std::int64_t skew = backupproject::crypto::kBpcert1ClockSkewSeconds;
    Check(backupproject::crypto::Bpcert1CheckValidity(parsed, kNotBefore, &reason,
                                                      &message) &&
              reason == Bpcert1Error::kOk,
          "T01 恰好在 not_before 有效");
    Check(backupproject::crypto::Bpcert1CheckValidity(
              parsed, parsed.not_after, &reason, &message),
          "T02 恰好在 not_after 有效");
    Check(backupproject::crypto::Bpcert1CheckValidity(
              parsed, kNotBefore - skew, &reason, &message),
          "T03 not_before 前 5 分钟（容差内）仍然有效");
    Check(!backupproject::crypto::Bpcert1CheckValidity(
              parsed, kNotBefore - skew - 1, &reason, &message) &&
              reason == Bpcert1Error::kNotYetValid,
          "T04 not_before 前 5 分钟零 1 秒 -> not-yet-valid");
    Check(backupproject::crypto::Bpcert1CheckValidity(
              parsed, parsed.not_after + skew, &reason, &message),
          "T05 not_after 后 5 分钟（容差内）仍然有效");
    Check(!backupproject::crypto::Bpcert1CheckValidity(
              parsed, parsed.not_after + skew + 1, &reason, &message) &&
              reason == Bpcert1Error::kExpired,
          "T06 not_after 后 5 分钟零 1 秒 -> expired");
    backupproject::crypto::Bpcert1CheckValidity(parsed, kNotBefore - 86400 * 30,
                                                &reason, &message);
    Check(message.find("时钟") != std::string::npos,
          "T07 时间差很远时给出\"检查系统时钟\"的提示", message);
    backupproject::crypto::Bpcert1CheckValidity(
        parsed, parsed.not_after + 86400 * 30, &reason, &message);
    Check(message.find("时钟") != std::string::npos,
          "T08 过期很久时同样提示检查系统时钟", message);
  }

  // ---- X. 六类畸形 ----
  {
    // X01 截断：所有前缀都必须被拒，且原因是 truncated。
    bool all_truncated = true;
    std::string first_bad;
    for (std::size_t len = 0; len < raw.size(); ++len) {
      Bpcert1 parsed;
      const Bpcert1Error e =
          backupproject::crypto::Bpcert1Parse(raw.substr(0, len), &parsed);
      if (e != Bpcert1Error::kTruncated) {
        all_truncated = false;
        if (first_bad.empty()) {
          first_bad = std::to_string(len) + "->" + Bpcert1ErrorName(e);
        }
      }
    }
    Check(all_truncated, "X01 所有前缀（0..n-1）都报 truncated", first_bad);

    // X02 超长
    CheckParseError(raw + std::string(4096, 'A'), Bpcert1Error::kOversized,
                    "X02a 超过 4096 字节 -> oversized");
    CheckParseError(raw + std::string(1, 'A'), Bpcert1Error::kTrailingGarbage,
                    "X02b 追加 1 字节 -> trailing-garbage");
    CheckParseError(raw + std::string(63, 'A'), Bpcert1Error::kTrailingGarbage,
                    "X02c 追加 63 字节 -> trailing-garbage");

    // X03 魔数
    CheckParseError(Mutate(raw, 0, 'X'), Bpcert1Error::kBadMagic,
                    "X03a 魔数首字节被改 -> bad-magic");
    CheckParseError(Mutate(raw, 6, 'X'), Bpcert1Error::kBadMagic,
                    "X03b 魔数末字节被改 -> bad-magic");

    // X04 版本
    {
      std::string t0 = raw;
      PutU16(&t0, 7, 0);
      CheckParseError(t0, Bpcert1Error::kUnknownVersion, "X04a 版本 0 -> unknown-version");
      std::string t2 = raw;
      PutU16(&t2, 7, 2);
      CheckParseError(t2, Bpcert1Error::kUnknownVersion, "X04b 版本 2 -> unknown-version");
      std::string t3 = raw;
      PutU16(&t3, 7, 0xFFFF);
      CheckParseError(t3, Bpcert1Error::kUnknownVersion,
                      "X04c 版本 0xFFFF -> unknown-version");
    }

    // X05 算法
    CheckParseError(Mutate(raw, off.key_alg, 0), Bpcert1Error::kUnsupportedAlgorithm,
                    "X05a 公钥算法 0 -> unsupported-algorithm");
    CheckParseError(Mutate(raw, off.key_alg, 2), Bpcert1Error::kUnsupportedAlgorithm,
                    "X05b 公钥算法 2 -> unsupported-algorithm");
    CheckParseError(Mutate(raw, off.sig_alg, 2), Bpcert1Error::kUnsupportedAlgorithm,
                    "X05c 签名算法 2 -> unsupported-algorithm");

    // X06 用途
    CheckParseError(Mutate(raw, off.key_usage, 0),
                    Bpcert1Error::kUnsupportedKeyUsage,
                    "X06a 用途 0 -> unsupported-key-usage");
    CheckParseError(Mutate(raw, off.key_usage, 2),
                    Bpcert1Error::kUnsupportedKeyUsage,
                    "X06b 用途 2 -> unsupported-key-usage");

    // X07 长度不合法
    {
      std::string z = raw;
      PutU16(&z, 9, 0);
      CheckParseError(z, Bpcert1Error::kMalformedLength, "X07a server_id_len=0 -> malformed-length");
      std::string big = raw;
      PutU16(&big, 9, 129);
      CheckParseError(big, Bpcert1Error::kMalformedLength, "X07b server_id_len=129 -> malformed-length");
      std::string huge = raw;
      PutU16(&huge, 9, 0xFFFF);
      CheckParseError(huge, Bpcert1Error::kMalformedLength,
                      "X07c server_id_len=0xFFFF -> malformed-length");
      std::string iz = raw;
      PutU16(&iz, off.issuer_len, 0);
      CheckParseError(iz, Bpcert1Error::kMalformedLength,
                      "X07d issuer_id_len=0 -> malformed-length");
      std::string ibig = raw;
      PutU16(&ibig, off.issuer_len, 200);
      CheckParseError(ibig, Bpcert1Error::kMalformedLength,
                      "X07e issuer_id_len=200 -> malformed-length");
    }
  }

  // ---- F. 字段语义 ----
  {
    std::string zero_serial = raw;
    PutU64(&zero_serial, off.serial, 0);
    CheckParseError(zero_serial, Bpcert1Error::kBadFieldValue,
                    "F01 序列号 0 -> bad-field-value");

    std::string zero_before = raw;
    PutU64(&zero_before, off.not_before, 0);
    CheckParseError(zero_before, Bpcert1Error::kBadFieldValue,
                    "F02 not_before=0 -> bad-field-value");

    std::string equal_times = raw;
    PutU64(&equal_times, off.not_after, static_cast<std::uint64_t>(kNotBefore));
    CheckParseError(equal_times, Bpcert1Error::kBadFieldValue,
                    "F03 not_after == not_before -> bad-field-value");

    std::string reversed = raw;
    PutU64(&reversed, off.not_after,
           static_cast<std::uint64_t>(kNotBefore - 10));
    CheckParseError(reversed, Bpcert1Error::kBadFieldValue,
                    "F04 not_after < not_before -> bad-field-value");

    std::string too_long = raw;
    PutU64(&too_long, off.not_after,
           static_cast<std::uint64_t>(kNotBefore + 11LL * 366 * 24 * 60 * 60));
    CheckParseError(too_long, Bpcert1Error::kBadFieldValue,
                    "F05 有效期窗口 11 年 -> bad-field-value");

    std::string negative_time = raw;
    PutU64(&negative_time, off.not_before, 0xFFFFFFFFFFFFFFFFULL);
    CheckParseError(negative_time, Bpcert1Error::kBadFieldValue,
                    "F06 时间最高位为 1 -> bad-field-value");

    std::string control_char = Mutate(raw, off.server_id + 3, 0x00);
    CheckParseError(control_char, Bpcert1Error::kBadFieldValue,
                    "F07 server_id 含 NUL -> bad-field-value");
    std::string high_byte = Mutate(raw, off.server_id + 3, 0xA0);
    CheckParseError(high_byte, Bpcert1Error::kBadFieldValue,
                    "F08 server_id 含高位字节 -> bad-field-value");
    std::string issuer_control = Mutate(raw, off.issuer + 1, 0x1F);
    CheckParseError(issuer_control, Bpcert1Error::kBadFieldValue,
                    "F09 issuer_id 含控制字符 -> bad-field-value");
  }

  // ---- S. 签名 ----
  {
    Check(backupproject::crypto::Bpcert1VerifySignature(raw, other_pub) ==
              Bpcert1Error::kSignatureInvalid,
          "S01 换一把根公钥 -> signature-invalid");
    Check(backupproject::crypto::Bpcert1VerifySignature(raw, issuer_pub.substr(0, 31)) ==
              Bpcert1Error::kBadIssuerKeySize,
          "S02 根公钥长度不对 -> bad-issuer-key-size");
    Check(backupproject::crypto::Bpcert1VerifySignature(
              Mutate(raw, off.signature, static_cast<unsigned char>(raw[off.signature] ^ 0x01)),
              issuer_pub) == Bpcert1Error::kSignatureInvalid,
          "S03 签名翻一个 bit -> signature-invalid");
    Check(backupproject::crypto::Bpcert1VerifySignature(
              Mutate(raw, off.public_key, 0xFF), issuer_pub) ==
              Bpcert1Error::kSignatureInvalid,
          "S04 改动被签名的服务器公钥 -> signature-invalid");
    Check(backupproject::crypto::Bpcert1VerifySignature(
              Mutate(raw, off.serial, 0x7F), issuer_pub) ==
              Bpcert1Error::kSignatureInvalid,
          "S05 改动序列号 -> signature-invalid");
  }

  // ---- M. 单字节扫描：任何一处改动都不能通过完整验签 ----
  {
    int survived = 0;
    std::string first_survivor;
    const unsigned char masks[3] = {0x01, 0x80, 0xFF};
    for (std::size_t i = 0; i < raw.size(); ++i) {
      for (const unsigned char mask : masks) {
        const std::string mutated =
            Mutate(raw, i, static_cast<unsigned char>(raw[i] ^ mask));
        if (backupproject::crypto::Bpcert1VerifySignature(mutated, issuer_pub) ==
            Bpcert1Error::kOk) {
          ++survived;
          if (first_survivor.empty()) {
            first_survivor = "offset " + std::to_string(i);
          }
        }
      }
    }
    Check(survived == 0, "M01 单字节变异扫描全部被拒（" +
                             std::to_string(raw.size() * 3) + " 例）",
          first_survivor);
  }

  // ---- Z. fuzz ----
  {
    int accepted_random = 0;
    for (int i = 0; i < 10000; ++i) {
      const std::size_t length = static_cast<std::size_t>(NextRandom() % 600);
      std::string blob;
      blob.resize(length);
      for (std::size_t j = 0; j < length; ++j) {
        blob[j] = static_cast<char>(NextRandom() & 0xFF);
      }
      Bpcert1 parsed;
      if (backupproject::crypto::Bpcert1Parse(blob, &parsed) == Bpcert1Error::kOk) {
        ++accepted_random;
      }
    }
    Check(accepted_random == 0,
          "Z01 10000 条随机字节串没有一条被当成有效证书",
          std::to_string(accepted_random));

    int fuzz_survived = 0;
    for (int i = 0; i < 10000; ++i) {
      const std::size_t offset =
          static_cast<std::size_t>(NextRandom() % raw.size());
      // 必须保证真的改了：直接随机一个字节，有 1/256 的概率与原值相同，
      // 那时"变异体"其实等于原文，通过验签本来就是对的（第一版这么写，
      // 10000 例里 33 例"通过"全是这种假阳性）。改成异或一个非零增量。
      const unsigned char delta =
          static_cast<unsigned char>(1 + (NextRandom() % 255));
      const std::string mutated =
          Mutate(raw, offset, static_cast<unsigned char>(raw[offset] ^ delta));
      if (backupproject::crypto::Bpcert1VerifySignature(mutated, issuer_pub) ==
          Bpcert1Error::kOk) {
        ++fuzz_survived;
      }
    }
    Check(fuzz_survived == 0,
          "Z02 10000 条随机单字节变异没有一条通过完整验签",
          std::to_string(fuzz_survived));
  }

  std::printf("[bpcert] passed=%d failed=%d\n", g_passed, g_failed);
  return g_failed == 0 ? 0 : 1;
}
