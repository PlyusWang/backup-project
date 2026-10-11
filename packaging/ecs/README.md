# ECS 生产 profile（backup-project-server-ecs）

这里放的是 **ECS 现场那台机器上真实运行的 systemd 配置**，以及一个只读核对脚本。

## 为什么单独有一份

`packaging/server/backup-project-server.service` 面向**发行包布局**：
`User=backup-project`、`/var/lib/backup-project-server`、`/usr/bin/backup-project-server`
（启动器解析 `server.conf`）。ECS 现场不是这个布局，而是：

| 项 | ECS 现场 |
|---|---|
| 用户 | `ubuntu` |
| 工作目录 | `/home/ubuntu/backup-project-server` |
| 可执行文件 | `bin/backup-server`（裸二进制，不经启动器） |
| 身份 | `state/transport.key` + `state/server-identity.bpcert`，**强制 BPSEC2** |
| 配置 | 全部走命令行参数（没有 server.conf） |

把发行包那份照抄过来会起不来（`ProtectHome=true` 会直接挡住 `/home/ubuntu`）。

## 文件

- `backup-project-server-ecs.service` —— 与现场逐字一致的 unit。
  **不含任何密钥**：只有路径与命令行开关，都是 `systemctl show` 本来就可见的信息。
- `verify-ecs-deployment.sh` —— 在 ECS 主机上运行的**只读**核对脚本：
  unit 存在 / enabled / active、单实例、无 nohup 残留、`--require-bpsec2` 在场、
  端口监听、二进制与证书哈希、私钥权限 0600，以及"现场 unit vs 本模板"的归一化比较
  （忽略注释与空行，所以改文档不算 drift）。它不安装、不重启、不改任何配置。

## 怎么核对（只读）

```bash
# 把本目录送到主机（或直接在主机上的仓库 checkout 里跑）
scp packaging/ecs/backup-project-server-ecs.service \
    packaging/ecs/verify-ecs-deployment.sh ubuntu@<host>:/tmp/ecs-verify/
ssh ubuntu@<host> "bash /tmp/ecs-verify/verify-ecs-deployment.sh \
    /tmp/ecs-verify/backup-project-server-ecs.service"
```

## 怎么安装（**运维人员手工执行**，本仓库不自动化）

```bash
sudo install -m 0644 -o root -g root packaging/ecs/backup-project-server-ecs.service \
     /etc/systemd/system/backup-project-server-ecs.service
sudo systemd-analyze verify /etc/systemd/system/backup-project-server-ecs.service
sudo systemctl daemon-reload
sudo systemctl enable --now backup-project-server-ecs
sudo systemctl status backup-project-server-ecs --no-pager
```

迁移（从手工 `nohup` 切过来）要先**优雅**停掉旧进程再启动 unit；服务端自己有数据目录锁，
旧进程没退出时新进程起不来，所以顺序本身也是安全的：

```bash
kill -TERM "$(pgrep -f 'bin/backup-server --bind' | head -1)"   # 等它自己退出，不要 SIGKILL
sudo systemctl start backup-project-server-ecs
```

## 怎么回滚

```bash
sudo systemctl disable --now backup-project-server-ecs
sudo rm -f /etc/systemd/system/backup-project-server-ecs.service
sudo systemctl daemon-reload
cd /home/ubuntu/backup-project-server && setsid nohup ./bin/backup-server \
  --bind 0.0.0.0 --port 18765 \
  --root /home/ubuntu/backup-project-server/data \
  --db /home/ubuntu/backup-project-server/state/metadata.sqlite3 \
  --secret-file /home/ubuntu/.config/backup-project-server/secrets.env \
  --transport-key-file /home/ubuntu/backup-project-server/state/transport.key \
  --log-file /home/ubuntu/backup-project-server/logs/server.log \
  --pid-file /home/ubuntu/backup-project-server/state/server.pid \
  --bpsec2-cert-file /home/ubuntu/backup-project-server/state/server-identity.bpcert \
  --require-bpsec2 --allow-public-bind PR23-Phase7-official-cloud-direct \
  >> logs/nohup.out 2>&1 < /dev/null &
```

上面的命令行是迁移前生产进程的真实 argv（从 `/proc/<pid>/cmdline` 读出），逐字保留。

## 边界

- 本目录**不包含**任何部署自动化：不会有人 clone 完就自动改生产。
- 不在 ECS 正式服务器上安装任何构建/检查依赖。
- 开机自启由 `systemctl enable` 提供；**不**需要 crontab / rc.local，也**不应该**再有
  第二套启动机制。
