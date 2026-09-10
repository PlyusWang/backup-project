# Sprint 1 质量回归测试记录

> 测试日期：2026-09-10  
> 测试对象：Sprint 1 / v0.1 基础 CLI Backup / Restore  
> 测试分支：`test/sprint1-quality-regression`  
> 自动化：GitHub Actions + `scripts/quality_test.sh`  
> CI Run：`34449945756`  
> 原始 Artifact：`sprint1-quality-evidence`  
> Artifact SHA-256：`1dcbf0db1ca638ca0551f6d392daa1d2449821315ac34b53ab4983e11eb08657`

## 1. 测试目的

本次测试在 Sprint 1 原有功能测试基础上，进一步验证以下质量属性：

- 可用性：CLI 帮助、参数错误提示、退出码是否符合约定；
- 鲁棒性：大量文件、大文件、深层目录、空目录、空格及 UTF-8 路径等边界场景；
- 稳定性：多次重复执行 Backup -> Restore 后结果是否一致；
- 健壮性：异常目录拓扑是否能够安全拒绝；
- 运行时安全：AddressSanitizer / UndefinedBehaviorSanitizer 是否发现内存或未定义行为问题。

## 2. 测试脚本规范

`scripts/quality_test.sh` 采用统一 Bash 工程脚本约定：

- `set -uo pipefail`；
- 测试逻辑函数化；
- 可调参数集中使用 `QUALITY_*` 环境变量；
- 所有 PASS / FAIL 写入统一日志；
- 潜在无限递归场景使用 `timeout` 保护；
- 每次运行产生独立 UTC 时间戳目录；
- 失败后仍继续收集后续证据，并最终返回非 0；
- 自动生成 Markdown summary、环境信息、Sanitizer 日志及专项 stderr；
- 注释比例由 `STYLE-01` 自动检查。

注释比例定义为：

```text
整行注释数 / （非空行数 - shebang）
```

本次脚本结果为：

```text
96 / 419 = 22.91%
```

满足 20%–23% 的约束。注释主要用于解释测试不变量、判定依据、边界场景原因和失败含义，不使用无意义填充注释。

## 3. 本次自动测试结果

| 测试项 | 结果 | 说明 |
| --- | --- | --- |
| STYLE-01 | PASS | 注释比例 22.91% |
| BUILD-01 | PASS | `-Wall -Wextra -Wpedantic` 下正常构建 |
| BASE-01 | PASS | Sprint 1 原有测试共 20 个 PASS 记录 |
| USE-01 | PASS | `--help` 可发现且退出码为 0 |
| USE-02 | PASS | 参数不足时退出码为 2，并给出可操作提示 |
| ROB-01 | PASS | 500 个小文件 + 16 MiB 随机二进制 + 24 层目录 + UTF-8 / 空格路径可完整 round-trip |
| STB-01 | PASS | 10 / 10 次重复 Backup -> Restore 均通过 |
| ROB-02 | FAIL | repository 位于 source 内部时发生自递归复制 |
| ROB-03 | FAIL | destination 位于 `repository/data` 内部时发生自递归复制 |
| SAN-01 | PASS | ASan + UBSan 构建成功 |
| SAN-02 | PASS | 代表性 round-trip 未发现 ASan / UBSan 错误 |

汇总：

```text
PASS  9
FAIL  2
SKIP  0
```

## 4. 已发现缺陷

### QD-S1-01：备份仓库位于源目录内部时发生自递归

复现场景：

```text
source/
├── file.txt
└── repository/
```

执行：

```text
backupctl backup source source/repository
```

当前实现先创建 `source/repository/data`，随后递归遍历 `source`。由于新建的 repository 本身已经成为 source 的子目录，CopyTree 会继续复制：

```text
repository/data/repository/data/repository/data/...
```

最终不是主动拒绝，而是依赖操作系统路径长度限制终止，并得到：

```text
File name too long
```

影响：

- 会产生大量无意义目录；
- 消耗 I/O、inode 和时间；
- 错误发生位置远离根因；
- 在不同文件系统限制下表现可能不同；
- 不符合“异常输入应合理拒绝且不能产生危险副作用”的健壮性目标。

### QD-S1-02：恢复目标位于备份数据目录内部时发生自递归

复现场景：

```text
repository/
└── data/
```

执行：

```text
backupctl restore repository repository/data/restored-inside-data
```

恢复开始后 destination 成为正在遍历的 `repository/data` 的子目录，递归过程随后不断复制刚产生的 destination：

```text
restored-inside-data/restored-inside-data/restored-inside-data/...
```

最终同样依赖 `File name too long` 被动结束。

影响与 QD-S1-01 相同，属于对称的路径包含关系缺陷。

## 5. 当前结论

Sprint 1 当前版本在正常功能、常见错误路径、较大数据量、重复执行以及 ASan / UBSan 检查方面表现正常。现有基础功能测试 20 个用例全部通过，10 次重复 round-trip 全部通过，500 个小文件和 16 MiB 二进制文件的压力场景也未发现数据损坏。

但当前版本不能据此宣称“健壮性完全通过”。自动测试已经稳定复现两个路径包含关系缺陷：Backup 和 Restore 都缺少 source / destination 与 repository/data 之间的祖先-后代关系检查。

因此本次 Sprint 1 质量回归结论为：

```text
可用性：通过当前测试
基础鲁棒性：通过当前测试
重复运行稳定性：通过当前测试
运行时内存 / UB 检查：通过当前测试
危险目录拓扑健壮性：未通过，发现 2 个缺陷
```

后续修复后，应保留 ROB-02 / ROB-03 作为永久回归测试，验证程序在任何复制动作发生前主动拒绝危险目录拓扑。
