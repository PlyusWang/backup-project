#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
if [ -x "$HERE/../bin/backup-server" ]; then
  BIN="$HERE/../bin"
  DEFAULT_DATA="$HERE/../test-server-data"
else
  BIN="$HERE/../build"
  DEFAULT_DATA="$HERE/../tests/output/manual-test-server"
fi
DATA=""
if [ "$#" -gt 0 ]; then DATA="$1"; fi
if [ -z "$DATA" ]; then DATA="$DEFAULT_DATA"; fi
set +u
PORT="$BACKUP_TEST_PORT"
set -u
if [ -z "$PORT" ]; then PORT=18766; fi
mkdir -p "$DATA/root" "$DATA/logs"
if [ ! -f "$DATA/secrets.env" ]; then
  printf 'BACKUP_TOKEN_SECRET=%s\n' "$(head -c 32 /dev/urandom | sha256sum | cut -c1-64)" > "$DATA/secrets.env"
  chmod 600 "$DATA/secrets.env"
fi
if [ ! -f "$DATA/transport.key" ]; then
  "$BIN/backup-server-keygen" --output "$DATA/transport.key" > "$DATA/logs/keygen.log" 2>&1
fi
"$BIN/backup-server-keygen" --show --key-file "$DATA/transport.key" > "$DATA/transport-fingerprint.txt"
FP="$(awk '/指纹 \(SHA-256\)/ {print $NF}' "$DATA/transport-fingerprint.txt" | tail -1)"
if [ -z "$FP" ]; then
  echo "无法读取服务器身份指纹；请检查 $DATA/transport-fingerprint.txt" >&2
  exit 1
fi
cat <<EOF
[test-server] 地址: 127.0.0.1
[test-server] 端口: $PORT
[test-server] 数据目录: $DATA
[test-server] 指纹: sha256:$FP
[test-server] 指纹查询: $BIN/backup-server-keygen --show --key-file $DATA/transport.key
[test-server] 服务端日志: $DATA/logs/server.log
[test-server] 测试账号: 请在本机 GUI 注册；脚本不会创建账号或密码。
[test-server] 停止: Ctrl+C
EOF
exec "$BIN/backup-server" --bind 127.0.0.1 --port "$PORT" \
  --root "$DATA/root" --db "$DATA/root/metadata.db" \
  --secret-file "$DATA/secrets.env" --transport-key-file "$DATA/transport.key" \
  --log-file "$DATA/logs/server.log" --quiet
