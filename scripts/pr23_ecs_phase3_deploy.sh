#!/usr/bin/env bash
#
# pr23_ecs_phase3_deploy.sh —— PR #23 Phase 3：把 BPSEC2 服务端与身份证书
# 部署到 ECS，**仍然只监听 127.0.0.1**，并且失败自动回滚。
#
#   bash scripts/pr23_ecs_phase3_deploy.sh
#
# 顺序（任何一步失败 -> 立刻回滚到部署前的二进制与启动参数）：
#   1. 在 ECS 上建 deploy-backups/pr23-before-<时间戳>/（复制真实二进制 +
#      记录当前启动命令行 + sha256 清单）；
#   2. 把新二进制、证书、payload 脚本送上去；
#   3. 安装并重启（带 --bpsec2-cert-file 与 --require-bpsec2）；
#   4. 校验：仍在 127.0.0.1:18765、**没有**公网监听、日志确认加载了证书；
#   5. 失败则回滚并再次校验。
#
# 退出码：0 = 部署成功且校验通过。

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

ECS="aliyun-ecs"
SERVER_ROOT="/home/ubuntu/backup-project-server"
set +u
CERT_SOURCE="$PR23_CERT"
set -u
if [ -z "$CERT_SOURCE" ]; then
  CERT_SOURCE="/home/pw-is-123/pr23-artifacts/server-cert/backup-project-cloud.bpcert"
fi
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"

echo "[phase3] 部署时间戳 $STAMP"

if [ ! -f "$CERT_SOURCE" ]; then
  echo "错误：找不到服务器身份证书 $CERT_SOURCE（用 PR23_CERT 指定）" >&2
  exit 1
fi
if [ ! -x build/backup-server ]; then
  echo "错误：build/backup-server 不存在，先 make server" >&2
  exit 1
fi

echo "[phase3] 1/5 上传 payload 并在 ECS 上做回滚备份"
ssh "$ECS" "mkdir -p $SERVER_ROOT/deploy/incoming" || { echo "无法创建 incoming 目录" >&2; exit 1; }
scp -q scripts/pr23_ecs_phase3_payload.sh "$ECS:$SERVER_ROOT/deploy/incoming/pr23_ecs_phase3_payload.sh" \
  || { echo "上传 payload 失败" >&2; exit 1; }
ssh "$ECS" "bash $SERVER_ROOT/deploy/incoming/pr23_ecs_phase3_payload.sh backup $SERVER_ROOT $STAMP" \
  || { echo "备份失败，未做任何改动" >&2; exit 1; }

echo "[phase3] 2/5 在目标机上就地构建并安装新二进制"
# 本机是 Ubuntu 24.04（glibc 2.39），ECS 是 22.04（glibc 2.35）：直接拷本机
# 二进制过去会 "GLIBC_2.38 not found"。所以复用 PR #20 起就在用的那条路径
# —— 打包源码子集、上传、在目标机上 make。它同时保证"部署的二进制来自哪个
# commit"能用源码 tarball 的 SHA-256 对上。
if ! bash scripts/deploy_aliyun_server.sh "$ECS" backup-project-server > /tmp/pr23/phase3-build.log 2>&1; then
  echo "[phase3] 目标机构建/安装失败，开始回滚" >&2
  tail -12 /tmp/pr23/phase3-build.log >&2
  ssh "$ECS" "bash $SERVER_ROOT/deploy/incoming/pr23_ecs_phase3_payload.sh rollback $SERVER_ROOT $STAMP" || true
  exit 1
fi
tail -3 /tmp/pr23/phase3-build.log

echo "[phase3] 2b/5 上传身份证书"
scp -q "$CERT_SOURCE" "$ECS:$SERVER_ROOT/deploy/incoming/server-identity.bpcert" \
  || { echo "上传证书失败" >&2; exit 1; }

echo "[phase3] 3/5 安装并重启（只监听 127.0.0.1）"
if ! ssh "$ECS" "bash $SERVER_ROOT/deploy/incoming/pr23_ecs_phase3_payload.sh deploy $SERVER_ROOT $STAMP"; then
  echo "[phase3] 部署失败，开始回滚" >&2
  ssh "$ECS" "bash $SERVER_ROOT/deploy/incoming/pr23_ecs_phase3_payload.sh rollback $SERVER_ROOT $STAMP" || true
  exit 1
fi

echo "[phase3] 4/5 校验"
if ssh "$ECS" "bash $SERVER_ROOT/deploy/incoming/pr23_ecs_phase3_payload.sh verify $SERVER_ROOT $STAMP"; then
  echo "[phase3] 部署成功"
else
  echo "[phase3] 校验失败，开始回滚" >&2
  ssh "$ECS" "bash $SERVER_ROOT/deploy/incoming/pr23_ecs_phase3_payload.sh rollback $SERVER_ROOT $STAMP" || true
  exit 1
fi

echo "[phase3] 5/5 记录部署后状态"
ssh "$ECS" "sha256sum $SERVER_ROOT/bin/backup-server $SERVER_ROOT/state/server-identity.bpcert; ls -la $SERVER_ROOT/deploy-backups/ | tail -4"

echo "[phase3] 全部完成（回滚点：$SERVER_ROOT/deploy-backups/pr23-before-$STAMP）"
