# 基础 CLI 使用说明（Sprint 1 / v0.1）

> 分支：feature/basic-backup-restore
> 状态：v0.1 实现文档

## 1. 本阶段实现范围

- 普通文件的递归备份与恢复（字节级一致，含空文件与二进制文件）；
- 普通目录（含空目录、多级嵌套目录）的备份与恢复；
- 文件名支持空格与 UTF-8（中文）；
- 错误路径明确报错并返回非 0：源不存在、源不是目录、仓库不存在、
  仓库缺少 data/、目标已存在且非空、不支持的文件类型、权限不足、
  无法创建目录等。

## 2. 本阶段不包含

元数据（UID/GID/mode/时间戳）、符号链接与硬链接身份、FIFO/设备/socket、
文件过滤、打包、压缩、加密、定时备份、实时备份（inotify）、网络备份
等。遇到不支持的文件类型会明确失败并返回非 0。

## 3. 构建

```bash
./scripts/build.sh
# 或
make
# 清理后重建
make clean && make
```

产物：`build/backupctl`

## 4. CLI 用法

```bash
./build/backupctl backup <source_directory> <repository>
./build/backupctl restore <repository> <destination>
./build/backupctl --help
```

备份数据保存在 `<repository>/data/` 下。

## 5. 完整示例

```bash
./build/backupctl backup testdata/source repository
./build/backupctl restore repository restored
diff -r testdata/source restored   # 应无差异
```

## 6. 目标路径已存在时的策略

- `repository/data` 已存在且非空 → 备份失败，拒绝覆盖；
- restore 的 destination 已存在且非空 → 恢复失败，拒绝覆盖；
- 已存在的**空目录** → 允许直接使用。

## 7. 退出码

| 退出码 | 含义 |
| --- | --- |
| 0 | 成功 |
| 1 | 操作失败（路径、文件类型、I/O 等） |
| 2 | 命令行用法错误 |

## 8. 测试与质量检查

```bash
make test       # 端到端 round-trip + 错误路径测试
./scripts/lint.sh
valgrind --leak-check=full --show-leak-kinds=all ./build/backupctl backup <src> <repo>
valgrind --leak-check=full --show-leak-kinds=all ./build/backupctl restore <repo> <dest>
make sanitize   # ASan + UBSan 构建，产物 build-sanitize/backupctl
```

## 9. 主要源文件及职责

| 文件 | 职责 |
| --- | --- |
| app/backupctl.cpp | CLI 参数解析、结果输出、退出码 |
| include/backup_engine.h / src/core/backup_engine.cpp | Backup / Restore 高层流程 |
| include/file_system.h / src/filesystem/file_system.cpp | 目录树递归复制（CopyTree） |
| scripts/test.sh | 自动测试脚本 |
