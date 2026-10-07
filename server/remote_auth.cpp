// server/remote_auth.cpp

// 边界：本文件是纯算法层 —— 不碰 SQLite、不碰 socket、不写日志、不落盘，
// 只回答"这个口令该怎么存/怎么比"与"这枚 token 是不是我签的"。
//
// token 的字节布局（先拼 payload，再拼签名，最后整体转十六进制）：
//   offset  size  field
//   0       8     user_id     (u64 大端，0 视为非法)
//   8       8     issued_at   (u64 大端，Unix 秒)
//   16      8     expires_at  (u64 大端，Unix 秒)
//   24      16    nonce       (CSPRNG 字节，随 payload 一起被签名)
//   40      32    HMAC-SHA256(secret, payload[0..40))
// 十六进制展开后正好 144 个字符，因此可以安全地放进 header / JSON。
//
// 不变量：任何一处拿不到随机数就直接失败，绝不回落到固定 salt 或固定 nonce。
#include "remote_auth.h"

#include <cstring>

#include "crypto.h"
#include "network_protocol.h"

namespace backupproject {
namespace net {
namespace {

// token 的 payload 字段用协议那套大端原语拼，避免再写一份字节序代码。
// 大端写入。手写而不再引入一份字节序工具：协议上"字段多宽、怎么排"只有
// 这一个定义点，避免两处实现对同一份 payload 产生不同理解。
void AppendU64BE(std::string* out, std::uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out->push_back(static_cast<char>((value >> shift) & 0xFFu));
  }
}

std::uint64_t LoadU64BE(const unsigned char* data) {
  std::uint64_t value = 0;
  for (int index = 0; index < 8; ++index) {
    value = (value << 8) | static_cast<std::uint64_t>(data[index]);
  }
  return value;
}

}  // namespace

// 用新的随机 salt 派生口令记录。前置条件：口令 8..256 字节且不含 NUL ——
// NUL 不是 PBKDF2 的问题（它按长度取字节），而是这条口令一路要经过
// C 字符串与文本协议，中途被截断会让两个不同的口令比出相同结果。
// 随机数取不到时返回失败，绝不用固定 salt 顶上。
bool HashPassword(const std::string& password, PasswordRecord* out,
                  std::string* error_message) {
  if (out == nullptr) {
    if (error_message != nullptr) {
      *error_message = "password record output is null";
    }
    return false;
  }
  if (password.size() < kMinPasswordBytes ||
      password.size() > kMaxPasswordBytes) {
    if (error_message != nullptr) {
      *error_message = "password must be 8 to 256 bytes long";
    }
    return false;
  }
  if (password.find('\0') != std::string::npos) {
    if (error_message != nullptr) {
      *error_message = "password must not contain a NUL byte";
    }
    return false;
  }
  std::string salt;
  std::string random_error;
  if (!crypto::RandomBytes(kPasswordSaltBytes, &salt, &random_error)) {
    if (error_message != nullptr) {
      *error_message = "cannot obtain random salt: " + random_error;
    }
    return false;
  }
  std::string hash;
  std::string kdf_error;
  if (!crypto::Pbkdf2HmacSha256(password, salt, kPasswordIterations,
                                kPasswordHashBytes, &hash, &kdf_error)) {
    if (error_message != nullptr) {
      *error_message = "PBKDF2 failed: " + kdf_error;
    }
    return false;
  }
  out->salt = salt;
  out->hash = hash;
  out->iterations = kPasswordIterations;
  return true;
}

// 定长比较。返回 false 表示"记录本身不可用"（字段长度不对、迭代次数低于
// 下限），返回 true 且 *matches=false 才是"口令不对"。这两件事必须分开报：
// 前者是数据或版本问题，后者只是用户输错了。
// 迭代次数低于下限直接拒绝，而不是按记录里的参数算一遍 —— 能被改小的只有
// 库本身或版本，照它算等于接受一次静默的参数降级。
// *matches 在进函数时就置 false，调用方只能在返回 true 时读它。
bool VerifyPassword(const std::string& password, const PasswordRecord& record,
                    bool* matches, std::string* error_message) {
  if (matches == nullptr) {
    if (error_message != nullptr) {
      *error_message = "match output is null";
    }
    return false;
  }
  *matches = false;
  if (record.salt.size() != kPasswordSaltBytes ||
      record.hash.size() != kPasswordHashBytes) {
    if (error_message != nullptr) {
      *error_message = "stored password record has unexpected field sizes";
    }
    return false;
  }
  if (record.iterations < kPasswordIterations) {
    // 迭代次数被改小只可能意味着库被改过或版本不对：明确拒绝，
    // 而不是"按记录的参数算一遍"。
    if (error_message != nullptr) {
      *error_message = "stored password record uses too few iterations";
    }
    return false;
  }
  std::string derived;
  std::string kdf_error;
  if (!crypto::Pbkdf2HmacSha256(password, record.salt, record.iterations,
                                kPasswordHashBytes, &derived, &kdf_error)) {
    if (error_message != nullptr) {
      *error_message = "PBKDF2 failed: " + kdf_error;
    }
    return false;
  }
  *matches = crypto::ConstantTimeEquals(derived, record.hash);
  return true;
}

// 签发：nonce 每次都取新的 CSPRNG 字节，expires_at 固定为 now + 12 小时。
// 服务端不保存会话表，所以 token 一旦签出就无法单独吊销 —— 这是无状态换
// 来的代价，写在 KNOWN-LIMITATIONS 里，代码里不假装它有。
bool IssueToken(const std::string& secret, std::uint64_t user_id,
                std::uint64_t now_seconds, std::string* token,
                std::string* error_message) {
  if (token == nullptr) {
    if (error_message != nullptr) {
      *error_message = "token output is null";
    }
    return false;
  }
  if (secret.empty()) {
    if (error_message != nullptr) {
      *error_message = "token secret is empty";
    }
    return false;
  }
  std::string nonce;
  std::string random_error;
  if (!crypto::RandomBytes(kTokenNonceBytes, &nonce, &random_error)) {
    if (error_message != nullptr) {
      *error_message = "cannot obtain a token nonce: " + random_error;
    }
    return false;
  }
  std::string payload;
  AppendU64BE(&payload, user_id);
  AppendU64BE(&payload, now_seconds);
  AppendU64BE(&payload, now_seconds + kTokenTtlSeconds);
  payload += nonce;

  const std::string signature = crypto::HmacSha256Raw(secret, payload);
  const std::string raw = payload + signature;
  *token = crypto::ToHex(reinterpret_cast<const unsigned char*>(raw.data()),
                         raw.size());
  return true;
}

// 校验顺序（廉价检查在前）：secret 非空 -> 十六进制长度必须正好
// kTokenHexBytes -> 解码后长度必须正好 kTokenBytes -> HMAC 定长比较 ->
// user_id 非 0 -> 未过期。
// 长度先于解码是刻意的：不认识的输入不该换来一次 144 字符的分配与解析。
// 过期判定用 <=：expires_at == now 的那一秒已经算过期。
bool VerifyToken(const std::string& secret, const std::string& token,
                 std::uint64_t now_seconds, TokenPayload* payload,
                 std::string* error_message) {
  if (payload == nullptr) {
    if (error_message != nullptr) {
      *error_message = "token payload output is null";
    }
    return false;
  }
  if (secret.empty()) {
    if (error_message != nullptr) {
      *error_message = "token secret is empty";
    }
    return false;
  }
  if (token.size() != kTokenHexBytes) {
    if (error_message != nullptr) {
      *error_message = "token has the wrong length";
    }
    return false;
  }
  std::string raw;
  if (!crypto::FromHex(token, &raw) || raw.size() != kTokenBytes) {
    if (error_message != nullptr) {
      *error_message = "token is not valid hexadecimal";
    }
    return false;
  }
  const std::string signed_part = raw.substr(0, kTokenPayloadBytes);
  const std::string signature = raw.substr(kTokenPayloadBytes);
  // 定长比较，不按字节短路：签名每个字节都被读到，避免用比较耗时泄露
  // "前缀猜对了多少"。
  const std::string expected = crypto::HmacSha256Raw(secret, signed_part);
  if (!crypto::ConstantTimeEquals(expected, signature)) {
    if (error_message != nullptr) {
      // 不区分"签名不对"和"换了另一台服务器的 secret"，避免给攻击者反馈。
      *error_message = "token signature does not verify";
    }
    return false;
  }
  const unsigned char* bytes =
      reinterpret_cast<const unsigned char*>(signed_part.data());
  TokenPayload parsed;
  // 三个 u64 的顺序必须与 IssueToken 的写入顺序逐字对应，偏移量就是上面
  // 布局表里的 0 / 8 / 16；改任何一边都必须同时改另一边。
  parsed.user_id = LoadU64BE(bytes);
  parsed.issued_at = LoadU64BE(bytes + 8);
  parsed.expires_at = LoadU64BE(bytes + 16);
  if (parsed.user_id == 0) {
    if (error_message != nullptr) {
      *error_message = "token does not name a user";
    }
    return false;
  }
  if (parsed.expires_at <= now_seconds) {
    if (error_message != nullptr) {
      *error_message = "token has expired";
    }
    return false;
  }
  *payload = parsed;
  return true;
}

}  // namespace net
}  // namespace backupproject
