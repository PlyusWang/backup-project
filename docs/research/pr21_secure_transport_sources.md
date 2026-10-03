# PR #21 资料与出处（BPSEC1 传输加密 / 远端增量）

本文件只记录**查了什么事实、事实来自哪份标准、哪个测试向量取自哪里**。
它不包含任何第三方实现源码，也不描述"参考了某库的写法"。

## 1. X25519（Curve25519 上的 Diffie-Hellman）

| 项目 | 内容 |
|---|---|
| 标准 | RFC 7748, *Elliptic Curves for Security*（IETF） |
| 查了什么 | §5 的 X25519 函数定义与 Montgomery ladder 伪代码；§5 的 decodeUCoordinate / decodeScalar25519（clamp）规则；§4.1 的曲线参数（p = 2^255 - 19、a24 = 121665）；§6.1 的测试向量 |
| 用到的测试向量 | §5.2 两条标量乘法向量（输入标量 / 输入 u 坐标 / 输出 u 坐标）；§5.2 的迭代向量（迭代 1 次与 1000 次的期望输出）；§6.1 的 Alice/Bob 私钥、公钥与共享秘密 |
| 代码位置 | include/x25519.h, src/crypto/x25519.cpp, tests/unit/x25519_hkdf_test.cpp |
| 采用方式 | 按标准给出的**算法定义**自行实现：有限域用 8 个 2^32 进制 limb（每次运算后归约到 < p），乘法是 8x8 教科书展开加 2^256 ≡ 38 的高位折叠，求逆用固定指数的平方-乘法（p-2 = 2^255-21 的二进制是低 5 位 01011、第 5..254 位全 1），ladder 用标准伪代码的步骤顺序、条件交换按位掩码实现 |

## 2. HKDF（基于 HMAC 的密钥派生）

| 项目 | 内容 |
|---|---|
| 标准 | RFC 5869, *HMAC-based Extract-and-Expand Key Derivation Function* |
| 查了什么 | §2.2 的 Extract（salt 缺省时用 HashLen 个 0 字节）与 §2.3 的 Expand（T(i) = HMAC(PRK, T(i-1) || info || i)，L ≤ 255·HashLen） |
| 用到的测试向量 | 附录 A 的 Test Case 1 / 2 / 3（SHA-256 的三组 PRK 与 OKM） |
| 代码位置 | include/hkdf.h, src/crypto/hkdf.cpp, tests/unit/x25519_hkdf_test.cpp |
| 采用方式 | 在本项目自己的 HMAC-SHA256（src/crypto/hmac.cpp）之上按定义实现；没有任何第三方 HKDF 代码 |

## 3. AES-256-CTR 与 HMAC-SHA256

| 项目 | 内容 |
|---|---|
| 标准 | NIST SP 800-38A（CTR 模式与计数器语义）、FIPS 197（AES）、FIPS 180-4（SHA-256）、RFC 2104（HMAC） |
| 查了什么 | CTR 模式"按块递增计数器、同一个 key 下计数器空间不得复用"的要求；128 位计数器块的自增语义 |
| 代码位置 | src/crypto/aes.cpp（已有实现，PR #21 未改动）、src/crypto/hmac.cpp、src/crypto/sha256.cpp |
| 采用方式 | 直接复用本项目自己已有的手写原语；BPSEC1 只决定**计数器块怎么拼**（nonce_prefix(4) || record_seq(8) || block_counter(4)），这一条是本项目自己的设计，不来自任何实现 |

## 4. 其他被查阅的规范 / 手册

| 主题 | 出处 | 查了什么 |
|---|---|---|
| 常量时间比较 | RFC 7748 §5（"implementations are encouraged to use a constant-time comparison"）；项目已有 crypto::ConstantTimeEquals | 为什么 pin 比对与 Finished 校验必须用定长比较 |
| 传输层认证加密的**设计原则** | RFC 8446（TLS 1.3）§2 与 §5.1-§5.3 **仅用于理解通用原则**：Encrypt-then-MAC / 方向分离的密钥 / 序号绑定 / transcript 绑定 | 本实现**不是** TLS，不实现 TLS 记录层，也不声称与 TLS 兼容；只借鉴公开教材级别的一般原则（先验后解、每方向独立密钥、序号进 MAC） |
| POSIX/Linux | man 2 open / fstat / read / write / fsync / rename / link；man 3 getrandom | O_NOFOLLOW/0600 的用法、原子发布的可行做法、唯一临时文件命名 |
| SQLite | 官方文档：ALTER TABLE、PRAGMA user_version、PRAGMA table_info、事务（BEGIN IMMEDIATE / ROLLBACK） | schema 迁移必须"只加列、不清库、可回滚、幂等" |

## 5. 测试向量的出处（明确标注）

* X25519：RFC 7748 §5.2 与 §6.1（标准正文里的十六进制常量，逐字节比对）。
* HKDF-SHA256：RFC 5869 附录 A 的 Test Case 1 / 2 / 3（含 PRK 与 OKM）。
* AES-256 / SHA-256 / HMAC 的向量：沿用项目既有测试（tests/unit/crypto_test.cpp，PR #12 时代加入的 FIPS/NIST/RFC 向量），本轮没有改动那些向量。
* BPSEC1 自己的握手/记录层：**没有**外部向量可言（这是本项目自定义的协议），因此用"性质测试"覆盖：真实 TCP 上的握手、线上字节里明文 marker 必须 0 次出现、篡改矩阵、重放/乱序/截断拒绝、错误 pin 拒绝、全零对端公钥拒绝。测试代码在 tests/unit/secure_transport_test.cpp。

## 6. 明确没有做的事

* 没有从任何第三方库（OpenSSL / LibreSSL / BoringSSL / libsodium / mbedTLS / wolfSSL / Crypto++ / Botan / Qt SSL / Boost.Asio SSL / libssh）复制、改编或链接任何实现代码；整个传输层只依赖 C++17 标准库、POSIX 与**本项目自己写的**密码学原语。
* 没有从博客、StackOverflow 或 RFC 附录的参考实现里抄代码：RFC 7748 的 ladder 以**伪代码形式**出现在标准正文中，本实现按该算法定义自行编写。
  数据结构与归约策略都是本项目自己的写法：有限域元素是 **8 个 2^32 进制 limb**（不是 5x51 / 10x25.5 之类的既有布局），乘法是 8x8 教科书展开后用 2^256 ≡ 38 折叠高位，进位链只用 uint64_t（**不依赖 __int128** 一类编译器扩展），求逆用固定指数的平方-乘法。
* 没有引入任何新的第三方依赖（Makefile 里的链接库与 PR #20 完全一致：只有 libsqlite3 与 pthread）。

No third-party implementation source was copied.
