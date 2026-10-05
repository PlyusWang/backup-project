# 升级服务端（upgrade）

升级要同时满足两件事：**新二进制能起来**，以及**用户数据一个字都不变**。

## 1. 升级前（30 秒）

    sudo backup-server-admin status
    sudo systemctl status backup-project-server --no-pager
    sha256sum /var/lib/backup-project-server/state/transport.key | tee /tmp/key.before
    sudo tar -C /var/lib -czf /root/backup-project-server-$(date +%F-%H%M).tgz backup-project-server

那三条信息就是升级后的核对基准：**状态**、**身份私钥摘要**、**一份可回滚的归档**。

## 2. .deb 升级

    sudo apt install ./backup-project-server_<新版本>_amd64.deb

（`apt install ./file.deb` 会自己处理"已安装的旧版本"。）

升级过程做的是：停服务 → 替换包内文件 → `daemon-reload` → 重新自检配置 →
启动服务。包的 `prerm` 在**升级**时不停服务，停与启由 systemd 的重启完成，
所以不会出现"二进制换了、服务没起来"的中间态；即使新二进制起不来，systemd 会
留在 failed 状态并保留 `journalctl` 日志，旧版本随时可以用同一个 `.deb` 装回去。

升级**不会**发生的事：

* 不重新生成 `transport.key`（换了它，所有客户端的 pin 立刻失效）；
* 不重写 `secrets.env`（token secret 变了，所有已登录会话失效）；
* 不重建数据库、不重置用户、不删备份；
* 不覆盖你改过的 `/etc/backup-project-server/server.conf`（conffile 规则：
  dpkg 会问你保留哪一个）。

## 3. 升级后核对

    sha256sum /var/lib/backup-project-server/state/transport.key   # 与 /tmp/key.before 相同
    sudo systemctl status backup-project-server                    # active (running)
    sudo backup-server-admin status                                # 用户数 / 快照数与升级前一致
    sudo backup-server-admin transport-identity                    # 指纹与升级前一致

指纹变了 = 身份私钥被换了 = 出问题了，按第 5 节回滚。

## 4. 便携安装的升级

    sudo /opt/backup-project-server/docs/../install.sh --prefix /opt/backup-project-server

便携装在同一个 `--prefix` 上再跑一次 `install.sh` 就是升级：bin/ 原子替换，
`etc/` 与 `var/` 已存在的文件一律保留。systemd unit（非 `--no-systemd`）
会被重写并 `daemon-reload`。

## 5. 回滚

    sudo apt install ./backup-project-server_<旧版本>_amd64.deb

包升级是可逆的：旧 `.deb` 还在就能装回去，数据从未被升级过程改动，所以回滚
只需要换二进制。回滚后确认：

    sha256sum /var/lib/backup-project-server/state/transport.key
    sudo systemctl status backup-project-server

## 6. 升级失败怎么办

1. **服务起不来**：`sudo journalctl -u backup-project-server -n 100 --no-pager`；
   先跑 `sudo backup-project-server --check-config`（配置问题会直接指出来）；
2. **配置自检不过**：服务不会被启动，也不会被"半启动"。修好配置再
   `sudo systemctl restart backup-project-server`；
3. **数据看着不对**：先停服务，再用第 1 节的归档恢复
   （`sudo systemctl stop backup-project-server`，解包覆盖 `/var/lib/backup-project-server`，
   `chown -R backup-project:backup-project`，再启）。

## 7. 绝对不要做的事

* 不要手工替换 `state/transport.key` —— 那等于换掉服务器身份；
* 不要在服务运行时直接覆盖二进制（用包管理器，它处理停/启）；
* 不要把 `/var/lib/backup-project-server` 删掉当作"重装"——备份就在里面；
* 不要期待卸载包会删数据：不会，这是有意的（见 `docs/install-server.md` 第 8 节）。
