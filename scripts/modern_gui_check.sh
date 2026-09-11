#!/usr/bin/env bash
#
# 现代 QML GUI 检查：构建 + 无显示启动 + QML 静态约束 + 端到端备份恢复。
#
# 刻意和 scripts/gui_smoke_test.sh 分成两个脚本：那个只管 PR #6 的 Widgets GUI，
# 这个管 PR #7 新增的 Qt Quick GUI，互不依赖，一个坏掉不会把另一个的检查带塌。
#
# 覆盖：
#   1. 构建（复用 Makefile 的 gui-modern 目标，不在这里重复拼编译参数）。
#   2. offscreen 启动自检：QML 运行期告警会让进程自己以非 0 退出。
#   3. qmllint 静态检查；机器上没装就明确说“跳过”，而不是静默算通过。
#   4. 几条 grep 断言：资源清单、忙时禁用、拒绝假进度、拒绝网络栈。
#   5. --self-test 真跑一次备份 + 恢复，再用 diff -r 比对目录树。
#
#
# 用法：在仓库根目录执行 ./scripts/modern_gui_check.sh，不需要任何参数。
# 日志写在 tests/output/modern-gui-check.log（该目录已 gitignore），
# 屏幕上只打结论，细节去日志里看。
# 它不做的事也说清楚：不模拟鼠标点击、不做像素比对，
# 这两件事换台机器就可能变，交给人工和截图。
# 它做的是“能在无人值守的环境里判定的部分”。
# 退出码：0 表示全部通过。

# -u 让拼错的变量名立刻报错，-o pipefail 让管道中间的失败不被后面吞掉；
# 检查脚本本身要是静默失败，比不检查还糟。
set -euo pipefail

# 用脚本自身位置推导仓库根目录，所以在哪个目录执行都一样。
ROOT_DIR="$(cd "$(dirname "$BASH_SOURCE")/.." && pwd)"
cd "$ROOT_DIR"

# 整个脚本只关心这三个路径：QML 源码目录、资源清单、日志文件。
QML_DIR="$ROOT_DIR/ui/modern/qml"
RESOURCE_FILE="$ROOT_DIR/ui/modern/resources.qrc"
LOG_DIR="$ROOT_DIR/tests/output"
# 日志统一写一个文件：每一轮检查覆盖上一轮，不留一堆带时间戳的旧日志。
LOG_FILE="$LOG_DIR/modern-gui-check.log"
# 通过 / 失败各自计数，最后一起汇报，中途不提前退出，
# 这样一次运行就能看到所有问题，不用修一个跑一次。
PASS_COUNT=0
FAIL_COUNT=0

# 每项检查打一行结论，PASS / FAIL 分开计数。
# 这样 CI 日志里能一眼看出挂的是哪一项，而不用回头翻整段输出。
record_pass() {
  PASS_COUNT=$((PASS_COUNT + 1))
  echo "[modern-gui]   PASS: $1"
}

# 失败同样打一行，但不中断：能继续查的检查继续查完。
record_fail() {
  FAIL_COUNT=$((FAIL_COUNT + 1))
  echo "[modern-gui]   FAIL: $1"
}

# 断言某个模式出现恰好 N 次。次数比“存在性”严格，
# 能拦住复制粘贴多出一个按钮之类的改动。
# 计数式断言比“存在性”断言更能挡住复制粘贴带来的退化，
# 也顺便说明这个脚本对界面的期待是“可数的”。
expect_count() {
  local file="$1"
  local pattern="$2"
  local want="$3"
  local label="$4"
  local got
  got="$(grep -c -- "$pattern" "$file" || true)"
  if [[ "$got" == "$want" ]]; then
    record_pass "$label（$got 处）"
  else
    record_fail "$label（期望 $want 处，实际 $got 处）"
  fi
}

# 先建日志目录再 tee：不这么做的话，第一次运行时 tee 会直接报错。
mkdir -p "$LOG_DIR"
echo "[modern-gui] 现代 QML GUI 检查开始" | tee "$LOG_FILE"

# 依赖缺失要尽早失败，并且给出能照抄的安装命令；
# 直接往下走只会得到一屏找不到头文件的编译错误。
if ! pkg-config --exists Qt6Quick Qt6Qml Qt6QuickControls2 Qt6Concurrent; then
  echo "[modern-gui] 缺少 Qt 6 QML 开发包，无法继续。"
  echo "[modern-gui] Ubuntu 上可执行:"
  echo "[modern-gui]   sudo apt-get install -y qt6-declarative-dev qt6-declarative-dev-tools \\"
  echo "[modern-gui]     qml6-module-qtquick qml6-module-qtquick-controls qml6-module-qtquick-layouts \\"
  echo "[modern-gui]     qml6-module-qtquick-dialogs qml6-module-qtquick-window qml6-module-qtqml-workerscript"
  exit 1
fi

# 构建一律走 Makefile 的目标，脚本里不再重复拼编译参数：
# 参数写两份，日后改一处漏一处的概率很高。
echo "[modern-gui] 1) 构建 build/backup-gui-modern"
make gui-modern 2>&1 | tee -a "$LOG_FILE" | tail -3
if [[ -x "$ROOT_DIR/build/backup-gui-modern" ]]; then
  record_pass "构建产物存在"
else
  record_fail "构建产物缺失"
fi

# offscreen 让没有显示器的环境也能真正把窗口建出来；
# QSG_RHI_BACKEND=software 避开虚拟机里没有 3D 驱动的问题。
# --smoke-test 自己会把 QML 运行期告警算进退出码，所以这里只看退出码。
echo "[modern-gui] 2) offscreen 启动自检"
set +e
QT_QPA_PLATFORM=offscreen QSG_RHI_BACKEND=software timeout 60 \
  ./build/backup-gui-modern --smoke-test >> "$LOG_FILE" 2>&1
smoke_status=$?
set -e
if [[ "$smoke_status" -eq 0 ]]; then
  record_pass "启动自检退出码 0（QML 运行期无告警）"
else
  record_fail "启动自检退出码 $smoke_status"
  tail -20 "$LOG_FILE"
fi

# qmllint 的输出里噪音不少，判定规则见下面的注释：
# 只挑真正的错误，其余按提示计数写进日志。
echo "[modern-gui] 3) qmllint 静态检查"
# qmllint 不一定在 PATH 上：apt 装的 Qt 把工具放在 /usr/lib/qt6/bin，
# 非交互式 ssh 的 PATH 里通常没有它，所以三个位置都找一遍。
QMLLINT_BIN=""
for candidate in qmllint /usr/lib/qt6/bin/qmllint /usr/lib/qt6/libexec/qmllint; do
  if command -v "$candidate" >/dev/null 2>&1; then
    QMLLINT_BIN="$candidate"
    break
  fi
done
if [[ -n "$QMLLINT_BIN" ]]; then
  lint_failed=0
  info_count=0
  # QML 文件清单从 qrc 里取，保证“检查的”和“打进二进制的”是同一批文件。
  while read -r qml; do
    [[ -z "$qml" ]] && continue
    out="$("$QMLLINT_BIN" "ui/modern/$qml" 2>&1 || true)"
    echo "--- qmllint $qml" >> "$LOG_FILE"
    echo "$out" >> "$LOG_FILE"
    info_count=$((info_count + $(echo "$out" | grep -c "^Warning" || true)))
    # qmllint 6.4.2 对上下文属性（theme / controller）一律报 Unqualified access，
    # 对 easing 组和 Qt.AlignTop 报解析失败；这些是工具在 6.4 上的已知局限，
    # 不是代码问题。所以只把语法错误和未使用导入当失败，其余只统计条数。
    if echo "$out" | grep -qE "Syntax error|Expected token|Unused import"; then
      echo "[modern-gui]     qmllint 实质报错: $qml"
      lint_failed=1
    fi
  done < <(grep -o "qml/[A-Za-z/]*\.qml" "$RESOURCE_FILE" | sort -u)
  if [[ "$lint_failed" -eq 0 ]]; then
    record_pass "qmllint 无语法错误与未使用导入（工具版本局限产生的提示 $info_count 条，见日志）"
  else
    record_fail "qmllint 报出语法错误或未使用导入"
  fi
else
  echo "[modern-gui]     跳过：本机没有 qmllint（qt6-declarative-dev-tools 提供）"
fi

# 下面几条是对 QML 源码的断言：凡是脚本能查的，就不留给人工。
echo "[modern-gui] 4) 静态约束"
# 反向也要查：有文件没进清单，界面会缺一块；
# 清单里有文件但磁盘上没有，rcc 编译时才会报错。
# 资源清单必须覆盖 qml 目录下每个文件，否则会出现“文件在仓库里、
# 却没打进二进制、界面上少一块”的问题。
# missing 用 0 / 1 记录“有没有漏”，不提前退出：
# 一次运行把缺的文件全列出来，比只报第一个更有用。
# 这里不用 set -e 兜底，是因为 grep 找不到时本来就返回非 0。
missing=0
while read -r file; do
  rel="$(realpath --relative-to="$QML_DIR" "$file")"
  if ! grep -q -- "qml/$rel" "$RESOURCE_FILE"; then
    echo "[modern-gui]     未打进资源: $rel"
    missing=1
  fi
done < <(find "$QML_DIR" -name '*.qml' | sort)
if [[ "$missing" -eq 0 ]]; then
  record_pass "所有 QML 文件都在 resources.qrc 里"
else
  record_fail "有 QML 文件没进资源清单"
fi

# 两个操作页共用一个 OperationPage.qml：两个文本框、两个“浏览”按钮
# 和一个主操作按钮，五处都要绑 !controller.busy，忙的时候不能重复点。
# 这条断言同时防两种退化：漏绑 busy（忙时还能点）和
# 多出绑定位（复制粘贴出来的多余按钮）。
expect_count "$QML_DIR/pages/OperationPage.qml" "enabled: !controller.busy" 5 \
  "忙碌时禁用输入与按钮"

# 进度显示是最容易“看起来能用、其实是假的”的地方，
# 所以这里用断言把它钉死。
# 不真做进度条就不许出现百分比字段，避免“看起来很精确”的假进度。
if grep -rqE 'percent|progressValue|estimatedSeconds' "$QML_DIR"; then
  record_fail "出现了假进度字段"
else
  record_pass "没有假进度字段（进度只用不确定动画）"
fi

# 备份工具一旦偷偷联网，性质就变了：这条断言是给未来的改动上的锁。
# 这是一套纯本地工具，不应该出现任何网络栈导入。
if grep -rqE 'QtWebView|QtWebEngine|QtNetwork|XMLHttpRequest' "$QML_DIR"; then
  record_fail "QML 里出现了网络栈导入"
else
  record_pass "QML 无网络栈导入"
fi

# 前面都是静态检查，这一段是真的跑一次备份再恢复：
# 构建能不能过、界面能不能起，和“功能对不对”是两回事。
echo "[modern-gui] 5) 端到端备份 + 恢复"
# 测试数据刻意包含最容易出问题的几类：中文名、带空格的名字、空文件、
# 空目录、子目录。备份工具最容易在这几处丢文件或改名字。
# 临时目录跑完就删，不往仓库里留东西。
WORK_DIR="$(mktemp -d)"
mkdir -p "$WORK_DIR/source/sub" "$WORK_DIR/source/空目录"
printf 'hello modern gui\n' > "$WORK_DIR/source/readme.txt"
printf '中文内容\n' > "$WORK_DIR/source/中文 名字.txt"
: > "$WORK_DIR/source/empty.txt"
printf 'binary\000\001\002' > "$WORK_DIR/source/sub/nested.bin"
set +e
QT_QPA_PLATFORM=offscreen QSG_RHI_BACKEND=software timeout 120 \
  ./build/backup-gui-modern --self-test \
  "$WORK_DIR/source" "$WORK_DIR/repo" "$WORK_DIR/restore" >> "$LOG_FILE" 2>&1
selftest_status=$?
set -e
if [[ "$selftest_status" -eq 0 ]]; then
  record_pass "--self-test 备份与恢复都成功"
else
  record_fail "--self-test 退出码 $selftest_status"
  tail -10 "$LOG_FILE"
fi
# 自测自己说 ok 还不够，必须真的逐文件比对一遍目录树，
# 确认恢复出来的东西与原始目录一致。
if diff -r "$WORK_DIR/source" "$WORK_DIR/restore" >> "$LOG_FILE" 2>&1; then
  record_pass "diff -r 目录树完全一致"
else
  record_fail "diff -r 有差异"
fi
rm -rf "$WORK_DIR"

# 汇总：只要有一项失败就以非 0 退出，脚本可以被 CI 直接调用。
# 失败时也把日志路径打出来，方便贴进问题记录。
echo "[modern-gui] 通过 $PASS_COUNT 项，失败 $FAIL_COUNT 项"
echo "[modern-gui] 日志: $LOG_FILE"
if [[ "$FAIL_COUNT" -eq 0 ]]; then
  # 全部通过时才打 PASS 并以 0 退出。
echo "[modern-gui] PASS"
else
  echo "[modern-gui] FAIL"
  exit 1
fi

