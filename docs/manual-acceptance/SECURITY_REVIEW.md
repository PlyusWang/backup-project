# 安全审查

## 本地备份

- 路径遍历：归档条目经过规范化，恢复不跟随危险路径。
- 覆盖：发布使用 NoReplace；普通同名文件不会被静默覆盖。
- 实现：`src/core/file_io.cpp`、`src/network/snapshot_bundle.cpp`、`src/archive/`。
- 证据：`final_gate` 中 ustar、bundle-source-mutation、raw-archive-restore、cleanup-contract。

## BPSNAP1 发布

- 仓库目录 flock 下按 `.identity -> .manifest -> .bak` 发布，`.bak` 是提交点。
- 只清理带产品 magic 的孤儿副文件；符号链接/普通同名文件不删除。
- 证据：G2 SIGKILL 测试与 final_gate。

## 暂存目录

- `mkdtemp` 0700、`BPSCOPY1 <32hex>` 租约、flock、openat/fstatat、`(dev,ino)` 复核。
- 证据：`copy_staging_test` 24/0，final_gate。

## 传输与身份

- BPSEC1 pin 模式必须配置服务器公钥/指纹，无 TOFU。
- BPSEC2 证书模式验证可信根和 server_id；证书失败不退回 pin。
- 续传从服务端确认的偏移开始，最终 SHA-256 校验。
- 证据：`bpsec2`、`secure-transport`、`resumable-transfer`、GUI 续传探针。

## 云端自动淘汰

- 只用长期归属账本的 `snapshot_id + sha256`，显示名称不参与删除。
- 账本写入失败/读取失败时跳过淘汰。
- 保留点祖先强制保留；缺父、重名、摘要不一致时停止删除。
- 证据：`cloud-retry` 生产链、`ownership-ledger`、G4 真实删除和读错误测试。

## 账本

- 目录 0700，文件 0600，`O_NOFOLLOW`，flock，fsync。
- 损坏、截断、同 ID 不同摘要、非 0600、符号链接均拒绝。
- 证据：`ownership-ledger-test`。

## 无人值守凭据

- 支持 `password_command`，5 秒生命周期超时、输出上限 4096、进程组回收。
- 0600 文件是权限保护，不是加密；未安装系统密钥环做 E2E。
- 证据：`cloud-retry` 中 P2/凭据超时测试、`remote-test`。

## 未验证

- 真实 ECS BPSEC2 最新版联调未执行。
- Windows 原生未验收。
- 系统密钥环未 E2E。
- 后台 SSH 云端未实现。
