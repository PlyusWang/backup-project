# 客户端快速上手（client quick start）

本文只讲**用户**要做什么。已经实现并验证过的行为写在"现在的状态"一节。

## 1. 官方云端：什么都不用配

打开客户端，连的就是 `Backup Project Cloud`。用户需要知道的只有三件事：

1. 服务器名字显示为 **Backup Project Cloud**；
2. 旁边有"服务器已验证 / 未通过"的状态；
3. 然后填用户名和密码。

**不需要**：配置 SSH、知道 @aliyun-ecs` 是什么、开隧道、复制指纹、点"应用"、
理解 X25519 / Root Key / 证书这些词。

技术上发生了什么（用户不需要看这一段）：客户端用编译进二进制的一份
`OfficialCloudProfile` 去连，服务端出示由**离线根**签发的身份证书，客户端用
编译进二进制的官方根验签；证书里必须出现固定的 `server_id`。全程没有指纹，
也没有"第一次见到谁就信谁"。

## 2. 自定义服务器

自托管/内网服务器用一个 `.bpserver` 描述文件。三种身份方式选一种：

    # 方式一：签名身份（服务端有证书时最省事）
    display_name = 我的服务器
    host = 192.168.1.10
    port = 18765
    identity = certificate
    expected_server_id = my-server
    trusted_roots = /etc/backup-project/my-root.pub

    # 方式二：手工指纹（老式、但不需要证书体系）
    identity = pin
    server_key_pin = sha256:<64 位十六进制>

    # 方式三：SSH 隧道（在隧道里仍然用指纹）
    identity = ssh
    server_key_pin = sha256:<64 位十六进制>
    ssh_target = user@192.168.1.10

规则很简单：**模式决定要填哪些字段**，填错/漏填/混填都会在解析时直接报错，
不会出现"界面以为在用证书、CLI 却还在比对指纹"这种半配置状态。未知键也会报错
（拼错的键名如果被忽略，用户会以为配置生效了）。

## 3. 命令行等价物

    backupctl remote ping   --host <地址> --port <端口> --expected-server-id <名字>
    backupctl remote login  --user <用户名> --host ... --port ... --expected-server-id ...
    backupctl remote list   --user <用户名> ...

签名身份只需要 `--expected-server-id`；不给 `--trusted-roots` 就用**内置官方根**
（这就是官方云端的用法）。要换成自托管的根，加 `--trusted-roots <根文件>`。
指纹方式仍然用 `--server-key sha256:<指纹>`，也可以放在环境变量
`BACKUP_REMOTE_SERVER_KEY` 里。

## 4. 出错时用户会看到什么

失败原因是**分开报**的，不是一句"连接失败"：

| 界面文案 | 实际含义 |
|---|---|
| 服务器身份证书无效 | 签名验不过，或证书结构不合法 |
| 签发这张证书的根不在本机可信列表里 | 陌生根（官方云端收到非官方根签的证书） |
| 服务器身份证书里的服务器名字不是这个云端 | `server_id` 不匹配 |
| 服务器身份证书不在有效期内 | 过期/未生效（文案会提示检查本机时钟） |
| 服务器出示的证书与实际使用的身份密钥不是同一把 | 证书公钥 ≠ 握手公钥 |
| 服务器要求使用签名身份，对端却试图退回旧协议 | 拒绝降级 |

## 5. 现在的状态（诚实记录）

* 客户端侧的证书模式、@.bpserver` 解析、内置官方根都已经实现并有测试
  （`scripts/server_profile_test.sh` 23/23、`scripts/bpsec2_test.sh` 29/29、
  `scripts/bpsec2_loopback_e2e.sh` 17/17）；
* **官方云端的公网端口已经打通**：`0.0.0.0:18765` 监听，安全组与主机 `ufw`
  两处放行之后，公网直连已经端到端通过：Phase 8 **8/8**（内置官方根、零指纹、
  ssh 进程 delta = 0）、Phase 9 **8/8**（公网对抗之后 PID 未变）。
  注意**开公网要两处都做**：阿里云安全组入方向 + ECS 主机 `ufw`；只做一处会表现为
  timeout（丢包）而不是 refused。详见 `docs/server-quick-start.md` 第 8 节。
