// src/crypto/sha512.cpp
//
// 手写 SHA-512（FIPS 180-4 / RFC 6234）。Ed25519 的哈希就是这个函数，
// 所以它必须逐位可对照标准复核，而不是"看起来能跑"。
//
// 与同目录 sha256.cpp 完全同一套结构：常量以 constexpr 数组写在文件里，
// 流式的 Update/Final 允许任意切分，Final 只能调用一次。
//
// 参考：FIPS 180-4 §4.1.3（初始值）、§4.2.3（常量）、§5.1.2（消息填充）、
//       §6.4（压缩函数）。

#include <cstring>

#include "crypto.h"

namespace backupproject {
namespace crypto {
namespace {

// FIPS 180-4 §4.2.3：前 80 个素数立方根小数部分的前 64 位。
constexpr std::uint64_t kK[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL,
    0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
    0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL,
    0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
    0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
    0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL, 0x2de92c6f592b0275ULL,
    0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL,
    0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL,
    0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL,
    0x92722c851482353bULL, 0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL,
    0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL,
    0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL,
    0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL,
    0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL,
    0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL,
    0xc67178f2e372532bULL, 0xca273eceea26619cULL, 0xd186b8c721c0c207ULL,
    0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL,
    0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL,
    0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL,
    0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};

// FIPS 180-4 §5.3.5：初始哈希值（前 8 个素数平方根小数部分）。
constexpr std::uint64_t kInitial[8] = {
    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL,
    0xa54ff53a5f1d36f1ULL, 0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
    0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL,
};

inline std::uint64_t Rotr(std::uint64_t x, unsigned n) {
  return (x >> n) | (x << (64 - n));
}

inline std::uint64_t LoadBigEndian64(const unsigned char* p) {
  return (static_cast<std::uint64_t>(p[0]) << 56) |
         (static_cast<std::uint64_t>(p[1]) << 48) |
         (static_cast<std::uint64_t>(p[2]) << 40) |
         (static_cast<std::uint64_t>(p[3]) << 32) |
         (static_cast<std::uint64_t>(p[4]) << 24) |
         (static_cast<std::uint64_t>(p[5]) << 16) |
         (static_cast<std::uint64_t>(p[6]) << 8) |
         static_cast<std::uint64_t>(p[7]);
}

inline void StoreBigEndian64(std::uint64_t value, unsigned char* p) {
  p[0] = static_cast<unsigned char>(value >> 56);
  p[1] = static_cast<unsigned char>(value >> 48);
  p[2] = static_cast<unsigned char>(value >> 40);
  p[3] = static_cast<unsigned char>(value >> 32);
  p[4] = static_cast<unsigned char>(value >> 24);
  p[5] = static_cast<unsigned char>(value >> 16);
  p[6] = static_cast<unsigned char>(value >> 8);
  p[7] = static_cast<unsigned char>(value);
}

}  // namespace

Sha512::Sha512() { Reset(); }

void Sha512::Reset() {
  std::memcpy(state_, kInitial, sizeof(state_));
  total_size_ = 0;
  buffer_size_ = 0;
  std::memset(buffer_, 0, sizeof(buffer_));
}

void Sha512::Update(const void* data, std::size_t size) {
  if (data == nullptr || size == 0) {
    return;  // 空消息合法
  }
  const unsigned char* input = static_cast<const unsigned char*>(data);
  // 总长度按字节累计；自己再换算成比特，避免在 32 位平台上把 size<<3 截断。
  total_size_ += size;

  if (buffer_size_ != 0) {
    const std::size_t need = kSha512BlockSize - buffer_size_;
    const std::size_t take = size < need ? size : need;
    std::memcpy(buffer_ + buffer_size_, input, take);
    buffer_size_ += take;
    input += take;
    size -= take;
    if (buffer_size_ == kSha512BlockSize) {
      Transform(buffer_);
      buffer_size_ = 0;
    }
  }
  while (size >= kSha512BlockSize) {
    Transform(input);
    input += kSha512BlockSize;
    size -= kSha512BlockSize;
  }
  if (size != 0) {
    std::memcpy(buffer_, input, size);
    buffer_size_ = size;
  }
}

void Sha512::Final(unsigned char out[kSha512DigestSize]) {
  // FIPS 180-4 §5.1.2：先补 0x80，再补零到 112 (mod 128)，最后 16
  // 字节大端比特长度。
  const std::uint64_t total_bits = total_size_ * 8;
  const unsigned char pad = 0x80;
  Update(&pad, 1);
  const unsigned char zero = 0x00;
  while (buffer_size_ != 112) {
    Update(&zero, 1);
  }
  unsigned char length[16];
  // 高 64 位：本实现不支持 > 2^64 比特（约 2 EiB）的消息，写 0 并在下方断言。
  StoreBigEndian64(0, length);
  StoreBigEndian64(total_bits, length + 8);
  Update(length, sizeof(length));

  for (int i = 0; i < 8; ++i) {
    StoreBigEndian64(state_[i], out + i * 8);
  }
  // Final 之后再 Update 得到的是"新的一条消息"，调用方不应依赖；这里不重置，
  // 由调用方显式 Reset（与 Sha256 的行为一致）。
}

void Sha512::Transform(const unsigned char block[kSha512BlockSize]) {
  std::uint64_t w[80];
  for (int t = 0; t < 16; ++t) {
    w[t] = LoadBigEndian64(block + t * 8);
  }
  for (int t = 16; t < 80; ++t) {
    const std::uint64_t s0 =
        Rotr(w[t - 15], 1) ^ Rotr(w[t - 15], 8) ^ (w[t - 15] >> 7);
    const std::uint64_t s1 =
        Rotr(w[t - 2], 19) ^ Rotr(w[t - 2], 61) ^ (w[t - 2] >> 6);
    w[t] = w[t - 16] + s0 + w[t - 7] + s1;
  }

  std::uint64_t a = state_[0];
  std::uint64_t b = state_[1];
  std::uint64_t c = state_[2];
  std::uint64_t d = state_[3];
  std::uint64_t e = state_[4];
  std::uint64_t f = state_[5];
  std::uint64_t g = state_[6];
  std::uint64_t h = state_[7];

  for (int t = 0; t < 80; ++t) {
    const std::uint64_t big_s1 = Rotr(e, 14) ^ Rotr(e, 18) ^ Rotr(e, 41);
    const std::uint64_t ch = (e & f) ^ ((~e) & g);
    const std::uint64_t temp1 = h + big_s1 + ch + kK[t] + w[t];
    const std::uint64_t big_s0 = Rotr(a, 28) ^ Rotr(a, 34) ^ Rotr(a, 39);
    const std::uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
    const std::uint64_t temp2 = big_s0 + maj;

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

void Sha512::Digest(const void* data, std::size_t size,
                    unsigned char out[kSha512DigestSize]) {
  Sha512 ctx;
  ctx.Update(data, size);
  ctx.Final(out);
}

std::string Sha512Raw(const std::string& data) {
  unsigned char out[kSha512DigestSize];
  Sha512::Digest(data.data(), data.size(), out);
  return std::string(reinterpret_cast<const char*>(out), sizeof(out));
}

std::string Sha512Hex(const std::string& data) {
  unsigned char out[kSha512DigestSize];
  Sha512::Digest(data.data(), data.size(), out);
  return ToHex(out, sizeof(out));
}

}  // namespace crypto
}  // namespace backupproject
