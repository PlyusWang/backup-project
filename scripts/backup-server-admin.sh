#!/usr/bin/env bash
#
# backup-server-admin.sh —— 备份服务器本机管理菜单（只在 ECS 本机使用）。
#
# 它只做四件事：清屏、显示菜单、读选择、调用同目录下的 backup-server-admin。
#
# 真正危险的数据操作**一个都不在这里做**：没有 SQL，没有 rm，没有直接改目录。
# shell 只负责把用户的选择翻成一次子命令调用，所以"删除"永远只有一份实现
# （C++ 侧的 RemoteMaintenance，与服务端 DELETE / DELETE_ACCOUNT 共用）。
# 这条边界是刻意的：把 rm 与 SQL 写进 shell，等于给数据留了第二条不受保护的
# 删除路径。
#
# 用法（先 SSH 登录到 ECS，再在本机执行）：
#
#   ssh aliyun-ecs
#   cd ~/backup-project-server
#   ./bin/backup-server-admin.sh
#
# 环境变量（都有默认值，部署脚本不写死路径）：
#
#   BACKUP_SERVER_ROOT   数据目录，默认 $HOME/backup-project-server/data
#   BACKUP_SERVER_DB     元数据库，默认 $BACKUP_SERVER_ROOT/metadata.sqlite3
#   BACKUP_SERVER_ADMIN  管理工具，默认与本脚本同目录的 backup-server-admin
#
# 本脚本不读、不打印 token secret：管理工具不需要它。

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ADMIN_BIN="${BACKUP_SERVER_ADMIN:-$SCRIPT_DIR/backup-server-admin}"
ROOT_DIR="${BACKUP_SERVER_ROOT:-$HOME/backup-project-server/data}"
DB_PATH="${BACKUP_SERVER_DB:-$ROOT_DIR/metadata.sqlite3}"

if [ ! -x "$ADMIN_BIN" ]; then
  echo "找不到管理工具：$ADMIN_BIN" >&2
  echo "先构建：make server（产物在 build/backup-server-admin）" >&2
  exit 1
fi
if [ ! -d "$ROOT_DIR" ]; then
  echo "数据目录不存在：$ROOT_DIR" >&2
  echo "用 BACKUP_SERVER_ROOT 指定 backup-server 的 --root。" >&2
  exit 1
fi

clear_screen() {
  if command -v clear >/dev/null 2>&1; then
    clear
  else
    printf '\033[H\033[2J'
  fi
}

admin() {
  "$ADMIN_BIN" --root "$ROOT_DIR" --db "$DB_PATH" "$@"
}

pause() {
  printf '\n按回车返回...'
  read -r _ || true
}

banner() {
  clear_screen
  printf '========================================\n'
  printf '      Backup Project Server Admin\n'
  printf '========================================\n\n'
  admin status
  printf '\n'
}

# ---- 用户管理 ----

user_menu() {
  while true; do
    banner
    printf '用户管理\n'
    printf '  1. 用户列表\n'
    printf '  2. 查看用户详情\n'
    printf '  3. 删除用户及其全部备份（危险）\n'
    printf '  0. 返回\n\n'
    printf '请选择：'
    read -r choice || return 0
    case "$choice" in
      1)
        clear_screen
        admin list-users
        pause
        ;;
      2)
        printf '用户 id 或用户名：'
        read -r selector || return 0
        clear_screen
        admin show-user "$selector"
        pause
        ;;
      3)
        printf '要删除的用户 id 或用户名：'
        read -r selector || return 0
        [ -n "$selector" ] || continue
        clear_screen
        admin show-user "$selector" || { pause; continue; }
        printf '\n'
        printf '警告：这会永久删除该账户以及它的全部云端备份，且无法撤销。\n'
        printf '请输入 DELETE <用户名> 以确认（直接回车取消）：'
        read -r confirmation || return 0
        [ -n "$confirmation" ] || continue
        admin delete-user "$selector" --confirm "$confirmation"
        pause
        ;;
      0) return 0 ;;
      *) ;;
    esac
  done
}

# ---- 备份文件管理 ----

snapshot_menu() {
  while true; do
    banner
    printf '备份文件管理\n'
    printf '  1. 列出某个用户的备份\n'
    printf '  2. 查看备份详情（含 SHA-256）\n'
    printf '  3. 删除单个备份（危险）\n'
    printf '  0. 返回\n\n'
    printf '请选择：'
    read -r choice || return 0
    case "$choice" in
      1)
        printf '用户 id 或用户名：'
        read -r selector || return 0
        clear_screen
        admin list-snapshots "$selector"
        pause
        ;;
      2)
        printf '快照 id：'
        read -r snapshot_id || return 0
        clear_screen
        admin show-snapshot "$snapshot_id"
        pause
        ;;
      3)
        printf '用户 id 或用户名：'
        read -r selector || return 0
        printf '快照 id：'
        read -r snapshot_id || return 0
        [ -n "$selector" ] && [ -n "$snapshot_id" ] || continue
        clear_screen
        admin show-snapshot "$snapshot_id" || { pause; continue; }
        printf '\n'
        printf '警告：这会永久删除这一份云端备份，且无法撤销。\n'
        printf '请再次输入快照 id 以确认（直接回车取消）：'
        read -r confirmation || return 0
        [ -n "$confirmation" ] || continue
        admin delete-snapshot "$snapshot_id" --user "$selector" \
          --confirm "$confirmation"
        pause
        ;;
      0) return 0 ;;
      *) ;;
    esac
  done
}

# ---- 主循环 ----

while true; do
  banner
  printf '  1. 用户管理\n'
  printf '  2. 备份文件管理\n'
  printf '  3. 存储概览\n'
  printf '  4. 刷新服务状态\n'
  printf '  0. 退出\n\n'
  printf '请选择：'
  read -r choice || break
  case "$choice" in
    1) user_menu ;;
    2) snapshot_menu ;;
    3)
      clear_screen
      admin overview
      pause
      ;;
    4) ;;
    0) break ;;
    *) ;;
  esac
done

clear_screen
echo "已退出管理工具。"
