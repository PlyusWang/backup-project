# Archive Format v0.1（归档格式 v0.1）

> 状态：v0.1，随 `feature/archive-format-v01` 分支引入。
> 实现见 `include/archive.h` 与 `src/archive/archive.cpp`；
> 代码里的 `archive_v01` 常量与本文的偏移表逐字段对应，改一个必须同时改另一个。

## 0. 这是打包，不是压缩

- 归档里的文件正文（payload）是**原始字节**：不压缩、不编码、不转码、不 base64、不加密。
- 源文件里的 `ABCDEF0123456789` 在归档中仍然是这 16 个字节。
- 归档只会比原始内容**大**（多了全局 header、每条 entry 的 header 和路径），绝不会更小。
- 因此本项目不使用也没有实现 Huffman / zlib / gzip / deflate / zstd / lz4 / bzip2 等任何压缩算法，
  也不使用 tar / zip / 7z / libarchive 等任何第三方打包库：header、entry、路径、payload
  全部由本仓库的 C++ 代码自己写、自己读。

测试 `scripts/test.sh` 的 NOCMP 区用三条断言守住这件事：
源文件里的 `UNCOMPRESSED_ARCHIVE_PAYLOAD_0123456789` 必须能在归档里原样搜到；
归档字节数 ≥ payload 总字节数；1 MiB 全零文件的归档必须比文件本身更大。

## 1. 编码约定

- 所有整数字段：**little-endian**，固定宽度。
- 不使用 `write(fd, &struct, sizeof(struct))`：结构体布局受 padding、对齐和主机字节序影响，
  换个编译器就会变。写入端逐字节拼缓冲区（`AppendU16LE` / `AppendU32LE` / `AppendU64LE`），
  读取端逐字节还原（`ReadU16LE` / `ReadU32LE` / `ReadU64LE`），字段不完整即失败。
- 路径：UTF-8 字节串，不带结尾 NUL，长度由 `path_length` 给出。
- 字节序无关的文件名内容不做任何规范化：中文、空格、`#`、`%` 原样保存。

## 2. 全局 header（固定 24 字节）

| offset | size | field | 说明 |
| --- | --- | --- | --- |
| 0 | 8 | `magic` | `42 4B 50 41 52 43 48 00`，即 `"BKPARCH\0"` |
| 8 | 2 | `version` | uint16，v0.1 固定为 `1`，其它值拒绝 |
| 10 | 2 | `flags` | uint16，v0.1 固定为 `0`，其它值拒绝 |
| 12 | 4 | `header_size` | uint32，固定为 `24`，其它值拒绝 |
| 16 | 8 | `entry_count` | uint64，entry 条数 |

`flags` 是留给后续版本的显式扩展位；v0.1 只接受 0，读到别的值直接失败，
而不是猜测新版本的含义。

写入顺序：先写 `entry_count = 0` 占位 → 写完所有 entry → 最后回到 offset 16 回填真实条数。
这样不需要为了数一遍而预扫整棵目录树，也不需要把结果攒在内存里。

## 3. Entry header（每条固定 32 字节）

| offset | size | field | 说明 |
| --- | --- | --- | --- |
| 0 | 1 | `type` | `1` = directory，`2` = regular file，其它值拒绝 |
| 1 | 1 | `reserved0` | 必须为 0 |
| 2 | 2 | `reserved1` | 必须为 0 |
| 4 | 4 | `path_length` | uint32，`1 ≤ path_length ≤ 4096`，超出拒绝 |
| 8 | 4 | `mode` | uint32，只有低 9 位有效（`st_mode & 0777`），高位非 0 拒绝 |
| 12 | 4 | `mtime_nsec` | uint32，`0 ≤ nsec ≤ 999999999`，超出拒绝 |
| 16 | 8 | `mtime_sec` | **有符号** int64（按补码写成 8 字节 LE） |
| 24 | 8 | `payload_size` | uint64；directory 必须为 0，regular file 为文件字节数 |

## 4. Entry 布局

```text
[32 字节 entry header][path_length 字节 path][payload_size 字节 payload]
```

- directory：没有 payload。
- regular file：payload 就是源文件的原始内容，逐字节照抄。
- 条目之间没有对齐填充：上一条的 payload 结束处就是下一条 header 的起点。

## 5. 路径语义

- 全部是**相对于 source root** 的相对路径，分隔符固定 `/`（不是 `\`）。
- 第一条 entry 必须是 source root 本身：`path = "."`、`type = directory`、`payload_size = 0`。
  它存在的意义是把源目录自己的 mode / mtime 也保存下来。
- `"."` 只允许出现在第一条，且必须是目录。
- 其它路径一律拒绝下列形态：
  - 绝对路径（`/` 开头）；
  - 空 component（`foo//bar`）、`.` component（`foo/./bar`）、`..` component（`../escape`）；
  - 结尾斜杠（`foo/`）；
  - 空路径；
  - 含 NUL 字节；
  - 含反斜杠；
  - Windows 盘符形式（`C:\...`）。v0.1 是 Linux/POSIX 格式，不定义 Windows 路径语义。
- 同一个归档路径不允许出现两次（重复路径直接判无效）。
- 不允许"某路径已经是普通文件，后面又把它当父目录"：例如 `a` 是文件、`a/b` 是条目，直接拒绝。
- 每条路径的父目录必须已经作为目录 entry 出现过（DFS 先序，父先于子）。

## 6. 保存的元数据（Metadata v0.1）

| 项目 | 是否保存 | 说明 |
| --- | --- | --- |
| 相对路径 | 是 | 见第 5 节 |
| 条目类型 | 是 | directory / regular file |
| mode | 是 | 仅 `st_mode & 0777` |
| mtime 秒 | 是 | int64 |
| mtime 纳秒 | 是 | uint32 |
| 文件大小 | 是 | `payload_size` |
| uid / gid | 否 | v0.1 不保存属主 |
| ACL / xattr | 否 | — |
| birth time / ctime | 否 | — |
| setuid / setgid / sticky | 否 | 只留 0777 这 9 位 |
| atime | 否 | 解包时用 `UTIME_OMIT`，不去动它 |

## 7. 限制

- 路径长度 ≤ 4096 字节（与 Linux PATH_MAX 同量级）。
- `entry_count` 为 uint64；解析时按实际文件大小逐条校验，不做基于 count 的预分配。
- `payload_size` 为 uint64；判断 payload 是否越界时用减法（`payload_size > file_size - position`），
  避免 `position + payload_size` 溢出。
- payload 全程流式处理（64 KiB 固定缓冲），不把文件或整个归档读进内存。

## 8. Reader 的校验规则（preflight）

解包分两阶段：先把整个归档校验完，再动磁盘。preflight 至少要确认：

1. magic、version、flags、header_size 正确；
2. 每条 entry header 完整（不够 32 字节即失败）；
3. `type` 合法，`reserved0` / `reserved1` 为 0；
4. `path_length` 在 `[1, 4096]`，path 字节完整；
5. 路径合法（第 5 节全部规则，含 traversal、重复、父子冲突）；
6. directory 的 `payload_size == 0`；
7. `mtime_nsec ≤ 999999999`，`mode` 不含权限位以外的位；
8. payload 完整落在文件内，且不溢出；
9. 读满 `entry_count` 条之后位置**正好等于文件末尾**：v0.1 不接受 trailing bytes，
   也不"忽略尾巴"；
10. 第一条 entry 必须是 `.` + directory。

任何一条不满足都返回失败，并且此时 destination 还没有被创建过——
包括 destination 的父目录也不会被创建。

## 9. 安全规则（path traversal）

虽然这不是 ZIP，但同一类漏洞必须防：

- 归档里的 `../../outside.txt` 绝不允许落到 destination 之外；
- 绝对路径、`.` / `..` component、空 component、NUL、重复路径、文件当父目录，
  全部在 preflight 阶段判失败；
- 因此所有路径拼接都只发生在 destination 之内，且不需要 `..` 归一化；
- destination 只允许"不存在"或"存在但是空目录"；是普通文件、非空目录、软链接、特殊文件时拒绝。

写入侧有对称的一条规则：归档文件不能等于源目录，也不能落在源目录里面，
否则扫描过程中归档自己会成为输入树的一部分。该检查在创建任何文件之前完成，
非法拓扑保证 0 文件系统改动。

## 10. 不支持的文件类型

v0.1 只支持普通目录和普通文件。源目录里出现下列任何一种，**整次打包失败**：

- 软链接（不跟随、不当普通文件复制、也不跳过）；
- FIFO；
- unix socket；
- 块设备 / 字符设备；
- 其它非目录非普通文件的类型。

失败信息形如 `Unsupported source entry type: <路径>`。
这是刻意的选择：静默跳过会让"备份成功"变成假话，而硬盘上的备份恰恰不能是假话。

## 11. 解包时的写入顺序

1. 先建目录，但一律用 `0700` 这类可写权限：归档里可能是 `0555` 的目录，
   先设成最终权限会让后面的子文件写不进去；
2. 写普通文件：流式复制 payload，close 之后再 `chmod`、`utimensat`；
3. 最后统一恢复目录的 mode 与 mtime，按路径深度**从深到浅**，
   因为创建子项会改变父目录的 mtime；source root（`"."`）自然排在最后。

## 12. 在备份流水线中的位置

```text
源目录 ──► ArchiveWriter ──► 未压缩归档文件 ──► ArchiveReader ──► 恢复目录
                                   ▲
                     （未来：Compression 作为独立一层接在这里）
```

- 本文件描述的是中间那一层的格式。`ArchiveWriter` / `ArchiveReader` 不依赖
  BackupEngine、CLI 或 GUI，只认路径参数，可以单独测试、单独复用；
- **压缩、加密、过滤、增量、去重、多版本、网络都不属于 v0.1**，本仓库当前也没有实现；
- 将来接入压缩层时，`version` 与 `flags` 是现成的演进位：压缩可以做成 payload 的
  一层包装（改 flags），也可以做成整个归档之外的一道流水线工序（归档层完全不变）。
  无论哪种，v0.1 的归档仍然是压缩层的输入，格式本身不需要推翻重来。

## 13. 已知限制

- 不做跨平台归档：v0.1 只定义 Linux/POSIX 语义（mode、mtime、`/` 分隔符）。
- 不抵抗"另一个本地恶意进程在 restore 过程中主动制造 TOCTOU 竞争"：
  当前实现沿用项目已有的安全级别（先检查后写入，路径解析与写入之间存在理论窗口），
  没有使用 `openat2` / `dirfd` 沙箱 / `chroot` 这类手段。
- 打包过程中如果源文件被并发修改：写入端按初始 `stat` 的字节数读取，读不满即失败；
  写完后再 `fstat` 一次，size 变化也失败。v0.1 不做快照，只是不假装成功。
- 归档文件本身没有校验和（没有 content hash / checksum 系统）。

## 14. 版本演进规则

- 任何字段语义变化都要提升全局 header 的 `version`；
- 读侧遇到不认识的 `version` 或 `flags` 一律拒绝，而不是尽力解析；
- `header_size` 用于将来扩展全局 header：新增字段时把它改大，读侧按它跳过未知部分，
  但 v0.1 只接受 24。

## 附：手工构造归档（测试用）

`scripts/test.sh` 里带一个很小的 python 辅助脚本 `testdata/archive_tool.py`，
按本文的偏移表手工拼字节，用来构造 BAD / SEC 区的损坏样本（CLI 不可能产出这种归档）。
它只在测试里使用，也是本文档与实现是否一致的一次独立复核。
