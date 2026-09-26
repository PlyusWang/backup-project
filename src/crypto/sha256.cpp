// src/crypto/sha256.cpp
//
// SHA-256（FIPS 180-4）+ 本模块共用的十六进制工具。
//
// 设计要点：
//   * 流式：内部保留 64 字节分组缓冲，Update 可以任意切分；长度字段用 64 位
//     bit 计数（按 2^64 取模，和标准一致），因此在 32 位平台上也不会提前截断。
//   * 常量表全部写在源码里，没有魔数来自外部文件。

#include <cstring>

#include "crypto.h"

namespace backupproject {
namespace crypto {
namespace {

// 前 64 个素数立方根小数部分的前 32 位（FIPS 180-4 4.2.2）。
constexpr std::uint32_t kRoundConstants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
    0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

// 前 8 个素数平方根小数部分的前 32 位（FIPS 180-4 5.3.3）。
constexpr std::uint32_t kInitialState[8] = {
    0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

// bits 必须落在 [1, 31]：本文件里只用于 32 位循环移位。
inline std::uint32_t RotateRight(std::uint32_t value, unsigned bits) {
  return (value >> bits) | (value << (32u - bits));
}

int HexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

Sha256::Sha256() : state_{}, buffer_{}, buffer_size_(0), total_bits_(0) {
  std::memcpy(state_, kInitialState, sizeof(state_));
}

void Sha256::Transform(const unsigned char block[kSha256BlockSize]) {
  std::uint32_t w[64];
  for (std::size_t i = 0; i < 16; ++i) {
    w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
           (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
           static_cast<std::uint32_t>(block[i * 4 + 3]);
  }
  for (std::size_t i = 16; i < 64; ++i) {
    const std::uint32_t s0 = RotateRight(w[i - 15], 7) ^
                             RotateRight(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const std::uint32_t s1 = RotateRight(w[i - 2], 17) ^
                             RotateRight(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];
  for (std::size_t i = 0; i < 64; ++i) {
    const std::uint32_t sigma1 =
        RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
    const std::uint32_t choose = (e & f) ^ (~e & g);
    const std::uint32_t temp1 = h + sigma1 + choose + kRoundConstants[i] + w[i];
    const std::uint32_t sigma0 =
        RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = sigma0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }
  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::Update(const void* data, std::size_t size) {
  if (data == nullptr || size == 0) return;
  const unsigned char* p = static_cast<const unsigned char*>(data);
  // 长度字段是 bit 数，64 位无符号回绕即标准要求的 mod 2^64。
  total_bits_ += static_cast<std::uint64_t>(size) * 8u;

  if (buffer_size_ > 0) {
    const std::size_t need = kSha256BlockSize - buffer_size_;
    const std::size_t take = size < need ? size : need;
    std::memcpy(buffer_ + buffer_size_, p, take);
    buffer_size_ += take;
    p += take;
    size -= take;
    if (buffer_size_ == kSha256BlockSize) {
      Transform(buffer_);
      buffer_size_ = 0;
    }
  }
  while (size >= kSha256BlockSize) {
    Transform(p);
    p += kSha256BlockSize;
    size -= kSha256BlockSize;
  }
  if (size > 0) {
    std::memcpy(buffer_, p, size);
    buffer_size_ = size;
  }
}

void Sha256::Final(unsigned char out[kSha256DigestSize]) {
  if (out == nullptr) return;
  // 先记下真实长度：下面的补位会继续走 Update，从而让 total_bits_ 变大。
  const std::uint64_t message_bits = total_bits_;

  // 补位：0x80，然后补 0 到 56 (mod 64)，最后 8 字节大端 bit 长度。
  static const unsigned char kZeroes[64] = {0};
  const unsigned char one = 0x80;
  Update(&one, 1);
  const std::size_t pad_size =
      (buffer_size_ <= 56) ? (56 - buffer_size_) : (64 + 56 - buffer_size_);
  Update(kZeroes, pad_size);
  unsigned char length_bytes[8];
  for (std::size_t i = 0; i < 8; ++i) {
    length_bytes[i] =
        static_cast<unsigned char>(message_bits >> (56u - 8u * i));
  }
  Update(length_bytes, sizeof(length_bytes));

  for (std::size_t i = 0; i < 8; ++i) {
    out[i * 4] = static_cast<unsigned char>(state_[i] >> 24);
    out[i * 4 + 1] = static_cast<unsigned char>(state_[i] >> 16);
    out[i * 4 + 2] = static_cast<unsigned char>(state_[i] >> 8);
    out[i * 4 + 3] = static_cast<unsigned char>(state_[i]);
  }
}

void Sha256::Hash(const void* data, std::size_t size,
                  unsigned char out[kSha256DigestSize]) {
  Sha256 hasher;
  hasher.Update(data, size);
  hasher.Final(out);
}

std::string Sha256Raw(const std::string& data) {
  unsigned char digest[kSha256DigestSize];
  Sha256::Hash(data.data(), data.size(), digest);
  return std::string(reinterpret_cast<const char*>(digest), sizeof(digest));
}

std::string Sha256Hex(const std::string& data) {
  unsigned char digest[kSha256DigestSize];
  Sha256::Hash(data.data(), data.size(), digest);
  return ToHex(digest, sizeof(digest));
}

std::string ToHex(const unsigned char* data, std::size_t size) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  if (data == nullptr || size == 0) return out;
  if (size > out.max_size() / 2) return out;
  out.resize(size * 2);
  for (std::size_t i = 0; i < size; ++i) {
    out[i * 2] = kDigits[data[i] >> 4];
    out[i * 2 + 1] = kDigits[data[i] & 0x0fu];
  }
  return out;
}

bool FromHex(const std::string& hex, std::string* out) {
  if (out == nullptr) return false;
  if (hex.size() % 2 != 0) return false;
  std::string result;
  result.resize(hex.size() / 2);
  for (std::size_t i = 0; i < result.size(); ++i) {
    const int high = HexValue(hex[i * 2]);
    const int low = HexValue(hex[i * 2 + 1]);
    if (high < 0 || low < 0) return false;
    result[i] = static_cast<char>((high << 4) | low);
  }
  // 全部解析成功后才写回，失败时调用方的旧值保持不变。
  *out = result;
  return true;
}

}  // namespace crypto
}  // namespace backupproject
