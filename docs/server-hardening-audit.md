# 服务端加固审计（§32）

本文只记录能被证据支持的事实：每条写清楚现状与证据在哪。审计对象是 PR #23 部署在
阿里云 ECS 上的 backup-server（当前只监听 127.0.0.1:18765），以及仓库里的部署与运维脚本。

## 1. 已经做到的

| 项 | 现状 | 证据 |
|---|---|---|
| 默认只回环 | --bind 非 127.0.0.1 一律拒绝；要开公网必须同时给 --allow-public-bind 理由、--bpsec2-cert-file、--require-bpsec2 | 红队 13/13 真实二进制检查；Phase 7 脚本 A01/A02/A02b/A03 |
| 身份密钥文件 | 传输身份私钥必须 0600、普通文件、O_NOFOLLOW，长度恰好 32 字节 | secure_transport.cpp LoadTransportIdentity；红队 C4 |
| 证书与密钥一致性 | 启动时核对证书里的公钥就是本机 transport.key 的公钥，不一致拒绝启动 | 红队 C4-13（rc=1 且无监听） |
| 拒绝降级 | --require-bpsec2 时收到 BPSEC1 的 ClientHello 直接拒绝，且不回任何字节 | 红队 C2（0 字节返回） |
| 证书大小上限 | --bpsec2-cert-file 超过 4096 字节在读入前就被拒 | remote_server.cpp（红队 F5 修复） |
| 握手整体预算 | 慢速滴水在预算到点被杀：1 字节/100ms 的 slowloris 在 1500ms 预算下 1499ms 死掉 | network_protocol.cpp ReceiveAll（poll + 预算）；红队实测 |
| 记录层 | 序号连续（重放、跳号、乱序拒绝）、单记录与单帧都有上限、tag 校验失败即断 | 既有 BPSEC1 套件；红队记录层矩阵 |
| 无 TOFU | 没有根或没有 pin 就直接拒绝，绝不首次见到谁就信谁 | kNoPinConfigured；空存储不信任任何东西 |
| 私钥不出机器 | 根私钥只在离线机器（0600，仓库之外）；传输私钥只在服务器本机生成 | Phase 1/3 的部署记录；仓库内私钥检索为 0 |
| 登录失败节流 | 同一用户名连续失败达到阈值后，窗口内即使口令正确也拒绝；按用户名字符串计数，表有上限 | scripts/login_throttle_test.sh 7/7；final_gate 已注册 |
| 管理工具不监听 | backup-server-admin 源码里没有 socket() / bind() / listen() | 既有实现与文档 |

## 2. 没有做到的（如实列出）

* ~~没有登录限速或锁定~~ **已完成（§33）**：同一个用户名连续失败达到
  --max-login-failures（默认 5）之后，接下来 --login-lockout-seconds（默认 60）秒内
  **即使口令正确也拒绝**，成功一次即清零。按用户名字符串计数（不存在的名字也走同一条
  路径，否则限速本身会成为存在性探针），节流表 4096 条上限并清理过期条目。
  证据：scripts/login_throttle_test.sh 7/7，已注册进 final_gate.sh。
* ~~没有主机防火墙配置~~ **更正（Phase 7 实测）**：ECS 上 **ufw 是开着的** ——
  /etc/ufw/ufw.conf 的 ENABLED=yes、/etc/default/ufw 的 DEFAULT_INPUT_POLICY=DROP
  都是世界可读的，所以主机侧并不是「不知道」，而是**默认丢弃一切未显式放行的入方向
  流量**。这才是开放公网时的实际闸门：安全组放行之后，18765 从外部仍然是 timeout
  （丢包）而不是 refused，而 22/80 能连上（它们被显式放行过）。
  要开放必须两处都做：控制台安全组 **和** ECS 本机 sudo ufw allow 18765/tcp。
  仓库里仍然没有随部署下发的 ufw 规则，这一点保留为缺口。
  教训：审计时用 sudo -n 读不到就写「读不到」，但**配置文件本身可能世界可读** ——
  应该顺手看一眼 /etc/ufw/ufw.conf 与 /etc/default/ufw。
* **安全组不可自动化**：SECURITY_GROUP_AUTOMATION = UNAVAILABLE（不允许在仓库或制品
  里放阿里云 AccessKey，ECS 上也没有任何凭据）。开端口只能人工在控制台做。
* **服务端不在 systemd 下**：当前是 nohup 直接起（PPID=1），因此没有
  NoNewPrivileges / ProtectSystem / PrivateTmp / 用户隔离那一套；进程以部署用户
  ubuntu 运行，没有做权限分离。
* **根没有硬件保护**：离线根是一个 0600 的种子文件，放在开发机仓库之外的目录里
  （路径与公钥指纹都记录在案）。它不是 HSM，也不是硬件令牌 —— 这一点不掩饰。
* **管理工具没有二次认证**：backup-server-admin 只依赖能在服务器上以该用户登录这一
  事实（即 SSH 与文件权限），本身没有额外的口令或令牌。

## 3. 开放公网之前的清单

按重要性排序，前两条是硬前提：

1. ~~补登录限速（§33）~~ **已完成**：见第 2 节与 scripts/login_throttle_test.sh；三个新选项：
2. 两处一起做：控制台安全组入方向放行 18765/tcp（**已完成**，源限定为
   171.221.252.81/32），以及 ECS 本机 sudo ufw allow 18765/tcp（**待做**）；
3. 确认服务端仍然是 --require-bpsec2 加证书（Phase 7 脚本会校验）；
4. 记录一次外部探测作为确实可达的证据（Phase 7 的 C 段已经这么做）；
5. 可选：把服务端纳入 systemd 以获得进程级加固；把根私钥换到硬件载体。

## 4. 这份审计的证据边界

上面每一条都指向代码位置、脚本断言或红队实测。**没有做过的渗透测试、没有跑过的
参数、没有配过的规则，这里一个字都不写** —— 审计的价值全在于此。