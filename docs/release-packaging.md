# 发行打包（release packaging）

本文说明发行制品是怎么做出来的、为什么这么选基线、以及哪些性质是**可核验**的。
所有数字都来自实际构建的实测输出（发行目录 `dist/release/<版本>/`，由
`.github/workflows/release.yml` 在基线容器里产出）。

**正式发行**：`v0.1.0`（2026-10-08，GitHub Releases）。

## 1. 两个产品族

| 产品 | 制品 | 目标机器 |
|---|---|---|
| Backup Project Client | `Backup-Project-Client-<版本>-x86_64.AppImage`、`backup-project-client_<版本>_amd64.deb`、`backup-project-client-<版本>-linux-x86_64.tar.xz` | 用户桌面（linux x86_64） |
| Backup Project Server | `backup-project-server_<版本>_amd64.deb`、`backup-project-server-<版本>-linux-x86_64.tar.xz` | 服务器（linux x86_64） |

两者**完全独立**：升级客户端不会碰服务器二进制，升级服务器也不会碰桌面。

## 2. 为什么必须在旧基线容器里构建

开发机是 Ubuntu 24.04（glibc 2.39），而生产 ECS 是 Ubuntu 22.04（glibc 2.35）。
本项目真实踩过一次：拿开发机构建的服务端二进制直接拷到 ECS，启动时报
`GLIBC_2.38 not found`。所以发行制品只在**旧基线的干净容器**里构建。

| 产品 | 构建基线 | 理由 |
|---|---|---|
| server | `ubuntu:20.04`（glibc 2.31，gcc 9.4） | 服务端不依赖 Qt，20.04 足够老；基线越老能跑的发行版越多 |
| client | `debian:12`（glibc 2.36，Qt 6.4.2） | 见下 |

### 客户端为什么不是 Ubuntu 22.04

客户端 GUI 大量使用 `QtQuick.Dialogs` 的 `FolderDialog`：

    ui/modern/qml/pages/BackupPage.qml:263      FolderDialog {
    ui/modern/qml/pages/SchedulePage.qml:726    FolderDialog {
    ui/modern/qml/pages/RemotePage.qml:1529     FolderDialog { ... 共 4 处
    ui/modern/qml/components/BackupRecordCard.qml:228

而 `FolderDialog` 的官方文档写着 **Since: Qt 6.3**，Ubuntu 22.04（jammy）只有
**Qt 6.2.4**（`qt6-declarative 6.2.4+dfsg-3ubuntu1`）。也就是说 22.04 上
GUI 会直接报 `FolderDialog is not a type`。

**结论**：客户端**构建**基线取 **Debian 12（Qt 6.4.2 / glibc 2.36）** ——
Ubuntu 22.04 不能当构建机（它的 Qt 6.2.4 缺 FolderDialog）。

但**运行时**是另一回事：发行包自带 Qt 6.4.2 与它的依赖（包括 ICU 72），
所以在 Ubuntu 22.04 上跑得起来。CI 里有一个作业在干净的 ubuntu:22.04 容器里
实测：AppImage 启动正常、.deb 装得上、GUI 起得来、QML 告警 0。

（本轮早些时候这里写过“22.04 不支持”，那是把 libicu*.so.72 当成系统依赖了 ——
它其实在 AppImage 里。同一轮修掉的另一个问题是 .deb 的 Depends：dpkg-shlibdeps
会为我们**自己随包**的库 declare 发行版包，于是出现“在 22.04 上装不上、但装上了
本来能跑”的矛盾；现在随包 .so 会逐个映射回提供包并从 Depends 中剔除。）

## 3. 构建入口

    # 完整发行（两个产品族）
    bash packaging/build-release.sh --version 0.1.0

    # 只出一个产品族
    bash packaging/build-release.sh --version 0.1.0 --only server
    bash packaging/build-release.sh --version 0.1.0 --only client

内容来源是**既有 staging 脚本**（`scripts/stage-client-release.sh` /
`scripts/stage-server-release.sh`）：干净工作树门禁、BUILD-INFO、MANIFEST、
ldd 冒烟、CLI/GUI 冒烟、私钥排除全部沿用，打包层只加启动器 / 配置 / unit /
安装脚本。本轮的三个兼容扩展：`--version <semver>`、
BUILD-INFO 时间戳走 `SOURCE_DATE_EPOCH`、服务端 staging 补上管理员菜单脚本。

版本号由参数决定；没有 tag 时用 `0.0.0+g<commit>`。**打包脚本从不创建 tag、
从不发布 Release**。

## 4. 可复现性

固定项：

* `SOURCE_DATE_EPOCH` = commit 时间（默认自动取，可显式覆盖）；
* `TZ=UTC`、`LC_ALL=C`；
* tar：`--sort=name --owner=0 --group=0 --numeric-owner --mtime=@epoch`；
* deb：`dpkg-deb --root-owner-group`，全部文件 mtime 固定为 epoch，md5sums 排序生成；
* AppImage：AppDir 内所有文件 mtime 固定，`VERSION`/`ARCH` 固定。

两次 clean 构建（同一个 commit、同一个容器）会比较：

1. 文件清单是否完全相同；
2. 每个 MANIFEST.sha256 是否完全相同；
3. 制品本身的 sha256 是否逐字节相同（deb / tar.xz 期望 bit-for-bit；
   AppImage 里 squashfs 会写入自己的时间戳，若不同则解包后比较内容，
   并在 RELEASE-INFO/证据里如实写明差异原因）。

## 5. 第三方工具固定

`packaging/tools.lock` 记录 linuxdeploy / linuxdeploy-plugin-qt / appimagetool 的
版本、文件名、字节数、sha256、许可证、官方 URL；`packaging/fetch-tools.sh`
下载后逐个核对，对不上立即失败（没有"警告后继续"）。这三个工具只在**构建期**
使用，不随制品分发。Qt 以动态库形式随客户端分发（LGPL-3.0，见包内
`THIRD-PARTY-NOTICES.txt`）。

## 6. 私钥边界（贯穿整条流水线）

* 服务端制品里**没有**任何私钥：传输身份私钥由 `backup-server-keygen` 在目标机
  本机生成（0600），根私钥永远在离线机器上；
* 打包完成后把每个制品解开，按**结构规则**（PEM 私钥块、`seed-hex:` 行、
  `BACKUP_TOKEN_SECRET=` 行、32 字节裸密钥文件）扫描一遍，命中即失败；
* 同时检查制品里不存在 `.key` / `secrets.env` / `.bpcert` 文件。

## 7. CI

`.github/workflows/release.yml`：在容器里构建（server=ubuntu:20.04、
client=debian:12），再做干净机器安装验收（debian:12 容器 + Ubuntu 22.04/24.04
真机 runner）。第三方 Action 全部 pin 到 commit SHA。触发方式：
`workflow_dispatch`（人工，带版本号）与 `push` 到开发分支（开发期用）；
`tag v*` 的触发只是设计保留，**本轮没有创建任何 tag**。

## 8. 已知限制

* 架构只有 x86_64；没有 ARM64、没有 Windows、没有 macOS（未构建、未验证，因此
  不声称支持）；
* Ubuntu 22.04 只能作为客户端**运行**环境（已验证），不能作为**构建**基线
  （Qt 6.2.4 缺 FolderDialog，见第 2 节）；
* 上游仓库**没有 LICENSE 文件**：发行包的 `copyright` 如实记录这一点，不替权利人
  补许可证；正式对外发布前应由权利人决定许可证；
* 桌面集成是最小的：仓库没有正式图标资源，所以 `.desktop` 里不放 `Icon=`；
  AppImage 用的 PNG 是仓库自己 `AppIcon.qml` 矢量标记的导出，不是新画的品牌；
* 便携安装不注册系统级服务发现（没有 `systemd-sysusers`、没有 `debconf` 集成）。
