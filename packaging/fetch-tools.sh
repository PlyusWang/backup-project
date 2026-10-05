#!/usr/bin/env bash
# packaging/fetch-tools.sh —— 下载并校验打包用的第三方工具（AppImage 工具链）。
#
#   bash packaging/fetch-tools.sh [--dir <目录>]
#
# 只读 packaging/tools.lock：版本、字节数、sha256、来源 URL 全部固定。
# 校验失败即删除文件并以非 0 退出 —— 绝不用一个来源不明的 appimagetool 去
# 生产发行包。
set -Eeuo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/lib/common.sh"

TOOLS_DIR="$PACKAGING_DIR/.tools"
while [ $# -gt 0 ]; do
  case "$1" in
    --dir) TOOLS_DIR="$2"; shift 2 ;;
    -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
    *) die_usage "未知参数：$1" ;;
  esac
done

require_tool curl sha256sum stat
mkdir -p "$TOOLS_DIR"

while IFS='|' read -r name version filename bytes sha256 license url; do
  case "$name" in ''|'#'*) continue ;; esac
  target="$TOOLS_DIR/$filename"
  if [ -f "$target" ] && [ "$(sha256_of "$target")" = "$sha256" ]; then
    log "$name $version 已就绪（sha256 校验通过）"
    continue
  fi
  log "下载 $name $version：$url"
  rm -f "$target"
  curl -fsSL --retry 3 --proto '=https' -o "$target.part" "$url" || { rm -f "$target.part"; die "下载失败：$url"; }
  got_bytes="$(size_of "$target.part")"
  got_sha="$(sha256_of "$target.part")"
  if [ "$got_bytes" != "$bytes" ] || [ "$got_sha" != "$sha256" ]; then
    rm -f "$target.part"
    die "$name 校验失败：bytes=$got_bytes（期望 $bytes）sha256=$got_sha（期望 $sha256）"
  fi
  mv "$target.part" "$target"
  chmod 0755 "$target"
  log "$name $version OK（bytes=$got_bytes sha256=$got_sha license=$license）"
done < "$PACKAGING_DIR/tools.lock"

ls -l "$TOOLS_DIR"
