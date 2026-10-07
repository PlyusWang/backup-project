# 软件测试

> 文档编号：03
> 状态：本轮收尾（2026-10-07 实测）
> 验收基线：main = `6d1e2ba71a48862927f23442604d6bd4b430e62d`（PR #27 已合并）
> 说明：本文的数字全部来自一次真实执行，证据位置与复现命令见第 5、6 节。
> 本轮产品代码已冻结：只写文档，没有改动 `app/` `src/` `include/` `server/` `tools/` `ui/` 下的任何文件。

## 1. 测试对象与目标

这一轮要交付的不是单个功能，而是三样东西作为一个整体：

- 客户端 CLI `backupctl`：`backup` / `restore` / `preview` / `schedule` / `realtime` / `remote` / `repository` / `config`；
- Modern Qt 6 QML GUI，7 个页面（首页 / 备份 / 自动备份 / 备份管理 / 设置 / 实时备份 / 远程备份）；
- 四个服务端二进制：`backup-server`、`backup-server-admin`、`backup-server-keygen`、`backup-cert-tool`。

测试要回答三层问题：

1. **功能对不对**。备份出来的归档恢复回来是否与源目录逐字节相同；该拒绝的输入是否被拒绝，拒绝之后有没有留下半成品。
2. **失败的时候安不安全**。崩在写盘中间、磁盘写不进去、对端送来恶意字节流时，行为是不是 fail closed。
3. **能不能交出去**。归档格式能否与外部工具互操作、发行目录是不是拿了就能跑、代码规范是否达标。

第 2 节给当前的真实结果，第 3 节按层次说明每一层"测什么、怎么测、入口在哪、为什么这样测"，第 7 节写清楚哪些没测。

## 2. 当前真实结果

### 2.1 汇总

| 入口 | 本次结果 | 说明 |
| --- | --- | --- |
| `scripts/test.sh` | PASS=279 FAIL=0 | 功能套件（CLI 全链路） |
| `scripts/final_gate.sh` | 49/49，failed suites = 0 | 46 个套件 + 3 条构建记录 |
| `scripts/modern_gui_check.sh` | 通过 692 项 / 失败 0 | 其中 `--remote-test` 一个自检就有 212 条断言 |
| `scripts/quality_test.sh`（在 gate 内） | 66 例全过；稳定性 10/10 轮；sanitizer 41 次调用 0 报告 | 四个维度：可用性 / 鲁棒性 / 稳定性 / 健壮性 |
| release staging | client 9 个文件、server 13 个文件，冒烟全 PASS | 单独执行，不在 gate 里 |
| 编译 | 0 warning | `make all gui-all test-fixtures`、`make sanitize`、GUI 重建三次都是 0 |
| 静态门禁 | clang-format PASS；注释率 21.40%；超 80 列 actionable = 0 | `lint.sh` / `comment_ratio.py` / `source_style_check.py` |

### 2.2 运行环境

| 项目 | 实测值 |
| --- | --- |
| 操作系统 | Ubuntu 24.04.4 LTS，Linux 7.0.0-34-generic x86_64 |
| 编译器 | g++ (Ubuntu 14.2.0-4ubuntu2~24.04.1) 14.2.0 |
| 构建工具 | GNU Make 4.3，bash 5.2.21 |
| Qt | Qt 6.4.2（Qt6Core / Network / Quick / QuickControls2），测试一律 `offscreen` |
| 本地依赖 | libsqlite3 3.45.1、libgtest-dev 1.14.0、GNU tar 1.35、OpenSSL 3.0.13 |
| oracle 依赖 | Python 3.12.3 + cryptography 41.0.7 + PyNaCl 1.5.0 |
| sanitizer | AddressSanitizer + UndefinedBehaviorSanitizer（`make sanitize`，`-g -O1 -fno-omit-frame-pointer`） |
| 格式化检查 | clang-format 18.1.3 |

测试全程在 SSH 会话里执行，没有图形显示；GUI 相关的检查靠 `QT_QPA_PLATFORM=offscreen` 真的把窗口构造出来。

### 2.3 关于这些数字的两点说明

- `final_gate.sh` 里的 `test.sh` 报 **279**。在它之前，`quality_test.sh` 会先 `make clean`，那次基线跑出来是 **269**；差的 10 条是"没有 `build/backup-gui-modern` 就跳过 GUI 对比"的预览一致性用例。gate 的套件顺序（quality → 重建 GUI → 功能套件）就是为了让最后那次是 279。所以引用 PASS 数字时要连带说明 GUI 已构建。
- 这是**某一次运行**的结果，不是与负载无关的保证。换机器、换 Qt 小版本、换 tar 版本都可能改变个别条目（例如 qmllint 的放行条数）。要复核请按第 6 节从 clean 重跑。

## 3. 测试分层

| 层 | 入口 | 规模 | 主要回答 |
| --- | --- | --- | --- |
| 单元测试 | `tests/unit/`，由各套件脚本编译运行 | 57 个文件 | 每个模块自己的不变量 |
| 集成 / 端到端 | `scripts/test.sh`、`archive_pipeline_test.sh`、`ustar_test.sh`、`filter_*_test.sh` | 279 条 + 27 组合矩阵 | 用户拿到的东西对不对 |
| 故障注入 | `cleanup_contract_test.sh`、`bundle_source_mutation_test.sh`、`scheduled_backup_test.sh` D3/D5 区、`file_io_test.sh` | 清理合同 27 条 + 判别矩阵 13 条 + C0–C8 | 失败与崩溃时安不安全 |
| GUI 契约 | `scripts/modern_gui_check.sh` | 692 项 | 界面与核心之间的最后一跳有没有接上 |
| 网络回环 e2e | `network_test.sh`、`remote_incremental_test.sh`、`bpsec2_loopback_e2e.sh`、`login_throttle_test.sh`、`account_deletion_test.sh`、`ssh_tunnel_manager_test.sh` | — | 真进程之间的协议与时序 |
| 安全 oracle / fuzz | `sha512` / `ed25519` / `bpcert` / `bpsec2` / `secure_transport` 五套 | 30000 例 fuzz（bpcert 20000 + BPSEC2 10000） | 有没有独立证据，而不是自证 |
| 消毒剂 | `make sanitize` 后在 gate 里复跑 7 个套件 | 41 次调用 + 7 套 | 越界 / 溢出 / 泄漏 |
| release staging | `stage-client-release.sh` / `stage-server-release.sh` | 9 + 13 个文件 | 打出来的包能不能直接用 |
| 静态门禁 | `lint.sh` / `comment_ratio.py` / `source_style_check.py` | 183 个文件 | 规范与可读性 |

### 3.1 单元测试：`tests/unit/`（57 个文件）

57 个文件 = 55 个 `.cpp` + 2 个共享头（`test_support.h`、`remote_test_support.h`）。

**怎么测。** 刻意不引入测试框架——`test_support.h` 里写得很直白："多一个依赖就多一个跑不起来的理由"。断言形式是 `Check(条件, 用例名, 失败详情)`，打印 `PASS` / `FAIL`，末行固定汇总 `xxx: N/M checks passed`，有失败就非零退出。少数套件（`backup_catalog`、`filter_rule_builder`）装了 GoogleTest 就用，没装也能退回内置 harness，两种环境都必须跑得起来。

**入口。** 单元测试没有独立的 `make test-unit` 目标，而是由各个套件脚本用统一方式编译：

```bash
g++ -std=c++17 -Wall -Wextra -Wpedantic -Iinclude -Itests/unit \
    <test.cpp> <产品目标文件>
```

被测的是**产品目标文件本身**，不做链接替身。出现任何 `warning:` 直接判失败——不是"允许警告"，是"警告算失败"。

**代表性模块**（不逐个罗列）：

- 格式与容器：`archive_container_test`、`archive_pipeline_test`、`ustar_test`、`incremental_format_test`、`archive_mutation_test`；
- 增量与计划：`incremental_crash_test`、`incremental_closure_test`、`incremental_retention_test`、`scheduled_backup_test`、`scheduler_core_test`；
- 实时（inotify）：`realtime_watcher_test`、`realtime_debouncer_test`、`realtime_backup_test`、`realtime_retention_test`、`realtime_store_test`；
- 网络与账户：`network_protocol_test`、`remote_server_test`、`remote_client_test`、`remote_account_test`、`remote_metadata_store_test`（SQLite schema 迁移）、`remote_chain_test`；
- 密码学：`sha512_test`、`ed25519_test`、`bpcert_test`、`bpsec2_test`、`x25519_hkdf_test`、`crypto_test`；
- 过滤：`filter_semantics_test`、`filter_metadata_test`、`filter_rule_builder_test`、`legacy_filter_test`。

**为什么这样测。** 能在进程里精确造出来的状态，就不要靠外部条件碰运气。最典型的例子是崩溃一致性：那一组用例的状态是**造出来**的，不是杀进程杀出来的——造出来的状态更确定，而且正是崩溃会留下的那个（见 3.3）。

### 3.2 集成与端到端

`scripts/test.sh`（2497 行，本次 279 条）按分区组织：A 正常回环 / B 错误路径 / C 路径拓扑 / D 内容形态 / E 元数据 / F 损坏归档 / G 路径安全 / H 不支持的特殊文件 / I 证明 payload 没有被压缩 / J Writer 与 Reader 的路径规则对称 / K 文件筛选 / L 预览与增量。

判定约定写在脚本头部，这一层所有套件共用：

- 每个 `backupctl` 调用带 60 秒硬超时，用 `timeout --signal=KILL`（归档解析陷入死循环时 SIGTERM 未必叫得停），退出码 124 算失败；
- 成功用例必须真的核对磁盘结果：`diff -r`（结构、存在性、空目录）加全树 `sha256sum` 清单（diff 发现不了的同长度差异），大文件另加 `cmp` 逐字节；
- "被拒绝"的用例除了要求退出码正确（1，用法错误是 2）、错误信息里出现关键原因，还要求**没有留下任何文件**——拒绝同时不留痕才算合格；
- 被信号打死（退出码 >= 128）一律算失败。

代表性的端到端：

- **归档流水线**（`scripts/archive_pipeline_test.sh`）：3 种 pack × 3 种压缩 × 3 种加密 = 27 组合矩阵，每一组都跑 backup → restore → diff；另有元数据（07777 / uid / gid / mtime 秒+纳秒）、symlink / hardlink / FIFO / 设备节点语义、容器头截断与尾部垃圾、目标目录原子性、`RLIMIT_AS` 资源上限，以及一次确定性 mutation 扫描。
- **ustar 与 GNU tar 双向互操作**（`scripts/ustar_test.sh`）：A 我们写的 baseline 归档能被 `tar -tf / -tvf` 列出（条目数、大小、软链接目标、硬链接、目录、FIFO 类型都对得上）；B Fast USTAR 与 baseline 的条目数一致；C `tar -xf` 解开我们的两种归档后与源目录 `diff -r` 一致、硬链接共享 inode、权限位保住；D `tar --format=ustar` 写的标准归档，我们的 Scan / ExtractData 能读回来并还原成同一棵树。系统 `tar` 只在这一个脚本里当 oracle，产品代码里没有任何地方调用它。
- **筛选语义等价**（`scripts/filter_rule_builder_int_test.sh`）：同一场景分别用"GUI 规则编辑器生成的规则"与"手写 DSL 规则"各跑一次 backup → restore，两份结果必须逐字节相同。这是"GUI 没有偏离 CLI 语义"的证据，而不是"GUI 自己也能跑"。
- **CLI == GUI 预览一致**（`test.sh` 的 L 区）：同一条命令行预览输出与 GUI `--preview-test` 的输出 `diff -u` 必须为空，包括超过预览窗口时的截断契约与非法规则报出的原文。
- **同尺寸同 mtime 的改写**：`INC-10` 一类用例专门构造"内容变了但大小和时间戳都没变"的改写，验证增量与计划路径不是只比大小和时间。

**为什么这样测。** 单元测试证明"函数对"，端到端证明"用户拿到的东西对"。两者不能互相替代：容器头被截断这件事，只有把字节真的写坏再读一遍才发现得了。

### 3.3 故障注入：把"写了一半"造出来

磁盘写失败与进程崩溃这两类问题在正常路径上永远看不见，所以这一层专门制造它们。这也是本轮收尾里最不自然、但最必要的部分。

#### （1）清理合同：链接期注入 `fsync` / `close` 失败

入口 `scripts/cleanup_contract_test.sh`，被测代码是 `src/core` 与 `src/realtime` 里"写临时文件 → fsync → close → rename"那几段。

- **要抓的缺陷形状**是 `if (fsync(fd) != 0 || close(fd) != 0)`：fsync 失败时 close 被整条短路掉，**每失败一次泄漏一个 fd**。ENOSPC / EIO 恰恰是会连续失败的场景，长驻的实时备份与计划任务进程会稳定耗尽 fd 表；而成功路径完全正常，光看代码不容易发现。
- **注入方式**：`tests/review/cleanup_fault_interposer.cpp` 在**链接期定义** `fsync()` / `close()` 两个符号，放在 libc 之前，所以产品代码里的同名调用会解析到它。装弹时指定"第几次调用失败"以及失败时返回的 `errno`；不注入的调用通过 `syscall(SYS_fsync/SYS_close)` 原样转调真实现（不能调同名函数，否则递归到自己）。不需要 `LD_PRELOAD`，也不需要 `sleep` 撞运气。
- **能断言到什么程度**：因为注入是确定性的，用例可以精确断言"close 恰好被调用 1 次"、"诊断里报的是**第一次**失败的原因（fsync/ENOSPC），没有被后续清理改写成 close/EIO"、"两个 fd 都尝试关闭过"。
- **被测入口是真实产品代码**：`RealtimeStore::Save` 的收尾（fsync → close → unlink 半成品）与 delta payload 抽取的双 fd 收尾。脚本编译整份 `src/**`，不做链接替身，出现编译警告也算失败。

同一套手艺还用在 `tests/review/read_eintr_interposer.cpp`：只在 `LoadTransportIdentity()` 读满 32 字节之后那一次"尾部 1 字节"读取上注入 `EINTR`，证明它必须重试——否则一次信号打断就能让一个超过 32 字节的文件被当成合法私钥。

#### （2）OLD / NEW 判别矩阵：证明缺陷真的被修掉

入口 `scripts/bundle_source_mutation_test.sh`。只证明"新版本能跑"是不够的——那可能只是因为复现程序写得不对。所以它把**同一个复现程序编译两次**：一次链当前树的核心目标文件，一次链 `git archive 2889116` 导出的独立旧树（旧源码一个字节都没改），然后跑 2×2 矩阵：

| 目标文件 | `--expect new` | `--expect old` |
| --- | --- | --- |
| NEW（当前树） | 必须通过 | 必须失败 |
| OLD（2889116 树） | 必须失败 | 必须通过 |

对角通过、反对角失败，才算证明了"缺陷在旧树上真实存在、在新树上被修掉"。触发用显式可观察事件（`.part` 文件出现、`.part` 字节数）加可证明的前置条件，不用 `sleep`。它覆盖的缺陷是 `BuildSnapshotBundle()` 的"第一遍算摘要 → 第二遍复制字节"窗口里一次**同长度**改写会让包头声明的 SHA-256 与实际写进去的字节脱钩（材料包照样能被服务端收下，坏材料要等到 restore 才炸）。同一个脚本在 `BUNDLE_SOURCE_MUTATION_SANITIZE=1` 下再跑一遍新树，要求 ASan / UBSan 报告为 0。

#### （3）崩溃一致性 C0–C8

入口 `scripts/scheduled_backup_test.sh` 的 D3 区，用例实现在 `tests/unit/scheduled_backup_test.cpp` 的 L 节（"crash consistency: the manifest must name its baseline"）。

archive / manifest / `schedule.json` 是三个各自原子替换的文件，没有任何时刻能让它们一起提交。这一组把每一个"写了一半"的状态造出来，再要求下一轮的行为是安全的：

- **C0** 正常配对（state 与 manifest 指向同一份快照、两份归档都在、源没变）——这是唯一允许跳过的形状；
- **C1** state 还停在上一份、manifest 已经是新的（崩在两次写盘之间）——必须**不**跳过，如实重建完整基线。这是本轮真实抓到的 bug，不是假想用例；
- **C2** 反向：state 前进到新快照、manifest 还停在旧的；
- **C3** manifest 整个不见了；**C4** manifest 被截断（真实的"写到一半掉电"形状）；**C5** 旧版本格式的 manifest 永远不是可信基线；
- **C6** 基线归档文件不见了；**C7** manifest 绑定的是另一个仓库；**C8** manifest 绑定的是另一个源。

每个用例不只断言"重建了"，还会把新快照**单独拷贝出去**再恢复、与当前源 `diff -r`——证明它不依赖仓库里别的文件。C1 重建之后还要收敛：源没再变时，下一轮必须回到正常跳过。

#### （4）其它故障注入落点

- `scheduled_backup_test.sh` 的 D5 区：坏 store、只读目录、仓库与源临时不可用、软链接；
- `scripts/file_io_test.sh`：`FileSink` 的清理状态机（同样用 fsync / close 注入失败）、`PublishNoReplace`、`TempDirectoryGuard`、磁盘空间检查，以及 0600 产物与"失败后不留最终 `.bak`"；
- `scripts/archive_pipeline_test.sh`：容器头截断、尾部垃圾、篡改、目标目录原子性；
- `scripts/raw_archive_restore_test.sh` 的 B 段：截断 / 任意字节 / 离谱的声明长度 / 加密归档的三种情况 / 单独的 delta / 不存在的 id，每一条都要求 fail closed 且消毒剂报告为 0。

**为什么这样测。** 这些状态在正常开发里几乎不会被触发，一旦触发就是数据损坏或资源泄漏。造出来比跑出来更可靠，失败时也能精确指到"第几次调用"。

### 3.4 GUI 契约：`scripts/modern_gui_check.sh`

GUI 刻意**不做像素比对、不模拟鼠标点击**（这两件事换台机器就可能变），只做"能在无人值守环境里判定的部分"。脚本自己的注释就是这么写的。

1. **构建**：复用 Makefile 的 `gui-modern` 目标，另外构建 `backup-server` 与 `backup-server-keygen`——后面的 `--remote-test` 要起真服务端。同时断言缺 `--transport-key-file` 时服务端以用法错误（退出码 2）退出，没有明文模式。
2. **offscreen 真实启动自检**：`QT_QPA_PLATFORM=offscreen QSG_RHI_BACKEND=software`，真的把窗口构造出来；QML 运行期有告警就以非 0 退出。这一步不是 grep 源码，是真的启动了进程。
3. **qmllint 分类**：既不把工具噪音当失败，也不静默放过。Qt 6.4.2 的已知工具局限（上下文属性、deferred property 里放 id 等）逐条登记后放行，本次放行 742 条；**只有登记过的才放行**，出现未登记告警即失败。机器上没装 qmllint 时明确打印"跳过"，不算通过。
4. **静态源码契约**：资源清单是否收录每个 QML；侧栏是否恰好 7 个导航项、`StackLayout` 的 7 页可见性是否各自绑定；密码框是否 `echoMode=2`；删除是否先弹确认对话框、是否只有一处真正调用删除；QML 是否直接引用 `ConfigManager` / `BackupCatalog` / `BackupEngine`、是否手写归档完整路径、是否自己拼仓库路径。这类断言把"架构约束"变成可执行的检查，而不是写在文档里没人看。
5. **`objectName` 作为 QML 与自检之间的契约**：`ui/modern/qml/` 下 300 处 `objectName`；`dev_harnesses.cpp` 按名字在**可视项树**里找控件（不能用 `QObject::findChild()`——`Repeater` 的委托不在 QObject 树里，只有可视项树认得）。改名就会立刻失败，而不是让某条断言静默地测了个空。列表里的卡片按卡片上显示的文件名认领，不假定 `Repeater` 的顺序。
6. **真实控制器路径**：`--repository-test` 走 ConfigManager + BackupCatalog + BackupController + BackupEngine 的完整产品链路（保存仓库 / 自动命名备份 / 列表 / 恢复 / 删除），并直接读产物字节断言落成 v2 容器、catalog 记录 `format_version=2`；`--remote-test` 真的起一个 `backup-server` 进程，走注册 / 登录 / 上传真实归档 / 列表 / 下载 / 删除 / 退出登录，并断言密码框是掩码回显、口令与 token 不落盘、忙碌时冲突请求被拒（212 条断言）。
7. **截图**：两套主题 × 七页 + 高级选项展开 + 加密恢复密码对话框，写进 `tests/output/screenshots/`（不进仓库），给人看。
8. 所有 GUI 调用都带 `--config-file` 指向临时目录，并导出临时 `XDG_CONFIG_HOME`：测试不读写真实用户配置。

**为什么这样测。** GUI 最容易出的问题不是"渲染错了"，而是数据和界面之间的**最后一跳没接上**——例如 catalog 早就算好了链是否可恢复、控制器也早就把字段塞进了 model，卡片却一个字段都没传。这类缺陷只有"真实控制器 + 真实控件树"能抓到。

### 3.5 网络回环端到端：真进程、真 TCP、真 SQLite，不需要公网

这一层的共同点是**不 mock**：起真实的服务端二进制，客户端用产品自己的实现（CLI 与 GUI 共用同一个 `RemoteArchiveClient`），TCP 走 `127.0.0.1` 的随机高位端口（`20000 + RANDOM % 20000`），账户与元数据落在真实 SQLite 文件上，归档是真实备份引擎写出来的。

| 入口 | 测什么 |
| --- | --- |
| `network_test.sh`（+ `NETWORK_TEST_SANITIZE=1`） | 协议层帧编解码、截断、假 magic、超大长度、字段上限；真实 TCP 环回上的 PING、状态机、帧损坏断开；服务端"没有明文回退"这条路 |
| `remote_incremental_test.sh` | full → delta → delta → restore（自动拉整条链）→ 冷缓存 bootstrap → 依赖感知删除 → 服务端 blob 被篡改时拒绝恢复 → 无变化时不上传；外加 SQLite schema 1→2 迁移（保留旧数据、幂等、只读路径拒绝升级） |
| `bpsec2_loopback_e2e.sh` | 离线测试根 → 为服务器传输身份签发证书 → `--require-bpsec2` 启动；正向 ping / register / login / list 成功，负向（server_id 不符 / 不受信任的根 / 内置官方根 / BPSEC1 降级）全部失败；并证明全程没有 ssh 进程 |
| `login_throttle_test.sh` | 连续错口令达阈值后**正确口令也被拒**（否则攻击者碰对一次就绕过节流）、窗口过后恢复、服务端日志里留下明确的限速记录而不是伪装成口令错误 |
| `account_deletion_test.sh` | 账户注销的真实链路；断言不看服务端的自述——元数据直接查库、blob 与目录直接看磁盘 |
| `same_instance_truth_test.sh` | 四个真值源必须同时一致：协议、管理 CLI、元数据库文件本身（realpath + device/inode + 原始计数）、服务端进程实际使用的路径（从 `/proc/<pid>/cmdline` 解析） |
| `ssh_tunnel_manager_test.sh`（+ sanitize） | SSH 安全通道的进程生命周期、参数向量（`--` 之后的目标是**一个**参数、`BatchMode=yes`、`ExitOnForwardFailure=yes`、没有任何弱化 host verification 的开关）、就绪判定、超时、回收、僵尸子进程、外部监听者复用；用替身 `ssh` 精确控制失败原因 |
| `server_admin_test.sh` | 只能在服务器本机跑的管理工具：不监听端口、BPNET1 里没有 admin 操作码、只读命令是真的只读（连接 READONLY、不产生写事务）、输出里不出现 password hash / salt / token secret |

**为什么这样测。** 网络层的缺陷大多不在"函数算错了"，而在两个真实进程之间的时序、半包、断连，以及"某一侧其实没在做它声称做的事"。用 mock 测不出这些：`same_instance_truth_test.sh` 就是为了修一次真实 P0 而写的——GUI 显示已登录，同一台机器上的管理 CLI 却显示"还没有任何用户"，根因是状态根分叉，管理 CLI 安静地新建了一个空库，而"没有用户"和"你看错实例了"在屏幕上长得一模一样。

### 3.6 安全 oracle 与 fuzz：不让产品自己给自己打分

手写的密码学原语如果只和自己的测试向量比对，等于自己给自己打分。这一层的核心是**独立实现的对照**，以及"攻击者能构造的畸形输入一条都不许被接受"。

- **SHA-512**（`scripts/sha512_test.sh`）：官方向量 + 流式一致性 + 边界长度 + Reset 语义，再对随机长度与随机内容与 coreutils 的 `sha512sum` 逐条比对。coreutils 是独立实现，这一层不是"再跑一遍自己"。
- **Ed25519**（`scripts/ed25519_test.sh`）：先跑 RFC 8032 官方向量、往返、变异、畸形输入（每一条都要求具名的失败原因）；再用本机两套**互相独立**的实现做双向交叉——`cryptography`（走 OpenSSL 的 Ed25519）与 PyNaCl（libsodium）：公钥导出一致、我们的签名两个 oracle 都验得过、两个 oracle 签的名我们验得过、变异后的签名我们必须拒绝；最后再用 `openssl` CLI 对第三次。默认 300 组随机用例（`ED25519_ORACLE_CASES` 可调）。这些 oracle 只出现在测试脚本里，绝不进入产品运行路径。
- **BPCERT1 证书**（`scripts/bpcert_test.sh`）：正向、时间窗、六类畸形、字段语义；**单字节全扫描**——在每一个字节位置上套 3 种掩码，之后完整验签必须失败；再加 **20000 例 fuzz**（10000 条随机字节串 + 10000 条随机单字节变异）。第二层是 Python 侧按格式规范**自己重新拼一遍字节**，再用 `cryptography` 的 Ed25519 对同一个 body 签名——Ed25519 是确定性的，所以两边产出的完整证书必须**逐字节相同**。格式与签名各有一个独立实现对照。
- **BPSEC2 握手**（`scripts/bpsec2_test.sh`）：正向握手、九类失败路径、拒绝降级，以及 **10000 条畸形 ServerHello / ServerCertificate** 的 fuzz，一条都不许被接受；同时把 `secure_transport_test.sh` 再跑一遍，证明"加了 BPSEC2 之后 BPSEC1 的行为基线一点没变"。
- **线上字节捕获**（`tests/unit/secure_transport_test.cpp`）：客户端 → 捕获代理 → 服务端，抓**真实 TCP 字节**，断言已知的口令 marker 与 token marker 在线上出现 **0 次**，同时断言服务端确实收到了业务数据。只断言"握手成功了"是不够的——那不能排除"握手成功了但业务数据是明文"。
- **篡改矩阵**：密文翻位、tag 翻位、序号改写、截断、多余字节、超长声明、错误方向密钥、重放，逐项要求 fail closed 且不交出明文；外加握手报文被改一个字节双方都必须失败、错误 pin、全零对端公钥、未配置 pin 时的拒绝。
- **review-only 回归**（`tests/review/`，直接 `include` 产品 `.cpp` 来测内部不变式，不进入产品构建）：AES-CTR 计数器块唯一性（10 万个"方向 × 序号"组合两两不同 + 方向分离）、记录层序号耗尽（发送侧拒绝 `UINT64_MAX` 且不回绕）、`LoadTransportIdentity()` 的尾部 1 字节 `EINTR` 重试、x25519 百万次标量乘、BPSNAP1 记录与材料包的 fuzz。

**为什么这样测。** 对密码学而言，"通过了自己写的测试"几乎不构成证据；能构成证据的是"另一个独立实现得到了同样的结果"，以及"畸形输入没有一条被接受"。

### 3.7 消毒剂：ASan + UBSan

手写大整数与标量归约的越界、回绕问题，只有消毒剂才稳定暴露（`final_gate.sh` 的注释里记着：本项目已经在本轮的 Ed25519 实现上真实抓到过两次）。

- `make -j4 sanitize` 产出 `build-sanitize/`（`-g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer`）；
- 在这个构建上**复跑**的套件有 7 个：`ed25519`、`bpcert`、`ssh-tunnel`、`secure-transport`、`network`、`bundle-source-mutation`、`raw-archive-restore`；
- `scripts/quality_test.sh` 的维度 4 另有 41 次 sanitizer 调用：正常往返、空目录树、50 层深目录、4 MiB 文件、`--help`、源不存在、截断归档、magic 破坏、`entry_count` 篡改、路径穿越、FIFO、非空目标、0 字节归档，以及 24 次随机位翻转后逐个恢复；
- `scripts/ustar_test.sh` 内部也自己跑一遍 ASan + UBSan 的单元测试。

判定：任何一次调用出现 `AddressSanitizer` / `LeakSanitizer` / `runtime error:` / `SUMMARY:` 即失败（ASan 默认打开泄漏检测）。本次 41 次调用、0 报告。

**为什么这样测。** 消毒剂不是"更严格的单元测试"，它测的是 C++ 里最容易被忽略的一类错误：越界读写、有符号溢出、未初始化读取。这些在 x86 上通常表现为"跑得通但结果是错的"。

### 3.8 release staging 冒烟与静态门禁

**release staging** 不在 `final_gate.sh` 里，单独执行：

- `bash scripts/stage-client-release.sh`：产出 `dist/client/`，本次 **9 个文件**（`bin/backupctl`、`bin/backup-gui-modern`、`share/backup-project/official-root-ed25519.pub`、`VERSION`、三份 `docs/`、`BUILD-INFO.txt`、`MANIFEST.sha256`，清单里每个文件都有 sha256）。冒烟三项：`ldd` 不许有 `not found`、`backupctl --help` 成功、GUI 在 offscreen 下 10 秒内不许崩（退出码 124 = 还在跑，算通过）。
- `bash scripts/stage-server-release.sh`：产出 `dist/server/`，本次 **13 个文件**。冒烟五项：`ldd`、`backup-server --help`、`backup-cert-tool --help`、包内文件清单完全符合预期、**包内没有任何根密钥文件内容**。服务端包里没有私钥：传输身份私钥由 `backup-server-keygen` 在服务器本机生成，根私钥永远留在离线机器上。
- 两个脚本共用一条约定：工作树不干净时**默认直接失败**，要覆盖必须显式 `--allow-dirty` 并把 "dirty" 写进 `BUILD-INFO.txt`——发行包和提交对不上是最难查的一类问题。

**静态门禁**：

| 工具 | 入口 | 本次结果 |
| --- | --- | --- |
| clang-format 18.1.3 | `bash scripts/lint.sh` | 通过（`--dry-run --Werror`） |
| 注释率 | `python3 scripts/comment_ratio.py` | 21.40% |
| 80 列显示宽度 | `python3 scripts/source_style_check.py` | actionable 超宽 = 0 |

三个工具各自值得说一句"为什么需要"：

- `lint.sh` 的目录列表是 `app src include server tools ui/desktop ui/modern`。GUI 与服务端也必须走同一个格式入口，否则它们在别的目录下就算格式不合规也照样报 PASS。
- `source_style_check.py` 按 **Unicode East Asian Width** 算显示宽度而不是字节数：中文在等宽字体下占两列，用 `len()` 会把"一行 40 个汉字"（80 列）误判成很短，也会把"一行 80 个 ASCII"误判成刚好。它只补 clang-format 管不到的两块——注释文本与 QML；URL、32 位以上十六进制串、shell 命令与路径、协议测试向量是登记过的例外，不拆。
- `comment_ratio.py` 不是 `line.startswith("//")`：它维护一个最小词法状态机（`NORMAL / STRING / CHAR / LINE_COMMENT / BLOCK_COMMENT / RAW_STRING`），否则字符串字面量里的 `//` 会被误判成注释。统计口径是 `COMMENT_ONLY_LINES / TOTAL_PHYSICAL_LINES`，行尾注释单独计数、不计入分子，避免用行尾注释冲比例。本次统计范围 183 个文件、91438 物理行、19566 注释行。

## 4. 判定约定

跨套件通用的几条，集中写在这里，避免每套各说各话。

1. **超时与被信号打死一律算失败**。退出码 124（超时被杀）与 >= 128（被信号打死）都不算"成功拒绝"。所有 CLI 调用带硬超时，且用 `--signal=KILL`：解析陷入死循环时 SIGTERM 未必叫得停。
2. **"拒绝"必须同时不留痕**。除了退出码正确、错误信息里出现关键原因，还要求没有留下目标目录、没有留下半成品归档。
3. **成功必须双证**。`diff -r`（结构、存在性、空目录）加全树 `sha256sum` 清单（同长度内容差异是 diff 发现不了的），大文件另加 `cmp`。
4. **零警告即门禁**。测试编译与产品编译都按 `-Wall -Wextra -Wpedantic`，出现任何 `warning:` 直接判失败，不留"以后再说"。
5. **不许静默跳过**。环境缺东西时要么明确打印"跳过"（例如没有 qmllint、没有 GoogleTest），要么直接失败；不允许"没跑到"被当成"通过"。`final_gate.sh` 里 `--remote-test` 的 C01..C07 由两条脚本各自断言一遍、缺一条 `ok` 行就算失败，就是这条规则的落地。
6. **不信自述，看磁盘**。涉及服务端的用例不看服务端打印了什么：元数据查 SQLite 文件本身，blob 看磁盘，进程实际用的路径从 `/proc/<pid>/cmdline` 解析。
7. **判别性**。修 bug 的回归必须能证明"旧树上会失败"（3.3 的 2×2 矩阵），否则无法区分"真的修好了"和"复现程序本来就不触发"。

## 5. 日志与证据位置

| 内容 | 位置 |
| --- | --- |
| final_gate 完整日志 | `/tmp/final_gate.log`（stdout 同时给出逐行 `GATE <名字> exit=<码> 秒数` 摘要） |
| quality 套件 | `/tmp/backup-project-quality/`（`build.txt`、`functional-suite.txt`、`sanitizer-output.txt`、`last-output.txt`） |
| GUI 契约日志 | `tests/output/modern-gui-check.log` |
| GUI 截图 | `tests/output/screenshots/` |
| 各套件构建与运行日志 | `testdata/` 下的各套件私有目录（失败时才保留） |

`tests/output/` 与 `testdata/` 都不进仓库：测试现场只是排查用的，不是交付物。想保留现场就设 `KEEP_TESTDATA=1`。

## 6. 复现步骤

从 clean 开始，一次跑完本文引用的全部数字：

```bash
# 0. 取到基线
cd /home/pw-is-123/Study/UESTC/CS_Courses/26_27_1_SW_Dev_Exp/backup-project
git rev-parse HEAD        # 应打印 6d1e2ba71a48862927f23442604d6bd4b430e62d
git status --porcelain    # 应为空；staging 脚本会拒绝脏树

# 1. 从零构建：CLI + 服务端 + 两个 GUI + 测试夹具 + 证书工具，以及 sanitizer 版
make clean
make -j4 all server gui-all cert-tool test-fixtures
make -j4 sanitize

# 2. 功能套件（约 20 秒）
bash scripts/test.sh                  # 末行: [test] results: PASS=279 FAIL=0

# 3. 三个 gate
bash scripts/modern_gui_check.sh      # 末行: 通过 692 项，失败 0 项
bash scripts/final_gate.sh            # 末行: [gate] failed suites = 0

# 4. 发行 staging 冒烟（会写 dist/，要求工作树干净）
bash scripts/stage-client-release.sh  # 9 个文件
bash scripts/stage-server-release.sh  # 13 个文件

# 5. 静态门禁（final_gate 里也会跑 lint）
bash scripts/lint.sh
python3 scripts/comment_ratio.py
python3 scripts/source_style_check.py
```

几点说明：

- `final_gate.sh` 自己会跑 `make -j4 all gui-all test-fixtures` 与 `make -j4 sanitize`，第 1 步只是为了在跑 gate 之前就能看到编译警告。gate 中间的 `quality_test.sh` 会 `make clean`，之后脚本自己重建两个 GUI——顺序是刻意的，否则 `scripts/test.sh` 会走"没有 `build/backup-gui-modern`，跳过 GUI 对比"的分支，少 10 条用例。
- 整套 `final_gate.sh` 在这次机器上约 45 分钟（46 个套件累计 2614 秒，不含三次构建；最慢的是 `archive_pipeline` 367 秒、`quality` 290 秒、`network-sanitize` 276 秒、`network` 265 秒）。想跳过慢的部分可以用各套件的环境变量（例如 `ARCHIVE_PIPELINE_SKIP_BENCH=1`），但跳过之后就不能引用本文的数字。
- 失败时套件返回非 0；`quality_test.sh` 与 `test.sh` 默认在通过后清理 `testdata/`，失败或设置 `KEEP_TESTDATA=1` 时保留现场。

## 7. 局限性与未覆盖范围

这一节写清楚"这轮没做什么"，避免把没测的东西当成测过的。

### 7.1 测试专项的空白

- **没有独立的性能与压力专项**。`tests/performance/` 目前只有占位文件；性能数字散在 `archive_pipeline_test.sh`（128 MiB 大文件、10000 个小文件、baseline 与 Fast USTAR 对比）、`compression_test.sh`（压缩 benchmark）、`ustar_test.sh`（三个 corpus）里，打印的是真实墙钟时间，但**没有基线值、没有阈值门禁**。所以"性能有没有退化"目前靠人看数字，不靠自动判定。
- **没有采集代码覆盖率**。要说清"哪些行被测过"只能读代码。本轮也没有重跑 valgrind 全量检查，以 ASan + UBSan 代替。
- **没有并发压测与 TOCTOU 专项**。打包过程中源文件被改动只有既有功能用例覆盖；多进程相关的只有计划任务的锁竞争（D4 区：两个 watch、watch 与 run 互斥、SIGTERM 释放锁）与登录节流两块。
- **fuzz 不是覆盖率引导的**。位翻转型模糊测试是随机改写，只能证明"这些输入下没崩"，不能证明"不存在任何崩溃"。10000 / 20000 的例数是成本与覆盖的折中，不是安全性的证明。
- **部分模块的依赖是"可选"的**。`backup_catalog_test` 与 `filter_rule_builder_test` 在没有 GoogleTest 的环境里会退回内置 harness，两条路径都保留，但本机只验证了装了 GoogleTest 的那一条。

### 7.2 GUI 的空白

- **不做像素比对、不模拟真实鼠标点击**。这类断言换台机器（字体、主题、Qt 版本）就可能变，所以交给人工与截图。脚本注释里也是这么写的。
- **部分 GUI 分支没有自动化**：拖拽、窗口缩放、忙碌态的观感、关闭守卫的完整交互过程，目前只有静态契约加一次 offscreen 启动加人工确认。`gui_smoke_test.sh` 只验证 legacy Widgets GUI"能构建、能起来、不崩"。
- **qmllint 的一部分告警是登记放行的**（Qt 6.4.2 的工具局限），放行清单需要随 Qt 版本升级重新审一遍；它不等于"零告警"。

### 7.3 需要外部资源的用例

- 需要公网 ECS 的脚本不在本机流水线里：`aliyun_network_e2e.sh`、`aliyun_sequence_e2e.sh`、`aliyun_truth_matrix.sh`、`pr22_ecs_e2e.sh`、`pr23_ecs_phase3` 到 `phase9`。它们要求一台已部署的 ECS（服务端只绑 `127.0.0.1:18765`）以及一条免密 SSH 隧道，本机跑不了，因此**本文的结果里不包含它们**。把同类断言搬回环上跑的那部分是 `bpsec2_loopback_e2e.sh`、`login_throttle_test.sh`、`account_deletion_test.sh`，这部分在 gate 里。
- `pr23_ecs_phase4_local_bpsec2.sh` 名字里有 "local"，但它是在 **ECS 本机**执行的：客户端二进制要在 ECS 上就地构建（开发机 glibc 2.39，ECS 是 2.35，拷过去跑不了）。

### 7.4 判定强度的边界

- `scripts/test.sh` 的 GUI 预览一致性用例在 GUI 二进制不存在时会降级成"只跑 CLI 并记 PASS"。这是刻意的（让没有 Qt 的环境也能跑功能套件），代价是**光看 PASS=279 不能说明 GUI 被覆盖过**——必须确认 `build/backup-gui-modern` 存在，或者看 gate 里 `gui-rebuild` 那一行。
- 本文数字来自 2026-10-07 的一次具体运行。换机器、换 Qt 小版本、换 tar 或 libc 版本都可能改变个别条目（例如 qmllint 的放行条数、tar 输出的措辞）。复核要按第 6 节重跑，而不是直接引用这里的数字。

## 8. 结论

在 `main = 6d1e2ba71a48862927f23442604d6bd4b430e62d` 上：

- 功能正确性：`scripts/test.sh` PASS=279 FAIL=0；
- 全量门禁：`scripts/final_gate.sh` 49/49，failed suites = 0（46 个套件 + 3 条构建记录），其中质量套件的四个维度（可用性 / 鲁棒性 / 稳定性 10 轮 / 健壮性 41 次 sanitizer 调用 0 报告）全部通过；
- GUI 契约：`scripts/modern_gui_check.sh` 通过 692 项、失败 0；
- 发行 staging：客户端 9 个文件、服务端 13 个文件，冒烟全部 PASS；
- 构建与规范：零编译警告；clang-format 通过；注释率 21.40%；超 80 列 actionable = 0。

结论是：功能、失败路径、互操作性、安全边界与发行产物都经过了可复现的验证，而且验证方式是**独立实现对照 + 真实进程 + 故障注入**，不是产品给自己打分。本文第 7 节列出的空白——性能与压力专项、代码覆盖率、GUI 交互自动化、ECS 真机用例——是明确的未覆盖范围，不应被当作已验证。
