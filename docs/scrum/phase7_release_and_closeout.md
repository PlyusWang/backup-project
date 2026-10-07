# Phase 7 Review & Retrospective：发行打包、源码质量收口与课程收口

> 覆盖阶段：2026-10-06 -- 2026-10-07（收口进行中）
> 对应 PR：#24（feat/release-packaging）、#26（feat/source-quality-final-cleanup）、
> #27（fix/gui-snapshot-chain-status）
> 冻结基线：`6d1e2ba71a48862927f23442604d6bd4b430e62d`（PR #27 合并后）
> 本阶段之后产品代码冻结，本轮只写文档。

---

## 1. 阶段 Goal

功能开发在 Phase 6 结束，剩下的是把项目**交付出去**并要求它是可核验的：

1. 产出可以在干净机器上安装运行的发行制品，且构建过程可复现；
2. 把源代码质量做成**可执行的检查**（格式、注释、行宽、重复实现），而不是答辩前
   的手工统计；
3. 修掉最后一轮真实使用中发现的问题（备份管理页的快照链恢复状态）；
4. 把课程需要的文档与证据归档：需求、架构、测试、Gantt、Scrum 记录、安装与
   使用说明、演示材料。

---

## 2. Review

### 2.1 发行打包（PR #24）

两个产品族完全独立：客户端（AppImage / `.deb` / `tar.xz`）与服务端（`.deb` /
`tar.xz`）。打包入口是 `packaging/build-release.sh`，内容来源是既有的
`scripts/stage-client-release.sh` / `scripts/stage-server-release.sh`。

**staging 脚本先跑门禁，再谈打包**：

- 工作树不干净直接失败（要覆盖必须显式 `--allow-dirty`，并把 dirty 写进
  `BUILD-INFO.txt`）；
- 输出目录先清空再填，避免上一次构建的残留混进清单；
- 产出 `BUILD-INFO.txt` 与按路径排序的 `MANIFEST.sha256`；
- 冒烟：`ldd` 不许有 not found、CLI 必须能 `--help`、GUI 在 offscreen 下
  10 秒内不许崩。

**为什么要用旧基线容器构建。** 开发机是 Ubuntu 24.04（glibc 2.39），生产 ECS 是
Ubuntu 22.04（glibc 2.35）。项目真实踩过一次：把开发机构建的服务端二进制拷到
ECS，启动时报 `GLIBC_2.38 not found`。因此服务端在 `ubuntu:20.04`、
客户端在 `debian:12`（Qt 6.4.2）里构建；客户端不能选 Ubuntu 22.04，因为
jammy 的 Qt 6.2.4 缺 `FolderDialog`（Qt 6.3 起才有的类型）。

**可复现性固定项**：`SOURCE_DATE_EPOCH`（默认取 commit 时间）、`TZ=UTC`、
`LC_ALL=C`、tar 的 `--sort=name --owner=0 --group=0 --numeric-owner
--mtime=@epoch`、`dpkg-deb --root-owner-group` 与固定的文件 mtime。
两次 clean 构建会比较文件清单、`MANIFEST.sha256` 与制品本身的 sha256。

**私钥边界**：服务端制品里不含任何私钥；打包完成后按结构规则（PEM 私钥块、
`seed-hex:` 行、`BACKUP_TOKEN_SECRET=` 行、32 字节裸密钥文件）扫描每个制品，
命中即失败。

**本阶段实测（2026-10-07）**：客户端 staging 9 个文件、服务端 staging
13 个文件（均含 `MANIFEST.sha256` 自身），冒烟全部 PASS；两份
`BUILD-INFO.txt` 记录的构建提交都是 `b3cd6bd`、`working_tree = clean`、
编译器 `g++ 14.2.0`。

### 2.2 源码质量收口（PR #26）

这一轮没有加功能，做的是"把重复与隐式合同收干净"：

| 提交 | 内容 |
| --- | --- |
| `fix(core): harden cleanup and failure-path contracts` | 清理路径与失败路径的合同 |
| `refactor(core): single source for delta layout and byte-order readers` | delta 布局与字节序读取收敛到单处 |
| `refactor(core): finish shared persistence and presentation helpers` | 共享持久化与展示辅助函数 |
| `style: enforce formatting across server and tools` | 服务端与工具统一 clang-format |
| `docs(code): finalize source comments and contracts` | 源码注释与合同收尾 |
| `refactor(gui): separate development harness from application entry` | GUI 开发夹具与产品入口分离 |
| `fix(ui): restore the source-text contracts the cleanup broke` | 修回被清理改坏的界面 source-text 合同 |
| `style: add trailing newlines to new text files` | 文本文件结尾换行 |
| `chore(deploy): update official cloud endpoint` | 官方云端地址 |

### 2.3 GUI 修正（PR #27）

备份管理页在恢复快照链时没有把"这一份是链上的一员、恢复它需要哪些父快照"的状态
显示出来。PR #27 在 `BackupRecordCard.qml` 与相关控制器/夹具上补齐了这部分显示，
并把开发夹具（`dev_harnesses`）与产品入口分开维护。

### 2.4 收口验收（2026-10-07 实测）

| 检查 | 命令 | 结果 |
| --- | --- | --- |
| 功能基线 | `bash scripts/test.sh` | PASS=279 / FAIL=0 |
| 全量门禁 | `bash scripts/final_gate.sh` | 49 行 GATE 结果全部 `exit=0`（46 个独立套件 + 3 个构建 / 重建步骤），failed suites = 0 |
| 现代 GUI | `bash scripts/modern_gui_check.sh` | 692 项通过 / 0 项失败 |
| 编译器警告 | gate 的 build / sanitize-build / gui-rebuild | 0 |
| 格式与 lint | `bash scripts/lint.sh` | PASS |
| 注释率 | `python3 scripts/comment_ratio.py` | 21.4%（183 个文件、91438 行；19566 行注释行、597 行行尾注释） |
| 行宽 | `python3 scripts/source_style_check.py` | 超 80 列的代码行与注释行均为 0 |

上表是 2026-10-07 收口期间的真实结果：功能基线与全量门禁产自 PR #26 合并后的
`448b623`（那一轮 gate 日志里 suite-main 自报 PASS=279 / FAIL=0，failed suites = 0），
现代 GUI 检查完整的一轮为 692 项通过 / 0 项失败，并在 PR #27 合并后于当前基线上再跑了
一次。PR #27 只改 GUI 显示与开发夹具，没有改动任何套件脚本。

这一轮保留的证据文档包括：`docs/release-packaging.md`、
`docs/install-client.md`、`docs/install-server.md`、`docs/upgrade-server.md`、
`docs/release-layout.md`、`docs/secure_transport.md`、`docs/bpsec2-design.md`，
以及本次补写的 `docs/scrum/` 阶段记录与更新后的 `docs/gantt/project_gantt.tex`。

---

## 3. Retrospective

### 3.1 做得较好的地方

**发行包的门禁从 staging 脚本继承，而不是在打包层重写一遍。**
干净树、`BUILD-INFO`、`MANIFEST`、`ldd` 冒烟、CLI/GUI 冒烟、私钥排除都是既有
脚本的行为，打包层只加启动器、配置、unit 与安装脚本。

**质量指标是只读工具，不是手工统计。**
`scripts/comment_ratio.py` 用最小词法状态机区分字符串里的 `//` 与真正的注释行，
`scripts/source_style_check.py` 按**显示宽度**（中文占两列）而不是字节数判断 80 列；
两个工具都只读、确定性、可重复运行，因此"注释率 21.4%"这类数字可以被别人复核。

**gate 把"没跑到"当成失败。**
`scripts/final_gate.sh` 里缺一条 ok 行就算失败，避免"某个套件其实没被执行"被当作
通过；这也是 Phase 6 里发现"某条套件从未进过 gate"之后补上的机制。

**升级失败必须能回滚，而且回滚快照是硬前置条件。**
服务端升级前先做回滚快照，快照失败就在解包**之前**中止（fail closed）；这条合同有
专门的测试证明"失败即中止、且服务器状态保持不变"。

### 3.2 遇到的问题与处理

**1）打包脚本的失败大多来自 shell 细节，而不是打包逻辑。**
本阶段实际修掉的：`dpkg -S` 返回非零在 `set -e` 下中断构建；
`ARCH=x86_64` 泄漏进 `.deb` 的 Architecture 字段；`apt install` 拒绝
`./绝对路径`；`pipefail` 下 SIGPIPE（141）把一次干净的扫描变成脚本中止；
制品上传/下载丢可执行位；`.gitignore` 把 `backupctl` 包装脚本一起吞掉；
新增的 `preinst` 其实没有被打进 `.deb`。每一条都补了对应的回归断言——这些坑
单独看都是小事，但都会让"看起来成功"的发行包在别人机器上失败。

**2）关于 Ubuntu 22.04 的结论一度是错的。**
早期把 `libicu*.so.72` 当成系统依赖，于是得出"22.04 不支持"的结论；实际上 Qt 与
它的依赖是随客户端包分发的。同一轮还发现 `.deb` 的 `Depends` 由
`dpkg-shlibdeps` 为**我们自己随包**的库申报了发行版包名，导致"能跑但装不上"的
矛盾。现在随包 `.so` 会逐个映射回提供包并从 `Depends` 中剔除，CI 里有一个作业
在干净的 `ubuntu:22.04` 容器里实测 AppImage 启动、`.deb` 安装与 GUI 启动。

**3）"清理"改坏了界面依赖的 source-text 合同。**
源码质量收口改动了一批注释与字符串，其中有一些是现代 GUI 检查脚本按文本断言的
合同。修复方式是恢复合同，并把这类隐式约定显式写进检查脚本（source-contract
计数器），让下一次清理能立刻发现。

**4）文档与代码的"最后一公里"。**
安装、升级、发行布局这些文档是在打包过程中边做边写的，出现过"文档说的路径和实际
制品不一致"的问题，靠"按制品实测后再改文档"的方式逐条对齐。

### 3.3 遗留与如实说明

- 仓库没有 LICENSE 文件，发行包的 `copyright` 如实记录这一点，不替权利人补许可证；
- 只构建并验证了 x86_64 Linux；没有 ARM64 / Windows / macOS；
- AppImage 里的 squashfs 会写入自己的时间戳，逐字节可复现性用"解包后比较内容"
  的方式处理，并在证据里写明原因；
- Widgets 与 Qt Quick 两套 GUI 仍然并存，业务逻辑只在核心一处；
- 安全传输与证书层是手写实现，有官方向量、交叉 oracle、fuzz 与消毒剂验证，
  但**没有经过外部安全审计**。

---

## 4. 阶段结论

收口阶段完成了三件事：可复现的发行制品、可执行的质量检查、齐全的课程文档与证据。
产品代码冻结在 `6d1e2ba`，此后只更新文档。

```text
代码冻结（6d1e2ba）
  → 发行打包：Client AppImage / deb / tar.xz + Server deb / tar.xz
  → 质量收口：test.sh 279 PASS、final_gate 49 项全绿、GUI 692 项全绿、注释率 21.4%
  → 文档归档：Gantt、Scrum 阶段记录、安装 / 升级 / 安全传输文档
  → 下一步：最终报告与答辩
```
