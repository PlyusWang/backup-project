// src/network/secure_transport.cpp
//
// BPSEC1 的实现。协议与安全性质的说明见 include/secure_transport.h。
//
// 这个文件里没有第三方密码学代码：X25519 / HKDF / AES-256-CTR / HMAC-SHA256
// 全部来自本项目自己的 src/crypto/。

#include "secure_transport.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "crypto.h"
#include "hkdf.h"

namespace backupproject {
namespace net {
namespace {

using crypto::ConstantTimeEquals;
using crypto::HkdfExpand;
using crypto::HkdfExtract;
using crypto::HmacSha256;
using crypto::Sha256;
using crypto::X25519GenerateKeyPair;
using crypto::X25519SharedSecret;

inline constexpr std::size_t kX25519Bytes = crypto::kX25519KeySize;     // 32
inline constexpr std::size_t kSha256Bytes = crypto::kSha256DigestSize;  // 32

std::string StrerrorText() { return std::string(std::strerror(errno)); }

void AppendU16(std::string* out, std::uint16_t value) {
  out->push_back(static_cast<char>((value >> 8) & 0xFFu));
  out->push_back(static_cast<char>(value & 0xFFu));
}

void AppendU32(std::string* out, std::uint32_t value) {
  out->push_back(static_cast<char>((value >> 24) & 0xFFu));
  out->push_back(static_cast<char>((value >> 16) & 0xFFu));
  out->push_back(static_cast<char>((value >> 8) & 0xFFu));
  out->push_back(static_cast<char>(value & 0xFFu));
}

void AppendU64(std::string* out, std::uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out->push_back(static_cast<char>((value >> shift) & 0xFFu));
  }
}

std::uint32_t LoadU32(const unsigned char* data) {
  return (static_cast<std::uint32_t>(data[0]) << 24) |
         (static_cast<std::uint32_t>(data[1]) << 16) |
         (static_cast<std::uint32_t>(data[2]) << 8) |
         static_cast<std::uint32_t>(data[3]);
}

std::uint64_t LoadU64(const unsigned char* data) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    value = (value << 8) | static_cast<std::uint64_t>(data[i]);
  }
  return value;
}

std::string HmacTag(const std::string& key, const std::string& data) {
  unsigned char digest[kSha256Bytes];
  HmacSha256::Compute(key.data(), key.size(), data.data(), data.size(), digest);
  return std::string(reinterpret_cast<const char*>(digest), sizeof(digest));
}

std::string Sha256Of(const std::string& data) {
  unsigned char digest[kSha256Bytes];
  Sha256::Hash(data.data(), data.size(), digest);
  return std::string(reinterpret_cast<const char*>(digest), sizeof(digest));
}

// 记录层的 tag：
//   HMAC(mac_key, "BPSEC1 record v1" || direction || seq || ciphertext_length
//   || ciphertext)
// 用流式 HMAC 拼，避免为 1 MiB 的记录再复制一份 mac 输入。
std::string RecordTag(const std::string& mac_key, std::uint8_t direction,
                      std::uint64_t sequence, const std::string& ciphertext) {
  unsigned char header[13];
  header[0] = direction;
  for (std::size_t i = 0; i < 8; ++i) {
    header[1 + i] =
        static_cast<unsigned char>((sequence >> (56 - 8 * i)) & 0xFFu);
  }
  const std::uint32_t length = static_cast<std::uint32_t>(ciphertext.size());
  header[9] = static_cast<unsigned char>((length >> 24) & 0xFFu);
  header[10] = static_cast<unsigned char>((length >> 16) & 0xFFu);
  header[11] = static_cast<unsigned char>((length >> 8) & 0xFFu);
  header[12] = static_cast<unsigned char>(length & 0xFFu);

  HmacSha256 mac(mac_key);
  mac.Update(kBssec1RecordLabel, kBssec1RecordLabelSize);
  mac.Update(header, sizeof(header));
  mac.Update(ciphertext.data(), ciphertext.size());
  unsigned char digest[kSha256Bytes];
  mac.Final(digest);
  return std::string(reinterpret_cast<const char*>(digest), sizeof(digest));
}

// AES-CTR 的 128 位计数器块：
//   nonce_prefix(4 字节) || record_sequence(8 字节大端) || block_counter(4
//   字节大端)
//
// 这样**每条记录的计数器空间互不重叠**：block_counter 从 0 开始，最多
// 2^32 - 1 个块（一条记录远达不到），因此不会像"IV = base + seq，然后让
// 128 位 CTR 自己跨块累加"那样，让第 N 条记录的后半段和第 N+1 条记录的
// 开头撞在同一个计数器值上。
std::string CounterBlock(const std::string& nonce_prefix,
                         std::uint64_t sequence) {
  std::string block;
  block.reserve(crypto::kAesBlockSize);
  block.append(nonce_prefix);
  AppendU64(&block, sequence);
  AppendU32(&block, 0);
  return block;
}

std::string LowerHex(const std::string& text) {
  std::string out = text;
  for (char& ch : out) {
    if (ch >= 'A' && ch <= 'F') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  return out;
}

bool IsHex64(const std::string& text) {
  if (text.size() != kSha256Bytes * 2) {
    return false;
  }
  for (const char ch : text) {
    const bool digit = ch >= '0' && ch <= '9';
    const bool lower = ch >= 'a' && ch <= 'f';
    const bool upper = ch >= 'A' && ch <= 'F';
    if (!digit && !lower && !upper) {
      return false;
    }
  }
  return true;
}

// ---- 握手消息的构造与校验 ----

std::string BuildClientHello(const std::string& client_random,
                             const std::string& ephemeral_public,
                             std::uint8_t version) {
  std::string out;
  out.reserve(kBssec1ClientHelloSize);
  AppendU32(&out, kBssec1Magic);
  out.push_back(static_cast<char>(kBssec1MessageClientHello));
  out.push_back(static_cast<char>(version));
  AppendU16(&out, kBssec1SuiteX25519Aes256CtrHmacSha256);
  out.append(client_random);
  out.append(ephemeral_public);
  return out;
}

std::string BuildServerHello(const std::string& server_random,
                             const std::string& static_public,
                             const std::string& ephemeral_public,
                             std::uint8_t version) {
  std::string out;
  out.reserve(kBssec1ServerHelloSize);
  AppendU32(&out, kBssec1Magic);
  out.push_back(static_cast<char>(kBssec1MessageServerHello));
  out.push_back(static_cast<char>(version));
  AppendU16(&out, kBssec1SuiteX25519Aes256CtrHmacSha256);
  out.append(server_random);
  out.append(static_public);
  out.append(ephemeral_public);
  return out;
}

std::string BuildFinished(std::uint8_t type, const std::string& tag,
                          std::uint8_t version) {
  std::string out;
  out.reserve(kBssec1FinishedSize);
  AppendU32(&out, kBssec1Magic);
  out.push_back(static_cast<char>(type));
  out.push_back(static_cast<char>(version));
  AppendU16(&out, 0);
  out.append(tag);
  return out;
}

// BPSEC2：ServerCertificate 消息 = 12 字节定长头 + 证书字节。
// 头里的长度是**唯一**的长度来源，证书本身（BPCERT1）内部还有自己的长度
// 前缀；解析器会把两者都校验一遍（多一层长度就必须多一层怀疑）。
std::string BuildServerCertificateMessage(const std::string& certificate) {
  std::string out;
  out.reserve(kBssec2CertificateHeaderSize + certificate.size());
  AppendU32(&out, kBssec1Magic);
  out.push_back(static_cast<char>(kBssec2MessageServerCertificate));
  out.push_back(static_cast<char>(kBssec2Version));
  AppendU16(&out, 0);
  AppendU32(&out, static_cast<std::uint32_t>(certificate.size()));
  out.append(certificate);
  return out;
}

// 校验一条固定长度握手消息的公共前缀。失败时写 error_message 并分类。
bool CheckMessageHeader(const unsigned char* raw, std::size_t size,
                        std::uint8_t expected_type, std::size_t expected_size,
                        std::uint8_t expected_version, const char* what,
                        SecureTransportError* error,
                        std::string* error_message) {
  if (size != expected_size) {
    *error = SecureTransportError::kMalformedMessage;
    if (error_message != nullptr) {
      *error_message = std::string(what) + " 长度不是 " +
                       std::to_string(expected_size) + " 字节";
    }
    return false;
  }
  if (LoadU32(raw) != kBssec1Magic) {
    *error = SecureTransportError::kMalformedMessage;
    if (error_message != nullptr) {
      *error_message = std::string(what) +
                       " 的 magic 不是 BPS1（对端不是 BPSEC1，"
                       "或链路上有东西在改写字节）";
    }
    return false;
  }
  if (raw[4] != expected_type) {
    *error = SecureTransportError::kMalformedMessage;
    if (error_message != nullptr) {
      *error_message = std::string(what) + " 的类型字段不对";
    }
    return false;
  }
  if (raw[5] != expected_version) {
    *error = SecureTransportError::kUnsupportedVersion;
    if (error_message != nullptr) {
      *error_message = std::string(what) + " 的协议版本不被支持（期望 " +
                       std::to_string(expected_version) + "，实际 " +
                       std::to_string(raw[5]) + "）";
    }
    return false;
  }
  // 第 6、7 字节在 ClientHello/ServerHello 里是密码套件、在 Finished 里是
  // 保留位，含义不同，由调用方各自的校验负责。
  return true;
}

// ---- 会话密钥 ----

// 尽力清零（best-effort）：用 volatile 写，避免被优化掉；但**不声称**获得了
// 形式化的内存保密性——C++ 里无法保证编译器/运行时不留下副本。
void Zeroize(std::string* secret);

struct SessionKeys {
  std::string c2s_enc;
  std::string c2s_mac;
  std::string c2s_nonce;
  std::string s2c_enc;
  std::string s2c_mac;
  std::string s2c_nonce;
  std::string client_finished;
  std::string server_finished;
};

// HKDF-Extract(salt = client_random || server_random, IKM = DH_static ||
// DH_ephemeral) 之后按 8 个互不相同的 info 标签各 Expand 一次：方向分离 +
// 用途分离。
bool DeriveSessionKeys(const std::string& shared_secret,
                       const std::string& client_random,
                       const std::string& server_random, SessionKeys* keys,
                       std::string* error_message) {
  const std::string salt = client_random + server_random;
  std::string prk;
  if (!HkdfExtract(salt, shared_secret, &prk, error_message)) {
    return false;
  }
  struct Request {
    const char* info;
    std::size_t length;
    std::string* out;
  };
  const Request requests[] = {
      {kBssec1InfoClientToServerEnc, kX25519Bytes, &keys->c2s_enc},
      {kBssec1InfoClientToServerMac, kX25519Bytes, &keys->c2s_mac},
      {kBssec1InfoClientToServerNonce, 4, &keys->c2s_nonce},
      {kBssec1InfoServerToClientEnc, kX25519Bytes, &keys->s2c_enc},
      {kBssec1InfoServerToClientMac, kX25519Bytes, &keys->s2c_mac},
      {kBssec1InfoServerToClientNonce, 4, &keys->s2c_nonce},
      {kBssec1InfoClientFinished, kSha256Bytes, &keys->client_finished},
      {kBssec1InfoServerFinished, kSha256Bytes, &keys->server_finished},
  };
  for (const Request& request : requests) {
    if (!HkdfExpand(prk, std::string(request.info), request.length, request.out,
                    error_message)) {
      Zeroize(&prk);
      return false;
    }
  }
  // PRK 是所有会话密钥的母体：派生完立刻清掉本地副本（best-effort）。
  Zeroize(&prk);
  return true;
}

void Zeroize(std::string* secret) {
  if (secret == nullptr || secret->empty()) {
    return;
  }
  volatile char* cursor = &(*secret)[0];
  for (std::size_t i = 0; i < secret->size(); ++i) {
    cursor[i] = 0;
  }
  secret->clear();
}

}  // namespace

const char* SecureTransportErrorName(SecureTransportError error) {
  switch (error) {
    case SecureTransportError::kNone:
      return "none";
    case SecureTransportError::kIoError:
      return "io-error";
    case SecureTransportError::kMalformedMessage:
      return "malformed-message";
    case SecureTransportError::kUnsupportedVersion:
      return "unsupported-version";
    case SecureTransportError::kServerKeyMismatch:
      return "server-key-mismatch";
    case SecureTransportError::kAuthenticationFailed:
      return "handshake-authentication-failed";
    case SecureTransportError::kWeakSharedSecret:
      return "weak-shared-secret";
    case SecureTransportError::kNoPinConfigured:
      return "no-server-key-pin";
    case SecureTransportError::kCertificateMissing:
      return "server-certificate-missing";
    case SecureTransportError::kCertificateInvalid:
      return "server-certificate-invalid";
    case SecureTransportError::kCertificateUntrusted:
      return "server-certificate-untrusted";
    case SecureTransportError::kCertificateWrongServerId:
      return "server-certificate-wrong-server-id";
    case SecureTransportError::kCertificateExpired:
      return "server-certificate-expired";
    case SecureTransportError::kCertificateKeyMismatch:
      return "server-certificate-key-mismatch";
    case SecureTransportError::kDowngradeRefused:
      return "bpsec1-downgrade-refused";
    case SecureTransportError::kRecordAuthentication:
      return "record-authentication-failed";
    case SecureTransportError::kReplayDetected:
      return "record-replay-detected";
    case SecureTransportError::kOversizedRecord:
      return "oversized-record";
    case SecureTransportError::kStateError:
      return "channel-state-error";
    case SecureTransportError::kCryptoFailure:
      return "crypto-failure";
  }
  return "unknown";
}

std::string SecureTransportErrorMessage(SecureTransportError error) {
  switch (error) {
    case SecureTransportError::kNone:
      return "成功";
    case SecureTransportError::kIoError:
      return "加密传输层读写失败（连接被关闭或超时）";
    case SecureTransportError::kMalformedMessage:
      return "BPSEC1 握手消息格式不合法";
    case SecureTransportError::kUnsupportedVersion:
      return "BPSEC1 版本或密码套件不被支持";
    case SecureTransportError::kServerKeyMismatch:
      return "服务端传输身份公钥与本地 pin "
             "不一致（可能存在中间人，或服务端换过密钥）";
    case SecureTransportError::kAuthenticationFailed:
      return "BPSEC1 握手校验失败：握手的字节被改过，或双方密钥不一致";
    case SecureTransportError::kWeakSharedSecret:
      return "X25519 共享秘密退化（对端给了低阶点），拒绝建立会话";
    case SecureTransportError::kNoPinConfigured:
      return "没有配置服务端传输公钥/"
             "指纹，拒绝连接（本项目不做首次连接自动信任）";
    case SecureTransportError::kCertificateMissing:
      return "服务器没有配置身份证书，无法使用签名身份连接";
    case SecureTransportError::kCertificateInvalid:
      return "服务器身份证书无效：这张证书不是它声称的根签发的";
    case SecureTransportError::kCertificateUntrusted:
      return "签发这张服务器身份证书的根不在本机可信列表里";
    case SecureTransportError::kCertificateWrongServerId:
      return "服务器身份证书里的服务器名字不是这个云端";
    case SecureTransportError::kCertificateExpired:
      return "服务器身份证书不在有效期内（若本机时间不对，请先校正系统时钟）";
    case SecureTransportError::kCertificateKeyMismatch:
      return "服务器出示的证书与实际使用的身份密钥不是同一把";
    case SecureTransportError::kDowngradeRefused:
      return "服务器要求使用签名身份，对端却试图退回旧协议，已拒绝";
    case SecureTransportError::kRecordAuthentication:
      return "加密记录校验失败：数据在传输途中被修改过";
    case SecureTransportError::kReplayDetected:
      return "加密记录序号不连续：检测到重放、乱序或丢包";
    case SecureTransportError::kOversizedRecord:
      return "加密记录长度超过上限";
    case SecureTransportError::kStateError:
      return "加密通道状态错误（未握手就使用，或记录序号耗尽）";
    case SecureTransportError::kCryptoFailure:
      return "本地密码学原语失败";
  }
  return "未知的加密传输错误";
}

bool ParseServerKeyPin(const std::string& text, ServerKeyPin* out,
                       std::string* error_message) {
  if (out == nullptr) {
    if (error_message != nullptr) {
      *error_message = "解析服务端 pin 时输出指针为空";
    }
    return false;
  }
  *out = ServerKeyPin();
  if (text.compare(0, 7, "sha256:") == 0) {
    const std::string body = text.substr(7);
    if (!IsHex64(body)) {
      if (error_message != nullptr) {
        *error_message =
            "sha256: 前缀后面必须是 64 个十六进制字符（SHA-256 指纹）";
      }
      return false;
    }
    out->fingerprint_hex = LowerHex(body);
    return true;
  }
  if (text.compare(0, 4, "hex:") == 0) {
    std::string key;
    std::string parse_error;
    if (!crypto::X25519ParseKeyText(text.substr(4), &key, &parse_error)) {
      if (error_message != nullptr) {
        *error_message = "hex: 前缀后面的公钥不合法：" + parse_error;
      }
      return false;
    }
    out->has_key = true;
    out->public_key = key;
    out->fingerprint_hex = crypto::X25519Fingerprint(key);
    return true;
  }
  if (error_message != nullptr) {
    *error_message =
        "服务端 pin 必须写成 \"sha256:<64 个十六进制字符>\" 或 "
        "\"hex:<64 个十六进制字符>\"；不接受不带前缀的裸十六进制，"
        "因为公钥和指纹的长度相同，混起来会把指纹当成公钥用";
  }
  return false;
}

bool GenerateTransportIdentity(TransportIdentity* out,
                               std::string* error_message) {
  if (out == nullptr) {
    if (error_message != nullptr) {
      *error_message = "生成传输身份密钥时输出指针为空";
    }
    return false;
  }
  return X25519GenerateKeyPair(&out->private_key, &out->public_key,
                               error_message);
}

bool SaveTransportIdentity(const std::string& path,
                           const TransportIdentity& identity, bool overwrite,
                           std::string* error_message) {
  if (identity.private_key.size() != kX25519Bytes) {
    if (error_message != nullptr) {
      *error_message = "传输身份私钥长度不是 32 字节，拒绝写盘";
    }
    return false;
  }
  // O_NONBLOCK 的理由与 LoadTransportIdentity 相同：FIFO 上 open(O_WRONLY)
  // 会等读者，先阻塞在 open 上就永远走不到 S_ISREG 检查（审查轮缺陷 B）。
  const int flags = O_WRONLY | O_CREAT | O_NOFOLLOW | O_NONBLOCK |
                    (overwrite ? O_TRUNC : O_EXCL);
  const int fd = ::open(path.c_str(), flags, 0600);
  if (fd < 0) {
    if (error_message != nullptr) {
      *error_message = "无法创建 " + path + "：" + StrerrorText();
    }
    return false;
  }
  struct stat info;
  if (::fstat(fd, &info) != 0) {
    const std::string reason = StrerrorText();
    ::close(fd);
    if (error_message != nullptr) {
      *error_message = "无法检查 " + path + "：" + reason;
    }
    return false;
  }
  if (!S_ISREG(info.st_mode)) {
    ::close(fd);
    if (error_message != nullptr) {
      *error_message = path + " 不是普通文件，拒绝把私钥写进去";
    }
    return false;
  }
  // 确认是普通文件之后，把探测用的 O_NONBLOCK 清掉，再按正常语义写。
  const int save_probe_flags = ::fcntl(fd, F_GETFL);
  if (save_probe_flags < 0 ||
      ::fcntl(fd, F_SETFL, save_probe_flags & ~O_NONBLOCK) != 0) {
    const std::string reason = StrerrorText();
    ::close(fd);
    if (error_message != nullptr) {
      *error_message = "无法恢复 " + path + " 的阻塞语义：" + reason;
    }
    return false;
  }
  // 文件权限必须是"只有所有者可读写"。O_CREAT 的 mode 会被 umask 削，
  // 所以这里显式 fchmod 一次。
  if (::fchmod(fd, 0600) != 0) {
    const std::string reason = StrerrorText();
    ::close(fd);
    if (error_message != nullptr) {
      *error_message = "无法把 " + path + " 设成 0600：" + reason;
    }
    return false;
  }
  std::size_t written = 0;
  while (written < kX25519Bytes) {
    const ssize_t count = ::write(fd, identity.private_key.data() + written,
                                  kX25519Bytes - written);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      const std::string reason = StrerrorText();
      ::close(fd);
      if (error_message != nullptr) {
        *error_message = "写 " + path + " 失败：" + reason;
      }
      return false;
    }
    written += static_cast<std::size_t>(count);
  }
  if (::fsync(fd) != 0) {
    const std::string reason = StrerrorText();
    ::close(fd);
    if (error_message != nullptr) {
      *error_message = "fsync " + path + " 失败：" + reason;
    }
    return false;
  }
  if (::close(fd) != 0) {
    if (error_message != nullptr) {
      *error_message = "关闭 " + path + " 失败：" + StrerrorText();
    }
    return false;
  }
  return true;
}

bool LoadTransportIdentity(const std::string& path, TransportIdentity* out,
                           std::string* error_message) {
  if (out == nullptr) {
    if (error_message != nullptr) {
      *error_message = "读取传输身份密钥时输出指针为空";
    }
    return false;
  }
  // O_NONBLOCK 先上：FIFO 上 open(O_RDONLY) 会等写者，S_ISREG 检查根本没机会
  // 跑（审查轮缺陷 B：state/transport.key 被换成 FIFO 时服务端启动就卡死）。
  // 普通文件上这个标志不影响读写，校验通过后立刻清掉。
  const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) {
    if (error_message != nullptr) {
      *error_message = "无法打开传输身份密钥 " + path + "：" + StrerrorText();
    }
    return false;
  }
  struct stat info;
  if (::fstat(fd, &info) != 0) {
    const std::string reason = StrerrorText();
    ::close(fd);
    if (error_message != nullptr) {
      *error_message = "无法检查 " + path + "：" + reason;
    }
    return false;
  }
  if (!S_ISREG(info.st_mode)) {
    ::close(fd);
    if (error_message != nullptr) {
      *error_message = path + " 不是普通文件（私钥文件不允许是软链接或设备）";
    }
    return false;
  }
  // 到这里已经确认是普通文件，把探测用的 O_NONBLOCK 清掉。
  const int probe_flags = ::fcntl(fd, F_GETFL);
  if (probe_flags < 0 || ::fcntl(fd, F_SETFL, probe_flags & ~O_NONBLOCK) != 0) {
    const std::string reason = StrerrorText();
    ::close(fd);
    if (error_message != nullptr) {
      *error_message = "无法恢复 " + path + " 的阻塞语义：" + reason;
    }
    return false;
  }
  if ((info.st_mode & 077) != 0) {
    ::close(fd);
    if (error_message != nullptr) {
      *error_message = path +
                       " 的权限过于宽松（组或其他用户可访问）；私钥文件必须是 "
                       "0600";
    }
    return false;
  }
  std::string material(kX25519Bytes, '\0');
  std::size_t read_bytes = 0;
  while (read_bytes < kX25519Bytes) {
    const ssize_t count =
        ::read(fd, &material[read_bytes], kX25519Bytes - read_bytes);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      const std::string reason = StrerrorText();
      ::close(fd);
      if (error_message != nullptr) {
        *error_message = "读 " + path + " 失败：" + reason;
      }
      return false;
    }
    if (count == 0) {
      ::close(fd);
      if (error_message != nullptr) {
        *error_message = path + " 长度不足 32 字节，不是合法的传输身份私钥";
      }
      return false;
    }
    read_bytes += static_cast<std::size_t>(count);
  }
  // 尾部这一个字节决定"文件是不是正好 32 字节"。EINTR 只重试：把它当成
  // "没有多余字节"就等于让一次信号打断来放宽 exact-32 不变式（一个比 32
  // 字节长的文件会被当成合法私钥收下）。
  char extra = 0;
  ssize_t tail = 0;
  for (;;) {
    tail = ::read(fd, &extra, 1);
    if (tail < 0 && errno == EINTR) {
      continue;
    }
    break;
  }
  if (tail < 0) {
    const std::string reason = StrerrorText();
    ::close(fd);
    if (error_message != nullptr) {
      *error_message = "读 " + path + " 失败：" + reason;
    }
    return false;
  }
  if (tail == 1) {
    ::close(fd);
    if (error_message != nullptr) {
      *error_message = path + " 长度超过 32 字节，不是合法的传输身份私钥";
    }
    return false;
  }
  ::close(fd);

  std::string public_key;
  std::string derive_error;
  if (!crypto::X25519PublicKeyFromPrivate(material, &public_key,
                                          &derive_error)) {
    Zeroize(&material);
    if (error_message != nullptr) {
      *error_message = "由 " + path + " 推导公钥失败：" + derive_error;
    }
    return false;
  }
  out->private_key = material;
  Zeroize(&material);
  out->public_key = public_key;
  return true;
}

SecureChannel::SecureChannel() = default;

SecureChannel::~SecureChannel() { ClearSecrets(); }

void SecureChannel::ClearSecrets() {
  Zeroize(&send_key_);
  Zeroize(&send_mac_key_);
  Zeroize(&send_nonce_prefix_);
  Zeroize(&receive_key_);
  Zeroize(&receive_mac_key_);
  Zeroize(&receive_nonce_prefix_);
  Zeroize(&client_finished_key_);
  Zeroize(&server_finished_key_);
  send_direction_ = 0;
  receive_direction_ = 0;
  established_ = false;
  send_sequence_ = 0;
  receive_sequence_ = 0;
}

void SecureChannel::Reset() {
  ClearSecrets();
  last_error_ = SecureTransportError::kNone;
  corrupt_next_tag_ = false;
  peer_public_key_.clear();
  peer_fingerprint_.clear();
  transcript_hash_.clear();
}

bool SecureChannel::Fail(SecureTransportError error, const std::string& detail,
                         std::string* error_message) {
  last_error_ = error;
  // 失败之后这条通道**永远**不可再用。规则放在这里，而不是散在十几个返回点：
  // 变异测试抓到过一个真实缺口——服务端在"读 ClientFinished 失败"时返回了
  // false，但 DeriveKeys 已经把手上的 established_ 置成了 true。调用方即使
  // 误判，也不该把明文交给一条没有完成认证的连接。
  established_ = false;
  if (error_message != nullptr) {
    std::string text = SecureTransportErrorMessage(error);
    if (!detail.empty()) {
      text += "：" + detail;
    }
    *error_message = text;
  }
  return false;
}

bool SecureChannel::DeriveKeys(const std::string& shared_secret,
                               const std::string& client_random,
                               const std::string& server_random, bool is_client,
                               std::string* error_message) {
  SessionKeys keys;
  if (!DeriveSessionKeys(shared_secret, client_random, server_random, &keys,
                         error_message)) {
    return Fail(SecureTransportError::kCryptoFailure, "会话密钥派生失败",
                error_message);
  }
  if (is_client) {
    send_key_ = keys.c2s_enc;
    send_mac_key_ = keys.c2s_mac;
    send_nonce_prefix_ = keys.c2s_nonce;
    send_direction_ = kBssec1DirectionClientToServer;
    receive_key_ = keys.s2c_enc;
    receive_mac_key_ = keys.s2c_mac;
    receive_nonce_prefix_ = keys.s2c_nonce;
    receive_direction_ = kBssec1DirectionServerToClient;
  } else {
    send_key_ = keys.s2c_enc;
    send_mac_key_ = keys.s2c_mac;
    send_nonce_prefix_ = keys.s2c_nonce;
    send_direction_ = kBssec1DirectionServerToClient;
    receive_key_ = keys.c2s_enc;
    receive_mac_key_ = keys.c2s_mac;
    receive_nonce_prefix_ = keys.c2s_nonce;
    receive_direction_ = kBssec1DirectionClientToServer;
  }
  client_finished_key_ = keys.client_finished;
  server_finished_key_ = keys.server_finished;
  // 本地副本用完立刻清掉：密钥已经复制进成员，这里不留第二份。
  Zeroize(&keys.c2s_enc);
  Zeroize(&keys.c2s_mac);
  Zeroize(&keys.c2s_nonce);
  Zeroize(&keys.s2c_enc);
  Zeroize(&keys.s2c_mac);
  Zeroize(&keys.s2c_nonce);
  Zeroize(&keys.client_finished);
  Zeroize(&keys.server_finished);
  send_sequence_ = 0;
  receive_sequence_ = 0;
  established_ = true;
  return true;
}

bool SecureChannel::HandshakeClient(int fd, const ServerKeyPin& pin,
                                    std::string* error_message) {
  return HandshakeClientInternal(fd, &pin, nullptr, error_message);
}

bool SecureChannel::HandshakeClientWithCertificate(
    int fd, const ServerIdentityPolicy& policy, std::string* error_message) {
  return HandshakeClientInternal(fd, nullptr, &policy, error_message);
}

bool SecureChannel::HandshakeClientInternal(int fd, const ServerKeyPin* pin,
                                            const ServerIdentityPolicy* policy,
                                            std::string* error_message) {
  Reset();
  const bool certificate_mode = policy != nullptr;
  // 两条路互斥：证书模式完全不看 pin，pin 模式完全不看证书。
  // 这里**没有**"证书验不过就退回 pin"的分支 —— 那种分支就是降级漏洞。
  const std::uint8_t version =
      certificate_mode ? kBssec2Version : kBssec1Version;
  if (certificate_mode) {
    if (policy->expected_server_id.empty()) {
      return Fail(SecureTransportError::kCertificateWrongServerId,
                  "证书模式必须先配置期望的 server_id", error_message);
    }
    if (policy->roots.empty()) {
      return Fail(SecureTransportError::kCertificateUntrusted,
                  "本机没有任何可信根，拒绝一切服务器身份", error_message);
    }
  } else if (!pin->has_key && pin->fingerprint_hex.empty()) {
    return Fail(SecureTransportError::kNoPinConfigured, std::string(),
                error_message);
  }
  // 整体预算（0 = 不设限）。客户端也要有：一个持有正确身份私钥、却慢慢滴水的
  // 对端同样能把客户端挂住。
  const std::int64_t handshake_deadline =
      handshake_timeout_ms_ == 0
          ? 0
          : MonotonicMillis() +
                static_cast<std::int64_t>(handshake_timeout_ms_);

  std::string client_random;
  std::string ephemeral_private;
  std::string ephemeral_public;
  if (!crypto::RandomBytes(kSha256Bytes, &client_random, error_message) ||
      !X25519GenerateKeyPair(&ephemeral_private, &ephemeral_public,
                             error_message)) {
    return Fail(SecureTransportError::kCryptoFailure,
                "生成握手随机数或临时密钥失败", error_message);
  }

  const std::string hello =
      BuildClientHello(client_random, ephemeral_public, version);
  std::string io_error;
  if (!SendAll(fd, hello.data(), hello.size(), &io_error)) {
    return Fail(SecureTransportError::kIoError,
                "发送 ClientHello 失败：" + io_error, error_message);
  }

  unsigned char raw[kBssec1ServerHelloSize];
  bool closed = false;
  if (!ReceiveAll(fd, raw, sizeof(raw), &closed, &io_error,
                  handshake_deadline)) {
    return Fail(SecureTransportError::kIoError,
                closed ? "对端在 ServerHello 之前关闭了连接"
                       : "读取 ServerHello 失败：" + io_error,
                error_message);
  }
  SecureTransportError header_error = SecureTransportError::kNone;
  if (!CheckMessageHeader(raw, sizeof(raw), kBssec1MessageServerHello,
                          kBssec1ServerHelloSize, version, "ServerHello",
                          &header_error, error_message)) {
    last_error_ = header_error;
    return false;
  }
  if (((static_cast<std::uint16_t>(raw[6]) << 8) | raw[7]) !=
      kBssec1SuiteX25519Aes256CtrHmacSha256) {
    return Fail(SecureTransportError::kUnsupportedVersion,
                "服务端选择的密码套件不被支持", error_message);
  }

  const std::string server_random(reinterpret_cast<const char*>(raw + 8),
                                  kSha256Bytes);
  const std::string server_static(reinterpret_cast<const char*>(raw + 40),
                                  kX25519Bytes);
  const std::string server_ephemeral(reinterpret_cast<const char*>(raw + 72),
                                     kX25519Bytes);

  peer_public_key_ = server_static;
  peer_fingerprint_ = crypto::X25519Fingerprint(server_static);

  // BPSEC2：整张 ServerCertificate 消息（含 12 字节头）的原始字节要进
  // transcript；BPSEC1 时它是空串，因此 transcript 与过去逐字节相同。
  std::string certificate_message;
  if (certificate_mode) {
    unsigned char cert_header[kBssec2CertificateHeaderSize];
    if (!ReceiveAll(fd, cert_header, sizeof(cert_header), &closed, &io_error,
                    handshake_deadline)) {
      return Fail(SecureTransportError::kIoError,
                  closed ? "对端在 ServerCertificate 之前关闭了连接"
                         : "读取 ServerCertificate 失败：" + io_error,
                  error_message);
    }
    if (LoadU32(cert_header) != kBssec1Magic ||
        cert_header[4] != kBssec2MessageServerCertificate ||
        cert_header[5] != kBssec2Version || cert_header[6] != 0 ||
        cert_header[7] != 0) {
      return Fail(SecureTransportError::kMalformedMessage,
                  "ServerCertificate 消息头不合法", error_message);
    }
    const std::uint32_t certificate_length = LoadU32(cert_header + 8);
    if (certificate_length == 0 ||
        certificate_length > crypto::kBpcert1MaxCertificateSize) {
      return Fail(SecureTransportError::kCertificateInvalid,
                  "证书长度超出 1..4096 的范围", error_message);
    }
    std::string certificate_raw(certificate_length, '\0');
    if (!ReceiveAll(fd, &certificate_raw[0], certificate_raw.size(), &closed,
                    &io_error, handshake_deadline)) {
      return Fail(SecureTransportError::kIoError,
                  closed ? "对端在证书传输过程中关闭了连接"
                         : "读取证书字节失败：" + io_error,
                  error_message);
    }
    certificate_message.assign(
        reinterpret_cast<const char*>(cert_header), sizeof(cert_header));
    certificate_message.append(certificate_raw);

    // ---- 校验顺序：任一步失败即终止，不回退 pin、不回退 BPSEC1 ----
    // 1) 结构（含版本、算法、用途、长度、尾部字节）
    crypto::Bpcert1 certificate;
    const crypto::Bpcert1Error parse_result =
        crypto::Bpcert1Parse(certificate_raw, &certificate);
    if (parse_result != crypto::Bpcert1Error::kOk) {
      return Fail(SecureTransportError::kCertificateInvalid,
                  std::string("服务器身份证书结构不合法：") +
                      crypto::Bpcert1ErrorName(parse_result),
                  error_message);
    }
    // 2) 在可信根里按 issuer_id 找根（空存储在入口已经挡掉）
    const crypto::TrustedRoot* root =
        policy->roots.FindRoot(certificate.issuer_id);
    if (root == nullptr) {
      return Fail(SecureTransportError::kCertificateUntrusted,
                  "证书的签发者 " + certificate.issuer_id +
                      " 不在本机可信根列表里",
                  error_message);
    }
    if (root->revoked) {
      return Fail(SecureTransportError::kCertificateUntrusted,
                  "签发这张证书的根已被吊销", error_message);
    }
    if (root->not_before != 0 && certificate.not_before < root->not_before) {
      return Fail(SecureTransportError::kCertificateUntrusted,
                  "证书签发时该根尚未生效", error_message);
    }
    if (root->not_after != 0 && certificate.not_before > root->not_after) {
      return Fail(SecureTransportError::kCertificateUntrusted,
                  "证书签发时该根已过期", error_message);
    }
    // 3) 根签名
    const crypto::Bpcert1Error signature_result =
        crypto::Bpcert1VerifySignature(certificate_raw, root->public_key);
    if (signature_result != crypto::Bpcert1Error::kOk) {
      return Fail(SecureTransportError::kCertificateInvalid,
                  std::string("证书签名无效：") +
                      crypto::Bpcert1ErrorName(signature_result),
                  error_message);
    }
    // 4) server_id 必须与期望值逐字节相等：挡住"同一把根签发的另一台服务器"
    if (certificate.server_id != policy->expected_server_id) {
      return Fail(SecureTransportError::kCertificateWrongServerId,
                  "证书里的 server_id 是 " + certificate.server_id +
                      "，与期望的 " + policy->expected_server_id + " 不一致",
                  error_message);
    }
    // 5) 有效期（key_usage = SERVER_AUTH 已经由解析器强制校验）
    const std::int64_t now = policy->now_unix_seconds != 0
                                 ? policy->now_unix_seconds
                                 : crypto::Bpcert1NowUnixSeconds();
    crypto::Bpcert1Error validity_error = crypto::Bpcert1Error::kOk;
    std::string validity_message;
    if (!crypto::Bpcert1CheckValidity(certificate, now, &validity_error,
                                      &validity_message)) {
      return Fail(SecureTransportError::kCertificateExpired, validity_message,
                  error_message);
    }
    // 6) 证书认证的公钥必须**就是**握手里实际使用的身份公钥。
    //    少了这一步，攻击者可以拿一张真证书配一把自己的临时密钥。
    if (!ConstantTimeEquals(certificate.server_public_key, server_static)) {
      return Fail(SecureTransportError::kCertificateKeyMismatch,
                  "证书认证的公钥与握手里实际使用的身份公钥不一致",
                  error_message);
    }
    peer_server_id_ = certificate.server_id;
    peer_certificate_fingerprint_ = crypto::Bpcert1Fingerprint(certificate_raw);
  } else if (pin->has_key) {
    // 服务端身份校验：pin 说什么就只接受什么。这里**没有** TOFU 分支。
    if (!ConstantTimeEquals(pin->public_key, server_static)) {
      return Fail(SecureTransportError::kServerKeyMismatch,
                  "ServerHello 里的身份公钥与本地 pin 的公钥不同",
                  error_message);
    }
  }
  if (!certificate_mode &&
      LowerHex(pin->fingerprint_hex) != peer_fingerprint_) {
    return Fail(SecureTransportError::kServerKeyMismatch,
                "服务端身份公钥的指纹是 " + peer_fingerprint_ +
                    "，与本地 pin 的指纹不一致",
                error_message);
  }

  std::string dh_static;
  std::string dh_ephemeral;
  std::string dh_error;
  if (!X25519SharedSecret(ephemeral_private, server_static, &dh_static,
                          &dh_error) ||
      !X25519SharedSecret(ephemeral_private, server_ephemeral, &dh_ephemeral,
                          &dh_error)) {
    return Fail(SecureTransportError::kWeakSharedSecret, dh_error,
                error_message);
  }
  Zeroize(&ephemeral_private);

  std::string shared_secret = dh_static + dh_ephemeral;
  Zeroize(&dh_static);
  Zeroize(&dh_ephemeral);
  if (!DeriveKeys(shared_secret, client_random, server_random, true,
                  error_message)) {
    Zeroize(&shared_secret);
    return false;
  }
  Zeroize(&shared_secret);

  const std::string server_hello(reinterpret_cast<const char*>(raw),
                                 sizeof(raw));
  transcript_hash_ = Sha256Of(hello + server_hello + certificate_message);

  const std::string client_finished =
      HmacTag(client_finished_key_, transcript_hash_);
  const std::string finished_message = BuildFinished(
      kBssec1MessageClientFinished, client_finished, version);
  if (!SendAll(fd, finished_message.data(), finished_message.size(),
               &io_error)) {
    return Fail(SecureTransportError::kIoError,
                "发送 ClientFinished 失败：" + io_error, error_message);
  }

  unsigned char server_finished_raw[kBssec1FinishedSize];
  if (!ReceiveAll(fd, server_finished_raw, sizeof(server_finished_raw), &closed,
                  &io_error, handshake_deadline)) {
    return Fail(SecureTransportError::kIoError,
                closed ? "对端在 ServerFinished 之前关闭了连接"
                       : "读取 ServerFinished 失败：" + io_error,
                error_message);
  }
  if (LoadU32(server_finished_raw) != kBssec1Magic ||
      server_finished_raw[4] != kBssec1MessageServerFinished ||
      server_finished_raw[5] != version || server_finished_raw[6] != 0 ||
      server_finished_raw[7] != 0) {
    return Fail(SecureTransportError::kMalformedMessage,
                "ServerFinished 消息格式不合法", error_message);
  }
  const std::string expected_server_finished =
      HmacTag(server_finished_key_, transcript_hash_ + client_finished);
  const std::string got_server_finished(
      reinterpret_cast<const char*>(server_finished_raw + 8), kSha256Bytes);
  if (!ConstantTimeEquals(expected_server_finished, got_server_finished)) {
    established_ = false;
    return Fail(SecureTransportError::kAuthenticationFailed,
                "ServerFinished 校验失败（对端不持有会话密钥，或握手被改写）",
                error_message);
  }
  // 握手完成：Finished 密钥不再需要，立刻清掉（记录层用的是另外两把）。
  Zeroize(&client_finished_key_);
  Zeroize(&server_finished_key_);
  return true;
}

bool SecureChannel::HandshakeServer(int fd, const TransportIdentity& identity,
                                    std::string* error_message) {
  return HandshakeServerInternal(fd, identity, false, error_message);
}

bool SecureChannel::HandshakeServerRequireCertificate(
    int fd, const TransportIdentity& identity, std::string* error_message) {
  return HandshakeServerInternal(fd, identity, true, error_message);
}

bool SecureChannel::HandshakeServerInternal(int fd,
                                            const TransportIdentity& identity,
                                            bool require_certificate,
                                            std::string* error_message) {
  Reset();
  if (identity.private_key.size() != kX25519Bytes ||
      identity.public_key.size() != kX25519Bytes) {
    return Fail(SecureTransportError::kStateError,
                "服务端传输身份密钥没有配置好", error_message);
  }
  if (require_certificate && identity.certificate.empty()) {
    return Fail(SecureTransportError::kCertificateMissing,
                "本服务端被配置为只接受 BPSEC2，但没有加载身份证书",
                error_message);
  }
  // 整体预算（0 = 不设限）。服务端尤其需要：握手是**未认证**阶段，慢速滴水的
  // 对端本来可以靠 SO_RCVTIMEO 只约束单次 recv 这点占住一个 worker。
  const std::int64_t handshake_deadline =
      handshake_timeout_ms_ == 0
          ? 0
          : MonotonicMillis() +
                static_cast<std::int64_t>(handshake_timeout_ms_);

  unsigned char raw[kBssec1ClientHelloSize];
  bool closed = false;
  std::string io_error;
  if (!ReceiveAll(fd, raw, sizeof(raw), &closed, &io_error,
                  handshake_deadline)) {
    return Fail(SecureTransportError::kIoError,
                closed ? "客户端在 ClientHello 之前关闭了连接"
                       : "读取 ClientHello 失败：" + io_error,
                error_message);
  }
  // 版本判别：客户端说 BPSEC2 就走证书握手；说 BPSEC1 时，若本服务端被
  // 配置为只接受证书身份，就直接拒绝 —— 这就是"拒绝降级"的落点。
  std::uint8_t version = kBssec1Version;
  bool certificate_mode = false;
  if (raw[5] == kBssec2Version) {
    if (identity.certificate.empty()) {
      return Fail(SecureTransportError::kCertificateMissing,
                  "客户端要求 BPSEC2，但本服务端没有配置身份证书",
                  error_message);
    }
    certificate_mode = true;
    version = kBssec2Version;
  } else if (raw[5] == kBssec1Version) {
    if (require_certificate) {
      return Fail(SecureTransportError::kDowngradeRefused,
                  "本服务端只接受 BPSEC2（签名身份），拒绝 BPSEC1 降级握手",
                  error_message);
    }
  } else {
    return Fail(SecureTransportError::kUnsupportedVersion,
                "ClientHello 的协议版本不被支持", error_message);
  }
  SecureTransportError header_error = SecureTransportError::kNone;
  if (!CheckMessageHeader(raw, sizeof(raw), kBssec1MessageClientHello,
                          kBssec1ClientHelloSize, version, "ClientHello",
                          &header_error, error_message)) {
    last_error_ = header_error;
    return false;
  }
  if (((static_cast<std::uint16_t>(raw[6]) << 8) | raw[7]) !=
      kBssec1SuiteX25519Aes256CtrHmacSha256) {
    return Fail(SecureTransportError::kUnsupportedVersion,
                "客户端要求的密码套件不被支持", error_message);
  }

  const std::string client_random(reinterpret_cast<const char*>(raw + 8),
                                  kSha256Bytes);
  const std::string client_ephemeral(reinterpret_cast<const char*>(raw + 40),
                                     kX25519Bytes);

  std::string server_random;
  std::string ephemeral_private;
  std::string ephemeral_public;
  if (!crypto::RandomBytes(kSha256Bytes, &server_random, error_message) ||
      !X25519GenerateKeyPair(&ephemeral_private, &ephemeral_public,
                             error_message)) {
    return Fail(SecureTransportError::kCryptoFailure,
                "生成握手随机数或临时密钥失败", error_message);
  }

  const std::string server_hello = BuildServerHello(
      server_random, identity.public_key, ephemeral_public, version);
  if (!SendAll(fd, server_hello.data(), server_hello.size(), &io_error)) {
    return Fail(SecureTransportError::kIoError,
                "发送 ServerHello 失败：" + io_error, error_message);
  }
  // BPSEC2：紧接着出示身份证书（里面只有公钥材料）。客户端会在派生任何
  // 会话密钥之前把它验完。
  std::string certificate_message;
  if (certificate_mode) {
    certificate_message = BuildServerCertificateMessage(identity.certificate);
    if (!SendAll(fd, certificate_message.data(), certificate_message.size(),
                 &io_error)) {
      return Fail(SecureTransportError::kIoError,
                  "发送 ServerCertificate 失败：" + io_error, error_message);
    }
  }

  std::string dh_static;
  std::string dh_ephemeral;
  std::string dh_error;
  if (!X25519SharedSecret(identity.private_key, client_ephemeral, &dh_static,
                          &dh_error) ||
      !X25519SharedSecret(ephemeral_private, client_ephemeral, &dh_ephemeral,
                          &dh_error)) {
    return Fail(SecureTransportError::kWeakSharedSecret, dh_error,
                error_message);
  }
  Zeroize(&ephemeral_private);

  std::string shared_secret = dh_static + dh_ephemeral;
  Zeroize(&dh_static);
  Zeroize(&dh_ephemeral);
  if (!DeriveKeys(shared_secret, client_random, server_random, false,
                  error_message)) {
    Zeroize(&shared_secret);
    return false;
  }
  Zeroize(&shared_secret);

  const std::string client_hello(reinterpret_cast<const char*>(raw),
                                 sizeof(raw));
  transcript_hash_ = Sha256Of(client_hello + server_hello + certificate_message);

  unsigned char client_finished_raw[kBssec1FinishedSize];
  if (!ReceiveAll(fd, client_finished_raw, sizeof(client_finished_raw), &closed,
                  &io_error, handshake_deadline)) {
    return Fail(SecureTransportError::kIoError,
                closed ? "客户端在 ClientFinished 之前关闭了连接"
                       : "读取 ClientFinished 失败：" + io_error,
                error_message);
  }
  if (LoadU32(client_finished_raw) != kBssec1Magic ||
      client_finished_raw[4] != kBssec1MessageClientFinished ||
      client_finished_raw[5] != version || client_finished_raw[6] != 0 ||
      client_finished_raw[7] != 0) {
    return Fail(SecureTransportError::kMalformedMessage,
                "ClientFinished 消息格式不合法", error_message);
  }
  const std::string expected_client_finished =
      HmacTag(client_finished_key_, transcript_hash_);
  const std::string got_client_finished(
      reinterpret_cast<const char*>(client_finished_raw + 8), kSha256Bytes);
  if (!ConstantTimeEquals(expected_client_finished, got_client_finished)) {
    established_ = false;
    return Fail(SecureTransportError::kAuthenticationFailed,
                "ClientFinished 校验失败（客户端不持有会话密钥，或握手被改写）",
                error_message);
  }

  const std::string client_finished = got_client_finished;
  const std::string server_finished =
      HmacTag(server_finished_key_, transcript_hash_ + client_finished);
  const std::string finished_message = BuildFinished(
      kBssec1MessageServerFinished, server_finished, version);
  if (!SendAll(fd, finished_message.data(), finished_message.size(),
               &io_error)) {
    return Fail(SecureTransportError::kIoError,
                "发送 ServerFinished 失败：" + io_error, error_message);
  }
  // 握手完成：Finished 密钥不再需要，立刻清掉（记录层用的是另外两把）。
  Zeroize(&client_finished_key_);
  Zeroize(&server_finished_key_);
  return true;
}

bool SecureChannel::SendRecord(int fd, const std::string& plaintext,
                               std::string* error_message) {
  if (!established_) {
    return Fail(SecureTransportError::kStateError,
                "还没有完成 BPSEC1 握手就发送记录", error_message);
  }
  if (plaintext.size() > kBssec1MaxPlaintextBytes) {
    return Fail(SecureTransportError::kOversizedRecord,
                "单条记录的明文超过 " +
                    std::to_string(kBssec1MaxPlaintextBytes) + " 字节",
                error_message);
  }
  if (send_sequence_ == 0xFFFFFFFFFFFFFFFFull) {
    return Fail(SecureTransportError::kStateError,
                "发送方向的记录序号即将耗尽，必须重新握手", error_message);
  }

  const std::string counter = CounterBlock(send_nonce_prefix_, send_sequence_);
  crypto::Aes256Ctr cipher(send_key_, counter);
  if (!cipher.valid()) {
    return Fail(SecureTransportError::kCryptoFailure,
                "AES-256-CTR 初始化失败（密钥或计数器长度不合法）",
                error_message);
  }
  std::string ciphertext;
  cipher.Process(plaintext.data(), plaintext.size(), &ciphertext);
  if (ciphertext.size() != plaintext.size()) {
    return Fail(SecureTransportError::kCryptoFailure,
                "AES-256-CTR 输出长度与输入不一致", error_message);
  }

  const std::string tag =
      RecordTag(send_mac_key_, send_direction_, send_sequence_, ciphertext);
  std::string record;
  record.reserve(kBssec1RecordHeaderSize + ciphertext.size() + kBssec1TagSize);
  AppendU32(&record, kBssec1Magic);
  record.push_back(static_cast<char>(kBssec1MessageRecord));
  record.push_back(static_cast<char>(kBssec1Version));
  AppendU16(&record, 0);
  AppendU64(&record, send_sequence_);
  AppendU32(&record, static_cast<std::uint32_t>(ciphertext.size()));
  record.append(ciphertext);
  if (corrupt_next_tag_) {
    // 测试注入：把 tag 的第一个字节翻转，接收端必须拒绝。
    std::string broken = tag;
    broken[0] = static_cast<char>(broken[0] ^ 0x01);
    record.append(broken);
    corrupt_next_tag_ = false;
  } else {
    record.append(tag);
  }

  std::string io_error;
  if (!SendAll(fd, record.data(), record.size(), &io_error)) {
    return Fail(SecureTransportError::kIoError, "发送加密记录失败：" + io_error,
                error_message);
  }
  send_sequence_ += 1;
  return true;
}

FrameReadStatus SecureChannel::ReceiveRecord(int fd, std::string* plaintext,
                                             std::string* error_message) {
  if (plaintext == nullptr) {
    if (error_message != nullptr) {
      *error_message = "接收记录的输出指针为空";
    }
    return FrameReadStatus::kIoError;
  }
  plaintext->clear();
  if (!established_) {
    Fail(SecureTransportError::kStateError, "还没有完成 BPSEC1 握手就接收记录",
         error_message);
    return FrameReadStatus::kCorruptStream;
  }

  unsigned char header[kBssec1RecordHeaderSize];
  bool closed = false;
  std::string io_error;
  if (!ReceiveAll(fd, header, sizeof(header), &closed, &io_error)) {
    if (error_message != nullptr) {
      *error_message = io_error;
    }
    if (closed) {
      return FrameReadStatus::kClosed;
    }
    Fail(SecureTransportError::kIoError, io_error, nullptr);
    return FrameReadStatus::kIoError;
  }

  if (LoadU32(header) != kBssec1Magic) {
    Fail(SecureTransportError::kMalformedMessage, "记录头 magic 不是 BPS1",
         nullptr);
    if (error_message != nullptr) {
      *error_message = "记录头 magic 不对：对端不是 BPSEC1，或链路被改写";
    }
    established_ = false;
    return FrameReadStatus::kCorruptStream;
  }
  if (header[4] != kBssec1MessageRecord) {
    Fail(SecureTransportError::kMalformedMessage, "记录类型字段不对", nullptr);
    if (error_message != nullptr) {
      *error_message = "记录类型字段不对";
    }
    established_ = false;
    return FrameReadStatus::kCorruptStream;
  }
  if (header[5] != kBssec1Version) {
    Fail(SecureTransportError::kUnsupportedVersion, "记录版本不被支持",
         nullptr);
    if (error_message != nullptr) {
      *error_message = "记录层的 BPSEC1 版本不被支持";
    }
    established_ = false;
    return FrameReadStatus::kCorruptStream;
  }
  if (header[6] != 0 || header[7] != 0) {
    Fail(SecureTransportError::kMalformedMessage, "记录保留字段不是 0",
         nullptr);
    if (error_message != nullptr) {
      *error_message = "记录头保留字段不是 0";
    }
    established_ = false;
    return FrameReadStatus::kCorruptStream;
  }

  const std::uint64_t sequence = LoadU64(header + 8);
  const std::uint32_t ciphertext_length = LoadU32(header + 16);
  if (ciphertext_length > kBssec1MaxPlaintextBytes) {
    Fail(SecureTransportError::kOversizedRecord, "记录声明的密文长度超过上限",
         nullptr);
    if (error_message != nullptr) {
      *error_message = "记录声明的密文长度 " +
                       std::to_string(ciphertext_length) + " 超过上限 " +
                       std::to_string(kBssec1MaxPlaintextBytes);
    }
    established_ = false;
    return FrameReadStatus::kCorruptStream;
  }
  // 接收方向的序号耗尽（审查轮缺陷 A）：发送侧已经拒绝发出 UINT64_MAX
  // （见 SendFrame），接收侧也必须拒绝收下它——否则 receive_sequence_ += 1
  // 会回绕到 0，同一会话里 seq=0 的记录就能被再接受一次（可重放）。
  // 合规对端永远不会发出这个序号，所以这里对互操作性零影响。
  if (sequence == 0xFFFFFFFFFFFFFFFFull) {
    Fail(SecureTransportError::kReplayDetected,
         "接收方向的记录序号耗尽（收到 UINT64_MAX，计数器不允许回绕）",
         nullptr);
    if (error_message != nullptr) {
      *error_message = "接收方向的记录序号耗尽，拒绝回绕";
    }
    established_ = false;
    return FrameReadStatus::kCorruptStream;
  }
  if (sequence != receive_sequence_) {
    Fail(SecureTransportError::kReplayDetected,
         "记录序号不连续（期望 " + std::to_string(receive_sequence_) +
             "，收到 " + std::to_string(sequence) + "）",
         nullptr);
    if (error_message != nullptr) {
      *error_message = "记录序号不连续：期望 " +
                       std::to_string(receive_sequence_) + "，收到 " +
                       std::to_string(sequence) +
                       "（重放、乱序或丢包都会走到这里）";
    }
    established_ = false;
    return FrameReadStatus::kCorruptStream;
  }

  std::string body(static_cast<std::size_t>(ciphertext_length) + kBssec1TagSize,
                   '\0');
  bool body_closed = false;
  if (!ReceiveAll(fd, &body[0], body.size(), &body_closed, &io_error)) {
    if (error_message != nullptr) {
      *error_message = io_error;
    }
    Fail(SecureTransportError::kIoError, io_error, nullptr);
    established_ = false;
    return FrameReadStatus::kIoError;
  }
  const std::string ciphertext = body.substr(0, ciphertext_length);
  const std::string tag = body.substr(ciphertext_length, kBssec1TagSize);

  // 方向字节来自本端在握手里的角色（见 DeriveKeys），不靠对端声明。
  const std::string expected_tag =
      RecordTag(receive_mac_key_, receive_direction_, sequence, ciphertext);
  if (!ConstantTimeEquals(expected_tag, tag)) {
    Fail(SecureTransportError::kRecordAuthentication, "记录 HMAC 校验失败",
         nullptr);
    if (error_message != nullptr) {
      *error_message =
          "加密记录校验失败：数据被修改过（或密钥不一致）；这条连接不可信";
    }
    established_ = false;
    return FrameReadStatus::kCorruptStream;
  }

  const std::string counter = CounterBlock(receive_nonce_prefix_, sequence);
  crypto::Aes256Ctr cipher(receive_key_, counter);
  if (!cipher.valid()) {
    Fail(SecureTransportError::kCryptoFailure, "AES-256-CTR 初始化失败",
         nullptr);
    if (error_message != nullptr) {
      *error_message = "AES-256-CTR 初始化失败（本地密码学原语错误）";
    }
    established_ = false;
    return FrameReadStatus::kCorruptStream;
  }
  cipher.Process(ciphertext.data(), ciphertext.size(), plaintext);
  if (plaintext->size() != ciphertext.size()) {
    Fail(SecureTransportError::kCryptoFailure, "解密输出长度不一致", nullptr);
    if (error_message != nullptr) {
      *error_message = "解密输出长度与密文不一致";
    }
    established_ = false;
    return FrameReadStatus::kCorruptStream;
  }
  receive_sequence_ += 1;
  return FrameReadStatus::kOk;
}

bool SecureChannel::SendFrame(int fd, std::uint16_t opcode,
                              std::uint32_t status, std::uint64_t request_id,
                              const std::string& payload,
                              std::string* error_message) {
  if (payload.size() > kMaxPayloadBytes) {
    if (error_message != nullptr) {
      *error_message = "refusing to send a payload larger than 1 MiB";
    }
    return false;
  }
  FrameHeader header;
  header.version = kProtocolVersion;
  header.opcode = opcode;
  header.flags = 0;
  header.reserved = 0;
  header.status = status;
  header.request_id = request_id;
  header.payload_length = payload.size();
  const std::string encoded = EncodeFrameHeader(header);
  std::string plaintext;
  plaintext.reserve(encoded.size() + payload.size());
  plaintext.append(encoded);
  plaintext.append(payload);
  return SendRecord(fd, plaintext, error_message);
}

FrameReadStatus SecureChannel::ReceiveFrame(int fd, FrameHeader* header,
                                            std::string* payload,
                                            std::string* error_message) {
  if (header == nullptr || payload == nullptr) {
    if (error_message != nullptr) {
      *error_message = "frame output pointers are null";
    }
    return FrameReadStatus::kIoError;
  }
  std::string plaintext;
  const FrameReadStatus status = ReceiveRecord(fd, &plaintext, error_message);
  if (status != FrameReadStatus::kOk) {
    return status;
  }
  if (plaintext.size() < kFrameHeaderSize) {
    if (error_message != nullptr) {
      *error_message = "记录里的明文不足一个 BPNET1 帧头";
    }
    established_ = false;
    return FrameReadStatus::kCorruptStream;
  }
  const unsigned char* raw =
      reinterpret_cast<const unsigned char*>(plaintext.data());
  DecodeFrameHeaderFields(raw, kFrameHeaderSize, header);
  FrameHeader validated;
  std::string header_error;
  if (!DecodeFrameHeader(raw, kFrameHeaderSize, &validated, &header_error)) {
    if (error_message != nullptr) {
      *error_message = header_error;
    }
    const bool magic_bad = LoadU32(raw) != kProtocolMagic;
    const bool length_bad = LoadU64(raw + 24) > kMaxPayloadBytes;
    if (magic_bad || length_bad) {
      established_ = false;
      return FrameReadStatus::kCorruptStream;
    }
    return FrameReadStatus::kInvalidFrame;
  }
  if (plaintext.size() !=
      kFrameHeaderSize + static_cast<std::size_t>(validated.payload_length)) {
    if (error_message != nullptr) {
      *error_message =
          "加密记录里的帧长度与记录长度不一致（记录内的帧被截断或拼接）";
    }
    established_ = false;
    return FrameReadStatus::kCorruptStream;
  }
  *header = validated;
  payload->assign(plaintext, kFrameHeaderSize,
                  static_cast<std::size_t>(validated.payload_length));
  return FrameReadStatus::kOk;
}

}  // namespace net
}  // namespace backupproject
