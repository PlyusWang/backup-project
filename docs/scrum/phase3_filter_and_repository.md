# Phase 3 Review & Retrospective：Filter 层与仓库驱动的产品形态

> 覆盖阶段：2026-09-17 -- 2026-09-25
> 对应 PR：#9（feature/file-filtering）、#12（feat/modern-gui-filter-builder）、
> #13（feature/repository-core-integration）、#14（feature/backup-management-settings）
> 收口基线：PR #14 合并后的 `main`（c640f80）
> 记录方式与 Sprint 1 / 2 相同：先写 Review（交付结果），再写 Retrospective（过程与问题）。

---

## 1. 阶段 Goal

Sprint 1 / 2 的备份是"整个源目录照单全收"。这一阶段要解决三件事：

1. 在归档之前引入一层 **Filter**：源目录 → Filter → ArchiveWriter → `.bak`，
   让用户可以按名字、路径、类型、大小、时间与属主挑出要备份的内容；
2. 让 CLI、Qt Widgets GUI、Modern QML GUI **三个入口共用同一套过滤核心**，
   并且给 Modern GUI 一个不写规则语法的可视化编辑入口；
3. 把产品形态从"一条命令给两个路径"改成**仓库驱动**：先配置仓库，归档由程序
   在仓库里命名，GUI 有备份管理页与设置页。

明确不在这一阶段：压缩、加密、增量、定时、实时、网络（其中压缩与加密在下一阶段）。

---

## 2. Review

### 2.1 Filter 核心

位置是刻意的：`include/filter.h` 里写明 "源目录 → Filter → ArchiveWriter"，Filter
只回答"这条路径要不要进备份"，不负责压缩、加密、增量或校验，也**不读文件内容**，
判断依据全部来自 `lstat` 拿到的元数据。

规则只有两个动作 `kInclude` / `kExclude`，语义是 **exclude 优先于 include**；
语法、优先级与剪枝语义写在 `docs/filter_usage.md`（含 11 个字段的完整取值表），
明确不做与后续计划写在 `docs/backlog/filter_future.md`。

字段是分两代长出来的：

| 代次 | 字段 | 说明 |
| --- | --- | --- |
| 初版（PR #9） | name / path / stem / ext / is_directory / size / mtime | 只看路径与 `lstat` 基础字段；不做创建时间、访问时间这类平台差异大的字段 |
| 扩展（PR #15 阶段） | type / uid / gid / user / group | `type` 细分为 file / folder / symlink / fifo / char / block / socket；属主用 `uid` / `gid` / `user` / `group` 四个独立字段 |

合并后一共 11 个字段：name / path / stem / ext / type / size / mtime / uid / gid /
user / group。

**旧调用方的行为被单独守住了。** `FilterEntry::type` 的默认值是
`EntryType::kRegularFile`，所以"只填 `is_directory`"的调用方在新字段加入后行为
一字不变；这条约束写在 `include/filter.h` 的注释里，并由
`scripts/legacy_filter_test.sh` 专门覆盖。

### 2.2 可视化规则编辑器（PR #12）

Modern GUI 的规则编辑器没有把匹配逻辑搬进 QML，而是加了一个**纯 C++、零 Qt** 的
中间层 `src/filter/filter_rule_builder.cpp`，它只做四件事：

1. 表单级结构校验（空值、非法日期、区间反向等）；
2. 把 `FilterClauseDraft` / `FilterRuleDraft` 序列化成现有 DSL；
3. 生成人类可读的中文摘要；
4. 拼装 `--include` / `--exclude` 形式 CLI 参数。

**语法的最终裁决仍然在 `Filter::AddRule`**：中间层的 `ValidateRule()` 就是把
生成的规则文本交给真实核心验证，所以"前端校验通过"与"后端接受"不可能给出不同答案。
Qt 桥 `ui/modern/filter_rule_model.*` 只负责 QObject / 信号槽 / QML 暴露。
放在 `src/filter/` 而不是 GUI 私有目录，是为了让 CLI、GUI 与测试都复用同一份实现。

### 2.3 仓库、配置与备份管理页（PR #13 / #14）

- `config_manager`：仓库路径等配置的读写；
- `backup_catalog`：列出、查询仓库里的归档，路径安全边界单独写清楚；
- Modern GUI 增加**备份管理页**与**设置页**，备份流程改为仓库驱动；
- `application_instance_lock`：**全应用单实例锁**——同一时刻只允许一个产品进程，
  GUI 与 CLI 共用同一把锁，并且锁与仓库路径、配置文件路径都无关（换仓库、换参数
  都不能绕过它）；底层是 `flock`，进程退出或崩溃由内核释放。它与
  `SchedulerLock` 是两个不同的问题，锁顺序固定为
  `ApplicationInstanceLock -> SchedulerLock`。

### 2.4 验收与测试

这一阶段新增的套件在收口后都进了 `scripts/final_gate.sh`：

| 套件 | 覆盖 |
| --- | --- |
| scripts/filter_semantics_test.sh | 规则语法、优先级、剪枝语义 |
| scripts/filter_metadata_test.sh | 元数据字段与 `type` / 属主取值 |
| scripts/legacy_filter_test.sh | 旧调用方（只填 `is_directory`）行为不变 |
| scripts/filter_rule_builder_test.sh | 中间层单元测试；断言每条生成的 DSL 都能被真实核心接受 |
| scripts/filter_rule_builder_int_test.sh | 端到端：builder 生成的规则驱动真实 `backupctl` 做 backup + restore，并与手写 CLI 规则逐字节比对 |

在 2026-10-07 的收口回归中，这五条以及现代 GUI 检查全部 `exit=0`（见
`docs/scrum/phase7_release_and_closeout.md`）。

### 2.5 未在这一阶段完成

- 过滤规则的可视化**编辑**（回填已有规则到表单）留到后续版本，`docs/filter_usage.md`
  末尾如实记录；
- 组合规则的嵌套/分组不在本阶段范围内（后续在 CLI 与 GUI 上做的是复合条件的表达，
  而不是任意嵌套的表达式树）。

---

## 3. Retrospective

### 3.1 做得较好的地方

**Filter 被放在归档之前，而不是塞进 ArchiveWriter。**
归档层因此始终只看到"已经决定要备份的条目"，后续加打包、压缩、加密时不需要在
归档里再判断一次规则。

**前端校验与后端校验只有一个权威。**
中间层不重新实现匹配，只生成规则文本并把它交给真实核心验证——这一条避免了
"界面说可以、核心说不行"的长期维护问题。

**元数据扩展靠默认值而不是分支。**
新增 `type` 时没有在调用点写 `if (has_type)`，而是让默认值等于旧语义，
再用一个专门套件守住它。

**规则文本成为唯一语法权威。**
CLI 的 `--include` / `--exclude`、GUI 表单、测试夹具最终都落到同一份 DSL，
不存在第二套查询语法。

### 3.2 遇到的问题与处理

**1）先校验路径、后做过滤，会把被排除的东西也变成错误。**
早期实现对遍历到的每个条目录都做归档路径校验，于是一个被 `--exclude` 排除掉的
socket 仍然会让整次备份失败。修法是**只对进入归档的条目录做路径校验**，并且让
`preview` 与 `backup` 走同一遍遍历（后续 `refactor: share source traversal
between backup and preview`、`fix: validate archive paths only after the filter
decisions`）。这条问题的根源是"校验"和"选择"两个阶段的顺序，而不是某一行代码。

**2）元数据语义一开始不够紧。**
`type:` 的细分取值与 `uid / gid / user / group` 在早期实现里与旧语义混在一起，
出现过"同一份规则在 legacy 路径与扩展路径上结果不同"的隐患，随后收紧为
`fix: tighten filter metadata semantics` 与
`fix: align legacy filtering with extended metadata semantics`。

**3）GUI 预览会陈旧。**
快速连续修改规则时，先发出的预览请求可能后返回，界面显示的是旧结果。改成
latest-request-wins：只接受最后一次请求的结果，中间的响应直接丢弃。

**4）两套 GUI 的维护成本开始显现。**
同一时期 Widgets 与 QML 都要接 Filter。处理办法不是砍掉一套，而是把**业务与
校验全部下沉到核心和中间层**，界面只做表单与展示；这一约定在后续每个阶段都被
沿用（例如三页共用同一个过滤编辑器）。

### 3.3 下一阶段的改进动作

1. Archive v2 的打包 / 压缩 / 加密作为独立流水线层接入，不改 Filter 语义；
2. 已有的 round-trip 与变异测试思路扩展到压缩与加密的字节级格式；
3. 归档路径校验、preview 与 backup 的遍历继续共用一个实现；
4. 配置与仓库状态在 GUI 与 CLI 上保持同一套模型。

---

## 4. 阶段结论

这一阶段把项目从"能备份"推进到"**能选择、能配置、能管理**"：

```text
源目录 → Filter（11 个字段的规则）→ ArchiveWriter → 仓库里的 .bak
```

Filter 成为归档之前的独立层，仓库驱动的产品形态（配置 → 备份 → 备份管理）在
CLI 与 Modern GUI 上同时成立；测试从"功能往返"扩展到"规则语义 + 旧行为不变 +
前端与后端一致"。下一阶段的重点是归档本身的打包、压缩与加密。
