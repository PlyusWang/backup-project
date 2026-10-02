# Backup Project

Linux 环境下的数据备份与恢复软件课程项目。

## 技术路线

- 核心后端：C++17
- 主要运行平台：Linux
- 桌面前端：Qt 6 Widgets
- 异步执行：Qt Concurrent
- 构建：GNU Make
- 版本控制：Git / GitHub
- C++ 编程规范：Google C++ Style Guide
- 工程文档：Markdown
- 最终报告：LaTeX → PDF

## 当前目标

项目计划完成不少于 120 分的课程功能。

当前首先完成：

```text
基础 Backup / Restore
```

即建立：

```text
源目录
   │
   ▼
备份
   │
   ▼
备份仓库
   │
   ▼
还原
   │
   ▼
恢复目录
```

的最小闭环。

## 文档

- `docs/00_project_baseline.md`：项目工程基线
- `docs/01_requirements.md`：需求分析
- `docs/02_architecture.md`：系统设计
- `docs/03_testing.md`：软件测试
- `docs/04_release_and_demo.md`：发布与演示
- `docs/backlog/backup_mode_roadmap.md`：备份触发方式 × 备份策略的后续开发路线
- `docs/scheduled_backup_usage.md`：定时备份（Scheduled + Full）的使用说明
- `docs/network_backup_usage.md`：远程备份使用说明（BPSEC1 传输加密 + 远端增量）
- `docs/secure_transport.md`：BPSEC1 —— 本项目自己的认证加密传输层（协议、安全性质、明确的非目标）
- `docs/remote_incremental.md`：远端增量备份与恢复（材料包、链元数据、信任模型、限制）
- `docs/research/pr21_secure_transport_sources.md`：X25519 / HKDF / AES-CTR 的规范出处与测试向量来源

## 开发环境

主要开发环境：

```text
Windows
   │
   │ SSH
   ▼
VMware Ubuntu
   │
   ├── GCC / G++
   ├── GNU Make
   ├── Git
   ├── gdb
   ├── Valgrind
   └── backup-project
```

后续网络备份可连接 Alibaba Cloud ECS。
## Sprint 1：基础 CLI Backup / Restore（v0.1）

```bash
make                      # 构建产品 CLI build/backupctl
./build/backupctl backup <source_directory>          # 写进配置好的备份仓库
./build/backupctl preview <source_directory>         # 只读：先看看会选中哪些条目
./build/backupctl restore <file_name> <destination_directory>
```

产品 CLI 与 Modern GUI 是**同一套业务模型**：归档落在配置好的仓库里、文件名由程序
生成（`backupctl repository list` 可以列出），恢复只接受仓库内的单组件
`.bak` 名字。想指定任意归档路径的能力只存在于测试夹具
`build/archive-cli`（不是产品命令、也不在默认构建目标里，要
`make test-fixtures` 才会构建，见 `docs/basic_cli_usage.md`）。

打包时可以按需筛选哪些内容进入备份文件；`preview` 用同一组规则先列一遍，
它不建归档、不需要仓库、也不改任何状态：

```bash
./build/backupctl preview ./source \
  --include 'ext:cpp;h;hpp' \
  --exclude 'path:**/build/**'

./build/backupctl backup ./source \
  --include 'ext:cpp;h;hpp' \
  --exclude 'path:**/build/**' \
  --exclude 'ext:tmp;log'
```

`--include` / `--exclude` 都可以重复出现，规则语法与语义见
`docs/filter_usage.md`，后续计划见 `docs/backlog/filter_future.md`。
不加任何规则时行为与之前完全一致。

备份产物是一个单独的归档文件（推荐扩展名 `.bak`），里面是我们自己的
Archive Format v0.1：全局 header + 逐条 entry header + 原样照抄的文件正文。
这是**打包**而不是压缩——payload 与源文件逐字节相同，归档只会比原内容大。

格式说明见 `docs/format/archive_v0.1.md`，用法与行为约定见
`docs/basic_cli_usage.md`。

## 桌面 GUI

项目里有两套并行存在的桌面界面，共用同一份 `BackupEngine`，互不依赖、互不覆盖：

### Qt 6 Widgets 版（稳定）

```bash
make gui                                                     # 构建 build/backup-gui
./build/backup-gui                                           # 启动图形界面
QT_QPA_PLATFORM=offscreen ./build/backup-gui --smoke-test    # 无显示环境下自检
./scripts/gui_smoke_test.sh                                  # 构建 + 自检一步完成
```

依赖：Qt 6 开发包（Ubuntu：`sudo apt-get install -y qt6-base-dev qt6-base-dev-tools`）。
详见 `docs/ui/desktop_gui.md`。

### Qt Quick / QML 现代版

```bash
make gui-modern                                              # 构建 build/backup-gui-modern
make gui-all                                                 # 两套一起构建
./build/backup-gui-modern                                   # 启动图形界面
QT_QPA_PLATFORM=offscreen QSG_RHI_BACKEND=software ./build/backup-gui-modern --smoke-test
./scripts/modern_gui_check.sh                               # 构建 + 启动 + 静态约束 + 端到端
```

依赖：Qt 6 QML 开发包（`sudo apt-get install -y qt6-declarative-dev qt6-declarative-dev-tools
qml6-module-qtquick qml6-module-qtquick-controls qml6-module-qtquick-layouts
qml6-module-qtquick-dialogs qml6-module-qtquick-window qml6-module-qtqml-workerscript`）。
详见 `docs/ui/modern_qml_gui.md`。

`make` 仍然只构建 CLI；两套 GUI 各自独立，`make gui` 不会顺手把现代版也编出来。
Filter 可视化规则编辑器：Modern GUI 提供可视化 Filter 规则编辑器（字段表单 + 规则列表 + 后台预览 + 只读 DSL + CLI 等价参数），规则最终仍由同一个 C++ Filter 核心匹配，语义与 CLI 一致。
