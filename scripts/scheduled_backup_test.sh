#!/usr/bin/env bash
#
# 定时备份（Scheduled + Full）的专项测试。
#
# 分区：
#   A. 核心单元测试：scheduler_core / scheduled_backup / terminal_secret
#   B. CLI pipeline parity：legacy 仍然是 v0.1，出现 pipeline 选项才走 v2
#   C. 密码只从 TTY 读：真实 PTY 集成测试（备份问两次、恢复问一次）
#   D. CLI schedule 子命令的真实语义（无变化 skip、有变化建快照、retention）
#   D2. review-fix 回归：enable 校验、启用时刻、clear-filters、baseline 绑定
#   D3. 崩溃一致性：archive / manifest / schedule.json 的中间状态（C0-C8）
#   D4. 多进程：真实的两个 watch / watch 与 run 互斥 / SIGTERM 释放锁
#   D5. 故障注入：坏 store、只读目录、仓库与源临时不可用、软链接
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

# 全应用单实例锁住在配置根里（<XDG_CONFIG_HOME>/backup-project/backup-gui-modern/
# app.lock，见 app_paths.h）。把配置根指到测试私有目录有两个好处：
#   * 测试不会去抢用户桌面上那把真实的锁，也不会被用户开着的 GUI 干扰；
#   * 仍然真实验证"GUI 与 CLI 解析出同一个锁路径"这条约束——两边都只认这一个
#     环境变量，谁也没有第二套算法。
export XDG_CONFIG_HOME="$TEST_ROOT/xdg"
mkdir -p "$XDG_CONFIG_HOME"

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

run_unit application_lock_test
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
#
# 先把计划重新启用：schedule run 现在会**先**拒绝"没启用"的计划（与 GUI 的
# runNow 是同一个结论、同一个顺序），不启用的话这条用例测到的是那条规则，
# 而不是锁。判断顺序本身也是 parity 的一部分。
"$BACKUPCTL" --config-file "$CONFIG" --schedule-file "$STORE" schedule enable   >/dev/null 2>&1
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
echo "[schedule-test] D2. review-fix：enable 校验 / clear-filters / baseline"
# ============================================================
#
# 这一区对应 GPT 源码 review 提出的问题：
#   * enable 之前必须真的配好仓库（CLI 与 GUI 用同一个校验）；
#   * 已经启用的计划，任何一次修改之后都必须仍然"真的能跑"；
#   * 首次启用把下一次运行排在一个完整周期之后；
#   * manifest 必须绑定到仓库里真实存在的 baseline 快照，删掉它就必须重建；
#   * --clear-filters 让 CLI 也能清空规则（之前只能不断 append）。

FIX="$TEST_ROOT/fix"
mkdir -p "$FIX/src" "$FIX/repo-a" "$FIX/repo-b"
printf 'fix\n' > "$FIX/src/a.txt"
FIX_CONFIG="$FIX/config.json"
FIX_STORE="$FIX/schedule.json"

# ---- 没配仓库时不许"启用" ----
expect_exit "D2.01 未配仓库也能先写好计划（未启用）" 0 \
  "$BACKUPCTL" --config-file "$FIX_CONFIG" --schedule-file "$FIX_STORE" schedule set \
  --source "$FIX/src" --interval-minutes 60
expect_exit "D2.02 没有仓库时 schedule enable 必须失败" 1 \
  "$BACKUPCTL" --config-file "$FIX_CONFIG" --schedule-file "$FIX_STORE" schedule enable
expect_grep "D2.03 报错点名仓库没配" "No backup repository is configured"
"$BACKUPCTL" --config-file "$FIX_CONFIG" --schedule-file "$FIX_STORE" schedule show >"$OUT" 2>&1
expect_grep "D2.04 enable 失败之后仍然是未启用" "Enabled:        no"

# ---- 配好仓库后启用；下一次运行必须在一个周期之后 ----
expect_exit "D2.05 config repository set" 0 \
  "$BACKUPCTL" --config-file "$FIX_CONFIG" config repository set "$FIX/repo-a"
ENABLED_AT="$(date +%s)"
expect_exit "D2.06 schedule enable" 0 \
  "$BACKUPCTL" --config-file "$FIX_CONFIG" --schedule-file "$FIX_STORE" schedule enable
NEXT_RUN="$(sed -n 's/.*"next_run_time_sec": \(-\{0,1\}[0-9]*\).*/\1/p' "$FIX_STORE" | head -1)"
DELTA=$(( NEXT_RUN - ENABLED_AT ))
if [ "$DELTA" -ge 3595 ] && [ "$DELTA" -le 3610 ]; then
  record_pass "D2.07 首次启用把下一次运行排在一个周期之后（delta=$DELTA 秒）"
else
  record_fail "D2.07 首次启用把下一次运行排在一个周期之后" "delta=$DELTA 秒"
fi

# ---- 已启用时改坏 source：拒绝，且旧配置原样保留 ----
expect_exit "D2.08 已启用时非法 source 被拒绝" 1 \
  "$BACKUPCTL" --config-file "$FIX_CONFIG" --schedule-file "$FIX_STORE" schedule set \
  --source /does/not/exist
expect_grep "D2.09 报错说明源目录不可用" "Schedule source directory"
"$BACKUPCTL" --config-file "$FIX_CONFIG" --schedule-file "$FIX_STORE" schedule show >"$OUT" 2>&1
expect_grep "D2.10 被拒绝后旧 source 原样保留" "$FIX/src"
expect_grep "D2.11 被拒绝后仍然启用" "Enabled:        yes"

# ---- 已启用 + 仓库缺失：任何修改都必须被拒绝 ----
expect_exit "D2.12 已启用但仓库缺失时 schedule set 被拒绝" 1 \
  "$BACKUPCTL" --config-file "$FIX/no-config.json" --schedule-file "$FIX_STORE" schedule set \
  --interval-minutes 30
expect_grep "D2.13 报错点名仓库没配" "No backup repository is configured"
"$BACKUPCTL" --config-file "$FIX_CONFIG" --schedule-file "$FIX_STORE" schedule show >"$OUT" 2>&1
expect_grep "D2.14 被拒绝后旧周期原样保留" "Interval:       60 minute(s)"

# ---- --clear-filters ----
"$BACKUPCTL" --config-file "$FIX_CONFIG" --schedule-file "$FIX_STORE" schedule set \
  --include 'ext:cpp' --exclude 'path:**/build/**' >/dev/null 2>&1
"$BACKUPCTL" --config-file "$FIX_CONFIG" --schedule-file "$FIX_STORE" schedule show >"$OUT" 2>&1
expect_grep "D2.15 规则先追加进来" "Include rules:  ext:cpp"
expect_grep "D2.16 exclude 也追加进来了" "Exclude rules:  path:**/build/**"

expect_exit "D2.17 --clear-filters + 新规则" 0 \
  "$BACKUPCTL" --config-file "$FIX_CONFIG" --schedule-file "$FIX_STORE" schedule set \
  --clear-filters --include 'ext:txt;md'
"$BACKUPCTL" --config-file "$FIX_CONFIG" --schedule-file "$FIX_STORE" schedule show >"$OUT" 2>&1
expect_grep "D2.18 clear 之后 include 只剩新的" "Include rules:  ext:txt;md"
expect_grep "D2.19 clear 之后 exclude 被清空" "Exclude rules:  (none)"

expect_exit "D2.20 --include 写在 --clear-filters 之前也一样" 0 \
  "$BACKUPCTL" --config-file "$FIX_CONFIG" --schedule-file "$FIX_STORE" schedule set \
  --include 'ext:cpp' --clear-filters --include 'ext:txt'
"$BACKUPCTL" --config-file "$FIX_CONFIG" --schedule-file "$FIX_STORE" schedule show >"$OUT" 2>&1
expect_grep "D2.21 顺序无关：两条新规则都在" "Include rules:  ext:cpp, ext:txt"

expect_exit "D2.22 只给 --clear-filters" 0 \
  "$BACKUPCTL" --config-file "$FIX_CONFIG" --schedule-file "$FIX_STORE" schedule set \
  --clear-filters
"$BACKUPCTL" --config-file "$FIX_CONFIG" --schedule-file "$FIX_STORE" schedule show >"$OUT" 2>&1
expect_grep "D2.23 规则被清空" "Include rules:  (none)"

# ---- baseline：删掉最新快照之后必须重建，而不是 skip ----
expect_exit "D2.24 建立 baseline" 0 \
  "$BACKUPCTL" --config-file "$FIX_CONFIG" --schedule-file "$FIX_STORE" schedule run
expect_grep "D2.25 建立的是完整快照" "Created a new full snapshot"
FIX_BAK="$(ls -t "$FIX/repo-a" | head -1)"
expect_exit "D2.26 源没变时照样 skip" 0 \
  "$BACKUPCTL" --config-file "$FIX_CONFIG" --schedule-file "$FIX_STORE" schedule run
expect_grep "D2.27 无变化跳过" "Skipped: the source has not changed"

rm -f "$FIX/repo-a/$FIX_BAK"
expect_exit "D2.28 手工删掉 baseline 之后再跑一轮" 0 \
  "$BACKUPCTL" --config-file "$FIX_CONFIG" --schedule-file "$FIX_STORE" schedule run
expect_grep "D2.29 明确报告基线被重建" "baseline reset"
expect_grep "D2.30 建立的是新的完整快照" "Created a new full snapshot"
FIX_COUNT_A="$(ls "$FIX/repo-a" | wc -l)"
if [ "$FIX_COUNT_A" = "1" ]; then
  record_pass "D2.31 仓库里又恰好只剩一份快照"
else
  record_fail "D2.31 仓库里又恰好只剩一份快照" "实际 $FIX_COUNT_A 份"
fi

expect_exit "D2.32 重建之后源没变，继续 skip" 0 \
  "$BACKUPCTL" --config-file "$FIX_CONFIG" --schedule-file "$FIX_STORE" schedule run
expect_grep "D2.33 新的 baseline 生效" "Skipped: the source has not changed"

# ---- 换仓库：新仓库必须拿到一份 baseline ----
expect_exit "D2.34 换到另一个仓库" 0 \
  "$BACKUPCTL" --config-file "$FIX_CONFIG" config repository set "$FIX/repo-b"
expect_exit "D2.35 换仓库后立即检查并运行" 0 \
  "$BACKUPCTL" --config-file "$FIX_CONFIG" --schedule-file "$FIX_STORE" schedule run
expect_grep "D2.36 新仓库拿到的是基线快照" "baseline reset"
FIX_COUNT_B="$(ls "$FIX/repo-b" | wc -l)"
if [ "$FIX_COUNT_B" = "1" ]; then
  record_pass "D2.37 新仓库里有且只有一份快照"
else
  record_fail "D2.37 新仓库里有且只有一份快照" "实际 $FIX_COUNT_B 份"
fi
FIX_COUNT_A2="$(ls "$FIX/repo-a" | wc -l)"
if [ "$FIX_COUNT_A2" = "1" ]; then
  record_pass "D2.38 旧仓库没有被写入"
else
  record_fail "D2.38 旧仓库没有被写入" "实际 $FIX_COUNT_A2 份"
fi

# ---- 手工构造 manual + incremental：必须明确失败，绝不偷偷按全量跑 ----
FIX_BAD="$FIX/bad-schedule.json"
sed -e 's/"trigger": "scheduled"/"trigger": "manual"/' \
    -e 's/"strategy": "full"/"strategy": "incremental"/' \
    "$FIX_STORE" > "$FIX_BAD"
# 计数必须在被观测的那一轮**之前**取：两边都在之后取的话，这个断言永远成立，
# 也就永远测不出"偷偷按全量跑了一份"。
FIX_BAK_BEFORE="$(ls "$FIX/repo-b" | wc -l)"
expect_exit "D2.39 手改出来的 manual + incremental 在运行期被拒绝" 1 \
  "$BACKUPCTL" --config-file "$FIX_CONFIG" --schedule-file "$FIX_BAD" schedule run
expect_grep "D2.40 拒绝原因点名组合" "Unsupported backup mode: Manual + Incremental"
FIX_BAK_AFTER="$(ls "$FIX/repo-b" | wc -l)"
if [ "$FIX_BAK_BEFORE" = "$FIX_BAK_AFTER" ]; then
  record_pass "D2.41 没有偷偷生成全量备份"
else
  record_fail "D2.41 没有偷偷生成全量备份" "$FIX_BAK_BEFORE -> $FIX_BAK_AFTER"
fi

# ============================================================
echo "[schedule-test] D3. 崩溃一致性：manifest 必须写明它属于哪一份快照"
# ============================================================
#
# archive / manifest / schedule.json 是三个独立文件，各自原子替换，**没有任何
# 时刻能让三个一起提交**。所以"崩在两次写盘之间"留下的中间状态不是假想，
# 而是必然会出现的真实磁盘状态。D3 把每一种都摆出来，钉住同一条不变式：
#
#   只有 manifest 自己声明的归属与 state 记录的 baseline 完全一致，
#   才允许由 "manifest == current" 推出"可以跳过"。
#
# 任何一环对不上都必须重建一份完整基线快照：多建一份是安全的代价，
# 错误跳过是一个补不回来的数据缺口。
#
# 素材全部由真实运行产生，场景只做搬运与就地篡改——不手写字节。

CC="$TEST_ROOT/crash-cut"
rm -rf "$CC"
mkdir -p "$CC/src" "$CC/repo"
printf 'v1\n' > "$CC/src/file.txt"
printf 'keep\n' > "$CC/src/keep.log"
CC_CONFIG="$CC/config.json"
CC_STORE="$CC/schedule.json"
CC_MANIFEST="$CC/schedule-manifest.dat"
CC_TAB=$'\t'

"$BACKUPCTL" --config-file "$CC_CONFIG" config repository set "$CC/repo" >/dev/null 2>&1
"$BACKUPCTL" --config-file "$CC_CONFIG" --schedule-file "$CC_STORE" schedule set \
  --source "$CC/src" --interval-minutes 1 --retain 10 >/dev/null 2>&1
"$BACKUPCTL" --config-file "$CC_CONFIG" --schedule-file "$CC_STORE" schedule enable >/dev/null 2>&1
"$BACKUPCTL" --config-file "$CC_CONFIG" --schedule-file "$CC_STORE" schedule run >/dev/null 2>&1
CC_S1="$(ls -1 "$CC/repo")"
cp "$CC_STORE" "$CC/state-S1.json"
cp "$CC_MANIFEST" "$CC/manifest-S1.dat"
sleep 1
printf 'v2\n' > "$CC/src/file.txt"
"$BACKUPCTL" --config-file "$CC_CONFIG" --schedule-file "$CC_STORE" schedule run >/dev/null 2>&1
CC_S2="$(ls -1 "$CC/repo" | grep -v -x "$CC_S1")"
cp "$CC_STORE" "$CC/state-S2.json"
cp "$CC_MANIFEST" "$CC/manifest-S2.dat"

if [ -n "$CC_S1" ] && [ -n "$CC_S2" ] && [ "$CC_S1" != "$CC_S2" ]; then
  record_pass "D3.00 两轮真实运行拿到两份不同的快照"
else
  record_fail "D3.00 两轮真实运行拿到两份不同的快照" "S1=$CC_S1 S2=$CC_S2"
fi

# cc_place <state 素材|-> <manifest 素材|->：把磁盘摆成某个中间状态。
cc_place() {
  case "$1" in -) rm -f "$CC_STORE" ;; *) cp "$CC/$1" "$CC_STORE" ;; esac
  case "$2" in -) rm -f "$CC_MANIFEST" ;; *) cp "$CC/$2" "$CC_MANIFEST" ;; esac
}

# cc_case <编号> <说明> <期望结果 key> [恢复检查编号]
# 跑一轮真实 CLI 评估，从 history 读回结果；只要这一轮真的新建了快照，
# 就把它**单独拷出来**恢复，与当前源逐字节比较。
cc_case() {
  local id="$1" label="$2" expect="$3" recovery_id="${4:-}"
  local result new
  ls -1 "$CC/repo" | sort > "$CC/before.txt"
  "$BACKUPCTL" --config-file "$CC_CONFIG" --schedule-file "$CC_STORE" schedule run >"$OUT" 2>&1
  result="$("$BACKUPCTL" --config-file "$CC_CONFIG" --schedule-file "$CC_STORE" schedule history 2>/dev/null |
            awk 'NR>1 && NF>1 {k=$3} END{print k}')"
  ls -1 "$CC/repo" | sort > "$CC/after.txt"
  new="$(comm -13 "$CC/before.txt" "$CC/after.txt" | head -1)"
  if [ "$result" = "$expect" ]; then
    record_pass "$id $label（$result）"
  else
    record_fail "$id $label" "期望 $expect，实际 $result: $(first_line)"
  fi
  if [ -n "$recovery_id" ] && [ -n "$new" ]; then
    rm -rf "$CC/restored"
    if "$BACKUPCTL" restore "$CC/repo/$new" "$CC/restored" >/dev/null 2>&1 &&
       diff -r "$CC/src" "$CC/restored" >/dev/null 2>&1; then
      record_pass "$recovery_id 新建的 $new 单独恢复 == 当前源"
    else
      record_fail "$recovery_id 新建的 $new 单独恢复 == 当前源" "恢复结果与源不一致"
    fi
  fi
}

# ---- C0：正常配对。这是唯一允许 skip 的形状 ----
cc_place state-S2.json manifest-S2.dat
cc_case "D3.01" "正常配对、源没变，必须 skip" "skipped_no_changes"

# ---- C1：真实 bug。崩在 SaveManifest 与 Save(state) 之间 ----
# state 还停在 S1，manifest 却已经是 S2 的（内容 = M2），两份归档都在。
# S1 依然存在、依然 managed，manifest 也依然等于当前源——只比这两条就会
# 错误地跳过一轮，而仓库里根本没有任何一份快照装得下 M2。
cc_place state-S1.json manifest-S2.dat
cc_case "D3.02" "旧 state + 新 manifest 绝不能 skip" "success_created" "D3.03"
expect_grep "D3.04 明确报告基线被重建" "baseline reset"
CC_COUNT="$(ls -1 "$CC/repo" | wc -l)"
if [ "$CC_COUNT" = "3" ]; then
  record_pass "D3.05 仓库里现在是三份快照（S1、S2、重建的那一份）"
else
  record_fail "D3.05 仓库里现在是三份快照（S1、S2、重建的那一份）" "实际 $CC_COUNT 份"
fi
# 重建之后必须收敛：源没再变，下一轮回到正常的 skip。
cc_case "D3.06" "重建之后源没变，下一轮回到 skip" "skipped_no_changes"

# ---- C2：崩在另一次写盘之间——state 前进了，manifest 还停在旧的 ----
cc_place state-S2.json manifest-S1.dat
cc_case "D3.07" "新 state + 旧 manifest 绝不能 skip" "success_created" "D3.08"

# ---- C3：manifest 整个不见了 ----
cc_place state-S2.json -
cc_case "D3.09" "manifest 不见了，必须重建完整基线" "success_created" "D3.10"

# ---- C4：manifest 被截断（真实的"写到一半掉电"形状）----
head -c 64 "$CC/manifest-S2.dat" > "$CC/truncated.dat"
cc_place state-S2.json truncated.dat
cc_case "D3.11" "manifest 被截断，必须重建完整基线" "success_created" "D3.12"

# ---- C5：旧版本留下的 v1 manifest：读得出来，但没有归属信息 ----
{
  printf 'BPMANIFEST1 %s\n' "$(sed -n '1s/^BPMANIFEST2 \([0-9]*\).*/\1/p' "$CC/manifest-S2.dat")"
  tail -n +2 "$CC/manifest-S2.dat"
} > "$CC/v1.dat"
if head -n 1 "$CC/v1.dat" | grep -q '^BPMANIFEST1 '; then
  record_pass "D3.13 造出了一份真正的 v1 manifest（条目正文不变、没有 binding）"
else
  record_fail "D3.13 造出了一份真正的 v1 manifest（条目正文不变、没有 binding）" "$(head -n 1 "$CC/v1.dat")"
fi
cc_place state-S2.json v1.dat
cc_case "D3.14" "v1 manifest 绝不是可信基线" "success_created" "D3.15"
expect_grep "D3.16 诊断点明 manifest 来自旧版本" "older version"
if head -n 1 "$CC_MANIFEST" | grep -q '^BPMANIFEST2 '; then
  record_pass "D3.17 重建出来的 manifest 是 version 2（升级不会永远重建下去）"
else
  record_fail "D3.17 重建出来的 manifest 是 version 2（升级不会永远重建下去）" "$(head -n 1 "$CC_MANIFEST")"
fi

# ---- C6：state 与 manifest 都指向 S2，但 S2 的归档已经被外部删掉 ----
cc_place state-S2.json manifest-S2.dat
rm -f "$CC/repo/$CC_S2"
cc_case "D3.18" "baseline 归档不见了，必须重建完整基线" "success_created" "D3.19"

# ---- C7：manifest 自己声明的仓库与 state 不一致 ----
sed "1s|${CC_TAB}${CC}/repo${CC_TAB}|${CC_TAB}/tmp/other-repository${CC_TAB}|" \
  "$CC/manifest-S2.dat" > "$CC/other-repo.dat"
if cmp -s "$CC/manifest-S2.dat" "$CC/other-repo.dat"; then
  record_fail "D3.20 篡改 manifest 的仓库字段确实生效了" "字节没有变化"
else
  record_pass "D3.20 篡改 manifest 的仓库字段确实生效了"
fi
cc_place state-S2.json other-repo.dat
cc_case "D3.21" "manifest 声明的是别的仓库，绝不能 skip" "success_created" "D3.22"

# ---- C8：manifest 自己声明的源与 state 不一致（源是头行最后一个字段）----
sed "1s|${CC_TAB}${CC}/src\$|${CC_TAB}/tmp/other-source|" \
  "$CC/manifest-S2.dat" > "$CC/other-src.dat"
if cmp -s "$CC/manifest-S2.dat" "$CC/other-src.dat"; then
  record_fail "D3.23 篡改 manifest 的源字段确实生效了" "字节没有变化"
else
  record_pass "D3.23 篡改 manifest 的源字段确实生效了"
fi
cc_place state-S2.json other-src.dat
cc_case "D3.24" "manifest 声明的是别的源，绝不能 skip" "success_created" "D3.25"

# ============================================================
echo "[schedule-test] D4. 全应用单实例：真实进程"
# ============================================================
#
# 产品规则：整个产品同一时刻只允许**一个**进程。GUI+GUI / GUI+CLI /
# CLI+CLI / CLI+GUI 四种组合全部拒绝，而且必须在进入任何业务逻辑之前拒绝
# （不能出现第二个 controller，不能先读一遍配置再发现已经有实例）。
#
# 锁路径只由配置根（app_paths.h）决定，与 repository、--config-file、
# --schedule-file 全都没有关系——所以下面每个 CLI 都带自己的 --config-file，
# 互相之间照样冲突。"换个参数就能绕过单实例"这条捷径是被堵死的。
#
# 判锁永远靠 flock，不是"锁文件存在"。SIGKILL 与"锁文件仍然留在磁盘上"
# 两条用例专门钉住这一点。

SI="$TEST_ROOT/single-instance"
rm -rf "$SI"
mkdir -p "$SI/src" "$SI/repo"
printf 'single\n' > "$SI/src/a.txt"
SI_CONFIG="$SI/config.json"
SI_STORE="$SI/schedule.json"
SI_APP_LOCK="$XDG_CONFIG_HOME/backup-project/backup-gui-modern/app.lock"

"$BACKUPCTL" --config-file "$SI_CONFIG" config repository set "$SI/repo" >/dev/null 2>&1
"$BACKUPCTL" --config-file "$SI_CONFIG" --schedule-file "$SI_STORE" schedule set \
  --source "$SI/src" --interval-minutes 5 --retain 3 >/dev/null 2>&1
"$BACKUPCTL" --config-file "$SI_CONFIG" --schedule-file "$SI_STORE" schedule enable >/dev/null 2>&1

"$BACKUPCTL" --config-file "$SI_CONFIG" --schedule-file "$SI_STORE" schedule watch \
  >"$SI/watch.log" 2>&1 &
SI_WATCH=$!
SI_HOLD=0
for _ in $(seq 1 100); do
  if grep -q "^pid=$SI_WATCH " "$SI_APP_LOCK" 2>/dev/null; then SI_HOLD=1; break; fi
  sleep 0.1
done
if [ "$SI_HOLD" = "1" ]; then
  record_pass "D4.01 运行中的 CLI 持有的是全应用锁（app.lock 里的 pid 就是它）"
else
  record_fail "D4.01 运行中的 CLI 持有的是全应用锁（app.lock 里的 pid 就是它）" \
    "app.lock 里没有 pid=$SI_WATCH"
fi

SI_ARCHIVES_BEFORE="$(ls -1 "$SI/repo" 2>/dev/null | wc -l)"
SI_STORE_SUM_BEFORE="$(cksum "$SI_STORE" | cut -d' ' -f1)"

expect_exit "D4.02 第二个 CLI（watch）被拒绝" 3 \
  "$BACKUPCTL" --config-file "$SI_CONFIG" --schedule-file "$SI_STORE" schedule watch
expect_grep "D4.03 理由是产品规则：只允许一个 GUI 或 CLI" "only one GUI or CLI process"

expect_exit "D4.04 schedule show 在业务逻辑之前就被拒绝" 3 \
  "$BACKUPCTL" --config-file "$SI_CONFIG" --schedule-file "$SI_STORE" schedule show
expect_exit "D4.05 repository list 同样被拒绝" 3 \
  "$BACKUPCTL" --config-file "$SI_CONFIG" repository list
expect_exit "D4.06 config repository show 同样被拒绝" 3 \
  "$BACKUPCTL" --config-file "$SI_CONFIG" config repository show
expect_exit "D4.07 连 config repository set 都被拒绝（不写任何持久状态）" 3 \
  "$BACKUPCTL" --config-file "$SI/other.json" config repository set "$SI/repo"
expect_absent "D4.08 被拒绝的那次 set 没有创建它的 config 文件" "$SI/other.json"
expect_exit "D4.09 backup 也被拒绝" 3 \
  "$BACKUPCTL" --config-file "$SI_CONFIG" backup "$SI/src" "$SI/other.bak"
expect_absent "D4.10 被拒绝的 backup 没有留下任何归档" "$SI/other.bak"
# 纯 --help 不进入业务状态，因此刻意不抢锁：已经有 GUI 在跑的时候，用户仍然
# 应该看得到用法。
expect_exit "D4.11 --help 不抢锁，有实例在跑时仍然可用" 0 "$BACKUPCTL" --help

if [ "$(ls -1 "$SI/repo" 2>/dev/null | wc -l)" = "$SI_ARCHIVES_BEFORE" ] &&
   [ "$(cksum "$SI_STORE" | cut -d' ' -f1)" = "$SI_STORE_SUM_BEFORE" ]; then
  record_pass "D4.12 这一串拒绝既没建归档也没改 schedule 状态"
else
  record_fail "D4.12 这一串拒绝既没建归档也没改 schedule 状态" "仓库或 store 变了"
fi

if [ -x "$GUI" ]; then
  export QT_QPA_PLATFORM=offscreen
  expect_exit "D4.13 GUI 在 CLI 持锁时拒绝启动" 3 \
    "$GUI" --schedule-show --config-file "$SI_CONFIG" --schedule-file "$SI_STORE"
  expect_grep "D4.14 GUI 报的是同一句单实例拒绝" "only one GUI or CLI process"
else
  echo "  SKIP  D4.13/D4.14：没有 build/backup-gui-modern"
fi

# SIGTERM：flock 随进程退出自动释放——这正是选 flock 而不是 pidfile 的理由。
kill -TERM "$SI_WATCH" 2>/dev/null
for _ in $(seq 1 100); do kill -0 "$SI_WATCH" 2>/dev/null || break; sleep 0.1; done
if kill -0 "$SI_WATCH" 2>/dev/null; then
  record_fail "D4.15 SIGTERM 之后 watch 退出" "进程仍然存活"
  kill -KILL "$SI_WATCH" 2>/dev/null
else
  record_pass "D4.15 SIGTERM 之后 watch 正常退出"
fi
wait "$SI_WATCH" 2>/dev/null

expect_exit "D4.16 SIGTERM 之后锁自动释放：schedule show 又能跑" 0 \
  "$BACKUPCTL" --config-file "$SI_CONFIG" --schedule-file "$SI_STORE" schedule show
expect_file "D4.17 锁文件仍然留在磁盘上（锁不靠删文件释放）" "$SI_APP_LOCK"

# SIGKILL：内核必须释放 flock。stale 的 pid 提示留在文件里也不许把产品锁死。
"$BACKUPCTL" --config-file "$SI_CONFIG" --schedule-file "$SI_STORE" schedule watch \
  >"$SI/watch2.log" 2>&1 &
SI_WATCH2=$!
for _ in $(seq 1 100); do
  if grep -q "^pid=$SI_WATCH2 " "$SI_APP_LOCK" 2>/dev/null; then break; fi
  sleep 0.1
done
kill -KILL "$SI_WATCH2" 2>/dev/null
for _ in $(seq 1 100); do kill -0 "$SI_WATCH2" 2>/dev/null || break; sleep 0.1; done
wait "$SI_WATCH2" 2>/dev/null
expect_exit "D4.18 SIGKILL 之后内核释放 flock：schedule show 能跑" 0 \
  "$BACKUPCTL" --config-file "$SI_CONFIG" --schedule-file "$SI_STORE" schedule show

# CLI vs GUI / GUI vs GUI：GUI 用的必须是同一把锁、同一个路径。
if [ -x "$GUI" ]; then
  export QT_QPA_PLATFORM=offscreen
  "$GUI" --config-file "$SI_CONFIG" --schedule-file "$SI_STORE" \
    >"$SI/gui1.log" 2>&1 &
  SI_GUI1=$!
  SI_GUI_HOLD=0
  for _ in $(seq 1 150); do
    if grep -q "^pid=$SI_GUI1 " "$SI_APP_LOCK" 2>/dev/null; then SI_GUI_HOLD=1; break; fi
    sleep 0.1
  done
  if [ "$SI_GUI_HOLD" = "1" ]; then
    record_pass "D4.19 GUI 启动后持有同一把全应用锁"
  else
    record_fail "D4.19 GUI 启动后持有同一把全应用锁" "app.lock 里没有 pid=$SI_GUI1"
  fi

  expect_exit "D4.20 CLI vs GUI：GUI 在跑时 CLI 被拒绝" 3 \
    "$BACKUPCTL" --config-file "$SI_CONFIG" --schedule-file "$SI_STORE" schedule show
  expect_exit "D4.21 GUI vs GUI：第二个 GUI 被拒绝" 3 \
    "$GUI" --config-file "$SI_CONFIG" --schedule-file "$SI_STORE"
  expect_grep "D4.22 第二个 GUI 报的是同一句话" "only one GUI or CLI process"
  expect_exit "D4.23 第二个 GUI 没有建出第二套 controller（退出码不是 0）" 3 \
    "$GUI" --schedule-test --config-file "$SI_CONFIG" --schedule-file "$SI_STORE"

  kill -TERM "$SI_GUI1" 2>/dev/null
  for _ in $(seq 1 150); do kill -0 "$SI_GUI1" 2>/dev/null || break; sleep 0.1; done
  kill -KILL "$SI_GUI1" 2>/dev/null
  wait "$SI_GUI1" 2>/dev/null
  expect_exit "D4.24 GUI 退出之后 CLI 立刻恢复" 0 \
    "$BACKUPCTL" --config-file "$SI_CONFIG" --schedule-file "$SI_STORE" schedule show
else
  echo "  SKIP  D4.19-D4.24：没有 build/backup-gui-modern"
fi

# 恶意锁路径：符号链接 / 目录 / FIFO 一律 fail closed，而且绝不 truncate 目标。
# 这三种都不是"已有实例"，必须是 exit 1 的错误，绝不能报成 exit 3 ——
# 把环境问题说成并发问题会让排障方向直接跑偏。
SI_VICTIM="$SI/victim.txt"
printf 'do not touch\n' > "$SI_VICTIM"
mv "$SI_APP_LOCK" "$SI_APP_LOCK.real"
ln -s "$SI_VICTIM" "$SI_APP_LOCK"
expect_exit "D4.25 锁路径是符号链接 -> 明确失败（exit 1，不是 3）" 1 \
  "$BACKUPCTL" --config-file "$SI_CONFIG" --schedule-file "$SI_STORE" schedule show
expect_grep "D4.26 报错说清楚它是符号链接" "symbolic link"
expect_content "D4.27 被指向的文件一个字节都没变" "$SI_VICTIM" "do not touch"
rm -f "$SI_APP_LOCK"
mkdir "$SI_APP_LOCK"
expect_exit "D4.28 锁路径是目录 -> 明确失败" 1 \
  "$BACKUPCTL" --config-file "$SI_CONFIG" --schedule-file "$SI_STORE" schedule show
rmdir "$SI_APP_LOCK"
mkfifo "$SI_APP_LOCK"
expect_exit "D4.29 锁路径是 FIFO -> 明确失败（而不是挂住）" 1 \
  "$BACKUPCTL" --config-file "$SI_CONFIG" --schedule-file "$SI_STORE" schedule show
expect_grep "D4.30 报错说清楚占名字的是 FIFO" "FIFO"
rm -f "$SI_APP_LOCK"
mv "$SI_APP_LOCK.real" "$SI_APP_LOCK"
expect_exit "D4.31 恢复之后一切照旧" 0 \
  "$BACKUPCTL" --config-file "$SI_CONFIG" --schedule-file "$SI_STORE" schedule show

# SchedulerLock 不再是产品层的多进程协作机制，但它仍然是 scheduler 内部的
# 不变量：它的行为由 tests/unit/application_lock_test.cpp 的 AL-18..AL-22
# 直接覆盖（同一个进程里两把锁照样互斥），这里不再假装用两个 CLI 去验证它。

# ============================================================
echo "[schedule-test] D5. 故障注入：坏文件 / 写不进去 / 仓库与源临时不可用"
# ============================================================
#
# 全部是**非 root 也能真实制造**的故障。共同的不变式只有一条：
# 出错时绝不静默前进——宁可下一轮多建一份完整快照，也绝不把一次失败
# 当成"没有变化"。

FI="$TEST_ROOT/fault-injection"
rm -rf "$FI"
mkdir -p "$FI/src" "$FI/repo" "$FI/state"
printf 'one\n' > "$FI/src/a.txt"
FI_CONFIG="$FI/config.json"
FI_STORE="$FI/state/schedule.json"
FI_MANIFEST="$FI/state/schedule-manifest.dat"

"$BACKUPCTL" --config-file "$FI_CONFIG" config repository set "$FI/repo" >/dev/null 2>&1
"$BACKUPCTL" --config-file "$FI_CONFIG" --schedule-file "$FI_STORE" schedule set \
  --source "$FI/src" --interval-minutes 1 --retain 10 >/dev/null 2>&1
"$BACKUPCTL" --config-file "$FI_CONFIG" --schedule-file "$FI_STORE" schedule enable >/dev/null 2>&1
"$BACKUPCTL" --config-file "$FI_CONFIG" --schedule-file "$FI_STORE" schedule run >/dev/null 2>&1

# ---- A. schedule.json 本身坏掉 ----
cp "$FI_STORE" "$FI/full-state.json"
head -c 20 "$FI/full-state.json" > "$FI/truncated-state.json"
expect_exit "D5.01 截断的 schedule.json 被明确拒绝" 1 \
  "$BACKUPCTL" --config-file "$FI_CONFIG" --schedule-file "$FI/truncated-state.json" schedule show
: > "$FI/empty-state.json"
expect_exit "D5.02 空的 schedule.json 被明确拒绝" 1 \
  "$BACKUPCTL" --config-file "$FI_CONFIG" --schedule-file "$FI/empty-state.json" schedule show
printf 'not json at all\n' > "$FI/garbage-state.json"
expect_exit "D5.03 非 JSON 的 schedule.json 被明确拒绝" 1 \
  "$BACKUPCTL" --config-file "$FI_CONFIG" --schedule-file "$FI/garbage-state.json" schedule show
# 坏文件绝不能被当成"没配过"而静默回退到默认配置：run 同样必须明确失败，
# 而不是拿一份空配置去跑一轮。
FI_ARCHIVES_BAD="$(ls -1 "$FI/repo" | wc -l)"
expect_exit "D5.04 坏 store 上 schedule run 也明确失败" 1   "$BACKUPCTL" --config-file "$FI_CONFIG" --schedule-file "$FI/garbage-state.json" schedule run
if [ "$FI_ARCHIVES_BAD" = "$(ls -1 "$FI/repo" | wc -l)" ]; then
  record_pass "D5.04b 坏 store 没有偷偷产生任何快照"
else
  record_fail "D5.04b 坏 store 没有偷偷产生任何快照" "仓库里的数量变了"
fi

# ---- B. 写不进去：目录只读 ----
FI_MANIFEST_BEFORE="$(md5sum "$FI_MANIFEST" | cut -d' ' -f1)"
printf 'two\n' > "$FI/src/b.txt"
chmod 0500 "$FI/state"
"$BACKUPCTL" --config-file "$FI_CONFIG" --schedule-file "$FI_STORE" schedule run >"$OUT" 2>&1
FI_STATUS=$?
chmod 0700 "$FI/state"
if [ "$FI_STATUS" = "0" ]; then
  record_pass "D5.05 归档已发布之后，state 写不进去只降级成诊断（退出 0）"
else
  record_fail "D5.05 归档已发布之后，state 写不进去只降级成诊断（退出 0）" "exit=$FI_STATUS: $(first_line)"
fi
expect_grep "D5.06 诊断说明 state 没能保存" "could not be saved"
FI_MANIFEST_AFTER="$(md5sum "$FI_MANIFEST" | cut -d' ' -f1)"
if [ "$FI_MANIFEST_BEFORE" = "$FI_MANIFEST_AFTER" ]; then
  record_pass "D5.07 只读目录下 manifest 也没有被写坏（原子替换没留半个文件）"
else
  record_fail "D5.07 只读目录下 manifest 也没有被写坏（原子替换没留半个文件）" "字节变了"
fi
expect_exit "D5.08 恢复可写之后下一轮仍然不漏变化" 0 \
  "$BACKUPCTL" --config-file "$FI_CONFIG" --schedule-file "$FI_STORE" schedule run
expect_grep "D5.09 报告的是新建了一份完整快照，而不是 skip" "Created a new full snapshot"

# ---- C. 仓库临时不可用 ----
FI_LAST_BEFORE="$("$BACKUPCTL" --config-file "$FI_CONFIG" --schedule-file "$FI_STORE" schedule show 2>/dev/null | sed -n 's/^Last success:   //p')"
FI_MANAGED_BEFORE="$("$BACKUPCTL" --config-file "$FI_CONFIG" --schedule-file "$FI_STORE" schedule show 2>/dev/null | sed -n 's/^Managed:        //p')"
FI_ARCHIVES_BEFORE="$(ls -1 "$FI/repo" | wc -l)"
mv "$FI/repo" "$FI/repo-moved"
expect_exit "D5.10 仓库被移走时运行必须失败（不能当成一个空仓库）" 1 \
  "$BACKUPCTL" --config-file "$FI_CONFIG" --schedule-file "$FI_STORE" schedule run
expect_grep "D5.11 报错说明 baseline 被原样保留、什么都没写" "baseline was kept and nothing was written"
mv "$FI/repo-moved" "$FI/repo"
FI_LAST_AFTER="$("$BACKUPCTL" --config-file "$FI_CONFIG" --schedule-file "$FI_STORE" schedule show 2>/dev/null | sed -n 's/^Last success:   //p')"
FI_MANAGED_AFTER="$("$BACKUPCTL" --config-file "$FI_CONFIG" --schedule-file "$FI_STORE" schedule show 2>/dev/null | sed -n 's/^Managed:        //p')"
if [ "$FI_LAST_BEFORE" = "$FI_LAST_AFTER" ]; then
  record_pass "D5.12 失败没有推进 last success"
else
  record_fail "D5.12 失败没有推进 last success" "$FI_LAST_BEFORE -> $FI_LAST_AFTER"
fi
if [ "$FI_MANAGED_BEFORE" = "$FI_MANAGED_AFTER" ]; then
  record_pass "D5.13 失败没有清空 managed 名单"
else
  record_fail "D5.13 失败没有清空 managed 名单" "$FI_MANAGED_BEFORE -> $FI_MANAGED_AFTER"
fi
expect_exit "D5.14 仓库回来了、源没变，必须 skip（ownership 没有被错误清空）" 0 \
  "$BACKUPCTL" --config-file "$FI_CONFIG" --schedule-file "$FI_STORE" schedule run
expect_grep "D5.15 确实是一次 skip，而不是又建了一份" "Skipped: the source has not changed"
FI_ARCHIVES_AFTER="$(ls -1 "$FI/repo" | wc -l)"
if [ "$FI_ARCHIVES_BEFORE" = "$FI_ARCHIVES_AFTER" ]; then
  record_pass "D5.16 整个停摆期间仓库里的快照数量没有变化"
else
  record_fail "D5.16 整个停摆期间仓库里的快照数量没有变化" "$FI_ARCHIVES_BEFORE -> $FI_ARCHIVES_AFTER"
fi

# ---- D. 源目录临时不可用 ----
mv "$FI/src" "$FI/src-moved"
expect_exit "D5.17 源目录被移走时运行必须失败" 1 \
  "$BACKUPCTL" --config-file "$FI_CONFIG" --schedule-file "$FI_STORE" schedule run
expect_grep "D5.18 报错点名源目录" "$FI/src"
mv "$FI/src-moved" "$FI/src"
expect_exit "D5.19 源回来之后内容没变，必须 skip（state 没有错误前进）" 0 \
  "$BACKUPCTL" --config-file "$FI_CONFIG" --schedule-file "$FI_STORE" schedule run
expect_grep "D5.20 确实是一次 skip" "Skipped: the source has not changed"

# ---- E. 软链接：仓库与源都必须被拒绝 ----
ln -s "$FI/repo" "$FI/repo-link"
expect_exit "D5.21 软链接仓库被拒绝" 1 \
  "$BACKUPCTL" --config-file "$FI_CONFIG" config repository set "$FI/repo-link"
expect_grep "D5.22 报错说明仓库不能是软链接" "must not be a symbolic link"
ln -s "$FI/src" "$FI/src-link"
expect_exit "D5.23 软链接源目录被拒绝" 1 \
  "$BACKUPCTL" --config-file "$FI_CONFIG" --schedule-file "$FI_STORE" schedule set \
  --source "$FI/src-link"
expect_grep "D5.24 报错说明源目录不能是软链接" "symbolic link"
"$BACKUPCTL" --config-file "$FI_CONFIG" --schedule-file "$FI_STORE" schedule show >"$OUT" 2>&1
expect_grep "D5.25 被拒绝之后旧源目录原样保留" "$FI/src"

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

  # GUI 自检跑在自己的 QTemporaryDir 里，进程一退出那个仓库就不存在了；而它
  # 留下的这份计划是 enabled 的，任何修改都会先过一遍"仍然真的能跑"的完整校验
  # （review-fix 新加的那条）。所以先把仓库重新指到一个真实存在的目录：
  # 这一步失败本身就是那条新约束在起作用的证据。
  expect_exit "E.07b 重新指向一个真实存在的仓库" 0 \
    "$BACKUPCTL" --config-file "$CROSS_CONFIG" config repository set "$CROSS/repo"

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
  if [ "$(grep -c 'BPMANIFEST2' "$ROOT_DIR/src/scheduler/source_manifest.cpp")" -ge 1 ] &&
     [ "$(grep -c 'BPMANIFEST1' "$ROOT_DIR/src/scheduler/source_manifest.cpp")" -ge 1 ] &&
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
echo "[schedule-test] I. 前端 parity 契约：同一份核心、同一套判断"
# ============================================================
#
# E 区证明两个前端读的是**同一份 store**；I 区证明它们用的是**同一套判断**：
#   * repository：一个前端写、另一个前端读；
#   * 归档：GUI 创建、CLI 列出与删除（同一个 Catalog、同一套命名规则）；
#   * 非法输入：两个前端都拒绝，而且拒绝的理由就是共享核心那一句话。
#
# 这里刻意不逐字比较两个前端的输出文本——措辞可以不同，业务判断不能不同。

if [ ! -x "$GUI" ]; then
  echo "  SKIP  I 区：没有 build/backup-gui-modern"
else
  export QT_QPA_PLATFORM=offscreen

  PAR="$TEST_ROOT/parity"
  rm -rf "$PAR"
  mkdir -p "$PAR/src" "$PAR/repo-a" "$PAR/repo-b" "$PAR/restore"
  printf 'parity\n' > "$PAR/src/a.txt"
  PAR_CONFIG="$PAR/config.json"
  PAR_STORE="$PAR/schedule.json"

  # ---- repository：两个方向都要通 ----
  "$BACKUPCTL" --config-file "$PAR_CONFIG" config repository set "$PAR/repo-a" \
    >"$OUT" 2>&1
  "$GUI" --schedule-show --config-file "$PAR_CONFIG" --schedule-file "$PAR_STORE" \
    >"$OUT" 2>&1
  expect_grep "I.01 CLI 设的仓库 GUI 读到同一个路径" "repository=$PAR/repo-a"

  # --config-file / --schedule-file 必须一并给：不给就是让 GUI 去用默认位置，
  # 那样比对的就不是同一份配置了（"测试自己写错隔离路径"是最容易骗过自己的
  # 那类假通过）。
  #
  # 这个自检最后会把它自己的产物删掉（产品行为：测完就清理），所以先用
  # BACKUP_MODERN_KEEP_ARTIFACT 留一份副本，好让 CLI 去恢复它。
  PAR_KEEP="$PAR/gui-artifact.bak"
  BACKUP_MODERN_KEEP_ARTIFACT="$PAR_KEEP" "$GUI" --repository-test \
    "$PAR/src" "$PAR/repo-b" "$PAR/restore-gui" \
    --config-file "$PAR_CONFIG" --schedule-file "$PAR_STORE" \
    >"$PAR/gui-repo.log" 2>&1
  if [ $? -eq 0 ]; then
    record_pass "I.02 GUI 的仓库链路自检通过（保存 / 备份 / 列表 / 恢复 / 删除）"
  else
    record_fail "I.02 GUI 的仓库链路自检通过（保存 / 备份 / 列表 / 恢复 / 删除）" \
      "$(tail -2 "$PAR/gui-repo.log" | tr '\n' ' ')"
  fi
  "$BACKUPCTL" --config-file "$PAR_CONFIG" config repository show >"$OUT" 2>&1
  expect_grep "I.03 GUI 设的仓库 CLI 读到同一个路径" "Repository:  $PAR/repo-b"

  # ---- 归档：GUI 产的 v2 容器，CLI 必须恢复得回来 ----
  expect_file "I.04 GUI 的产物被留了下来（BACKUP_MODERN_KEEP_ARTIFACT）" "$PAR_KEEP"
  expect_exit "I.05 CLI 能恢复 GUI 产的归档" 0 \
    "$BACKUPCTL" restore "$PAR_KEEP" "$PAR/restore-cli"
  PAR_RESTORED="$(find "$PAR/restore-cli" -name a.txt -type f 2>/dev/null | head -n 1)"
  if [ -n "$PAR_RESTORED" ] && cmp -s "$PAR/src/a.txt" "$PAR_RESTORED"; then
    record_pass "I.06 CLI 恢复出来的内容与源文件逐字节一致"
  else
    record_fail "I.06 CLI 恢复出来的内容与源文件逐字节一致" "找不到或不同: $PAR_RESTORED"
  fi
  # GUI 的自检把它的产物从共享仓库里删掉了，CLI 必须看到同一个结果。
  "$BACKUPCTL" --config-file "$PAR_CONFIG" repository list >"$OUT" 2>&1
  expect_grep "I.07 GUI 删掉的归档在 CLI 这边也确实没了" "Archives:   0"

  # ---- 非法输入：两个前端拒绝的是同一件事，理由是同一句核心原文 ----
  "$BACKUPCTL" --config-file "$PAR_CONFIG" --schedule-file "$PAR_STORE" schedule set \
    --source "$PAR/src" --interval-minutes 5 --retain 3 >/dev/null 2>&1
  "$BACKUPCTL" --config-file "$PAR_CONFIG" --schedule-file "$PAR_STORE" schedule enable \
    >/dev/null 2>&1
  expect_exit "I.09 启用状态下把源改成不存在的目录 -> 拒绝" 1 \
    "$BACKUPCTL" --config-file "$PAR_CONFIG" --schedule-file "$PAR_STORE" schedule set \
    --source "$PAR/does-not-exist"
  expect_grep "I.10 理由是共享核心的原文（Schedule source directory）" \
    "Schedule source directory"
  expect_exit "I.11 非法周期在解析阶段就被拒绝（exit 2，不落盘）" 2 \
    "$BACKUPCTL" --config-file "$PAR_CONFIG" --schedule-file "$PAR_STORE" schedule set \
    --interval-minutes 0
  expect_exit "I.12 未知打包方式 key 被拒绝（exit 2）" 2 \
    "$BACKUPCTL" --config-file "$PAR_CONFIG" --schedule-file "$PAR_STORE" schedule set \
    --pack tar
  expect_exit "I.13 未知压缩方式 key 被拒绝（exit 2）" 2 \
    "$BACKUPCTL" --config-file "$PAR_CONFIG" --schedule-file "$PAR_STORE" schedule set \
    --compression zip
  expect_exit "I.14 定时加密边界：aes 在 CLI 侧被拒绝（exit 2）" 2 \
    "$BACKUPCTL" --config-file "$PAR_CONFIG" --schedule-file "$PAR_STORE" schedule set \
    --encryption aes-256-ctr-hmac-sha256
  expect_grep "I.15 拒绝理由与 GUI 挂起时用的是同一条核心规则" "none only"
  expect_exit "I.16 非法筛选规则被拒绝（exit 2）" 2 \
    "$BACKUPCTL" --config-file "$PAR_CONFIG" --schedule-file "$PAR_STORE" schedule set \
    --include "type:bogus"

  # ---- 结构性契约：共享规则只有一份实现 ----
  # 这几条不是"风格检查"：key 表或校验函数一旦被复制成两份，就会在某个
  # 边界上漂移，而漂移的表现是"GUI 能存、CLI 读不了"这种最难查的 bug。
  PAR_SHARED=1
  for symbol in ParsePackMethodKey ParseCompressionMethodKey \
                ValidateScheduleConfig ValidateScheduleForEnable; do
    if ! grep -q "$symbol" "$ROOT_DIR/ui/modern/schedule_controller.cpp" ||
       ! grep -q "$symbol" "$ROOT_DIR/src/cli/cli_commands.cpp"; then
      PAR_SHARED=0
      record_fail "I.17 GUI 与 CLI 都调用共享核心的 $symbol" "有一侧没有引用它"
    fi
  done
  if [ "$PAR_SHARED" = "1" ]; then
    record_pass "I.17 GUI 与 CLI 调用的是同一批 key 表与校验函数（没有第二套判断）"
  fi

  # 加密边界在界面上只有一个取值：none。它不自己造一套"unsupported"判断，
  # 而是把 none 交给共享的 ValidateScheduleConfig —— 那条规则因此在两个前端上
  # 不可能给出不同的答案。
  if grep -q "EncryptionMethod::kNone" \
       "$ROOT_DIR/ui/modern/schedule_controller.cpp" &&
     grep -q "ValidateScheduleConfig" \
       "$ROOT_DIR/ui/modern/schedule_controller.cpp"; then
    record_pass "I.18 界面只提供 none，边界判断交给共享 ValidateScheduleConfig"
  else
    record_fail "I.18 界面只提供 none，边界判断交给共享 ValidateScheduleConfig" \
      "界面侧出现了自己的加密判断"
  fi

  if ! grep -rq "kMinIntervalMinutes" "$ROOT_DIR/ui/modern" &&
     ! grep -rq "kMaxRetainCount" "$ROOT_DIR/ui/modern"; then
    record_pass "I.19 界面侧没有复制边界常量（范围只在共享核心里定义）"
  else
    record_fail "I.19 界面侧没有复制边界常量（范围只在共享核心里定义）" \
      "ui/modern 下出现了边界常量"
  fi
fi

# ============================================================
echo "[schedule-test] J. 错误语义：同一件事，两个前端给同一个业务结论"
# ============================================================
#
# 要的是**含义相同**，不是措辞逐字相同。这一区把每条边界在 CLI 侧的结论钉住；
# GUI 侧的对应断言在 --schedule-test 里：SCH-99..SCH-109（非法输入的拒绝理由
# 逐字来自共享核心）与 SCH-74..SCH-86（配置不合法 -> 挂起）。
#
# 共同的三条要求：
#   * Core 给出稳定原因（下面每条 assert 的都是核心原文里的关键词）；
#   * 不允许一边 silent fallback（"没启用也照样跑一次"就是典型）；
#   * 不允许一边自动修、一边 fail（挂起态既不修也不猜）。

ERR="$TEST_ROOT/error-semantics"
rm -rf "$ERR"
mkdir -p "$ERR/src" "$ERR/repo" "$ERR/dest-nonempty"
printf 'err\n' > "$ERR/src/a.txt"
printf 'busy\n' > "$ERR/dest-nonempty/keep.txt"
printf 'not-a-dir\n' > "$ERR/repo-file"
ERR_CONFIG="$ERR/config.json"
ERR_STORE="$ERR/schedule.json"
"$BACKUPCTL" --config-file "$ERR_CONFIG" config repository set "$ERR/repo" \
  >/dev/null 2>&1

expect_exit "J.01 源路径不存在 -> 失败" 1 \
  "$BACKUPCTL" backup "$ERR/missing" "$ERR/out.bak"
expect_grep "J.02 原因是核心给出的稳定原因" "Source directory does not exist"
expect_absent "J.03 失败没有留下半份归档" "$ERR/out.bak"

expect_exit "J.04 仓库路径是普通文件 -> 失败" 1 \
  "$BACKUPCTL" --config-file "$ERR_CONFIG" config repository set "$ERR/repo-file"
expect_grep "J.05 原因是核心给出的稳定原因" "is not a directory"

expect_exit "J.06 归档路径落在源目录里 -> 失败" 1 \
  "$BACKUPCTL" backup "$ERR/src" "$ERR/src/inner.bak"
expect_grep "J.07 原因说清楚是路径拓扑问题" "Destination is inside the source"
expect_absent "J.08 被拒绝的归档没有落盘" "$ERR/src/inner.bak"

expect_exit "J.09 未知选项 -> 用法错误" 2 \
  "$BACKUPCTL" backup "$ERR/src" "$ERR/out.bak" --bogus
expect_exit "J.10 非法筛选规则 -> 用法错误" 2 \
  "$BACKUPCTL" backup "$ERR/src" "$ERR/out.bak" --include 'size:not-a-number'
expect_absent "J.11 用法错误没有留下任何归档" "$ERR/out.bak"

expect_exit "J.12 没有交互终端时加密备份明确失败" 1 \
  "$BACKUPCTL" backup "$ERR/src" "$ERR/out.bak" --encryption aes-256-ctr-hmac-sha256
expect_grep "J.13 原因说清楚是终端问题，且不会退回空密码" \
  "interactive terminal is required"
expect_absent "J.14 失败没有留下归档" "$ERR/out.bak"

"$BACKUPCTL" --config-file "$ERR_CONFIG" --schedule-file "$ERR_STORE" schedule set \
  --source "$ERR/src" --interval-minutes 5 --retain 3 >/dev/null 2>&1
expect_exit "J.15 未启用时 schedule run 明确失败（不假装成功）" 1 \
  "$BACKUPCTL" --config-file "$ERR_CONFIG" --schedule-file "$ERR_STORE" schedule run
expect_grep "J.16 理由与 GUI 的 runNow 是同一个结论：没启用" "is disabled"
if [ "$(ls -1 "$ERR/repo" 2>/dev/null | wc -l)" = "0" ]; then
  record_pass "J.17 未启用时没有创建任何归档"
else
  record_fail "J.17 未启用时没有创建任何归档" "仓库里多出了文件"
fi

printf '{ not json' > "$ERR/broken.json"
ERR_SUM_BEFORE="$(cksum "$ERR/broken.json" | cut -d' ' -f1)"
expect_exit "J.18 store 读不懂时 schedule run 明确失败" 1 \
  "$BACKUPCTL" --config-file "$ERR_CONFIG" --schedule-file "$ERR/broken.json" schedule run
expect_grep "J.19 报的是解析器给出的原文" "Invalid JSON"
expect_exit "J.20 store 读不懂时 schedule show 也明确失败" 1 \
  "$BACKUPCTL" --config-file "$ERR_CONFIG" --schedule-file "$ERR/broken.json" schedule show
if [ "$(cksum "$ERR/broken.json" | cut -d' ' -f1)" = "$ERR_SUM_BEFORE" ]; then
  record_pass "J.21 坏 store 没有被静默改写、修复或删除"
else
  record_fail "J.21 坏 store 没有被静默改写、修复或删除" "文件内容变了"
fi

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
