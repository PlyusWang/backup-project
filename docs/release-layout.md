# 发布布局（release layout）

本文说明 PR #23 起的"客户端 / 服务端分开发布"到底发布什么、怎么构建、
拿到包的人怎么核对。**本文只描述已经存在的东西**。

## 1. 为什么要分开

客户端和服务端是两件不同的东西：

* **客户端**装在用户自己的机器上（Ubuntu 桌面），交付物是 `backupctl`
  （命令行）和 `backup-gui-modern`（现代 GUI）；
* **服务端**装在服务器上，交付物是 `backup-server` 和两个只在服务器本机
  跑的运维工具。

把两者打成一个包会带来一个很实际的坏处：客户端升级时会顺手把服务器二进制
也换掉，而服务器上还有状态（数据库、传输身份私钥）。分开之后，客户端的
升级完全不用碰服务器。

## 2. 构建入口

    make client        # backupctl + backup-gui-modern（需要 Qt6）
    make client-cli    # 只出 backupctl（没有 Qt 的构建机用）
    make server        # backup-server + backup-server-admin + backup-server-keygen
    make cert-tool     # backup-cert-tool（离线根与证书工具）

`make all` 的行为保持不变（`backupctl` + `backup-server`），既有的脚本与
流程不受影响。

打包脚本：

    bash scripts/stage-client-release.sh [--out <目录>] [--no-gui] [--allow-dirty]
    bash scripts/stage-server-release.sh [--out <目录>] [--allow-dirty]

两个脚本都遵守同一条规矩：**工作树不干净就拒绝打包**。真想打一个"内容和
commit 对不上"的包时必须显式给 `--allow-dirty`，而且 BUILD-INFO.txt 里会
写明 dirty —— 这类不一致是最难查的问题，不能靠默认行为掩盖。

## 3. 包内容

### dist/client/

    bin/backupctl                                    客户端 CLI
    bin/backup-gui-modern                            现代 GUI（--no-gui 时没有）
    share/backup-project/official-root-ed25519.pub   官方根公钥（公开材料）
    share/backup-project/VERSION                      git describe 结果
    docs/                                             随包文档
    BUILD-INFO.txt                                    commit / 时间 / 是否含 GUI / 编译器
    MANIFEST.sha256                                   包内每个文件的 sha256（按路径排序）

### dist/server/

    bin/backup-server            服务端
    bin/backup-server-admin      本机管理工具（不监听端口）
    bin/backup-server-keygen     传输身份密钥工具
    tools/backup-cert-tool       离线根与证书工具
    share/backup-project/official-root-ed25519.pub
    share/backup-project/VERSION
    docs/
    BUILD-INFO.txt / MANIFEST.sha256

服务端包里**没有任何私钥**：

* 传输身份私钥由 `backup-server-keygen` 在服务器本机生成（0600），从不随
  包分发、也从不出现在产物里；
* 根私钥永远留在离线机器上，服务端包里只有公开的根公钥。

这一点在打包脚本里是可检查的：脚本会扫描包内容，只有工具二进制里允许出现
`seed-hex` 这个字段名，其它位置出现即判失败。

## 4. 冒烟测试

打包脚本打完包立刻做三件事，任何一项失败即判打包失败：

1. `ldd` 所有二进制，不允许出现 `not found`；
2. `backupctl --help` / `backup-server --help` / `backup-cert-tool --help`
   必须退出码 0；
3. 客户端含 GUI 时，用 `QT_QPA_PLATFORM=offscreen` 启动 10 秒：退出码
   124（还在运行）或 0 都算通过，其它退出码一律算失败 —— 这能抓住"缺 QML
   资源"、"缺插件"这类只在运行时才暴露的问题。

## 5. 拿到包的人怎么核对

    cd dist/client
    sha256sum -c MANIFEST.sha256     # 包内文件是否被改过
    cat BUILD-INFO.txt               # 这份包到底是哪个 commit 构建的
    cat share/backup-project/VERSION

再和官方仓库里同一 commit 的 `resources/security/official-root-ed25519.pub`
对一下（只看非注释行），一致就说明"这个客户端内置的官方根"和仓库里公开的
那把是同一把。

## 6. 关于根公钥与私钥

* 仓库里、包里、文档里出现的**只有公钥**；
* 私钥（根私钥、服务器传输身份私钥）都不在 Git、不在制品、不在 ZIP、
  不在日志里；
* 根私钥只在离线机器上，权限 0600，通过 `--root-key <文件路径>` 使用；
  `backup-cert-tool` 没有任何"把私钥写在命令行上"的入口，也会显式拒绝
  `--root-key-hex` / `--seed-hex` 这类参数。
