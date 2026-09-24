// include/crypto.h
//
// 手写密码学原语：SHA-256、HMAC-SHA256、PBKDF2-HMAC-SHA256、DES-CBC、
// AES-256、AES-256-CTR，外加 OS 随机数与定长比较工具。
//
// 为什么手写：归档容器的密钥派生与分组加密属于容器格式的一部分，交给第三方库
// 会把"格式 ↔ 实现"绑死在外部版本上，也没法逐表对照标准复核。本模块只依赖
// C++17 标准库和 POSIX（getrandom / /dev/urandom）：S-box、置换表、轮常量
// 全部以 constexpr 数组写在 .cpp 里，可对照 FIPS 180-4、RFC 2104、RFC 8018、
// FIPS 46-3、FIPS 197、NIST SP 800-38A 逐条检查。
//
// 边界与失败约定（调用方必须知道）：
//   * 长度非法的输入一律显式失败，绝不静默截断、补零或"尽力而为"：
//     返回 bool 的接口写 error_message，返回 std::string 的接口返回空串。
//   * 流式类（Sha256/HmacSha256/DesCbc*/Aes256Ctr）允许把输入任意切分，
//     分块调用与一次性调用的结果必须逐字节相同；Finish 只能调用一次。
//   * 本模块不提供认证加密：CBC/CTR 只给保密性，完整性必须由调用方另外
//     叠加 HMAC-SHA256，且两个密钥必须相互独立。
//   * DES 有效密钥强度只有 56 位，仅为兼容既有容器保留，新数据请用 AES-256。
//   * 对象内部保存密钥材料（轮密钥、链值），析构时会清零；但 C++ 的拷贝
//     语义会复制这些材料，调用方不要把加密对象随意传来传去。

#ifndef BACKUP_PROJECT_INCLUDE_CRYPTO_H_
#define BACKUP_PROJECT_INCLUDE_CRYPTO_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace backupproject {
namespace crypto {

// SHA-256 摘要长度（FIPS 180-4）。
inline constexpr std::size_t kSha256DigestSize = 32;
// SHA-256 分组长度。HMAC 用它判断"密钥是否需要先哈希一次"。
inline constexpr std::size_t kSha256BlockSize = 64;
// DES 密钥与分组长度（FIPS 46-3）。
inline constexpr std::size_t kDesKeySize = 8;
inline constexpr std::size_t kDesBlockSize = 8;
// AES 分组长度与两种公开密钥长度（FIPS 197）。
inline constexpr std::size_t kAesBlockSize = 16;
inline constexpr std::size_t kAes128KeySize = 16;
inline constexpr std::size_t kAes256KeySize = 32;

// ---- 十六进制工具 ----

// 小写十六进制表示。data == nullptr 或 size == 0 时返回空串。
std::string ToHex(const unsigned char* data, std::size_t size);
// 解析十六进制（大小写都接受）。长度为奇数、含非十六进制字符、out == nullptr
// 时返回 false，并且不改动 *out——调用方可以放心复用旧值。
bool FromHex(const std::string& hex, std::string* out);

// ---- SHA-256（FIPS 180-4，支持流式）----
class Sha256 {
 public:
  Sha256();

  // 追加数据。data == nullptr 或 size == 0 时不做任何事（空消息合法）。
  void Update(const void* data, std::size_t size);

  // 补位、写入 64 位大端 bit 长度并输出摘要。只能调用一次；out == nullptr
  // 时直接返回。调用后对象状态未定义，要继续用请新建对象。
  void Final(unsigned char out[kSha256DigestSize]);

  static void Hash(const void* data, std::size_t size,
                   unsigned char out[kSha256DigestSize]);

 private:
  void Transform(const unsigned char block[kSha256BlockSize]);

  std::uint32_t state_[8];
  unsigned char buffer_[kSha256BlockSize];
  std::size_t buffer_size_;   // buffer_ 中待处理的字节数，恒 < 64
  std::uint64_t total_bits_;  // 已吸收的 bit 数（按 2^64 取模）
};

std::string Sha256Raw(const std::string& data);  // 32 字节原始摘要
std::string Sha256Hex(const std::string& data);  // 64 个小写十六进制字符

// ---- HMAC-SHA256（RFC 2104，支持流式）----
// 构造时就把 ipad/opad 吸收进两个哈希状态，因此后续 Update 大 payload 不需要
// 重新做密钥填充；密钥超过 64 字节时按标准先哈希压缩。
class HmacSha256 {
 public:
  explicit HmacSha256(const std::string& key);
  HmacSha256(const void* key, std::size_t key_size);

  void Update(const void* data, std::size_t size);
  // 只能调用一次；out == nullptr 时直接返回。
  void Final(unsigned char out[kSha256DigestSize]);

  static void Compute(const void* key, std::size_t key_size, const void* data,
                      std::size_t data_size,
                      unsigned char out[kSha256DigestSize]);

 private:
  Sha256 outer_;  // 已吸收 opad 的外层哈希
  Sha256 inner_;  // 已吸收 ipad 的内层哈希
};

std::string HmacSha256Raw(const std::string& key, const std::string& data);
std::string HmacSha256Hex(const std::string& key, const std::string& data);

// ---- PBKDF2-HMAC-SHA256（RFC 8018 5.2）----
// 成功返回 true 并把 derived_key_size 字节写进 *out；失败返回 false 并在
// error_message 里说明原因（iterations == 0、derived_key_size == 0、
// 输出指针为空、长度超出上限）。error_message 可以为 nullptr。
bool Pbkdf2HmacSha256(const std::string& password, const std::string& salt,
                      std::uint32_t iterations, std::size_t derived_key_size,
                      std::string* out, std::string* error_message);

// ---- 定长比较 ----

// 长度由调用方给定：size == 0 视为相等；size > 0 且任一指针为空返回 false。
// 循环里用 volatile 累加，避免编译器把比较提前短路成"发现不同就退出"。
bool ConstantTimeEquals(const void* a, const void* b, std::size_t size);
// 带长度的重载：长度不同立即返回 false，长度相同走定长比较。
bool ConstantTimeEquals(const std::string& a, const std::string& b);

// ---- OS CSPRNG ----

// 优先 getrandom(2)，不可用时回退 /dev/urandom。成功返回 true 并把 size 字节
// 写进 *out（size == 0 时 *out 为空串）；两条路径都失败时返回 false 并写
// error_message（其中会同时带上 getrandom 与回退失败的原因）。
bool RandomBytes(std::size_t size, std::string* out,
                 std::string* error_message);

// 只走 /dev/urandom。RandomBytes 在 getrandom 不可用时自动回退到它；单独暴露
// 是为了让单元测试能直接验证回退实现（老内核或被 seccomp 限制的沙箱里，
// 这条路径就是唯一的熵来源）。
bool RandomBytesFromUrandom(std::size_t size, std::string* out,
                            std::string* error_message);

// ---- DES（FIPS 46-3）：密钥 8 字节，单块 8 字节 ----
// 密钥/数据长度不合法时返回空串（没有错误通道，调用方必须自己判长度）。
std::string DesBlockEncrypt(const std::string& key8, const std::string& block8);
std::string DesBlockDecrypt(const std::string& key8, const std::string& block8);

// 流式 DES-CBC + PKCS#7。Process 可以任意切分输入，内部缓冲不足一个块的
// 尾巴；Finish 里补/去 padding 并输出最后一个块。
class DesCbcEncryptor {
 public:
  DesCbcEncryptor(const std::string& key8, const std::string& iv8);
  ~DesCbcEncryptor();

  // 长度非法（对象 !valid()）、out == nullptr、size == 0 时不做任何事。
  void Process(const void* data, std::size_t size, std::string* out);
  // 输出 PKCS#7 补齐后的尾块。只能成功调用一次。
  bool Finish(std::string* out, std::string* error_message);
  bool valid() const { return valid_; }

 private:
  void EncryptOneBlock(const unsigned char in[kDesBlockSize],
                       unsigned char out[kDesBlockSize]);

  std::uint64_t subkeys_[16];
  unsigned char chain_[kDesBlockSize];   // 上一块密文（CBC 链值）
  unsigned char buffer_[kDesBlockSize];  // 不足一块的尾巴
  std::size_t buffer_size_;
  bool finished_;
  bool valid_;
};

class DesCbcDecryptor {
 public:
  DesCbcDecryptor(const std::string& key8, const std::string& iv8);
  ~DesCbcDecryptor();

  // 与加密侧对称：内部只保留最后一块明文，等 Finish 确认 padding 后才输出，
  // 因此 padding 非法时不会把未经验证的明文交出去。
  void Process(const void* data, std::size_t size, std::string* out);
  // 去掉 PKCS#7。密文长度不是 8 的倍数、密文为空、padding 非法、
  // 长度/密钥非法、重复调用都返回 false。
  bool Finish(std::string* out, std::string* error_message);
  bool valid() const { return valid_; }

 private:
  void DecryptOneBlock(const unsigned char in[kDesBlockSize],
                       unsigned char out[kDesBlockSize]);

  std::uint64_t subkeys_[16];
  unsigned char chain_[kDesBlockSize];
  unsigned char buffer_[kDesBlockSize];
  unsigned char pending_[kDesBlockSize];  // 已解密但还没交出去的尾块
  std::size_t buffer_size_;
  bool has_pending_;
  bool finished_;
  bool valid_;
};

// 便捷的一次性接口。失败时把 *out 清空并写 error_message。
bool DesCbcEncrypt(const std::string& key8, const std::string& iv8,
                   const std::string& plaintext, std::string* out,
                   std::string* error_message);
bool DesCbcDecrypt(const std::string& key8, const std::string& iv8,
                   const std::string& ciphertext, std::string* out,
                   std::string* error_message);

// ---- AES（FIPS 197）----

// AES-256 分组加密。密钥长度必须是 32 字节：不合法时 valid() == false，
// 且 EncryptBlock/DecryptBlock 只写 16 个 0，绝不读越界的内存。
// 这里不用 assert 中断进程——容器解析失败要能回退成错误码，而不是把程序打挂。
class Aes256 {
 public:
  explicit Aes256(const std::string& key32);
  ~Aes256();

  void EncryptBlock(const unsigned char in[kAesBlockSize],
                    unsigned char out[kAesBlockSize]) const;
  void DecryptBlock(const unsigned char in[kAesBlockSize],
                    unsigned char out[kAesBlockSize]) const;
  bool valid() const { return valid_; }

 private:
  unsigned char round_keys_[240];  // 15 组轮密钥（AES-256 共 14 轮）
  int rounds_;
  bool valid_;
};

// 单块接口。长度不合法（key32 != 32、block16 != 16）时返回空串。
std::string Aes256BlockEncrypt(const std::string& key32,
                               const std::string& block16);
std::string Aes256BlockDecrypt(const std::string& key32,
                               const std::string& block16);
// AES-128 单块接口。容器只用 AES-256，但保留 128 位密钥是为了能用 FIPS 197
// C.1 的官方向量独立验证 S-box / 行移位 / 列混合本身没写错。
std::string Aes128BlockEncrypt(const std::string& key16,
                               const std::string& block16);
std::string Aes128BlockDecrypt(const std::string& key16,
                               const std::string& block16);

// 流式 AES-256-CTR：counter 是 16 字节初始计数器，按大端整数自增
// （NIST SP 800-38A 约定）。加密与解密是同一个操作。
// 密钥或 IV 长度非法时对象失效，Process 不再产生任何输出。
class Aes256Ctr {
 public:
  Aes256Ctr(const std::string& key32, const std::string& iv16);
  ~Aes256Ctr();

  void Process(const void* data, std::size_t size, std::string* out);
  bool valid() const { return valid_; }

 private:
  void GenerateKeystream();
  void IncrementCounter();

  Aes256 cipher_;
  unsigned char counter_[kAesBlockSize];
  unsigned char keystream_[kAesBlockSize];
  std::size_t keystream_pos_;  // keystream_ 中已消费的字节数，16 表示用完
  bool valid_;
};

// 一次性 CTR。密钥/IV 长度非法时返回空串。
std::string Aes256CtrCrypt(const std::string& key32, const std::string& iv16,
                           const std::string& data);

}  // namespace crypto
}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_CRYPTO_H_
