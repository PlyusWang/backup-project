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

## 4. GUI（本轮补齐：CLI 与 Modern GUI 是同一套 core）

远程页现在分两层能力，刻意不混在一起：

* **远端备份（产品级）**：选**源目录** + 策略（完整 / 增量）→ "开始远端备份"。
  与 backupctl remote backup 走**同一个** core；界面只传"源目录 + 策略"，
  能不能续链、父是谁、代数、lineage 全部由 core 决定。用户选"增量"而云端还没有
  可续的链时，界面如实说"本次创建的是完整基线"；源目录没有变化时显示
  "没有检测到有效变化，本次未创建新备份"，云端列表也不会多一行。
* **云端备份（列表 + 恢复）**：每条显示 完整/增量、代数、父快照前 12 位；
  主操作是 **恢复**（自动解析并下载整条依赖链、逐成员校验、原子发布到目标目录），
  次级操作是 **下载归档**（低层 raw：只取回那一个 blob）与 **删除**。
  原始归档上传（"高级"区域）产生的条目不属于备份链：卡片上直接写明原因并禁用
  "恢复"，而不是等点了以后才报底层错误。

GUI 演示路径（约 3 分钟）：

    连接（host / port / 服务器身份指纹 -> 注册 -> 登录）
    -> "选择源目录" 挑一个目录 -> 策略选"增量" -> 开始远端备份
       （第一次云端没有可续的链，界面会说明"本次创建的是完整基线"）
    -> 改一个文件 -> 再点一次"开始远端备份"
       （这次是真的增量：列表里代数 +1、父=上一次的快照）
    -> 在列表里点这一条的"恢复" -> 选一个空目录（自动拉整条链）
    -> diff -r 源目录 目标目录（逐字节一致）

对应的自动化证据：backup-gui-modern --remote-test 的 GUI-P01..P12（见
pr21-gui-closure-handoff.zip 里的 02-GUI-CONTRACT.txt / 10-ECS-GUI-E2E.txt）。

本轮的无人值守验收再往前一步：--remote-acceptance 把上面这条演示路径真的
跑一遍（本地隔离服务端与 ECS 真机各一次），并把 12 张**真实窗口**截图
（1180x760 浅色 / 深色 + 900x700 窄窗口 + 两张整页总览）连同每个关键控件的
真实几何（x / y / 宽 / 高 / 可见 / 可用）一起留下来，同时断言：不重叠、不越界、
卡片内容在卡片内、恢复按钮在卡片内、策略分段控件在"远端备份"卡片内。
截图与几何证据见 pr21-final-acceptance-handoff.zip（02-GUI-ACCEPTANCE.md /
03-GEOMETRY-CHECKS.txt / 04-QML-WARNINGS.txt / screenshots/）。

长操作进行中，页面上会直接显示"正在做什么"（remoteBusyText，文本来自控制器的
busyAction），冲突操作同时变成不可用——不是只把按钮变灰，也不是画一条
假的进度条。

本轮的无人值守验收再往前一步：--remote-acceptance 把上面这条演示路径真的
跑一遍（本地隔离服务端与 ECS 真机各一次），并把 12 张**真实窗口**截图
（1180x760 浅色 / 深色 + 900x700 窄窗口 + 两张整页总览）连同每个关键控件的
真实几何（x / y / 宽 / 高 / 可见 / 可用）一起留下来，同时断言：不重叠、不越界、
卡片内容在卡片内、恢复按钮在卡片内、策略分段控件在"远端备份"卡片内。
截图与几何证据见 pr21-final-acceptance-handoff.zip（02-GUI-ACCEPTANCE.md /
03-GEOMETRY-CHECKS.txt / 04-QML-WARNINGS.txt / screenshots/）。

长操作进行中，页面上会直接显示"正在做什么"（remoteBusyText，文本来自控制器的
busyAction），冲突操作同时变成不可用——不是只把按钮变灰，也不是画一条
假的进度条。

本轮的无人值守验收再往前一步：--remote-acceptance 把上面这条演示路径真的
跑一遍（本地隔离服务端与 ECS 真机各一次），并把 12 张**真实窗口**截图
（1180x760 浅色 / 深色 + 900x700 窄窗口 + 两张整页总览）连同每个关键控件的
真实几何（x / y / 宽 / 高 / 可见 / 可用）一起留下来，同时断言：不重叠、不越界、
卡片内容在卡片内、恢复按钮在卡片内、策略分段控件在"远端备份"卡片内。
截图与几何证据见 pr21-final-acceptance-handoff.zip（02-GUI-ACCEPTANCE.md /
03-GEOMETRY-CHECKS.txt / 04-QML-WARNINGS.txt / screenshots/）。

长操作进行中，页面上会直接显示"正在做什么"（remoteBusyText，文本来自控制器的
busyAction），冲突操作同时变成不可用——不是只把按钮变灰，也不是画一条
假的进度条。

本轮的无人值守验收再往前一步：--remote-acceptance 把上面这条演示路径真的
跑一遍（本地隔离服务端与 ECS 真机各一次），并把 12 张**真实窗口**截图
（1180x760 浅色 / 深色 + 900x700 窄窗口 + 两张整页总览）连同每个关键控件的
真实几何（x / y / 宽 / 高 / 可见 / 可用）一起留下来，同时断言：不重叠、不越界、
卡片内容在卡片内、恢复按钮在卡片内、策略分段控件在"远端备份"卡片内。
截图与几何证据见 pr21-final-acceptance-handoff.zip（02-GUI-ACCEPTANCE.md /
03-GEOMETRY-CHECKS.txt / 04-QML-WARNINGS.txt / screenshots/）。

长操作进行中，页面上会直接显示"正在做什么"（remoteBusyText，文本来自控制器的
busyAction），冲突操作同时变成不可用——不是只把按钮变灰，也不是画一条
假的进度条。

本轮的无人值守验收再往前一步：--remote-acceptance 把上面这条演示路径真的
跑一遍（本地隔离服务端与 ECS 真机各一次），并把 12 张**真实窗口**截图
（1180x760 浅色 / 深色 + 900x700 窄窗口 + 两张整页总览）连同每个关键控件的
真实几何（x / y / 宽 / 高 / 可见 / 可用）一起留下来，同时断言：不重叠、不越界、
卡片内容在卡片内、恢复按钮在卡片内、策略分段控件在"远端备份"卡片内。
截图与几何证据见 pr21-final-acceptance-handoff.zip（02-GUI-ACCEPTANCE.md /
03-GEOMETRY-CHECKS.txt / 04-QML-WARNINGS.txt / screenshots/）。

长操作进行中，页面上会直接显示"正在做什么"（remoteBusyText，文本来自控制器的
busyAction），冲突操作同时变成不可用——不是只把按钮变灰，也不是画一条
假的进度条。

本轮的无人值守验收再往前一步：--remote-acceptance 把上面这条演示路径真的
跑一遍（本地隔离服务端与 ECS 真机各一次），并把 12 张**真实窗口**截图
（1180x760 浅色 / 深色 + 900x700 窄窗口 + 两张整页总览）连同每个关键控件的
真实几何（x / y / 宽 / 高 / 可见 / 可用）一起留下来，同时断言：不重叠、不越界、
卡片内容在卡片内、恢复按钮在卡片内、策略分段控件在"远端备份"卡片内。
截图与几何证据见 pr21-final-acceptance-handoff.zip（02-GUI-ACCEPTANCE.md /
03-GEOMETRY-CHECKS.txt / 04-QML-WARNINGS.txt / screenshots/）。

长操作进行中，页面上会直接显示"正在做什么"（remoteBusyText，文本来自控制器的
busyAction），冲突操作同时变成不可用——不是只把按钮变灰，也不是画一条
假的进度条。
