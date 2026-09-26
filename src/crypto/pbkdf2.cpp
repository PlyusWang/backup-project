// src/crypto/pbkdf2.cpp
//
// PBKDF2-HMAC-SHA256（RFC 8018 5.2，PRF 为 HMAC-SHA256）。
//
// 实现注意：
//   * salt || INT(i) 只用一块缓冲，U/T 都是栈上定长数组，迭代几百万次也不会
//     在循环里分配内存。
//   * 迭代次数为 0 是非法的（标准要求 c >= 1），这里显式失败而不是当成 c = 1，
//     否则调用方一个笔误就会静默退化成"只迭代一次"。
//   * 中间量（U、T、salt 块）用完即清零。

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

#include "crypto.h"

namespace backupproject {
namespace crypto {
namespace {

void SecureZero(void* data, std::size_t size) {
  volatile unsigned char* p = static_cast<volatile unsigned char*>(data);
  while (size-- > 0) *p++ = 0;
}

}  // namespace

bool Pbkdf2HmacSha256(const std::string& password, const std::string& salt,
                      std::uint32_t iterations, std::size_t derived_key_size,
                      std::string* out, std::string* error_message) {
  const auto fail = [error_message](const char* message) {
    if (error_message != nullptr) *error_message = message;
    return false;
  };
  if (error_message != nullptr) error_message->clear();
  if (out == nullptr) return fail("PBKDF2: 输出指针为空");
  out->clear();
  if (iterations == 0) return fail("PBKDF2: 迭代次数必须大于 0");
  if (derived_key_size == 0) return fail("PBKDF2: 派生密钥长度必须大于 0");
  // 上界来自 RFC 8018：(2^32 - 1) 个分组，每块 32 字节。先用 64 位比较，
  // 32 位平台上 size_t 也放得下，不会因为乘法回绕而误判。
  const std::uint64_t max_size =
      static_cast<std::uint64_t>(0xFFFFFFFFu) * kSha256DigestSize;
  if (static_cast<std::uint64_t>(derived_key_size) > max_size) {
    return fail("PBKDF2: 派生密钥长度超出上限");
  }

  const std::size_t blocks =
      (derived_key_size + kSha256DigestSize - 1) / kSha256DigestSize;
  // salt || INT(i)：循环外分配一次，之后只改最后 4 个字节。
  std::vector<unsigned char> block_input(salt.size() + 4);
  if (!salt.empty()) {
    std::memcpy(block_input.data(), salt.data(), salt.size());
  }

  const unsigned char* password_data =
      reinterpret_cast<const unsigned char*>(password.data());
  const std::size_t password_size = password.size();

  std::string result;
  result.resize(derived_key_size);
  std::size_t written = 0;

  unsigned char u[kSha256DigestSize];
  unsigned char next[kSha256DigestSize];
  unsigned char t[kSha256DigestSize];
  // U_j 只用这两块缓冲轮换，避免"边读边写"相互覆盖。
  unsigned char* const chain[2] = {u, next};
  for (std::size_t block = 1; block <= blocks; ++block) {
    const std::uint32_t index = static_cast<std::uint32_t>(block);
    block_input[salt.size()] = static_cast<unsigned char>(index >> 24);
    block_input[salt.size() + 1] = static_cast<unsigned char>(index >> 16);
    block_input[salt.size() + 2] = static_cast<unsigned char>(index >> 8);
    block_input[salt.size() + 3] = static_cast<unsigned char>(index);

    // U_1 = PRF(P, S || INT(i))，T = U_1。
    HmacSha256::Compute(password_data, password_size, block_input.data(),
                        block_input.size(), chain[0]);
    std::memcpy(t, chain[0], kSha256DigestSize);

    // U_j = PRF(P, U_{j-1})，T 逐轮异或；下标奇偶决定读写哪一块缓冲。
    for (std::uint32_t iteration = 1; iteration < iterations; ++iteration) {
      const std::size_t parity = (iteration - 1) % 2;
      unsigned char* const target = chain[1 - parity];
      const unsigned char* const source = chain[parity];
      HmacSha256::Compute(password_data, password_size, source,
                          kSha256DigestSize, target);
      for (std::size_t i = 0; i < kSha256DigestSize; ++i) t[i] ^= target[i];
    }

    const std::size_t take =
        std::min(kSha256DigestSize, derived_key_size - written);
    std::memcpy(&result[written], t, take);
    written += take;
  }

  SecureZero(u, sizeof(u));
  SecureZero(next, sizeof(next));
  SecureZero(t, sizeof(t));
  SecureZero(block_input.data(), block_input.size());
  *out = std::move(result);
  return true;
}

}  // namespace crypto
}  // namespace backupproject
