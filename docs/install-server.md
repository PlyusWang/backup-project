# 安装服务端（Backup Project Server）

本文讲**服务器管理员**要做的事：装、初始化、开机自启、管理、升级、卸载，
以及怎样**安全地**把它开到公网。

发行包面向 **Linux x86_64**。本轮候选在 **Ubuntu 24.04.4 LTS / glibc 2.39**
构建并验证；具体构建系统、glibc、编译器与提交号以该包的 `BUILD-INFO.txt` 和
发行目录的 `RELEASE-INFO.txt` 为准。不能据此承诺 Ubuntu 20.04、22.04 或 Debian 12
兼容：旧系统可能缺少所需的 glibc、libstdc++ 或其他运行依赖，安装前应核对该包的
依赖并在目标系统验证。Windows、macOS、ARM64 尚无本轮构建与运行验证。

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

### 2.1 便携实例的首次初始化

在解压后的服务器包目录运行：

```bash
./backup-server-manager
```

已通过便携安装器安装时，运行安装完成后打印的管理菜单命令。安装器会保留已有实例的
配置和身份；不要重新初始化已经配置或含备份数据的目录。

选择“初始化服务器”，按提示输入实例根目录的绝对路径、监听 IPv4 地址和业务端口。
默认 `127.0.0.1` 供本机客户端或 SSH 隧道访问；其他机器不能直接用这个地址连接本机。

传输身份可选 `1` 生成新身份，或选 `2` 导入已有私钥。导入时输入原私钥的绝对路径，
核对显示的公钥与指纹，再输入 `IMPORT IDENTITY`；原文件不会被修改，目标私钥以
0600 权限保存，不覆盖已有身份。

选择 BPSEC2 后，证书来源有两种：

- `1`：创建本机测试根并签发证书，再输入 `server_id`。测试根私钥保存在实例的
  `private/root.key`，仅客户端所需的根公钥与身份证书可共享。
- `2`：导入已有根公钥及服务器身份证书，分别输入两个文件的绝对路径，并在通过
  可信渠道核对根公钥后输入 `IMPORT BPSEC2`。只提供根公钥，不提供根私钥。
  向导检查签名、有效期和证书公钥是否匹配所选传输身份，不匹配则拒绝初始化。

非回环监听只能使用 BPSEC2。向导要求填写用途，再输入 `ALLOW PUBLIC` 才创建配置，
并设置 `require_bpsec2 = true`；不会修改防火墙、安全组或其他网络放行规则。
`0.0.0.0` 表示监听全部 IPv4 网卡，客户端应填写服务器实际可达的局域网或公网地址。

初始化完成后查看“连接与身份信息”：BPSEC1 显示指纹，根公钥和证书为“不适用”；
BPSEC2 显示可信根公钥、服务器身份证书的完整路径及真实证书身份。将公开材料复制到
客户端机器，再在 GUI 中选择对应的客户端本地文件路径。确认信息后选择“启动服务器”；
初始化本身不会自动启动监听。

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
**服务器身份信息**（客户端要填的指纹与公钥）。菜单“管理证书与可信根”支持安全导入
已有根公钥与服务器证书：先停服务，校验签名、有效期和当前传输公钥，再输入
`IMPORT BPSEC2` 确认；导入不接触根私钥或传输私钥，旧材料保留。

安装版首次生成的是 BPSEC1 传输身份。要为它添加本机测试证书，在主菜单先选
“停止服务器”，再选“管理证书与可信根” → `2. 为当前身份签发本机测试证书`，
输入 `server_id`，核对说明后输入 `CREATE TEST CERTIFICATE`。菜单为现有传输公钥
签发证书并设置仅接受 BPSEC2，不更换传输私钥，也不删除旧证书、用户或备份。

测试根私钥保存在实例的 `private/test-root.<标识>/root.key`，权限 0600；客户端资料
另存于 `state/client-identity.<标识>/trusted-root.pub` 与 `server.bpcert`。以菜单打印的
完整路径为准，仅将根公钥和服务器身份证书通过可信渠道复制到客户端；根私钥不能共享。
完成后选择“启动服务器”，再在“服务器身份与连接信息”查看并复制新证书资料。
重新启动已有实例不会重新生成密钥或证书。

菜单“查看与修改服务器配置”可修改端口、工作线程数、I/O 超时、日志路径和认证方式。
输入 `SAVE` 前不会发布新配置，保存前使用启动器的 `--check-config` 校验；旧配置
保存在配置目录下的 `.server-config-backup.<标识>/server.conf`。公网安全条件、
数据路径和私钥路径不由该编辑器修改。允许 BPSEC1 需另输入 `ALLOW BPSEC1` 明确确认，
切回仅 BPSEC2 时重新验证证书与传输身份。操作详情见
[self-hosted-server.md](self-hosted-server.md)。

证书导入、测试证书签发和配置编辑都要求服务已停。在菜单选择“停止服务器”，完成编辑后再选择
“启动服务器”；安装版只调用固定的 `backup-project-server.service`，并核实实际进程和
端口，不以 root 另启服务端。若系统没有 systemd，使用服务器便携包的
`./backup-server-manager` 管理独立实例。也可手动执行
`sudo systemctl stop backup-project-server` / `sudo systemctl start backup-project-server`。

菜单自身不绕过数据目录锁。需要 `ss`（`iproute2`）查询真实监听及 `flock`
（`util-linux`，同时提供 `runuser`）防止与运行中的服务并发修改；Debian 包声明了这两项运行依赖，便携版
所在系统也需安装它们。缺少工具时菜单明确失败。

`sudo` 用于管理固定系统服务和受保护的实例配置；安装版的数据管理命令会通过
`runuser -u backup-project` 调用共享后台，与服务端使用同一个数据目录锁。
便携版保持当前用户直接调用后台。管理工具只在本机跑，不监听任何端口。

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

**快照是升级的前置条件（fail closed）**：上面那份材料先建在 `rollback.tmp.<pid>/` 里，
逐项自检（manifest 非空、version 非空、条目数一致、每条路径都在载荷白名单里、每个
regular file 的 sha256 对得上）通过之后，才**原子换入** `rollback/`。只要这一步有任何
问题 —— 读不到旧包文件清单、复制失败、manifest 写不下去、自检不过、一个文件都没有 ——
`preinst` 就返回非零，`dpkg` 因此**在解包之前**中止这次升级：

    [backup-project-server] preinst: ERROR: rollback snapshot preparation failed: ...
    [backup-project-server] preinst: ERROR: upgrade aborted before unpack
    [backup-project-server] preinst: ERROR:   old service was active（...）
    [backup-project-server] preinst: ERROR:   existing runnable installation left untouched（...）

也就是说：**旧服务本来在跑的时候，快照建不起来就不升级** —— 不会出现"新二进制覆盖了
旧版本、然后又起不来"。反过来，旧服务升级前本来就没在跑时没有这个前提：不做快照、
允许升级，只是不承诺回滚到"可运行旧版本"。

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

    sudo apt remove backup-project-server     # 删除程序，保留配置、数据与身份
    sudo apt purge backup-project-server      # 另删除 server.conf，保留数据、身份与生成的 secret

`server.conf` 是 dpkg 管理的配置文件：`remove` 会保留它，`purge` 会删除它。
自动生成的 `secrets.env`、`/var/lib/backup-project-server` 下的备份数据、数据库与
传输身份私钥不随卸载或 purge 删除。如需保留监听地址、端口和证书路径等设置，请在 purge
之前备份 `/etc/backup-project-server/server.conf`。

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