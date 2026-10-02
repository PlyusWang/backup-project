#!/usr/bin/env bash
#
# final_gate.sh —— PR #20 的 canonical final gate。
#
#   bash scripts/final_gate.sh
#
# 每个套件一行 "GATE <名字> exit=<码> 秒数"；失败套件 = exit != 0 的条数。
# 顺序上有一处是刻意的：quality_test.sh 会 make clean（从零构建 CLI 与 sanitizer
# 版本），所以它之后要重新构建两个 GUI，否则 scripts/test.sh 会走"没有
# build/backup-gui-modern，跳过 GUI 对比"的分支（少 10 个 GUI parity 用例）。
#
# 完整的日志在 /tmp/final_gate.log，逐行摘要写在 stdout（同时落到
# /tmp/final_gate_summary.txt 时需要调用方重定向）。

set -uo pipefail
cd "$(dirname "$BASH_SOURCE")/.."

LOG="${FINAL_GATE_LOG:-/tmp/final_gate.log}"
BUILD_LOG="${FINAL_GATE_BUILD_LOG:-/tmp/gate_build.log}"
SAN_BUILD_LOG="${FINAL_GATE_SAN_BUILD_LOG:-/tmp/gate_sanitize_build.log}"
GUI_BUILD_LOG="${FINAL_GATE_GUI_BUILD_LOG:-/tmp/gate_gui_rebuild.log}"
: > "$LOG"

echo "[gate] FINAL_HEAD=$(git rev-parse HEAD)" | tee -a "$LOG"
echo "[gate] worktree=$(git status --porcelain | wc -l) 个改动" | tee -a "$LOG"

make -j4 all gui-all test-fixtures > "$BUILD_LOG" 2>&1
echo "GATE build exit=$? warnings=$(grep -ci warning "$BUILD_LOG" || true)" | tee -a "$LOG"

make -j4 sanitize > "$SAN_BUILD_LOG" 2>&1
echo "GATE sanitize-build exit=$? warnings=$(grep -ci warning "$SAN_BUILD_LOG" || true)" | tee -a "$LOG"

run() {
  local name="$1"
  shift
  local start end code
  start=$(date +%s)
  "$@" >>"$LOG" 2>&1
  code=$?
  end=$(date +%s)
  echo "GATE $name exit=$code seconds=$((end - start))" | tee -a "$LOG"
}

run lint bash scripts/lint.sh
run quality bash scripts/quality_test.sh

# quality 会 make clean：把两个 GUI 重新构建出来，后面 test.sh 的 GUI parity 用例
# 与 modern_gui / gui_smoke 才是在"GUI 真的存在"的前提下跑的。
make -j4 gui-all > "$GUI_BUILD_LOG" 2>&1
echo "GATE gui-rebuild exit=$? warnings=$(grep -ci warning "$GUI_BUILD_LOG" || true)" | tee -a "$LOG"

run suite-main bash scripts/test.sh
run network bash scripts/network_test.sh
run network-sanitize env NETWORK_TEST_SANITIZE=1 bash scripts/network_test.sh
run account-deletion bash scripts/account_deletion_test.sh
run same-instance-truth bash scripts/same_instance_truth_test.sh
run server-admin bash scripts/server_admin_test.sh
run scheduled_backup bash scripts/scheduled_backup_test.sh
run realtime bash scripts/realtime_test.sh
run modern_gui bash scripts/modern_gui_check.sh
run gui_smoke bash scripts/gui_smoke_test.sh
run archive_pipeline bash scripts/archive_pipeline_test.sh
run backup_catalog bash scripts/backup_catalog_test.sh
run compression bash scripts/compression_test.sh
run config_manager bash scripts/config_manager_test.sh
run crypto bash scripts/crypto_test.sh
run file_io bash scripts/file_io_test.sh
run filter_metadata bash scripts/filter_metadata_test.sh
run filter_rule_builder bash scripts/filter_rule_builder_test.sh
run filter_rule_builder_int bash scripts/filter_rule_builder_int_test.sh
run filter_semantics bash scripts/filter_semantics_test.sh
run legacy_filter bash scripts/legacy_filter_test.sh
run ustar bash scripts/ustar_test.sh

failed=$(grep -c "^GATE .* exit=[^0]" "$LOG" || true)
echo "[gate] failed suites = $failed" | tee -a "$LOG"
grep -E "^GATE |failed suites|ASAN: aggregate|RSS_BOUND|ASAN: transfer" "$LOG"
