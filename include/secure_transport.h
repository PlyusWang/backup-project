// include/secure_transport.h
//
// PR #21：BPSEC1 —— 本项目自己的认证加密传输层。
//
// 它不是 TLS，也不打算与 TLS 兼容：它是 BPNET1 业务协议**下面**的一层，
// 只做四件事——握手、密钥派生、把整帧明文加密成记录、以及拒绝一切被改动过
// 的字节。层次关系：
//
//     BPNET1 帧（32 字节帧头 + payload）   <-- 业务协议，尽量不改
//              ↓  整帧作为明文
//     BPSEC1 记录（seq + 密文 + HMAC）
//              ↓
//     TCP socket
//
// 关键性质（每条都有对应的自动化测试）：
//   * 保密性：帧头（opcode / status / 长度）与 payload 一起加密。**不是**
//     只加密 payload——否则 opcode 与长度仍然把业务语义漏在线上。
//   * 完整性：Encrypt-then-MAC，先验 tag 再解密；tag 不对时一个字节的明文
//     都不会交出去。
//   * 服务端认证：服务端有长期 X25519 身份密钥，客户端**必须**预先配置
//     期望的公钥或指纹（pin）。没有配置就明确报错，不做 TOFU。
//   * 前向保密：会话密钥来自两份临时 DH（static 与 ephemeral 的共享秘密
//     都乘进了 KDF 的 IKM），长期私钥泄漏也不能解出历史会话。
//   * 抗重放：每个方向的记录序号严格递增且必须连续，重放 / 跳号 / 乱序
//     一律断连。
//   * transcript 绑定：ClientFinished / ServerFinished 的 HMAC 覆盖
//     SHA256(ClientHello || ServerHello)，任何一方改过握手字节都会失败。
//   * 不降级：握手失败就是失败，绝不回退到明文 BPNET1（没有这条代码路径）。
//
// 密码学组件全部是本项目自己的手写实现：X25519（include/x25519.h）、
// HKDF-SHA256（include/hkdf.h）、AES-256-CTR 与 HMAC-SHA256
// （include/crypto.h）。没有链接或复制任何第三方密码库。
//
// 这是**教学用途**的实现：算法与测试向量都对得上标准，但它没有经过外部
// 审计，也不要做成"生产级 TLS 替代品"来宣传（见 docs/secure_transport.md）。

#ifndef BACKUP_PROJECT_INCLUDE_SECURE_TRANSPORT_H_
#define BACKUP_PROJECT_INCLUDE_SECURE_TRANSPORT_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include "network_protocol.h"
#include "trusted_root_store.h"
#include "x25519.h"

namespace backupproject {
namespace net {

// ---- 协议常量 ----

// "BPS1"：0x42 0x50 0x53 0x31。
inline constexpr std::uint32_t kBssec1Magic = 0x42505331u;
inline constexpr std::uint8_t kBssec1Version = 1;

// BPSEC2：在 BPSEC1 的帧与记录层**之上**加一层服务器身份证书认证。
// 沿用同一个 magic（"BPS1"），版本升到 2，并新增一条 ServerCertificate
// 消息。版本的判别点就在每条握手消息的固定位置，所以两边都不需要"猜对端
// 说的是哪套协议"；官方云端模式只接受版本 2，收到版本 1 直接拒绝。
inline constexpr std::uint8_t kBssec2Version = 2;

// 目前唯一的密码套件：X25519 + HKDF-SHA256 + AES-256-CTR + HMAC-SHA256。
inline constexpr std::uint16_t kBssec1SuiteX25519Aes256CtrHmacSha256 = 1;

inline constexpr std::uint8_t kBssec1MessageClientHello = 1;
inline constexpr std::uint8_t kBssec1MessageServerHello = 2;
inline constexpr std::uint8_t kBssec1MessageClientFinished = 3;
inline constexpr std::uint8_t kBssec1MessageServerFinished = 4;
inline constexpr std::uint8_t kBssec1MessageRecord = 5;
// BPSEC2 新增：服务器身份证书（变长 = 12 字节定长头 + 证书字节）。
// 放在 ServerHello 之后、ClientFinished 之前 —— 客户端在派生任何会话密钥、
// 发出任何已认证字节之前就完成身份判断。
inline constexpr std::uint8_t kBssec2MessageServerCertificate = 6;

inline constexpr std::size_t kBssec1HelloMagicSize = 4;
inline constexpr std::size_t kBssec1ClientHelloSize = 72;
inline constexpr std::size_t kBssec1ServerHelloSize = 104;
inline constexpr std::size_t kBssec1FinishedSize = 40;
// ServerCertificate 的定长头：magic 4 + type 1 + version 1 + reserved 2 +
// 证书长度 4。
inline constexpr std::size_t kBssec2CertificateHeaderSize = 12;
inline constexpr std::size_t kBssec1RecordHeaderSize = 20;
inline constexpr std::size_t kBssec1TagSize = 32;

// 一条记录里最多装下"一个完整的 BPNET1 帧"：32 字节帧头 + 1 MiB payload。
inline constexpr std::size_t kBssec1MaxPlaintextBytes =
    kFrameHeaderSize + static_cast<std::size_t>(kMaxPayloadBytes);
inline constexpr std::size_t kBssec1MaxRecordSize =
    kBssec1RecordHeaderSize + kBssec1MaxPlaintextBytes + kBssec1TagSize;

// HMAC 覆盖的范围里带的协议标签（16 字节，正好一个分组）：
//   HMAC(mac_key, label || direction || sequence || ciphertext_length ||
//   ciphertext)
inline constexpr char kBssec1RecordLabel[] = "BPSEC1 record v1";
inline constexpr std::size_t kBssec1RecordLabelSize = 16;
inline constexpr std::uint8_t kBssec1DirectionClientToServer = 1;
inline constexpr std::uint8_t kBssec1DirectionServerToClient = 2;

// HKDF-Expand 的 info 标签。**每个用途一个标签**：同一把密钥绝不同时用于
// AES 与 HMAC，两个方向也绝不复用同一把密钥。
inline constexpr char kBssec1InfoClientToServerEnc[] = "BPSEC1 c2s enc";
inline constexpr char kBssec1InfoClientToServerMac[] = "BPSEC1 c2s mac";
inline constexpr char kBssec1InfoClientToServerNonce[] = "BPSEC1 c2s nonce";
inline constexpr char kBssec1InfoServerToClientEnc[] = "BPSEC1 s2c enc";
inline constexpr char kBssec1InfoServerToClientMac[] = "BPSEC1 s2c mac";
inline constexpr char kBssec1InfoServerToClientNonce[] = "BPSEC1 s2c nonce";
inline constexpr char kBssec1InfoClientFinished[] = "BPSEC1 client finished";
inline constexpr char kBssec1InfoServerFinished[] = "BPSEC1 server finished";

// ---- 错误分类 ----
//
// 调用方（CLI / GUI）需要把"服务端密钥不对"与"网络抖了一下"分开说，
// 所以错误不止一个布尔值。名字与一句话说明都从这里出，界面不再自己翻译。

enum class SecureTransportError {
  kNone = 0,
  kIoError,           // 传输层读写失败 / 对端中途关闭
  kMalformedMessage,  // 消息 magic、类型、保留字段或长度不合法
  kUnsupportedVersion,  // 版本或密码套件不认识
  kServerKeyMismatch,   // 服务端身份公钥与本地 pin 不一致
  kAuthenticationFailed,  // Finished 校验失败（transcript 被改 / 密钥不一致）
  kWeakSharedSecret,  // 共享秘密全零（对端给了低阶点）
  kNoPinConfigured,  // 客户端没有配置服务端公钥/指纹（拒绝连接，不做 TOFU）
  // ---- BPSEC2（证书身份）。失败原因分开报，界面才能说清楚到底哪里不对。----
  kCertificateMissing,      // 对端要用 BPSEC2，但服务端没有配置证书
  kCertificateInvalid,      // 证书结构不合法，或根签名验不过
  kCertificateUntrusted,    // 签发者不在可信根里（根为空 / 被吊销 / 时间越界）
  kCertificateWrongServerId,  // 证书里的 server_id 与期望值不符
  kCertificateExpired,      // 证书不在有效期内（含本机时钟不对的情形）
  kCertificateKeyMismatch,  // 证书认证的公钥 ≠ 握手里实际使用的身份公钥
  kDowngradeRefused,        // 要求 BPSEC2 却收到 BPSEC1：拒绝降级
  kRecordAuthentication,  // 记录层 tag 校验失败
  kReplayDetected,        // 记录序号不连续：重放、跳号或乱序
  kOversizedRecord,       // 记录长度超过上限
  kStateError,     // 没有完成握手就使用记录层，或序号耗尽
  kCryptoFailure,  // 本地密码学原语失败（密钥/IV 长度不合法等）
};

const char* SecureTransportErrorName(SecureTransportError error);
// 给人看的一句话（中文，不含任何密钥材料）。CLI 与 GUI 共用。
std::string SecureTransportErrorMessage(SecureTransportError error);

// ---- 服务端身份（pin）----

// 客户端侧的服务端身份期望值。两种形式二选一：
//   * 直接 pin 公钥（32 字节），或
//   * 只 pin 指纹（SHA-256 的 64 个小写十六进制字符）。
// 两者都没有时不能连接：SecureChannel::HandshakeClient() 会以
// kNoPinConfigured 失败，**不会**"第一次见到谁就信谁"。
struct ServerKeyPin {
  bool has_key = false;
  std::string public_key;       // 32 字节，has_key 为真时有效
  std::string fingerprint_hex;  // 64 个小写十六进制字符，永远有效
};

// 解析用户给的 pin 文本。接受的形式（前缀是**必须的**，避免把指纹当成
// 公钥这种危险的歧义）：
//   "sha256:<64 个十六进制字符>"   只 pin 指纹
//   "hex:<64 个十六进制字符>"      直接 pin 公钥（同时算出指纹）
// 其他形式（包括不带前缀的裸十六进制）一律失败并给出可照做的提示。
bool ParseServerKeyPin(const std::string& text, ServerKeyPin* out,
                       std::string* error_message);

// ---- BPSEC2：客户端侧的服务端身份策略 ----

// 用证书认证服务端时需要的全部输入。**没有** pin 字段：证书模式与 pin 模式
// 是两条互斥的路，不存在"证书验不过就退回 pin"这种降级分支。
struct ServerIdentityPolicy {
  // 可信根。空存储 = 什么都不信（构造后必须先 AddRoot 或 LoadFrom*）。
  crypto::TrustedRootStore roots;
  // 期望的 server_id，必须与证书里的逐字节相等。
  std::string expected_server_id;
  // 校验时间（Unix 秒）。0 = 用系统时钟。
  std::int64_t now_unix_seconds = 0;
};

// ---- 服务端长期身份密钥 ----

struct TransportIdentity {
  std::string private_key;  // 32 字节 X25519 标量（已 clamp）
  std::string public_key;   // 32 字节 X25519 公钥
  // BPSEC2：这张服务器身份证书（BPCERT1 原始字节，由离线根签发）。
  // 空 = 不提供证书，只能走 BPSEC1。证书里只有公钥材料，可以自由分发；
  // 私钥永远不在这里。
  std::string certificate;
};

// OS CSPRNG 生成一对身份密钥。
bool GenerateTransportIdentity(TransportIdentity* out,
                               std::string* error_message);

// 把身份私钥写成**只有所有者可读**的文件（O_NOFOLLOW + 0600 + 普通文件校验，
// 与 token secret 同一套规矩）。overwrite 为假时用 O_EXCL，已存在就失败。
bool SaveTransportIdentity(const std::string& path,
                           const TransportIdentity& identity, bool overwrite,
                           std::string* error_message);

// 读回身份密钥。文件必须是普通文件、属主可读写、长度恰好 32 字节。
bool LoadTransportIdentity(const std::string& path, TransportIdentity* out,
                           std::string* error_message);

// ---- 加密通道 ----

class SecureChannel {
 public:
  SecureChannel();
  ~SecureChannel();

  SecureChannel(const SecureChannel&) = delete;
  SecureChannel& operator=(const SecureChannel&) = delete;

  // 客户端握手：fd 必须是已经连上的 TCP socket。pin 必须已经配置好公钥或
  // 指纹；服务端身份与 pin 不符、Finished 校验失败、共享秘密退化，全部
  // 返回 false，并且调用方**必须**关掉这条连接（没有明文回退）。
  bool HandshakeClient(int fd, const ServerKeyPin& pin,
                       std::string* error_message);

  // BPSEC2 客户端握手：服务端必须出示由可信根签发的 BPCERT1 证书。
  // 校验顺序（任一步失败即终止，且**不会**退回 pin 模式或明文）：
  //   帧头 -> 解析证书 -> 在可信根里找 issuer -> 根签名 -> server_id ->
  //   有效期 -> 用途 -> 证书公钥 == 握手里的身份公钥 -> 低阶点 -> 密钥派生。
  bool HandshakeClientWithCertificate(int fd,
                                      const ServerIdentityPolicy& policy,
                                      std::string* error_message);

  // 服务端握手：使用长期身份私钥与一份新生成的临时密钥。
  // identity.certificate 非空时同时接受 BPSEC1 与 BPSEC2 客户端。
  bool HandshakeServer(int fd, const TransportIdentity& identity,
                       std::string* error_message);

  // 只接受 BPSEC2 的服务端握手（官方云端用）：收到 BPSEC1 的 ClientHello
  // 直接以 kDowngradeRefused 拒绝，绝不将就。identity.certificate 必须非空。
  bool HandshakeServerRequireCertificate(int fd,
                                         const TransportIdentity& identity,
                                         std::string* error_message);

  // 握手的**整体**时间预算（毫秒），0 = 不设限。调用方必须在握手前设置：
  // socket 上的 SO_RCVTIMEO 只约束单次 recv，对"每个超时周期挤 1 个字节"的
  // 未认证对端无效（审查轮缺陷 C）。超时按 kIoError 处理，并且直接关连接。
  void SetHandshakeTimeoutMs(std::uint64_t milliseconds) {
    handshake_timeout_ms_ = milliseconds;
  }

  bool established() const { return established_; }
  SecureTransportError last_error() const { return last_error_; }
  // 客户端：对端（服务端）这次握手实际出示的身份公钥，32 字节。
  const std::string& peer_public_key() const { return peer_public_key_; }
  // 客户端：对端身份公钥的指纹（64 个小写十六进制字符）。
  const std::string& peer_fingerprint() const { return peer_fingerprint_; }
  // BPSEC2：对端证书里的 server_id（只在证书握手校验通过后才有值）。
  const std::string& peer_server_id() const { return peer_server_id_; }
  // BPSEC2：对端证书的 SHA-256 指纹（64 个小写十六进制字符）。
  const std::string& peer_certificate_fingerprint() const {
    return peer_certificate_fingerprint_;
  }
  // 这次会话的握手 transcript 摘要（32 字节）。BPSEC1 是
  // SHA-256(ClientHello||ServerHello)；BPSEC2 还包含整张 ServerCertificate
  // 消息的原始字节。只用于日志/测试断言，不能当密钥用。
  const std::string& transcript_hash() const { return transcript_hash_; }
  std::uint64_t send_sequence() const { return send_sequence_; }
  std::uint64_t receive_sequence() const { return receive_sequence_; }

  // 换一条 TCP 连接时清空全部会话状态（密钥、序号、握手结果）。
  void Reset();

  // ---- 与 network_protocol.h 中同名函数**语义一致**的收发接口 ----
  //
  // 服务端与客户端只把 SendFrame/ReceiveFrame 换成这两个成员函数，
  // 业务代码不需要知道下面有加密层。
  bool SendFrame(int fd, std::uint16_t opcode, std::uint32_t status,
                 std::uint64_t request_id, const std::string& payload,
                 std::string* error_message);
  FrameReadStatus ReceiveFrame(int fd, FrameHeader* header,
                               std::string* payload,
                               std::string* error_message);

  // ---- 记录层原语（测试与诊断用；产品路径上是上面两个函数）----
  bool SendRecord(int fd, const std::string& plaintext,
                  std::string* error_message);
  FrameReadStatus ReceiveRecord(int fd, std::string* plaintext,
                                std::string* error_message);

  // 测试注入点：让下一次 SendRecord 故意把 tag 写坏（模拟被篡改的发送方）。
  // 产品代码从不设置它。
  void CorruptNextTagForTesting() { corrupt_next_tag_ = true; }

 private:
  // 两条握手路径共用同一份实现：pin/policy 恰有一个非空，两者都空属于
  // 编程错误（调用方只用下面两个公开入口，不会走到那里）。
  bool HandshakeClientInternal(int fd, const ServerKeyPin* pin,
                               const ServerIdentityPolicy* policy,
                               std::string* error_message);
  bool HandshakeServerInternal(int fd, const TransportIdentity& identity,
                               bool require_certificate,
                               std::string* error_message);
  bool DeriveKeys(const std::string& shared_secret,
                  const std::string& client_random,
                  const std::string& server_random, bool is_client,
                  std::string* error_message);
  bool Fail(SecureTransportError error, const std::string& detail,
            std::string* error_message);
  void ClearSecrets();

  bool established_ = false;
  bool corrupt_next_tag_ = false;
  SecureTransportError last_error_ = SecureTransportError::kNone;
  std::string send_key_;
  std::string send_mac_key_;
  std::string send_nonce_prefix_;
  std::string receive_key_;
  std::string receive_mac_key_;
  std::string receive_nonce_prefix_;
  // 握手完成密钥：握手期间用来算/验 Finished，握手成功后清零（见 Reset）。
  std::string client_finished_key_;
  std::string server_finished_key_;
  // 记录层 HMAC 输入里的方向字节（c2s = 1，s2c = 2）。两端的 send/receive
  // 方向互为镜像，由"我是客户端还是服务端"决定，不由对端声明决定。
  std::uint8_t send_direction_ = 0;
  std::uint8_t receive_direction_ = 0;
  std::uint64_t send_sequence_ = 0;
  std::uint64_t receive_sequence_ = 0;
  // 握手的整体预算（毫秒）。0 = 不设限，见 SetHandshakeTimeoutMs。
  std::uint64_t handshake_timeout_ms_ = 0;
  std::string peer_public_key_;
  std::string peer_fingerprint_;
  // BPSEC2 专有：对端证书里的 server_id 与整张证书的 sha256。
  std::string peer_server_id_;
  std::string peer_certificate_fingerprint_;
  std::string transcript_hash_;
};

}  // namespace net
}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_SECURE_TRANSPORT_H_
