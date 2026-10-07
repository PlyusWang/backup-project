# 系统设计

> 文档编号：02
> 文档名称：数据备份与恢复系统设计
> 状态：当前 main 的完整架构（CLI + Modern GUI + Core + 服务端）
> 基线：commit 6d1e2ba71a48862927f23442604d6bd4b430e62d（已合并 PR #27）
> 相关文档：docs/00_project_baseline.md（工程基线）、docs/01_requirements.md、docs/03_testing.md、
> docs/format/archive_v0.1.md、docs/format/archive_v2_container.md、docs/filter_usage.md、
> docs/scheduled_backup_usage.md、docs/basic_cli_usage.md、docs/network_backup_usage.md、
> docs/secure_transport.md、docs/bpsec2-design.md、docs/remote_incremental.md、
> docs/ui/modern_qml_gui.md、docs/release-layout.md

本文描述当前源码里**已经存在**的结构。每一条结论后面给出源码位置（path:line），
可以逐条对照。文中不写"未来会怎样"，只写"现在是什么样、为什么这样"。

---

## 1. 系统范围

产品有四个可执行文件和两类运行角色：

| 产物 | 入口 | 说明 |
|---|---|---|
| `backupctl` | app/backupctl.cpp:127 | 命令行前端，8 个子命令 |
| `backup-gui-modern` | ui/modern/main.cpp:513 | Modern GUI（Qt Quick / QML，7 个页面） |
| `backup-server` | server/main.cpp | 远程备份服务端，独立进程 |
| `backup-server-admin` / `backup-server-keygen` / `backup-cert-tool` | server/admin_main.cpp、server/keygen_main.cpp、tools/cert_tool_main.cpp | 只在服务器本机或离线机器上运行的运维工具 |

`backup-gui`（Qt Widgets 版）仍在源码里（ui/desktop/），是 Sprint 阶段的桌面入口，
构建目标与 Modern GUI 完全并行（Makefile:286-316）。

功能上分两条独立的线：

- **本地线**：源目录 → 筛选 → 打包 → 压缩 → 加密 → 仓库里的 `.bak`；恢复是逆向。
- **远程线**：本地已经生成好的 `.bak` → BPSEC1/BPSEC2 加密通道 → BPNET1 → 服务端
  存字节 + SQLite 元数据。

两条线共用同一个本地核心：远程上传的归档与本地仓库里的归档是同一份字节、同一个格式。

---

## 2. 总体分层

```text
┌──────────────────────────────────────────────────────────────────────┐
│ Presentation                                                         │
│   backupctl            backup-gui-modern           backup-gui        │
│   (app/backupctl.cpp   (ui/modern/*.cpp + qml)     (ui/desktop/*)    │
│    + src/cli/*)                                                      │
└───────────────┬────────────────────┬─────────────────────────────────┘
                │  CliContext        │  Controller / QML context property
                ▼                    ▼
┌──────────────────────────────────────────────────────────────────────┐
│ Shared core（纯 C++17，不链接 Qt）                                    │
│   Filter        BackupCatalog     BackupEngine     archive_pipeline  │
│   Scheduler     Realtime          Incremental      Network client    │
│   (src/core、src/filter、src/archive、src/compression、src/crypto、   │
│    src/scheduler、src/realtime、src/network、src/catalog、src/config) │
└───────────────┬──────────────────────────────────────────────────────┘
                │
     ┌──────────┼───────────┬───────────────┐
     ▼          ▼           ▼               ▼
 src/archive src/compression src/crypto   src/network
 pack+容器    HUF1/LZH1      AES/DES/HMAC  BPNET1 + BPSEC1/2
     │          │           │               │
     └──────────┴───────────┘               ▼
                ▼                    backup-server（独立进程）
        仓库里的 .bak（BKPCNT2 /        server/remote_server.cpp
        BKPINC1 delta + 副文件）        server/remote_metadata_store.cpp
```

### 2.1 为什么核心不依赖 Qt

- 核心源文件全部列在 `CORE_SOURCES` 里（Makefile:9-30、Makefile:36-53、Makefile:110-116），
  通用编译规则只加 `-Iinclude`，不带任何 Qt 头文件路径（Makefile:264-266）；
  只有 GUI 专用的模式规则才追加 `-fPIC $(QT_CFLAGS)`（Makefile:314-316、Makefile:359-361）。
- 所以"没有装 Qt 的开发机也能编译核心"不是一句口号，而是构建规则直接保证的：
  `make all` 只构建 `backupctl` + `backup-server`（Makefile:154），
  `make client-cli` 在 Qt 缺失时仍然能出客户端 CLI（Makefile:164）。
- 核心的每个头文件都自己声明了这条约束，例如 include/incremental_backup.h:21、
  include/incremental_delta.h:50、include/scheduled_backup_service.h:32、
  include/backup_mode.h:23、include/realtime_watcher.h:3。

### 2.2 CLI 与 GUI 共用同一份业务模型

- CLI 与 Modern GUI 链接的是**同一批目标文件**：`$(TARGET)` 链接 `$(OBJECTS)`
  （Makefile:224-226），Modern GUI 链接 `$(MODERN_OBJECTS) $(CORE_OBJECTS)`
  （Makefile:355-357），两者用的 `CORE_OBJECTS` 是同一份 `CORE_SOURCES` 编出来的
  （Makefile:295）。
- 命令层自己也是这么写的：include/cli_app.h:10-17 明确列出"计划相关 → ScheduledBackupService /
  ScheduleStore、仓库相关 → ConfigManager / BackupCatalog、算法 key → backup_option_keys、
  密码 → terminal_secret"，并写明"CLI 不复制第二套 scheduler，也不自己拼 repository 路径"。
- GUI 侧同样是桥接：ui/modern/main.cpp:449-489 把 BackupController、ScheduleController、
  RealtimeController、RemoteController 与三个 FilterRuleModel 建好，再用上下文属性交给 QML
  （ui/modern/main.cpp:494-512）。QML 不实现归档算法，也不实现 glob——
  规则最终都走 `FilterRuleBuilder → Filter::AddRule`（ui/modern/main.cpp:486）。
- 服务端是唯一被刻意排除在外的一侧：它**不链接** BackupEngine / Filter / MyPack / USTAR /
  压缩 / 加密 / 增量链（Makefile:55-60），只把客户端送来的归档当成"有名字、有长度、
  有 SHA-256 的不透明字节"（include/remote_server.h:5-16）。

---

## 3. 两条主数据流

### 3.1 本地：GUI / CLI → shared core → archive / filter / compression / crypto

```text
backupctl backup <src> [选项]                     Modern GUI「备份」页
  src/cli/cli_commands.cpp:372                      ui/modern/backup_controller.cpp
        │                                                  │
        │ 解析选项、编译 Filter 规则                        │ controller.startBackup()
        │ (cli_commands.cpp:396-480)                        │
        ▼                                                  ▼
   ┌────────────────────────────────────────────────────────────┐
   │ BackupCatalog::EnsureRepository / BuildArchivePath          │
   │   src/catalog/backup_catalog.cpp:295 / :545                 │
   │ BackupOptionCombination 校验（唯一答案来源）                 │
   │   src/core/backup_mode.cpp:126 / :153                       │
   └───────────────┬──────────────────────────┬─────────────────┘
        strategy=full│                  strategy=incremental│
                     ▼                                     ▼
   BackupEngine::Backup（v2 入口）              RunIncrementalBackup
   src/core/backup_engine.cpp:132               src/core/incremental_backup.cpp:1209
                     │                                     │
                     ▼                                     ▼
   ┌────────────────────────────────────────────────────────────┐
   │ RunBackupPipeline  src/core/archive_pipeline.cpp:467        │
   │   ScanSourceTree(Filter) → PackEntries → compress → encrypt │
   │   → container header 160B → 工作目录里 fsync                │
   └───────┬──────────────┬───────────────┬─────────────────────┘
           ▼              ▼               ▼
    src/archive       src/compression    src/crypto
    mypack_v2/ustar    huffman/lzss      aes/des + hmac + pbkdf2
    container_format   codec_io          random/sha256
           └──────────────┴───────────────┘
                          ▼
               PublishNoReplace（link / renameat2）
               src/core/file_io.cpp:564
                          ▼
              仓库里的一份 .bak（BKPCNT2）
```

恢复方向：

```text
backupctl restore <file_name> <dest>            Modern GUI / 备份管理页恢复
  src/cli/cli_commands.cpp:710                    ui/modern/backup_controller.cpp:1152
        │ 先按 magic 分类（cli_commands.cpp:764-781）
        ├── kUnknown（v0.1 等） → BackupEngine::Restore → ArchiveReader（旧路径）
        └── v2 container / BKPINC1 delta → RestoreSnapshotChain
                                            src/core/incremental_restore.cpp:638
                    │
                    ▼
     base 快照 → delta 链 → preflight → staging 恢复 → 原子发布
    （顺序见第 6 节，代码在 incremental_restore.cpp:694-788）
```

要点：

- 三个前端只传"源目录 + 规则 + 选项"，归档**名字**由 `BackupCatalog::BuildArchivePath`
  生成，调用方给不出任意路径（app/backupctl.cpp:18-32、cli_commands.cpp:512-518）。
- 打包、压缩、加密三层互不知道对方存在，顺序写死在编排层：
  `PACK → COMPRESS → ENCRYPT`（include/archive_pipeline.h:8-13、
  src/core/archive_pipeline.cpp:9-12）。
- 恢复先认证再解密（Encrypt-then-MAC 的必然要求），错密码必须在 HMAC 这一关失败
  （src/core/archive_pipeline.cpp:894、:964）。
- 中间产物只写在 0700 的私有工作目录里，最终靠一次 link/rename 出现
  （src/core/archive_pipeline.cpp:36-37）。

### 3.2 远程：remote client → secure transport → backup-server → metadata + storage

```text
Modern GUI「远程备份」页                backupctl remote <子命令>
  ui/modern/remote_controller.cpp         src/cli/remote_commands.cpp:414
        │                                       │
        └───────────────┬───────────────────────┘
                        ▼
        RemoteArchiveClient  src/network/remote_backup_client.cpp
          Connect(:255) → Login(:614) → List(:669)
          UploadSnapshotFile(:745) / DownloadArchiveFile(:918)
          Delete(:1119) / DeleteAccount(:1140)
                        │
                        │  先握手，再谈业务帧
                        ▼
        SecureChannel（BPSEC1 pin / BPSEC2 证书）
          src/network/secure_transport.cpp:914 / :1221
          SendFrame(:1675) / ReceiveFrame(:1704)
                        │  BPNET1 帧：32 字节头 + ≤1 MiB payload
                        ▼
        backup-server：main 线程 accept，worker 线程一连接一线程
          server/main.cpp:47（信号只写原子标志）
          server/remote_server.cpp:329 Configure / :456 Start / :2215 worker
          opcode 分派 remote_server.cpp:2177-2201
                        │
            ┌───────────┴────────────┐
            ▼                        ▼
   RemoteMetadataStore        blob 存储 <root>/<user_id>/*.bak
   server/remote_metadata_store.cpp   remote_server.cpp:1241 UserDirectory
   表 users / snapshots / deleted_users  原子发布 + trash 删除
   （:68 / :76 / :94）                  server/remote_maintenance.cpp:196
```

远端增量不重新实现算法：客户端把远端的"三件套"下载、逐字节验证、解包成本地目录，
再交给既有的本地增量引擎（include/remote_incremental.h:5-24）。
`RunRemoteBackup` 在 src/network/remote_incremental.cpp:703，`RunRemoteRestore` 在 :846。

---

## 4. 构件设计

### 4.1 CLI：`app/backupctl.cpp` + `src/cli/*`

`main()` 只做四件事：摘全局选项、判命令名、抢全应用单实例锁、按命令分派
（app/backupctl.cpp:127-231）。8 个命令在 app/backupctl.cpp:117-123 列出：

| 命令 | 实现 | 说明 |
|---|---|---|
| `backup` | src/cli/cli_commands.cpp:372 | 写进配置好的仓库，名字由 Catalog 生成 |
| `restore` | src/cli/cli_commands.cpp:710 | 按 magic 分流 legacy / 依赖链 |
| `preview` | src/cli/cli_commands.cpp:623 | 只读预览，与 GUI 的 Manual Backup 预览同源 |
| `schedule` | src/cli/cli_commands.cpp:1380（show:856 / set:924 / enable·disable:1137 / run:1183 / history:1246 / watch:1288） | 计划备份 |
| `realtime` | src/cli/realtime_commands.cpp:442（watch:276） | 实时备份 |
| `remote` | src/cli/remote_commands.cpp:414 | 注册 / 登录 / 列表 / 上传 / 下载 / 删除 / 注销 |
| `repository` | src/cli/cli_commands.cpp:1556（list:1438 / delete:1505） | 仓库列举与删除 |
| `config` | src/cli/cli_commands.cpp:1651（repository show:1593 / set:1622） | 配置读写 |

退出码集中在 include/cli_app.h:29-31（0 成功 / 1 操作失败 / 2 用法错误），
"已有另一个实例"是 3（include/application_instance_lock.h:43）。

几条写进代码的约束：

- 用法错误在**碰任何持久状态之前**返回（app/backupctl.cpp:63）。
- 未知命令先判、再抢锁（app/backupctl.cpp:164-171）；`--help` 不需要锁
  （app/backupctl.cpp:152-158）。
- `--config-file` 等单值选项重复出现直接报错，不做"后者覆盖前者"
  （app/backupctl.cpp:79-114）。
- 密码只从 `/dev/tty` 交互读取，命令行、环境变量、管道都不接受
  （src/cli/terminal_secret.cpp:128-129、:158-160；cli_commands.cpp:492-503）；
  用完立刻清零（cli_commands.cpp:782-784）。

legacy v0.1 归档的**写入**不是产品功能：产品 CLI 里没有任何一条路径能生成任意路径的
v0.1 归档，那项能力只在测试夹具 tests/tools/archive_cli.cpp 里（app/backupctl.cpp:34-41、
Makefile:127-148）。读取兼容一直保留（include/backup_engine.h:48-49）。

### 4.2 Modern GUI：`ui/modern/*`（7 页 + 6 个 controller）

页面在 ui/modern/qml/Main.qml:285-344，页面数在 ui/modern/dev_harnesses.h:70
（`const int kPageCount = 7;`），自动化检查也按这个数字逐页切页：

| # | 页面 | QML | 背后的 controller |
|---|---|---|---|
| 1 | 首页 Home | qml/pages/HomePage.qml（Main.qml:285） | BackupController |
| 2 | 备份 Backup | qml/pages/BackupPage.qml（:296） | BackupController |
| 3 | 自动备份 Schedule | qml/pages/SchedulePage.qml（:307） | ScheduleController |
| 4 | 备份管理 BackupManagement | qml/pages/BackupManagementPage.qml（:316） | BackupController |
| 5 | 设置 Settings | qml/pages/SettingsPage.qml（:326） | BackupController（仓库路径） |
| 6 | 实时备份 Realtime | qml/pages/RealtimePage.qml（:335） | RealtimeController |
| 7 | 远程备份 Remote | qml/pages/RemotePage.qml（:344） | RemoteController |

6 个 controller / 模型（前四个与三个 FilterRuleModel 实例在 ui/modern/main.cpp:449-489 构造；
SshTunnelManager 是 RemoteController 的成员，见 remote_controller.h:982）：

| 构件 | 文件 | 职责 |
|---|---|---|
| `BackupController` | backup_controller.cpp / .h | 手动备份、恢复、预览、仓库与记录列表 |
| `ScheduleController` | schedule_controller.cpp / .h | 计划页的桥，转发给 ScheduledBackupService |
| `RealtimeController` | realtime_controller.cpp / .h | 实时页的桥，转发给 RealtimeBackupService |
| `RemoteController` | remote_controller.cpp / .h | 远程页的桥，背后是 RemoteArchiveClient |
| `FilterRuleModel` | filter_rule_model.cpp / .h | 规则编辑模型（三个页面各一份实例） |
| `SshTunnelManager` | ssh_tunnel_manager.cpp / .h | GUI 自管的 SSH 安全通道（remote_controller.h:982 持有） |

其它支撑件：`OperationGate`（ui/modern/operation_gate.h:33，同一时刻只有一个会改动持久状态的
业务操作）、`AppTheme`、`schedule_frequency.cpp`（界面频率 ↔ 语义换算）、
`dev_harnesses.cpp`（验收自检代码，`main.cpp` 只留启动路径）。
GUI 侧的验收由 scripts/modern_gui_check.sh 驱动，走的是同一批控制器代码路径
（例如 --self-test 在 scripts/modern_gui_check.sh:1009，仓储路径在 :1437）。

GUI 不做的事：不解析归档、不实现 glob、不自己取 application lock
（ui/modern/main.cpp:466-468 说明了原因：flock 绑在 open file description 上，
GUI 主进程已经按 per-UID 持有那把锁）。远程网络 I/O 不占用 OperationGate，
因为它不改动本地仓库的持久状态（ui/modern/main.cpp:476-479）。

### 4.3 Core：`src/core/*`

| 文件 | 职责 | 关键位置 |
|---|---|---|
| archive_pipeline.cpp | 打包/压缩/加密的编排，v2 容器的读与写 | RunBackupPipeline:467、RunBackupPipelineFromEntries:515、RunRestorePipeline:1047、RunRestorePackedStream:1231、IdentifyArchiveFile:1470、VerifyContainerPayloadBytes:1411 |
| backup_engine.cpp | 参数层薄封装：查源目录类型，然后交给 Writer/Reader/pipeline | Backup→ArchiveWriter:38-79、Restore→Reader:86-128、v2 入口:132-154 |
| file_io.cpp | 发布原语、原子写、sink/source | PublishNoReplace:564、PublishReplacing:634、WriteFileAtomicallyReplacing（rename 在 :790） |
| tree_scanner.cpp | 一次扫描得到 `ArchiveEntry` 列表（Filter 决策在这里被调用） | ScanSourceTree:196 |
| source_tree_walker.cpp | 遍历与失败分类（lstat / opendir / readdir 的错误分开报） | include/source_tree_walker.h:97-194 |
| user_directory.cpp | uid/gid → 名字的唯一解析入口（带缓存） | include/user_directory.h:1-38 |
| backup_mode.cpp | 触发方式 × 策略 × 算法的支持矩阵 | 支持表:57-69、IsSupportedBackupMode:126、组合校验:153 |
| backup_option_keys.cpp | CLI 与 GUI 共用的一张算法 key 表 | — |
| backup_preview.cpp | 预览：不创建归档、不写状态 | include/backup_preview.h:69-155 |
| simple_json.cpp | 状态文件的文本编解码 | — |
| format_bytes.cpp / source_digest.cpp | 展示与摘要工具 | — |

`BackupEngine` 的公开签名与行为见 include/backup_engine.h:48-49：它按 magic 判断
"v0.1 legacy 走 ArchiveReader，v2 容器走 container pipeline"，不看扩展名。

### 4.4 Filter：`src/filter/*`

职责：决定哪些条目进入归档（include/filter.h:1-13）。它在归档写入之前，
只看 `lstat` 能拿到的元数据，不读文件内容（include/filter.h:8-10）。

- 11 个字段：`name / path / stem / ext / type / size / mtime / uid / gid / user / group`
  （include/filter_rule_builder.h:24-35）。
- `exclude` 优先；有 include 时普通文件至少命中一条 include（include/filter.h:27）。
- glob 用 DP 匹配，避免朴素回溯在病态模式上指数爆炸（src/filter/filter.cpp:143）。
- 子树剪枝、文件判定、特殊条目判定分别是
  `ShouldPruneDirectory`（src/filter/filter.cpp:719）、`ShouldIncludeFile`（:754）、
  `ShouldSkipSpecialEntry`（:768）；规则解析入口 `AddRule`（:783）。
- `FilterEntry` 的类型/属主字段是后补的，只填旧字段的调用方行为一字不变
  （include/filter.h:33-52）。
- 规则构造中间层在 src/filter/filter_rule_builder.cpp（给 GUI 的规则编辑器用），
  语法裁决仍然只有一条路：`FilterRuleBuilder → Filter::AddRule`（ui/modern/main.cpp:486）。
- 语法与语义见 docs/filter_usage.md，明确不做的事见 docs/backlog/filter_future.md。

### 4.5 Archive / Pack：`src/archive/*`

打包层产出的统一叫 "packed 流"，压缩与加密只把它当成一段字节
（include/pack_stream.h:1-11）：

| PackMethod | id | 实现 | 说明 |
|---|---:|---|---|
| `kMyPack` | 0 | src/archive/mypack_v2.cpp | `BKPARCH\0` + version 2 的扩展格式 |
| `kUstar` | 1 | src/archive/ustar.cpp | 标准 POSIX USTAR，能与 GNU tar 互通 |
| `kFastUstar` | 2 | src/archive/ustar.cpp | wire format 与 USTAR 相同，只改 I/O 策略 |

- MyPack v2 常量：全局头 32 字节、entry 头 64 字节（include/pack_stream.h:38-50）。
- USTAR 的边界写得很清楚：八进制字段放不下就明确失败、不 follow 软链接、
  读侧先 preflight（include/ustar.h:10-28）；pax / GNU 扩展一律不支持。
- 旧格式 v0.1 的读写留在 src/archive/archive.cpp：
  `ArchiveWriter::Write`:1136/:1149、`ArchiveReader::Extract`:1246、
  `InspectHeader`:1370、preflight:830。v0.1 的头部布局见 docs/format/archive_v0.1.md。
- 归档内部路径的注册与去重：src/archive/archive_path.cpp（include/archive_path.h:45）。
- 外层容器（BKPCNT2，固定 160 字节头）在 src/archive/container_format.cpp：
  Encode:143、Decode:212、InspectContainerFile:435、DeriveKeys:481。
  完整布局表见 docs/format/archive_v2_container.md。

### 4.6 Compression：`src/compression/*`

两个手写编解码，不链接任何第三方压缩库（include/compression.h:25）：

- `HUF1`：Canonical Huffman，276 字节头（256 字节码长表 + 原始长度 + 有效 bit 数），
  见 include/compression.h:11-16。流式入口 src/compression/huffman.cpp:679 / :726。
- `LZH1`：LZSS + Canonical Huffman，20 字节头，内层是一个完整的 HUF1 流，
  见 include/compression.h:18-23。窗口 32 KiB、最短匹配 3、最长 258
  （include/compression.h:43-49）。流式入口 src/compression/lzss.cpp:659 / :730。
- 产品流水线只走 Stream（文件到文件）接口，峰值内存与输入大小无关
  （include/compression.h:83-91）；LZSS 的 token 流落在私有临时文件里
  （src/core/archive_pipeline.cpp:16-20）。
- 公共 I/O 适配层：bit 读写、滚动窗口、顺序读，src/compression/codec_io.cpp:226/:274/:305。

### 4.7 Crypto：`src/crypto/*`

全部手写，不依赖 OpenSSL 之类的库（include/x25519.h:5 说明了动机）。

| 原语 | 位置 |
|---|---|
| SHA-256 / SHA-512 | include/crypto.h:58 / :88 |
| HMAC-SHA256 | include/crypto.h:120 |
| PBKDF2-HMAC-SHA256 | src/crypto/pbkdf2.cpp:39（容器里 200000 轮，include/container_format.h:72） |
| AES-256-CTR | src/crypto/aes.cpp:365（Process:402） |
| DES-CBC（legacy / educational） | src/crypto/des.cpp:261 / :352 |
| CSPRNG（getrandom / /dev/urandom） | src/crypto/random.cpp:124 |
| HKDF-SHA256（BPSEC1 会话密钥） | src/crypto/hkdf.cpp:25 / :48 / :85 |
| X25519（密钥协商） | src/crypto/x25519.cpp |
| Ed25519（离线根签名，RFC 8032） | src/crypto/ed25519.cpp:3 |
| BPCERT1 证书编解码 | src/crypto/bpcert.cpp:3（唯一实现） |
| 可信根存储 | src/crypto/trusted_root_store.cpp:5 |

安全措辞在头文件里就写死了：本模块是课程项目的手写实现，未经过专业密码学审计；
DES-CBC 是 legacy / educational，AES-256-CTR + HMAC-SHA256 + PBKDF2 + 随机 salt/IV +
Encrypt-then-MAC 是本项目能给的最强组合，但同样不是经过审计的密码学产品
（include/container_format.h:13-16）。密钥材料在析构时清零
（src/network/secure_transport.cpp:802-830、src/crypto/aes.cpp:318）。

### 4.8 Scheduler：`src/scheduler/*`

```text
到点 → 扫描源目录 → 与上一份成功 manifest 比较
     → 没变化：skip（不调用引擎、不产生 .bak、不动 manifest）
     → 有变化：Full 建一份完整 v2 快照；Incremental 交给共享增量引擎
     → 成功后落盘 manifest / state → retention → 记录 history
```

- 服务本体：src/scheduler/scheduled_backup_service.cpp（Evaluate:478、EvaluateNow:484、
  EvaluateInternal:494）。状态枚举与"为什么要区分 kFailed 与 kConfigInvalid"
  见 include/scheduled_backup_service.h:44-69。
- 它**不取系统时间**：`now_sec` 由调用方注入，单元测试可以喂假时钟
  （include/scheduled_backup_service.h:14-18）。
- Full 路径用 metadata-first 的 manifest 比较（src/scheduler/source_manifest.cpp:511
  `DiffManifests`），并且如实写下已知盲区：same-size + same-mtime 的人为原地改写
  逃得过这一版检测（include/source_manifest.h:15-21）。
- Incremental 路径不走上面那套 metadata 比较，而是把结论交给共享增量引擎
  （src/scheduler/scheduled_backup_service.cpp:668-735，调用在 :703）。
  原因写在同处 668-673：内容身份必须是真实摘要，否则漏掉的那一次变化会成为
  所有后代的错误祖先。
- 持久化：schedule.json 的读写、state / history / baseline / manifest 由
  src/scheduler/schedule_store.cpp 负责（Load:1253、Save:1293、LoadManifest:1394、
  SaveManifest:1444）。进程互斥用 `SchedulerLock`（src/scheduler/scheduler_lock.cpp），
  锁顺序固定为 ApplicationInstanceLock → SchedulerLock（app/backupctl.cpp:50-54）。
- 依赖方向是单向的：ScheduledBackupService → ScheduleStore / BackupCatalog /
  BackupEngine；反过来不存在，所以删掉 schedule.json 之后仓库里的 .bak 照样能
  列出、恢复、删除（include/scheduled_backup_service.h:24-30）。
- 计划路径不接受加密：无人值守没有安全的持久口令来源（include/backup_mode.h:112-117、
  src/scheduler/scheduled_backup_service.cpp:699-700）。

### 4.9 Realtime：`src/realtime/*`

四个构件串成一条链：

| 构件 | 文件 | 职责 |
|---|---|---|
| `InotifyWatcher` | realtime_watcher.cpp（Attach:281、Rebuild:351、Drain:391） | 递归 inotify，只回答"哪里变了"；结构变化后整体重建，新 fd 建好才关旧的（include/realtime_watcher.h:16-23） |
| `RealtimeDebouncer` | realtime_debouncer.cpp（NoteEvents:46、Consume:74） | 把事件流合并成"一批" |
| `RealtimeStore` | realtime_store.cpp（Load:586、Save:721） | realtime.json 的读写与路径重叠校验（include/realtime_store.h:74） |
| `RealtimeBackupService` | realtime_backup_service.cpp（RunRealtimeBackupOnce:824、retention:722、marker:495） | 真正跑一次备份 |

- watcher **不**决定备份集合：Filter 与类型判定全部留在既有 core，不在 watcher 里复制
  第二套语义（include/realtime_watcher.h:13-14）。
- 策略支持 Full 与 Incremental（src/core/backup_mode.cpp:68-69），
  实现上 Full 走 `BackupEngine`、Incremental 走 `RunIncrementalBackup`
  （src/realtime/realtime_backup_service.cpp:819-906）。
- 溢出与源根丢失是真事件：`IN_Q_OVERFLOW` → resync，`root_lost` 单独上报
  （include/realtime_watcher.h:37-51）。
- 快照的来源用仓库里的 marker 文件记录，淘汰只动 realtime 自己管理的快照
  （src/realtime/realtime_backup_service.cpp:252、:597、:722）。

### 4.10 Network client：`src/network/*`

- 协议编解码：src/network/network_protocol.cpp（帧头 32 字节全大端、
  每条客户端给出的长度先过上限再谈分配、帧边界明确，include/network_protocol.h:8-31）。
- 客户端：src/network/remote_backup_client.cpp。`Connect`:255 → BPSEC 握手
  （:305-315，握手失败就关连接，**没有**退回明文的分支）→ `Login`:614 /
  `ResumeSession`:432 → `List`:669 / `UploadSnapshotFile`:745 / `DownloadArchiveFile`:918 /
  `Delete`:1119 / `DeleteAccount`:1140。
- 客户端不解析归档内容：送出去的是本地已经生成并验证过的 .bak 字节
  （include/remote_backup_client.h:12-13）。
- 身份三种方式（签名身份证书 / 手工指纹 pin / SSH 隧道里再用 pin）在
  src/network/server_profile.cpp:259-280 做唯一翻译；头文件说明见
  include/server_profile.h:11-12。
- 远端增量：src/network/remote_incremental.cpp。
  三件套下载与逐字节验证（`FetchSnapshotMaterial`:378、`EnsureChainMaterial`:472）、
  本地已验证缓存（`PrepareRemoteCache`:553）、远端链解析（`ResolveRemoteChain`:595）、
  `RunRemoteBackup`:703、`RunRemoteRestore`:846、原始归档的
  staging + 原子发布会话（`RemoteRawRestoreSession`:1022-1226）。
- 三条硬规则写在 include/remote_incremental.h:15-24：服务端元数据只用来定位不用来信任；
  缓存里没有凭据；链不能从中间断开。

### 4.11 Secure transport：`src/network/secure_transport.cpp`

BPSEC1 是本项目自己的认证加密传输层，位于 BPNET1 之下，不是 TLS，也不打算与 TLS 兼容
（include/secure_transport.h:3-28）。

- 分层：BPNET1 帧 → BPSEC1 记录（seq + 密文 + HMAC）→ TCP
  （include/secure_transport.h:9-11）。
- 握手：X25519 + HKDF-SHA256 派生双向密钥；客户端 `HandshakeClient`:914、
  服务端 `HandshakeServer`:1221；"要求证书"的变体在 :919 / :1226。
- 记录层：`SendRecord`:1436、`ReceiveRecord`:1512、`SendFrame`:1675、`ReceiveFrame`:1704。
  一条 record 恰好装一个完整 BPNET1 帧，不合并、不拆分（src/network/secure_transport.cpp:17）。
- 不降级：握手失败就是失败，绝不回退到明文（include/secure_transport.h:28）；
  服务端被配置成只接受 BPSEC2 时，连 BPSEC1 的 ClientHello 都拒绝
  （src/network/secure_transport.cpp:1295）。
- 身份校验顺序写在一个地方：结构 → 可信根 → 根签名 → server_id → 有效期 →
  证书公钥等于握手公钥，任一步失败即终止（src/network/secure_transport.cpp:927-928、:1054）。
- 会话密钥与序号在 Reset / 析构时清零（src/network/secure_transport.cpp:802-830）。

### 4.12 Server：`server/*`

- 定位：存储后端 + 传输边界。监听、帧编解码、连接状态机、用户认证、SQLite 元数据、
  流式落盘、原子发布、安全删除（include/remote_server.h:5-16）。
- 并发模型刻意简单且有界：主线程 accept，`worker_count` 个固定 worker 线程，
  排队留在 listen backlog，不存在"每连接 new 一个 detached 线程"
  （include/remote_server.h:18-26）。SQLite 访问用一把互斥锁串行化 + busy timeout。
- 入口 server/main.cpp：只解析参数、查 PID 文件、把信号变成一次优雅停止、跑服务循环；
  信号处理器只做一次原子写（server/main.cpp:5-13、:44-51）。
- 数据目录锁：`<root>/.backup-server.lock` 上的 flock 独占锁，同时是
  "同一数据目录只有一个写者"和"admin 判断服务端是否在运行"的依据
  （include/remote_server.h:28-32）。
- opcode 分派：server/remote_server.cpp:2177-2201（REGISTER / LOGIN / LOGOUT / RESUME /
  LIST / UPLOAD_BEGIN·CHUNK·END / DOWNLOAD_BEGIN·CHUNK·END / DELETE / DELETE_ACCOUNT）。
- 认证：include/remote_auth.h:7-15 —— PBKDF2-HMAC-SHA256 + 每用户随机 salt，
  绝不存明文、绝不存 `SHA256(password)`、绝不用固定 salt；token 是无状态 HMAC 签名，
  12 小时有效（include/remote_auth.h:28-41）。
- 运维工具与主进程共用同一份实现：admin 的删除动作与服务端的 DELETE / DELETE_ACCOUNT
  走同一批函数（Makefile:172-174）；keygen 与证书工具分别共用 secure_transport.cpp 与
  bpcert.cpp / trusted_root_store.cpp（Makefile:180-210）。
- 加固审计结论见 docs/server-hardening-audit.md。

### 4.13 Remote metadata：`server/remote_metadata_store.cpp`

- 表：`users`（:68）、`snapshots`（:76）、`deleted_users`（:94）；
  索引 `snapshots_by_user`（:98）、`snapshots_by_parent`（:109）。
- 版本化 schema：版本号就是 `PRAGMA user_version`，升级只做
  `ALTER TABLE ADD COLUMN` 与 `CREATE INDEX IF NOT EXISTS`（:114-118、:421）。
  打开时先 `VerifyExistingSchema`（:437），不匹配就拒绝，而不是"凑合用"。
- 关键操作：`CreateUser`:652、`FindUser`:736、`InsertSnapshot`:830、`ListSnapshots`:1053、
  `FindSnapshot`:1092、`DeleteSnapshot`:1135、`CountSnapshotChildren`:1234、
  `CountSnapshots`:1267、`StorageOverview`:1350、`DeleteUser`:1416。
- 行解码只有一个地方（:159 起），列下标、CREATE TABLE 与 ALTER TABLE 的列序三者必须一致
  （:21）。

### 4.14 Incremental：`src/core/incremental_*.cpp` + `src/network/remote_incremental.cpp`

三个文件分工明确：

| 文件 | 职责 |
|---|---|
| src/core/incremental_delta.cpp | BKPINC1 信封的编解码（唯一实现，:9）；定长头 24 字节（:721-729） |
| src/core/incremental_backup.cpp | 基线发现（`FindIncrementalBaseline`:932）、身份与 manifest 验证（`LoadVerifiedSnapshotIdentity`:808）、写快照与副文件（`PublishSnapshotSidecars`:384）、主入口（`RunIncrementalBackup`:1209）、依赖感知保留（`PlanDependencyAwareRetention`:1089） |
| src/core/incremental_restore.cpp | 依赖链解析（`ResolveSnapshotChain`:479）与恢复（`RestoreSnapshotChain`:638） |

设计要点：

- delta 必须是**另一种顶层 magic**：复用 BKPCNT2 会让旧 reader "成功"恢复出一棵只含
  变化部分的树，那是静默错误，比明确拒绝危险（include/incremental_delta.h:5-11）。
- 一份快照在仓库里是三件套：`<name>.bak` + `<name>.manifest` + `<name>.identity`
  （BPIDENT2）。identity 记录 snapshot_file_name / snapshot_id / manifest_digest /
  format_version，把"这份副文件属于哪个 .bak"钉死（include/incremental_backup.h:63-88）。
- 身份不是"读得出来的声明值"，而是**实际字节**：payload 必须通过
  `VerifyContainerPayloadBytes`（include/incremental_backup.h:119-122）。
- 增量第一版只支持 MyPack、不支持加密，而且是明确拒绝，不是"先跑一次基线、
  第二次 delta 才失败"，也不是静默降级（include/incremental_backup.h:38-57）。
- 上限：链深 64、tombstone 200 万、单 delta 条目 200 万
  （include/incremental_delta.h:71-75）。
- 远端侧只做"下载 + 验证 + 翻译成本地仓库"，增量算法本身一行都没有重写
  （include/remote_incremental.h:5-13）。

### 4.15 Packaging：`packaging/` + `scripts/stage-*-release.sh`

- 分层：`scripts/stage-client-release.sh` / `scripts/stage-server-release.sh` 产出可直接发出去的
  目录（scripts/stage-client-release.sh:8-16、scripts/stage-server-release.sh:7-16）；
  `packaging/build-release.sh` 在其上做 AppImage / deb / tar.xz，只加启动器、配置、
  systemd unit 与安装脚本，不改写内容（packaging/build-release.sh:15-23）。
- 三条性质：可复现（SOURCE_DATE_EPOCH = commit 时间、固定 TZ/LC_ALL、固定排序与
  owner/group）、不夹带私钥、包装而不是重写（packaging/build-release.sh:15-20）。
- 工作树不干净默认拒绝打包，要覆盖必须显式 `--allow-dirty` 并写进 BUILD-INFO.txt
  （scripts/stage-client-release.sh:17-23、:48-57）。
- 当前 main 上 `dist/client` 9 个文件、`dist/server` 13 个文件（数字含各自的
  `MANIFEST.sha256`），两个 staging 脚本都会做冒烟（ldd 无 not found、
  CLI `--help`、GUI offscreen 存活）。
- 包内容与核对方式见 docs/release-layout.md:19-59。

---

## 5. 关键不变量

| # | 不变量 | 代码位置 | 违反时的行为 |
|---|---|---|---|
| 1 | 归档发布**不覆盖**已有文件 | src/core/file_io.cpp:564（先 link，再 renameat2(RENAME_NOREPLACE)，都不可用就 fail closed，:602-628）；O_EXCL 创建在 :164 | 明确失败并报出两个 errno，不退回非原子 rename |
| 2 | 恢复目标必须不存在或为空 | src/core/incremental_restore.cpp:653-674；include/archive_pipeline.h:74-78 | 直接拒绝；失败时 destination 一定不出现 |
| 3 | 口令不落盘、不进 argv、不进环境变量 | 只从 /dev/tty 读（src/cli/terminal_secret.cpp:128-129）；容器选项只在内存（include/archive_pipeline.h:37-40）；config_manager 里没有任何 password 字段 | 非交互终端直接失败；口令用完清零（src/cli/cli_commands.cpp:782-784） |
| 4 | 依赖链删除 fail-closed | 本地：src/core/incremental_backup.cpp:1085-1141（链走不完整就清空 remove）；服务端：server/remote_maintenance.cpp:196-213（还有后代就不许删，检查在动磁盘之前） | 宁可不回收，也不删掉一条恢复链的祖先 |
| 5 | 链上的每一跳都要验证，不按文件名顺找 | include/incremental_restore.h:9-14；src/core/incremental_backup.cpp:808、:932 | 链解析失败即拒绝，不"尽力恢复" |
| 6 | 不做静默降级 | 传输：include/secure_transport.h:28、src/network/secure_transport.cpp:1295；策略：include/backup_mode.h:65-68；增量：include/incremental_backup.h:47-48 | 明确报错；"选了增量就偷偷按全量跑"被禁止 |
| 7 | 全应用同一时刻只有一个进程；一次只有一个写者 | 锁：app/backupctl.cpp:173-193、ui/modern/main.cpp:466-468；写者闸门：ui/modern/operation_gate.h:33 | CLI 退出码 3；GUI 把按钮与任务都挡住 |
| 8 | 失败不留半成品 | src/core/archive_pipeline.cpp:36-37；src/core/file_io.cpp:559-563 | 中间产物只在私有工作目录，失败连工作目录一起清掉 |
| 9 | 服务端不认识归档格式 | Makefile:55-60；include/remote_server.h:10-16 | 结构上不允许出现第二个 BackupEngine |
| 10 | 错误信息不含秘密 | include/remote_backup_client.h:15-16；include/remote_auth.h:72-73 | 只回错误码与分类，不把 token / 口令 / 服务端路径带出去 |

---

## 6. 增量恢复的执行顺序

这是增量功能最需要说清的一段，代码在 src/core/incremental_restore.cpp:638-796，
顺序是固定的五步：

```text
1. base snapshot
   chain.files.front() 走既有的完整恢复路径
   → RunRestorePipeline(base, staging)                 :695-699

2. delta chain
   for index = 1 .. N：
     ReadDeltaEnvelope(delta)                          :708
     ExtractDeltaPayload(delta, inner_container)       :709
     RunRestorePipeline(inner_container, overlay)      :711   ← 每个 delta 一个全新 overlay
     tombstone：深的先删，不允许穿过软链接祖先           :717-744
     MergeTree(overlay → staging)                      :751
     overlay 根的 metadata 单独贴一次                    :752-758
   Δ1 → Δ2 → … → ΔN 逐层叠加，绝不在 staging 上直接解包

3. preflight
   链解析在动任何东西之前：ResolveSnapshotChain         :643-647
   destination 必须不存在或为空                         :653-674
   每一步都走 LoadVerifiedSnapshotIdentity（实际字节）  （incremental_backup.cpp:808）

4. staging restore
   staging / overlay / inner_container 是 destination 的兄弟目录
   （带 pid 后缀），进入时先清残留                      :676-684
   任何一步失败都跳到统一清理，只有 ok == true 才算成功  :686-694、:788-795

5. atomic publish
   destination 若是空目录先 rmdir                      :767-776
   然后 PublishReplacing（单次 rename + 同步父目录）    :784
   rename 成功后不做任何"失败就把 destination 删掉"的补救 :780-783
```

两个容易写错的细节，代码里专门加了注释：

- "这一轮 delta 失败了"必须由布尔量表达，不能去读 `error_message`：它是可选出参，
  拿它当状态机在传 nullptr 时会失效，于是一次失败的恢复被报成成功
  （src/core/incremental_restore.cpp:689-693）。
- overlay 保证 tombstone 看到的一定是 base 应用完之后的真实状态，而不是"解到一半"
  的中间结果（src/core/incremental_restore.cpp:700-704）。

---

## 7. 磁盘与线上格式清单

| 格式 | magic | 定义位置 | 文档 |
|---|---|---|---|
| Archive v0.1（legacy，仅读兼容） | `BKPARCH\0` | src/archive/archive.cpp:24、include/archive.h | docs/format/archive_v0.1.md |
| MyPack v2 packed 流 | `BKPARCH\0` + version 2 | include/pack_stream.h:36-50、src/archive/mypack_v2.cpp:6 | docs/format/archive_v2_container.md |
| v2 外层容器 | `BKPCNT2\0`（160 字节头） | include/container_format.h:48-80、src/archive/container_format.cpp:143/212/435 | docs/format/archive_v2_container.md |
| USTAR / fast-ustar | POSIX tar（无自定义 magic） | include/ustar.h:1-28、src/archive/ustar.cpp | docs/format/archive_v2_container.md（pack 方法一节） |
| HUF1 | `HUF1` | include/compression.h:11-16 | docs/format/archive_v2_container.md（压缩一节） |
| LZH1 | `LZH1` | include/compression.h:18-23 | docs/format/archive_v2_container.md（压缩一节） |
| BKPINC1（增量 delta 信封） | `BKPINC1\0`（24 字节定长头） | include/incremental_delta.h:13-25、src/core/incremental_delta.cpp:721-729 | docs/remote_incremental.md:27 |
| BPIDENT2（快照身份副文件） | 规范化文本 | include/incremental_backup.h:63-89、src/core/incremental_backup.cpp:581 | docs/remote_incremental.md:29 |
| manifest 副文件 | 规范化文本 | include/source_manifest.h、src/scheduler/source_manifest.cpp:652 | docs/scheduled_backup_usage.md |
| BPSNAP1（材料包） | `BPSNAP1\0` | include/snapshot_bundle.h:12-27、src/network/snapshot_bundle.cpp:286 | docs/remote_incremental.md:35 |
| BPNET1（线上协议） | `BPN1`（4 字节，大端） | include/network_protocol.h:18-31、src/network/network_protocol.cpp:155 | docs/network_backup_usage.md:3 |
| BPSEC1（认证加密传输） | `BPS1`（握手与记录层） | include/secure_transport.h:53、src/network/secure_transport.cpp:1426 | docs/secure_transport.md |
| BPSEC2（BPSEC1 + 服务器身份证书） | 沿用 `BPS1`，握手版本升到 2 | include/secure_transport.h:57-71、src/network/secure_transport.cpp:260-261 | docs/bpsec2-design.md |
| BPCERT1（服务器身份证书） | `BPCERT1` | include/bpcert.h:3-19、src/crypto/bpcert.cpp:3 | docs/bpsec2-design.md:96 |
| schedule.json / realtime.json / config.json | 规范化文本 | src/scheduler/schedule_store.cpp:1253/1293、src/realtime/realtime_store.cpp:586/721、src/config/config_manager.cpp | docs/scheduled_backup_usage.md |

所有自有格式的共同约定：整数手工按 little-endian 或 big-endian 逐字节拼接，
不把 C++ struct 直接写进文件；保留字段必须真的为 0，读侧不"反正没人用"地放过它
（include/byte_order.h、include/pack_stream.h:47-48、include/network_protocol.h:10-11）。

---

## 8. 错误处理与资源管理

约定在核心层基本一致，写下来是为了后面改动时不走样：

1. **返回值 + 可选错误文本**。函数返回 `bool`（或状态枚举），失败时把面向用户的
   原因写进 `std::string* error_message`；该指针允许为 `nullptr`，所以任何状态判断
   都不能依赖它（src/core/incremental_restore.cpp:689-693 就是这个坑的现场记录）。
2. **错误文本三段式**：`动作: 路径: strerror`（src/core/archive_pipeline.cpp:77-80、
   src/archive/archive.cpp:93）。调用方拿到的是能直接给用户看的一句话。
3. **errno 不被伪装**。发布原语在失败时把 `link()` 与 `renameat2()` 两个 errno 都原样
   报出来，只有真正"这个内核/文件系统不支持"才标注 unsupported
   （src/core/file_io.cpp:609-626）。
4. **RAII 管资源**：`FileSink` / `FileSource` / `TempDirectoryGuard`
   （include/file_io.h:54、:105、:178）；fd 与 `DIR*` 都是"析构即关闭"，
   核心不变量是"输出永远 0600 且 O_CREAT|O_EXCL（或 mkstemp）——不截断、不跟随"
   （src/core/file_io.cpp:11）。
5. **EINTR 重试、短读短写显式处理**：src/archive/ustar.cpp:447/:471、
   src/archive/archive.cpp:186、src/network/network_protocol.cpp:510、
   src/network/secure_transport.cpp:641、src/cli/terminal_secret.cpp:107。
6. **失败即清理**：写侧失败删除半成品归档与工作目录；读侧先 preflight 再落盘
   （include/archive_pipeline.h:58-78）。
7. **不留空窗**：realtime 重建 watch 时新 fd 建成功之后才关旧的
   （include/realtime_watcher.h:20-21）。
8. **进程入口不抛异常**：异常逃出 `main` 只会变成 `std::terminate`，
   所以 server 的错误全部走 stderr + 非 0 退出码（server/main.cpp:21-22）。

---

## 9. 并发、锁与单实例

| 锁 / 闸门 | 作用范围 | 位置 |
|---|---|---|
| `ApplicationInstanceLock` | 整个产品（backupctl / Classic GUI / Modern GUI 同一把锁、同一路径） | src/platform/application_instance_lock.cpp:68、:136；app/backupctl.cpp:173-193 |
| `SchedulerLock` | 计划备份的评估与执行 | src/scheduler/scheduler_lock.cpp；锁顺序 app/backupctl.cpp:50-54 |
| `OperationGate` | GUI 内"同一时刻只有一个改动持久状态的操作" | ui/modern/operation_gate.h:33；ui/modern/main.cpp:445-448 |
| `<root>/.backup-server.lock`（flock） | 服务端数据目录的唯一写者 | include/remote_server.h:28-32 |
| 服务端 SQLite 互斥 + busy timeout | 元数据的并发写 | include/remote_server.h:24-26 |

`backup-server` 不参与前端的单实例锁：它是独立服务进程，自己有 PID 文件 +
`/proc` 复核（server/main.cpp:8-13、:19）。

核心本身没有全局可变状态；同一目标路径上的并发调用不靠互斥，而是靠
"目标必须为空" + 内核的 no-replace 语义兜住（src/core/archive_pipeline.cpp:39-40、
include/archive_pipeline.h:74-78）。

---

## 10. 已知限制

如实列出，不藏的才叫限制：

1. **TOCTOU**：路径检查通过之后按 pathname 再使用，不抵抗恶意并发进程在检查与使用
   之间替换路径组件。仓库层（include/backup_catalog.h:30-38）与归档层
   （src/archive/archive.cpp:750）都写明了这一点；更强的做法是 dirfd + openat2，
   当前版本没有实现。
2. **手写密码学未审计**：DES-CBC 明确是 legacy / educational，AES 组合也不是
   经过审计的密码学产品（include/container_format.h:13-16）。
3. **v0.1 只读兼容**：产品里没有生成 v0.1 归档的路径，写入能力只在测试夹具
   （app/backupctl.cpp:34-41、Makefile:127-148）。
4. **计划路径的 Full 变化检测是 metadata-first**：same-size + same-mtime 的人为
   原地改写逃得过检测；增量路径用内容摘要，不受这条影响
   （include/source_manifest.h:15-21、src/scheduler/scheduled_backup_service.cpp:668-673）。
5. **增量第一版只支持 MyPack、不支持加密**，理由是信封明文不受内层 HMAC 覆盖
   （include/incremental_backup.h:38-57）。
6. **自动触发（计划 / 实时）一律不加密**：无人值守没有安全的持久口令来源
   （include/backup_mode.h:112-117）。
7. **服务端 token 无状态**：12 小时内无法单独吊销，只能等过期或注销账户
   （include/remote_auth.h:10-13）。
8. **服务端并发有上限**：最大并发客户端 = `worker_count`，SQLite 写入串行化
   （include/remote_server.h:18-26）。
9. **仓库列表只读 header**：Catalog 不解析 entry、不读 payload、也不要求密码，
   所以列表里的记录"不一定是能用的归档"，recognized_archive=false 时带诊断原因
   （include/backup_catalog.h:14-19、:64-68）。
10. **链的规模有硬上限**：深度 64、tombstone 200 万、单 delta 条目 200 万
    （include/incremental_delta.h:71-75）。
11. **realtime 依赖 Linux inotify**：`IN_Q_OVERFLOW` 之后必须 resync，这是设计内的
    路径而不是异常（include/realtime_watcher.h:5-11）。
12. **单实例是产品级约束**：同一台机器上同时只能跑一个前端进程（退出码 3），
    这是有意的，不是缺陷（app/backupctl.cpp:50-54）。
13. **服务端不解析归档**：它无法回答"这份快照内容是否可用"，那是客户端与本地核心
    的判断（include/remote_server.h:10-16）。

---

## 11. 与其它文档的分工

| 主题 | 看哪里 |
|---|---|
| 工程基线与评分对应 | docs/00_project_baseline.md |
| 需求与用例 | docs/01_requirements.md、docs/requirements/use_case_description.md |
| 测试分层与运行方式 | docs/03_testing.md |
| 归档格式细节 | docs/format/archive_v0.1.md、docs/format/archive_v2_container.md |
| 筛选规则语法 | docs/filter_usage.md |
| 定时备份 | docs/scheduled_backup_usage.md |
| 远程备份使用 | docs/network_backup_usage.md、docs/remote_incremental.md |
| 传输层设计与密码学选择 | docs/secure_transport.md、docs/bpsec2-design.md |
| 服务端部署与加固 | docs/install-server.md、docs/server-quick-start.md、docs/server-hardening-audit.md |
| GUI | docs/ui/modern_qml_gui.md、docs/ui/desktop_gui.md、docs/ui/filter_rule_builder.md |
| 发布与打包 | docs/release-layout.md、docs/release-packaging.md |
| UML（Sprint 1+2 阶段基线） | docs/uml/class.mdj、component.mdj、sequence_backup.mdj、sequence_restore.mdj |

docs/uml/ 下的图是 Sprint 1+2 阶段留下的证据，只表达当时存在的
`BackupEngine` / `FileSystem` / `ArchiveWriter` / `ArchiveReader`；
Scheduler、Realtime、增量、远程与压缩加密分支以本文和上表列出的文档为准。
