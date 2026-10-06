// include/x25519.h
//
// 手写 X25519（RFC 7748）。
//
// 为什么手写：本项目的传输加密（BPSEC1）需要一条不依赖任何第三方密码库的
// 密钥交换路径。X25519 的全部有限域运算、Montgomery ladder、标量 clamp 与
// 编解码都在 src/crypto/x25519.cpp 里自己实现，只依赖 C++17 标准库与本项目
// 已有的 SHA-256 / OS CSPRNG（include/crypto.h）。
//
// 算法来源是**规范本身**：RFC 7748 "Elliptic Curves for Security" 第 5 节的
// X25519 函数定义与 ladder 伪代码、第 6.1 节的官方测试向量。没有复制任何
// 第三方实现源码（详见 docs/research/pr21_secure_transport_sources.md）。
//
// 表示与不变量（实现内部）：
//   * 有限域元素用 8 个 2^32 进制 limb 表示（小端在前），每次运算结束后都
//     归约成**规范值**（< p = 2^255 - 19）。因此减法可以安全地用
//     "借位则减去 38" 的补偿（2^256 ≡ 38 mod p）而不会二次借位。
//   * 乘法是 8x8 的教科书展开（16 个 limb），再用 2^256 ≡ 38 把高 8 个 limb
//     折回，全程只用 uint64_t：不使用 __int128 一类编译器扩展，也不依赖
//     任何未定义行为。
//   * ladder 里的条件交换按位掩码实现，不按秘密比特分支。
//
// 失败约定（与 include/crypto.h 一致）：
//   * 长度不合法一律显式失败，绝不静默截断或补零：返回 false 并写
//     error_message。
//   * X25519() 是**原始**标量乘法：低阶点（例如 u = 0）会得到全零输出，
//     这本身是合法的曲线运算结果，函数照样返回 true（RFC 7748 的测试向量
//     里就有这种输入）。要拒绝全零共享秘密的调用方请用
//     X25519SharedSecret()——握手路径用的就是它。

#ifndef BACKUP_PROJECT_INCLUDE_X25519_H_
#define BACKUP_PROJECT_INCLUDE_X25519_H_

#include <cstddef>
#include <string>

namespace backupproject {
namespace crypto {

// X25519 的标量（private key）与 u 坐标（public key）都是 32 字节。
inline constexpr std::size_t kX25519KeySize = 32;

// RFC 7748 第 5 节的 decodeScalar25519：k[0] &= 248; k[31] &= 127; k[31] |=
// 64。 就地修改 32 字节标量。这个操作是幂等的（对已经 clamp 过的标量再调用一次
// 结果不变），所以"文件里存 clamp 过的标量 + 使用时再 clamp"是安全的。
void X25519ClampScalar(unsigned char scalar[kX25519KeySize]);

// 原始 X25519：out = X25519(scalar, u)。两个输入都必须是 32 字节，否则返回
// false。输出恒为 32 字节（可能是全零，见文件头说明）。
bool X25519(const std::string& scalar, const std::string& u_coordinate,
            std::string* out, std::string* error_message);

// 会话用的版本：与 X25519() 相同，但**拒绝全零输出**（对端给了低阶点 /
// 小主子群元素时，共享秘密会退化成 0，继续用下去等于没有密钥交换）。
bool X25519SharedSecret(const std::string& scalar,
                        const std::string& u_coordinate, std::string* out,
                        std::string* error_message);

// 由 32 字节 private 材料推导 public key（base point u = 9）。输入不必预先
// clamp：函数内部会先 clamp。
bool X25519PublicKeyFromPrivate(const std::string& private_key,
                                std::string* public_key,
                                std::string* error_message);

// OS CSPRNG 生成一对密钥：32 字节随机数 -> clamp -> 推导公钥。
// private_key 是 32 字节原始标量（已 clamp），public_key 是 32 字节 u 坐标。
bool X25519GenerateKeyPair(std::string* private_key, std::string* public_key,
                           std::string* error_message);

// 公钥的展示用指纹：SHA-256(public_key) 的 64 个小写十六进制字符。
// 公钥不是秘密，指纹才是人眼比对用的短标识；私钥**没有**指纹接口。
// public_key 长度不是 32 时返回空串。
std::string X25519Fingerprint(const std::string& public_key);

// 解析文本形式的公钥：接受 "hex:<64 个十六进制字符>"，也接受不带前缀的
// 64 个十六进制字符（生成工具就是这么打印的）。长度不对、字符不合法、
// 带未知前缀都返回 false 并写 error_message。
bool X25519ParseKeyText(const std::string& text, std::string* out,
                        std::string* error_message);

// 32 字节公钥的十六进制展开（小写）。长度不对返回空串。
std::string X25519FormatKeyHex(const std::string& public_key);

}  // namespace crypto
}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_X25519_H_
