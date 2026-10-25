# 测试结果

环境：Ubuntu 24.04.4 LTS x86_64，Qt 6.4.2，g++ 14.2.0。

| 测试 | 结果 | 证据 |
|---|---|---|
| final_gate | PASS（57 入口 / 114 tee 行 / 0 失败） | evidence/final-gate/final-gate.log |
| run-test-server 无参数/端口/目录/指纹 | PASS | evidence/run-test-server.log |
| 客户端 ping/register/login + Ctrl+C | PASS | evidence/run-test-server.log |
| clean build | PASS | evidence/clean-build/build.log |
| GUI smoke（clean build） | PASS | evidence/executable-smoke/gui.log |
| CLI help | PASS | evidence/executable-smoke/cli-help.log |
| server start/listen/stop | PASS | evidence/executable-smoke/server.log |
| 本地 full backup + restore | PASS，中文文件 SHA 一致 | evidence/local-backup-restore |
| cloud-retry 生产链 | PASS（40 次、保留 36/32、恢复） | evidence/final-gate |
| 账本写失败 | PASS（旧快照逐 ID/SHA 保留） | evidence/final-gate |
| 账本读 EIO/部分读取/EINTR | PASS | evidence/final-gate |
| 实时负向控制 | PASS（failed=0，正向断言会失败） | evidence/final-gate |
| 真实 HOME/XDG 隔离 | PASS，清单错误 fail-closed | evidence/final-gate |
| BPSEC1/BPSEC2 | PASS（final gate） | evidence/final-gate |
| 断点续传 | PASS（final gate） | evidence/final-gate |
| Windows 原生 | NOT_RUN | — |
| 真实 ECS | NOT_RUN | — |

最终门禁统计以 `evidence/final-gate/final-gate.log` 为准；不同入口数与 tee 重复行分开统计。
