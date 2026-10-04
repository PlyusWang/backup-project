// src/crypto/hkdf.cpp
//
// 手写 HKDF-SHA256（RFC 5869）。实现只用到 include/crypto.h 里的
// HmacSha256（本项目自己的手写 HMAC）。

#include "hkdf.h"

#include "crypto.h"

namespace backupproject {
namespace crypto {
namespace {

void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) {
    *error_message = text;
  }
}

}  // namespace

bool HkdfExtract(const std::string& salt, const std::string& ikm,
                 std::string* prk, std::string* error_message) {
  if (prk == nullptr) {
    SetError(error_message, "HKDF-Extract 的输出指针为空");
    return false;
  }
  // RFC 5869 §2.2：salt 缺省时用 HashLen 个 0 字节。
  // HMAC 对空密钥与对 32 个 0 字节的密钥结果相同（都会先补零到 64 字节），
  // 这里显式写出来是为了让"等价于标准定义"这件事在代码里可见。
  const std::string effective_salt =
      salt.empty() ? std::string(kSha256DigestSize, '\0') : salt;
  unsigned char digest[kSha256DigestSize];
  HmacSha256::Compute(effective_salt.data(), effective_salt.size(), ikm.data(),
                      ikm.size(), digest);
  prk->assign(reinterpret_cast<const char*>(digest), sizeof(digest));
  return true;
}

bool HkdfExpand(const std::string& prk, const std::string& info,
                std::size_t length, std::string* okm,
                std::string* error_message) {
  if (okm == nullptr) {
    SetError(error_message, "HKDF-Expand 的输出指针为空");
    return false;
  }
  okm->clear();
  if (length > kHkdfMaxOutputBytes) {
    SetError(error_message, "HKDF-Expand 的输出长度超过 255 * HashLen（请求 " +
                                std::to_string(length) + " 字节）");
    return false;
  }
  std::string previous;
  std::string output;
  output.reserve(length);
  std::size_t counter = 1;
  while (output.size() < length) {
    HmacSha256 mac(prk);
    mac.Update(previous.data(), previous.size());
    mac.Update(info.data(), info.size());
    const unsigned char index = static_cast<unsigned char>(counter);
    mac.Update(&index, 1);
    unsigned char digest[kSha256DigestSize];
    mac.Final(digest);
    previous.assign(reinterpret_cast<const char*>(digest), sizeof(digest));
    const std::size_t need = length - output.size();
    output.append(previous.data(),
                  need < previous.size() ? need : previous.size());
    counter += 1;
  }
  *okm = output;
  return true;
}

bool Hkdf(const std::string& salt, const std::string& ikm,
          const std::string& info, std::size_t length, std::string* okm,
          std::string* error_message) {
  std::string prk;
  if (!HkdfExtract(salt, ikm, &prk, error_message)) {
    return false;
  }
  return HkdfExpand(prk, info, length, okm, error_message);
}

}  // namespace crypto
}  // namespace backupproject
