// src/network/remote_auth.cpp

#include "remote_auth.h"

#include <cstring>

#include "crypto.h"
#include "network_protocol.h"

namespace backupproject {
namespace net {
namespace {

// token 的 payload 字段用协议那套大端原语拼，避免再写一份字节序代码。
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
