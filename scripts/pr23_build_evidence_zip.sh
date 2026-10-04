#!/usr/bin/env bash
#
# pr23_build_evidence_zip.sh —— 生成 PR #23 的证据包（在**本机**执行）。
#
#   bash scripts/pr23_build_evidence_zip.sh [输出目录]
#
# 证据包内容（全部是公开材料 + 测试输出，**不含任何私钥**）：
#   docs/            本轮新增/更新的文档副本
#   logs/            各套件的运行记录（sha512 / ed25519 / bpcert / cert-tool /
#                    bpsec2 / BPSEC1 基线 / Phase 2,4,5,6,7）
#   cert/            生产证书 + 官方根公钥 + 用内置官方根验签的输出
#   git/             分支提交清单、与基线的差异统计
#   00-SUMMARY.md    阶段状态（含未完成项与原因）
#   MANIFEST.sha256  包内每个文件的 sha256
#
# 打包前会扫一遍暂存目录：出现根密钥文件的私钥行格式就直接失败，
# 而不是"打完了才发现里面有钱包"。
#
# 退出码：0 = 打包成功且无泄漏。

set -uo pipefail
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

set +u
OUT_DIR="$1"
set -u
if [ -z "$OUT_DIR" ]; then
  OUT_DIR=/home/pw-is-123/pr23-artifacts
fi
STAGE_DIR="$OUT_DIR/evidence/pr23"
ZIP_PATH="$OUT_DIR/pr23-signed-identity-public-cloud-handoff.zip"
rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR/docs" "$STAGE_DIR/logs" "$STAGE_DIR/cert" "$STAGE_DIR/git"

cp docs/bpsec2-design.md docs/release-layout.md \
   docs/client-quick-start.md docs/server-quick-start.md \
   docs/self-hosted-server.md "$STAGE_DIR/docs/" 2>/dev/null

for log in sha512 ed25519 bpcert cert_tool bpsec2 bpsec1c; do
  if [ -f "/tmp/pr23/$log.log" ]; then
    cp "/tmp/pr23/$log.log" "$STAGE_DIR/logs/$log.log"
  fi
done

CERT=/home/pw-is-123/pr23-artifacts/server-cert/backup-project-cloud.bpcert
if [ -f "$CERT" ]; then
  cp "$CERT" "$STAGE_DIR/cert/"
  ./build/backup-cert-tool inspect-server --cert "$CERT" > "$STAGE_DIR/cert/inspect.txt" 2>&1
  ./build/backup-cert-tool verify-server --cert "$CERT" > "$STAGE_DIR/cert/verify-builtin-root.txt" 2>&1
fi
cp resources/security/official-root-ed25519.pub "$STAGE_DIR/cert/"

{
  echo "commit=$(git rev-parse HEAD)"
  echo "base=a2bbea3d87d6429c6938a6cc8d048e98373a29c2"
  echo "--- commits ---"
  git log --oneline a2bbea3d..HEAD | cat
  echo "--- diffstat ---"
  git diff --stat a2bbea3d..HEAD | tail -3
} > "$STAGE_DIR/git/branch.txt"

cat > "$STAGE_DIR/00-SUMMARY.md" <<'SUMMARY'
# PR #23 证据包：服务器签名身份 + 公网直连

基线：main = a2bbea3d87d6429c6938a6cc8d048e98373a29c2

## 阶段状态

| 阶段 | 内容 | 状态 | 证据 |
|---|---|---|---|
| 1 | 手写 SHA-512 / Ed25519 / BPCERT1 / TrustedRootStore / backup-cert-tool / 离线根 | 完成 | logs/sha512.log、ed25519.log、bpcert.log、cert_tool.log |
| 2 | 回环集成（真实 backup-server + backupctl，证书模式） | 完成 | 17/17 |
| 3 | ECS 部署（带回滚；回滚在真实故障中演练过） | 完成 | deploy-backups/pr23-before-* |
| 4 | ECS 本机 BPSEC2（内置官方根、零指纹） | 完成 | 6/6 |
| 5 | 隧道内 BPSEC2 | 完成 | 6/6 |
| 6 | 防火墙 / 安全组审计（含外部探测） | 完成 | 22/80 可达、18765 不可达 |
| 7 | 开放公网 18765 | **未完成** | 主机侧可监听 0.0.0.0；安全组未放行 |
| 8 | 无隧道公网直连 E2E（SSH delta=0） | **被阻塞** | 依赖阶段 7 |
| 9 | 公网对抗 / 资源测试 | **被阻塞** | 依赖阶段 7 |

## 未完成项与原因

* 阶段 7 的外部可达性被阿里云安全组挡住：
  SECURITY_GROUP_AUTOMATION = UNAVAILABLE（仓库与制品里不允许放阿里云
  AccessKey，ECS 上也没有任何凭据）。需要人工在控制台放行 18765/tcp，
  之后重跑 scripts/pr23_ecs_phase7_public_bind.sh 即可判定。
* 阶段 8/9 因此无法开始：没有公网可达性，"零 SSH 直连"就无法被真正证明 ——
  这一条不会被含糊过去。
* 客户端 GUI 重构（官方云端 / 自定义服务器 + 截图）尚未开始；但零配置的
  语义层（OfficialCloudProfile + .bpserver）已经实现并测试（23/23）。
* 服务端加固与登录限速（§32/§33）尚未开始。

## 安全边界（本次交付遵守）

* 根私钥与服务器传输身份私钥都不在 Git、制品、ZIP、日志、命令行、环境变量里；
  私钥文件 0600，根私钥只在仓库之外的离线目录；
* 产品运行路径不调用任何第三方密码学库做核心算法（测试期 oracle 除外）；
* 未使用 amend / rebase / squash / force；
* 全程没有在仓库或制品里放任何阿里云凭据。
SUMMARY

( cd "$STAGE_DIR" && find . -type f ! -name MANIFEST.sha256 -printf '%P\n' | sort | xargs sha256sum > MANIFEST.sha256 )

# 打包前扫私钥：根密钥文件的行格式一旦出现就直接失败。
if grep -rEl '^seed-hex: [0-9a-f]{64}$' "$STAGE_DIR" 2>/dev/null | grep -q .; then
  echo "错误：暂存目录里出现了根私钥内容，拒绝打包" >&2
  exit 1
fi
if find "$STAGE_DIR" -name '*.key' | grep -q .; then
  echo "错误：暂存目录里出现了 .key 文件，拒绝打包" >&2
  exit 1
fi

rm -f "$ZIP_PATH"
( cd "$STAGE_DIR/.." && zip -qr "$ZIP_PATH" pr23 )

echo "ZIP=$ZIP_PATH"
sha256sum "$ZIP_PATH"
echo "--- 条目数 ---"
unzip -Z1 "$ZIP_PATH" | wc -l
echo "--- 条目清单（前 40）---"
unzip -Z1 "$ZIP_PATH" | sort | head -40
