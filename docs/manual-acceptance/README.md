# Backup Project 人工验收准备

本目录的验收准备材料对应历史候选基线 `a6dc901`，不是当前 RC4 发布包的验收记录。
当前候选为 `v0.2.1-rc4`（提交 `0c07045`）；真实 ECS 已按该版本的 Release 说明完成
更新和公网复测。目录内仍保留的“真实 ECS 未验收”只描述旧基线。

本包是软件候选交付包：完整最终源码、Ubuntu 24.04 已构建程序、技术文档和验证证据。

快速开始：

1. 阅读 `docs/USER_GUIDE.md`。
2. 运行 `ubuntu/scripts/run-gui.sh` 启动 GUI。
3. 按 `docs/MANUAL_ACCEPTANCE.md` 逐项验收，使用专用测试目录。
4. 云端版本淘汰测试不得使用真实重要备份。

历史限制：依赖系统 Qt 6.4.2；该基线未验收 Windows 原生和真实 ECS。当前候选的支持
与验收范围见 `v0.2.1-rc4` Release 说明。
