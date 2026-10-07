# 发布与演示

> 文档编号：04
> 状态：已完成（基线 main = 6d1e2ba，PR #27 已合并）
> 说明：本文的命令都在本轮这台 Ubuntu VM 上执行过；标"实测"的输出是真实回显，
> 路径与归档名按现场替换。所有演示操作只往 `/tmp` 或 `~/demo` 写数据，不改产品源码。

## 0. 这份文档怎么用

* 只想现场演示：先看**第 13 节**（10~15 分钟编排）与**第 14 节**（演示前检查表），
  出问题翻**第 15 节**（退路）。
* 想复现一次交付：看**第 1~4 节**（构建、制品、安装）。
* 想按主题讲：**第 5~12 节**，每节是一个独立演示，彼此不依赖。

全文约定：命令都在**仓库根目录**执行；`$B` 指产品 CLI，`$CFG` 指演示用的配置
文件。演示用的变量在每节开头给出，可以直接复制。

## 1. 发布基线：构建什么、在什么上构建、为什么

### 1.1 两件事，不要混在一起

| 用途 | 入口 | 产物 | 耗时 |
| --- | --- | --- | --- |
| 现场演示 / 验收 | `scripts/stage-client-release.sh`、`scripts/stage-server-release.sh` | `dist/client/`、`dist/server/` 两个目录包 | 秒级（已构建时） |
| 对外发行 | `packaging/build-release.sh --version <semver>` | AppImage / .deb / tar.xz + `SHA256SUMS`、`RELEASE-INFO.txt` | 分钟级（容器里构建） |

演示用的是**目录包**：内容就是 `build/` 里的二进制加几份文档，冒烟通过即可发。
发行制品是另一层（加启动器、配置、systemd unit、安装脚本，并做可复现性处理），
见 `docs/release-packaging.md`。

顺带一句：`scripts/package.sh` 目前只打印一行 `Packaging is not implemented yet.`，
它不是发布入口，别照着它敲。

### 1.2 为什么发行制品必须在旧基线容器里构建（一条真实教训）

开发机是 Ubuntu 24.04（glibc 2.39），生产 ECS 是 Ubuntu 22.04（glibc 2.35）。
本项目真实踩过一次：把开发机构建的服务端二进制直接拷到 ECS，启动时报

    GLIBC_2.38 not found

所以发行制品只在**旧基线的干净容器**里构建：

| 产品 | 构建基线 | 理由 |
| --- | --- | --- |
| server | `ubuntu:20.04`（glibc 2.31，gcc 9.4） | 服务端不依赖 Qt，20.04 足够老；基线越老能跑的发行版越多 |
| client | `debian:12`（glibc 2.36，Qt 6.4.2） | 见下 |

客户端为什么不用 Ubuntu 22.04：GUI 用了 QtQuick.Dialogs 的 `FolderDialog`
（备份页 / 自动备份页 / 远程页 / 备份记录卡片共 4 处），而官方文档写着
**Since: Qt 6.3**，Ubuntu 22.04（jammy）只有 **Qt 6.2.4** —— 在 22.04 上构建会直接报
`FolderDialog is not a type`。

但**运行**是另一回事：发行包自带 Qt 6.4.2 与它的依赖（含 ICU 72），所以在 22.04
上跑得起来。CI 里有一个作业在干净的 ubuntu:22.04 容器里实测：AppImage 启动正常、
.deb 装得上、GUI 起得来、QML 告警 0。

一句话总结：**构建看工具链的下限，运行看目标机**。演示机同时也是构建机时
（本文直接用仓库里的 `build/` 产物）这条不适用；但只要涉及"把二进制拷到别的机器"，
就必须回到旧基线构建，或者用带运行时的 AppImage。

### 1.3 打一个演示用的目录包

    cd <仓库根>
    bash scripts/stage-client-release.sh              # -> dist/client（含 GUI）
    bash scripts/stage-client-release.sh --no-gui     # 构建机没有 Qt 时
    bash scripts/stage-server-release.sh              # -> dist/server

可选参数：`--out <目录>`、`--version <semver>`、`--allow-dirty`（客户端另有 `--no-gui`）。
几条刻意的规矩：

* **工作树不干净就直接失败**。真想打一个"内容和 commit 对不上"的包，必须显式给
  `--allow-dirty`，而且 `BUILD-INFO.txt` 里会写 `working_tree = dirty` ——
  这类不一致是最难查的问题，不能靠默认行为掩盖；
* 输出目录**先清空再填**，避免上一次构建的残留混进清单；
* 打完包立刻冒烟（见 2.3 / 3.3），任何一项失败即打包失败；
* 版本号：不给 `--version` 就用 `git describe --tags --always --dirty`；给了就写进
  `share/backup-project/VERSION` 与 BUILD-INFO。**打包脚本从不创建 tag、从不发布
  Release**，发布是人的决定。

### 1.4 真要出发行制品

    bash packaging/build-release.sh --version 0.1.0                 # 两个产品族
    bash packaging/build-release.sh --version 0.1.0 --only server
    bash packaging/build-release.sh --version 0.1.0 --only client

产出 5 个制品（客户端 AppImage / .deb / tar.xz，服务端 .deb / tar.xz）加
`SHA256SUMS`、`RELEASE-INFO.txt`。可复现性的固定项（`SOURCE_DATE_EPOCH` 取 commit
时间、`TZ=UTC`/`LC_ALL=C`、tar 排序与 mtime 固定、`dpkg-deb --root-owner-group`、
AppImage 内文件 mtime 固定）与第三方工具锁（`packaging/tools.lock` +
`packaging/fetch-tools.sh` 逐个校验 sha256）见 `docs/release-packaging.md` 第 4、5 节。
linuxdeploy 等工具只在构建期使用，不随制品分发。

## 2. 客户端制品（`dist/client`）

### 2.1 内容：9 个文件

    bin/backupctl                                    客户端 CLI
    bin/backup-gui-modern                            现代 GUI（--no-gui 时没有这一项）
    share/backup-project/official-root-ed25519.pub   官方根公钥（公开材料）
    share/backup-project/VERSION                     git describe 结果
    docs/client-quick-start.md                       \ 随包文档，客观复制，不改内容
    docs/release-layout.md                            |
    docs/secure_transport.md                         /
    BUILD-INFO.txt                                   commit / 时间 / 是否含 GUI / 编译器
    MANIFEST.sha256                                  包内每个文件的 sha256（按路径排序）

### 2.2 BUILD-INFO 与 MANIFEST 长什么样

真实内容（本轮这台机器）：

    bundle            = client
    version           = b3cd6bd
    commit            = b3cd6bdf6765d259c6ff4defe41f8c50aa609361
    commit_short      = b3cd6bd
    built_at_utc      = 2026-10-07T03:07:53Z
    working_tree      = clean
    gui               = included
    compiler          = g++ (Ubuntu 14.2.0-4ubuntu2~24.04.1) 14.2.0
    built_with        = make client
    official_root     = share/backup-project/official-root-ed25519.pub

`MANIFEST.sha256` 覆盖包内每个文件（**不含自己**）：客户端 8 行、服务端 12 行，
所以文件总数是 9 与 13。打包时用的是

    find . -type f ! -name MANIFEST.sha256 -printf '%P\n' | sort | xargs sha256sum

想看这份包是不是当前基线构建的，直接比 `commit` 与 `git rev-parse HEAD`：
本轮 `dist/` 里现存的那份是 `b3cd6bd`，而 main 已经是 `6d1e2ba` ——
**演示前如果要"包与基线一致"，重新跑一次 staging 脚本**（第 1.3 节）。

### 2.3 冒烟测试（脚本自动做，失败即失败）

1. `ldd` 所有二进制，不允许出现 `not found`；
2. `backupctl --help` 退出码 0；
3. 含 GUI 时：

       QT_QPA_PLATFORM=offscreen timeout 10 bin/backup-gui-modern

   退出码 **124（10 秒到了还在跑）或 0 都算通过**，其它退出码一律失败 ——
   这能抓住"缺 QML 资源""缺平台插件"这类只在运行时才暴露的问题。

### 2.4 拿到包的人怎么核对

    cd dist/client
    sha256sum -c MANIFEST.sha256                  # 包内文件有没有被改过
    cat BUILD-INFO.txt                            # 这份包是哪个 commit 构建的
    cat share/backup-project/VERSION
    grep -v '^#' share/backup-project/official-root-ed25519.pub

最后一行再和仓库里同一 commit 的 `resources/security/official-root-ed25519.pub`
非注释行对一下：一致就说明"包内置的官方根"和仓库公开的那把是同一把。

## 3. 服务端制品（`dist/server`）

### 3.1 内容：13 个文件

    bin/backup-server                    服务端
    bin/backup-server-admin              本机管理工具（不监听端口）
    bin/backup-server-admin.sh           交互式管理菜单（管理员日常入口）
    bin/backup-server-keygen             传输身份密钥工具
    tools/backup-cert-tool               离线根与证书工具
    share/backup-project/official-root-ed25519.pub
    share/backup-project/VERSION
    docs/server-quick-start.md           \ 随包文档
    docs/self-hosted-server.md            |
    docs/release-layout.md                |
    docs/secure_transport.md             /
    BUILD-INFO.txt
    MANIFEST.sha256

服务端的 BUILD-INFO 多一行 `private_keys = none（服务端包内不含任何私钥）`。

### 3.2 包里没有任何私钥，而且这是可检查的

* 传输身份私钥由 `backup-server-keygen` 在**服务器本机**生成（0600），
  从不随包分发、也从不出现在产物里；
* 根私钥永远留在离线机器上，包里只有公开的根公钥。

打包脚本为此做两条检查：

1. 包内文件清单必须完全落在白名单里（多一个文件就是问题 —— 这才是"有没有夹带
   东西"的直接证据）；
2. 任何文件都不许含根密钥文件的行格式 `^seed-hex: <64 位十六进制>$`。

注意：**二进制里出现 `seed-hex` 这个字段名是正常的**（解析器要在里面），
所以不能拿字段名当判据 —— 第一版检查就是这么误报的，脚本注释里留了记录。

### 3.3 冒烟：`ldd` 无 not found、`backup-server --help` 与
`tools/backup-cert-tool --help` 退出 0，加上 3.2 的两条私钥检查。

## 4. 安装

安装说明在仓库的 `docs/install-client.md`、`docs/install-server.md`、
`docs/upgrade-server.md` 里；发行制品（.deb / tar.xz / AppImage）会把这些文档一起装进去，
而 **staging 目录包（`dist/client`、`dist/server`）只带 quick-start / release-layout /
secure_transport（服务端另加 self-hosted-server）**。演示机通常直接用 `build/` 里的
二进制，不必真的安装；下面两步是给"拿到包的人"准备的说辞。

### 4.1 客户端

三条路，按省事程度排：

    # 1) AppImage：不需要安装，Qt 运行时和 QML 模块都在里面
    chmod +x Backup-Project-Client-<版本>-x86_64.AppImage
    ./Backup-Project-Client-<版本>-x86_64.AppImage
    ./Backup-Project-Client-<版本>-x86_64.AppImage backupctl --help
    # 没有 FUSE 的容器里：
    APPIMAGE_EXTRACT_AND_RUN=1 ./Backup-Project-Client-<版本>-x86_64.AppImage

    # 2) .deb（系统安装，依赖由 dpkg-shlibdeps 按真实 ELF 需求算出）
    sudo apt install ./backup-project-client_<版本>_amd64.deb
    #   /usr/bin/backupctl、/usr/bin/backup-project、/usr/lib/backup-project-client/…

    # 3) portable tar.xz：不需要 root
    tar -xf backup-project-client-<版本>-linux-x86_64.tar.xz
    cd Backup-Project-Client-<版本>-linux-x86_64
    ./install.sh                     # 默认装到 ~/.local
    ./uninstall.sh --prefix ~/.local

支持范围：linux x86_64；Debian 12 与 Ubuntu 24.04 已实测，Ubuntu 22.04 可作为
**运行**环境（包自带 Qt 6.4），Windows / macOS / ARM64 没有构建也没有验证。
卸载只删程序文件，`~/.config`、`~/.local/share` 里的备份与配置一个都不动。

第一次用：打开 `backup-project` → 左侧**远程备份** → 连接方式保持**官方云端（推荐）**
（主机 / 端口 / 指纹都不需要填）→ 输入用户名与口令 → 登录。自建服务器改用
SSH 安全通道或直接连接，字段填法见 `docs/client-quick-start.md`。

出问题先看这张表（来自 `docs/install-client.md` 第 5 节）：

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| `libQt6Core.so.6: cannot open shared object file` | 单独拷了二进制 | 用同一个 .deb/AppImage 里的成套文件 |
| `GLIBC_2.36 not found` | 目标发行版比构建基线旧 | 换 Debian 12 / Ubuntu 22.04+，或自己从源码构建 |
| `QtQuick.Dialogs is not installed` | 用了系统 Qt | 用 AppImage，或确认包内 qml 目录存在 |
| 界面能开但选目录没反应 | 缺 xcb 平台插件 | 反馈时附 `QT_DEBUG_PLUGINS=1` 的输出 |

### 4.2 服务端

    sudo apt install ./backup-project-server_<版本>_amd64.deb

装出来：`/usr/lib/backup-project-server/bin/`（服务端、admin、keygen、cert-tool、
菜单脚本、启动器）、`/usr/bin/` 下的入口（`backup-server`、`backup-project-server`、
`backup-server-admin`、`backup-server-admin-menu`、`backup-server-keygen`、
`backup-cert-tool`、`backup-server-purge-data`）、`/etc/backup-project-server/`
（`server.conf`、0600 的 `secrets.env`）、`/var/lib/backup-project-server/`
（`data/`、`state/`）与 systemd unit。

首次安装自动做：建专用系统用户 `backup-project`、建目录并设属主、生成真正随机的
`BACKUP_TOKEN_SECRET`（0600）、在本机生成传输身份私钥 `state/transport.key`（0600）、
配置自检通过后启动并设为开机自启。
**绝不做**：覆盖任何已存在文件、碰防火墙（ufw/iptables/云安全组都不动）、
默认监听 `0.0.0.0`（默认只绑 `127.0.0.1`）、卸载时删数据。

日常管理走菜单，不背子命令：

    sudo backup-server-admin-menu
    sudo backup-server-admin status          # 不进菜单时看状态
    sudo backup-server-admin transport-identity   # 客户端要填的指纹与公钥

要让公网直连，四条同时满足才允许启动（缺一条启动器直接拒绝）：
`bind` 改成非回环、`certificate` 指向离线根签发的 BPCERT1 证书、
`public_bind_reason` 写一句理由、`require_bpsec2 = true`。
**防火墙要你自己开**：云安全组放行 TCP 18765 + 主机 `sudo ufw allow 18765/tcp`；
只做一处会表现为 timeout 而不是 refused。

升级：`sudo apt install ./backup-project-server_<新版本>_amd64.deb`。升级不重新生成
`transport.key`、不重写 `secrets.env`、不重建数据库；起不来时 `postinst` 会自动把
程序载荷换回升级前的版本，并把这次升级**报成失败**（元数据与载荷的一致化办法见
`docs/upgrade-server.md` 第 6 节）。
卸载不会删数据；真要删只有 `sudo backup-server-purge-data`（需要手打确认短语），
而且必须在包还没卸载时做。

## 5. 本地演示：CLI 最小闭环

前置：仓库根目录，`build/backupctl` 已构建。整段照抄即可：

    export D=~/demo
    rm -rf "$D"; mkdir -p "$D/repo" "$D/source/sub"
    export CFG="$D/config.json"
    export B=./build/backupctl
    printf 'alpha\n' > "$D/source/a.txt"
    printf 'charlie\n' > "$D/source/sub/c.txt"

    $B --config-file "$CFG" config repository set "$D/repo"
    $B --config-file "$CFG" config repository show
    $B --config-file "$CFG" backup "$D/source"
    $B --config-file "$CFG" repository list          # 抄下 Archive 那一列的名字
    $B --config-file "$CFG" restore <上一步列出的 .bak 名> "$D/restored"
    diff -r "$D/source" "$D/restored" && echo DEMO_DIFF_PASS

实测输出（本轮这台机器；回显里的 `/tmp/dsh-demo-probe` 是实测目录，现场会换成 `~/demo`）：

    $ ... config repository set ~/demo/repo
    Repository set to <仓库的绝对路径>.
    $ ... backup ~/demo/source
    Backup completed successfully.
    Repository: /tmp/dsh-demo-probe/repo
    Archive:    src_20261007_182134.bak
    Strategy:   full
    Pipeline: pack=mypack compression=none encryption=none
    $ ... repository list
    Repository: /tmp/dsh-demo-probe/repo
    Archives:   1
      src_20261007_182134.bak  480 B  2026-10-07 18:21:34  container-v2  mypack/none/none  origin=manual
    $ ... restore src_20261007_182134.bak ~/demo/restored
    Restore completed successfully.

要讲的几件事：

* 归档名是**程序在仓库里生成**的（`<源目录名>_YYYYMMDD_HHMMSS.bak`，同一秒冲突时
  追加 `_001`、`_002`）。CLI 没有"把备份写到哪就是哪"的入口，那是测试夹具
  `build/archive-cli` 的能力，不是产品功能；
* 产物恒为 v2 容器（`BKPCNT2\0`）：`repository list` 的 `container-v2` 与
  `mypack/none/none` 就是从 160 字节外层 header 里读出来的；
* 恢复目标是"不存在或空目录"：已存在且非空会被拒绝，拒绝时一个字节都不动；
* 每条命令的耗时是 10~20 ms（实测 `real 0m0.006s`~`0m0.016s`），现场重跑成本为零；
* 退出码：`0` 成功 / `1` 操作失败 / `2` 用法错误 / `3` 已经有另一个实例在跑；
* 如果演示用 `--config-file` 指到别处，**GUI 也要指到同一个文件**，否则两边看到的
  不是一个仓库（这是现场最容易被误判成"GUI 里没有备份"的原因）。

## 6. GUI 演示：七个页面

配图见 `images/gui/`（七页各一张浅色主题截图）：
`01-home.png` / `02-backup.png` / `03-backup-advanced.png` /
`04-backup-management.png` / `05-schedule.png` / `06-realtime.png` /
`07-remote.png` / `11-settings.png`。

前置：Qt 6 QML 依赖（缺的话 `scripts/modern_gui_check.sh` 开头会打印可以照抄的
apt 清单）。有显示会话直接启动；纯 SSH / 无显示器用 offscreen 自检：

    ./build/backup-gui-modern                      # 正常启动（GNOME Wayland 实测可用）
    QT_QPA_PLATFORM=offscreen QSG_RHI_BACKEND=software \
      ./build/backup-gui-modern --smoke-test --config-file "$CFG"

`--smoke-test` 会真的建出窗口、遍历七个页面、换一次主题，然后把 **QML 运行期告警
计进退出码**：告警 0 条才退出 0。屏幕上看不到界面，但"七页都建得出来、没有告警"
这件事是真的验证过的。

七个页面（顺序就是 `Main.qml` 的 StackLayout 顺序，也是 `--screenshot` 的编号 0..6）：

| # | 页面 | 现场演示什么 |
| --- | --- | --- |
| 0 | 首页 | 三张入口卡片（备份 / 备份管理 / 自动备份）+ 当前状态；没有编出来的统计数字 |
| 1 | 备份 | 源目录 + 已配置的仓库（**界面上没有"归档路径"输入框**）；高级选项：备份策略（完整 / 增量）、打包格式、压缩方式、加密方式与密码 / 确认密码；内嵌筛选规则编辑器 + 文件预览（逐条标注"进入归档 / 被规则排除 / 目录被剪枝"） |
| 2 | 自动备份 | 启用开关、源目录、频率（每 N 分钟 / 小时 / 天 / 周）、备份方式、保留版本、打包 / 压缩、规则；运行状态、运行历史、计划快照 |
| 3 | 备份管理 | 仓库里的备份列表（完整 / 增量、父快照、大小、时间、是否加密），从列表发起恢复（加密记录弹密码框）与删除；链断掉的增量会直接标注"现在恢复不了" |
| 4 | 设置 | 备份仓库路径；备份页与管理页的"更改仓库"都会跳到这里 |
| 5 | 实时备份 | 启用状态、备份目录、备份方式、保留版本；高级：响应延迟、最长等待、打包、压缩、规则；运行状态、技术细节、最近备份 |
| 6 | 远程备份 | 连接方式（官方云端（推荐）/ SSH 安全通道（兼容）/ 直接连接（高级））、账户（注册 / 登录）、远端备份（源目录 + 策略）、云端备份列表（原始归档 / 完整 / 增量，恢复 / 下载 / 删除）、上传原始归档（高级）、退出登录 / 注销账户 |

讲稿要点：

* QML 里**没有业务逻辑**：能不能续链、父是谁、代数、规则合不合法全部回到共享核心，
  界面只收集输入。界面底部显示的"CLI 等价参数"就是证据；
* 忙碌时冲突操作逐项禁用（`enabled: !controller.busy`），长操作会写清"正在做什么"；
* 本地备份 / 恢复**没有进度百分比**（核心没有进度回调，宁可少显示也不显示编出来的
  数字），远程上传 / 下载显示的是真实字节数；
* `--screenshot <目录>` 可以现场出图投屏：七页 × 两套主题共 14 张
  `<页>-<light|dark>.png`，另加备份页高级选项展开与远程页三种身份模式；
* 需要人工确认的只有指针手势（拖拽移动 / 边缘缩放）；能脚本化的部分都在
  `scripts/modern_gui_check.sh` 里断言（本轮 692 项通过、0 失败）。

无显示环境下还能单独跑的自检入口（都在 `ui/modern/main.cpp` 文件头有清单）：
`--repository-test <源> <仓库> <恢复目录>`（配置→命名备份→列表→恢复→删除全链路）、
`--preview-test <源> [--include/--exclude]`（打印与 CLI 逐行可比的预览）、
`--incremental-test <源> <仓库>`、`--schedule-test`、`--realtime-test`、
`--backup-options-test`、`--remote-test`（真的起一个 backup-server）、
`--path-test`、`--close-guard-test`。

## 7. Filter 演示：CLI 与 GUI 是同一组规则

规则共 11 个字段：`name` / `path` / `stem` / `ext` / `type` / `size` / `mtime` /
`uid` / `gid` / `user` / `group`；一条规则内子句是 AND，多条规则之间是 OR，
`exclude` 优先（完整语义见 `docs/filter_usage.md`）。

    export SRC="$D/source"
    mkdir -p "$SRC/build"
    printf 'alpha\n' > "$SRC/a.txt"; printf 'bravo\n' > "$SRC/b.log"
    printf 'obj\n' > "$SRC/build/out.o"
    RULES=(--include 'ext:txt' --exclude 'path:**/build/**')

    $B --config-file "$CFG" preview "$SRC" "${RULES[@]}"
    $B --config-file "$CFG" backup  "$SRC" "${RULES[@]}"
    $B --config-file "$CFG" repository list
    $B --config-file "$CFG" restore <新的 .bak 名> "$D/filtered-restored"

实测 preview 输出：

    Preview: 3 matching item(s) in the effective backup selection.
    a.txt
    sub
    sub/c.txt

三个必须讲清楚的点：

1. **preview 与 backup 共用同一份遍历和同一个 Filter**：预览列出的条目（含作为
   结构保留的目录，如上面的 `sub`）就是归档会装的东西。预览是只读的 ——
   不创建归档、不需要仓库、不改 config / schedule / history。预览最多**列出**
   300 条；超出时会分开写"完整遍历的匹配总数 / 窗口大小 / 窗口里列出的条数"
   三个数字，不会把窗口大小说成匹配数。按规则必然失败的情况（例如有个没被排除的
   socket）预览直接报 `Backup would fail unless this entry is excluded.` 并以 1 退出。

2. **有筛选时 `diff -r 源 恢复目录` 一定不干净**，被排除的文件本来就不该进归档。
   正确的验收是"预览 == 恢复出来的节点集"：

       diff -u <($B --config-file "$CFG" preview "$SRC" "${RULES[@]}" | grep -v '^Preview: ' | grep -v '^Note: ' | sort) \
               <(cd "$D/filtered-restored" && find . -mindepth 1 -printf '%P\n' | sort) \
         && echo FILTER_PARITY_PASS

   本轮实测该命令打印 `FILTER_PARITY_PASS`。

3. **GUI 用的是同一套**：备份页的规则编辑器只是把表单翻译成同一条 DSL，最终裁决
   仍然是 `Filter::AddRule`。逐行对比 CLI 预览与 GUI 预览：

       diff <($B --config-file "$CFG" preview "$SRC" "${RULES[@]}" | grep -v '^Preview: ' | grep -v '^Note: ') \
            <(QT_QPA_PLATFORM=offscreen ./build/backup-gui-modern --preview-test "$SRC" "${RULES[@]}" \
                --config-file "$CFG" | grep -v '^Preview: ' | grep -v '^Note: ') \
         && echo GUI_CLI_PREVIEW_IDENTICAL

   更强的证据在 `scripts/filter_rule_builder_int_test.sh`：同一场景用"界面点出来的
   规则"和"手写 CLI 规则"各跑一次备份 + 恢复，再用 `diff -r` 与全树 sha256sum 比对，
   必须逐字节相同。

## 8. 元数据演示

    export M="$D/meta"; rm -rf "$M" "$D/out-meta"; mkdir -p "$M"
    printf 'x\n' > "$M/setuid.txt"; chmod 4755 "$M/setuid.txt"
    printf 'y\n' > "$M/nano.txt";   touch -d '2024-01-02 03:04:05.123456789' "$M/nano.txt"
    printf 'z\n' > "$M/hard-a.txt"; ln "$M/hard-a.txt" "$M/hard-b.txt"
    ln -s hard-a.txt "$M/link.txt"
    mkfifo "$M/pipe.fifo"
    $B --config-file "$CFG" backup "$M"
    $B --config-file "$CFG" restore <新的 .bak 名> "$D/out-meta"

逐条核对（实测）：

    mode    src=[4755] dst=[4755]
    mtime   src=[1704135845.123456789] dst=[1704135845.123456789]
    symlink dst=[hard-a.txt] type=[符号链接]
    fifo    dst=[先进先出文件]
    inode   src=[2129957/2129957] dst=[2129965/2129965] nlink_dst=[2]

| 事实 | 归档里怎么存 | 恢复怎么做 | 需要 root 吗 |
| --- | --- | --- | --- |
| mode `07777`（含 setuid / setgid / sticky） | 条目的 mode 字段（`st_mode & 07777`） | **先 lchown 再 chmod**（顺序不能换：lchown 会清掉 setuid/setgid 位），软链接绝不 chmod | 不需要（自己的文件） |
| mtime 秒 + 纳秒 | `mtime_sec` + `mtime_nsec` | `utimensat`，atime 用 `UTIME_OMIT` | 不需要；**USTAR 只有秒级**，要纳秒必须 MyPack |
| 软链接 | 存 link target，不跟随 | `symlink()` + `lchown` + `utimensat(NOFOLLOW)` | 不需要 |
| FIFO | 类型 5，无 payload | `mkfifo()` | 不需要 |
| hardlink | 同一 `(st_dev, st_ino)` 第二次出现写成指向第一条的 hardlink 条目，不重复存 payload | `link()`；目标还没出现就先挂起，主循环结束后按拓扑补 | 不需要 |
| uid / gid | 逐条精确保存 | root 能精确还原；非 root **尽力而为 + 如实记录**（`RestoreReport.skipped_ownership` + notes，CLI 打印 `Notes: N (ownership or metadata steps were skipped)`），不会因此让整次恢复失败 | **跨属主需要 root** |
| 字符 / 块设备 | `dev_major` / `dev_minor` | `mknod()`，失败会明确写 `requires CAP_MKNOD`，**不静默降级**成普通文件 | **需要 root（CAP_MKNOD）** |
| socket | 归档格式里没有这种类型 | 备份直接失败（除非被规则明确排除） | — |

树对比用下面这个函数，**不要用 `diff -r`**：GNU diff 遇到 FIFO 会报
"是先进先出文件"，把正常的元数据树判成不一致。

    tree_snapshot() { ( cd "$1" && find . -mindepth 1 -printf '%y %m %s %P\n' | LC_ALL=C sort ); }
    diff <(tree_snapshot "$M") <(tree_snapshot "$D/out-meta") && echo META_TREE_PASS

硬链接单独证：`stat -c '%i %h' "$D/out-meta/hard-a.txt" "$D/out-meta/hard-b.txt"`
（如上：两个目标 inode 相同、nlink = 2）。

**root 边界的准确说法**：本节的 setuid / 纳秒 mtime / 软链接 / FIFO / 硬链接
**都不需要 root**（归档和恢复跑在同一个用户下）；只有**设备节点**（`mknod` 要
CAP_MKNOD）与**把文件还原成别的属主**需要 root。演示机没有 root 时，把设备节点
从现场脚本里去掉，直接讲源码里 `src/core/archive_pipeline.cpp` 的 `mknod` 分支
与格式文档第 7 节，一样说得清楚。

## 9. 压缩与加密演示

### 9.1 三种压缩：用可压缩的数据，差异一眼可见

    export K="$D/kb"; rm -rf "$K"; mkdir -p "$K"
    python3 -c "open('$K/text.txt','w').write('the quick brown fox jumps over the lazy dog\n'*20000)"
    $B --config-file "$CFG" backup "$K" --compression none
    $B --config-file "$CFG" backup "$K" --compression huffman
    $B --config-file "$CFG" backup "$K" --compression lzss-huffman
    $B --config-file "$CFG" repository list

实测（880 KiB 文本，三份在同一秒生成，所以自动带上 `_001` / `_002`）：

    cmp_20261007_182208_002.bak    3.1 KiB  ...  mypack/lzss-huffman/none  origin=manual
    cmp_20261007_182208.bak      859.7 KiB  ...  mypack/none/none          origin=manual
    cmp_20261007_182208_001.bak  488.9 KiB  ...  mypack/huffman/none       origin=manual

三份都 `restore` 回同一条内容（本轮实测三份的 `diff -r` 全部通过）。讲解点：

* 压缩是无损的，**不改变条目集**；恢复端不需要用户告诉它用了哪种压缩 ——
  外层容器 header 里写着；
* 顺序是硬性的 `PACK → COMPRESS → ENCRYPT`，且压缩 / 加密都是流式实现，
  峰值内存与归档大小无关；
* Huffman 与 LZSS+Huffman 都是本仓库手写实现，"同一输入 byte-for-byte 相同"
  由 `scripts/compression_test.sh` 与 `docs/format/archive_v2_container.md` 第 5 节固定；
* 打包格式单独有 `--pack mypack|ustar|fast-ustar`：USTAR 与 Fast USTAR 的
  wire format 相同、同一棵树产出逐字节相同（只差 I/O 策略）；USTAR 装不下的
  超长路径 / 超大 uid-gid 会**明确失败，绝不截断**。

### 9.2 两种加密：密码必须在真终端里手打

    $B --config-file "$CFG" backup "$K" --encryption aes-256-ctr-hmac-sha256
    #   Backup password: ****
    #   Confirm password: ****
    $B --config-file "$CFG" repository list
    #   这一行末尾会多出 password-required
    $B --config-file "$CFG" restore <加密的那份 .bak 名> "$D/out-enc"
    #   Restore password: ****

要点：

* 口令**只从 `/dev/tty` 读**，关闭回显，备份问两次、恢复问一次；命令行里没有
  `--password`，也不读环境变量、管道或重定向文件。非交互环境下会**明确失败**
  （本轮的实测是 `Cannot read a password: ...` / `the input ended before a password
  was entered`）——所以**这一节不要用脚本喂密码，现场手打**，这本身就是产品语义；
* 恢复是**先认证后解密**：密码错一定失败在 HMAC 那一关，报
  `Authentication failed: wrong password or tampered archive`，而且此时**一个字节都
  还没写**（目标目录不存在）；不是靠 PKCS#7 padding 校验失败才发现；
* 参数：`PBKDF2-HMAC-SHA256(password, salt, 200000, 64)`，AES-256-CTR + HMAC-SHA256，
  Encrypt-then-MAC；salt 与 IV 来自 OS CSPRNG（`getrandom()`，失败退 `/dev/urandom`），
  密码不写 config、不写日志、不回显、不以明文进归档；
* `des-cbc-hmac-sha256` 是 **legacy / educational**（56 bit 密钥）：保留它是为了
  讲清分组密码、CBC 链与 PKCS#7，DES-CBC 一定比 AES 版本多 1..8 字节的 padding；
* **增量不支持加密**：带 `--encryption` 的增量会在写盘前被拒绝（外层信封不受内层
  保护）；计划任务与实时任务的 `--encryption` 只接受 `none`，理由会明确打印出来；
* GUI 里不用终端也能演示加密：备份页 → **高级选项** → 加密方式 + 密码 / 确认密码 →
  开始备份；到备份管理页点这条记录的"恢复"，会弹"此备份已加密，请输入恢复密码"。
  自动证据在 `scripts/modern_gui_check.sh` 的 `--backup-options-test`（四种算法组合、
  密码校验、加密与 legacy 恢复、密码不落盘）与
  `tests/unit/archive_pipeline_test.cpp` 的 wrong-password 矩阵（同时断言"失败且不留
  半成品"）。

## 10. 自动备份与实时备份：都要求进程常驻

### 10.1 自动备份（定时触发 + 变化检测）

    export S="$D/sched"; mkdir -p "$S/source"; printf 'v1\n' > "$S/source/x.txt"
    export SF="$S/schedule.json"
    # $C 就是"命令前缀"（路径不含空格，所以可以这样拼）
    C="$B --config-file $CFG --schedule-file $SF"

    $C schedule set --source "$S/source" --interval-minutes 1 --retain 3 --strategy full
    $C schedule show
    $C schedule enable                # 必须有仓库 + 真实源目录才允许启用
    $C schedule run                   # 立即评估一次（有变化就建快照）
    $C schedule run                   # 再跑一次：没有变化 -> Skipped: the source has not changed...
    printf 'v2\n' >> "$S/source/x.txt"
    $C schedule run                   # Created a new full snapshot
    $C schedule history

输出形状（来自源码，现场对照）：

    Scheduled backup enabled.
    It runs only while this program or 'backupctl schedule watch' is running.
    Scheduled evaluation: Created a new full snapshot
      changes:   +0 added, -0 removed, ~1 modified, 0 metadata
      archive:   source_20261007_183012.bak
      next run:  2026-10-07 18:31:12 (at 1791370272)

现场要讲清的四条：

* **计划只在 runner 运行时生效**：runner 要么是 GUI 的自动备份页，要么是
  `backupctl schedule watch`（前台运行到 Ctrl+C，不 daemonize、不注册 systemd）。
  关掉程序之后到点也不会发生任何事，界面上与 `schedule enable` 的输出都会写明这一点；
* 同一个计划同时只允许一个 runner：GUI 与 CLI 抢同一把 flock，第二个明确报
  `The scheduled backup is already held by another process`，不会各备份一遍；
* `schedule run` **不是"强制备份"**：它跳过"还没到点"，但仍然做真实变化检测 ——
  没变化就是 `Skipped: the source has not changed since the last snapshot`（退出码 0，
  但什么都没创建）；另外它要求计划已启用，未启用会明确拒绝并提示先 `schedule enable`；
* 变化检测是 metadata-first（路径 / 类型 / 大小 / mtime / mode / uid-gid / 链接目标 /
  设备号 / 硬链接关系），**不读文件内容**。所以"同大小 + 同 mtime 的人为原地改写"
  在这个入口下逃得过检测 —— 这是已知盲区，写在 `docs/scheduled_backup_usage.md` 第 8 节。
  想演示"改写被识别"要用增量（第 12 节），增量走的是内容摘要。

计划也支持 `--strategy incremental`（本轮 INC-09..INC-11 覆盖）：输出里
`diagnostic:` 会写 `Incremental delta on top of '<父快照>'`，保留策略不会删掉链上
必需的祖先。演示时间紧就用默认的 `full`，语义更好讲。

### 10.2 实时备份（inotify）

终端 A：

    export T="$D/rt"; mkdir -p "$T/source"
    export RF="$T/realtime.json"
    R="$B --config-file $CFG --realtime-file $RF"       # 同上的命令前缀写法
    $R realtime set --source "$T/source" --debounce-ms 800 --max-wait-ms 3000 \
       --retain 5 --strategy full --pack mypack --compression none --encryption none
    $R realtime show
    $R realtime enable
    $R realtime watch            # 前台常驻，直到 Ctrl+C

终端 B（**不要再起 backupctl**，用 shell 重定向写文件）：

    printf 'hello\n' > "$T/source/new.txt"; sleep 2; printf 'hello2\n' >> "$T/source/new.txt"

终端 A 会依次打印：

    [realtime] settled generation #1 (2 event(s))
    [realtime] outcome=full snapshot=source_20261007_183501.bak
    [realtime] <共享核心给出的那句摘要>
    ^C[realtime] stopped

退出码 0（Ctrl+C 停止不算错误）。停掉之后再用 CLI 看结果：

    $R realtime history
    $B --config-file "$CFG" repository list

**现场最容易踩的坑**：整个产品同一时刻只允许一个进程。Modern GUI、`backupctl`
与历史遗留的 Qt Widgets GUI 共用同一把按 Unix UID 定位的应用锁
（`/run/user/<uid>/backup-project.lock`；没有可用的 `/run/user/<uid>` 时退到
`/tmp/backup-project-<uid>.lock`）。`realtime watch` 还开着的时候，任何另一条
backupctl 命令（**包括只读的 `repository list`**）都会以 **3** 退出：

    Error: Another backup-project instance is already running. This program allows only
    one GUI or CLI process at a time (application lock: /run/user/1000/backup-project.lock).
    Close the other instance and try again.

所以：watch 期间改文件要用 `printf >` 或编辑器，不要用 backupctl；要执行 CLI 命令就
先 `Ctrl+C` 停掉 watch。`schedule watch` 与 GUI 开着时同理（这条在检查表里也列了）。

实时备份还有两个可讲的点：源目录**不能与仓库重叠**（`realtime enable` 时会拒绝）；
watch 重启后会先合成一次 `[realtime] resync trigger: capturing the current source tree`
（事件历史在停机期间丢了，先补一次，而不是假装什么都没发生）。

## 11. 网络演示：完全在环回上做

三个入口都不需要公网、不需要 ECS、不需要 SSH，跑完自己清理临时目录；每个断言一行
`PASS/FAIL`，结尾打印统计，失败返回非 0。**首次运行会先 make 需要的产物**，
所以留出编译时间；下面括号里的耗时是本轮 `final_gate` 日志里的实测值（已构建前提下）。

    bash scripts/bpsec2_loopback_e2e.sh      # 4 秒：真 backup-server（证书模式 + --require-bpsec2）+ 真 backupctl
    bash scripts/remote_incremental_test.sh  # 31 秒：full -> delta -> delta -> 链恢复 -> 冷缓存 -> 依赖删除 -> 篡改拒恢复
    bash scripts/network_test.sh             # 265 秒：协议层单元 + 真 TCP 环回 + 绑定/权限/负路径，太长，建议不现场跑

`bpsec2_loopback_e2e.sh` 覆盖：离线测试根 → 服务器传输身份密钥 → 签发证书，
正向的 ping / register / login / list，负向的 server_id 不符 / 不受信任的根 /
内置官方根 / BPSEC1 降级全部失败，最后一条断言是"全程没有新增 ssh 进程（delta = 0）"。

想现场讲协议就手工起一个最小环回（约 3 分钟）：

    export N="$D/net"; rm -rf "$N"; mkdir -p "$N/data" "$N/state"
    echo "BACKUP_TOKEN_SECRET=$(head -c 32 /dev/urandom | sha256sum | cut -c1-64)" > "$N/secrets.env"
    chmod 600 "$N/secrets.env"                       # 权限宽了产品自己会拒绝
    ./build/backup-server-keygen --output "$N/transport.key"
    ./build/backup-server-keygen --show --key-file "$N/transport.key"     # 抄下 --server-key sha256:...
    ./build/backup-server --bind 127.0.0.1 --port 18765 --root "$N/data" \
      --db "$N/state/metadata.sqlite3" --secret-file "$N/secrets.env" \
      --transport-key-file "$N/transport.key" --pid-file "$N/state/server.pid" \
      --log-file "$N/state/server.log" --quiet &
    sleep 1; ss -ltn | grep 18765                    # 只应该看到 127.0.0.1:18765

客户端（另开一个终端，或者把服务端放后台）：

    $B remote ping --host 127.0.0.1 --port 18765                  # 没有 pin：exit 2 + 用法说明
    export BACKUP_REMOTE_SERVER_KEY=sha256:<上一步的指纹>
    $B remote ping --host 127.0.0.1 --port 18765                  # PING 正常: backup-server 协议版本 1 服务端时间 ...
    $B remote register --user demo --host 127.0.0.1 --port 18765  # 口令手打两次
    $B remote login    --user demo --host 127.0.0.1 --port 18765  # 登录成功: demo @ 127.0.0.1:18765

要讲的：没有 pin 就不许连（不做"第一次见到谁就信谁"）；pin 错了握手失败且**绝不退回
明文**；服务端默认只绑 `127.0.0.1`，`--bind` 给别的地址会拒绝启动 —— 要开公网必须
同时给出 `--bpsec2-cert-file` + `--require-bpsec2` + `--allow-public-bind <理由>`；
BPNET1 的全部流量由 BPSEC1 加密（X25519 + HKDF-SHA256 + AES-256-CTR + HMAC-SHA256），
线上 magic 是 `BPN1`。收尾：

    kill "$(cat "$N/state/server.pid")"     # 关掉这个演示服务端（--pid-file 写的）

**官方云端只当加分项**：只有在网络确实可用时才展示 —— GUI 远程页选"官方云端（推荐）"，
地址 / 端口 / 指纹都不需要填（编译进二进制的 OfficialCloudProfile + 内置官方根 +
BPSEC2 证书），或 `backupctl remote ping --official-cloud`。演示前先确认公网可达
（云安全组与主机 `ufw` 两处都要放行，只做一处表现为 timeout），不可达就当场退回环回，
**不要把主演示押在公网 ECS 上**。

## 12. 增量链演示

### 12.1 远端链：`GEN` / `PARENT` 就在这里看

接第 11 节（环回服务端已起、`demo` 已登录）：

    mkdir -p "$D/src"; head -c 20000000 /dev/urandom > "$D/src/big.bin"; echo v1 > "$D/src/notes.txt"
    $B remote backup "$D/src" --strategy full --user demo --host 127.0.0.1 --port 18765 --name demo-R0
    echo v2 >> "$D/src/notes.txt"
    $B remote backup "$D/src" --user demo --host 127.0.0.1 --port 18765 --name demo-R1
    printf 'v3\n' >> "$D/src/notes.txt"
    $B remote backup "$D/src" --user demo --host 127.0.0.1 --port 18765 --name demo-R2
    $B remote list   --user demo --host 127.0.0.1 --port 18765

期望：`remote backup` 打印"已上传增量快照 / 快照 ID / 类型: incremental / 代数 / 父快照 /
本次上传: N 字节 / 链根大小: …（本次只传了增量部分）"；`remote list` 的表头是
`SNAPSHOT_ID SIZE CREATED SHA256 KIND GEN PARENT NAME`，三行分别是
`full GEN=0 PARENT=-`、`incremental GEN=1`、`incremental GEN=2`，后两行的 PARENT 是
上一行的快照 ID 前 12 位。第二次的"本次上传"只有 KB 级 —— 20 MB 的源目录里只改了
几个字节。（本项目的现场记录见 `docs/demo/pr21_network_score_demo.md`。）

依赖感知删除与链恢复：

    $B remote delete <R0 的快照 ID> --user demo --host 127.0.0.1 --port 18765
    #   Error: snapshot <id> still has N dependent incremental snapshot(s); delete the descendants first
    $B remote delete <R2 的快照 ID> --user demo --host 127.0.0.1 --port 18765   # 叶子，允许
    rm -rf ~/.config/backup-project/backup-gui-modern/remote-cache             # 冷缓存（可选）
    $B remote restore <R1 的快照 ID> "$D/remote-restored" --user demo --host 127.0.0.1 --port 18765
    diff -r "$D/src" "$D/remote-restored" && echo REMOTE_DIFF_PASS

`remote restore` 会打印"已从远端恢复 <归档名> / 目标目录 / 依赖链: N 份快照（M 个增量）/
本次下载: X 字节 / 恢复条目: N"：用户只选**恢复点**，整条链自动下载、逐成员校验、
原子发布，不需要手工挑父或逐个下载 delta。
（远端缓存固定在 `<应用配置目录>/remote-cache/<服务端指纹前 16 位>/<用户名>/`，
与 `--config-file` 无关；删掉它是安全的，只是下次要重新下载。）

服务端只做三件事：存字节、存链关系、enforce 归属与依赖。它不扫源目录、不算 diff ——
`server/` 里没有备份引擎代码，父必须存在、同用户、同 lineage，代数由服务端按父推导，
所以"代数跳跃"在协议上就不存在。

收尾（不可撤销，会删掉账户与它的全部云端备份）：

    $B remote delete-account --user demo --confirm demo --host 127.0.0.1 --port 18765

已知边界：远端增量只支持 MyPack + 不压缩 / 不加密；链深上限 64，到顶后引擎自动重建
完整基线；远端没有"整条链一次删"的命令，只能按后代优先逐个删。

### 12.2 本地链：不想起服务端时用这条

    $B --config-file "$CFG" backup "$D/source" --strategy incremental   # 1) Kind: full baseline (no trustworthy baseline: ...)
    $B --config-file "$CFG" backup "$D/source" --strategy incremental   # 2) No effective changes ...; nothing was written
    # 3) 原地改写，并且把 mtime 恢复原值：大小与 mtime 都不变，metadata-first 看不见它
    python3 - "$D/source/a.txt" <<'PY'
    import os, sys
    path = sys.argv[1]
    info = os.lstat(path)
    with open(path, 'r+b') as handle:
        handle.write(b'ALPHA')          # 'alpha\n' -> 'ALPHA\n'，长度不变
    os.utime(path, (info.st_atime, info.st_mtime), follow_symlinks=False)
    PY
    $B --config-file "$CFG" backup "$D/source" --strategy incremental   #    Kind: delta / Parent: <基线名> / Changes: added=0 modified=1 ...
    printf 'charlie' > "$D/source/c.txt"
    $B --config-file "$CFG" backup "$D/source" --strategy incremental   #    Kind: delta / Parent: <上一份 delta>
    $B --config-file "$CFG" repository list
    $B --config-file "$CFG" repository delete <基线名>
    #   Error: Cannot delete '<基线名>': the snapshot '<子快照名>' is built on top of it and would
    #          become unrestorable. Delete the dependent snapshot(s) first.
    $B --config-file "$CFG" repository delete <中间那份 delta>          # 同样被拒
    $B --config-file "$CFG" restore <最后一份 delta> "$D/out-d"        # 自动把整条链应用出来
    diff -r "$D/source" "$D/out-d" && echo CHAIN_DIFF_PASS

实测的三步输出：

    Kind:       full baseline (no trustworthy baseline: no snapshot with a usable content manifest was found)
    Kind:       delta
    Parent:     inc_20261007_182209.bak
    Changes:    added=0 modified=1 metadata=0 removed=0

两个"看列表"的细节，先说清楚免得现场被问住：

* 本地 `repository list` **不打印 GEN / PARENT**。链关系由 backup 命令打印的
  `Kind:` / `Parent:` 给出（远端看 `remote list`）。要展示"代数"就用远端链（12.1）；
* delta 那一行在本地列表里的 kind 列显示 `legacy-v0.1` —— BKPINC1 delta 的
  `format_version` 是 1，而这个列表只区分 `container-v2` 与 `legacy-v0.1`，它**不是**
  "旧格式备份"的意思。GUI 的备份管理页会正确显示"增量 + 父快照"，并在这条链断掉时
  直接标注"现在恢复不了"。

自动证据：`scripts/test.sh` 的 INC-01..INC-11（CLI 三步种类、GUI/CLI 种类序列一致、
same-size + same-mtime 改写被识别为一次修改、从 delta 依赖链恢复、retain 不删链上祖先）、
`scripts/remote_incremental_test.sh` 的单元段与端到端段。

## 13. 推荐现场演示顺序（10~15 分钟）

开演前按第 14 节过一遍检查表，并把第 5 节的 `$D` 与仓库准备好。

| 步 | 内容 | 命令要点 | 预计 | 前置 |
| --- | --- | --- | --- | --- |
| 0 | 交代基线 | `git rev-parse HEAD`；`cat dist/client/BUILD-INFO.txt` | 0:40 | — |
| 1 | CLI 最小闭环 | 第 5 节：config set → backup → list → restore → `DEMO_DIFF_PASS` | 2:00 | 第 5 节的 `$D`、`$CFG` |
| 2 | Filter 一致性 | 第 7 节：preview → backup → `FILTER_PARITY_PASS` → GUI 预览逐行相同 | 1:30 | 步 1 |
| 3 | 元数据 | 第 8 节：4755 / 纳秒 mtime / 软链接 / FIFO / hardlink 同 inode + `META_TREE_PASS` | 1:30 | 步 1；设备节点要 root，可跳 |
| 4 | 压缩 | 第 9.1：三份大小对比 + 任选一份恢复 | 1:00 | 步 1 |
| 5 | 增量链 | 第 12.2（本地，快）或第 12.1（环回服务端，更有说服力） | 2:00 | 步 1；12.1 需先起第 11 节的服务端 |
| 6 | GUI 七页 | 第 6 节：打开 GUI 走七页，重点备份 / 备份管理 / 设置；无显示就跑 `--smoke-test` | 3:00 | Qt 依赖；与 CLI 同一个 config |
| 7 | 自动 / 实时 | 第 10 节二选一：`schedule run` 两次（秒级）或 `realtime watch` + 另一终端写文件 | 1:30 | 两个终端 |
| 8 | 网络环回 | `bash scripts/bpsec2_loopback_e2e.sh`（4 秒），时间够再加 `remote_incremental_test.sh`（31 秒） | 2:00 | 已构建 |
| 9 | 加密（可选） | 第 9.2：手打密码备份 → 错误密码失败 → 正确密码恢复 | 1:30 | 真终端 |

裁剪建议：时间不够就砍 9 → 4 → 3；**步 1、2、6 是核心，不要砍**。
表中的耗时是"命令本身 + 讲稿留白"的估计，命令本身都在秒级以内；网络套件的秒数来自
本轮 `final_gate` 实测日志（`bpsec2-loopback` 4 s、`remote-incremental` 31 s、
`network` 265 s）。

## 14. 演示前检查表

逐条打勾，任何一条不过都先修好再开演：

* [ ] **知道自己在哪台机器、哪个 commit**：`git rev-parse HEAD`（本轮 `6d1e2ba`）、
      `git status --short`（演示用的目录包与当前 commit 是否一致，见 2.2）。
* [ ] **二进制都在**：
      `ls build/backupctl build/backup-gui-modern build/backup-server build/backup-server-keygen build/backup-cert-tool`。
      缺了就 `make all gui-all server cert-tool`（留 2~3 分钟）。
      `build/archive-cli` 是测试夹具，演示不需要。
* [ ] **没有第二个实例**（最容易翻车的一条）：
      `fuser -v /run/user/$(id -u)/backup-project.lock` 应该什么都不打印；
      `pgrep -a -x backupctl`、`pgrep -a -f backup-gui-modern`、`pgrep -a -f backup-server` 应为空。
      有残留就关掉 —— 否则现场的每条命令都会以 **3** 退出。
* [ ] **演示期间没人在跑测试**：`pgrep -a -f "scripts/.*[.]sh"`。
      `final_gate.sh` / `test.sh` / `modern_gui_check.sh` 会长时间占用应用锁，
      把现场命令挤成 exit 3（本轮就真实遇到过）。
* [ ] **仓库已设置**：`./build/backupctl config repository show`，`Repository` 不是 `(not set)`；
      现场用 `--config-file` 的话，GUI 也要用同一个文件。
* [ ] **目录干净**：`~/demo` 已按第 5 节重建；恢复目标目录不存在或为空目录
      （非空会被拒绝，这是有意的）。
* [ ] **root**：本演示默认**不需要 root**；只有设备节点（CAP_MKNOD）与"还原成别的属主"
      需要。没有 root 就把这两条从脚本里划掉。
* [ ] **GUI 依赖**：`pkg-config --exists Qt6Quick Qt6Qml Qt6QuickControls2 Qt6Concurrent`；
      缺就按 `scripts/modern_gui_check.sh` 打印的 apt 清单装。没有显示会话时准备
      `QT_QPA_PLATFORM=offscreen QSG_RHI_BACKEND=software`。
* [ ] **终端数**：本地演示 1 个够；实时 / 计划要 2 个（A 跑 watch，B 写文件）；
      网络环回 1 个（服务端放后台）。
* [ ] **公网**：默认不依赖。要加官方云展示，先确认端点可达（云安全组 + 主机 ufw 两处），
      不可达就跳过 —— 主演示必须能在纯环回下走完。
* [ ] **磁盘**：都写在 `/tmp` 与 `~/demo`，元数据演示是 KB 级；12.1 会造一个 20 MB
      随机文件，按需准备。
* [ ] **屏幕**：提前开好一个页签放 `git rev-parse HEAD` 与
      `dist/client/BUILD-INFO.txt`，开场直接用。

## 15. 演示失败时的退路

| 主演示 | 可能怎么坏 | 退路（更稳的等价证据） |
| --- | --- | --- |
| CLI 最小闭环 | 忘了设仓库；恢复目标非空 | 现场 `config repository set` 到 `/tmp` 新目录；恢复目标换成新建目录。命令本身 10 ms 级，重跑成本为零 |
| 任何命令报 `exit 3` | 有 GUI / watch / 别的 CLI 在跑 | `fuser -v /run/user/$(id -u)/backup-project.lock` 找到 pid，关掉它再重跑。锁由内核在进程退出时释放，磁盘上留一个 stale 锁文件不会把产品锁死 |
| Filter 演示 | 用 `diff -r` 判等，结果一片差异，像"备份错了" | 换成第 7 节的 `FILTER_PARITY_PASS`（预览 == 恢复节点集）。有筛选时 `diff -r` 本来就不干净 |
| 元数据：设备节点 | 非 root 恢复设备节点失败（`requires CAP_MKNOD`） | 直接讲这条边界，用 setuid / 纳秒 mtime / 软链接 / FIFO / hardlink 五条非 root 证据；投屏 `src/core/archive_pipeline.cpp` 的 `mknod` 分支 |
| 元数据：`diff -r` | 树里有 FIFO，diff 报"是先进先出文件" | 换成 `tree_snapshot` 的 `%y %m %s %P` 对比，硬链接另用 `stat -c '%i %h'` |
| 压缩 | 现场数据不可压缩，三种方式大小差不多 | 换成第 9.1 的重复文本（实测 859.7 KiB / 488.9 KiB / 3.1 KiB，差异一眼可见） |
| 加密 | 终端不支持交互口令（重定向 / 管道 / CI） | 用 GUI 路径（备份页高级选项 + 管理页恢复弹密码框），或改为展示 `tests/unit/archive_pipeline_test.cpp` 的 wrong-password 矩阵与格式文档第 6 节。**不要用脚本喂密码**：产品明确拒绝非交互口令，这是设计而不是故障 |
| 自动备份 | 等不到下一个周期 | 用 `schedule run`（立即评估，仍然做变化检测）；讲"必须常驻 runner"时开 `schedule watch`，Ctrl+C 退出 |
| 实时备份 | 以为要点"开始"，等半天没反应 | watch 必须前台常驻；文件要在**另一个终端**写；watch 期间不要起第二条 backupctl（会 exit 3） |
| 网络环回 | 端口被占 / 上次的服务端没退 | 换端口（`--port 0` 由内核分配，或自选高端口）；`pgrep -a -f backup-server` 清残留；三个脚本自己都用临时端口 |
| 网络脚本太慢 | 现场只剩两分钟 | 只跑 `bpsec2_loopback_e2e.sh`（4 秒）；`network_test.sh`（265 秒）不要现场跑，讲它的分层（协议单元 / 真 TCP 环回 / 绑定与权限）即可 |
| 官方云 | 公网不可达 / 安全组没开 / 指纹对不上 | 退回第 11 节的环回：同样的服务端二进制、同样的 BPSEC1/BPSEC2、同样的 `remote` 命令，功能等价。官方云只当"网络可用"时的加分项 |
| GUI 起不来 | 没有显示会话 / 没有 3D 驱动 / 缺 QML 模块 | ① `QT_QPA_PLATFORM=offscreen QSG_RHI_BACKEND=software ./build/backup-gui-modern --smoke-test --config-file "$CFG"`（退出码 0 = 七页都建出来了、QML 告警 0）；② `--screenshot <目录>` 出 14 张 PNG 投屏；③ 退回 CLI 等价命令（第 5~7 节逐条对应）；④ 桌面集成问题用 `--native-frame` 兜底 |
| 需要"一份包"而不是源码构建 | — | 现场跑 `bash scripts/stage-client-release.sh` / `stage-server-release.sh` 出 `dist/client`、`dist/server`（秒级；要求工作树干净，脏了加 `--allow-dirty`，BUILD-INFO 会写明 dirty） |

## 16. 本轮实测数据与证据位置

* 测试（本轮 main）：`scripts/test.sh` PASS=279 FAIL=0；`scripts/final_gate.sh`
  49 行 `GATE` 记录全部 `exit=0`、`[gate] failed suites = 0`（其中 3 行是构建 /
  消毒剂构建 / GUI 重建，46 行是独立套件）；`scripts/modern_gui_check.sh` 692 项通过、
  0 失败；`release staging` 冒烟全 PASS（客户端 9 个文件、服务端 13 个文件）；
  编译警告 0、clang-format / lint 通过。
* 日志：`/tmp/final_gate.log`（逐套件 exit 与秒数）、`tests/output/modern-gui-check.log`；
  截图与几何证据在 `--screenshot` 指定的目录（评审产物，已被 `.gitignore` 覆盖）。
* 相关文档：`docs/release-layout.md`（包内容）、`docs/release-packaging.md`（发行制品与
  可复现性）、`docs/install-client.md` / `docs/install-server.md` / `docs/upgrade-server.md`
  （安装与升级）、`docs/client-quick-start.md` / `docs/server-quick-start.md`（第一次用）、
  `docs/basic_cli_usage.md`（CLI 用法与退出码）、`docs/filter_usage.md`（筛选语义）、
  `docs/format/archive_v2_container.md`（v2 容器与三层算法）、`docs/remote_incremental.md`
  （远端增量）、`docs/scheduled_backup_usage.md`（计划备份与它的已知盲区）、
  `docs/secure_transport.md`（BPSEC1/BPSEC2）、`docs/self-hosted-server.md`（自托管根）。
