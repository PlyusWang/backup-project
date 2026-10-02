#!/usr/bin/env bash
#
# ECS 真机请求序列：PR #20 人工验收缺陷修复的复验入口。
#
#   bash scripts/aliyun_sequence_e2e.sh [ssh-alias] [host] [port] [idle-seconds]
#
# 默认 aliyun-ecs / 127.0.0.1 / 18765 / 31。
#
# 前置条件（脚本自己**不**做端口转发，也不改任何网络配置）：
#   * ECS 上的 backup-server 正在 127.0.0.1:<port> 监听；
#   * 本机已经有一条 SSH 隧道把 127.0.0.1:<port> 转到 ECS 的 127.0.0.1:<port>；
#   * ECS 上有 PR #21 的服务端与传输身份私钥
#     （<server-root>/state/transport.key，由 deploy_aliyun_server.sh 在本机生成）。
#     客户端 pin 从这把私钥的公钥指纹读出来；私钥不离开 ECS。
#
# 它做四件事：
#   1. 构建序列驱动器（make remote-sequence）；
#   2. 用产品 CLI 生成一份真实归档；
#   3. 跑完整序列：注册 / 错误口令 ×4 / 正确登录 / LIST ×10 / 空闲 N 秒 /
#      上传 / LIST / 下载并逐字节比对 / 删除快照 / 错误口令注销（必须失败）/
#      LIST（必须成功）/ 正确口令注销 / 原凭据登录（必须失败）；
#   4. 用 ECS 本机的 admin CLI 交叉验证账户确实不存在，然后**重启服务端**再确认
#      一次原凭据登录仍然失败（排除内存缓存 / stale 句柄）。
#
# 口令只从环境变量 BACKUP_REMOTE_PASSWORD 读，脚本不打印它。

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "$BASH_SOURCE")/.." && pwd)"
cd "$ROOT_DIR"

ALIAS="${1:-aliyun-ecs}"
HOST="${2:-127.0.0.1}"
PORT="${3:-18765}"
IDLE="${4:-31}"

# 工作目录刻意放在 /tmp 而不是仓库的 testdata/ 下：仓库里的测试套件（scripts/
# test.sh）会整棵清掉 testdata，跑在同一个仓库上的复验会被它顺手删掉中间产物。
WORK="${TMPDIR:-/tmp}/aliyun-sequence"
rm -rf "$WORK"
mkdir -p "$WORK/src" "$WORK/repo" "$WORK/out"

echo "[aliyun-sequence] 构建序列驱动器"
if ! make -j4 remote-sequence > "$WORK/build.log" 2>&1; then
  echo "[aliyun-sequence] 构建失败：" >&2
  tail -5 "$WORK/build.log" >&2
  exit 1
fi
echo "[aliyun-sequence] 构建警告：$(grep -ci warning "$WORK/build.log" || true) 条"

echo "[aliyun-sequence] 生成一份真实归档"
printf 'aliyun-sequence\n' > "$WORK/src/a.txt"
head -c 8192 /dev/urandom > "$WORK/src/blob.bin"
./build/backupctl config repository set "$WORK/repo" --config-file "$WORK/config.json" >/dev/null 2>&1
./build/backupctl backup "$WORK/src" --config-file "$WORK/config.json" >/dev/null 2>&1
ARCHIVE="$(ls -1 "$WORK/repo"/*.bak 2>/dev/null | head -1)"
if [ -z "$ARCHIVE" ]; then
  echo "[aliyun-sequence] 无法生成归档" >&2
  exit 1
fi

USER_NAME="ecs-seq-$(head -c 4 /dev/urandom | od -An -tx1 | tr -d ' \n')"
export BACKUP_REMOTE_PASSWORD="$(head -c 24 /dev/urandom | sha256sum | cut -c1-24)"

# ---- PR #21：客户端 pin（服务端身份公钥指纹）----
# 从 ECS 本机的身份私钥上读出公钥与指纹（--show 从不打印私钥内容），私钥
# 不 scp、不进日志。pin 放进环境变量，后面的 backupctl remote 调用不用逐个
# 加 --server-key，也绝不写成明文常量。
KEYGEN_OUT="$WORK/keygen.out"
if ! ssh -o BatchMode=yes "$ALIAS" \
     '~/backup-project-server/bin/backup-server-keygen --show --key-file ~/backup-project-server/state/transport.key' \
     > "$KEYGEN_OUT" 2>&1; then
  echo "[aliyun-sequence] 无法从 ECS 读取服务端传输身份（缺 state/transport.key？）：" >&2
  head -2 "$KEYGEN_OUT" >&2
  exit 1
fi
export BACKUP_REMOTE_SERVER_KEY="$(grep -oE 'sha256:[0-9a-f]{64}' "$KEYGEN_OUT" | head -1)"
if [ -z "$BACKUP_REMOTE_SERVER_KEY" ]; then
  echo "[aliyun-sequence] ECS 的 keygen 输出里没有 sha256:<64 位十六进制> 指纹" >&2
  exit 1
fi
# 注意：./build/remote-sequence（tests/tools/remote_sequence.cpp）用的是同一个
# RemoteArchiveClient，它同样需要服务端 pin。这个脚本只能把 pin 放进环境变量
# BACKUP_REMOTE_SERVER_KEY；如果那个驱动器自己的选项里还没有 pin，它会在握手
# 之前以"无法使用服务端传输身份 pin"失败——那是**产品侧**要补的接口，不能
# 为了跑通就降级成不校验身份（本项目不做 TOFU）。
echo "[aliyun-sequence] 端点 $HOST:$PORT 用户 $USER_NAME 空闲 $IDLE 秒（pin 已就绪）"

echo
echo "[aliyun-sequence] === 真实 BPNET1 序列 ==="
./build/remote-sequence --host "$HOST" --port "$PORT" --user "$USER_NAME" \
  --upload "$ARCHIVE" --download "$WORK/out/downloaded.bak" \
  --idle-seconds "$IDLE"
SEQUENCE_EXIT=$?
echo "[aliyun-sequence] sequence exit=$SEQUENCE_EXIT"
echo "[aliyun-sequence] 上传与下载的 SHA-256："
sha256sum "$ARCHIVE" "$WORK/out/downloaded.bak" 2>/dev/null

echo
echo "[aliyun-sequence] === ECS 本机 admin CLI 交叉验证 ==="
ssh -o BatchMode=yes "$ALIAS" "cd ~/backup-project-server && \
  ./bin/backup-server-admin --root data --db state/metadata.sqlite3 show-user $USER_NAME ; \
  echo show_user_exit=\$? ; \
  echo -n 'list-users 里匹配 ecs-seq 的行数：' ; \
  ./bin/backup-server-admin --root data --db state/metadata.sqlite3 list-users | grep -c 'ecs-seq' ; \
  ./bin/backup-server-admin --root data --db state/metadata.sqlite3 overview | head -4 ; \
  echo -n 'data/users：' ; ls data/users | tr '\n' ' ' ; echo ; \
  echo -n 'data/trash 条目数：' ; ls -A data/trash | wc -l"

echo
echo "[aliyun-sequence] === 重启服务端之后再用原凭据登录（必须失败）==="
# 重启同样要带 --transport-key-file（PR #21 起缺这个参数服务端直接以用法错误
# 退出）。这里顺带把新 pid 与 loopback 监听数一起取回来：如果服务端没起来，
# 下面的"登录必须失败"会变成一个**假通过**（连不上也算失败），所以最终判定
# 里额外要求"新进程存在且端口在监听"。
RESTART_OUT="$(ssh -o BatchMode=yes "$ALIAS" "cd ~/backup-project-server && \
  OLD=\$(pgrep -f 'bin/backup-server --bind' | head -1) && kill -TERM \"\$OLD\" && sleep 2 && \
  setsid nohup ./bin/backup-server --bind 127.0.0.1 --port $PORT \
    --root /home/ubuntu/backup-project-server/data \
    --db /home/ubuntu/backup-project-server/state/metadata.sqlite3 \
    --secret-file /home/ubuntu/.config/backup-project-server/secrets.env \
    --transport-key-file /home/ubuntu/backup-project-server/state/transport.key \
    --log-file /home/ubuntu/backup-project-server/logs/server.log \
    --pid-file /home/ubuntu/backup-project-server/state/server.pid \
    > /home/ubuntu/backup-project-server/logs/server.stdout 2>&1 < /dev/null & \
  sleep 3 ; echo pid=\$(pgrep -f 'bin/backup-server --bind' | head -1) ; \
  echo listeners=\$(ss -ltn | grep -c '127.0.0.1:$PORT')")"
NEW_PID="$(printf '%s\n' "$RESTART_OUT" | sed -n 's/^pid=//p')"
LISTENERS="$(printf '%s\n' "$RESTART_OUT" | sed -n 's/^listeners=//p')"
echo "[aliyun-sequence] 重启后的服务端：pid=$NEW_PID 监听数=${LISTENERS:-0}"
if [ -n "$NEW_PID" ] && [ "${LISTENERS:-0}" -ge 1 ]; then
  echo "[aliyun-sequence] 重启成功（服务端确实在 127.0.0.1:$PORT 监听）"
else
  echo "[aliyun-sequence] 重启失败：服务端没有起来，下面的登录结果不能作为证据" >&2
fi
sleep 2
./build/backupctl remote login --user "$USER_NAME" --host "$HOST" --port "$PORT" > "$WORK/relogin.txt" 2>&1
RELOGIN_EXIT=$?
echo "[aliyun-sequence] 重启之后登录退出码=$RELOGIN_EXIT（必须非 0）"
head -2 "$WORK/relogin.txt"

echo
if [ "$SEQUENCE_EXIT" = "0" ] && [ "$RELOGIN_EXIT" != "0" ] \
   && [ -n "$NEW_PID" ] && [ "${LISTENERS:-0}" -ge 1 ]; then
  echo "ALIYUN_SEQUENCE_ALL_PASS"
  exit 0
fi
echo "ALIYUN_SEQUENCE_FAILED" >&2
exit 1
