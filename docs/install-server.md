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
    /etc/backup-project-server/secrets.env        token secret（0600 backup-project）
    /var/lib/backup-project-server/data/          备份数据（0750 backup-project）
    /var/lib/backup-project-server/state/         元数据库 + 传输身份私钥
    /lib/systemd/system/backup-project-server.service

## 2. 首次安装会自动做什么（以及绝不做什么）

自动做：

* 创建**专用系统用户** `backup-project`（`/usr/sbin/nologin`、无家目录登录）；
* 创建 `data/` 与 `state/`（0750）并设好属主；
* 生成**真正随机**的 `BACKUP_TOKEN_SECRET`（32 字节随机数的 sha256 十六进制，**0600**）；
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
（长度 ≥ 16 字节，且**权限必须是 0600 / 0400** —— 产品自己也会拒绝更宽的权限；内容不打印）、transport.key 是否 32 字节且 0600、证书能否被
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

当升级失败时（配置自检不过 / 服务起不来 / 端口没进入监听），`postinst` 会**先自动回滚**、
再带着非零退出码失败 —— 细节见 7.1。

升级前稳妥做法：

    sudo backup-server-admin status            # 先看一眼
    sudo tar -C /var/lib -czf /root/backup-project-server-$(date +%F).tgz backup-project-server
    sha256sum /var/lib/backup-project-server/state/transport.key   # 记下，升级后应完全相同

升级后核对：

    sha256sum /var/lib/backup-project-server/state/transport.key   # 与升级前一致
    sudo systemctl status backup-project-server
    sudo backup-server-admin status

### 7.1 升级失败会自动回滚（不用你手工救）

升级开始前，`preinst` 会在**旧服务确实在跑**（`systemctl is-active` = active）的前提下，
把**程序载荷**复制一份到 `/var/cache/backup-project-server/rollback/`：

    /usr/lib/backup-project-server/...        服务端二进制、启动器、管理工具、purge-data
    /usr/bin/backup-server 等入口的包装脚本
    /lib/systemd/system/backup-project-server.service
    /usr/lib/tmpfiles.d/backup-project-server.conf

来源是 dpkg 自己的旧包文件清单（不猜路径、不搜目录），材料里记着每个文件的 sha256 /
权限 / 属主。升级之后如果出现下面任意一种情况：

1. 配置自检失败（`backup-project-server --check-config`）；
2. `systemctl restart` 之后服务没有进入 active；
3. 配置里的回环端口（默认 `127.0.0.1:18765`）没有进入监听；

`postinst` 会逐个校验回滚材料的 sha256，把程序载荷换回升级前的版本、`daemon-reload`、
重启服务并**再次验证**，然后把这次升级报成失败（`apt` / `dpkg` 返回非零）。

回滚的对象**只有程序载荷**：`/etc/backup-project-server`（`server.conf`、`secrets.env`）
与 `/var/lib/backup-project-server`（`data/`、`state/`、`transport.key`、数据库）一个字节
都不会被碰。材料放在 `/var/cache`（FHS 里就是「可再生、删掉不丢用户状态」的缓存），
升级成功立刻删除，卸载 / purge 也删除，平时手工删掉它没有任何副作用。

**为什么升级仍然算失败**：Debian 的 maintainer script 没有事务回滚，`dpkg` 在解包时就已经
把元数据记成新版本了。所以回滚之后 `dpkg -s` 会显示新版本号、状态是 `half-configured`，
而磁盘上跑的是旧版本 —— 这是有意的：升级必须被报成失败，而不是看起来成功了。要让元数据
与载荷一致：

    sudo apt install --reinstall ./backup-project-server_<上一个可用版本>_amd64.deb

在这件事做完之前，如果又跑 `sudo dpkg --configure -a`，`postinst` 会明确拒绝报告成功并
退出非零（fail closed），不会把「其实装的是旧版本」悄悄变成「升级成功」。

**首次安装失败**没有旧版本可回滚：安装返回失败并打印原因，但**不会**重新生成
`secrets.env` 与 `transport.key`；修好之后重装会继续用第一次生成的那份。

## 8. 卸载（数据不会被删）

    sudo apt remove backup-project-server     # 删程序文件
    sudo apt purge backup-project-server      # 仍然保留数据与配置

输出会明确告诉你数据在哪里，卸载时的 `prerm` 也会再提醒一次。真要删数据只有一条路，
而且**必须在包还没卸载时做**（`backup-server-purge-data` 是包提供的命令，`apt remove` 之后
就没了），还要手打确认短语：

    sudo backup-server-purge-data --dry-run   # 先看要删什么
    sudo backup-server-purge-data             # 需要输入 DELETE ALL BACKUP DATA
    sudo backup-server-purge-data --include-identity   # 连身份私钥一起删（会让所有 pin 失效）

### 8.1 手工清理残留数据（包已经卸载之后）

`backup-server-purge-data` 是**包提供的**命令：`apt remove` 之后就没了。所以想连数据一起
清掉，要在包还在的时候做（`prerm` 会在卸载时提醒你）：

    sudo backup-server-purge-data --dry-run            # 先看要删什么
    sudo backup-server-purge-data                      # 需要手打 DELETE ALL BACKUP DATA
    sudo backup-server-purge-data --include-identity   # 连身份私钥一起删（会让所有 pin 失效）

包已经 purge 掉、只剩数据时，按下面手工做（先确认服务没在跑）：

    sudo systemctl stop backup-project-server 2>/dev/null || true
    sudo rm -rf /var/lib/backup-project-server/data                  # 所有备份 blob（不可逆）
    sudo rm -f  /var/lib/backup-project-server/state/metadata.sqlite3*
    sudo rm -f  /var/lib/backup-project-server/state/transport.key   # 删了它，所有客户端 pin 失效
    sudo rm -rf /var/lib/backup-project-server                       # 实例根（含证书等）
    sudo rm -rf /etc/backup-project-server                           # server.conf 与 secrets.env
    sudo rm -rf /var/cache/backup-project-server                     # 升级回滚缓存
    sudo userdel backup-project; sudo groupdel backup-project        # 数据没了，用户也没必要留

purge 之后 `backup-server-purge-data` 不存在了，所以 `postrm` 不会再建议你去跑它 ——
它只告诉你数据还在哪里，并指向这一节。

## 9. 自托管（自己的根）

想用自己的根签发证书（而不是官方根）：见
[自托管服务器](self-hosted-server.md)。工具 `backup-cert-tool` 已经在
`/usr/bin/backup-cert-tool`，根私钥必须留在离线机器上（0600），
**不要**放到服务器上。