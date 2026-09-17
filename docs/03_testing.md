# 软件测试

> 文档编号：03
> 状态：Sprint 1 + Sprint 2 阶段收尾（2026-09-17 实测完成）
> 验收基线：main = 5e1e54e；质量回归套件提交 835c049（分支 test/sprint12-quality-validation-20260917）
> 说明：本文档的数字全部来自真实执行，复现命令见第 7 节，原始日志位置见第 8 节。

## 1. 测试目标与结论

Sprint 1（CLI 备份/恢复 + 归档格式）与 Sprint 2（两个 Qt GUI）收尾时，需要回答的不是
"某个功能对不对"，而是"这套东西能不能交付"。因此本阶段在功能套件之外，新增一个
质量回归入口 @scripts/quality_test.sh@，按四个维度取证：

| 层面 | 入口 | 覆盖 | 2026-09-17 实测 |
| --- | --- | --- | --- |
| 功能正确性 | @scripts/test.sh@ | round-trip、错误路径、路径拓扑、内容形态、元数据、损坏归档、安全、不支持的文件类型 | PASS=159 FAIL=0 |
| 可用性 | @scripts/quality_test.sh@ 维度 1 | CLI 契约、退出码、错误可诊断性、成功反馈 | 10/10 |
| 鲁棒性 | 维度 2 | 边界输入与损坏输入 | 29/29 |
| 稳定性 | 维度 3 | 连续 10 轮 backup -> restore | 10/10 轮 diff -r 与 sha256sum 一致 |
| 健壮性 | 维度 4 | ASan + UBSan 构建下的正常与异常路径 | 41 次调用，报告 0 次 |
| 代码规范 | @scripts/lint.sh@ | clang-format 检查 | 退出码 0，检查通过 |
| GUI 静态检查 | @scripts/modern_gui_check.sh@ | QML 语法/结构/忙碌守卫等 14 项 | 14/14 |
| 构建 | @make@ / @make gui-all@ / @make sanitize@ | 从零构建 | 全部 0 警告，3 个产物生成 |

**结论**：在 main = 5e1e54e 上，功能正确性、可用性、鲁棒性、稳定性、健壮性五个层面全部通过，
无已知崩溃、无 sanitizer 报告、无编译警告。未覆盖范围与已知局限见第 9 节。

## 2. 测试环境

| 项目 | 实测值 |
| --- | --- |
| 操作系统 | Ubuntu 24.04 LTS，Linux 7.0.0-31-generic x86_64 |
| 编译器 | g++ (Ubuntu 14.2.0-4ubuntu2~24.04.1) 14.2.0 |
| 构建工具 | GNU Make 4.3 |
| Qt | Qt 6.4.2（Qt6Widgets / Qt6Concurrent / Qt6Quick / Qt6QuickControls2），Wayland 会话 |
| sanitizer | AddressSanitizer + UndefinedBehaviorSanitizer（@make sanitize@，-O1 -g -fno-omit-frame-pointer） |
| 格式化检查 | clang-format 18.1.3 |
| 脚本运行时 | bash 5（@set -uo pipefail@），每个 backupctl 调用带 120s 硬超时 |

## 3. 测试分层与入口

| 层 | 入口 | 说明 |
| --- | --- | --- |
| 功能层 | @make test@ -> @scripts/test.sh@ | 159 个端到端用例，成功用例用 diff -r / cmp / stat 核对磁盘结果 |
| 质量层 | @bash scripts/quality_test.sh@ | 63 个质量用例 + 10 轮稳定性往返 + sanitizer 模糊输入 |
| 静态层 | @scripts/lint.sh@、@scripts/modern_gui_check.sh@ | 代码格式、QML 结构检查 |
| 构建层 | @make@、@make gui-all@、@make sanitize@ | CLI、两个 GUI、sanitizer 版 CLI |

质量套件与功能套件的分工是刻意的：@test.sh@ 保证"功能没退化"，@quality_test.sh@ 保证
"交付属性达标"（能不能用、扛不扛得住、稳不稳、有没有未定义行为）。

## 4. 质量套件用例明细（63 项）

### 4.1 构建与基线（6 项）

- BLD-01 从零构建成功（@make@ 与 @make sanitize@ 均退出 0）
- BLD-02 @-Wall -Wextra -Wpedantic@ 下 **0 条编译警告**
- BLD-03/BLD-04 产物 @build/backupctl@ 与 @build-sanitize/backupctl@ 可执行
- FUNC-01 功能基线 @scripts/test.sh@ 全部通过（PASS=159 FAIL=0）
- SETUP-01 参考归档生成成功

### 4.2 维度 1：可用性（10 项，US-01..US-10）

| 用例 | 断言 |
| --- | --- |
| US-01/US-02 | @--help@ 退出 0，给出用法并说明归档文件参数 |
| US-03 | 无参数 -> 退出码 2 且打印用法 |
| US-04 | 未知子命令 -> 退出码 2 |
| US-05/US-06 | backup / restore 缺参数 -> 退出码 2 |
| US-07 | 源目录不存在 -> 退出码 1，错误信息包含用户给出的源路径 |
| US-08 | 归档不存在 -> 退出码 1，错误信息说明原因（@Backup file does not exist@） |
| US-09/US-10 | backup / restore 成功时给出可读反馈（@Backup completed successfully.@ / @Restore completed successfully.@）且退出 0 |

### 4.3 维度 2：鲁棒性（29 项，RB-01..RB-20）

边界输入：

- RB-01 只含空目录的树：备份、恢复、空目录骨架一致（含空目录本身）
- RB-02 50 层深目录：备份、恢复、全树一致
- RB-03 混合树（空文件、单字节文件、中文目录、含空格与 @#%@ 的文件名、512 KiB 二进制、mode 0640）：备份、恢复、6 个文件 diff -r 与 sha256sum 一致
- RB-04 4 MiB 随机二进制：备份、恢复、@cmp@ 字节级一致

错误路径与拒绝策略：

- RB-05 源目录不存在 -> exit 1；RB-06 源是普通文件 -> exit 1
- RB-07 归档父目录不存在 -> 自动补建（mkdir -p 语义）且归档确实生成
- RB-08/RB-09 归档已存在 -> 拒绝覆盖，且原文件 sha256 未变
- RB-10/RB-11 restore 目标非空 -> 拒绝，且原有内容未被破坏
- RB-12/RB-13 源含 FIFO -> 整次备份失败，且不留下半成品归档
- RB-14 0 字节归档 -> 拒绝

损坏输入：

- RB-15 截断归档（保留前 200 字节）-> 拒绝
- RB-16 破坏 magic -> 拒绝
- RB-17 篡改全局头 entry_count（改成 0x0000FFFF）-> 拒绝
- RB-18/RB-19 归档中用 @../@ 路径替换真实 entry 路径（同长度原地改写，结构仍然合法）-> 拒绝，且文件未逃逸出恢复目标目录
- RB-20 对参考归档做 **64 次随机单字节破坏**，每次恢复 -> 0 次崩溃、0 次挂死，退出码全部落在 {0,1,2}

判定约定：退出码 124（超时被杀）与 >=128（被信号打死）一律算失败；"拒绝"类用例还要求错误信息里
出现关键原因。

### 4.4 维度 3：稳定性（10/10 轮）

对同一棵混合树连续做 10 轮"新归档 + 新目标目录"的 backup -> restore，**每一轮**都执行
@diff -r@ 与全树 @sha256sum@ 比对：

- 结果：**10/10 轮全部一致**，无失败、无残留
- 单轮往返耗时：约 48–56 ms（第 4–10 轮实测值），套件总耗时 22 s
- 每轮使用全新的归档与目标目录，因此可以排除"上一轮残留造成假通过"

### 4.5 维度 4：健壮性 / sanitizer（41 次调用，报告 0 次）

在 @build-sanitize/backupctl@（ASan + UBSan）下重跑代表性操作：

- SN-01 正常往返：退出 0 且 diff -r 与 sha256sum 一致
- SN-02/SN-02b 空目录树往返；SN-03/SN-03b 50 层深目录往返；SN-04/SN-04b 4 MiB 文件往返
- SN-05 @--help@；SN-06 源不存在；SN-07 截断归档；SN-08 magic 破坏；SN-09 entry_count 篡改；
  SN-10 路径穿越归档；SN-11 FIFO；SN-12 非空目标；SN-13 0 字节归档
- SN-14 对归档做 **24 次随机位翻转**后逐个恢复

判定：任何一次调用出现 @AddressSanitizer@ / @LeakSanitizer@ / @runtime error:@ / @SUMMARY:@ 即失败。
实测 **41 次调用、0 次报告、0 次崩溃**（含 ASan 默认打开的泄漏检测）。

### 4.6 GUI 与代码规范

- @scripts/lint.sh@：退出码 0，clang-format 检查通过
- @scripts/modern_gui_check.sh@：main 上 **14 项通过 / 0 项失败**（QML 语法、组件结构、忙碌守卫等）
- @make gui-all@：从零构建两个 GUI 成功，0 警告；产物 @build/backup-gui@（436,536 字节）与
  @build/backup-gui-modern@（450,064 字节）

## 5. 数据一致性判定方法

所有"往返一致"的结论都由两个独立证据共同支撑，缺一不可：

1. @diff -r <源> <恢复目录>@：覆盖目录结构、文件存在性、空目录、文件内容；
2. 全树 @sha256sum@ 清单（按相对路径排序）逐字节比较：覆盖 diff 无法发现的同长度内容差异。

大文件另有 @cmp@ 独立复核。测试数据统一生成在 @testdata/quality@（已 gitignore），
全套通过后自动清理；设置 @KEEP_TESTDATA=1@ 可保留现场。

## 6. 首轮实测暴露的问题（诚实记录）

第一次执行 @quality_test.sh@ 时结果为 **PASS=61 FAIL=2**，两处失败经核查**都是测试自身断言写错**，
不是产品缺陷：

| 失败用例 | 原因 | 处理 |
| --- | --- | --- |
| US-08 | 断言假设错误信息里有 @archive@ 字样，实际措辞是 @Error: Backup file does not exist: <path>@ | 按真实措辞修正断言（仍要求说明原因） |
| US-10 | 断言把恢复目标路径当关键字，实际成功反馈是 @Restore completed successfully.@ | 改为断言真实成功文案 |

修正断言后复跑为 **PASS=63 FAIL=0**。**没有为了让测试变绿而放宽判定标准**：失败用例的
判定强度未变（仍然要求退出码正确 + 关键原因出现在输出里），只是把期望值改成真实契约。
产品代码在本阶段未做任何修改。

## 7. 复现步骤

@ @ @ bash
# 1. 取到基线
cd /home/pw-is-123/backup-project
git switch test/sprint12-quality-validation-20260917   # 含质量套件，基于 main = 5e1e54e

# 2. 功能 + 质量回归（含从零构建、10 轮稳定性、sanitizer）
make clean && bash scripts/quality_test.sh
echo "exit=$?"        # 0 = 四个维度全部通过

# 3. 单独重跑各层
make test                       # 功能套件
bash scripts/lint.sh            # 代码格式
bash scripts/modern_gui_check.sh  # QML 静态检查
make gui-all                    # 两个 GUI 构建
@ @ @

失败时套件返回非 0，并把现场保留在 @testdata/quality@ 供定位。

## 8. 原始日志

原始日志不纳入版本库（体积大且含机器相关路径），位置：

- 质量套件日志：Ubuntu 测试机 @/tmp/backup-project-quality/@（@build.txt@、@functional-suite.txt@、
  @sanitizer-output.txt@、@last-output.txt@）
- GUI 静态检查日志：@tests/output/modern-gui-check.log@（已 gitignore）

可核对的汇总证据与提交号记录在 @docs/testing/sprint12_validation_20260917.md@。

## 9. 局限性与未覆盖范围

本阶段**尚未实现、因而未测试**的能力（不得当作已完成）：

- 压缩、加密、增量备份、内容哈希校验、atime/ctime 保存
- 定时备份、实时备份（inotify）、网络/远程备份
- 文件过滤（属未合并的 PR #9，不在 Sprint 1/2 验收范围内）
- 符号链接/硬链接身份、FIFO/设备/socket 的备份（当前策略是**整次失败**，不是跳过）

测试本身的局限：

- 位翻转型模糊测试是随机单字节改写，**不是覆盖率引导**的模糊测试，因此不能证明"不存在任何崩溃"
- 未采集代码覆盖率数据；未在本轮重新跑 valgrind 全量检查（以 ASan + UBSan 代替）
- TOCTOU（打包过程中源文件被改动）只有既有功能用例覆盖，未做并发压测
- GUI 的交互行为（拖拽、缩放、忙碌态、关闭守卫）目前只有静态检查；@scripts/gui_smoke_test.sh@
  需要图形会话，本阶段由人工在桌面会话中确认，未纳入自动化
- 稳定性 10 轮的规模是单棵 6 文件混合树，不代表大数据量场景的稳定性

## 10. 结论

在 main = 5e1e54e 基线上，Sprint 1 + Sprint 2 交付物的功能正确性（159/159）、可用性（10/10）、
鲁棒性（29/29）、稳定性（10/10 轮）、健壮性（41 次 sanitizer 调用 0 报告）均通过，
构建零警告、代码格式检查通过。上述局限明确列为后续 Sprint 的工作项。

