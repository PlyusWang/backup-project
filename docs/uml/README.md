# UML 图

`docs/uml/` 下有两代产物，用途不同，不要混用。

## `export/` —— 当前权威图（提交在仓库里，可直接引用）

```text
use_case.svg / .png                     用例图
class.svg / .png                        类图（只画核心职责类，不是全部文件）
component.svg / .png                    构件图
sequence_backup.svg / .png              本地备份时序
sequence_restore.svg / .png             本地还原时序（staging + 原子发布）
sequence_remote_incremental.svg / .png  远端备份与增量链还原时序
```

SVG 用于报告与 LaTeX（矢量、可缩放），PNG 用于 Markdown 与 PPT。两者由同一份几何描述生成
（`/tmp/dia_make.py` + `/tmp/dia_lib.py`，在仓库外，不随仓库分发），所以内容一致。

覆盖的视图：用例、类、构件、时序（备份 / 还原 / 远端增量）。**没有**状态图、部署图、
活动图、对象图、包图。

## `*.mdj` —— 第一版 StarUML 工程（历史）

`use_case.mdj`、`class.mdj`、`component.mdj`、`sequence_backup.mdj`、
`sequence_restore.mdj` 是项目早期用 StarUML 画的源文件，最后更新于 2026-09-17。
它们反映的是当时（Sprint 1+2）的规模：类图只有 4 个类、构件图没有服务端与网络层、
用例图只有"本地用户"一个参与者。**不要**拿它们当当前系统的说明。

保留它们是为了留痕：可以对照看出系统从"本地 CLI 备份"扩到"网络 + 增量 + 实时"的过程。

## 与其它文档的关系

- 需求侧：`docs/01_requirements.md`（用例编号 UC-01…UC-14）
- 结构侧：`docs/02_architecture.md`（分层、构件、不变量、格式清单）
- 过程侧：`docs/scrum/`、`docs/gantt/`
