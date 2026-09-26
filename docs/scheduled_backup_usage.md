# 定时备份（Scheduled + Full）使用说明

> 本页对应 PR #17：**定时触发 + 完整快照 + 变化检测 + 保留策略**。
> 它不包含增量存储，也不包含实时（inotify）触发 —— 见文末的"明确不做"。

---

## 1. 它到底做了什么

到达计划时间时，程序按这个顺序走一遍：

~~~text
到达计划时间
  → 扫描源目录
  → 与上一份"成功快照"的源清单（manifest）逐条比较
  → 没有变化：跳过（不调用备份引擎、不产生 .bak、不更新清单）
  → 有变化：创建一份新的**完整独立**快照
  → 记录运行历史
  → 执行保留策略（只淘汰计划任务自己管理的旧快照）
~~~

一句话概括：

> **定时触发 + 变化检测 + 有变化才创建 + 每份都是完整独立快照 +
> 只淘汰 scheduler 自己管理的旧快照。**

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

### 3.3 启停

~~~bash
backupctl schedule enable
backupctl schedule disable
~~~

enable 之前会做完整校验：trigger / strategy 必须是当前支持的模式、周期与保留数量
必须在范围内、规则必须合法、源目录必须存在且是真实目录（不是软链接）、
必须已经配置备份仓库。任何一条不满足都**不会**启用。

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
$XDG_CONFIG_HOME/backup-project/backup-gui-modern/config.json
$XDG_CONFIG_HOME/backup-project/backup-gui-modern/schedule.json
$XDG_CONFIG_HOME/backup-project/backup-gui-modern/schedule-manifest.dat
$XDG_CONFIG_HOME/backup-project/backup-gui-modern/schedule.json.lock
~~~

XDG_CONFIG_HOME 未设置或不是绝对路径时回退到 $HOME/.config。

* schedule.json 是 versioned 的固定 schema，字段集合**完全相等**：
  少一个字段报 missing required field，多一个字段报 unknown field，
  重复 key 直接报错。坏配置只如实报告，绝不静默回退到默认值。
* 保存是原子的：临时文件 → fsync → rename → 目录 fsync。
* 新建文件的权限固定 0600（不受 umask 影响）。
* schedule.json 里**没有任何密码字段**。

---

## 7. 崩溃一致性

顺序是刻意的：

~~~text
发现变化
  → 创建完整 .bak（归档自己会 fsync / 发布）
  → 保存源清单
  → 保存 schedule state
  → 执行保留策略
  → 记录 history
  → 再次保存 schedule state
~~~

由此得到的保证：

* 归档已经发布、state 还没写：这份 .bak 最多变成"没人认领的普通备份"，
  照样能列出、能恢复、能删除，**不会**被误删；
* 保留策略已经删掉旧归档、state 还没更新：下一次对账会发现它不见了，
  自动把这条过期记录摘掉；
* **state 永远不让归档的正确性依赖它**：删掉 schedule.json 之后，
  仓库里的 .bak 依然完整可用，只是"来源"退化成未知。

---

## 8. 已知盲区（诚实说明）

* 变化检测是 **metadata-first**：只比较路径、类型、大小、mtime、mode、uid/gid、
  软链接目标、设备号、硬链接关系，**不读文件内容**、不算 SHA-256。
  因此

  > **同大小 + 同 mtime 的人为原地改写逃得过这一版变化检测。**

  这不是密码学意义上的完整性校验。
* **目录的 mtime 不参与比较**。任何子项的新增/删除都会顺带改掉父目录的 mtime，
  把它算成变化会让"新增一个被筛选规则排除的文件"也触发一次完整快照，
  而实际备份集合一个字都没变。代价是"单独 touch 一个目录、内容不变"看不出来。
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
