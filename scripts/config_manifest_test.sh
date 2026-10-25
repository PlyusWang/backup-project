#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$ROOT/scripts/config_manifest.sh"
W="$(mktemp -d)"
trap 'rm -rf "$W"' EXIT
pass=0; fail=0
ok(){ pass=$((pass+1)); echo "  PASS  $1"; }
bad(){ fail=$((fail+1)); echo "  FAIL  $1 -- $2"; }
mkdir -p "$W/a/sub"
printf 'one\n' > "$W/a/f1"; printf 'two\n' > "$W/a/sub/f2"
if manifest_dir "$W/a" "$W/before"; then ok "正常目录采集"; else bad "正常目录采集" "rc=$?"; fi
if manifest_dir "$W/a" "$W/same" && cmp -s "$W/before" "$W/same"; then ok "未修改清单一致"; else bad "未修改清单一致" ""; fi
printf 'changed\n' > "$W/a/f1"
if manifest_dir "$W/a" "$W/changed" && ! cmp -s "$W/before" "$W/changed"; then ok "修改文件被检测"; else bad "修改文件被检测" ""; fi
rm "$W/a/f1"; if manifest_dir "$W/a" "$W/deleted" && ! cmp -s "$W/before" "$W/deleted"; then ok "删除文件被检测"; else bad "删除文件被检测" ""; fi
printf 'new\n' > "$W/a/new"; if manifest_dir "$W/a" "$W/added" && ! cmp -s "$W/deleted" "$W/added"; then ok "新增文件被检测"; else bad "新增文件被检测" ""; fi
mkdir -p "$W/bad"; printf 'x' > "$W/bad/unreadable"; chmod 000 "$W/bad/unreadable"
if manifest_dir "$W/bad" "$W/bad.manifest"; then bad "不可读文件必须失败" "returned 0"; else ok "不可读文件采集失败"; fi
rm -f "$W/bad/unreadable"
if manifest_dir "$W/missing" "$W/absent"; then bad "目录不存在返回 ABSENT" "returned 0"; else rc=$?; [ "$rc" -eq 2 ] && ok "目录不存在返回 ABSENT" || bad "目录不存在返回 ABSENT" "rc=$rc"; fi
grep -qx 'ABSENT' "$W/absent" && ok "ABSENT 清单内容" || bad "ABSENT 清单内容" ""
echo "[config-manifest] passed=$pass failed=$fail"
[ "$fail" -eq 0 ]
