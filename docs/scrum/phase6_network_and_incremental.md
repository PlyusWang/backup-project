# Phase 6 Review & Retrospective：增量备份与网络备份

> 覆盖阶段：2026-09-28 -- 2026-10-06
> 对应 PR：#18（feature/incremental-backup-strategy）、#20（feature/network-backup-foundation）、
> #21（feature/network-score-closure）、#22（feat/remote-connection-ux）、
> #23（feat/signed-server-identity-public-cloud）
> 收口基线：PR #23 合并后的 `main`（3871a8c）
> 网络备份在原计划中是 STRETCH，这一阶段把它做成了产品功能。

---

## 1. 阶段 Goal

四件事，按依赖顺序推进：

1. **增量**：同一源目录的连续快照只存变化部分（delta），并且能恢复到链上的任意
   一个恢复点；
2. **网络**：把快照存到远端服务端，服务端只管存字节、存链关系和归属，不重新实现
   备份与增量算法；
3. **安全传输**：远端链路必须认证加密，服务端身份要能被验证；
4. **端到端验收**：在真实的公网服务器上跑通注册、备份、增量、恢复与对抗场景。

---

## 2. Review

### 2.1 增量备份（PR #18）

- **`BKPINC1` delta 归档格式**：与 v0.1 / v2 并存，读侧同样严格（未知版本、
  保留字段非 0、长度越界一律拒绝）；
- **恢复链**：支持恢复到链中间的任意恢复点，链上每一跳都要校验；
- **三件套**：增量引擎眼中一份快照是 `<name>.bak` + `<name>.manifest` +
  `<name>.identity`（`BPIDENT2`），生成下一份 delta 需要父的三件套；
- **归属验证只看实际字节**：snapshot identity 从真实归档字节推导，
  `fix: verify snapshot identities from actual archive bytes`；
- **保留策略依赖感知**：被增量链引用的父快照不能被单独删掉；删除按
  **descendants-first** 顺序（先删后代再删祖先），避免删到一半留下断链；
- **性能**：没有期望摘要时不整块哈希 payload（`perf: avoid full payload hashing
  without an expected digest`）；
- **暴露面**：CLI 与 Modern GUI 都能选择 full / incremental；非法的选项组合
  （例如对增量使用不允许的加密方式）在入口处就被拒绝，GUI 的选项面板也如实反映
  真实约束，不做"能点但一定失败"的按钮；
- **测试**：链中恢复点、加密 delta、真实链上的特殊文件与硬链接、崩溃边界
  （`test: harden the incremental crash boundaries`），并在 ASan + UBSan 下再跑
  一遍增量套件。

### 2.2 网络备份地基（PR #20）

- 协议 `BPNET1`：32 字节定长帧头，线上 magic 是 `BPN1`（`0x42504E31`），
  全部**显式大端**、手工逐字节拼接；客户端给出的每个长度先过上限再谈分配；
  帧边界靠"读满 32 字节帧头 + 按声明长度收 payload"确定；控制帧与文件块的
  payload 上限同为 1 MiB；
- 服务端只做三件事：**存字节、存链关系、enforce 归属与依赖**。它不扫源目录、
  不算 diff、不解释文件级 tombstone、不重新实现 `BackupEngine`；
- 元数据用 SQLite（PR #20 时 schema 1，snapshots 表七列）；
- 配套 `backup-server-admin`：用户管理、快照列表、删除、账号删除；
  同一实例上客户端、admin CLI 与 SQLite 三方的一致性有独立套件覆盖。

### 2.3 材料包与远端增量（PR #21）

远端存的是 **`BPSNAP1` 材料包**：把三件套按顺序装进一个容器（每个成员带长度与
SHA-256）。原因是踩过的真实坑：**只上传 `.bak` 的链，在"副文件验证"这一步必然
失败**（`sidecars_verified=false`）。

材料包的信任链按三层校验，其中一条来自同长度改写的教训：

> 包头声明的摘要必须等于**实际复制进容器的字节**的 SHA-256。打包走两遍（第一遍
> 算长度与摘要、第二遍流式复制），第二遍在复制每个成员时**重算**摘要，不一致就整包
> 放弃：删掉本次 part、不发布。

回归入口 `scripts/bundle_source_mutation_test.sh` 用同一个复现程序在当前树与
`git archive` 出来的旧树上各跑一次，做 OLD/NEW 判别——也就是说这条修复本身是被
一个会失败的旧版本对照验证过的。

同一阶段还做了：远端快照在 GUI 上区分"原始归档"与"备份链"、原始归档走正常的
恢复流程、传输层 `EINTR` 重试（含 key 文件尾部读取）、bundle 摘要绑定到实际
复制的字节、bundle 解析与记录层的确定性变异 fuzzer。

### 2.4 安全传输与服务器身份（PR #22 / #23）

- `BPSEC1`：`BPNET1` 的**每一帧**（含 opcode、status、长度）整体作为明文进入
  记录层，加密并带 HMAC-SHA256 标签；握手用 X25519（static + ephemeral）+ HKDF
  派生会话密钥，客户端必须事先 pin 服务端公钥或指纹，不符立刻断开；
- `BPSEC2`：在**不改 BPSEC1 线格式**的前提下，把服务端身份从"人工 pin 指纹"
  换成"离线根签发的证书"。硬约束是 fail closed：证书校验失败不得回退 BPSEC1、
  不得回退人工 pin、不得 TOFU；transcript 绑定协议版本、认证方式、证书全部字节、
  证书签名、server_id、被认证的公钥与双方随机数；官方云端模式只接受 BPSEC2；
- 手写原语与工具：`sha512` / `ed25519` / `x25519` / `hkdf` / `bpcert`，
  以及离线 `backup-cert-tool`；服务端身份私钥由 `backup-server-keygen` 在
  目标机本机生成（0600），根私钥不进仓库、不进制品；
- 服务端加固：默认只绑定回环地址、私密文件权限、只读命令必须真正只读、
  下载发布不覆盖竞争（先临时文件后 rename）、同一实例的状态根 fail closed；
  admin 的用户选择必须显式给 id 或名字，**不做猜测**；
- 连接层（PR #22）：产品自己管理 SSH 安全通道（`SshTunnelManager`），
  "应用"与登录的关系、pin 的生效时机、失败分层提示都按人工验收的反馈重做；
  测试用真实 GUI 进程与真实 QML 点击断言 C01--C07 七条合同。

### 2.5 公网验收（PR #23）

在阿里云 ECS 上按阶段脚本执行并留档（`scripts/pr23_ecs_phase3..phase9`：
部署、本地 BPSEC2、隧道、防火墙审计、公网绑定、公网端到端、对抗测试），
外加 `scripts/aliyun_sequence_e2e.sh` 等可复现序列。结论写在
`docs/bpsec2-design.md` §5 与 `docs/pr22-remote-connection-ux.md`。

### 2.6 测试

进入收口回归的套件（均 `exit=0`）：

| 类别 | 套件 |
| --- | --- |
| 增量 | scripts/remote_incremental_test.sh（自报 PASS=29）、增量相关用例在 sanitizer 下重跑 |
| 网络 | scripts/network_test.sh（+ sanitizer 版）、scripts/bundle_source_mutation_test.sh（自报 PASS=13 / PASS=5，含 sanitizer 版） |
| 安全传输 | scripts/secure_transport_test.sh（+ sanitizer 版）、scripts/bpsec2_test.sh（含 10000 例 fuzz 与 BPSEC1 行为基线）、scripts/bpsec2_loopback_e2e.sh、scripts/server_profile_test.sh、scripts/login_throttle_test.sh |
| 原语与证书 | scripts/sha512_test.sh（官方向量 + sha512sum oracle）、scripts/ed25519_test.sh（RFC 8032 + PyNaCl / cryptography 双 oracle + OpenSSL 交叉验证）、scripts/bpcert_test.sh（六类畸形 + 单字节全扫描 + 20000 例 fuzz）、scripts/cert_tool_test.sh |
| 远程 GUI | scripts/remote_connection_ux_test.sh、scripts/ssh_tunnel_manager_test.sh（+ sanitizer 版）、scripts/raw_archive_restore_test.sh（自报 PASS=16 / PASS=7） |
| 服务端 | scripts/server_admin_test.sh、scripts/account_deletion_test.sh、scripts/same_instance_truth_test.sh |

---

## 3. Retrospective

### 3.1 做得较好的地方

**网络层不重新实现任何增量算法。**
变化的判断、delta 生成、链验证、恢复应用全部沿用本地核心；网络层只解决"怎么把
三件套搬到远端、怎么保证搬过去的字节没被改过"。

**把"三件套"当成一个整体传输。**
材料包把 `.bak` / `.manifest` / `.identity` 绑在一起，并让每个成员都带摘要，
避免"传了一半的链"这种将来才暴露的坏数据。

**协议层与记录层各自 fail closed。**
未知版本、保留字段非 0、长度超限、认证失败、证书失败——一律断开，不做"尽力继续"。

**每个安全相关的实现都有独立 oracle。**
SHA-512 / Ed25519 用官方向量 + 系统工具 / 第三方库交叉验证，证书解析用 Python
独立拼字节再喂回产品解析器；手写大整数与标量归约在 ASan + UBSan 下再跑一遍
（这一项在本项目里真实抓到过两次问题）。

### 3.2 遇到的问题与处理

**1）只传 `.bak` 的链在远端一定是坏的。**
副文件验证会失败，而失败点在恢复时才暴露。修法是把三件套打包成一个容器，并让
服务端把它当成不透明字节。

**2）"声明摘要"与"实际写入字节"可能脱钩。**
两遍打包之间一次**同长度**的改写，会让包头摘要与容器内容不一致，而服务端照收
（它只校验整个 blob 的摘要、不解析成员）。修法是在第二遍复制时重算摘要，并用
新旧对照的判别测试固定这条合同。

**3）安全通道的失败必须分层说清楚。**
人工验收发现"点了应用没反应""连接失败说不清在哪一层"。原因是失败被压成了一句
"网络错误"。现在按 SSH 通道、pin 校验、协议握手、业务错误分层报告，并且删掉了
重复的 pin 校验实现。

**4）有套件一直没被 gate 跑到。**
红队复核发现 `bpsec2_loopback_e2e.sh` 从来没进过 `final_gate`，因此它"在干净树上
跑不过"这件事一直没被发现。现在三类套件全部进 gate，并且 gate 把"缺一条 ok 行"
也算失败——不允许"没跑到"被当成通过。

**5）测试机器的真实性。**
ECS 序列最初把工作区放在仓库的 testdata 里，导致"测试产物污染仓库"的风险。改成
显式指定仓库外的工作区。

**6）官方云与自定义服务端的要求不同。**
官方云端只接受 BPSEC2（证书身份），自建服务端可以选择 BPSEC1（pin 指纹）。这个
区别写在 `docs/secure_transport.md` 与 `docs/self-hosted-server.md` 里，
避免用户按错误的预期配置。

### 3.3 下一阶段的改进动作

1. 全量回归：把这一阶段新增的十几条套件与旧套件一起放进 `scripts/final_gate.sh`；
2. 发行打包：产物必须能在更旧的发行版上运行，且不含任何私钥；
3. 源码质量收口：把重复的布局解析、字节序读取与持久化辅助函数收敛到单处。

---

## 4. 阶段结论

这一阶段把两个 STRETCH 项做成了产品功能：

```text
同一源目录 → full 快照 + BKPINC1 delta 链（三件套）
远端         → BPSNAP1 材料包 → 服务端只存字节 / 链关系 / 归属
线上         → BPNET1（magic BPN1）跑在 BPSEC1 / BPSEC2 之上
身份         → 离线根签发证书（BPSEC2）；自建服务端可用 pin（BPSEC1）
```

同时要把边界如实写下来：安全传输与证书层是**课程项目里的手写实现**，有官方向量、
交叉 oracle、fuzz 与消毒剂验证，但**没有经过外部安全审计**；产品只有 x86_64
Linux 一种目标平台，远端能力依赖用户自己准备的服务器与 SSH 通道。
