// src/crypto/bpcert.cpp
//
// BPCERT1 的实现：规范编码 + 严格解析（布局与理由见 include/bpcert.h）。
//
// 解析器的原则：**先验证，再前进**。每一步都先确认"要读的字节真的存在"
// 再读，读完立刻推进偏移；任何不合规立刻返回具名错误，不写出半个结果。
// 这样"截断 / 超长 / 未知版本 / 未知算法 / 长度不合法 / 尾部多余字节"
// 六类畸形输入都有各自的原因，而不是笼统的 false。

#include "bpcert.h"

#include <chrono>
#include <cstring>

#include "crypto.h"
#include "ed25519.h"

namespace backupproject {
namespace crypto {
namespace {

constexpr char kMagic[7] = {'B', 'P', 'C', 'E', 'R', 'T', '1'};
constexpr std::uint16_t kFormatVersion = 1;
constexpr std::uint8_t kPublicKeyAlgorithmX25519 = 1;
constexpr std::uint8_t kKeyUsageServerAuth = 1;
constexpr std::uint8_t kSignatureAlgorithmEd25519 = 1;

// body 的最小长度：server_id 与 issuer_id 都取最短的 1 字节。
constexpr std::size_t kMinBodySize = sizeof(kMagic) + 2 + 2 + 1 + 1 +
                                     kBpcert1PublicKeySize + 8 + 8 + 8 + 2 + 1 +
                                     1 + 1;

void SetError(std::string* error_message, const char* text) {
  if (error_message != nullptr) {
    *error_message = text;
  }
}

// 一律大端（"网络字节序"）：格式只有一种表示，不依赖主机字节序。
void AppendU16(std::string* out, std::uint16_t value) {
  out->push_back(static_cast<char>((value >> 8) & 0xFF));
  out->push_back(static_cast<char>(value & 0xFF));
}

void AppendU64(std::string* out, std::uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out->push_back(static_cast<char>((value >> shift) & 0xFF));
  }
}

std::uint16_t ReadU16(const unsigned char* p) {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) |
                                    static_cast<std::uint16_t>(p[1]));
}

std::uint64_t ReadU64(const unsigned char* p) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value = (value << 8) | static_cast<std::uint64_t>(p[i]);
  }
  return value;
}

// 一个字段的"读之前先确认存在"。
bool Fits(std::size_t offset, std::size_t need, std::size_t size) {
  return offset + need <= size;
}

}  // namespace

const char* Bpcert1ErrorName(Bpcert1Error error) {
  switch (error) {
    case Bpcert1Error::kOk:
      return "ok";
    case Bpcert1Error::kBadArgument:
      return "bad-argument";
    case Bpcert1Error::kTruncated:
      return "truncated";
    case Bpcert1Error::kOversized:
      return "oversized";
    case Bpcert1Error::kBadMagic:
      return "bad-magic";
    case Bpcert1Error::kUnknownVersion:
      return "unknown-version";
    case Bpcert1Error::kUnsupportedAlgorithm:
      return "unsupported-algorithm";
    case Bpcert1Error::kUnsupportedKeyUsage:
      return "unsupported-key-usage";
    case Bpcert1Error::kMalformedLength:
      return "malformed-length";
    case Bpcert1Error::kTrailingGarbage:
      return "trailing-garbage";
    case Bpcert1Error::kBadFieldValue:
      return "bad-field-value";
    case Bpcert1Error::kBadIssuerKeySize:
      return "bad-issuer-key-size";
    case Bpcert1Error::kSignatureInvalid:
      return "signature-invalid";
    case Bpcert1Error::kNotYetValid:
      return "not-yet-valid";
    case Bpcert1Error::kExpired:
      return "expired";
  }
  return "unknown";
}

const char* Bpcert1ErrorMessage(Bpcert1Error error) {
  switch (error) {
    case Bpcert1Error::kOk:
      return "证书有效";
    case Bpcert1Error::kBadArgument:
      return "证书字段不合法（长度、时间窗或序列号）";
    case Bpcert1Error::kTruncated:
      return "证书数据不完整（被截断）";
    case Bpcert1Error::kOversized:
      return "证书数据超出 4096 字节上限";
    case Bpcert1Error::kBadMagic:
      return "不是 BPCERT1 证书（魔数不匹配）";
    case Bpcert1Error::kUnknownVersion:
      return "证书版本不受支持，请升级客户端";
    case Bpcert1Error::kUnsupportedAlgorithm:
      return "证书使用了不受支持的算法";
    case Bpcert1Error::kUnsupportedKeyUsage:
      return "证书用途不是服务器身份认证";
    case Bpcert1Error::kMalformedLength:
      return "证书字段长度不合法";
    case Bpcert1Error::kTrailingGarbage:
      return "证书签名之后还有多余字节";
    case Bpcert1Error::kBadFieldValue:
      return "证书字段取值不合法（标识符或时间窗）";
    case Bpcert1Error::kBadIssuerKeySize:
      return "签发者公钥长度不是 32 字节";
    case Bpcert1Error::kSignatureInvalid:
      return "证书签名无效：这张证书不是该根签发的";
    case Bpcert1Error::kNotYetValid:
      return "证书尚未生效";
    case Bpcert1Error::kExpired:
      return "证书已过期";
  }
  return "证书校验失败";
}

bool Bpcert1IsValidIdentity(const std::string& text) {
  if (text.empty() || text.size() > kBpcert1MaxIdentitySize) {
    return false;
  }
  for (const char raw : text) {
    const unsigned char c = static_cast<unsigned char>(raw);
    if (c < 0x20 || c > 0x7E) {
      return false;  // 控制字符（含 NUL）与高位字节一律拒绝
    }
  }
  return true;
}

bool Bpcert1EncodeUnsigned(const Bpcert1& certificate, std::string* out,
                           std::string* error_message) {
  if (out == nullptr) {
    SetError(error_message, "输出指针为空");
    return false;
  }
  out->clear();
  if (!Bpcert1IsValidIdentity(certificate.server_id)) {
    SetError(error_message, "server_id 必须是 1..128 字节的可打印 ASCII");
    return false;
  }
  if (!Bpcert1IsValidIdentity(certificate.issuer_id)) {
    SetError(error_message, "issuer_id 必须是 1..128 字节的可打印 ASCII");
    return false;
  }
  if (certificate.server_public_key.size() != kBpcert1PublicKeySize) {
    SetError(error_message, "服务器公钥必须是 32 字节");
    return false;
  }
  if (certificate.serial_number == 0) {
    SetError(error_message, "序列号不能为 0");
    return false;
  }
  if (certificate.not_before <= 0 || certificate.not_after <= 0) {
    SetError(error_message, "生效/失效时间必须是正的 Unix 秒");
    return false;
  }
  if (certificate.not_after <= certificate.not_before) {
    SetError(error_message, "失效时间必须晚于生效时间");
    return false;
  }
  if (certificate.not_after - certificate.not_before >
      kBpcert1MaxValiditySeconds) {
    SetError(error_message, "有效期窗口超过 10 年上限");
    return false;
  }

  std::string body;
  body.reserve(kMinBodySize + certificate.server_id.size() +
               certificate.issuer_id.size());
  body.append(kMagic, sizeof(kMagic));
  AppendU16(&body, kFormatVersion);
  AppendU16(&body, static_cast<std::uint16_t>(certificate.server_id.size()));
  body.append(certificate.server_id);
  body.push_back(static_cast<char>(kPublicKeyAlgorithmX25519));
  body.append(certificate.server_public_key);
  AppendU64(&body, certificate.serial_number);
  AppendU64(&body, static_cast<std::uint64_t>(certificate.not_before));
  AppendU64(&body, static_cast<std::uint64_t>(certificate.not_after));
  AppendU16(&body, static_cast<std::uint16_t>(certificate.issuer_id.size()));
  body.append(certificate.issuer_id);
  body.push_back(static_cast<char>(kKeyUsageServerAuth));
  body.push_back(static_cast<char>(kSignatureAlgorithmEd25519));

  if (body.size() + kBpcert1SignatureSize > kBpcert1MaxCertificateSize) {
    SetError(error_message, "证书超过 4096 字节上限");
    return false;
  }
  *out = body;
  return true;
}

bool Bpcert1Encode(const Bpcert1& certificate, std::string* out,
                   std::string* error_message) {
  if (out == nullptr) {
    SetError(error_message, "输出指针为空");
    return false;
  }
  out->clear();
  if (certificate.signature.size() != kBpcert1SignatureSize) {
    SetError(error_message, "签名必须是 64 字节");
    return false;
  }
  std::string body;
  if (!Bpcert1EncodeUnsigned(certificate, &body, error_message)) {
    return false;
  }
  body.append(certificate.signature);
  *out = body;
  return true;
}

bool Bpcert1Issue(const Bpcert1& unsigned_certificate,
                  const std::string& issuer_seed, std::string* out,
                  std::string* error_message) {
  if (out == nullptr) {
    SetError(error_message, "输出指针为空");
    return false;
  }
  out->clear();
  Bpcert1 filled = unsigned_certificate;
  if (filled.not_before == 0) {
    filled.not_before = Bpcert1NowUnixSeconds();
  }
  if (filled.not_after == 0) {
    filled.not_after = filled.not_before + kBpcert1OfficialValiditySeconds;
  }
  std::string body;
  if (!Bpcert1EncodeUnsigned(filled, &body, error_message)) {
    return false;
  }
  std::string signature;
  if (!Ed25519Sign(issuer_seed, body.data(), body.size(), &signature,
                   error_message)) {
    return false;
  }
  if (signature.size() != kBpcert1SignatureSize) {
    SetError(error_message, "签名长度异常");
    return false;
  }
  body.append(signature);
  *out = body;
  return true;
}

Bpcert1Error Bpcert1Parse(const std::string& raw, Bpcert1* out) {
  if (out == nullptr) {
    return Bpcert1Error::kBadArgument;
  }
  const std::size_t size = raw.size();
  if (size > kBpcert1MaxCertificateSize) {
    return Bpcert1Error::kOversized;
  }
  if (size < kMinBodySize + kBpcert1SignatureSize) {
    return Bpcert1Error::kTruncated;
  }
  const unsigned char* p = reinterpret_cast<const unsigned char*>(raw.data());
  std::size_t off = 0;

  if (std::memcmp(p, kMagic, sizeof(kMagic)) != 0) {
    return Bpcert1Error::kBadMagic;
  }
  off += sizeof(kMagic);

  if (!Fits(off, 2, size)) return Bpcert1Error::kTruncated;
  if (ReadU16(p + off) != kFormatVersion) {
    return Bpcert1Error::kUnknownVersion;
  }
  off += 2;

  if (!Fits(off, 2, size)) return Bpcert1Error::kTruncated;
  const std::size_t server_id_size = ReadU16(p + off);
  off += 2;
  if (server_id_size == 0 || server_id_size > kBpcert1MaxIdentitySize) {
    return Bpcert1Error::kMalformedLength;
  }
  if (!Fits(off, server_id_size, size)) return Bpcert1Error::kTruncated;
  const std::string server_id(reinterpret_cast<const char*>(p + off),
                              server_id_size);
  off += server_id_size;
  if (!Bpcert1IsValidIdentity(server_id)) {
    return Bpcert1Error::kBadFieldValue;
  }

  if (!Fits(off, 1, size)) return Bpcert1Error::kTruncated;
  if (p[off] != kPublicKeyAlgorithmX25519) {
    return Bpcert1Error::kUnsupportedAlgorithm;
  }
  off += 1;

  if (!Fits(off, kBpcert1PublicKeySize, size)) return Bpcert1Error::kTruncated;
  const std::string server_public_key(
      reinterpret_cast<const char*>(p + off), kBpcert1PublicKeySize);
  off += kBpcert1PublicKeySize;

  if (!Fits(off, 8, size)) return Bpcert1Error::kTruncated;
  const std::uint64_t serial_number = ReadU64(p + off);
  off += 8;

  if (!Fits(off, 8, size)) return Bpcert1Error::kTruncated;
  const std::uint64_t not_before_raw = ReadU64(p + off);
  off += 8;

  if (!Fits(off, 8, size)) return Bpcert1Error::kTruncated;
  const std::uint64_t not_after_raw = ReadU64(p + off);
  off += 8;

  if (!Fits(off, 2, size)) return Bpcert1Error::kTruncated;
  const std::size_t issuer_id_size = ReadU16(p + off);
  off += 2;
  if (issuer_id_size == 0 || issuer_id_size > kBpcert1MaxIdentitySize) {
    return Bpcert1Error::kMalformedLength;
  }
  if (!Fits(off, issuer_id_size, size)) return Bpcert1Error::kTruncated;
  const std::string issuer_id(reinterpret_cast<const char*>(p + off),
                              issuer_id_size);
  off += issuer_id_size;
  if (!Bpcert1IsValidIdentity(issuer_id)) {
    return Bpcert1Error::kBadFieldValue;
  }

  if (!Fits(off, 1, size)) return Bpcert1Error::kTruncated;
  if (p[off] != kKeyUsageServerAuth) {
    return Bpcert1Error::kUnsupportedKeyUsage;
  }
  off += 1;

  if (!Fits(off, 1, size)) return Bpcert1Error::kTruncated;
  if (p[off] != kSignatureAlgorithmEd25519) {
    return Bpcert1Error::kUnsupportedAlgorithm;
  }
  off += 1;

  if (!Fits(off, kBpcert1SignatureSize, size)) {
    return Bpcert1Error::kTruncated;
  }
  const std::string signature(reinterpret_cast<const char*>(p + off),
                              kBpcert1SignatureSize);
  off += kBpcert1SignatureSize;

  // 尾部不允许有任何字节：签名不覆盖 body 之外的字节，留着就会出现
  // "同一张证书、两种字节表示"，也会给指纹/缓存带来歧义。
  if (off != size) {
    return Bpcert1Error::kTrailingGarbage;
  }

  // 上面只保证"结构合法"，这里再判字段语义。
  constexpr std::uint64_t kMaxInt64 =
      static_cast<std::uint64_t>(0x7FFFFFFFFFFFFFFFULL);
  if (serial_number == 0) {
    return Bpcert1Error::kBadFieldValue;
  }
  if (not_before_raw == 0 || not_after_raw == 0 ||
      not_before_raw > kMaxInt64 || not_after_raw > kMaxInt64) {
    return Bpcert1Error::kBadFieldValue;
  }
  const std::int64_t not_before = static_cast<std::int64_t>(not_before_raw);
  const std::int64_t not_after = static_cast<std::int64_t>(not_after_raw);
  if (not_after <= not_before) {
    return Bpcert1Error::kBadFieldValue;
  }
  if (not_after - not_before > kBpcert1MaxValiditySeconds) {
    return Bpcert1Error::kBadFieldValue;
  }

  out->server_id = server_id;
  out->server_public_key = server_public_key;
  out->issuer_id = issuer_id;
  out->serial_number = serial_number;
  out->not_before = not_before;
  out->not_after = not_after;
  out->signature = signature;
  return Bpcert1Error::kOk;
}

std::string Bpcert1Body(const std::string& raw) {
  if (raw.size() <= kBpcert1SignatureSize) {
    return std::string();
  }
  // 签名是布局里的最后一个字段，所以 body 就是"去掉尾部 64 字节"。
  return raw.substr(0, raw.size() - kBpcert1SignatureSize);
}

Bpcert1Error Bpcert1VerifySignature(const std::string& raw,
                                    const std::string& issuer_public_key) {
  if (issuer_public_key.size() != kEd25519PublicKeySize) {
    return Bpcert1Error::kBadIssuerKeySize;
  }
  Bpcert1 parsed;
  const Bpcert1Error parsed_result = Bpcert1Parse(raw, &parsed);
  if (parsed_result != Bpcert1Error::kOk) {
    return parsed_result;
  }
  const std::string body = Bpcert1Body(raw);
  if (!Ed25519Verify(issuer_public_key, body.data(), body.size(),
                     parsed.signature, nullptr)) {
    return Bpcert1Error::kSignatureInvalid;
  }
  return Bpcert1Error::kOk;
}

bool Bpcert1CheckValidity(const Bpcert1& certificate,
                          std::int64_t now_unix_seconds,
                          Bpcert1Error* error_result, std::string* message) {
  const std::int64_t skew = kBpcert1ClockSkewSeconds;
  if (now_unix_seconds + skew < certificate.not_before) {
    if (error_result != nullptr) *error_result = Bpcert1Error::kNotYetValid;
    if (message != nullptr) {
      const bool far_future =
          certificate.not_before - now_unix_seconds > 24 * 60 * 60;
      *message = far_future
                     ? "证书生效时间在未来很久，本机系统时钟很可能不正确"
                     : "证书尚未生效，请检查本机系统时钟";
    }
    return false;
  }
  if (now_unix_seconds > certificate.not_after + skew) {
    if (error_result != nullptr) *error_result = Bpcert1Error::kExpired;
    if (message != nullptr) {
      const bool far_past =
          now_unix_seconds - certificate.not_after > 24 * 60 * 60;
      *message = far_past
                     ? "证书已过期很久；若确定它刚签发，请检查本机系统时钟"
                     : "证书已过期，需要重新签发";
    }
    return false;
  }
  if (error_result != nullptr) *error_result = Bpcert1Error::kOk;
  if (message != nullptr) *message = "证书在有效期内";
  return true;
}

std::int64_t Bpcert1NowUnixSeconds() {
  return static_cast<std::int64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::string Bpcert1Fingerprint(const std::string& raw_certificate) {
  if (raw_certificate.empty()) {
    return std::string();
  }
  return Sha256Hex(raw_certificate);
}

std::string Bpcert1Describe(const Bpcert1& certificate) {
  std::string text = "BPCERT1 server_id=" + certificate.server_id;
  text += " serial=" + std::to_string(certificate.serial_number);
  text += " not_before=" + std::to_string(certificate.not_before);
  text += " not_after=" + std::to_string(certificate.not_after);
  text += " issuer_id=" + certificate.issuer_id;
  if (certificate.server_public_key.size() == kBpcert1PublicKeySize) {
    text += " key_sha256=" + Sha256Hex(certificate.server_public_key).substr(0, 16);
  }
  return text;
}

}  // namespace crypto
}  // namespace backupproject
