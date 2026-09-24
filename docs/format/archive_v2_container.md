# Archive Format v2：可组合容器

> 状态：v2，随 feature/archive-pipeline-complete-core 引入。
> 实现见 `include/pack_stream.h`、`include/container_format.h`、
> `include/archive_pipeline.h` 与对应的 src/ 文件。
> 代码里的常量与本文的偏移表逐字段对应，改一个必须同时改另一个。

## 0. 为什么要有 v2

v0.1（见 `archive_v0.1.md`）是一层完整的"打包"：全局 header + 逐条 entry header +
原样照抄的文件正文。它的边界很明确：只支持普通目录和普通文件，只保存 mode 的
0777 九位与 mtime，没有压缩、没有加密、没有 uid/gid。

v2 不是推翻 v0.1，而是在它旁边加一条**可组合的流水线**：

```text
source tree
   ↓  metadata + filter + special-file scan   （TreeScanner）
pack
   ├─ MyPack v2（BKPARCH\0 + version 2）
   ├─ USTAR（POSIX tar，baseline）
   └─ Fast USTAR（同样的 wire format，只改 I/O 策略）
   ↓  compression
   ├─ None
   ├─ Canonical Huffman（HUF1）
   └─ LZSS + Canonical Huffman（LZH1）
   ↓  encryption
   ├─ None
   ├─ DES-CBC + HMAC-SHA256（legacy / educational）
   └─ AES-256-CTR + HMAC-SHA256
   ↓
v2 container（magic BKPCNT2\0）→ .bak
```

恢复是它的逆序，其中**认证必须在解密之前**：

```text
.bak → container header 校验 → HMAC/认证 → 解密 → 解压 → unpack
     → metadata + 特殊文件恢复（暂存目录）→ rename 成 destination
```

三层各自只认字节：压缩层不知道路径和 uid，加密层不知道 TAR 和 Filter。
顺序是硬性的 `PACK → COMPRESS → ENCRYPT`——反过来先加密再压缩没有意义。

v0.1 的读写实现（`archive.cpp`）在 v2 里**一行都没改**，旧 `.bak` 的 list /
restore / delete 继续工作。

## 1. 编码约定

- 所有整数字段：**little-endian**，固定宽度。
- 不使用 `write(fd, &struct, sizeof(struct))`：结构体布局受 padding、对齐和主机
  字节序影响。写侧逐字节拼缓冲区，读侧逐字节还原，字段不完整即失败。
- 路径与链接目标：UTF-8 字节串，不带结尾 NUL，长度由长度字段给出。
- 保留字段必须真的为 0：读侧不"反正没人用"地放过它们。
- 不认识的 version / flags / 算法 id 一律拒绝，不做"尽力解析"。

## 2. 统一条目模型

三种 pack 后端消费同一份 `std::vector<ArchiveEntry>`（见 `include/archive_entry.h`），
由 `TreeScanner`（见 `include/tree_scanner.h`）一次扫描产出。条目类型：

| 值 | 含义 | 归档里有 payload |
| --- | --- | --- |
| 1 | directory | 否 |
| 2 | regular file | 是 |
| 3 | symbolic link | 否（有 link target） |
| 4 | hard link | 否（link target 是归档内第一次出现的 path） |
| 5 | FIFO | 否 |
| 6 | character device | 否（有 major/minor） |
| 7 | block device | 否（有 major/minor） |

socket **不在**归档格式里：遇到会进入归档的 socket，整次备份明确失败；只有被
Filter 明确排除时才跳过。静默跳过、跟随它、把它当普通文件复制这三种做法都会让
"备份成功"变成假话。

每条条目保存：archive_path、mode（**07777**，含 setuid/setgid/sticky）、uid、gid、
mtime 秒 + 纳秒、size、link target（若适用）、device major/minor（若适用）。

## 3. MyPack v2

magic 与 v0.1 相同（`BKPARCH\0`），靠 `version` 分流：

```text
version 1 → legacy v0.1（archive.cpp，未改动）
version 2 → 本节描述的扩展格式
other     → reject
```

### 3.1 全局 header（固定 32 字节）

| offset | size | field | 说明 |
| --- | --- | --- | --- |
| 0 | 8 | `magic` | `"BKPARCH\\0"` |
| 8 | 2 | `version` | 固定 2 |
| 10 | 2 | `flags` | 固定 0，其它值拒绝 |
| 12 | 4 | `header_size` | 固定 32 |
| 16 | 8 | `entry_count` | 条目数 |
| 24 | 8 | `reserved` | 必须全 0 |

### 3.2 Entry header（每条固定 64 字节）

| offset | size | field | 说明 |
| --- | --- | --- | --- |
| 0 | 1 | `type` | 1..7，其它值拒绝 |
| 1 | 1 | `flags` | 必须为 0 |
| 2 | 2 | `reserved0` | 必须为 0 |
| 4 | 4 | `path_length` | `1 ≤ len ≤ 4096` |
| 8 | 4 | `link_length` | `0 ≤ len ≤ 4096` |
| 12 | 4 | `mode` | 07777，高位非 0 拒绝 |
| 16 | 4 | `uid` | |
| 20 | 4 | `gid` | |
| 24 | 4 | `mtime_nsec` | `≤ 999999999` |
| 28 | 8 | `mtime_sec` | **有符号** int64（补码按 LE 写） |
| 36 | 8 | `payload_size` | 只有普通文件非 0 |
| 44 | 4 | `dev_major` | 字符/块设备 |
| 48 | 4 | `dev_minor` | 字符/块设备 |
| 52 | 12 | `reserved` | 必须全 0 |

### 3.3 Entry 布局

```text
[64 字节 entry header][path_length 字节 path][link_length 字节 link][payload]
```

类型与其它字段的自洽性是**格式规则**，读侧必须检查：

- directory / FIFO / char / block：link_length == 0 且 payload_size == 0；
- symlink / hardlink：link_length > 0 且 payload_size == 0；
- regular file：link_length == 0。

### 3.4 路径语义

与 v0.1 完全一致（见 `archive_v0.1.md` 第 5 节），由 `archive_path.cpp` 里**唯一一份**
实现裁决：相对路径、`/` 分隔、拒绝绝对路径 / 空 component / `.` / `..` / 结尾斜杠 /
NUL / 反斜杠 / Windows 盘符；同一条路径不得重复；父目录必须先以目录身份出现；
第一条必须是 `.` 且是目录。写侧与读侧共用同一份规则，保证"写得出"蕴含"读得回"。

### 3.5 读侧 preflight

读满 `entry_count` 条之后位置必须**正好等于文件末尾**——不接受 trailing bytes。
硬链接目标必须存在，而且必须是一条普通文件条目（指向目录、软链接或另一条硬链接
都是坏数据）。所有越界判断用减法，避免 `offset + size` 溢出。

## 4. USTAR（POSIX tar）

`PackMethod::kUstar` 与 `PackMethod::kFastUstar` 的 wire format **都是标准 USTAR**，
区别只在 syscall 数量、缓冲大小与数据搬运方式：

- baseline：64 KiB 输出缓冲，header / padding / payload 各自 write，清晰易读；
- Fast：约 1 MiB 统一输出缓冲，把 header、payload、padding 聚合进同一个缓冲再一次
  write，读 payload 用大块 read，对输入 fd 调 `POSIX_FADV_SEQUENTIAL`。

两者共享同一份 header codec / 路径拆分 / checksum / typeflag 映射，不复制格式逻辑，
所以同一棵树的输出**逐字节相同**（benchmark 里有断言）。

- 512 字节 block；name 100 / prefix 155 / linkname 100；数字字段是八进制 ASCII，
  7 位或 11 位数字 + 1 个 NUL；checksum 计算时把 chksum 字段当 8 个空格。
- typeflag：`'0'`/`'\\0'` 普通文件、`'1'` 硬链接、`'2'` 软链接、`'3'` 字符设备、
  `'4'` 块设备、`'5'` 目录、`'6'` FIFO。socket 没有 typeflag，遇到 `'s'` 明确拒绝。
- 装不下的值（超长路径、uid/gid/size 溢出八进制字段、linkname > 100 字节）一律
  **明确失败，绝不截断**；本实现不采用 GNU base-256 扩展。
- 归档结尾是两个全零 block；其后**非零**字节一律拒绝（GNU tar 会按 10240 字节记录
  对齐补零，这种纯零填充允许）。
- 读侧容忍 GNU 写出的 `./` 前缀与目录名结尾 `/`。

## 5. 压缩

### 5.1 HUF1（Canonical Huffman）

```text
0    4   magic "HUF1"
4    8   uint64 original_size
12   256 每个符号的 code length（0 表示不出现）
268  8   uint64 bit_count（有效 bit 数，不是字节数）
276  ... bitstream，每个字节从最高位开始填
```

流长度必须**正好**是 `276 + ceil(bit_count / 8)`，多一个字节或少一个字节都是坏流。
空输入是 276 字节纯头部；只有一种符号时该符号 code length 固定为 1（不能是 0 bit）；
码长上限 32 bit，超限用"加深最小频率叶子"的 Kraft 修正压回去。

### 5.2 LZH1（LZSS + 上面的 HUF1）

```text
0    4   magic "LZH1"
4    8   uint64 original_size
12   8   uint64 token_stream_size
20   ... 一个完整的 HUF1 流（它自己的 original_size 必须等于 token_stream_size）
```

token stream：每 8 个 token 前 1 个 control byte，bit 从最高位开始。
bit = 1 是 literal（后跟 1 字节），bit = 0 是 match（后跟 `uint16 distance` LE +
`uint8 length_minus_3`）。distance ∈ [1, 32768]，length ∈ [3, 258]。
窗口 32768、最短匹配 3、最长匹配 258；编码器用 3 字节 hash + 链式最近位置表，
候选上限 128，不做逐字节全窗口扫描。解码必须支持 overlap copy（distance = 1 的
长重复串）。

同一输入的压缩输出必须 **byte-for-byte 相同**：code length 用 (frequency, 子树最小
符号) 的全序生成，不序列化指针树，不依赖任何无序容器的迭代顺序。

## 6. v2 外层容器

### 6.1 header（固定 160 字节）

| offset | size | field | 说明 |
| --- | --- | --- | --- |
| 0 | 8 | `magic` | `"BKPCNT2\\0"` |
| 8 | 2 | `version` | 固定 2 |
| 10 | 2 | `header_size` | 固定 160 |
| 12 | 1 | `pack_method` | 0 MyPack / 1 USTAR / 2 FastUSTAR |
| 13 | 1 | `compression_method` | 0 None / 1 Huffman / 2 LZSS-Huffman |
| 14 | 1 | `encryption_method` | 0 None / 1 DES-CBC-HMAC / 2 AES-256-CTR-HMAC |
| 15 | 1 | `flags` | 固定 0 |
| 16 | 8 | `entry_count` | |
| 24 | 8 | `packed_size` | pack 完成、尚未压缩 |
| 32 | 8 | `compressed_size` | 压缩完成、尚未加密 |
| 40 | 8 | `payload_size` | 最终写进容器的 payload 长度 |
| 48 | 4 | `kdf_iterations` | PBKDF2 轮数 |
| 52 | 1 | `salt_len` | |
| 53 | 1 | `iv_len` | |
| 54 | 1 | `tag_len` | |
| 55 | 1 | `reserved0` | 必须为 0 |
| 56 | 16 | `salt` | 未使用部分必须为 0 |
| 72 | 16 | `iv` | 未使用部分必须为 0 |
| 88 | 32 | `auth_tag` | HMAC-SHA256；未使用部分必须为 0 |
| 120 | 32 | `payload_sha256` | 最终存储 payload 的 SHA-256 |
| 152 | 8 | `reserved` | 必须全 0 |
| 160 | ... | payload | 之后不允许有任何多余字节 |

### 6.2 size 语义与自洽性

- 不压缩时 `compressed_size == packed_size`；
- 不加密或 AES-CTR 时 `payload_size == compressed_size`（流密码不改变长度）；
- DES-CBC 因为 PKCS#7 一定补 1..8 字节，`payload_size == (compressed_size / 8 + 1) * 8`；
- 任何其它组合都是坏数据，直接拒绝。

### 6.3 加密参数

| 算法 | kdf_iterations | salt_len | iv_len | tag_len |
| --- | --- | --- | --- | --- |
| None | 0 | 0 | 0 | 0 |
| DES-CBC + HMAC-SHA256 | 200000 | 16 | 8 | 32 |
| AES-256-CTR + HMAC-SHA256 | 200000 | 16 | 16 | 32 |

不加密却带着 KDF 参数、或者算法与长度字段对不上，一律拒绝。

### 6.4 密钥派生与分离

`PBKDF2-HMAC-SHA256(password, salt, kdf_iterations, 64)`：

- AES：前 32 字节 → AES-256 密钥，后 32 字节 → HMAC 密钥；
- DES：前 8 字节 → DES 密钥，后 32 字节 → HMAC 密钥。

salt 与 IV 都来自 OS CSPRNG（`getrandom()`，失败回退 `/dev/urandom`），
绝不用 `rand()` / `mt19937`。密码不写 config、不写日志、不回显、不以明文进归档。

### 6.5 Encrypt-then-MAC

HMAC 的输入是 `normalized header + ciphertext payload`，其中 normalized header
把 `auth_tag[32]` 视为全 0，**其它字段全部参与**（包括 `payload_sha256`、
三个 size、salt、IV、三个 method id）。

这有一个实现上的直接后果：`payload_sha256` 必须**先定下来**，MAC 才能算。
所以写侧的顺序是"写 header（tag 与 sha 先占位）→ 流式加密并同时算 SHA →
回填 payload_sha256 → 再读一遍密文算 HMAC → 回填 auth_tag"。

恢复侧严格分两遍：

1. **只读认证**：算 payload 的 SHA-256 与 HMAC，与 header 比对。HMAC 用
   constant-time 比较；不通过就报 "Authentication failed"，此时一个字节都还没写过。
2. **解密**：认证通过之后才解密。

因此 wrong password 一定失败在认证这一关，而不是靠 PKCS#7 padding 校验失败才发现。

`payload_sha256` 是**最终存储 payload**（加密时就是密文）的 SHA-256：
不加密时用于发现意外损坏；加密时它仍然保留，但认证靠 HMAC，SHA-256 本身不是认证。

## 7. 恢复的写入顺序与 failure atomicity

新流水线**不直接往 destination 里写**：

1. 先做完整 preflight：容器 header、认证、解密、解压、packed 流的全部结构校验。
   任何一步失败，destination 都还不存在。
2. 在 destination 的兄弟位置建一个唯一的暂存目录
   （`<destination>.bptmp-<pid>-<pid>`，同文件系统）。
3. 逐条恢复：目录先用 `0700` 建出来；普通文件流式写 payload；软链接 `symlink()`；
   FIFO `mkfifo()`；字符/块设备 `mknod()`（非 root 会明确失败并说明需要 CAP_MKNOD，
   不静默降级成普通文件）；硬链接 `link()`，目标还没出现就先挂起、主循环结束后
   按拓扑顺序补。
4. metadata 收尾，顺序是 **ownership → mode → mtime**：
   - 目录排在最后、按深度**从深到浅**（创建子项会改父目录的 mtime）；
   - 软链接用 `lchown` + `utimensat(AT_SYMLINK_NOFOLLOW)`，**绝不 chmod 软链接**
     （chmod 会跟随链接去改别人的目标）；
   - 硬链接与目标共享 inode，metadata 由第一次出现的普通文件负责；
   - atime 用 `UTIME_OMIT`：恢复不该顺手改掉"上次访问时间"。
5. 全部成功之后才 `rename(staging, destination)`。失败则删掉暂存目录，
   destination 保持不存在。

**ownership 的现实边界**：归档精确保存 uid/gid；恢复时 root 能精确还原，非 root
进程不被允许把文件改成任意属主。这时的策略是**尽力而为 + 如实记录**：
`RestoreReport::skipped_ownership` 计数 + `notes` 里写清楚哪条路径没还原属主，
既不因此让整次普通恢复不可用，也不假装已经完整恢复。

## 8. 临时文件

流水线的中间产物（packed 流、compressed 流）都放在目标文件/目标目录所在的目录里，
名字由 `mkstemp` 生成（含 pid 与随机后缀），由 RAII 守卫保证**成功或失败都清理**。
不使用固定的 `/tmp/foo.tmp`，也不把整条归档一次读进内存。

内存边界要说清楚：pack 与 encrypt 全程流式（256 KiB 缓冲）；**压缩层例外**——
HUF1 / LZH1 是"一个 header + 一条 bitstream"的格式，频次表必须看过全部输入才能确定，
所以这一层需要 O(n) 内存，当前实现给它设了 1 GiB / 条流的明确上限，
超过就报错而不是 OOM。

## 9. 安全措辞与边界

- DES-CBC 明确是 **legacy / educational**：56 bit 密钥按今天的标准远远不够，
  保留它只是为了讲清楚分组密码、CBC 链与 PKCS#7。
- AES-256-CTR + HMAC-SHA256 + PBKDF2-HMAC-SHA256 + random salt/IV +
  Encrypt-then-MAC 是本项目能给出的最强组合。
- SHA-256 / HMAC-SHA256 / PBKDF2 / DES / AES 全部是本仓库手写的：不链接 OpenSSL、
  libsodium、libcrypto 或任何第三方密码学库。
- **但这是课程项目的手写实现，未经专业密码学审计**。真正面向公网/云的产品应当
  换成经过审计的成熟库。
- 不抵抗"另一个本地恶意进程在 restore 过程中主动制造 TOCTOU 竞争"：当前沿用
  项目已有的安全级别（先检查后写入），没有使用 `openat2` / `dirfd` 沙箱。
- 不保存 ACL / xattr / SELinux label / capabilities / birth time / ctime——不在本轮范围。

## 10. 与 v0.1 的兼容

- `.bak` 的格式判断只看 magic，不看扩展名：
  `BKPARCH\0`（version 1）走 legacy reader，`BKPCNT2\0` 走 v2 流水线。
- 不显式选择 v2 的调用方（旧 CLI、旧 GUI、旧 API）行为一字不变。
- `ArchiveReader::InspectHeader` 同时认识两种 .bak：v2 容器的
  `format_version` 报 2，`entry_count` 不需要密码就能读到。
- 每个 pack 后端都回写 entry_count，因此"备份列表"不需要解密就能显示条目数。

## 11. 已知限制

- USTAR 的 mtime 只有秒级精度（12 字节八进制秒，格式本身没有纳秒位）；
  需要纳秒精度时用 MyPack v2。
- USTAR 的路径上限是 name 100 + prefix 155 = 255 字节（含 `/`）；
  更长的路径必须换 MyPack v2 或明确失败。
- 压缩层需要 O(n) 内存（见第 8 节）。
- 同一输入在 `PackMethod::kUstar` 与 `kFastUstar` 下产出**逐字节相同**的归档；
  两个 id 只表达"用哪条 I/O 策略写"，恢复时走同一个 reader。
