// include/ed25519.h
//
// 手写 Ed25519（RFC 8032）：数字签名，用来给服务器身份证书签名 / 验签。
//
// 与 X25519 的分工（不能混用）：
//   X25519  = 密钥协商（BPSEC1/BPSEC2 的会话密钥）
//   Ed25519 = 数字签名（谁批准了这把服务器身份公钥）
//
// 本模块只依赖 C++17 标准库、POSIX 随机数与同目录手写的 SHA-512，
// 不调用 OpenSSL / libsodium / mbedTLS 等任何第三方密码库。
//
// 边界与失败约定（与 crypto.h 同一条规矩）：
//   * 任何长度不对、编码不规范、不在曲线上的输入一律**显式失败**，
//     绝不"尽力而为"地解析出半个结果；
//   * 验签失败会给出结构化原因（见 Ed25519VerifyResult），调用方据此区分
//     "签名不对" / "公钥不合法" / "S 不是规范标量" 等，而不是一句"失败"；
//   * 本模块不做 constant-time 的强承诺（与项目其它手写原语一致的诚实边界），
//     但标量乘与比较都写成固定轮数 + 掩码选择，没有按比特提前退出。

#ifndef BACKUP_PROJECT_INCLUDE_ED25519_H_
#define BACKUP_PROJECT_INCLUDE_ED25519_H_

#include <cstddef>
#include <string>

namespace backupproject {
namespace crypto {

// RFC 8032 §5.1：种子 32 字节、公钥 32 字节、签名 64 字节。
inline constexpr std::size_t kEd25519SeedSize = 32;
inline constexpr std::size_t kEd25519PublicKeySize = 32;
inline constexpr std::size_t kEd25519SignatureSize = 64;

// 验签的结构化结果。数字 0 是成功，其余都是失败原因，调用方可以直接
// 映射成用户文案（"证书签名无效" / "公钥编码不合法" …）。
enum class Ed25519VerifyResult {
  kOk = 0,
  kBadPublicKeySize,
  kBadSignatureSize,
  kNonCanonicalPublicKey,  // y >= p，或最高位之外的编码不合法
  kInvalidPublicKey,       // 不在 edwards25519 上
  kSmallOrderPublicKey,    // 落在小阶子群（torsion）上，拒绝
  kNonCanonicalScalar,     // S >= L
  kInvalidSignature,       // 验签方程不成立
};

const char* Ed25519VerifyResultName(Ed25519VerifyResult result);

// 由 32 字节种子导出公钥。seed 长度不对 / 输出指针为空时返回 false。
bool Ed25519PublicKeyFromSeed(const std::string& seed, std::string* public_key,
                              std::string* error_message);

// 生成一个新的密钥对（种子来自 OS CSPRNG）。seed 就是私钥材料本身，
// 调用方负责按 0600 保存；本函数不落盘、不打印。
bool Ed25519GenerateKeyPair(std::string* seed, std::string* public_key,
                            std::string* error_message);

// 签名。message 可以为空（size == 0 合法）。成功时 *signature 是 64 字节。
bool Ed25519Sign(const std::string& seed, const void* message,
                 std::size_t size, std::string* signature,
                 std::string* error_message);

// 带原因的验签。message 可以为空。
Ed25519VerifyResult Ed25519VerifyDetailed(const std::string& public_key,
                                          const void* message, std::size_t size,
                                          const std::string& signature);

// 只要布尔结果的包装。失败原因进 error_message（可为 nullptr）。
bool Ed25519Verify(const std::string& public_key, const void* message,
                   std::size_t size, const std::string& signature,
                   std::string* error_message);

// 公钥的展示用指纹：sha256(public_key) 的小写十六进制前 64 位（32 字节全量，
// 与 X25519Fingerprint 的用法一致，便于日志与界面显示）。
std::string Ed25519Fingerprint(const std::string& public_key);

// 公钥文本互转：接受 "ed25519:<64 位十六进制>" 或纯十六进制；输出统一带前缀。
bool Ed25519ParsePublicKeyText(const std::string& text, std::string* out,
                               std::string* error_message);
std::string Ed25519FormatPublicKeyHex(const std::string& public_key);

}  // namespace crypto
}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_ED25519_H_
