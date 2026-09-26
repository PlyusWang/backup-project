// container_format.h
//
// v2 外层容器（BKPCNT2）的格式定义与 header 编解码。
//
// 为什么要有外层容器：打包、压缩、加密是三层互相不知道对方存在的东西。
// PackMethod 只知道路径和 uid，CompressionMethod 只知道一段字节，
// EncryptionMethod 也只知道一段字节。要把它们串起来，就需要一个"我用了哪三种
// 算法、各阶段多大、salt/IV 是什么、认证标签是多少"的清单——这就是这 160 字节。
//
// 三种算法各有一个 id，缺一不可：restore 靠它决定调哪条解码路径，
// 而不是靠扩展名或"猜"。
//
// 安全措辞：本模块是课程项目的手写实现，未经专业密码学审计。
// DES-CBC 明确是 legacy / educational，不要用于任何真实场景；
// AES-256-CTR + HMAC-SHA256 + PBKDF2-HMAC-SHA256 + random salt/IV +
// Encrypt-then-MAC 是本项目能给出的最强组合，但它同样不是经过审计的密码学产品。

#ifndef BACKUP_PROJECT_INCLUDE_CONTAINER_FORMAT_H_
#define BACKUP_PROJECT_INCLUDE_CONTAINER_FORMAT_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include "pack_stream.h"

namespace backupproject {

// 压缩策略 id。数值就是 v2 container header 里的 compression_method 字段。
enum class CompressionMethod : std::uint8_t {
  kNone = 0,
  kHuffman = 1,
  kLzssHuffman = 2,
};

// 加密策略 id。数值就是 v2 container header 里的 encryption_method 字段。
enum class EncryptionMethod : std::uint8_t {
  kNone = 0,
  kDesCbcHmacSha256 = 1,
  kAes256CtrHmacSha256 = 2,
};

const char* CompressionMethodName(CompressionMethod method);
const char* EncryptionMethodName(EncryptionMethod method);
bool ParseCompressionMethodId(std::uint8_t id, CompressionMethod* method);
bool ParseEncryptionMethodId(std::uint8_t id, EncryptionMethod* method);

namespace container_v2 {

inline constexpr std::size_t kHeaderSize = 160;
inline constexpr std::uint16_t kVersion = 2;
inline constexpr std::uint16_t kHeaderSizeField = 160;

inline constexpr unsigned char kMagic[8] = {'B', 'K', 'P', 'C',
                                            'N', 'T', '2', '\0'};
inline constexpr std::size_t kMagicSize = sizeof(kMagic);

inline constexpr std::size_t kSaltFieldSize = 16;
inline constexpr std::size_t kIvFieldSize = 16;
inline constexpr std::size_t kAuthTagSize = 32;
inline constexpr std::size_t kSha256Size = 32;
inline constexpr std::size_t kReservedSize = 8;

inline constexpr std::size_t kSaltOffset = 56;
inline constexpr std::size_t kIvOffset = 72;
inline constexpr std::size_t kAuthTagOffset = 88;
inline constexpr std::size_t kPayloadSha256Offset = 120;
inline constexpr std::size_t kReservedOffset = 152;

// PBKDF2 的 production 轮数。DES 与 AES 都用它；格式里仍然存了这个数字，
// 读侧按"必须等于本版本约定的值"校验，而不是"随便读一个数就用"。
inline constexpr std::uint32_t kProductionIterations = 200000;
// PBKDF2 派生 64 字节，再按算法切分（见 DeriveKeys）。
inline constexpr std::size_t kDerivedKeySize = 64;

// 单个整段流允许的最大字节数：uint64 字段本身没有上限，但把 2^63 这样的
// 数字当成"可信长度"去分配内存是另一种漏洞。这个上界只是"显然不合理"的拦截。
inline constexpr std::uint64_t kMaxStreamSize = 1ull << 42;  // 4 TiB

}  // namespace container_v2

// 容器 header 的字段值。salt/iv/auth_tag/payload_sha256 都用原始字节
// （不是十六进制），长度由 salt_len / iv_len / tag_len 与算法共同决定。
struct ContainerHeader {
  std::uint8_t pack_method = 0;
  std::uint8_t compression_method = 0;
  std::uint8_t encryption_method = 0;
  std::uint8_t flags = 0;
  std::uint64_t entry_count = 0;
  std::uint64_t packed_size = 0;
  std::uint64_t compressed_size = 0;
  std::uint64_t payload_size = 0;
  std::uint32_t kdf_iterations = 0;
  std::uint8_t salt_len = 0;
  std::uint8_t iv_len = 0;
  std::uint8_t tag_len = 0;
  std::uint8_t reserved0 = 0;
  std::string salt;
  std::string iv;
  std::string auth_tag;
  std::string payload_sha256;
};

// 编码成 160 字节。所有保留区、未使用的 salt/iv/tag 尾部都写成 0。
bool EncodeContainerHeader(const ContainerHeader& header, std::string* out,
                           std::string* error_message);

// 解码 + 校验。校验分两层：格式层（magic / version / header_size / flags /
// 保留字节全 0、三个 method id 都是已知取值）与语义层（算法与
// kdf_iterations / salt_len / iv_len / tag_len 的组合必须自洽，
// 三个 size 字段之间的关系必须成立）。
bool DecodeContainerHeader(const unsigned char* block, std::size_t size,
                           ContainerHeader* header, std::string* error_message);

// 按"未知密码也能读"的方式打开一个 v2 容器，只读 header。
bool InspectContainerFile(const std::string& path, ContainerHeader* header,
                          std::string* error_message);

// 这段字节是不是以 BKPCNT2 magic 开头。
bool LooksLikeContainer(const unsigned char* data, std::size_t size);

// HMAC 的输入是"归一化 header + 密文 payload"：归一化 = auth_tag[32] 全部当 0，
// 其它字段（含 sizes、salt、iv、payload_sha256）全部参与。
std::string NormalizedHeaderForMac(const ContainerHeader& header);

// 由 PBKDF2 的 64 字节派生结果按算法切分密钥。
//   AES：前 32 字节 → AES-256 密钥，后 32 字节 → HMAC 密钥
//   DES：前 8 字节  → DES 密钥，    后 32 字节 → HMAC 密钥
bool DeriveKeys(const std::string& password, const ContainerHeader& header,
                std::string* cipher_key, std::string* mac_key,
                std::string* error_message);

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_CONTAINER_FORMAT_H_
