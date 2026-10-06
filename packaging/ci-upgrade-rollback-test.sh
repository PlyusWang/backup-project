#!/usr/bin/env bash
# packaging/ci-upgrade-rollback-test.sh —— 真实"升级失败 → 自动回滚"验收。
#
#   sudo bash packaging/ci-upgrade-rollback-test.sh <release 目录> [client 制品目录]
#
# 必须在**有真 systemd 的机器**上以 root 跑（CI 里的 systemd-acceptance job）。
# 它证明的不是"把版本号改成 999 再装回去"（那只是重装），而是：
#
#   新版本自己起不来 → 升级命令返回失败 → 自动恢复升级前的可运行版本 →
#   服务重新可用，且用户状态（私钥 / secret / 数据库 / 数据 / 配置）一个字节没变。
#
# 失败来源是**确定性的**：坏包只把服务端二进制换成一个立刻 exit 7 的 stub，
# 因此失败一定来自"新版本的运行时载荷"，不是测试环境的随机故障。
set -Eeuo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/ci-lib.sh"

REL="${1:?用法: ci-upgrade-rollback-test.sh <release 目录> [client 制品目录]}"
REL="$(cd "$REL" && pwd)"
CLIENT_REL="${2:-}"
if [ -n "$CLIENT_REL" ]; then CLIENT_REL="$(cd "$CLIENT_REL" && pwd)"; fi

REAL_DEB="$(ls "$REL"/backup-project-server_*.deb)"
INSTANCE=/var/lib/backup-project-server
CONF=/etc/backup-project-server/server.conf
BIN=/usr/lib/backup-project-server/bin/backup-server
CACHE=/var/cache/backup-project-server
WORK=/tmp/rollback-test
SENTINEL="$INSTANCE/data/ci-rollback-sentinel"

rm -rf "$WORK"; mkdir -p "$WORK"

log() { printf '  ----  %s\n' "$*"; }
hash_of() { if [ -e "$1" ]; then sha256sum "$1" | cut -d' ' -f1; else printf 'none'; fi; }
service_state() { systemctl is-active backup-project-server 2>/dev/null || true; }
port_listening() { ss -ltn 2>/dev/null | grep -q '127.0.0.1:18765'; }

wait_active() {
  local i state=""
  for i in $(seq 1 40); do
    state="$(service_state)"
    [ "$state" = "active" ] && return 0
    sleep 1
  done
  printf '  最后状态：%s\n' "$state" >&2
  return 1
}

# 用户状态的五个指纹：传输身份私钥、token secret、配置、元数据库、data 目录内容。
state_fingerprint() {
  printf 'key=%s\nsecret=%s\nconf=%s\ndb=%s\ndata=%s\n' \
    "$(hash_of "$INSTANCE/state/transport.key")" \
    "$(hash_of /etc/backup-project-server/secrets.env)" \
    "$(hash_of "$CONF")" \
    "$(hash_of "$INSTANCE/state/metadata.sqlite3")" \
    "$(hash_of "$SENTINEL")"
}
fp_field() { printf '%s\n' "$FP_A" | sed -n "s/^$1=//p"; }

# 造一个"载荷不同 / 版本号更高"的包：$4=yes 时把服务端二进制换成 exit 7 的 stub。
mk_deb() {  # $1=源 deb $2=输出 deb $3=版本 $4=是否注入启动失败
  local src="$1" out="$2" ver="$3" broken="$4"
  local root="$WORK/pkg"
  rm -rf "$root"; mkdir -p "$root"
  dpkg-deb -R "$src" "$root"
  sed -i "s/^Version: .*/Version: $ver/" "$root/DEBIAN/control"
  if [ "$broken" = "yes" ]; then
    cat > "$root/usr/lib/backup-project-server/bin/backup-server" <<'STUB'
#!/bin/sh
echo "backup-server: injected startup failure (rollback test payload)" >&2
exit 7
STUB
    chmod 0755 "$root/usr/lib/backup-project-server/bin/backup-server"
  fi
  ( cd "$root" && find . -type f ! -path './DEBIAN/*' -printf '%P\n' | LC_ALL=C sort | xargs -r md5sum > DEBIAN/md5sums )
  rm -f "$out"
  dpkg-deb --root-owner-group -Zxz --build "$root" "$out" > /dev/null
  rm -rf "$root"
}

cleanup_all() {
  dpkg --purge backup-project-server > /dev/null 2>&1 || true
  dpkg --purge --force-all backup-project-server > /dev/null 2>&1 || true
  systemctl stop backup-project-server > /dev/null 2>&1 || true
  systemctl reset-failed backup-project-server > /dev/null 2>&1 || true
  rm -rf "$INSTANCE" /etc/backup-project-server "$CACHE"
  userdel backup-project 2>/dev/null || true
  groupdel backup-project 2>/dev/null || true
}

# ================================================================ 1. 安装版本 A
ci_section "1. 安装版本 A（真实发行包）并确认服务可用"
apt-get update -qq
expect_ok "apt-get install ./<发行包>" apt-get install -y -qq --no-install-recommends "$REAL_DEB"
expect_ok "服务进入 active" wait_active
expect_ok "回环端口 127.0.0.1:18765 在监听" port_listening
install -o backup-project -g backup-project -m 0640 /dev/null "$SENTINEL"
printf 'user data must survive an automatic rollback\n' > "$SENTINEL"

VERSION_A="$(dpkg-query -W -f '${Version}' backup-project-server)"
EXEC_A="$(hash_of "$BIN")"
FP_A="$(state_fingerprint)"
log "版本 A = $VERSION_A"
log "服务端二进制 sha256 = $EXEC_A"
printf '%s\n' "$FP_A" | sed 's/^/  A /'

# ================================================================ 2. 成功升级
ci_section "2. 成功升级（载荷相同、版本号更高）必须提交并清理回滚材料"
UPGRADE_DEB="$WORK/upgrade-ok.deb"
VERSION_UP="${VERSION_A}+upgradetest1"
mk_deb "$REAL_DEB" "$UPGRADE_DEB" "$VERSION_UP" no
expect_ok "版本号排序：$VERSION_UP > $VERSION_A" dpkg --compare-versions "$VERSION_UP" gt "$VERSION_A"
expect_ok "dpkg -i <升级包>" dpkg -i "$UPGRADE_DEB"
expect_ok "升级后服务 active" wait_active
expect_eq "升级后用户状态指纹不变" "$FP_A" "$(state_fingerprint)"
if [ -e "$CACHE/rollback/manifest" ]; then ci_fail "升级成功后回滚材料还在（应该被提交清理）"; else ci_pass "升级提交后回滚材料已清理"; fi

# ================================================================ 3. 回到 A
ci_section "3. 装回 A：为失败升级准备一个确实在跑的旧版本"
expect_ok "dpkg -i <发行包>" dpkg -i "$REAL_DEB"
expect_ok "服务 active" wait_active
expect_eq "二进制回到 A" "$EXEC_A" "$(hash_of "$BIN")"
expect_eq "用户状态指纹不变" "$FP_A" "$(state_fingerprint)"

# ================================================================ 4. 失败升级
ci_section "4. 故意坏掉的版本 B：升级必须失败，并且必须自动回滚"
BROKEN_DEB="$WORK/upgrade-broken.deb"
VERSION_B="${VERSION_A}+rollbacktest1"
mk_deb "$REAL_DEB" "$BROKEN_DEB" "$VERSION_B" yes
expect_ok "版本号排序：$VERSION_B > $VERSION_A" dpkg --compare-versions "$VERSION_B" gt "$VERSION_A"
log "坏包里的 backup-server = 立刻 exit 7 的 stub（确定性失败来源）"

set +e
UPGRADE_OUT="$(dpkg -i "$BROKEN_DEB" 2>&1)"
UPGRADE_CODE=$?
set -e
printf '%s\n' "$UPGRADE_OUT" | sed 's/^/  | /'
printf '%s\n' "$UPGRADE_OUT" > "$WORK/broken-upgrade.log"

if [ "$UPGRADE_CODE" -ne 0 ]; then ci_pass "升级命令返回失败（exit $UPGRADE_CODE）"; else ci_fail "升级命令居然成功了（exit 0）：失败升级被伪装成成功"; fi
expect_contains "postinst 明确报告升级失败" "$WORK/broken-upgrade.log" "升级失败"
expect_contains "postinst 报告发生了自动回滚" "$WORK/broken-upgrade.log" "ROLLBACK OK"
expect_contains "postinst 告诉管理员怎么让 dpkg 元数据一致" "$WORK/broken-upgrade.log" "--reinstall"
expect_eq "服务端二进制已回滚成 A（sha256）" "$EXEC_A" "$(hash_of "$BIN")"
expect_ok "回滚后服务 active" wait_active
expect_ok "回滚后回环端口在监听" port_listening
expect_file "回滚标记（rolled-back）存在" "$CACHE/rollback/rolled-back"

expect_eq "transport.key 未变" "$(fp_field key)" "$(hash_of "$INSTANCE/state/transport.key")"
expect_eq "secrets.env 未变" "$(fp_field secret)" "$(hash_of /etc/backup-project-server/secrets.env)"
expect_eq "server.conf 未变（conffile 没被回滚覆盖）" "$(fp_field conf)" "$(hash_of "$CONF")"
expect_eq "元数据库未变" "$(fp_field db)" "$(hash_of "$INSTANCE/state/metadata.sqlite3")"
expect_eq "data 目录内容未变" "$(fp_field data)" "$(hash_of "$SENTINEL")"
expect_eq "用户状态指纹整体未变" "$FP_A" "$(state_fingerprint)"

# 诚实记录 Debian 的边界：载荷回滚了，dpkg 元数据仍是新版本（half-configured）。
DB_VERSION="$(dpkg-query -W -f '${Version}' backup-project-server)"
dpkg-query -W -f '${Status}\n' backup-project-server > "$WORK/dpkg-status.txt"
log "dpkg 版本 = $DB_VERSION  状态 = $(cat "$WORK/dpkg-status.txt")"
expect_eq "dpkg 元数据仍是失败的新版本（Debian 的 maintainer script 没有事务回滚）" "$VERSION_B" "$DB_VERSION"
expect_contains "dpkg 状态是 half-configured（明确地没成功）" "$WORK/dpkg-status.txt" "half-configured"
log "失败升级证据：failure injected = exit 7 stub / install exit = $UPGRADE_CODE / rollback = OK / restored = $VERSION_A / restored sha256 = $EXEC_A"

# ================================================================ 5. 回滚后仍然可用
ci_section "5. 回滚之后本机服务端仍然真的可用（回环 ping，真实 BPSEC1 握手）"
if [ -n "$CLIENT_REL" ]; then
  CLIENT_DEB="$(ls "$CLIENT_REL"/backup-project-client_*.deb)"
  expect_ok "安装客户端制品（ping 用）" apt-get install -y -qq --no-install-recommends "$CLIENT_DEB"
  PIN="$(backup-server-keygen --show --key-file "$INSTANCE/state/transport.key" | sed -n 's/.*--server-key //p' | sed -n '1p')"
  if [ -n "$PIN" ]; then ci_pass "拿到服务器身份指纹（不打印内容）"; else ci_fail "拿不到指纹"; fi
  expect_ok "回滚后 backupctl remote ping" backupctl remote ping --host 127.0.0.1 --port 18765 --server-key "$PIN"
  expect_ok "ping 可重复" backupctl remote ping --host 127.0.0.1 --port 18765 --server-key "$PIN"
else
  echo "  NOTE  没有给客户端制品目录，跳过回环 ping（回滚后的端口监听已经单独断言）"
fi

# ================================================================ 6. 首次安装失败
ci_section "6. 首次安装就失败：必须明确失败，且不许重新生成 key / secret"
cleanup_all
FIRST_BROKEN="$WORK/first-install-broken.deb"
VERSION_C="${VERSION_A}+rollbacktest2"
mk_deb "$REAL_DEB" "$FIRST_BROKEN" "$VERSION_C" yes
set +e
FIRST_OUT="$(dpkg -i "$FIRST_BROKEN" 2>&1)"
FIRST_CODE=$?
set -e
printf '%s\n' "$FIRST_OUT" | sed 's/^/  | /'
printf '%s\n' "$FIRST_OUT" > "$WORK/first-install.log"
if [ "$FIRST_CODE" -ne 0 ]; then ci_pass "首次安装失败时 dpkg -i 返回非零（exit $FIRST_CODE）"; else ci_fail "首次安装失败却返回 0"; fi
expect_contains "明确说明这是首次安装、没有旧版本可回滚" "$WORK/first-install.log" "首次安装"
expect_file "失败之后 secrets.env 仍然被保留" /etc/backup-project-server/secrets.env
expect_file "失败之后 transport.key 仍然被保留" "$INSTANCE/state/transport.key"
KEY_AFTER_FAILED="$(hash_of "$INSTANCE/state/transport.key")"
SECRET_AFTER_FAILED="$(hash_of /etc/backup-project-server/secrets.env)"
if [ "$(service_state)" = "active" ]; then ci_fail "坏版本居然把服务起起来了"; else ci_pass "坏版本没有留下 active 的服务（state=$(service_state)）"; fi

expect_ok "装回正常版本 A" dpkg -i "$REAL_DEB"
expect_ok "服务 active" wait_active
expect_eq "重装复用第一次生成的 transport.key（没有重新生成）" "$KEY_AFTER_FAILED" "$(hash_of "$INSTANCE/state/transport.key")"
expect_eq "重装复用第一次生成的 secret（没有重新生成）" "$SECRET_AFTER_FAILED" "$(hash_of /etc/backup-project-server/secrets.env)"

# ================================================================ 7. 收尾
ci_section "7. 收尾：remove / purge 都不删数据，且不留下回滚材料"
KEY_FINAL="$(hash_of "$INSTANCE/state/transport.key")"
expect_ok "apt-get remove" apt-get remove -y -qq backup-project-server
expect_file "remove 之后数据还在" "$INSTANCE/data"
expect_file "remove 之后私钥还在" "$INSTANCE/state/transport.key"
expect_ok "apt-get purge" apt-get purge -y -qq backup-project-server
expect_file "purge 之后数据还在" "$INSTANCE/data"
expect_eq "purge 之后私钥内容未变" "$KEY_FINAL" "$(hash_of "$INSTANCE/state/transport.key")"
if [ -e "$CACHE" ]; then ci_fail "purge 之后回滚材料目录还在（$CACHE）"; else ci_pass "purge 之后回滚材料目录已清理"; fi
log "清理测试环境留下的系统状态"
cleanup_all

ci_finish "upgrade-rollback"
