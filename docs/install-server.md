# 安装服务端（Backup Project Server）

本文讲**服务器管理员**要做的事：装、初始化、开机自启、管理、升级、卸载，
以及怎样**安全地**把它开到公网。

支持范围：**linux x86_64**，构建基线 **Ubuntu 20.04 / glibc 2.31**
（构建基线越旧，能跑的发行版越多）。已实测：Ubuntu 20.04 / 22.04 / 24.04、
Debian 12。Windows / macOS / ARM64 没有构建也没有验证，不声称支持。

## 1. 安装（apt / dpkg）

    sudo apt install ./backup-project-server_<版本>_amd64.deb

装出来的布局：

    /usr/lib/backup-project-server/bin/backup-server              服务端
    /usr/lib/backup-project-server/bin/backup-server-admin        本机管理工具（不监听端口）
    /usr/lib/backup-project-server/bin/backup-server-keygen       传输身份密钥工具
    /usr/lib/backup-project-server/bin/backup-cert-tool           离线根 / 证书工具
    /usr/lib/backup-project-server/bin/backup-server-admin.sh     交互式管理菜单的实现
    /usr/lib/backup-project-server/bin/launch-server.sh           配置 -> 命令行 的启动器
    /usr/bin/backup-server              命令行入口（= 启动器）
    /usr/bin/backup-project-server      同上（更直观的名字）
    /usr/bin/backup-server-admin        管理工具
    /usr/bin/backup-server-admin-menu   **交互式管理菜单**（日常就用这个）
    /usr/bin/backup-server-keygen       传输身份密钥
    /usr/bin/backup-cert-tool           证书工具
    /usr/bin/backup-server-purge-data   显式删除数据的唯一入口
    /etc/backup-project-server/server.conf        配置（conffile）
    /etc/backup-project-server/secrets.env        token secret（0640 root:backup-project）
    /var/lib/backup-project-server/data/          备份数据（0750 backup-project）
    /var/lib/backup-project-server/state/         元数据库 + 传输身份私钥
    /lib/systemd/system/backup-project-server.service

## 2. 首次安装会自动做什么（以及绝不做什么）

自动做：

* 创建**专用系统用户** `backup-project`（`/usr/sbin/nologin`、无家目录登录）；
* 创建 `data/` 与 `state/`（0750）并设好属主；
* 生成**真正随机**的 `BACKUP_TOKEN_SECRET`（32 字节随机数的 sha256 十六进制，0640）；
* 用 `backup-server-keygen` 在**本机**生成传输身份私钥 `state/transport.key`（0600）；
* `systemd` 重新加载；配置自检通过时启动并设为开机自启。

**绝不**做：

* 覆盖任何已存在的文件（secrets.env / transport.key / 数据库 / data / 证书全保留）；
* 碰防火墙：**不**动 ufw、iptables、nftables、firewalld，也**不**动云厂商安全组；
* 默认监听 `0.0.0.0`：默认只有 `127.0.0.1`；
* 在卸载时删除你的数据（哪怕 `purge`）。

## 3. 配置：`/etc/backup-project-server/server.conf`

严格的 `key = value`；`#` 之后是注释；**未知键直接报错**；值不做任何 shell 展开
（不 eval、不 source）。每个键一对一映射到 `backup-server` 自己的参数。

    bind = 127.0.0.1                       # 默认只回环
    port = 18765
    data_root = /var/lib/backup-project-server/data
    database = /var/lib/backup-project-server/state/metadata.sqlite3
    secret_file = /etc/backup-project-server/secrets.env
    transport_key = /var/lib/backup-project-server/state/transport.key
    certificate =                          # 留空 = 只支持 BPSEC1 指纹模式
    require_bpsec2 = false
    allow_public_bind = false
    public_bind_reason =
    allow_public_bind_insecure = false      # 逃生舱，见第 6 节
    workers = 4
    io_timeout = 30
    max_upload_bytes = 8589934592
    max_login_failures = 5
    login_lockout_seconds = 60

改完先自检，再重启：

    sudo backup-project-server --check-config
    sudo systemctl restart backup-project-server

`--check-config` 会检查：目录是否存在、secrets.env 里有没有 `BACKUP_TOKEN_SECRET`
（长度 ≥ 16 字节，内容不打印）、transport.key 是否 32 字节且 0600、证书能否被
`backup-cert-tool inspect-server` 解析、以及公网绑定的三个前提。它**不打印任何
秘密内容**。

命令行临时覆盖（管理员应急用，参数会追加在配置后面）：

    sudo backup-project-server --config /etc/backup-project-server/server.conf --quiet

## 4. systemd

    sudo systemctl status backup-project-server
    sudo systemctl restart backup-project-server
    sudo journalctl -u backup-project-server -n 100 --no-pager

unit 的加固是**逐项评估**过的，不是"全打开"：

| 选项 | 为什么可以开 |
|---|---|
| `NoNewPrivileges` | 服务端不需要任何提权 |
| `PrivateTmp` | 只用得着自己的临时目录（SQLite 临时文件也够） |
| `ProtectSystem=strict` + `ReadWritePaths=/var/lib/backup-project-server` | 二进制与配置只读；写的地方只有实例目录 |
| `ProtectHome` | 服务端不读任何人家目录 |
| `ProtectKernel*` / `ProtectClock` / `ProtectHostname` / `ProtectControlGroups` | 服务端不碰内核参数、时钟、主机名、cgroup |
| `RestrictSUIDSGID` / `RestrictRealtime` / `RestrictNamespaces` / `LockPersonality` | 不需要这些能力 |
| `MemoryDenyWriteExecute` | 没有 JIT；服务端是纯 C++ |
| `RestrictAddressFamilies=AF_INET AF_INET6 AF_UNIX` | 只用 TCP 与本地套接字 |
| `UMask=0027` | 新建文件默认不给同组写、不给其他人任何权限 |

改 unit 之后：`sudo systemctl daemon-reload && sudo systemctl restart backup-project-server`。

## 5. 日常管理：用菜单，不要背子命令

    sudo backup-server-admin-menu

菜单里能直接做：**用户管理**（列表 / 详情 / 删除用户及其备份）、
**备份文件管理**（列出 / 查看含 SHA-256 / 删除单份）、**存储概览**、
**服务器身份信息**（客户端要填的指纹与公钥）。

为什么用 `sudo`：数据目录是 0750 `backup-project`，root 才能读；
管理工具只在本机跑，不监听任何端口。

要看状态但不进菜单：

    sudo backup-server-admin status
    sudo backup-server-admin list-users
    sudo backup-server-admin transport-identity

## 6. 开到公网（明确、危险、所以要做对）

默认**不**对外。要让互联网上的客户端直连，必须同时满足：

1. `bind` 改成非回环地址（例如 `0.0.0.0`）；
2. `certificate` 指向一张由离线根签发的服务器身份证书（BPCERT1）；
3. `public_bind_reason` 写一句话理由（会进日志，供事后审计）；
4. `require_bpsec2 = true`（只接受签名身份客户端，不做 BPSEC1 降级）。

缺任何一条，启动器直接拒绝启动 —— 与产品自身的 fail-closed 合同一致。

逃生舱：确实要用 BPSEC1 指纹模式开公网，必须显式写
`allow_public_bind_insecure = true`，启动器会打警告，日志里也留痕。

**防火墙要你自己开**（我们不碰）：

* 云厂商安全组：放行 TCP 18765，来源尽量限制到已知 IP；
* 主机防火墙：`sudo ufw allow 18765/tcp`（或 nftables 等价规则）。

顺手核对：

    ss -ltn | grep 18765          # 应该是 0.0.0.0:18765 或 :::18765

## 7. 升级

    sudo apt install ./backup-project-server_<新版本>_amd64.deb

升级会：替换二进制与 unit、`daemon-reload`、重启服务。
升级**不会**：动 `data/`、`state/`、`secrets.env`、`server.conf`（conffile 若被你改过，
dpkg 会问你怎么办）、重置用户、重新生成身份私钥、造第二个数据库。

升级前稳妥做法：

    sudo backup-server-admin status            # 先看一眼
    sudo tar -C /var/lib -czf /root/backup-project-server-$(date +%F).tgz backup-project-server
    sha256sum /var/lib/backup-project-server/state/transport.key   # 记下，升级后应完全相同

升级后核对：

    sha256sum /var/lib/backup-project-server/state/transport.key   # 与升级前一致
    sudo systemctl status backup-project-server
    sudo backup-server-admin status

## 8. 卸载（数据不会被删）

    sudo apt remove backup-project-server     # 删程序文件
    sudo apt purge backup-project-server      # 仍然保留数据与配置

输出会明确告诉你数据在哪里。真要删数据，只有一条路，而且要手打确认短语：

    sudo backup-server-purge-data --dry-run   # 先看要删什么
    sudo backup-server-purge-data             # 需要输入 DELETE ALL BACKUP DATA
    sudo backup-server-purge-data --include-identity   # 连身份私钥一起删（会让所有 pin 失效）

## 9. 自托管（自己的根）

想用自己的根签发证书（而不是官方根）：见
[自托管服务器](self-hosted-server.md)。工具 `backup-cert-tool` 已经在
`/usr/bin/backup-cert-tool`，根私钥必须留在离线机器上（0600），
**不要**放到服务器上。
