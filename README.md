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
make                      # 构建 build/backupctl
./build/backupctl backup <source_directory> <repository>
./build/backupctl restore <repository> <destination>
```

备份数据存放于 `<repository>/data/`。更多用法、行为约定与测试方法见
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
