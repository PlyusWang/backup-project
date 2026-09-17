# Sprint 1 + Sprint 2 阶段验收证据归档（2026-09-17）

> 本文件记录**一次真实执行**的完整环境与结果，供评审复核。
> 原始日志不入库，汇总数字与提交号在此固化。

## 1. 验收对象

| 项目 | 值 |
| --- | --- |
| 仓库 | PlyusWang/backup-project |
| 验收基线（main） | @5e1e54ef578ca541cd2ad84fedcdd99dccf24dea@ |
| 质量套件提交 | @835c049e3544bc8693edcd1e22251247af7dee83@（分支 test/sprint12-quality-validation-20260917） |
| 该提交的父提交 | @5e1e54e@（即 main，未夹带其他改动） |
| 二阶段文档分支 | docs/sprint12-closeout-20260917（基于 main） |
| 执行时间 | 2026-09-17 16:0x–16:1x（UTC+8），套件总耗时 22 s |

## 2. 环境

| 项目 | 实测值 |
| --- | --- |
| uname | @Linux 7.0.0-31-generic x86_64@ |
| 发行版 | Ubuntu 24.04 LTS |
| g++ | @g++ (Ubuntu 14.2.0-4ubuntu2~24.04.1) 14.2.0@ |
| make | @GNU Make 4.3@ |
| Qt | @6.4.2@（pkg-config Qt6Widgets；会话为 Wayland） |
| clang-format | @Ubuntu clang-format version 18.1.3 (1ubuntu1)@ |
| sanitizer | @make sanitize@：ASan + UBSan，@-g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer@ |

## 3. 执行结果

执行命令：@bash scripts/quality_test.sh@（在 test/sprint12-quality-validation-20260917 的 @835c049@ 上）

| 指标 | 结果 | 判定 |
| --- | --- | --- |
| 从零构建 @make@ | 退出 0，0 警告 | PASS |
| 从零构建 @make sanitize@ | 退出 0 | PASS |
| 编译警告总数（@-Wall -Wextra -Wpedantic@） | 0 | PASS |
| 功能基线 @scripts/test.sh@ | PASS=159 FAIL=0 | PASS |
| 质量维度用例 | PASS=63 FAIL=0 | PASS |
| 稳定性往返 | 10/10 轮 diff -r + sha256sum 一致 | PASS |
| sanitizer 调用 / 报告 | 41 / 0 | PASS |
| 套件退出码 | 0 | PASS |
| 套件总耗时 | 22 s | — |

维度分布：构建与基线 6 项、可用性 10 项、鲁棒性 29 项、稳定性 1 项（聚合 10 轮）、健壮性 17 项 = 63 项。

## 4. 稳定性明细

10 轮，每轮使用全新归档与全新目标目录，逐轮执行 @diff -r@ + 全树 @sha256sum@：

| 轮次 | 结果 | 耗时 |
| --- | --- | --- |
| 1–3 | OK | 约 48–52 ms |
| 4 | OK | 49 ms |
| 5 | OK | 49 ms |
| 6 | OK | 48 ms |
| 7 | OK | 52 ms |
| 8 | OK | 51 ms |
| 9 | OK | 51 ms |
| 10 | OK | 56 ms |

合计 **10/10 通过**，无失败、无残留数据。

## 5. 失败项

**最终结果无失败项。**

首轮执行曾出现 2 处失败（PASS=61 FAIL=2），经核查为**测试断言写错**：
US-08 期望错误信息含 @archive@（实际为 @Error: Backup file does not exist: <path>@）、
US-10 期望成功反馈包含目标路径（实际为 @Restore completed successfully.@）。
修正断言后复跑全绿；判定强度未放宽，产品代码未改动。详见 @docs/03_testing.md@ 第 6 节。

## 6. 其他检查

| 检查 | 命令 | 结果 |
| --- | --- | --- |
| 代码格式 | @bash scripts/lint.sh@ | 退出 0，clang-format 检查通过 |
| QML 静态检查 | @bash scripts/modern_gui_check.sh@ | 14 项通过 / 0 项失败 |
| GUI 构建 | @make gui-all@ | 退出 0，0 警告；backup-gui 436,536 B、backup-gui-modern 450,064 B |

## 7. 复现

@ @ @ bash
git switch test/sprint12-quality-validation-20260917    # 835c049，父提交 = main 5e1e54e
make clean && bash scripts/quality_test.sh
echo "exit=$?"    # 期望 0
@ @ @

原始日志：Ubuntu 测试机 @/tmp/backup-project-quality/@ 与 @tests/output/modern-gui-check.log@（均不入库）。

## 8. 三端一致性（阶段一完成时）

| 端 | 分支 | SHA |
| --- | --- | --- |
| Ubuntu 开发机 | test/sprint12-quality-validation-20260917 | 835c049e3544bc8693edcd1e22251247af7dee83 |
| GitHub（远端 ref） | test/sprint12-quality-validation-20260917 | 835c049e3544bc8693edcd1e22251247af7dee83 |
| Windows 中继仓库 | test/sprint12-quality-validation-20260917 | 835c049e3544bc8693edcd1e22251247af7dee83 |
| 三端 main | main | 5e1e54ef578ca541cd2ad84fedcdd99dccf24dea |

