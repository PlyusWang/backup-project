#!/usr/bin/env bash
#
# PR #20：把 backup-server 部署到阿里云 ECS。
#
#   bash scripts/deploy_aliyun_server.sh [ssh-alias] [remote-workspace]
#
# 默认 ssh-alias = aliyun-ecs，remote-workspace = ~/backup-project-server。
#
# 只做四件事：打包**服务端构建所需的源码子集**、上传、在目标机上构建、
# 安装二进制。它不做任何产品数据传输——产品数据必须走 BPNET1。
#
# 为什么在目标机构建而不是直接拷本机二进制：本机是 Ubuntu 24.04
# （glibc 2.39），ECS 是 Ubuntu 22.04（glibc 2.35），直接拷过去会因为
# GLIBC_2.38 not found 起不来。目标机有 g++ 与 libsqlite3-dev，就地构建
# 是可靠做法，也让"部署的二进制来自哪个 commit"能用源码 tarball 的
# SHA-256 对上。
#
# 上传的内容**不含** .git、testdata、任何 secret。

set -uo pipefail

ALIAS="$1"
[ -n "$ALIAS" ] || ALIAS=aliyun-ecs
WORKSPACE="$2"
[ -n "$WORKSPACE" ] || WORKSPACE=backup-project-server
REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
STAGE_DIR="$PR20_STAGE_DIR"
[ -n "$STAGE_DIR" ] || STAGE_DIR=/tmp/pr20-deploy
mkdir -p "$STAGE_DIR"

cd "$REPO_DIR" || exit 1
SOURCE_SHA="$(git rev-parse HEAD)"
SHORT_SHA="$(git rev-parse --short HEAD)"
TARBALL="$STAGE_DIR/backup-server-src-$SHORT_SHA.tar.gz"

echo "[deploy] commit $SOURCE_SHA"
tar -czf "$TARBALL" Makefile include src/server src/network src/crypto third_party
sha256sum "$TARBALL" | tee "$STAGE_DIR/backup-server-src-$SHORT_SHA.sha256"

echo "[deploy] upload the source subset"
scp -q "$TARBALL" "$ALIAS:$WORKSPACE/deploy/" || exit 1

echo "[deploy] build on the target and install"
ssh -o BatchMode=yes "$ALIAS" "SHA=$SHORT_SHA bash -s" <<'REMOTE'
set -e
SRV="$HOME/backup-project-server"
cd "$SRV/deploy"
rm -rf "src-$SHA"
mkdir -p "src-$SHA"
tar -xzf "backup-server-src-$SHA.tar.gz" -C "src-$SHA"
cd "src-$SHA"
make -j2 server > build.log 2>&1 || { tail -20 build.log; exit 1; }
echo "[deploy] build warnings: $(grep -ci warning build.log || true)"
sha256sum build/backup-server | tee "$SRV/deploy/backup-server-$SHA.sha256"
install -m 0755 build/backup-server "$SRV/bin/backup-server"
echo "[deploy] installed sha256: $(sha256sum "$SRV/bin/backup-server" | cut -d' ' -f1)"
REMOTE

echo "[deploy] source tarball: $TARBALL"
