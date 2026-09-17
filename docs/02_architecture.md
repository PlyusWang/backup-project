# 系统设计

> 文档编号：02  
> 文档名称：数据备份与恢复系统设计  
> 状态：Sprint 1+2 收口版 + Filter 集成  
> 基线：PR #8 的 Archive Format v0.1、Sprint 1+2 收口文档，以及 PR #9 Filter  
> 对应 UML：`docs/uml/class.mdj`、`component.mdj`、`sequence_backup.mdj`、`sequence_restore.mdj`

---

## 1. 设计目标

Sprint 1+2 的系统设计目标是建立一个可演示、可测试、可继续扩展的本地备份核心；PR #9 在这个核心上增加可选的文件筛选层：

```text
源目录
  ↓
Filter（可选；无规则时等价于全收）
  ↓
ArchiveWriter
  ↓
Archive Format v0.1 (.bak)
  ↓
ArchiveReader
  ↓
恢复目录
```

当前 Archive 层只做**归档**，不做压缩和加密。Filter 只决定哪些源条目进入归档，不修改归档格式，也不参与恢复端重新筛选。

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
│ Filter               │  │ ArchiveReader        │
│ optional rules       │  │ Preflight + Extract  │
└──────────┬───────────┘  └──────────┬───────────┘
           │                          │
           ▼                          │
┌──────────────────────┐             │
│ ArchiveWriter        │             │
│ Archive Format v0.1  │             │
└──────────┬───────────┘             │
           ├─────────────┬────────────┤
           ▼             ▼
      FileSystem     Linux / POSIX
```

三个用户入口共用同一个 `BackupEngine`，不存在 GUI 自己复制/归档、CLI 走另一套实现的情况。CLI 与两套 GUI 的筛选规则最终也进入同一份 C++ `Filter` 实现，界面层不另写 glob 匹配逻辑。

---

## 3. 构件设计

### 3.1 CLI：`app/backupctl.cpp`

职责：

- 解析命令；
- 校验命令行参数数量；
- Backup 时解析可重复的 `--include` / `--exclude` 规则；
- 调用 `BackupEngine::Backup` / `Restore`；
- 输出错误；
- 返回退出码。

CLI 不解析 Archive Format，不直接复制文件，也不自行实现匹配算法。

### 3.2 Qt Widgets GUI

职责：

- 选择源目录；
- 选择备份文件；
- 选择恢复目录；
- Backup 时编辑 Include / Exclude 规则；
- 异步发起备份 / 恢复；
- 展示状态和错误。

核心操作通过 C++ `BackupEngine` 完成；筛选规则同样交给共享的 C++ Filter。

### 3.3 Qt Quick / QML GUI

职责与 Widgets 版本一致。

QML 负责界面与交互；C++ Controller 负责把界面请求和规则转给 `BackupEngine`。QML 不实现归档算法，也不实现 glob。

### 3.4 `BackupEngine`

对应类：`backupproject::BackupEngine`

职责：

- 处理高层参数；
- 使用 `FileSystem::InspectPath` 判断输入类型；
- Backup 时接收可选筛选规则并创建、调用 `ArchiveWriter`；
- Restore 时创建并调用 `ArchiveReader`；
- 向上层返回统一的成功 / 失败和错误信息。

`BackupEngine` **不负责**：

- header 编码；
- payload 流式读写；
- path traversal 校验细节；
- metadata 序列化；
- preflight 解析；
- GUI 层的规则编辑。

这些分别属于 Archive、Filter 或 Presentation 层。

### 3.5 `Filter`

对应：`include/filter.h`、`src/filter/filter.cpp`。

职责：

- 解析 Include / Exclude 规则；
- 基于 `name`、`path`、`stem`、`ext`、`type`、`size`、`mtime` 判断条目；
- 处理 `*`、`?`、`**` glob；
- 对明确排除的目录执行子树剪枝；
- 保证 `exclude` 优先；
- 在存在 include 时要求普通文件至少命中一条 include；
- 无规则时保持 PR #8 行为不变。

Filter 只看条目的路径和 `lstat` 可得元数据，不读文件内容，不做压缩、加密、增量、网络或完整性校验。

规则语法和固定语义见 `docs/filter_usage.md`，延期能力见 `docs/backlog/filter_future.md`。

### 3.6 `ArchiveWriter`

职责：

1. 检查源目录；
2. 复用 `FileSystem::IsDestinationOutsideSource` 做危险拓扑检查；
3. 确认归档目标不存在；
4. 按需创建父目录；
5. 使用 `O_EXCL` 创建归档；
6. 写全局 Header；
7. 递归扫描目录，并在写 entry 前应用可选 Filter；
8. 写 Entry Header / path / metadata / raw payload；
9. 回填 `entry_count`；
10. 失败时删除半成品归档。

普通文件 payload 使用固定 64 KiB 缓冲流式处理，不把整个文件读入内存。

被 Filter 剪枝的目录不再向下扫描；没有被排除的 symlink / FIFO / socket / 设备文件仍然按当前归档能力边界让整次备份失败。

### 3.7 `ArchiveReader`

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

恢复端不重新执行 Filter；归档中实际存在什么条目，就按归档内容恢复什么。

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

`ArchiveWriter` / `ArchiveReader` 是方法内部创建并调用的对象，因此表示 Dependency 更符合当前代码。Filter 作为备份调用中的可选策略数据传入归档写入流程，不在恢复流程重复运行。

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

其中 `CopyTree` 主要保留自 Sprint 1 的基础文件系统能力；PR #8 之后的正式 `.bak` 流程主要由 ArchiveWriter / Reader 使用更细粒度 POSIX 操作。

### 4.3 `Filter`

核心公开能力包括：

```text
AddRule(...)
ShouldPruneDirectory(...)
ShouldIncludeFile(...)
ShouldSkipSpecialEntry(...)
```

规则内部多个子句为 AND，规则之间为 OR；`exclude` 优先。Glob 使用 DP 匹配，避免朴素递归回溯在病态模式下指数爆炸。

### 4.4 `ArchiveWriter`

公开接口仍由 `include/archive.h` 定义；PR #9 在不改变 Archive Format v0.1 的前提下，把可选 Filter 接到递归扫描和 entry 写入判断之前。

### 4.5 `ArchiveReader`

公开接口：

```text
Extract(archive_file, destination_directory, error_message)
```

Filter 不改变 `ArchiveReader` 的恢复语义。

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

Filter 仅决定哪些 entry 被写入，不改变 Header / Entry Header 的二进制布局，因此 PR #9 不提升归档格式版本。

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

Filter 当前可基于部分扫描期元数据（例如 size / mtime）做选择，但这不等于这些额外字段都会写入归档。

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

当前阶段不实现。

---

## 8. 错误处理与资源管理

- fd / `DIR*` 使用 RAII helper 管理；
- `EINTR` 重试；
- partial read / write 显式处理；
- 创建归档使用 `O_EXCL`；
- 写侧失败删除半成品；
- 读侧先 preflight 后落盘；
- 非法 Filter 规则明确报错，不能被静默忽略；
- 非法规则失败时不留下半成品 `.bak`；
- 数值边界尽量使用避免溢出的比较方式；
- 错误信息包含动作、路径和系统错误原因。

---

## 9. 备份顺序

Sprint 1+2 的顺序图记录了归档主链路；加入 PR #9 后，备份路径增加 Filter 决策：

```text
用户
→ CLI / Desktop GUI
→ BackupEngine
→ FileSystem::InspectPath
→ 解析 / 传入 Filter 规则
→ ArchiveWriter::Write
→ topology / target checks
→ create archive
→ recursive scan
→ Filter 决定 prune / include / exclude
→ write entry / metadata / raw payload
→ patch entry_count
→ return result
```

关键点：

- UI 不直接访问归档格式；
- UI 不实现 glob；
- `BackupEngine` 不负责二进制布局；
- ArchiveWriter 在创建归档前完成危险拓扑检查；
- Filter 发生在 entry 写入之前；
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
- root metadata 最后应用；
- Restore 不再次运行 Filter。

---

## 11. UML 对应关系

| UML | 主要表达内容 |
|---|---|
| 总体用例图 | Sprint 1+2 收口时本地用户能完成什么，以及规划能力边界 |
| `class.mdj` | Sprint 1+2 的 `BackupEngine`、`FileSystem`、`ArchiveWriter`、`ArchiveReader` 真实关系 |
| `component.mdj` | Sprint 1+2 的 CLI / 两套 GUI / Core / Archive / POSIX 构件依赖 |
| `sequence_backup.mdj` | Sprint 1+2 一次真实 Backup 的调用顺序 |
| `sequence_restore.mdj` | 一次真实 Restore 的调用顺序 |

这些 UML 是 Sprint 1+2 的阶段基线；PR #9 的 Filter 为后续功能增量，其真实结构以本文件和 `docs/filter_usage.md` 为准。后续若生成新的阶段 UML，应把 Filter 正式加入 Backup 路径，但不需要改写历史阶段证据。

---

## 12. 后续演进边界

当前已经实现：

```text
源目录
  ↓
Filter
  ↓
Archive
  ↓
.bak
```

后续规划流水线：

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

恢复执行逆向流程，但 Filter 只在备份侧决定进入归档的源条目，不在恢复侧重复执行。

Scheduler / Watcher 未来只负责触发统一 Backup 流程，不创建第二套备份实现。

当前仍未实现 Compression、Crypto、Scheduler、Watcher、增量与网络备份等未来模块。
