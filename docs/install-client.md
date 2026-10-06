# 安装客户端（Backup Project Client）

本文只讲**拿到包的人**要做什么。客户端有两个东西：命令行 `backupctl` 和现代
图形界面 `backup-project`。它们共用同一份核心实现，装哪个都用得着。

支持范围先说清楚：

| 发行版 | 客户端 |
|---|---|
| Debian 12（bookworm） | **支持**（构建基线） |
| Ubuntu 24.04 LTS | **支持**（已实测） |
| Ubuntu 23.10 / 23.04 | 应该可以（glibc ≥ 2.36），本轮未实测 |
| Ubuntu 22.04 LTS | **支持（运行时）**：包自带 Qt 6.4，AppImage 与 .deb 都在 22.04 上实测通过 |
| Windows / macOS / ARM64 | **不支持**（没有构建、也没有验证过，不声称支持） |

架构：**linux x86_64**。

## 1. 最省事：AppImage（不需要安装）

    chmod +x Backup-Project-Client-<版本>-x86_64.AppImage
    ./Backup-Project-Client-<版本>-x86_64.AppImage

Qt 运行时和 QML 模块都在 AppImage 里，目标机器不需要装 Qt，也不需要本仓库的
源码或 build 目录。

命令行入口也在这一个文件里：

    ./Backup-Project-Client-<版本>-x86_64.AppImage backupctl --help

没有 FUSE 的环境（容器 / CI）：

    APPIMAGE_EXTRACT_AND_RUN=1 ./Backup-Project-Client-<版本>-x86_64.AppImage

想在桌面菜单里出现：装上 `appimaged`/AppImageLauncher，或把 .desktop 文件放进
`~/.local/share/applications`。AppImage 自己不在系统里留任何东西。

## 2. 系统安装：.deb

    sudo apt install ./backup-project-client_<版本>_amd64.deb

装出来的东西：

    /usr/bin/backupctl                        命令行
    /usr/bin/backup-project                   图形界面
    /usr/lib/backup-project-client/           自带的 Qt 运行时 + QML 模块
    /usr/share/applications/backup-project.desktop

依赖由 `dpkg-shlibdeps` 按真实 ELF 需求算出（不是手写的库列表）。

**卸载只删程序文件**：

    sudo apt remove backup-project-client

你的备份、配置、缓存都**不会**被动：

* `~/.config/...`、`~/.local/share/...` 里的东西全部保留；
* 包本身也不往 `$HOME` 里写任何东西。

## 3. 不需要 root：portable tar.xz

    tar -xf backup-project-client-<版本>-linux-x86_64.tar.xz
    cd Backup-Project-Client-<版本>-linux-x86_64
    ./install.sh                       # 默认装到 ~/.local
    ./install.sh --prefix /opt/backup-client --no-desktop

卸载：

    ./uninstall.sh --prefix ~/.local

同样是"只删程序文件"：用户数据一个字都不动。

## 4. 第一次用

1. 打开 `backup-project`（或桌面菜单里的"备份工具"）；
2. 左边选**远程备份**；
3. 连接方式保持 **官方云端（推荐）**：主机 / 端口 / 指纹都不需要填；
4. 输入用户名与口令，点登录。

自建服务器请用 **SSH 安全通道** 或 **直接连接**，并按
[客户端快速上手](client-quick-start.md) 填服务器地址与身份指纹。

## 4.5 关于 Ubuntu 22.04（此前写错，在这里更正）

本文早先的版本把客户端在 Ubuntu 22.04 上写成“不支持”，理由是“GUI 需要 Qt 6.3
才有的 FolderDialog”。这个结论只对“用系统 Qt 在 22.04 上构建”成立，对发行包不成立：
发行包自带 Qt 6.4.2 和它需要的依赖（含 ICU 72），所以在 22.04 上照样跑得起来。
CI 里有一个专门作业在干净的 ubuntu:22.04 容器里验证：AppImage 启动正常、
安装 .deb、GUI 起得来、QML 告警 0。

构建基线仍然是 Debian 12（22.04 的 Qt 6.2.4 缺 FolderDialog，不能当构建机），
但运行时没有这个问题。

## 5. 出问题先看这里

| 现象 | 原因 | 处理 |
|---|---|---|
| 启动即退出，提示 `libQt6Core.so.6: cannot open shared object file` | 用的是旧包或手工搬了二进制 | 用同一个 .deb/AppImage 里的成套文件，不要单独拷二进制 |
| `GLIBC_2.36 not found` | 目标发行版比构建基线旧（glibc < 2.36，例如 Ubuntu 20.04） | 换 Debian 12 / Ubuntu 22.04+；或自己从源码构建 |
| `QtQuick.Dialogs is not installed` | 用的是系统 Qt 而不是包里的 Qt | 用 AppImage，或确认 `/usr/lib/backup-project-client/qml` 存在 |
| 界面能开但选择目录没有反应 | 缺平台插件（xcb） | 反馈时附上 `QT_DEBUG_PLUGINS=1` 的输出 |

## 6. 怎么核对拿到的包

    sha256sum -c SHA256SUMS                                  # 顶层校验
    cat RELEASE-INFO.txt                                     # 版本 / commit / 构建基线
    cat usr/share/doc/backup-project-client/BUILD-INFO.txt   # deb 内
    cat share/backup-project/VERSION                         # 包内版本
    cat share/backup-project/official-root-ed25519.pub       # 内置的官方根公钥（公开材料）

仓库里同一 commit 的 `resources/security/official-root-ed25519.pub` 应该与包里的
非注释行完全一致。
