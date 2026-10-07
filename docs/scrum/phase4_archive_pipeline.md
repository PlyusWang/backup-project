# Phase 4 Review & Retrospective：Archive v2 流水线与产品 CLI / GUI 收口

> 覆盖阶段：2026-09-26（单日连续合并两个 PR）
> 对应 PR：#15（feature/archive-pipeline-complete-core）、#16（feature/backup-options-gui）
> 收口基线：PR #16 合并后的 `main`（2fe004b）
> 这一阶段是原 Gantt 中的 S5--S7，实际在同一轮里完成。

---

## 1. 阶段 Goal

Sprint 2 的 Archive v0.1 只做了"打包"一件事：全局 header + 逐条 entry header +
原样照抄的文件正文，没有压缩、没有加密、只保存 `mode & 0777` 与 mtime。
这一阶段要把它升级成一条**可组合的流水线**，同时不破坏旧归档：

```text
source tree
   → metadata + filter + special-file scan
   → PACK        （mypack v2 / ustar / fast-ustar）
   → COMPRESS    （none / canonical Huffman / LZSS + Huffman）
   → ENCRYPT     （none / AES-256-CTR + HMAC-SHA256 / DES-CBC + HMAC-SHA256）
   → v2 容器 BKPCNT2 → .bak
```

配套目标是：CLI 与 Modern GUI 都能选打包 / 压缩 / 加密算法，受密码保护的归档在
托管恢复流程里能正常工作；v0.1 归档继续可读。

---

## 2. Review

### 2.1 容器与格式

- 外层容器 magic 为 `BKPCNT2`，编码约定写在
  `docs/format/archive_v2_container.md`：所有整数字段 little-endian、固定宽度；
  不使用 `write(fd, &struct, sizeof(struct))`；保留字段必须真的为 0；
  不认识的 version / flags / 算法 id 一律拒绝，不做"尽力解析"。
- 三层的顺序是硬性的 `PACK → COMPRESS → ENCRYPT`，恢复是逆序，其中
  **认证必须在解密之前**：先校验容器头与 HMAC，再解密，再解压，最后 unpack。
- v0.1 的读写实现（`src/archive/archive.cpp`）在 v2 里**一行都没改**，
  旧的 list / restore / delete 继续工作；`docs/format/archive_v0.1.md` 保留，
  并注明它是 legacy 只读兼容。

### 2.2 打包层

| 后端 | 说明 |
| --- | --- |
| MyPack v2 | 项目自有格式（`BKPARCH\0` + version 2），支持特殊文件（symlink / fifo / char / block / socket）与硬链接身份 |
| USTAR | POSIX tar 的基线实现，用于对照与互操作 |
| Fast USTAR | 与 USTAR 相同的 wire format，只改 I/O 策略 |

三个后端由同一组测试断言**输出一致且备份可复现**。

### 2.3 压缩层（原计划 S6，本轮提前完成）

- 手写 **Canonical Huffman**（`HUF1`）与 **LZSS + Canonical Huffman**（`LZH1`），
  实现在 `src/compression/`，不引入 zlib 等第三方库；
- 管线改成**流式、内存有界**（`fix: stream compression pipeline with bounded
  memory`），测试里加了 1 MiB 与 10 MiB 的打包基准；
- 读侧拒绝**非规范的压缩填充**（`fix: reject noncanonical compression padding`），
  而不是容忍后照常解压。

### 2.4 加密层（原计划 S7，本轮提前完成）

- 手写原语：SHA-256 / HMAC / HKDF / AES / DES / PBKDF2 / 随机数，全部在
  `src/crypto/`，没有链接第三方密码学库；
- 两种模式：`aes-256-ctr-hmac-sha256`（产品默认选择）与
  `des-cbc-hmac-sha256`（教学 / legacy 对照，文档里如实写明它今天的定位）；
- 密码从 `/dev/tty` 读取，不走命令行参数（`include/terminal_secret.h`）；
- Modern GUI 支持选择备份选项与**受密码保护的托管恢复**，提交后清除内存中的密码。

### 2.5 失败路径与发布

- 临时数据受保护，归档**原子发布**：先写临时文件，校验通过后 rename；
  原子发布不可用时**直接失败**，不降级成"直接写目标文件"
  （`fix: fail closed when atomic archive publication is unavailable`）；
- 资源与完整性回归单独一套（`test: cover resource and integrity regressions`）；
- Filter 与元数据语义在这一轮继续收紧（`type` / 属主 / 大小写等），
  并保证 legacy 路径与扩展路径结果一致。

### 2.6 验收与测试

| 套件 | 覆盖 |
| --- | --- |
| scripts/archive_pipeline_test.sh | 打包 × 压缩 × 加密全矩阵、1 MiB / 10 MiB 基准、确定性变异扫描 |
| scripts/compression_test.sh | Huffman / LZSS-Huffman 的往返与边界 |
| scripts/crypto_test.sh | 对称加密与认证、篡改检测 |
| scripts/ustar_test.sh | USTAR / Fast USTAR 的格式与一致性 |

这些套件在 2026-10-07 的收口回归中全部 `exit=0`；`archive_pipeline` 一条本身
要跑约 6 分钟，是全套里最慢的一条。

---

## 3. Retrospective

### 3.1 做得较好的地方

**分层只认字节，层与层之间不需要互相理解。**
压缩层不知道路径和 uid，加密层不知道 TAR 和 Filter，因此每一层都能单独做
round-trip 与变异测试，也能单独替换（三个打包后端就是靠这一点并存的）。

**格式文档与实现对得上。**
容器文档开头就写明"代码里的常量与本文的偏移表逐字段对应，改一个必须同时改
另一个"；后来的 `refactor(core): single source for delta layout and byte-order
readers` 进一步把重复的布局与字节序读取收敛到一处。

**不引入第三方编解码库是有代价、也有收益的选择。**
收益是格式、错误处理与测试向量完全可控（例如读侧可以要求"保留字段必须为 0"）；
代价是工作量与踩坑（canonical Huffman 的填充规范、ustar 的硬链接与源快照语义），
这些都在这一轮里实际付掉了。

**密码不进命令行、不进日志。**
密码只从 `/dev/tty` 读，GUI 提交后清空，避免它出现在 shell history 与进程表里。

### 3.2 遇到的问题与处理

**1）压缩管线最初不是流式的。**
第一版把整块数据读进内存再压缩，10 MiB 的基准立刻就暴露了内存占用问题；改成
有界内存的流式管线，并把基准留在套件里，避免以后回退。

**2）非规范输入的处理方式被明确下来。**
压缩填充、保留字段、未知算法 id 这三类输入，读侧一律拒绝而不是"按最宽松的方式
解释"；这条约定后来同样用在增量 delta 与网络协议上。

**3）ustar 的硬链接与源快照语义。**
打包期间源目录如果发生变化（文件被改写、链接目标被替换），归档可能出现"内容与
头不一致"的组合。处理方式是先做稳定的源快照，再解析硬链接
（`fix: harden ustar source snapshot and hard link resolution`）。

**4）GUI 选项面板与核心的取值必须同一份。**
算法名、默认值、合法性检查如果 GUI 各写一份，很快就会出现"界面能选、核心拒绝"。
处理方式是让 GUI 直接使用核心提供的选项定义。

**5）原子发布的失败路径容易被忽略。**
当临时目录不可用（例如同目录不可写、文件系统不支持 rename 语义）时，最危险的做法
是退回直接写目标文件——那样失败会留下半个看起来正常的归档。现在是 fail closed。

### 3.3 下一阶段的改进动作

1. 定时与实时触发都复用这一套引擎与选项，不再写第二套备份路径；
2. 增量 delta 复用 v2 容器与压缩层的约定（尤其是读侧严格性）；
3. 全矩阵测试的成本要控制：慢套件（如 archive_pipeline）只进 final gate，不进
   每次提交都要跑的快套件。

---

## 4. 阶段结论

这一阶段把归档从"一个自定义格式"变成"一条可组合、可验证的流水线"：

```text
Archive v0.1（legacy，只读兼容）
Archive v2 外层容器 BKPCNT2（产品当前恒用）
```

原计划排在 S6 / S7 的压缩与加密在这一轮里一起落地，因此后续阶段的重点转向
"什么时候触发备份"（定时 / 实时）与"备份到哪里"（网络 / 增量）。
