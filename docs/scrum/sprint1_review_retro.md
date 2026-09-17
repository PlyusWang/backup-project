# Sprint 1 Review & Retrospective

> Sprint：Sprint 1  
> 目标版本：v0.1 基础 Backup / Restore  
> 本文将 Review（交付结果）与 Retrospective（过程改进）分开记录。

---

## 1. Sprint Goal

建立最小可运行的本地备份闭环：

```text
源目录
→ Backup
→ 本地备份存储
→ Restore
→ 恢复目录
```

本 Sprint 优先保证：

- 普通文件；
- 普通目录；
- 递归目录树；
- CLI；
- 基础错误处理；
- 可重复回归测试。

归档格式、基础 metadata 和 GUI 在项目滚动计划中进入下一 Sprint 收口。

---

## 2. Sprint Review

### 2.1 已完成

Sprint 1 最终形成：

- `backupctl` CLI；
- `BackupEngine`；
- POSIX `FileSystem` 封装；
- 普通文件和目录递归处理；
- 空文件 / 空目录；
- 二进制文件；
- 基础错误信息；
- source / destination 拓扑检查；
- 自动回归脚本；
- lint 与 sanitizer 检查基础。

### 2.2 关键 PR / 里程碑

- **PR #1**：建立基本 CLI backup / restore。
- **PR #4**：整理关键 C++ 注释，使实现可解释。
- **PR #5**：修复危险路径拓扑，拒绝 `destination == source` 和 destination 位于 source 内部的情况。

PR #5 合并后的冻结版本成为 Sprint 1 代表性演示节点。

### 2.3 验收结果

Sprint 1 在收口时具备：

- 可构建 CLI；
- backup / restore round-trip；
- 路径错误可识别；
- 危险递归拓扑在写盘前被拒绝；
- 39 项回归测试全部通过；
- clang-format / lint 通过；
- ASan / UBSan 基础验证通过。

### 2.4 未在 Sprint 1 完成

- 单文件 `.bak` 自定义 Archive；
- mode / mtime 持久化；
- GUI；
- Filter；
- Compression；
- Encryption。

这些没有伪装成已实现功能，而是进入后续 Sprint。

---

## 3. Sprint Retrospective

### 3.1 做得较好的地方

**先建立可工作的最小闭环。**  
项目不是先做复杂格式，而是先把真实文件系统的 backup / restore 跑通，为后续 Archive 奠定了可测试基线。

**回归测试发现了真实结构性问题。**  
路径拓扑测试暴露了 destination 位于 source 内部时的递归复制风险。问题随后在 PR #5 中被修复，并加入“先检查后写盘”的约束。

**核心文件系统操作没有依赖 shell 复制命令。**  
POSIX `lstat / open / read / write / mkdir / opendir` 等路径为后续归档和元数据处理保留了足够控制力。

### 3.2 可以改进的地方

**早期需求、UML 和代码同步不足。**  
代码快速迭代后，部分 UML 和正式文档仍停留在更早版本，后续需要把“文档同步”纳入每个 Sprint 的 Definition of Done。

**测试最初不是从第一天就覆盖危险拓扑。**  
核心闭环能运行并不等于健壮。后续 Sprint 应在功能提交前同时设计正常路径和破坏性边界测试。

**早期 backup storage 语义与最终课程交付形式仍有距离。**  
Sprint 1 的目标是核心闭环，下一 Sprint 必须尽快切换到真正单文件 Archive，而不是长期停留在目录复制模型。

### 3.3 下一 Sprint 的改进动作

1. Archive 成为独立模块，不把二进制格式塞进 `BackupEngine`。
2. Backup / Restore 的上层 API 尽量保持稳定。
3. 新功能必须带 round-trip 和错误路径测试。
4. GUI 只调用共享 Core，不复制第二套业务逻辑。
5. UML / Architecture 随真实实现更新。

---

## 4. Sprint 1 结论

Sprint 1 达成了“可工作的本地 Backup / Restore 核心”目标，并通过一次真实缺陷修复将项目从“能运行”推进到“具有基础健壮性”。

最大的后续任务不是继续扩展目录复制，而是把该闭环升级成：

```text
源目录
→ 自定义单文件 Archive
→ Restore
```

并加入基础 metadata 与 GUI。
