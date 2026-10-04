#!/usr/bin/env bash
#
# bundle_source_mutation_test.sh —— BPSNAP1 打包一致性（复制绑定）的判别性回归。
#
# 覆盖 PR #21 最后一个 merge blocker：
#   BuildSnapshotBundle() 的"第一遍算摘要 -> 第二遍复制字节"窗口里，一次**同
#   长度**的改写会让包头声明的 SHA-256 与实际写进去的字节脱钩；这样的材料包
#   服务端照收（它只校验整个 blob 的摘要，不解析成员），坏材料要等到 restore
#   才炸。修复后的不变式是：SHA-256(实际复制进 bundle 的字节) == 包头声明。
#
# 两段：
#   A. OLD/NEW 判别矩阵（tests/review/bundle_source_mutation.cpp）
#      同一个复现程序编译两次：一次链**当前树**的核心目标文件，一次链
#      "git archive 2889116" 导出的独立旧树（旧源码一个字节都没改）的核心
#      目标文件。2x2 全跑：
#                        --expect new     --expect old
#        NEW 目标文件      必须通过          必须失败
#        OLD 目标文件      必须失败          必须通过
#      对角通过 + 反对角失败，才说明"缺陷在旧树上真实存在、在新树上被修掉"，
#      而不是只证明了"新版本能跑"。触发用显式可观察事件（.part 出现 / .part
#      字节数）+ 可证明的前置条件，不用 sleep 撞运气。
#   B. 消毒剂维度（BUNDLE_SOURCE_MUTATION_SANITIZE=1）：只跑新树，并要求
#      ASan/UBSan 报告为 0（与 network_test.sh 的 sanitize 维度同一套写法）。
#
# 退出码：0 = 全部通过。

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

PASS=0
FAIL=0
record_pass() { PASS=$((PASS + 1)); echo "  PASS $1"; }
record_fail() {
  FAIL=$((FAIL + 1))
  if [ -n "$2" ]; then
    echo "  FAIL $1 -- $2"
  else
    echo "  FAIL $1"
  fi
}

TEST_REL="tests/review/bundle_source_mutation.cpp"
OLD_HEAD="2889116945b443c23b2946a8bd0a40406793cd28"
WORK_DIR="$(mktemp -d /tmp/bundle-source-mutation-test-XXXXXX)"
trap 'rm -rf "$WORK_DIR"' EXIT

CXX=${CXX:-g++}
SAN_MODE=${BUNDLE_SOURCE_MUTATION_SANITIZE:-0}

# 目标文件与编译参数：与 network_test.sh 的 sanitize 维度同一套约定。
if [ "$SAN_MODE" = "1" ]; then
  OBJ_ROOT="build-sanitize"
  BUILD_CMD="make -j4 sanitize"
  EXTRA_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
else
  OBJ_ROOT="build"
  BUILD_CMD="make -j4 all"
  EXTRA_FLAGS=""
fi

echo "[bundle-source-mutation] 构建产品目标文件（$BUILD_CMD）"
if ! $BUILD_CMD >"$WORK_DIR/product-build.log" 2>&1; then
  record_fail "产品构建" "$(tail -3 "$WORK_DIR/product-build.log" | tr '\n' ' ')"
  echo "[bundle-source-mutation] 失败项：$FAIL"
  exit 1
fi
record_pass "产品构建"

CORE_OBJECTS="$(find "$OBJ_ROOT/src" -name '*.o' 2>/dev/null | sort | tr '\n' ' ')"
if [ -z "$CORE_OBJECTS" ]; then
  record_fail "收集核心目标文件" "$OBJ_ROOT/src 下没有 .o"
  echo "[bundle-source-mutation] 失败项：$FAIL"
  exit 1
fi

# 编译：$1 = 源码树根，$2 = 目标文件列表，$3 = 输出，$4 = 日志
compile_case() {
  local tree="$1"
  local objects="$2"
  local binary="$3"
  local log="$4"
  # shellcheck disable=SC2086
  "$CXX" $EXTRA_FLAGS -std=c++17 -Wall -Wextra -Wpedantic -I"$tree/include" \
    -pthread "$tree/$TEST_REL" $objects -o "$binary" >"$log" 2>&1
}

# 运行：$1 = 二进制，$2 = 期望模式，$3 = 标签，$4 = 日志；返回进程退出码
run_case() {
  local binary="$1"
  local expect="$2"
  local label="$3"
  local log="$4"
  timeout --signal=KILL 900 "$binary" --expect "$expect" >"$log" 2>&1
  local code=$?
  if [ "$SAN_MODE" = "1" ]; then
    scan_sanitizer_reports "$label" "$log"
  fi
  return "$code"
}

# 消毒剂报告计数（只在 sanitize 维度有意义）。与 network_test.sh 一样，
# 把计数打在同一行上，final_gate 的摘要能直接抓到。
scan_sanitizer_reports() {
  local label="$1"
  local log="$2"
  local reports
  reports="$(grep -cE 'ERROR: (AddressSanitizer|LeakSanitizer|UndefinedBehaviorSanitizer)|runtime error:' "$log" || true)"
  echo "ASAN: aggregate reports = $reports（$label）"
  if [ "$reports" = "0" ]; then
    record_pass "$label 消毒剂报告 0"
  else
    record_fail "$label 消毒剂报告" "$(grep -m1 -E 'ERROR:|runtime error:' "$log")"
  fi
}

NEW_BIN="$WORK_DIR/bundle_source_mutation_new"
OLD_BIN="$WORK_DIR/bundle_source_mutation_old"

echo "[bundle-source-mutation] 编译新树版本"
if compile_case "$ROOT_DIR" "$CORE_OBJECTS" "$NEW_BIN" "$WORK_DIR/new-link.log"; then
  if [ -s "$WORK_DIR/new-link.log" ]; then
    record_fail "新树编译零警告" "$(head -2 "$WORK_DIR/new-link.log" | tr '\n' ' ')"
  else
    record_pass "新树编译零警告"
  fi
else
  record_fail "新树编译" "$(head -3 "$WORK_DIR/new-link.log" | tr '\n' ' ')"
  echo "[bundle-source-mutation] 失败项：$FAIL"
  exit 1
fi

if [ "$SAN_MODE" = "1" ]; then
  # ---- B. 消毒剂维度：只跑新树 ----
  echo "[bundle-source-mutation] B. 消毒剂维度（新树 --expect new）"
  run_case "$NEW_BIN" new "新树--expect new（ASan/UBSan）" "$WORK_DIR/san-new.log"
  code=$?
  if [ "$code" = "0" ]; then
    record_pass "新树 --expect new 通过"
  else
    record_fail "新树 --expect new" "$(grep -m3 'FAIL' "$WORK_DIR/san-new.log" | tr '\n' ' ')"
  fi
  triggers="$(grep -c '触发：' "$WORK_DIR/san-new.log" || true)"
  if [ "$triggers" = "4" ]; then
    record_pass "四个改写窗口都被真实触发（4/4）"
  else
    record_fail "四个改写窗口都被真实触发" "只有 $triggers 个"
  fi
  grep -E '触发：|检查|观测' "$WORK_DIR/san-new.log" | sed 's/^/    /' || true
  echo
  echo "[bundle-source-mutation] PASS=$PASS FAIL=$FAIL"
  if [ "$FAIL" -ne 0 ]; then
    exit 1
  fi
  echo "[bundle-source-mutation] 全部通过"
  exit 0
fi

# ---- A. OLD/NEW 判别矩阵 ----
echo "[bundle-source-mutation] A. OLD/NEW 判别矩阵"

OLD_TREE="$WORK_DIR/old-tree"
mkdir -p "$OLD_TREE"
if git archive "$OLD_HEAD" | tar -x -C "$OLD_TREE"; then
  record_pass "导出旧树（git archive $OLD_HEAD）"
else
  record_fail "导出旧树" "git archive 失败"
  echo "[bundle-source-mutation] 失败项：$FAIL"
  exit 1
fi
# 复现程序本身从当前树复制过去；旧树的产品源码（src/ include/）一个字节都不动。
cp "$TEST_REL" "$OLD_TREE/$TEST_REL"
if (cd "$OLD_TREE" && make -j"$(nproc)" all) >"$WORK_DIR/old-product-build.log" 2>&1; then
  record_pass "旧树产品构建（旧源码，未改动）"
else
  record_fail "旧树产品构建" "$(tail -3 "$WORK_DIR/old-product-build.log" | tr '\n' ' ')"
  echo "[bundle-source-mutation] 失败项：$FAIL"
  exit 1
fi
OLD_OBJECTS="$(find "$OLD_TREE/build/src" -name '*.o' 2>/dev/null | sort | tr '\n' ' ')"
if [ -z "$OLD_OBJECTS" ]; then
  record_fail "收集旧树核心目标文件" "没有 .o"
  echo "[bundle-source-mutation] 失败项：$FAIL"
  exit 1
fi
if compile_case "$OLD_TREE" "$OLD_OBJECTS" "$OLD_BIN" "$WORK_DIR/old-link.log"; then
  if [ -s "$WORK_DIR/old-link.log" ]; then
    record_fail "旧树编译零警告" "$(head -2 "$WORK_DIR/old-link.log" | tr '\n' ' ')"
  else
    record_pass "旧树编译零警告"
  fi
else
  record_fail "旧树编译" "$(head -3 "$WORK_DIR/old-link.log" | tr '\n' ' ')"
  echo "[bundle-source-mutation] 失败项：$FAIL"
  exit 1
fi

# 对角：新树必须拒绝改写。
run_case "$NEW_BIN" new "NEW 树 --expect new" "$WORK_DIR/new-expect-new.log"
code=$?
if [ "$code" = "0" ]; then
  record_pass "对角 1/2：新树 --expect new 通过（当场拒绝、不留半成品）"
else
  record_fail "对角 1/2：新树 --expect new" "$(grep -m3 'FAIL' "$WORK_DIR/new-expect-new.log" | tr '\n' ' ')"
fi
triggers="$(grep -c '触发：' "$WORK_DIR/new-expect-new.log" || true)"
if [ "$triggers" = "4" ]; then
  record_pass "新树上四个改写窗口都被真实触发（4/4）"
else
  record_fail "新树上四个改写窗口都被真实触发" "只有 $triggers 个"
fi

# 对角：旧树必须表现出缺陷（Build 成功、自己解不开）。
run_case "$OLD_BIN" old "OLD 树 --expect old" "$WORK_DIR/old-expect-old.log"
code=$?
if [ "$code" = "0" ]; then
  record_pass "对角 2/2：旧树 --expect old 通过（缺陷在旧树上真实存在）"
else
  record_fail "对角 2/2：旧树 --expect old" "$(grep -m3 'FAIL' "$WORK_DIR/old-expect-old.log" | tr '\n' ' ')"
fi
observations="$(grep -c '观测：' "$WORK_DIR/old-expect-old.log" || true)"
if [ "$observations" = "4" ]; then
  record_pass "旧树 4 个用例都留下了观测证据（3 个自相矛盾 + 1 个静默丢尾巴）"
else
  record_fail "旧树观测证据" "只有 $observations 条"
fi

# 反对角：同一个复现程序、同一种期望，在另一棵树上必须失败。
run_case "$NEW_BIN" old "NEW 树 --expect old" "$WORK_DIR/new-expect-old.log"
code=$?
if [ "$code" != "0" ]; then
  record_pass "反对角 1/2：新树满足不了「缺陷存在」的期望（已修复）"
else
  record_fail "反对角 1/2：新树 --expect old" "竟然通过了 —— 说明新树没有拒绝改写"
fi
fixed_evidence="$(grep -c 'OLD 会接受' "$WORK_DIR/new-expect-old.log" || true)"
if [ "$fixed_evidence" = "4" ]; then
  record_pass "反对角 1/2 证据：4 个用例都因为「新树拒绝了这个改写」而失败"
else
  record_fail "反对角 1/2 证据" "只有 $fixed_evidence 条"
fi

run_case "$OLD_BIN" new "OLD 树 --expect new" "$WORK_DIR/old-expect-new.log"
code=$?
if [ "$code" != "0" ]; then
  record_pass "反对角 2/2：旧树满足不了「必须拒绝」的期望（缺陷存在）"
else
  record_fail "反对角 2/2：旧树 --expect new" "竟然通过了 —— 说明旧树也拒绝了改写"
fi
bug_evidence="$(grep -c 'NEW 打包必须失败' "$WORK_DIR/old-expect-new.log" || true)"
if [ "$bug_evidence" = "4" ]; then
  record_pass "反对角 2/2 证据：4 个用例都因为「旧树放行了改写」而失败"
else
  record_fail "反对角 2/2 证据" "只有 $bug_evidence 条"
fi

echo
echo "---- 旧树（$OLD_HEAD）观测 ----"
grep '观测：' "$WORK_DIR/old-expect-old.log" | sed 's/^/    /' || true
echo "---- 新树触发点 ----"
grep '触发：' "$WORK_DIR/new-expect-new.log" | sed 's/^/    /' || true
echo "---- 新树判定摘要 ----"
tail -1 "$WORK_DIR/new-expect-new.log" | sed 's/^/    /'
echo "---- 旧树判定摘要 ----"
tail -1 "$WORK_DIR/old-expect-old.log" | sed 's/^/    /'

echo
echo "[bundle-source-mutation] PASS=$PASS FAIL=$FAIL"
if [ "$FAIL" -ne 0 ]; then
  exit 1
fi
echo "[bundle-source-mutation] 全部通过"
exit 0
