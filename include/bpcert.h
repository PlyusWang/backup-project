// include/bpcert.h
//
// BPCERT1 —— 服务器身份证书（PR #23 Phase 1 的第三块）。
//
// 它回答的问题只有一个：**这把服务器公钥是谁批准的**。
// 批准者是一个离线保存的 Ed25519 根私钥；客户端只内置根本身的公钥。
//
// 为什么要有自己的证书格式，而不是套 X.509：
//   * 需求只有一个签名者、一种公钥算法、一种用途，X.509 的解析面和历史
//     坑（名字编码、扩展、可选字段、BER/DER 歧义）在这里全是纯风险；
//   * 本格式是"规范二进制"：字段顺序固定、大端定长、长度前缀有上限，
//     任何不合规（截断 / 超长 / 未知版本 / 未知算法 / 长度不合法 / 尾部
//     多余字节）一律**显式拒绝**，绝不"尽力而为"地解析出半个结果；
//   * 整个证书有 4096 字节硬上限，解析器不会因为输入长度而分配不可控内存。
//
// 规范布局（大端；body = magic 到 signature_algorithm 的全部字节）：
//
//   偏移  长度            字段
//   0     7               magic = "BPCERT1"
//   7     2               format_version = 1
//   9     2               server_id_len (1..128)
//   11    server_id_len   server_id（可打印 ASCII，展示与匹配都用它）
//   ..    1               public_key_algorithm = 1 (X25519)
//   ..    32              server_public_key
//   ..    8               serial_number (u64, 非 0)
//   ..    8               not_before (i64, Unix 秒)
//   ..    8               not_after  (i64, Unix 秒)
//   ..    2               issuer_id_len (1..128)
//   ..    issuer_id_len   issuer_id
//   ..    1               key_usage = 1 (SERVER_AUTH)
//   ..    1               signature_algorithm = 1 (Ed25519)
//   ..    64              signature = Ed25519(body)
//
// 签名只覆盖 body；body 之外的任何字节都不参与验签，所以"尾部多余字节"
// 必须由解析器拒绝，否则就会出现"同一张证书有两种字节表示"。
//
// 有效期策略：签发工具**默认**发 180 天（--days 可调，上限 10 年）；解析器
// 只做格式合法性（窗口必须为正且不超过 10 年），时钟判断交给
// Bpcert1CheckValidity，容差 ±5 分钟。

#ifndef BACKUP_PROJECT_INCLUDE_BPCERT_H_
#define BACKUP_PROJECT_INCLUDE_BPCERT_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace backupproject {
namespace crypto {

// 证书总长硬上限（含签名）。
inline constexpr std::size_t kBpcert1MaxCertificateSize = 4096;
// server_id / issuer_id 的长度上限。
inline constexpr std::size_t kBpcert1MaxIdentitySize = 128;
inline constexpr std::size_t kBpcert1PublicKeySize = 32;
inline constexpr std::size_t kBpcert1SignatureSize = 64;
// 官方云端的签发策略：180 天。
inline constexpr std::int64_t kBpcert1OfficialValiditySeconds =
    180LL * 24 * 60 * 60;
// 允许的时钟偏差：±5 分钟。
inline constexpr std::int64_t kBpcert1ClockSkewSeconds = 5 * 60;
// 解析器允许的最长有效期窗口（10 年）——防止"签发者写错一个 0"变成永久证书。
inline constexpr std::int64_t kBpcert1MaxValiditySeconds =
    10LL * 366 * 24 * 60 * 60;

// 结构化失败原因。0 是成功；调用方据此给用户不同的文案，而不是一句"失败"。
enum class Bpcert1Error {
  kOk = 0,
  kBadArgument,           // 空指针 / 编码侧字段不合法
  kTruncated,             // 声明要读的字段超出实际长度
  kOversized,             // 超过 4096 字节上限
  kBadMagic,              // 不是 "BPCERT1"
  kUnknownVersion,        // format_version 不认识
  kUnsupportedAlgorithm,  // 公钥算法 / 签名算法不认识
  kUnsupportedKeyUsage,   // key_usage 不是 SERVER_AUTH
  kMalformedLength,       // 长度前缀为 0 或超过 128
  kTrailingGarbage,       // 签名之后还有字节
  kBadFieldValue,  // 字段本身不合法（标识符含控制字符、时间窗为负…）
  kBadIssuerKeySize,  // 传入的签发者公钥不是 32 字节
  kUntrustedIssuer,  // issuer_id 不在可信根列表里 / 根已吊销或过期
  kSignatureInvalid,  // Ed25519 验签不通过
  kNotYetValid,       // 还没生效（很可能是本机时钟不对）
  kExpired,           // 已过期
};

const char* Bpcert1ErrorName(Bpcert1Error error);
const char* Bpcert1ErrorMessage(Bpcert1Error error);

// 一张证书的解码结果。签名单独放，便于调用方区分"结构合法"与"签名可信"。
struct Bpcert1 {
  std::string server_id;
  std::string server_public_key;  // 32 字节 X25519
  std::string issuer_id;
  std::uint64_t serial_number = 0;
  std::int64_t not_before = 0;
  std::int64_t not_after = 0;
  std::string signature;  // 64 字节 Ed25519
};

// 标识符规则：1..128 字节、可打印 ASCII（0x20..0x7E）、不含 NUL。
bool Bpcert1IsValidIdentity(const std::string& text);

// 编码 body（不含签名）。已设置签名的证书会被忽略签名字段。
bool Bpcert1EncodeUnsigned(const Bpcert1& certificate, std::string* out,
                           std::string* error_message);

// 编码完整证书（body || signature，签名字段必须已填好 64 字节）。
bool Bpcert1Encode(const Bpcert1& certificate, std::string* out,
                   std::string* error_message);

// 用签发者种子（32 字节 Ed25519 私钥材料）签名，产出完整证书。
// not_before == 0 时取当前时间；not_after == 0 时取 not_before + 180 天。
bool Bpcert1Issue(const Bpcert1& unsigned_certificate,
                  const std::string& issuer_seed, std::string* out,
                  std::string* error_message);

// 严格解析。任何不合规都返回对应的 Bpcert1Error，不会写出半个结果：
// 只有返回 kOk 时 *out 才被赋值。
Bpcert1Error Bpcert1Parse(const std::string& raw, Bpcert1* out);

// 取 body 部分（末尾 64 字节之前的所有字节）。长度不足时返回空串。
std::string Bpcert1Body(const std::string& raw);

// 校验结构 + 用签发者公钥验签。返回 kOk 表示"结构合法且签名有效"。
// 注意：它**不**判断签发者是否可信——那是 TrustedRootStore 的职责。
Bpcert1Error Bpcert1VerifySignature(const std::string& raw,
                                    const std::string& issuer_public_key);

// 时间窗校验。失败时 *error_result 给结构化原因，*message 给用户文案
// （消息里会区分"证书过期"和"看起来是本机时钟不对"两种情形）。
bool Bpcert1CheckValidity(const Bpcert1& certificate,
                          std::int64_t now_unix_seconds,
                          Bpcert1Error* error_result, std::string* message);

// 当前 Unix 秒（system_clock）。
std::int64_t Bpcert1NowUnixSeconds();

// 整张证书的 sha256 十六进制（展示 / 日志用，不含任何私钥材料）。
std::string Bpcert1Fingerprint(const std::string& raw_certificate);

// 一行摘要，给日志和管理界面用。
std::string Bpcert1Describe(const Bpcert1& certificate);

}  // namespace crypto
}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_BPCERT_H_
