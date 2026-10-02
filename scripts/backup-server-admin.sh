#!/usr/bin/env bash
#
# backup-server-admin.sh —— 备份服务器本机管理菜单（只在 ECS 本机使用）。
#
# 它只做四件事：清屏、显示菜单、读选择、调用同目录下的 backup-server-admin。
# 真正危险的数据操作**一个都不在这里做**：没有 SQL，没有 rm，没有直接改目录。
#
# ---- 实例身份（这一版的重点）----
#
# 部署布局（scripts/deploy_aliyun_server.sh 安装出来的样子）：
#
#   <server-root>/bin/backup-server-admin        管理工具
#   <server-root>/bin/backup-server-admin.sh     本脚本
#   <server-root>/data                           数据根
#   <server-root>/state/metadata.sqlite3         元数据库
#
# 脚本从**自己所在的目录**推出这个布局，因此天然与同一个部署里的 backup-server
# 指向同一个状态根。上一版硬编码成 "$HOME/backup-project-server/data/metadata.sqlite3"，
# 而服务端用的是 state/metadata.sqlite3 —— 路径分叉加上 SQLite 的"文件不存在就
# 新建"，让管理工具读到了一个**刚被自己创建出来的空库**，屏幕上显示"还没有任何
# 用户"，人工验收因此以为 ECS 上没有任何用户（其实用户都在正确的库里）。
#
# 现在：路径错了就明确失败（fail closed），绝不创建空库；而且每次运行都会先打印
# Host / Server root / Data root / Metadata DB / Service，让人一眼看出在看哪个实例。
#
# 用法（先 SSH 登录到 ECS，再在本机执行）：
#
#   ssh aliyun-ecs
#   cd ~/backup-project-server
#   ./bin/backup-server-admin.sh
#
# 环境变量（覆盖自动推导；都是可选）：
#
#   BACKUP_SERVER_ROOT   部署根，默认 = 本脚本所在目录的上一级；
#                        在源码树里运行时**必须**显式给出，否则 fail closed
#   BACKUP_SERVER_DATA   数据根，默认 = $BACKUP_SERVER_ROOT/data
#   BACKUP_SERVER_DB     元数据库，默认 = $BACKUP_SERVER_ROOT/state/metadata.sqlite3
#   BACKUP_SERVER_ADMIN  管理工具，默认 = 与本脚本同目录的 backup-server-admin
#
# 本脚本不读、不打印 token secret：管理工具不需要它。

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# 部署布局：管理工具与本脚本同在 bin/ 下，部署根就是它的上一级——这一条只对
# **部署后**的脚本成立，因为那个 bin/.. 就是被管理的那个实例。
if [ -x "$SCRIPT_DIR/backup-server-admin" ]; then
  ADMIN_BIN="${BACKUP_SERVER_ADMIN:-$SCRIPT_DIR/backup-server-admin}"
  SERVER_ROOT_DEFAULT="$(cd "$SCRIPT_DIR/.." && pwd)"
  SERVER_ROOT="${BACKUP_SERVER_ROOT:-$SERVER_ROOT_DEFAULT}"
else
  # 源码树里直接跑（<repo>/scripts/backup-server-admin.sh）：产物在 build/ 下，
  # 这里**推不出**任何真实实例。以前它猜 $HOME/backup-project-server——猜错了就会
  # 去读另一个实例（SQLite 还会顺手把不存在的文件建成空库），人工验收因此得出过
  # 完全错误的结论。现在 fail closed：要么用部署后的脚本，要么显式给出
  # BACKUP_SERVER_ROOT。
  ADMIN_BIN="${BACKUP_SERVER_ADMIN:-$SCRIPT_DIR/../build/backup-server-admin}"
  if [ -z "${BACKUP_SERVER_ROOT:-}" ]; then
    echo "ERROR: 这是源码树里的脚本，无法推断要管理哪个实例。" >&2
    echo "  请 SSH 到服务器，使用部署后的 <server-root>/bin/backup-server-admin.sh；" >&2
    echo "  或者显式设置 BACKUP_SERVER_ROOT（可选 BACKUP_SERVER_DATA / BACKUP_SERVER_DB）。" >&2
    exit 1
  fi
  SERVER_ROOT="$BACKUP_SERVER_ROOT"
fi

DATA_ROOT="${BACKUP_SERVER_DATA:-$SERVER_ROOT/data}"
DB_PATH="${BACKUP_SERVER_DB:-$SERVER_ROOT/state/metadata.sqlite3}"

if [ ! -x "$ADMIN_BIN" ]; then
  echo "找不到管理工具：$ADMIN_BIN" >&2
  echo "先构建：make server（产物在 build/backup-server-admin）" >&2
  exit 1
fi
if [ ! -d "$DATA_ROOT" ]; then
  echo "ERROR: 数据目录不存在：$DATA_ROOT" >&2
  echo "  Server root: $SERVER_ROOT" >&2
  echo "  用 BACKUP_SERVER_ROOT / BACKUP_SERVER_DATA 指向正确的实例。" >&2
  exit 1
fi
if [ ! -f "$DB_PATH" ]; then
  echo "ERROR: 未找到服务器状态数据库：$DB_PATH" >&2
  echo "  Server root: $SERVER_ROOT" >&2
  echo "  请确认正在管理正确的 backup-server 实例；管理工具不会创建空库。" >&2
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
  "$ADMIN_BIN" --server-root "$SERVER_ROOT" --root "$DATA_ROOT" \
    --db "$DB_PATH" "$@"
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
        printf '用户选择器（id:<编号> 或 name:<用户名>）：'
        read -r selector || return 0
        clear_screen
        admin show-user "$selector"
        pause
        ;;
      3)
        # 删除是不可逆的：这里**要求**用户写成 id:<编号> 或 name:<用户名>。
        # 裸输入可能是编号也可能是用户名，管理工具不会替用户猜。
        printf '要删除的用户（必须写成 id:<编号> 或 name:<用户名>）：'
        read -r selector || return 0
        [ -n "$selector" ] || continue
        case "$selector" in
          id:*|name:*) ;;
          *)
            printf '\n删除操作必须明确指定用户：id:<编号> 或 name:<用户名>。\n'
            printf '（裸输入可能是编号也可能是用户名，管理工具不会替你猜。）\n'
            pause
            continue
            ;;
        esac
        clear_screen
        local detail target_name target_id
        detail="$(admin show-user "$selector" 2>&1)"
        printf '%s\n' "$detail"
        target_name="$(printf '%s\n' "$detail" | awk -F'：' '/^用户名：/ { print $2; exit }')"
        target_id="$(printf '%s\n' "$detail" | awk -F'：' '/^用户 ID：/ { print $2; exit }')"
        if [ -z "$target_name" ] || [ -z "$target_id" ]; then
          pause
          continue
        fi
        printf '\n'
        printf '警告：这会永久删除账户 "%s"（id=%s）以及它的全部云端备份，且无法撤销。\n' \
          "$target_name" "$target_id"
        printf '请输入 DELETE %s#%s 以确认（直接回车取消）：' \
          "$target_name" "$target_id"
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
        printf '用户选择器（id:<编号> 或 name:<用户名>）：'
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
        # 与删除用户同一条规则：破坏性操作必须显式指定选择器。
        printf '用户选择器（必须写成 id:<编号> 或 name:<用户名>）：'
        read -r selector || return 0
        case "$selector" in
          id:*|name:*) ;;
          *)
            printf '\n删除操作必须明确指定用户：id:<编号> 或 name:<用户名>。\n'
            printf '（裸输入可能是编号也可能是用户名，管理工具不会替你猜。）\n'
            pause
            continue
            ;;
        esac
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
