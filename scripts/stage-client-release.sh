#!/usr/bin/env bash
#
# stage-client-release.sh —— 把客户端打成可以直接发出去的目录（PR #23）。
#
#   bash scripts/stage-client-release.sh [--out <目录>] [--no-gui] [--allow-dirty]
#                                      [--version <semver>]
#
# 产出（默认 dist/client/）：
#   bin/backupctl                                  客户端 CLI
#   bin/backup-gui-modern                          现代 GUI（--no-gui 时没有这一项）
#   share/backup-project/official-root-ed25519.pub  官方根公钥（公开材料，供核对）
#   share/backup-project/VERSION                    版本标识（git describe + commit）
#   docs/                                           随包文档（客观复制，不改内容）
#   BUILD-INFO.txt                                  构建信息：commit / 时间 / 是否含 GUI / 编译器
#   MANIFEST.sha256                                 包内每个文件的 sha256（按路径排序）
#
# 行为约定：
#   * 工作树不干净时**默认直接失败**，要覆盖必须显式 --allow-dirty，并把
#     "dirty" 写进 BUILD-INFO.txt —— 发行包和提交对不上是最难查的一类问题；
#   * 目录先清空再填，避免上一次构建的残留文件混进清单；
#   * 打完包做冒烟测试：ldd 不许有 not found，CLI 必须能 --help，
#     GUI 在 offscreen 下 10 秒内不许崩（退出码 124 = 还在跑，算通过）。
#
# 退出码：0 = 打包并冒烟通过。

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

OUT_DIR="dist/client"
WITH_GUI=1
ALLOW_DIRTY=0
# 发行版号由打包层给（packaging/build-release.sh --version）；不给就沿用
# git describe 的老行为 —— 既有调用方式完全不变。
VERSION_ARG=""
while [ $# -gt 0 ]; do
  case "$1" in
    --out) OUT_DIR="$2"; shift 2 ;;
    --version) VERSION_ARG="$2"; shift 2 ;;
    --no-gui) WITH_GUI=0; shift ;;
    --allow-dirty) ALLOW_DIRTY=1; shift ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
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

echo "== 构建客户端（commit $SHORT）=="
GUI_NOTE="included"
if [ "$WITH_GUI" -eq 1 ]; then
  if ! make client > /tmp/stage-client-build.log 2>&1; then
    echo "错误：make client 失败，最后 15 行：" >&2
    tail -15 /tmp/stage-client-build.log >&2
    exit 1
  fi
else
  GUI_NOTE="excluded (--no-gui)"
  if ! make client-cli > /tmp/stage-client-build.log 2>&1; then
    echo "错误：make client-cli 失败，最后 15 行：" >&2
    tail -15 /tmp/stage-client-build.log >&2
    exit 1
  fi
fi

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR/bin" "$OUT_DIR/share/backup-project" "$OUT_DIR/docs"

cp build/backupctl "$OUT_DIR/bin/backupctl"
if [ "$WITH_GUI" -eq 1 ]; then
  cp build/backup-gui-modern "$OUT_DIR/bin/backup-gui-modern"
fi
cp resources/security/official-root-ed25519.pub "$OUT_DIR/share/backup-project/"
for doc in docs/client-quick-start.md docs/release-layout.md docs/secure_transport.md; do
  [ -f "$doc" ] && cp "$doc" "$OUT_DIR/docs/"
done
if [ -n "$VERSION_ARG" ]; then
  printf '%s\n' "$VERSION_ARG" > "$OUT_DIR/share/backup-project/VERSION"
else
  printf '%s\n' "$(git describe --tags --always --dirty 2>/dev/null || echo "$SHORT")" > "$OUT_DIR/share/backup-project/VERSION"
fi

CXX_VERSION="$(g++ --version | awk 'NR==1')"
cat > "$OUT_DIR/BUILD-INFO.txt" <<EOF
bundle            = client
version           = ${VERSION_ARG:-$(git describe --tags --always 2>/dev/null || echo "$SHORT")}
commit            = $COMMIT
commit_short      = $SHORT
built_at_utc      = $STAMP
working_tree      = $( [ "$DIRTY" = "yes" ] && echo "dirty（构建内容可能与 commit 不一致）" || echo "clean" )
gui               = $GUI_NOTE
compiler          = $CXX_VERSION
built_with        = make $( [ "$WITH_GUI" -eq 1 ] && echo client || echo client-cli )
official_root     = share/backup-project/official-root-ed25519.pub
EOF

( cd "$OUT_DIR" && find . -type f ! -name MANIFEST.sha256 -printf '%P\n' | sort | xargs sha256sum > MANIFEST.sha256 )

echo "== 冒烟测试 =="
FAILED=0
if ldd "$OUT_DIR"/bin/* 2>/dev/null | grep -q "not found"; then
  echo "  FAIL ldd 报告缺少共享库" >&2
  ldd "$OUT_DIR"/bin/* 2>/dev/null | grep "not found" >&2
  FAILED=1
else
  echo "  PASS 共享库全部可解析（ldd 无 not found）"
fi
if "$OUT_DIR/bin/backupctl" --help > /dev/null 2>&1; then
  echo "  PASS backupctl --help"
else
  echo "  FAIL backupctl --help" >&2
  FAILED=1
fi
if [ "$WITH_GUI" -eq 1 ]; then
  # 保留 GUI 的输出："启动即退出"如果只说退出码，排查要从头再来一遍。
  QT_QPA_PLATFORM=offscreen timeout 10 "$OUT_DIR/bin/backup-gui-modern" \
    > /tmp/stage-client-gui.log 2>&1
  GUI_CODE=$?
  if [ "$GUI_CODE" -eq 124 ] || [ "$GUI_CODE" -eq 0 ]; then
    echo "  PASS GUI 在 offscreen 下启动正常（退出码 $GUI_CODE，124 = 仍在运行）"
  else
    echo "  FAIL GUI 启动即退出，退出码 $GUI_CODE" >&2
    echo "  --- GUI 输出（最后 25 行）---" >&2
    tail -25 /tmp/stage-client-gui.log >&2 || true
    FAILED=1
  fi
fi

echo "== 包内容 =="
( cd "$OUT_DIR" && find . -type f -printf '%10s  %P\n' | sort -k2 )
echo "文件数：$(find "$OUT_DIR" -type f | wc -l)"
if [ "$FAILED" -ne 0 ]; then
  echo "[stage-client] FAILED"
  exit 1
fi
echo "[stage-client] OK -> $OUT_DIR"
