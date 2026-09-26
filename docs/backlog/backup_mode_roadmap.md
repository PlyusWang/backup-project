# Backup Mode Development Roadmap

本文记录备份模式后续开发思路。这里刻意把“什么时候触发备份”和“备份保存什么内容”拆成两个正交维度，避免把六种组合各写成一套独立逻辑。

## 1. 两个正交维度

### 触发方式（Trigger）

- **Manual**：用户手动触发。
- **Scheduled**：按计划周期触发。
- **Realtime**：监听文件系统变化后触发。

### 备份策略（Strategy）

- **Full**：每个恢复点都是完整、独立可恢复的快照。
- **Incremental**：只保存相对基线/前序恢复点发生的变化，需要维护依赖关系。

因此产品层可以形成 2 × 3 共六种组合：

| 触发方式 \ 备份策略 | Full | Incremental |
| --- | --- | --- |
| Manual | 手动完整备份 | 手动增量备份 |
| Scheduled | 定时完整备份 | 定时增量备份 |
| Realtime | 实时完整备份 | 实时增量备份 |

这六种是产品层组合，不应实现成六套互不相关的代码路径。底层应保持“Trigger + Strategy”组合。

## 2. 当前状态

当前已经完成的是：

```text
Manual + Full
```

也就是现有 GUI / CLI 的普通手动备份与恢复。

打包、压缩、加密、Filter 等属于备份流水线的其它正交配置，不应与 Trigger / Strategy 耦合。

## 3. 开发顺序

后续按以下顺序推进：

### 阶段 A：Scheduled + Full

下一阶段先实现定时完整备份，并完成课程要求中的“周期性定时备份和数据淘汰”。

核心语义：

1. 用户配置源目录、周期、备份选项和保留数量。
2. 到达计划时间后扫描源目录。
3. 若与上一次成功快照相比没有变化，则跳过本轮，避免制造完全重复的备份。
4. 若存在新增、删除、内容变化或关键元数据变化，则生成一个新的**完整独立快照**。
5. 新快照成功后执行 retention。
6. retention 默认保留最近 **12** 个由该计划任务创建的快照；用户可修改。
7. 自动淘汰只管理该计划任务自己的快照，不能误删用户手工创建的备份。
8. 用户仍可在备份管理中手动删除允许删除的归档。

注意：这里的“检测变化后再建立快照”不是增量存储。每个 Scheduled + Full 产物仍然可以独立恢复。

### 阶段 B：Incremental Strategy

定时完整备份稳定后，再实现真正的增量备份核心。

增量策略需要明确表示：

- baseline；
- added / modified / removed；
- 恢复点之间的依赖；
- 完整恢复链校验；
- 增量恢复；
- dependency-aware deletion / retention。

完成 Incremental Strategy 后即可自然暴露：

```text
Manual + Incremental
Scheduled + Incremental
```

而不是再复制两套增量算法。

### 阶段 C：Realtime Trigger

最后实现实时触发方式，例如 Linux inotify：

```text
文件变化
→ debounce / coalescing
→ 形成一次稳定触发
→ 交给当前 Strategy
```

Realtime Trigger 完成后，可组合得到：

```text
Realtime + Full
Realtime + Incremental
```

实际实现应避免“一个 inotify event 就立即生成一个备份”，需要 debounce、事件合并和 busy/pending 语义。

## 4. Retention 规则

### Full 快照

Full 快照彼此独立，因此 count-based retention 可以直接执行：

```text
S1 S2 ... S12 S13
→ 新 S13 成功
→ 淘汰最旧的 S1
```

第一版推荐默认：

```text
retain_count = 12
```

### Incremental 快照

Incremental 存在依赖，不能简单按文件时间删除任意旧归档。

未来推荐按 generation 管理，例如：

```text
F0
├── Δ1
├── Δ2
├── ...
└── Δ11
```

达到阈值后：

```text
创建新的完整 baseline F12
→ 验证 F12 可独立恢复
→ 再整组淘汰旧 generation
```

在实现 dependency-safe retention 之前，不能允许用户随意删除仍被后续恢复点依赖的中间增量。

## 5. Change Detection

Scheduled + Full 阶段即可先建立可复用的 change detector，用于判断是否需要产生新快照，并为 GUI 展示版本差异。

每个 path 至少记录：

```text
path
type
size
mtime
mode
uid
gid
link_target
device major/minor
```

对比前后 manifest 可以得到：

```text
added
removed
modified
metadata_changed
```

第一阶段不要求给所有普通文件计算内容哈希；后续 Incremental Strategy 可以根据正确性和性能需求继续强化。

## 6. GUI / CLI 对等原则

新增的核心备份能力必须位于共享 C++ core/service 层，而不是只写在 QML 中。

原则：

```text
                ┌─ Modern GUI
Shared Core  ───┤
                └─ CLI
```

GUI 与 CLI 应能访问同一套：

- schedule 配置与启停；
- retention 配置；
- 手动执行计划；
- history / 最近一次运行 / 下一次运行；
- 备份列表与删除；
- Trigger / Strategy；
- pack / compression / encryption / filter 等备份选项。

具体命令行语法可以在实现对应功能时确定，但不允许出现“GUI 有真实能力、CLI 没有同等核心入口”的长期分叉。

## 7. 定时加密的安全边界

当前密码不会持久化到 ConfigManager。

因此无人值守 Scheduled Backup 若使用加密，需要单独设计安全 secret 来源，例如系统 keyring / key file / 启动后解锁等。

在安全 secret 机制设计完成前，不应为了定时任务方便而把明文密码写入配置文件。

## 8. 架构目标

最终目标不是“六套备份实现”，而是：

```text
Trigger
  Manual / Scheduled / Realtime
            │
            ▼
Strategy
  Full / Incremental
            │
            ▼
Existing Backup Pipeline
  Filter
  → Pack
  → Compress
  → Encrypt
  → Repository
            │
            ▼
Retention / History
```

这样新增触发方式不会重写备份算法，新增备份策略也不会重写 GUI、CLI 或调度器。
