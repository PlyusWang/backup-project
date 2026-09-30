# 远程备份使用说明（PR #20 Network Backup Foundation）

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
        |  127.0.0.1:18765
        |  SSH encrypted tunnel
        v
    ECS 127.0.0.1:18765 -> backup-server

* backup-server **只**绑 127.0.0.1，不绑 0.0.0.0，也不对外开放安全组。
* 本机用 SSH 隧道把 127.0.0.1:18765 转到 ECS 的 127.0.0.1:18765：

      ssh -o BatchMode=yes -o ExitOnForwardFailure=yes -N \
          -L 127.0.0.1:18765:127.0.0.1:18765 aliyun-ecs

* **BPNET1 本身没有原生 TLS**：当前的传输机密性完全来自它跑在 SSH 隧道里。
  代码结构把"连接 + 收发"限制在一个很小的接口上，将来替换成 TLS 不需要动
  协议语义，但**本版本没有实现原生 TLS**，不要当成已有能力。

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

* 端点默认 127.0.0.1:18765，可用 --host / --port 覆盖。
* 口令只从 /dev/tty 读（register 问两次）；命令行里**没有** --password。
  自动测试用 BACKUP_REMOTE_PASSWORD（只读不打印）。
* token 只在进程内存里，命令结束即丢弃；服务端不保存会话表。
* upload --repository <仓库> 会先调用产品自己的
  LoadVerifiedSnapshotIdentity 证明这份归档"实际字节与声明一致"，再上传。
* download 默认不覆盖已存在的目标；写入 <目标>.part，长度与 SHA-256 都
  通过之后才原子改名，失败只删自己的 .part。

## 5. 认证与元数据

* 口令：PBKDF2-HMAC-SHA256，每个用户 16 字节随机 salt，200000 次迭代；
  校验用定长比较。库里没有明文口令、没有 SHA256(password)、没有固定 salt。
* token：HMAC-SHA256 无状态签名，签名覆盖 user_id / issued_at / expires_at /
  16 字节随机 nonce，固定 12 小时有效期。
* 元数据：SQLite 两张表 users / snapshots，WAL + synchronous=FULL，
  预编译语句 + 绑定参数，进程内一把互斥锁把写串行化。
* 归属隔离：所有查询都带 user_id，别人的 snapshot id 与不存在的 id 返回
  完全相同的 NOT_FOUND，不能用来探测"某个 id 是否存在"。

## 6. 当前限制

* 原生 TLS 未实现，机密性依赖 SSH 隧道。
* 没有 systemd / 守护进程化；服务端就是前台进程 + PID 文件。
* 不支持断点续传（resume），也不支持远程块级增量（delta）。
* token 在有效期内无法单独吊销（服务端无会话状态）；轮换
  BACKUP_TOKEN_SECRET 会让所有已签发 token 立即失效。
* list 一次最多 4096 条、响应必须装进一个 1 MiB 的帧；超出会明确报
  TOO_LARGE，没有分页。
* 登录失败没有速率限制（当前部署只在 SSH 隧道内可达）。
* 服务端只校验磁盘 blob 的**长度**与元数据一致；内容完整性由客户端按
  DOWNLOAD_BEGIN 声明的 SHA-256 判定。
* 删除时若最后一步 unlink 失败，会留下一个不可见的 trash 孤儿（元数据
  已经删除），需要将来的 startup reconciliation 清理。

## 7. 测试

    bash scripts/network_test.sh            # 单元 + 本地 CLI 端到端
    bash scripts/aliyun_network_e2e.sh      # 阿里云真实端到端（需要隧道前置条件）

scripts/network_test.sh 覆盖：协议编解码边界、认证、元数据与归属隔离、
流式传输（1 字节 / 块边界 +-1 / 5 MiB / 32 MiB）、七种失败路径之后"磁盘上
不留已发布 blob 与临时文件"、以及 backupctl remote 全流程。
