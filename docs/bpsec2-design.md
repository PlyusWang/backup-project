# BPSEC2 设计（BPSEC1 + 服务器身份证书）

本文是 **施工图**，不是"已完成"的说明。已经实现的部分见
`docs/release-layout.md` 与各模块头注释；BPSEC2 本身的状态写在文末。

## 1. 目标与硬约束

BPSEC2 要在**不动 BPSEC1 线格式**的前提下，把"服务器身份"从"人工 pin 一个
指纹"换成"一张由离线根签发的证书"。

硬约束（来自 PR #23 的范围）：

1. BPSEC1 的帧格式、握手消息、记录层、标签一字不改；官方云端模式
   **只接受 BPSEC2**；
2. **fail closed**：证书校验失败时不得回退到 BPSEC1、不得回退到人工 pin、
   不得 TOFU（首次信任）——否则就是一条现成的降级攻击路径；
3. transcript 必须绑定：协议版本、认证方式、证书**全部字节**、证书签名、
   server_id、被认证的公钥、双方随机数、双方临时公钥；
4. 用户侧不需要知道 SSH、隧道、指纹、X25519、Root Key、证书这些词。

## 2. 现状地图（已核对的代码位置）

BPSEC1 握手是 4 条定长消息（都在 `src/network/secure_transport.cpp` 的匿名
命名空间里构造）：

| 消息 | 方向 | 长度 | 构造 / 校验 |
|---|---|---|---|
| ClientHello（type=1） | C→S | 72 | `BuildClientHello` :154；服务端解析 :893-919 |
| ServerHello（type=2） | S→C | 104 | `BuildServerHello` :167；客户端解析 :786-791 |
| ClientFinished（type=3） | C→S | 40 | `BuildFinished` :182；服务端校验 :972-983 |
| ServerFinished（type=4） | S→C | 40 | 客户端校验 :854-865 |

* 头部校验：`CheckMessageHeader` :194（**强制精确长度**，因此变长消息需要新的
  辅助函数）；版本检查 :222-227；suite 检查：客户端 :780、服务端 :910；
* 常量：`include/secure_transport.h` :53-76（magic `BPS1` / version 1 /
  suite 1 / 消息类型 1-4 / 各消息长度 / HKDF label）；
* **transcript = SHA256(ClientHello ‖ ServerHello)**，只有这一处定义：
  客户端 :834、服务端 :962；
* Finished：`HMAC(cfk, transcript)` :836/:979；
  `HMAC(sfk, transcript ‖ CF)` :991/:861；失败即 `kAuthenticationFailed`
  且 `Fail()` :668 强制 `established_ = false`；
* 客户端身份校验（今天）：:793-808 —— 先比 pin 公钥再比指纹，不一致 →
  `kServerKeyMismatch`；没有 pin 则**在发任何字节之前**就
  `kNoPinConfigured`（:735-738）；调用方
  `src/network/remote_backup_client.cpp` :281-292 直接关连接，无明文回退；
* 服务端身份：`RemoteServer::LoadTransportIdentityKey`
  (`server/remote_server.cpp` :430) → `LoadTransportIdentity` :516 ——
  严格 32 字节、0600、公钥由私钥**推导**（:625）；服务端只知道私钥/公钥
  和自己的指纹（:436），**没有**证书、序列号、issuer、server_id；
* 记录层（不在握手内，BPSEC2 不碰）：:1041-1056；
* 既有测试：`scripts/secure_transport_test.sh` + `tests/unit/secure_transport_test.cpp`
  （15 个用例，覆盖真实 TCP 握手、线上无明文、记录层篡改矩阵、握手变异、
  畸形 fuzz、服务端必须证明持有私钥、分片、半关闭、握手超时等）。
  **这些用例必须继续原样通过** —— 它们就是"BPSEC1 没被改坏"的证据。

## 3. 设计决定

### 3.1 判别方式：同 magic，版本升到 2

沿用 magic `BPS1`（4 字节）与既有帧头布局，把 `version` 从 1 提到 **2**，
新增一个消息类型 `type = 6 = ServerCertificate`。理由：

* 版本字段本来就在每条握手消息的固定位置，客户端可以先读完头部再决定
  接不接受，不需要"猜测对端说的是哪套协议"；
* 服务端可以按 ClientHello 的版本字段**显式区分**：版本 2 走 BPSEC2 流程，
  版本 1 走 BPSEC1；配置成"只允许 BPSEC2"时，收到版本 1 直接拒绝并记审计，
  这就是 fail closed 的落点；
* 不新开 magic，避免"两个几乎一样的协议"长期并存带来的混淆。

### 3.2 证书怎么进握手：新增 type=6，放在 ServerHello 之后

ServerCertificate 消息布局（大端，变长但有上限）：

    magic 4 | type 1 (=6) | version 1 (=2) | reserved 2 (=0)
    | certificate_length u32 | certificate bytes（BPCERT1，<= 4096）

顺序变成：ClientHello → ServerHello → **ServerCertificate** → ClientFinished
→ ServerFinished。放在 ServerHello 之后的好处是客户端在**派生任何会话密钥、
发出任何已认证字节之前**就完成了身份判断。

### 3.3 transcript：把整张证书算进去

    transcript = SHA256(ClientHello ‖ ServerHello ‖ ServerCertificate)

三个消息都是**收到的原始字节**。于是：

* 协议版本、认证方式 → 在三条消息的 version 字段里；
* 证书全部字节、证书签名、server_id、被认证的公钥 → 在证书里；
* 双方随机数、双方临时公钥 → 在 ClientHello / ServerHello 里。

任何一处被改，transcript 变，Finished 必然对不上。这条一定要写成测试
（改证书一个 bit → 客户端必须在 Finished 之前就拒绝）。

### 3.4 客户端的校验顺序（10 步，任一步失败即终止）

1. 帧头：magic / type / **version=2** / 精确长度；
2. 解析 BPCERT1（`Bpcert1Parse`，结构不合法即失败）；
3. 在可信根里按 `issuer_id` 找到根（`TrustedRootStore::VerifyCertificate`，
   空存储 = 不信任任何东西）；
4. Ed25519 验签（根公钥对 body）；
5. `server_id` 与配置里的 `expected_server_id` 逐字节相等；
6. 时间窗（±5 分钟，`Bpcert1CheckValidity`，过期/未生效文案区分时钟问题）；
7. `key_usage == SERVER_AUTH`（解析器已强制，这里再断言一次）；
8. `cert.server_public_key == ServerHello.server_static`（**关键一步**：
   把"证书认证的公钥"和"握手里实际用的公钥"绑在一起）；
9. 既有的 X25519 低阶点拒绝规则（原样复用）；
10. 以上都过了才派生会话密钥、才发 ClientFinished、才校验 ServerFinished。

### 3.5 服务端侧

* `TransportIdentity` 增加可选的 `certificate`（原始 BPCERT1 字节）；
  证书从新配置项 `--bpsec2-cert-file <path>` 读，**不塞进 transport.key**
  （`LoadTransportIdentity` 要求那个文件严格 32 字节，这是有意的）；
* `--require-bpsec2`：只接受版本 2 的 ClientHello；收到版本 1 时拒绝并记录
  "拒绝降级"审计；
* 服务端启动时打印自己的 `server_id` 与证书指纹（公开材料），便于运维核对。

### 3.6 客户端侧（配置层）

* 官方云端 profile：`display_name = Backup Project Cloud`、
  host 来自 `ssh -G aliyun-ecs` 的 HostName、端口 18765、
  `expected_server_id = backup-project-cloud-production`、
  信任根 = 内置官方根；**用户不需要填任何指纹**；
* 自定义服务器 profile（`.bpserver`）：三种身份模式任选其一 ——
  签名身份（指定根文件）/ 手工 fingerprint（BPSEC1 原样）/ SSH 兼容；
* 错误文案统一走 `SecureTransportErrorMessage`，新增"证书类"错误码。

## 4. 施工顺序（下一步）

1. `secure_transport.h`：+`kBssec2Version`、+`kTypeServerCertificate`、
   +长度上限；`SecureTransportError` 增加证书类错误码与文案；
2. `secure_transport.cpp`：+`BuildServerCertificate` / `CheckCertificateMessage`；
   `HandshakeClient` / `HandshakeServer` 增加一个可空的 BPSEC2 参数
  （null = 今天的行为，逐字节不变），BPSEC2 分支按 §3.4 的顺序实现；
3. 服务端配置与加载（`--bpsec2-cert-file` / `--require-bpsec2`）+ 启动日志；
4. 测试：
   * `tests/unit/bpsec2_test.cpp` + `scripts/bpsec2_test.sh`：正向握手、
     证书被改一个 bit、证书签名无效、server_id 不匹配、过期/未生效、
     不受信任的根、证书公钥与握手公钥不一致、空根存储、降级（v1 → 被拒）、
     transcript 绑定（两边 transcript 相等且随证书变化）、fuzz（>=10000 条
     畸形 ServerCertificate / ServerHello 组合，必须全部具名失败且不崩）；
   * `scripts/secure_transport_test.sh` 原样必须继续全过。

## 5. 当前状态（诚实记录）

* 已完成并提交：SHA-512、Ed25519、BPCERT1、TrustedRootStore、backup-cert-tool、
  离线根 Root-A、以及为 ECS 现有 `transport.key` 签发并用**内置官方根**
  验过的第一张生产证书（`verdict = TRUSTED`）；
* **未完成**：§4 的 1-4 步都还没写 —— 也就是说今天的网络路径仍然是 BPSEC1 +
  人工 pin，证书还没有进入握手。本文的作用就是让这一段可以按图施工，
  而不是重新做一遍逆向。
