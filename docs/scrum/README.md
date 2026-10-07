# Scrum / 阶段记录索引

本目录按**真实里程碑**记录各阶段的 Review 与 Retrospective，不是每周一篇。
Sprint 1 / 2 两篇写于项目早期（2026-09-17），沿用当时的 Sprint 编号；从 Phase 3
起改用"阶段（phase）"命名，因为后半程的迭代节奏与原计划"一周一个 Sprint"不再
一一对应——2026-09-26 到 09-30 之间就连续收口了四个阶段的工作。

## 1. 记录一览

| 记录 | 覆盖时间 | 对应 PR | 阶段末尾基线 | 一句话内容 |
| --- | --- | --- | --- | --- |
| `sprint1_review_retro.md` | 2026-09-03 -- 09-12 | #1、#4、#5 | PR #5 合并后 | 本地 Backup / Restore 最小闭环与危险路径拓扑修复 |
| `sprint2_review_retro.md` | 2026-09-12 | #6、#7、#8 | PR #8 合并后 | Archive v0.1 + Metadata v0.1 + 两套 GUI |
| `phase3_filter_and_repository.md` | 2026-09-17 -- 09-25 | #9、#12、#13、#14 | c640f80 | Filter 层、可视化规则编辑器、仓库驱动的产品形态 |
| `phase4_archive_pipeline.md` | 2026-09-26 | #15、#16 | 2fe004b | Archive v2 容器：打包 / 压缩 / 加密流水线 |
| `phase5_scheduler_and_realtime.md` | 2026-09-27 -- 09-30 | #17、#19 | e15c012 | 定时备份 + 保留策略、inotify 实时备份 |
| `phase6_network_and_incremental.md` | 2026-09-28 -- 10-06 | #18、#20、#21、#22、#23 | 3871a8c | 增量 delta 链、远端存储、BPSEC1 / BPSEC2、公网验收 |
| `phase7_release_and_closeout.md` | 2026-10-06 -- 10-07 | #24、#26、#27 | 6d1e2ba | 发行打包、源码质量收口、课程文档与证据归档 |

阶段划分依据是真实的合并历史（`git log --first-parent`）与每个 PR 的实际内容，
不是事后重新编排的故事线。

## 2. 这些记录里的数字从哪里来

- 功能与质量数字取自**真实执行过的** `scripts/test.sh`、`scripts/final_gate.sh`、
  `scripts/modern_gui_check.sh` 与 `scripts/comment_ratio.py`、
  `scripts/source_style_check.py`；原始日志不入库，汇总数字与基线写入记录。
- 早期两篇（Sprint 1 / 2）的数字固化在
  `docs/testing/sprint12_validation_20260917.md`，包含完整的执行环境与复现命令。
- 每条记录都标出对应的 PR 编号，可以在 `git log --merges` 中逐条对上。

## 3. 记录里不写什么

- 不写没做过的事：没有实现的能力不会被写进"已完成"，无法核实的数字不写；
- 不把测试数量当成绩宣传：数量只在需要说明覆盖范围时出现（例如某个套件自报的
  PASS 数），并且注明它是哪一条套件的结果；
- 不隐藏失败：每个阶段的 Retrospective 都记了当时真实遇到的问题与处理方式，
  包括测试竞态、打包脚本的 shell 陷阱、被清理改坏的界面合同，以及
  "某条套件从未进过 gate"这类流程漏洞。

## 4. 与其他文档的关系

| 想了解 | 看哪里 |
| --- | --- |
| 项目基线与课程评分项 | `docs/00_project_baseline.md` |
| 需求与总体架构 | `docs/01_requirements.md`、`docs/02_architecture.md` |
| 测试方法与阶段验收证据 | `docs/03_testing.md`、`docs/testing/` |
| 归档格式 | `docs/format/archive_v0.1.md`、`docs/format/archive_v2_container.md` |
| Filter、定时、实时、网络的使用说明 | `docs/filter_usage.md`、`docs/scheduled_backup_usage.md`、`docs/network_backup_usage.md` |
| 安全传输与服务器身份 | `docs/secure_transport.md`、`docs/bpsec2-design.md` |
| 发行与安装 | `docs/release-packaging.md`、`docs/install-client.md`、`docs/install-server.md`、`docs/upgrade-server.md` |
| 进度计划 | `docs/gantt/project_gantt.tex`（PDF 需在有 LaTeX 的机器上重新生成） |
