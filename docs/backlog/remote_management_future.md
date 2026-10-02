# 远程管理面的后续方向（DEFERRED / FUTURE）

本文只记录**方向**，不是实现计划，也不代表已经决定要做。
当前版本（PR #20 closure）**没有实现其中任何一项**。

## Current boundary

当前产品阶段：

    普通用户客户端（Windows / Ubuntu 桌面程序）
      C++ core ├─ Modern QML GUI
               └─ backupctl CLI
        └─ RemoteArchiveClient → BPNET1 → backup-server（ECS 127.0.0.1:18765）

    服务器管理员
      SSH 登录 ECS → 在 ECS 本机运行 backup-server-admin

管理员能力**只存在于 ECS 本机**：

* 不是 RemoteArchiveClient 的功能；
* 不是 `backupctl remote admin`；
* 不是 QML 普通用户界面的一部分；
* 不是 BPNET1 的 ADMIN opcode（协议里 0x0100..0x01FF 整段为空）；
* 不监听任何 TCP 端口。

当前**不提供**：管理员 GUI、管理员 Web、普通用户 Web portal。

## Possible future Web surfaces

如果将来确实有产品需要，可以考虑像 DSH Web 那样增加浏览器管理面。候选架构
（只记录方向；端口一律用占位符，现在不决定具体数字）：

    ECS 127.0.0.1:<USER_WEB_PORT>    普通用户 Web Surface
    ECS 127.0.0.1:<ADMIN_WEB_PORT>   管理员 Web Surface

## Authentication separation

普通用户 Web 与管理员 Web 必须是**两个安全域**，而不是"同一个 Web 登录后按
role 显示不同菜单"：

* 不同 listener / 不同 port；
* 不同 authentication realm；
* 不同 session / token；
* 不同 authorization path。

管理员身份必须是**独立管理员身份**：管理员 credential 与普通用户 password
独立设置、独立验证、独立 KDF / 存储、独立 session / token。明确禁止：

* 普通 user password → admin login；
* 普通 BPNET1 user token → admin Web token；
* `user_id == 某个值` → 自动成为管理员；
* 共用 login endpoint 之后仅靠 role 字段升级权限。

即使某个管理员本人也拥有普通备份账户，这两个身份仍然应当视为两个独立
principal / credential domain。

## Network exposure

未来如果真的实现 Web，默认仍然只 bind 127.0.0.1，访问方式沿用现在的 SSH
转发模型：

    本机 127.0.0.1:<LOCAL_PORT> --SSH LocalForward--> ECS 127.0.0.1:<REMOTE_PORT>

不开放 0.0.0.0，不新增安全组规则。本轮不写任何部署脚本、不创建 listener、
不改防火墙。

## Deferred decisions

* 管理员管理面到底是"本地 C++/QML 管理 GUI"、"loopback-only Web 管理面"，
  还是"继续只保留 SSH + 命令行"，**尚未做正式架构决定**。
* 目前的倾向：Web 管理面是值得考虑的候选——管理员本来就要通过 SSH 运维
  ECS，浏览器界面在服务器管理场景里可能比再做一个桌面 GUI 更自然。
* 这不等于"后续一定实现 Web"。

## 两种终端的使用方式

管理工具位于 ECS 本机，任何已经获得合法 ECS SSH 权限的终端都能用：

    Windows 物理机  --SSH--> ECS --本机执行--> backup-server-admin
    Ubuntu 开发 VM  --SSH--> ECS --本机执行--> backup-server-admin

不要把 Ubuntu VM 的 SSH 私钥放进仓库或任何交付物；Windows 若以后需要长期
独立访问，推荐使用独立的 Windows key，并把 public key 授权到 ECS——这是未来
的运维配置，本轮不创建。

## 本轮明确未实现

未新增：HTTP server、WebSocket、REST API、管理员 TCP port、普通用户 Web、
任何 Web 框架（cpp-httplib / Crow / Boost.Beast / QtHttpServer / Node /
Flask / FastAPI …）、nginx / 反向代理 / TLS 证书、管理员 Web 登录或 cookie、
前端资源、安全组规则；也没有新增 BPNET1 ADMIN_LOGIN / ADMIN_LIST_USERS /
ADMIN_DELETE_USER。

当前管理员能力仍然是 **LOCAL ECS ONLY**。
