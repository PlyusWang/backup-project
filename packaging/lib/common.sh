#!/usr/bin/env bash
# packaging/lib/common.sh —— release 打包脚本共用的最小工具集。
#
# 这里只放"每个打包脚本都要用、而且必须只有一份实现"的东西：日志、失败即停、
# 版本号解析、SOURCE_DATE_EPOCH、清单、锁、原子发布。产品语义一律不在这里，
# 也不在任何打包脚本里重新实现 —— 安装器包装产品，不重写产品。
#
# 约定：
#   * 所有脚本 set -Eeuo pipefail，并且用 die() 报错（退出码 1）；
#   * 用户输入错误（版本号、参数）退出码 2，机器/构建错误退出码 1；
#   * 绝不 echo 任何私钥/口令内容。

set -Eeuo pipefail

PACKAGING_LIB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PACKAGING_DIR="$(cd "$PACKAGING_LIB_DIR/.." && pwd)"
REPO_ROOT="$(cd "$PACKAGING_DIR/.." && pwd)"

log()  { printf '[release] %s\n' "$*"; }
warn() { printf '[release] WARN: %s\n' "$*" >&2; }
die()  { printf '[release] ERROR: %s\n' "$*" >&2; exit 1; }
die_usage() { printf '[release] ERROR: %s\n' "$*" >&2; exit 2; }

require_tool() {
  local tool
  for tool in "$@"; do
    command -v "$tool" >/dev/null 2>&1 || die "缺少工具：$tool"
  done
}

# 工作树必须干净：制品和 commit 对不上是最难查的一类问题。
require_clean_tree() {
  cd "$REPO_ROOT"
  local dirty
  dirty="$(git status --porcelain)"
  if [ -n "$dirty" ]; then
    printf '%s\n' "$dirty" >&2
    die "工作树不干净，拒绝构建 release 制品（本轮不允许 --allow-dirty）"
  fi
}

# 版本号：只接受 semver 形状（可带 +build 元数据）。没有 tag 时调用方给
# "0.0.0+g<sha>"；打包脚本**从不**创建 tag。
validate_version() {
  local version="$1"
  case "$version" in
    ""|*[!A-Za-z0-9.+-]*) die_usage "版本号只允许 [A-Za-z0-9.+-]：'$version'" ;;
  esac
  case "$version" in
    [0-9]*) ;;
    *) die_usage "版本号必须以数字开头：'$version'" ;;
  esac
  printf '%s' "$version"
}

default_dev_version() {
  cd "$REPO_ROOT"
  printf '0.0.0+g%s' "$(git rev-parse --short=7 HEAD)"
}

# semver -> Debian 版本：'-' 之后的预发布段用 '~' 表达（~ 在 dpkg 里排序更小，
# 这正是 rc 应该的位置）。'+build' 原样保留。
deb_version_of() {
  local version="$1" head rest
  case "$version" in
    *-*) head="${version%%-*}"; rest="${version#*-}"; printf '%s~%s' "$head" "$rest" ;;
    *)   printf '%s' "$version" ;;
  esac
}

# 可复现：时间戳、locale、排序全部固定。SOURCE_DATE_EPOCH 默认取 commit 时间。
setup_reproducible_env() {
  cd "$REPO_ROOT"
  if [ -z "${SOURCE_DATE_EPOCH:-}" ]; then
    SOURCE_DATE_EPOCH="$(git log -1 --format=%ct)"
  fi
  case "$SOURCE_DATE_EPOCH" in
    ''|*[!0-9]*) die "SOURCE_DATE_EPOCH 必须是 Unix 秒：'$SOURCE_DATE_EPOCH'" ;;
  esac
  export SOURCE_DATE_EPOCH
  export TZ=UTC
  export LC_ALL=C
  export LANG=C
  log "SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH ($(date -u -d "@$SOURCE_DATE_EPOCH" +%Y-%m-%dT%H:%M:%SZ))"
}

# 目录清单：路径排序固定、只算文件、不含清单自己。
write_manifest() {
  local dir="$1" manifest="${2:-}"
  [ -n "$manifest" ] || manifest="$dir/MANIFEST.sha256"
  ( cd "$dir" && find . -type f ! -name MANIFEST.sha256 -printf '%P\n' | LC_ALL=C sort | xargs -r sha256sum > "$manifest" )
  printf '%s' "$manifest"
}

verify_manifest() {
  local dir="$1" manifest="$2"
  ( cd "$dir" && sha256sum -c --quiet "$manifest" )
}

# 并发安装/打包锁：有 flock 就用，没有就明确拒绝（不静默降级）。
with_lock() {
  local lock_file="$1"; shift
  if ! command -v flock >/dev/null 2>&1; then
    die "缺少 flock：并发安装/打包需要它，这里不做静默降级"
  fi
  mkdir -p "$(dirname "$lock_file")"
  exec 9>"$lock_file"
  flock -w 300 9 || die "等待锁超时：$lock_file"
  "$@"
}

# 原子发布：先在同一个文件系统上的临时目录里做完整棵，成功后再 rename 过去。
publish_dir() {
  local stage="$1" target="$2"
  case "$stage" in "$target"/*|"$target") die "staging 目录不能在目标目录里面：$stage" ;; esac
  rm -rf "$target.tmp.$$"
  mkdir -p "$(dirname "$target")"
  mv "$stage" "$target.tmp.$$"
  rm -rf "$target"
  mv "$target.tmp.$$" "$target"
}

sha256_of() { sha256sum "$1" | awk '{print $1}'; }
size_of()   { stat -c %s "$1"; }

# ---- 制品安全检查规则（构建期自查与 CI 正式扫描共用同一份实现）----------
#
# 两个入口（packaging/ci-artifact-selfscan.sh 构建期、packaging/ci-secret-scan.sh
# CI）**必须**用同一套规则：v0.1.1 的教训就是"两处各写一份、改一处漏一处"。
# 规则集中在这里，两个入口只负责把结果翻译成各自的日志与返回码。
#
# 1) 内容规则（对全部解包内容 grep -rE，命中即失败）：
#      * PEM 私钥块          -----BEGIN ... PRIVATE KEY
#      * seed-hex 行         ^seed-hex: <64 hex>
#      * secret 文本行       ^BACKUP_TOKEN_SECRET=<16 位以上 hex>
# 2) 敏感文件名：*.key / secrets.env / *.bpcert（本项目的私钥与证书材料形状）
# 3) 原始密钥"形状"：恰好 32 字节的文件（transport.key 的长度）。
#    **这不是"32 字节文件必然是密钥"的断言**，而是一条形状启发式：它把"看起来像
#    transport.key 的东西"挑出来交给发布者确认。为了不淹没在正常数据里，排除下列
#    目录，工程依据是"这些位置本来就放着 32 字节的正常内容"（随包资源 / 文档样例 /
#    QML 插件数据 / 第三方库自带的占位文件）：
#      /usr/share/  /docs/  /qml/  /lib/
#    排除目录之外出现 32 字节文件即失败。两个方向的边界都有合成数据测试
#    （packaging/ci-packaging-quality-test.sh 的一致性矩阵）。
SECRET_SCAN_CONTENT_PATTERNS='-----BEGIN [A-Z ]*PRIVATE KEY|^seed-hex: [0-9a-f]{64}$|^BACKUP_TOKEN_SECRET=[0-9a-fA-F]{16,}$'
SECRET_SCAN_RAW_KEY_BYTES=32
SECRET_SCAN_RAW_KEY_EXCLUDE='/usr/share/|/docs/|/qml/|/lib/'

# ---- 扫描的"错误状态"必须传播（第3轮 P2-01）--------------------------------
#
# 旧实现是 grep 后面跟 2>/dev/null || true。GNU grep 用退出码区分三种结果：
#   0 = 有命中，1 = 正常完成但没有命中，2 = 执行 / 读取 / 参数错误。
# || true 把 2 也折叠成"没有发现" —— 而"读不出来"恰恰是最该报警的情形（不可读的
# 目录、坏掉的挂载、模式写错、grep 被换掉）。所以本文件的三个扫描函数统一遵守：
#   * stdout = 命中清单；返回 0 = 扫描成功（清单可能为空）；
#   * 返回 2 = 扫描本身出错（诊断打到 stderr），调用方**必须**据此失败；
#   * 扫描根不存在 / 不是目录 / 不可进入，同样算扫描错误 —— 静默扫一个不存在的
#     目录，等价于宣布"这里没有私钥"，那正是假通过。
SECRET_SCAN_ERROR_RC=2

secret_scan_require_root() {   # $1 = 扫描根；不合法时打印诊断并返回 2
  local root="${1:-}"
  if [ -z "$root" ]; then
    printf '[release] ERROR: 安全扫描失败：没有给出扫描根目录\n' >&2
    return "$SECRET_SCAN_ERROR_RC"
  fi
  if [ ! -e "$root" ]; then
    printf '[release] ERROR: 安全扫描失败：扫描根目录不存在：%s\n' "$root" >&2
    return "$SECRET_SCAN_ERROR_RC"
  fi
  if [ ! -d "$root" ]; then
    printf '[release] ERROR: 安全扫描失败：扫描根不是目录：%s\n' "$root" >&2
    return "$SECRET_SCAN_ERROR_RC"
  fi
  if [ ! -r "$root" ] || [ ! -x "$root" ]; then
    printf '[release] ERROR: 安全扫描失败：扫描根目录不可进入（需要 r+x）：%s\n' "$root" >&2
    return "$SECRET_SCAN_ERROR_RC"
  fi
}

secret_scan_content_files() {  # 命中内容规则的文件清单（每行一个）；2 = 扫描错误
  local root="$1" out rc=0
  # -I：只把**文本文件**算进来。这是刻意的判据 —— 私钥文件是文本；而随包的 Qt TLS
  #     后端（lib/libQt6Network.so.6、plugins/tls/libqopensslbackend.so）里含有
  #     "-----BEGIN ... PRIVATE KEY" 这样的**字符串常量**，按二进制匹配会在每个合法
  #     发行包上误报。合成数据测试钉住了两个方向：文本 PEM 必拦、真品必过。
  # --：pattern 以 '-' 开头，不加 -- 会被 grep 当成选项（GNU grep 直接退出 2），
  #     而这个错误又被 2>/dev/null + || true 吞掉 —— 于是这条规则在 v0.1.1 及更早
  #     的版本里**从未真正生效过**（上一轮发现，属"检查形同虚设"一类）。
  secret_scan_require_root "$root" || return $?
  # stderr 故意**不**重定向：grep 自己的诊断（哪一层目录读不了）原样进日志，只把
  # stdout（命中清单）收进变量 —— 两条流合流会让"警告文本"被当成"命中文件"。
  out="$(grep -rIlE -- "$SECRET_SCAN_CONTENT_PATTERNS" "$root")" || rc=$?
  if [ "$rc" -gt 1 ]; then
    printf '[release] ERROR: 安全扫描失败：内容规则扫描出错（grep 退出码 %s）：%s\n' "$rc" "$root" >&2
    return "$SECRET_SCAN_ERROR_RC"
  fi
  [ -z "$out" ] || printf '%s\n' "$out"
}

secret_scan_key_files() {      # 命中敏感文件名的文件清单；2 = 扫描错误
  local root="$1" out rc=0
  secret_scan_require_root "$root" || return $?
  out="$(find "$root" -type f \( -name '*.key' -o -name 'secrets.env' -o -name '*.bpcert' \))" || rc=$?
  if [ "$rc" -ne 0 ]; then
    printf '[release] ERROR: 安全扫描失败：敏感文件名扫描出错（find 退出码 %s）：%s\n' "$rc" "$root" >&2
    return "$SECRET_SCAN_ERROR_RC"
  fi
  [ -z "$out" ] || printf '%s\n' "$out"
}

secret_scan_raw_key_files() {  # "32 字节原始密钥形状"的文件清单；2 = 扫描错误
  local root="$1" files f size rc=0 grc erc
  secret_scan_require_root "$root" || return $?
  # 只枚举 17..63 字节的文件。旧实现是
  #   find ... 2>/dev/null | while ... done | { grep -vE ... || true; }
  # 整条管道把 find 的错误状态吞掉（而且 while 在子 shell 里，错误传不出来）。
  # 这里改成先取清单再在**当前 shell** 里循环，任何一步出错都返回 2。
  files="$(find "$root" -type f -size -64c -size +16c)" || rc=$?
  if [ "$rc" -ne 0 ]; then
    printf '[release] ERROR: 安全扫描失败：枚举候选文件出错（find 退出码 %s）：%s\n' "$rc" "$root" >&2
    return "$SECRET_SCAN_ERROR_RC"
  fi
  [ -n "$files" ] || return 0
  while IFS= read -r f; do
    [ -n "$f" ] || continue
    size="$(stat -c %s -- "$f")" || {
      printf '[release] ERROR: 安全扫描失败：读不到候选文件的大小：%s\n' "$f" >&2
      return "$SECRET_SCAN_ERROR_RC"
    }
    [ "$size" = "$SECRET_SCAN_RAW_KEY_BYTES" ] || continue
    grc=0
    if LC_ALL=C grep -qP '^[\x00-\xff]{32}$' -- "$f" 2>/dev/null; then grc=0; else grc=$?; fi
    if [ "$grc" -gt 1 ]; then
      printf '[release] ERROR: 安全扫描失败：读取候选文件出错（grep 退出码 %s）：%s\n' "$grc" "$f" >&2
      return "$SECRET_SCAN_ERROR_RC"
    fi
    [ "$grc" -eq 0 ] || continue
    erc=0
    if printf '%s\n' "$f" | grep -qE -- "$SECRET_SCAN_RAW_KEY_EXCLUDE"; then erc=0; else erc=$?; fi
    # 排除规则出错时**不放行**：宁可多问一句，也不把可疑文件当正常数据放过。
    if [ "$erc" -eq 0 ]; then continue; fi
    printf '%s\n' "$f"
  done <<< "$files"
}

scan_for_secrets() {           # 兼容旧调用：返回内容规则命中数（清单打到 stderr）；2 = 扫描错误
  local root="$1" found rc=0
  found="$(secret_scan_content_files "$root")" || rc=$?
  [ "$rc" -eq 0 ] || return "$rc"
  if [ -n "$found" ]; then
    printf '%s\n' "$found" >&2
    printf '%s\n' "$found" | awk 'END { print NR }'
  else
    printf '0'
  fi
}
