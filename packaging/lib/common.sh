#!/usr/bin/env bash
# packaging/lib/common.sh —— release 打包脚本共用的最小工具集。
#
# 这里只放"每个打包脚本都要用、而且必须只有一份实现"的东西：日志、失败即停、
# 版本号解析、SOURCE_DATE_EPOCH、清单、锁、原子发布。产品语义一律不在这里，
# 也不在任何打包脚本里重新实现 —— 安装器包装产品，不重写产品。
#
# 约定：
#   * 所有脚本 set -Eeuo pipefail，并且用 die() 报错（退出码 1）；
#   * 用户输入错误（版本号、参数）退出码 2，机器/构建错误退出码 1；
#   * 绝不 echo 任何私钥/口令内容。

set -Eeuo pipefail

PACKAGING_LIB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PACKAGING_DIR="$(cd "$PACKAGING_LIB_DIR/.." && pwd)"
REPO_ROOT="$(cd "$PACKAGING_DIR/.." && pwd)"

log()  { printf '[release] %s\n' "$*"; }
warn() { printf '[release] WARN: %s\n' "$*" >&2; }
die()  { printf '[release] ERROR: %s\n' "$*" >&2; exit 1; }
die_usage() { printf '[release] ERROR: %s\n' "$*" >&2; exit 2; }

require_tool() {
  local tool
  for tool in "$@"; do
    command -v "$tool" >/dev/null 2>&1 || die "缺少工具：$tool"
  done
}

# 工作树必须干净：制品和 commit 对不上是最难查的一类问题。
require_clean_tree() {
  cd "$REPO_ROOT"
  local dirty
  dirty="$(git status --porcelain)"
  if [ -n "$dirty" ]; then
    printf '%s\n' "$dirty" >&2
    die "工作树不干净，拒绝构建 release 制品（本轮不允许 --allow-dirty）"
  fi
}

# 版本号：只接受 semver 形状（可带 +build 元数据）。没有 tag 时调用方给
# "0.0.0+g<sha>"；打包脚本**从不**创建 tag。
validate_version() {
  local version="$1"
  case "$version" in
    ""|*[!A-Za-z0-9.+-]*) die_usage "版本号只允许 [A-Za-z0-9.+-]：'$version'" ;;
  esac
  case "$version" in
    [0-9]*) ;;
    *) die_usage "版本号必须以数字开头：'$version'" ;;
  esac
  printf '%s' "$version"
}

default_dev_version() {
  cd "$REPO_ROOT"
  printf '0.0.0+g%s' "$(git rev-parse --short=7 HEAD)"
}

# semver -> Debian 版本：'-' 之后的预发布段用 '~' 表达（~ 在 dpkg 里排序更小，
# 这正是 rc 应该的位置）。'+build' 原样保留。
deb_version_of() {
  local version="$1" head rest
  case "$version" in
    *-*) head="${version%%-*}"; rest="${version#*-}"; printf '%s~%s' "$head" "$rest" ;;
    *)   printf '%s' "$version" ;;
  esac
}

# 可复现：时间戳、locale、排序全部固定。SOURCE_DATE_EPOCH 默认取 commit 时间。
setup_reproducible_env() {
  cd "$REPO_ROOT"
  if [ -z "${SOURCE_DATE_EPOCH:-}" ]; then
    SOURCE_DATE_EPOCH="$(git log -1 --format=%ct)"
  fi
  case "$SOURCE_DATE_EPOCH" in
    ''|*[!0-9]*) die "SOURCE_DATE_EPOCH 必须是 Unix 秒：'$SOURCE_DATE_EPOCH'" ;;
  esac
  export SOURCE_DATE_EPOCH
  export TZ=UTC
  export LC_ALL=C
  export LANG=C
  log "SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH ($(date -u -d "@$SOURCE_DATE_EPOCH" +%Y-%m-%dT%H:%M:%SZ))"
}

# 目录清单：路径排序固定、只算文件、不含清单自己。
write_manifest() {
  local dir="$1" manifest="${2:-}"
  [ -n "$manifest" ] || manifest="$dir/MANIFEST.sha256"
  ( cd "$dir" && find . -type f ! -name MANIFEST.sha256 -printf '%P\n' | LC_ALL=C sort | xargs -r sha256sum > "$manifest" )
  printf '%s' "$manifest"
}

verify_manifest() {
  local dir="$1" manifest="$2"
  ( cd "$dir" && sha256sum -c --quiet "$manifest" )
}

# 并发安装/打包锁：有 flock 就用，没有就明确拒绝（不静默降级）。
with_lock() {
  local lock_file="$1"; shift
  if ! command -v flock >/dev/null 2>&1; then
    die "缺少 flock：并发安装/打包需要它，这里不做静默降级"
  fi
  mkdir -p "$(dirname "$lock_file")"
  exec 9>"$lock_file"
  flock -w 300 9 || die "等待锁超时：$lock_file"
  "$@"
}

# 原子发布：先在同一个文件系统上的临时目录里做完整棵，成功后再 rename 过去。
publish_dir() {
  local stage="$1" target="$2"
  case "$stage" in "$target"/*|"$target") die "staging 目录不能在目标目录里面：$stage" ;; esac
  rm -rf "$target.tmp.$$"
  mkdir -p "$(dirname "$target")"
  mv "$stage" "$target.tmp.$$"
  rm -rf "$target"
  mv "$target.tmp.$$" "$target"
}

sha256_of() { sha256sum "$1" | awk '{print $1}'; }
size_of()   { stat -c %s "$1"; }

# 私钥/口令的结构化扫描：不使用"字段名"这种会误报的判据。
# 只接受 32/64 字节十六进制、PEM 私钥块、以及已知的 secret 文件形状。
scan_for_secrets() {
  local root="$1" hits=0
  local patterns='-----BEGIN [A-Z ]*PRIVATE KEY|^seed-hex: [0-9a-f]{64}$|^BACKUP_TOKEN_SECRET=[0-9a-fA-F]{16,}$'
  local found
  found="$(grep -rEl "$patterns" "$root" 2>/dev/null || true)"
  if [ -n "$found" ]; then
    printf '%s\n' "$found" >&2
    hits="$(printf '%s\n' "$found" | wc -l)"
  fi
  printf '%s' "$hits"
}
