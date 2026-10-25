# Backup Project 用户指南

## 支持环境

- Ubuntu 24.04.4 LTS x86_64（本轮实际验证）
- Qt 6.4.2（系统 Qt 6）
- g++ 14.2.0
- 需要 Qt6 Core/Gui/Qml/Quick/QuickControls2/Network/Concurrent/OpenGL

## 构建

```bash
make -j4 all server gui-modern
```

产物：

- `build/backup-gui-modern`：QML GUI
- `build/backupctl`：CLI
- `build/backup-server`：备份服务端
- `build/backup-server-keygen`：服务端身份密钥工具

## 启动 GUI

```bash
./build/backup-gui-modern
```

无桌面环境可用 `QT_QPA_PLATFORM=offscreen ./build/backup-gui-modern --smoke-test` 做启动自检，但这不等于真实桌面交互验收。

## 本地 CLI 备份/恢复

```bash
./build/backupctl config repository set /path/to/repository
./build/backupctl backup /path/to/source --strategy full
./build/backupctl repository list
./build/backupctl restore <file.bak> /path/to/restore
```

增量备份需已有完整基线，且只支持 MyPack、不加密：

```bash
./build/backupctl backup /path/to/source --strategy incremental
```

## 本地服务端

```bash
./build/backup-server --bind 127.0.0.1 --port 18765 \
  --root /path/to/root --db /path/to/root/metadata.db \
  --secret-file /path/to/secrets.env --transport-key-file /path/to/transport.key
```

`--db` 必须是数据库文件路径，不是目录。

## 隔离测试服务器

候选包内启动：

```bash
cd ubuntu
./scripts/run-test-server.sh
```

默认监听 `127.0.0.1:18766`，默认数据目录 `ubuntu/test-server-data`。  
可用第一个参数覆盖数据目录，用 `BACKUP_TEST_PORT` 覆盖端口：

```bash
BACKUP_TEST_PORT=18767 ./scripts/run-test-server.sh /tmp/backup-test-server
```

脚本会打印地址、实际端口、数据目录和指纹命令：

```bash
./bin/backup-server-keygen --show --key-file /tmp/backup-test-server/transport.key
```

测试账号请在本机 GUI 注册；脚本不预置账号或密码。

## 云端连接

1. 在 GUI“备份管理 → 云端仓库”填写服务器地址、端口、账号。
2. 选择直连；自建服务器可选公钥指纹或证书身份。
3. 快速备份可选择本地或云端目标。
4. 定时/实时备份的后台云端任务只支持直连 BPSEC1/BPSEC2，不支持后台 SSH 隧道。

无人值守凭据文件必须 0600，支持 `password=` 或 `password_command=`。0600 是文件权限保护，不是密码加密存储。

## 数据文件与配置

- GUI 配置：`~/.config/backup-project/backup-gui-modern/`
- 定时：`schedule.json`
- 实时：`realtime.json`
- 归属账本：`ownership-ledgers/*.ledger`
- 本地仓库：由用户选择，包含 `.bak/.manifest/.identity`

## 常见错误

- “无法连接到服务器”：检查地址、端口、服务端是否启动。
- “服务器身份指纹不匹配”：向管理员核对 pin，不要跳过校验。
- “增量备份只支持 MyPack”：改打包方式为 MyPack 且关闭加密。
- “另一个实例正在运行”：GUI/CLI 有单实例锁，先关闭已有实例。
