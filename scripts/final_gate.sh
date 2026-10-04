#!/usr/bin/env bash
#
# final_gate.sh —— PR #22 的 canonical final gate（含 PR #21 / #20 的全部套件）。
#
# PR #22 新增三个套件：ssh-tunnel（+ 消毒剂版）与 remote-connection-ux；
# 另外 --remote-test 里的 C01..C07 也由 modern_gui 与 remote-connection-ux
# 两条脚本各自断言了一遍（缺一条 ok 行就算失败，不允许"没跑到"被当成通过）。
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

# PR #23：手写密码学原语与证书层的独立套件。四个都是纯 C++（不链接 Qt）、
# 跑得快，而且是 BPSEC2 的地基 —— 放最前面，坏的时候报错最直白：
#   sha512      官方向量 + 流式一致性 + sha512sum oracle
#   ed25519     RFC 8032 官方向量 + cryptography/PyNaCl 双 oracle + OpenSSL 交叉验证
#   bpcert      BPCERT1 六类畸形 + 单字节全扫描 + 20000 例 fuzz + python 独立拼字节
#   cert-tool   离线根 0600 / 拒绝覆盖 / 空存储不信任 / 输出无私钥
run sha512 bash scripts/sha512_test.sh
run ed25519 bash scripts/ed25519_test.sh
run bpcert bash scripts/bpcert_test.sh
run cert-tool bash scripts/cert_tool_test.sh
# PR #23 的 BPSEC2 与 profile 层。红队复核指出：bpsec2_loopback_e2e.sh 从来
# 没被 final_gate 跑过（所以它"从干净树跑不过"这件事一直没被发现）。
# 三个套件补上：协议层（含 10000 例 fuzz 与 BPSEC1 行为基线）、零配置 profile 层、
# 以及真实进程的回环端到端。
run bpsec2 bash scripts/bpsec2_test.sh
run server-profile bash scripts/server_profile_test.sh
run bpsec2-loopback bash scripts/bpsec2_loopback_e2e.sh
# 前两个再在 ASan + UBSan 下跑一遍：手写大整数与标量归约的越界/回绕问题
# 只有消毒剂才稳定暴露（本项目已经在这个文件里真实抓到过两次）。
run ed25519-sanitize env ED25519_TEST_EXTRA_FLAGS="-g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer" ED25519_ORACLE_CASES=6 bash scripts/ed25519_test.sh
run bpcert-sanitize env BPCERT_TEST_EXTRA_FLAGS="-g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer" bash scripts/bpcert_test.sh
# PR #22：SshTunnelManager 的独立单元测试（进程生命周期 / 参数向量 / 就绪判定 /
# 超时 / 回收）。它只链接 Qt6Core + Qt6Network，不构建整个 GUI，所以放在最前面：
# 这一层出问题的话，后面所有网络套件都会以"看不懂的方式"红掉。
run ssh-tunnel bash scripts/ssh_tunnel_manager_test.sh
# 同一个套件在 ASan + UBSan 下再跑一遍：QProcess 生命周期与"在信号里析构"这类
# 问题只有在消毒剂下才稳定复现。
run ssh-tunnel-sanitize env SSH_TUNNEL_TEST_EXTRA_FLAGS="-g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer" SSH_TUNNEL_TEST_SANITIZE=1 bash scripts/ssh_tunnel_manager_test.sh
run secure-transport bash scripts/secure_transport_test.sh
run remote-incremental bash scripts/remote_incremental_test.sh
run secure-transport-sanitize env SECURE_TRANSPORT_TEST_EXTRA_FLAGS="-g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer" bash scripts/secure_transport_test.sh
run quality bash scripts/quality_test.sh

# quality 会 make clean：把两个 GUI 重新构建出来，后面 test.sh 的 GUI parity 用例
# 与 modern_gui / gui_smoke 才是在"GUI 真的存在"的前提下跑的。
make -j4 gui-all > "$GUI_BUILD_LOG" 2>&1
echo "GATE gui-rebuild exit=$? warnings=$(grep -ci warning "$GUI_BUILD_LOG" || true)" | tee -a "$LOG"

run suite-main bash scripts/test.sh
run network bash scripts/network_test.sh
run network-sanitize env NETWORK_TEST_SANITIZE=1 bash scripts/network_test.sh
# BPSNAP1 打包一致性（copy 绑定）：同一个复现程序在**当前树**与 git archive
# 2889116 出来的独立旧树上各编一次，跑 2x2 判别（对角必须通过、反对角必须失败）。
# 与 network-sanitize 一样必须排在 quality 之前：quality 会 make clean，把
# build-sanitize 一起删掉。
run bundle-source-mutation bash scripts/bundle_source_mutation_test.sh
run bundle-source-mutation-sanitize env BUNDLE_SOURCE_MUTATION_SANITIZE=1 bash scripts/bundle_source_mutation_test.sh
run account-deletion bash scripts/account_deletion_test.sh
run same-instance-truth bash scripts/same_instance_truth_test.sh
run server-admin bash scripts/server_admin_test.sh
# 原始归档的"单独恢复"（QML -> RemoteController -> RunRemoteRawRestore ->
# 既有本地恢复核心）：RAW-R01..RAW-R05 五条由 --remote-test 里的真服务端走一遍，
# 本脚本只断言那些条目真的跑了并且全部通过。
run raw-archive-restore bash scripts/raw_archive_restore_test.sh
# PR #22：连接层合同（C01..C07）。它跑的是真的 GUI 进程、真的 QML、真的按钮
# 点击 —— 人工验收发现的"点了应用没反应"只有这一条路径能抓到。
run remote-connection-ux bash scripts/remote_connection_ux_test.sh
# 同一个入口在 ASan + UBSan 下跑边界输入（截断 / 任意字节 / 离谱的声明长度 /
# 加密归档的三种情况 / 单独的 delta / 不存在的 id）：每一条都要求 fail-closed
# 且消毒剂报告为 0。和 network-sanitize 一样必须排在 quality 之前。
run raw-archive-restore-sanitize env RAW_RESTORE_SANITIZE=1 bash scripts/raw_archive_restore_test.sh
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
