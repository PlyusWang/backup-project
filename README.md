# Backup Project

Linux 环境下的数据备份与恢复软件，课程项目。核心用 C++17 写，图形界面用 Qt 6，
自带一套手写的归档格式、压缩、加密与传输协议。

---

## 1. 当前功能

产品代码已经收口，下面这些能力都有实现、都有自动化测试。

### 本地备份与恢复

- `backup` 把源目录写成一份归档，落到**配置好的备份仓库**里，文件名由程序生成；
- `restore` 从仓库里取一份归档恢复成目录，写入采用「先恢复到暂存目录、全部成功
  再改名发布」的原子做法，失败不留半个目标目录；
- 恢复只接受仓库内的单组件 `.bak` 名字，不接受任意路径——归档落在哪里、叫什么
  由仓库决定，用户不需要（也不能）自己拼路径。

### 元数据

除文件内容外还保存并恢复 mode（0777 九位）、mtime（秒 + 纳秒）、uid / gid、
属主与属组名。`type:file|folder` 这类细分判断依赖这些元数据。

### 备份筛选（Filter）

11 个字段：`name` / `path` / `stem` / `ext` / `type` / `size` / `mtime` /
`uid` / `gid` / `user` / `group`。通配符 `*`（不跨 `/`）、`?`（单个字符）、
`**`（可跨 `/`）。语义固定为 **exclude 优先**；只要写了 include，普通文件必须
命中至少一条 include 才进归档；一条规则都不写时行为与没有筛选功能时完全一致。
`preview` 用同一组规则先列一遍会选中什么，它不建归档、不需要仓库、不改任何状态。

### 打包 / 压缩 / 加密

打包支持 `mypack`（本项目自有格式）、`ustar`（POSIX tar）、`fast-ustar`
（同样的 wire format，只改 I/O 策略）；压缩支持 `none` / `huffman`（Canonical
Huffman）/ `lzss-huffman`；加密支持 `none` /
`aes-256-ctr-hmac-sha256` / `des-cbc-hmac-sha256`。三者是可组合的三层流水线，
顺序固定为 **打包 → 压缩 → 加密**。

### 增量备份

增量 delta 用独立格式 `BKPINC1`，恢复时自动解析并应用整条依赖链
（完整基线 → Δ1 → Δ2 → …）。详见第 9 节。

### 定时与实时

- **定时**：按间隔触发，先和上一份源清单比对，有变化才建新快照，再用
  dependency-aware 的保留策略淘汰自己管理的旧快照；
- **实时**：递归 inotify 监听 + debounce 合并，事件风暴只落一份归档。

两者都需要进程常驻才能跑，见第 7 节。

### 网络备份与用户管理

`backup-server` 做远端存储后端，`backupctl remote` 做客户端：注册 / 登录 /
列表 / 上传 / 下载 / 删除 / 注销账户，以及产品级的 `remote backup` 与
`remote restore`（远端增量链）。服务端有自己的用户库与元数据库，本机管理工具
`backup-server-admin` 可以直接查用户、查快照、看存储概览。

### 传输加密

传输层是项目自己实现的 `BPSEC1`（X25519 握手 + HKDF-SHA256 + AES-256-CTR +
HMAC-SHA256），业务协议 `BPNET1` 的**每一个字节**（含帧头里的 opcode / status /
长度）都被加密封装成记录，Encrypt-then-MAC，序号严格递增抗重放。服务端身份有两种
模式：`BPSEC1` 指纹 pin（客户端必须事先知道服务端公钥，**不做**首次连接自动
信任），以及 `BPSEC2` 签名身份证书（`BPCERT1`，由离线根签发，客户端用可信根验签）。
BPSEC1 **不是 TLS、也不与 TLS 兼容**，是教学用途的手写实现，没有经过外部审计。

### 图形界面

Modern Qt 6 QML GUI，7 个页面，是**正式发布的前端**，见第 5 节。

---

## 2. 架构概览

```text
        CLI：backupctl            GUI：backup-gui-modern         服务端进程
        app/backupctl.cpp         ui/modern/                     （独立机器）
              │                         │                          │
              │  src/cli/*              │  BackupController 等     │  server/*
              │                         │  QObject 桥 + QML        │  只做存储后端
              └───────────┬─────────────┘                          │  与传输边界
                          │                                        │
              ┌───────────▼────────────────────────────┐           │
              │  业务核心（纯 C++17，不含 Qt）          │           │
              │  BackupEngine / 增量引擎 / 恢复         │           │
              │  TreeScanner / Filter / BackupCatalog   │           │
              │  Schedule / Realtime(inotify)           │           │
              │  RemoteArchiveClient ── BPNET1 ─────────┼───────────┘
              └───────────┬────────────────────────────┘
                          │
              ┌───────────▼────────────────────────────┐
              │  归档层：pack → compress → encrypt      │
              │  MyPack / USTAR / fast-ustar            │
              │  Huffman / LZSS-Huffman                 │
              │  容器 BKPCNT2（header + 分层解码）      │
              └───────────┬────────────────────────────┘
                          │
              ┌───────────▼────────────────────────────┐
              │  算法与系统层（纯 C++17）                │
              │  AES / DES / HMAC / SHA-256 / SHA-512   │
              │  X25519 / Ed25519 / HKDF / PBKDF2       │
              │  FileSystem / ConfigManager / 路径 / 锁 │
              └────────────────────────────────────────┘
```

三句话说明：

1. **共享核心**：CLI、Modern GUI、定时服务与实时服务跑的是同一份业务核心
   （`BackupEngine`、Filter、打包压缩加密流水线、增量引擎、RemoteArchiveClient），
   没有任何一个前端自己复制了一份拷贝、压缩或恢复逻辑。GUI 的 C++ 侧只做
   QObject 桥接与线程调度，QML 只画界面。
2. **无 Qt 依赖的那一层**：从业务核心往下（`src/core`、`src/archive`、
   `src/compression`、`src/crypto`、`src/filter`、`src/scheduler`、
   `src/realtime`、`src/network`、`src/platform`、`src/filesystem`）全部是纯
   C++17 + POSIX，`pkg-config Qt6` 一个都不需要。所以没装 Qt 的机器照样能构建、
   运行和测试整个 CLI，GUI 才能做成独立构建目标。
3. **服务端不复用业务核心**：`backup-server` 只存字节、存链关系、enforce 归属与
   依赖，不扫源目录、不算 diff、不重新实现第二套备份引擎，也不链接
   Qt / Filter / 打包 / 压缩 / 加密 / 增量——归档怎么产生、怎么恢复，全项目只有
   一套实现。

图形化的分层、构件与时序图在 `docs/uml/export/`（SVG 用于报告，PNG 用于
Markdown）：`component.svg` 构件图、`class.svg` 核心类图，以及
`sequence_backup.svg` / `sequence_restore.svg` /
`sequence_remote_incremental.svg` 三条主流程的时序图。

---

## 3. 构建

### 依赖

| 用途 | 包 |
| --- | --- |
| 编译器 / 构建 | `g++`（C++17）、GNU Make、`pkg-config` |
| 服务端 | SQLite：系统 `libsqlite3-dev`，或仓库内 `third_party/sqlite` 的官方头 + 系统运行时库 `libsqlite3.so.0` |
| 旧版 GUI | `qt6-base-dev`、`qt6-base-dev-tools` |
| Modern GUI | 上面两个 + `qt6-declarative-dev`、`qt6-declarative-dev-tools`、`qml6-module-qtquick`、`qml6-module-qtquick-controls`、`qml6-module-qtquick-layouts`、`qml6-module-qtquick-dialogs`、`qml6-module-qtquick-window`、`qml6-module-qtqml-workerscript` |
| 格式门禁（可选） | `clang-format` |

### make 目标

| 目标 | 产出 |
| --- | --- |
| `make` / `make all` | `build/backupctl` **和** `build/backup-server` |
| `make server` | `backup-server` + `backup-server-admin` + `backup-server-keygen` |
| `make client` | `backupctl` + `build/backup-gui-modern`（需要 Qt 6） |
| `make client-cli` | 只出 `backupctl`（没有 Qt 的构建机用） |
| `make cert-tool` | `build/backup-cert-tool`（离线根与证书工具，**不在** `all` / `server` 里） |
| `make gui` | `build/backup-gui`（旧版 Qt Widgets GUI） |
| `make gui-modern` | `build/backup-gui-modern`（正式 GUI） |
| `make gui-all` | 两套 GUI 一起构建 |
| `make debug` | `build-debug/`，带 `-g`，不覆盖 release 产物 |
| `make sanitize` | `build-sanitize/`，ASan + UBSan，顺带构建测试夹具与服务端工具 |
| `make test` | 构建后跑 `scripts/test.sh` |
| `make test-fixtures` | `build/archive-cli`，**测试夹具**，不是产品命令 |
| `make remote-sequence` | `build/remote-sequence`，ECS 真机序列驱动器，测试工具 |

两个容易踩的点：

- **`make` 现在同时构建 CLI 与 `backup-server`**，不再是「只构建 CLI」。想只出
  客户端用 `make client-cli`，想只出服务端用 `make server`。
- **GUI 是独立目标**：`make` 与 `make all` 都不会顺带把 GUI 编出来，`make gui`
  也不会顺手编现代版。缺 Qt 时 `check-qt` / `check-qt-qml` 会给出能照做的安装
  命令，而不是让 g++ 抛一屏找不到头文件的错误。

```bash
make -j4 all                 # CLI + 服务端
make -j4 gui-modern          # 正式 GUI（需要 Qt 6 QML 开发包）
make -j4 gui-all             # 两套 GUI
```

---

## 4. 快速开始（CLI 最小闭环）

```bash
make                                    # 构建 backupctl 与 backup-server

./build/backupctl config repository set ~/backups
./build/backupctl preview ~/project     # 只读：先看看会选中哪些条目
./build/backupctl backup  ~/project     # 归档写进仓库，名字由程序生成
./build/backupctl repository list       # 拿到上面那份归档的 file_name
./build/backupctl restore <file_name> ~/restored
```

配置、计划表、实时表默认都在 `~/.config/backup-project/backup-gui-modern/` 下的
`config.json` / `schedule.json` / `realtime.json`——CLI 与 GUI 共用同一个目录，
所以 GUI 里存的计划 CLI 读得到，反过来也一样。三个文件都可以用 `--config-file` /
`--schedule-file` / `--realtime-file` 覆盖。

退出码：`0` 成功、`1` 操作失败、`2` 命令行用法错误、`3` 已经有一个实例在跑
（CLI 与 Modern GUI 共用一把单实例锁）。

---

## 5. GUI

### Modern Qt 6 QML GUI（正式 GUI）

`ui/modern/` 下的 Qt Quick / QML 实现是**正式发布的前端**：发布包
（`dist/client/bin/backup-gui-modern`）里只有它，`make client` 构建的也是它。

7 个页面：

| # | 页面 | 做什么 |
| --- | --- | --- |
| 0 | 首页 | 只摆导航入口与全局操作状态，不读配置、不发起操作 |
| 1 | 备份 | 手动备份：源目录 + 仓库，高级选项里选打包 / 压缩 / 加密，内嵌 Filter 可视化规则编辑器（左栏文件预览、右栏规则卡片与表单），可先在后台预览再执行 |
| 2 | 自动备份 | 定时备份：启用状态、备份目录、频率（「每 N 分钟 / 小时」）、完整或增量、保留版本数、打包 / 压缩 / 筛选规则，以及上次 / 下次 / 运行历史 / 计划快照列表 |
| 3 | 备份管理 | 列出仓库里当前有哪些备份，并从列表发起**恢复**与**删除**（恢复是这一页里的一个动作，没有独立的恢复页） |
| 4 | 设置 | 目前只有一个全局项：备份仓库路径 |
| 5 | 实时备份 | 启用状态、备份目录、备份方式、保留版本、响应延迟、最长等待、打包 / 压缩 / 筛选规则，以及运行状态与最近备份 |
| 6 | 远程备份 | 连接服务器（地址 / 端口 / 用户名 / 密码 / 服务器身份指纹）、远端备份（源目录 → 云端，完整或增量）、云端列表与恢复 / 下载 / 删除、低层上传下载原始归档、技术详情 |

两处刻意的设计：任务进行中**不允许关窗**（核心没有取消能力，不能让用户以为
「关掉窗口 = 安全取消」），以及进度只显示不确定动画——核心没有进度回调，
宁可少显示也不显示编出来的百分比。

```bash
make gui-modern                                              # 构建
./build/backup-gui-modern                                    # 启动

# 无显示环境下自检（纯 SSH / CI）
QT_QPA_PLATFORM=offscreen QSG_RHI_BACKEND=software ./build/backup-gui-modern --smoke-test

./scripts/modern_gui_check.sh                                # 构建 + 启动 + 静态约束 + 端到端
```

自检会把 QML 运行期告警计入退出码：只要出现一条告警就以非 0 退出，避免
「界面看着正常、日志里其实在报错」。完整设计、逐页说明与已知限制见
`docs/ui/modern_qml_gui.md`。

#### 界面预览

七页各一张（浅色主题）：

| 首页 | 手动备份 | 高级选项展开 |
| --- | --- | --- |
| ![首页](docs/images/gui/01-home.png) | ![手动备份](docs/images/gui/02-backup.png) | ![高级选项](docs/images/gui/03-backup-advanced.png) |

| 备份管理 | 自动备份 | 实时备份 |
| --- | --- | --- |
| ![备份管理](docs/images/gui/04-backup-management.png) | ![自动备份](docs/images/gui/05-schedule.png) | ![实时备份](docs/images/gui/06-realtime.png) |

| 远程备份 | 设置 |  |
| --- | --- | --- |
| ![远程备份](docs/images/gui/07-remote.png) | ![设置](docs/images/gui/11-settings.png) |  |

这些图由 GUI 自己的 `--screenshot` 模式产出，和用户看到的是同一条渲染路径，
不是另画的示意图。生成方式见 `docs/images/gui/README.md`。

### 旧版 Qt 6 Widgets GUI（legacy）

`ui/desktop/` 下的 Widgets 实现是**第一版界面，保留作为 legacy / regression
reference**：它继续可构建、继续进 lint，`scripts/gui_smoke_test.sh` 继续管它，
但它**不作为正式发布前端**，也不随发布包装箱（`stage-client-release.sh` 只收
`backup-gui-modern`）。两套界面互不依赖、互不覆盖，共用同一份 `BackupEngine`。
见 `docs/ui/desktop_gui.md`。

---

## 6. CLI（`backupctl`）

8 个子命令：

| 命令 | 一行说明 | 例子 |
| --- | --- | --- |
| `backup` | 把源目录写成归档，落进配置好的仓库 | `backupctl backup ~/project --compression lzss-huffman` |
| `restore` | 从仓库里按 file_name 恢复成目录 | `backupctl restore 20261007-101500.bak ~/restored` |
| `preview` | 只读：列出这组筛选规则会选中哪些条目 | `backupctl preview ~/project --include 'ext:cpp;h'` |
| `schedule` | 定时备份的 show / set / enable / disable / run / history / watch | `backupctl schedule set --source ~/project --interval-minutes 30 --retain 12` |
| `realtime` | 实时备份的 show / set / enable / disable / watch / history | `backupctl realtime set --source ~/project --debounce-ms 500` |
| `remote` | 与 backup-server 通信：ping / register / login / list / upload / download / delete / backup / restore / delete-account | `backupctl remote ping --host 127.0.0.1 --port 18765 --official-cloud` |
| `repository` | 列出仓库里的备份，或删掉其中一份 | `backupctl repository delete 20261007-101500.bak` |
| `config` | 读写全局配置（目前只有备份仓库路径） | `backupctl config repository set ~/backups` |

`backup` 与 `schedule set` / `realtime set` 都接受
`--pack` / `--compression` / `--encryption` / `--include` / `--exclude`，
以及 `--strategy full|incremental`。口令只从 `/dev/tty` 交互读取，不接受参数、
环境变量或管道输入（自动测试走专门的测试入口）。完整的选项与语义看
`backupctl --help` 和 `docs/basic_cli_usage.md`。

---

## 7. 定时备份与实时备份

**这两种触发方式都需要一个进程常驻**——它们不是内核服务、也不是 systemd 单元，
配置里存的是「到点该做什么」，得有东西在跑才会做：

- 命令行：`backupctl schedule watch` / `backupctl realtime watch` 在前台跑，直到
  `SIGINT` / `SIGTERM`；
- 图形界面：Modern GUI 运行期间自己驱动（页面上的「启用」只是写配置，真正跑的
  是这个进程）。

CLI 与 GUI 共用一把计划锁：同时开着 GUI 又开着 `schedule watch` 时，一方持有、
另一方在页面上如实显示「已被另一进程持有」，不会两边同时建快照。

**定时**的每一轮顺序是：确认上一份源清单确实属于仓库里一份仍存在、仍归本计划
管理的快照 → 扫描源目录 → 逐条比对 → 没变化就跳过（不调用备份引擎、不产生
`.bak`、不更新清单）→ 有变化才建新快照 → 执行只淘汰自己管理的旧快照的保留
策略 → 记运行历史。**变化检测不是增量存储**：`strategy full` 下每一份 `.bak`
都是完整独立的，单文件即可恢复。

**实时**用递归 inotify 监听源树，配一个 debounce / coalescing 状态机：短促的连续
改动合并成一次备份（trailing edge），持续不断的改动也有 `--max-wait-ms` 上限兜底，
不会永远不触发。监听队列溢出（`IN_Q_OVERFLOW`）或监听丢失时标记为降级并重扫，
恢复后继续。多个自动化触发路径都**不允许加密**（`--encryption` 只能是 `none`），
因为无人值守时没有地方安全地拿口令。

用法见 `docs/scheduled_backup_usage.md` 与 `backupctl realtime --help`。

---

## 8. 网络备份

两条路，客户端看到的是同一个 `BPNET1` 协议：

**（一）自建服务器**。装 `dist/server/`，用 `backup-server-keygen` 在服务器本机
生成传输身份私钥，启动 `backup-server`，客户端用它的公钥指纹 pin 住它。服务端
默认只允许监听 `127.0.0.1`；本机开一条 SSH 隧道就能用：

```bash
ssh -N -L 127.0.0.1:18765:127.0.0.1:18765 <服务器>
backupctl remote ping --host 127.0.0.1 --port 18765 \
  --server-key sha256:<服务端打印的指纹>
```

**（二）官方云**。`--official-cloud`（或 GUI 上的 `Backup Project Cloud`）用
编译进二进制里的官方云端 profile 连：服务端出示由**离线根**签发的身份证书，
客户端用内置官方根验签，用户不需要知道也不需要核对任何指纹。

**回环就够跑完整个演示**：`127.0.0.1:18765` 上起一个 `backup-server`，注册 /
登录 / 上传 / 列表 / 恢复 / 删除整条链路都能走通，不需要公网、不需要云主机，
也不需要配证书（指纹 pin 模式即可）。自动化测试就是这么跑的——
`scripts/modern_gui_check.sh` 的远程备份页一节会真的起一个服务端进程，走完注册 /
登录 / 上传真实归档 / 列表 / 下载 / 删除 / 退出登录。

要开公网是另一回事，且是**显式**的：服务端默认拒绝任何非回环的 `--bind`，
必须同时给出 `--allow-public-bind "<理由>"` 与身份证书；而且阿里云安全组和主机
`ufw` **两处**都要放行，只做一处会表现为 timeout 而不是 refused。详见
`docs/network_backup_usage.md` 与 `docs/server-quick-start.md`。

服务端侧共 4 个二进制：`backup-server`（服务端）、`backup-server-admin`
（**只能在服务器本机跑**的管理工具，不监听任何端口）、`backup-server-keygen`
（传输身份密钥）、`backup-cert-tool`（离线根与证书工具，一般不在服务器上跑）。

---

## 9. 增量备份

增量 delta 用独立格式 `BKPINC1`：完整快照继续用 v2 容器（`BKPCNT2`），一个字节
不改；delta 必须是**另一种顶层 magic**——如果 delta 复用了 `BKPCNT2`，旧版本的
reader 会把它当成一份完整快照，然后安静地恢复出错的东西，这比明确拒绝危险得多。

- **依赖链**：一份 delta 记录它的 `parent`。恢复任意一个 restore point 时由
  `ResolveSnapshotChain` 自动解析出「完整基线 → Δ1 → Δ2 → … → ΔN」整条链，并逐跳
  验证：父节点存在且是合法单组件名字、父的身份等于子记录的 `parent_snapshot_id`、
  不允许环与自指、链深度有上界、整条链必须属于同一个 generation。不需要用户手工
  先下载中间的 delta。
- **中间节点不能删**：链中间的任何一份 delta 被删掉，它后面的那些 restore point
  就再也恢复不出来了。保留策略是 dependency-aware 的，不会淘汰仍被依赖的节点，
  但手动 `repository delete` 不管这些。
- **没有可用的基线时会自己建**：第一次增量、或基线不可信时先写一份完整基线；
  没有任何有效变化时什么都不写。
- **不支持的组合显式拒绝**，不做 silent fallback——「选了增量却偷偷按全量跑」是
  最危险的那种降级。

远端增量同理，但多一层：客户端把「归档 + 强 manifest + 身份记录」三件套打成一个
`BPSNAP1` 材料包上传，服务端只当不透明字节存。生成下一份 delta 需要父的三件套，
只传 `.bak` 的话远端链在副文件验证那一步必然失败。服务端 enforce 依赖关系，
删除一个仍被依赖的快照会被拒绝。详见 `docs/remote_incremental.md`。

---

## 10. 测试

三个入口，各管一件事：

| 脚本 | 管什么 | 怎么跑 |
| --- | --- | --- |
| `scripts/test.sh` | 功能基线：CLI 的 backup / restore 全链路、错误路径、路径拓扑、归档往返、元数据、损坏归档拒绝、路径穿越拒绝、不支持的文件类型、payload 未压缩的证明，以及其他子系统的用例 | `bash scripts/test.sh` |
| `scripts/final_gate.sh` | 总门禁：全量重建（含 sanitizer 构建）+ lint + 把全部独立套件各跑一遍，每个套件一行 `GATE <名字> exit=<码> 秒数` | `bash scripts/final_gate.sh`（日志在 `/tmp/final_gate.log`） |
| `scripts/modern_gui_check.sh` | Modern GUI：构建 + offscreen 启动 + qmllint + 静态约束 + 端到端（含真起一个 `backup-server` 的远程页合同测试） | `bash scripts/modern_gui_check.sh`（日志在 `tests/output/modern-gui-check.log`） |

**当前 main 的真实结果**（commit `6d1e2ba`）：

```text
scripts/test.sh              PASS=279 FAIL=0
scripts/final_gate.sh        49/49 套件 exit=0，failed suites = 0，构建警告 0
scripts/modern_gui_check.sh  通过 692 项，失败 0
```

`final_gate.sh` 的 49 个套件里包含功能基线、质量维度、各个格式与算法的独立
套件、多个 ASan + UBSan 消毒剂套件，以及两套 GUI 检查。`modern_gui_check.sh` 与
`gui_smoke_test.sh` 刻意分成两个脚本：后者只管旧版 Widgets GUI，一个坏掉不会把
另一个的检查带塌。

代码质量门禁：`scripts/lint.sh`（clang-format `--dry-run --Werror`，覆盖 `app`
`src` `include` `server` `tools` `ui/desktop` `ui/modern`）、
`scripts/source_style_check.py`（可执行动作的超 80 列行数）、`scripts/comment_ratio.py`
（产品源码注释率）。当前注释率约 21.4%，actionable 超 80 列 = 0。

注意：测试脚本会写 `build/` 与 `tests/output/`，这两个目录都已 gitignore。
截图默认产出在 `tests/output/screenshots/`（不进仓库）；文档里正式引用的
那几张是挑选后放进 `docs/images/gui/` 的。

---

## 11. 发布包

分客户端与服务端两个独立的包，**升级客户端不会碰服务器二进制**：

```bash
bash scripts/stage-client-release.sh     # 默认 dist/client/
bash scripts/stage-server-release.sh     # 默认 dist/server/
```

| 脚本 | 产出 |
| --- | --- |
| `stage-client-release.sh` | `bin/backupctl`、`bin/backup-gui-modern`（`--no-gui` 时不含）、`share/backup-project/official-root-ed25519.pub`、`share/backup-project/VERSION`、`docs/`、`BUILD-INFO.txt`、`MANIFEST.sha256` |
| `stage-server-release.sh` | `bin/backup-server`、`bin/backup-server-admin`、`bin/backup-server-admin.sh`（交互式管理菜单）、`bin/backup-server-keygen`、`tools/backup-cert-tool`、官方根公钥、`VERSION`、`docs/`、`BUILD-INFO.txt`、`MANIFEST.sha256` |

两个脚本都遵守同一条规矩：**工作树不干净就拒绝打包**，要打一个「内容和 commit
对不上」的包必须显式给 `--allow-dirty`，而且 `BUILD-INFO.txt` 里会写明 dirty。
打完包立刻做冒烟测试：`ldd` 不许有 `not found`、`--help` 必须退出 0、含 GUI 时
用 offscreen 启动 10 秒不许崩。

包里**没有任何私钥**：传输身份私钥由 `backup-server-keygen` 在服务器本机生成，
根私钥永远留在离线机器上；打包脚本会扫描包内容，私钥字段出现在不该出现的地方
即判失败。更上层的发行制品（AppImage / .deb / portable tar.xz）由
`packaging/build-release.sh` 在旧基线容器里构建，见 `docs/release-packaging.md`。

---

## 12. 文档索引

### 入门

| 文档 | 内容 |
| --- | --- |
| `docs/basic_cli_usage.md` | CLI 最小闭环、选项与行为约定 |
| `docs/filter_usage.md` | Filter 规则语法、include / exclude 语义、11 个字段 |
| `docs/client-quick-start.md` | 客户端快速上手：官方云端 / 自建服务器 / SSH 隧道 |
| `docs/server-quick-start.md` | 服务端部署、证书、开公网的两处放行 |
| `docs/install-client.md` / `docs/install-server.md` | 发行包安装、卸载、支持范围 |
| `docs/upgrade-server.md` | 服务端升级与回滚 |
| `docs/self-hosted-server.md` | 自托管：自己的根、自己的服务器 |

### 参考

| 文档 | 内容 |
| --- | --- |
| `docs/00_project_baseline.md` | 工程基线 |
| `docs/01_requirements.md` | 需求分析 |
| `docs/02_architecture.md` | 系统设计 |
| `docs/ui/modern_qml_gui.md` | Modern GUI 的设计、逐页说明与已知限制 |
| `docs/ui/desktop_gui.md` | 旧版 Widgets GUI（legacy） |
| `docs/ui/filter_rule_builder.md` | Filter 规则构造中间层 |
| `docs/scheduled_backup_usage.md` | 定时备份：变化检测、源清单绑定、保留策略 |
| `docs/network_backup_usage.md` | 远程备份：拓扑、BPSEC1、远端增量 |
| `docs/release-layout.md` | 客户端 / 服务端发布布局与核对方式 |
| `docs/release-packaging.md` | 发行制品怎么做的、基线怎么选的 |
| `docs/pr22-remote-connection-ux.md` | 连接层：pin 应用反馈 + GUI 自管 SSH 隧道 |

### 格式与协议

| 文档 | 内容 |
| --- | --- |
| `docs/format/archive_v0.1.md` | Archive v0.1（legacy，只读兼容） |
| `docs/format/archive_v2_container.md` | v2 容器 `BKPCNT2`：打包 / 压缩 / 加密三层 |
| `docs/secure_transport.md` | `BPSEC1`：协议、安全性质、明确的非目标 |
| `docs/bpsec2-design.md` | `BPSEC2` 签名身份证书（`BPCERT1`）的设计与实施状态 |
| `docs/remote_incremental.md` | `BKPINC1` 依赖链、`BPSNAP1` 材料包、信任模型与限制 |
| `docs/server-hardening-audit.md` | 服务端加固审计：逐条现状与证据 |
| `docs/research/pr21_secure_transport_sources.md` | X25519 / HKDF / AES-CTR 的规范出处与测试向量来源 |

### 测试

| 文档 | 内容 |
| --- | --- |
| `docs/03_testing.md` | 软件测试方法与用例组织 |
| `docs/testing/sprint12_validation_20260917.md` | 一次真实执行的完整验收证据（环境 + 结果 + 提交号） |
| `docs/pr23-prv41-closure.md` | 一条长期失败的用例到底是产品缺陷、契约写错还是构造不成立 |
| `docs/demo/pr21_network_score_demo.md` | 网络部分的现场演示最短路径 |

### 过程与历史归档

| 文档 | 内容 |
| --- | --- |
| `docs/scrum/sprint1_review_retro.md` / `docs/scrum/sprint2_review_retro.md` | Sprint Review 与 Retrospective |
| `docs/requirements/use_case_description.md` | 功能用例描述 |
| `docs/requirements/requirements_revision_suggestions.md` | 对需求文档与 UML 的修改建议 |
| `docs/uml/*.mdj` | 用例图 / 类图 / 组件图 / 备份与恢复时序图（StarUML） |
| `docs/gantt/project_gantt.tex`（含 `archive/` 下的历史版本） | 项目甘特图 |
| `docs/backlog/backup_mode_roadmap.md` | 备份触发方式 × 备份策略的后续路线 |
| `docs/backlog/filter_future.md` | Filter 的技术债与待决策事项 |
| `docs/backlog/remote_management_future.md` | 远程管理面的后续方向（**均未实现**） |
| `docs/04_release_and_demo.md` | 发布与演示流程（Draft，尚未展开） |

最终报告在 `report/`（LaTeX，`report/main.tex` 汇总 `sections/` 与
`appendices/`）。

---

## 13. 开发环境

主要开发环境是 Windows 上用 SSH 连到 VMware Ubuntu 虚拟机：

```text
Windows ──SSH──> VMware Ubuntu
                   ├── GCC / G++
                   ├── GNU Make / pkg-config
                   ├── Git / gdb / Valgrind
                   ├── clang-format
                   ├── Qt 6（base + declarative）
                   └── backup-project
```

网络备份的远端是一台阿里云 ECS。主要运行平台是 Linux（`linux x86_64`）；
Windows / macOS / ARM64 没有构建过也没有验证过，不声称支持。

