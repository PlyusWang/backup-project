# Backup Project

Linux 环境下的数据备份与恢复软件课程项目。

## 技术路线

- 核心后端：C++17
- 主要运行平台：Linux
- 前端：Web
- Web 辅助层：Python
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
