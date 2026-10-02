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
# PR #21 起还负责一件事：确保服务器上有 BPSEC1 传输身份私钥
# （<server-root>/state/transport.key，0600）。私钥**只在服务器本机生成**，
# 不 scp 回开发机、不进 tarball / ZIP / Git，脚本只打印公钥与指纹。
#
# 为什么在目标机构建而不是直接拷本机二进制：本机是 Ubuntu 24.04
# （glibc 2.39），ECS 是 Ubuntu 22.04（glibc 2.35），直接拷过去会因为
# GLIBC_2.38 not found 起不来。目标机有 g++ 与 libsqlite3-dev，就地构建
# 是可靠做法，也让"部署的二进制来自哪个 commit"能用源码 tarball 的
# SHA-256 对上。
#
# 上传的内容**不含** .git、testdata、任何 secret。

set -uo pipefail

ALIAS="${1:-}"
[ -n "$ALIAS" ] || ALIAS=aliyun-ecs
WORKSPACE="${2:-}"
[ -n "$WORKSPACE" ] || WORKSPACE=backup-project-server
REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
STAGE_DIR="${PR20_STAGE_DIR:-}"
[ -n "$STAGE_DIR" ] || STAGE_DIR=/tmp/pr20-deploy
mkdir -p "$STAGE_DIR"

cd "$REPO_DIR" || exit 1
SOURCE_SHA="$(git rev-parse HEAD)"
SHORT_SHA="$(git rev-parse --short HEAD)"
TARBALL="$STAGE_DIR/backup-server-src-$SHORT_SHA.tar.gz"

echo "[deploy] commit $SOURCE_SHA"
# 服务端独有源码在顶层 server/；src/network 只保留协议与客户端
# （backupctl 与 GUI 也要用）。tar 失败必须立刻中止：否则会拿上一次的
# 二进制继续部署，看起来"成功"其实部署的是旧东西。
if ! tar -czf "$TARBALL" Makefile include server src/network src/platform \
     src/crypto scripts/backup-server-admin.sh third_party; then
  echo "[deploy] cannot build the source tarball" >&2
  exit 1
fi
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
# --transport-key-file 那一把身份私钥由 backup-server-keygen 生成 / 查看，
# 所以它和 backup-server、backup-server-admin 一样要进校验清单、进 bin/。
sha256sum build/backup-server build/backup-server-admin build/backup-server-keygen \
  | tee "$SRV/deploy/backup-server-$SHA.sha256"
install -m 0755 build/backup-server "$SRV/bin/backup-server"
install -m 0755 build/backup-server-admin "$SRV/bin/backup-server-admin"
install -m 0755 build/backup-server-keygen "$SRV/bin/backup-server-keygen"
install -m 0755 scripts/backup-server-admin.sh "$SRV/bin/backup-server-admin.sh"
echo "[deploy] installed sha256: $(sha256sum "$SRV/bin/backup-server" | cut -d' ' -f1)"
echo "[deploy] admin sha256: $(sha256sum "$SRV/bin/backup-server-admin" | cut -d' ' -f1)"
echo "[deploy] keygen sha256: $(sha256sum "$SRV/bin/backup-server-keygen" | cut -d' ' -f1)"

# ---- PR #21：BPSEC1 传输身份私钥 ----
# 位置固定在 <server-root>/state/transport.key（与 metadata.sqlite3 同一个状态
# 根，但不会被误当成第二个库）。**已经存在就绝不覆盖**：换掉一把在服役的身份
# 会让所有客户端的 pin 失效。私钥内容永远不打印、不离开这台机器。
TRANSPORT_KEY="$SRV/state/transport.key"
mkdir -p "$SRV/state"
if [ ! -e "$TRANSPORT_KEY" ]; then
  if ! "$SRV/bin/backup-server-keygen" --output "$TRANSPORT_KEY" \
       > "$SRV/state/transport-keygen.log" 2>&1; then
    echo "[deploy] 生成传输身份私钥失败：" >&2
    tail -3 "$SRV/state/transport-keygen.log" >&2
    exit 1
  fi
  chmod 600 "$TRANSPORT_KEY"
  echo "[deploy] 已在服务器本机生成传输身份私钥：$TRANSPORT_KEY（0600，不离开服务器）"
else
  echo "[deploy] 传输身份私钥已存在，保持不变：$TRANSPORT_KEY"
fi
if [ "$(stat -c '%a' "$TRANSPORT_KEY")" != "600" ]; then
  echo "[deploy] 传输身份私钥权限不是 0600：$TRANSPORT_KEY" >&2
  exit 1
fi
# 只打印公钥与指纹（backup-server-keygen 从不打印私钥内容）：客户端 pin 就是
# 下面这一行的 sha256:<64 位十六进制>。
"$SRV/bin/backup-server-keygen" --show --key-file "$TRANSPORT_KEY" \
  | grep -E '指纹|--server-key sha256:'
REMOTE

echo "[deploy] source tarball: $TARBALL"
