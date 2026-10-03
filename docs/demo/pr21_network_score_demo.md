# PR #21 网络加分现场演示（最短路径）

> 目标：**7 分钟**内演示"传输加密"与"远端增量"两部分。所有命令都不含口令、
> 不含私钥；服务端身份指纹是公开信息，可以投屏。

## 0. 前置（演示机与 ECS）

    1) Ubuntu VM 上有一条到 ECS 的隧道（本项目的常规部署方式）：
         ssh -o BatchMode=yes -o ExitOnForwardFailure=yes -f -N \
             -L 127.0.0.1:18765:127.0.0.1:18765 aliyun-ecs
    2) 服务端只绑 127.0.0.1（演示前可以当面确认）：
         ssh aliyun-ecs "ss -ltn | grep 18765"
       -> 只应该出现 127.0.0.1:18765
    3) 服务端有传输身份私钥（泄漏不了：它不离开服务器）：
         ssh aliyun-ecs "~/backup-project-server/bin/backup-server-keygen --show \
             --key-file ~/backup-project-server/state/transport.key"
       -> 打印公钥与指纹；下面把这一行的 sha256:... 当作 pin

## 1. 传输加密（3 分钟）

    # (1) 没有 pin 就不许连：明确报错、不做"第一次见到谁就信谁"
    backupctl remote ping --host 127.0.0.1 --port 18765
    #   -> Error: 缺少 --server-key <sha256:指纹|hex:公钥> …（退出码 2）

    # (2) 给出正确的 pin：加密握手成功
    export BACKUP_REMOTE_SERVER_KEY=sha256:<粘贴上一步的 64 位十六进制指纹>
    backupctl remote ping --host 127.0.0.1 --port 18765
    #   -> PING 正常: backup-server 协议版本 1 服务端时间 …

    # (3) 给出错误的 pin：握手失败，且**不会**退回明文
    backupctl remote ping --host 127.0.0.1 --port 18765 \
        --server-key sha256:$(printf 'ab%.0s' $(seq 32))
    #   -> Error: BPSEC1 握手失败（server-key-mismatch）…

    # (4) 注册一个演示账户（口令只从终端读，不回显、不进 shell 历史）
    backupctl remote register --user demo-20261003 --host 127.0.0.1 --port 18765

    # (5) 登录（同样只从终端读口令）
    backupctl remote login --user demo-20261003 --host 127.0.0.1 --port 18765

演示要点（口头）：从 (2) 开始，**每一个字节**（包括 opcode、长度这些帧头字段）
都在 BPSEC1 记录里加密，并且带 HMAC 认证；抓包只能看到长度与随机密文。
线上字节 0 次出现口令 marker 的自动化证据见 04-WIRE-CAPTURE.txt。

## 2. 远端增量（4 分钟）

    # (1) 准备一个 20 MiB 左右的源目录
    mkdir -p ~/demo-src && head -c 20000000 /dev/urandom > ~/demo-src/big.bin
    echo v1 > ~/demo-src/notes.txt

    # (2) 第一次：完整基线（没有可信基线时引擎自己建）
    backupctl remote backup ~/demo-src --strategy full \
        --user demo-20261003 --name demo-R0
    #   -> 类型: full / 代数: 0 / 本次上传: 20175xx 字节

    # (3) 改一个文件（只改 5 个字节）
    echo v2 >> ~/demo-src/notes.txt

    # (4) 第二次：增量。只看"本次上传"那一行
    backupctl remote backup ~/demo-src --user demo-20261003 --name demo-R1
    #   -> 类型: incremental / 代数: 1 / 父快照: <R0 的 id>
    #   -> 本次上传: 约 2.7 KB（而不是 20 MB）
    #   -> 链根大小: 20175xx 字节（本次只传了增量部分）

    # (5) 列表：kind / generation / parent 一眼可见
    backupctl remote list --user demo-20261003
    #   -> … full         0  -             demo-R0
    #      … incremental  1  <R0 前 12 位> demo-R1

    # (6) 冷缓存恢复：先删掉本地缓存，再只指定目标快照
    rm -rf ~/.config/backup-project/backup-gui-modern/remote-cache
    backupctl remote restore <R1 的快照 ID> /tmp/demo-restore \
        --user demo-20261003
    #   -> 依赖链: 2 份快照（1 个增量）/ 本次下载: … 字节

    # (7) 一致性：现场 diff
    diff -r ~/demo-src /tmp/demo-restore && echo DEMO_DIFF_PASS

    # (8) 依赖感知删除：有后代就不许删
    backupctl remote delete <R0 的快照 ID> --user demo-20261003
    #   -> Error: 当前状态不允许这个操作（…还有增量快照依赖它，必须先删后代）

    # (9) 收尾：注销临时账户（连带它的云端数据）
    backupctl remote delete-account --user demo-20261003 --confirm demo-20261003

## 3. 常见追问（准备好答案）

* **"这只是把 delta 上传了吧？"** 不是：服务端有 parent/generation/lineage 元数据，
  父必须存在、同用户、同 lineage，代数由服务端按父推导；(8) 的依赖删除就是证据。
* **"服务端会不会自己扫目录算 diff？"** 不会：服务端只有 blob + 元数据，
  没有任何备份引擎代码（server/ 不链接 BackupEngine / Filter / MyPack）。
* **"重放一次上传行不行？"** 记录层序号严格递增，重放会被拒（见 05-TAMPER-MATRIX.txt）。
* **"服务端元数据被改了怎么办？"** 客户端对**下载到的实际字节**做 SHA-256，
  再让引擎按归档自己的信封校验父子关系；证据见 07-CHAIN-E2E 与本轮的
  10-CHAIN-INVARIANTS.txt。

## 4. 没有 GUI 的部分（如实说明）

远程页目前只有：连接设置（host/port/账户/口令/**服务端身份指纹**）、登录注册、
上传（选文件）、列表、下载、删除。**没有**"远端完整/增量备份"与"远端链恢复"的
界面入口——那两条目前只在 CLI 上（core / CLI / ECS 的端到端证据齐全）。
