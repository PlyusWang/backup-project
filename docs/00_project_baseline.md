# 数据备份软件项目工程基线

> 文档编号：00  
> 文档名称：Project Baseline  
> 当前版本：v0.3  
> 状态：Draft  
> 创建日期：2026-09-03  
> 最后更新：2026-09-03  
> 开发环境：Linux VM  
> 核心语言：C++  
> 开发方法：Agile + Object-Oriented Design + Top-Down Stepwise Refinement  
> 最终文档格式：LaTeX / PDF  
> 当前工作文档格式：Markdown

---

# 1. 文档目的

本文档定义项目开发全过程的第一版工程基线，用于统一三名成员对以下问题的理解：

- 项目最终要完成什么；
- 哪些功能属于课程基础要求；
- 哪些扩展功能计划实现；
- 项目目标功能分值；
- 各成员主要职责；
- 软件采用什么总体架构；
- 如何使用面向对象方法；
- 如何进行自顶向下逐步求精；
- 哪些第三方库允许使用；
- 哪些核心功能必须自主实现；
- Git 如何协作；
- UML 需要绘制哪些图；
- 项目如何测试；
- 每周如何形成可验收成果；
- Markdown 与 LaTeX 文档如何维护；
- 最终答辩和评分需要留下哪些证据。

本文档不是最终提交的小组报告。

正式文档计划包括：

```text
00_project_baseline.md
01_requirements.md
02_architecture.md
03_testing.md
04_release_and_demo.md
```

最终将其中有效内容整理进入教师要求的三合一报告：

```text
需求分析说明
+
系统设计文档
+
软件测试报告
```

最终报告采用 LaTeX 编写并生成 PDF。

---

# 2. 课程项目定位

本课程项目要求三人小组开发一款：

> 数据备份与恢复软件。

项目采用软件工程方法推进。

课程关注的不仅是软件“有没有功能”，还包括：

- 正确性；
- 易用性；
- 健壮性；
- 软件工程过程；
- 项目进度；
- 团队分工；
- 编程规范；
- Git 版本管理；
- 源码质量；
- UML 建模；
- 测试质量；
- 文档质量；
- 演示质量；
- 对源代码实现逻辑的理解。

因此本项目不能采用：

> 最后几天集中写完全部代码，再补文档。

的开发方式。

项目全过程必须同步保留：

```text
代码
设计
测试
Git 历史
UML
项目计划
功能验收记录
```

---

# 3. 项目一句话定义

> 在 Linux 环境下实现一个以 C++ 为核心的数据备份与恢复系统，支持普通文件及 Linux 文件系统相关特性的备份和还原，并逐步提供元数据保存、文件过滤、打包、压缩、加密、定时备份、实时备份、Web 图形界面以及可选的网络备份能力；核心评分算法尽量自主实现。

---

# 4. 项目核心操作

整个系统围绕两个最核心操作组织：

```text
Backup
Restore
```

基本数据流：

```text
源目录
   │
   ▼
扫描文件
   │
   ▼
筛选文件
   │
   ▼
收集元数据
   │
   ▼
打包归档
   │
   ▼
压缩数据
   │
   ▼
加密数据
   │
   ▼
存储备份
```

恢复过程原则上执行逆操作：

```text
备份存储
   │
   ▼
读取备份
   │
   ▼
解密数据
   │
   ▼
解压数据
   │
   ▼
解包归档
   │
   ▼
重建文件与目录
   │
   ▼
恢复元数据
```

---

# 5. 总体目标

## 5.1 功能目标

本组目标不是仅完成及格线。

当前设定：

```text
规划功能池：约 165 分
承诺功能池：约 135 分
最终最低目标：实际完成 ≥ 120 分
课程难度系数：按教师规则封顶 1.1
```

因此：

> 120 分不是功能选择上限，而是最低完成目标。

额外规划的功能主要用于提供风险冗余。

---

## 5.2 质量目标

软件至少应满足：

### 正确性

```text
Restore(Backup(Data)) == Data
```

对于声明支持的元数据：

```text
Restore(Metadata) == Original Metadata
```

---

### 健壮性

需要合理处理：

- 路径不存在；
- 文件不存在；
- 权限不足；
- 文件正在变化；
- 空文件；
- 空目录；
- 特殊文件；
- 备份文件损坏；
- 错误密码；
- 存储空间不足；
- 网络中断；
- 用户非法输入。

程序不能简单崩溃或错误报告“成功”。

---

### 易用性

Web UI 应保证：

- 核心操作容易找到；
- 用户可以明确知道备份源和目标；
- 具有操作结果反馈；
- 错误信息可理解；
- 危险操作有确认；
- 备份状态和进度可查看。

---

### 可维护性

要求：

- 模块职责明确；
- 命名统一；
- 编码风格统一；
- 构建方式统一；
- Git 历史清晰；
- 接口尽可能稳定；
- 重要逻辑具有说明；
- 测试可重复运行。

---

# 6. 课程功能评分基线

以下内容根据课堂 PPT、教师讲解和课堂评分表整理。

若后续教师发布正式新版评分规则，以新版为准。

---

# 7. 基础功能

基础功能：

```text
数据备份
数据还原
```

分值：

```text
40
```

目标：

```text
A Directory
    ↓
Backup
    ↓
B Repository
```

以及：

```text
B Repository
    ↓
Restore
    ↓
C Directory
```

---

# 8. 扩展功能总表

当前已明确的扩展项目包括：

| 功能                  |   当前课程分值 |
| --------------------- | -------------: |
| 文件类型支持          |             10 |
| 元数据支持            |             10 |
| 自定义备份 / 文件过滤 |             10 |
| 打包 / 解包           |    每种算法 10 |
| 压缩 / 解压           |    每种算法 10 |
| 加密 / 解密           |    每种算法 10 |
| 图形界面              |             10 |
| 定时备份              |             10 |
| 实时备份              |             15 |
| 网络备份基础          |             10 |
| 网络用户管理          |              5 |
| 网络元数据管理        |              5 |
| 网络传输加密          |              5 |
| 增量备份              |              5 |
| 其他功能              | 与教师讨论确定 |

其他教师明确提到可讨论的方向包括：

```text
断点续传
大文件处理
其他有实际价值的扩展
```

这些项目具体分值需提前和教师确认。

---

# 9. 当前功能战略

功能分为：

```text
MUST
SHOULD
STRETCH
```

三层。

---

# 10. MUST：承诺完成

## 10.1 基础备份与恢复

分值：

```text
40
```

实现：

- 普通文件备份；
- 普通目录备份；
- 递归目录扫描；
- 空目录恢复；
- 二进制文件恢复；
- 基础错误处理。

---

## 10.2 元数据支持

分值：

```text
10
```

保存并恢复：

- UID；
- GID；
- 权限；
- 时间信息；
- 文件类型。

根据实现能力进一步保存其他 Linux 元数据。

---

## 10.3 自定义备份

分值：

```text
10
```

计划支持：

```text
路径
文件名
文件类型
时间
尺寸
用户
用户组
```

过滤。

设计目标类似：

```text
.gitignore
+
Everything Filter
```

但规则语法由本项目自行定义。

---

## 10.4 打包与解包

计划至少自主实现一种归档方案。

分值目标：

```text
10
```

原则上不直接调用：

```text
tar
libarchive
```

完成核心归档功能。

计划设计自定义 Backup Archive Format。

示例：

```text
Backup Archive
├── Header
├── File Entry
│   ├── Path
│   ├── Type
│   ├── Metadata
│   ├── Size
│   └── Data
├── File Entry
└── ...
```

具体格式在 `02_architecture.md` 中设计。

---

## 10.5 压缩与解压

至少自主实现一种压缩算法。

第一候选：

```text
Huffman Coding
```

可选后续算法：

```text
LZ77
LZ78
```

基础正确性条件：

```text
decompress(compress(x)) == x
```

必须能够处理：

```text
text
binary
empty data
random data
large data
```

分值目标：

```text
10+
```

---

## 10.6 加密与解密

至少自主实现一种算法。

候选：

```text
AES
DES
Stream Cipher
```

最终算法需综合：

- 实现难度；
- 教学价值；
- 时间成本；
- 教师评分认可；

再确定。

原则上：

> 若本项按“自主实现算法”计分，则不能直接调用 OpenSSL 完成核心算法。

要求：

```text
decrypt(encrypt(data, key), key) == data
```

错误密码不得正常恢复。

分值目标：

```text
10+
```

---

## 10.7 Web 图形界面

分值：

```text
10
```

采用：

```text
HTML
CSS
JavaScript
```

构建 Web GUI。

风格可以参考：

- 7-Zip；
- WinRAR；
- Everything；
- QCE Web；
- DSH Web；

但不直接复制。

计划提供：

```text
Backup
Restore
Backup History
File Browser
Filter Settings
Encryption Settings
Scheduler
Status / Progress
```

等页面或区域。

---

## 10.8 定时备份

分值：

```text
10
```

至少支持：

```text
每 N 分钟
每小时
每天
每周
```

根据教师要求：

> 即使不实现复杂的数据淘汰策略，只要能够可靠周期备份，也可完成该主要功能要求。

---

## 10.9 实时备份

分值：

```text
15
```

Linux 下优先使用：

```text
inotify
```

监听：

```text
create
modify
delete
move
```

等事件。

事件发生后触发相应备份逻辑。

---

# 11. MUST 功能总分

当前承诺功能：

```text
基础功能             40
元数据               10
自定义备份           10
打包                 10
压缩                 10
加密                 10
GUI                  10
定时备份             10
实时备份             15
-------------------------
合计                125
```

因此：

> 即使暂不开发网络备份，本组仍计划完成至少 125 分功能。

这也是当前最重要的最低实施方案。

---

# 12. SHOULD：优先扩展

## 12.1 特殊文件类型支持

目标：

```text
10
```

计划支持：

- 软链接；
- 硬链接；
- 有名管道 FIFO；
- 字符设备；
- 块设备。

Socket 文件原则上不作为普通备份恢复目标。

具体行为需要针对 Linux 文件语义分别设计。

加入本项后：

```text
125 + 10 = 135
```

因此当前推荐的：

> 承诺功能池目标约为 135 分。

---

# 13. STRETCH：网络备份

本地系统稳定后再开发。

网络部署：

```text
Linux VM
    │
    │ Network
    ▼
Alibaba Cloud ECS
```

---

## 13.1 网络备份基础

分值：

```text
10
```

实现远端 Storage Backend。

概念：

```text
StorageBackend
├── LocalStorage
└── RemoteStorage
```

---

## 13.2 用户管理

分值：

```text
5
```

包括：

- 注册；
- 登录；
- 用户身份识别；
- 不同用户数据隔离。

---

## 13.3 云端元数据管理

分值：

```text
5
```

管理：

- 用户；
- 快照；
- 文件；
- 大小；
- 时间；
- 版本；
- 备份任务。

---

## 13.4 传输加密

分值：

```text
5
```

用于保护：

```text
Client
    ↕
Server
```

之间的数据。

需要注意：

> “传输加密”与“备份文件本身的加密”属于两个不同问题。

---

## 13.5 增量备份

分值：

```text
5
```

先实现：

```text
File-Level Incremental Backup
```

如时间允许再实现：

```text
Block-Level Incremental Backup
```

例如：

```text
Large File
├── Block 0
├── Block 1
├── Block 2
├── Block 3
└── ...
```

只传输变化的数据块。

---

# 14. 完整规划功能池

如果网络部分全部完成：

```text
本地承诺方案         125
特殊文件支持          10
网络备份              10
用户管理               5
云端元数据             5
传输加密               5
增量备份               5
-------------------------
规划功能池           165
```

项目策略不是强制完成 165 分。

优先级为：

```text
120 分稳定完成
        ↓
135 分稳定完成
        ↓
网络扩展
        ↓
其他创新功能
```

---

# 15. 为什么不一开始开发网络版

虽然本组具有 Alibaba Cloud ECS，可以实现网络备份，但 Sprint 0～Sprint 早期禁止网络功能影响核心闭环。

正确顺序：

```text
本地备份可用
	↓
本地恢复可用
	↓
元数据可用
	↓
归档可用
	↓
压缩可用
	↓
加密可用
	↓
自动化可用
	↓
远程存储
```

不能采用：

```text
Web + ECS + Network + Backup + Compression + Encryption
```

全部同时开发。

---

# 16. 开发语言和技术栈

## 16.1 开发平台

建议统一：

```text
Ubuntu 24.04 LTS VM
```

最终版本由三名成员确认。

需要统一记录：

```text
Ubuntu Version
GCC Version
G++ Version
Python Version
Make Version
Git Version
```

---

## 16.2 C++ Core

```text
Language: C++
Standard: C++17
Compiler: GCC / G++
```

C++ 负责：

- 文件系统访问；
- 文件扫描；
- Backup Engine；
- Restore Engine；
- Metadata；
- Filter；
- Archive；
- Compression；
- Encryption；
- Scheduler；
- File Watcher；
- 网络核心；
- Storage Backend。

---

# 17. Web Frontend

前端：

```text
HTML
CSS
JavaScript
```

---

# 18. Python Web Bridge

Python 仅作为薄层：

```text
Browser
   ↓
Python Web Bridge
   ↓
C++ Core
```

允许负责：

- HTTP；
- 静态文件；
- 请求解析；
- 调用 C++；
- 状态返回；
- WebSocket / Streaming 等前端通信辅助。

原则：

> Python 不能替代 C++ Core 完成评分功能。

禁止把以下逻辑迁移给 Python：

```text
Backup
Archive
Compression
Encryption
Incremental Backup
File Metadata Core
```

---

# 19. 第三方库政策

基本原则：

> 允许使用语言基础设施、操作系统原语和开发工具；不使用已经替我们完成评分算法的现成实现。

---

## 19.1 标准库允许

包括但不限于：

```cpp
<cstdio>
<cstdlib>
<cstdint>
<cstring>
<string>
<vector>
<array>
<map>
<unordered_map>
<set>
<queue>
<algorithm>
<fstream>
<iostream>
<chrono>
<thread>
<mutex>
<condition_variable>
```

---

## 19.2 Linux / POSIX API 允许

例如：

```text
open
read
write
close

opendir
readdir
closedir

stat
lstat
fstat

chmod
chown
utimensat

readlink
symlink
link

mkfifo

socket
bind
listen
accept
connect

inotify
```

原因：

> 它们提供的是操作系统原语，而不是现成的数据备份算法。

---

## 19.3 开发工具允许

```text
GCC / G++
GNU Make
gdb
Git
clang-format
cpplint
Valgrind
AddressSanitizer
UBSan
gprof
perf
```

---

## 19.4 核心功能禁止直接替代

原则上不使用：

```text
zlib
libarchive
现成 Huffman 库
现成 LZ77 / LZ78 库
rsync
现成 Backup Framework
现成 Incremental Backup Engine
```

来替代对应课程功能。

加密自主实现阶段不直接通过：

```text
OpenSSL AES API
```

完成核心算法。

---

## 19.5 禁止调用系统命令偷换实现

例如：

```cpp
system("tar ...");
system("gzip ...");
system("rsync ...");
```

不得作为对应评分功能的核心实现。

---

# 20. 第三方工具与第三方测试工具是不同概念

课程明确鼓励使用第三方测试工具。

因此：

> “不用第三方库偷懒”不等于“不使用开发和测试工具”。

我们主动使用：

```text
cpplint
Valgrind
gprof
perf
Sanitizers
```

这些不会替我们实现功能，反而属于软件工程质量证明。

---

# 21. 软件总体架构

初始组件关系：

```text
┌────────────────────────────┐
│        Web Browser         │
│ HTML / CSS / JavaScript    │
└──────────────┬─────────────┘
               │
               ▼
┌────────────────────────────┐
│      Python Web Bridge     │
└──────────────┬─────────────┘
               │
               ▼
┌──────────────────────────────────────┐
│            C++ Backup Core           │
│                                      │
│ BackupEngine       RestoreEngine     │
│ FileScanner        FilterEngine      │
│ MetadataManager    ArchiveEngine     │
│ Compressor         Encryptor         │
│ Scheduler          FileWatcher       │
│ StorageBackend                       │
└────────────────┬─────────────────────┘
                 │
          ┌──────┴────────┐
          ▼               ▼
   LocalStorage     RemoteStorage
                          │
                          ▼
                   Alibaba Cloud ECS
```

---

# 22. Core 与 UI 必须解耦

C++ Core 必须能够独立运行。

最低 CLI：

```bash
./backupctl backup SOURCE REPOSITORY

./backupctl restore REPOSITORY DESTINATION
```

Web UI 只是 Core 的一种 Frontend。

这可以同时提高：

- 可测试性；
- 可维护性；
- 演示稳定性；
- Web 开发灵活性。

---

# 23. 面向对象设计

课程推荐以面向对象方法为主。

主要目的：

> 把需求变化限制在有限模块和类中，避免牵一发而动全身。

初步候选对象：

```text
BackupJob

FileScanner
FileEntry
Metadata

Filter
FilterRule

ArchiveWriter
ArchiveReader

Compressor
HuffmanCompressor

Encryptor

StorageBackend
LocalStorage
RemoteStorage

Scheduler
FileWatcher
```

实际类设计以后续需求和代码为准。

---

# 24. 不为了 OOP 而 OOP

禁止出现无实际意义的：

```text
BackupFactoryManager
BackupServiceProvider
ArchiveFactoryManagerProvider
```

等层级。

原则：

```text
职责明确
接口清晰
低耦合
不过度抽象
```

---

# 25. 自顶向下逐步求精

虽然整体软件主要采用 OOP，具体问题分析仍使用：

> 自顶向下，逐步求精。

例如：

```text
backup
 ├── scan
 ├── filter
 ├── collect metadata
 ├── archive
 ├── compress
 ├── encrypt
 └── store
```

继续：

```text
scan
 ├── open directory
 ├── enumerate entries
 ├── determine type
 ├── collect metadata
 └── recurse
```

两种方法关系：

```text
Top-Down
    ↓
解决“怎么拆问题”

OOP
    ↓
解决“由谁负责这些职责”
```

---

# 26. 软件开发过程

课程介绍了：

- 瀑布模型；
- 原型模型；
- 增量模型；
- 螺旋模型；
- 喷泉模型；
- 敏捷开发。

本项目主要使用：

```text
Agile
+
Incremental Development
+
Prototype
```

思想。

要求：

> 每轮都有能够运行、能够展示、能够测试的软件。

---

# 27. Sprint 原则

禁止：

```text
Member A 写前端 5 周
Member B 写压缩 5 周
Member C 写网络 5 周

然后第 6 周第一次集成
```

优先：

> Vertical Slice。

每一阶段得到一个真实可运行版本。

---

# 28. 每周功能检查机制

教师明确说明：

每周可以选择一个功能点进行检查。

检查包括两部分：

```text
1. 功能演示
2. 源代码精讲
```

即：

> 不能只有效果，还必须能够解释代码实现逻辑。

每周最多检查：

```text
1 个功能点
```

通过后该组可以按照课堂安排提前结束本次实验。

因此本项目所有主要功能都必须满足：

```text
能运行
能演示
能解释
```

而不能只依赖 AI 生成后无人理解。

---

# 29. 推荐每周可验收路线

初始规划：

| 周次 | 主要可验收功能        |
| ---- | --------------------- |
| 1    | 基础 Backup / Restore |
| 2    | 元数据                |
| 3    | 自定义过滤            |
| 4    | 打包 / 解包           |
| 5    | GUI 或阶段整合        |
| 6    | 压缩 / 解压           |
| 7    | 加密 / 解密           |
| 8    | 定时 / 实时备份之一   |
| 9    | 实时备份或综合功能    |

实际检查顺序根据：

- 教师当周课程；
- 开发完成情况；
- 功能成熟度；

调整。

原则：

> 未成熟功能不为了“抢检查”强行展示。

---

# 30. UML 建模要求

根据教师最新讲解，最终重点检查：

```text
Use Case Diagram
Component Diagram
Class Diagram
Sequence Diagram
```

另外：

```text
Gantt Chart
```

属于项目计划的重要评分材料。

---

# 31. 用例图

主要位于：

```text
需求分析
```

回答：

> 系统可以为参与者做什么？

参与者初步包括：

```text
Local User
Remote User
Remote Backup Server
```

实际用例包括：

```text
Backup
Restore
Configure Filter
Set Password
Configure Schedule
View Backup Status
View History
```

网络版增加：

```text
Register
Login
Remote Backup
Remote Restore
```

---

# 32. 用例描述

不能只有图。

每个核心 Use Case 同时编写文字描述。

建议字段：

```text
Use Case ID
Name
Actor
Precondition
Trigger
Main Flow
Alternative Flow
Exception Flow
Postcondition
```

并保证：

> 用例描述和用例图一致。

评分表对此单独评分。

---

# 33. Component Diagram

主要位于：

```text
系统设计
```

必须展示主要子系统关系。

例如：

```text
Web UI
Python Bridge
C++ Core
Storage Backend
Remote Server
```

同时需要文字描述每个组件：

- 主要职责；
- 输入；
- 输出；
- 依赖关系。

---

# 34. Class Diagram

必须描述核心对象。

需要关注：

- 类名；
- 主要属性；
- 主要方法；
- Association；
- Aggregation；
- Composition；
- Generalization；

等关系是否合理。

类图必须与最终实现保持较高一致性。

---

# 35. Sequence Diagram

针对主要 Use Case 绘制。

至少优先覆盖：

```text
Backup
Restore
```

进一步考虑：

```text
Scheduled Backup
Realtime Backup
Remote Backup
```

评分表明确关注：

> Sequence Diagram 对 Use Case 的覆盖程度。

因此不能只画一张无代表性的顺序图。

---

# 36. Gantt Chart

需求分析部分需要项目计划甘特图。

至少包含：

- 时间；
- Sprint；
- 任务；
- 负责人；
- 依赖；
- 计划完成时间。

Gantt Chart 必须随真实计划更新。

不能最后为了报告重新编造一份和 Git 历史完全不一致的计划。

---

# 37. UI 原型

需求分析评分明确包含：

> 用户交互方式设计。

因此 Sprint 0 / Sprint 1 阶段即建立 UI Wireframe。

至少设计：

```text
Home
Backup
Restore
Filter
Schedule
History
Settings
```

早期只需要原型，不要求立即实现。

---

# 38. UML 工具

课程使用：

```text
StarUML
```

建议保留 UML 源文件。

目录：

```text
docs/uml/
```

例如：

```text
use_case.mdj
component.mdj
class.mdj
sequence_backup.mdj
sequence_restore.mdj
```

同时导出：

```text
PDF
SVG
PNG
```

用于 Markdown 和 LaTeX。

最终论文优先使用：

```text
PDF / vector graphics
```

保证清晰度。

---

# 39. 三人分工

成员姓名确定后替换 Member A / B / C。

分工原则：

> 每个模块设置 Primary Owner，但所有核心模块至少有一名 Secondary Reviewer。

禁止形成三个互相完全不了解的独立子项目。

---

## 39.1 Member A：Core / Filesystem / Integration

主要职责：

```text
BackupEngine
RestoreEngine
FileScanner
MetadataManager
File Types
LocalStorage
```

同时负责：

```text
整体架构协调
核心接口
构建系统
持续集成
主干集成
```

文档主要负责：

```text
总体架构
Component Diagram
Core 部分 Class Diagram
Backup / Restore Sequence Diagram
```

Secondary：

```text
Member B
```

---

## 39.2 Member B：Algorithms / Quality

主要职责：

```text
Archive
Compression
Decompression
Encryption
Decryption
```

同时重点负责：

```text
Algorithm Tests
Performance Tests
Valgrind
Sanitizer
perf / gprof
```

文档主要负责：

```text
算法设计
算法测试
测试报告核心内容
```

Secondary：

```text
Member A
```

---

## 39.3 Member C：Frontend / Automation / Network

主要职责：

```text
Web UI
Python Bridge
Scheduler
inotify FileWatcher
RemoteStorage
Alibaba Cloud ECS
```

网络扩展阶段负责：

```text
User Management
Remote Metadata
Network Transfer
Incremental Backup
```

文档主要负责：

```text
UI Design
User Interaction
Use Case Diagram
Network Architecture
```

Secondary：

```text
Member A / Member B
```

---

# 40. 共同职责

以下内容禁止只由一个人完成：

```text
需求讨论
架构 Review
功能验收
Code Review
最终测试
最终演示
答辩准备
```

所有成员必须至少知道：

```text
软件如何构建
软件如何启动
Backup Pipeline
Restore Pipeline
Git 工作方式
自己的模块代码
总体架构
```

---

# 41. 组长

课堂规则：

> 分组表中每组第一名默认作为组长。

组长可以调整。

最终答辩时：

> 以课程在线表格中的组长记录为准。

组长比组员成绩：

```text
+5
```

但最终成绩不超过：

```text
100
```

组长除了行政提交外，主要负责：

- 进度协调；
- 合并冲突协调；
- 每周功能检查安排；
- 最终材料汇总；
- 答辩组织。

组长不等于：

> 承担全部核心代码。

---

# 42. Git 工作流

Repository 使用：

```text
GitHub
```

长期稳定分支：

```text
main
```

功能分支：

```text
feature/*
fix/*
test/*
docs/*
refactor/*
```

---

# 43. Branch 示例

```text
feature/file-scanner
feature/metadata
feature/archive
feature/huffman
feature/web-ui
feature/inotify
feature/remote-storage

fix/empty-directory-restore

test/archive-roundtrip

docs/use-case-v1
```

---

# 44. main 规则

`main` 应始终尽可能保持：

```text
可构建
可运行
核心测试通过
```

功能开发禁止直接长期工作在 main。

---

# 45. Git 提交要求

教师明确说明：

> Git 不能只提交一两次。

评分需要体现：

> 平时持续使用版本控制。

教师口头提出：

```text
至少 > 10 次提交
```

本组内部目标：

```text
整个开发周期 ≥ 30 个有意义 Commit
```

但：

> 不允许为了凑数量拆成无意义提交。

---

# 46. Commit Convention

采用：

```text
<type>: <description>
```

type：

```text
feat
fix
test
docs
refactor
build
style
perf
```

例如：

```text
feat: implement recursive directory scanner

feat: preserve file permission metadata

fix: correctly restore empty directory

test: add archive roundtrip tests

docs: add initial use case diagram

refactor: separate metadata from file scanner

perf: reduce duplicate disk reads
```

禁止：

```text
update
final
final2
修改
111
test123
```

---

# 47. C++ 编程规范

统一采用：

> Google C++ Style Guide

格式化：

```text
clang-format
```

`.clang-format`：

```yaml
BasedOnStyle: Google
```

---

# 48. 编译选项

至少：

```bash
-std=c++17
-Wall
-Wextra
-Wpedantic
```

Debug：

```bash
-g
```

质量检查阶段：

```bash
-fsanitize=address,undefined
```

---

# 49. C++ 基本规则

包括：

- 避免头文件 `using namespace std;`；
- 尽可能使用 RAII；
- 尽可能使用 `const`；
- 尽量避免裸 `new/delete`；
- 函数保持单一职责；
- 避免可变全局状态；
- 错误不得静默忽略；
- 接口命名统一；
- 文件、类、函数、变量命名统一；
- 公共接口和复杂算法具有必要注释。

---

# 50. 注释要求

评分表明确存在：

```text
程序注释是否达到 10% 以上
程序注释是否达到 20% 以上
```

且两项分别计分。

因此内部目标：

```text
按教师最终统计方式达到 ≥20%
```

但禁止采用大量无意义注释凑比例。

优先加入：

- 类职责；
- 公共 API；
- 复杂算法；
- 文件格式；
- 错误处理原因；
- 非直观设计决策。

---

# 51. 推荐项目目录

```text
backup-project/
│
├── README.md
├── Makefile
├── .gitignore
├── .clang-format
│
├── docs/
│   ├── 00_project_baseline.md
│   ├── 01_requirements.md
│   ├── 02_architecture.md
│   ├── 03_testing.md
│   ├── 04_release_and_demo.md
│   │
│   ├── uml/
│   ├── ui/
│   └── gantt/
│
├── report/
│   ├── main.tex
│   ├── sections/
│   ├── figures/
│   ├── tables/
│   └── appendices/
│
├── include/
│
├── src/
│   ├── core/
│   ├── filesystem/
│   ├── archive/
│   ├── compression/
│   ├── crypto/
│   ├── scheduler/
│   ├── watcher/
│   └── network/
│
├── app/
│   └── backupctl.cpp
│
├── ui/
│   ├── index.html
│   ├── css/
│   ├── js/
│   └── server.py
│
├── tests/
│   ├── unit/
│   ├── integration/
│   ├── system/
│   └── performance/
│
├── scripts/
│   ├── build.sh
│   ├── test.sh
│   ├── lint.sh
│   └── package.sh
│
└── build/
```

`build/` 不进入 Git。

---

# 52. 自动构建

教师评分表明确区分：

```text
源码能否构建成功
是否能够自动化构建
```

因此两项都必须满足。

---

## 52.1 Make

至少支持：

```bash
make
make debug
make test
make clean
```

---

## 52.2 build.sh

必须提供：

```bash
./scripts/build.sh
```

脚本负责：

1. 检查必要环境；
2. 创建构建目录；
3. 执行构建；
4. 检查返回值；
5. 正确返回 exit code。

最终目标：

> 教师拿到源码后，不需要修改 IDE 项目即可构建。

---

# 53. 软件测试总体目标

课程测试报告分值：

```text
20
```

评分表明确要求：

- 测试文档规范；
- 第三方测试工具；
- 测试用例设计；
- 测试种类；
- 测试用例数量。

因此测试从 Sprint 1 即开始。

---

# 54. 测试类型目标

为了获得完整测试种类评分，本组目标：

```text
> 4 种测试
```

计划至少包含：

### 1. Unit Test

针对：

```text
Filter
Archive
Compression
Encryption
Metadata
```

等独立模块。

### 2. Integration Test

验证模块组合。

例如：

```text
Archive + Compression
Compression + Encryption
Backup + Storage
```

### 3. Black-Box Test

只根据需求测试外部行为。

### 4. White-Box Test

针对：

- 分支；
- 边界；
- 异常路径；

设计测试。

### 5. System / End-to-End Test

测试：

```text
Backup → Restore
```

完整流程。

### 6. Performance / Stress Test

包括：

- 大文件；
- 大量小文件；
- 深层目录；
- 高文件数量；
- 实时事件压力。

因此计划实际测试类型：

```text
≥ 5
```

---

# 55. 测试用例数量

评分表设置：

```text
>10
>15
>20
>25
```

多个递进评分项。

因此内部标准：

```text
正式测试用例 ≥ 30
```

而不是刚好 26 个。

这样留出调整空间。

---

# 56. Round-Trip Test

整个项目最重要的测试模式：

```text
Original
   ↓
Backup
   ↓
Restore
   ↓
Restored
   ↓
Compare
```

目标：

```text
Original == Restored
```

---

# 57. 基础测试数据

至少包括：

```text
空文件
文本文件
UTF-8 中文文件
二进制文件
随机文件
空目录
多级目录
大量小文件
单个大文件
长文件名
带空格文件名
特殊字符路径
```

---

# 58. Linux 文件测试

根据实现功能逐步增加：

```text
Permission
UID
GID
Timestamp
Symlink
Hard Link
FIFO
Character Device
Block Device
```

设备文件测试必须注意权限和安全问题。

---

# 59. 压缩测试

核心条件：

```text
decompress(compress(x)) == x
```

同时测：

```text
empty
single-symbol
random binary
large data
highly repetitive data
already compressed-like data
```

---

# 60. 加密测试

验证：

```text
decrypt(encrypt(x, key), key) == x
```

以及：

```text
Wrong Key → Failure
Corrupted Ciphertext → Failure
```

---

# 61. 第三方测试和分析工具

为满足课程评分并提高质量，计划使用：

```text
Valgrind
AddressSanitizer
UndefinedBehaviorSanitizer
cpplint
perf
gprof
```

如后续使用：

```text
GoogleTest
```

需要明确：

> 它属于测试框架，不替代我们的备份算法。

是否采用 GoogleTest 在组内确认。

---

# 62. 评分表：源码质量

当前课堂评分表显示源码质量共：

```text
10
```

已明确子项：

| 项目                   | 分值 |
| ---------------------- | ---: |
| 源码能否构建成功       |    4 |
| 是否能自动化构建       |    1 |
| 代码格式规范程度       |    1 |
| 代码命名规范程度       |    1 |
| 源文件目录安排合理程度 |    1 |
| 注释是否达到 10% 以上  |    1 |
| 注释是否达到 20% 以上  |    1 |

因此：

> 构建失败是源码质量评分中的最高风险项。

---

# 63. 评分表：需求分析

需求分析文档：

```text
10
```

评分项：

| 项目                                   | 分值 |
| -------------------------------------- | ---: |
| 书写规范、格式正确、可读性强、内容完整 |    3 |
| 用户交互方式设计                       |    1 |
| 系统用例图规范程度                     |    2 |
| 用例描述规范程度                       |    2 |
| 用例描述与用例图一致程度               |    1 |
| 项目规划甘特图规范程度                 |    1 |

因此 `01_requirements.md` 必须包含：

```text
Requirements
UI Prototype
Use Case Diagram
Use Case Description
Gantt Chart
```

---

# 64. 评分表：系统设计

系统设计文档：

```text
20
```

评分项：

| 项目                                   | 分值 |
| -------------------------------------- | ---: |
| 书写规范、格式正确、可读性强、内容完整 |    6 |
| 构件图规范程度                         |    2 |
| 构件/子系统描述质量                    |    1 |
| 类图规范程度                           |    3 |
| 类/对象描述质量                        |    2 |
| 顺序图规范程度                         |    2 |
| 顺序图用例流程描述规范程度             |    1 |
| 顺序图对用例覆盖程度                   |    2 |
| 开发工具/库介绍                        |    1 |

因此：

> UML 是系统设计评分的核心内容，不是装饰图。

---

# 65. 评分表：软件测试

软件测试文档：

```text
20
```

当前评分项：

| 项目                                   | 分值 |
| -------------------------------------- | ---: |
| 书写规范、格式正确、可读性强、内容完整 |    6 |
| 是否使用第三方测试工具                 |    2 |
| 测试用例书写规范程度                   |    2 |
| 测试种类超过 2 种                      |    2 |
| 测试种类超过 3 种                      |    1 |
| 测试种类超过 4 种                      |  0.5 |
| 测试用例超过 10 个                     |    3 |
| 测试用例超过 15 个                     |    2 |
| 测试用例超过 20 个                     |    1 |
| 测试用例超过 25 个                     |  0.5 |

因此我们的内部目标直接设置为：

```text
测试类型 ≥ 5
测试用例 ≥ 30
使用第三方测试工具
```

---

# 66. PPT / 答辩要求

课堂评分表中 PPT 部分原有：

```text
项目管理工具使用情况
人员分工安排情况
项目进度把控情况
编程规范完善程度
版本管理工具使用情况
答辩总体感官
答辩 PPT 质量
```

教师在本次课堂中明确表示：

> “项目管理工具使用情况”这一项删除，不要求必须使用 Teambition。

因此本组：

> 不为了评分专门引入复杂项目管理工具。

GitHub Issues / Projects 是否使用由实际需要决定。

---

# 67. PPT 中必须体现

即使最后才制作 PPT，从现在开始就保存以下证据：

```text
人员分工
项目时间线
甘特图
编程规范
Git 使用过程
Commit History
版本演进
系统架构
测试结果
关键功能演示
```

编程规范必须明确写：

```text
Google C++ Style Guide
```

而不是只说：

> “代码比较规范”。

---

# 68. Git 评分证据

最终答辩准备：

```text
Commit History
Branch History
Contributor History
Feature Development History
```

截图或统计。

目标是证明：

> Git 真正用于整个开发过程，而不是答辩前一次性上传源码。

---

# 69. 演示要求

演示评分约：

```text
20
```

教师强调关注：

- 程序能否正常启动；
- 能否按预定方案完成演示；
- 教师随机操作是否出现 Bug；
- 易用性；
- 健壮性。

因此最终演示不能只准备：

> 一条固定 Happy Path。

还需要准备随机操作和异常输入。

---

# 70. AI 课程证书

证书部分：

```text
10
```

课程要求完成指定 AI 课程/实验并取得证书。

教师说明流程包括：

1. 报名；
2. 完成在线实验；
3. 完成课程任务；
4. 领取约 300 元阿里云资源包；
5. 完成指定笔记系统任务；
6. 提交任务截图；
7. 获取 AI 证书。

三位成员原则上均完成。

最终证书：

> 作为小组报告附件加入报告最后。

---

# 71. Markdown 文档阶段

项目初期使用 Markdown。

原因：

- Git diff 清晰；
- Merge 冲突容易处理；
- 修改速度快；
- 适合频繁迭代；
- 方便 Review。

因此前中期：

> Markdown 是工程文档的 Source of Truth。

---

# 72. LaTeX 最终文档阶段

最终正式报告全部转为 LaTeX。

计划：

```text
report/
├── main.tex
├── sections/
│   ├── requirements.tex
│   ├── architecture.tex
│   ├── testing.tex
│   └── appendix.tex
├── figures/
├── tables/
└── appendices/
```

---

# 73. Markdown → LaTeX 转换原则

早期：

```text
Markdown authoritative
```

进入报告冻结阶段后：

```text
LaTeX authoritative
```

不能长期同时人工维护两个完全独立版本。

否则极易发生：

```text
Markdown 已更新
LaTeX 忘记更新
```

的问题。

---

# 74. LaTeX 编译规范

建议：

```text
XeLaTeX
```

自动构建：

```bash
latexmk -xelatex main.tex
```

如果教师提供正式 LaTeX 模板：

> 教师模板具有最高优先级。

---

# 75. LaTeX 编码

统一：

```text
UTF-8
```

中文推荐：

```latex
\documentclass{ctexart}
```

或最终使用教师模板。

---

# 76. LaTeX 基本规范

禁止手工维护：

```text
图编号
表编号
章节编号
交叉引用编号
```

统一使用：

```latex
\label{}
\ref{}
```

例如：

```latex
\begin{figure}
    \centering
    \includegraphics[width=0.9\textwidth]{figures/component.pdf}
    \caption{系统构件图}
    \label{fig:component}
\end{figure}
```

正文：

```latex
如图~\ref{fig:component} 所示。
```

---

# 77. LaTeX 图形规范

UML 与 Gantt 优先导出：

```text
PDF
```

等矢量格式。

不优先使用课堂拍照或模糊截图作为正式系统图。

截图仅用于：

```text
运行结果
GUI
测试结果
Git 证据
```

---

# 78. LaTeX 表格规范

优先：

```latex
booktabs
longtable
tabularx
```

避免过量竖线和复杂视觉装饰。

---

# 79. 文档写作规范

要求：

- 术语统一；
- 同一对象名称前后一致；
- 图和文字一致；
- 图与代码一致；
- 需求和测试可以追踪；
- 避免仅描述实现过程而不解释设计原因。

---

# 80. Requirement Traceability

从现在开始为需求编号。

例如：

```text
FR-001 Backup Directory
FR-002 Restore Backup
FR-003 Preserve Metadata
FR-004 File Filtering
FR-005 Archive
FR-006 Compression
FR-007 Encryption
FR-008 Scheduled Backup
FR-009 Realtime Backup
```

测试用例：

```text
TC-FR001-01
TC-FR001-02
```

这样最终可以建立：

```text
Requirement
    ↓
Design
    ↓
Implementation
    ↓
Test
```

完整追踪链。

这对最终需求、设计和测试文档一致性非常有帮助。

---

# 81. Sprint 0

目标：

> 建立项目工程基础。

完成：

- Repository；
- Linux 环境；
- 编程规范；
- 功能池；
- 分工；
- Git Workflow；
- 技术栈；
- 初始 UML；
- UI Wireframe；
- Gantt Chart；
- Makefile；
- build.sh；
- 文档结构。

---

# 82. Sprint 1

目标：

```text
v0.1
```

实现最小闭环：

```bash
./backupctl backup source repository

./backupctl restore repository restored
```

支持：

```text
普通文件
普通目录
空目录
多级目录
二进制文件
```

测试：

```bash
diff -r source restored
```

---

# 83. 后续版本建议

```text
v0.1  Basic Backup / Restore
v0.2  Metadata
v0.3  Filter
v0.4  Archive
v0.5  Compression
v0.6  Encryption
v0.7  Web GUI
v0.8  Scheduled Backup
v0.9  Realtime Backup
v1.0  Stable Local Release
v1.1  Special File Support
v1.2  Remote Backup
```

具体顺序允许调整。

---

# 84. Definition of Done

任何 Feature 只有全部满足以下条件才能标记 Done。

### Implementation

- [ ] 功能完成；
- [ ] 典型正常输入通过；
- [ ] 典型异常输入已处理。

### Build

- [ ] `make` 成功；
- [ ] `build.sh` 成功；
- [ ] 无新增严重 Warning。

### Test

- [ ] 对应测试存在；
- [ ] 测试通过；
- [ ] 不破坏现有测试。

### Code Quality

- [ ] clang-format；
- [ ] 命名符合规范；
- [ ] 必要注释；
- [ ] 无残留 debug hack。

### Documentation

- [ ] 需求变化已更新；
- [ ] 设计变化已更新；
- [ ] UML 如有影响已更新；
- [ ] 测试文档已更新。

### Git

- [ ] Meaningful Commit；
- [ ] Review；
- [ ] Merge 完成。

### Knowledge

- [ ] Primary Owner 可以讲清代码；
- [ ] 至少一名 Reviewer 理解实现。

---

# 85. Sprint 0 Definition of Done

- [ ] GitHub Repository 创建；
- [ ] 三名成员加入；
- [ ] 组长确认；
- [ ] Ubuntu 版本统一；
- [ ] GCC 版本统一；
- [ ] C++17 确认；
- [ ] Python 环境确认；
- [ ] Makefile 创建；
- [ ] build.sh 创建；
- [ ] `.clang-format` 创建；
- [ ] Google C++ Style 确认；
- [ ] Dependency Policy 确认；
- [ ] 功能池确认；
- [ ] 目标 ≥120 分确认；
- [ ] 三人分工确认；
- [ ] Use Case Diagram v0.1；
- [ ] Component Diagram v0.1；
- [ ] UI Wireframe v0.1；
- [ ] Gantt Chart v0.1；
- [ ] 项目目录建立；
- [ ] `make` 可成功运行；
- [ ] 本文档三人 Review；
- [ ] Baseline 状态改为 Accepted。

---

# 86. 当前禁止提前开展的内容

Sprint 0 未结束前，不投入大量时间开发：

```text
ECS
复杂网络协议
块级增量
复杂 GUI 动画
性能优化
多线程压缩
复杂数据库
```

可以调研，但不能影响：

```text
Baseline
Backup
Restore
```

主线。

---

# 87. 风险控制

## Risk 1：功能过多

措施：

```text
125 Minimum Core
135 Preferred Core
165 Full Plan
```

逐级推进。

---

## Risk 2：模块最后才集成

措施：

> 每个 Sprint 都形成完整 Vertical Slice。

---

## Risk 3：AI 生成代码但成员无法解释

课程每周检查明确要求：

```text
Demo
+
Source Code Explanation
```

因此：

> 任何进入 main 的关键代码必须至少有两名成员理解。

---

## Risk 4：文档最后突击

评分中：

```text
需求分析   10
系统设计   20
测试报告   20
```

共：

```text
50
```

因此文档必须随代码同步。

---

## Risk 5：Git 使用证据不足

措施：

- 持续 Commit；
- Feature Branch；
- Review；
- 保留版本历史。

---

## Risk 6：测试数量最后不够

目标直接设置：

```text
≥5 Test Categories
≥30 Formal Test Cases
```

避免最后人为凑用例。

---

# 88. 项目成功标准

项目不仅要求：

```text
功能分 ≥120
```

还要求：

```text
Build      Stable
Restore    Correct
UI         Usable
Software   Robust
Code       Explainable
Git        Real History
UML        Consistent
Tests      ≥5 Categories
Cases      ≥30
Docs       Complete
LaTeX      Reproducible
```

---

# 89. 当前最高优先级

现在不应该首先写 Huffman、AES 或 ECS。

当前顺序：

```text
1. 创建 GitHub Repository
2. 三人确定分工
3. 冻结本 Baseline
4. 统一 VM 环境
5. 建立项目目录
6. 建立 Makefile
7. 建立 build.sh
8. 绘制 Use Case v0.1
9. 绘制 Component Diagram v0.1
10. 绘制 UI Wireframe v0.1
11. 绘制 Gantt Chart v0.1
12. Sprint 1 实现 Backup / Restore
```

---

# 90. Baseline 修改规则

本文档允许迭代。

以下变动属于 Major Baseline Change：

- MUST 功能变化；
- 技术栈变化；
- 核心架构变化；
- 第三方依赖政策变化；
- 三人主要职责大幅变化；
- 编程规范变化。

必须由三名成员共同确认。

---

# 91. Change Log

| Version | Date       | Description                                                  |
| ------- | ---------- | ------------------------------------------------------------ |
| v0.1    | 2026-09-03 | 初始工程基线                                                 |
| v0.2    | 2026-09-03 | 增加功能规划与 LaTeX 文档策略                                |
| v0.3    | 2026-09-03 | 根据课堂评分表和新增录音补充功能池、分工、每周检查、UML、测试及最终报告规范 |

---

# 92. Baseline Approval

当前：

```text
Draft
```

三名成员 Review 完成后修改为：

```text
Accepted
```

成员：

```text
Member A:
Role:

Member B:
Role:

Member C:
Role:
```

确认日期：

```text
YYYY-MM-DD
```