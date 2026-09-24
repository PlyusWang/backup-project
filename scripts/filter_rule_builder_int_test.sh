#!/usr/bin/env bash
#
# 可视化规则编辑器：端到端集成测试。
#
# 链条：规则草稿 -> builder 序列化成 DSL -> 真实 backupctl（同一个 Filter 核心）
#       -> .bak -> restore -> diff -r + sha256sum
#
# 两组断言：
#   A. 行为正确：该进归档的进了、该排除的没进。
#   B. 与 CLI 等价：同一场景分别用「builder 生成的规则」与「手写规则」各跑一次，
#      两次恢复结果必须逐字节相同 —— GUI 不会偏离 CLI 语义的证据。
#
# 退出码：0 = 全部通过。

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

CTL="$ROOT_DIR/build/backupctl"
BIN="$ROOT_DIR/tests/output/filter_rule_builder_test"
OUT_DIR="$ROOT_DIR/tests/output/rule-builder-int"
LOG="$OUT_DIR/last-output.txt"
TIMEOUT=60
PASS_COUNT=0
FAIL_COUNT=0

record_pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo "[rb-int]   PASS: $1"; }
record_fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo "[rb-int]   FAIL: $1 -- $2"; }
judge() {
  local name="$1" expect="$2" actual="$3" ok="$4"
  if [ "$ok" = "1" ]; then record_pass "$name（$actual）"
  else record_fail "$name" "期望 $expect，实际 $actual"; fi
}
present() { [ -e "$1" ] && echo 1 || echo 0; }
absent() { [ -e "$1" ] && echo 0 || echo 1; }
tree_manifest() { ( cd "$1" 2> /dev/null && find . -type f -print0 | sort -z | xargs -0 sha256sum 2> /dev/null ); }

if [ ! -x "$BIN" ]; then bash "$ROOT_DIR/scripts/filter_rule_builder_test.sh" > /dev/null 2>&1; fi
if [ ! -x "$CTL" ]; then make -C "$ROOT_DIR" > /dev/null 2>&1; fi
if [ ! -x "$BIN" ] || [ ! -x "$CTL" ]; then
  echo "[rb-int] 缺少测试二进制或 backupctl，无法继续" >&2
  exit 1
fi

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR/src/keep" "$OUT_DIR/src/build" "$OUT_DIR/src/cache"
printf 'aaa\n' > "$OUT_DIR/src/a.txt"
printf 'bbb\n' > "$OUT_DIR/src/b.md"
printf 'ccc\n' > "$OUT_DIR/src/c.bin"
printf 'secret\n' > "$OUT_DIR/src/secret.txt"
printf 'note\n' > "$OUT_DIR/src/keep/note.md"
printf 'object\n' > "$OUT_DIR/src/build/object.o"
printf 'cache\n' > "$OUT_DIR/src/cache/c.tmp"

run_with_builder() {
  local scenario="$1" tag="$2"
  local arc="$OUT_DIR/$tag.bak" dst="$OUT_DIR/$tag-dest"
  rm -f "$arc"; rm -rf "$dst"
  set --
  while IFS= read -r line; do set -- "$@" "$line"; done < <("$BIN" --emit-args "$scenario")
  if ! timeout "$TIMEOUT" "$CTL" backup "$OUT_DIR/src" "$arc" "$@" > "$LOG" 2>&1; then echo "backup-failed"; return; fi
  if ! timeout "$TIMEOUT" "$CTL" restore "$arc" "$dst" > "$LOG" 2>&1; then echo "restore-failed"; return; fi
  echo "$dst"
}

run_with_literal() {
  local tag="$1"; shift
  local arc="$OUT_DIR/$tag.bak" dst="$OUT_DIR/$tag-dest"
  rm -f "$arc"; rm -rf "$dst"
  if ! timeout "$TIMEOUT" "$CTL" backup "$OUT_DIR/src" "$arc" "$@" > "$LOG" 2>&1; then echo "backup-failed"; return; fi
  if ! timeout "$TIMEOUT" "$CTL" restore "$arc" "$dst" > "$LOG" 2>&1; then echo "restore-failed"; return; fi
  echo "$dst"
}

echo "[rb-int] 场景 A：只保留 txt / md（include ext:txt;md）"
A=$(run_with_builder ext_txt_md A)
judge "A 恢复成功" "目录" "$A" "$( [ -d "$A" ] && echo 1 || echo 0 )"
judge "A a.txt 进入归档" "存在" "$(present "$A/a.txt")" "$(present "$A/a.txt")"
judge "A b.md 进入归档" "存在" "$(present "$A/b.md")" "$(present "$A/b.md")"
judge "A c.bin 被排除" "不存在" "$(present "$A/c.bin")" "$(absent "$A/c.bin")"
judge "A secret.txt 属于 .txt，应随 include 规则一起进入归档" "存在" "$(present "$A/secret.txt")" "$(present "$A/secret.txt")"

echo "[rb-int] 场景 B：排除 build/（exclude path:**/build/**）"
B=$(run_with_builder exclude_build B)
judge "B 恢复成功" "目录" "$B" "$( [ -d "$B" ] && echo 1 || echo 0 )"
judge "B build/object.o 被排除" "不存在" "$(present "$B/build/object.o")" "$(absent "$B/build/object.o")"
judge "B 其他文件保留" "a.txt 与 keep/note.md 都在" "$(present "$B/a.txt")/$(present "$B/keep/note.md")" \
  "$( [ "$(present "$B/a.txt")" = "1" ] && [ "$(present "$B/keep/note.md")" = "1" ] && echo 1 || echo 0 )"

echo "[rb-int] 场景 C：include txt 但 exclude secret.txt（exclude 优先）"
C=$(run_with_builder include_txt_exclude_secret C)
judge "C 恢复成功" "目录" "$C" "$( [ -d "$C" ] && echo 1 || echo 0 )"
judge "C a.txt 保留" "存在" "$(present "$C/a.txt")" "$(present "$C/a.txt")"
judge "C secret.txt 被 exclude 优先排除" "不存在" "$(present "$C/secret.txt")" "$(absent "$C/secret.txt")"

echo "[rb-int] 场景 D：多子条件 AND（exclude type:folder path:**/cache）"
D=$(run_with_builder multi_clause_folder_cache D)
judge "D 恢复成功" "目录" "$D" "$( [ -d "$D" ] && echo 1 || echo 0 )"
judge "D cache/c.tmp 被整棵剪掉" "不存在" "$(present "$D/cache/c.tmp")" "$(absent "$D/cache/c.tmp")"
judge "D 其他文件保留" "a.txt 存在" "$(present "$D/a.txt")" "$(present "$D/a.txt")"

echo "[rb-int] 等价性：builder 生成的规则 vs 手写规则"
A2=$(run_with_literal A2 --include "ext:txt;md")
if [ -d "$A" ] && [ -d "$A2" ] && diff -r "$A" "$A2" > /dev/null 2>&1 && [ "$(tree_manifest "$A")" = "$(tree_manifest "$A2")" ]; then
  record_pass "A 等价：builder 结果与手写结果逐字节相同"
else
  record_fail "A 等价" "两次恢复结果不同"
fi
C2=$(run_with_literal C2 --include "ext:txt" --exclude "name:secret.txt")
if [ -d "$C" ] && [ -d "$C2" ] && diff -r "$C" "$C2" > /dev/null 2>&1 && [ "$(tree_manifest "$C")" = "$(tree_manifest "$C2")" ]; then
  record_pass "C 等价：include+exclude 的 builder 结果与手写结果逐字节相同"
else
  record_fail "C 等价" "两次恢复结果不同"
fi
D2=$(run_with_literal D2 --exclude "type:folder path:**/cache")
if [ -d "$D" ] && [ -d "$D2" ] && diff -r "$D" "$D2" > /dev/null 2>&1 && [ "$(tree_manifest "$D")" = "$(tree_manifest "$D2")" ]; then
  record_pass "D 等价：多子条件 AND 的 builder 结果与手写结果逐字节相同"
else
  record_fail "D 等价" "两次恢复结果不同"
fi

echo "[rb-int] 非法规则：前端拦下 + 后端兜底"
BAD_ARC="$OUT_DIR/bad.bak"
rm -f "$BAD_ARC"
if "$BIN" --emit-args invalid_empty_ext > /dev/null 2>&1; then
  record_fail "非法草稿被前端拦下" "invalid_empty_ext 竟然通过了 builder 校验"
else
  record_pass "非法草稿被前端拦下（builder 校验失败，不会提交到后端）"
fi
timeout "$TIMEOUT" "$CTL" backup "$OUT_DIR/src" "$BAD_ARC" --include "bogus:x" > "$LOG" 2>&1
BAD_CODE=$?
if [ $BAD_CODE -eq 2 ] && [ ! -e "$BAD_ARC" ]; then
  record_pass "后端兜底：绕过前端提交非法规则 exit 2 且不留 .bak"
else
  record_fail "后端兜底" "exit=$BAD_CODE"
fi

echo "[rb-int] 结果：PASS=$PASS_COUNT FAIL=$FAIL_COUNT"
if [ $FAIL_COUNT -ne 0 ]; then echo "[rb-int] 结论：存在失败" >&2; exit 1; fi
echo "[rb-int] 结论：集成测试全部通过"
exit 0
