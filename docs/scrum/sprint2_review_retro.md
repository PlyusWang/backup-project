# Sprint 2 Review & Retrospective

> Sprint：Sprint 2  
> 目标版本：Archive v0.1 + Metadata v0.1 + 桌面 GUI  
> 基线：PR #8 合并后的 `main`

---

## 1. Sprint Goal

在 Sprint 1 的本地 Backup / Restore 核心上，完成课程可演示的“单文件备份包”：

```text
源目录
→ 自定义 Archive v0.1
→ 一个 .bak 文件
→ Restore
→ 恢复目录
```

同时加入基础 metadata 和可操作的桌面 GUI。

---

## 2. Sprint Review

### 2.1 已完成：Archive Format v0.1

归档由项目自己的 C++ 实现读写，不调用 tar / zip / libarchive。

格式包括：

- 24 B Global Header；
- 32 B Entry Header；
- 相对路径；
- type；
- mode；
- mtime；
- payload size；
- 原始 payload。

payload 不压缩、不加密。

### 2.2 已完成：Metadata v0.1

当前保存并恢复：

- type；
- `mode & 0777`；
- `mtime` 秒；
- `mtime` 纳秒；
- file size。

明确没有在本 Sprint 声称支持：

- UID / GID；
- ACL；
- xattr；
- 特殊权限位；
- 特殊文件。

### 2.3 已完成：Restore Preflight

恢复前完整验证归档结构，包括：

- magic / version / flags；
- Header 长度；
- Entry 类型；
- reserved 字段；
- path length；
- 非法路径；
- duplicate path；
- file-as-parent 冲突；
- payload 边界；
- entry count；
- trailing bytes；
- root entry。

preflight 不通过时不创建 destination。

### 2.4 已完成：两套 GUI

- PR #6：Qt Widgets GUI；
- PR #7：Qt Quick / QML 现代 GUI。

两套 GUI 都直接调用同一 `BackupEngine`，没有通过 subprocess 调 CLI，也没有复制第二套 backup / restore 逻辑。

### 2.5 已完成：单文件 Archive 全链路

PR #8 将 CLI 与两套 GUI 全部切换到：

```text
backup <source_directory> <backup_file>
restore <backup_file> <destination_directory>
```

归档推荐使用 `.bak` 扩展名，但格式识别依赖 magic / version，而不是文件名。

### 2.6 质量结果

Sprint 2 收口时已记录：

- `scripts/test.sh`：159 PASS / 0 FAIL；
- `scripts/modern_gui_check.sh`：14 PASS / 0 FAIL；
- lint 通过；
- CLI / Widgets / Modern 构建 0 warning；
- ASan + UBSan 构建和恶意归档样本验证通过；
- 33 个手工损坏 / 恶意归档样本无 sanitizer 报告和崩溃；
- Wayland 下实际 backup / restore + `diff -r` 通过。

### 2.7 关键 PR

- **PR #6**：Qt Widgets GUI。
- **PR #7**：Qt Quick / QML GUI。
- **PR #8**：Archive Format v0.1 + Metadata v0.1 + CLI / GUI 全链路接入。

PR #8 merge 后的版本作为 Sprint 2 代表性冻结节点。

---

## 3. Sprint Retrospective

### 3.1 做得较好的地方

**Archive 与 Application 分层清楚。**  
`BackupEngine` 只做参数检查和编排；`ArchiveWriter / ArchiveReader` 负责格式和文件系统落盘细节。后续 Compression / Filter 可以继续作为独立层扩展。

**恢复端采用 preflight 再写盘。**  
坏归档不会在磁盘上留下一个看似成功但不可用的半恢复目录，这一设计直接提高了可解释性与安全性。

**没有为了 GUI 快速交付复制业务逻辑。**  
CLI、Widgets、QML 三个入口共享同一 Core，后续功能不需要维护三套 backup 逻辑。

**测试规模随风险一起增长。**  
从 Sprint 1 的 39 项扩展到 159 项，并增加格式破坏、path traversal、metadata、special file、sanitizer 等验证。

### 3.2 可以改进的地方

**UML 和正式文档再次落后于代码。**  
ArchiveWriter / Reader、两套 GUI 已经成为真实架构，但类图、构件图、顺序图仍长期保留 Sprint 1 模型。该问题需要在 Sprint 2 收口时补齐。

**两套 GUI 提前增加了维护面。**  
Widgets 和 QML 并行有演示价值，但也使后续每个业务功能都需要验证两个界面。后续必须坚持“UI 只接线、逻辑只在 Core 一处实现”。

**GUI 视觉优化不能抢占核心 Sprint。**  
Modern GUI 已经达到可用，但离目标视觉仍有差距。视觉 polish 应继续作为后期任务，不阻塞 Archive / Filter / Compression 等核心功能。

**真正 CI 尚未进入 main。**  
当前已经有自动测试脚本和回归流程，但仍需要 GitHub Actions 等持续集成环境，让 PR 自动执行核心检查。

### 3.3 下一 Sprint 的改进动作

1. 收口 Sprint 1+2 的 UML、Requirements、Architecture 和 Testing 文档。
2. 将 Filter 设计为 Archive 之前的独立层，不把匹配规则塞进 ArchiveWriter。
3. CLI / Widgets / QML 继续共享同一 C++ Core。
4. 新增规则语法时同时写 malformed-input 和 round-trip 测试。
5. 建立真正 CI。
6. 继续记录 deferred GUI / reliability / special file 问题，不在当前 Sprint 无限制扩 scope。

---

## 4. Sprint 2 结论

Sprint 2 已经从 Sprint 1 的“本地文件复制闭环”升级为真正可演示的：

```text
源目录
→ 自定义 .bak
→ 完整 preflight
→ 文件与基础 metadata 恢复
```

并且拥有 CLI + 两套 GUI。

功能层面的 Sprint 2 已完成；收口重点转为：

- UML 与系统设计统一；
- 正式需求 / 测试文档；
- Sprint Review / Retro 归档；
- CI；
- 然后进入下一 Sprint 的 Filter。
