#!/usr/bin/env bash
#
# stage-server-release.sh —— 把服务端打成可以直接发出去的目录（PR #23）。
#
#   bash scripts/stage-server-release.sh [--out <目录>] [--allow-dirty] [--version <semver>]
#
# 产出（默认 dist/server/）：
#   bin/backup-server            服务端
#   bin/backup-server-admin      本机管理工具（不监听端口）
#   bin/backup-server-keygen     传输身份密钥工具
#   tools/backup-cert-tool       离线根与证书工具（运维在别的机器上跑）
#   bin/backup-server-admin.sh   交互式管理菜单（管理员真正会用的入口）
#   share/backup-project/official-root-ed25519.pub
#   share/backup-project/VERSION
#   docs/                        随包文档
#   BUILD-INFO.txt / MANIFEST.sha256
#
# 服务端包里**没有**任何私钥：传输身份私钥由 backup-server-keygen 在服务器
# 本机生成，根私钥永远留在离线机器上。包里只有公开的根公钥。
#
# 退出码：0 = 打包并冒烟通过。

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

OUT_DIR="dist/server"
ALLOW_DIRTY=0
# 发行版号由打包层给（packaging/build-release.sh --version）；不给就沿用老行为。
VERSION_ARG=""
while [ $# -gt 0 ]; do
  case "$1" in
    --out) OUT_DIR="$2"; shift 2 ;;
    --version) VERSION_ARG="$2"; shift 2 ;;
    --allow-dirty) ALLOW_DIRTY=1; shift ;;
    -h|--help) sed -n '2,22p' "$0"; exit 0 ;;
    *) echo "未知参数：$1" >&2; exit 2 ;;
  esac
done

if [ -n "$(git status --porcelain)" ]; then
  if [ "$ALLOW_DIRTY" -eq 0 ]; then
    echo "错误：工作树不干净，拒绝打包（要覆盖请显式给 --allow-dirty）。" >&2
    git status --short >&2
    exit 1
  fi
  DIRTY="yes"
else
  DIRTY="no"
fi

COMMIT="$(git rev-parse HEAD)"
SHORT="$(git rev-parse --short HEAD)"
# 可复现构建：有 SOURCE_DATE_EPOCH 就用 commit 时间，没有才用当前时间。
if [ -n "${SOURCE_DATE_EPOCH:-}" ]; then
  STAMP="$(date -u -d "@$SOURCE_DATE_EPOCH" +%Y-%m-%dT%H:%M:%SZ)"
else
  STAMP="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
fi

echo "== 构建服务端（commit $SHORT）=="
if ! make server cert-tool > /tmp/stage-server-build.log 2>&1; then
  echo "错误：make server cert-tool 失败，最后 15 行：" >&2
  tail -15 /tmp/stage-server-build.log >&2
  exit 1
fi

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR/bin" "$OUT_DIR/tools" "$OUT_DIR/share/backup-project" "$OUT_DIR/docs"

cp build/backup-server "$OUT_DIR/bin/backup-server"
cp build/backup-server-admin "$OUT_DIR/bin/backup-server-admin"
cp build/backup-server-keygen "$OUT_DIR/bin/backup-server-keygen"
cp build/backup-cert-tool "$OUT_DIR/tools/backup-cert-tool"
# 真正给管理员用的交互式入口是 scripts/backup-server-admin.sh。之前 staging
# 只放了底层命令，装完没有菜单可用 —— 这是 Release 可用性缺口（PR 要求 §32）。
cp scripts/backup-server-admin.sh "$OUT_DIR/bin/backup-server-admin.sh"
chmod 0755 "$OUT_DIR/bin/backup-server-admin.sh"
cp resources/security/official-root-ed25519.pub "$OUT_DIR/share/backup-project/"
for doc in docs/server-quick-start.md docs/self-hosted-server.md docs/release-layout.md docs/secure_transport.md; do
  [ -f "$doc" ] && cp "$doc" "$OUT_DIR/docs/"
done
if [ -n "$VERSION_ARG" ]; then
  printf '%s\n' "$VERSION_ARG" > "$OUT_DIR/share/backup-project/VERSION"
else
  printf '%s\n' "$(git describe --tags --always --dirty 2>/dev/null || echo "$SHORT")" > "$OUT_DIR/share/backup-project/VERSION"
fi

CXX_VERSION="$(g++ --version | head -1)"
cat > "$OUT_DIR/BUILD-INFO.txt" <<EOF
bundle            = server
version           = ${VERSION_ARG:-$(git describe --tags --always 2>/dev/null || echo "$SHORT")}
commit            = $COMMIT
commit_short      = $SHORT
built_at_utc      = $STAMP
working_tree      = $( [ "$DIRTY" = "yes" ] && echo "dirty（构建内容可能与 commit 不一致）" || echo "clean" )
compiler          = $CXX_VERSION
built_with        = make server cert-tool
private_keys      = none（服务端包内不含任何私钥）
official_root     = share/backup-project/official-root-ed25519.pub
EOF

( cd "$OUT_DIR" && find . -type f ! -name MANIFEST.sha256 -printf '%P\n' | sort | xargs sha256sum > MANIFEST.sha256 )

echo "== 冒烟测试 =="
FAILED=0
if ldd "$OUT_DIR"/bin/* "$OUT_DIR"/tools/* 2>/dev/null | grep -q "not found"; then
  echo "  FAIL ldd 报告缺少共享库" >&2
  FAILED=1
else
  echo "  PASS 共享库全部可解析"
fi
if "$OUT_DIR/bin/backup-server" --help > /dev/null 2>&1; then
  echo "  PASS backup-server --help"
else
  echo "  FAIL backup-server --help" >&2
  FAILED=1
fi
if "$OUT_DIR/tools/backup-cert-tool" --help > /dev/null 2>&1; then
  echo "  PASS backup-cert-tool --help"
else
  echo "  FAIL backup-cert-tool --help" >&2
  FAILED=1
fi
# 包里不许出现私钥。两条检查：
#   1. 文件清单必须完全落在预期集合内（多一个文件就算问题 —— 这才是
#      "有没有夹带东西"的直接证据）；
#   2. 任何文件都不许含根密钥文件的行格式（^seed-hex: <64 位十六进制>$）。
# 注意：二进制里出现 "seed-hex" 这个**字段名**是正常的（解析器要在里面），
# 所以不能拿字段名当判据 —— 第一版就是这么误报的。
UNEXPECTED="$(cd "$OUT_DIR" && find . -type f -printf '%P\n' | sort | grep -vE '^(bin/backup-server|bin/backup-server-admin|bin/backup-server-admin[.]sh|bin/backup-server-keygen|tools/backup-cert-tool|share/backup-project/official-root-ed25519[.]pub|share/backup-project/VERSION|docs/[A-Za-z0-9._-]+|BUILD-INFO[.]txt|MANIFEST[.]sha256)$' || true)"
if [ -n "$UNEXPECTED" ]; then
  echo "  FAIL 包内出现预期之外的文件：" >&2
  printf '%s\n' "$UNEXPECTED" >&2
  FAILED=1
else
  echo "  PASS 包内文件清单完全符合预期"
fi
if grep -rEl '^seed-hex: [0-9a-f]{64}$' "$OUT_DIR" 2>/dev/null | grep -q .; then
  echo "  FAIL 包内出现了根密钥文件的内容" >&2
  FAILED=1
else
  echo "  PASS 包内没有任何根密钥文件内容"
fi

echo "== 包内容 =="
( cd "$OUT_DIR" && find . -type f -printf '%10s  %P\n' | sort -k2 )
echo "文件数：$(find "$OUT_DIR" -type f | wc -l)"
if [ "$FAILED" -ne 0 ]; then
  echo "[stage-server] FAILED"
  exit 1
fi
echo "[stage-server] OK -> $OUT_DIR"
