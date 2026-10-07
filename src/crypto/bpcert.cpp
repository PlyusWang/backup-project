// src/crypto/bpcert.cpp
//
// BPCERT1 的实现：规范编码 + 严格解析（布局与理由见 include/bpcert.h）。
//
// 解析器的原则：**先验证，再前进**。每一步都先确认"要读的字节真的存在"
// 再读，读完立刻推进偏移；任何不合规立刻返回具名错误，不写出半个结果。
// 这样"截断 / 超长 / 未知版本 / 未知算法 / 长度不合法 / 尾部多余字节"
// 六类畸形输入都有各自的原因，而不是笼统的 false。

// 本文件是 BPCERT1 的**唯一**编解码实现：格式的权威定义写在 include/bpcert.h，
// 但字节布局只在这里被拼出来、也只在这里被拆开，别处不许再实现一遍。
//
// 职责边界：
//   * 不判断签发者是否可信（issuer_id 在这里只是个字符串）——那是
//     TrustedRootStore 的事；
//   * 不决定"这张证书该不该被接受"（用途、吊销、时间窗的组合策略在调用方）；
//   * 不读文件、不联网、无全局状态，输入输出全是 std::string。
//
// 两端合同：Encode 产出的字节必须能被 Parse 接受，Parse 接受的字节也必须能
// 被重新编码成同一串——"签名之后不允许有多余字节"正是这条唯一性的保证。
#include "bpcert.h"

#include <chrono>
#include <cstring>

#include "crypto.h"
#include "ed25519.h"

namespace backupproject {
namespace crypto {
namespace {

// 下面这几个常量是**格式的一部分**，改值等于换协议：magic 是第一道判据
// （"这到底是不是一张 BPCERT1"），version 是留给未来的拒绝开关（不认识的
// 版本一律拒绝，绝不猜），算法与用途各占一个字节，全部进 body 并参与验签。
constexpr char kMagic[7] = {'B', 'P', 'C', 'E', 'R', 'T', '1'};
constexpr std::uint16_t kFormatVersion = 1;
constexpr std::uint8_t kPublicKeyAlgorithmX25519 = 1;
constexpr std::uint8_t kKeyUsageServerAuth = 1;
constexpr std::uint8_t kSignatureAlgorithmEd25519 = 1;

// body 的最小长度：server_id 与 issuer_id 都取最短的 1 字节。
// 它只用于最前面那一次"不可能通过就快速拒绝"，真实判断仍然逐字段做；
// 数值写错了最多让某类畸形输入换一个错误名，不会让解析器接受非法数据。
constexpr std::size_t kMinBodySize = sizeof(kMagic) + 2 + 2 + 1 + 1 +
                                     kBpcert1PublicKeySize + 8 + 8 + 8 + 2 + 1 +
                                     1 + 1;

// error_message 是可选出参：不关心细节的调用方传 nullptr，函数照样给出唯一
// 有意义的失败信号（false 或具名的 Bpcert1Error）。诊断文本永不决定控制流。
void SetError(std::string* error_message, const char* text) {
  if (error_message != nullptr) {
    *error_message = text;
  }
}

// 一律大端（"网络字节序"）：格式只有一种表示，不依赖主机字节序。
// 大端是**唯一的**线上表示：编码器不写"主机序 + 标记"，解析器也不探测字节
// 序。格式里没有任何"看情况"的分支，不同字节序的机器读到的必然是同一张证
// 书。下面这四个 Append / Read 就是本模块全部的字节序逻辑。
void AppendU16(std::string* out, std::uint16_t value) {
  out->push_back(static_cast<char>((value >> 8) & 0xFF));
  out->push_back(static_cast<char>(value & 0xFF));
}

void AppendU64(std::string* out, std::uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out->push_back(static_cast<char>((value >> shift) & 0xFF));
  }
}

// 读侧不做边界检查，是**故意**的：唯一的调用模式是先用 Fits 确认要读的字节
// 存在，再调用这里。把边界检查塞进每个读函数只会让"谁负责验证"变得模糊。
std::uint16_t ReadU16(const unsigned char* p) {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) |
                                    static_cast<std::uint16_t>(p[1]));
}

std::uint64_t ReadU64(const unsigned char* p) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value = (value << 8) | static_cast<std::uint64_t>(p[i]);
  }
  return value;
}

// 一个字段的"读之前先确认存在"。
// "先验证，再前进"的落地：off 只增不减，任何越界都在读之前被拦住，因此后面
// 的 p[off] / ReadU16 / ReadU64 不可能读到 raw 之外。size 有 4096 的硬上限，
// off + need 不会溢出。
bool Fits(std::size_t offset, std::size_t need, std::size_t size) {
  return offset + need <= size;
}

}  // namespace

// 枚举到稳定的机器可读 token。这些字符串会进日志、CLI 输出与自动化断言，
// 属于对外合同：改名是破坏性变更；新增枚举值必须在这里补一条（switch 不写
// default，漏了会被 -Wswitch 抓住，而不是悄悄退化成 "unknown"）。
const char* Bpcert1ErrorName(Bpcert1Error error) {
  switch (error) {
    case Bpcert1Error::kOk:
      return "ok";
    case Bpcert1Error::kBadArgument:
      return "bad-argument";
    case Bpcert1Error::kTruncated:
      return "truncated";
    case Bpcert1Error::kOversized:
      return "oversized";
    case Bpcert1Error::kBadMagic:
      return "bad-magic";
    case Bpcert1Error::kUnknownVersion:
      return "unknown-version";
    case Bpcert1Error::kUnsupportedAlgorithm:
      return "unsupported-algorithm";
    case Bpcert1Error::kUnsupportedKeyUsage:
      return "unsupported-key-usage";
    case Bpcert1Error::kMalformedLength:
      return "malformed-length";
    case Bpcert1Error::kTrailingGarbage:
      return "trailing-garbage";
    case Bpcert1Error::kBadFieldValue:
      return "bad-field-value";
    case Bpcert1Error::kBadIssuerKeySize:
      return "bad-issuer-key-size";
    case Bpcert1Error::kUntrustedIssuer:
      return "untrusted-issuer";
    case Bpcert1Error::kSignatureInvalid:
      return "signature-invalid";
    case Bpcert1Error::kNotYetValid:
      return "not-yet-valid";
    case Bpcert1Error::kExpired:
      return "expired";
  }
  return "unknown";
}

// 面向用户的中文文案，与 Name 分开是为了让"给机器看的标识"和"给人看的解
// 释"各自演化：改文案不影响任何脚本，改 token 会。
const char* Bpcert1ErrorMessage(Bpcert1Error error) {
  switch (error) {
    case Bpcert1Error::kOk:
      return "证书有效";
    case Bpcert1Error::kBadArgument:
      return "证书字段不合法（长度、时间窗或序列号）";
    case Bpcert1Error::kTruncated:
      return "证书数据不完整（被截断）";
    case Bpcert1Error::kOversized:
      return "证书数据超出 4096 字节上限";
    case Bpcert1Error::kBadMagic:
      return "不是 BPCERT1 证书（魔数不匹配）";
    case Bpcert1Error::kUnknownVersion:
      return "证书版本不受支持，请升级客户端";
    case Bpcert1Error::kUnsupportedAlgorithm:
      return "证书使用了不受支持的算法";
    case Bpcert1Error::kUnsupportedKeyUsage:
      return "证书用途不是服务器身份认证";
    case Bpcert1Error::kMalformedLength:
      return "证书字段长度不合法";
    case Bpcert1Error::kTrailingGarbage:
      return "证书签名之后还有多余字节";
    case Bpcert1Error::kBadFieldValue:
      return "证书字段取值不合法（标识符或时间窗）";
    case Bpcert1Error::kBadIssuerKeySize:
      return "签发者公钥长度不是 32 字节";
    case Bpcert1Error::kUntrustedIssuer:
      return "签发这张证书的根不在本机可信根列表里";
    case Bpcert1Error::kSignatureInvalid:
      return "证书签名无效：这张证书不是该根签发的";
    case Bpcert1Error::kNotYetValid:
      return "证书尚未生效";
    case Bpcert1Error::kExpired:
      return "证书已过期";
  }
  return "证书校验失败";
}

// 标识符 grammar：1..128 字节的可打印 ASCII（0x20..0x7E）。刻意不支持 UTF-8、
// 换行与 NUL：标识符会出现在日志、命令行、配置文件和界面里，允许不可打印
// 字节等于允许日志注入与显示歧义；"只比字节"也让签发侧与验证侧不可能因为
// 归一化或大小写折叠产生分歧。
bool Bpcert1IsValidIdentity(const std::string& text) {
  if (text.empty() || text.size() > kBpcert1MaxIdentitySize) {
    return false;
  }
  for (const char raw : text) {
    const unsigned char c = static_cast<unsigned char>(raw);
    if (c < 0x20 || c > 0x7E) {
      return false;  // 控制字符（含 NUL）与高位字节一律拒绝
    }
  }
  return true;
}

// 编码 body（不含签名）。这里每一项校验都必须在签发侧挡住：解析器会对同一
// 组字段再判一次，编码器把不合格的输入编出来，只会得到一张谁都读不了的证
// 书。*out 先被 clear()，失败时保持"没有半份结果"。
bool Bpcert1EncodeUnsigned(const Bpcert1& certificate, std::string* out,
                           std::string* error_message) {
  if (out == nullptr) {
    SetError(error_message, "输出指针为空");
    return false;
  }
  out->clear();
  if (!Bpcert1IsValidIdentity(certificate.server_id)) {
    SetError(error_message, "server_id 必须是 1..128 字节的可打印 ASCII");
    return false;
  }
  if (!Bpcert1IsValidIdentity(certificate.issuer_id)) {
    SetError(error_message, "issuer_id 必须是 1..128 字节的可打印 ASCII");
    return false;
  }
  if (certificate.server_public_key.size() != kBpcert1PublicKeySize) {
    SetError(error_message, "服务器公钥必须是 32 字节");
    return false;
  }
  if (certificate.serial_number == 0) {
    SetError(error_message, "序列号不能为 0");
    return false;
  }
  if (certificate.not_before <= 0 || certificate.not_after <= 0) {
    SetError(error_message, "生效/失效时间必须是正的 Unix 秒");
    return false;
  }
  if (certificate.not_after <= certificate.not_before) {
    SetError(error_message, "失效时间必须晚于生效时间");
    return false;
  }
  if (certificate.not_after - certificate.not_before >
      kBpcert1MaxValiditySeconds) {
    SetError(error_message, "有效期窗口超过 10 年上限");
    return false;
  }

  std::string body;
  body.reserve(kMinBodySize + certificate.server_id.size() +
               certificate.issuer_id.size());
  // 从这里开始的 append 顺序**就是**磁盘布局：magic / version / server_id /
  // 公钥算法 / 公钥 / serial / not_before / not_after / issuer_id / key_usage /
  // 签名算法，最后再由 Encode 或 Issue 追加签名。顺序不能重排——旧证书按固定
  // offset 解析；新增字段只能追加到末尾，并把 kFormatVersion 提升。
  body.append(kMagic, sizeof(kMagic));
  AppendU16(&body, kFormatVersion);
  AppendU16(&body, static_cast<std::uint16_t>(certificate.server_id.size()));
  body.append(certificate.server_id);
  body.push_back(static_cast<char>(kPublicKeyAlgorithmX25519));
  body.append(certificate.server_public_key);
  AppendU64(&body, certificate.serial_number);
  AppendU64(&body, static_cast<std::uint64_t>(certificate.not_before));
  AppendU64(&body, static_cast<std::uint64_t>(certificate.not_after));
  AppendU16(&body, static_cast<std::uint16_t>(certificate.issuer_id.size()));
  body.append(certificate.issuer_id);
  body.push_back(static_cast<char>(kKeyUsageServerAuth));
  body.push_back(static_cast<char>(kSignatureAlgorithmEd25519));

  // 长度上限在编码侧也要挡：否则我们能造出一张自己都解析不了的证书
  // （Bpcert1Parse 会直接返回 kOversized）。上限同时是解析侧的内存下界保证。
  if (body.size() + kBpcert1SignatureSize > kBpcert1MaxCertificateSize) {
    SetError(error_message, "证书超过 4096 字节上限");
    return false;
  }
  *out = body;
  return true;
}

// 完整证书 = body || signature。签名必须由调用方（或 Bpcert1Issue）事先算
// 好，这里不做任何隐式签名；长度不对的签名一律拒绝——长度错误意味着"签的
// 不是这张证书"，静默补零或截断比拒绝危险得多。
bool Bpcert1Encode(const Bpcert1& certificate, std::string* out,
                   std::string* error_message) {
  if (out == nullptr) {
    SetError(error_message, "输出指针为空");
    return false;
  }
  out->clear();
  if (certificate.signature.size() != kBpcert1SignatureSize) {
    SetError(error_message, "签名必须是 64 字节");
    return false;
  }
  std::string body;
  if (!Bpcert1EncodeUnsigned(certificate, &body, error_message)) {
    return false;
  }
  body.append(certificate.signature);
  *out = body;
  return true;
}

// 签发：not_before == 0 取当前时间，not_after == 0 取 not_before + 180 天
// （kBpcert1OfficialValiditySeconds），其余字段原样使用，然后对 body 签名。
//
// 它只返回字节：不落盘、不打印、不碰 key 文件。issuer_seed 归调用方所有，
// 本函数不清零它（清零是持有者的责任，见 tools/cert_tool_main.cpp）。
bool Bpcert1Issue(const Bpcert1& unsigned_certificate,
                  const std::string& issuer_seed, std::string* out,
                  std::string* error_message) {
  if (out == nullptr) {
    SetError(error_message, "输出指针为空");
    return false;
  }
  out->clear();
  Bpcert1 filled = unsigned_certificate;
  if (filled.not_before == 0) {
    filled.not_before = Bpcert1NowUnixSeconds();
  }
  if (filled.not_after == 0) {
    filled.not_after = filled.not_before + kBpcert1OfficialValiditySeconds;
  }
  std::string body;
  if (!Bpcert1EncodeUnsigned(filled, &body, error_message)) {
    return false;
  }
  std::string signature;
  if (!Ed25519Sign(issuer_seed, body.data(), body.size(), &signature,
                   error_message)) {
    return false;
  }
  if (signature.size() != kBpcert1SignatureSize) {
    SetError(error_message, "签名长度异常");
    return false;
  }
  body.append(signature);
  *out = body;
  return true;
}

// 严格解析，也是本模块**唯一**接触不可信字节的入口：raw 可能来自网络握手，
// 也可能是磁盘上被人替换过的文件。
//
// 分层拒绝的顺序是刻意的：先只比较总长（>4096 → kOversized，< 最小合法长度
// → kTruncated），这两步不索引、不分配，任何输入都不能让解析器按输入声明的
// 长度去要内存；之后才逐字段走 "Fits -> 读 -> 推进偏移"。
//
// 全有或全无：*out 只在最后几个赋值语句里被写，中途任何 return 都不会留下
// 半个 Bpcert1。返回 kOk 之前 *out 的内容是未定义的，调用方必须看返回值。
Bpcert1Error Bpcert1Parse(const std::string& raw, Bpcert1* out) {
  if (out == nullptr) {
    return Bpcert1Error::kBadArgument;
  }
  const std::size_t size = raw.size();
  if (size > kBpcert1MaxCertificateSize) {
    return Bpcert1Error::kOversized;
  }
  if (size < kMinBodySize + kBpcert1SignatureSize) {
    return Bpcert1Error::kTruncated;
  }
  const unsigned char* p = reinterpret_cast<const unsigned char*>(raw.data());
  std::size_t off = 0;

  if (std::memcmp(p, kMagic, sizeof(kMagic)) != 0) {
    return Bpcert1Error::kBadMagic;
  }
  off += sizeof(kMagic);

  if (!Fits(off, 2, size)) return Bpcert1Error::kTruncated;
  if (ReadU16(p + off) != kFormatVersion) {
    return Bpcert1Error::kUnknownVersion;
  }
  off += 2;

  if (!Fits(off, 2, size)) return Bpcert1Error::kTruncated;
  const std::size_t server_id_size = ReadU16(p + off);
  off += 2;
  if (server_id_size == 0 || server_id_size > kBpcert1MaxIdentitySize) {
    return Bpcert1Error::kMalformedLength;
  }
  if (!Fits(off, server_id_size, size)) return Bpcert1Error::kTruncated;
  const std::string server_id(reinterpret_cast<const char*>(p + off),
                              server_id_size);
  off += server_id_size;
  if (!Bpcert1IsValidIdentity(server_id)) {
    return Bpcert1Error::kBadFieldValue;
  }

  if (!Fits(off, 1, size)) return Bpcert1Error::kTruncated;
  if (p[off] != kPublicKeyAlgorithmX25519) {
    return Bpcert1Error::kUnsupportedAlgorithm;
  }
  off += 1;

  if (!Fits(off, kBpcert1PublicKeySize, size)) return Bpcert1Error::kTruncated;
  const std::string server_public_key(reinterpret_cast<const char*>(p + off),
                                      kBpcert1PublicKeySize);
  off += kBpcert1PublicKeySize;

  if (!Fits(off, 8, size)) return Bpcert1Error::kTruncated;
  const std::uint64_t serial_number = ReadU64(p + off);
  off += 8;

  if (!Fits(off, 8, size)) return Bpcert1Error::kTruncated;
  const std::uint64_t not_before_raw = ReadU64(p + off);
  off += 8;

  if (!Fits(off, 8, size)) return Bpcert1Error::kTruncated;
  const std::uint64_t not_after_raw = ReadU64(p + off);
  off += 8;

  if (!Fits(off, 2, size)) return Bpcert1Error::kTruncated;
  const std::size_t issuer_id_size = ReadU16(p + off);
  off += 2;
  if (issuer_id_size == 0 || issuer_id_size > kBpcert1MaxIdentitySize) {
    return Bpcert1Error::kMalformedLength;
  }
  if (!Fits(off, issuer_id_size, size)) return Bpcert1Error::kTruncated;
  const std::string issuer_id(reinterpret_cast<const char*>(p + off),
                              issuer_id_size);
  off += issuer_id_size;
  if (!Bpcert1IsValidIdentity(issuer_id)) {
    return Bpcert1Error::kBadFieldValue;
  }

  if (!Fits(off, 1, size)) return Bpcert1Error::kTruncated;
  if (p[off] != kKeyUsageServerAuth) {
    return Bpcert1Error::kUnsupportedKeyUsage;
  }
  off += 1;

  if (!Fits(off, 1, size)) return Bpcert1Error::kTruncated;
  if (p[off] != kSignatureAlgorithmEd25519) {
    return Bpcert1Error::kUnsupportedAlgorithm;
  }
  off += 1;

  if (!Fits(off, kBpcert1SignatureSize, size)) {
    return Bpcert1Error::kTruncated;
  }
  const std::string signature(reinterpret_cast<const char*>(p + off),
                              kBpcert1SignatureSize);
  off += kBpcert1SignatureSize;

  // 尾部不允许有任何字节：签名不覆盖 body 之外的字节，留着就会出现
  // "同一张证书、两种字节表示"，也会给指纹/缓存带来歧义。
  if (off != size) {
    return Bpcert1Error::kTrailingGarbage;
  }

  // 上面只保证"结构合法"，这里再判字段语义。
  constexpr std::uint64_t kMaxInt64 =
      static_cast<std::uint64_t>(0x7FFFFFFFFFFFFFFFULL);
  if (serial_number == 0) {
    return Bpcert1Error::kBadFieldValue;
  }
  if (not_before_raw == 0 || not_after_raw == 0 || not_before_raw > kMaxInt64 ||
      not_after_raw > kMaxInt64) {
    return Bpcert1Error::kBadFieldValue;
  }
  const std::int64_t not_before = static_cast<std::int64_t>(not_before_raw);
  const std::int64_t not_after = static_cast<std::int64_t>(not_after_raw);
  if (not_after <= not_before) {
    return Bpcert1Error::kBadFieldValue;
  }
  if (not_after - not_before > kBpcert1MaxValiditySeconds) {
    return Bpcert1Error::kBadFieldValue;
  }

  // 唯一的写出点：走到这里，所有字段都已通过结构检查与语义检查，赋值不会再
  // 失败。若将来要在赋值之后再判什么，必须先把结果写进局部 Bpcert1 再整体拷
  // 过去，否则"返回错误但 *out 已被改"会破坏上面那条全有或全无的承诺。
  out->server_id = server_id;
  out->server_public_key = server_public_key;
  out->issuer_id = issuer_id;
  out->serial_number = serial_number;
  out->not_before = not_before;
  out->not_after = not_after;
  out->signature = signature;
  return Bpcert1Error::kOk;
}

// body = 去掉尾部 64 字节签名。长度不足时返回空串（调用方据此走失败路径），
// 不抛异常、不返回半截：本函数在验签路径上，不能因为输入畸形就崩。
std::string Bpcert1Body(const std::string& raw) {
  if (raw.size() <= kBpcert1SignatureSize) {
    return std::string();
  }
  // 签名是布局里的最后一个字段，所以 body 就是"去掉尾部 64 字节"。
  return raw.substr(0, raw.size() - kBpcert1SignatureSize);
}

// 结构 + 签名，两个都通过才返回 kOk。内部**重新解析一次** raw，而不是接受
// 调用方已经解析好的结构：签名覆盖的是字节本身，让调用方传结构等于允许
// "验一份字节、用另一份字段"。
//
// 它不判断 issuer_id 是否可信、也不看时间窗——本函数只回答"这串字节是不是
// 这把公钥签的"。发行者身份与有效期分别由 TrustedRootStore 与
// Bpcert1CheckValidity 负责，分开是为了让"签名无效"与"根不受信"在界面上
// 是两句不同的话。
Bpcert1Error Bpcert1VerifySignature(const std::string& raw,
                                    const std::string& issuer_public_key) {
  if (issuer_public_key.size() != kEd25519PublicKeySize) {
    return Bpcert1Error::kBadIssuerKeySize;
  }
  Bpcert1 parsed;
  const Bpcert1Error parsed_result = Bpcert1Parse(raw, &parsed);
  if (parsed_result != Bpcert1Error::kOk) {
    return parsed_result;
  }
  const std::string body = Bpcert1Body(raw);
  if (!Ed25519Verify(issuer_public_key, body.data(), body.size(),
                     parsed.signature, nullptr)) {
    return Bpcert1Error::kSignatureInvalid;
  }
  return Bpcert1Error::kOk;
}

// 时间窗判断，容差 ±kBpcert1ClockSkewSeconds（5 分钟）且两边对称：证书刚签发
// 而客户端时钟略慢时，不该被判成"尚未生效"。
//
// 不因为时钟可疑就放行，也不因为时钟可疑就拒绝：结论与解释分别放进
// error_result / message，由调用方决定提示"证书过期"还是"检查本机时钟"，
// 这两种情形的处置完全不同。
bool Bpcert1CheckValidity(const Bpcert1& certificate,
                          std::int64_t now_unix_seconds,
                          Bpcert1Error* error_result, std::string* message) {
  // now ± 300 在 INT64 边界上会溢出，而有符号溢出是 UB。用 128 位中间量
  // 做比较（与 ed25519.cpp 里同一个 __extension__ 写法）。
  __extension__ typedef __int128 Wide;
  const Wide now_wide = static_cast<Wide>(now_unix_seconds);
  const Wide skew_wide = static_cast<Wide>(kBpcert1ClockSkewSeconds);
  const Wide before_wide = static_cast<Wide>(certificate.not_before);
  const Wide after_wide = static_cast<Wide>(certificate.not_after);
  if (now_wide + skew_wide < before_wide) {
    if (error_result != nullptr) *error_result = Bpcert1Error::kNotYetValid;
    if (message != nullptr) {
      const bool far_future =
          certificate.not_before - now_unix_seconds > 24 * 60 * 60;
      *message = far_future ? "证书生效时间在未来很久，本机系统时钟很可能不正确"
                            : "证书尚未生效，请检查本机系统时钟";
    }
    return false;
  }
  if (now_wide > after_wide + skew_wide) {
    if (error_result != nullptr) *error_result = Bpcert1Error::kExpired;
    if (message != nullptr) {
      const bool far_past =
          now_unix_seconds - certificate.not_after > 24 * 60 * 60;
      *message = far_past ? "证书已过期很久；若确定它刚签发，请检查本机系统时钟"
                          : "证书已过期，需要重新签发";
    }
    return false;
  }
  if (error_result != nullptr) *error_result = Bpcert1Error::kOk;
  if (message != nullptr) *message = "证书在有效期内";
  return true;
}

// system_clock 是墙上时钟：会被 NTP 或用户改动，也可能回跳。它只用于"现在
// 几点"这类绝对时间判断（证书窗口、界面显示），绝不用于测量时长或退避间隔。
// 单独包一个函数，是为了让测试与命令行的 --now 能替换同一条路径上的时间。
std::int64_t Bpcert1NowUnixSeconds() {
  return static_cast<std::int64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

// 整张证书的 SHA-256 十六进制，给人核对用（"服务器上那张是不是这一张"）。
// 空输入返回空串而不是"空串的摘要"：调用方据此区分"没有证书"与"有一张摘
// 要为 X 的证书"，不让一个不存在的对象得到看起来合法的指纹。
std::string Bpcert1Fingerprint(const std::string& raw_certificate) {
  if (raw_certificate.empty()) {
    return std::string();
  }
  return Sha256Hex(raw_certificate);
}

// 一行摘要，给日志与管理界面用。公钥只打印 sha256 的前 16 个十六进制字符：
// 完整公钥在日志里没人看又占地方，摘要足够回答"是不是换了密钥"。本函数只
// 输出公开材料，任何私钥 / seed 都不经过它。
std::string Bpcert1Describe(const Bpcert1& certificate) {
  std::string text = "BPCERT1 server_id=" + certificate.server_id;
  text += " serial=" + std::to_string(certificate.serial_number);
  text += " not_before=" + std::to_string(certificate.not_before);
  text += " not_after=" + std::to_string(certificate.not_after);
  text += " issuer_id=" + certificate.issuer_id;
  if (certificate.server_public_key.size() == kBpcert1PublicKeySize) {
    text +=
        " key_sha256=" + Sha256Hex(certificate.server_public_key).substr(0, 16);
  }
  return text;
}

}  // namespace crypto
}  // namespace backupproject
