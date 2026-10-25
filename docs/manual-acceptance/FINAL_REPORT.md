# 最终报告

## 版本身份

- 最终 HEAD：`ac9e5c71c4c262089d39582e8cca9f91e04fd433`
- 分支：`feature/unified-product-manual-acceptance-prep`
- 基线：`a6dc901`

## 本轮修改

1. P1 负向控制改为按旧 `snapshot_id` 精确移除，不再依赖列表位置；增加反转和新快照首项的顺序无关检查。
2. 配置清单采集改为 fail-closed：find/sort/sha256sum 任一失败都使清单失败；前后清单只有都成功才比较。
3. G2/G3 日志改为 ready/injected_failures/forwarded_connections/result 实际计数。
4. 增加 config-manifest 正式门禁入口。
5. 补齐用户、安全、测试、已知问题、人工验收文档。
6. 新增正式 `scripts/run_test_server.sh`，修复无参数 `$1`/端口未定义；默认 18766，可参数覆盖；打印地址/端口/数据目录/指纹，不预置账号。

## 核心现状

- 本地完整/增量备份与恢复可用。
- 云端 BPSEC1/BPSEC2、上传下载、断点续传可用。
- 定时/实时云端备份、长期归属账本、按 ID/SHA 版本淘汰可用。
- 账本写失败/读失败均禁止不安全清理。
- GUI、CLI、服务端在干净源码构建后可启动。

## 最终门禁

- 57 个不同入口，114 条 tee 输出，failed suites = 0。
- 原始日志：`evidence/final-gate/final-gate.log`。

## 干净构建和基本往返

- 源码来自 `git archive HEAD`。
- Ubuntu 24.04.4 LTS / Qt 6.4.2 / g++ 14.2.0。
- GUI smoke、CLI help、server listen/stop 通过。
- 中文文件名完整备份→恢复，`diff -r` 0，SHA-256 一致。

## 已知限制

见 `KNOWN_ISSUES.md`。Windows 原生和真实 ECS 未执行；后台 SSH 云端未支持；密钥环未 E2E。

## 交付索引

- 完整源码：`source/`
- Ubuntu 程序：`ubuntu/bin/`
- 人工验收：`docs/MANUAL_ACCEPTANCE.md`
- 构建/启动证据：`evidence/`
