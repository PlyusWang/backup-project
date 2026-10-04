#!/usr/bin/env bash
#
# ssh_tunnel_manager_test.sh —— SshTunnelManager 的独立单元测试（PR #22）。
#
#   bash scripts/ssh_tunnel_manager_test.sh
#
# 它只链接 Qt6Core + Qt6Network 与被测的两个文件，**不构建整个 GUI**：这样
# "进程生命周期"这条最容易出错的路径在任何一台装了 Qt 基础开发包的机器上
# 都能几秒钟跑一遍，而不是要先过一遍 QML 资源编译。
#
# 依赖：pkg-config 能找到 Qt6Core / Qt6Network，moc 可执行，python3
# （替身 ssh 用它绑定端口）。缺任何一条都**明确报错**，不是静默跳过。
#
# 退出码：0 = 全部通过。

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

BUILD_DIR="${SSH_TUNNEL_BUILD_DIR:-build-ssh-tunnel}"
BINARY="$BUILD_DIR/ssh_tunnel_manager_test"

QT_PACKAGES="Qt6Core Qt6Network"
if ! pkg-config --exists $QT_PACKAGES; then
  echo "[ssh-tunnel] 未找到 Qt6Core / Qt6Network 开发包（Ubuntu: sudo apt-get install -y qt6-base-dev）" >&2
  exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
  echo "[ssh-tunnel] 需要 python3（替身 ssh 用它绑定本地端口）" >&2
  exit 1
fi

QT_CFLAGS="$(pkg-config --cflags $QT_PACKAGES)"
QT_LIBS="$(pkg-config --libs $QT_PACKAGES)"

# moc 与 Makefile 用同一套定位方式，不写死机器相关路径。
QT_TOOLS_DIR="$(qtpaths6 --query QT_INSTALL_LIBEXECS 2>/dev/null || true)"
MOC="$(command -v moc 2>/dev/null || true)"
if [ -z "$MOC" ] && [ -n "$QT_TOOLS_DIR" ] && [ -x "$QT_TOOLS_DIR/moc" ]; then
  MOC="$QT_TOOLS_DIR/moc"
fi
if [ -z "$MOC" ]; then
  echo "[ssh-tunnel] 找不到 moc（qt6-base-dev-tools）" >&2
  exit 1
fi

EXTRA_FLAGS="${SSH_TUNNEL_TEST_EXTRA_FLAGS:-}"

mkdir -p "$BUILD_DIR"
echo "[ssh-tunnel] moc ui/modern/ssh_tunnel_manager.h"
"$MOC" ui/modern/ssh_tunnel_manager.h -o "$BUILD_DIR/moc_ssh_tunnel_manager.cpp"

echo "[ssh-tunnel] 编译"
# shellcheck disable=SC2086
g++ -std=c++17 -Wall -Wextra -Wpedantic -Iinclude -Iui/modern -fPIC \
  $EXTRA_FLAGS $QT_CFLAGS \
  tests/unit/ssh_tunnel_manager_test.cpp \
  ui/modern/ssh_tunnel_manager.cpp \
  "$BUILD_DIR/moc_ssh_tunnel_manager.cpp" \
  $QT_LIBS -o "$BINARY"

if [ -n "${SSH_TUNNEL_TEST_SANITIZE:-}" ]; then
  export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1:abort_on_error=0}"
  export UBSAN_OPTIONS="${UBSAN_OPTIONS:-print_stacktrace=1}"
fi

echo "[ssh-tunnel] 运行"
"$BINARY"
echo "[ssh-tunnel] PASS"
