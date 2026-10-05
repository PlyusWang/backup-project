# 自托管服务器（self-hosted）

给"自己有一台机器、想自己当根"的人。官方云端用的是**离线官方根**；自托管
用的是**你自己的根**，两者互不影响，也不应该混用。

## 1. 为什么自托管也要有根

没有根就只能回到"人工核对指纹"（`identity = pin`）。指纹方式仍然支持，
但每台客户端都要手工抄一遍 64 位十六进制，而且服务器换密钥时每台都要改。
自建一个根之后，客户端只认"谁签的"，服务器换密钥只要重新签一张证书。

## 2. 建根（在**离线**机器上做）

    ./build/backup-cert-tool root-init \
        --root-key /path/to/my-root.key \
        --root-id  my-org-root-a

产出两个文件：

* `my-root.key` —— **私钥**，权限 0600，只在离线机器上，绝不进 Git / 制品 /
  日志 / 命令行参数（工具里根本没有把私钥写在命令行上的入口，也会显式拒绝
  `--root-key-hex` / `--seed-hex`）；
* `my-root.key.pub` —— 公钥，一行文本，可以随便发给客户端：

      my-org-root-a ed25519:0530...66df9 unlimited

  格式是 `<root-id> <公钥> [两个时间戳 | unlimited] [active|revoked]`。
  有效期必须**显式**写：要么两个 Unix 秒，要么写 `unlimited`。只写一个数字
  或者什么都不写都会被拒绝 —— 一个笔误不该把"根 2033 年过期"悄悄变成
  "永久有效"。

## 3. 签服务器证书

    ./build/backup-cert-tool issue-server \
        --root-key /path/to/my-root.key \
        --server-id my-server \
        --server-pubkey <服务器上 backup-server-keygen 打印的公钥> \
        --out my-server.bpcert

证书只覆盖**公钥**，与服务器的私钥文件无关；服务器换密钥 = 用新公钥重签一张，
把新证书放到 `state/server-identity.bpcert` 重启即可（旧客户端只要还信任同一
个根就继续能用）。

## 4. 客户端怎么连

写一个 `.bpserver`：

    display_name = 我的服务器
    host = 192.168.1.10
    port = 18765
    identity = certificate
    expected_server_id = my-server
    trusted_roots = /etc/backup-project/my-root.key.pub

或命令行：

    backupctl remote ping --host 192.168.1.10 --port 18765 \
        --expected-server-id my-server --trusted-roots /etc/backup-project/my-root.key.pub

`expected_server_id` 不能省：它挡住"同一把根签发的另一台服务器"拿自己的证书
来冒充（否则任何一台被你签过的机器都能顶替这一台）。

## 5. 根轮换

根文件支持多行，所以轮换是"先加后撤"：

1. 生成 Root-B，把 `my-org-root-b ... unlimited` 加到客户端的根文件里，
   同时服务端用 Root-B 签新证书；
2. 等所有客户端都更新过根文件之后，把 Root-A 那行标成 `revoked`。

在 Root-A 被标 revoked 之前，用它签的老证书仍然有效。

## 6. 只用指纹也可以

不想建根就这样：

    identity = pin
    server_key_pin = sha256:<backup-server-keygen --show 打印的指纹>

服务端不需要 `--bpsec2-cert-file`，客户端也不需要 `--trusted-roots`。
代价是每台客户端都要手工确认那个指纹（这正是官方云端不想让用户做的事），
而且服务器换密钥时每台都要改。

## 7. 边界（诚实写在这里）

* 签名身份只保证"这台服务器确实是那个根批准的那把密钥"；它**不**替代
  传输加密 —— 记录层仍然是 BPSEC1 的 X25519 + HKDF-SHA256 + AES-256-CTR +
  HMAC-SHA256，证书只是把"服务器身份"从人工 pin 换成可签发、可轮换的证明；
* 本项目的密码学是手写实现（SHA-512 / Ed25519 / X25519 / AES / HMAC / HKDF），
  有官方向量、独立 oracle 与红队复核，但没有经过形式化验证或第三方审计 ——
  这一点在 `include/ed25519.h` 与 `src/crypto/ed25519.cpp` 里都写明了；
* 证书格式是自己定义的 BPCERT1，不是 X.509，不能与其它 PKI 工具互操作 ——
  这是有意的取舍（见 `include/bpcert.h` 开头）。
