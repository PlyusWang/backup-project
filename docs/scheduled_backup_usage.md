# 定时备份（Scheduled + Full）使用说明

> 本页对应 PR #17：**定时触发 + 完整快照 + 变化检测 + 保留策略**。
> 它不包含增量存储，也不包含实时（inotify）触发 —— 见文末的"明确不做"。

---

## 1. 它到底做了什么

到达计划时间时，程序按这个顺序走一遍：

~~~text
到达计划时间
  → 确认"上一份源清单"确实属于仓库里一份仍然存在、仍然归本计划管理的快照
  → 扫描源目录
  → 与那份源清单（manifest）逐条比较
  → 没有变化：跳过（不调用备份引擎、不产生 .bak、不更新清单）
  → 有变化：创建一份新的**完整独立**快照
  → 执行保留策略（只淘汰计划任务自己管理的旧快照）
  → 记录运行历史
~~~

一句话概括：

> **定时触发 + 变化检测 + 有变化才创建 + 每份都是完整独立快照 +
> 只淘汰 scheduler 自己管理的旧快照。**

### 源清单是"绑定"在某一份真实快照上的

第一行那一步不是装饰。程序记下的不只是"上次扫描时源长什么样"，还记下了
**那份清单属于仓库里的哪一份快照、哪个仓库、哪个源目录**。

为什么必须这样：如果只看清单，会出现两种"看起来没变化、其实已经错了"的情况。

~~~text
情况一：最新的那份快照被手工删掉
  清单仍然等于当前源 → 老逻辑会跳过
  但仓库里只剩一份更旧的快照，它并不包含这中间的变化

情况二：把仓库换成了另一个（全新的）目录
  清单仍然等于当前源 → 老逻辑会跳过
  但新仓库里一份可用的完整快照都没有
~~~

所以每一轮判定"没变化"之前，都会先确认：清单对应的那份快照**仍然存在于
当前仓库里、仍然归本计划管理**。任何一条不成立，这一轮就会老老实实再建一份
完整快照（界面上显示"已重建基线快照"，CLI 输出 `baseline reset`）。
**宁可多建一份，也绝不少一份。**

另外，仓库本身读不出来时（比如没挂载），程序会直接失败退出，**不会**把
"读不到"当成"快照被删了"——那会导致往一个空的挂载点里新建备份。

### "变化检测"不是"增量备份"

这是本页最重要的一条。

* **变化检测**：只是决定"这一轮要不要建新快照"。
* **增量存储**：把新快照写成相对上一份的差异（delta），恢复时需要依赖链。

本版本做的是前者，产出的每一份 .bak 都是**完整、独立、单文件即可恢复**的：

~~~text
把任意一份计划快照单独拷到另一台机器
  → 不需要仓库里的任何其它文件
  → 直接 backupctl restore 就能完整恢复
~~~

真正的增量策略（delta 格式、baseline 依赖链、generation compaction）**留给下一阶段**，
本版本一行都没有实现，也没有留半成品接口。

---

## 2. 生命周期：只在 runner 运行期间生效

第一版的 Scheduled Trigger 定义得很窄：

> **定时任务只在一个 scheduler runner 正在运行时生效。**

runner 可以是：

~~~text
Modern GUI（自动备份页）
或
backupctl schedule watch
~~~

也就是说：

* 关掉程序之后，定时任务**不会**继续跑；
* 不提供 systemd service、不提供后台 daemon、不 fork / 不 daemonize、
  也不做开机启动。

界面上有一行原文写着这件事，CLI 的 schedule enable 也会提示同一句话。
之所以这么定，是因为"看起来装了定时任务、其实没人跑"比"明确告诉你需要开着程序"
危险得多。

同一个计划在任意时刻**只允许一个 runner**。GUI 与 backupctl schedule watch
同时启动时，只有一个能拿到锁（flock），另一个会明确显示：

~~~text
The scheduled backup is already held by another process
~~~

它**不会**也去备份一遍。

---

## 3. CLI

所有命令都支持两个全局选项：

~~~text
--config-file <path>    覆盖 config.json（默认位置见 app_paths.h）
--schedule-file <path>  覆盖计划存储文件（默认 <config 目录>/schedule.json）
~~~

### 3.1 查看配置

~~~bash
backupctl schedule show
~~~

报告：启用状态、trigger、strategy、源目录、周期、保留数量、打包 / 压缩 / 加密、
筛选规则、上次成功时间、下次运行时间、当前管理的快照列表、最近一次运行结果。

### 3.2 修改配置

~~~bash
backupctl schedule set \
  --source /home/user/data \
  --interval-minutes 60 \
  --retain 12 \
  --pack mypack \
  --compression none \
  --include 'ext:cpp;h' \
  --exclude 'path:**/build/**'
~~~

* --pack：mypack / ustar / fast-ustar
* --compression：none / huffman / lzss-huffman
* --encryption：**只接受 none**（原因见第 5 节）
* 筛选规则的语法与 backupctl backup --include/--exclude 完全一致，
  保存前会用真实的 Filter::AddRule 逐条校验，非法规则直接拒绝。

不带 --clear-filters 时，--include / --exclude 是**追加**：多次执行会不断累积。
要把已经存下来的规则整批换掉，加 --clear-filters：

~~~bash
# 先清空已有的 include / exclude，再把这次命令行给的规则作为新的规则集合写进去
backupctl schedule set --clear-filters --include 'ext:txt;md'

# 只清空，不加新规则
backupctl schedule set --clear-filters
~~~

--clear-filters 与 --include / --exclude 的**书写顺序无关**：命令行上给的所有
规则都保留，被清掉的只有上一次存下来的那些。GUI 的计划页可以直接删除单条规则，
CLI 靠这个开关做到同样的事，两边看到的是同一份 schedule.json。

**如果这份计划当前是 enabled 的**，那么任何一次 schedule set 之后，程序都会用
一次完整的启用校验检查"改完之后还跑不跑得起来"（模式、周期、保留数量、规则、
源目录、仓库）。校验不过就一个字节都不写，旧配置原样保留 —— 不允许出现一份
"写着 enabled、实际一跑就失败"的配置。

### 3.3 启停

~~~bash
backupctl schedule enable
backupctl schedule disable
~~~

enable 之前会做完整校验：trigger / strategy 必须是当前支持的模式、周期与保留数量
必须在范围内、规则必须合法、源目录必须存在且是真实目录（不是软链接）、
必须已经配置备份仓库且仓库存在、是真实目录（不是软链接）。任何一条不满足都
**不会**启用，也不会把 enabled 悄悄写成 true。

GUI 的"保存并启用"调用的是同一个校验函数，两边不会一边松一边紧。

**第一次启用会把下一次运行排在一个完整周期之后**（next run = 现在 + 周期），
不会勾上就在下一秒跑一次。想立刻跑用 schedule run / 界面上的"立即检查并运行"。
已经启用的计划反复保存配置**不会**重置这个时间点 —— 只有 disabled → enabled
这一次转换才会重算。

### 3.4 立即检查并运行

~~~bash
backupctl schedule run
~~~

它**跳过"还没到点"这一条**，但仍然做真实的变化检测：

~~~text
没有变化 → skipped_no_changes（跳过，不产生新归档）
有变化   → 创建一份完整快照
~~~

所以它**不是**"强制备份"。GUI 上的"立即检查并运行"走的是同一条语义。

### 3.5 运行历史

~~~bash
backupctl schedule history
~~~

每条记录包含：完成时间、结果（success_created / skipped_no_changes /
failed / success_with_retention_warning）、变化摘要、归档名、诊断原文。
历史只保留最近 32 条，超出就丢最旧的 —— 它是一段日志，**不是**当前文件列表。

### 3.5b 运行期换仓库

计划任务用的是**当前**配置里的仓库，不是启动时的那个：

* GUI 在设置页改了仓库之后，计划控制器会立刻跟上，下一次评估写到新仓库；
* `backupctl schedule watch` 在每一轮真正评估之前重新读一次配置，所以另一个
  CLI 或 GUI 执行过 `config repository set B` 之后，下一轮就写到 B 去。

换仓库之后，旧仓库里那份"基线快照"在新仓库里不存在，于是第一轮会先在 B 里
建立一份完整的基线快照（CLI 输出 `baseline reset`），旧仓库不会被写入。

### 3.6 前台长运行

~~~bash
backupctl schedule watch
~~~

前台运行，到点执行评估；Ctrl+C 干净退出并释放锁。它不会 daemonize。

### 3.7 仓库管理

~~~bash
backupctl repository list
backupctl repository delete <file_name>
backupctl config repository show
backupctl config repository set <path>
~~~

repository list 会标出每条记录的来源：

~~~text
origin=scheduled   由计划任务创建，受保留策略管理
origin=manual      手动备份，永远不会被自动淘汰
~~~

这个判断来自 ScheduleStore，**不来自文件名解析**：文件名永远不是 ownership 的真相来源。

`repository delete` 删掉一份仍然登记在计划任务 managed 名单里的归档时，会立刻调用
共享核心的 `ScheduledBackupService::ReconcileManagedSnapshots` 把那条过期记录摘掉，
再保存 schedule state。

**GUI 的备份管理页删同一个文件时走的是同一步**（Modern GUI 订阅
`BackupController::archiveDeleted`，由 ScheduleController 执行同一份 reconcile）。
两个前端在这件事上的后果完全一致：删完之后"计划快照"名单与 `backupctl schedule show`
都不会再显示一个已经不在仓库里的文件名。

---

## 4. GUI

Modern GUI 的侧栏新增一页「自动备份」，包含：

~~~text
[启用定时备份] 开关
源目录         [路径] [浏览目录]
周期           每 [60] 分钟
保留版本       最近 [12] 个计划快照
打包           MyPack / USTAR / Fast USTAR
压缩           不压缩 / Huffman / LZSS + Huffman
加密           不加密（固定）
筛选规则       include / exclude，语法与备份页一致
[保存计划] [立即检查并运行]
状态           上次运行 / 下次运行 / 最近结果 / 运行者
运行历史       时间 / 结果 / 变化摘要 / 归档名
计划快照       只列 scheduler 自己管理的那些
~~~

界面上的约束：

* **没有** Incremental 按钮，**没有** Realtime 按钮，也没有任何"即将支持"的假入口；
  只有一行架构说明写下当前支持什么。
* 加密是固定的"不加密"，旁边写着原因。
* 业务逻辑一行都不在 QML 里：next run 的计算、到期判定、变化检测、保留策略、
  归档删除全部在共享核心，CLI 用的是同一份。
* 手动备份 / 恢复与计划备份**不会同时写盘**：计划到期时如果正在手动操作，
  只记一个 pending 标记（多个到期事件合并成一次），手动操作结束后补跑一次。

---

## 5. 为什么定时备份不接受加密

现有项目没有安全的持久化密钥来源。要做到"无人值守地加密"，必须有地方存放密钥，
而本版本**没有任何**这样的地方：

~~~text
× 不写 config.json
× 不写 schedule.json
× 不写环境变量
× 不接受命令行参数（密码会进 argv、ps、shell history）
× 不复用 GUI 上一次输入的密码
~~~

所以 schedule 的 encryption 只能是 none，配置成别的会被**明确拒绝**
并给出这句原因：

~~~text
定时无人值守加密需要安全的密钥来源；当前版本不会持久化明文密码。
~~~

需要加密备份时，用手动备份：

~~~bash
backupctl backup <source> <archive> --encryption aes-256-ctr-hmac-sha256
~~~

密码只从 /dev/tty 交互读取（关闭回显，备份问两次，恢复问一次），
只在本进程内存里活着：不写配置、不写日志、不进归档、不回显。
标准输入不是交互终端时**明确失败**，绝不偷偷从管道读秘密。

> DES-CBC 在本项目中是 **educational / legacy**，不要用于任何真实场景。

---

## 6. 存储与权限

默认布局（与 Modern GUI 严格同源，见 include/app_paths.h）：

~~~text
$XDG_CONFIG_HOME/backup-project/backup-gui-modern/app.lock
$XDG_CONFIG_HOME/backup-project/backup-gui-modern/config.json
$XDG_CONFIG_HOME/backup-project/backup-gui-modern/schedule.json
$XDG_CONFIG_HOME/backup-project/backup-gui-modern/schedule-manifest.dat
$XDG_CONFIG_HOME/backup-project/backup-gui-modern/schedule.json.lock
~~~

XDG_CONFIG_HOME 未设置或不是绝对路径时回退到 $HOME/.config。目录按 0700 创建。

* schedule.json 是 versioned 的固定 schema，字段集合**完全相等**：
  少一个字段报 missing required field，多一个字段报 unknown field，
  重复 key 直接报错。坏配置只如实报告，绝不静默回退到默认值。
* 保存是原子的：同目录唯一临时文件（`mkstemp`，`O_CREAT|O_EXCL`）→ 写入 → fsync →
  close → rename → 目录 fsync。临时文件名每次不同，所以既不会跟随别人预放的符号链接，
  也不会截断别人预放的文件，同一进程里两次并行保存也不会撞名。config.json 走的是
  同一个函数。
* 新建文件的权限固定 0600（不受 umask 影响）。
* schedule.json 里**没有任何密码字段**。

### 6.1 全应用单实例

**整个产品同一时刻只允许一个进程。** GUI + GUI、GUI + CLI、CLI + CLI、CLI + GUI
四种组合全部拒绝，而且是在进入任何业务逻辑之前就拒绝（不会先读一遍配置再发现
"已经有另一个实例"）。

* 锁是 `app.lock`，用 `flock(LOCK_EX | LOCK_NB)`：进程正常退出、崩溃、被 SIGKILL
  都由内核自动释放。磁盘上留一个 stale 的锁文件**不会**把产品永久锁死，锁文件里的
  pid 只是给人看的提示。
* 锁路径只由配置根决定（`app_paths.h`），**与仓库、`--config-file`、
  `--schedule-file` 全都无关**：换个参数启动不会绕过单实例。
* 锁路径本身如果是符号链接、目录或 FIFO，一律 fail closed 并明确报错，绝不 truncate
  目标文件。
* 纯 `--help` 不抢锁：已经有实例在跑时，用户仍然看得到用法。

退出码：

~~~text
0  成功
1  操作失败
2  命令行用法错误（未知命令/选项、缺少选项值、多余的位置参数、
   重复的单值选项、越界数字、非法筛选规则）
3  已经有另一个 backup-project 实例在跑
~~~

GUI 与 CLI 在"已经有另一个实例"这件事上给**同一个退出码 3**：这是同一条产品规则。

每个命令都明确消费全部 argv：`backupctl schedule show extra` 是用法错误（exit 2），
不会静默忽略 `extra`；`--retain 1 --retain 2` 也是用法错误，不会"后者覆盖前者"。
`--include` / `--exclude` 是重复有意义的可重复选项，不受这条限制。

---

## 7. 崩溃一致性

顺序是刻意的：

~~~text
发现变化
  → 创建完整 .bak（归档自己会 fsync / 发布）
  → 保存源清单（清单自带"我属于哪份快照"的归属）
  → 执行保留策略
  → 保存 schedule state
  → 记录 history
  → 再次保存 schedule state
~~~

保留策略排在第一次保存 state **之前**，理由不是顺序偏好，而是"写出去的 state
必须读得回来"：这一轮刚把新快照 push 进 managed 名单，名单长度是 retain + 1 条，
而 ScheduleStore 只接受 kMaxRetainCount 条。先写后淘汰在 retain_count 取到上界时
会要求写出一份自己都读不回来的 state —— 保存被拒绝、这一轮在写盘处提前返回、
淘汰永远轮不到执行、下一轮再建一份，仓库无上限增长，而且每一轮都报成功。
先淘汰再落盘，写出去的长度就恒 <= retain_count <= 上界。淘汰删的是最旧的，
而 baseline 刚被设成最新的这一份，所以它不可能被删掉。

由此得到的保证：

* 归档已经发布、state 还没写：这份 .bak 最多变成"没人认领的普通备份"，
  照样能列出、能恢复、能删除，**不会**被误删；
* 保留策略已经删掉旧归档、state 还没更新：下一次对账会发现它不见了，
  自动把这条过期记录摘掉；
* **state 永远不让归档的正确性依赖它**：删掉 schedule.json 之后，
  仓库里的 .bak 依然完整可用，只是"来源"退化成未知。

---

### 7.1 落盘配置不合法：明确的挂起状态

"配置不合法"与"这一次运行失败"是两件事，后果也不同：

~~~text
运行失败（kFailed）
  → 推进 next_run，记录 history，下一轮到点再试

配置不合法（kConfigInvalid）
  → 一个字节都不写（改也存不回去），next_run 原地不动，不记 history
  → 调度器进入明确的"已挂起"状态：停止 1 Hz 的重试，不再备份
~~~

* 走到这一步基本只有一种可能：有人手工改了 `schedule.json`，改成了一个
  JSON 读得懂、业务规则不认的配置（例如 `interval_minutes: 0`，或者
  `encryption` 不是 `none`）。写入口自己不会产出这种文件：`schedule set` /
  `enable` / GUI 保存都会先做同一套完整校验。
* 程序**不会**自动修复、不会删除、不会把文件改成"我们能接受的样子"。
* 恢复只有一条路：用户显式保存一份合法配置（GUI 的保存按钮 / `backupctl
  schedule set`）。保存成功即自动恢复。
* `backupctl schedule run` 遇到挂起配置返回非 0；`backupctl schedule watch`
  打印原因后明确退出——产品只允许一个进程，watch 运行期间用户根本没法去改那份
  文件，继续每 30 秒重试一遍没有任何意义。
* `schedule show` 仍然能读：它如实显示 store 状态，不会因为配置不合法就
  拒绝工作。GUI 的其它页面（手动备份 / 恢复 / 备份管理）完全不受影响。

---

## 8. 已知盲区（诚实说明）

* 变化检测是 **metadata-first**：只比较路径、类型、大小、mtime、mode、uid/gid、
  软链接目标、设备号、硬链接关系，**不读文件内容**、不算 SHA-256。
  因此

  > **同大小 + 同 mtime 的人为原地改写逃得过这一版变化检测。**

  这不是密码学意义上的完整性校验。
* **目录自身的 mtime 不参与变化检测**，但目录 metadata 参与。最终语义只有一套
  （没有所谓的 Strict Metadata 模式）：

  ~~~text
  仅目录 mtime 改变                    -> 不触发
  目录 mode / uid / gid 改变            -> 触发（metadata_changed）
  被包含的子项 新增 / 删除 / 改名        -> 触发（由子项 path diff 得出）
  被排除的子项 内容变 / 新增 / 删除      -> 不触发
  被排除子项导致父目录 mtime 改变        -> 仍然不触发
  ~~~

  理由：任何子项的新增/删除都会顺带改掉父目录的 mtime，把它算成变化会让
  "新增一个被筛选规则排除的文件"也触发一次完整快照，而实际备份集合一个字都没变。
  代价是"单独 touch 一个目录、内容不变"看不出来。

  > **目录 mtime 仍然会被归档保存、也会被恢复。** 它只是不作为定时备份的触发条件。
  > 这一条由 `scripts/test.sh` 的 META-08 / META-09 / META-10 与
  > `tests/unit/scheduler_core_test.cpp` 的 DIR-01..DIR-19 两侧分别钉住。
* 被筛选规则排除的文件发生变化**不会**触发新快照 —— 因为计划的备份集合没变。
* 第一版只支持**一个**计划任务，没有多任务列表。

---

## 9. 明确不做（留给后续阶段）

~~~text
真正的增量归档（delta 格式 / baseline 依赖链 / generation compaction）
实时触发（inotify / watcher 线程）
针对增量的依赖安全保留策略
systemd / 后台常驻服务
无人值守加密的安全密钥存储
网络备份 / 云端
去重
数据库
多任务调度
cron 解析器 / 日历式重复规则
密码 keyring
~~~

维度已经留好了（BackupTrigger / BackupStrategy），但产品入口只接受当前实现的两组：
**Manual + Full** 与 **Scheduled + Full**。其余组合会**明确报错**，
绝不静默降级 —— "选了增量却拿到全量"是最危险的那种失败。
