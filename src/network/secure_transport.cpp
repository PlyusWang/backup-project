// src/network/secure_transport.cpp
//
// BPSEC1 的实现。协议与安全性质的说明见 include/secure_transport.h。
//
// 这个文件里没有第三方密码学代码：X25519 / HKDF / AES-256-CTR / HMAC-SHA256
// 全部来自本项目自己的 src/crypto/。
// 本文件实现 BPSEC1 / BPSEC2 的全部线上字节：握手消息、记录层与错误分类。
// 边界：它不解释 BPNET1 帧的语义（opcode、status 的含义都在上层），不负责
// 重连与重传，不缓存对端身份，也不持有或关闭 socket —— fd 由调用方管理。
//
// 数据流（发送）：FrameHeader + payload -> SendFrame 拼成一段明文 ->
// SendRecord 加密并附 HMAC -> 一段 record 字节 -> send(2)。
// 数据流（接收）：recv(2) -> ReceiveRecord 先验 tag 再解密 ->
// ReceiveFrame 按帧头切出 payload -> 交给上层。
//
// 不变量（都有测试钉住，改动前先读 include/secure_transport.h）：
//   * 一条 record 恰好装一个完整 BPNET1 帧，不合并、不拆分；
//   * 每个方向的 record 序号从 0 起严格 +1，回绕或被重放即断连；
//   * Encrypt-then-MAC：tag 覆盖方向字节、序号、密文长度与密文；
//   * 任何一次握手或校验失败之后 established_ 恒为 false，无明文回退。
//
// 失败边界：公开函数一律返回 bool / FrameReadStatus，不抛异常；分类写进
// last_error_ 与可选的 error_message，一次失败就要求调用方关掉这条连接。
//
// 线程与生命周期：SecureChannel 不带锁，同一条连接上的握手、发送、接收必须
// 由同一个线程串行调用（序号与密钥都是可变状态）；析构会清零会话密钥。

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

// errno 必须在失败的第一时间翻成文本：它是线程局部的，会被后续任何一次系统
// 调用覆盖；strerror 的返回值还可能指向下一次调用就会被复写的静态缓冲，
// 所以这个函数只做一件事——立刻把当时的 errno 取走。
std::string StrerrorText() { return std::string(std::strerror(errno)); }

// 线上整数一律大端（网络字节序），与主机字节序无关。刻意不用 htonl /
// htobe32 那一套：写死字节序之后，测试可以逐字节比对固定期望值。
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

// 64 位字段只出现在记录序号与 AES-CTR 计数器块里：两处都要求固定的 8 字节
// 大端整数语义，不能退化成变长编码。
void AppendU64(std::string* out, std::uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out->push_back(static_cast<char>((value >> shift) & 0xFFu));
  }
}

// 与 AppendU16/32/64 对称的读取端，不做长度检查：调用方必须先保证缓冲区
// 至少有 4（LoadU32）/ 8（LoadU64）字节。本文件里的调用点要么是定长数组，
// 要么是刚刚按长度校验过的字符串，长度在调用处就能一眼看出来。
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

// 摘要统一以裸 32 字节返回，而不是十六进制文本：里面出现 0x00 是正常的，
// 因此只能按 size() 使用，绝不能当 C 字符串（strlen 会提前截断）。需要给
// 人看时走 X25519Fingerprint / Bpcert1Fingerprint 那类十六进制包装。
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
// 下面这几个字段必须进 MAC 输入，缺一不可：
//   * direction：把方向绑死，防止把对端发来的记录原样反射回去；
//   * sequence：序号自己也要被保护，否则改掉序号就能绕过重放检测；
//   * ciphertext_length：长度被改会让接收端切错边界；纳入覆盖范围之后，
//     密文的截断与拼接都会在 tag 校验处暴露。
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
// nonce_prefix 只有 4 字节、每个方向一份（见 DeriveKeys 里的 info 标签），
// 配合每方向独立的 key，(key, counter) 组合在整个会话里不会重复。
std::string CounterBlock(const std::string& nonce_prefix,
                         std::uint64_t sequence) {
  std::string block;
  block.reserve(crypto::kAesBlockSize);
  block.append(nonce_prefix);
  AppendU64(&block, sequence);
  AppendU32(&block, 0);
  return block;
}

// 只把 A-F 折成 a-f，不碰其它字节：输入来源固定是十六进制文本，因此不需要
// 触碰 locale，也不会把非 ASCII 字节卷进大小写转换。
std::string LowerHex(const std::string& text) {
  std::string out = text;
  for (char& ch : out) {
    if (ch >= 'A' && ch <= 'F') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  return out;
}

// pin 与指纹文本的规范化：比较前统一成小写。对输入大小写宽容，输出永远是
// 小写 64 位十六进制，日志、配置与测试断言之间就不会出现假不匹配。
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

// ClientHello 的线上布局（72 字节定长，大端）：
//     0   4  magic "BPS1"（0x42505331）
//     4   1  消息类型 = kBssec1MessageClientHello
//     5   1  版本：1 = BPSEC1（pin 身份），2 = BPSEC2（证书身份）
//     6   2  密码套件
//     8  32  client_random：进 transcript，也是 KDF salt 的前半段
//    40  32  客户端临时 X25519 公钥（每次握手新生成，用完即弃）
// 版本字节放在第 5 字节是刻意的：服务端读满这条定长消息、还没做任何密钥
// 运算时就知道该走哪条握手，不需要试探、不需要协商。
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

// ServerHello 的线上布局（104 字节定长，大端）：
//     0   4  magic
//     4   1  类型 = kBssec1MessageServerHello
//     5   1  版本：回显客户端请求的版本
//     6   2  密码套件，逐字节校验（当前只有一种，但不写成“跳过检查”）
//     8  32  server_random
//    40  32  服务端长期身份公钥：客户端拿它与本地 pin 或证书比对
//    72  32  服务端临时公钥：提供前向保密，不参与身份判断
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

// Finished 的布局是定长 40 字节：magic(4) + type(1) + version(1) +
// reserved(2，恒为 0) + HMAC-SHA256 tag(32)。注意第 6、7 字节在 Hello 里是
// 密码套件、在这里是保留位，含义不同，所以公共校验只覆盖前 6 个字节。
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
// 12 字节头的布局：magic(4) + type(1) + version=2(1) + reserved(2) +
// certificate_length(4，大端，只算证书字节、不含本头)。接收端先读这 12 字节
// 定长头再按长度读证书，变长字段因此不会把解析变成“读到多少算多少”。
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
// 四条定长握手消息共用这一个前缀校验器，检查顺序即错误分类的优先级：
// 长度 -> magic -> 类型 -> 版本。顺序固定，同一种坏字节在任何一条消息上
// 都会得到同一个错误码。
// 前置条件：error 非空（函数内不判空）；error_message 可空，只影响是否
// 附带一句给人看的说明。它不校验第 6、7 字节，那两个字节由调用点自己验。
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

// 一次握手里派生出的全部密钥材料，按用途与方向切成 8 份：enc / mac / nonce
// 各两个方向各一份，再加两把 Finished 密钥。字段之间没有派生关系（每份都是
// HKDF-Expand 的独立输出），任何一份泄漏都不影响其它份。
// 所有权：由 DeriveSessionKeys 填充、由 SecureChannel::DeriveKeys 取走；用完
// 必须逐字段 Zeroize —— 复制这个结构体会留下第二份密钥副本。
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
// 会话密钥派生的两个输入各自解决一个问题：
//   * DH_static = X25519(本端临时私钥, 对端长期身份公钥)：绑定身份，中间人
//     没有长期私钥就推不出同一份秘密；
//   * DH_ephemeral = X25519(本端临时私钥, 对端临时公钥)：提供前向保密，长期
//     私钥事后泄漏也解不开已经过去的会话。
// IKM 是两者的顺序拼接，salt 是 client_random || server_random：顺序必须与
// 对端逐字节一致，差一个字节派生出的密钥就完全不同。
// 各份输出的长度按用途给足：AES 与 HMAC 各 32 字节，CTR 的 nonce 前缀只要
// 4 字节（计数器块剩下的 12 字节由序号与块号填满），Finished 密钥 32 字节。
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

// 清零之后把长度也清掉：让“误用已清零的密钥”立刻表现为空串，而不是一把
// 全零、看起来仍然合法的密钥。best-effort 的边界见上面声明处的说明。
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

// 机器可读的稳定名字：日志、测试断言、告警分组用的都是它，因此这些 ASCII
// 串不能随文案调整而改名——改名等于破坏下游的匹配规则。
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

// 给人看的中文说明：措辞可以随时改，不承诺稳定，也不要拿它做匹配。
// 两类文本都不含密钥材料，可以安全写进日志。
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

// 输入是管理员写进配置或命令行的可信文本，但格式仍然是严格的：公钥与指纹
// 都是 64 个十六进制字符，去掉前缀就无法区分，把指纹当公钥用只会表现为
// “连不上”，排查成本极高，所以前缀是必须的。
// hex: 形式解析成功后顺手算出指纹，后续校验路径因此只需要一个
// fingerprint_hex 字段，不必到处写 if (has_key) 分支。
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

// 只在服务端首次初始化时调用。返回的结构体里含私钥，落盘必须走
// SaveTransportIdentity（0600 + 普通文件校验），不要自己写文件。
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

// 磁盘布局就是裸 32 字节私钥：没有 PEM/ASN.1 头、没有口令加密、没有版本
// 号。前提是权限位与 O_NOFOLLOW 已经挡住了别的用户；换来的是格式不随实现
// 演进，读回时“长度恰好 32 字节”就是全部的一致性检查。
// overwrite 为假时用 O_EXCL：私钥已存在就失败，绝不静默覆盖。覆盖会让所有
// pin 过这把公钥的客户端从此连不上，而且报的是最像中间人攻击的那个错误，
// 必须由管理员显式决定。
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

// 公钥不落盘，由私钥现算（X25519PublicKeyFromPrivate）：文件里不可能出现
// “公钥和私钥不是一对”这种状态。
// 读取顺序刻意是 open -> fstat -> 校验类型与权限 -> 读 32 字节 -> 再读 1
// 字节确认没有尾巴：多一个字节就说明这不是本模块写的文件（被追加过内容），
// 宁可拒绝，也不接受“前 32 字节看着没错”的文件。
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

// 构造之后处于“未握手”状态：所有密钥为空、序号为 0，任何收发都会以
// kStateError 失败，直到某一次握手把 established_ 置为 true。
SecureChannel::SecureChannel() = default;

// 析构只清零内存里的密钥与序号，不关闭 fd、也不给对方发任何告警：BPSEC1
// 没有 close_notify 这类消息，对端只会看到 TCP 连接结束。fd 的生命周期
// 完全属于调用方。
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
  // BPSEC2 的两个字段也必须一起清。红队复核指出第一版漏了它们：同一条
  // SecureChannel 复用（Reset 之后再握手失败）时，peer_server_id() 会继续
  // 报上一次那张证书的身份 —— 一个"陈旧身份"。今天没有生产调用方读它，
  // 但这是典型的"等有人读的时候就晚了"的状态泄漏。
  peer_server_id_.clear();
  peer_certificate_fingerprint_.clear();
  transcript_hash_.clear();
}

// 统一的失败出口：写 last_error_、把通道标记为不可用、组装错误文本，然后
// 返回 false —— 返回值恒为 false，就是为了让调用点写成 return Fail(...)，
// “记录错误”和“返回失败”因此不可能被拆开。detail 是给用户的补充信息
// （路径、长度、期望值之类），不要放密钥材料。
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

// 把派生结果按本端角色装进成员：客户端把 c2s 当发送方向，服务端反过来。
// 两个方向各有独立密钥、独立 nonce 前缀、独立序号，所以一条记录被反射回
// 发送方也无法通过校验（方向字节与 MAC 密钥都对不上）。
// 顺序上先装密钥、最后才置 established_：任何一步失败都会走 Fail 把它复位，
// 不存在“密钥只装了一半、通道却已标记可用”的窗口。
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

// 两个公开入口是互斥的两条路：传 pin 走 BPSEC1，传 policy 走 BPSEC2。
// 分开暴露而不是做成一个带可选参数的重载，就是为了让调用方在类型层面
// 明确写下自己选择的是哪一种身份模型。
bool SecureChannel::HandshakeClient(int fd, const ServerKeyPin& pin,
                                    std::string* error_message) {
  return HandshakeClientInternal(fd, &pin, nullptr, error_message);
}

bool SecureChannel::HandshakeClientWithCertificate(
    int fd, const ServerIdentityPolicy& policy, std::string* error_message) {
  return HandshakeClientInternal(fd, nullptr, &policy, error_message);
}

// 客户端握手步骤，顺序即信任建立的顺序：
//   1. 生成本次会话的 client_random 与临时 X25519 密钥对；
//   2. 发 ClientHello；读定长 ServerHello，校验头与密码套件；
//   3. 验身份：BPSEC2 逐级走“结构 -> 可信根 -> 根签名 -> server_id ->
//      有效期 -> 证书公钥等于握手公钥”；BPSEC1 比对 pin 的公钥或指纹；
//   4. 身份确认之后才做两次 X25519 并派生会话密钥；
//   5. 用 transcript 摘要算 ClientFinished 发出，再验 ServerFinished。
// 第 3 步必须在第 4 步之前：对一个没验过身份的连接做 DH 和密钥派生，等于把
// 密钥协商过程交给一个还没确认身份的对端，失败分类也会失去意义。
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

  // 这两个字段记录的是“对端实际出示了什么”，而不是“我们接受了什么”：它们
  // 在校验之前就写入，因此当次握手失败时里面仍可能留着未被认可的值。调用方
  // 只能把 peer_fingerprint() 当诊断信息用（例如报 pin 不匹配时打印实际值），
  // 不能当成身份已确认的凭据——那要看 established() 与返回值。
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
    certificate_message.assign(reinterpret_cast<const char*>(cert_header),
                               sizeof(cert_header));
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
      return Fail(
          SecureTransportError::kCertificateUntrusted,
          "证书的签发者 " + certificate.issuer_id + " 不在本机可信根列表里",
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
    // 这个分支只认公钥（逐字节、常数时间比较），不做 TOFU，也不接受任何
    // “指纹看起来对就行”的柔化；指纹那一侧由下面的独立检查负责。
  } else if (pin->has_key) {
    // 服务端身份校验：pin 说什么就只接受什么。这里**没有** TOFU 分支。
    if (!ConstantTimeEquals(pin->public_key, server_static)) {
      return Fail(SecureTransportError::kServerKeyMismatch,
                  "ServerHello 里的身份公钥与本地 pin 的公钥不同",
                  error_message);
    }
  }
  // 指纹检查对所有 pin 写法都执行：只给指纹时它是唯一判据，同时给了公钥
  // 时它是对同一把密钥的第二次、互相独立的确认（比较对象都是从线上字节算
  // 出来的 peer_fingerprint_）。任一不符都按 kServerKeyMismatch 终止。
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
  // 客户端侧的两份 DH：客户端没有长期身份密钥，所以“绑定身份”的那一份用
  // 服务端的长期公钥（dh_static），前向保密的那一份用服务端临时公钥
  // （dh_ephemeral）。两次都对低阶点零容忍：X25519SharedSecret 拒绝全零
  // 输出，失败一律映射成 kWeakSharedSecret，不再继续派生任何密钥。
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

  // transcript 绑定三次握手的原始字节：ClientHello || ServerHello ||
  // ServerCertificate（BPSEC1 时第三项为空串，因此与旧版本逐字节相同）。
  const std::string server_hello(reinterpret_cast<const char*>(raw),
                                 sizeof(raw));
  transcript_hash_ = Sha256Of(hello + server_hello + certificate_message);

  // 客户端先发自己的 Finished（证明自己算出了同一份会话密钥），再等对端的：
  // 服务端要在回 ServerFinished 之前认证客户端，两端因此都不需要“先证明
  // 自己”的额外往返。Finished 密钥只在握手期间存在，用完立刻清零。
  const std::string client_finished =
      HmacTag(client_finished_key_, transcript_hash_);
  const std::string finished_message =
      BuildFinished(kBssec1MessageClientFinished, client_finished, version);
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
  // 常数时间比较：期望值是由会话密钥算出来的 MAC，逐字节短路比较会把
  // “前几个字节猜对了”这个信息通过耗时泄漏出去，等于给伪造 tag 留了侧信道。
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

// 服务端握手全程处于未认证阶段，因此每一步都有上界：只读定长消息、按已校验
// 的长度读一次证书，再叠加整体 deadline；没有任何“读到某个字节为止”的循环。
// 服务端先验 ClientFinished 再发 ServerFinished：客户端在看到 ServerFinished
// 之前不会认为握手成功，两端对“什么时候算完成”的判断因此一致。
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
  // 配置成“只接受证书身份”却没有证书属于启动期配置错误，在这里立刻失败，
  // 而不是等客户端连上来才报错。
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
  // 先把长度/magic/类型校验掉，**再**按版本分流。顺序不能反：反过来的话，
  // 一个 magic 被改写、版本字节又恰好不是 1/2 的包会被报成"版本不支持"，
  // 把"链路上有东西在改字节"说成了"对端版本太新"—— BPSEC1 的既有用例
  // 正是靠这条分类来区分这两种情况的。
  if (LoadU32(raw) != kBssec1Magic || raw[4] != kBssec1MessageClientHello) {
    return Fail(SecureTransportError::kMalformedMessage,
                "ClientHello 的 magic 或类型字段不合法", error_message);
  }
  // 版本判别：客户端说 BPSEC2 就走证书握手；说 BPSEC1 时，若本服务端被
  // 配置为只接受证书身份，就直接拒绝 —— 这就是"拒绝降级"的落点。
  // 这里的初值只是占位；真正决定走哪条路的是下面这条 if/else 链，三个分支
  // 互斥，版本不认识就直接失败——不猜测、不协商、不将就。
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
  // 与客户端镜像对称的两次 DH：dh_static 用长期身份私钥，dh_ephemeral 用
  // 本次临时私钥，对端都是客户端的临时公钥。两端的 IKM 因此逐字节相同
  // （DH_static || DH_ephemeral），拼接顺序不能调换。
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

  // transcript 覆盖三次握手的原始字节（ClientHello || ServerHello ||
  // ServerCertificate）：Finished 的 HMAC 因此把版本、套件、双方随机数、
  // 临时公钥与整张证书一起钉死，改任何一个字节都会在 Finished 处暴露。
  const std::string client_hello(reinterpret_cast<const char*>(raw),
                                 sizeof(raw));
  transcript_hash_ =
      Sha256Of(client_hello + server_hello + certificate_message);

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
  const std::string finished_message =
      BuildFinished(kBssec1MessageServerFinished, server_finished, version);
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

// 记录层发送端。写出的记录布局（全部大端）：
//     0   4  magic "BPS1"
//     4   1  类型 = kBssec1MessageRecord
//     5   1  版本 = 1（记录层恒为 BPSEC1，BPSEC2 只改握手）
//     6   2  保留，恒为 0
//     8   8  序号：本方向的第几条记录，从 0 开始
//    16   4  密文长度，等于明文长度（AES-CTR 是流模式，不补齐）
//    20   N  密文
//  20+N  32  HMAC-SHA256 tag
// 加密与认证的顺序是 Encrypt-then-MAC，tag 覆盖方向、序号、长度与密文本身；
// 接收端先验 tag 再解密，因此伪造的记录一个字节的明文都拿不到。
// 明文上限 kBssec1MaxPlaintextBytes = 帧头 + 1 MiB，超限直接失败而不是分片：
// “一条 record 恰好一个帧”是接收端那条长度一致性检查的前提。
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
  // 序号耗尽必须在本端拒绝发出，而不是发出去后回绕：接收端看到
  // UINT64_MAX 会直接判定计数器耗尽并断连，与其让对端报“序号不连续”，
  // 不如本端先说清楚原因。
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
  // 测试注入点：只翻转 tag 的第一个字节，其余部分完全正确，用来验证
  // “tag 不对就必须断连”这条性质。产品路径上这个标志恒为 false。
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

// 记录层接收端。校验顺序不能重排：
//   1. 先读满 20 字节定长头，对端在读完头之前干净关闭 -> kClosed；
//   2. 校验 magic / 类型 / 版本 / 保留位；
//   3. 用头部声明的密文长度先卡上限，再决定是否继续读：未认证的长度绝不能
//      被当成分配内存的依据；
//   4. 序号连续性与序号耗尽检查（重放、乱序、丢包都在这里暴露）；
//   5. 读满 密文 || tag，用本方向的 mac_key 重算 tag 并常数时间比较；
//   6. 只有 tag 通过之后才解密，明文永不从一条未认证的记录里流出去。
// 除第 1 步之外，任何失败都会把 established_ 置为 false（通道作废）并返回
// kCorruptStream，让上层断开连接而不是继续复用这条流。
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

  // 计数器块用收到的序号重建：这个序号已经通过连续性与耗尽检查，因此和发送
  // 端加密时用的那块 keystream 必然一致；先验 tag 再用它，顺序不能反。
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

// payload 上限与 BPNET1 明文路径完全一致（1 MiB），超限直接失败而不是发半个
// 帧；帧头与 payload 拼成一段明文，加密后正好成为一个 record。
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

// 一条记录 = 一个完整帧，这条不变式两端都成立：发送端把帧头与 payload 拼成
// 一段明文交给 SendRecord，接收端要求记录长度恰好等于帧头声明的长度。因此
// 这里既不做粘包合并，也不做跨记录的分片重组——那是上层 BPNET1 的事。
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
  // 先用不校验的解码把字段填进调用方的 header（错误路径上调用方往往要打印
  // request_id 之类的东西），随后再用 DecodeFrameHeader 校验一份副本。
  // 注意：校验失败时 *header 里是未经校验的值，调用方必须先看返回状态，
  // 不能把 header 的内容当成可信输入。
  DecodeFrameHeaderFields(raw, kFrameHeaderSize, header);
  FrameHeader validated;
  std::string header_error;
  if (!DecodeFrameHeader(raw, kFrameHeaderSize, &validated, &header_error)) {
    if (error_message != nullptr) {
      *error_message = header_error;
    }
    // 分类依据与 BPNET1 明文路径一致：magic 被改写或长度超过 1 MiB 时流已经
    // 无法安全同步，只能断连（kCorruptStream）；其余帧头问题（版本、opcode、
    // flags）流位置仍然完好，回一个错误帧后可以继续服务（kInvalidFrame）。
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
