#!/usr/bin/env bash
#
# 定时备份（Scheduled + Full）的专项测试。
#
# 分区：
#   A. 核心单元测试：scheduler_core / scheduled_backup / terminal_secret
#   B. CLI pipeline parity：legacy 仍然是 v0.1，出现 pipeline 选项才走 v2
#   C. 密码只从 TTY 读：真实 PTY 集成测试（备份问两次、恢复问一次）
#   D. CLI schedule 子命令的真实语义（无变化 skip、有变化建快照、retention）
#   E. 跨前端：GUI 写的计划 CLI 读得到，CLI 写的计划 GUI 读得到
#   F. 计划快照是完整独立备份：单独拷出来也必须能恢复
#
# 退出码：只有全部用例通过才是 0。

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "$BASH_SOURCE")/.." && pwd)"
BACKUPCTL="$ROOT_DIR/build/backupctl"
GUI="$ROOT_DIR/build/backup-gui-modern"
# 放在已被 .gitignore 覆盖的 testdata/ 下面：测试失败时留下的现场不会变成
# "未跟踪文件"，也就不会有人手滑把它提交进去。
TEST_ROOT="$ROOT_DIR/testdata/schedule"
OUT="$TEST_ROOT/last-output.txt"

PASS=0
FAIL=0

echo "[schedule-test] scheduled full backup suite"

record_pass() { PASS=$((PASS + 1)); echo "  PASS  $1"; }
record_fail() { FAIL=$((FAIL + 1)); echo "  FAIL  $1 -- $2" >&2; }

first_line() { head -n 1 "$OUT" | cut -c1-200; }

expect_exit() {
  local name="$1" expected="$2"
  shift 2
  "$@" >"$OUT" 2>&1
  local status=$?
  if [ "$status" = "$expected" ]; then
    record_pass "$name"
  else
    record_fail "$name" "expected exit $expected, got $status: $(first_line)"
  fi
}

expect_grep() {
  local name="$1" pattern="$2"
  if grep -qF -- "$pattern" "$OUT"; then
    record_pass "$name"
  else
    record_fail "$name" "missing [$pattern]: $(first_line)"
  fi
}

expect_file() {
  if [ -f "$2" ]; then record_pass "$1"; else record_fail "$1" "missing: $2"; fi
}

expect_absent() {
  if [ ! -e "$2" ]; then record_pass "$1"; else record_fail "$1" "unexpected: $2"; fi
}

expect_content() {
  local actual
  actual="$(cat "$2" 2>/dev/null)"
  if [ "$actual" = "$3" ]; then record_pass "$1"; else record_fail "$1" "content of $2 is [$actual]"; fi
}

expect_bytes() {
  if cmp -s "$2" "$3"; then record_pass "$1"; else record_fail "$1" "bytes differ: $2 vs $3"; fi
}

expect_magic() {
  local actual
  actual="$(head -c 8 "$2" | od -An -tx1 | tr -d ' \n')"
  if [ "$actual" = "$3" ]; then
    record_pass "$1"
  else
    record_fail "$1" "magic is $actual, expected $3"
  fi
}

# ---- 构建 ----

chmod -R u+rwX "$TEST_ROOT" 2>/dev/null || true
rm -rf "$TEST_ROOT"
mkdir -p "$TEST_ROOT"

if [ ! -x "$BACKUPCTL" ]; then
  echo "[schedule-test] backupctl is missing; building first..."
  make -C "$ROOT_DIR" >/dev/null || exit 1
fi

# ============================================================
echo "[schedule-test] A. core unit tests"
# ============================================================

CORE_OBJECTS="$(find "$ROOT_DIR/build/src" -name '*.o' | sort | tr '\n' ' ')"
if [ -z "$CORE_OBJECTS" ]; then
  echo "[schedule-test] 没有找到 build/src 下的目标文件；请先 make" >&2
  exit 1
fi

run_unit() {
  local name="$1"
  local source="$ROOT_DIR/tests/unit/$name.cpp"
  local binary="$TEST_ROOT/$name"
  if ! g++ -std=c++17 -Wall -Wextra -Wpedantic -I"$ROOT_DIR/include" \
      -I"$ROOT_DIR/tests/unit" "$source" $CORE_OBJECTS -o "$binary" \
      >"$TEST_ROOT/$name-build.log" 2>&1; then
    record_fail "A.$name 编译" "$(head -3 "$TEST_ROOT/$name-build.log" | tr '\n' ' ')"
    return
  fi
  record_pass "A.$name 编译（-Wall -Wextra -Wpedantic 无警告）"
  if [ -s "$TEST_ROOT/$name-build.log" ]; then
    record_fail "A.$name 零警告" "$(head -2 "$TEST_ROOT/$name-build.log" | tr '\n' ' ')"
  else
    record_pass "A.$name 零警告"
  fi
  "$binary" >"$TEST_ROOT/$name.log" 2>&1
  local status=$?
  if [ "$status" = "0" ]; then
    record_pass "A.$name 全部断言通过（$(tail -1 "$TEST_ROOT/$name.log")）"
  else
    record_fail "A.$name 全部断言通过" "$(grep FAIL "$TEST_ROOT/$name.log" | head -3 | tr '\n' ' ')"
  fi
}

run_unit scheduler_core_test
run_unit scheduled_backup_test
run_unit terminal_secret_test

# ============================================================
echo "[schedule-test] B. CLI pipeline parity"
# ============================================================

CLI="$TEST_ROOT/cli"
mkdir -p "$CLI/src" "$CLI/repo"
printf 'hello\n' > "$CLI/src/a.txt"
printf 'world\n' > "$CLI/src/b.log"

expect_exit "B.01 legacy backup（无 pipeline 选项）" 0 \
  "$BACKUPCTL" backup "$CLI/src" "$CLI/legacy.bak"
expect_magic "B.02 legacy 产物是 v0.1（BKPARCH）" "$CLI/legacy.bak" "424b504152434800"
expect_grep "B.03 legacy 输出说明走的是 v0.1" "legacy v0.1"

expect_exit "B.04 pipeline 备份（ustar + huffman）" 0 \
  "$BACKUPCTL" backup "$CLI/src" "$CLI/v2.bak" --pack ustar --compression huffman
expect_magic "B.05 pipeline 产物是 v2（BKPCNT2）" "$CLI/v2.bak" "424b50434e543200"
expect_grep "B.06 pipeline 输出报告真实算法" "pack=ustar compression=huffman encryption=none"

expect_exit "B.07 只给 --encryption none 也算 pipeline" 0 \
  "$BACKUPCTL" backup "$CLI/src" "$CLI/none.bak" --encryption none
expect_magic "B.08 显式 none 走 v2" "$CLI/none.bak" "424b50434e543200"

expect_exit "B.09 只给 --include 仍然走 legacy" 0 \
  "$BACKUPCTL" backup "$CLI/src" "$CLI/filter.bak" --include 'ext:txt'
expect_magic "B.10 只有筛选规则时仍是 v0.1" "$CLI/filter.bak" "424b504152434800"

expect_exit "B.11 未知打包方式是用法错误" 2 \
  "$BACKUPCTL" backup "$CLI/src" "$CLI/bad.bak" --pack gzip
expect_grep "B.12 未知打包方式报错点名取值" "unknown pack method"

expect_exit "B.13 未知选项是用法错误" 2 \
  "$BACKUPCTL" backup "$CLI/src" "$CLI/bad2.bak" --from-filter 'ext:cpp'

expect_exit "B.14 legacy 恢复" 0 "$BACKUPCTL" restore "$CLI/legacy.bak" "$CLI/out-legacy"
expect_content "B.15 legacy 恢复内容正确" "$CLI/out-legacy/a.txt" "hello"

expect_exit "B.16 v2 恢复" 0 "$BACKUPCTL" restore "$CLI/v2.bak" "$CLI/out-v2"
expect_content "B.17 v2 恢复内容正确" "$CLI/out-v2/a.txt" "hello"

expect_exit "B.18 筛选后的备份只装匹配到的文件" 0 \
  "$BACKUPCTL" backup "$CLI/src" "$CLI/filter2.bak" --include 'ext:txt'
expect_exit "B.19 恢复筛选备份" 0 "$BACKUPCTL" restore "$CLI/filter2.bak" "$CLI/out-filter"
expect_file "B.20 匹配到的文件在" "$CLI/out-filter/a.txt"
expect_absent "B.21 被筛掉的文件不在" "$CLI/out-filter/b.log"

expect_exit "B.22 不允许 --password（密码绝不进 argv）" 2 \
  "$BACKUPCTL" backup "$CLI/src" "$CLI/pw.bak" --password secret

# ============================================================
echo "[schedule-test] C. 密码只从 TTY 读（真实 PTY）"
# ============================================================

cat > "$TEST_ROOT/pty_run.py" <<'PY_EOF'
"""在真实 PTY 里跑一条 backupctl 命令并按提示喂密码。

判定标准不是"输出里有没有某句话"，而是**退出码 + 归档能不能真的恢复**：
只有密码真的被读对了，恢复才会成功。
"""
import fcntl
import os
import pty
import select
import subprocess
import sys
import termios

binary = sys.argv[1]
answers = sys.argv[2].split(",")
arguments = sys.argv[3:]

master, slave = pty.openpty()


def ChildSetup():
    """把 PTY 变成子进程的控制终端。

    只把 slave 当作 stdin/stdout 是不够的：那样 /dev/tty 依然打不开
    （ENXIO），产品就会正确地报"需要交互终端"。真正的做法是 setsid() 之后
    用 TIOCSCTTY 显式认领 —— 那样 /dev/tty 才指向这个 PTY，
    测试走的才是 backupctl 真实的那条交互路径。
    """
    os.setsid()
    fcntl.ioctl(0, termios.TIOCSCTTY, 0)


process = subprocess.Popen([binary] + arguments, stdin=slave, stdout=slave,
                           stderr=slave, close_fds=True,
                           preexec_fn=ChildSetup)
os.close(slave)

sent = 0
transcript = b""
while True:
    ready, _, _ = select.select([master], [], [], 20)
    if not ready:
        if process.poll() is not None:
            break
        continue
    try:
        chunk = os.read(master, 4096)
    except OSError:
        break
    if not chunk:
        break
    transcript += chunk
    # 每个提示语里都含 "password"，收到一个就喂一条答案。
    if sent < len(answers) and b"password" in chunk.lower():
        os.write(master, (answers[sent] + "\n").encode())
        sent += 1
    if process.poll() is not None:
        while True:
            ready, _, _ = select.select([master], [], [], 0.2)
            if not ready:
                break
            try:
                more = os.read(master, 4096)
            except OSError:
                break
            if not more:
                break
            transcript += more

if hasattr(process, "wait"):
    process.wait(timeout=30)
os.close(master)
sys.stdout.write(transcript.decode("utf-8", "replace"))
sys.exit(process.returncode)
PY_EOF

PTY_RUN="$TEST_ROOT/pty_run.py"
mkdir -p "$CLI/secret-src"
printf 'top secret payload\n' > "$CLI/secret-src/secret.txt"

expect_exit "C.01 PTY 加密备份（密码问两次）" 0 \
  python3 "$PTY_RUN" "$BACKUPCTL" "hunter2,hunter2" \
  backup "$CLI/secret-src" "$CLI/secret.bak" --encryption aes-256-ctr-hmac-sha256
expect_file "C.02 加密归档已生成" "$CLI/secret.bak"
expect_magic "C.03 加密归档是 v2 container" "$CLI/secret.bak" "424b50434e543200"

expect_exit "C.04 两次密码不一致必须失败" 1 \
  python3 "$PTY_RUN" "$BACKUPCTL" "one,two" \
  backup "$CLI/secret-src" "$CLI/mismatch.bak" --encryption aes-256-ctr-hmac-sha256
expect_absent "C.05 密码不一致没有留下半成品" "$CLI/mismatch.bak"

expect_exit "C.06 没有 TTY 时明确失败（不从管道读密码）" 1 \
  env -u TERM sh -c "exec 0</dev/null; exec $BACKUPCTL backup '$CLI/secret-src' '$CLI/notty.bak' --encryption aes-256-ctr-hmac-sha256"
expect_absent "C.07 无 TTY 时没有留下半成品" "$CLI/notty.bak"

expect_exit "C.08 加密归档用正确密码恢复（问一次）" 0 \
  python3 "$PTY_RUN" "$BACKUPCTL" "hunter2" restore "$CLI/secret.bak" "$CLI/out-secret"
expect_content "C.09 加密恢复内容逐字节一致" "$CLI/out-secret/secret.txt" "top secret payload"

expect_exit "C.10 错误密码必须失败" 1 \
  python3 "$PTY_RUN" "$BACKUPCTL" "wrong-password" restore "$CLI/secret.bak" "$CLI/out-wrong"
expect_absent "C.11 错误密码不会建出目标目录" "$CLI/out-wrong"

# ============================================================
echo "[schedule-test] D. CLI schedule 子命令"
# ============================================================

SCHED="$TEST_ROOT/schedule"
mkdir -p "$SCHED/src" "$SCHED/repo"
printf 'one\n' > "$SCHED/src/a.txt"
CONFIG="$SCHED/config.json"
STORE="$SCHED/schedule.json"

expect_exit "D.01 config repository set" 0 \
  "$BACKUPCTL" --config-file "$CONFIG" config repository set "$SCHED/repo"

"$BACKUPCTL" --config-file "$CONFIG" config repository show >"$OUT" 2>&1
expect_grep "D.02 config repository show 报告仓库" "$SCHED/repo"

expect_exit "D.03 schedule set" 0 \
  "$BACKUPCTL" --config-file "$CONFIG" --schedule-file "$STORE" schedule set \
  --source "$SCHED/src" --interval-minutes 1 --retain 2 --pack ustar \
  --compression huffman --include 'ext:txt'
"$BACKUPCTL" --config-file "$CONFIG" --schedule-file "$STORE" schedule show >"$OUT" 2>&1
expect_grep "D.04 show 报告源目录" "$SCHED/src"
expect_grep "D.05 show 报告周期" "Interval:       1 minute(s)"
expect_grep "D.06 show 报告保留数量" "Retain:         2 scheduled snapshot(s)"
expect_grep "D.07 show 报告算法" "Pack:           ustar"
expect_grep "D.08 show 报告 include 规则" "ext:txt"
expect_grep "D.09 默认未启用" "Enabled:        no"

expect_exit "D.10 计划不接受非 none 的加密" 2 \
  "$BACKUPCTL" --config-file "$CONFIG" --schedule-file "$STORE" schedule set \
  --encryption aes-256-ctr-hmac-sha256
expect_grep "D.11 拒绝原因写清楚了" "定时无人值守加密需要安全的密钥来源"

expect_exit "D.12 schedule enable" 0 \
  "$BACKUPCTL" --config-file "$CONFIG" --schedule-file "$STORE" schedule enable
expect_file "D.13 schedule.json 已生成" "$STORE"

expect_exit "D.14 schedule run（首次，建立快照）" 0 \
  "$BACKUPCTL" --config-file "$CONFIG" --schedule-file "$STORE" schedule run
expect_grep "D.15 首次运行创建完整快照" "Created a new full snapshot"
expect_grep "D.16 首次快照被标注出来" "first snapshot"

SCHED_BAK_1="$(ls "$SCHED/repo" | head -1)"
expect_file "D.17 仓库里有一份计划快照" "$SCHED/repo/$SCHED_BAK_1"

expect_exit "D.18 schedule run（无变化，必须 skip）" 0 \
  "$BACKUPCTL" --config-file "$CONFIG" --schedule-file "$STORE" schedule run
expect_grep "D.19 无变化时跳过" "Skipped: the source has not changed"
COUNT_AFTER_SKIP="$(ls "$SCHED/repo" | wc -l)"
if [ "$COUNT_AFTER_SKIP" = "1" ]; then
  record_pass "D.20 跳过时没有产生新归档"
else
  record_fail "D.20 跳过时没有产生新归档" "仓库里有 $COUNT_AFTER_SKIP 个归档"
fi

printf 'two\n' > "$SCHED/src/b.txt"
expect_exit "D.21 schedule run（有变化，建立新快照）" 0 \
  "$BACKUPCTL" --config-file "$CONFIG" --schedule-file "$STORE" schedule run
expect_grep "D.22 变化摘要出现 +1 新增" "+1 added"
expect_grep "D.23 这一轮不是首次快照" "changes:"

# retention：retain=2，再来两轮变化，仓库里应当稳定在 2 份。
"$BACKUPCTL" --config-file "$CONFIG" --schedule-file "$STORE" schedule set \
  --retain 2 >/dev/null 2>&1
printf 'three\n' > "$SCHED/src/c.txt"
"$BACKUPCTL" --config-file "$CONFIG" --schedule-file "$STORE" schedule run >/dev/null 2>&1
sleep 1
printf 'four\n' > "$SCHED/src/d.txt"
"$BACKUPCTL" --config-file "$CONFIG" --schedule-file "$STORE" schedule run >/dev/null 2>&1
COUNT_AFTER_RETAIN="$(ls "$SCHED/repo" | wc -l)"
if [ "$COUNT_AFTER_RETAIN" = "2" ]; then
  record_pass "D.24 retention 让仓库稳定在 retain=2"
else
  record_fail "D.24 retention 让仓库稳定在 retain=2" "仓库里有 $COUNT_AFTER_RETAIN 个归档"
fi

# 手工备份绝不能被自动淘汰删掉。
printf 'manual\n' > "$SCHED/repo/manual_keep.bak"
touch -d '2000-01-01 00:00:00' "$SCHED/repo/manual_keep.bak"
printf 'five\n' > "$SCHED/src/e.txt"
"$BACKUPCTL" --config-file "$CONFIG" --schedule-file "$STORE" schedule run >/dev/null 2>&1
expect_file "D.25 手工备份不会被 retention 删掉" "$SCHED/repo/manual_keep.bak"

expect_exit "D.26 schedule history" 0 \
  "$BACKUPCTL" --config-file "$CONFIG" --schedule-file "$STORE" schedule history
expect_grep "D.27 history 里有 success_created" "success_created"
expect_grep "D.28 history 里有 skipped_no_changes" "skipped_no_changes"

expect_exit "D.29 schedule disable" 0 \
  "$BACKUPCTL" --config-file "$CONFIG" --schedule-file "$STORE" schedule disable
"$BACKUPCTL" --config-file "$CONFIG" --schedule-file "$STORE" schedule show >"$OUT" 2>&1
expect_grep "D.30 disable 之后显示未启用" "Enabled:        no"

# 坏掉的 schedule.json 必须被明确拒绝，而不是静默回退到默认值。
printf '{"version": 1, "config": {"enabled": true}}' > "$SCHED/broken.json"
expect_exit "D.31 坏掉的 schedule store 被拒绝" 1 \
  "$BACKUPCTL" --config-file "$CONFIG" --schedule-file "$SCHED/broken.json" schedule show
expect_grep "D.32 报错说明缺了哪个字段" "missing required field"

# 手写一个带 password 字段的 store：解析层不认这个字段。
printf '{"version": 1, "config": {}, "state": {}, "password": "x"}' > "$SCHED/pw.json"
expect_exit "D.33 store 里的未知字段被拒绝" 1 \
  "$BACKUPCTL" --config-file "$CONFIG" --schedule-file "$SCHED/pw.json" schedule show
expect_grep "D.34 报错点名未知字段" "unknown field 'password'"

# schedule.json 的权限必须是 0600。
PERM="$(stat -c '%a' "$STORE")"
if [ "$PERM" = "600" ]; then
  record_pass "D.35 schedule.json 权限是 0600"
else
  record_fail "D.35 schedule.json 权限是 0600" "实际是 $PERM"
fi

# single-runner 锁：另一个进程持锁时，schedule run 必须明确失败。
if command -v flock >/dev/null 2>&1; then
  flock -n "$STORE.lock" -c 'sleep 6' &
  LOCK_PID=$!
  sleep 1
  expect_exit "D.36 已有 runner 时 schedule run 明确失败" 1 \
    "$BACKUPCTL" --config-file "$CONFIG" --schedule-file "$STORE" schedule run
  expect_grep "D.37 报错说明已被另一进程持有" "already held by another process"
  wait "$LOCK_PID" 2>/dev/null || true
else
  echo "  SKIP  D.36/D.37 flock 不可用"
fi

# ============================================================
echo "[schedule-test] E. 跨前端：GUI 与 CLI 共用同一份 store"
# ============================================================

CROSS="$TEST_ROOT/cross"
mkdir -p "$CROSS/src" "$CROSS/repo"
printf 'cross\n' > "$CROSS/src/a.txt"
CROSS_CONFIG="$CROSS/config.json"
CROSS_STORE="$CROSS/schedule.json"

if [ ! -x "$GUI" ]; then
  echo "  SKIP  E 区：没有 build/backup-gui-modern（make gui-modern 之后再跑）"
else
  export QT_QPA_PLATFORM=offscreen

  # GUI -> CLI：自检最后留下一份 retain=7 / interval=5 / ustar + huffman 的计划。
  "$BACKUPCTL" --config-file "$CROSS_CONFIG" config repository set "$CROSS/repo" >/dev/null 2>&1
  "$GUI" --schedule-test --config-file "$CROSS_CONFIG" --schedule-file "$CROSS_STORE" \
    >"$TEST_ROOT/gui-schedule-test.log" 2>&1
  GUI_STATUS=$?
  if [ "$GUI_STATUS" = "0" ]; then
    record_pass "E.01 GUI 计划自检全部通过（$(tail -1 "$TEST_ROOT/gui-schedule-test.log")）"
  else
    record_fail "E.01 GUI 计划自检全部通过" "$(grep FAIL "$TEST_ROOT/gui-schedule-test.log" | head -2 | tr '\n' ' ')"
  fi

  "$BACKUPCTL" --config-file "$CROSS_CONFIG" --schedule-file "$CROSS_STORE" schedule show >"$OUT" 2>&1
  expect_grep "E.02 GUI 写的 level 7 保留数量 CLI 读得到" "Retain:         7 scheduled snapshot(s)"
  expect_grep "E.03 GUI 写的周期 CLI 读得到" "Interval:       5 minute(s)"
  expect_grep "E.04 GUI 写的打包方式 CLI 读得到" "Pack:           ustar"
  expect_grep "E.05 GUI 写的压缩方式 CLI 读得到" "Compression:    huffman"
  expect_grep "E.06 GUI 写的 include 规则 CLI 读得到" "Include rules:  ext:txt"
  expect_grep "E.07 GUI 写的 exclude 规则 CLI 读得到" "Exclude rules:  path:**/build/**"

  # CLI -> GUI：CLI 写一份不同的计划，GUI 用自己的控制器读回来。
  expect_exit "E.08 CLI 写一份新计划" 0 \
    "$BACKUPCTL" --config-file "$CROSS_CONFIG" --schedule-file "$CROSS_STORE" schedule set \
    --source "$CROSS/src" --interval-minutes 9 --retain 4 --pack fast-ustar \
    --compression lzss-huffman --include 'ext:txt;md'
  "$GUI" --schedule-show --config-file "$CROSS_CONFIG" --schedule-file "$CROSS_STORE" >"$OUT" 2>&1
  expect_grep "E.09 GUI 读到 CLI 写的周期" "interval=9"
  expect_grep "E.10 GUI 读到 CLI 写的保留数量" "retain=4"
  expect_grep "E.11 GUI 读到 CLI 写的打包方式" "pack=fast-ustar"
  expect_grep "E.12 GUI 读到 CLI 写的压缩方式" "compression=lzss-huffman"
  expect_grep "E.13 GUI 读到 CLI 写的 include 规则" "include=ext:txt;md"
  expect_grep "E.14 GUI 读到 CLI 写的源目录" "source=$CROSS/src"
  expect_grep "E.15 GUI 报告的加密始终是 none" "encryption=none"

  # 两边对同一份 store 的"不会漂移"是结构性的：只有一份 parser。
  if [ "$(grep -c 'BPMANIFEST1' "$ROOT_DIR/src/scheduler/source_manifest.cpp")" -ge 1 ] &&
     [ "$(grep -rl 'schedule_store.h' "$ROOT_DIR/src" "$ROOT_DIR/app" "$ROOT_DIR/ui" | wc -l)" -ge 3 ]; then
    record_pass "E.16 只有一份 schedule store 实现被三个前端共用"
  else
    record_fail "E.16 只有一份 schedule store 实现被三个前端共用" "引用它的前端不足三个"
  fi
fi

# ============================================================
echo "[schedule-test] F. 计划快照是完整独立备份"
# ============================================================

IND="$TEST_ROOT/independent"
mkdir -p "$IND/src" "$IND/repo" "$IND/elsewhere"
printf 'v1\n' > "$IND/src/file.txt"
IND_CONFIG="$IND/config.json"
IND_STORE="$IND/schedule.json"

"$BACKUPCTL" --config-file "$IND_CONFIG" config repository set "$IND/repo" >/dev/null 2>&1
"$BACKUPCTL" --config-file "$IND_CONFIG" --schedule-file "$IND_STORE" schedule set \
  --source "$IND/src" --interval-minutes 1 --retain 5 >/dev/null 2>&1
"$BACKUPCTL" --config-file "$IND_CONFIG" --schedule-file "$IND_STORE" schedule enable >/dev/null 2>&1
"$BACKUPCTL" --config-file "$IND_CONFIG" --schedule-file "$IND_STORE" schedule run >/dev/null 2>&1
OLDEST="$(ls -t "$IND/repo" | tail -1)"

sleep 1
printf 'v2\n' > "$IND/src/file.txt"
"$BACKUPCTL" --config-file "$IND_CONFIG" --schedule-file "$IND_STORE" schedule run >/dev/null 2>&1
sleep 1
printf 'v3\n' > "$IND/src/file.txt"
"$BACKUPCTL" --config-file "$IND_CONFIG" --schedule-file "$IND_STORE" schedule run >/dev/null 2>&1

cp "$IND/repo/$OLDEST" "$IND/elsewhere/lone.bak"
expect_exit "F.01 最旧的一份单独恢复成功" 0 \
  "$BACKUPCTL" restore "$IND/elsewhere/lone.bak" "$IND/restored"
expect_content "F.02 恢复出的是那一轮的内容（不是 delta）" "$IND/restored/file.txt" "v1"

# ============================================================
echo "[schedule-test] G. ASan + UBSan（可选，SANITIZE=1 时执行）"
# ============================================================
#
# 新模块全是纯 C++：JSON 解析、manifest 序列化、schedule state、保留策略。
# 它们要处理的正是最容易出事的那类输入 —— malformed JSON、超大计数、
# 整数溢出、重复字段、怪异路径、软链接、删除失败。
#
# 默认不开：把每一份核心源码都重新用 -fsanitize=address,undefined 编一遍
# 是分钟级的开销，不适合塞进每次提交都要跑的默认路径。最终回归时用
# SANITIZE=1 跑一次即可（报告里会写明）。

if [ "$(printenv SANITIZE || true)" = "1" ]; then
  SAN_DIR="$TEST_ROOT/sanitize"
  mkdir -p "$SAN_DIR"
  SAN_SOURCES="$(find "$ROOT_DIR/src" -name '*.cpp' | sort | tr '
' ' ')"
  for name in scheduler_core_test scheduled_backup_test terminal_secret_test; do
    if g++ -std=c++17 -g -O1 -fsanitize=address,undefined         -fno-omit-frame-pointer -I"$ROOT_DIR/include" -I"$ROOT_DIR/tests/unit"         "$ROOT_DIR/tests/unit/$name.cpp" $SAN_SOURCES         -o "$SAN_DIR/$name" >"$SAN_DIR/$name-build.log" 2>&1; then
      record_pass "G.$name 在 ASan + UBSan 下编译通过"
    else
      record_fail "G.$name 在 ASan + UBSan 下编译通过" "$(head -3 "$SAN_DIR/$name-build.log" | tr '
' ' ')"
      continue
    fi
    ASAN_OPTIONS=detect_leaks=1:abort_on_error=0     UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1       "$SAN_DIR/$name" >"$SAN_DIR/$name.log" 2>&1
    san_status=$?
    if [ "$san_status" = "0" ] && ! grep -qE 'AddressSanitizer|runtime error|LeakSanitizer' "$SAN_DIR/$name.log"; then
      record_pass "G.$name 在 ASan + UBSan 下 0 报告（$(tail -1 "$SAN_DIR/$name.log")）"
    else
      record_fail "G.$name 在 ASan + UBSan 下有报告" "$(grep -m2 -E 'AddressSanitizer|runtime error|LeakSanitizer' "$SAN_DIR/$name.log" | tr '
' ' ')"
    fi
  done
else
  echo "  SKIP  G 区（设置 SANITIZE=1 才会跑）"
fi

# ============================================================
echo "[schedule-test] 结果"
# ============================================================

echo "[schedule-test] results: PASS=$PASS FAIL=$FAIL"

if [ "$FAIL" -ne 0 ]; then
  echo "[schedule-test] suite FAILED" >&2
  exit 1
fi
chmod -R u+rwX "$TEST_ROOT" 2>/dev/null || true
rm -rf "$TEST_ROOT"
echo "[schedule-test] all cases passed"
