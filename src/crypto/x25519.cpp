// src/crypto/x25519.cpp
//
// 手写 X25519（RFC 7748）实现。算法结构见 include/x25519.h 的文件头说明。
//
// 这里刻意不复用项目里既有的"加密原语"之外的任何东西：有限域、ladder、
// 编解码都是本文件自己的代码；只有 SHA-256（指纹）与 OS CSPRNG（密钥生成）
// 来自 include/crypto.h——它们本来就是本项目自己写的手写原语。
//
// 实现要点（都直接对应 RFC 7748 §5）：
//   * 域元素是 8 个 2^32 进制 limb，小端在前；内部只保证 < 2^256，规范值
//     （< p）只在归约函数的出口与 FeToBytes 出现；
//   * 与秘密相关的选择一律用掩码完成（Cswap、条件减 p），ladder 固定 255 轮、
//     FeInvert 固定 255 次平方，没有秘密相关的分支或数组下标；
//   * 输入不可信：u 坐标的最高位按标准屏蔽，标量在本地 clamp，共享秘密全零
//     （低阶点）一律拒绝，属于 fail-closed 的边界判断。

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

// 域元素的表示：8 个 limb，v[0] 是最低 32 位；允许非规范值（v >= p），
// 只有经过 ReduceCanonical / FeToBytes 之后才是 [0, p) 的规范表示。
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
// 全程用位运算选择结果，不写 if (r >= p) r -= p：比较会引入数据相关的分支，
// 而这里的输入与私钥相关。borrow 只可能是 0 或 1。
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

// 32 字节小端 -> 域元素，输入通常来自对端（不可信）。
// 屏蔽 bit 255 是 RFC 7748 的规定：这样任意 32 字节都能映射成一个 < 2^255
// 的值，解码永远不失败，也就不存在"用失败来探测输入"的旁路。
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

// 域元素 -> 32 字节小端，出口先归约到规范值：同一份数学结果只能有一种线上
// 表示，否则双方对"共享秘密是否相等"的比较可能得到错误结论。
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
  std::int64_t cur =
      static_cast<std::int64_t>(r[0]) - static_cast<std::int64_t>(value);
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

// 先把 a * b 算成 16 个 limb，再按 2^256 ≡ 38 (mod p) 折叠回 8 个 limb。
// t[] 的每个 limb 在使用时都 < 2^32，所以 32x32 乘法加上进位不会溢出 uint64。
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
// 用费马小定理求逆：a^(p-2) = a^-1 (mod p)，因为 p 是素数且 a != 0。
// 指数是公开常量，迭代次数固定；是否做乘法只取决于循环下标 i，
// 与秘密无关，因此执行时间是常数级的。
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
// swap 只取 0/1，掩码 0 - swap 得到全 0 或全 1：交换整份元素而不按秘密位
// 分支。调用方的 swap 是"上一位与本位异或"的累计值，因此循环结束后还要
// 再补一次交换，让最终结果落在正确的变量里。
void Cswap(Limb swap, Fe* a, Fe* b) {
  const Limb mask = static_cast<Limb>(0u - swap);
  for (std::size_t i = 0; i < 8; ++i) {
    const Limb t = mask & (a->v[i] ^ b->v[i]);
    a->v[i] ^= t;
    b->v[i] ^= t;
  }
}

// RFC 7748 §5 的 Montgomery ladder（X25519 函数）。
// 输入 scalar 必须已经 clamp（位数与 cofactor 都处理过），x1 是仿射 u 坐标。
// 变量对应 RFC 7748 的伪代码：(x2, z2) 是"差"点，(x3, z3) 是"和"点，
// 不变量是 x3 - x2 = x1；每轮用差分加法公式同时更新两点，由标量位决定是否
// 先交换。x1 固定不变，正是 Montgomery 曲线差分加法的前提。
// 出口用一次 FeInvert 把射影坐标变回仿射：x = x2 / z2。
// 这样做只需要一次求逆而不是每轮一次，代价是必须保证 z2 非零。
void MontgomeryLadder(const unsigned char scalar[32], const Fe& x1, Fe* out) {
  Fe x2{};
  Fe z2{};
  Fe x3 = x1;
  Fe z3{};
  x2.v[0] = 1u;
  z3.v[0] = 1u;

  Limb swap = 0;
  for (int t = 254; t >= 0; --t) {
    const Limb bit =
        static_cast<Limb>((scalar[static_cast<std::size_t>(t) >> 3] >>
                           (static_cast<unsigned>(t) & 7u)) &
                          1u);
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

// 全零检测按位累加，不提前返回：提前退出会让执行时间泄漏"第几个字节非零"。
// 这里的输入是共享秘密，因此连这种顺序无关的时间差也避开。
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

// RFC 7748 §5 的 decodeScalar25519：清低 3 位（保证是 cofactor 8 的倍数，
// 消除小子群分量）、清最高位、置倒数第二位。clamp 由本地做，所以调用方传入
// 未经处理的随机字节也是安全的；对已经 clamp 过的输入是幂等的。
void X25519ClampScalar(unsigned char scalar[kX25519KeySize]) {
  scalar[0] = static_cast<unsigned char>(scalar[0] & 248u);
  scalar[31] = static_cast<unsigned char>(scalar[31] & 127u);
  scalar[31] = static_cast<unsigned char>(scalar[31] | 64u);
}

// 原始 X25519 标量乘。scalar 与 u_coordinate 长度不是 32 字节即失败；
// 成功时 *out 被赋成 32 字节结果，失败时 *out 保持调用前的值。
// 更正：本函数在入口就执行 out->clear()，因此长度校验失败时 *out 是空串，
// 而不是"保持调用前的值"；调用方不要依赖失败时出参不变。
// 本函数不做低阶点检查：公钥推导本来就不需要它，需要共享秘密语义的调用方
// 应使用 X25519SharedSecret。
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
                                std::to_string(u_coordinate.size()) +
                                " 字节）");
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

// 共享秘密语义：比 X25519 多一条"结果不能是全零"的检查。
// 全零意味着对端给的 u 坐标落在低阶点上（攻击者可以让所有共享秘密都变成
// 同一个值），这里 fail-closed 直接拒绝，并把 *out 清空而不是交出那个零值。
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

// 公钥 = 私钥 * 基点，而 X25519 的基点在 wire format 里就是 u = 9、其余 31
// 字节为 0。私钥在这里被内部 clamp，传入未 clamp 的字节也得到一致结果。
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

// 随机私钥来自 OS CSPRNG（RandomBytes 失败即整体失败，绝不回落到弱随机源），
// clamp 之后才存进 *private_key——存的是参与运算的那个形式，这样"保存的
// 私钥"与"实际使用的标量"是同一个值，不会出现换个实现推导出不同公钥的
// 兼容性问题。
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

// 指纹 = 公钥的 SHA-256 十六进制串，只用于人工核对与 TOFU 展示，不是密钥
// 派生：长度不合法时返回空串，绝不哈希一个被截断的公钥。
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

// 解析用户输入的公钥文本：接受 "hex:<64 个十六进制字符>" 或裸的 64 个
// 十六进制字符；出现其它前缀（如 base64:）直接拒绝，而不是"猜一种编码"。
// 长度与字符集都按最严格的方式检查，避免同一个密钥有多种文本表示。
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
    SetError(
        error_message,
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
