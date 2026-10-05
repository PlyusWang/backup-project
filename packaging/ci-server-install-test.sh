#!/usr/bin/env bash
# packaging/ci-server-install-test.sh —— 服务端两个制品 + 首次运行 + 升级 + 卸载。
#
#   bash packaging/ci-server-install-test.sh <release 目录>
#
# 容器里没有 systemd（PID 1 不是 systemd），所以这里：
#   * 用 systemd-analyze verify 校验 unit 语法（不等于"能启动"，只证明 unit 是合法的）；
#   * 服务本身**手工启动**（走包装出来的启动器 + server.conf），并做真实的
#     回环 ping —— 装完能用这件事在容器里必须证明，不能靠"真机上应该没问题"。
# 真机 systemd 的启动/重启/升级由 CI 里的 host runner（有 sudo）单独验收。
set -Eeuo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/ci-lib.sh"

REL="${1:?用法: ci-server-install-test.sh <release 目录>}"
DEB="$(ls "$REL"/backup-project-server_*.deb)"
TARBALL="$(ls "$REL"/backup-project-server-*-linux-x86_64.tar.xz)"
INSTANCE=/var/lib/backup-project-server
CONF=/etc/backup-project-server/server.conf

ci_section "1. .deb 安装（容器里没有 systemd，postinst 必须照样成功）"
expect_ok "dpkg -i $(basename "$DEB")" dpkg -i "$DEB"
expect_ok "系统用户 backup-project 已创建" getent passwd backup-project
expect_eq "backup-project 的 shell 不是交互式" "/usr/sbin/nologin" "$(getent passwd backup-project | cut -d: -f7)"
expect_file "数据目录" "$INSTANCE/data"
expect_file "状态目录" "$INSTANCE/state"
expect_mode "数据目录权限 0750" "$INSTANCE/data" "750"
expect_file "secrets.env（随机生成）" /etc/backup-project-server/secrets.env
expect_mode "secrets.env 权限 0640" /etc/backup-project-server/secrets.env "640"
secret_len="$(sed -n 's/^BACKUP_TOKEN_SECRET=//p' /etc/backup-project-server/secrets.env | tr -d '\n' | wc -c)"
expect_eq "BACKUP_TOKEN_SECRET 长度 64" "64" "$secret_len"
expect_file "传输身份私钥（本机生成）" "$INSTANCE/state/transport.key"
expect_mode "transport.key 权限 0600" "$INSTANCE/state/transport.key" "600"
expect_eq "transport.key 是 32 字节" "32" "$(stat -c %s "$INSTANCE/state/transport.key")"
expect_file "配置文件" "$CONF"
expect_eq "默认只监听回环" "127.0.0.1" "$(sed -n 's/^bind = //p' "$CONF")"

ci_section "2. 命令入口齐全（含管理员菜单）"
for cmd in backup-server backup-project-server backup-server-admin backup-server-admin-menu \
           backup-server-keygen backup-cert-tool backup-server-purge-data; do
  expect_file "/usr/bin/$cmd" "/usr/bin/$cmd"
done
expect_ok "backup-server --help" backup-server --help
expect_ok "backup-cert-tool --help" backup-cert-tool --help
expect_ok "启动器 --check-config" backup-project-server --check-config --config "$CONF"
expect_ok "keygen --show 能打印指纹" backup-server-keygen --show --key-file "$INSTANCE/state/transport.key"
# fail closed：还没有数据库时，管理菜单必须明确失败，而不是造一个空库。
expect_fail "管理菜单在库不存在时 fail closed" backup-server-admin list-users
expect_file "管理菜单没有偷偷造出空库" "$INSTANCE/state" 
if [ -e "$INSTANCE/state/metadata.sqlite3" ]; then ci_fail "管理工具造出了空数据库（这是不允许的）"; else ci_pass "没有造出空数据库"; fi

ci_section "3. systemd unit 语法（容器里 systemd 不是 PID 1，不能靠 systemctl 判定）"
if command -v systemd-analyze >/dev/null 2>&1; then
  expect_ok "systemd-analyze verify" systemd-analyze verify /lib/systemd/system/backup-project-server.service
else
  echo "  NOTE  systemd-analyze 不可用，跳过（真机 job 里会做）"
fi

ci_section "4. 装完真的能服务（手工启动 + 回环 ping）"
PIN="$(backup-server-keygen --show --key-file "$INSTANCE/state/transport.key" | sed -n 's/.*--server-key //p' | head -1)"
if [ -n "$PIN" ]; then ci_pass "拿到服务器身份指纹（不打印内容）"; else ci_fail "拿不到指纹"; fi
runuser -u backup-project -- /usr/lib/backup-project-server/bin/launch-server.sh --config "$CONF" \
  > /tmp/server-run.log 2>&1 &
SERVER_PID=$!
for i in $(seq 1 40); do
  ss -ltn 2>/dev/null | grep -q '127.0.0.1:18765' && break
  sleep 0.5
done
if ss -ltn 2>/dev/null | grep -q '127.0.0.1:18765'; then ci_pass "服务端在 127.0.0.1:18765 监听"; else ci_fail "服务端没有监听"; tail -20 /tmp/server-run.log >&2; fi
if ! ss -ltn 2>/dev/null | grep -q '0.0.0.0:18765'; then ci_pass "没有监听 0.0.0.0（默认不是公网）"; else ci_fail "竟然监听了 0.0.0.0"; fi
expect_ok "客户端回环 ping（真实 BPSEC1 握手）" backupctl remote ping --host 127.0.0.1 --port 18765 --server-key "$PIN"
expect_ok "ping 能在干净环境里重复一次" backupctl remote ping --host 127.0.0.1 --port 18765 --server-key "$PIN"

ci_section "5. 升级/重装：状态必须一个字都不变"
BEFORE_DB="$(sha256sum "$INSTANCE/state/metadata.sqlite3" 2>/dev/null | cut -d' ' -f1 || echo none)"
BEFORE_KEY="$(sha256sum "$INSTANCE/state/transport.key" | cut -d' ' -f1)"
BEFORE_SECRET="$(sha256sum /etc/backup-project-server/secrets.env | cut -d' ' -f1)"
BEFORE_CONF_MODE="$(stat -c %a "$CONF")"
expect_ok "同版本重装（模拟升级路径）" dpkg -i "$DEB"
expect_eq "transport.key 未变化" "$BEFORE_KEY" "$(sha256sum "$INSTANCE/state/transport.key" | cut -d' ' -f1)"
expect_eq "secrets.env 未变化" "$BEFORE_SECRET" "$(sha256sum /etc/backup-project-server/secrets.env | cut -d' ' -f1)"
expect_eq "配置文件权限未变化（conffile 没被覆盖）" "$BEFORE_CONF_MODE" "$(stat -c %a "$CONF")"
if [ -e "$INSTANCE/state/metadata.sqlite3" ]; then
  expect_eq "元数据库未变化" "$BEFORE_DB" "$(sha256sum "$INSTANCE/state/metadata.sqlite3" | cut -d' ' -f1)"
else
  ci_pass "元数据库本来就不存在（服务器还没有用户），重装没有造出第二个库"
fi
if pgrep -f 'backup-server --bind' > /dev/null; then ci_pass "重装没有把正在跑的服务打挂"; else
  ci_fail "重装过程中服务进程消失了"; tail -10 /tmp/server-run.log >&2; fi

ci_section "6. 卸载：数据必须留下"
kill "$SERVER_PID" 2>/dev/null || true
sleep 1
expect_ok "dpkg -r backup-project-server" dpkg -r backup-project-server
if [ -e /usr/bin/backup-server ]; then ci_fail "程序文件仍在"; else ci_pass "程序文件已删除"; fi
expect_file "卸载后数据目录仍在" "$INSTANCE/data"
expect_file "卸载后传输身份私钥仍在" "$INSTANCE/state/transport.key"
expect_file "卸载后 secrets.env 仍在" /etc/backup-project-server/secrets.env
expect_eq "卸载后 transport.key 内容未变" "$BEFORE_KEY" "$(sha256sum "$INSTANCE/state/transport.key" | cut -d' ' -f1)"

ci_section "7. purge 包也不许删数据"
expect_ok "dpkg --purge backup-project-server" dpkg --purge backup-project-server
expect_file "purge 之后数据目录仍在" "$INSTANCE/data"
expect_file "purge 之后私钥仍在" "$INSTANCE/state/transport.key"

ci_section "8. portable tar.xz：--prefix（含空格）+ --no-systemd"
PREFIX="/tmp/server prefix/opt"
rm -rf "/tmp/server prefix"; mkdir -p "/tmp/server prefix"
expect_ok "解包 tar.xz" tar -xf "$TARBALL" -C "/tmp/server prefix"
TOP="$(ls -d "/tmp/server prefix"/*/ | head -1)"
expect_file "install.sh" "${TOP}install.sh"
expect_file "MANIFEST.sha256" "${TOP}MANIFEST.sha256"
expect_ok "install.sh --prefix（含空格）--no-systemd" "${TOP}install.sh" --prefix "$PREFIX" --no-systemd --port 18999
expect_file "portable backup-server" "$PREFIX/bin/backup-server"
expect_file "portable 管理员菜单" "$PREFIX/bin/backup-server-admin.sh"
expect_file "portable 配置" "$PREFIX/etc/server.conf"
expect_mode "portable transport.key 0600" "$PREFIX/var/state/transport.key" "600"
expect_ok "portable --check-config" "$PREFIX/bin/launch-server.sh" --check-config --config "$PREFIX/etc/server.conf"
PIN2="$("$PREFIX/bin/backup-server-keygen" --show --key-file "$PREFIX/var/state/transport.key" | sed -n 's/.*--server-key //p' | head -1)"
"$PREFIX/bin/launch-server.sh" --config "$PREFIX/etc/server.conf" > /tmp/server-portable.log 2>&1 &
PPORT_PID=$!
for i in $(seq 1 40); do ss -ltn 2>/dev/null | grep -q '127.0.0.1:18999' && break; sleep 0.5; done
if ss -ltn 2>/dev/null | grep -q '127.0.0.1:18999'; then ci_pass "portable 服务端在 127.0.0.1:18999 监听"; else ci_fail "portable 服务端没有监听"; tail -20 /tmp/server-portable.log >&2; fi
kill "$PPORT_PID" 2>/dev/null || true
sleep 1

ci_section "9. portable 卸载（默认保留数据）"
expect_ok "uninstall.sh --prefix" "${TOP}uninstall.sh" --prefix "$PREFIX"
if [ -e "$PREFIX/bin/backup-server" ]; then ci_fail "portable 程序文件仍在"; else ci_pass "portable 程序文件已删除"; fi
expect_file "portable 数据仍在" "$PREFIX/var/data"
expect_file "portable 私钥仍在" "$PREFIX/var/state/transport.key"

ci_section "10. purge-data 需要显式确认（在 deb 实例上）"
deb_bin=/usr/lib/backup-project-server/bin/purge-data.sh
if [ -x "$deb_bin" ]; then
  expect_ok "purge-data --dry-run 不删任何东西" "$deb_bin" --dry-run
  expect_fail "purge-data 没有确认短语时拒绝执行" bash -c "printf 'nope\n' | $deb_bin --config $CONF"
  expect_file "拒绝之后数据还在" "$INSTANCE/data"
fi

ci_finish "server-install"
