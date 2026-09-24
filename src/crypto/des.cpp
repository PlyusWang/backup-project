// src/crypto/des.cpp
//
// DES（FIPS 46-3）单块 + DES-CBC + PKCS#7 填充。
//
// 表全部按标准原文的 1-based 位号写在源码里（第 1 位是最高位），由 Permute()
// 统一解释，这样任何一张表和 FIPS 46-3 的排版都是逐行对得上的；把表改写成
// 0-based 掩码虽然快一点，但核对时要靠人脑换算，容易藏错。
//
// 已知强度问题：DES 有效密钥只有 56 位（每字节最低位是奇偶校验位，PC-1 直接
// 丢弃，这里不做校验位修正），单独使用不足以抵御现代暴力破解。保留它是为了
// 兼容旧容器格式；新数据请走 AES-256-CTR。

#include <cstring>

#include "crypto.h"

namespace backupproject {
namespace crypto {
namespace {

// 初始置换 IP。
constexpr int kInitialPermutation[64] = {
    58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4,
    62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8,
    57, 49, 41, 33, 25, 17, 9,  1, 59, 51, 43, 35, 27, 19, 11, 3,
    61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7};

// 逆初始置换 FP = IP^-1。
constexpr int kFinalPermutation[64] = {
    40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31,
    38, 6, 46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29,
    36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27,
    34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9,  49, 17, 57, 25};

// 轮函数里的扩展置换 E：32 -> 48 位。
constexpr int kExpansion[48] = {32, 1,  2,  3,  4,  5,  4,  5,  6,  7,  8,  9,
                                8,  9,  10, 11, 12, 13, 12, 13, 14, 15, 16, 17,
                                16, 17, 18, 19, 20, 21, 20, 21, 22, 23, 24, 25,
                                24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1};

// S 盒输出后的置换 P：32 -> 32 位。
constexpr int kPermutation[32] = {16, 7, 20, 21, 29, 12, 28, 17, 1,  15, 23,
                                  26, 5, 18, 31, 10, 2,  8,  24, 14, 32, 27,
                                  3,  9, 19, 13, 30, 6,  22, 11, 4,  25};

// 密钥置换 PC-1：64 -> 56 位（丢掉 8 个奇偶校验位）。
constexpr int kKeyPermutation1[56] = {
    57, 49, 41, 33, 25, 17, 9,  1,  58, 50, 42, 34, 26, 18, 10, 2,  59, 51, 43,
    35, 27, 19, 11, 3,  60, 52, 44, 36, 63, 55, 47, 39, 31, 23, 15, 7,  62, 54,
    46, 38, 30, 22, 14, 6,  61, 53, 45, 37, 29, 21, 13, 5,  28, 20, 12, 4};

// 压缩置换 PC-2：56 -> 48 位，生成轮密钥。
constexpr int kKeyPermutation2[48] = {
    14, 17, 11, 24, 1,  5,  3,  28, 15, 6,  21, 10, 23, 19, 12, 4,
    26, 8,  16, 7,  27, 20, 13, 2,  41, 52, 31, 37, 47, 55, 30, 40,
    51, 45, 33, 48, 44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32};

// 每个轮次的循环左移位数。
constexpr int kKeyShifts[16] = {1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1};

// 8 个 S 盒，每个 4 行 16 列，按行展开存放。
constexpr unsigned char kSBoxes[8][64] = {
    {// S1
     14, 4,  13, 1, 2,  15, 11, 8,  3,  10, 6,  12, 5,  9,  0, 7,
     0,  15, 7,  4, 14, 2,  13, 1,  10, 6,  12, 11, 9,  5,  3, 8,
     4,  1,  14, 8, 13, 6,  2,  11, 15, 12, 9,  7,  3,  10, 5, 0,
     15, 12, 8,  2, 4,  9,  1,  7,  5,  11, 3,  14, 10, 0,  6, 13},
    {// S2
     15, 1,  8,  14, 6,  11, 3,  4,  9,  7, 2,  13, 12, 0, 5,  10,
     3,  13, 4,  7,  15, 2,  8,  14, 12, 0, 1,  10, 6,  9, 11, 5,
     0,  14, 7,  11, 10, 4,  13, 1,  5,  8, 12, 6,  9,  3, 2,  15,
     13, 8,  10, 1,  3,  15, 4,  2,  11, 6, 7,  12, 0,  5, 14, 9},
    {// S3
     10, 0,  9,  14, 6, 3,  15, 5,  1,  13, 12, 7,  11, 4,  2,  8,
     13, 7,  0,  9,  3, 4,  6,  10, 2,  8,  5,  14, 12, 11, 15, 1,
     13, 6,  4,  9,  8, 15, 3,  0,  11, 1,  2,  12, 5,  10, 14, 7,
     1,  10, 13, 0,  6, 9,  8,  7,  4,  15, 14, 3,  11, 5,  2,  12},
    {// S4
     7,  13, 14, 3, 0,  6,  9,  10, 1,  2, 8, 5,  11, 12, 4,  15,
     13, 8,  11, 5, 6,  15, 0,  3,  4,  7, 2, 12, 1,  10, 14, 9,
     10, 6,  9,  0, 12, 11, 7,  13, 15, 1, 3, 14, 5,  2,  8,  4,
     3,  15, 0,  6, 10, 1,  13, 8,  9,  4, 5, 11, 12, 7,  2,  14},
    {// S5
     2,  12, 4,  1,  7,  10, 11, 6,  8,  5,  3,  15, 13, 0, 14, 9,
     14, 11, 2,  12, 4,  7,  13, 1,  5,  0,  15, 10, 3,  9, 8,  6,
     4,  2,  1,  11, 10, 13, 7,  8,  15, 9,  12, 5,  6,  3, 0,  14,
     11, 8,  12, 7,  1,  14, 2,  13, 6,  15, 0,  9,  10, 4, 5,  3},
    {// S6
     12, 1,  10, 15, 9, 2,  6,  8,  0,  13, 3,  4,  14, 7,  5,  11,
     10, 15, 4,  2,  7, 12, 9,  5,  6,  1,  13, 14, 0,  11, 3,  8,
     9,  14, 15, 5,  2, 8,  12, 3,  7,  0,  4,  10, 1,  13, 11, 6,
     4,  3,  2,  12, 9, 5,  15, 10, 11, 14, 1,  7,  6,  0,  8,  13},
    {// S7
     4,  11, 2,  14, 15, 0, 8,  13, 3,  12, 9, 7,  5,  10, 6, 1,
     13, 0,  11, 7,  4,  9, 1,  10, 14, 3,  5, 12, 2,  15, 8, 6,
     1,  4,  11, 13, 12, 3, 7,  14, 10, 15, 6, 8,  0,  5,  9, 2,
     6,  11, 13, 8,  1,  4, 10, 7,  9,  5,  0, 15, 14, 2,  3, 12},
    {// S8
     13, 2,  8,  4, 6,  15, 11, 1,  10, 9,  3,  14, 5,  0,  12, 7,
     1,  15, 13, 8, 10, 3,  7,  4,  12, 5,  6,  11, 0,  14, 9,  2,
     7,  11, 4,  1, 9,  12, 14, 2,  0,  6,  10, 13, 15, 3,  5,  8,
     2,  1,  14, 7, 4,  10, 8,  13, 15, 12, 9,  0,  3,  5,  6,  11}};

void SecureZero(void* data, std::size_t size) {
  volatile unsigned char* p = static_cast<volatile unsigned char*>(data);
  while (size-- > 0) *p++ = 0;
}

// 按 1-based 位号表做置换：out_bits 是输出位数，in_bits 是输入的位宽。
// 表里所有位号都在 [1, in_bits] 内，因此移位量恒在 [0, in_bits - 1]。
std::uint64_t Permute(std::uint64_t input, const int* table,
                      std::size_t out_bits, std::size_t in_bits) {
  std::uint64_t out = 0;
  for (std::size_t i = 0; i < out_bits; ++i) {
    const std::size_t source = static_cast<std::size_t>(table[i]);
    const std::uint64_t bit = (input >> (in_bits - source)) & 1u;
    out = (out << 1) | bit;
  }
  return out;
}

std::uint64_t LoadBigEndian64(const unsigned char* data) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    value = (value << 8) | data[i];
  }
  return value;
}

void StoreBigEndian64(std::uint64_t value, unsigned char* data) {
  for (std::size_t i = 0; i < 8; ++i) {
    data[i] = static_cast<unsigned char>(value >> (56u - 8u * i));
  }
}

// 由 64 位密钥生成 16 个 48 位轮密钥。密钥的奇偶校验位被 PC-1 丢弃，
// 因此不做校验位修正（与 FIPS 46-3 的"密钥奇偶性不影响结果"一致）。
void KeySchedule(std::uint64_t key, std::uint64_t subkeys[16]) {
  const std::uint64_t permuted = Permute(key, kKeyPermutation1, 56, 64);
  std::uint32_t left =
      static_cast<std::uint32_t>((permuted >> 28) & 0x0FFFFFFFu);
  std::uint32_t right = static_cast<std::uint32_t>(permuted & 0x0FFFFFFFu);
  for (std::size_t round = 0; round < 16; ++round) {
    const unsigned shift = static_cast<unsigned>(kKeyShifts[round]);
    left = ((left << shift) | (left >> (28u - shift))) & 0x0FFFFFFFu;
    right = ((right << shift) | (right >> (28u - shift))) & 0x0FFFFFFFu;
    const std::uint64_t joined =
        (static_cast<std::uint64_t>(left) << 28) | right;
    subkeys[round] = Permute(joined, kKeyPermutation2, 48, 56);
  }
}

// 轮函数 F：扩展 -> 与轮密钥异或 -> S 盒代换 -> P 置换。
// 输入是 32 位右半部分，输出 32 位。
std::uint64_t Feistel(std::uint64_t half, std::uint64_t subkey) {
  const std::uint64_t expanded = Permute(half, kExpansion, 48, 32) ^ subkey;
  std::uint64_t substituted = 0;
  for (std::size_t box = 0; box < 8; ++box) {
    // 6 位一组，最高位在前；行号 = 第 1、6 位，列号 = 中间 4 位。
    const unsigned group = static_cast<unsigned>(
        (expanded >> (42u - 6u * static_cast<unsigned>(box))) & 0x3Fu);
    const unsigned row = ((group & 0x20u) >> 4) | (group & 0x01u);
    const unsigned column = (group >> 1) & 0x0Fu;
    substituted = (substituted << 4) | kSBoxes[box][row * 16 + column];
  }
  return Permute(substituted, kPermutation, 32, 32);
}

// 16 轮 Feistel + 末置换。subkeys 的顺序由调用方决定：正序加密、逆序解密，
// 这是 DES 的结构性质（解密用同一台机器、同一批轮密钥倒着跑）。
std::uint64_t DesCrypt(std::uint64_t input, const std::uint64_t subkeys[16]) {
  const std::uint64_t permuted = Permute(input, kInitialPermutation, 64, 64);
  std::uint32_t left = static_cast<std::uint32_t>(permuted >> 32);
  std::uint32_t right = static_cast<std::uint32_t>(permuted & 0xFFFFFFFFu);
  for (std::size_t round = 0; round < 16; ++round) {
    const std::uint32_t next_left = right;
    const std::uint32_t next_right =
        left ^ static_cast<std::uint32_t>(Feistel(right, subkeys[round]));
    left = next_left;
    right = next_right;
  }
  // 最后一轮后左右交换（R16 || L16）。
  const std::uint64_t preoutput =
      (static_cast<std::uint64_t>(right) << 32) | left;
  return Permute(preoutput, kFinalPermutation, 64, 64);
}

}  // namespace

std::string DesBlockEncrypt(const std::string& key8,
                            const std::string& block8) {
  if (key8.size() != kDesKeySize || block8.size() != kDesBlockSize) {
    return std::string();
  }
  std::uint64_t subkeys[16];
  KeySchedule(
      LoadBigEndian64(reinterpret_cast<const unsigned char*>(key8.data())),
      subkeys);
  const std::uint64_t encrypted = DesCrypt(
      LoadBigEndian64(reinterpret_cast<const unsigned char*>(block8.data())),
      subkeys);
  SecureZero(subkeys, sizeof(subkeys));
  std::string out(kDesBlockSize, '\0');
  StoreBigEndian64(encrypted, reinterpret_cast<unsigned char*>(&out[0]));
  return out;
}

std::string DesBlockDecrypt(const std::string& key8,
                            const std::string& block8) {
  if (key8.size() != kDesKeySize || block8.size() != kDesBlockSize) {
    return std::string();
  }
  std::uint64_t subkeys[16];
  KeySchedule(
      LoadBigEndian64(reinterpret_cast<const unsigned char*>(key8.data())),
      subkeys);
  std::uint64_t reversed[16];
  for (std::size_t i = 0; i < 16; ++i) reversed[i] = subkeys[15 - i];
  const std::uint64_t decrypted = DesCrypt(
      LoadBigEndian64(reinterpret_cast<const unsigned char*>(block8.data())),
      reversed);
  SecureZero(subkeys, sizeof(subkeys));
  SecureZero(reversed, sizeof(reversed));
  std::string out(kDesBlockSize, '\0');
  StoreBigEndian64(decrypted, reinterpret_cast<unsigned char*>(&out[0]));
  return out;
}

DesCbcEncryptor::DesCbcEncryptor(const std::string& key8,
                                 const std::string& iv8)
    : subkeys_{},
      chain_{},
      buffer_{},
      buffer_size_(0),
      finished_(false),
      valid_(false) {
  if (key8.size() != kDesKeySize || iv8.size() != kDesBlockSize) return;
  KeySchedule(
      LoadBigEndian64(reinterpret_cast<const unsigned char*>(key8.data())),
      subkeys_);
  std::memcpy(chain_, iv8.data(), kDesBlockSize);
  valid_ = true;
}

DesCbcEncryptor::~DesCbcEncryptor() {
  SecureZero(subkeys_, sizeof(subkeys_));
  SecureZero(chain_, sizeof(chain_));
  SecureZero(buffer_, sizeof(buffer_));
}

void DesCbcEncryptor::EncryptOneBlock(const unsigned char in[kDesBlockSize],
                                      unsigned char out[kDesBlockSize]) {
  unsigned char xored[kDesBlockSize];
  for (std::size_t i = 0; i < kDesBlockSize; ++i) xored[i] = in[i] ^ chain_[i];
  const std::uint64_t encrypted = DesCrypt(LoadBigEndian64(xored), subkeys_);
  StoreBigEndian64(encrypted, out);
  // CBC：本块密文成为下一块的链值。
  std::memcpy(chain_, out, kDesBlockSize);
  SecureZero(xored, sizeof(xored));
}

void DesCbcEncryptor::Process(const void* data, std::size_t size,
                              std::string* out) {
  if (!valid_ || finished_ || out == nullptr || data == nullptr || size == 0) {
    return;
  }
  const unsigned char* p = static_cast<const unsigned char*>(data);
  while (size > 0) {
    const std::size_t need = kDesBlockSize - buffer_size_;
    const std::size_t take = size < need ? size : need;
    std::memcpy(buffer_ + buffer_size_, p, take);
    buffer_size_ += take;
    p += take;
    size -= take;
    if (buffer_size_ == kDesBlockSize) {
      unsigned char cipher[kDesBlockSize];
      EncryptOneBlock(buffer_, cipher);
      out->append(reinterpret_cast<const char*>(cipher), kDesBlockSize);
      buffer_size_ = 0;
      SecureZero(cipher, sizeof(cipher));
    }
  }
}

bool DesCbcEncryptor::Finish(std::string* out, std::string* error_message) {
  const auto fail = [error_message](const char* message) {
    if (error_message != nullptr) *error_message = message;
    return false;
  };
  if (error_message != nullptr) error_message->clear();
  if (out == nullptr) return fail("DES-CBC: 输出指针为空");
  if (!valid_) return fail("DES-CBC: 密钥或 IV 长度非法（需要 8 字节）");
  if (finished_) return fail("DES-CBC: Finish 只能调用一次");
  // PKCS#7：补齐 1..8 字节，每字节等于补的长度。明文为空时也补满一整块。
  const unsigned char pad =
      static_cast<unsigned char>(kDesBlockSize - buffer_size_);
  std::memset(buffer_ + buffer_size_, pad, pad);
  unsigned char cipher[kDesBlockSize];
  EncryptOneBlock(buffer_, cipher);
  out->append(reinterpret_cast<const char*>(cipher), kDesBlockSize);
  SecureZero(cipher, sizeof(cipher));
  SecureZero(buffer_, sizeof(buffer_));
  buffer_size_ = 0;
  finished_ = true;
  return true;
}

DesCbcDecryptor::DesCbcDecryptor(const std::string& key8,
                                 const std::string& iv8)
    : subkeys_{},
      chain_{},
      buffer_{},
      pending_{},
      buffer_size_(0),
      has_pending_(false),
      finished_(false),
      valid_(false) {
  if (key8.size() != kDesKeySize || iv8.size() != kDesBlockSize) return;
  KeySchedule(
      LoadBigEndian64(reinterpret_cast<const unsigned char*>(key8.data())),
      subkeys_);
  std::memcpy(chain_, iv8.data(), kDesBlockSize);
  valid_ = true;
}

DesCbcDecryptor::~DesCbcDecryptor() {
  SecureZero(subkeys_, sizeof(subkeys_));
  SecureZero(chain_, sizeof(chain_));
  SecureZero(buffer_, sizeof(buffer_));
  SecureZero(pending_, sizeof(pending_));
}

void DesCbcDecryptor::DecryptOneBlock(const unsigned char in[kDesBlockSize],
                                      unsigned char out[kDesBlockSize]) {
  std::uint64_t reversed[16];
  for (std::size_t i = 0; i < 16; ++i) reversed[i] = subkeys_[15 - i];
  const std::uint64_t decrypted = DesCrypt(LoadBigEndian64(in), reversed);
  unsigned char plain[kDesBlockSize];
  StoreBigEndian64(decrypted, plain);
  for (std::size_t i = 0; i < kDesBlockSize; ++i) out[i] = plain[i] ^ chain_[i];
  std::memcpy(chain_, in, kDesBlockSize);
  SecureZero(reversed, sizeof(reversed));
  SecureZero(plain, sizeof(plain));
}

void DesCbcDecryptor::Process(const void* data, std::size_t size,
                              std::string* out) {
  if (!valid_ || finished_ || out == nullptr || data == nullptr || size == 0) {
    return;
  }
  const unsigned char* p = static_cast<const unsigned char*>(data);
  while (size > 0) {
    const std::size_t need = kDesBlockSize - buffer_size_;
    const std::size_t take = size < need ? size : need;
    std::memcpy(buffer_ + buffer_size_, p, take);
    buffer_size_ += take;
    p += take;
    size -= take;
    if (buffer_size_ == kDesBlockSize) {
      // 先压住这一块：它也可能是最后一块（带 padding），要等 Finish 才能确认。
      unsigned char plain[kDesBlockSize];
      DecryptOneBlock(buffer_, plain);
      if (has_pending_) {
        out->append(reinterpret_cast<const char*>(pending_), kDesBlockSize);
      }
      std::memcpy(pending_, plain, kDesBlockSize);
      has_pending_ = true;
      buffer_size_ = 0;
      SecureZero(plain, sizeof(plain));
    }
  }
}

bool DesCbcDecryptor::Finish(std::string* out, std::string* error_message) {
  const auto fail = [error_message](const char* message) {
    if (error_message != nullptr) *error_message = message;
    return false;
  };
  if (error_message != nullptr) error_message->clear();
  if (out == nullptr) return fail("DES-CBC: 输出指针为空");
  if (!valid_) return fail("DES-CBC: 密钥或 IV 长度非法（需要 8 字节）");
  if (finished_) return fail("DES-CBC: Finish 只能调用一次");
  if (buffer_size_ != 0) return fail("DES-CBC: 密文长度不是 8 的倍数");
  if (!has_pending_) return fail("DES-CBC: 密文为空");
  const unsigned char pad = pending_[kDesBlockSize - 1];
  if (pad == 0 || pad > kDesBlockSize) {
    return fail("DES-CBC: PKCS#7 padding 非法");
  }
  for (std::size_t i = 0; i < pad; ++i) {
    if (pending_[kDesBlockSize - pad + i] != pad) {
      return fail("DES-CBC: PKCS#7 padding 非法");
    }
  }
  out->append(reinterpret_cast<const char*>(pending_), kDesBlockSize - pad);
  SecureZero(pending_, sizeof(pending_));
  finished_ = true;
  return true;
}

bool DesCbcEncrypt(const std::string& key8, const std::string& iv8,
                   const std::string& plaintext, std::string* out,
                   std::string* error_message) {
  if (error_message != nullptr) error_message->clear();
  if (out == nullptr) {
    if (error_message != nullptr) *error_message = "DES-CBC: 输出指针为空";
    return false;
  }
  out->clear();
  DesCbcEncryptor encryptor(key8, iv8);
  encryptor.Process(plaintext.data(), plaintext.size(), out);
  return encryptor.Finish(out, error_message);
}

bool DesCbcDecrypt(const std::string& key8, const std::string& iv8,
                   const std::string& ciphertext, std::string* out,
                   std::string* error_message) {
  if (error_message != nullptr) error_message->clear();
  if (out == nullptr) {
    if (error_message != nullptr) *error_message = "DES-CBC: 输出指针为空";
    return false;
  }
  // 失败时不把"没通过 padding 校验的明文"留给调用方。
  out->clear();
  DesCbcDecryptor decryptor(key8, iv8);
  decryptor.Process(ciphertext.data(), ciphertext.size(), out);
  if (!decryptor.Finish(out, error_message)) {
    out->clear();
    return false;
  }
  return true;
}

}  // namespace crypto
}  // namespace backupproject
