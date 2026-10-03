#!/usr/bin/env bash
#
# remote_incremental_test.sh —— PR #21 远端增量链的测试入口。
#
# 两段：
#   A. 单元测试（tests/unit/remote_chain_test.cpp）：材料包 BPSNAP1 的打包 /
#      解包 / 篡改 / 不覆盖；远端链解析与 head 选择；SQLite schema 1 -> 2 的
#      迁移（保留旧数据、幂等、只读路径拒绝升级）与依赖感知删除。
#   B. 端到端（真实 backup-server + BPSEC1 加密传输 + 真实增量引擎）：
#      full -> delta -> delta -> restore（自动拉整条链）-> 冷缓存 bootstrap ->
#      依赖感知删除 -> 服务端 blob 被篡改时拒绝恢复 -> 无变化时不上传。
#
# 每个断言一行 PASS/FAIL；退出码 0 = 全通过。

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

PASS=0
FAIL=0
record_pass() { PASS=$((PASS + 1)); echo "  PASS $1"; }
record_fail() { FAIL=$((FAIL + 1)); echo "  FAIL $1${2:+ -- $2}"; }

TEST_ROOT="$(mktemp -d /tmp/remote-incremental-test-XXXXXX)"
SERVER_PID=""
cleanup() {
  if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill -TERM "$SERVER_PID" 2>/dev/null
    wait "$SERVER_PID" 2>/dev/null
  fi
  rm -rf "$TEST_ROOT"
}
trap cleanup EXIT

echo "[remote-incremental] 构建产品二进制"
if ! make -j4 all server >"$TEST_ROOT/build.log" 2>&1; then
  record_fail "产品构建" "$(tail -3 "$TEST_ROOT/build.log" | tr '\n' ' ')"
  echo "[remote-incremental] 失败项：$FAIL"
  exit 1
fi
record_pass "产品构建"

# ---- A. 单元测试 ----
echo "[remote-incremental] A. 单元测试（材料包 / 链解析 / 迁移 / 依赖删除）"

SQLITE_LIBRARY="$(ls -1 /usr/lib/x86_64-linux-gnu/libsqlite3.so \
  /usr/lib/x86_64-linux-gnu/libsqlite3.so.0 /usr/lib64/libsqlite3.so \
  /usr/lib/libsqlite3.so 2>/dev/null | head -1)"
SQLITE_INCLUDE_DIR="$(dirname "$(ls -1 /usr/include/sqlite3.h \
  "$ROOT_DIR"/third_party/sqlite/include/sqlite3.h 2>/dev/null | head -1)")"
if [ -z "$SQLITE_LIBRARY" ] || [ ! -f "$SQLITE_INCLUDE_DIR/sqlite3.h" ]; then
  record_fail "sqlite3 运行库/头文件" "找不到 libsqlite3.so 或 sqlite3.h"
else
  CORE_OBJECTS="$(find build/src -name '*.o' 2>/dev/null | sort | tr '\n' ' ')"
  # PR #21 红队审查轮：单元测试现在也起一个**真的**服务端（TCP 环回 + BPSEC1
  # 握手 + 真 SQLite），所以编译它还要链上服务端对象。三个 *main.o 是三个
  # 可执行文件的入口，必须排除（否则与测试自己的 main 冲突）。
  STORE_OBJECT="$(find build/server/server -name '*.o' \
    ! -name 'main.o' ! -name 'admin_main.o' ! -name 'keygen_main.o' \
    2>/dev/null | sort | tr '\n' ' ')"
  UNIT_BIN="$TEST_ROOT/remote_chain_test"
  if g++ -std=c++17 -Wall -Wextra -Wpedantic -Iinclude \
      -I"$SQLITE_INCLUDE_DIR" -Itests/unit \
      tests/unit/remote_chain_test.cpp $CORE_OBJECTS $STORE_OBJECT \
      "$SQLITE_LIBRARY" -pthread -o "$UNIT_BIN" \
      >"$TEST_ROOT/unit-build.log" 2>&1; then
    record_pass "A.remote_chain_test 编译"
    if [ -s "$TEST_ROOT/unit-build.log" ]; then
      record_fail "A.remote_chain_test 零警告" \
        "$(head -2 "$TEST_ROOT/unit-build.log" | tr '\n' ' ')"
    else
      record_pass "A.remote_chain_test 零警告"
    fi
    if timeout --signal=KILL 300 "$UNIT_BIN" >"$TEST_ROOT/unit.log" 2>&1; then
      record_pass "A.remote_chain_test 运行（$(tail -1 "$TEST_ROOT/unit.log")）"
    else
      record_fail "A.remote_chain_test 运行" \
        "$(grep -m3 'FAIL' "$TEST_ROOT/unit.log" | tr '\n' ' ')"
    fi
  else
    record_fail "A.remote_chain_test 编译" \
      "$(head -3 "$TEST_ROOT/unit-build.log" | tr '\n' ' ')"
  fi
fi

# ---- B. 端到端 ----
echo "[remote-incremental] B. 端到端（真实服务端 + BPSEC1 + 增量引擎）"

WORK="$TEST_ROOT/e2e"
mkdir -p "$WORK/data" "$WORK/state" "$WORK/logs" "$WORK/src" "$WORK/home/.config"
# 缓存与配置落在测试自己的 HOME 里，绝不碰真实 HOME。
export HOME="$WORK/home"
# 本地已验证缓存的位置（由服务端指纹与用户名决定）。B.4b 与 B.9 都要用它，
# 所以在这里一次性定下来，不要在脚本中间再定义一遍。
CACHE_ROOT="$HOME/.config/backup-project/backup-gui-modern/remote-cache"

head -c 32 /dev/urandom | sha256sum | cut -c1-64 >"$WORK/secret.value"
echo "BACKUP_TOKEN_SECRET=$(cat "$WORK/secret.value")" >"$WORK/secrets.env"
chmod 600 "$WORK/secrets.env"

if ./build/backup-server-keygen --output "$WORK/transport.key" >"$WORK/keygen.out" 2>&1; then
  record_pass "B.0 生成传输身份密钥"
else
  record_fail "B.0 生成传输身份密钥" "$(tail -1 "$WORK/keygen.out")"
fi
if [ "$(stat -c '%a' "$WORK/transport.key" 2>/dev/null)" = "600" ]; then
  record_pass "B.0 身份私钥权限 0600"
else
  record_fail "B.0 身份私钥权限 0600" "$(stat -c '%a' "$WORK/transport.key" 2>/dev/null)"
fi
export BACKUP_REMOTE_SERVER_KEY="$(grep -oE 'sha256:[0-9a-f]{64}' "$WORK/keygen.out" | head -1)"
if [ -n "$BACKUP_REMOTE_SERVER_KEY" ]; then
  record_pass "B.0 取得客户端 pin（指纹）"
else
  record_fail "B.0 取得客户端 pin（指纹）"
fi

# 缺 --transport-key-file 时服务端必须拒绝启动（没有明文模式）。
if timeout 30 ./build/backup-server --bind 127.0.0.1 --port 0 --root "$WORK/data" \
    --db "$WORK/state/nope.sqlite3" --secret-file "$WORK/secrets.env" \
    >"$WORK/no-key.log" 2>&1; then
  record_fail "B.0 缺传输密钥时服务端拒绝启动" "竟然启动了"
else
  if grep -q -- "--transport-key-file" "$WORK/no-key.log"; then
    record_pass "B.0 缺传输密钥时服务端拒绝启动"
  else
    record_fail "B.0 缺传输密钥时服务端拒绝启动" "报错里没有提到这个参数"
  fi
fi
# 没有 pin 的客户端必须拒绝连接（不做 TOFU）。
if env -u BACKUP_REMOTE_SERVER_KEY ./build/backupctl remote ping \
    --host 127.0.0.1 --port 1 >"$WORK/no-pin.log" 2>&1; then
  record_fail "B.0 没有 pin 的客户端拒绝连接" "竟然成功了"
else
  if grep -q -- "--server-key" "$WORK/no-pin.log"; then
    record_pass "B.0 没有 pin 的客户端拒绝连接"
  else
    record_fail "B.0 没有 pin 的客户端拒绝连接" "报错里没有提到 --server-key"
  fi
fi

PORT=0
for candidate in $(seq 22100 22140); do
  if ! ss -ltn 2>/dev/null | grep -q ":$candidate "; then
    PORT=$candidate
    break
  fi
done
if [ "$PORT" = "0" ]; then
  record_fail "B.1 找空闲端口"
  echo "[remote-incremental] 失败项：$FAIL"
  exit 1
fi

./build/backup-server --bind 127.0.0.1 --port "$PORT" --root "$WORK/data" \
  --db "$WORK/state/metadata.sqlite3" --secret-file "$WORK/secrets.env" \
  --transport-key-file "$WORK/transport.key" --pid-file "$WORK/state/server.pid" \
  --log-file "$WORK/logs/server.log" --quiet &
SERVER_PID=$!
ready=0
for _ in $(seq 1 50); do
  if ss -ltn 2>/dev/null | grep -q "127.0.0.1:$PORT "; then ready=1; break; fi
  sleep 0.1
done
if [ "$ready" = "1" ]; then
  record_pass "B.1 服务端监听 127.0.0.1:$PORT"
else
  record_fail "B.1 服务端监听" "$(tail -2 "$WORK/logs/server.log" | tr '\n' ' ')"
  echo "[remote-incremental] 失败项：$FAIL"
  exit 1
fi

export BACKUP_REMOTE_PASSWORD="pw-$(head -c 16 /dev/urandom | sha256sum | cut -c1-24)"
USER_NAME="pr21-inc-$$"
REMOTE=(--user "$USER_NAME" --host 127.0.0.1 --port "$PORT")
run_remote() { ./build/backupctl remote "$@" 2>&1; }

if run_remote register "${REMOTE[@]}" >"$WORK/register.log"; then
  record_pass "B.2 注册（经 BPSEC1 加密）"
else
  record_fail "B.2 注册" "$(tail -1 "$WORK/register.log")"
fi

# 初始树：一个 200 KiB 的随机文件 + 两个小文件，让"完整 vs 增量"的字节差异可见。
head -c 200000 /dev/urandom >"$WORK/src/big.bin"
printf 'A-v1\n' >"$WORK/src/A.txt"
printf 'C-v1\n' >"$WORK/src/C.txt"

run_remote backup "$WORK/src" --strategy full "${REMOTE[@]}" --name R0 >"$WORK/r0.log"
if grep -q "已上传完整快照" "$WORK/r0.log"; then
  record_pass "B.3 第一次远端备份是完整基线（full / gen 0）"
else
  record_fail "B.3 第一次远端备份是完整基线" "$(tail -2 "$WORK/r0.log" | tr '\n' ' ')"
fi
R0_SIZE="$(grep -oE '本次上传:  [0-9]+' "$WORK/r0.log" | grep -oE '[0-9]+' | head -1)"

# 修改：A 内容变化 / 添加 D。
printf 'A-v2-with-more-content\n' >"$WORK/src/A.txt"
printf 'D-new\n' >"$WORK/src/D.txt"
run_remote backup "$WORK/src" "${REMOTE[@]}" --name R1 >"$WORK/r1.log"
if grep -q "已上传增量快照" "$WORK/r1.log" && grep -q "代数:      1" "$WORK/r1.log"; then
  record_pass "B.4 第二次远端备份是增量（gen 1）"
else
  record_fail "B.4 第二次远端备份是增量" "$(tail -3 "$WORK/r1.log" | tr '\n' ' ')"
fi
R1_SIZE="$(grep -oE '本次上传:  [0-9]+' "$WORK/r1.log" | grep -oE '[0-9]+' | head -1)"
R0_ID="$(grep -oE '快照 ID:   [0-9a-f]{32}' "$WORK/r0.log" | grep -oE '[0-9a-f]{32}')"
if grep -q "$R0_ID" "$WORK/r1.log"; then
  record_pass "B.4 增量的父是 R0（服务端登记一致）"
else
  record_fail "B.4 增量的父是 R0" "$R0_ID"
fi

# 本地缓存必须真的被用上：续链时不该重新下载父快照的材料。
# （这条断言是必需的：少了它，"索引丢了但靠兜底路径重新解包"也会让用例通过，
#   而那条路径其实是把已经验证过的材料又重新下载了一遍。）
if grep -q "download" "$WORK/r1.log"; then
  record_fail "B.4b 续链命中本地缓存（没有重新下载父）" \
    "$(grep -m1 download "$WORK/r1.log")"
else
  record_pass "B.4b 续链命中本地缓存（没有重新下载父）"
fi
INDEX_FILE="$(find "$CACHE_ROOT" -name '.remote-index.tsv' 2>/dev/null | head -1)"
if [ -n "$INDEX_FILE" ] && grep -q "$R0_ID" "$INDEX_FILE" 2>/dev/null; then
  record_pass "B.4b 本地缓存索引记录了服务端快照 id"
else
  record_fail "B.4b 本地缓存索引" "找不到索引或里面没有 R0"
fi

# 再修改一次：C 变化、D 改名。
printf 'C-v2\n' >"$WORK/src/C.txt"
mv "$WORK/src/D.txt" "$WORK/src/D-renamed.txt"
run_remote backup "$WORK/src" "${REMOTE[@]}" --name R2 >"$WORK/r2.log"
if grep -q "已上传增量快照" "$WORK/r2.log" && grep -q "代数:      2" "$WORK/r2.log"; then
  record_pass "B.5 第三次远端备份是增量（gen 2）"
else
  record_fail "B.5 第三次远端备份是增量" "$(tail -3 "$WORK/r2.log" | tr '\n' ' ')"
fi
R2_ID="$(grep -oE '快照 ID:   [0-9a-f]{32}' "$WORK/r2.log" | grep -oE '[0-9a-f]{32}')"
R1_ID="$(grep -oE '快照 ID:   [0-9a-f]{32}' "$WORK/r1.log" | grep -oE '[0-9a-f]{32}')"

# 只传了增量：delta 的字节数必须远小于完整基线。
if [ -n "$R0_SIZE" ] && [ -n "$R1_SIZE" ] && [ "$R1_SIZE" -lt $((R0_SIZE / 5)) ]; then
  record_pass "B.6 只上传了增量（R1=$R1_SIZE 字节 vs R0=$R0_SIZE 字节）"
else
  record_fail "B.6 只上传了增量" "R0=$R0_SIZE R1=$R1_SIZE"
fi

# 无变化：必须不上传新快照。
run_remote backup "$WORK/src" "${REMOTE[@]}" --name R3 >"$WORK/r3.log"
if grep -q "没有变化" "$WORK/r3.log"; then
  record_pass "B.7 源目录无变化时不创建新快照"
else
  record_fail "B.7 源目录无变化时不创建新快照" "$(tail -2 "$WORK/r3.log" | tr '\n' ' ')"
fi

# 恢复 head：只指定 R2，自动解析整条链。
mkdir -p "$WORK/restore-head"
run_remote restore "$R2_ID" "$WORK/restore-head" "${REMOTE[@]}" >"$WORK/restore.log"
if grep -q "依赖链:    3 份快照（2 个增量）" "$WORK/restore.log"; then
  record_pass "B.8 恢复 head 时自动解析出 3 份快照的链"
else
  record_fail "B.8 恢复 head 时自动解析链" "$(tail -2 "$WORK/restore.log" | tr '\n' ' ')"
fi
if diff -r "$WORK/src" "$WORK/restore-head" >/dev/null 2>&1; then
  record_pass "B.8 恢复结果与源目录逐字节一致（diff -r）"
else
  record_fail "B.8 恢复结果与源目录不一致" "$(diff -r "$WORK/src" "$WORK/restore-head" 2>&1 | head -3 | tr '\n' ' ')"
fi

# 冷缓存 bootstrap：删掉整个缓存后恢复 R1，必须自动从服务端取回依赖。
rm -rf "$CACHE_ROOT"
mkdir -p "$WORK/expect-r1"
printf 'A-v2-with-more-content\n' >"$WORK/expect-r1/A.txt"
printf 'C-v1\n' >"$WORK/expect-r1/C.txt"
printf 'D-new\n' >"$WORK/expect-r1/D.txt"
cp "$WORK/src/big.bin" "$WORK/expect-r1/big.bin"
mkdir -p "$WORK/restore-cold"
run_remote restore "$R1_ID" "$WORK/restore-cold" "${REMOTE[@]}" >"$WORK/restore-cold.log"
if [ -d "$CACHE_ROOT" ] && grep -q "已从远端恢复" "$WORK/restore-cold.log"; then
  record_pass "B.9 冷缓存 bootstrap 恢复成功"
else
  record_fail "B.9 冷缓存 bootstrap" "$(tail -2 "$WORK/restore-cold.log" | tr '\n' ' ')"
fi
if diff -r "$WORK/expect-r1" "$WORK/restore-cold" >/dev/null 2>&1; then
  record_pass "B.9 冷缓存恢复出的树与 R1 时刻一致"
else
  record_fail "B.9 冷缓存恢复结果不一致" "$(diff -r "$WORK/expect-r1" "$WORK/restore-cold" 2>&1 | head -3 | tr '\n' ' ')"
fi

# 依赖感知删除：R0 有后代必须被拒绝，叶子可以删。
if run_remote delete "$R0_ID" "${REMOTE[@]}" >"$WORK/delete-r0.log" 2>&1; then
  record_fail "B.10 删除有后代的祖先被拒绝" "竟然成功了"
else
  if grep -qE "后代|依赖" "$WORK/delete-r0.log"; then
    record_pass "B.10 删除有后代的祖先被拒绝"
  else
    record_fail "B.10 删除有后代的祖先被拒绝" "$(tail -1 "$WORK/delete-r0.log")"
  fi
fi
if run_remote delete "$R2_ID" "${REMOTE[@]}" >"$WORK/delete-r2.log" 2>&1; then
  record_pass "B.10 删除叶子快照成功"
else
  record_fail "B.10 删除叶子快照" "$(tail -1 "$WORK/delete-r2.log")"
fi

# 服务端字节被篡改时，恢复必须失败（元数据不能替代归档自验证）。
#
# 注意要改的是**声明长度之内**的字节：在文件末尾追加一个字节是测不到的——
# 服务端只按元数据声明的长度发数据，多出来的尾巴根本不会上线。所以要
# 覆盖中间的一个字节，让"实际字节的 SHA-256"与元数据声明不符。
#
# 先把缓存清掉：B.9 已经把 R1 的材料验证着放进缓存了，命中缓存时根本不会
# 重新下载，也就测不到"下载到的字节被改过"。
rm -rf "$CACHE_ROOT"
R1BLOB="$WORK/data/users/1/$R1_ID.bak"
if [ -f "$R1BLOB" ]; then
  BLOB_SIZE="$(stat -c %s "$R1BLOB")"
  printf 'X' | dd of="$R1BLOB" bs=1 seek=$((BLOB_SIZE / 2)) conv=notrunc 2>/dev/null
  mkdir -p "$WORK/restore-tampered"
  if run_remote restore "$R1_ID" "$WORK/restore-tampered" "${REMOTE[@]}" >"$WORK/tampered.log" 2>&1; then
    record_fail "B.11 服务端 blob 被篡改时拒绝恢复" "竟然成功了"
  else
    record_pass "B.11 服务端 blob 被篡改时拒绝恢复"
  fi
  if [ -z "$(ls -A "$WORK/restore-tampered" 2>/dev/null)" ]; then
    record_pass "B.11 被拒绝时目标目录保持为空（原子发布）"
  else
    record_fail "B.11 被拒绝时目标目录应该为空" "$(ls -A "$WORK/restore-tampered" | head -3 | tr '\n' ' ')"
  fi
else
  record_fail "B.11 找不到 R1 的 blob" "$R1BLOB"
fi

# 元数据落库的形状：直接看**产品自己的输出**。
#
# 原来这里用 sqlite3 CLI 查库，但 VM 上并不一定装了 sqlite3（没装时整段被静默
# 跳过 —— 一条"永远不跑"的断言比没有断言更糟）。改成两条不依赖外部工具的检查：
#   * remote list 的 KIND / GEN / PARENT 列（用户与老师看得见的产品契约）；
#   * 服务端日志里发布的 kind/generation（服务端自己写下的事实）。
run_remote list "${REMOTE[@]}" >"$WORK/list.txt" 2>&1
# B.10 已经把叶子 R2 删掉了，所以这里应当只剩 R0 与 R1 两行——R2 的
# kind/generation 由下面那条服务端日志断言覆盖（日志是当时写下的，不会被删除）。
LIST_ROWS="$(grep -cE '^[0-9a-f]{32}' "$WORK/list.txt")"
if grep -qE "full +0 +- +R0" "$WORK/list.txt" && \
   grep -qE "incremental +1 +${R0_ID:0:12} +R1" "$WORK/list.txt" && \
   [ "$LIST_ROWS" = "2" ] && \
   ! grep -q "$R2_ID" "$WORK/list.txt"; then
  record_pass "B.12 remote list 显示 full/gen0/无父 与增量的父/代数，且被删的 R2 已消失"
else
  record_fail "B.12 remote list 的链关系列" "rows=$LIST_ROWS $(grep -E 'full|incremental' "$WORK/list.txt" | tr '\n' ' ')"
fi
if grep -q "kind=0 generation=0" "$WORK/logs/server.log" && \
   grep -q "kind=1 generation=1" "$WORK/logs/server.log" && \
   grep -q "kind=1 generation=2" "$WORK/logs/server.log"; then
  record_pass "B.12 服务端日志记录了 kind=0/1/1 与 generation=0/1/2"
else
  record_fail "B.12 服务端日志里的 kind/generation" "$(grep 'published' "$WORK/logs/server.log" | tail -3 | tr '\n' ' ')"
fi

kill -TERM "$SERVER_PID" 2>/dev/null
wait "$SERVER_PID" 2>/dev/null
SERVER_PID=""

echo
echo "[remote-incremental] PASS=$PASS FAIL=$FAIL"
if [ "$FAIL" -ne 0 ]; then
  exit 1
fi
echo "[remote-incremental] 全部通过"
exit 0
