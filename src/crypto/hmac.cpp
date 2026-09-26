// src/crypto/hmac.cpp
//
// HMAC-SHA256（RFC 2104）+ 定长比较。
//
// 流式实现的关键：构造时就把 ipad/opad 吸收进内层/外层哈希状态，之后 Update
// 大 payload 只做一次内层扫描；Final 才补上外层。这样把"归档头 + 大文件内容"
// 分多次喂进来，代价与一次性计算相同。
//
// 比较函数放在这里是因为它唯一的用途就是校验 MAC：用 memcmp 校验标签会在
// 第一个不同字节处提前返回，攻击者可以用它逐字节把标签试出来。

#include <cstring>

#include "crypto.h"

namespace backupproject {
namespace crypto {
namespace {

// 用 volatile 写零，防止编译器把"马上要析构的密钥缓冲"优化掉。
void SecureZero(void* data, std::size_t size) {
  volatile unsigned char* p = static_cast<volatile unsigned char*>(data);
  while (size-- > 0) *p++ = 0;
}

}  // namespace

HmacSha256::HmacSha256(const void* key, std::size_t key_size) {
  // key == nullptr 时按空密钥处理：构造函数没有错误通道，但绝不能解引用空指针。
  if (key == nullptr) key_size = 0;

  unsigned char key_block[kSha256BlockSize];
  std::memset(key_block, 0, sizeof(key_block));
  if (key_size > kSha256BlockSize) {
    // 超过分组长度必须先哈希压缩成 32 字节，否则密钥会被截断。
    Sha256::Hash(key, key_size, key_block);
  } else if (key_size > 0) {
    std::memcpy(key_block, key, key_size);
  }

  unsigned char pad[kSha256BlockSize];
  for (std::size_t i = 0; i < kSha256BlockSize; ++i) {
    pad[i] = static_cast<unsigned char>(key_block[i] ^ 0x36u);
  }
  inner_.Update(pad, sizeof(pad));
  for (std::size_t i = 0; i < kSha256BlockSize; ++i) {
    pad[i] = static_cast<unsigned char>(key_block[i] ^ 0x5cu);
  }
  outer_.Update(pad, sizeof(pad));

  SecureZero(key_block, sizeof(key_block));
  SecureZero(pad, sizeof(pad));
}

HmacSha256::HmacSha256(const std::string& key)
    : HmacSha256(key.data(), key.size()) {}

void HmacSha256::Update(const void* data, std::size_t size) {
  inner_.Update(data, size);
}

void HmacSha256::Final(unsigned char out[kSha256DigestSize]) {
  if (out == nullptr) return;
  unsigned char inner_digest[kSha256DigestSize];
  inner_.Final(inner_digest);
  outer_.Update(inner_digest, sizeof(inner_digest));
  outer_.Final(out);
  SecureZero(inner_digest, sizeof(inner_digest));
}

void HmacSha256::Compute(const void* key, std::size_t key_size,
                         const void* data, std::size_t data_size,
                         unsigned char out[kSha256DigestSize]) {
  HmacSha256 hmac(key, key_size);
  hmac.Update(data, data_size);
  hmac.Final(out);
}

std::string HmacSha256Raw(const std::string& key, const std::string& data) {
  unsigned char digest[kSha256DigestSize];
  HmacSha256::Compute(key.data(), key.size(), data.data(), data.size(), digest);
  return std::string(reinterpret_cast<const char*>(digest), sizeof(digest));
}

std::string HmacSha256Hex(const std::string& key, const std::string& data) {
  unsigned char digest[kSha256DigestSize];
  HmacSha256::Compute(key.data(), key.size(), data.data(), data.size(), digest);
  return ToHex(digest, sizeof(digest));
}

bool ConstantTimeEquals(const void* a, const void* b, std::size_t size) {
  if (size == 0) return true;
  if (a == nullptr || b == nullptr) return false;
  const unsigned char* left = static_cast<const unsigned char*>(a);
  const unsigned char* right = static_cast<const unsigned char*>(b);
  // volatile 让累加真的发生在每一次迭代里：否则编译器有权在发现差异后提前
  // 跳出循环，比较时间就重新变成了与"第几个字节不同"相关。
  volatile unsigned char difference = 0;
  for (std::size_t i = 0; i < size; ++i) {
    difference = static_cast<unsigned char>(difference | (left[i] ^ right[i]));
  }
  return difference == 0;
}

bool ConstantTimeEquals(const std::string& a, const std::string& b) {
  // 长度本身不是秘密（标签长度在格式里是固定的），可以提前返回。
  if (a.size() != b.size()) return false;
  return ConstantTimeEquals(a.data(), b.data(), a.size());
}

}  // namespace crypto
}  // namespace backupproject
