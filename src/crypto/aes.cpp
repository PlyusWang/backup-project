// src/crypto/aes.cpp
//
// AES（FIPS 197）：128/192/256 位密钥的分组加解密 + AES-256-CTR 流式加密。
//
// 虽然容器只用 AES-256-CTR，但这里把密钥长度参数化了：一是可以用 FIPS 197
// 附录 C.1（AES-128）的官方向量单独验证 S-box / 行移位 / 列混合本身是否正确，
// 二是 C.3 的 AES-256 向量能同时覆盖 14 轮密钥扩展。
//
// 状态布局按 FIPS 197 的列主序：state[4 * column + row]，输入 16 字节直接就是
// state[0..15]，这样不需要额外的转置表。
//
// 轮密钥扩展只在构造/调用时做一次；Aes256 对象析构时会清零轮密钥。

#include <cstring>

#include "crypto.h"

namespace backupproject {
namespace crypto {
namespace {

// S-box（FIPS 197 图 7）：先求 GF(2^8) 乘法逆元，再做仿射变换的结果表。
constexpr unsigned char kSBox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b,
    0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0,
    0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26,
    0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2,
    0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0,
    0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed,
    0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f,
    0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5,
    0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec,
    0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14,
    0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c,
    0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d,
    0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f,
    0x4b, 0xbd, 0x8b, 0x8a, 0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e,
    0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11,
    0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f,
    0xb0, 0x54, 0xbb, 0x16};

// 逆 S-box（FIPS 197 图 14），解密与等价逆密码都要用。
constexpr unsigned char kInverseSBox[256] = {
    0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e,
    0x81, 0xf3, 0xd7, 0xfb, 0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87,
    0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb, 0x54, 0x7b, 0x94, 0x32,
    0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
    0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49,
    0x6d, 0x8b, 0xd1, 0x25, 0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16,
    0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92, 0x6c, 0x70, 0x48, 0x50,
    0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
    0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05,
    0xb8, 0xb3, 0x45, 0x06, 0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02,
    0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b, 0x3a, 0x91, 0x11, 0x41,
    0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
    0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8,
    0x1c, 0x75, 0xdf, 0x6e, 0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89,
    0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b, 0xfc, 0x56, 0x3e, 0x4b,
    0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
    0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59,
    0x27, 0x80, 0xec, 0x5f, 0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d,
    0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef, 0xa0, 0xe0, 0x3b, 0x4d,
    0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63,
    0x55, 0x21, 0x0c, 0x7d};

// 轮常量 Rcon：只需要 AES-256 的 7 个，多留几个给 AES-128/192 用。
constexpr unsigned char kRoundConstants[15] = {0x01, 0x02, 0x04, 0x08, 0x10,
                                               0x20, 0x40, 0x80, 0x1b, 0x36,
                                               0x6c, 0xd8, 0xab, 0x4d, 0x9a};

// AES-256 需要 60 个 32 位轮密钥字 = 240 字节，这是三种密钥长度的最大值。
constexpr std::size_t kMaxRoundKeyBytes = 240;
constexpr int kMaxRounds = 14;

void SecureZero(void* data, std::size_t size) {
  volatile unsigned char* p = static_cast<volatile unsigned char*>(data);
  while (size-- > 0) *p++ = 0;
}

// GF(2^8) 上乘 2（模 AES 的不可约多项式 x^8 + x^4 + x^3 + x + 1）。
inline unsigned char Xtime(unsigned char value) {
  const unsigned char shifted = static_cast<unsigned char>(value << 1);
  return static_cast<unsigned char>(shifted ^
                                    ((value & 0x80u) ? 0x1bu : 0x00u));
}

// GF(2^8) 上的通用乘法，用于逆列混合里的乘 9/11/13/14。
unsigned char Multiply(unsigned char a, unsigned char b) {
  unsigned char result = 0;
  for (int i = 0; i < 8; ++i) {
    if ((b & 0x01u) != 0) result = static_cast<unsigned char>(result ^ a);
    a = Xtime(a);
    b = static_cast<unsigned char>(b >> 1);
  }
  return result;
}

void AddRoundKey(unsigned char state[kAesBlockSize],
                 const unsigned char* round_key) {
  for (std::size_t i = 0; i < kAesBlockSize; ++i) state[i] ^= round_key[i];
}

void SubBytes(unsigned char state[kAesBlockSize], const unsigned char* box) {
  for (std::size_t i = 0; i < kAesBlockSize; ++i) state[i] = box[state[i]];
}

// 行 r 循环左移 r 字节。状态是列主序，行 r 的第 c 列在 state[4 * c + r]。
void ShiftRows(unsigned char state[kAesBlockSize]) {
  unsigned char row[4];
  for (std::size_t r = 1; r < 4; ++r) {
    for (std::size_t c = 0; c < 4; ++c) {
      row[c] = state[4 * ((c + r) % 4) + r];
    }
    for (std::size_t c = 0; c < 4; ++c) state[4 * c + r] = row[c];
  }
}

void InverseShiftRows(unsigned char state[kAesBlockSize]) {
  unsigned char row[4];
  for (std::size_t r = 1; r < 4; ++r) {
    for (std::size_t c = 0; c < 4; ++c) {
      row[(c + r) % 4] = state[4 * c + r];
    }
    for (std::size_t c = 0; c < 4; ++c) state[4 * c + r] = row[c];
  }
}

void MixColumns(unsigned char state[kAesBlockSize]) {
  for (std::size_t c = 0; c < 4; ++c) {
    unsigned char* column = state + 4 * c;
    const unsigned char sum = static_cast<unsigned char>(column[0] ^ column[1] ^
                                                         column[2] ^ column[3]);
    const unsigned char a0 = column[0];
    const unsigned char a1 = column[1];
    const unsigned char a2 = column[2];
    const unsigned char a3 = column[3];
    column[0] = static_cast<unsigned char>(
        a0 ^ sum ^ Xtime(static_cast<unsigned char>(a0 ^ a1)));
    column[1] = static_cast<unsigned char>(
        a1 ^ sum ^ Xtime(static_cast<unsigned char>(a1 ^ a2)));
    column[2] = static_cast<unsigned char>(
        a2 ^ sum ^ Xtime(static_cast<unsigned char>(a2 ^ a3)));
    column[3] = static_cast<unsigned char>(
        a3 ^ sum ^ Xtime(static_cast<unsigned char>(a3 ^ a0)));
  }
}

void InverseMixColumns(unsigned char state[kAesBlockSize]) {
  for (std::size_t c = 0; c < 4; ++c) {
    unsigned char* column = state + 4 * c;
    const unsigned char a0 = column[0];
    const unsigned char a1 = column[1];
    const unsigned char a2 = column[2];
    const unsigned char a3 = column[3];
    column[0] =
        static_cast<unsigned char>(Multiply(a0, 0x0e) ^ Multiply(a1, 0x0b) ^
                                   Multiply(a2, 0x0d) ^ Multiply(a3, 0x09));
    column[1] =
        static_cast<unsigned char>(Multiply(a0, 0x09) ^ Multiply(a1, 0x0e) ^
                                   Multiply(a2, 0x0b) ^ Multiply(a3, 0x0d));
    column[2] =
        static_cast<unsigned char>(Multiply(a0, 0x0d) ^ Multiply(a1, 0x09) ^
                                   Multiply(a2, 0x0e) ^ Multiply(a3, 0x0b));
    column[3] =
        static_cast<unsigned char>(Multiply(a0, 0x0b) ^ Multiply(a1, 0x0d) ^
                                   Multiply(a2, 0x09) ^ Multiply(a3, 0x0e));
  }
}

// 密钥扩展（FIPS 197 5.2）。返回轮数；key_size 不是 16/24/32 时返回 -1。
int ExpandKey(const unsigned char* key, std::size_t key_size,
              unsigned char round_keys[kMaxRoundKeyBytes]) {
  if (key_size != 16 && key_size != 24 && key_size != 32) return -1;
  const int nk = static_cast<int>(key_size / 4);  // 4 / 6 / 8 个字
  const int rounds = nk + 6;                      // 10 / 12 / 14
  const int total_words = 4 * (rounds + 1);       // 44 / 52 / 60

  std::memcpy(round_keys, key, key_size);
  unsigned char temp[4];
  for (int i = nk; i < total_words; ++i) {
    const unsigned char* previous = round_keys + 4 * (i - 1);
    std::memcpy(temp, previous, sizeof(temp));
    if (i % nk == 0) {
      // RotWord + SubWord + Rcon。
      const unsigned char first = temp[0];
      temp[0] = kSBox[temp[1]];
      temp[1] = kSBox[temp[2]];
      temp[2] = kSBox[temp[3]];
      temp[3] = kSBox[first];
      temp[0] =
          static_cast<unsigned char>(temp[0] ^ kRoundConstants[i / nk - 1]);
    } else if (nk > 6 && i % nk == 4) {
      // AES-256 特有的额外 SubWord。
      for (std::size_t j = 0; j < sizeof(temp); ++j) temp[j] = kSBox[temp[j]];
    }
    unsigned char* current = round_keys + 4 * i;
    const unsigned char* base = round_keys + 4 * (i - nk);
    for (std::size_t j = 0; j < sizeof(temp); ++j) {
      current[j] = static_cast<unsigned char>(base[j] ^ temp[j]);
    }
  }
  SecureZero(temp, sizeof(temp));
  return rounds;
}

void EncryptBlockRaw(const unsigned char round_keys[kMaxRoundKeyBytes],
                     int rounds, const unsigned char in[kAesBlockSize],
                     unsigned char out[kAesBlockSize]) {
  unsigned char state[kAesBlockSize];
  std::memcpy(state, in, kAesBlockSize);
  AddRoundKey(state, round_keys);
  for (int round = 1; round < rounds; ++round) {
    SubBytes(state, kSBox);
    ShiftRows(state);
    MixColumns(state);
    AddRoundKey(state, round_keys + 16 * round);
  }
  SubBytes(state, kSBox);
  ShiftRows(state);
  AddRoundKey(state, round_keys + 16 * rounds);
  std::memcpy(out, state, kAesBlockSize);
  SecureZero(state, sizeof(state));
}

// 直接逆密码（FIPS 197 图 12 的逆序），不做等价逆密码变换：
// 对照标准时不用先证明"等价形式"这一步，代码量与正确性更容易核对。
void DecryptBlockRaw(const unsigned char round_keys[kMaxRoundKeyBytes],
                     int rounds, const unsigned char in[kAesBlockSize],
                     unsigned char out[kAesBlockSize]) {
  unsigned char state[kAesBlockSize];
  std::memcpy(state, in, kAesBlockSize);
  AddRoundKey(state, round_keys + 16 * rounds);
  for (int round = rounds - 1; round >= 1; --round) {
    InverseShiftRows(state);
    SubBytes(state, kInverseSBox);
    AddRoundKey(state, round_keys + 16 * round);
    InverseMixColumns(state);
  }
  InverseShiftRows(state);
  SubBytes(state, kInverseSBox);
  AddRoundKey(state, round_keys);
  std::memcpy(out, state, kAesBlockSize);
  SecureZero(state, sizeof(state));
}

// 自由函数用的薄封装：长度非法时返回空串。
std::string AesSingleBlock(const std::string& key, const std::string& block16,
                           bool decrypt) {
  if (block16.size() != kAesBlockSize) return std::string();
  unsigned char round_keys[kMaxRoundKeyBytes];
  const int rounds =
      ExpandKey(reinterpret_cast<const unsigned char*>(key.data()), key.size(),
                round_keys);
  if (rounds < 0) return std::string();
  unsigned char out[kAesBlockSize];
  const unsigned char* in =
      reinterpret_cast<const unsigned char*>(block16.data());
  if (decrypt) {
    DecryptBlockRaw(round_keys, rounds, in, out);
  } else {
    EncryptBlockRaw(round_keys, rounds, in, out);
  }
  SecureZero(round_keys, sizeof(round_keys));
  std::string result(reinterpret_cast<const char*>(out), sizeof(out));
  SecureZero(out, sizeof(out));
  return result;
}

}  // namespace

Aes256::Aes256(const std::string& key32)
    : round_keys_{}, rounds_(0), valid_(false) {
  // 这里刻意不用 assert：容器解析到坏密钥时应当返回错误码，而不是让进程 abort。
  const int rounds =
      ExpandKey(reinterpret_cast<const unsigned char*>(key32.data()),
                key32.size(), round_keys_);
  if (rounds < 0) return;
  rounds_ = rounds;
  valid_ = true;
}

Aes256::~Aes256() { SecureZero(round_keys_, sizeof(round_keys_)); }

void Aes256::EncryptBlock(const unsigned char in[kAesBlockSize],
                          unsigned char out[kAesBlockSize]) const {
  if (!valid_) {
    std::memset(out, 0, kAesBlockSize);
    return;
  }
  EncryptBlockRaw(round_keys_, rounds_, in, out);
}

void Aes256::DecryptBlock(const unsigned char in[kAesBlockSize],
                          unsigned char out[kAesBlockSize]) const {
  if (!valid_) {
    std::memset(out, 0, kAesBlockSize);
    return;
  }
  DecryptBlockRaw(round_keys_, rounds_, in, out);
}

std::string Aes256BlockEncrypt(const std::string& key32,
                               const std::string& block16) {
  return AesSingleBlock(key32, block16, false);
}

std::string Aes256BlockDecrypt(const std::string& key32,
                               const std::string& block16) {
  return AesSingleBlock(key32, block16, true);
}

std::string Aes128BlockEncrypt(const std::string& key16,
                               const std::string& block16) {
  return AesSingleBlock(key16, block16, false);
}

std::string Aes128BlockDecrypt(const std::string& key16,
                               const std::string& block16) {
  return AesSingleBlock(key16, block16, true);
}

Aes256Ctr::Aes256Ctr(const std::string& key32, const std::string& iv16)
    : cipher_(key32),
      counter_{},
      keystream_{},
      keystream_pos_(kAesBlockSize),
      valid_(false) {
  if (!cipher_.valid() || iv16.size() != kAesBlockSize) return;
  std::memcpy(counter_, iv16.data(), kAesBlockSize);
  valid_ = true;
}

Aes256Ctr::~Aes256Ctr() {
  SecureZero(counter_, sizeof(counter_));
  SecureZero(keystream_, sizeof(keystream_));
}

void Aes256Ctr::IncrementCounter() {
  // 大端整数自增，128 位整体回绕（计数器空间足够大，实际不会走到回绕）。
  for (std::size_t i = kAesBlockSize; i-- > 0;) {
    counter_[i] = static_cast<unsigned char>(counter_[i] + 1);
    if (counter_[i] != 0) return;  // 没有进位就结束
  }
}

void Aes256Ctr::GenerateKeystream() {
  cipher_.EncryptBlock(counter_, keystream_);
  IncrementCounter();
}

void Aes256Ctr::Process(const void* data, std::size_t size, std::string* out) {
  if (!valid_ || out == nullptr || data == nullptr || size == 0) return;
  const unsigned char* p = static_cast<const unsigned char*>(data);
  char buffer[kAesBlockSize];
  while (size > 0) {
    if (keystream_pos_ == kAesBlockSize) {
      GenerateKeystream();
      keystream_pos_ = 0;
    }
    const std::size_t available = kAesBlockSize - keystream_pos_;
    const std::size_t take = size < available ? size : available;
    for (std::size_t i = 0; i < take; ++i) {
      buffer[i] = static_cast<char>(p[i] ^ keystream_[keystream_pos_ + i]);
    }
    out->append(buffer, take);
    p += take;
    size -= take;
    keystream_pos_ += take;
  }
}

std::string Aes256CtrCrypt(const std::string& key32, const std::string& iv16,
                           const std::string& data) {
  Aes256Ctr ctr(key32, iv16);
  if (!ctr.valid()) return std::string();
  std::string out;
  ctr.Process(data.data(), data.size(), &out);
  return out;
}

}  // namespace crypto
}  // namespace backupproject
