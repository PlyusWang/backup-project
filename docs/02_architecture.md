# 系统设计

> 文档编号：02  
> 文档名称：数据备份与恢复系统设计  
> 状态：Sprint 1+2 收口版  
> 基线：PR #8 合并后的 `main`  
> 对应 UML：`docs/uml/class.mdj`、`component.mdj`、`sequence_backup.mdj`、`sequence_restore.mdj`

---

## 1. 设计目标

Sprint 1+2 的系统设计目标是建立一个可演示、可测试、可继续扩展的本地备份核心：

```text
源目录
  ↓
ArchiveWriter
  ↓
Archive Format v0.1 (.bak)
  ↓
ArchiveReader
  ↓
恢复目录
```

当前 Archive 层只做**归档**，不做压缩和加密。

---

## 2. 总体分层

```text
┌───────────────────────────────────────────────┐
│ Presentation                                  │
│                                               │
│  backupctl   Qt Widgets GUI   Qt Quick/QML GUI│
└───────────────────────┬───────────────────────┘
                        │
                        ▼
┌───────────────────────────────────────────────┐
│ Application                                   │
│                   BackupEngine                │
└───────────────┬──────────────────┬────────────┘
                │                  │
          Backup│                  │Restore
                ▼                  ▼
┌──────────────────────┐  ┌──────────────────────┐
│ ArchiveWriter        │  │ ArchiveReader        │
│ Archive Format v0.1  │  │ Preflight + Extract  │
└──────────┬───────────┘  └──────────┬───────────┘
           │                          │
           ├─────────────┬────────────┤
           ▼             ▼
      FileSystem     Linux / POSIX
```

三个用户入口共用同一个 `BackupEngine`，不存在 GUI 自己复制/归档、CLI 走另一套实现的情况。

---

## 3. 构件设计

### 3.1 CLI：`app/backupctl.cpp`

职责：

- 解析命令；
- 校验命令行参数数量；
- 调用 `BackupEngine::Backup` / `Restore`；
- 输出错误；
- 返回退出码。

CLI 不解析 Archive Format，不直接复制文件。

### 3.2 Qt Widgets GUI

职责：

- 选择源目录；
- 选择备份文件；
- 选择恢复目录；
- 异步发起备份 / 恢复；
- 展示状态和错误。

核心操作通过 C++ `BackupEngine` 完成。

### 3.3 Qt Quick / QML GUI

职责与 Widgets 版本一致。

QML 负责界面与交互；C++ Controller 负责把界面请求转给 `BackupEngine`。QML 不实现归档算法。

### 3.4 `BackupEngine`

对应类：`backupproject::BackupEngine`

职责：

- 处理高层参数；
- 使用 `FileSystem::InspectPath` 判断输入类型；
- Backup 时创建并调用 `ArchiveWriter`；
- Restore 时创建并调用 `ArchiveReader`；
- 向上层返回统一的成功 / 失败和错误信息。

`BackupEngine` **不负责**：

- header 编码；
- payload 流式读写；
- path traversal 校验细节；
- metadata 序列化；
- preflight 解析。

这些属于 Archive 层。

### 3.5 `ArchiveWriter`

职责：

1. 检查源目录；
2. 复用 `FileSystem::IsDestinationOutsideSource` 做危险拓扑检查；
3. 确认归档目标不存在；
4. 按需创建父目录；
5. 使用 `O_EXCL` 创建归档；
6. 写全局 Header；
7. 递归扫描目录；
8. 写 Entry Header / path / metadata / raw payload；
9. 回填 `entry_count`；
10. 失败时删除半成品归档。

普通文件 payload 使用固定 64 KiB 缓冲流式处理，不把整个文件读入内存。

### 3.6 `ArchiveReader`

职责分为两阶段。

#### 阶段一：Preflight

完整读取并验证：

- magic；
- format version；
- flags；
- header size；
- entry count；
- entry type；
- reserved 字段；
- path length；
- mode；
- mtime nanoseconds；
- 路径合法性；
- 重复路径；
- 父目录结构；
- payload 边界；
- trailing bytes；
- root `.` entry。

Preflight 阶段只读归档，不写目标目录。

#### 阶段二：Extract

Preflight 成功后：

1. 检查目标目录不存在或为空；
2. 建立目标根目录；
3. 目录先使用 `0700` staging 权限创建；
4. 流式恢复普通文件；
5. 文件关闭后恢复 mode / mtime；
6. 目录 metadata 从深到浅恢复；
7. root 最后恢复。

---

## 4. 核心类设计

### 4.1 `BackupEngine`

```text
BackupEngine
  - file_system_: FileSystem

  + Backup(...)
  + Restore(...)
```

与 `FileSystem` 是强生命周期关系：`FileSystem` 作为成员随 `BackupEngine` 存在，可在类图中表示为 Composition。

`ArchiveWriter` / `ArchiveReader` 是方法内部创建并调用的对象，因此表示 Dependency 更符合当前代码。

### 4.2 `FileSystem`

公开能力：

- `JoinPath`
- `InspectPath`
- `MakeDirectories`
- `IsMissingOrEmptyDirectory`
- `CopyTree`
- `IsDestinationOutsideSource`

`PathStatus`：

```text
kMissing
kDirectory
kRegularFile
kOther
kError
```

其中 `CopyTree` 主要保留自 Sprint 1 的基础文件系统能力；PR #8 的正式 `.bak` 流程主要由 ArchiveWriter / Reader 使用更细粒度 POSIX 操作。

### 4.3 `ArchiveWriter`

公开接口：

```text
Write(source_directory, archive_file, error_message)
```

### 4.4 `ArchiveReader`

公开接口：

```text
Extract(archive_file, destination_directory, error_message)
```

---

## 5. Archive Format v0.1

### 5.1 全局 Header

固定 24 字节：

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | magic = `BKPARCH\0` |
| 8 | 2 | version = 1 |
| 10 | 2 | flags = 0 |
| 12 | 4 | header_size = 24 |
| 16 | 8 | entry_count |

### 5.2 Entry Header

固定 32 字节：

| Offset | Size | Field |
|---:|---:|---|
| 0 | 1 | type |
| 1 | 1 | reserved0 |
| 2 | 2 | reserved1 |
| 4 | 4 | path_length |
| 8 | 4 | mode |
| 12 | 4 | mtime_nsec |
| 16 | 8 | mtime_sec |
| 24 | 8 | payload_size |

Entry 实际布局：

```text
[32 B header][relative path bytes][raw payload]
```

目录 payload size 为 0。

### 5.3 字节序

所有整数手工按 little-endian 序列化，不直接把 C++ struct 写进文件，从而避免：

- padding；
- alignment；
- 主机字节序；
- 编译器布局差异。

---

## 6. 元数据设计

v0.1 保存：

- path；
- type；
- `mode & 0777`；
- `mtime_sec`；
- `mtime_nsec`；
- file size。

暂不保存：

- UID；
- GID；
- ACL；
- xattr；
- atime；
- ctime；
- birth time；
- setuid / setgid / sticky。

---

## 7. 路径与安全设计

### 7.1 Backup 拓扑

归档文件不得：

- 等于 source；
- 位于 source 内部。

检查在输出文件创建之前完成。

### 7.2 Restore Path Traversal

ArchiveReader 在 preflight 中拒绝：

- 绝对路径；
- `..`；
- `.` 中间段；
- 重复分隔符；
- trailing slash；
- NUL；
- backslash；
- Windows drive-like path；
- 重复路径；
- 文件被当作父目录的冲突结构；
- 缺失父目录 entry。

### 7.3 已知 TOCTOU 限制

当前设计仍属于：

```text
pathname 检查
→ 稍后再次根据 pathname 执行
```

因此不抵抗恶意并发本地进程在检查与使用之间主动替换路径组件。

后续高可靠方案优先考虑：

```text
dirfd + openat
```

以及 Linux：

```text
openat2 + RESOLVE_*
```

当前 Sprint 不实现。

---

## 8. 错误处理与资源管理

- fd / `DIR*` 使用 RAII helper 管理；
- `EINTR` 重试；
- partial read / write 显式处理；
- 创建归档使用 `O_EXCL`；
- 写侧失败删除半成品；
- 读侧先 preflight 后落盘；
- 数值边界尽量使用避免溢出的比较方式；
- 错误信息包含动作、路径和系统错误原因。

---

## 9. 备份顺序

对应 `sequence_backup.mdj`：

```text
用户
→ CLI / Desktop GUI
→ BackupEngine
→ FileSystem::InspectPath
→ ArchiveWriter::Write
→ topology / target checks
→ create archive
→ recursive scan
→ write entry / metadata / raw payload
→ patch entry_count
→ return result
```

关键点：

- UI 不直接访问归档格式；
- `BackupEngine` 不负责二进制布局；
- ArchiveWriter 在创建归档前完成危险拓扑检查；
- payload 流式写入。

---

## 10. 恢复顺序

对应 `sequence_restore.mdj`：

```text
用户
→ CLI / Desktop GUI
→ BackupEngine
→ FileSystem::InspectPath
→ ArchiveReader::Extract
→ PreflightArchive
→ destination validation
→ staging directories
→ restore files
→ file metadata
→ directory metadata deepest-to-root
→ return result
```

关键点：

- preflight 未通过时不创建恢复目标；
- 目录先可写、后恢复真实权限；
- 文件 close 后再恢复 metadata；
- root metadata 最后应用。

---

## 11. UML 对应关系

| UML | 主要表达内容 |
|---|---|
| 总体用例图 | 本地用户能完成什么；规划能力与当前能力边界 |
| `class.mdj` | `BackupEngine`、`FileSystem`、`ArchiveWriter`、`ArchiveReader` 的真实关系 |
| `component.mdj` | CLI / 两套 GUI / Core / Archive / POSIX 的构件依赖 |
| `sequence_backup.mdj` | 一次真实 Backup 的调用顺序 |
| `sequence_restore.mdj` | 一次真实 Restore 的调用顺序 |

UML 中不为了“画得丰富”创造代码中不存在的核心类。

---

## 12. 后续演进边界

后续功能应以独立层接入，不应破坏当前职责边界。

规划流水线：

```text
源目录
  ↓
Filter
  ↓
Archive
  ↓
Compression
  ↓
Crypto
  ↓
Storage
```

恢复执行逆向流程。

Scheduler / Watcher 未来只负责触发统一 Backup 流程，不创建第二套备份实现。

Sprint 1+2 当前版本不实现这些未来模块。
