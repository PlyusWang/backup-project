// tests/unit/bpsec2_test.cpp
//
// BPSEC2（服务器签名身份）的握手测试。
//
// 与 secure_transport_test.cpp 的分工：那一个证明 **BPSEC1 没被改坏**
// （15 个用例原样保留），这一个证明 BPSEC2 在 BPSEC1 的记录层之上把身份
// 换成了证书，并且**每一条失败路径都是具名的、都 fail closed**。
//
// 覆盖：
//   P. 正向：证书握手成功、server_id 与证书指纹可读、两端 transcript 相同、
//      并且 BPSEC1 仍然可用（同一台服务端同时接受两种客户端）；
//   N. 失败路径：证书被改一个 bit / 换根 / 空根存储 / server_id 不符 /
//      过期 / 未生效 / 吊销 / 证书公钥与握手公钥不是同一把 / 服务端没证书；
//   D. 拒绝降级：只接受 BPSEC2 的服务端碰到 BPSEC1 客户端必须拒绝；
//   F. fuzz：10000 条畸形的 ServerHello + ServerCertificate 组合，客户端
//      每一次都必须具名失败，绝不建立通道。
//
// 退出码 0 = 全部通过。

#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "bpcert.h"
#include "crypto.h"
#include "ed25519.h"
#include "secure_transport.h"
#include "trusted_root_store.h"
#include "x25519.h"

namespace {

using backupproject::crypto::Bpcert1;
using backupproject::crypto::Bpcert1Error;
using backupproject::crypto::Bpcert1ErrorName;
using backupproject::crypto::TrustedRoot;
using backupproject::crypto::TrustedRootStore;
using backupproject::net::SecureChannel;
using backupproject::net::SecureTransportError;
using backupproject::net::SecureTransportErrorName;
using backupproject::net::ServerIdentityPolicy;
using backupproject::net::ServerKeyPin;
using backupproject::net::TransportIdentity;

int g_passed = 0;
int g_failed = 0;

void Check(bool ok, const std::string& label, const std::string& detail = "") {
  if (ok) {
    ++g_passed;
    std::printf("[bpsec2]   ok   %s\n", label.c_str());
    return;
  }
  ++g_failed;
  if (detail.empty()) {
    std::printf("[bpsec2]   FAIL %s\n", label.c_str());
  } else {
    std::printf("[bpsec2]   FAIL %s（%s）\n", label.c_str(), detail.c_str());
  }
}

// ---- 测试用的根与证书（只在内存里，不是任何真实身份）----

struct TestRoot {
  std::string root_id;
  std::string seed;        // 32 字节 Ed25519 私钥材料
  std::string public_key;  // 32 字节
};

TestRoot MakeRoot(const std::string& root_id) {
  TestRoot root;
  root.root_id = root_id;
  std::string error;
  if (!backupproject::crypto::Ed25519GenerateKeyPair(&root.seed,
                                                     &root.public_key,
                                                     &error)) {
    std::printf("[bpsec2]   FATAL 生成测试根失败：%s\n", error.c_str());
    std::exit(2);
  }
  return root;
}

TrustedRootStore StoreOf(const TestRoot& root) {
  TrustedRootStore store;
  TrustedRoot entry;
  entry.root_id = root.root_id;
  entry.public_key = root.public_key;
  std::string error;
  if (!store.AddRoot(entry, &error)) {
    std::printf("[bpsec2]   FATAL 构造可信根失败：%s\n", error.c_str());
    std::exit(2);
  }
  return store;
}

const char* kServerId = "backup-project-cloud-production";

std::string IssueCertificate(const TestRoot& root,
                             const std::string& server_public_key,
                             const std::string& server_id,
                             std::int64_t not_before, std::int64_t not_after) {
  Bpcert1 certificate;
  certificate.server_id = server_id;
  certificate.issuer_id = root.root_id;
  certificate.server_public_key = server_public_key;
  certificate.serial_number = 20261005001ULL;
  certificate.not_before = not_before;
  certificate.not_after = not_after;
  std::string raw;
  std::string error;
  if (!backupproject::crypto::Bpcert1Issue(certificate, root.seed, &raw,
                                           &error)) {
    std::printf("[bpsec2]   FATAL 签发测试证书失败：%s\n", error.c_str());
    std::exit(2);
  }
  return raw;
}

std::int64_t Now() { return backupproject::crypto::Bpcert1NowUnixSeconds(); }

// ---- 握手运行器 ----

struct Outcome {
  bool client_ok = false;
  bool server_ok = false;
  SecureTransportError client_error = SecureTransportError::kNone;
  std::string client_message;
  std::string server_message;
  std::string client_transcript;
  std::string server_transcript;
  std::string server_id;
  std::string certificate_fingerprint;
  std::string peer_key;
  std::string peer_fingerprint;
  bool client_established = false;
};

// 在 socketpair 上跑一次真实握手。客户端在主线程，服务端在另一个线程。
// pin 与 policy 恰有一个非空。
Outcome RunHandshake(const TransportIdentity& identity, bool require_certificate,
                     const ServerKeyPin* pin,
                     const ServerIdentityPolicy* policy) {
  Outcome out;
  int fds[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
    out.client_message = "socketpair 失败";
    return out;
  }
  SecureChannel server_channel;
  server_channel.SetHandshakeTimeoutMs(4000);
  std::thread server_thread([&]() {
    std::string error;
    const bool ok =
        require_certificate
            ? server_channel.HandshakeServerRequireCertificate(fds[1], identity,
                                                               &error)
            : server_channel.HandshakeServer(fds[1], identity, &error);
    out.server_ok = ok;
    out.server_message = error;
    out.server_transcript = server_channel.transcript_hash();
  });

  SecureChannel client;
  client.SetHandshakeTimeoutMs(4000);
  std::string error;
  if (policy != nullptr) {
    out.client_ok = client.HandshakeClientWithCertificate(fds[0], *policy, &error);
  } else {
    out.client_ok = client.HandshakeClient(fds[0], *pin, &error);
  }
  out.client_error = client.last_error();
  out.client_message = error;
  out.client_established = client.established();
  out.client_transcript = client.transcript_hash();
  out.server_id = client.peer_server_id();
  out.certificate_fingerprint = client.peer_certificate_fingerprint();
  out.peer_key = client.peer_public_key();
  out.peer_fingerprint = client.peer_fingerprint();

  // 先关掉客户端这一端再 join：负向用例里服务端正在读，早一点关它才能
  // 立刻看到 EOF，而不是等满握手超时。
  ::close(fds[0]);
  server_thread.join();
  ::close(fds[1]);
  return out;
}

// 伪造的服务端：把 bytes 原样写下去就关掉。用来跑畸形输入。
Outcome RunAgainstBytes(const std::string& bytes,
                        const ServerIdentityPolicy& policy) {
  Outcome out;
  int fds[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
    return out;
  }
  std::thread writer([&]() {
    std::size_t written = 0;
    while (written < bytes.size()) {
      const ssize_t n = ::write(fds[1], bytes.data() + written,
                                bytes.size() - written);
      if (n <= 0) break;
      written += static_cast<std::size_t>(n);
    }
    ::close(fds[1]);
  });
  SecureChannel client;
  client.SetHandshakeTimeoutMs(4000);
  std::string error;
  out.client_ok = client.HandshakeClientWithCertificate(fds[0], policy, &error);
  out.client_error = client.last_error();
  out.client_message = error;
  out.client_established = client.established();
  ::close(fds[0]);
  writer.join();
  return out;
}

void AppendU32(std::string* out, std::uint32_t value) {
  out->push_back(static_cast<char>((value >> 24) & 0xFF));
  out->push_back(static_cast<char>((value >> 16) & 0xFF));
  out->push_back(static_cast<char>((value >> 8) & 0xFF));
  out->push_back(static_cast<char>(value & 0xFF));
}

// 按布局拼一条版本 2 的 ServerHello（内容随便，客户端在验完证书之前
// 不会用这些字节做任何密码学运算）。
std::string BuildFakeServerHello(std::uint8_t version) {
  std::string out;
  AppendU32(&out, 0x42505331u);
  out.push_back(static_cast<char>(2));  // type = ServerHello
  out.push_back(static_cast<char>(version));
  out.push_back(static_cast<char>(0));
  out.push_back(static_cast<char>(1));  // suite = 1
  for (int i = 0; i < 96; ++i) {
    out.push_back(static_cast<char>((i * 37 + 11) & 0xFF));
  }
  return out;
}

std::string BuildCertificateMessage(const std::string& certificate,
                                    std::uint32_t declared_length) {
  std::string out;
  AppendU32(&out, 0x42505331u);
  out.push_back(static_cast<char>(6));  // type = ServerCertificate
  out.push_back(static_cast<char>(2));  // version = 2
  out.push_back(static_cast<char>(0));
  out.push_back(static_cast<char>(0));
  AppendU32(&out, declared_length);
  out.append(certificate);
  return out;
}

std::uint64_t g_random_state = 0x243F6A8885A308D3ULL;
std::uint64_t NextRandom() {
  g_random_state = g_random_state * 6364136223846793005ULL +
                   1442695040888963407ULL;
  return g_random_state >> 17;
}

}  // namespace

int main() {
  // 输出不缓冲：万一某条用例把进程挂住，日志里最后一行就是"卡在哪一条"。
  // （第一版没这行，卡住时 run.log 是 0 字节，只能靠 gdb，很难查。）
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  const TestRoot root = MakeRoot("test-official-root-a");
  const TestRoot other_root = MakeRoot("test-other-root-b");
  const std::int64_t now = Now();

  // 服务端身份：真实生成的 X25519 密钥 + 为它签发的证书。
  TransportIdentity identity;
  std::string error;
  if (!backupproject::net::GenerateTransportIdentity(&identity, &error)) {
    std::printf("[bpsec2]   FATAL 生成服务端身份失败：%s\n", error.c_str());
    return 2;
  }
  identity.certificate = IssueCertificate(root, identity.public_key, kServerId,
                                          now - 60, now + 180 * 24 * 60 * 60);

  ServerIdentityPolicy policy;
  policy.roots = StoreOf(root);
  policy.expected_server_id = kServerId;

  // ---- P. 正向 ----
  {
    const Outcome outcome = RunHandshake(identity, false, nullptr, &policy);
    Check(outcome.client_ok, "P01 证书握手成功（客户端）", outcome.client_message);
    Check(outcome.server_ok, "P02 证书握手成功（服务端）", outcome.server_message);
    Check(outcome.client_established, "P03 客户端通道 established");
    Check(outcome.server_id == kServerId, "P04 客户端读到证书里的 server_id",
          outcome.server_id);
    Check(outcome.certificate_fingerprint.size() == 64,
          "P05 客户端拿到证书指纹（sha256 十六进制）",
          outcome.certificate_fingerprint);
    Check(outcome.peer_key == identity.public_key,
          "P06 对端公钥就是服务端身份公钥");
    Check(outcome.peer_fingerprint ==
              backupproject::crypto::X25519Fingerprint(identity.public_key),
          "P07 对端指纹与服务端身份一致");
    Check(!outcome.client_transcript.empty() &&
              outcome.client_transcript == outcome.server_transcript,
          "P08 两端 transcript 相同（含整张证书）");
  }

  // 同一台服务端（带证书）仍然接受 BPSEC1 客户端：既有部署不被破坏。
  {
    ServerKeyPin pin;
    Check(backupproject::net::ParseServerKeyPin(
              "sha256:" +
                  backupproject::crypto::X25519Fingerprint(identity.public_key),
              &pin, &error),
          "P09 构造 BPSEC1 pin", error);
    const Outcome outcome = RunHandshake(identity, false, &pin, nullptr);
    Check(outcome.client_ok && outcome.server_ok,
          "P10 同一服务端仍然接受 BPSEC1 客户端（向后兼容）",
          outcome.client_message + " / " + outcome.server_message);
  }

  // ---- N. 失败路径 ----
  {
    // N01 证书被改一个 bit -> 签名无效
    TransportIdentity tampered = identity;
    tampered.certificate[identity.certificate.size() / 2] ^= 0x20;
    const Outcome o = RunHandshake(tampered, false, nullptr, &policy);
    Check(!o.client_ok && o.client_error == SecureTransportError::kCertificateInvalid,
          "N01 证书被改一个 bit -> certificate-invalid",
          SecureTransportErrorName(o.client_error));

    // N02 换一把根签发的证书（issuer_id 对不上）-> 不受信任
    TransportIdentity foreign = identity;
    foreign.certificate = IssueCertificate(
        other_root, identity.public_key, kServerId, now - 60,
        now + 180 * 24 * 60 * 60);
    const Outcome o2 = RunHandshake(foreign, false, nullptr, &policy);
    Check(!o2.client_ok &&
              o2.client_error == SecureTransportError::kCertificateUntrusted,
          "N02 陌生根签发的证书 -> certificate-untrusted",
          SecureTransportErrorName(o2.client_error));

    // N03 空根存储 -> 什么都不信
    ServerIdentityPolicy empty_policy;
    empty_policy.expected_server_id = kServerId;
    const Outcome o3 = RunHandshake(identity, false, nullptr, &empty_policy);
    Check(!o3.client_ok &&
              o3.client_error == SecureTransportError::kCertificateUntrusted,
          "N03 空根存储 -> certificate-untrusted（默认拒绝）",
          SecureTransportErrorName(o3.client_error));

    // N04 server_id 不符（同一把根签发的另一台服务器）
    ServerIdentityPolicy wrong_id = policy;
    wrong_id.expected_server_id = "some-other-cloud";
    const Outcome o4 = RunHandshake(identity, false, nullptr, &wrong_id);
    Check(!o4.client_ok &&
              o4.client_error == SecureTransportError::kCertificateWrongServerId,
          "N04 server_id 不符 -> certificate-wrong-server-id",
          SecureTransportErrorName(o4.client_error));

    // N05 证书已过期
    ServerIdentityPolicy expired_policy = policy;
    expired_policy.now_unix_seconds = now + 400 * 24 * 60 * 60;
    const Outcome o5 = RunHandshake(identity, false, nullptr, &expired_policy);
    Check(!o5.client_ok &&
              o5.client_error == SecureTransportError::kCertificateExpired,
          "N05 证书已过期 -> certificate-expired",
          SecureTransportErrorName(o5.client_error));

    // N06 证书还没生效
    ServerIdentityPolicy early_policy = policy;
    early_policy.now_unix_seconds = now - 400 * 24 * 60 * 60;
    const Outcome o6 = RunHandshake(identity, false, nullptr, &early_policy);
    Check(!o6.client_ok &&
              o6.client_error == SecureTransportError::kCertificateExpired,
          "N06 证书尚未生效 -> certificate-expired（文案会提示时钟）",
          SecureTransportErrorName(o6.client_error));

    // N07 根被吊销
    TrustedRootStore revoked_store;
    TrustedRoot revoked_entry;
    revoked_entry.root_id = root.root_id;
    revoked_entry.public_key = root.public_key;
    revoked_entry.revoked = true;
    std::string add_error;
    Check(revoked_store.AddRoot(revoked_entry, &add_error),
          "N07a 构造被吊销的根", add_error);
    ServerIdentityPolicy revoked_policy = policy;
    revoked_policy.roots = revoked_store;
    const Outcome o7 = RunHandshake(identity, false, nullptr, &revoked_policy);
    Check(!o7.client_ok &&
              o7.client_error == SecureTransportError::kCertificateUntrusted,
          "N07b 根被吊销 -> certificate-untrusted",
          SecureTransportErrorName(o7.client_error));

    // N08 证书认证的公钥 ≠ 握手里实际使用的身份公钥
    TransportIdentity mismatched = identity;
    {
      std::string other_private;
      std::string other_public;
      if (!backupproject::crypto::X25519GenerateKeyPair(&other_private,
                                                        &other_public, &error)) {
        std::printf("[bpsec2]   FATAL 生成另一把服务器密钥失败\n");
        return 2;
      }
      mismatched.certificate = IssueCertificate(
          root, other_public, kServerId, now - 60, now + 180 * 24 * 60 * 60);
    }
    const Outcome o8 = RunHandshake(mismatched, false, nullptr, &policy);
    Check(!o8.client_ok &&
              o8.client_error == SecureTransportError::kCertificateKeyMismatch,
          "N08 证书公钥 ≠ 握手公钥 -> certificate-key-mismatch",
          SecureTransportErrorName(o8.client_error));

    // N09 客户端要 BPSEC2，服务端没配证书
    TransportIdentity no_cert = identity;
    no_cert.certificate.clear();
    const Outcome o9 = RunHandshake(no_cert, false, nullptr, &policy);
    // 服务端的选择是"不说话"：它没有证书，就不能假装能用签名身份交谈。
    // 于是客户端在等 ServerHello 时等到的是超时（io-error）—— 这是**正确**
    // 的观测结果，不是缺陷：客户端无从知道对端为什么不回答。生产路径里
    // 调用方失败后立刻关连接，客户端会更快拿到断开。
    Check(!o9.server_ok && o9.server_message.find("证书") != std::string::npos,
          "N09a 服务端没有证书时必须拒绝 BPSEC2 客户端（服务端侧）",
          o9.server_message);
    Check(!o9.client_ok && o9.client_error != SecureTransportError::kNone,
          "N09b 客户端也因此失败，且带具名原因",
          SecureTransportErrorName(o9.client_error));

    // N10 证书被截断（客户端必须报带名字的原因，而不是"网络错误"）
    TransportIdentity truncated = identity;
    truncated.certificate =
        identity.certificate.substr(0, identity.certificate.size() - 20);
    const Outcome o10 = RunHandshake(truncated, false, nullptr, &policy);
    Check(!o10.client_ok &&
              o10.client_error == SecureTransportError::kCertificateInvalid,
          "N10 证书被截断 -> certificate-invalid",
          SecureTransportErrorName(o10.client_error));
  }

  // ---- D. 拒绝降级 ----
  {
    ServerKeyPin pin;
    backupproject::net::ParseServerKeyPin(
        "sha256:" + backupproject::crypto::X25519Fingerprint(identity.public_key),
        &pin, &error);
    const Outcome outcome = RunHandshake(identity, true, &pin, nullptr);
    Check(!outcome.server_ok, "D01 只接受 BPSEC2 的服务端拒绝 BPSEC1 客户端",
          outcome.server_message);
    Check(!outcome.client_ok, "D02 客户端也因此失败（没有明文回退）");
    Check(outcome.server_message.find("降级") != std::string::npos ||
              outcome.server_message.find("BPSEC2") != std::string::npos,
          "D03 服务端给出的原因是拒绝降级", outcome.server_message);

    // 只接受 BPSEC2 的服务端 + 证书客户端 = 正常
    const Outcome ok_outcome = RunHandshake(identity, true, nullptr, &policy);
    Check(ok_outcome.client_ok && ok_outcome.server_ok,
          "D04 只接受 BPSEC2 的服务端 + 证书客户端 -> 成功",
          ok_outcome.client_message + " / " + ok_outcome.server_message);

    // 只接受 BPSEC2 但没有证书 = 配置错误，必须启动即失败（不是连上才失败）
    TransportIdentity no_cert = identity;
    no_cert.certificate.clear();
    const Outcome bad = RunHandshake(no_cert, true, nullptr, &policy);
    Check(!bad.server_ok &&
              bad.server_message.find("证书") != std::string::npos,
          "D05 require-certificate 但没有证书 -> 直接拒绝", bad.server_message);
  }

  // ---- F. fuzz：10000 条畸形的 ServerHello + ServerCertificate ----
  {
    const std::string good_hello = BuildFakeServerHello(2);
    int accepted = 0;
    int wrong_error = 0;
    std::string first_accept;
    for (int i = 0; i < 10000; ++i) {
      std::string bytes;
      const int shape = i % 5;
      if (shape == 0) {
        // 完全随机的 ServerHello（长度都不一定对）
        const std::size_t length = 1 + NextRandom() % 200;
        bytes.assign(length, '\0');
        for (std::size_t j = 0; j < length; ++j) {
          bytes[j] = static_cast<char>(NextRandom() & 0xFF);
        }
      } else if (shape == 1) {
        // 合法 ServerHello + 完全随机的证书消息
        bytes = good_hello;
        const std::size_t length = 1 + NextRandom() % 300;
        for (std::size_t j = 0; j < length; ++j) {
          bytes.push_back(static_cast<char>(NextRandom() & 0xFF));
        }
      } else if (shape == 2) {
        // 合法 ServerHello + 合法头 + 随机证书体
        bytes = good_hello;
        const std::size_t length = 1 + NextRandom() % 300;
        std::string body(length, '\0');
        for (std::size_t j = 0; j < length; ++j) {
          body[j] = static_cast<char>(NextRandom() & 0xFF);
        }
        bytes += BuildCertificateMessage(
            body, static_cast<std::uint32_t>(body.size()));
      } else if (shape == 3) {
        // 合法 ServerHello + 头里声明一个荒唐的长度
        static const std::uint32_t kBadLengths[] = {0u, 1u, 137u, 4097u,
                                                    0xFFFFFFFFu, 0x80000000u};
        bytes = good_hello;
        bytes += BuildCertificateMessage(
            std::string(8, '\0'),
            kBadLengths[NextRandom() % (sizeof(kBadLengths) / sizeof(kBadLengths[0]))]);
      } else {
        // 合法 ServerHello + 真证书 + 随机改一个字节
        std::string body = identity.certificate;
        const std::size_t position = NextRandom() % body.size();
        body[position] = static_cast<char>(body[position] ^
                                           (1 + (NextRandom() % 255)));
        bytes = good_hello;
        bytes += BuildCertificateMessage(
            body, static_cast<std::uint32_t>(body.size()));
      }
      const Outcome outcome = RunAgainstBytes(bytes, policy);
      if (outcome.client_ok || outcome.client_established) {
        ++accepted;
        if (first_accept.empty()) {
          first_accept = "shape " + std::to_string(shape) + " 第 " +
                         std::to_string(i) + " 例";
        }
      } else if (outcome.client_error == SecureTransportError::kNone) {
        ++wrong_error;
      }
    }
    Check(accepted == 0,
          "F01 10000 条畸形 ServerHello/ServerCertificate 没有一条被接受",
          first_accept);
    Check(wrong_error == 0,
          "F02 每一条失败都带具名原因（没有裸失败）",
          std::to_string(wrong_error));
  }

  std::printf("[bpsec2] passed=%d failed=%d\n", g_passed, g_failed);
  return g_failed == 0 ? 0 : 1;
}
