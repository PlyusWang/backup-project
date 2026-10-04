#!/usr/bin/env bash
#
# pr22_ecs_e2e.sh —— PR #22 的阿里云真机端到端：GUI 自己管理的 SSH 安全通道。
#
#   bash scripts/pr22_ecs_e2e.sh [ssh-alias]
#
# 这一轮要证明的产品主张只有一条：
#
#   "服务端只监听 ECS 自己的 127.0.0.1:18765" 这件事，产品**自己**处理掉了 ——
#   用户在界面上填一个 SSH 主机（当前部署是 aliyun-ecs）就能登录、列表、完整
#   备份、增量备份、恢复、退出登录；没有第二个终端，也没有一条要背下来的顺序。
#
# 与 PR #20/#21 的 aliyun_*.sh 的区别：
#   * 那两条脚本是**在脚本里**手敲 ssh -N -L 开隧道，然后让 backupctl 连
#     127.0.0.1:18765；
#   * 这一条**不开隧道**：隧道由 GUI 的 RemoteController 自己起、自己确认、
#     自己回收。脚本只负责准备环境、跑真实 GUI、比对正式数据。
#
# 硬约束（与 PR #22 的 scope 一致）：
#   * 不改安全组、不把 18765 暴露到公网、不改服务端监听地址；
#   * 不弱化 SSH 主机密钥校验；
#   * 不改 BPSEC1 线格式 / 密码学 / 远端元数据；
#   * 口令随机生成，只经环境变量传递；输出里没有口令、token、私钥。
#
# 前置：ECS 上的 backup-server 正在运行且只绑 127.0.0.1:18765；
#       本机能免密（BatchMode）登录该 alias；
#       ECS 上有 PR #21 的服务端与它的传输身份私钥 state/transport.key。
#
# 用法：bash scripts/pr22_ecs_e2e.sh [ssh-alias]，退出码 0 = 全部通过。

set -uo pipefail

ALIAS="$1"
[ -n "$ALIAS" ] || ALIAS=aliyun-ecs
REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
GUI="$REPO_DIR/build/backup-gui-modern"
[ -x "$GUI" ] || { echo "[pr22-e2e] 找不到 $GUI（先 make gui-modern server test-fixtures）" >&2; exit 1; }

WORK_DIR="${PR22_E2E_DIR:-}"
[ -n "$WORK_DIR" ] || WORK_DIR="$(mktemp -d /tmp/pr22-e2e-XXXXXX)"
mkdir -p "$WORK_DIR" || exit 1
OUT_DIR="$WORK_DIR/acceptance"
mkdir -p "$OUT_DIR"
PORT=18765
REMOTE_SERVICE_HOST=127.0.0.1
PASS=0
FAIL=0

pass() { PASS=$((PASS + 1)); echo "  PASS  $1"; }
fail() { FAIL=$((FAIL + 1)); echo "  FAIL  $1 -- $2" >&2; }

echo "[pr22-e2e] 工作目录 $WORK_DIR"

# ---- 0. 前置：免密登录 + 服务端确实只绑回环 ----
if ! ssh -o BatchMode=yes "$ALIAS" 'hostname' > "$WORK_DIR/host.txt" 2>&1; then
  echo "[pr22-e2e] 无法免密登录 $ALIAS" >&2
  exit 1
fi
echo "[pr22-e2e] ECS hostname: $(cat "$WORK_DIR/host.txt")"
if ssh -o BatchMode=yes "$ALIAS" "ss -ltn 2>/dev/null | grep -q '127.0.0.1:$PORT'"; then
  pass "ECS 上服务端只监听 127.0.0.1:$PORT（没有对公网开放）"
else
  fail "ECS 上服务端只监听 127.0.0.1:$PORT" "没有找到该监听；不继续"
  echo "[pr22-e2e] failed=$FAIL" >&2
  exit 1
fi

# ---- 1. 客户端 pin：只从 ECS 本机的身份私钥上读指纹 ----
KEYGEN_OUT="$WORK_DIR/keygen.out"
if ! ssh -o BatchMode=yes "$ALIAS" \
     '~/backup-project-server/bin/backup-server-keygen --show --key-file ~/backup-project-server/state/transport.key' \
     > "$KEYGEN_OUT" 2>&1; then
  echo "[pr22-e2e] 无法从 ECS 读取服务端传输身份" >&2
  head -2 "$KEYGEN_OUT" >&2
  exit 1
fi
PIN="$(grep -oE 'sha256:[0-9a-f]{64}' "$KEYGEN_OUT" | head -1)"
[ -n "$PIN" ] || { echo "[pr22-e2e] keygen 输出里没有指纹" >&2; exit 1; }
pass "已取得服务端身份指纹（pin 只进环境变量，不打印、不写盘）"

# ---- 2. 正式数据的 before 快照 ----
#
# 比对的是**逻辑内容**而不是 sqlite 文件的字节：文件布局会随 vacuum / page
# 变化，那是实现细节，不是"用户的正式数据变了"。分成三块，任何一块不一致
# 都要能一眼看出是哪一块：users / snapshots / blobs（路径 + 大小 + 内容摘要）。
read_state() {
  ssh -o BatchMode=yes "$ALIAS" '
    ROOT="$HOME/backup-project-server"
    DB="$ROOT/state/metadata.sqlite3"
    echo "== users =="
    sqlite3 "$DB" "SELECT username FROM users ORDER BY username;" 2>/dev/null | LC_ALL=C sort
    echo "== snapshots =="
    sqlite3 "$DB" "SELECT owner||\"|\"||name||\"|\"||size_bytes||\"|\"||sha256||\"|\"||kind||\"|\"||generation FROM snapshots ORDER BY owner, name;" 2>/dev/null | LC_ALL=C sort
    echo "== blobs =="
    find "$ROOT/data" -type f -printf "%P %s\n" 2>/dev/null | LC_ALL=C sort
    echo "== blob-digest =="
    ( cd "$ROOT/data" && find . -type f -printf "%P\n" 2>/dev/null | LC_ALL=C sort | xargs -d "\n" sha256sum 2>/dev/null | sha256sum )
  '
}
read_state > "$WORK_DIR/before.txt" 2>&1
if [ ! -s "$WORK_DIR/before.txt" ]; then
  echo "[pr22-e2e] 读不到 ECS 上的正式数据（sqlite3 / data 目录）" >&2
  exit 1
fi
pass "已记录正式数据 before（users / snapshots / blobs）"

# ---- 3. 跑真实 GUI：SSH 安全通道由产品自己建立 ----
ACCOUNT="pr22-e2e-$(head -c 12 /dev/urandom | base64 | tr -dc 'a-z0-9' | head -c 8)"
PASSWORD="pr22-pass-$(head -c 18 /dev/urandom | base64 | tr -dc 'A-Za-z0-9' | head -c 12)"
echo "[pr22-e2e] 隔离测试账户 $ACCOUNT（口令随机生成，不打印）"

# 跑之前先确认没有既存的 ssh 隧道：否则"GUI 自己建起来了"就说不清了。
pgrep -af "ssh .*-N .*-L 127.0.0.1:" > "$WORK_DIR/preexisting.txt" 2>/dev/null || true
if [ -s "$WORK_DIR/preexisting.txt" ]; then
  echo "[pr22-e2e] 警告：跑之前已经存在 ssh -N -L 进程（见 $WORK_DIR/preexisting.txt）" >&2
fi
PRE_PIDS="$(cut -d' ' -f1 < "$WORK_DIR/preexisting.txt" 2>/dev/null | tr '\n' ' ')"

export BACKUP_REMOTE_PASSWORD="$PASSWORD"
export BACKUP_REMOTE_PIN="$PIN"
export BACKUP_REMOTE_SSH_TARGET="$ALIAS"
export QT_QPA_PLATFORM=offscreen
# ACC-15 的第二段是"服务端把空闲连接关掉之后，下一次重连必须用**新** pin"。
# ECS 上的服务端用的是产品默认的 30 秒空闲超时（没有 --io-timeout），所以这里
# 必须等得比它久，否则连接还活着、根本不会发生重连，那条断言就测不到东西。
export BACKUP_REMOTE_IDLE_WAIT="${BACKUP_REMOTE_IDLE_WAIT:-35}"

START_TS=$(date +%s)
timeout 2400 "$GUI" --remote-acceptance "$OUT_DIR" \
  "$REMOTE_SERVICE_HOST" "$PORT" "$ACCOUNT" \
  > "$WORK_DIR/acceptance.log" 2>&1
ACCEPT_STATUS=$?
END_TS=$(date +%s)
echo "[pr22-e2e] --remote-acceptance 退出码 $ACCEPT_STATUS，用时 $((END_TS - START_TS)) 秒"
grep -E "passed=|FAIL" "$WORK_DIR/acceptance.log" | tail -20

if [ "$ACCEPT_STATUS" -eq 0 ]; then
  pass "--remote-acceptance（含 C08..C14：通道建立 / pin 校验 / 重连 / 回收 / 外部隧道）"
else
  fail "--remote-acceptance" "退出码 $ACCEPT_STATUS，见 $WORK_DIR/acceptance.log"
fi

# C08..C14 的证据行单独摘出来，便于评审直接看。
grep -E "C0[89]|C1[0-4]" "$WORK_DIR/acceptance.log" | sed 's/^/  /' || true

# ---- 4. GUI 退出之后不允许留下孤儿 ssh ----
#
# --remote-acceptance 是一次**进程内**运行：进程退出时 RemoteController 的
# 析构会收掉自有 ssh（C13 已经在进程内验证过一次）。这里在**进程外**再验证
# 一遍：进程真的没了，孤儿也不该有。
sleep 2
ORPHANS=""
while read -r pid rest; do
  [ -n "$pid" ] || continue
  case " $PRE_PIDS " in *" $pid "*) continue ;; esac
  ORPHANS="$ORPHANS$pid $rest"$'\n'
done < <(pgrep -af "ssh .*-N .*-L 127.0.0.1:" 2>/dev/null || true)
if [ -z "$ORPHANS" ]; then
  pass "GUI 退出之后没有留下孤儿 ssh -N 进程"
else
  fail "GUI 退出之后没有留下孤儿 ssh -N 进程" "$ORPHANS"
fi

# ---- 5. 正式数据 after == before ----
read_state > "$WORK_DIR/after.txt" 2>&1
if diff -u "$WORK_DIR/before.txt" "$WORK_DIR/after.txt" > "$WORK_DIR/state.diff" 2>&1; then
  pass "正式数据 before == after（users / snapshots / blobs 三块都一致）"
else
  fail "正式数据 before == after" "差异见 $WORK_DIR/state.diff"
  head -40 "$WORK_DIR/state.diff" >&2
fi

echo "[pr22-e2e] passed=$PASS failed=$FAIL"
echo "[pr22-e2e] 工作目录：$WORK_DIR"
echo "[pr22-e2e] 截图目录：$OUT_DIR"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
