# 系统设计

> 文档编号：02  
> 状态：Draft

本文档用于记录系统总体架构、构件设计、类设计、顺序图以及开发工具和依赖说明。

---

## 分层与备份流水线（Archive v0.1）

```text
CLI (app/backupctl.cpp)
Qt Widgets GUI (ui/desktop)
Qt Quick GUI (ui/modern)
        │
        ▼
BackupEngine（include/backup_engine.h）
        │  只做校验与编排：路径是否合理、什么时候可以动手
        ▼
Filter（include/filter.h）  ← 决定哪些条目进入归档（可选，无规则即全收）
        │
        ▼
ArchiveWriter / ArchiveReader（include/archive.h）
        │  Archive Format v0.1：全局 header + 逐条 entry header + 原样 payload
        ▼
POSIX 文件系统（open / read / write / mkdir / chmod / utimensat）
```

三个入口共用同一份 BackupEngine 和同一份归档实现：`make`、`make gui`、
`make gui-modern` 链接的都是 `src/core/backup_engine.cpp` 与 `src/archive/archive.cpp`，
不存在"某个入口还在用旧结构"的情况。

归档格式见 `docs/format/archive_v0.1.md`，要点：

- 是**打包**不是压缩：payload 逐字节原样保存，归档只会比原内容大；
- 保存相对路径、条目类型、mode（0777 位）、mtime（秒 + 纳秒）和文件大小；
- 读侧先 preflight 校验整个归档，结构合法之后才动磁盘，坏归档不会留下半个恢复目录；
- 筛选（Filter）在归档层之前：CLI 与两套 GUI 共用同一份 C++ 实现，
  规则语法与语义见 docs/filter_usage.md，后续计划见 docs/backlog/filter_future.md；
- 软链接、FIFO、socket、设备文件一律让整次备份失败，不跳过、不跟随。

**压缩（Compression）与加密（Encryption）当前都不存在**：本仓库没有实现任何压缩算法，
也没有加密、文件过滤、增量备份、去重、多版本、网络备份等能力。
归档层将来的定位是压缩层的输入层，但这一层本身只负责打包。

## 已知可靠性限制与后续改进

### TOCTOU 路径竞争风险

> 状态：**已知限制，尚未实现**。本节只记录问题、现状与候选方案，当前代码没有做任何 TOCTOU 强化。

#### 1. 当前已经解决的问题

Sprint 1 的实现里，`FileSystem::CopyTree` 在真正产生任何文件系统写操作之前，先对 source / destination 做一次路径拓扑检查：

```text
std::filesystem::absolute
    → std::filesystem::weakly_canonical
    → lexically_normal
    → 按 path component 逐段比较
```

只要出现下面两种情况就直接拒绝，不建目录、不开始复制：

- `destination == source`；
- `destination` 是 `source` 的后代。

这一条规则解决的是**调用开始时路径就已经非法**的真实 Bug（ROB-02 / ROB-03），覆盖：

- 归档文件（backup file）位于 source 内部；
- restore 的 destination 与归档文件、源目录形成危险的父子关系；
- `archive_file == source_directory`；
- 相对路径，以及 `.`、`..` 规范化之后才暴露出来的危险拓扑；
- 父目录中存在软链接而形成的实际父子关系。

#### 2. 当前仍存在的理论风险

上面的检查仍然是“先按 pathname 判断，之后再按 pathname 操作”，因此带有典型的 TOCTOU（Time Of Check To Time Of Use，检查时刻与使用时刻之间的竞争）风险。当前流程可以概括为：

1. 根据 pathname 做 canonicalize；
2. 比较 source 与 destination 的关系；
3. 检查通过；
4. 稍后再用 pathname 执行 `mkdir` / `open` / 递归复制。

如果另一个进程恰好在这两步之间改动目录结构（rename 目录、删除后重建目录、把某个路径组件替换成 symlink），那么“检查时看到的路径对象”和“真正执行复制时 pathname 解析到的对象”理论上可能已经不是同一个。

举例：检查时 `/tmp/output` 在 `/tmp/source` 之外，判定安全；检查完成之后，另一个进程把 `/tmp/output` 替换成指向 `/tmp/source/subdir` 的符号链接。如果后续操作再次根据字符串 pathname 解析路径，之前的安全结论就可能失效。

需要特别区分：

- 现在已经修复的是“路径从一开始就非法”；
- TOCTOU 讨论的是“路径检查时合法，但在检查和真正使用之间被外部并发修改”。

这不是本次 ROB-02 / ROB-03 的原始问题，两者不能混为一谈。

#### 3. 候选方案与评价

| 方法 | 能解决什么 | 对 TOCTOU 的效果 |
| --- | --- | --- |
| `std::mutex` | 当前进程内部多个线程之间协调 | 基本不能解决来自其它进程的 TOCTOU |
| `flock` / `fcntl` | 遵守同一锁协议的进程之间协调 | 不能可靠保护 pathname，非协作进程仍可修改目录结构 |
| 限制目录写权限 | 从权限层控制其它进程修改目录 | 有帮助，但依赖部署环境和权限模型 |
| `canonicalize → open` | 发现调用开始时已经存在的错误路径拓扑 | 仍然存在检查与使用之间的时间窗口 |
| `dirfd + openat` | 先固定已打开的目录对象，再相对该对象访问子项 | 能显著降低 pathname 被替换带来的风险 |
| `openat2 + RESOLVE_*` | 让 Linux 内核在真正解析/打开路径时同时施加约束 | 更强，适合后续高可靠实现 |

#### 4. 后续推荐实现方向（TODO，未实现）

优先方向：

```text
dirfd
  + 按路径组件使用 openat
  + 必要的 O_DIRECTORY / O_NOFOLLOW 等约束
```

核心思想是**不要反复依赖完整 pathname 的重新解析**：

1. 尽早打开可信父目录并取得 fd；
2. 后续访问尽量相对于这个已经打开的目录 fd；
3. 必要时逐级打开子目录；
4. 避免在安全检查完成之后，又从根目录重新解析整条字符串路径。

在 Linux 平台上还应进一步研究 `openat2()` 以及 `RESOLVE_BENEATH`、`RESOLVE_NO_SYMLINKS`、`RESOLVE_NO_MAGICLINKS` 等路径解析约束。

需要重点说明：**`openat` 本身不是“写上这个函数名就自动解决全部 TOCTOU”**。如果仍然把包含多个路径组件、且允许跟随 symlink 的字符串直接交给它，风险依旧存在。更可靠的设计是：

- 固定 dirfd；
- 逐组件解析；
- 对中间目录施加约束；
- 或者直接使用 `openat2` 的 `RESOLVE_*` 规则让内核参与路径解析约束。

#### 5. 为什么 Sprint 1 暂不实现

当前项目是课程阶段的本地 Ubuntu 备份工具。Sprint 1 的主要可靠性目标是：**防止用户提供错误的父子路径导致递归自复制**。这个目标已经由 canonicalization + component comparison + 先检查后写入解决。

完整的 fd-based 文件系统遍历会明显增加：

- FileSystem 层复杂度；
- POSIX / Linux 专用代码量；
- 测试复杂度；
- 文件描述符生命周期管理；
- 特殊文件与 symlink 行为的设计成本。

因此本轮不实现 TOCTOU 强化，把它记录为**后续 Reliability / Security Hardening TODO**。当项目进入下面这些场景时再提高优先级：

- 长期运行的 backup daemon；
- 多进程并发环境；
- 多用户环境；
- 不可信本地用户；
- 高权限运行；
- 对路径竞争攻击有明确安全要求。
