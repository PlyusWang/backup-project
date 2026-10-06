// include/remote_auth.h
//
// 远程备份的用户认证原语。
//
// 两件事，都不依赖 SQLite，也不依赖 socket：
//
//   1. 口令存储。PBKDF2-HMAC-SHA256 + 每用户随机 salt + 迭代下限。
//      绝不存明文、绝不存 SHA256(password)、绝不用固定 salt。
//      校验用定长比较，避免"前缀对不对"这种时序侧信道。
//   2. 无状态签名 token。HMAC-SHA256 覆盖 (user_id, issued_at, expires_at,
//      nonce)，固定 12 小时有效期。服务端不保存会话表，因此重启不掉线；
//      代价是签发出去的 token 在过期前无法单独吊销——这条写进
//      KNOWN-LIMITATIONS，不假装它有。
//
// token 只以字符串形式在客户端内存里存在；本模块不打印、不落盘、不写日志。

#ifndef BACKUP_PROJECT_INCLUDE_REMOTE_AUTH_H_
#define BACKUP_PROJECT_INCLUDE_REMOTE_AUTH_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace backupproject {
namespace net {

// PBKDF2 迭代次数下限（RFC 8018 建议量级；夜班机器上实测约几百毫秒）。
inline constexpr std::uint32_t kPasswordIterations = 200000;
inline constexpr std::size_t kPasswordSaltBytes = 16;
inline constexpr std::size_t kPasswordHashBytes = 32;
// 注册时要求的最短口令。协议层的 kMaxPasswordBytes 是上限。
inline constexpr std::size_t kMinPasswordBytes = 8;
// token 有效期：12 小时。
inline constexpr std::uint64_t kTokenTtlSeconds = 12ull * 60 * 60;
inline constexpr std::size_t kTokenNonceBytes = 16;
// 40 字节 payload + 32 字节 HMAC，十六进制展开成 144 个字符。
inline constexpr std::size_t kTokenPayloadBytes = 40;
inline constexpr std::size_t kTokenSignatureBytes = 32;
inline constexpr std::size_t kTokenBytes =
    kTokenPayloadBytes + kTokenSignatureBytes;
inline constexpr std::size_t kTokenHexBytes = kTokenBytes * 2;

// 一个用户的"口令怎么存的"全部信息。salt / hash 是**原始字节**，
// 不是十六进制文本。
struct PasswordRecord {
  std::string salt;
  std::string hash;
  std::uint32_t iterations = kPasswordIterations;
};

// 用新的随机 salt 派生。口令为空、过短、过长（> 256）或随机数取不到时失败。
bool HashPassword(const std::string& password, PasswordRecord* out,
                  std::string* error_message);

// 定长比较。record 的字段长度不对时返回 false（不是"不匹配"，是"记录本身
// 不可用"）。密码不匹配时返回 true 且 *matches = false。
bool VerifyPassword(const std::string& password, const PasswordRecord& record,
                    bool* matches, std::string* error_message);

// token 里签名保护的字段。
struct TokenPayload {
  std::uint64_t user_id = 0;
  std::uint64_t issued_at = 0;
  std::uint64_t expires_at = 0;
};

// 签发：nonce 由 CSPRNG 生成，expires_at = now + kTokenTtlSeconds。
bool IssueToken(const std::string& secret, std::uint64_t user_id,
                std::uint64_t now_seconds, std::string* token,
                std::string* error_message);

// 校验：十六进制长度、HMAC 定长比较、有效期。失败原因写 error_message，
// 里面**不含** secret，也不含 token 内容。
bool VerifyToken(const std::string& secret, const std::string& token,
                 std::uint64_t now_seconds, TokenPayload* payload,
                 std::string* error_message);

}  // namespace net
}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_REMOTE_AUTH_H_
