// tests/unit/remote_auth_test.cpp
//
// PR #20：口令派生与签名 token 的专项测试。
//
// 口令在这个文件里全部是**运行时随机生成**的，不打印、不写文件、不进断言
// 文本。测试断言的是"行为"，不是"某个固定口令的摘要"。

#include <chrono>
#include <cstdint>
#include <string>

#include "crypto.h"
#include "network_protocol.h"
#include "remote_auth.h"
#include "test_support.h"

namespace bp = backupproject;
namespace net = backupproject::net;

namespace {

std::string RandomHex(std::size_t bytes) {
  std::string raw;
  std::string error;
  if (!bp::crypto::RandomBytes(bytes, &raw, &error)) {
    return std::string();
  }
  return bp::crypto::ToHex(
      reinterpret_cast<const unsigned char*>(raw.data()), raw.size());
}

bool IsLowerHex(const std::string& text) {
  for (const char character : text) {
    const bool digit = character >= '0' && character <= '9';
    const bool lower = character >= 'a' && character <= 'f';
    if (!digit && !lower) {
      return false;
    }
  }
  return !text.empty();
}

}  // namespace

int main() {
  test_support::Section("AUTH 1. 口令派生：随机 salt + 迭代下限");
  {
    const std::string password = RandomHex(12);  // 24 个十六进制字符
    net::PasswordRecord first;
    std::string error;
    const auto started = std::chrono::steady_clock::now();
    const bool hashed = net::HashPassword(password, &first, &error);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    test_support::Check(hashed, "AUTH T1 口令派生成功", error);
    test_support::Check(first.salt.size() == net::kPasswordSaltBytes &&
                            first.hash.size() == net::kPasswordHashBytes,
                        "AUTH T1 salt 16 字节 / hash 32 字节");
    test_support::Check(first.iterations == net::kPasswordIterations &&
                            net::kPasswordIterations >= 200000,
                        "AUTH T1 迭代次数不低于 200000");
    test_support::Note("一次 PBKDF2-HMAC-SHA256(" +
                       std::to_string(net::kPasswordIterations) + " 次) 用时 " +
                       std::to_string(elapsed) + " ms");

    net::PasswordRecord second;
    error.clear();
    test_support::Check(net::HashPassword(password, &second, &error),
                        "AUTH T1 第二次派生成功", error);
    test_support::Check(second.salt != first.salt && second.hash != first.hash,
                        "AUTH T1 判别：salt 随机，同一口令两次得到不同摘要");
  }

  test_support::Section("AUTH 2. 口令派生的失败路径");
  {
    net::PasswordRecord record;
    std::string error;
    test_support::Check(!net::HashPassword("", &record, &error),
                        "AUTH T2 空口令被拒绝");
    error.clear();
    test_support::Check(!net::HashPassword("short", &record, &error),
                        "AUTH T2 少于 8 字节的口令被拒绝");
    error.clear();
    test_support::Check(!net::HashPassword(std::string(257, 'x'), &record, &error),
                        "AUTH T2 超过 256 字节的口令被拒绝");
    error.clear();
    test_support::Check(!net::HashPassword(std::string("abcd\0efgh", 9), &record,
                                           &error),
                        "AUTH T2 含 NUL 的口令被拒绝");
    error.clear();
    test_support::Check(!net::HashPassword("long-enough", nullptr, &error),
                        "AUTH T2 输出指针为空被拒绝");
  }

  test_support::Section("AUTH 3. 口令校验：定长比较与不可用记录");
  {
    const std::string password = RandomHex(12);
    net::PasswordRecord record;
    std::string error;
    net::HashPassword(password, &record, &error);

    bool matches = false;
    test_support::Check(net::VerifyPassword(password, record, &matches, &error) &&
                            matches,
                        "AUTH T3 正确口令通过", error);
    bool wrong_matches = true;
    error.clear();
    test_support::Check(net::VerifyPassword(password + "x", record, &wrong_matches,
                                            &error) &&
                            !wrong_matches,
                        "AUTH T3 错误口令不通过（但调用本身成功）", error);

    net::PasswordRecord truncated = record;
    truncated.hash = truncated.hash.substr(0, 31);
    error.clear();
    test_support::Check(!net::VerifyPassword(password, truncated, &matches, &error),
                        "AUTH T3 hash 长度不对的记录被拒绝");

    net::PasswordRecord weak = record;
    weak.iterations = 1000;
    error.clear();
    test_support::Check(!net::VerifyPassword(password, weak, &matches, &error),
                        "AUTH T3 迭代次数被改小的记录被拒绝");

    error.clear();
    test_support::Check(!net::VerifyPassword(password, record, nullptr, &error),
                        "AUTH T3 输出指针为空被拒绝");
  }

  test_support::Section("AUTH 4. token 签发与校验");
  {
    const std::string secret = RandomHex(32);
    const std::string password = RandomHex(12);
    std::string error;
    const std::uint64_t now = 1700000000;
    std::string token;
    test_support::Check(net::IssueToken(secret, 42, now, &token, &error),
                        "AUTH T4 token 签发成功", error);
    test_support::Check(token.size() == net::kTokenHexBytes && IsLowerHex(token),
                        "AUTH T4 token 是定长小写十六进制");
    test_support::Check(token.find(secret) == std::string::npos,
                        "AUTH T4 判别：token 里不包含 secret 本身");
    test_support::Check(token.find(password) == std::string::npos,
                        "AUTH T4 判别：token 里不包含口令");

    net::TokenPayload payload;
    error.clear();
    test_support::Check(net::VerifyToken(secret, token, now, &payload, &error),
                        "AUTH T4 token 校验通过", error);
    test_support::Check(payload.user_id == 42 && payload.issued_at == now &&
                            payload.expires_at == now + net::kTokenTtlSeconds,
                        "AUTH T4 判别：user_id / 签发时间 / 12 小时有效期正确");
    test_support::Check(net::kTokenTtlSeconds == 12ull * 3600,
                        "AUTH T4 TTL 固定为 12 小时");

    // 同一用户连续签发两次：nonce 不同，token 必须不同。
    std::string second;
    error.clear();
    net::IssueToken(secret, 42, now, &second, &error);
    test_support::Check(second != token,
                        "AUTH T4 判别：nonce 随机，两次签发结果不同");
    net::TokenPayload second_payload;
    error.clear();
    test_support::Check(net::VerifyToken(secret, second, now, &second_payload,
                                         &error) &&
                            second_payload.user_id == 42,
                        "AUTH T4 第二个 token 同样有效");
  }

  test_support::Section("AUTH 5. token 的失败路径");
  {
    const std::string secret = RandomHex(32);
    const std::string other_secret = RandomHex(32);
    const std::uint64_t now = 1700000000;
    std::string token;
    std::string error;
    net::IssueToken(secret, 7, now, &token, &error);
    net::TokenPayload payload;

    std::string tampered = token;
    const std::size_t position = tampered.size() / 2;
    tampered[position] = (tampered[position] == 'a') ? 'b' : 'a';
    error.clear();
    test_support::Check(!net::VerifyToken(secret, tampered, now, &payload, &error),
                        "AUTH T5 篡改一个字符之后校验失败", error);

    error.clear();
    test_support::Check(
        !net::VerifyToken(other_secret, token, now, &payload, &error),
        "AUTH T5 用另一台服务器的 secret 校验失败", error);

    error.clear();
    test_support::Check(
        !net::VerifyToken(secret, token, now + net::kTokenTtlSeconds, &payload,
                          &error),
        "AUTH T5 到期之后校验失败", error);
    error.clear();
    test_support::Check(net::VerifyToken(secret, token,
                                         now + net::kTokenTtlSeconds - 1,
                                         &payload, &error),
                        "AUTH T5 到期前一刻仍然有效", error);

    error.clear();
    test_support::Check(!net::VerifyToken(secret, token.substr(0, 143), now,
                                          &payload, &error),
                        "AUTH T5 长度不对的 token 被拒绝");
    error.clear();
    std::string not_hex = token;
    not_hex[0] = 'z';
    test_support::Check(!net::VerifyToken(secret, not_hex, now, &payload, &error),
                        "AUTH T5 非十六进制 token 被拒绝");
    error.clear();
    test_support::Check(!net::VerifyToken("", token, now, &payload, &error),
                        "AUTH T5 空 secret 被拒绝");
    error.clear();
    test_support::Check(!net::IssueToken(secret, 0, now, &token, &error) ||
                            !net::VerifyToken(secret, token, now, &payload,
                                              &error),
                        "AUTH T5 user_id 为 0 的 token 不可用");
  }

  return test_support::Finish("remote_auth_test");
}
