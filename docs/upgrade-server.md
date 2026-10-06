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
所以不会出现「二进制换了、服务没起来」的中间态。新版本如果起不来（配置自检失败 /
没进入 active / 回环端口没监听），`postinst` 会**自动把程序载荷换回升级前的版本**、
重启并验证，然后把这次升级报成失败 —— 见第 5 节与第 6 节。

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

升级还有一个**前置条件**：如果旧服务升级前是 active，`preinst` 必须先成功建立回滚快照
（先建在 `rollback.tmp.<pid>/` 里、自检通过后原子换入 `/var/cache/backup-project-server/rollback/`）。
快照建不起来时 `dpkg` 会**在解包之前**中止升级并打印 `rollback snapshot preparation failed` /
`upgrade aborted before unpack` —— 旧版本与用户状态原封不动，服务继续跑。旧服务升级前
没在跑时没有这个前提：不建快照、允许升级，也不承诺自动回滚。

**升级失败时不用你动手**：`postinst` 会先把程序载荷换回升级前的版本、重启并验证，然后带着
非零退出码失败（`packaging/ci-upgrade-rollback-test.sh` 就是这条路径的验收）。它用的是
`preinst` 在升级前留下的材料 `/var/cache/backup-project-server/rollback/`：只含二进制、
入口脚本、unit 与 tmpfiles 片段，来源是 dpkg 的旧包文件清单，每个文件都带 sha256 / 权限 /
属主。恢复时逐个校验 sha256，而且**只碰程序载荷** —— `/etc/backup-project-server` 与
`/var/lib/backup-project-server` 里的用户状态一个字节都不动。

手工回滚（想主动退版本时）还是同一条路：

    sudo apt install --reinstall ./backup-project-server_<旧版本>_amd64.deb

包升级是可逆的：旧 `.deb` 还在就能装回去，数据从未被升级过程改动，所以回滚只需要换二进制。
回滚后确认：

    sha256sum /var/lib/backup-project-server/state/transport.key
    sudo systemctl status backup-project-server

## 6. 升级失败怎么办

0. **先看 `apt` / `dpkg` 的退出码**：失败升级一定返回非零。`postinst` 的输出里有失败原因、
   回滚结果与当前服务状态：回滚成功会打印 `ROLLBACK OK`，回滚失败会明确写
   `rollback result = FAILED`，不会伪装成功；
1. **服务起不来**：`sudo journalctl -u backup-project-server -n 100 --no-pager`；
   先跑 `sudo backup-project-server --check-config`（配置问题会直接指出来）；
2. **配置自检不过**：服务不会被启动，也不会被「半启动」。修好配置再
   `sudo systemctl restart backup-project-server`；
3. **回滚之后 dpkg 说装的是新版本**：这是 Debian 的边界 —— maintainer script 没有事务回滚，
   解包时元数据就已经记成新版本（状态 `half-configured`），而磁盘上跑的是回滚后的旧版本。
   让元数据与载荷一致：

       sudo apt install --reinstall ./backup-project-server_<当前真正在跑的版本>_amd64.deb

   在没做这件事之前，`sudo dpkg --configure -a` 会被 `postinst` 拒绝（fail closed），
   以免把「其实装的是旧版本」悄悄算成升级成功；
4. **升级前旧服务本来就没在跑**：`preinst` 不会留回滚材料（没有「可运行的旧版本」可言）。
   这时 `postinst` 会明确说明，并按第 3 条的方式重装；
5. **数据看着不对**：先停服务，再用第 1 节的归档恢复
   （`sudo systemctl stop backup-project-server`，解包覆盖 `/var/lib/backup-project-server`，
   `chown -R backup-project:backup-project`，再启）。

## 7. 绝对不要做的事

* 不要手工替换 `state/transport.key` —— 那等于换掉服务器身份；
* 不要在服务运行时直接覆盖二进制（用包管理器，它处理停/启）；
* 不要把 `/var/lib/backup-project-server` 删掉当作"重装"——备份就在里面；
* 不要期待卸载包会删数据：不会，这是有意的（见 `docs/install-server.md` 第 8 节）。
