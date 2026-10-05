#!/usr/bin/env bash
#
# pr23_ecs_phase9_public_adversarial.sh —— PR #23 Phase 9：公网端点上的对抗与
# 资源测试。
#
#   bash scripts/pr23_ecs_phase9_public_adversarial.sh
#
# 前提：Phase 8 已经通过（公网可达且证书链路正常）。脚本会先确认这一点。
#
# 刻意**不做**真 DDoS：所有"压力"都是个位数到几十个连接、总量受控，
# 目的是证明"畸形输入与少量并发不会把服务端打挂或让它失去服务能力"，
# 不是把带宽或连接数打满。
#
# 覆盖：
#   A. 畸形输入：随机字节、截断的 ClientHello、超大 blob、半开连接；
#   B. 资源：20 个既不发数据也不读的占用连接；之后**合法客户端仍然能连上**；
#   C. §33 在公网上同样生效：连续错误口令之后正确口令也被拒；
#   D. 收尾：服务端 PID 未变（没崩没重启），合法 ping 仍然成功。
#
# 退出码：0 = 全部通过。

set -uo pipefail
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

SERVER_ID="backup-project-cloud-production"
PUBLIC_HOST="8.130.9.200"
PUBLIC_PORT=18765
USER_NAME="phase9-public-user"
ACCOUNT_PASSWORD="phase9-public-password"
BAD_PASSWORD="phase9-wrong-password"

PASS=0
FAIL=0
record_pass() { PASS=$((PASS + 1)); echo "  PASS  $1"; }
record_fail() { FAIL=$((FAIL + 1)); echo "  FAIL  $1 -- $2" >&2; }

echo "[phase9] 0/5 前置：公网可达（Phase 8 的前提）"
if ! timeout 8 bash -c "cat < /dev/null > /dev/tcp/$PUBLIC_HOST/$PUBLIC_PORT" 2>/dev/null; then
  record_fail "公网可达" "连不上：先让 Phase 7/8 通过（安全组 + ufw 都要放行 18765/tcp）"
  echo "[phase9] passed=$PASS failed=$FAIL"
  exit 1
fi
record_pass "公网 $PUBLIC_HOST:$PUBLIC_PORT 可达"

PID_BEFORE="$(ssh aliyun-ecs 'cat /home/ubuntu/backup-project-server/state/server.pid' 2>/dev/null)"
if [ -n "$PID_BEFORE" ]; then
  record_pass "记下服务端 PID=$PID_BEFORE"
else
  record_fail "读取服务端 PID" "拿不到 state/server.pid"
fi

echo "[phase9] 1/5 A. 畸形输入"
python3 - "$PUBLIC_HOST" "$PUBLIC_PORT" <<'PY'
import socket, sys, os
host, port = sys.argv[1], int(sys.argv[2])

def send(payload: bytes, label: str, read_back: bool = True) -> None:
    s = socket.socket()
    s.settimeout(6)
    try:
        s.connect((host, port))
        s.sendall(payload)
        if read_back:
            try:
                s.recv(64)
            except Exception:
                pass
        print("sent %-28s %6d bytes" % (label, len(payload)))
    except Exception as exc:
        print("sent %-28s failed: %s" % (label, exc))
    finally:
        s.close()

send(os.urandom(4096), "random-4k")
send(b"\x00" * 1024, "zeros-1k")
send(b"BPS1" + b"\x01" + b"\x01", "truncated-clienthello")
send(b"BPS1" + b"\x01" + b"\x01" + b"\x00\x01" + os.urandom(1000000), "oversized-1mb")
send(os.urandom(72), "random-72-hello-sized")
PY
record_pass "A01 畸形输入已从公网发送（服务端是否存活见 D 段）"

echo "[phase9] 2/5 B. 受控并发占用（20 个连接，不发数据）"
python3 - "$PUBLIC_HOST" "$PUBLIC_PORT" <<'PY'
import socket, sys, time
host, port = sys.argv[1], int(sys.argv[2])
holders = []
for _ in range(20):
    s = socket.socket()
    s.settimeout(5)
    try:
        s.connect((host, port))
        holders.append(s)
    except Exception:
        pass
print("held %d idle connections for 3s" % len(holders))
time.sleep(3)
for s in holders:
    s.close()
print("released")
PY
record_pass "B01 20 个占用连接已用完并释放（总量受控，不是 DDoS）"

echo "[phase9] 3/5 占用之后合法客户端仍然能连上"
if ./build/backupctl remote ping --host "$PUBLIC_HOST" --port "$PUBLIC_PORT" \
     --expected-server-id "$SERVER_ID" > /tmp/pr23/phase9-ping1.txt 2>&1; then
  record_pass "B02 合法 ping 在并发占用之后仍然成功"
else
  record_fail "B02 合法 ping" "$(tail -3 /tmp/pr23/phase9-ping1.txt | tr '\n' ' ')"
fi

echo "[phase9] 4/5 C. §33 登录限速在公网上同样生效"
BACKUP_REMOTE_PASSWORD="$ACCOUNT_PASSWORD" ./build/backupctl remote register \
  --user "$USER_NAME" --host "$PUBLIC_HOST" --port "$PUBLIC_PORT" \
  --expected-server-id "$SERVER_ID" > /tmp/pr23/phase9-register.txt 2>&1 || true
for _ in 1 2 3 4 5 6; do
  BACKUP_REMOTE_PASSWORD="$BAD_PASSWORD" ./build/backupctl remote login \
    --user "$USER_NAME" --host "$PUBLIC_HOST" --port "$PUBLIC_PORT" \
    --expected-server-id "$SERVER_ID" > /dev/null 2>&1 || true
done
if BACKUP_REMOTE_PASSWORD="$ACCOUNT_PASSWORD" ./build/backupctl remote login \
     --user "$USER_NAME" --host "$PUBLIC_HOST" --port "$PUBLIC_PORT" \
     --expected-server-id "$SERVER_ID" > /tmp/pr23/phase9-login.txt 2>&1; then
  record_fail "C01 连续错口令之后正确口令应被限速拒绝" "竟然登录成功了（默认阈值 5／窗口 60s）"
else
  record_pass "C01 公网上连续错口令之后正确口令也被拒（§33 生效）"
fi

echo "[phase9] 5/5 D. 收尾：没崩、没重启、仍然可用"
PID_AFTER="$(ssh aliyun-ecs 'cat /home/ubuntu/backup-project-server/state/server.pid' 2>/dev/null)"
if [ "$PID_BEFORE" = "$PID_AFTER" ] && [ -n "$PID_AFTER" ]; then
  record_pass "D01 服务端 PID 未变（$PID_BEFORE）：没崩也没被重启"
else
  record_fail "D01 服务端 PID" "$PID_BEFORE -> $PID_AFTER"
fi
if ./build/backupctl remote ping --host "$PUBLIC_HOST" --port "$PUBLIC_PORT" \
     --expected-server-id "$SERVER_ID" > /tmp/pr23/phase9-ping2.txt 2>&1; then
  record_pass "D02 全部对抗之后合法 ping 仍然成功"
else
  record_fail "D02 收尾 ping" "$(tail -3 /tmp/pr23/phase9-ping2.txt | tr '\n' ' ')"
fi

echo "[phase9] passed=$PASS failed=$FAIL"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
