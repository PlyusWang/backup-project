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
#   evidence/        PR #23 closure 结论（PRV-41 复核 + 最终收口）
#   00-SUMMARY.md    阶段状态、closure 结论与已知限制
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
   docs/self-hosted-server.md docs/pr23-prv41-closure.md \
   "$STAGE_DIR/docs/" 2>/dev/null

# Phase 7-9（公网直连）的结果汇总：不是原始终端日志，而是"命令 + 观测结果"，
# 因为那三步是在解锁 ufw 之后一次性跑完的，原始输出留在会话记录里。
if [ -f /tmp/pr23/phase7-9-results.md ]; then
  cp /tmp/pr23/phase7-9-results.md "$STAGE_DIR/logs/phase7-9-results.md"
fi

for log in sha512 ed25519 bpcert cert_tool bpsec2 bpsec1c; do
  if [ -f "/tmp/pr23/$log.log" ]; then
    cp "/tmp/pr23/$log.log" "$STAGE_DIR/logs/$log.log"
  fi
done

# PR #23 closure：evidence/ 放“结论 + 指向原始输出的坐标”，logs/ 放原始输出。
mkdir -p "$STAGE_DIR/evidence"
if [ -d /tmp/pr23/evidence ]; then
  cp /tmp/pr23/evidence/*.md "$STAGE_DIR/evidence/" 2>/dev/null
  # 人工验收的真实截图（PR #23 官方云端登录路径）也进证据包：
  # 文字说“指纹区不存在”是一回事，一张真实窗口的 PNG 是另一回事。
  cp /tmp/pr23/evidence/*.png "$STAGE_DIR/evidence/" 2>/dev/null
fi
for log in prv41-closure test-suite-final final-gate-suites ecs-provenance public-smoke official-acceptance; do
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
| 7 | 开放公网 18765 | 完成 | 0.0.0.0:18765 监听 + 外部可达（安全组 + ufw 两处） |
| 8 | 无隧道公网直连 E2E（SSH delta=0） | 完成 | 8/8，logs/public-smoke.log |
| 9 | 公网对抗 / 资源测试 | 完成 | 8/8（PID 未变、限速仍生效） |

## 最终收口（closure round）

> **统计口径（canonical）**：`scripts/test.sh` 的正式结果一律取**完整 Final Gate 环境**下的跑法
> —— GUI 已构建、GUI parity 已被执行。任何 `269/0` 都只是它的 **core/headless 子集**
> （未构建 GUI 时），不是另一个最终结果。

* `scripts/test.sh`（**canonical**：完整 Final Gate 环境，GUI 已构建，GUI parity 已执行）**PASS=279 FAIL=0**；
  拆解：core/headless 子集 = **269/0**，GUI parity = **10/0**（**279 = 269 + 10**）。
  closure 之前唯一红的就是 PRV-41（当时 headless 子集为 266/1）。
* **人工视觉验收发现并修复了一个 merge blocker**：官方云端模式仍然经过 manual-pin
  校验，登录被一句“服务器身份指纹不合法”挡死。两层根因：QML 无条件走 `*WithPin`，
  且 `BeginOperation` 无条件要求 pin 非空。修法是**控制器按模式分流**（官方模式不碰
  人工 pin，ssh / direct 一个字都不放松），并补 10 条离线回归 + `--official-acceptance`
  的真实窗口验收。证据：evidence/22-MANUAL-ACCEPTANCE-FIX.md、
  evidence/official-cloud-manual-login.png、logs/official-acceptance.log。
* **PRV-41 复核结论：产品行为正确，红的是夹具。** 夹具把源目录放在
  `<repo>/testdata/preview/grammar/...`（前缀 110）下，而那条树形的守卫前缀预算
  只有 84，于是内核 `PATH_MAX` 的检查先于项目自己的长度守卫触发。
  现在拆成两半、各自显式构造前缀：a) 项目守卫（短前缀 19）b) 内核路径上限
  （长前缀 249）；两半都断言两侧退出码恰好 1、首行完全相同、报出的是那条
  真的越界的路径、仓库里不留半个归档。详见 evidence/20-PRV41-CLOSURE.md。
* **ECS 产品溯源 PASS**：closure 未改任何产品代码（`git diff 759679c..HEAD` 对
  服务端构建输入为空），并且在目标机上用最终 HEAD 的源码子集重新构建，
  sha256 = `afb68e5827eabaafec0acbf6092d889fccdc07257fcbad28ae34188d9e2c3b17`，
  与运行中的二进制（`/proc/<pid>/exe`）逐字节一致，构建 0 warning。
  详见 logs/ecs-provenance.log。

## 已知限制（如实保留）

* **证书吊销未实现**：TrustedRootStore 只有 active / revoked 两种**根**状态，
  没有 CRL / OCSP，也没有单张服务器证书的吊销路径；轮换只能靠换根或换证书；
* **没有 fd 相对（openat）遍历**：源目录的**绝对**路径超出 `PATH_MAX` 时，
  遍历会以内核 `ENAMETOOLONG` 受控失败，而不是继续逐级下钻。本 PR 只登记
  `FUTURE: fd-relative traversal / openat-based deep-path support`，不实现
  （见 docs/pr23-prv41-closure.md）；
* **开放公网需要两处人工动作**（阿里云安全组 + 主机 ufw），
  SECURITY_GROUP_AUTOMATION = UNAVAILABLE：仓库与制品里不放任何阿里云凭据；
* 服务器证书有效期 180 天，到期后需要重新签发并重启服务端。

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
