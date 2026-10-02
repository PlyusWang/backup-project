// src/crypto/x25519.cpp
//
// 手写 X25519（RFC 7748）实现。算法结构见 include/x25519.h 的文件头说明。
//
// 这里刻意不复用项目里既有的"加密原语"之外的任何东西：有限域、ladder、
// 编解码都是本文件自己的代码；只有 SHA-256（指纹）与 OS CSPRNG（密钥生成）
// 来自 include/crypto.h——它们本来就是本项目自己写的手写原语。

#include "x25519.h"

#include <cstring>

#include "crypto.h"

namespace backupproject {
namespace crypto {
namespace {

using Limb = std::uint32_t;

constexpr std::uint64_t kLimbMask64 = 0xFFFFFFFFull;

// p = 2^255 - 19，2^32 进制 8 个 limb，小端在前：
//   limb0 = 2^32 - 19，limb1..6 = 2^32 - 1，limb7 = 2^31 - 1。
constexpr Limb kPrime[8] = {0xFFFFFFEDu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
                            0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x7FFFFFFFu};

struct Fe {
  Limb v[8];
};

Limb Low32(std::uint64_t value) {
  return static_cast<Limb>(value & kLimbMask64);
}

// 把 64 位有符号中间结果截成 limb（二进制补码低 32 位）。
Limb Low32Signed(std::int64_t value) {
  return static_cast<Limb>(static_cast<std::uint64_t>(value) & kLimbMask64);
}

// r >= p 时常量时间地取 r - p。borrow == 0 表示 r >= p。
void ConditionalSubtractPrime(Limb r[8]) {
  Limb candidate[8];
  std::int64_t borrow = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    const std::int64_t cur = static_cast<std::int64_t>(r[i]) -
                             static_cast<std::int64_t>(kPrime[i]) - borrow;
    candidate[i] = Low32Signed(cur);
    borrow = (cur < 0) ? 1 : 0;
  }
  // borrow 是 0/1；borrow - 1 在补码里是 0xFFFF...（borrow == 0）或 0。
  const Limb take =
      static_cast<Limb>(static_cast<std::uint64_t>(borrow - 1) & kLimbMask64);
  for (std::size_t i = 0; i < 8; ++i) {
    r[i] = (r[i] & ~take) | (candidate[i] & take);
  }
}

// 归约到规范值（< p）。调用前 r 必须 < 2^256：此时 r < 2p + 38，
// 两次条件减 p 一定够。
void ReduceCanonical(Limb r[8]) {
  ConditionalSubtractPrime(r);
  ConditionalSubtractPrime(r);
}

void FeFromBytes(Fe* out, const unsigned char bytes[32]) {
  Limb r[8];
  for (std::size_t i = 0; i < 8; ++i) {
    r[i] = static_cast<Limb>(bytes[4 * i]) |
           (static_cast<Limb>(bytes[4 * i + 1]) << 8) |
           (static_cast<Limb>(bytes[4 * i + 2]) << 16) |
           (static_cast<Limb>(bytes[4 * i + 3]) << 24);
  }
  // RFC 7748 §5 decodeUCoordinate：X25519 用到的 u 坐标要屏蔽最高位
  // （bit 255），这样 32 字节输入总能映射到一个 < 2^255 的值。
  r[7] &= 0x7FFFFFFFu;
  ReduceCanonical(r);
  std::memcpy(out->v, r, sizeof(r));
}

void FeToBytes(const Fe& value, unsigned char bytes[32]) {
  Limb r[8];
  std::memcpy(r, value.v, sizeof(r));
  ReduceCanonical(r);
  for (std::size_t i = 0; i < 8; ++i) {
    bytes[4 * i] = static_cast<unsigned char>(r[i] & 0xFFu);
    bytes[4 * i + 1] = static_cast<unsigned char>((r[i] >> 8) & 0xFFu);
    bytes[4 * i + 2] = static_cast<unsigned char>((r[i] >> 16) & 0xFFu);
    bytes[4 * i + 3] = static_cast<unsigned char>((r[i] >> 24) & 0xFFu);
  }
}

void FeAdd(const Fe& a, const Fe& b, Fe* out) {
  Limb r[8];
  std::uint64_t carry = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    const std::uint64_t cur =
        static_cast<std::uint64_t>(a.v[i]) + b.v[i] + carry;
    r[i] = Low32(cur);
    carry = cur >> 32;
  }
  // a, b < p < 2^255，所以 a + b < 2^256，carry 必然为 0；这里仍然把它
  // 折回去（2^256 ≡ 38），只是为了"任何输入都不越界"这条更强的性质。
  if (carry != 0) {
    std::uint64_t cur = static_cast<std::uint64_t>(r[0]) + carry * 38ull;
    r[0] = Low32(cur);
    std::uint64_t extra = cur >> 32;
    for (std::size_t i = 1; extra != 0 && i < 8; ++i) {
      cur = static_cast<std::uint64_t>(r[i]) + extra;
      r[i] = Low32(cur);
      extra = cur >> 32;
    }
  }
  ReduceCanonical(r);
  std::memcpy(out->v, r, sizeof(r));
}

// r -= value（小整数）。调用方保证不会二次借位（见 FeSub 的说明）。
void SubtractSmall(Limb r[8], Limb value) {
  std::int64_t borrow = 0;
  std::int64_t cur = static_cast<std::int64_t>(r[0]) -
                     static_cast<std::int64_t>(value);
  r[0] = Low32Signed(cur);
  borrow = (cur < 0) ? 1 : 0;
  for (std::size_t i = 1; borrow != 0 && i < 8; ++i) {
    cur = static_cast<std::int64_t>(r[i]) - borrow;
    r[i] = Low32Signed(cur);
    borrow = (cur < 0) ? 1 : 0;
  }
}

void FeSub(const Fe& a, const Fe& b, Fe* out) {
  Limb r[8];
  std::int64_t borrow = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    const std::int64_t cur = static_cast<std::int64_t>(a.v[i]) -
                             static_cast<std::int64_t>(b.v[i]) - borrow;
    r[i] = Low32Signed(cur);
    borrow = (cur < 0) ? 1 : 0;
  }
  // 借位说明真实差值为负，此时 r 是 a - b + 2^256。
  // 因为 2^256 ≡ 38 (mod p)，减去 38 就把它改回 a - b (mod p)。
  //
  // 这一步不会再次借位：输入都是规范值，d = b - a <= p - 1，
  // 所以 r = 2^256 - d >= 2^256 - p + 1 = 2^255 + 20 > 38。
  if (borrow != 0) {
    SubtractSmall(r, 38u);
  }
  ReduceCanonical(r);
  std::memcpy(out->v, r, sizeof(r));
}

void FeMul(const Fe& a, const Fe& b, Fe* out) {
  // 8x8 教科书乘法，16 个 2^32 进制 limb。每一步都带进位链，
  // 因此 t[] 里每个 limb 在使用时都 < 2^32，累加不会溢出 uint64_t：
  //   (2^32-1)^2 + 2*(2^32-1) = 2^64 - 1。
  std::uint64_t t[16] = {0};
  for (std::size_t i = 0; i < 8; ++i) {
    std::uint64_t carry = 0;
    for (std::size_t j = 0; j < 8; ++j) {
      const std::uint64_t cur =
          t[i + j] + static_cast<std::uint64_t>(a.v[i]) * b.v[j] + carry;
      t[i + j] = cur & kLimbMask64;
      carry = cur >> 32;
    }
    std::uint64_t cur = t[i + 8] + carry;
    t[i + 8] = cur & kLimbMask64;
    std::uint64_t extra = cur >> 32;
    for (std::size_t k = i + 9; extra != 0 && k < 16; ++k) {
      cur = t[k] + extra;
      t[k] = cur & kLimbMask64;
      extra = cur >> 32;
    }
  }

  // 2^256 ≡ 38 (mod p)：把高 8 个 limb 折回低 8 个。
  // 38 * (2^32 - 1) + (2^32 - 1) + carry < 2^38，仍然只用 uint64_t。
  Limb w[9];
  std::uint64_t carry = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    const std::uint64_t cur = t[i] + 38ull * t[i + 8] + carry;
    w[i] = Low32(cur);
    carry = cur >> 32;
  }
  w[8] = Low32(carry);
  // 折回来之后可能又产生一个很小的第 9 个 limb，再折一次（最多两轮）。
  while (w[8] != 0) {
    std::uint64_t extra = 38ull * static_cast<std::uint64_t>(w[8]);
    w[8] = 0;
    std::uint64_t cur = static_cast<std::uint64_t>(w[0]) + extra;
    w[0] = Low32(cur);
    extra = cur >> 32;
    for (std::size_t i = 1; extra != 0 && i < 8; ++i) {
      cur = static_cast<std::uint64_t>(w[i]) + extra;
      w[i] = Low32(cur);
      extra = cur >> 32;
    }
    w[8] = Low32(extra);
  }

  Limb r[8];
  std::memcpy(r, w, sizeof(r));
  ReduceCanonical(r);
  std::memcpy(out->v, r, sizeof(r));
}

void FeMulSmall(const Fe& a, Limb k, Fe* out) {
  Fe constant{};
  constant.v[0] = k;
  FeMul(a, constant, out);
}

// a^(p-2)：p - 2 = 2^255 - 21，二进制低 5 位是 01011，第 5..254 位全是 1。
// 固定次数的平方-乘法（255 次平方 + 253 次乘法）。
void FeInvert(const Fe& a, Fe* out) {
  Fe result{};
  result.v[0] = 1u;
  Fe base = a;
  for (int i = 254; i >= 0; --i) {
    Fe squared;
    FeMul(result, result, &squared);
    result = squared;
    const bool bit = (i > 4) || (i == 0) || (i == 1) || (i == 3);
    if (bit) {
      Fe multiplied;
      FeMul(result, base, &multiplied);
      result = multiplied;
    }
  }
  *out = result;
}

// 条件交换：swap 只能是 0 或 1，按掩码整份交换，不按秘密比特分支。
void Cswap(Limb swap, Fe* a, Fe* b) {
  const Limb mask = static_cast<Limb>(0u - swap);
  for (std::size_t i = 0; i < 8; ++i) {
    const Limb t = mask & (a->v[i] ^ b->v[i]);
    a->v[i] ^= t;
    b->v[i] ^= t;
  }
}

// RFC 7748 §5 的 Montgomery ladder（X25519 函数）。
void MontgomeryLadder(const unsigned char scalar[32], const Fe& x1, Fe* out) {
  Fe x2{};
  Fe z2{};
  Fe x3 = x1;
  Fe z3{};
  x2.v[0] = 1u;
  z3.v[0] = 1u;

  Limb swap = 0;
  for (int t = 254; t >= 0; --t) {
    const Limb bit = static_cast<Limb>(
        (scalar[static_cast<std::size_t>(t) >> 3] >>
         (static_cast<unsigned>(t) & 7u)) & 1u);
    swap ^= bit;
    Cswap(swap, &x2, &x3);
    Cswap(swap, &z2, &z3);
    swap = bit;

    Fe a;
    Fe aa;
    Fe b;
    Fe bb;
    Fe e;
    Fe c;
    Fe d;
    Fe da;
    Fe cb;
    Fe square;
    Fe tmp;
    FeAdd(x2, z2, &a);
    FeMul(a, a, &aa);
    FeSub(x2, z2, &b);
    FeMul(b, b, &bb);
    FeSub(aa, bb, &e);
    FeAdd(x3, z3, &c);
    FeSub(x3, z3, &d);
    FeMul(d, a, &da);
    FeMul(c, b, &cb);
    // x_3 = (DA + CB)^2
    FeAdd(da, cb, &tmp);
    FeMul(tmp, tmp, &x3);
    // z_3 = x_1 * (DA - CB)^2
    FeSub(da, cb, &tmp);
    FeMul(tmp, tmp, &square);
    FeMul(x1, square, &z3);
    // x_2 = AA * BB
    FeMul(aa, bb, &x2);
    // z_2 = E * (AA + a24 * E)，a24 = 121665（RFC 7748 §5）。
    Fe a24e;
    FeMulSmall(e, 121665u, &a24e);
    FeAdd(aa, a24e, &tmp);
    FeMul(e, tmp, &z2);
  }
  Cswap(swap, &x2, &x3);
  Cswap(swap, &z2, &z3);

  Fe inverse;
  FeInvert(z2, &inverse);
  FeMul(x2, inverse, out);
}

bool AllZero(const std::string& data) {
  unsigned char acc = 0;
  for (std::size_t i = 0; i < data.size(); ++i) {
    acc = static_cast<unsigned char>(acc | static_cast<unsigned char>(data[i]));
  }
  return acc == 0;
}

void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) {
    *error_message = text;
  }
}

}  // namespace

void X25519ClampScalar(unsigned char scalar[kX25519KeySize]) {
  scalar[0] = static_cast<unsigned char>(scalar[0] & 248u);
  scalar[31] = static_cast<unsigned char>(scalar[31] & 127u);
  scalar[31] = static_cast<unsigned char>(scalar[31] | 64u);
}

bool X25519(const std::string& scalar, const std::string& u_coordinate,
            std::string* out, std::string* error_message) {
  if (out == nullptr) {
    SetError(error_message, "X25519 的输出指针为空");
    return false;
  }
  out->clear();
  if (scalar.size() != kX25519KeySize) {
    SetError(error_message, "X25519 的标量必须是 32 字节（实际 " +
                                std::to_string(scalar.size()) + " 字节）");
    return false;
  }
  if (u_coordinate.size() != kX25519KeySize) {
    SetError(error_message, "X25519 的 u 坐标必须是 32 字节（实际 " +
                                std::to_string(u_coordinate.size()) + " 字节）");
    return false;
  }

  unsigned char clamped[32];
  std::memcpy(clamped, scalar.data(), 32);
  X25519ClampScalar(clamped);

  Fe x1;
  FeFromBytes(&x1, reinterpret_cast<const unsigned char*>(u_coordinate.data()));

  Fe result;
  MontgomeryLadder(clamped, x1, &result);

  unsigned char bytes[32];
  FeToBytes(result, bytes);
  out->assign(reinterpret_cast<const char*>(bytes), sizeof(bytes));
  return true;
}

bool X25519SharedSecret(const std::string& scalar,
                        const std::string& u_coordinate, std::string* out,
                        std::string* error_message) {
  std::string secret;
  if (!X25519(scalar, u_coordinate, &secret, error_message)) {
    return false;
  }
  if (AllZero(secret)) {
    if (out != nullptr) {
      out->clear();
    }
    SetError(error_message,
             "X25519 共享秘密全零：对端给的 u 坐标落在低阶点上，拒绝使用");
    return false;
  }
  if (out != nullptr) {
    *out = secret;
  }
  return true;
}

bool X25519PublicKeyFromPrivate(const std::string& private_key,
                                std::string* public_key,
                                std::string* error_message) {
  if (private_key.size() != kX25519KeySize) {
    SetError(error_message, "X25519 私钥必须是 32 字节（实际 " +
                                std::to_string(private_key.size()) + " 字节）");
    return false;
  }
  static const char kBasePoint[kX25519KeySize] = {9};
  std::string base_point(kBasePoint, kX25519KeySize);
  return X25519(private_key, base_point, public_key, error_message);
}

bool X25519GenerateKeyPair(std::string* private_key, std::string* public_key,
                           std::string* error_message) {
  if (private_key == nullptr || public_key == nullptr) {
    SetError(error_message, "X25519 生成密钥对时输出指针为空");
    return false;
  }
  std::string material;
  if (!RandomBytes(kX25519KeySize, &material, error_message)) {
    return false;
  }
  unsigned char clamped[32];
  std::memcpy(clamped, material.data(), 32);
  X25519ClampScalar(clamped);
  private_key->assign(reinterpret_cast<const char*>(clamped), sizeof(clamped));
  return X25519PublicKeyFromPrivate(*private_key, public_key, error_message);
}

std::string X25519Fingerprint(const std::string& public_key) {
  if (public_key.size() != kX25519KeySize) {
    return std::string();
  }
  return Sha256Hex(public_key);
}

std::string X25519FormatKeyHex(const std::string& public_key) {
  if (public_key.size() != kX25519KeySize) {
    return std::string();
  }
  return ToHex(reinterpret_cast<const unsigned char*>(public_key.data()),
               public_key.size());
}

bool X25519ParseKeyText(const std::string& text, std::string* out,
                        std::string* error_message) {
  if (out == nullptr) {
    SetError(error_message, "X25519 解析公钥时输出指针为空");
    return false;
  }
  std::string body = text;
  if (body.compare(0, 4, "hex:") == 0) {
    body = body.substr(4);
  } else if (body.find(':') != std::string::npos) {
    SetError(error_message,
             "不认识的公钥前缀（只接受 hex: 前缀，或直接给 64 个十六进制字符）");
    return false;
  }
  if (body.size() != kX25519KeySize * 2) {
    SetError(error_message, "公钥的十六进制长度必须是 64 个字符（实际 " +
                                std::to_string(body.size()) + " 个）");
    return false;
  }
  std::string parsed;
  if (!FromHex(body, &parsed) || parsed.size() != kX25519KeySize) {
    SetError(error_message, "公钥不是合法的十六进制字符串");
    return false;
  }
  *out = parsed;
  return true;
}

}  // namespace crypto
}  // namespace backupproject
