// include/hkdf.h
//
// 手写 HKDF-SHA256（RFC 5869）。
//
// 传输加密的密钥派生用它：X25519 得到的两份共享秘密进 HKDF-Extract，
// 再用 HKDF-Expand 按**标签**导出方向分离的加密/MAC 密钥与握手完成密钥。
//
// 只依赖本项目自己的 HMAC-SHA256（include/crypto.h）。算法按 RFC 5869 §2
// 的定义实现，测试向量取自 RFC 5869 附录 A 的 SHA-256 用例（Test Case 1/2/3）。
//
// 失败约定：长度非法（L > 255 * HashLen）或输出指针为空时返回 false 并写
// error_message，绝不静默截断。

#ifndef BACKUP_PROJECT_INCLUDE_HKDF_H_
#define BACKUP_PROJECT_INCLUDE_HKDF_H_

#include <cstddef>
#include <string>

namespace backupproject {
namespace crypto {

// RFC 5869 允许的最大输出长度：255 * HashLen。
inline constexpr std::size_t kHkdfMaxOutputBytes = 255 * 32;

// HKDF-Extract：PRK = HMAC-SHA256(salt, IKM)。
// salt 为空串时按 RFC 5869 §2.2 用 32 个 0 字节代替（与该节定义的
// "HashLen zeros" 等价）。输出恒为 32 字节。
bool HkdfExtract(const std::string& salt, const std::string& ikm,
                 std::string* prk, std::string* error_message);

// HKDF-Expand：T(1) = HMAC(PRK, info || 0x01)，T(i) = HMAC(PRK, T(i-1) || info
// || i)。 length == 0 时输出空串（合法）；length > 255 * 32 时失败。
bool HkdfExpand(const std::string& prk, const std::string& info,
                std::size_t length, std::string* okm,
                std::string* error_message);

// Extract + Expand 的组合。prk 只活在栈上，不返回给调用方。
bool Hkdf(const std::string& salt, const std::string& ikm,
          const std::string& info, std::size_t length, std::string* okm,
          std::string* error_message);

}  // namespace crypto
}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_HKDF_H_
