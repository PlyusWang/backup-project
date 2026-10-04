# PR #22：连接层 —— pin 应用反馈 + GUI 自管的 SSH 安全通道

这一轮不改网络协议，只解决两条**人工验收发现的真实产品问题**：

1. 「服务器身份指纹」点"应用"之后界面上**什么都没有发生**；
2. 服务端只监听 ECS 的 `127.0.0.1:18765`，而 GUI 默认连本机 `127.0.0.1:18765` ——
   SSH 隧道没开时用户只得到一句 `connection refused`，产品没有把这条部署链路管起来。

## 1. "应用"为什么没反应，以及现在是什么样

旧实现：

    onClicked: remote.setServerKeyPin(page.draftServerKeyPin)

控制器合法时返回 `true`，QML 把返回值丢掉了 —— 于是用户点完"应用"，屏幕上一个
像素都没变。这是本轮修掉的第一个 bug。

现在按钮（与输入框回车）走 `remote.applyServerKeyPin(...)`，它做同样的校验与提交，
另外写下一行**看得见**的结论：

| 情形 | 界面上的结果 |
|---|---|
| 合法且是新值，当前没有活动连接 | ✓ 已应用，将在下一次连接时用于服务器身份校验 |
| 合法且是新值，当前有活动连接 | ✓ 已应用；当前连接保持不变，下次重连时生效 |
| 合法但和已生效的完全相同 | ✓ 已是当前服务器身份指纹 |
| 不合法 | 输入框下面照旧红字；**已经生效的指纹一个字节都不改**，绿字同时收起 |

"应用"**只改配置**：不联网、不登录、不断开当前连接。它不是 connection test。

## 2. 登录 / 注册不再要求用户先点"应用"

最自然的路径是"填 pin -> 填账号 -> 点登录"。旧实现会用**上一次应用过的** pin
（没有就是空），用户要么莫名失败，要么以为自己填的已经生效了。

现在 `loginWithPin(...)` / `registerAccountWithPin(...)` 在提交之前先把**当前
输入框里的** pin 校验并提交：

    填 pin -> 点登录          ✔ 立即生效（与"先点应用"完全等价）
    填 pin -> 应用 -> 登录     ✔ 同一条路径
    pin 不合法                ✘ 一个字节都不发，红字贴在指纹框下面

页面同时区分草稿与生效值：输入框里的值和已生效的值不一致时显示"尚未应用"，
不会出现"输入框显示 A、控制器实际用 B 而界面毫无提示"。

## 3. 连接方式：SSH 安全通道（默认）/ 直接连接（高级）

Remote 页新增"连接方式"，默认 **SSH 安全通道**：

    SSH 主机        aliyun-ecs（~/.ssh/config 里的别名，或 user@host）
    本地端口        留空 = 自动挑一个空闲回环端口
    远端服务地址    127.0.0.1
    远端服务端口    18765

点"建立连接"（或者直接点"登录"）时，GUI 自己起一条

    ssh -N -o BatchMode=yes -o ExitOnForwardFailure=yes -L 127.0.0.1:<自动端口>:127.0.0.1:18765 -- aliyun-ecs

并把 BPSEC1 客户端指向 `127.0.0.1:<自动端口>`。**本地端口默认不写死 18765**：
那个端口很可能已经被用户自己开的隧道占着，写死就会撞车。

直接连接模式保留原样（PR #21 的逻辑），用于服务端直接可达的场合。

### 这一层是什么、不是什么

* **是** transport / deployment utility：把 socket 送到服务端门口。
* **不是** 密码学实现的一部分：BPSEC1 的握手、X25519、HKDF、AES-GCM、HMAC 全部
  照旧在隧道**里面**跑，server pin 校验一个字节都没有少。OpenSSH 的 host key 与
  BPSEC1 的 pin 是两层独立证据（defense-in-depth），不是替代关系。

### 安全边界（硬要求，code review 时逐条对）

* 只用 `QProcess` + **参数向量**启动 ssh：没有 `system()`、没有 `sh -c`、没有
  字符串拼接，因此没有 shell injection 面。
* **不弱化** SSH host verification：不设 `StrictHostKeyChecking=no`，不设
  `UserKnownHostsFile=/dev/null`，不自动接受未知 host key。用用户自己的
  `~/.ssh/config`、`known_hosts`、`ssh-agent`。
* 不收集 SSH 口令：`BatchMode=yes` 关掉一切交互式提问，需要密码时 ssh 立刻失败，
  产品把它归类成"需要交互式认证，请先配置 SSH 密钥或 ssh-agent"，而不是把提示
  藏到后台把 GUI 卡死。
* `ssh_target` 会校验（拒绝空、`-` 开头、空白与控制字符），参数向量里再加一个 `--`。
* 自有进程才回收：`owned_` 为真才 `terminate → 有界等待 → 必要时 kill`；
  用户自己开的隧道**只借用、绝不杀**。

## 4. 失败必须分层说，不能全压成"网络错误"

| 层 | 错误类别 | 用户看到的一句话 |
|---|---|---|
| 本机 | ssh-missing | 本机找不到 ssh 命令… |
| SSH 建连 | ssh-host-unreachable | 连不上 SSH 服务器：主机名解析失败、端口不通或被拒绝… |
| SSH 身份 | ssh-hostkey | SSH 服务器身份校验失败，请先检查 SSH 配置（本程序不会自动接受未知主机密钥） |
| SSH 认证 | ssh-auth | SSH 连接需要交互式认证，请先配置 SSH 密钥或 ssh-agent |
| 转发 | ssh-forward / ssh-timeout | 本地端口转发没有建立起来 / 建立安全通道超时 |
| 隧道 | tunnel-not-ready / ssh-exit | 安全通道还没有建立 / 已断开 |
| BPSEC1 | handshake | 已经连上服务器，但 BPSEC1 安全握手没有通过 |
| 身份 pin | pin-mismatch | 服务器身份校验没有通过：当前填的指纹与服务器上的身份不符 |
| 口令 | credentials | 用户名或密码错误 |
| 服务端业务 | server / rejected / contract-* | 服务器拒绝了这个请求… |

登录 / 注册表单**不再**把连接层失败改写成"无法连接到服务器，请稍后重试" ——
那恰好是用户最不该做的事（重试一百次通道也不会自己起来）。

## 5. 通道生命周期

* GUI 启动时通道是 `stopped`；第一次需要联网时才建立（ensure tunnel -> BPSEC1 Connect）。
* 状态机是 `Stopped / Starting / Ready / Failed / Stopping`，不是 `bool connected`。
* **Ready 的判定是"真的连得上"**：`127.0.0.1:<local>` 必须真的 accept 过连接，
  用固定间隔定时器 + 单调截止时间做有界重试；"ssh 进程还活着"不算证据。
* Ready 之后仍以低频异步探测监视那个端口，连续 4 次拒绝才判定通道已死
  （单次抖动不该让界面喊"通道断了"）。
* 隧道在使用中断掉：下一次操作自动重建通道、BPSEC1 重连 + RESUME，
  **不需要用户重新登录**。
* GUI 退出：自有 ssh 被 `terminate → 有界等待 → 必要时 kill` 收掉，不留孤儿。

## 6. 本轮验证

    bash scripts/ssh_tunnel_manager_test.sh     # 71 项：进程生命周期 / 参数向量 / 就绪判定 / 超时 / 回收
    bash scripts/remote_connection_ux_test.sh   # C01..C07（真 GUI、真点击）
    bash scripts/pr22_ecs_e2e.sh aliyun-ecs     # C08..C14（真 ECS、真 SSH）
    bash scripts/final_gate.sh                  # 全部套件 + 消毒剂

`--remote-test` 里的连接层编号与交接文档一致：C01 pin 应用反馈、C02 非法 pin、
C03 登录自动采用当前输入、C04 活动连接、C05 ssh 不存在、C06 主机不存在、
C07 host key 不被信任。C08..C14（真的 SSH 目标）由 `scripts/pr22_ecs_e2e.sh`
驱动：C08 SSH 认证失败、C09 通道建立、C10 隧道通但 pin 不对、C11 登录通过、
C12 隧道断掉之后自动重连并 RESUME、C13 退出不留孤儿、C14 外部隧道不被杀。

## 7. 已知限制

* 服务端身份**仍然需要手工配置 BPSEC1 pin**。自动的、带签名的服务器身份验证
  是有意留到下一个安全 / 身份 PR 的，本轮不引入。
* 通道配置（SSH 主机 / 本地端口 / ssh 路径）与 host / port / pin 一样**只在内存**里：
  当前部署的唯一事实来源是 `~/.ssh/config`，产品不另造一套持久化。
* 端口自动分配与 ssh 绑定之间有固有的竞态窗口。产品靠两件事兜住：
  `ExitOnForwardFailure=yes` 让 ssh 失败而不是"连上了却没转发"，以及撞车时
  **有界**重挑端口（最多 3 次）。
* 本地端口被**无关程序**占用时无法与"用户自己的隧道"区分，一律按复用处理，
  安全性由 BPSEC1 的 pin 兜底（连错东西会以 `pin-mismatch` 失败）。
