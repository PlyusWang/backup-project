# Phase 5 Review & Retrospective：定时备份与实时（inotify）备份

> 覆盖阶段：2026-09-27 -- 2026-09-30
> 对应 PR：#17（feature/scheduled-backup-retention）、#19（feature/realtime-backup-trigger）
> 收口基线：PR #19 合并后的 `main`（e15c012）
> 这一阶段是原 Gantt 中的 S8，实际比原计划提前完成。

---

## 1. 阶段 Goal

到 Phase 4 为止，备份都是"人按下按钮"才发生。这一阶段要让备份在**无人值守**的
两种场景下发生：

1. **定时**：到达计划时间就检查源目录有没有变化，有变化才创建快照，并按保留策略
   淘汰旧快照；
2. **实时**：源目录发生变化后，等变化稳定下来（debounce）就触发一次备份。

两条路径都必须复用既有引擎：定时与实时只决定"什么时候触发"，不重新实现 Full /
Incremental 策略，也不重新实现归档、压缩与加密。

---

## 2. Review

### 2.1 定时备份（PR #17）

CLI 形态是 `backupctl schedule show | set | enable | disable | run | history | watch`，
`schedule set` 接受 `--source`、`--interval-minutes`、`--retain`、`--strategy`、
`--pack`、`--compression`、`--encryption none` 与 `--include` / `--exclude`
（另有 `--clear-filters`：先清掉已存规则再加本次命令行给的规则）。

一次触发按固定顺序走：

```text
到达计划时间
  → 确认"上一份源清单"确实属于仓库里一份仍然存在、仍然归本计划管理的快照
  → 扫描源目录
  → 与那份源清单（manifest）逐条比较
  → 没有变化：跳过（不调用备份引擎、不产生 .bak、不更新清单）
  → 有变化：创建一份新的完整独立快照
  → 执行保留策略（只淘汰计划任务自己管理的旧快照）
  → 记录运行历史
```

**源清单是绑定在某一份真实快照上的。** 只记"上次扫描时源长什么样"会出现两种
"看起来没变化、其实已经错了"的情况：最新快照被手工删掉，或者仓库被换成了另一个
目录。所以每一轮判定"没变化"之前，都会先确认清单对应的快照仍然存在于同一个仓库、
同一个源目录下。`run` 只是忽略到期时间立即评估一次，同样不会变成"强制备份"。

### 2.2 实时备份（PR #19）

CLI 形态是 `backupctl realtime show | set | enable | disable | watch | history`，
`set` 接受 `--debounce-ms`、`--max-wait-ms` 以及和定时相同的选项。

三条硬规则（写在 `include/realtime_backup_service.h` 顶部）：

1. **实时只决定什么时候触发**：Full 走 `BackupEngine`，Incremental 走
   `RunIncrementalBackup`，这里不复制任何策略实现；
2. **归属不用中央可变 state**：每份实时快照配一个 per-snapshot marker sidecar
   `<snapshot>.realtime`（`BPREALTIME1`），机器写机器读。没有"每次备份都改写"
   的实时历史 JSON，也就没有"监听 home 时状态文件自己触发自己"的问题；
3. **marker 必须绑定实际 archive bytes**：写 marker 之前先用
   `LoadVerifiedSnapshotIdentity` 拿到**验证过的** snapshot id，只信实际字节，
   不信 header 或信封里的声明值。

另外两条来自实际使用的前提：源目录与仓库不允许重叠；`realtime enable` 要求仓库
已配置、源目录真实存在；**自动触发不允许加密**（`--encryption` 必须是 none），
因为没有人在旁边输入密码。

### 2.3 Modern GUI

- 新增实时备份页，并把三页（备份 / 自动备份 / 实时备份）的过滤规则编辑抽成
  **同一个可视化编辑器**（`feat: share one visual filter editor across the three
  pages`）；
- 计划频率从"一个分钟数"改成 **数值 + 单位**；
- 实时页的保留策略文案不再"假装不确定"（`fix: ... stop faking retention
  uncertainty`），规则在写入前先校验、JSON 正确转义；
- 自检通知不再掩盖真实错误（`fix: keep self-check notice from masking GUI errors`）；
- 修掉组合框的 hover 与键盘导航互相干扰的视觉问题。

### 2.4 测试

| 套件 | 内容 | 该套件自报的结果 |
| --- | --- | --- |
| scripts/scheduled_backup_test.sh | 到期判定、无变化跳过、清单与快照绑定关系、保留策略、运行历史、`run` 语义 | PASS=318 / FAIL=0 |
| scripts/realtime_test.sh | watcher → debouncer → service 全链路、marker 归属、源与仓库重叠拒绝 | PASS=36 / FAIL=0 |

两个数字取自 2026-10-07 收口回归中 `scripts/final_gate.sh` 的真实日志；两条套件
在 gate 中的 `exit=0`。

### 2.5 未在这一阶段完成

- 依赖感知的保留策略（父快照被增量链引用时不能直接删）在这一阶段只是为完整快照
  服务；链式依赖的处理随 Phase 6 的增量一起补齐；
- 客户端侧没有随包分发的系统服务单元（`packaging/` 里只有服务端的
  `backup-project-server.service`）：`backupctl realtime watch` 是前台运行、
  收到 SIGINT / SIGTERM 才结束的形态，定时与实时的配置存在产品自己的配置里，
  由 CLI 与 GUI 页面管理。

---

## 3. Retrospective

### 3.1 做得较好的地方

**触发与策略彻底分开。**
定时与实时都只是"触发器"，备份策略、压缩、加密、保留的实现在核心一处；因此增量
策略在 Phase 6 接入时，两条触发路径都不需要改。

**用 per-snapshot marker 代替中央状态文件。**
这一条直接消掉了一整类问题：监听自己 home 目录时，状态文件本身会触发下一次备份。
marker 是只写一次的旁文件，读写都不需要全局锁。

**"没变化就跳过"必须先证明清单还有效。**
这句约束是用两个真实反例换来的（快照被删、仓库被换），现在写进了实现与文档。

**把测试竞态当缺陷修，而不是加 sleep。**
调度器测试出现过断言与后台线程赛跑的问题，处理方式是让断言等待**状态条件**
（`make schedule busy-manual-backup assertion race-free` 等），并把自检信号的状态
保活到断言之后。

### 3.2 遇到的问题与处理

**1）UI 相关缺陷在这一阶段集中出现。**
组合框 hover 与键盘导航互相干扰、轻主题 hover 颜色闪烁、通知掩盖错误——都不是
核心逻辑问题，但都影响可用性。处理方式是逐个修并补测试（有的用截图几何断言），
而不是留到最后统一"美化"。

**2）三页各写一套过滤器编辑器。**
自动备份页与实时备份页一开始各自实现了一份规则编辑界面，很快就出现行为不一致。
处理方式是抽出共享编辑器，页面只负责摆放。

**3）实时页的文案与真实语义不一致。**
早期版本用"可能保留/可能不保留"之类的模糊措辞掩盖了保留策略的真实行为。
改成如实说明后，用户才能在配置前判断后果。

**4）监听根目录在运行中消失。**
watcher 在 attach 之后如果根目录被删除或替换，继续运行会产生不可预测的行为。
现在 attach 时发现根目录已经不存在就直接拒绝，运行时根目录消失则结束监听并报告。

### 3.3 下一阶段的改进动作

1. 增量策略接入定时与实时，保留策略升级为依赖感知（父快照被引用时不能删）；
2. 删除顺序按 descendants-first 处理，避免删到一半留下断链；
3. 网络传输复用同一套增量与材料包语义，不在网络层重写增量算法。

---

## 4. 阶段结论

这一阶段之后，"什么时候备份"这件事不再需要人参与：

```text
手动 backupctl backup / GUI 按钮
定时 backupctl schedule（+ 保留策略 + 运行历史）
实时 backupctl realtime（inotify + debounce + per-snapshot marker）
```

三条触发路径共用同一套引擎与同一套 Archive v2 流水线；下一阶段处理"备份到哪里"
（增量链与远端存储）。
