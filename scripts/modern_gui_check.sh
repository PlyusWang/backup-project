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
#   6. --path-test：本地路径与 URL 互转（中文、空格、#、%）不丢字符。
#   7. --close-guard-test：任务进行中关窗被拦下，结束后可以正常退出。
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

# 同上，但按正则匹配：用来断言“某一行以 onClosing: 开头”这类结构，
# 免得注释里提到同一个词也被算进去。
expect_count_re() {
  local file="$1"
  local pattern="$2"
  local want="$3"
  local label="$4"
  local got
  got="$(grep -cE -- "$pattern" "$file" || true)"
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
# qmllint 6.4.2 的已知工具局限，逐条对应已确认的误报，只有这里列出的才放行：
#   1. 上下文属性 theme / controller / useNativeFrame：qmllint 不知道它们是什么，
#      凡是引用都报 Unqualified access。判定时会看一眼紧邻的代码行，
#      只有确实是这三个名字才放行。
#   2. contentItem 的延迟赋值提示：Qt 自己的优化建议，运行期无影响。
#   3. easing 组：6.4 的 qmltypes 不完整，运行期动画实测正常。
#   4. Window.flags / MouseArea.cursorShape / Layout.alignment：同样缺 qmltypes，
#      属性本身有效。
#   5. Qt 的对齐、光标形状、窗口边缘枚举：Property "X" not found on type "Qt"，
#      这些枚举运行期都能解析。
# 其它任何 Warning / Error / Info 一律算未知问题，直接判失败。
classify_qmllint() {
  awk '
    function classify(msg, snippet) {
      # easing.type 这类分组属性 6.4 解析不了，qmllint 会额外报一条
      # Unqualified access；同一行已经由上面的 easing 规则放行，这里一并认掉。
      if (msg ~ /Unqualified access/ &&
          (snippet ~ /theme/ || snippet ~ /controller/ || snippet ~ /useNativeFrame/ ||
           snippet ~ /easing\./)) {
        print "ALLOWED\t" msg; return
      }
      if (msg ~ /Cannot defer property assignment to "contentItem"/) {
        print "ALLOWED\t" msg; return
      }
      if (msg ~ /unknown grouped property scope easing/ ||
          msg ~ /is used but it is not resolved/ ||
          msg ~ /Binding assigned to "type"/) {
        print "ALLOWED\t" msg; return
      }
      if (msg ~ /No type found for property "(flags|cursorShape|alignment)"/) {
        print "ALLOWED\t" msg; return
      }
      if (msg ~ /Property "(AlignTop|AlignRight|LeftEdge|RightEdge|TopEdge|BottomEdge|SizeHorCursor|SizeVerCursor)" not found on type "Qt"/) {
        print "ALLOWED\t" msg; return
      }
      print "UNKNOWN\t" msg
    }
    # “Info: Did you mean ...” 是上一条告警的补充说明，不是独立诊断，
    # 混进待判定队列会把后面那条告警的代码行抢走，导致误判。
    /^Info: Did you mean/ { next }
    /^(Warning|Error|Info):/ { pending[++n] = $0; next }
    {
      if (n > 0) {
        for (i = 1; i <= n; ++i) classify(pending[i], $0)
        n = 0
      }
    }
    END { for (i = 1; i <= n; ++i) classify(pending[i], "") }
  '
}

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
  allowed_total=0
  unknown_total=0
  # QML 文件清单从 qrc 里取，保证“检查的”和“打进二进制的”是同一批文件。
  while read -r qml; do
    [[ -z "$qml" ]] && continue
    raw="$("$QMLLINT_BIN" "ui/modern/$qml" 2>&1 || true)"
    classified="$(printf '%s\n' "$raw" | classify_qmllint)"
    allowed=$(printf '%s\n' "$classified" | grep -c '^ALLOWED' || true)
    unknown=$(printf '%s\n' "$classified" | grep -c '^UNKNOWN' || true)
    allowed_total=$((allowed_total + allowed))
    unknown_total=$((unknown_total + unknown))
    {
      echo "--- qmllint $qml（已知 $((allowed)) 条 / 未知 $((unknown)) 条）"
      printf '%s\n' "$classified"
    } >> "$LOG_FILE"
    if [[ "$unknown" -gt 0 ]]; then
      echo "[modern-gui]     $qml 出现未登记的告警："
      printf '%s\n' "$classified" | grep '^UNKNOWN' | sed 's/^UNKNOWN[[:space:]]*/      /'
    fi
  done < <(grep -o 'qml/[A-Za-z/]*\.qml' "$RESOURCE_FILE" | sort -u)
  if [[ "$unknown_total" -eq 0 ]]; then
    record_pass "qmllint 无未登记告警（已知 6.4.2 工具局限放行 $allowed_total 条）"
  else
    record_fail "qmllint 出现 $unknown_total 条未登记告警（原文见日志）"
  fi
else
  echo "[modern-gui]     跳过：本机没有 qmllint（qt6-declarative-dev-tools 提供）"
fi

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
# 只认赋值/字段形态：注释里出现的 percent-encoding 不算假进度。
if grep -rqE 'percent[[:space:]]*[:=]|progressValue|estimatedSeconds' "$QML_DIR"; then
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
# 根目录本身带中文和空格：路径问题往往就出在这一层，
# 只在 source 里面放中文文件名是测不出来的。
WORK_DIR="$(mktemp -d)/现代 GUI 路径测试"
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
echo "[modern-gui] 6) 路径转换（本地路径 ↔ URL）"
# “浏览”按钮选完目录走的就是 controller.localPathFromUrl()：
# 这个开关把它在中文、空格、#、% 上的行为摊开验证，而不是只测 ASCII。
set +e
QT_QPA_PLATFORM=offscreen QSG_RHI_BACKEND=software \
  ./build/backup-gui-modern --path-test > /tmp/modern-gui-path.log 2>&1
path_status=$?
set -e
sed 's/^/[modern-gui]     /' /tmp/modern-gui-path.log
cat /tmp/modern-gui-path.log >> "$LOG_FILE"
if [[ "$path_status" -eq 0 ]]; then
  record_pass "路径转换 round trip 一致（中文 / 空格 / # / %）"
else
  record_fail "路径转换 round trip 失败"
fi

echo "[modern-gui] 7) 关闭守卫"
# 静态确认守卫挂在窗口层：只写在自绘 × 按钮里的话，
# Alt+F4 与窗口管理器都能绕过去。
expect_count_re "$QML_DIR/Main.qml" "^[[:space:]]*onClosing:" 1 \
  "主窗口在 onClosing 里处理关闭请求"
# 错误正文要能选中复制，核心给的长路径才有可能贴出来。
expect_count_re "$QML_DIR/components/StatusBanner.qml" "selectByMouse:[[:space:]]*true" 1 \
  "状态栏正文可鼠标选中"
# 运行期契约：忙时拒绝关闭并提示，任务结束后放行。
set +e
QT_QPA_PLATFORM=offscreen QSG_RHI_BACKEND=software \
  ./build/backup-gui-modern --close-guard-test > /tmp/modern-gui-guard.log 2>&1
guard_status=$?
set -e
sed 's/^/[modern-gui]     /' /tmp/modern-gui-guard.log
cat /tmp/modern-gui-guard.log >> "$LOG_FILE"
if [[ "$guard_status" -eq 0 ]]; then
  record_pass "忙时关窗被拒绝并给出提示，任务结束后可正常关闭"
else
  record_fail "关闭守卫行为与预期不符"
fi

echo "[modern-gui] 通过 $PASS_COUNT 项，失败 $FAIL_COUNT 项"
echo "[modern-gui] 日志: $LOG_FILE"
if [[ "$FAIL_COUNT" -eq 0 ]]; then
  # 全部通过时才打 PASS 并以 0 退出。
echo "[modern-gui] PASS"
else
  echo "[modern-gui] FAIL"
  exit 1
fi

