# 服务端快速上手（server quick start）

面向"把 backup-server 部署到一台 Linux 服务器"的人。命令都在服务器本机执行。

## 1. 装什么

`dist/server/` 里有四个二进制（见 `docs/release-layout.md`）：

    bin/backup-server            服务端
    bin/backup-server-admin      本机管理工具（不监听任何端口）
    bin/backup-server-keygen     传输身份密钥工具
    tools/backup-cert-tool       离线根与证书工具（一般**不**在服务器上跑）

包里**没有任何私钥**。两把私钥各有各的生成地点：

* 传输身份私钥：在**服务器本机**生成（`backup-server-keygen --output <文件>`，
  0600，绝不离开服务器）；
* 根私钥：在**离线机器**上生成，永远不进服务器、不进仓库、不进制品。

## 2. 生成传输身份

    ./bin/backup-server-keygen --output ~/backup-project-server/state/transport.key

它会打印公钥与指纹（私钥内容从不打印）。**这把私钥不能换**：换了以后所有
客户端 pin/证书全部失效。

## 3. 拿到服务器身份证书

证书由**离线根**签发（在离线机器上做，或者由持有根私钥的人做）：

    ./tools/backup-cert-tool issue-server \
        --root-key /path/to/offline-root.key \
        --server-id <服务器名字，例如 backup-project-cloud-production> \
        --server-pubkey <上一步打印的公钥 hex> \
        --out server-identity.bpcert

把 `server-identity.bpcert` 拷到服务器（它只有公钥材料，可以自由复制）：

    ~/backup-project-server/state/server-identity.bpcert

服务端启动时会核对"证书里的公钥**就是**本机 transport.key 的公钥"，
不一致直接拒绝启动 —— 否则会拿着一把对不上的证书去握手，每个客户端都拒绝，
而原因要到线上才看得出来。

## 4. 启动

    ./bin/backup-server \
        --bind 127.0.0.1 --port 18765 \
        --root  ~/backup-project-server/data \
        --db    ~/backup-project-server/state/metadata.sqlite3 \
        --secret-file ~/.config/backup-project-server/secrets.env \
        --transport-key-file ~/backup-project-server/state/transport.key \
        --bpsec2-cert-file   ~/backup-project-server/state/server-identity.bpcert \
        --require-bpsec2 \
        --log-file ~/backup-project-server/logs/server.log \
        --pid-file ~/backup-project-server/state/server.pid

* `--bpsec2-cert-file`：出示身份证书（客户端因此不需要任何指纹）；
* `--require-bpsec2`：只接受签名身份客户端，收到 BPSEC1 的握手**直接拒绝**，
  不做降级。既有客户端如果还在用 pin，会连不上 —— 这是有意的。

## 5. 要开公网？先审计，再显式打开

默认**只允许监听 127.0.0.1**（给任何别的地址都会拒绝启动）。要让用户不开隧道
直连，必须同时满足三件事：

    --allow-public-bind "<一句话理由>"     # 会写进启动日志，供事后审计
    --bpsec2-cert-file <证书>
    （建议同时）--require-bpsec2

顺序不能省：先用回环把证书链路跑通，再审计防火墙与安全组（
`scripts/pr23_ecs_phase6_firewall_audit.sh`），最后才开公网
（`scripts/pr23_ecs_phase7_public_bind.sh`）。审计脚本会**从外部真的去连**，
并把"端口没开"和"端口开了但被挡住"区分开。

## 6. 核对部署

    ./tools/backup-cert-tool inspect-server --cert state/server-identity.bpcert
    ./tools/backup-cert-tool verify-server  --cert state/server-identity.bpcert \
        --roots <根公钥文件>        # 或省略 --roots 用内置官方根

`verify-server` 会打印 `verdict = TRUSTED` 或具名的失败原因；它也会检查
时间窗（±5 分钟容差），过期/未生效都会说清楚。

## 7. 回滚

`scripts/pr23_ecs_phase3_deploy.sh` 与 payload 脚本在部署前会把当前二进制
复制到 `deploy-backups/pr23-before-<时间戳>/`，并记下当时的**启动命令行**
与 sha256 清单。校验不过会自动回滚（这条路径在真实故障中演练过一次：
本机 glibc 2.39 编出来的二进制在 ECS 2.35 上起不来，脚本自动恢复了旧二进制）。

## 8. 开公网要**两处**都做：安全组 + ufw

只改安全组是不够的 —— 这是 PR #23 的 Phase 7 实际踩到的坑：安全组放行之后
从外部仍然连不上，最后定位到 ECS 上的 ufw。

ECS（Ubuntu）上 ufw 默认就是开着的，而且默认丢弃一切未显式放行的入方向流量：

    /etc/ufw/ufw.conf      ENABLED=yes
    /etc/default/ufw       DEFAULT_INPUT_POLICY=DROP

（这两个文件是**世界可读**的，不需要 sudo 就能确认 —— 用 sudo -n 读不到
ufw status 时不要就此写成「不知道」。）

完整两步：

    1) 阿里云控制台 -> ECS -> 安全组 -> 入方向 -> 放行 18765/tcp（源按需收紧）
    2) 服务器本机：sudo ufw allow 18765/tcp

### 怎么从探测结果区分原因

从外部探测时，两种失败的含义完全不同：

  * **丢包（timeout）** = 有防火墙在拦（安全组或 ufw）；
  * **拒绝（connection refused）** = 包到了主机但没人监听（服务端没起，
    或者没绑 0.0.0.0）。

所以顺序是：先用 `ss -ltn` 确认服务端确实在 `0.0.0.0:18765` 上听，
再看是不是 ufw。**如果 22/80 能连、18765 超时，那基本就是 ufw 的默认策略在丢。**

部署流程目前**没有**自动下发这条 ufw 规则（仓库里没有随部署的防火墙配置），
属于已知缺口，写入本节以免下次再花时间定位。
