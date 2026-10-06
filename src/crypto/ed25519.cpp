// src/crypto/ed25519.cpp
//
// 手写 Ed25519（RFC 8032 §5.1）。只用 C++17 标准库 + 手写 SHA-512。
//
// 表示与算法（都可逐条对照标准复核）：
//   * 域元素 mod p = 2^255 - 19：4 个 64 位 limb（little-endian），乘法用
//     UInt128 累加，再用 2^256 ≡ 38 (mod p) 折叠回 256 位；
//   * 标量 mod L = 2^252 + 27742317777372353535851937790883648493：
//     512 轮"左移一位 + 条件减 L"的按位归约，轮数固定；
//   * 点用扩展坐标 (X:Y:Z:T)，加法用 a = -1 twisted Edwards 的完备公式
//     （add-2008-hwcd-3）—— 同一个公式对 P == Q 也成立，所以倍点直接复用它；
//   * 标量乘：固定 256 轮的 double-and-add，加不加用掩码选择，不按比特跳步。
//
// 安全边界（诚实写在这里，不假装）：
//   * 本实现是**常量轮数**而非严格 constant-time：域乘法的进位链与 __int128
//     的代码生成是否与数据无关，不由本文件保证；也没有做侧信道审计；
//   * 验签只做公开数据运算；签名里对秘密标量用掩码选择，没有提前退出；
//   * 与项目其它手写原语一致：教学实现，未经形式化验证与第三方审计。

// 文件结构：匿名命名空间里由下往上依次是域运算（Fe）、标量运算（Sc）、
// 点运算（Ge），之后才是公开 API；三层都不用动态分配与异常。
//
// 失败语义：公开 API 返回 bool 或
// Ed25519VerifyResult，
// 失败时不留半截输出——输出参数要么被赋成完整结果，要么被清空。
// error_message 全部可选。
//
// 线程安全：本文件没有可变的全局状态，预计算常量都是函数内
// static（C++11 起初始化线程安全，之后只读），
// 因此公开函数可被多线程并发调用。
//
// 秘密材料只存在于栈上，返回前 memset 尽力清零；这是尽力而为：
// 编译器有权消除对即将离开作用域的数组的写入。
#include "ed25519.h"

#include <cstring>

#include "crypto.h"

namespace backupproject {
namespace crypto {
namespace {

// ---------------- 域元素 mod p = 2^255 - 19 ----------------

// 128 位中间量。用 __extension__ 明确告诉编译器这是有意的扩展：
// 项目的编译标准里带 -Wpedantic，不这样写会为每个 __int128 报一条
// "ISO C++ does not support __int128"，而本仓库要求 0 warning。
__extension__ typedef unsigned __int128 UInt128;

// 域元素 mod p，4 个 64 位 limb，
// little-endian（v[0] 是最低位）。对外可见的值保持在 [0, p)
// 的规范区间，只有 FeMul 内部的 8 limb 中间量允许超出。
//
// Fe 不是安全容器：没有清零、没有拷贝控制，别把它当秘密类型来用。
struct Fe {
  std::uint64_t v[4];
};

// p = 2^255 - 19，按 limb 从低到高书写。抄错一个 nibble
// 不会编译失败，只会让全部结果一起错，只有 RFC 8032 测试向量能发现。
constexpr std::uint64_t kP[4] = {0xFFFFFFFFFFFFFFEDULL, 0xFFFFFFFFFFFFFFFFULL,
                                 0xFFFFFFFFFFFFFFFFULL, 0x7FFFFFFFFFFFFFFFULL};

// d = -121665/121666 mod p（RFC 8032 §5.1）。
constexpr std::uint64_t kD[4] = {0x75EB4DCA135978A3ULL, 0x00700A4D4141D8ABULL,
                                 0x8CC740797779E898ULL, 0x52036CEE2B6FFE73ULL};

// 基点（RFC 8032 §5.1）。
constexpr std::uint64_t kBx[4] = {0xC9562D608F25D51AULL, 0x692CC7609525A7B2ULL,
                                  0xC0A4E231FDD6DC5CULL, 0x216936D3CD6E53FEULL};
constexpr std::uint64_t kBy[4] = {0x6666666666666658ULL, 0x6666666666666666ULL,
                                  0x6666666666666666ULL, 0x6666666666666666ULL};

inline void FeZero(Fe* r) { std::memset(r->v, 0, sizeof(r->v)); }

inline void FeOne(Fe* r) {
  FeZero(r);
  r->v[0] = 1;
}

inline void FeFromLimbs(Fe* r, const std::uint64_t limbs[4]) {
  for (int i = 0; i < 4; ++i) {
    r->v[i] = limbs[i];
  }
}

// 常数时间条件减法：v >= p 时减 p。掩码实现，无分支。
inline void FeCondSubP(std::uint64_t v[4]) {
  std::uint64_t r[4];
  std::uint64_t borrow = 0;
  for (int i = 0; i < 4; ++i) {
    const std::uint64_t t = v[i] - kP[i];
    const std::uint64_t b1 = (v[i] < kP[i]) ? 1ULL : 0ULL;
    const std::uint64_t u = t - borrow;
    const std::uint64_t b2 = (t < borrow) ? 1ULL : 0ULL;
    r[i] = u;
    borrow = b1 | b2;
  }
  const std::uint64_t mask = borrow - 1ULL;  // borrow==0（v>=p）-> 全 1
  for (int i = 0; i < 4; ++i) {
    v[i] = (v[i] & ~mask) | (r[i] & mask);
  }
}

// v + carry*2^256 ≡ v + carry*38 (mod p)，然后再规范一次。
// 两轮就够：carry*38 最多再产生一个进位，第二轮的进位必然为 0。
// 末尾连做两次 FeCondSubP，让结果无条件落入 [0, p)。
inline void FeReduceCarry(std::uint64_t v[4], std::uint64_t carry) {
  for (int round = 0; round < 2 && carry != 0; ++round) {
    UInt128 x = static_cast<UInt128>(v[0]) + static_cast<UInt128>(carry) * 38;
    v[0] = static_cast<std::uint64_t>(x);
    carry = static_cast<std::uint64_t>(x >> 64);
    for (int i = 1; i < 4; ++i) {
      x = static_cast<UInt128>(v[i]) + carry;
      v[i] = static_cast<std::uint64_t>(x);
      carry = static_cast<std::uint64_t>(x >> 64);
    }
  }
  FeCondSubP(v);
  FeCondSubP(v);
}

// 8 limb（512 位，t[7] 的高位必须为 0）归约 mod p。
// 前置条件：t[7] 的最高位为 0（乘积最大 2^510 量级，512
// 位放得下）。
//
// 折两次的理由：第一次把 2^256 == 38 (mod p) 作用到高 4 limb
// 上，会产生新的 limb4/limb5；第二次把这两个小 limb 折回去，
// 剩下的溢出才小到能被 FeReduceCarry 一次处理干净。
inline void FeReduceWide8(const std::uint64_t t[8], Fe* out) {
  std::uint64_t r[6] = {0, 0, 0, 0, 0, 0};
  UInt128 carry = 0;
  for (int i = 0; i < 4; ++i) {
    const UInt128 x = static_cast<UInt128>(t[4 + i]) * 38 + carry;
    r[i] = static_cast<std::uint64_t>(x);
    carry = x >> 64;
  }
  r[4] = static_cast<std::uint64_t>(carry);
  carry = 0;
  for (int i = 0; i < 4; ++i) {
    const UInt128 x = static_cast<UInt128>(r[i]) + t[i] + carry;
    r[i] = static_cast<std::uint64_t>(x);
    carry = x >> 64;
  }
  {
    const UInt128 x = static_cast<UInt128>(r[4]) + carry;
    r[4] = static_cast<std::uint64_t>(x);
    r[5] = static_cast<std::uint64_t>(x >> 64);
  }
  // 第二折：limb4/limb5 此时都很小。
  carry = 0;
  for (int i = 0; i < 4; ++i) {
    const std::uint64_t hi = (i == 0) ? r[4] : ((i == 1) ? r[5] : 0ULL);
    const UInt128 x =
        static_cast<UInt128>(r[i]) + static_cast<UInt128>(hi) * 38 + carry;
    r[i] = static_cast<std::uint64_t>(x);
    carry = x >> 64;
  }
  std::uint64_t v[4] = {r[0], r[1], r[2], r[3]};
  FeReduceCarry(v, static_cast<std::uint64_t>(carry));
  FeFromLimbs(out, v);
}

inline void FeAdd(Fe* out, const Fe& a, const Fe& b) {
  std::uint64_t v[4];
  std::uint64_t carry = 0;
  for (int i = 0; i < 4; ++i) {
    const UInt128 x = static_cast<UInt128>(a.v[i]) + b.v[i] + carry;
    v[i] = static_cast<std::uint64_t>(x);
    carry = static_cast<std::uint64_t>(x >> 64);
  }
  FeReduceCarry(v, carry);
  FeFromLimbs(out, v);
}

inline void FeSub(Fe* out, const Fe& a, const Fe& b) {
  std::uint64_t r[4];
  std::uint64_t borrow = 0;
  for (int i = 0; i < 4; ++i) {
    const std::uint64_t t = a.v[i] - b.v[i];
    const std::uint64_t b1 = (a.v[i] < b.v[i]) ? 1ULL : 0ULL;
    const std::uint64_t u = t - borrow;
    const std::uint64_t b2 = (t < borrow) ? 1ULL : 0ULL;
    r[i] = u;
    borrow = b1 | b2;
  }
  // a < b 时上面的减法借了一个 2^256，所以要补加 p 把差值拉回 [0, p)。
  // 补加 p 之后必然再产生一个新的 2^256 进位——那个进位是"借位表示"的
  // 人造产物，必须**丢掉**：低 256 位此时已经就是正确的 a - b + p。
  // 若把它当成真溢出折成 38，结果会凭空多出 38（这正是此前 -x + x != 0
  // 的根因）。
  const std::uint64_t mask = 0ULL - borrow;
  std::uint64_t carry = 0;
  for (int i = 0; i < 4; ++i) {
    const UInt128 x = static_cast<UInt128>(r[i]) + (kP[i] & mask) + carry;
    r[i] = static_cast<std::uint64_t>(x);
    carry = static_cast<std::uint64_t>(x >> 64);  // limb 间进位照常传
  }
  // 只剩最后一个最高位进位要处理：它是"借位表示 + 补加 p"这个人造过程的
  // 产物（值为 2^256），低 256 位已经是正确的 a - b + p，直接丢弃。
  // 注意：这里只能丢这一个，limb 之间的进位一个都不能少。
  (void)carry;
  FeCondSubP(r);  // 防御性规范化：规范输入下 r <= p-1，这里是空操作
  FeFromLimbs(out, r);
}

inline void FeMul(Fe* out, const Fe& a, const Fe& b) {
  // 学校算法，但**每行乘完立刻把进位往后传**。
  // 不能写成"先把同一列的 4 个乘积累加进一个 128 位槽、最后再统一进位"：
  // 单个乘积最大 (2^64-1)^2，同一列 4 个相加约 2^130，已经越过 128 位，
  // 会静默回绕成错值（(p-1)^2 就是这么算错的，且只有大数才暴露）。
  // 逐行进位的中间量上界：
  //   (2^64-1)^2 + (2^64-1) + (2^64-1) = 2^128 - 1，正好放得下。
  std::uint64_t t[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  for (int i = 0; i < 4; ++i) {
    UInt128 carry = 0;
    for (int j = 0; j < 4; ++j) {
      const UInt128 x =
          static_cast<UInt128>(a.v[i]) * b.v[j] + t[i + j] + carry;
      t[i + j] = static_cast<std::uint64_t>(x);
      carry = x >> 64;
    }
    t[i + 4] = static_cast<std::uint64_t>(carry);
  }
  FeReduceWide8(t, out);
}

inline void FeSqr(Fe* out, const Fe& a) { FeMul(out, a, a); }

inline void FeMulSmall(Fe* out, const Fe& a, std::uint64_t k) {
  Fe f;
  FeZero(&f);
  f.v[0] = k;
  FeMul(out, a, f);
}

inline void FeNeg(Fe* out, const Fe& a) {
  Fe zero;
  FeZero(&zero);
  FeSub(out, zero, a);
}

inline bool FeIsZero(const Fe& a) {
  return (a.v[0] | a.v[1] | a.v[2] | a.v[3]) == 0;
}

inline bool FeEqual(const Fe& a, const Fe& b) {
  std::uint64_t acc = 0;
  for (int i = 0; i < 4; ++i) {
    acc |= a.v[i] ^ b.v[i];
  }
  return acc == 0;
}

// b == 1 时 *r = a；掩码实现。
inline void FeCmov(Fe* r, const Fe& a, std::uint64_t b) {
  const std::uint64_t mask = 0ULL - (b & 1ULL);
  for (int i = 0; i < 4; ++i) {
    r->v[i] ^= (r->v[i] ^ a.v[i]) & mask;
  }
}

inline bool FeIsNegative(const Fe& a) { return (a.v[0] & 1ULL) != 0; }

inline void FeFromBytesMasked(Fe* out, const unsigned char in[32]) {
  for (int i = 0; i < 4; ++i) {
    std::uint64_t limb = 0;
    for (int j = 7; j >= 0; --j) {
      limb = (limb << 8) | in[i * 8 + j];
    }
    out->v[i] = limb;
  }
  out->v[3] &= 0x7FFFFFFFFFFFFFFFULL;
}

// 读入并要求 canonical（value < p）。
// 返回值必须检查：false 表示编码 >= p，
// 也就是同一个域元素存在第二种字节表示；不检查就等于允许可延展编码，
// 公钥指纹与缓存键都会失效。
inline bool FeFromBytesCanonical(Fe* out, const unsigned char in[32]) {
  FeFromBytesMasked(out, in);
  std::uint64_t borrow = 0;
  for (int i = 0; i < 4; ++i) {
    const std::uint64_t t = out->v[i] - kP[i];
    const std::uint64_t b1 = (out->v[i] < kP[i]) ? 1ULL : 0ULL;
    const std::uint64_t b2 = (t < borrow) ? 1ULL : 0ULL;
    borrow = b1 | b2;
  }
  return borrow == 1;  // 有借位 = v < p
}

inline void FeToBytes(unsigned char out[32], const Fe& a) {
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 8; ++j) {
      out[i * 8 + j] = static_cast<unsigned char>(a.v[i] >> (8 * j));
    }
  }
}

// 固定 256 轮的平方-乘。exp 是 32 字节 little-endian。
inline void FePow(Fe* out, const Fe& base, const unsigned char exp[32]) {
  Fe result;
  FeOne(&result);
  for (int i = 255; i >= 0; --i) {
    FeSqr(&result, result);
    Fe multiplied;
    FeMul(&multiplied, result, base);
    const std::uint64_t bit = (exp[i >> 3] >> (i & 7)) & 1ULL;
    FeCmov(&result, multiplied, bit);
  }
  *out = result;
}

inline void FeInvert(Fe* out, const Fe& a) {
  // a^(p-2) = a^(2^255 - 21)
  static const unsigned char kExp[32] = {
      0xEB, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
      0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
      0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F};
  FePow(out, a, kExp);
}

// sqrt(-1) = 2^((p-1)/4)。
// 指数不手抄 32 字节常量，而是从 kP 现场算出来：(p-1) 逻辑右移 2 位。
// 这里曾经手抄成 0x0F..F3（＝2^252-13，(p-5)/8 那个常量的走样版本），
// 后果不是"全错"而是"一半错"：只有需要第二个平方根候选的公钥会被判成
// 不在曲线上，非常难查。改成推导之后这类抄写错误不可能再发生。
const Fe& SqrtM1() {
  static const Fe value = [] {
    std::uint64_t v[4] = {kP[0] - 1ULL, kP[1], kP[2], kP[3]};  // p - 1
    std::uint64_t carry = 0;
    for (int i = 3; i >= 0; --i) {  // 整体右移 2 位 = 除以 4
      const std::uint64_t next = v[i] & 3ULL;
      v[i] = (v[i] >> 2) | (carry << 62);
      carry = next;
    }
    unsigned char exp[32];
    for (int i = 0; i < 4; ++i) {
      for (int j = 0; j < 8; ++j) {
        exp[i * 8 + j] = static_cast<unsigned char>(v[i] >> (8 * j));
      }
    }
    Fe two;
    FeZero(&two);
    two.v[0] = 2;
    Fe out;
    FePow(&out, two, exp);
    return out;
  }();
  return value;
}

const Fe& D() {
  static const Fe value = [] {
    Fe out;
    FeFromLimbs(&out, kD);
    return out;
  }();
  return value;
}

const Fe& D2() {
  static const Fe value = [] {
    Fe out;
    FeMulSmall(&out, D(), 2);
    return out;
  }();
  return value;
}

// ---------------- 标量 mod L ----------------

constexpr std::uint64_t kL[4] = {0x5812631A5CF5D3EDULL, 0x14DEF9DEA2F79CD6ULL,
                                 0x0000000000000000ULL, 0x1000000000000000ULL};

inline void ScCondSubL(std::uint64_t v[4]) {
  std::uint64_t r[4];
  std::uint64_t borrow = 0;
  for (int i = 0; i < 4; ++i) {
    const std::uint64_t t = v[i] - kL[i];
    const std::uint64_t b1 = (v[i] < kL[i]) ? 1ULL : 0ULL;
    const std::uint64_t u = t - borrow;
    const std::uint64_t b2 = (t < borrow) ? 1ULL : 0ULL;
    r[i] = u;
    borrow = b1 | b2;
  }
  const std::uint64_t mask = borrow - 1ULL;
  for (int i = 0; i < 4; ++i) {
    v[i] = (v[i] & ~mask) | (r[i] & mask);
  }
}

// 512 位 little-endian -> mod L。固定 512 轮。
// 不变量：每轮结束 acc < L。左移一位后 acc < 2L，所以一次条件减 L
// 就足以回到 [0, L)，不需要循环减法——这是固定 512 轮能成立的前提。
//
// 轮数固定、不提前结束：输入是秘密标量时，比特长度本身就是关于
// nonce 的信息。
void ScReduce512(const unsigned char in[64], std::uint64_t out[4]) {
  std::uint64_t acc[4] = {0, 0, 0, 0};
  for (int i = 511; i >= 0; --i) {
    const std::uint64_t bit = (in[i >> 3] >> (i & 7)) & 1ULL;
    acc[3] = (acc[3] << 1) | (acc[2] >> 63);
    acc[2] = (acc[2] << 1) | (acc[1] >> 63);
    acc[1] = (acc[1] << 1) | (acc[0] >> 63);
    acc[0] = (acc[0] << 1) | bit;
    ScCondSubL(acc);
  }
  for (int i = 0; i < 4; ++i) {
    out[i] = acc[i];
  }
}

void ScReduce32(const unsigned char in[32], std::uint64_t out[4]) {
  unsigned char wide[64];
  std::memcpy(wide, in, 32);
  std::memset(wide + 32, 0, 32);
  ScReduce512(wide, out);
}

// RFC 8032 §5.1.7 要求验签时检查 S < L。不查会留下签名延展性：
// 攻击者把合法签名的 S 加上 L，就得到一个不同却同样能通过的签名。
bool ScIsCanonical(const unsigned char in[32]) {
  std::uint64_t v[4];
  for (int i = 0; i < 4; ++i) {
    std::uint64_t limb = 0;
    for (int j = 7; j >= 0; --j) {
      limb = (limb << 8) | in[i * 8 + j];
    }
    v[i] = limb;
  }
  std::uint64_t borrow = 0;
  for (int i = 0; i < 4; ++i) {
    const std::uint64_t t = v[i] - kL[i];
    const std::uint64_t b1 = (v[i] < kL[i]) ? 1ULL : 0ULL;
    const std::uint64_t b2 = (t < borrow) ? 1ULL : 0ULL;
    borrow = b1 | b2;
  }
  return borrow == 1;
}

void ScToBytes(unsigned char out[32], const std::uint64_t v[4]) {
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 8; ++j) {
      out[i * 8 + j] = static_cast<unsigned char>(v[i] >> (8 * j));
    }
  }
}

// out = (a*b + c) mod L
void ScMulAdd(const std::uint64_t a[4], const std::uint64_t b[4],
              const std::uint64_t c[4], std::uint64_t out[4]) {
  // 和 FeMul 同一个坑：不能把同一列的 4 个乘积先累加进一个 128 位槽，
  // 4 * (2^64-1)^2 约 2^130 会静默回绕。改成逐行乘完立刻进位。
  std::uint64_t t[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  for (int i = 0; i < 4; ++i) {
    UInt128 row_carry = 0;
    for (int j = 0; j < 4; ++j) {
      const UInt128 x =
          static_cast<UInt128>(a[i]) * b[j] + t[i + j] + row_carry;
      t[i + j] = static_cast<std::uint64_t>(x);
      row_carry = x >> 64;
    }
    t[i + 4] = static_cast<std::uint64_t>(row_carry);
  }
  UInt128 carry = 0;
  for (int i = 0; i < 4; ++i) {
    const UInt128 x = static_cast<UInt128>(t[i]) + c[i] + carry;
    t[i] = static_cast<std::uint64_t>(x);
    carry = x >> 64;
  }
  for (int i = 4; i < 8; ++i) {
    const UInt128 x = static_cast<UInt128>(t[i]) + carry;
    t[i] = static_cast<std::uint64_t>(x);
    carry = x >> 64;
  }
  unsigned char wide[64];
  for (int i = 0; i < 8; ++i) {
    for (int j = 0; j < 8; ++j) {
      wide[i * 8 + j] = static_cast<unsigned char>(t[i] >> (8 * j));
    }
  }
  ScReduce512(wide, out);
}

// ---------------- edwards25519 点（扩展坐标） ----------------

// 扩展坐标点 (X : Y : Z : T)：x = X/Z、y = Y/Z、T = XY/Z，
// 满足 a = -1 的 twisted Edwards 方程。
//
// 不变量：Z 永不为 0（本文件从不产生射影无穷远点），仿射点用 Z = 1，
// 恒等元是 (0, 1, 1, 0)。
//
// 坐标不唯一：同一个点有无穷多种表示，比较字段之前必须用 GeEncode
// 归一化，或用 GeIsIdentity 做仿射化比较。
//
// Ge 是纯值类型（4 个 Fe、无堆分配），可以随意拷贝赋值，不存在所有权问题。
struct Ge {
  Fe X, Y, Z, T;
};

void GeIdentity(Ge* p) {
  FeZero(&p->X);
  FeOne(&p->Y);
  FeOne(&p->Z);
  FeZero(&p->T);
}

// a = -1 twisted Edwards 的完备加法（add-2008-hwcd-3）。P == Q 时同样成立。
// 完备公式的价值：P == Q、P == -Q、任一点为恒等元都不需要分支，
// 倍点直接复用同一段代码（见 GeDouble）。
//
// 别名安全：所有输入都在写 out 之前读进临时量，因此 out 可以与 p 或 q
// 是同一个对象。
void GeAdd(Ge* out, const Ge& p, const Ge& q) {
  Fe a, b, c, d, e, f, g, h, t, u;
  FeSub(&t, p.Y, p.X);
  FeSub(&u, q.Y, q.X);
  FeMul(&a, t, u);
  FeAdd(&t, p.Y, p.X);
  FeAdd(&u, q.Y, q.X);
  FeMul(&b, t, u);
  FeMul(&t, p.T, q.T);
  FeMul(&c, t, D2());
  FeMul(&d, p.Z, q.Z);  // D = 2*Z1*Z2
  FeMulSmall(&d, d, 2);
  FeSub(&e, b, a);
  FeSub(&f, d, c);
  FeAdd(&g, d, c);
  FeAdd(&h, b, a);
  FeMul(&out->X, e, f);
  FeMul(&out->Y, g, h);
  FeMul(&out->T, e, h);
  FeMul(&out->Z, f, g);
}

void GeDouble(Ge* out, const Ge& p) { GeAdd(out, p, p); }

void GeCmov(Ge* r, const Ge& a, std::uint64_t b) {
  FeCmov(&r->X, a.X, b);
  FeCmov(&r->Y, a.Y, b);
  FeCmov(&r->Z, a.Z, b);
  FeCmov(&r->T, a.T, b);
}

// 固定 256 轮 double-and-add。scalar 是 32 字节 little-endian。
void GeScalarMul(Ge* out, const Ge& p, const unsigned char scalar[32]) {
  Ge result;
  GeIdentity(&result);
  for (int i = 255; i >= 0; --i) {
    Ge doubled;
    GeDouble(&doubled, result);
    Ge sum;
    GeAdd(&sum, doubled, p);
    const std::uint64_t bit = (scalar[i >> 3] >> (i & 7)) & 1ULL;
    result = doubled;
    GeCmov(&result, sum, bit);
  }
  *out = result;
}

bool GeIsIdentity(const Ge& p) {
  Fe z_inv, x, y, one;
  FeInvert(&z_inv, p.Z);
  FeMul(&x, p.X, z_inv);
  FeMul(&y, p.Y, z_inv);
  FeOne(&one);
  return FeIsZero(x) && FeEqual(y, one);
}

// 8P == identity ？（落在小阶子群 / torsion 上）
bool GeIsSmallOrder(const Ge& p) {
  Ge t = p;
  for (int i = 0; i < 3; ++i) {
    GeDouble(&t, t);
  }
  return GeIsIdentity(t);
}

// 压缩编码布局（RFC 8032 §5.1.2）：32 字节 = y 的
// little-endian，最高位放 x 的最低位（符号位）；T 不参与编码，
// 解码侧由 x*y 重算。
//
// x = 0 且符号位为 1 是非规范编码，解码侧必须显式拒绝（见
// GeDecode）。
void GeEncode(unsigned char out[32], const Ge& p) {
  Fe z_inv, x, y;
  FeInvert(&z_inv, p.Z);
  FeMul(&x, p.X, z_inv);
  FeMul(&y, p.Y, z_inv);
  FeToBytes(out, y);
  out[31] =
      static_cast<unsigned char>(out[31] | (FeIsNegative(x) ? 0x80 : 0x00));
}

// 三态而不是 bool：kNonCanonical 是字节编码不合法（y >=
// p，或 x = 0 且符号位为 1），kNotOnCurve 是这个 y
// 在曲线上没有对应点。
//
// 验签把它们映射成不同的 Ed25519VerifyResult，
// 方便区分编码问题与数据被篡改。
enum class DecodeResult { kOk, kNonCanonical, kNotOnCurve };

// RFC 8032 §5.1.3
// 步骤（每一步都能对着 RFC 8032 §5.1.3 复核）：
//
//   1. 取符号位，解出 y 并要求规范（y < p）；
//
//   2. 令 u = y^2 - 1、v = d*y^2 + 1，x 的候选为 x =
//   (u/v)^((p+3)/8)；
//
//   3. 校验 v*x^2 == u，不等则改试 x*sqrt(-1)，
//   仍不等就是点不在曲线上；
//
//   4. 按符号位决定是否取 -x，最后补齐 Z = 1、T = x*y。
//
// 失败时 out 一个字节都不写，调用方可以安全地先解码再决定是否使用。
DecodeResult GeDecode(const unsigned char in[32], Ge* out) {
  const std::uint64_t sign = (in[31] >> 7) & 1ULL;
  Fe y;
  if (!FeFromBytesCanonical(&y, in)) {
    return DecodeResult::kNonCanonical;
  }
  Fe y2, u, v, one;
  FeOne(&one);
  FeSqr(&y2, y);
  FeSub(&u, y2, one);
  FeMul(&v, y2, D());
  FeAdd(&v, v, one);

  static const unsigned char kExpP58[32] = {
      0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
      0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
      0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F};
  Fe v2, v3, v7, uv3, uv7, powv, x;
  FeSqr(&v2, v);
  FeMul(&v3, v2, v);
  FeSqr(&v7, v3);     // v^6 = v^3 * v^3
  FeMul(&v7, v7, v);  // v^7
  // 这里以前多乘了一次 v^2（v^9），开方候选就永远对不上：v^9 与 v^7 的
  // 关系是 v^((p-1)/4-1)，不是平方根公式要的那个幂。
  FeMul(&uv3, u, v3);
  FeMul(&uv7, u, v7);
  FePow(&powv, uv7, kExpP58);
  FeMul(&x, uv3, powv);

  Fe vx2;
  FeSqr(&vx2, x);
  FeMul(&vx2, vx2, v);
  if (!FeEqual(vx2, u)) {
    Fe neg_u;
    FeNeg(&neg_u, u);
    if (FeEqual(vx2, neg_u)) {
      FeMul(&x, x, SqrtM1());
    } else {
      return DecodeResult::kNotOnCurve;
    }
  }
  if (FeIsZero(x) && sign == 1) {
    return DecodeResult::kNonCanonical;  // x = 0 且符号位为 1：非规范
  }
  if (FeIsNegative(x) != (sign == 1)) {
    FeNeg(&x, x);
  }
  out->X = x;
  out->Y = y;
  FeOne(&out->Z);
  FeMul(&out->T, x, y);
  return DecodeResult::kOk;
}

const Ge& BasePoint() {
  static const Ge value = [] {
    Ge p;
    FeFromLimbs(&p.X, kBx);
    FeFromLimbs(&p.Y, kBy);
    FeOne(&p.Z);
    FeMul(&p.T, p.X, p.Y);
    return p;
  }();
  return value;
}

void GeScalarMulBase(Ge* out, const unsigned char scalar[32]) {
  GeScalarMul(out, BasePoint(), scalar);
}

// RFC 8032 §5.1.5 的标量夹紧。
// 三步的效果：清掉最低 3 位（吸收 cofactor 8 带来的小阶分量）、
// 清最高位、置位第 254 位，得到 2^254 <= a < 2^255 且 a
// 是 8的倍数。
//
// 这是夹紧而不是校验：任何 32 字节输入都会被接受，不会失败。
void ClampScalar(unsigned char a[32]) {
  a[0] = static_cast<unsigned char>(a[0] & 248);
  a[31] = static_cast<unsigned char>(a[31] & 127);
  a[31] = static_cast<unsigned char>(a[31] | 64);
}

void SetError(std::string* error_message, const char* text) {
  if (error_message != nullptr) {
    *error_message = text;
  }
}

}  // namespace

// ---------------- 公开 API ----------------

const char* Ed25519VerifyResultName(Ed25519VerifyResult result) {
  switch (result) {
    case Ed25519VerifyResult::kOk:
      return "ok";
    case Ed25519VerifyResult::kBadPublicKeySize:
      return "bad-public-key-size";
    case Ed25519VerifyResult::kBadSignatureSize:
      return "bad-signature-size";
    case Ed25519VerifyResult::kNonCanonicalPublicKey:
      return "non-canonical-public-key";
    case Ed25519VerifyResult::kInvalidPublicKey:
      return "invalid-public-key";
    case Ed25519VerifyResult::kSmallOrderPublicKey:
      return "small-order-public-key";
    case Ed25519VerifyResult::kNonCanonicalScalar:
      return "non-canonical-scalar";
    case Ed25519VerifyResult::kInvalidSignature:
      return "invalid-signature";
  }
  return "unknown";
}

// 前置条件：seed 恰好 32 字节、public_key 非空；后置条件：
// 成功时是 32 字节压缩点编码，失败时被清空（不留下上一次的内容）。
//
// 派生过程：h = SHA-512(seed)，前 32 字节夹紧得到标量 a，公钥
// A= a*B；h 的后 32 字节是签名用的 prefix，本函数用不到。
bool Ed25519PublicKeyFromSeed(const std::string& seed, std::string* public_key,
                              std::string* error_message) {
  if (public_key == nullptr) {
    SetError(error_message, "输出公钥指针为空");
    return false;
  }
  public_key->clear();
  if (seed.size() != kEd25519SeedSize) {
    SetError(error_message, "Ed25519 种子必须是 32 字节");
    return false;
  }
  unsigned char h[64];
  Sha512::Digest(seed.data(), seed.size(), h);
  unsigned char a[32];
  std::memcpy(a, h, 32);
  ClampScalar(a);
  Ge A;
  GeScalarMulBase(&A, a);
  unsigned char encoded[32];
  GeEncode(encoded, A);
  public_key->assign(reinterpret_cast<const char*>(encoded), sizeof(encoded));
  // 尽力而为地清掉中间材料。
  std::memset(h, 0, sizeof(h));
  std::memset(a, 0, sizeof(a));
  return true;
}

// 原子性：先取随机种子并算出公钥，两步都成功了才写 *seed；任何一步失败，
// 两个输出都是空串，不会留下一对不匹配的密钥。
//
// 随机源是 OS CSPRNG（见 crypto.h 的 RandomBytes），
// 它失败就直接返回 false，绝不回落到时间戳一类的弱随机源。
bool Ed25519GenerateKeyPair(std::string* seed, std::string* public_key,
                            std::string* error_message) {
  if (seed == nullptr || public_key == nullptr) {
    SetError(error_message, "输出指针为空");
    return false;
  }
  seed->clear();
  public_key->clear();
  std::string fresh;
  if (!RandomBytes(kEd25519SeedSize, &fresh, error_message)) {
    return false;
  }
  if (!Ed25519PublicKeyFromSeed(fresh, public_key, error_message)) {
    return false;
  }
  *seed = fresh;
  return true;
}

// 确定性签名（RFC 8032 §5.1.6）：nonce r =
// SHA-512(prefix || M)，完全由私钥与消息决定，不消耗随机数。
//
// 为什么这样设计：ECDSA 的历史事故几乎都来自随机 nonce 重复或可预测，
// 而重复即泄漏私钥；确定性 nonce 从根上去掉了这个失败模式。
//
// 流程：A = a*B；R = r*B；k = SHA-512(R || A || M)
// mod L；S = (r + k*a) mod L；输出 64 字节 = R || S。
//
// 签名在进入函数时先清空，失败不留半截结果；h、a、rh、kh 返回前尽力清零。
bool Ed25519Sign(const std::string& seed, const void* message, std::size_t size,
                 std::string* signature, std::string* error_message) {
  if (signature == nullptr) {
    SetError(error_message, "输出签名指针为空");
    return false;
  }
  signature->clear();
  if (seed.size() != kEd25519SeedSize) {
    SetError(error_message, "Ed25519 种子必须是 32 字节");
    return false;
  }
  if (size != 0 && message == nullptr) {
    SetError(error_message, "消息指针为空但长度非零");
    return false;
  }
  unsigned char h[64];
  Sha512::Digest(seed.data(), seed.size(), h);
  unsigned char a[32];
  std::memcpy(a, h, 32);
  ClampScalar(a);
  const unsigned char* prefix = h + 32;

  Ge A;
  GeScalarMulBase(&A, a);
  unsigned char A_encoded[32];
  GeEncode(A_encoded, A);

  // r = SHA-512(prefix || M) mod L
  unsigned char rh[64];
  {
    Sha512 ctx;
    ctx.Update(prefix, 32);
    ctx.Update(message, size);
    ctx.Final(rh);
  }
  std::uint64_t r[4];
  ScReduce512(rh, r);
  unsigned char r_bytes[32];
  ScToBytes(r_bytes, r);

  Ge R;
  GeScalarMulBase(&R, r_bytes);
  unsigned char R_encoded[32];
  GeEncode(R_encoded, R);

  // k = SHA-512(R || A || M) mod L
  unsigned char kh[64];
  {
    Sha512 ctx;
    ctx.Update(R_encoded, 32);
    ctx.Update(A_encoded, 32);
    ctx.Update(message, size);
    ctx.Final(kh);
  }
  std::uint64_t k[4];
  ScReduce512(kh, k);
  std::uint64_t a_scalar[4];
  ScReduce32(a, a_scalar);
  std::uint64_t s[4];
  ScMulAdd(k, a_scalar, r, s);
  unsigned char S_encoded[32];
  ScToBytes(S_encoded, s);

  signature->assign(reinterpret_cast<const char*>(R_encoded), 32);
  signature->append(reinterpret_cast<const char*>(S_encoded), 32);

  std::memset(h, 0, sizeof(h));
  std::memset(a, 0, sizeof(a));
  std::memset(rh, 0, sizeof(rh));
  std::memset(kh, 0, sizeof(kh));
  return true;
}

// 检查顺序是有意排列的，每一条都对应一个可被攻击者利用的编码自由度：
//
//   1. 长度：公钥 32、签名 64，不符直接失败；
//
//   2. 先查 S 的规范性（S < L），此时还没做任何群运算；
//
//   3. 解公钥 A：区分非规范编码 / 不在曲线上 / 小阶；
//
//   4. 解 R：任何失败都归入签名无效（R 的编码问题就是签名问题）；
//
//   5. 计算 k = SHA-512(R || A || M) mod L，比较 [S]B
//   与 R + [k]A 的编码。
//
// 用 GeEncode 之后的字节相等代替坐标比较：编码规范且唯一，
// 字节相等同时证明了两个点相等与表示合法。
//
// 本函数不写 error_message、不抛异常，失败原因全部由枚举承载。
Ed25519VerifyResult Ed25519VerifyDetailed(const std::string& public_key,
                                          const void* message, std::size_t size,
                                          const std::string& signature) {
  if (public_key.size() != kEd25519PublicKeySize) {
    return Ed25519VerifyResult::kBadPublicKeySize;
  }
  if (signature.size() != kEd25519SignatureSize) {
    return Ed25519VerifyResult::kBadSignatureSize;
  }
  if (size != 0 && message == nullptr) {
    return Ed25519VerifyResult::kInvalidSignature;
  }
  const unsigned char* sig =
      reinterpret_cast<const unsigned char*>(signature.data());
  const unsigned char* pk =
      reinterpret_cast<const unsigned char*>(public_key.data());

  // S 必须是规范标量（RFC 8032 §5.1.7 明确要求检查）。
  if (!ScIsCanonical(sig + 32)) {
    return Ed25519VerifyResult::kNonCanonicalScalar;
  }
  Ge A;
  const DecodeResult a_result = GeDecode(pk, &A);
  if (a_result == DecodeResult::kNonCanonical) {
    return Ed25519VerifyResult::kNonCanonicalPublicKey;
  }
  if (a_result == DecodeResult::kNotOnCurve) {
    return Ed25519VerifyResult::kInvalidPublicKey;
  }
  if (GeIsSmallOrder(A)) {
    return Ed25519VerifyResult::kSmallOrderPublicKey;
  }
  Ge R;
  if (GeDecode(sig, &R) != DecodeResult::kOk) {
    return Ed25519VerifyResult::kInvalidSignature;
  }

  unsigned char kh[64];
  {
    Sha512 ctx;
    ctx.Update(sig, 32);
    ctx.Update(pk, 32);
    ctx.Update(message, size);
    ctx.Final(kh);
  }
  std::uint64_t k[4];
  ScReduce512(kh, k);
  unsigned char k_bytes[32];
  ScToBytes(k_bytes, k);

  Ge sB;
  GeScalarMulBase(&sB, sig + 32);
  Ge kA;
  GeScalarMul(&kA, A, k_bytes);
  Ge sum;
  GeAdd(&sum, R, kA);

  unsigned char left[32];
  unsigned char right[32];
  GeEncode(left, sB);
  GeEncode(right, sum);
  return std::memcmp(left, right, 32) == 0
             ? Ed25519VerifyResult::kOk
             : Ed25519VerifyResult::kInvalidSignature;
}

bool Ed25519Verify(const std::string& public_key, const void* message,
                   std::size_t size, const std::string& signature,
                   std::string* error_message) {
  const Ed25519VerifyResult result =
      Ed25519VerifyDetailed(public_key, message, size, signature);
  if (result == Ed25519VerifyResult::kOk) {
    return true;
  }
  if (error_message != nullptr) {
    *error_message = Ed25519VerifyResultName(result);
  }
  return false;
}

// 指纹 = sha256(公钥) 的小写十六进制，用于日志、配置与界面里的人工比对；
// 长度不对时返回空串，调用方必须自己判空——空指纹不能被当成匹配。
std::string Ed25519Fingerprint(const std::string& public_key) {
  if (public_key.size() != kEd25519PublicKeySize) {
    return std::string();
  }
  return Sha256Hex(public_key);
}

// 接受的文本格式：ed25519: 前缀 + 64 位十六进制，或纯 64
// 位十六进制（前缀可选，解析前剥掉）。
//
// 失败语义与其它解析函数一致：out 先清空，任一步不合法就返回 false
// 并给出原因，绝不尽力解析出半个公钥。
bool Ed25519ParsePublicKeyText(const std::string& text, std::string* out,
                               std::string* error_message) {
  if (out == nullptr) {
    SetError(error_message, "输出指针为空");
    return false;
  }
  out->clear();
  std::string body = text;
  const std::string prefix = "ed25519:";
  if (body.compare(0, prefix.size(), prefix) == 0) {
    body = body.substr(prefix.size());
  }
  if (body.size() != kEd25519PublicKeySize * 2) {
    SetError(error_message, "Ed25519 公钥必须是 32 字节（64 位十六进制）");
    return false;
  }
  std::string parsed;
  if (!FromHex(body, &parsed) || parsed.size() != kEd25519PublicKeySize) {
    SetError(error_message, "Ed25519 公钥不是合法的十六进制");
    return false;
  }
  *out = parsed;
  return true;
}

std::string Ed25519FormatPublicKeyHex(const std::string& public_key) {
  if (public_key.size() != kEd25519PublicKeySize) {
    return std::string();
  }
  return "ed25519:" +
         ToHex(reinterpret_cast<const unsigned char*>(public_key.data()),
               public_key.size());
}

}  // namespace crypto
}  // namespace backupproject