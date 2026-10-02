# BPSEC1：本项目的认证加密传输层

> 一句话：BPNET1 业务协议跑在 BPSEC1 之上，**每个字节**（包括帧头里的 opcode、
> status 与长度）都被加密并带认证标签；服务端有长期身份密钥，客户端必须事先
> pin 住它。这是**教学用途的手写实现**，没有经过外部审计。

## 1. 它在哪一层

    BPNET1 帧（32 字节帧头 + payload）        <-- 业务协议（PR #20 定义，几乎没改）
              |
              v  整帧作为明文
    BPSEC1 记录（序号 + 密文 + HMAC-SHA256）
              |
              v
    TCP socket

只有 `RemoteArchiveClient::Request()` 与 `RemoteServer::ServeConnection()` 这两处
"帧 -> 记录"的翻译点。业务处理函数（REGISTER / LOGIN / RESUME / LIST / UPLOAD /
DOWNLOAD / DELETE / DELETE_ACCOUNT）完全不知道自己跑在加密层之上。

## 2. 握手

    客户端                                                  服务端
      |-- ClientHello --------------------------------------->|   magic/版本/套件
      |   client_random(32) + client_ephemeral_pub(32)        |   72 字节
      |                                                        |
      |<--------------------------------------- ServerHello ---|   magic/版本/套件
      |   server_random(32) + server_static_pub(32)            |   104 字节
      |   + server_ephemeral_pub(32)                           |
      |                                                        |
      |   校验 server_static_pub 与本地 pin（公钥或 SHA-256 指纹）
      |   不符 -> 立刻断开（不发送任何业务字节）
      |                                                        |
      |-- ClientFinished ------------------------------------>|   HMAC(client_finished_key, transcript)
      |<-------------------------------------- ServerFinished --|   HMAC(server_finished_key, transcript || CF)

* `transcript = SHA256(ClientHello || ServerHello)`，两端的 Finished 都由它派生，
  因此任何一方改过握手字节，双方都会在 Finished 校验上失败。
* 共享秘密 = `X25519(client_ephemeral_priv, server_static_pub) || X25519(client_ephemeral_priv, server_ephemeral_pub)`
  （服务端用 `X25519(server_static_priv, client_ephemeral_pub)` 与
  `X25519(server_ephemeral_priv, client_ephemeral_pub)` 得到同样的两份值）。
  任一份为全零（对端给了低阶点）就拒绝。
* **static DH 提供身份绑定**（只有持有服务端身份私钥的一方能算出它），
  **ephemeral DH 提供每连接的新鲜性**：会话密钥需要两边的临时私钥才能算出，
  所以长期身份私钥事后泄漏也不能解开历史会话。

## 3. 密钥派生

    PRK = HKDF-Extract(salt = client_random || server_random,
                       IKM  = DH_static || DH_ephemeral)
    然后按 8 个互不相同的 info 标签各 Expand 一次：

      "BPSEC1 c2s enc" (32)   "BPSEC1 s2c enc" (32)
      "BPSEC1 c2s mac" (32)   "BPSEC1 s2c mac" (32)
      "BPSEC1 c2s nonce" (4)  "BPSEC1 s2c nonce" (4)
      "BPSEC1 client finished" (32)   "BPSEC1 server finished" (32)

方向分离与用途分离是**结构性**的：同一个密钥既做 AES 又做 HMAC 的情况不存在，
两个方向复用同一把密钥的情况也不存在。

## 4. 记录层

    offset  size  字段
    0       4     magic "BPS1"
    4       1     类型 = 5（记录）
    5       1     版本 = 1
    6       2     保留（必须为 0）
    8       8     序号（每方向从 0 开始，严格 +1）
    16      4     密文长度（<= 32 + 1 MiB）
    20      N     密文（AES-256-CTR）
    20+N    32    HMAC-SHA256

* 计数器块 = `nonce_prefix(4) || record_sequence(8) || block_counter(4)`，
  每条记录的 `block_counter` 都从 0 开始，而 `record_sequence` 单调递增，
  所以**不同记录的 AES 计数器空间绝不重叠**。（如果简单地做 IV = base + seq 再让
  128 位计数器自己跨块累加，record N 的后半段就会撞上 record N+1 的开头。）
* 认证标签覆盖：`HMAC(mac_key, "BPSEC1 record v1" || direction || sequence || ciphertext_length || ciphertext)`
  —— Encrypt-then-MAC，接收端**先验标签再解密**；标签不对时一个字节的明文都不会
  交出去，连接直接被判为不可信。
* 抗重放：每个方向维护期望序号，收到不等于期望值的记录（重放、跳号、乱序、
  丢包）一律断开。序号耗尽（2^64-1）要求重新握手。

## 5. 服务端身份与 pin（没有 TOFU）

* 服务端有一对长期 X25519 身份密钥，私钥存在服务器本机的 0600 文件里
  （`--transport-key-file`，O_NOFOLLOW、必须是普通文件），由
  `backup-server-keygen` 生成；它只在服务器上存在，不进 Git、不进日志、不进 ZIP。
* 客户端必须**事先**知道服务端身份公钥：`--server-key sha256:<指纹>` 或
  `--server-key hex:<公钥>`（CLI），或环境变量 `BACKUP_REMOTE_SERVER_KEY`。
  没有配置就明确报错（`kNoPinConfigured`），一个字节都不会发出去。
* 本实现**不做**"第一次见到谁就信谁"：那等于把中间人攻击变成默认行为。
* 公钥与指纹不是秘密，可以写进配置、日志与界面；私钥永远不。

## 6. 明确**不**声称的东西

* 不是 TLS，也不与 TLS 兼容；不实现 TLS 记录层、密码套件协商、会话票据、0-RTT 等。
* 没有经过第三方安全审计，没有形式化验证，不是"生产可直接上线"的协议。
* 没有做抗侧信道的形式化分析（有限域运算按固定次数执行、条件交换用掩码，
  但这不是常数时间性的证明）。
* 没有对流量做长度隐藏：记录长度本身是可见的（与 TLS 一样）。
* 前向保密的范围：会话密钥需要临时私钥，因此长期身份私钥泄漏不影响历史会话；
  但一台被完全攻陷的**端点**（能读到进程内存）当然能看到明文。

## 7. 错误分类（界面与日志共用）

`include/secure_transport.h` 里的 `SecureTransportError` 把失败分成
io-error / malformed-message / unsupported-version / server-key-mismatch /
handshake-authentication-failed / weak-shared-secret / no-server-key-pin /
record-authentication-failed / record-replay-detected / oversized-record /
channel-state-error / crypto-failure。CLI 与 GUI 直接使用这份分类的中文说明，
不再自己翻译一遍，也不会把密钥材料写进任何错误信息。

## 8. 代码与测试入口

| 文件 | 作用 |
|---|---|
| include/x25519.h, src/crypto/x25519.cpp | 手写 X25519（RFC 7748） |
| include/hkdf.h, src/crypto/hkdf.cpp | 手写 HKDF-SHA256（RFC 5869） |
| include/secure_transport.h, src/network/secure_transport.cpp | BPSEC1 握手 + 记录层 + 身份密钥文件 |
| server/keygen_main.cpp | backup-server-keygen 工具 |
| tests/unit/x25519_hkdf_test.cpp | 官方向量、随机对称性、退化输入 |
| tests/unit/secure_transport_test.cpp | 握手、线上字节捕获、篡改矩阵、pin 校验 |
| scripts/secure_transport_test.sh | 上面两个测试的入口（已接入 canonical gate） |

## 9. 与 SSH 隧道的关系

SSH 隧道**保留**，但它不再是机密性的来源。BPSEC1 之后：

* BPNET1 的业务流量由 BPSEC1 保护（端到端：客户端 <-> backup-server）；
* SSH 隧道是**部署层的纵深防御**（访问控制、端口不暴露、审计），并且继续是
  ECS 上访问服务端的路径；
* 服务端仍然只绑 127.0.0.1：把只该由隧道访问的端口暴露在共享网络上没有任何
  好处，所以这条 fail-closed 规则没有放宽。

## 10. 威胁模型（谁能做什么，谁做不到什么）

**假设**

* OS CSPRNG（getrandom / /dev/urandom）可信。
* 服务器上那份 0600 的身份私钥文件只有服务端进程（与 root）能读。
* 客户端与服务端的时间不需要同步：抗重放靠的是**每连接的记录序号**，不是时间戳。
* 客户端的 pin 是通过可信渠道拿到的（管理员当面 / 带外告知）。本项目禁止 TOFU，
  但**无法**阻止用户从不可信渠道复制一个攻击者的 pin —— 那等于自愿信任那个密钥。

**能防住的（每一条都有对应的自动化测试）**

| 威胁 | 依赖的性质 | 证据 |
|---|---|---|
| 被动窃听（口令、token、用户名、快照元数据、归档字节） | 整帧加密（AES-256-CTR） | tests/unit/secure_transport_test.cpp 的线上字节捕获：已知 marker 在真实 TCP 字节里 0 次出现 |
| 主动篡改（改密文、改 tag、改长度、截断、追加） | Encrypt-then-MAC，先验后解 | 篡改矩阵逐项拒绝，且被拒绝时不交出任何明文 |
| 重放 / 乱序 / 丢包 | 每方向严格递增的序号 | 重放同一条记录被拒；seq+2 被拒 |
| 冒充服务端（中间人） | 长期 X25519 身份 + 客户端 pin | 错误 pin -> kServerKeyMismatch，双方都不建立会话 |
| 握手被改写 | transcript（CH 串接 SH）绑定两个 Finished | 改 CH / SH / 任一 Finished 都失败 |
| 降级到明文 | **代码里没有明文分支** | 缺 pin 的客户端拒绝连接；旧客户端打新服务端失败；新客户端打旧服务端失败 |
| 前向保密（长期私钥事后泄漏） | 会话密钥需要临时私钥 | KDF 的 IKM 同时包含 static DH 与 ephemeral DH |
| 用别的东西替换服务器上的 blob | 客户端对**实际字节**做 SHA-256 | 篡改服务端 blob 后恢复必须失败（端到端用例） |

**防不住的 / 明确不在范围内的**

* **端点被攻陷**：进程内存里有明文与会话密钥，root 能读 0600 文件。协议解决不了这个。
* **流量分析**：记录长度可见（与 TLS 一样），连接的时间与字节数模式可见。
* **拒绝服务**：任何人都能连上来并占着一个 worker 直到 io-timeout（默认 30 秒）。
  并发上限是 --workers（默认 4），影响因此有界，但没有做速率限制或连接前挑战。
* **服务端身份轮换**：没有在线轮换协议；换密钥需要重新分发 pin。backup-server-keygen
  也**拒绝**覆盖已有私钥，正是为了避免"悄悄换掉一个在服役的身份"。
* **客户端身份**：BPSEC1 只认证服务端；客户端是谁由 BPNET1 的登录（口令 + token）回答。
  这与 TLS 里"只做服务器认证、应用层再登录"是同一种分工。
* **侧信道**：有限域运算按固定次数执行、条件交换用掩码、标签比较用定长比较，
  但没有做形式化的常数时间分析，也没有做功耗 / 缓存计时实验。
* **pin 的可用性**：pin 只能靠命令行参数、环境变量或 GUI 输入框提供，
  没有证书固定那样的分发机制。
