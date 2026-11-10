# 自托管服务器（self-hosted）

给"自己有一台机器、想自己当根"的人。官方云端用的是**离线官方根**；自托管
用的是**你自己的根**，两者互不影响，也不应该混用。

## 使用终端菜单

安装服务器 Debian 包后运行：

    sudo backup-server-admin-menu

便携版解压后进入服务器目录，使用当前普通用户的独立实例：

    ./backup-server-manager

便携版首次进入菜单可选择“初始化服务器”，填写实例绝对路径、回环业务端口，
以及是否创建 BPSEC2 本机测试根与服务器证书。已有配置、数据或密钥时初始化会拒绝
覆盖。向导只配置 `127.0.0.1`；其他机器可经 SSH 隧道访问，不能把回环地址当作
局域网或公网地址。Debian 包安装过程已创建的实例无需重复初始化。

菜单“服务器身份与连接信息”显示业务地址、端口、BPSEC1 指纹，或 BPSEC2 的
`server_id`、签发者、有效期及两个绝对路径。客户端只需取得可信根公钥和服务器
身份证书；`private/root.key` 与 `state/transport.key` 都是私钥，不提供给客户端。

### 导入已有 BPSEC2 证书

先在菜单选择“停止服务器”。安装版通过固定的 `backup-project-server.service`
操作系统服务；便携版只停止可执行文件、数据目录、数据库、PID 路径和进程启动标识
都与当前实例匹配的进程。安装版也可使用 `sudo systemctl stop backup-project-server`。
无 systemd 的系统应使用便携包管理独立实例，不会回退成以 root 直接启动服务端。

选择“管理证书与可信根”→“安全导入”，填写可信根**公钥**文件和服务器 BPCERT1
证书的绝对路径。菜单会验证根信任、签名、有效期，并确认服务器证书公钥与当前
传输私钥对应；错误根、过期/篡改证书、公钥不匹配都会拒绝导入。根公钥文件最大
65536 字节，证书最大 4096 字节。请先经可信渠道核对根和服务器身份，再输入
`IMPORT BPSEC2` 确认。

成功后启用仅 BPSEC2 认证，新公钥材料存入
`state/client-identity.<标识>/`，并显示可复制的完整路径。旧证书、旧根、传输私钥
与根私钥均保留。无需把离线根私钥复制到服务器。重新打开菜单仍使用已提交的根与证书。

### 修改运行配置

停机后选择“查看与修改服务器配置”→“修改”，可编辑端口、线程数、I/O 超时、
日志绝对路径和认证方式。回车保留原值，最后输入 `SAVE` 提交。设置先由同一个
服务端启动器校验，校验失败或取消时原配置不变。日志路径不能覆盖配置、数据库或身份
文件。旧配置保留在配置目录下的 `.server-config-backup.<标识>/server.conf`。

认证选项 1 允许 BPSEC1，并保留已有的可选 BPSEC2 证书，必须另输入 `ALLOW BPSEC1`
确认；选项 2 仅接受 BPSEC2，并重新验证当前根、证书和传输公钥。公网监听不允许在菜单中
降级。菜单不修改监听地址、公网安全条件、数据路径、PID 路径或任何私钥。

保存不会启动服务器；选择“启动服务器”后才生效。安装版菜单通过固定的 systemd
服务启动，并检查实际进程与业务端口；也可执行 `sudo systemctl start backup-project-server`。

安装版的升级和卸载说明见 [install-server.md](install-server.md)。
已有离线签发流程如下。

## 1. 为什么自托管也要有根

没有根就只能回到"人工核对指纹"（`identity = pin`）。指纹方式仍然支持，
但每台客户端都要手工抄一遍 64 位十六进制，而且服务器换密钥时每台都要改。
自建一个根之后，客户端只认"谁签的"，服务器换密钥只要重新签一张证书。

## 2. 建根（在**离线**机器上做）

    ./build/backup-cert-tool root-init \
        --root-key /path/to/my-root.key \
        --root-id  my-org-root-a

产出两个文件：

* `my-root.key` —— **私钥**，权限 0600，只在离线机器上，绝不进 Git / 制品 /
  日志 / 命令行参数（工具里根本没有把私钥写在命令行上的入口，也会显式拒绝
  `--root-key-hex` / `--seed-hex`）；
* `my-root.key.pub` —— 公钥，一行文本，可以随便发给客户端：

      my-org-root-a ed25519:0530...66df9 unlimited

  格式是 `<root-id> <公钥> [两个时间戳 | unlimited] [active|revoked]`。
  有效期必须**显式**写：要么两个 Unix 秒，要么写 `unlimited`。只写一个数字
  或者什么都不写都会被拒绝 —— 一个笔误不该把"根 2033 年过期"悄悄变成
  "永久有效"。

## 3. 签服务器证书

    ./build/backup-cert-tool issue-server \
        --root-key /path/to/my-root.key \
        --server-id my-server \
        --server-pubkey <服务器上 backup-server-keygen 打印的公钥> \
        --out my-server.bpcert

证书只覆盖**公钥**，与服务器的私钥文件无关；服务器换密钥 = 用新公钥重签一张，
把新证书放到 `state/server-identity.bpcert` 重启即可（旧客户端只要还信任同一
个根就继续能用）。

## 4. 客户端怎么连

写一个 `.bpserver`：

    display_name = 我的服务器
    host = 192.168.1.10
    port = 18765
    identity = certificate
    expected_server_id = my-server
    trusted_roots = /etc/backup-project/my-root.key.pub

或命令行：

    backupctl remote ping --host 192.168.1.10 --port 18765 \
        --expected-server-id my-server --trusted-roots /etc/backup-project/my-root.key.pub

`expected_server_id` 不能省：它挡住"同一把根签发的另一台服务器"拿自己的证书
来冒充（否则任何一台被你签过的机器都能顶替这一台）。

## 5. 根轮换

根文件支持多行，所以轮换是"先加后撤"：

1. 生成 Root-B，把 `my-org-root-b ... unlimited` 加到客户端的根文件里，
   同时服务端用 Root-B 签新证书；
2. 等所有客户端都更新过根文件之后，把 Root-A 那行标成 `revoked`。

在 Root-A 被标 revoked 之前，用它签的老证书仍然有效。

## 6. 只用指纹也可以

不想建根就这样：

    identity = pin
    server_key_pin = sha256:<backup-server-keygen --show 打印的指纹>

服务端不需要 `--bpsec2-cert-file`，客户端也不需要 `--trusted-roots`。
代价是每台客户端都要手工确认那个指纹（这正是官方云端不想让用户做的事），
而且服务器换密钥时每台都要改。

## 7. 边界（诚实写在这里）

* 签名身份只保证"这台服务器确实是那个根批准的那把密钥"；它**不**替代
  传输加密 —— 记录层仍然是 BPSEC1 的 X25519 + HKDF-SHA256 + AES-256-CTR +
  HMAC-SHA256，证书只是把"服务器身份"从人工 pin 换成可签发、可轮换的证明；
* 本项目的密码学是手写实现（SHA-512 / Ed25519 / X25519 / AES / HMAC / HKDF），
  有官方向量、独立 oracle 与红队复核，但没有经过形式化验证或第三方审计 ——
  这一点在 `include/ed25519.h` 与 `src/crypto/ed25519.cpp` 里都写明了；
* 证书格式是自己定义的 BPCERT1，不是 X.509，不能与其它 PKI 工具互操作 ——
  这是有意的取舍（见 `include/bpcert.h` 开头）。
