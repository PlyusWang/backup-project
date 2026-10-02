# 远程备份使用说明（PR #21：BPSEC1 传输加密 + 远端增量）

本文说明怎么用 backup-server + BPNET1 + backupctl remote 把本地已经生成并
验证过的 .bak 归档送到远端（当前部署在阿里云 ECS），以及再取回来恢复。

## 1. 它是什么，不是什么

网络层是**存储后端 + 传输边界**：

    Source -> Filter -> Pack -> Compress -> Encrypt -> verified .bak
                                                          |
                                                   RemoteArchiveClient
                                                          |
                                                        BPNET1
                                                          |
                                                    backup-server
                                                          |
                                              blob storage + SQLite metadata

服务端**不重新实现**任何备份语义：不做 Filter、不做 MyPack/USTAR、不做压缩、
不做加密、不碰增量链、不做恢复。归档怎么产生、怎么恢复，仍然只有一套实现
（BackupEngine），网络层只搬运"一段有名字、有长度、有 SHA-256 的不透明字节"。

## 2. 拓扑与传输加密

    backupctl / Modern GUI
        |  BPSEC1（X25519 握手 + AES-256-CTR + HMAC-SHA256）  <- 机密性在这一层
        |  127.0.0.1:18765
        |  SSH encrypted tunnel                                <- 部署层纵深防御
        v
    ECS 127.0.0.1:18765 -> backup-server

* backup-server **只**绑 127.0.0.1，不绑 0.0.0.0，也不对外开放安全组。这是一条
  **硬约束**，不是默认值：Configure() 对任何非 127.0.0.1 的 --bind
  （0.0.0.0 / 私网地址 / 公网地址 / 127.0.0.2）都直接拒绝启动并说明原因，
  没有 --insecure / --allow-public 之类的开关。理由与加密无关（业务流量的
  机密性由 BPSEC1 提供，见 2.2）：只该由隧道访问的端口直接暴露在共享网络上
  没有任何好处，所以这条 fail-closed 规则继续保留。
* 本机用 SSH 隧道把 127.0.0.1:18765 转到 ECS 的 127.0.0.1:18765：

      ssh -o BatchMode=yes -o ExitOnForwardFailure=yes -N \
          -L 127.0.0.1:18765:127.0.0.1:18765 aliyun-ecs

* **BPNET1 的每一个字节都由 BPSEC1 保护**（PR #21）：整帧（含帧头里的
  opcode / status / 长度）作为明文加密封装成记录，Encrypt-then-MAC，序号严格
  递增抗重放。服务端有长期 X25519 身份密钥，客户端必须事先 pin 住它——没有
  配置 pin 就拒绝连接，本项目**不做**"第一次见到谁就信谁"。
* **BPSEC1 不是 TLS，也不与 TLS 兼容**：它是本项目自己实现的教学协议，
  没有经过外部审计。安全性质、明确的非目标与已知限制见 docs/secure_transport.md。

### 2.2 传输加密（BPSEC1）

    服务端：backup-server-keygen --output <服务器上的私钥文件>   # 0600，只在本机
    客户端：backupctl remote ... --server-key sha256:<指纹>
            （或把同一个值放进环境变量 BACKUP_REMOTE_SERVER_KEY）

* 服务端启动必须带 --transport-key-file <文件>（缺了直接以用法错误退出，
  没有"不加密也能连"的模式）；私钥只在服务器上存在，不进 Git、不进日志、不进 ZIP。
* 公钥与指纹不是秘密：backup-server-keygen --show --key-file <文件> 会打印
  "sha256:<指纹>" 与 "hex:<公钥>" 两种可直接使用的 pin。
* 每次 TCP 连接都会重新握手、重新派生会话密钥；连接断了重连就必须重新握手，
  然后照旧用 RESUME 恢复 token 会话（连接与会话仍然是两件事，见 2.1）。
* 握手失败、pin 不符、记录校验失败、重放/乱序：一律断连，并且**绝不**退回明文。
  错误分类（server-key-mismatch / record-authentication-failed / ...）会原样
  出现在 CLI 与 GUI 的错误信息里。

## 2.1 连接与会话是两件事

BPNET1 的**会话是 token，不是 TCP 连接**：

* token 是 12 小时有效的签名凭据，与具体连接无关；
* 服务端会在 `--io-timeout`（默认 30 秒）之后主动关掉**空闲**连接（慢连接保护）；
* SSH 隧道重启、网络抖动同样会断掉连接。

客户端因此把两件事分开处理：

    连接断了        -> 关掉这条 socket，**保留 token**；下一次操作先重连，
                       再用 RESUME（opcode 5）在新连接上恢复会话
    token 真的无效  -> 服务端明确回 UNAUTHORIZED（过期、被轮换、账户已注销）；
                       这时才丢掉 token，并要求重新登录

这条规则修的是人工验收里"点一次刷新就被退出登录 / 奇数次失败偶数次正常"：以前任何
一次连接断开都会把 token 一起丢掉。现在**每一次独立用户操作，第一次请求就得到确定
结果**——客户端在发送请求之前会先确认连接可用（`poll` + `MSG_PEEK` 探测对端是否
已经关闭），必要时先重连并恢复会话，然后才发送。

客户端**不会自动重发**已经发出去的请求（no retry）：失败就如实报错，用户再点一次
即可（upload / delete / delete-account 这类有副作用的操作尤其不能悄悄重试）。自动化
测试用服务端的"按操作码请求计数"证明失败之后服务端只收到过一次请求。

传输中途失败（本地文件被截短、目标目录不存在这类）同样不重试，但客户端**一定
会把服务端那边的传输事务结束掉**：上传用关闭连接当事务边界（服务端读到 EOF 会
删掉上传临时文件），下载显式发一次 DOWNLOAD_END（服务端允许客户端提前收手）。
失败之后 token 仍在手里，下一次操作重连 + RESUME 即可继续，不会出现"必须重新
登录才能继续"的状态。

## 3. 服务端

在 ECS 上（~/backup-project-server/）：

    bin/backup-server --bind 127.0.0.1 --port 18765 \
      --root ~/backup-project-server/data \
      --db ~/backup-project-server/state/metadata.sqlite3 \
      --secret-file ~/.config/backup-project-server/secrets.env \
      --log-file ~/backup-project-server/logs/server.log \
      --pid-file ~/backup-project-server/state/server.pid

* --root 下按 users/<数字 user id>/<snapshot id>.bak 存 blob；
  上传中的临时文件在 users/<id>/tmp/，删除中的文件先挪到 users/<id>/trash/。
* 客户端的显示名**只**进 SQLite，永远不参与路径拼接。
* --workers 是并发上限（默认 4，最多 64）：worker 全忙时新连接留在 listen
  backlog 里，不会无限生成线程。
* 同一个 root/db/port 上启动第二个实例会明确失败（PID 文件 + bind 冲突）。
* --secret-file 必须是**普通文件**、不是符号链接、并且 group / other 位一个都
  没有（0600；只读的 0400 也接受）：0640 / 0644 / 0660 / 0666 一律拒绝启动。
  检查发生在**已经打开的那个 fd** 上（open 带 O_NOFOLLOW，随后 fstat），
  所以"检查的是哪个文件"与"读的是哪个文件"必定是同一个，不存在检查与读取之间
  被换掉的窗口；拒绝信息里也不会出现 secret 的值。

部署脚本：scripts/deploy_aliyun_server.sh（打包源码子集 → 上传 → 在 ECS 上
构建 → 安装二进制；不上传 .git、testdata 或任何 secret）。

## 4. 客户端命令

    backupctl remote ping
    backupctl remote register --user <用户名>
    backupctl remote login --user <用户名>
    backupctl remote list --user <用户名>
    backupctl remote upload <本地归档> --user <用户名> [--name <显示名>] \
        [--repository <仓库目录>]
    backupctl remote download <快照ID> <目标路径> --user <用户名> [--force]
    backupctl remote delete <快照ID> --user <用户名>
    backupctl remote delete-account --user <用户名> --confirm <用户名>

* 端点默认 127.0.0.1:18765，可用 --host / --port 覆盖。
* 口令只从 /dev/tty 读（register 问两次）；命令行里**没有** --password。
  自动测试用 BACKUP_REMOTE_PASSWORD（只读不打印）。
* token 只在进程内存里，命令结束即丢弃；服务端不保存会话表。
* delete-account 会**永久删除**该账户以及它的全部云端备份（服务端删除，
  不可撤销），因此要 --confirm 逐字给出同一个用户名，服务端还会用当前
  口令再校验一次。它与"退出登录"完全不同：后者只清本机内存。
* upload --repository <仓库> 会先调用产品自己的
  LoadVerifiedSnapshotIdentity 证明这份归档"实际字节与声明一致"，再上传。
* download 默认不覆盖已存在的目标。中间产物是目标目录里**唯一命名的**临时文件
  （mkstemp，0600），长度与 SHA-256 都通过、fsync 并 close 之后才发布：
  不允许覆盖时用原子的"不覆盖"发布（link / renameat2(RENAME_NOREPLACE)，
  两个都不可用就 fail closed），因此**即使目标是在下载过程中才被别的进程创建
  的，也绝不会被覆盖**；--force 时才做原子替换。失败只会删掉自己那个唯一命名
  的临时文件，既不会留下半成品，也不会碰用户自己的 <目标>.part。

## 5. 认证与元数据

* 口令：PBKDF2-HMAC-SHA256，每个用户 16 字节随机 salt，200000 次迭代；
  校验用定长比较。库里没有明文口令、没有 SHA256(password)、没有固定 salt。
* token：HMAC-SHA256 无状态签名，签名覆盖 user_id / issued_at / expires_at /
  16 字节随机 nonce，固定 12 小时有效期。
* 元数据：SQLite 两张表 users / snapshots，WAL + synchronous=FULL，
  预编译语句 + 绑定参数，进程内一把互斥锁把写串行化。
* 归属隔离：所有查询都带 user_id，别人的 snapshot id 与不存在的 id 返回
  完全相同的 NOT_FOUND，不能用来探测"某个 id 是否存在"。

## 5.1 服务器管理员：SSH 到 ECS 后使用本机管理工具

管理员能力**不在协议里**，也不在 GUI / CLI 里。唯一的使用方式是先 SSH 登录到
ECS，再在 ECS 本机运行 backup-server-admin：

    ssh aliyun-ecs
    cd ~/backup-project-server

    ./bin/backup-server-admin.sh          # 交互菜单（推荐）
    ./bin/backup-server-admin --help      # 子命令用法

菜单三块：用户管理（列表 / 详情 / 删除用户及其全部备份）、备份文件管理
（按用户列出 / 详情 / 删除单个快照）、存储概览（用户数 / 快照总数 / blob 总
大小 / 占用最多的用户），外加服务状态。

**每一次运行都会先打印"我在看哪个实例"**：

    Host:        2025040908016
    Server root: /home/ubuntu/backup-project-server
    Data root:   /home/ubuntu/backup-project-server/data
    Metadata DB: /home/ubuntu/backup-project-server/state/metadata.sqlite3
    Service:     backup-server 正在运行（pid=… started_at=…）

    用户数：12　快照数：0　blob 总大小：0 B　已注销账户：9

路径一律显示 realpath 之后的绝对路径：脱离上下文的相对路径正是"看错实例"的
温床（见下面那条 P0）。

**用户选择器：不会替你猜**（`show-user` / `list-snapshots` / `--user`）：

    id:<编号>        只按编号找，例如 id:23
    name:<用户名>    只按用户名找，例如 name:001（用户名允许是纯数字）
    裸输入           只有"不产生歧义"时才被接受

* 裸输入同时命中一个编号和一个用户名的账户时**拒绝执行**，并把两种写法都打印
  出来：`输入 001 存在歧义。请使用 id:001 或 name:001。`——旧实现直接把这个
  数字当编号用，用户看到的可能是**另一个账户**，而且屏幕上没有任何提示；
* 只命中一个候选时照常执行，但在 stderr 上说明这次按哪一种解析，并提示明确
  写法：`提示：001 这次按编号解析（id=1）。写成 id:001 就不会有歧义。`；
* **删除操作必须写成显式形式**（`delete-user`、`delete-snapshot --user`）：
  删除不可逆，不能由一个"这个数字到底是编号还是用户名"的疑问决定删掉谁；
* `delete-user` 的确认串是 `DELETE <用户名>#<编号>`：用户名与编号都要出现，
  同名不同号的两个账户在确认这一行就能分清；
* 快照 id 也可以写成 `snapshot:<32 位十六进制>`，与裸写法等价，校验规则不变。

**状态根从部署布局推导，且 fail closed**：

    <server-root>/bin/backup-server-admin.sh   从自己所在目录推 <server-root>
    <server-root>/data                         数据根（--root）
    <server-root>/state/metadata.sqlite3       元数据库（--db）

* `BACKUP_SERVER_ROOT` / `BACKUP_SERVER_DATA` / `BACKUP_SERVER_DB` 可以覆盖；
* 管理工具**只打开已经存在的数据库**（不带 SQLITE_OPEN_CREATE），而且打开方式
  按命令分成两个入口：只读命令用 `OpenExistingReadOnly`
  （SQLITE_OPEN_READONLY + PRAGMA query_only），破坏性命令用
  `OpenExistingReadWrite`（调用方必须先拿到数据目录锁）。两者都**只校验**
  schema（版本 + 需要的表都在），不建表、不写 user_version、不开写事务；
  路径不对就报错退出，绝不创建一个空库。原因是人工验收里出过一次真实事故：
  wrapper 的默认值是 `<server-root>/data/metadata.sqlite3`（正确的位置是
  `state/`），SQLite 在文件不存在时会新建，于是管理工具安静地读了一个**自己刚
  建出来的空库**，屏幕上"还没有任何用户"与"你看错实例了"完全一样，而 GUI 那边的
  "已登录"其实是对的。
* 回归测试：`scripts/same_instance_truth_test.sh`（本地四源一致性 + 错误实例根
  必须失败且不创建文件）与 `scripts/aliyun_truth_matrix.sh`（ECS 真机：
  客户端 / 管理 CLI / SQLite 三方真值矩阵）。

边界（每一条都有自动测试）：

* 管理工具**不监听任何端口**（源码里没有 socket/bind/listen/accept），也不
  新增公网或 localhost 的 admin 端口；
* 它是**本机程序**：任何已经获得合法 ECS SSH 权限的终端都能用——Windows
  物理机、Ubuntu 开发 VM 都一样，都是"SSH 进去，再在本机执行"；
* 破坏性操作（删快照 / 删账户）复用服务端的**同一份**删除实现，并且要求
  --confirm 与目标逐字一致；同时必须先抢到数据目录锁。
* 只读操作（列表 / 详情 / 概览）在服务端运行时照常可用，而且它们走的是
  **只读连接**：不建表、不写 user_version、不开写事务，"不会写库"是连接本身的
  性质而不是约定；**破坏性操作在 backup-server 运行时被明确拒绝**，并提示先停
  服务，而不是与正在写的服务端竞态（这不是 pgrep 猜一下，而是内核持有的 flock）。
* 输出里永远不会出现口令 salt / hash 或 token secret。

数据目录锁是 `<root>/.backup-server.lock` 上的一把 flock：进程崩溃、被
SIGKILL 都会自动释放，不留 stale 状态；同一个数据目录上启动第二个
backup-server 也会因此明确失败。

部署：scripts/deploy_aliyun_server.sh 会把 backup-server-admin 与
scripts/backup-server-admin.sh 一起装到 ECS 的 bin/ 下。

## 6. 当前限制

* BPSEC1 是本项目手写的教学协议：不是 TLS、没有外部审计、没有形式化验证。
  它提供机密性、完整性、服务端身份 pin、抗重放与每连接前向保密；
  **不**声称与任何标准传输层兼容。
* 没有 systemd / 守护进程化；服务端就是前台进程 + PID 文件。
* 不支持断点续传（resume）：一次被中断的上传/下载会在服务端清理临时文件，
  用户需要重新执行该命令。
* 远端增量（PR #21）已经可用：remote backup / remote restore 会复用本地
  增量引擎生成与恢复 delta 链，链关系（父 / 代数 / lineage）由服务端校验，
  依赖感知删除保证链不会从中间断开。用法与限制见 docs/remote_incremental.md。
* token 在有效期内无法单独吊销（服务端无会话状态）；轮换
  BACKUP_TOKEN_SECRET 会让所有已签发 token 立即失效。唯一的例外是账户
  注销：账户行不存在之后，旧 token 在任何操作上都会被拒绝（每次操作都会
  回查账户是否还在），而且被注销账户的 user id 不会被重用。
* 管理工具不做"运行中删除"：backup-server 在跑时，破坏性管理操作会被拒绝
  （见 5.1），需要先停服务。
* list 一次最多 4096 条、响应必须装进一个 1 MiB 的帧；超出会明确报
  TOO_LARGE，没有分页。
* 登录失败没有速率限制（当前部署只在 SSH 隧道内可达）。
* 服务端只校验磁盘 blob 的**长度**与元数据一致；内容完整性由客户端按
  DOWNLOAD_BEGIN 声明的 SHA-256 判定。
* 删除时若最后一步 unlink 失败，会留下一个不可见的 trash 孤儿（元数据
  已经删除），需要将来的 startup reconciliation 清理。

## 7. 测试

    bash scripts/secure_transport_test.sh   # BPSEC1：官方向量 / 握手 / 线上字节
                                            #   捕获 / 篡改矩阵 / pin 校验
    bash scripts/network_test.sh            # 单元 + 本地 CLI 端到端
                                            #   （含 remote_sequence_test：
                                            #    空闲超时 / 错误口令 × 6 /
                                            #    LIST × 10 / 20 轮 / 注销序列 /
                                            #    不重发 的确定性回归）
    bash scripts/account_deletion_test.sh   # 账户注销端到端（真实服务端 + 磁盘）
    bash scripts/same_instance_truth_test.sh # 四源一致性（客户端/管理 CLI/DB/进程）
    bash scripts/aliyun_truth_matrix.sh      # ECS 真机三方真值矩阵
    bash scripts/final_gate.sh               # canonical final gate（全部套件）
    bash scripts/server_admin_test.sh       # ECS 本地管理工具的安全边界
    bash scripts/aliyun_network_e2e.sh      # 阿里云真实端到端（需要隧道前置条件）

scripts/network_test.sh 覆盖：协议编解码边界、认证、元数据与归属隔离、
流式传输（1 字节 / 块边界 +-1 / 5 MiB / 32 MiB）、七种失败路径之后"磁盘上
不留已发布 blob 与临时文件"、backupctl remote 全流程，以及启动边界
（--bind 非环回地址一律拒绝且不产生 listener；secret 文件权限 / 类型 /
符号链接不合格时拒绝启动）。

下载发布的 TOCTOU 回归在 tests/unit/remote_client_test.cpp（CLI T4）：目标在
"下载开始之后、发布之前"才出现时，这次下载必须失败、目标内容一个字节不变、
自己的唯一临时文件被清掉，而预先存在的 <目标>.part 一字未动。

传输事务清理的回归也在同一个文件：CLI T5 在 UPLOAD_BEGIN 之后把本地文件截短，
CLI T6 在 DOWNLOAD_BEGIN 之后让临时文件必然建不出来。两者都必须失败、都不留临时
文件，而且紧接着的 List（必要时自动重连 + RESUME）必须成功——服务端不能被留在
"上传中 / 下载中"。两个触发点都在代码里确定的位置，不靠 sleep。
