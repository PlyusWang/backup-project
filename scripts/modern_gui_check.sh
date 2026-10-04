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
#   4. 几条 grep 断言：资源清单、忙时禁用、拒绝假进度、拒绝网络栈，
#      以及 repository-driven 架构约束（七页结构、没有 standalone 恢复页、
#      QML 不出现 archive 完整路径、不自己拼 repository 路径）。
#   5. --self-test 真跑一次 direct archive 打包 + 解包，再用 diff -r 比对目录树；
#      顺带断言这条 direct 测试路径的产物仍是 legacy v0.1
#      （BKPARCH\0 / version 1）——旧格式的回归入口没有被一起切到 v2。
#   6. --path-test：本地路径与 URL 互转（中文、空格、#、%）不丢字符。
#   7. --close-guard-test：任务进行中关窗被拦下，结束后可以正常退出。
#   8. 文件筛选在 GUI 路径上生效。
#   9. --repository-test：ConfigManager + BackupCatalog + BackupController +
#      BackupEngine 的真实产品链路（保存仓库 / 自动命名备份 / 列表 / 恢复 / 删除），
#      并直接读产物字节断言正常备份落成 v2 容器（BKPCNT2\0 / version 2 /
#      MyPack / 不压缩 / 不加密），catalog 记录 format_version=2、entry_count>0；
#      源目录含 symlink 与 FIFO 时，恢复后仍必须是 S_ISLNK（target 一致）与
#      S_ISFIFO。
#  10. 损坏 .bak 场景下管理页仍能正常渲染。
#  11. 备份算法选项与密码：QML 结构约束（高级选项面板 / 两个密码框 / 禁用措辞 /
#      产品 CLI 没有 --password 选项）；--backup-options-test 走真实控制器路径
#      验证解析表、四种算法组合、密码校验、未知 key、加密与 legacy 恢复、
#      目录字段、密码不落盘。
#  12. 截图（写进 tests/output/，评审产物不进仓库）：两套主题 × 七页 +
#      高级选项展开 + 加密恢复密码对话框。
#  13. 远程备份页（PR #20）：真的起一个 backup-server 进程，用页面背后的
#      RemoteController 走完 注册 / 登录 / 上传真实归档 / 列表 / 下载 / 删除 /
#      退出登录，并断言密码回显模式、口令与 token 不落盘、忙碌时冲突请求被拒、
#      页面提示不外泄、列表行显示名称 / 大小 / 时间、删除必须确认、两套主题
#      下关键控件几何正常。PR #21 起这个真服务端必须带 BPSEC1 身份私钥
#      （--transport-key-file）启动，客户端必须拿到它的公钥指纹 pin；构建一节
#      因此同时断言 backup-server-keygen 存在，并断言缺这个参数时服务端以
#      用法错误（2）退出。
#
# 所有 GUI 调用都带 --config-file 指向临时目录，并且导出临时 XDG_CONFIG_HOME：
# AppTheme 的 QSettings 与 QStandardPaths 都跟着它走，测试绝不读写真实用户配置。
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

# 真实用户配置隔离。AppTheme 的 QSettings 与 QStandardPaths 都以
# XDG_CONFIG_HOME 为根，指向临时目录之后，测试既不读也不写 ~/.config。
# 每个 GUI 调用另外显式传 --config-file，让 ConfigManager 也落在临时目录里。
TEST_STATE_DIR="$(mktemp -d)"
cleanup_test_state() {
  rm -rf "$TEST_STATE_DIR"
}
trap cleanup_test_state EXIT
export XDG_CONFIG_HOME="$TEST_STATE_DIR/xdg"
TEST_CONFIG_FILE="$TEST_STATE_DIR/config.json"

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

# 远程备份页的自检要真的起一个服务端进程：它就是与 network_test.sh、阿里云
# 部署同一个 Makefile 目标产出的那个 backup-server，不是测试专用的假服务端。
# PR #21 起这个服务端**必须**带 --transport-key-file（BPSEC1 传输身份私钥：
# 32 字节、0600）才能启动，而身份密钥只能用同一个 make server 目标产出的
# backup-server-keygen 生成 / 查看。--remote-test 在它自己的临时目录里生成
# 密钥、把公钥指纹（pin）给客户端用；私钥内容不经过这个脚本。
echo "[modern-gui] 1b) 构建 build/backup-server 与 build/backup-server-keygen（--remote-test 需要真服务端）"
make server 2>&1 | tee -a "$LOG_FILE" | tail -2
if [[ -x "$ROOT_DIR/build/backup-server" ]]; then
  record_pass "backup-server 构建产物存在"
else
  record_fail "backup-server 构建产物缺失"
fi
if [[ -x "$ROOT_DIR/build/backup-server-keygen" ]]; then
  record_pass "backup-server-keygen 构建产物存在（生成 / 查看服务端身份密钥）"
else
  record_fail "backup-server-keygen 构建产物缺失（--remote-test 起服务端要用它生成身份密钥）"
fi
# 缺 --transport-key-file 时服务端必须**直接以用法错误（2）退出**，而不是
# "先起来再说"。用端口 0 + 临时目录跑一次：即使这条契约坏了，也只会多出一个
# 被 timeout 收走的进程，不碰任何真实实例，也不占固定端口。
mkdir -p "$TEST_STATE_DIR/no-transport-key/data"
set +e
timeout 30 ./build/backup-server --bind 127.0.0.1 --port 0 \
  --root "$TEST_STATE_DIR/no-transport-key/data" \
  --db "$TEST_STATE_DIR/no-transport-key/state/metadata.sqlite3" \
  --secret-file "$TEST_STATE_DIR/no-transport-key/secrets.env" \
  > "$TEST_STATE_DIR/no-transport-key.log" 2>&1
no_transport_key_status=$?
set -e
if [[ "$no_transport_key_status" -eq 2 ]] \
   && grep -q -- '--transport-key-file' "$TEST_STATE_DIR/no-transport-key.log"; then
  record_pass "缺少 --transport-key-file 时服务端以用法错误退出（退出码 2）"
else
  record_fail "缺少 --transport-key-file 的服务端行为（退出码 $no_transport_key_status，日志 $TEST_STATE_DIR/no-transport-key.log）"
fi

# offscreen 让没有显示器的环境也能真正把窗口建出来；
# QSG_RHI_BACKEND=software 避开虚拟机里没有 3D 驱动的问题。
# --smoke-test 自己会把 QML 运行期告警算进退出码，所以这里只看退出码。
echo "[modern-gui] 2) offscreen 启动自检"
set +e
QT_QPA_PLATFORM=offscreen QSG_RHI_BACKEND=software timeout 60 \
  ./build/backup-gui-modern --smoke-test \
  --config-file "$TEST_CONFIG_FILE" >> "$LOG_FILE" 2>&1
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
#   1. 上下文属性 theme / controller / useNativeFrame / filterRuleModel：
#      qmllint 不知道它们是什么，凡是引用都报 Unqualified access。判定时会看一眼
#      紧邻的代码行，只有确实是这几个名字才放行。filterRuleModel 由 main.cpp 注册，
#      面板通过 OperationPage 显式注入（面板内部不直接访问全局属性）。
#   2. contentItem 的延迟赋值提示：Qt 自己的优化建议，运行期无影响。
#   3. easing 组：6.4 的 qmltypes 不完整，运行期动画实测正常。
#   4. Window.flags / MouseArea.cursorShape / Layout.alignment：同样缺 qmltypes，
#      属性本身有效。
#   5. Qt 的对齐、光标形状、窗口边缘枚举：Property "X" not found on type "Qt"，
#      这些枚举运行期都能解析。
# 其它任何 Warning / Error / Info 一律算未知问题，直接判失败。
classify_qmllint() {
  awk '
    BEGIN { prev_panel_allowed = 0; prev_allowed_file = "" }
    # 上一条诊断是否“已精确放行且来自 FilterEditorPanel.qml”。
    # 只用来放行紧跟其后的那条 companion Info，遇到任何别的诊断立即清空。
    # prev_allowed_file 同时记下这条已放行诊断来自哪个文件：companion Info
    # 自己不带文件名，只有“紧跟在同文件的已放行诊断之后”才允许放行，
    # 免得变成“全局允许某类提示”。
    function MarkAllowed(msg,    file) {
      print "ALLOWED\t" msg
      file = ""
      if (match(msg, /[A-Za-z_]+\.qml/)) file = substr(msg, RSTART, RLENGTH)
      prev_panel_allowed = (file ~ /(FilterEditorPanel|FilterRuleEditor)\.qml/) ? 1 : 0
      prev_allowed_file = file
    }
    function classify(msg, snippet) {
      # 既有放行：上下文属性 theme / controller / useNativeFrame，以及委托里的
      # index / modelData，easing 组等已知工具局限。
      if (msg ~ /Unqualified access/ &&
          (snippet ~ /theme/ || snippet ~ /controller/ || snippet ~ /useNativeFrame/ ||
           snippet ~ /easing\./ || snippet ~ /modelData/ || snippet ~ /\bindex\b/)) {
        MarkAllowed(msg); return
      }
      # PR #12 起：filterRuleModel 是 main.cpp 注册的上下文属性，qmllint 不认识上下文属性，
      # 凡是引用都报 Unqualified access。repository-driven 改造后注入点从
      # OperationPage.qml 搬到了 BackupPage.qml，放行规则跟着搬家 ——
      # 仍然精确限定到"这个文件 + filterRuleModel 这个名字"，不是按文件整体放行。
      if (msg ~ /Unqualified access/ && msg ~ /BackupPage\.qml/ &&
      snippet ~ /filterRuleModel/) {
      MarkAllowed(msg); return
      }
      # PR #12：编辑器面板内部引用本组件根 id / 注入属性（含必需的 ruleModelRef）。
      if (msg ~ /Unqualified access/ && msg ~ /FilterEditorPanel\.qml/ &&
          (snippet ~ /panel\./ || snippet ~ /ruleModel/ || snippet ~ /ruleModelRef/)) {
        MarkAllowed(msg); return
      }
      # PR #19 第二轮：三个页面的规则模型（filterRuleModel / scheduleFilterRuleModel /
      # realtimeFilterRuleModel）同样是 main.cpp 注册的上下文属性，qmllint 不认识
      # 它们。名字本身足够独特，按名字精确放行。
      if (msg ~ /Unqualified access/ &&
          (snippet ~ /filterRuleModel/ || snippet ~ /scheduleFilterRuleModel/ ||
           snippet ~ /realtimeFilterRuleModel/)) {
        MarkAllowed(msg); return
      }
      # PR #19 第二轮：FilterRuleEditor.qml 是三个页面共用的规则编辑器，它只引用
      # 上下文属性 theme 与本组件根 id editor（以及注入的 ruleModel）。qmllint
      # 不认识上下文属性，凡是引用都报 Unqualified access；放行同样精确限定到
      # "这个文件 + 这几个名字"。
      if (msg ~ /Unqualified access/ && msg ~ /FilterRuleEditor\.qml/ &&
          (snippet ~ /theme\./ || snippet ~ /editor\./ || snippet ~ /ruleModel/)) {
        MarkAllowed(msg); return
      }
      # PR #12：紧随上述已放行主诊断的 companion Info（qmllint 不给它文件路径）。
      # 只认这一句精确文本，且只在直接前一条是 FilterEditorPanel.qml 的已放行诊断时才放行；
      # 放行后立刻清状态，避免变成“全局允许某类提示”。
      if (msg ~ /^Info: (ruleModel|modelData) is a member of a parent element\.?$/ &&
          (prev_panel_allowed == 1 || prev_allowed_file ~ /SchedulePage\.qml/ ||
           prev_allowed_file ~ /RealtimePage\.qml/ ||
           prev_allowed_file ~ /SegmentedTabs\.qml/)) {
        print "ALLOWED\t" msg
        # 不清状态：SchedulePage 的委托用 required property var modelData，
        # qmllint 会在这条之后紧跟一条不带文件名的通用 Info，两条属于同一份诊断。
        return
      }
      if (msg ~ /^Info: You first have to give the element an id\.?$/ &&
          (prev_allowed_file ~ /SchedulePage\.qml/ ||
           prev_allowed_file ~ /RealtimePage\.qml/)) {
        print "ALLOWED\t" msg
        prev_panel_allowed = 0
        prev_allowed_file = ""
        return
      }
      # AppComboBox 的静态工具局限（runtime 已实测正常：gui-all 0 warning、
      # smoke exit 0 / 0 告警、close-guard exit 0）。逐条限定到该文件 + 精确诊断：
      #   1) delegateModel 的 QQmlInstanceModel 类型在 6.4 的 qmltypes 里没有暴露；
      #   2) popup 是 deferred property，qmllint 提示不要在里面放 id（这是优化提示，
      #      运行期正确，且去掉 id 会让滚轮/滚动条拿不到列表对象）；
      #   3) 委托与 popup 内部对 theme.*（上下文属性）与 control.*（本组件根 id）的访问。
      #      委托是独立组件作用域，6.4 的静态检查解析不到外层 id，运行期正常；
      #      实测诊断：AppComboBox.qml:107:16 与 109:22 的 "Unqualified access"。
      if (msg ~ /AppComboBox\.qml/ &&
          (msg ~ /Type "QQmlInstanceModel" of property "delegateModel" not found/ ||
           msg ~ /Cannot defer property assignment to "popup"/)) {
        print "ALLOWED\t" msg; return
      }
      if (msg ~ /Unqualified access/ && msg ~ /AppComboBox\.qml/ &&
          (snippet ~ /theme\./ || snippet ~ /control\./)) {
        print "ALLOWED\t" msg; return
      }
      # PR #17：SchedulePage.qml 只引用两个上下文属性 —— main.cpp 注册的
      # schedule（定时备份控制器）与本页自己的根 id page。qmllint 不认识上下文
      # 属性，凡是引用都会报 Unqualified access。放行规则同样精确限定到
      # "这个文件 + 这两个名字"，而不是按文件整体放行。
      # 委托里的 modelData 由上面第一条规则放行，紧随其后的 companion Info
      # 再由下面那条按 prev_allowed_file 放行。
      if (msg ~ /Unqualified access/ && msg ~ /SchedulePage\.qml/ &&
          (snippet ~ /schedule\./ || snippet ~ /page\./)) {
        MarkAllowed(msg); return
      }
      # PR #19：RealtimePage.qml 只引用两个上下文属性 —— main.cpp 注册的
      # realtime（实时备份控制器）与本页自己的根 id page。放行规则同样精确限定到
      # "这个文件 + 这两个名字"。
      if (msg ~ /Unqualified access/ && msg ~ /RealtimePage\.qml/ &&
          (snippet ~ /realtime\./ || snippet ~ /page\./)) {
        MarkAllowed(msg); return
      }
      # PR #20：RemotePage.qml 只引用两个上下文属性 —— main.cpp 注册的
      # remote（远程备份控制器）与本页自己的根 id page。qmllint 同样不认识
      # 上下文属性，放行规则精确限定到"这个文件 + 这两个名字"。
      if (msg ~ /Unqualified access/ && msg ~ /RemotePage\.qml/ &&
          (snippet ~ /remote\./ || snippet ~ /page\./)) {
        MarkAllowed(msg); return
      }
      # PR #20 closure：RemotePage.qml 用 Connections 把"一次操作结束"的结果落回
      # 界面草稿（注册成功切回登录标签、注销成功清空密码框）。qmllint 6.4.2 的
      # qmltypes 里没有 Connections 这个类型，于是同一处冒出三条诊断：
      # Connections was not found / Binding assigned to "target" / 紧跟其后的
      # Unqualified access（target: remote 这一行没有点号，因此上面的通用规则
      # 接不住它）。运行期实测正常：--remote-test 的 REMOTE-05 / 05a / 14 全绿、
      # 启动自检 0 条 QML 运行期告警。放行精确限定到"这个文件 + 这一组诊断"。
      if (msg ~ /RemotePage\.qml/ && msg ~ /Connections was not found/) {
        MarkAllowed(msg); return
      }
      if (msg ~ /RemotePage\.qml/ &&
          msg ~ /Binding assigned to "target", but no property "target" exists/) {
        MarkAllowed(msg); return
      }
      if (msg ~ /Unqualified access/ && msg ~ /RemotePage\.qml/ &&
          snippet ~ /target: remote/) {
        MarkAllowed(msg); return
      }
      # RemoteSnapshotCard.qml 是远程备份列表的委托组件：theme 由上面那条通用
      # 规则覆盖，这里补它自己的根 id card。
      if (msg ~ /Unqualified access/ && msg ~ /RemoteSnapshotCard\.qml/ &&
          (snippet ~ /card\./ || snippet ~ /theme\./)) {
        MarkAllowed(msg); return
      }
      # PR #20（人工验收修复）：SegmentedTabs.qml 是本轮新增的共享分段控件。
      # 它只引用上下文属性 theme、本组件根 id control，以及委托里的 segment /
      # modelData（委托是独立组件作用域，6.4 的静态检查解析不到外层 id；运行期
      # 正常，--remote-test 与启动自检都是 0 条 QML 运行期告警）。
      if (msg ~ /Unqualified access/ && msg ~ /SegmentedTabs\.qml/ &&
          (snippet ~ /theme\./ || snippet ~ /control\./ ||
           snippet ~ /segment\./ || snippet ~ /modelData/)) {
        MarkAllowed(msg); return
      }
      if (msg ~ /Cannot defer property assignment to "contentItem"/) {
        MarkAllowed(msg); return
      }
      if (msg ~ /unknown grouped property scope easing/ ||
          msg ~ /is used but it is not resolved/ ||
          msg ~ /Binding assigned to "type"/) {
        MarkAllowed(msg); return
      }
      if (msg ~ /No type found for property "(flags|cursorShape|alignment)"/) {
        MarkAllowed(msg); return
      }
      if (msg ~ /Property "(AlignTop|AlignRight|LeftEdge|RightEdge|TopEdge|BottomEdge|SizeHorCursor|SizeVerCursor)" not found on type "Qt"/) {
        MarkAllowed(msg); return
      }
      prev_panel_allowed = 0
      prev_allowed_file = ""
      print "UNKNOWN\t" msg
    }
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

# 页面结构：首页 / 备份 / 自动备份 / 备份管理 / 设置 五页。
# 恢复已经不是独立页面，而是备份管理页里的一个动作 —— 这几条断言把结构钉死，
# 免得日后又长回一个"恢复页"。
# PR #19 之后是六页：首页 / 备份 / 自动备份 / 实时备份 / 备份管理 / 设置。
# PR #20 之后是七页，最后加上"远程备份"。
expect_count "$QML_DIR/Main.qml" "NavItem {" 7 \
  "侧栏有七个导航项（首页 / 备份 / 自动备份 / 实时备份 / 备份管理 / 远程备份 / 设置）"
expect_count "$QML_DIR/Main.qml" "opacity: root.currentPage === " 7 \
  "StackLayout 里七页各自绑定可见性"
expect_count_re "$QML_DIR/Main.qml" "^[[:space:]]*currentIndex: root.currentPage" 1 \
  "StackLayout 跟随 root.currentPage"
if grep -rq 'OperationPage' "$QML_DIR" "$RESOURCE_FILE"; then
  record_fail "仍然存在 standalone OperationPage（恢复应当是管理页里的动作）"
else
  record_pass "没有 standalone OperationPage"
fi

# ---- 反向 / 正向断言的小工具 ----
#
# 这三个函数在 PR #20 的远程备份页一节就要用，而那一节在文件里的位置比"自动
# 备份页"更靠前，所以定义放在这里（函数在调用时才解析，定义一次两处都能用）。
expect_present() {
  local file="$1"
  local pattern="$2"
  local label="$3"
  if grep -qF -- "$pattern" "$file"; then
    record_pass "$label"
  else
    record_fail "$label（缺少：$pattern）"
  fi
}

# 反向断言（只看代码行）：注释里写"以前这里是 ext:cpp;h"是正常的说明，
# 断言的是**用户看得到的文案**里没有它，所以先剔掉 // 开头的行。
expect_missing_code() {
  local file="$1"
  local pattern="$2"
  local label="$3"
  # grep -n 的输出是 "行号:内容"（没有文件名前缀），所以过滤的是 ^行号: //
  if grep -nF -- "$pattern" "$file" | grep -vE '^[0-9]+:[[:space:]]*//' | grep -q .; then
    record_fail "$label（代码里不该出现：$pattern）"
  else
    record_pass "$label"
  fi
}

# 反向断言：界面上不该出现东西，和"该出现"一样重要。
expect_missing() {
  local file="$1"
  local pattern="$2"
  local label="$3"
  if grep -qF -- "$pattern" "$file"; then
    record_fail "$label（不该出现：$pattern）"
  else
    record_pass "$label"
  fi
}

# ---- PR #20 远程备份页 ----
#
# 这一页是 GUI 与远程备份网络层之间**唯一**的入口。下面这几条把它钉住：
# 页面进资源清单、导航只有一个入口、密码框是密码回显、删除必须经过确认、
# 进度条绑的是网络层的真实字节数。
REMOTE_PAGE_QML="$QML_DIR/pages/RemotePage.qml"
SEGMENTED_QML="$QML_DIR/components/SegmentedTabs.qml"
CLIENT_CPP="$ROOT_DIR/src/network/remote_backup_client.cpp"
CLIENT_H="$ROOT_DIR/include/remote_backup_client.h"
REMOTE_CARD_QML="$QML_DIR/components/RemoteSnapshotCard.qml"
REMOTE_CONTROLLER_H="$ROOT_DIR/ui/modern/remote_controller.h"
REMOTE_CONTROLLER_CPP="$ROOT_DIR/ui/modern/remote_controller.cpp"
expect_count "$RESOURCE_FILE" "qml/pages/RemotePage.qml" 1 \
  "resources.qrc 收录 RemotePage.qml"
expect_count "$RESOURCE_FILE" "qml/components/RemoteSnapshotCard.qml" 1 \
  "resources.qrc 收录 RemoteSnapshotCard.qml"
expect_count "$QML_DIR/Main.qml" 'objectName: "remoteNavItem"' 1 \
  "侧栏只有一个远程备份入口"
# 明文常显的密码框在这一页是绝不允许出现的样子。
# 登录密码 + 注册密码 + 注册确认密码 + 注销确认密码 + 原始归档的恢复密码：
# 五个都必须是密码回显。
expect_count "$REMOTE_PAGE_QML" "echoMode: TextInput.Password" 5 \
  "远程备份页的四个密码框都是密码回显模式"
# 删除必须经过确认：整页真正调用客户端删除的地方只有一处，
# 而且列表行只发意图（一个信号声明 + 一个触发）。
expect_count "$REMOTE_PAGE_QML" "remote.deleteSnapshot(" 1 \
  "整页只有一处真正调用删除"
expect_count "$REMOTE_CARD_QML" "deleteRequested(" 2 \
  "列表行只发删除意图（声明 + 触发）"
expect_count "$REMOTE_PAGE_QML" "deleteDialog.open()" 1 \
  "删除先打开确认对话框"
expect_count "$REMOTE_PAGE_QML" 'objectName: "remoteDeleteDialog"' 1 \
  "删除确认对话框存在"
# 这一页的临时提示属于它自己：离开即消费，不污染其它页面。
expect_count "$REMOTE_PAGE_QML" 'pageScope: "remote"' 1 \
  "远程备份页的状态栏声明了 pageScope"
expect_count "$QML_DIR/Main.qml" "remote.clearStatus()" 1 \
  "离开远程备份页时消费掉它的临时提示"
# 进度条必须绑网络层给出的真实比例。
expect_count "$REMOTE_PAGE_QML" "value: remote.progressRatio" 1 \
  "进度条绑的是网络层的真实字节比例"
expect_count "$REMOTE_PAGE_QML" "remote.transferActive" 1 \
  "进度只在真的有传输时出现"
# 复用现有组件，而不是另起一套视觉。
expect_count "$REMOTE_PAGE_QML" "AppCard {" 5 \
  "远程备份页的五张卡片都用共享 AppCard（连接 / 远端备份 / 云端备份 / 高级 / 技术详情）"
expect_count "$REMOTE_PAGE_QML" "StatusBanner {" 1 \
  "远程备份页用共享 StatusBanner"
# 地址 / 端口 / 用户名 / 服务器身份指纹 / 登录密码 / 注册密码 / 注册确认 /
# 远端备份源目录 / 上传路径 / 上传名称 / 下载目标 / 注销密码 / 注销账户名 /
# 原始归档「尝试恢复」的目标目录 / 恢复密码 = 15。
expect_count "$REMOTE_PAGE_QML" "AppTextField {" 15 \
  "远程备份页的输入框都用共享 AppTextField"
# PR #21：客户端连接之前**必须**有服务端传输身份的 pin。它不是口令（公钥与
# 指纹都可以公开），但它是必填的连接配置：页面上有自己的输入框与明确的提交
# 动作，校验走共享解析器——界面里不许出现第二套"看起来像指纹"的判断。
# 错误写在这个输入框下面那一行：不弹对话框，也不占用页面底部的横幅。
expect_count "$REMOTE_PAGE_QML" 'objectName: "remoteServerKeyPinField"' 1 \
  "连接设置区有服务器身份指纹输入框"
expect_count "$REMOTE_PAGE_QML" 'objectName: "remoteServerKeyPinApplyButton"' 1 \
  "指纹有明确的提交动作（「应用」按钮）"
# PR #22：连接方式 / SSH 安全通道 / pin 应用反馈。这三块是这一轮的产品
# 增量，QML 少了任何一个控件，界面上的"部署链路"就缺一角。
expect_count "$REMOTE_PAGE_QML" 'objectName: "remoteConnectionModeTabs"' 1 \
  "远程页有连接方式选择（SSH 安全通道 / 直接连接）"
expect_count "$REMOTE_PAGE_QML" 'objectName: "remoteSshHostField"' 1 \
  "远程页有 SSH 主机输入框"
expect_count "$REMOTE_PAGE_QML" 'objectName: "remoteSshLocalPortField"' 1 \
  "远程页有本地端口输入框（留空 = 自动分配）"
expect_count "$REMOTE_PAGE_QML" 'objectName: "remoteTunnelStateText"' 1 \
  "远程页有安全通道状态行"
expect_count "$REMOTE_PAGE_QML" 'objectName: "remoteTunnelFailureText"' 1 \
  "远程页有通道失败原因行（不是笼统的网络错误）"
expect_count "$REMOTE_PAGE_QML" 'objectName: "remoteEnsureConnectionButton"' 1 \
  "远程页有“建立连接”按钮"
expect_count "$REMOTE_PAGE_QML" 'objectName: "remoteStopTunnelButton"' 1 \
  "远程页有“关闭安全通道”按钮"
expect_count "$REMOTE_PAGE_QML" 'objectName: "remoteServerKeyPinApplied"' 1 \
  "远程页有 pin“已应用”的可见反馈行"
expect_count "$REMOTE_PAGE_QML" 'objectName: "remoteServerKeyPinDirty"' 1 \
  "远程页有“尚未应用”提示（输入框与生效值不一致时必须说出来）"
# "应用"必须调用**会留下反馈**的那一个入口：旧实现只调 setServerKeyPin 并把
# 返回值丢掉，于是点完"应用"界面上什么都不发生（人工验收发现的 UX bug）。
expect_count "$REMOTE_PAGE_QML" 'remote.applyServerKeyPin' 2 \
  "“应用”按钮与回车都走 applyServerKeyPin（有可见反馈）"
expect_count "$REMOTE_PAGE_QML" 'remote.loginWithPin' 2 \
  "登录（按钮 + 回车）走 loginWithPin：自动采用当前输入框里的指纹"
expect_count "$REMOTE_PAGE_QML" 'remote.registerAccountWithPin' 1 \
  "注册走 registerAccountWithPin：同样自动采用当前输入框里的指纹"
expect_count "$REMOTE_PAGE_QML" 'remote.setServerKeyPin' 0 \
  "页面上不再直接调用 setServerKeyPin（那条路径没有反馈）"
expect_count "$REMOTE_PAGE_QML" 'objectName: "remoteServerKeyPinError"' 1 \
  "指纹输入框有自己的错误行"
expect_count "$REMOTE_PAGE_QML" "remote.setServerKeyPin(" 2 \
  "回车与「应用」两条路径都调用 remote.setServerKeyPin()"
expect_count "$REMOTE_PAGE_QML" "remote.serverKeyPinError" 2 \
  "指纹错误行绑到控制器的 serverKeyPinError（可见性 + 文本）"
expect_count "$REMOTE_CONTROLLER_H" "QString serverKeyPin() const" 1 \
  "控制器提供 serverKeyPin（界面回读 + 自检比对用同一个值）"
expect_count "$REMOTE_CONTROLLER_CPP" "backupproject::net::ParseServerKeyPin" 1 \
  "指纹的校验复用共享解析器（只认带前缀的两种写法）"
expect_count "$REMOTE_CONTROLLER_CPP" \
  "request.endpoint.server_key_pin = serverKeyPin()" 11 \
  "十一处提交点每一处都带上 pin（含远端备份 / 链恢复 / 原始归档恢复的第一次与重试；漏一处就等于那条操作没有 pin）"
# PR #21 UI closure（第二轮）：三种类型（原始归档 / 完整备份 / 增量备份）的主操作
# 都叫"恢复"。内部走哪条流水线由 badge 与说明行交代，**不**写进按钮名字——
# 把"这次能不能成"这种内部不确定性放进按钮是上一版的做法，本轮删掉。
if grep -rn "尝试恢复" ui/modern/qml ui/modern/remote_controller.h ui/modern/remote_controller.cpp >/dev/null 2>&1; then
  record_fail "生产界面里又出现了“尝试恢复”（旧文案）：$(grep -rn '尝试恢复' ui/modern/qml ui/modern/remote_controller.h ui/modern/remote_controller.cpp | head -3 | tr '\n' ' ')"
else
  record_pass "生产 QML 与控制器里没有任何“尝试恢复”：三种类型的主操作统一是“恢复”"
fi
# 卡片按钮的文案来自控制器的 presentation model，不在这里另写一个词。
expect_count "$REMOTE_CONTROLLER_CPP" 'item.insert(QStringLiteral("restoreLabel"), QStringLiteral("恢复"));' 1 \
  "主操作文案由控制器统一下发（三种类型同一个词）"
# 密码只有在 core 明确说"这份归档加密了"之后才出现：对话框有两段。
expect_count "$REMOTE_PAGE_QML" 'readonly property bool passwordStage: remote.rawRestoreAwaitingPassword' 1 \
  "恢复对话框的密码段由控制器回来的事实驱动（不是一上来就显示）"
expect_count "$REMOTE_PAGE_QML" '"此备份已加密，请输入恢复密码。' 1 \
  "密码段有明确的一句话（core 说要密码之后才可能看到）"
expect_count "$REMOTE_PAGE_QML" '"继续恢复"' 1 \
  "密码段的确认按钮是“继续恢复”"
# 第二套 socket / 协议实现？GUI 这一侧只允许经 RemoteController 调共享客户端。
# 断言只看代码行：注释里说明"这里没有 socket"是正常的。
REMOTE_CODE_TMP="$TEST_STATE_DIR/remote-code.txt"
{
  sed 's://.*::' "$REMOTE_CONTROLLER_H"
  sed 's://.*::' "$REMOTE_CONTROLLER_CPP"
  sed 's://.*::' "$REMOTE_PAGE_QML"
  sed 's://.*::' "$REMOTE_CARD_QML"
} > "$REMOTE_CODE_TMP"
if grep -qE 'sys/socket\.h|netinet/in\.h|arpa/inet\.h|AF_INET|::socket\(|::send\(|::recv\(|FrameHeader|kProtocolMagic|Opcode::' "$REMOTE_CODE_TMP"; then
  record_fail "GUI 里出现了第二套 socket / 协议实现"
else
  record_pass "GUI 只经 RemoteController 调共享的 RemoteArchiveClient"
fi
expect_count "$REMOTE_CONTROLLER_H" "remote_backup_client.h" 1 \
  "RemoteController 复用共享客户端头"
# 口令与会话令牌只在内存里：这一侧不许有任何持久化调用。
if grep -qE 'QSettings|setValue\(|QStandardPaths::writableLocation' "$REMOTE_CODE_TMP"; then
  record_fail "RemoteController 或远程备份页里出现了持久化调用"
else
  record_pass "口令与令牌只在内存：GUI 侧没有任何持久化调用"
fi
expect_count "$REMOTE_CONTROLLER_CPP" "password_.fill(QChar(0))" 3 \
  "退出登录、注销账户与析构都会擦掉内存里的口令"

# ---- PR #20 closure：账户区域（登录 / 注册两个标签页）----
#
# 人工验收的结论：用户名 / 密码 / 注册 / 登录 / 退出登录堆在同一块里，用户分不清
# "我在登录还是在注册"，注册也只有一个密码框。这一节把新的信息架构钉成契约。
# 账户区域是一个**分段控件**：一个圆角容器 + 两个等宽分段。人工验收的结论是
# 两个各自独立的按钮看起来像"可以同时按"，不像"二选一"。
expect_count "$REMOTE_PAGE_QML" "SegmentedTabs {" 2 \
  "账户区域与远端备份策略都用共享的分段控件（不是各自独立的按钮）"
expect_count "$REMOTE_PAGE_QML" 'objectName: "remoteAccountTabs"' 1 \
  "分段控件有 objectName（自动化要能点到它）"
expect_count "$RESOURCE_FILE" "qml/components/SegmentedTabs.qml" 1 \
  "SegmentedTabs.qml 进了资源清单"
expect_count "$SEGMENTED_QML" "theme.accent" 2 \
  "选中分段用强调色（与 primary 按钮同一个 token）"
expect_count "$SEGMENTED_QML" "theme.surface" 2 \
  "容器用次级按钮的中性底色（浅色=较深灰 / 深色=较亮灰，由 token 决定）"
expect_count "$SEGMENTED_QML" "theme.border" 2 \
  "容器与分段边框用共享的 border token"
expect_count "$SEGMENTED_QML" "theme.hover" 2 \
  "未选中分段的 hover 用共享的 hover token"
expect_count "$SEGMENTED_QML" "radius: 9" 1 \
  "外圆角与 AppButton 一致（9）"
expect_count "$SEGMENTED_QML" '"#ffffff"' 1 \
  "选中分段的文字用白字（与 primary 按钮一致）"
expect_missing "$SEGMENTED_QML" "theme.dark" \
  "分段控件不自己判断主题：两套配色都走 token"
if grep -nE '#[0-9a-fA-F]{6}' "$SEGMENTED_QML" | grep -v '#ffffff' | grep -q .; then
  record_fail "分段控件硬编码了颜色（除了强调色上的白字）"
else
  record_pass "分段控件不硬编码任何颜色（除了强调色上的白字）"
fi
expect_count "$REMOTE_PAGE_QML" 'objectName: "remoteRegisterPasswordField"' 1 \
  "注册标签有独立的密码输入框"
expect_count "$REMOTE_PAGE_QML" 'objectName: "remoteRegisterConfirmField"' 1 \
  "注册标签有「确认密码」输入框"
expect_count "$REMOTE_PAGE_QML" '"确认密码"' 1 \
  "确认密码的标签是中文的「确认密码」"
expect_count "$REMOTE_PAGE_QML" 'objectName: "remoteAccountText"' 1 \
  "已登录时显示当前账户"
expect_count "$REMOTE_PAGE_QML" 'objectName: "remoteAccountStateText"' 1 \
  "已登录时显示「状态：已登录」"
expect_count "$REMOTE_PAGE_QML" 'objectName: "remoteDeleteAccountButton"' 1 \
  "已登录时提供注销账户入口"
expect_count "$REMOTE_PAGE_QML" 'objectName: "remoteDeleteAccountDialog"' 1 \
  "注销账户有独立的确认对话框"
expect_count "$REMOTE_PAGE_QML" 'objectName: "remoteDeletePasswordField"' 1 \
  "注销对话框要求再次输入当前密码"
expect_count "$REMOTE_PAGE_QML" 'objectName: "remoteDeleteNameField"' 1 \
  "注销对话框要求逐字输入当前账户名"
expect_present "$REMOTE_PAGE_QML" \
  "注销账户会永久删除该账户以及全部云端备份，此操作无法撤销。" \
  "注销对话框写明了不可撤销的后果"
expect_count "$REMOTE_PAGE_QML" "remote.deleteAccount(" 1 \
  "整页只有一处真正调用注销"
expect_count "$REMOTE_PAGE_QML" "remote.logoutLocal()" 1 \
  "「退出登录」只有一处，与注销是两个不同的动作"
# 注册页的正常状态只给一句弱化的辅助文字；只有真的不一致时才换成红色错误。
expect_count "$REMOTE_PAGE_QML" "请再次输入密码以确认。" 1 \
  "注册标签的正常状态只有一句弱化的辅助文字"
expect_missing "$REMOTE_PAGE_QML" "不会发送任何请求" \
  "页面不再写开发者式的说明"
expect_count "$REMOTE_PAGE_QML" "两次输入的密码不一致" 1 \
  "不一致时明确写出「两次输入的密码不一致」"
# 六处错误行（注册不一致 / 注册被拒 / 登录被拒 / 注销失败 / 服务器身份指纹 /
# 原始归档恢复的密码错误行）都用共享的 error 色：页面里没有第二套红色，也没有
# 硬编码的 #ff0000。最后那一处是本轮新增的：错密码必须**就地**告诉用户，
# 而不是弹一个通用失败框。
expect_count "$REMOTE_PAGE_QML" "color: theme.error" 6 \
  "六处错误行都用共享的 error 色"
expect_present "$REMOTE_PAGE_QML" "visible: page.registerPasswordMismatch" 1 \
  "错误行由「两次密码是否一致」这个计算属性驱动（改一个字符就更新）"
expect_present "$REMOTE_PAGE_QML" "page.draftRegisterPassword !== page.draftConfirmPassword" 1 \
  "不一致是本地判定的（不发网络请求）"
# 注销对话框：失败原因必须出现在对话框内部，而且只有成功才关闭。
expect_count "$REMOTE_PAGE_QML" 'objectName: "remoteDeleteAccountError"' 1 \
  "注销对话框里有自己的错误行"
expect_count "$REMOTE_PAGE_QML" "remote.deleteAccountError" 2 \
  "错误行绑到控制器的 deleteAccountError（可见性 + 文本）"
expect_missing "$REMOTE_PAGE_QML" "if (remote.deleteAccount(" \
  "确认注销不再在提交时就关闭对话框"
expect_count "$REMOTE_PAGE_QML" "deleteAccountDialog.close()" 2 \
  "对话框只由「取消」和「操作成功」两条路径关闭"
# 每一次主动操作都要在**触发它的那个位置**给出反馈（人工验收的核心标准）。
#
#   * 登录失败 -> 登录表单下面（remoteLoginError）
#   * 注册失败 -> 注册表单下面（remoteRegisterError）
#   * 注销失败 -> 对话框内部（remoteDeleteAccountError）
#   * 服务器身份指纹没填 / 写错 -> 那个输入框下面（remoteServerKeyPinError）
#   * 页面级操作（上传 / 下载 / 刷新 / 删除云端备份）-> 页面底部横幅
#
# 旧实现把登录 / 注册的校验失败写进页面底部的横幅（甚至只打终端日志），于是
# 用户看到的是"我点了，但不知道程序到底有没有反应"。
expect_count "$REMOTE_PAGE_QML" 'objectName: "remoteLoginError"' 1 \
  "登录表单有自己的错误行"
expect_count "$REMOTE_PAGE_QML" 'objectName: "remoteRegisterError"' 1 \
  "注册表单有自己的错误行"
expect_count "$REMOTE_PAGE_QML" "remote.loginError" 2 \
  "登录错误行绑到控制器的 loginError（可见性 + 文本）"
expect_count "$REMOTE_PAGE_QML" "remote.registerError" 4 \
  "注册错误行绑到控制器的 registerError（可见性 + 文本 + 两条互斥提示）"
expect_count "$REMOTE_PAGE_QML" "remote.clearLoginError()" 4 \
  "改动地址 / 端口 / 用户名 / 登录密码都会清掉登录错误行"
expect_count "$REMOTE_PAGE_QML" "remote.clearRegisterError()" 5 \
  "改动地址 / 端口 / 用户名 / 两个注册密码框都会清掉注册错误行"
expect_count "$REMOTE_PAGE_QML" "remote.clearDeleteAccountError()" 3 \
  "打开对话框与改动对话框里的两个输入框都会清掉对话框错误行"
# 冗余状态文本：账户卡片已经写了"当前账户：X"和"状态：已登录"，页面上不允许
# 再有第三行重复同一个事实（人工验收点名的那一行）。
expect_missing "$REMOTE_PAGE_QML" 'objectName: "remoteSessionText"' \
  "页面上没有第三行重复的登录状态文本"
expect_count "$REMOTE_PAGE_QML" "remote.sessionText" 1 \
  "sessionText 只留在默认折叠的「技术详情」里"
# 对话框里的错误行必须在**对话框内部**（在对话框起点之后、确认按钮之前）。
DIALOG_LINE="$(grep -n 'objectName: "remoteDeleteAccountDialog"' "$REMOTE_PAGE_QML" | head -1 | cut -d: -f1)"
DIALOG_ERROR_LINE="$(grep -n 'objectName: "remoteDeleteAccountError"' "$REMOTE_PAGE_QML" | head -1 | cut -d: -f1)"
DIALOG_CONFIRM_LINE="$(grep -n 'objectName: "remoteDeleteAccountConfirmButton"' "$REMOTE_PAGE_QML" | head -1 | cut -d: -f1)"
if [ -n "$DIALOG_LINE" ] && [ -n "$DIALOG_ERROR_LINE" ] && [ -n "$DIALOG_CONFIRM_LINE" ] \
   && [ "$DIALOG_LINE" -lt "$DIALOG_ERROR_LINE" ] \
   && [ "$DIALOG_ERROR_LINE" -lt "$DIALOG_CONFIRM_LINE" ]; then
  record_pass "注销失败的错误行在对话框内部（对话框之后、确认按钮之前）"
else
  record_fail "对话框错误行的位置" \
    "dialog=$DIALOG_LINE error=$DIALOG_ERROR_LINE confirm=$DIALOG_CONFIRM_LINE"
fi
# 控制器一侧：错误按位置路由，校验失败不再写横幅，操作结束横幅不留运行状态。
expect_present "$REMOTE_CONTROLLER_H" "enum class ErrorSurface" \
  "控制器把「错误该出现在哪里」写成一个枚举（登录 / 注册 / 对话框 / 横幅）"
expect_present "$REMOTE_CONTROLLER_H" "QString loginError() const" \
  "控制器提供 loginError 给登录表单"
expect_present "$REMOTE_CONTROLLER_H" "QString registerError() const" \
  "控制器提供 registerError 给注册表单"
expect_present "$REMOTE_CONTROLLER_CPP" "void RemoteController::ReportSurfaceError" \
  "错误只有一条上报路径：ReportSurfaceError"
expect_present "$REMOTE_CONTROLLER_CPP" "SurfaceFailureMessage(result.kind, result.error_kind, result.message)" \
  "失败信息按操作改写（登录 / 注册 / 注销各说各的话）"
expect_present "$REMOTE_CONTROLLER_CPP" "SetIdleBaseline();" \
  "操作结束后横幅回到当前真实状态的基线（不会停在「正在登录」）"
expect_missing "$REMOTE_CONTROLLER_CPP" "bool RemoteController::AcceptEndpoint" \
  "旧的 AcceptEndpoint 已经删除（它把校验失败写进页面底部横幅）"
expect_missing "$REMOTE_CONTROLLER_CPP" "bool RemoteController::AcceptPassword" \
  "旧的 AcceptPassword 已经删除"
expect_present "$REMOTE_CONTROLLER_CPP" "该用户名已被使用，请更换用户名" \
  "重复注册的文案是「该用户名已被使用，请更换用户名」"
expect_present "$REMOTE_CONTROLLER_CPP" "当前密码不正确，账户与全部云端备份都没有被删除" \
  "注销失败明确说明「账户与全部云端备份都没有被删除」"
# 用户名校验的**原因**：核心决定原因，界面只负责翻译。旧实现把"长度不合法"
# 与"字符不合法"合成一句固定文案，人工验收里输入 "W" 被误导成"字符有问题"。
expect_present "$ROOT_DIR/include/network_protocol.h" "enum class UsernameValidation" \
  "用户名校验有结构化的原因（empty / 太短 / 太长 / 字符非法 / ok）"
expect_present "$ROOT_DIR/include/network_protocol.h" "UsernameValidation ValidateUsername" \
  "共享头暴露 ValidateUsername（规则的唯一实现）"
expect_present "$ROOT_DIR/src/network/network_protocol.cpp" "UsernameValidation ValidateUsername" \
  "实现只有一份：ValidateUsername 决定原因"
expect_present "$ROOT_DIR/src/network/network_protocol.cpp" "bool IsValidUsername" \
  "IsValidUsername 仍然存在（兼容包装，行为不变）"
expect_present "$REMOTE_CONTROLLER_CPP" "backupproject::net::ValidateUsername" \
  "控制器用结构化校验器，不自己判断长度或字符集"
expect_count "$REMOTE_CONTROLLER_CPP" "用户名长度需要为" 1 \
  "长度原因有自己的一句话（只写一处）"
expect_count "$REMOTE_CONTROLLER_CPP" "用户名只能包含字母、数字、点、下划线或减号" 1 \
  "字符集原因有自己的一句话（与长度那句不是同一句）"
expect_missing "$REMOTE_PAGE_QML" "用户名长度" \
  "QML 不自己判断用户名校验（文案由控制器给出）"
expect_missing "$REMOTE_PAGE_QML" "用户名只能包含" \
  "QML 里没有第二份用户名校验文案"
# 传输层：请求生命周期与连接生命周期分开，且**没有**自动重发。
expect_present "$CLIENT_CPP" "PrepareConnection(&prepare_error)" 1 \
  "发请求之前先准备连接（重连 / 恢复会话都发生在发送之前）"
expect_present "$CLIENT_H" "bool session_resumable() const" 1 \
  "客户端能区分「连接断了」与「token 也没了」"
expect_present "$CLIENT_CPP" "Opcode::kResume" 1 \
  "新连接上用 RESUME 恢复会话，而不是让用户重新登录"
expect_missing "$CLIENT_CPP" "retry" \
  "客户端里没有 retry 逻辑：失败绝不自动重发"
expect_present "$ROOT_DIR/include/network_protocol.h" "kResume = 5" \
  "RESUME 是协议里的一个新操作码（token 说明它恢复的是谁）"
expect_count "$ROOT_DIR/include/network_protocol.h" "kResume = 5" 1 \
  "RESUME 的操作码值只定义一次"
# 本地校验：不一致时不发请求（检查在控制器里，且在 AcceptEndpoint 之前）。
expect_present "$REMOTE_CONTROLLER_CPP" "password != confirm_password" \
  "控制器在提交之前比较两次注册密码"
expect_present "$REMOTE_CONTROLLER_H" "const QString& confirm_password" \
  "registerAccount 的签名带确认密码"
expect_present "$ROOT_DIR/include/remote_backup_client.h" \
  "bool DeleteAccount(const std::string& password" \
  "共享客户端有真正的注销账户入口（CLI 与 GUI 共用）"
# 状态语义：不许再把"还没有连接"写成"未连接 / 已连接"。
expect_missing_code "$REMOTE_CONTROLLER_CPP" '"未连接"' \
  "控制器里不再有「未连接」这种常驻状态文案"
expect_missing "$REMOTE_PAGE_QML" "未连接" \
  "远程备份页不再显示「未连接」"
expect_missing "$REMOTE_PAGE_QML" "已连接" \
  "远程备份页不再显示「已连接」"
expect_present "$REMOTE_CONTROLLER_CPP" "serverReachabilityText" \
  "可达性是「上一次连接尝试的结果」，只有试过才有结论"
expect_present "$REMOTE_PAGE_QML" "remote.serverReachabilityText" \
  "可达性文案由控制器给出（QML 不自己判断）"
expect_present "$REMOTE_CONTROLLER_H" "RemoteReachability::kUnknown" \
  "可达性默认是「还不知道」，而不是「不可达」"
for page in BackupPage BackupManagementPage SettingsPage; do
  expect_count_re "$RESOURCE_FILE" "qml/pages/${page}\.qml" 1 "resources.qrc 收录 $page.qml"
done

# 备份页：源目录输入框 + 浏览 + 更改仓库 + 开始备份 = 4 处绑定 !controller.busy。
# 计数式断言同时防"漏绑 busy"和"复制粘贴出多余按钮"。
# "更改仓库"属于这一组：任务期间不给跳走，与管理页的"刷新"用同一条禁用规则。
expect_count "$QML_DIR/pages/BackupPage.qml" "enabled: !controller.busy" 4 \
  "备份页忙碌时禁用输入与按钮"
# 设置页：仓库输入框 + 浏览目录 + 保存设置 = 3 处。
expect_count "$QML_DIR/pages/SettingsPage.qml" "enabled: !controller.busy" 3 \
  "设置页忙碌时禁用输入与按钮"
# 管理页刷新按钮：列表刷新期间与数据操作期间都不能重复点。
expect_count "$QML_DIR/pages/BackupManagementPage.qml" \
  "enabled: !controller.catalogBusy && !controller.busy" 1 \
  "管理页刷新按钮在刷新或操作期间禁用"
# 记录卡片上的忙碌开关：恢复 / 删除 / 恢复密码输入 / 恢复密码确认共 4 处。
# 数量从 2 涨到 4 是 PR #16 加了加密恢复对话框 —— 多出来的两处同样必须
# 绑 busy，否则任务进行中密码框还能被编辑。
expect_count "$QML_DIR/components/BackupRecordCard.qml" "card.busy" 4 \
  "备份记录卡片的恢复 / 删除 / 密码输入 / 密码确认受忙碌状态约束"

# QML 与核心的分工：界面只调用控制器，不自己持有核心对象、不拼路径。
# 备份页从 PR #16 起走带算法选项的入口；PR #18 之后那个入口多带一个策略，
# 名字也随之变成 startBackupWithStrategy()。startBackup() / startBackupWithOptions()
# 在控制器内部仍然存在（旧调用方与自测用），但**产品页面**只走带策略的那一个。
expect_count_re "$QML_DIR/pages/BackupPage.qml" 'controller\.startBackupWithStrategy\(' 1 \
  "备份页调用 controller.startBackupWithStrategy()"
# 反向也钉死：产品页不再直接调用不带选项的旧入口。0 次是硬要求 ——
# 少了这一条，"两个入口都被调用"这种半迁移状态照样能通过。
expect_count_re "$QML_DIR/pages/BackupPage.qml" 'controller\.startBackup\(\)' 0 \
  "备份页不再直接调用 controller.startBackup()"
expect_count_re "$QML_DIR/pages/SettingsPage.qml" 'controller\.saveRepositoryPath\(' 1 \
  "设置页调用 controller.saveRepositoryPath()"
expect_count_re "$QML_DIR/pages/BackupManagementPage.qml" 'controller\.refreshBackups\(\)' 1 \
  "管理页调用 controller.refreshBackups()"
expect_count_re "$QML_DIR/pages/BackupManagementPage.qml" 'BackupRecordCard' 1 \
  "管理页的列表项是 BackupRecordCard"
expect_count_re "$QML_DIR/components/BackupRecordCard.qml" 'controller\.startManagedRestore\(' 1 \
  "恢复动作调用 controller.startManagedRestore()（只传 file name）"
expect_count_re "$QML_DIR/components/BackupRecordCard.qml" 'controller\.deleteBackup\(' 1 \
  "删除动作调用 controller.deleteBackup()（只传 file name）"
# recognizedArchive 只表示"全局 header 可读"，不能当作"可以恢复"的充分条件；
# 界面上至少要把不认得的那些挡在恢复入口之外。
expect_count_re "$QML_DIR/components/BackupRecordCard.qml" \
  'enabled: !card\.busy && card\.recognized' 1 \
  "恢复按钮受 recognizedArchive 约束"

# 产品 QML 里不允许再出现"任意归档完整路径"这个概念。
if grep -rq 'backupFilePath' "$QML_DIR"; then
  record_fail "产品 QML 里仍然出现 backupFilePath"
else
  record_pass "产品 QML 里没有 backupFilePath（不再手填归档完整路径）"
fi
if grep -rq 'restorePath' "$QML_DIR"; then
  record_fail "产品 QML 里仍然出现 restorePath"
else
  record_pass "产品 QML 里没有 restorePath"
fi
# 界面不得自己拼 repository + file name：解析必须交给 BackupCatalog::Resolve。
if grep -rqE 'repositoryPath[[:space:]]*\+' "$QML_DIR"; then
  record_fail "QML 自己拼接了 repositoryPath"
else
  record_pass "QML 不拼接 repositoryPath（路径解析交给 Catalog）"
fi
# 界面不得自己读配置 JSON、不得直接引用核心类。
# 只认代码行：注释里解释"这里由 Catalog 负责解析"是正常的，不能算违规。
core_hits="$(grep -rnE 'ConfigManager|BackupCatalog|BackupEngine|QSettings' "$QML_DIR" \
  | grep -vE ':[0-9]+:[[:space:]]*//' || true)"
if [[ -n "$core_hits" ]]; then
  record_fail "QML 直接引用了核心类"
  printf '%s\n' "$core_hits" | sed 's/^/      /'
else
  record_pass "QML 不直接引用 ConfigManager / BackupCatalog / BackupEngine"
fi

# 规则编辑器的 UI 现在只有一份，在 components/FilterRuleEditor.qml 里（备份页 /
# 自动备份页 / 实时备份页共用）。面板只剩预览卡片的"刷新预览"仍然绑 controller.busy。
# 两处都钉死数量：漏绑 busy 和复制粘贴出多余按钮都会被这里挡住。
expect_count "$QML_DIR/components/FilterEditorPanel.qml" "enabled: !controller.busy" 1 \
  "备份页的刷新预览在忙碌时禁用"
expect_count "$QML_DIR/components/FilterRuleEditor.qml" "enabled: !editor.busy" 23 \
  "共享规则编辑器忙碌时禁用全部输入与按钮（23 处）"
# 规则卡片上的三个动作按钮（上移 / 下移 / 删除）沿用各自的忙碌开关。
expect_count "$QML_DIR/components/RuleCard.qml" "enabled: !card.busy" 3 \
  "规则卡片忙碌时禁用上移 / 下移 / 删除"

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
  "$WORK_DIR/source" "$WORK_DIR/backup.bak" "$WORK_DIR/restore" \
  --config-file "$TEST_CONFIG_FILE" >> "$LOG_FILE" 2>&1
selftest_status=$?
set -e
if [[ "$selftest_status" -eq 0 ]]; then
  record_pass "--self-test 打包与解包都成功"
else
  record_fail "--self-test 退出码 $selftest_status"
  tail -10 "$LOG_FILE"
fi
# 备份产物必须是一个普通归档文件，不能再是"目录里放 data"的老结构。
if [[ -f "$WORK_DIR/backup.bak" && ! -d "$WORK_DIR/backup.bak" ]]; then
  record_pass "备份产物是单个普通文件（不是目录）"
else
  record_fail "备份产物不是普通文件"
fi
# direct 入口（startDirectBackupForTest）必须继续产 legacy v0.1：
# magic = BKPARCH\0、version = 1。它与仓库驱动的正常备份是两条明确的格式路径，
# 这条断言把"旧格式没有被顺手一起切到 v2"钉死（CLI 默认也仍然走这条路）。
# 直接读产物字节，不看任何一方的自述。
selftest_magic="$(head -c 8 "$WORK_DIR/backup.bak" | tr -d '\000')"
selftest_version="$(od -An -j8 -N2 -tu2 "$WORK_DIR/backup.bak" | tr -d ' ')"
if [[ "$selftest_magic" == "BKPARCH" && "$selftest_version" == "1" ]]; then
  record_pass "direct 测试路径的产物仍是 legacy v0.1（BKPARCH\\0 + version 1）"
else
  record_fail "direct 测试路径的产物不是 legacy v0.1（magic=[$selftest_magic] version=[$selftest_version]）"
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
  ./build/backup-gui-modern --path-test \
  --config-file "$TEST_CONFIG_FILE" > /tmp/modern-gui-path.log 2>&1
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
# 关闭条件必须同时覆盖每一位 writer：手动备份 / 恢复是 controller.busy，计划评估
# 与实时触发跑在 QtConcurrent 上，落盘的是各自的 libraryBusy；PR #20 之后还要
# 加上远程传输（remote.busy）。只写 controller.busy 会漏掉"实时备份正在写归档
# 或正在上传到云端时 Alt+F4 能把窗口关掉"。
# 用正则版：expect_present 定义在本文件靠后的位置，而这一节在它之前执行。
# 条件现在跨两行，所以拆成两条：前三位在首行，远程那一位在续行。
expect_count_re "$QML_DIR/Main.qml" \
  'if \(controller\.busy \|\| schedule\.libraryBusy \|\| realtime\.libraryBusy$' 1 \
  "关窗条件覆盖手动 / 计划 / 实时三位本地 writer"
expect_count_re "$QML_DIR/Main.qml" \
  '^[[:space:]]*\|\| remote\.busy\) \{$' 1 \
  "远程传输进行中同样不允许关窗"
# 错误正文要能选中复制，核心给的长路径才有可能贴出来。
expect_count_re "$QML_DIR/components/StatusBanner.qml" "selectByMouse:[[:space:]]*true" 1 \
  "状态栏正文可鼠标选中"
# 运行期契约：忙时拒绝关闭并提示，任务结束后放行。自检会真的把手动 / 实时 /
# 计划三位 writer 各跑起来一次，所以这里必须给它自己的 config / schedule /
# realtime 文件：它会用真实入口写 config.json 与两份 store，不能碰别的用例的
# 路径，更不能碰用户真实的配置。
set +e
QT_QPA_PLATFORM=offscreen QSG_RHI_BACKEND=software timeout 600 \
  ./build/backup-gui-modern --close-guard-test \
  --config-file "$TEST_STATE_DIR/close-guard-config.json" \
  --schedule-file "$TEST_STATE_DIR/close-guard-schedule.json" \
  --realtime-file "$TEST_STATE_DIR/close-guard-realtime.json" \
  > /tmp/modern-gui-guard.log 2>&1
guard_status=$?
set -e
sed 's/^/[modern-gui]     /' /tmp/modern-gui-guard.log
cat /tmp/modern-gui-guard.log >> "$LOG_FILE"
if [[ "$guard_status" -eq 0 ]]; then
  record_pass "忙时关窗被拒绝并给出提示，任务结束后可正常关闭"
else
  record_fail "关闭守卫行为与预期不符"
fi
# 判别力：三段都必须真的跑过。只断言退出码的话，自检在第一段之后就退出
# （比如实时 worker 没起来）也会是 0 退出，而那个洞依然在。
for pattern in "实时 worker 在飞时 close() 被拒绝" \
               "实时 worker 结束后 close() 被接受" \
               "计划 worker 在飞时 close() 被拒绝" \
               "计划 worker 结束后 close() 被接受"; do
  if grep -qF -- "$pattern" /tmp/modern-gui-guard.log; then
    record_pass "关闭守卫自检覆盖：$pattern"
  else
    record_fail "关闭守卫自检缺少：$pattern"
  fi
done

# 人工验收提出的两条 GUI 契约：
#   * 首页三张卡片的按钮必须完整落在卡片内（固定 196 高度时底边距是 -7px，
#     按钮压在下边框上）；
#   * 临时提示属于产生它的页面：离开即消费，回来不自动复现，后台任务在别的
#     页面结束时也不会把完成提示丢过去。
# 运行期断言真实几何与真实绑定结果，不做截图比对。
set +e
QT_QPA_PLATFORM=offscreen QSG_RHI_BACKEND=software \
  ./build/backup-gui-modern --gui-contract-test \
  --config-file /tmp/modern-gui-contract.json \
  --schedule-file /tmp/modern-gui-contract-schedule.json \
  > /tmp/modern-gui-contract.log 2>&1
contract_status=$?
set -e
sed 's/^/[modern-gui]     /' /tmp/modern-gui-contract.log
cat /tmp/modern-gui-contract.log >> "$LOG_FILE"
if [[ "$contract_status" -eq 0 ]]; then
  record_pass "首页按钮几何与临时提示的页面归属都符合契约"
else
  record_fail "首页按钮几何或临时提示的页面归属不符合契约" \
    "$(grep -m2 'FAIL' /tmp/modern-gui-contract.log | tr '\n' ' ')"
fi

# 上面那条是运行期证据，这里再静态钉住结构：scope 的过滤必须写在状态栏里
# （而不是每页各写一份 if），四个业务页各自声明自己的 pageScope，消费动作只有
# 一处（Main.qml 的页面切换处理）。
expect_count_re "$QML_DIR/components/StatusBanner.qml" "property string pageScope" 1 \
  "状态栏区分这条消息属于哪一页"
expect_count_re "$QML_DIR/components/StatusBanner.qml" "scope === pageScope" 1 \
  "状态栏按 scope 过滤，severity 与 scope 正交"
expect_count_re "$QML_DIR/components/StatusBanner.qml" "readonly property bool showsMessage" 1 \
  "状态栏把该不该显示暴露成一个可断言的位"
for page_scope in 'backup:BackupPage' 'settings:SettingsPage' \
                  'management:BackupManagementPage' 'home:HomePage'; do
  scope_name="${page_scope%%:*}"
  page_file="${page_scope#*:}"
  expect_count_re "$QML_DIR/pages/${page_file}.qml" "pageScope: \"${scope_name}\"" 1 \
    "${page_file} 的状态栏声明了自己的页面作用域"
done
expect_count_re "$QML_DIR/Main.qml" "dismissPageStatus" 3 \
  "页面切换时消费离开页面的临时提示（只有一处实现）"

# PR #18：增量策略必须在两个前端都能选到，而且用的是同一个 key。
# 备份页读 backupOptionsPanel 的策略；计划页读它自己的策略下拉。
# 面板里 strategyKeys 出现三次：声明、indexOf 反查、以及 currentIndex 选择。
expect_count_re "$QML_DIR/components/BackupOptionsPanel.qml" "strategyKeys" 3 \
  "备份页提供备份策略选择"
expect_count_re "$QML_DIR/pages/BackupPage.qml" "startBackupWithStrategy" 1 \
  "备份页把策略一起交给控制器"
expect_count_re "$QML_DIR/pages/SchedulePage.qml" "scheduleStrategyCombo" 1 \
  "计划页提供备份策略选择"
expect_count_re "$QML_DIR/pages/SchedulePage.qml" "strategyKeys\[page.draftStrategyIndex\]" 1 \
  "计划页把策略一起交给控制器"
expect_count_re "$ROOT_DIR/ui/modern/schedule_controller.cpp" "ParseBackupStrategyKey" 1 \
  "计划控制器用共享的 key 解析，不自己判断策略"

echo "[modern-gui] 8) 文件筛选在 GUI 路径上生效"
# 界面只负责收集规则文本，解析与匹配都在 C++ Filter 里：
# 下面先做静态确认，再用 --self-test 走一遍真实控制器路径。
# 可视化编辑器的链路固定为：面板 -> FilterRuleModel -> BackupController -> 真实 Filter。
# 面板只跟 model 打交道，model 才调用控制器，所以断言按这个真实结构落在两处。
# 普通用户看到的是"添加包含规则 / 添加排除规则"，内部才落到 include / exclude。
expect_count_re "$QML_DIR/components/FilterRuleEditor.qml" "openBuilder\(\"include\"\)" 1 \
  "规则编辑器提供包含规则入口"
expect_count_re "$QML_DIR/components/FilterRuleEditor.qml" "openBuilder\(\"exclude\"\)" 1 \
  "规则编辑器提供排除规则入口"
expect_count_re "$QML_DIR/components/RuleCard.qml" "ruleModelRef.removeRule" 1 \
  "Modern GUI 可以删除规则（由规则卡片调用模型）"
expect_count_re "$QML_DIR/components/RuleCard.qml" "ruleModelRef.moveRule" 2 \
  "Modern GUI 可以上移 / 下移规则"
expect_count_re "$ROOT_DIR/ui/modern/filter_rule_model.cpp" "addFilterRule" 1 \
  "规则文本统一经模型提交给控制器"
expect_count_re "$ROOT_DIR/ui/modern/filter_rule_model.cpp" "clearFilterRules" 1 \
  "规则列表变化时整体重放给控制器"
expect_count_re "$ROOT_DIR/ui/desktop/operation_page.cpp" "AddFilterRule" 3 \
  "Classic GUI 也走同一套规则逻辑"
expect_count_re "$ROOT_DIR/ui/desktop/operation_page.cpp" "CollectFilterRules" 3 \
  "Classic GUI 把规则收集后交给核心"

# 元数据字段（uid / gid / user / group）与 type 的 7 个取值：表单与模型两层都要
# 真的有接线，否则界面上能看到字段名，规则却永远生成不出来。下面只查"这一项
# 存在且成对"，具体实现细节不钉死，避免把重构变成改断言。
# 字段下拉的内容不再写死在 QML 里：它读 FilterRuleModel::editorOptions()，而那张
# 表直接来自 FilterRuleBuilder 的中文名表。这样"界面上能选的条件"与"核心真的能
# 生成的条件"是同一份定义，不可能出现"下拉里有一项核心执行不了"。
if grep -qF 'ruleModel.editorOptions()' "$QML_DIR/components/FilterRuleEditor.qml"; then
  record_pass "条件类型下拉的选项来自共享 builder（QML 不再自己维护字段表）"
else
  record_fail "条件类型下拉没有走共享 builder 的选项表"
fi
for field_key in kUid kGid kUser kGroup kMtime; do
  if grep -qF "bp::RuleField::${field_key}" "$ROOT_DIR/ui/modern/filter_rule_model.cpp"; then
    record_pass "editorOptions 暴露了 ${field_key}"
  else
    record_fail "editorOptions 缺少 ${field_key}"
  fi
done
for type_key in kSymlink kFifo kCharDevice kBlockDevice kSocket; do
  if grep -qF "bp::RuleTypeValue::${type_key}" "$ROOT_DIR/ui/modern/filter_rule_model.cpp"; then
    record_pass "type 下拉含 ${type_key}"
  else
    record_fail "type 下拉缺 ${type_key}"
  fi
done
for id_key in eq lt le gt ge range; do
  if grep -qF "{\"${id_key}\"," "$ROOT_DIR/ui/modern/filter_rule_model.cpp"; then
    record_pass "uid / gid 比较运算符含 ${id_key}"
  else
    record_fail "uid / gid 比较运算符缺 ${id_key}"
  fi
done
if grep -qF '"uid_high": editor.formUidHighText' "$QML_DIR/components/FilterRuleEditor.qml"; then
  record_pass "表单把 uid / gid 的上下界一起交给模型"
else
  record_fail "表单没有提交 uid / gid 的区间上界"
fi
# 模型侧：四个新字段都必须真的映射成 FilterClauseDraft 的成员。
for rule_field in kUid kGid kUser kGroup kMtime; do
  if grep -qE "RuleField::${rule_field}\b" "$ROOT_DIR/ui/modern/filter_rule_model.cpp"; then
    record_pass "模型处理 RuleField::${rule_field}"
  else
    record_fail "模型没有处理 RuleField::${rule_field}"
  fi
done
# 字段分发必须有 default 兜底：以后再添 RuleField，也不会静默落进已有分支。
if grep -qE '^[[:space:]]*default:$' "$ROOT_DIR/ui/modern/filter_rule_model.cpp"; then
  record_pass "表单字段分发有 default 兜底"
else
  record_fail "表单字段分发没有 default 兜底"
fi
# 预览与真实 Backup 必须共用**同一套** filesystem 事实：遍历、元数据与
# Filter 判定都在 src/core/source_tree_walker.cpp 里，Preview 与 GUI 都不许再
# 有自己的一份。
#
# 断言的是结构而不是"某个字符串出现在哪个文件里"：谁提供遍历、谁只做投影。
# 上一轮把判定搬进 backup_preview.cpp 之后，这里的断言指的还是那个文件——
# 而这一轮遍历又往下沉了一层，所以断言跟着指向真正的唯一实现。
if grep -qF 'bp::PreviewBackupSelection(' "$ROOT_DIR/ui/modern/filter_rule_model.cpp"; then
  record_pass "GUI 预览委托给共享核心 PreviewBackupSelection"
else
  record_fail "GUI 预览没有走共享核心"
fi
if grep -qF 'WalkSourceTree(' "$ROOT_DIR/src/core/backup_preview.cpp" \
   && grep -qF 'WalkSourceTree(' "$ROOT_DIR/src/core/tree_scanner.cpp"; then
  record_pass "预览与真实 Backup 调用同一个共享遍历 WalkSourceTree"
else
  record_fail "预览与真实 Backup 没有共用同一份遍历"
fi
# 调用形式是 filter_->ShouldIncludeFile(...)（成员指针），所以只匹配方法名。
if grep -qF 'ShouldIncludeFile(' "$ROOT_DIR/src/core/source_tree_walker.cpp" \
   && grep -qF 'ShouldPruneDirectory(' "$ROOT_DIR/src/core/source_tree_walker.cpp" \
   && grep -qF 'ShouldSkipSpecialEntry(' "$ROOT_DIR/src/core/source_tree_walker.cpp"; then
  record_pass "共享遍历沿用真实 Filter 的三条判定（include / 剪枝 / 特殊文件）"
else
  record_fail "共享遍历没有走真实 Filter 判定"
fi
if grep -qF '::lstat(' "$ROOT_DIR/src/core/source_tree_walker.cpp" \
   && grep -qF '::opendir(' "$ROOT_DIR/src/core/source_tree_walker.cpp" \
   && grep -qF '::readdir(' "$ROOT_DIR/src/core/source_tree_walker.cpp"; then
  record_pass "共享遍历用 lstat / opendir / readdir（不跟随软链接）"
else
  record_fail "共享遍历缺少真实 syscall"
fi
if grep -qF 'ShouldIncludeFile(' "$ROOT_DIR/ui/modern/filter_rule_model.cpp" \
   || grep -qF 'recursive_directory_iterator' "$ROOT_DIR/ui/modern/filter_rule_model.cpp" \
   || grep -qF 'recursive_directory_iterator' "$ROOT_DIR/src/core/backup_preview.cpp" \
   || grep -qF '::lstat(' "$ROOT_DIR/src/core/backup_preview.cpp"; then
  record_fail "预览层又出现了自己的遍历或匹配逻辑"
else
  record_pass "预览层没有第二套遍历（遍历与判定只有共享那一份）"
fi
# 没有被排除的 socket 不是"某一行的标签"，而是整次预览的失败：三个前端都必须
# 走同一条 blocked 语义，而不是一边给警告、一边报成功。
if grep -qF 'kSelectionBlocked' "$ROOT_DIR/src/core/backup_preview.cpp" \
   && grep -qF 'kSelectionBlocked' "$ROOT_DIR/src/cli/cli_commands.cpp" \
   && grep -qF 'kSelectionBlocked' "$ROOT_DIR/ui/modern/main.cpp"; then
  record_pass "未排除的 socket 走 blocked 语义（核心 / CLI / GUI 一致）"
else
  record_fail "未排除的 socket 的 blocked 语义不完整"
fi
# 预览要能把各类条目分开说清楚，并且点出 socket 的后果。
for tag in '符号链接' 'FIFO' '字符设备' '块设备' 'socket（不支持归档）'; do
  if grep -qF "QStringLiteral(\"${tag}\")" "$ROOT_DIR/ui/modern/filter_rule_model.cpp"; then
    record_pass "预览能标注 ${tag}"
  else
    record_fail "预览缺 ${tag} 标注"
  fi
done
if grep -qF '被规则排除' "$ROOT_DIR/ui/modern/filter_rule_model.cpp" \
   && grep -qF '目录被排除（整棵剪掉）' "$ROOT_DIR/ui/modern/filter_rule_model.cpp"; then
  record_pass "预览区分被规则排除与目录剪枝"
else
  record_fail "预览缺排除 / 剪枝提示"
fi
# 窗口的说明必须与实际语义一致：列出的是"前 300 个预览条目"，不是"前 300 条
# 匹配项"；也不能说"整棵源目录树都会被检查"——被排除的目录不会递归进去，
# 真实 Backup 也不递归。UI 只改文字，不改布局。
if grep -qF '列表只显示前 ' "$QML_DIR/components/FilterEditorPanel.qml" \
   && grep -qF '完整执行与备份一致的筛选遍历' \
     "$QML_DIR/components/FilterEditorPanel.qml" \
   && ! grep -qF '整棵源目录树都会被检查' \
     "$QML_DIR/components/FilterEditorPanel.qml"; then
  record_pass "预览窗口的说明与实际语义一致（前 N 个预览条目 / 与备份同一次遍历）"
else
  record_fail "预览窗口的说明与实际语义不一致"
fi

# mtime 的 5 种形态：字段下拉、类型键、天数与两个日期都要真的接到模型上。
if grep -qF '{"mtime", bp::RuleField::kMtime}' "$ROOT_DIR/ui/modern/filter_rule_model.cpp"; then
  record_pass "条件类型下拉含“修改时间”（核心真的支持 mtime:）"
else
  record_fail "条件类型下拉缺“修改时间”"
fi
if grep -qF 'parse-time' "$ROOT_DIR/src/filter/filter.cpp"; then
  record_fail "核心多了未接线的解析分支"
else
  record_pass "没有为 UI 新造核心能力"
fi
for mtime_key in today yesterday last_days day day_range; do
  if grep -qF "{\"${mtime_key}\"," "$ROOT_DIR/ui/modern/filter_rule_model.cpp"; then
    record_pass "mtime 类型下拉含 ${mtime_key}"
  else
    record_fail "mtime 类型下拉缺 ${mtime_key}"
  fi
done
if grep -q -- '"mtime_kind": editor.formMtimeKind' "$QML_DIR/components/FilterRuleEditor.qml" \
   && grep -q -- '"days_back": editor.formDaysBackText' "$QML_DIR/components/FilterRuleEditor.qml" \
   && grep -q -- '"date_low": editor.formDateLow' "$QML_DIR/components/FilterRuleEditor.qml" \
   && grep -q -- '"date_high": editor.formDateHigh' "$QML_DIR/components/FilterRuleEditor.qml"; then
  record_pass "表单把 mtime 类型 / 天数 / 两个日期一起交给模型"
else
  record_fail "表单没有提交 mtime 的完整取值"
fi
# 5 种形态在模型里都要有落点；日期合法性由 builder / 真实 Filter 裁决。
for mtime_kind in kToday kYesterday kLastDays kDay kDayRange; do
  if grep -qF "RuleMtimeKind::${mtime_kind}" "$ROOT_DIR/ui/modern/filter_rule_model.cpp"; then
    record_pass "模型处理 mtime 形态 ${mtime_kind}"
  else
    record_fail "模型没有处理 mtime 形态 ${mtime_kind}"
  fi
done
if grep -qF 'mtime 还没有表单控件' "$ROOT_DIR/ui/modern/filter_rule_model.cpp"; then
  record_fail "模型里还留着 mtime 无控件的兜底错误"
else
  record_pass "模型里没有 mtime 无控件的兜底错误"
fi

FILTER_DIR="$WORK_DIR/filter-src"
mkdir -p "$FILTER_DIR/build"
printf 'cpp\n' > "$FILTER_DIR/a.cpp"
printf 'log\n' > "$FILTER_DIR/b.log"
mkfifo "$FILTER_DIR/build/pipe"
set +e
QT_QPA_PLATFORM=offscreen QSG_RHI_BACKEND=software \
  ./build/backup-gui-modern --self-test "$FILTER_DIR" "$WORK_DIR/filter.bak" \
  "$WORK_DIR/filter-out" --include 'ext:cpp' --exclude 'path:**/build/**' \
  --config-file "$TEST_CONFIG_FILE" >> "$LOG_FILE" 2>&1
filter_status=$?
set -e
if [[ "$filter_status" -eq 0 ]]; then
  record_pass "带筛选的打包 / 解包成功（被剪掉的子树里有 FIFO）"
else
  record_fail "带筛选的打包 / 解包失败（退出码 $filter_status）"
  tail -10 "$LOG_FILE"
fi
if [[ -f "$WORK_DIR/filter-out/a.cpp" && ! -e "$WORK_DIR/filter-out/b.log" \
      && ! -e "$WORK_DIR/filter-out/build" ]]; then
  record_pass "筛选结果正确：只保留 a.cpp"
else
  record_fail "筛选结果不符合预期"
  find "$WORK_DIR/filter-out" -mindepth 1 | sed 's/^/      /'
fi
set +e
QT_QPA_PLATFORM=offscreen QSG_RHI_BACKEND=software \
  ./build/backup-gui-modern --self-test "$FILTER_DIR" "$WORK_DIR/bad.bak" \
  "$WORK_DIR/bad-out" --include 'bogus:x' \
  --config-file "$TEST_CONFIG_FILE" >> "$LOG_FILE" 2>&1
bad_status=$?
set -e
if [[ "$bad_status" -ne 0 && ! -e "$WORK_DIR/bad.bak" ]]; then
  record_pass "非法规则在 GUI 路径上同样被拒绝，且不留归档"
else
  record_fail "非法规则没有被正确拒绝（退出码 $bad_status）"
fi

echo "[modern-gui] 9) repository-driven 产品链路（--repository-test）"
# 这一条测的是产品入口本身：仓库设置 -> 自动命名备份 -> 列表 -> 恢复 -> 删除。
# 它和上面的 --self-test 互补：那个测的是"归档路径由调用方指定"的 direct 路径，
# 这个测的是界面真正使用的那条 repository-driven 路径，全程不传 archive 路径。
REPO_WORK="${TEST_STATE_DIR}/repo-test"
rm -rf "$REPO_WORK"
REPO_SRC="$REPO_WORK/source"
REPO_DIR="$REPO_WORK/repository"
REPO_DEST="$REPO_WORK/restored"
REPO_CFG="$REPO_WORK/product-config.json"
REPO_LOG="${TEST_STATE_DIR}/repo-test.log"
# 产物会在 --repository-test 的最后一步被删掉，脚本要自己读它的字节，
# 就得让程序在删除之前留一份副本（BACKUP_MODERN_KEEP_ARTIFACT 是纯测试旁路）。
REPO_KEPT="$REPO_WORK/kept-artifact.bak"
mkdir -p "$REPO_SRC/sub" "$REPO_SRC/emptydir"
printf 'plain\n' > "$REPO_SRC/plain.txt"
printf '中文内容\n' > "$REPO_SRC/中文文件.txt"
printf 'space name\n' > "$REPO_SRC/with space.txt"
: > "$REPO_SRC/empty.txt"
printf 'nested\n' > "$REPO_SRC/sub/nested.txt"
# 界面展示 uid / gid / symlink / FIFO，产物就必须真的装得下它们：
# 源目录里放上软链接（指向文件与指向目录各一条）和一条 FIFO，
# 恢复后逐个查文件类型与 link target。
ln -s plain.txt "$REPO_SRC/symlink.txt"
ln -s sub "$REPO_SRC/dirlink"
mkfifo "$REPO_SRC/pipe"

set +e
BACKUP_MODERN_KEEP_ARTIFACT="$REPO_KEPT" \
  QT_QPA_PLATFORM=offscreen QSG_RHI_BACKEND=software timeout 180 \
  ./build/backup-gui-modern --repository-test "$REPO_SRC" "$REPO_DIR" "$REPO_DEST" \
  --config-file "$REPO_CFG" > "$REPO_LOG" 2>&1
repo_status=$?
set -e
sed 's/^/[modern-gui]     /' "$REPO_LOG"
cat "$REPO_LOG" >> "$LOG_FILE"
if [[ "$repo_status" -eq 0 ]]; then
  record_pass "--repository-test 全链路通过（保存仓库 / 自动命名备份 / 列表 / 恢复 / 删除）"
else
  record_fail "--repository-test 退出码 $repo_status"
fi

# 9.1 产物本身：magic / version / header size / 三个算法 id / entry_count 全部
# 从字节上读一遍。断言的是文件内容，不是程序的"成功"自述。
if [[ -f "$REPO_KEPT" ]]; then
  repo_magic="$(head -c 8 "$REPO_KEPT" | tr -d '\000')"
  repo_version="$(od -An -j8 -N2 -tu2 "$REPO_KEPT" | tr -d ' ')"
  repo_header_size="$(od -An -j10 -N2 -tu2 "$REPO_KEPT" | tr -d ' ')"
  read -r repo_pack repo_comp repo_enc \
    <<<"$(od -An -j12 -N3 -tu1 "$REPO_KEPT" | tr -s ' ' | sed 's/^ //')" || true
  repo_entries="$(od -An -j16 -N8 -tu8 "$REPO_KEPT" | tr -d ' ')"
  repo_entries_dec=$((10#${repo_entries:-0}))
  if [[ "$repo_magic" == "BKPCNT2" && "$repo_version" == "2" \
        && "$repo_header_size" == "160" ]]; then
    record_pass "正常备份产物是 v2 容器（magic=BKPCNT2\\0 / version=2 / headerSize=160）"
  else
    record_fail "正常备份产物不是 v2 容器（magic=[$repo_magic] version=[$repo_version] headerSize=[$repo_header_size]）"
  fi
  if [[ "$repo_pack" == "0" && "$repo_comp" == "0" && "$repo_enc" == "0" ]]; then
    record_pass "外层 header 是 MyPack + 不压缩 + 不加密（三个算法 id 全为 0）"
  else
    record_fail "外层 header 的算法 id 不是 0/0/0（pack=[$repo_pack] compression=[$repo_comp] encryption=[$repo_enc]）"
  fi
  if [[ "$repo_entries_dec" -gt 0 ]]; then
    record_pass "外层 header 的 entry_count=$repo_entries_dec > 0"
  else
    record_fail "外层 header 的 entry_count 不是正数（得到 [$repo_entries]）"
  fi
else
  record_fail "正常备份产物的副本缺失：$REPO_KEPT"
fi

# 9.2 同一份产物的结论必须和 C++ 侧 IdentifyArchiveFile、catalog 的 record 一致：
# 脚本按偏移读字节、核心解析 header，两条独立路径给出同一个答案才算数。
if grep -qE '^identify: kind=container-v2 formatVersion=2 packMethod=mypack compressionMethod=none encryptionMethod=none entryCount=[1-9][0-9]*$' "$REPO_LOG"; then
  record_pass "IdentifyArchiveFile 复核为 v2 / MyPack / None / None"
else
  record_fail "IdentifyArchiveFile 的结论不是 v2 / MyPack / None / None"
  grep -n '^identify:' "$REPO_LOG" | sed 's/^/      /'
fi
if grep -qE '^record: .*recognizedArchive=true formatVersion=2 entryCount=[1-9][0-9]*$' "$REPO_LOG"; then
  record_pass "BackupCatalog 认得它（recognized_archive=true，entry_count > 0）"
else
  record_fail "BackupCatalog 没有把它认成 v2 容器"
  grep -n '^record:' "$REPO_LOG" | sed 's/^/      /'
fi

# 配置文件必须真的产生，并且产生在我们指定的那个路径上 ——
# 这一条同时证明 --config-file 生效、真实用户配置没有被碰。
if [[ -f "$REPO_CFG" ]] && grep -q '"backup_repository_path"' "$REPO_CFG"; then
  record_pass "配置文件确实产生在 --config-file 指定的路径"
else
  record_fail "配置文件没有产生：$REPO_CFG"
fi

# 9.3 恢复出来的类型必须原样回来：软链接还是软链接、link target 一字不差、
# FIFO 还是 FIFO。这几条不能只靠 diff —— 不加 --no-dereference 的话 diff 会
# 跟着软链接去比目标内容，"链接被恢复成普通文件"这种退化恰好查不出来。
if [[ -L "$REPO_DEST/symlink.txt" \
      && "$(readlink "$REPO_DEST/symlink.txt")" == "plain.txt" ]]; then
  record_pass "恢复后 symlink.txt 仍是符号链接且 target 一致（plain.txt）"
else
  record_fail "恢复后 symlink.txt 不是指向 plain.txt 的符号链接"
fi
if [[ -L "$REPO_DEST/dirlink" \
      && "$(readlink "$REPO_DEST/dirlink")" == "sub" ]]; then
  record_pass "恢复后 dirlink 仍是指向目录的符号链接"
else
  record_fail "恢复后 dirlink 不是指向 sub 的符号链接"
fi
# FIFO 用普通用户就能建，所以这一条不需要"环境不允许就跳过"的借口。
if [[ -p "$REPO_DEST/pipe" ]]; then
  record_pass "恢复后 pipe 仍是 FIFO（S_ISFIFO）"
else
  record_fail "恢复后 pipe 不是 FIFO"
fi

# 恢复结果必须与源目录逐字节一致（含中文名、空格名、空文件、空目录、子目录）。
# --no-dereference：软链接按链接本身比较，target 不同即判差异。
# FIFO 用 --exclude 排掉：diff 无法比较 FIFO，它的类型已由上面的 test -p 断言。
if diff -r --no-dereference --exclude=pipe "$REPO_SRC" "$REPO_DEST" >> "$LOG_FILE" 2>&1; then
  record_pass "diff -r --no-dereference 源目录与恢复目录完全一致"
else
  record_fail "diff -r --no-dereference 源目录与恢复目录有差异"
fi

# 自动命名的产物：名字必须由核心按 <source-base>_YYYYMMDD_HHMMSS.bak 生成，
# 而不是用户在界面上指定的完整归档路径（产品 QML 已经没有那个输入框了）。
auto_name="$(grep -oE 'fileName=[^ ]+' "${TEST_STATE_DIR}/repo-test.log" | head -1 | cut -d= -f2 || true)"
if [[ "$auto_name" =~ ^source_[0-9]{8}_[0-9]{6}\.bak$ ]]; then
  record_pass "自动命名符合核心规则（$auto_name）"
else
  record_fail "自动命名不符合 <source-base>_YYYYMMDD_HHMMSS.bak（得到 [$auto_name]）"
fi

# 删除之后仓库里不该再有 .bak。
remaining="$(find "$REPO_DIR" -maxdepth 1 -name '*.bak' 2>/dev/null | wc -l)"
if [[ "$remaining" -eq 0 ]]; then
  record_pass "delete 之后 repository 中不再有 .bak"
else
  record_fail "delete 之后 repository 仍有 $remaining 个 .bak"
fi

echo "[modern-gui] 10) 损坏 .bak 场景"
# 仓库里放一个内容不是归档的 .bak：控制器加载配置后会自动列目录，
# 管理页必须能把它渲染出来而不是崩掉或报 QML 警告。
BAD_REPO="${TEST_STATE_DIR}/bad-repo"
BAD_CFG="${TEST_STATE_DIR}/bad-config.json"
rm -rf "$BAD_REPO"
mkdir -p "$BAD_REPO"
printf 'this file is definitely not a backup archive.\n' > "$BAD_REPO/broken.bak"
printf '{\n  "version": 1,\n  "backup_repository_path": "%s"\n}\n' "$BAD_REPO" > "$BAD_CFG"
set +e
QT_QPA_PLATFORM=offscreen QSG_RHI_BACKEND=software timeout 60 \
  ./build/backup-gui-modern --smoke-test \
  --config-file "$BAD_CFG" >> "$LOG_FILE" 2>&1
bad_repo_status=$?
set -e
if [[ "$bad_repo_status" -eq 0 ]]; then
  record_pass "损坏 .bak 出现在列表里时管理页仍能正常渲染（QML 无告警）"
else
  record_fail "损坏 .bak 场景启动自检退出码 $bad_repo_status"
fi
# "坏 .bak 仍会出现在列表里"与"坏 .bak 必须能删掉"这两条契约由 core 测试直接覆盖，
# 这里只静态确认那两条测试确实存在，不重复实现一遍。
expect_count_re "$ROOT_DIR/tests/unit/backup_catalog_test.cpp" \
  'TEST\(CatalogList, KeepsCorruptedArchivesWithDiagnostic\)' 1 \
  "坏 .bak 仍进列表（由 core 测试覆盖）"
expect_count_re "$ROOT_DIR/tests/unit/backup_catalog_test.cpp" \
  'TEST\(CatalogDelete, DeletesCorruptedArchive\)' 1 \
  "坏 .bak 仍可删除（由 core 测试覆盖）"

echo "[modern-gui] 11) 备份算法选项与密码（QML 静态约束 + --backup-options-test）"
# 高级选项面板是独立组件：三个选择器 + 两个密码框都写在面板里，
# 备份页只负责把面板当前选中的键与密码交给控制器，不自己解释枚举数字。
expect_count_re "$QML_DIR/pages/BackupPage.qml" 'BackupOptionsPanel[[:space:]]*\{' 1 \
  "备份页引用 BackupOptionsPanel"
expect_count_re "$QML_DIR/components/BackupOptionsPanel.qml" \
  'objectName:[[:space:]]*"backupOptionsPanel"' 1 \
  "面板的 objectName 是 backupOptionsPanel"
for selector in packSelector compressionSelector encryptionSelector passwordField confirmPasswordField; do
  expect_count_re "$QML_DIR/components/BackupOptionsPanel.qml" \
    "objectName:[[:space:]]*\"$selector\"" 1 \
    "面板定义 $selector"
done
# 默认值必须与 PR #15 一字不差（mypack + 不压缩 + 不加密），并且默认收起：
# 不展开高级选项的用户，拿到的产物与加这个面板之前完全相同。
expect_count_re "$QML_DIR/components/BackupOptionsPanel.qml" \
  'property string packKey:[[:space:]]*"mypack"' 1 "默认打包方式是 mypack"
expect_count_re "$QML_DIR/components/BackupOptionsPanel.qml" \
  'property string compressionKey:[[:space:]]*"none"' 1 "默认压缩方式是 none"
expect_count_re "$QML_DIR/components/BackupOptionsPanel.qml" \
  'property string encryptionKey:[[:space:]]*"none"' 1 "默认加密方式是 none"
expect_count_re "$QML_DIR/components/BackupOptionsPanel.qml" \
  'property bool expanded:[[:space:]]*false' 1 "面板默认收起"
expect_count_re "$QML_DIR/components/BackupOptionsPanel.qml" \
  '展开高级选项' 1 "收起状态提供「展开高级选项」入口"
# 密码与确认密码都必须是密码框，数量一并钉死：少一个等于明文回显，
# 多一个说明有人又加了一个没有说明的密码入口。
expect_count "$QML_DIR/components/BackupOptionsPanel.qml" \
  "echoMode: TextInput.Password" 2 "面板的两个密码框都遮住输入"
expect_count "$QML_DIR/components/BackupRecordCard.qml" \
  "echoMode: TextInput.Password" 1 "恢复密码框也遮住输入"
# DES 是课程用的旧算法：下拉里的名字必须挂着这个标记，免得有人在真实数据上误选。
if grep -qF '教学 / 旧算法' "$QML_DIR/components/BackupOptionsPanel.qml"; then
  record_pass "DES 选项标注「教学 / 旧算法」"
else
  record_fail "DES 选项没有「教学 / 旧算法」标注"
fi
# 手写密码学实现的免责声明必须写在用户做选择的地方，而不是只写在文档里。
if grep -qF '未经专业密码学审计' "$QML_DIR/components/BackupOptionsPanel.qml"; then
  record_pass "选中加密时给出「未经专业密码学审计」声明"
else
  record_fail "缺少密码学免责声明"
fi
# 收起时的一行摘要：四段（策略 / 打包 / 压缩 / 加密）用「 · 」连接，
# 正好三个分隔符。策略排在最前是因为它回答"这次是什么"，另外三段是"怎么写"。
expect_count "$QML_DIR/components/BackupOptionsPanel.qml" '" · "' 3 \
  "摘要用「 · 」连接四段（策略 + 三段算法名）"
expect_count_re "$QML_DIR/components/BackupOptionsPanel.qml" \
  'objectName:[[:space:]]*"backupOptionsSummary"' 1 "摘要文本有 objectName"

# 记录卡片：加密记录要能提示需要密码，并且能在卡片内就地收一次恢复密码。
for token in passwordRequired pendingRestoreDestination restorePassword; do
  if grep -qF "$token" "$QML_DIR/components/BackupRecordCard.qml"; then
    record_pass "记录卡片包含 $token"
  else
    record_fail "记录卡片缺少 $token"
  fi
done
if grep -qF '需要密码恢复' "$QML_DIR/components/BackupRecordCard.qml"; then
  record_pass "加密记录标注「需要密码恢复」"
else
  record_fail "加密记录没有「需要密码恢复」标注"
fi
# 对话框正文与规范原文逐字一致：PR #16 §13 要求的就是
# 「此备份已加密，需要密码才能恢复。」。这里刻意只接受这一句 ——
# 少一个"才能"就说明实现、规范和测试三处已经开始各说各话，
# 而"两种措辞都接受"恰恰是让这种漂移不被发现的写法。
if grep -qF '此备份已加密，需要密码才能恢复。' "$QML_DIR/components/BackupRecordCard.qml"; then
  record_pass "恢复密码对话框说明「此备份已加密，需要密码才能恢复。」"
else
  record_fail "恢复密码对话框正文与规范不一致（应为「此备份已加密，需要密码才能恢复。」）"
fi

# 回调必须接在产品入口上：备份走带选项的入口，加密恢复走带密码的入口。
expect_count_re "$QML_DIR/pages/BackupPage.qml" \
  'controller\.startBackupWithStrategy\(' 1 \
  "备份页调用 controller.startBackupWithStrategy()"
expect_count_re "$QML_DIR/components/BackupRecordCard.qml" \
  'controller\.startManagedRestoreWithPassword\(' 1 \
  "加密恢复调用 controller.startManagedRestoreWithPassword()"

# 密码生命周期：产品提交路径必须在 Start() 接受任务之后才清空密码框。
# 只断言"面板里存在 clearPasswords() 函数"是挡不住问题的 —— 本轮修掉的洞正是
# "函数存在，但按钮压根没调用它"。所以这两条要一起看：
#   1) 结构断言：返回值被存进 started，且 clearPasswords() 只在 started 为真时调用；
#   2) 计数断言：这个调用在备份页里恰好一次，多出来的无保护调用会被抓住。
# QML 折行会把这个 if 拆成三行，正则跨不了行，所以先把文件压成一行再匹配整段结构。
SQUASHED_BACKUP_PAGE="$(tr -d '\n' < "$QML_DIR/pages/BackupPage.qml" | tr -s ' ')"
if printf '%s' "$SQUASHED_BACKUP_PAGE" \
    | grep -qE 'const started = controller\.startBackupWithStrategy\([^)]*\) if \(started\) panel\.clearPasswords\(\)'; then
  record_pass "备份页检查 startBackupWithStrategy() 的返回值，且只有成功才 panel.clearPasswords()"
else
  record_fail "备份页没有把 startBackupWithStrategy() 的返回值与 clearPasswords() 关联（同步校验失败时会误清密码）"
fi
expect_count_re "$QML_DIR/pages/BackupPage.qml" 'panel\.clearPasswords\(\)' 1 \
  "备份页调用 panel.clearPasswords()"
expect_count_re "$QML_DIR/components/BackupOptionsPanel.qml" 'function clearPasswords\(\)' 1 \
  "面板提供 clearPasswords()"
# 光有函数还不够：它必须真的把两个输入框都清掉，并且在清空的同时复位"已请求过校验"，
# 否则成功提交之后那行"密码不能为空"会立刻跳出来，看起来像刚输错。
# password / confirmPassword 是这两个 TextField 的 text 的 readonly 绑定
# （不是 property alias），所以清 text 就等于清掉对外暴露的那两个属性。
SQUASHED_PANEL="$(tr -d '\n' < "$QML_DIR/components/BackupOptionsPanel.qml" | tr -s ' ')"
if printf '%s' "$SQUASHED_PANEL" \
    | grep -qE 'function clearPasswords\(\) \{ passwordField\.text = "" confirmField\.text = "" panel\.passwordValidationRequested = false \}'; then
  record_pass "clearPasswords() 清空两个输入框并复位 passwordValidationRequested"
else
  record_fail "clearPasswords() 没有同时清空两个输入框并复位校验请求状态"
fi

# 被禁用的措辞。只认代码行：注释里写"这里绝不写校验通过"正是这些规则的用意，
# 把解释性注释也算成违规，只会逼着人删掉解释。
forbidden_hits="$(grep -rnE '军用级|不可破解|绝对安全|生产级安全|校验通过|HMAC verified|归档健康|密码正确|记住密码' "$QML_DIR" \
  | grep -vE ':[0-9]+:[[:space:]]*//' || true)"
if [[ -n "$forbidden_hits" ]]; then
  record_fail "QML 里出现被禁用的安全措辞"
  printf '%s\n' "$forbidden_hits" | sed 's/^/      /'
else
  record_pass "QML 没有出现被禁用的安全措辞（军用级 / 不可破解 / 校验通过 / 记住密码 …）"
fi

# 真实控制器路径：解析表 / 四种算法组合 / 密码校验 / 未知 key / 加密与 legacy 恢复 /
# 目录字段 / 密码不落盘全部由它自己断言，脚本只信退出码并把它那一行结论抄进日志 ——
# 在 bash 里重写一遍同样的断言，只会得到第二份需要同步维护的实现。
OPTIONS_LOG="$TEST_STATE_DIR/backup-options.log"
SHOT_ENC_BAK="$TEST_STATE_DIR/shot-encrypted.bak"
rm -f "$SHOT_ENC_BAK"
set +e
BACKUP_MODERN_KEEP_OPTIONS_ARTIFACT="$SHOT_ENC_BAK" \
  QT_QPA_PLATFORM=offscreen QSG_RHI_BACKEND=software timeout 600 \
  ./build/backup-gui-modern --backup-options-test \
  --config-file "$TEST_CONFIG_FILE" > "$OPTIONS_LOG" 2>&1
options_status=$?
set -e
grep -E '^\[backup-options\] (PASS|FAIL) ' "$OPTIONS_LOG" | sed 's/^/[modern-gui]     /' || true
sed 's/^/[modern-gui]     /' "$OPTIONS_LOG" | grep -E 'observed|records=' || true
cat "$OPTIONS_LOG" >> "$LOG_FILE"
if [[ "$options_status" -eq 0 ]] && grep -qE '^\[backup-options\] PASS [0-9]+/[0-9]+$' "$OPTIONS_LOG"; then
  options_line="$(grep -E '^\[backup-options\] PASS [0-9]+/[0-9]+$' "$OPTIONS_LOG" | tail -1)"
  record_pass "--backup-options-test 全部通过（$options_line）"
else
  record_fail "--backup-options-test 退出码 $options_status"
  tail -20 "$OPTIONS_LOG"
fi

# 产品 CLI 不允许出现密码选项：argv 里的密码会出现在 ps 输出与 shell 历史里，
# 这正是密码只走 GUI 输入框、测试只用固定密码的原因。
# backupctl.cpp 自己解析参数（没有共用的 parser 头），所以 app/ 整个目录
# 就是全部 CLI 选项面；将来多一个入口也跑不掉。
cli_password_hits="$(grep -rn -- '--password' "$ROOT_DIR/app" || true)"
if [[ -n "$cli_password_hits" ]]; then
  record_fail "产品 CLI 出现了 --password 选项"
  printf '%s\n' "$cli_password_hits" | sed 's/^/      /'
else
  record_pass "产品 CLI 没有 --password 选项（密码只经 GUI 输入框进控制器）"
fi

echo "[modern-gui] 12) 截图（人工评审用，不进仓库）"
# 截图放 tests/output/ 下（已 gitignore），是评审产物不是仓库内容。
SHOT_DIR="$ROOT_DIR/tests/output/screenshots"
SHOT_STATE="$TEST_STATE_DIR/shot-state"
SHOT_PLAIN_REPO="$SHOT_STATE/plain-repo"
SHOT_ENC_REPO="$SHOT_STATE/encrypted-repo"
SHOT_PLAIN_CFG="$SHOT_STATE/plain-config.json"
SHOT_ENC_CFG="$SHOT_STATE/encrypted-config.json"
rm -rf "$SHOT_DIR" "$SHOT_STATE"
mkdir -p "$SHOT_PLAIN_REPO" "$SHOT_ENC_REPO"
# 两种仓库状态各抓一轮：只含未加密 v2 记录 / 只含加密 v2 记录。
# 两份记录都是产品路径真实产出的 v2 容器副本（--repository-test 的 kept artifact
# 与 --backup-options-test 保留的 AES 产物），不是手工拼出来的假文件。
if [[ -f "$REPO_KEPT" ]]; then
  cp "$REPO_KEPT" "$SHOT_PLAIN_REPO/shot-plain_20260101_000000.bak"
else
  record_fail "截图用的未加密 v2 产物副本缺失：$REPO_KEPT"
fi
if [[ -f "$SHOT_ENC_BAK" ]]; then
  cp "$SHOT_ENC_BAK" "$SHOT_ENC_REPO/shot-encrypted_20260101_000000.bak"
else
  record_fail "截图用的加密 v2 产物副本缺失：$SHOT_ENC_BAK"
fi
printf '{\n  "version": 1,\n  "backup_repository_path": "%s"\n}\n' "$SHOT_PLAIN_REPO" > "$SHOT_PLAIN_CFG"
printf '{\n  "version": 1,\n  "backup_repository_path": "%s"\n}\n' "$SHOT_ENC_REPO" > "$SHOT_ENC_CFG"

# 一轮截图 = 十四张固定状态（七页 × 两主题）+ 高级选项展开两张（两主题）
# + 调用方追加的状态（只有加密仓库那一轮才有恢复密码对话框）。
shot_run() {
  local label="$1"
  local out_dir="$2"
  local config="$3"
  shift 3
  set +e
  QT_QPA_PLATFORM=offscreen QSG_RHI_BACKEND=software timeout 180 \
    ./build/backup-gui-modern --screenshot "$out_dir" \
    --config-file "$config" >> "$LOG_FILE" 2>&1
  local status=$?
  set -e
  if [[ "$status" -ne 0 ]]; then
    record_fail "截图模式失败（$label，退出码 $status）"
    return
  fi
  local expected="home-light home-dark backup-light backup-dark"
  expected="$expected schedule-light schedule-dark"
  expected="$expected management-light management-dark"
  expected="$expected settings-light settings-dark"
  expected="$expected remote-light remote-dark"
  expected="$expected backup-expanded-light backup-expanded-dark"
  for extra in "$@"; do
    expected="$expected $extra"
  done
  local expected_count=0
  local missing=0
  for name in $expected; do
    expected_count=$((expected_count + 1))
    if [[ ! -s "$out_dir/$name.png" ]]; then
      echo "[modern-gui]     缺少截图: $out_dir/$name.png"
      missing=$((missing + 1))
    fi
  done
  if [[ "$missing" -eq 0 ]]; then
    record_pass "截图 $label：$expected_count 张齐全"
  else
    record_fail "截图 $label：缺 $missing 张"
  fi
}

shot_run "未加密 v2 记录（备份页收起 / 展开 + 管理页）" "$SHOT_DIR/plain" "$SHOT_PLAIN_CFG"
shot_run "加密 v2 记录（管理页 + 恢复密码对话框）" "$SHOT_DIR/encrypted" \
  "$SHOT_ENC_CFG" "management-password-dialog-light" "management-password-dialog-dark"
echo "[modern-gui]     截图目录: $SHOT_DIR（评审产物，已被 gitignore）"

# 截图是评审产物，不是仓库内容：目录必须仍然被 .gitignore 覆盖，
# 否则下一次 git add -A 就会把 PNG 提交进去。
if grep -qE '^/tests/output/' "$ROOT_DIR/.gitignore"; then
  record_pass "截图目录 tests/output/ 仍被 .gitignore 覆盖"
else
  record_fail "tests/output/ 不再被 .gitignore 覆盖，截图有被提交的风险"
fi

echo "[modern-gui] 13) 仓库设置入口（configured 状态下也能直接改仓库）"

# 人工验收发现的缺口：仓库配好之后，备份页与管理页都只剩"当前路径"这一行，
# 没有回到设置页改仓库的入口 —— 而"配置还在、目录已经被删掉"恰恰是最需要它的时候。
# 两个页面本来就都有 openSettings() 信号、Main.qml 也已经连到设置页，
# 所以这里断言的是"按钮真的接在那个信号上"，而不是又造一套跳转机制。

# 从一个 objectName 处取到该 AppButton 块的结尾，再在块内逐项断言。
# 对整块写一条大正则太脆：缩进或换行一调就误报；块内断言则只在字段真的
# 被删掉或改坏时才失败。
button_block() {
  local file="$1"
  local name="$2"
  awk -v want="objectName: \"$name\"" '
    index($0, want) { inside = 1 }
    inside { print }
    inside && /^[[:space:]]*}[[:space:]]*$/ { exit }
  ' "$file"
}

assert_button() {
  local file="$1"
  local name="$2"
  local label="$3"
  shift 3
  local block
  block="$(button_block "$file" "$name")"
  if [[ -z "$block" ]]; then
    record_fail "$label（$(basename "$file") 里找不到 objectName: $name 的按钮）"
    return 0
  fi
  local missing=""
  local needle
  for needle in "$@"; do
    printf '%s\n' "$block" | grep -qF -- "$needle" || missing="$missing [$needle]"
  done
  if [[ -z "$missing" ]]; then
    record_pass "$label"
  else
    record_fail "$label（缺少：$missing）"
  fi
}

BACKUP_PAGE="$QML_DIR/pages/BackupPage.qml"
MGMT_PAGE="$QML_DIR/pages/BackupManagementPage.qml"

# 没有信号就谈不上接线，所以先把两个信号本身钉住。
expect_count_re "$BACKUP_PAGE" '^    signal openSettings\(\)$' 1 \
  "备份页声明 openSettings 信号"
expect_count_re "$MGMT_PAGE" '^    signal openSettings\(\)$' 1 \
  "备份管理页声明 openSettings 信号"

assert_button "$BACKUP_PAGE" changeRepositoryButton \
  "备份页 configured 状态提供「更改仓库」并接到 page.openSettings()" \
  'text: "更改仓库"' \
  'visible: controller.repositoryConfigured' \
  'iconName: "settings"' \
  'onClicked: page.openSettings()'

assert_button "$BACKUP_PAGE" goToSettingsButton \
  "备份页 unconfigured 状态仍是「前往设置」" \
  'text: "前往设置"' \
  'visible: !controller.repositoryConfigured' \
  'onClicked: page.openSettings()'

assert_button "$MGMT_PAGE" changeRepositoryButton \
  "管理页 configured 状态提供「更改仓库」并接到 page.openSettings()" \
  'text: "更改仓库"' \
  'visible: controller.repositoryConfigured' \
  'iconName: "settings"' \
  'onClicked: page.openSettings()'

assert_button "$MGMT_PAGE" refreshBackupsButton \
  "管理页「刷新」仍在，语义未变" \
  'text: "刷新"' \
  'iconName: "refresh"' \
  'onClicked: controller.refreshBackups()'

assert_button "$MGMT_PAGE" goToSettingsButton \
  "管理页 unconfigured 状态只留「前往设置」" \
  'text: "前往设置"' \
  'visible: !controller.repositoryConfigured'

# 「更改仓库」的可见性只能挂在 repositoryConfigured 上。人工验收遇到的正是
# "配置还在、目录已被删"（catalogError 非空、记录为 0）：那种情况下这个入口
# 恰恰最该出现，所以它绝不能顺带依赖 catalogError 或 backupRecords。
if printf '%s\n' "$(button_block "$MGMT_PAGE" changeRepositoryButton)" \
    | grep -qF -- 'visible: controller.repositoryConfigured'; then
  record_pass "管理页「更改仓库」只看 repositoryConfigured，catalogError 场景下依然可进入设置"
else
  record_fail "管理页「更改仓库」的可见性被别的条件影响，catalogError 场景可能进不去设置"
fi

# 换仓库这件事只能发生在设置页：这两个页面都不许自己落盘。
for repo_page in "$BACKUP_PAGE" "$MGMT_PAGE"; do
  if grep -q 'saveRepositoryPath' "$repo_page"; then
    record_fail "$(basename "$repo_page") 直接调用了 saveRepositoryPath()，绕过了设置页"
  else
    record_pass "$(basename "$repo_page") 没有绕过设置页直接保存仓库"
  fi
done

echo "[modern-gui] 14) 密码校验时机与自定义 Dialog 内边距"

PANEL_QML="$QML_DIR/components/BackupOptionsPanel.qml"
BACKUP_PAGE_QML="$QML_DIR/pages/BackupPage.qml"
SQUASHED_PANEL_V2="$(tr -d '\n' < "$PANEL_QML" | tr -s ' ')"
SQUASHED_PAGE_V2="$(tr -d '\n' < "$BACKUP_PAGE_QML" | tr -s ' ')"

# --- 密码校验从"纯实时"改成"提交时请求" ---
expect_count_re "$PANEL_QML" 'property bool passwordValidationRequested: false' 1 \
  "面板声明 passwordValidationRequested，默认 false"
expect_count_re "$PANEL_QML" 'function requestPasswordValidation\(\)' 1 \
  "面板提供 requestPasswordValidation()"
if printf '%s' "$SQUASHED_PANEL_V2" \
    | grep -qE 'function requestPasswordValidation\(\) \{ panel\.passwordValidationRequested = true \}'; then
  record_pass "requestPasswordValidation() 会把 passwordValidationRequested 置真"
else
  record_fail "requestPasswordValidation() 没有把 passwordValidationRequested 置真"
fi
# 提示的显示条件必须同时看"请求过校验"和"当前文案非空"。只跟 validationMessage
# 走就是本轮修掉的旧行为：密码被程序清空后立刻报"密码不能为空"。
if printf '%s' "$SQUASHED_PANEL_V2" \
    | grep -qE 'objectName: "passwordValidationText" .*visible: panel\.passwordValidationRequested && panel\.validationMessage\.length > 0'; then
  record_pass "校验提示的 visible 同时依赖 passwordValidationRequested 与非空 validationMessage"
else
  record_fail "校验提示的 visible 条件不对（可能又变回只跟着 validationMessage 实时显示）"
fi
# 切换加密算法必须复位"已尝试提交"，否则刚点过 AES 又切到 DES 会继承上一次的红字。
# 这一段先按行切出来再分别看两个分支：两个分支里都写了说明注释，直接把整段折叠成
# 一行再写一条大正则会卡在注释文本上 —— 注释是代码的一部分，不是可以忽略的噪音。
ENCRYPTION_HANDLER="$(awk '
  /^    onEncryptionKeyChanged: \{/ { inside = 1 }
  inside { print }
  inside && /^    \}$/ { exit }
' "$PANEL_QML" | tr -d '\n' | tr -s ' ')"
NONE_BRANCH="$(printf '%s' "$ENCRYPTION_HANDLER" | sed 's/.*encryptionKey === "none") {//')"
ELSE_BRANCH="$(printf '%s' "$ENCRYPTION_HANDLER" | sed 's/.*} else {//')"
# 三个条件缺一不可：确实有 else 分支、none 分支清空、else 分支复位。
# 少了第一条，sed 匹配不上时会原样返回整段，后面的 grep 就会变成"全文里存在"，
# 那正好把"没有 else 分支"这种情况放过去。
if printf '%s' "$ENCRYPTION_HANDLER" | grep -qF -- '} else {' \
   && printf '%s' "$NONE_BRANCH" | grep -qF -- 'panel.clearPasswords()' \
   && printf '%s' "$ELSE_BRANCH" | grep -qF -- 'panel.passwordValidationRequested = false'; then
  record_pass "切换加密算法会复位 passwordValidationRequested，切到 none 时清空密码"
else
  record_fail "切换加密算法没有正确复位 / 清空（缺少 else 分支，或两个分支内容不对）"
fi

# --- 备份页提交顺序 ---
assert_button "$BACKUP_PAGE_QML" startBackupButton \
  "开始备份按钮只在 busy 时禁用（密码不合法不再直接禁用按钮）" \
  'enabled: !controller.busy' \
  'onClicked: {'

START_BACKUP_BLOCK="$(button_block "$BACKUP_PAGE_QML" startBackupButton)"
if printf '%s\n' "$START_BACKUP_BLOCK" | grep -qF -- 'enabled: !controller.busy && panel.passwordAcceptable'; then
  record_fail "开始备份按钮仍被 passwordAcceptable 直接禁用（用户没有机会触发校验）"
else
  record_pass "开始备份按钮不再由 passwordAcceptable 直接禁用"
fi
if printf '%s' "$SQUASHED_PAGE_V2" \
    | grep -qE 'onClicked: \{ panel\.requestPasswordValidation\(\) if \(!panel\.passwordAcceptable\) return .*const started = controller\.startBackupWithStrategy\([^)]*\) if \(started\) panel\.clearPasswords\(\) \}'; then
  record_pass "提交顺序正确：先请求校验 -> 不合法就 return（不碰控制器）-> 合法才提交 -> 成功才清空"
else
  record_fail "提交顺序不对（可能先调用了控制器，或清空时机被改）"
fi

# --- 自定义 Dialog 的内容边距 ---
# 按 objectName 定位到各自 Dialog 之后的第一个 padding，而不是数全文的 "padding: 18"
# 总量：后者在新增一个 Dialog 或改别处内边距时会给出误导性的结果。
check_dialog_padding() {
  local file="$1"
  local name="$2"
  local got
  got="$(awk -v want="objectName: \"$name\"" '
    index($0, want) { found = 1; next }
    found && /padding:/ {
      line = $0
      sub(/^[[:space:]]*/, "", line)
      print line
      exit
    }
  ' "$file")"
  if [[ -z "$got" ]]; then
    record_fail "$(basename "$file") 的 $name 找不到 padding"
  elif [[ "$got" == "padding: 18" ]]; then
    record_pass "$(basename "$file") 的 $name 有 18 的内容边距"
  else
    record_fail "$(basename "$file") 的 $name 边距不是 18（实际：$got）"
  fi
}

check_dialog_padding "$QML_DIR/components/BackupRecordCard.qml" restorePasswordDialog
check_dialog_padding "$QML_DIR/components/BackupRecordCard.qml" deleteConfirmDialog
check_dialog_padding "$QML_DIR/Main.qml" busyCloseDialog

echo "[modern-gui] 15) 自动备份页（Scheduled + Full）"

SCHEDULE_PAGE_QML="$QML_DIR/pages/SchedulePage.qml"
SCHEDULE_CTRL_CPP="$ROOT_DIR/ui/modern/schedule_controller.cpp"
SCHEDULE_CTRL_H="$ROOT_DIR/ui/modern/schedule_controller.h"

# expect_present / expect_missing / expect_missing_code 定义在文件靠前的
# "远程备份页"一节之前：那里也要用，而这一节在它后面。

# --- 页面在资源清单与导航里 ---
expect_count "$RESOURCE_FILE" "qml/pages/SchedulePage.qml" 1 \
  "SchedulePage.qml 进了资源清单"
expect_count "$QML_DIR/Main.qml" 'text: "自动备份"' 1 \
  "侧栏有自动备份入口"
expect_count "$QML_DIR/Main.qml" "SchedulePage {" 1 \
  "StackLayout 里只有一个 SchedulePage 实例"

# --- 页面上该有的控件 ---
for name in scheduleEnabledToggle scheduleSourceField scheduleFrequencyValueField \
            scheduleFrequencyUnitCombo scheduleFrequencyHintText \
            scheduleRetainField schedulePackCombo scheduleCompressionCombo \
            scheduleEncryptionText scheduleEncryptionNote \
            scheduleAdvancedToggle scheduleAdvancedSection \
            scheduleStrategyCombo scheduleStrategyHelperText \
            runScheduleNowHintText \
            saveScheduleButton runScheduleNowButton scheduleHistoryList \
            scheduleManagedList scheduleLastRunText scheduleNextRunText \
            scheduleLastResultText scheduleRunnerText; do
  expect_count "$SCHEDULE_PAGE_QML" "objectName: \"$name\"" 1 \
    "计划页有 $name"
done

# --- 备份频率：值 + 单位（每 1 小时），而不是"周期 [60] 分钟" ---
#
# 人工验收："周期 [60] 分钟"功能没错，但用户每次都要自己心算。界面改成 值 + 单位，
# 换算在 C++ 的 schedule_frequency.cpp 里做，而数值文本仍然交给共享核心的
# ParseBoundedScheduleNumber 解析 —— 所以 QML 里不会出现 parseInt(x) * 10080。
expect_present "$SCHEDULE_PAGE_QML" "schedule.saveConfigFromFrequencyText(" \
  "计划页把频率（值 + 单位）与保留数量按文本交给共享核心"
expect_missing "$SCHEDULE_PAGE_QML" "parseInt(page.draft" \
  "计划页不用 QML 的 parseInt 截断数值"
expect_missing "$SCHEDULE_PAGE_QML" "* 10080" \
  "QML 里不做 值 × 单位 的乘法"
expect_count_re "$SCHEDULE_PAGE_QML" "schedule.frequencyUnitKey" 2 \
  "频率单位来自控制器（草稿初值 + 同步各一次，界面不自己定义单位表）"
expect_present "$ROOT_DIR/ui/modern/schedule_frequency.cpp" \
  "ParseBoundedScheduleNumber" \
  "频率的数值解析仍然走共享核心的 ParseBoundedScheduleNumber"
expect_present "$ROOT_DIR/ui/modern/schedule_frequency.cpp" "kMaxIntervalMinutes" \
  "频率的上界来自共享核心的 schedule_store.h（界面不另写一套范围）"
expect_present "$ROOT_DIR/ui/modern/schedule_controller.cpp" \
  "ParseBoundedScheduleNumber" \
  "界面侧调用的是共享的 ParseBoundedScheduleNumber"
expect_present "$ROOT_DIR/src/cli/cli_commands.cpp" \
  "ParseBoundedScheduleNumber" \
  "CLI 侧调用的是同一个 ParseBoundedScheduleNumber"
# 存储 schema 一个字节都没变：核心 / CLI / store 继续只看 interval_minutes。
# 这里查的是**落盘字段**，不是注释里那个词。
expect_present "$ROOT_DIR/src/scheduler/schedule_store.cpp" "\"interval_minutes\"" \
  "落盘字段仍然是 interval_minutes"
expect_present "$ROOT_DIR/ui/modern/schedule_controller.cpp" \
  "saveConfigFromFrequencyText" \
  "频率入口最终仍然走同一个 saveConfig（没有第二条落盘路径）"

# --- 加密边界：没有任何"选加密"的入口，只有一行说明 ---
expect_missing "$SCHEDULE_PAGE_QML" "scheduleEncryptionCombo" \
  "加密在计划页不是可选项（没有下拉框）"
expect_count "$SCHEDULE_PAGE_QML" "objectName: \"scheduleEncryptionNote\"" 1 \
  "计划页写明了不加密的原因"
expect_present "$SCHEDULE_PAGE_QML" "schedule.encryptionNote" \
  "计划页的加密说明问控制器要（QML 不复制那句字面量）"
expect_present "$ROOT_DIR/src/core/backup_mode.cpp" \
  "定时无人值守加密需要安全的密钥来源" \
  "那句话本身只有一处：backup_mode.cpp（CLI 与 GUI 都读它）"

# --- 不画未实现的假按钮 ---
#
# PR #18 之前这里断言"计划页不许出现 incremental"；现在它是一条真实支持的路，
# 所以改成断言"策略选择真的接在共享 key 上"，而 Realtime 仍然不许出现。
expect_present "$SCHEDULE_PAGE_QML" "scheduleStrategyCombo" \
  "计划页提供备份策略选择（incremental 现在是真实支持的路）"
expect_missing "$SCHEDULE_PAGE_QML" "Realtime" \
  "计划页没有 Realtime 假按钮"
expect_present "$SCHEDULE_PAGE_QML" "schedule.supportedModeText" \
  "计划页的模式说明来自共享核心，而不是 QML 自己写死"

# --- 生命周期必须诚实：关掉程序就不会再跑 ---
expect_present "$SCHEDULE_PAGE_QML" "定时任务只在本程序运行期间执行" \
  "计划页写明了定时任务只在程序运行时生效"
expect_missing_code "$SCHEDULE_PAGE_QML" "backupctl" \
  "命令行工具名不出现在计划页的用户文案里"

# --- 业务逻辑不在 QML 里 ---
expect_missing "$SCHEDULE_PAGE_QML" "Date.now" \
  "QML 不自己算时间"
expect_missing "$SCHEDULE_PAGE_QML" "new Date" \
  "QML 不自己算下次运行时间"
expect_missing "$SCHEDULE_PAGE_QML" "setInterval" \
  "QML 不自己起定时器（tick 在 C++ 侧，判定在共享核心）"

# --- 控制器只是桥：算法 key、store、核心服务全部来自共享核心 ---
expect_present "$SCHEDULE_CTRL_CPP" "backupproject::ScheduledBackupService" \
  "ScheduleController 直接使用共享的 ScheduledBackupService"
expect_present "$SCHEDULE_CTRL_CPP" "backupproject::ScheduleStore" \
  "ScheduleController 直接使用共享的 ScheduleStore"
expect_present "$SCHEDULE_CTRL_CPP" "backupproject::ParsePackMethodKey" \
  "算法 key 解析走共享核心的同一张表"
expect_present "$SCHEDULE_CTRL_CPP" "backupproject::IsScheduleDue" \
  "到点判定走共享核心"
expect_present "$SCHEDULE_CTRL_CPP" "backup_controller_->busy()" \
  "计划任务与手动备份共用同一个 busy 边界"

# --- 窄窗口：内容必须能滚动，而不是被裁掉 ---
expect_count "$SCHEDULE_PAGE_QML" "ScrollView {" 1 \
  "计划页用 ScrollView 承载内容"
expect_count "$SCHEDULE_PAGE_QML" "contentWidth: availableWidth" 1 \
  "计划页在窄窗口下启用横向自适应"
expect_present "$SCHEDULE_PAGE_QML" "width: Math.min(pageScroll.availableWidth - 64, 1400)" \
  "计划页的列宽随可用宽度收缩"

# --- 备份管理页的 JOIN：来源与计划变化摘要 ---
# 这三条用 expect_present（grep -F）而不是 expect_count：模式里同时有双引号和
# 方括号，走正则会把 ["fileName"] 当成字符类，断言就成了假阳性。
expect_present "$QML_DIR/pages/BackupManagementPage.qml" \
  'schedule.originForFile(String(modelData["fileName"] || ""))' \
  "管理页向 ScheduleStore 询问每条记录的来源"
expect_present "$QML_DIR/pages/BackupManagementPage.qml" \
  'schedule.changesForFile(String(modelData["fileName"] || ""))' \
  "管理页向 ScheduleStore 询问计划快照的变化摘要"
expect_present "$QML_DIR/components/BackupRecordCard.qml" \
  'objectName: "backupRecordOrigin"' \
  "记录卡片展示来源"

# --- 真实控制器链路自检 + 跨前端同一份 store ---
SCHEDULE_STORE="$TEST_STATE_DIR/schedule.json"
SCHEDULE_CONFIG="$TEST_STATE_DIR/schedule-config.json"
set +e
QT_QPA_PLATFORM=offscreen QSG_RHI_BACKEND=software timeout 240 \
  ./build/backup-gui-modern --schedule-test \
  --config-file "$SCHEDULE_CONFIG" --schedule-file "$SCHEDULE_STORE" \
  >> "$LOG_FILE" 2>&1
schedule_status=$?
set -e
if [[ "$schedule_status" -eq 0 ]]; then
  record_pass "计划页控制器链路自检通过（$(grep -c '   ok   ' "$LOG_FILE" || true) 项观测中，最后一次为全部通过）"
else
  record_fail "计划页控制器链路自检退出码 $schedule_status"
  grep 'FAIL' "$LOG_FILE" | tail -5
fi
if [[ -s "$SCHEDULE_STORE" ]]; then
  record_pass "自检写出的 schedule.json 存在"
else
  record_fail "自检没有写出 schedule.json"
fi

# GUI 写的计划，产品 CLI 必须逐项读得到 —— 这就是"共用同一份 store"的证据。
set +e
./build/backupctl --config-file "$SCHEDULE_CONFIG" --schedule-file "$SCHEDULE_STORE" \
  schedule show > "$TEST_STATE_DIR/schedule-show.txt" 2>&1
show_status=$?
set -e
if [[ "$show_status" -eq 0 ]]; then
  record_pass "backupctl schedule show 能读 GUI 写的 store"
else
  record_fail "backupctl schedule show 读不了 GUI 写的 store（退出码 $show_status）"
fi
for pattern in "Interval:       5 minute(s)" \
               "Retain:         7 scheduled snapshot(s)" \
               "Pack:           ustar" \
               "Compression:    huffman" \
               "Encryption:     none"; do
  if grep -qF -- "$pattern" "$TEST_STATE_DIR/schedule-show.txt"; then
    record_pass "跨前端一致：$pattern"
  else
    record_fail "跨前端不一致：$pattern"
  fi
done

# GUI 保存的筛选规则，CLI 必须逐字读得到。
for pattern in "Include rules:  ext:txt" \
               "Exclude rules:  path:**/build/**"; do
  if grep -qF -- "$pattern" "$TEST_STATE_DIR/schedule-show.txt"; then
    record_pass "跨前端一致（筛选规则）：$pattern"
  else
    record_fail "跨前端不一致（筛选规则）：$pattern"
  fi
done

# GUI 能删规则，CLI 也必须能：--clear-filters 之后两边看到的都是空。
#
# 注意：GUI 自检跑在 QTemporaryDir 里，进程一退出那个仓库就没了；而这一份计划
# 是 enabled 的，任何修改都会先过一遍"仍然真的能跑"的完整校验，所以这里先把
# 仓库重新指到一个真实存在的目录。这本身就是那条新约束在起作用。
mkdir -p "$TEST_STATE_DIR/cleared-repo" "$TEST_STATE_DIR/cleared-src"
set +e
./build/backupctl --config-file "$SCHEDULE_CONFIG" config repository set \
  "$TEST_STATE_DIR/cleared-repo" > "$TEST_STATE_DIR/schedule-repo.txt" 2>&1
repo_status=$?
./build/backupctl --config-file "$SCHEDULE_CONFIG" --schedule-file "$SCHEDULE_STORE" \
  schedule set --clear-filters --source "$TEST_STATE_DIR/cleared-src" \
  > "$TEST_STATE_DIR/schedule-clear.txt" 2>&1
clear_status=$?
./build/backupctl --config-file "$SCHEDULE_CONFIG" --schedule-file "$SCHEDULE_STORE" \
  schedule show > "$TEST_STATE_DIR/schedule-show-cleared.txt" 2>&1
set -e

if [[ "$repo_status" -eq 0 && "$clear_status" -eq 0 ]]; then
  record_pass "backupctl schedule set --clear-filters 退出码 0"
else
  record_fail "backupctl schedule set --clear-filters 退出码 $clear_status（repository set 退出码 $repo_status）"
fi
for pattern in "Include rules:  (none)" \
               "Exclude rules:  (none)" \
               "Interval:       5 minute(s)"; do
  if grep -qF -- "$pattern" "$TEST_STATE_DIR/schedule-show-cleared.txt"; then
    record_pass "CLI 清空规则后仍然读到：$pattern"
  else
    record_fail "CLI 清空规则后读不到：$pattern"
  fi
done

echo "[modern-gui] 16) 实时备份页（Realtime Trigger）"

REALTIME_PAGE_QML="$QML_DIR/pages/RealtimePage.qml"
REALTIME_CTRL_CPP="$ROOT_DIR/ui/modern/realtime_controller.cpp"
REALTIME_CTRL_H="$ROOT_DIR/ui/modern/realtime_controller.h"

# --- 页面在资源清单与导航里 ---
expect_count "$RESOURCE_FILE" "qml/pages/RealtimePage.qml" 1 \
  "RealtimePage.qml 进了资源清单"
expect_count "$QML_DIR/Main.qml" 'text: "实时备份"' 1 \
  "侧栏有实时备份入口"
expect_count "$QML_DIR/Main.qml" "RealtimePage {" 1 \
  "StackLayout 里只有一个 RealtimePage 实例"
expect_count "$QML_DIR/Main.qml" "realtime.clearStatus()" 1 \
  "离开实时页时消费它自己的临时提示"

# --- 页面上该有的控件（一个都不许丢，只是分到了不同的层级里） ---
for name in realtimeEnabledToggle realtimeSourceField browseRealtimeSourceButton \
            realtimeStrategyCombo realtimeRetainField saveRealtimeButton \
            realtimeSubtitleText realtimeDebounceField realtimeMaxWaitField \
            realtimePackCombo realtimeCompressionCombo realtimeDelayTermText \
            realtimeEncryptionText realtimeEncryptionNote \
            realtimeAdvancedToggle realtimeAdvancedSection realtimeTechnicalToggle \
            realtimeTechnicalSection realtimeSupportedModeText realtimeRunScopeText \
            realtimeRawPhaseText realtimeWatchText realtimeWatchCountText \
            realtimePendingCountText realtimePendingText realtimeOverflowText \
            realtimeLastEventText realtimeLastSnapshotText realtimeRepositoryText \
            realtimePhaseText realtimeLoadErrorText realtimeConfigErrorText \
            realtimeSnapshotEmptyText realtimeSnapshotList \
            refreshRealtimeSnapshotsButton realtimeStatusBanner; do
  expect_count "$REALTIME_PAGE_QML" "objectName: \"$name\"" 1 \
    "实时页有 $name"
done

# --- 信息架构：常用设置 / 高级设置（默认折叠）/ 运行状态 / 技术详情（默认折叠）/ 最近备份 ---
#
# 人工验收的结论是"好看，但像开发者控制台"：Debounce / Max wait / MyPack / 压缩 /
# include-exclude / watch 数 / pending / overflow 全部铺在主层。这一节把新的分层
# 钉成契约：折叠区默认收起，且区里每个具名控件都显式跟随折叠状态（不是只靠父级
# 不可见），这样"技术项退回高级区"这件事不会在后续改动里悄悄退化。
expect_count "$REALTIME_PAGE_QML" "property bool advancedExpanded: false" 1 \
  "高级设置默认折叠"
expect_count "$REALTIME_PAGE_QML" "property bool technicalExpanded: false" 1 \
  "技术详情默认折叠"
expect_present "$REALTIME_PAGE_QML" 'objectName: "realtimeAdvancedToggle"' \
  "高级设置有独立的展开/收起入口"
expect_present "$REALTIME_PAGE_QML" 'objectName: "realtimeTechnicalToggle"' \
  "技术详情有独立的展开/收起入口"
# 折叠区里每个具名控件都要自己 visible: false，不能只靠父级不可见：
# 否则"收起时它仍然占着布局"这种退化不会有人发现。数量断言会因为加一个控件就
# 失效、且失败信息说不清是谁，所以这里逐个控件断言，失败时直接点名。
for name in realtimeDebounceField realtimeMaxWaitField realtimePackCombo \
            realtimeCompressionCombo realtimeEncryptionText \
            realtimeEncryptionNote; do
  if grep -A3 "objectName: \"$name\"" "$REALTIME_PAGE_QML" | grep -q "visible: page.advancedExpanded"; then
    record_pass "$name 显式跟随高级设置折叠状态"
  else
    record_fail "$name 没有显式跟随高级设置折叠状态"
  fi
done
# 规则错误提示在共享编辑器里：它自己的 visible 绑的是"有没有错误消息"，
# 折叠可见性由外面的容器负责（容器已断言跟随 advancedExpanded）。
expect_present "$QML_DIR/components/FilterRuleEditor.qml" 'visible: text.length > 0' \
  "规则错误提示只在真的有错误时出现（折叠可见性由容器负责）"
if grep -A5 'objectName: editor.nameOf("AdvancedRulesSection")' \
     "$QML_DIR/components/FilterRuleEditor.qml" | \
   grep -q "visible: editor.advancedExpanded"; then
  record_pass "高级规则容器显式跟随折叠状态"
else
  record_fail "高级规则容器没有显式跟随折叠状态"
fi
for name in realtimeSupportedModeText realtimeRunScopeText realtimeRawPhaseText \
            realtimeWatchText realtimeWatchCountText realtimePendingCountText \
            realtimePendingText realtimeOverflowText realtimeLastEventText \
            realtimeLastSnapshotText realtimeRepositoryText; do
  if grep -A3 "objectName: \"$name\"" "$REALTIME_PAGE_QML" | grep -q "visible: page.technicalExpanded"; then
    record_pass "$name 显式跟随技术详情折叠状态"
  else
    record_fail "$name 没有显式跟随技术详情折叠状态"
  fi
done
# 常用设置那几个控件不允许挂到折叠状态上：主卡片必须一直是可见的。
for name in realtimeSourceField realtimeStrategyCombo realtimeRetainField \
            saveRealtimeButton; do
  if grep -A4 "objectName: \"$name\"" "$REALTIME_PAGE_QML" | grep -qE "page\.(advanced|technical)Expanded"; then
    record_fail "$name 属于常用设置，却被折叠状态控制"
  else
    record_pass "$name 常显（不被折叠状态控制）"
  fi
done
# 技术名词不再当主标签：普通 UI 只有中文，英文术语退到「技术详情」。
expect_present "$REALTIME_PAGE_QML" 'text: "响应延迟"' \
  "响应延迟是主标签（纯中文）"
expect_present "$REALTIME_PAGE_QML" 'text: "最长等待"' \
  "最长等待是主标签（纯中文）"
expect_missing_code "$REALTIME_PAGE_QML" "（Debounce）" \
  "主标签里不再出现英文术语"
expect_missing_code "$REALTIME_PAGE_QML" "（Max wait）" \
  "主标签里不再出现英文术语"
for term in Debounce "Max wait"; do
  if grep -A4 'objectName: "realtimeDelayTermText"' "$REALTIME_PAGE_QML" | grep -qF "$term"; then
    record_pass "英文术语 $term 只出现在「技术详情」里"
  else
    record_fail "英文术语 $term 没有退到「技术详情」"
  fi
done
if grep -A4 'objectName: "realtimeDelayTermText"' "$REALTIME_PAGE_QML" | grep -q "visible: page.technicalExpanded"; then
  record_pass "英文术语跟随「技术详情」折叠状态"
else
  record_fail "英文术语没有跟随「技术详情」折叠状态"
fi
expect_missing "$REALTIME_PAGE_QML" "100..60000" \
  "字段取值范围不作为主视觉文案"
expect_missing "$REALTIME_PAGE_QML" "500..300000" \
  "字段取值范围不作为主视觉文案"
# 策略解释：短、面向用户，不出现实现术语。
expect_present "$REALTIME_PAGE_QML" "完整备份：每次生成一份可以独立恢复的完整备份。" \
  "完整备份有一句用户向解释"
expect_present "$REALTIME_PAGE_QML" "增量备份：首次建立完整基线，之后只保存变化，更节省空间。" \
  "增量备份有一句用户向解释"
expect_missing "$REALTIME_PAGE_QML" "BKPINC1" \
  "主层不解释增量容器格式"
expect_missing "$REALTIME_PAGE_QML" "parent chain" \
  "主层不解释 parent chain"
# 最近备份：标题面向用户，空状态给出下一步，不在标题里解释 marker。
expect_present "$REALTIME_PAGE_QML" 'text: "最近备份"' \
  "最近快照卡片标题是用户语言"
expect_missing "$REALTIME_PAGE_QML" "只列带 .realtime 标记" \
  "标题不解释 .realtime marker"
expect_present "$REALTIME_PAGE_QML" "还没有实时备份。启用后，文件发生变化时会在这里看到新的备份版本。" \
  "空状态告诉用户接下来会发生什么"
# 同一状态不得出现两遍：底部状态栏不是"运行状态"的复读。
expect_missing "$REALTIME_PAGE_QML" "等待实时备份" \
  "页面底部没有和运行状态重复的孤立状态文案"
# 副标题不再是架构说明。
expect_missing "$REALTIME_PAGE_QML" "与 backupctl 共用同一份核心" \
  "副标题不再讲架构"

# --- 加密：不再占一个永远置灰的 ComboBox，降级成高级设置里的一条弱提示 ---
expect_missing "$REALTIME_PAGE_QML" "realtimeEncryptionCombo" \
  "实时页不再有加密选择器（置灰控件也去掉）"
expect_missing "$REALTIME_PAGE_QML" "aes-256-ctr-hmac-sha256" \
  "实时页不提供任何加密算法选项"
expect_present "$REALTIME_PAGE_QML" "realtime.encryptionNote" \
  "加密说明仍然问控制器要（QML 不复制那句字面量）"
expect_present "$REALTIME_CTRL_CPP" "UnattendedEncryptionDisabledReason" \
  "加密说明问的是核心那句唯一来源，控制器不复制字面量"
expect_present "$ROOT_DIR/src/core/backup_mode.cpp" \
  "实时无人值守备份当前不保存密码，因此不启用加密。" \
  "那句话本身只有一处：backup_mode.cpp（CLI 与 GUI 都读它）"

# --- Filter：与备份页 / 自动备份页共用同一个可视化编辑器 ---
#
# 人工验收的结论：两个 raw DSL 输入框（"例如 ext:cpp;h" + "添加包含规则"）是
# 不合格的默认交互 —— 普通用户被要求自己写语法。现在整块换成共享的
# FilterRuleEditor，规则文本仍然由同一个 builder 生成、由真实的 Filter 裁决。
expect_missing "$REALTIME_PAGE_QML" "realtime.validateRule(" \
  "实时页不再自己调控制器做规则校验（校验走共享编辑器 / builder）"
expect_missing_code "$REALTIME_PAGE_QML" "例如 ext:cpp;h" \
  "实时页不再要求用户输入 ext: 语法"
expect_missing_code "$REALTIME_PAGE_QML" "例如 path:**/build/**" \
  "实时页不再要求用户输入 path: 语法"
expect_present "$REALTIME_PAGE_QML" "realtimeFilterRuleModel" \
  "实时页注入的是自己的规则模型（DSL 由共享 builder 生成）"
expect_present "$REALTIME_PAGE_QML" 'objectPrefix: "realtime"' \
  "实时页用的是共享的 FilterRuleEditor"
expect_present "$REALTIME_PAGE_QML" "realtime.saveConfigFromText(" \
  "实时页把数字按文本交给共享核心解析"
expect_missing "$REALTIME_PAGE_QML" "parseInt(page.draft" \
  "实时页不用 QML 的 parseInt 截断 Debounce / Max wait / 保留数量"
expect_missing "$REALTIME_PAGE_QML" "backupFilePath" \
  "实时页没有归档完整路径这个概念"

# --- 业务逻辑不在 QML 里：inotify / 核心服务 / retention / 仓库路径拼接 ---
# 顶层注释里那句"它不直接调 inotify"是有意留下的说明，所以断言的是真实的
# API 名字，而不是那个词本身。
expect_missing "$REALTIME_PAGE_QML" "InotifyWatcher" \
  "QML 不直接碰 inotify"
expect_missing "$REALTIME_PAGE_QML" "RunRealtimeBackupOnce" \
  "QML 不直接调核心服务"
expect_missing "$REALTIME_PAGE_QML" "ListRealtimeSnapshots" \
  "QML 不直接列实时快照"
expect_missing "$REALTIME_PAGE_QML" "RunRealtimeRetention" \
  "QML 不自己执行 retention"
expect_present "$REALTIME_PAGE_QML" "realtime.repositoryPath" \
  "实时页只显示控制器给的仓库路径"
expect_missing "$REALTIME_PAGE_QML" 'repositoryPath + "/"' \
  "实时页不自己拼 repository 路径"

# --- 窄窗口：内容必须能滚动，而不是被裁掉 ---
expect_count "$REALTIME_PAGE_QML" "ScrollView {" 1 \
  "实时页用 ScrollView 承载内容"
expect_count "$REALTIME_PAGE_QML" "contentWidth: availableWidth" 1 \
  "实时页在窄窗口下启用横向自适应"
expect_present "$REALTIME_PAGE_QML" "width: Math.min(pageScroll.availableWidth - 64, 1400)" \
  "实时页的列宽随可用宽度收缩"

# --- 控制器只是桥：配置 / 监听 / 合并 / 执行 / 历史全部来自共享核心 ---
expect_present "$REALTIME_CTRL_H" "backupproject::RealtimeStore store_;" \
  "RealtimeController 直接使用共享的 RealtimeStore"
expect_present "$REALTIME_CTRL_H" "backupproject::InotifyWatcher watcher_;" \
  "监听走共享核心的 InotifyWatcher"
expect_present "$REALTIME_CTRL_CPP" "backupproject::RealtimeDebouncer" \
  "合并窗口走共享核心的 RealtimeDebouncer"
expect_present "$REALTIME_CTRL_CPP" "backupproject::RunRealtimeBackupOnce" \
  "执行走共享核心的 RunRealtimeBackupOnce"
expect_present "$REALTIME_CTRL_CPP" "backupproject::ListRealtimeSnapshots" \
  "实时快照列表走共享核心的 ListRealtimeSnapshots"
expect_present "$REALTIME_CTRL_CPP" "backupproject::ValidateRealtimeConfig" \
  "结构校验走共享核心"
expect_present "$REALTIME_CTRL_CPP" "backupproject::ValidateRealtimeForEnable" \
  "启用前的完整校验走共享核心（backupctl realtime enable 用的是同一个函数）"
expect_present "$REALTIME_CTRL_CPP" "backupproject::ParseBackupStrategyKey" \
  "策略 key 解析走共享核心的同一张表"
# --- 单实例锁与子进程：都**不许**出现 ---
# GUI 主进程启动时已经按 per-UID 持有了 ApplicationInstanceLock，而 flock 绑在
# open file description 上：同一进程第二次 open + LOCK_EX|LOCK_NB 会 EWOULDBLOCK，
# 自己把自己判成"另一个实例正在运行"。核心服务自己不加锁，所以这里也不许有。
expect_missing "$REALTIME_CTRL_CPP" "ApplicationInstanceLock" \
  "RealtimeController 不重复申请应用单实例锁"
expect_missing "$REALTIME_CTRL_CPP" "QProcess" \
  "RealtimeController 不 spawn 子 backupproject 进程来做备份"

# --- 闸门：进 worker 前必须拿到 kRealtimeEvaluation；保存配置走 kRealtimeConfig ---
expect_present "$REALTIME_CTRL_CPP" "OperationGate::Kind::kRealtimeEvaluation" \
  "后台实时评估先过闸门"
expect_present "$REALTIME_CTRL_CPP" "OperationGate::Kind::kRealtimeConfig" \
  "保存实时配置先过闸门"
expect_present "$REALTIME_CTRL_CPP" "retry_timer_.setInterval(kGateRetryMs)" \
  "抢不到闸门时用 150 ms 的轻量 retry timer，而不是 busy-spin"
expect_present "$REALTIME_CTRL_H" "backupproject::RealtimeGeneration pending_generation_;" \
  "抢不到闸门时只保留**一个** pending generation"
expect_present "$ROOT_DIR/ui/modern/operation_gate.h" "kRealtimeEvaluation," \
  "闸门里有实时评估这一格"
expect_present "$ROOT_DIR/ui/modern/operation_gate.h" "kRealtimeConfig," \
  "闸门里有保存实时配置这一格"
expect_count "$ROOT_DIR/ui/modern/operation_gate.h" "kRealtime" 2 \
  "闸门只加了实时相关的两格"

# --- 运行期改仓库：控制器必须跟上 BackupController::repositoryPathChanged ---
# 不订阅它，实时触发就会拿着启动时读到的仓库继续写：新事件产生的快照被静默
# 放进旧位置，新仓库与 source 的 overlap 也不会被重新检查。
expect_present "$REALTIME_CTRL_H" "void OnRepositoryPathChanged();" \
  "实时控制器有仓库变化的处理入口"
expect_count "$REALTIME_CTRL_CPP" "&BackupController::repositoryPathChanged" 1 \
  "实时控制器订阅的是与计划控制器同一个信号"
expect_count "$ROOT_DIR/ui/modern/schedule_controller.cpp" \
  "&BackupController::repositoryPathChanged" 1 \
  "计划控制器订阅的仍是同一个信号（两边接法一致）"
expect_present "$REALTIME_CTRL_CPP" "ValidateRealtimeForEnable" \
  "换仓库后重新做一次共享核心的完整校验（含三种 overlap）"
# 这个槽由 BackupController 在**持有 kRepositoryChange 闸门期间**同步调用，
# 再取一次闸门必然自冲突。全程只有"提交评估"这一处显式 Acquire。
expect_count "$REALTIME_CTRL_CPP" "operation_gate_->Acquire(" 1 \
  "实时控制器只在提交评估时取一次闸门，仓库变化的 handler 不取闸门"
expect_missing "$REALTIME_CTRL_CPP" "Kind::kRepositoryChange" \
  "实时控制器不碰改仓库那把闸门"

# --- 真实控制器链路自检 + 隔离路径 ---
REALTIME_STORE="$TEST_STATE_DIR/realtime.json"
REALTIME_CONFIG="$TEST_STATE_DIR/realtime-config.json"
REALTIME_LOG="$TEST_STATE_DIR/realtime-test.log"
set +e
QT_QPA_PLATFORM=offscreen QSG_RHI_BACKEND=software timeout 300 \
  ./build/backup-gui-modern --realtime-test \
  --config-file "$REALTIME_CONFIG" --realtime-file "$REALTIME_STORE" \
  > "$REALTIME_LOG" 2>&1
realtime_status=$?
set -e
cat "$REALTIME_LOG" >> "$LOG_FILE"
if [[ "$realtime_status" -eq 0 ]]; then
  record_pass "实时页控制器链路自检通过（$(grep -c '   ok   ' "$REALTIME_LOG" || true) 项观测全部通过）"
else
  record_fail "实时页控制器链路自检退出码 $realtime_status"
  grep 'FAIL' "$REALTIME_LOG" | tail -5
fi
# 固定格式的输出行：脚本按行断言，不靠"程序自己说成功"。
for pattern in "config strategy=full debounce=200 max_wait=2000 retain=3" \
               "config strategy=incremental debounce=200 max_wait=2000 retain=3" \
               "attach watches=" \
               "step1 kind=full-snapshot name=" \
               "step2 kind=full-snapshot name=" \
               "history count=" \
               "ok"; do
  if grep -qF -- "[realtime] $pattern" "$REALTIME_LOG"; then
    record_pass "实时自检输出：$pattern"
  else
    record_fail "实时自检缺少输出：$pattern"
  fi
done
# 两次触发的归档名必须不同：只断言"有 name=" 会漏掉"第二份没有真的新建"。
realtime_first="$(grep -oE '^\[realtime\] step1 kind=[^ ]+ name=.*$' "$REALTIME_LOG" | head -1 || true)"
realtime_second="$(grep -oE '^\[realtime\] step2 kind=[^ ]+ name=.*$' "$REALTIME_LOG" | head -1 || true)"
if [[ -n "$realtime_first" && -n "$realtime_second" && "$realtime_first" != "$realtime_second" ]]; then
  record_pass "实时自检的两次触发产出了不同的归档"
else
  record_fail "实时自检的两次触发没有产出不同的归档"
fi
if [[ -s "$REALTIME_STORE" ]]; then
  record_pass "自检写出的 realtime.json 存在"
else
  record_fail "自检没有写出 realtime.json"
fi

echo "[modern-gui] 17) 配置路径隔离（显式参数优先 / 自检不写真实 profile）"
#
# 人工验收现场：Demo 的 run.sh 不带参数启动，界面上出现了
# /tmp/backup-gui-modern-mJeWnE/repository。根因不是"路径解析错了"，而是以前某次
# 自检没带 --config-file，把自检的临时仓库写进了真实用户 profile 的 config.json，
# 于是一次普通启动读出来一个早就被删掉的临时仓库。
# 这一节把三件事钉死：
#   a) 非自检启动仍然走默认 AppPaths（隔离逻辑不许误伤正常启动）；
#   b) 自检模式在没给 --config-file/--schedule-file/--realtime-file 时自己隔离，
#      默认 profile 一个字节都不许变，也不许出现 backup-gui-modern-* 临时路径；
#   c) 显式给出的路径永远优先，自检就写在显式文件上。
ISO_HOME="$TEST_STATE_DIR/isolation-home"
ISO_XDG="$ISO_HOME/.config"
ISO_PROFILE="$ISO_XDG/backup-project/backup-gui-modern"
ISO_MARKER_REPO="$TEST_STATE_DIR/isolation-marker-repository"
ISO_MARKER_CFG="$ISO_PROFILE/config.json"
ISO_LOG="$TEST_STATE_DIR/isolation.log"
ISO_TTY_LOG="$TEST_STATE_DIR/isolation-tty.log"
ISO_EXPLICIT_DIR="$TEST_STATE_DIR/isolation-explicit"
ISO_EXPLICIT_CFG="$ISO_EXPLICIT_DIR/config.json"
ISO_EXPLICIT_RT="$ISO_EXPLICIT_DIR/realtime.json"
rm -rf "$ISO_HOME" "$ISO_EXPLICIT_DIR"
mkdir -p "$ISO_PROFILE" "$ISO_MARKER_REPO" "$ISO_EXPLICIT_DIR"
printf '{\n  "version": 1,\n  "backup_repository_path": "%s"\n}\n' "$ISO_MARKER_REPO" > "$ISO_MARKER_CFG"
printf '{\n  "version": 1,\n  "backup_repository_path": "%s"\n}\n' "$TEST_STATE_DIR/isolation-explicit-repository" > "$ISO_EXPLICIT_CFG"
iso_marker_md5="$(md5sum "$ISO_MARKER_CFG" | cut -d' ' -f1)"

# a) 非自检模式：--realtime-show 走默认 AppPaths，读到的必须是 marker 仓库。
set +e
XDG_CONFIG_HOME="$ISO_XDG" HOME="$ISO_HOME" QT_QPA_PLATFORM=offscreen \
  ./build/backup-gui-modern --realtime-show > "$ISO_LOG" 2>&1
iso_show_status=$?
set -e
if [[ "$iso_show_status" -eq 0 ]] && grep -qF -- "repository=$ISO_MARKER_REPO" "$ISO_LOG"; then
  record_pass "非自检启动仍按默认 AppPaths 读 profile（隔离逻辑没有误伤正常启动）"
else
  record_fail "非自检启动没有读到默认 profile 的仓库（退出码 $iso_show_status）"
fi

# b) 自检模式 + 不给任何 --*-file：必须自己隔离，默认 profile 不许被动。
set +e
XDG_CONFIG_HOME="$ISO_XDG" HOME="$ISO_HOME" QT_QPA_PLATFORM=offscreen timeout 180 \
  ./build/backup-gui-modern --realtime-test > "$ISO_LOG" 2>&1
iso_selftest_status=$?
set -e
if [[ "$iso_selftest_status" -eq 0 ]]; then
  record_pass "自检模式（不带 --*-file）自身仍然通过"
else
  record_fail "自检模式（不带 --*-file）退出码 $iso_selftest_status"
fi
# 自检提示是给人看的：stderr 被重定向时（自动化 / parity 对照）它必须让位，
# 否则真实业务错误不再是第一条 stderr，GUI/CLI 的错误契约就被这行提示遮住了。
# 隔离本身由下面两条硬不变量证明，不依赖这行提示。
if grep -qF -- "[self-check] 隔离配置目录" "$ISO_LOG"; then
  record_fail "重定向 stderr 时自检提示仍然出现（会遮住真实业务错误）"
else
  record_pass "重定向 stderr 时自检提示不出现（真实错误保住第一条 stderr）"
fi
# 交互终端上人仍然要看得到它：给它一个真正的 pty，再跑一次同样的自检。
set +e
XDG_CONFIG_HOME="$ISO_XDG" HOME="$ISO_HOME" QT_QPA_PLATFORM=offscreen timeout 180 \
  script -qec "./build/backup-gui-modern --realtime-test" /dev/null > "$ISO_TTY_LOG" 2>&1
iso_tty_status=$?
set -e
if [[ "$iso_tty_status" -eq 0 ]] && grep -qF -- "[self-check] 隔离配置目录" "$ISO_TTY_LOG"; then
  record_pass "交互终端（pty）上仍然报出隔离目录，人没有失去这条诊断"
else
  record_fail "交互终端上看不到隔离目录提示（pty 退出码 $iso_tty_status）"
fi
if [[ "$(md5sum "$ISO_MARKER_CFG" | cut -d' ' -f1)" == "$iso_marker_md5" ]]; then
  record_pass "自检模式没有改写默认 profile 的 config.json"
else
  record_fail "自检模式改写了默认 profile 的 config.json"
fi
if grep -rqF -- "backup-gui-modern-" "$ISO_XDG" 2>/dev/null; then
  record_fail "默认 profile 里出现了 backup-gui-modern-* 临时路径"
else
  record_pass "普通/自检启动都不会把 backup-gui-modern-* 临时路径写进默认 profile"
fi

# c) 自检 + 显式路径：显式文件被真正使用，默认 profile 依旧不动。
set +e
XDG_CONFIG_HOME="$ISO_XDG" HOME="$ISO_HOME" QT_QPA_PLATFORM=offscreen timeout 180 \
  ./build/backup-gui-modern --realtime-test \
  --config-file "$ISO_EXPLICIT_CFG" --realtime-file "$ISO_EXPLICIT_RT" > "$ISO_LOG" 2>&1
iso_explicit_status=$?
set -e
if [[ "$iso_explicit_status" -eq 0 && -s "$ISO_EXPLICIT_RT" ]]; then
  record_pass "自检 + 显式 --realtime-file：显式文件被真正使用"
else
  record_fail "自检 + 显式 --realtime-file 没有写出显式 realtime.json（退出码 $iso_explicit_status）"
fi
if grep -qF -- "backup-gui-modern-" "$ISO_EXPLICIT_CFG"; then
  record_pass "自检的临时仓库写在显式 --config-file 上（证明显式路径优先）"
else
  record_fail "显式 --config-file 没有被自检使用"
fi
if [[ "$(md5sum "$ISO_MARKER_CFG" | cut -d' ' -f1)" == "$iso_marker_md5" ]]; then
  record_pass "显式路径生效时默认 profile 仍然没有被改动"
else
  record_fail "显式路径生效时默认 profile 被改动了"
fi

# 静态面：隔离只挂在自检开关上，正常启动那条路径不允许出现 QTemporaryDir profile。
expect_present "$ROOT_DIR/ui/modern/main.cpp" "const bool self_check_mode =" \
  "main.cpp 有自检模式判定（隔离只对自检生效）"
expect_count "$ROOT_DIR/ui/modern/main.cpp" "static QTemporaryDir self_check_profile;" 1 \
  "隔离目录只有一处声明"
expect_present "$ROOT_DIR/ui/modern/main.cpp" "if (self_check_mode) {" \
  "路径重定向写在自检分支里"
expect_count "$ROOT_DIR/ui/modern/main.cpp" "QString config_file_path = ResolveConfigFilePath(arguments);" 1 \
  "配置路径只解析一次（解析后即定型，后面不再回默认值）"
expect_count "$ROOT_DIR/ui/modern/main.cpp" "QString schedule_file_path = ResolveScheduleFilePath(arguments);" 1 \
  "计划存储路径只解析一次"
expect_count "$ROOT_DIR/ui/modern/main.cpp" "QString realtime_file_path = ResolveRealtimeFilePath(arguments);" 1 \
  "实时存储路径只解析一次"

echo "[modern-gui] 18) 浅色主题 hover 结构回归（transparent × 颜色动画 = 黑闪）"
#
# 人工现场：浅色主题下鼠标进出任意可 hover 的块会先"黑一下"再恢复。
# 根因是共享组件的 background 在 "transparent"（RGBA 0,0,0,0）与不透明 hover 色
# 之间做 ColorAnimation —— 逐分量插值 alpha 与 RGB，中间帧就是"半透明黑"叠在
# 浅色底上（深色主题底色本就暗，所以看不出）。
# 修法：固定主题色覆盖层 + 只动画 opacity。这里把它钉成结构契约：
# 只要有文件同时出现 transparent 与 Behavior on color，黑闪就可能回来。
hover_bad="$(grep -rl --include=*.qml -- "transparent" "$QML_DIR" | xargs -r grep -l -- "Behavior on color" || true)"
if [[ -z "$hover_bad" ]]; then
  record_pass "没有 QML 同时出现 transparent 与 Behavior on color（黑闪根因不会再回来）"
else
  record_fail "仍有文件同时出现 transparent 与 Behavior on color：$hover_bad"
fi
expect_count "$QML_DIR/components/NavItem.qml" "Behavior on color" 0 \
  "NavItem 不再对颜色做动画"
expect_count "$QML_DIR/components/AppButton.qml" "Behavior on color" 0 \
  "AppButton 不再对颜色做动画"
expect_count "$QML_DIR/components/NavItem.qml" "Behavior on opacity" 2 \
  "NavItem 选中 / hover 各一条 opacity 动画"
expect_count "$QML_DIR/components/AppButton.qml" "Behavior on opacity" 2 \
  "AppButton hover / pressed 各一条 opacity 动画"
expect_present "$QML_DIR/components/NavItem.qml" "color: theme.hover" \
  "NavItem 的 hover 覆盖层用固定主题色"
expect_present "$QML_DIR/components/AppButton.qml" \
  "color: control.primary ? theme.accentHover : control.hoverColor" \
  "AppButton 的 hover 覆盖层用固定主题色"
# 输入框动画的是 border.color，两端都是不透明色，不是黑闪来源，明确保留。
expect_present "$QML_DIR/components/AppTextField.qml" "Behavior on border.color" \
  "输入框保留边框色过渡（两端不透明，非黑闪来源）"

echo "[modern-gui] 19) 三页共用的 Filter UX（可视化条件 + 高级 DSL）"
#
# 人工验收："普通用户仍被要求直接输入 ext:cpp;h、path:**/build/**、Include / Exclude、
# Debounce、Max wait 等内部概念。"这一节把"三处共用同一个规则编辑器"钉成契约，
# 并跑一遍 **--filter-ux-test**：它真的切页、真的点按钮、真的往输入框里打字，
# 然后断言三处拿到的是同一条 DSL。
SHARED_EDITOR="$QML_DIR/components/FilterRuleEditor.qml"
expect_count "$RESOURCE_FILE" "qml/components/FilterRuleEditor.qml" 1 \
  "共享规则编辑器进了资源清单"
expect_present "$QML_DIR/components/FilterEditorPanel.qml" "FilterRuleEditor {" \
  "备份页用共享规则编辑器"
expect_present "$SCHEDULE_PAGE_QML" "FilterRuleEditor {" \
  "自动备份页用共享规则编辑器"
expect_present "$REALTIME_PAGE_QML" "FilterRuleEditor {" \
  "实时备份页用共享规则编辑器"
# 每个页面只有一个规则编辑器实例，而且都是共享组件（不是各写一套）。
expect_count "$SCHEDULE_PAGE_QML" "FilterRuleEditor {" 1 \
  "自动备份页只有一个规则编辑器实例"
expect_count "$REALTIME_PAGE_QML" "FilterRuleEditor {" 1 \
  "实时页只有一个规则编辑器实例"
expect_count "$QML_DIR/components/FilterEditorPanel.qml" "FilterRuleEditor {" 1 \
  "备份页只有一个规则编辑器实例"
# 两个自动化页面都不再自己维护 include / exclude 文本列表。
for page_file in "$SCHEDULE_PAGE_QML" "$REALTIME_PAGE_QML"; do
  if grep -q "draftInclude" "$page_file" || grep -q "draftExclude" "$page_file"; then
    record_fail "$(basename "$page_file") 仍然自己维护 include / exclude 列表"
  else
    record_pass "$(basename "$page_file") 不再自己维护 include / exclude 列表"
  fi
done

# --- 普通模式：条件类型是中文下拉，不是裸文本框 ---
for label in "文件扩展名" "文件名" "路径" "主文件名" "文件类型" "文件大小" \
             "用户 ID" "用户组 ID" "修改时间"; do
  if grep -qF "return \"$label\";" "$ROOT_DIR/src/filter/filter_rule_builder.cpp"; then
    record_pass "条件类型里有“$label”"
  else
    record_fail "条件类型里缺“$label”"
  fi
done
expect_present "$SHARED_EDITOR" "RuleFieldCombo" \
  "条件类型是一个 ComboBox（用户从下拉里选，不写 ext:）"
expect_present "$SHARED_EDITOR" "RuleTypeCombo" \
  "文件类型用 ComboBox"
expect_present "$SHARED_EDITOR" "RuleSizeCompareCombo" \
  "文件大小先选比较方式"
expect_present "$SHARED_EDITOR" "RuleSizeValueField" \
  "文件大小再填数值"
expect_present "$SHARED_EDITOR" "RuleSizeUnitCombo" \
  "文件大小最后选单位"
expect_missing "$SHARED_EDITOR" "placeholderText: \"ext:" \
  "普通模式的占位符不教用户写 DSL"
expect_missing "$SHARED_EDITOR" "placeholderText: \"size:" \
  "普通模式的占位符不教用户写 DSL"

# --- 高级 DSL：默认折叠，但能力一点没少 ---
expect_count "$SHARED_EDITOR" "property bool advancedExpanded: false" 1 \
  "高级规则默认收起"
expect_present "$SHARED_EDITOR" 'objectName: editor.nameOf("AdvancedRuleField")' \
  "高级规则仍然可以写完整 DSL"
expect_present "$SHARED_EDITOR" "editor.ruleModel.addAdvancedRule(" \
  "高级规则走的是共享模型（语法裁决在 Filter::AddRule）"

# --- 主行讲人话，DSL 降级 ---
expect_present "$QML_DIR/components/RuleCard.qml" "actionLabel" \
  "规则卡片的主行用中文动作名"
expect_present "$QML_DIR/components/RuleCard.qml" "conditionLabel" \
  "规则卡片的主行用中文条件摘要"
expect_present "$ROOT_DIR/src/filter/filter_rule_builder.cpp" "SummarizeClauseShort" \
  "短摘要由共享 builder 生成（界面不自己拼术语）"
expect_missing "$QML_DIR/components/RuleCard.qml" '(card.isInclude ? "Include" : "Exclude")' \
  "规则卡片不再直接显示 Include / Exclude"

# --- 三处 parity：真实 GUI 交互 + 生成的 DSL ---
set +e
QT_QPA_PLATFORM=offscreen QSG_RHI_BACKEND=software timeout 300 \
  ./build/backup-gui-modern --filter-ux-test \
  --config-file "$TEST_STATE_DIR/fux-config.json" \
  --schedule-file "$TEST_STATE_DIR/fux-schedule.json" \
  --realtime-file "$TEST_STATE_DIR/fux-realtime.json" \
  > "$TEST_STATE_DIR/filter-ux.log" 2>&1
fux_status=$?
set -e
cat "$TEST_STATE_DIR/filter-ux.log" >> "$LOG_FILE"
if [[ "$fux_status" -eq 0 ]]; then
  record_pass "三页 Filter UX parity 自检通过（$(grep -c '   ok   ' "$TEST_STATE_DIR/filter-ux.log" || true) 项观测全部通过）"
else
  record_fail "三页 Filter UX parity 自检退出码 $fux_status"
  grep 'FAIL' "$TEST_STATE_DIR/filter-ux.log" | tail -8
fi
# 固定格式的关键行：脚本按行断言，不靠"程序自己说成功"。
for pattern in "PARITY-01 三处的包含规则逐字相同" \
               "PARITY-02 普通表单输入生成的就是核心认可的 DSL" \
               "PARITY-03 三处的排除规则逐字相同" \
               "PARITY-04 非法输入在三处得到同一句原因（来自共享 builder）"; do
  if grep -qF -- "$pattern" "$TEST_STATE_DIR/filter-ux.log"; then
    record_pass "Filter UX parity：$pattern"
  else
    record_fail "Filter UX parity 缺少：$pattern"
  fi
done
# 三处各自都要真的走完一遍（不是只测了某一页）。
for label in "备份页" "自动备份页" "实时备份页"; do
  if grep -qF -- "$label 生成 ext:txt;md（用户没有写过 ext:）" "$TEST_STATE_DIR/filter-ux.log"; then
    record_pass "$label 走完了真实 GUI 交互并生成 ext:txt;md"
  else
    record_fail "$label 没有走完真实 GUI 交互"
  fi
done
if grep -qF "qml-warning" "$TEST_STATE_DIR/filter-ux.log"; then
  record_fail "Filter UX 自检期间出现了 QML 运行期告警"
else
  record_pass "Filter UX 自检期间 0 QML 运行期告警"
fi

echo "[modern-gui] 20) 备份频率（每 N 单位）与存储 schema 不变"
#
# 人工验收："周期 [60] 分钟"功能没错但体验差。界面改成 值 + 单位，存储与 CLI
# 继续只看 interval_minutes —— 这一节同时钉住"界面变好"和"schema 没变"。
expect_present "$ROOT_DIR/ui/modern/schedule_frequency.h" "SplitFrequency" \
  "频率的反向折算（取最大整除单位）在 C++ 里"
expect_present "$ROOT_DIR/ui/modern/schedule_frequency.cpp" \
  "LargestExactFrequencyUnit" \
  "120 分钟必须显示成“每 2 小时”，不能显示成“每 120 分钟”"
expect_missing "$ROOT_DIR/ui/modern/schedule_frequency.cpp" "double" \
  "频率换算全程是整数（没有浮点，也就没有 1.5 小时）"
if grep -qF '{"weeks", "周", 10080u}' "$ROOT_DIR/ui/modern/schedule_frequency.cpp" &&
   grep -qF '{"days", "天", 1440u}' "$ROOT_DIR/ui/modern/schedule_frequency.cpp" &&
   grep -qF '{"hours", "小时", 60u}' "$ROOT_DIR/ui/modern/schedule_frequency.cpp" &&
   grep -qF '{"minutes", "分钟", 1u}' "$ROOT_DIR/ui/modern/schedule_frequency.cpp"; then
  record_pass "频率单位表是 分钟 / 小时 / 天 / 周"
else
  record_fail "频率单位表缺项"
fi
# 频率自检的输出（--schedule-test 里那一组 FREQ-xx）已经在第 15 节跑过，
# 这里只复核关键几行确实出现过。
for pattern in "FREQ-01 每 1 小时 -> 60 分钟" \
               "FREQ-01 每 2 天 -> 2880 分钟" \
               "FREQ-01 每 1 周 -> 10080 分钟" \
               "FREQ-02 60 分钟 -> 每 1 小时" \
               "FREQ-02 120 分钟 -> 每 2 小时" \
               "FREQ-02 1440 分钟 -> 每 1 天" \
               "FREQ-02 10080 分钟 -> 每 1 周" \
               "FREQ-02 90 分钟 -> 每 90 分钟" \
               "FREQ-03 拒绝 每 0 分钟" \
               "FREQ-03 拒绝 每 -1 小时" \
               "FREQ-03 拒绝 每 525601 分钟" \
               "FREQ-03 拒绝 每 1000 周" \
               "FREQ-02 2880 分钟 -> 每 2 天"; do
  if grep -qF -- "$pattern" "$LOG_FILE"; then
    record_pass "频率自检：$pattern"
  else
    record_fail "频率自检缺少：$pattern"
  fi
done
# schema：store 里只有 interval_minutes，没有任何单位字段。
if grep -qF '"interval_minutes"' "$ROOT_DIR/src/scheduler/schedule_store.cpp" &&
   ! grep -qE '"interval_(hours|days|weeks|unit)"' \
     "$ROOT_DIR/src/scheduler/schedule_store.cpp"; then
  record_pass "计划存储 schema 未变（仍然只有 interval_minutes）"
else
  record_fail "计划存储 schema 出现了单位字段"
fi

echo "[modern-gui] 21) 用户 UI 不出现开发者术语"
#
# 普通用户看到的每一句都应该是"这东西帮我做什么"。命令行工具名、内部格式名、
# 状态机字段只在「技术详情」里出现，或者根本不出现。
expect_missing "$REALTIME_PAGE_QML" "Filter parser" \
  "实时页不解释 Filter parser"
expect_missing "$REALTIME_PAGE_QML" "共享核心解析并校验" \
  "实时页不再写“规则由共享核心解析并校验”（那是源码注释，不是用户帮助）"
expect_missing "$REALTIME_PAGE_QML" "这里不做第二套解析" \
  "实时页不再写“这里不做第二套解析”"
expect_missing "$SCHEDULE_PAGE_QML" "这里不做第二套解析" \
  "自动备份页不再写“这里不做第二套解析”"
expect_missing "$SCHEDULE_PAGE_QML" 'text: "筛选规则（include / exclude）"' \
  "自动备份页不再用 include / exclude 当标题"
expect_missing "$SCHEDULE_PAGE_QML" '"添加 include"' \
  "自动备份页不再有“添加 include”按钮"
expect_missing "$SCHEDULE_PAGE_QML" '"添加 exclude"' \
  "自动备份页不再有“添加 exclude”按钮"
expect_missing "$REALTIME_PAGE_QML" '"Include"' \
  "实时页不再出现裸 Include 文案"
expect_present "$QML_DIR/components/FilterRuleEditor.qml" '"包含"' \
  "包含 / 排除才是普通 UI 的措辞"
expect_present "$SCHEDULE_PAGE_QML" '"保存设置"' \
  "自动备份页的按钮叫“保存设置”"
expect_present "$SCHEDULE_PAGE_QML" '"立即执行一次"' \
  "自动备份页的按钮叫“立即执行一次”"
expect_present "$SCHEDULE_PAGE_QML" "立即检查当前状态，并在需要时创建备份。" \
  "按钮附近说明“没有变化时不会产生新备份”，不做过度承诺"
expect_missing "$SCHEDULE_PAGE_QML" '"保存计划"' \
  "旧文案“保存计划”已经消失"

# 远程备份页同理：普通用户只需要知道"服务器 / 账号 / 云端备份 / 上传 / 下载 /
# 删除"，协议名、算法名、数据库名与传输层实现细节都不该出现在界面文案里。
# 只看代码行：注释里写"这里没有 BPNET1 / token"正是这条约束的说明，
# 不能把它自己判成违规（与上面 socket 那条用的是同一份去注释文本）。
for jargon in "BPNET1" "PBKDF2" "HMAC" "SQLite" "opcode" "request_id" \
              "FrameHeader" "kProtocolMagic" "SSH" "token"; do
  if grep -qF -- "$jargon" "$REMOTE_CODE_TMP"; then
    record_fail "远程备份页出现了开发者术语：$jargon"
  else
    record_pass "远程备份页不出现开发者术语：$jargon"
  fi
done
expect_present "$REMOTE_PAGE_QML" '"远程备份"' \
  "导航与标题用“远程备份”这个说法"
# 账户区域现在是"服务器账户"卡片：地址 / 端口 / 用户名 + 登录、注册两个标签页。
expect_present "$REMOTE_PAGE_QML" '"服务器账户"' \
  "远程备份页有账户区域（地址 / 端口 / 用户名 + 登录注册标签页）"
expect_present "$REMOTE_PAGE_QML" '"云端备份"' \
  "远程备份页有云端备份区域"
expect_present "$REMOTE_PAGE_QML" '"技术详情"' \
  "协议层面的信息折叠进“技术详情”"
expect_present "$REMOTE_PAGE_QML" '"覆盖并重新下载"' \
  "目标已存在时给的是明确动作，不是常驻开关"

echo "[modern-gui] 22) 共享 ComboBox 的下拉行状态（hover / 键盘光标 / 已选择）"
#
# 三轮人工验收踩的是同一个坑的三种形态，根因都是"用一个残影当输入状态用"：
#
#   第一轮：background 里 row.highlighted（= control.highlightedIndex === index）
#           与 row.hovered 返回同一块 theme.hover。打开下拉时 Qt 就把
#           highlightedIndex 设成当前已选择项，于是那一行从打开起就是灰的，
#           鼠标移开也不会消失。
#   第二轮：换成"键盘高亮 + accentSoft"之后仍然残留 —— highlightedIndex 会被
#           鼠标改脏（实测：指针移出 popup 后 highlightedIndex=4、row.hovered=false，
#           那一行照样被画出一块底色）。
#   第三轮：改成监听 Keys.onPressed 之后，真实桌面里键盘高亮**永远不亮** ——
#           真实 xcb 窗口实测：点开下拉后焦点在 ComboBox 上，按键由 ComboBox 处理，
#           popup 的 ListView 收不到 Keys；而它才是真正移动的对象
#           （0 -> 1 -> 2 -> 1，control.currentIndex 在 Enter 之前一直不变）。
#
# 结论：不要监听"用户有没有按键"，要观察"Qt 把键盘位置移到了哪里"。
# 键盘模式 = popup ListView 的 currentIndex 变了 && 没有指针活动 && 不是打开时的
# 初次同步。键盘光标就画在 currentIndex 那一行，与"已选择项"可以叠加。
COMBO_QML="$QML_DIR/components/AppComboBox.qml"

# --- 视觉：三种状态各走各的通道 ---
expect_present "$COMBO_QML" "hoverEnabled: true" \
  "下拉行显式打开 hover（默认值来自系统 style hint，headless 下不一定为真）"
expect_present "$COMBO_QML" "opacity: row.hovered ? 1 : 0" \
  "唯一的 hover 灰由 row.hovered 决定"
expect_present "$COMBO_QML" "objectName: \"comboItemHoverLayer\"" \
  "hover 是一个独立的固定色覆盖层"
expect_present "$COMBO_QML" "objectName: \"comboItemKeyboardLayer\"" \
  "键盘光标是另一个独立的固定色覆盖层"
expect_present "$COMBO_QML" "opacity: row.keyboardHighlighted ? 1 : 0" \
  "键盘层由 keyboardHighlighted 决定"
expect_missing "$COMBO_QML" "Behavior on color" \
  "两个覆盖层都不做颜色动画（浅色主题那一闪的根因）"
# hover 层必须画在键盘层之上：两者同时亮时看到的是 hover。
hover_layer_line="$(grep -n 'objectName: "comboItemHoverLayer"' "$COMBO_QML" | cut -d: -f1)"
keyboard_layer_line="$(grep -n 'objectName: "comboItemKeyboardLayer"' "$COMBO_QML" | cut -d: -f1)"
if [[ -n "$keyboard_layer_line" && -n "$hover_layer_line" && "$keyboard_layer_line" -lt "$hover_layer_line" ]]; then
  record_pass "hover 覆盖层画在键盘层之上（鼠标优先级更高）"
else
  record_fail "hover / 键盘覆盖层的叠放顺序不对"
fi

# --- 输入方式：由导航结果推断，不监听按键 ---
expect_present "$COMBO_QML" "property bool keyboardNavigationActive: false" \
  "有一个明确的输入方式闸门（纯 presentation 状态）"
expect_present "$COMBO_QML" "onCurrentIndexChanged: control.noteNavigationResult()" \
  "键盘模式来自 popup ListView 的导航结果"
expect_present "$COMBO_QML" "function noteNavigationResult()" \
  "推断逻辑有独立入口"
expect_present "$COMBO_QML" "if (control.suppressNavigationInference) return" \
  "打开 popup 时的初次同步不算导航"
expect_present "$COMBO_QML" "if (control.pointerHovering) return" \
  "有行正被 hover 时不推断键盘模式（hover 引起的 currentIndex 变化归鼠标）"
expect_missing "$COMBO_QML" "Timer {" \
  "推断不需要任何延时窗口（同步标志就够，也不引入额外类型）"
expect_present "$COMBO_QML" "Qt.callLater(function () {" \
  "初次同步的抑制在下一个事件循环解除（不用 sleep）"
# 只看代码行：注释里解释"前两轮试过 Keys.onPressed / 接管事件"是正常的历史说明。
expect_missing_code "$COMBO_QML" "Keys.onPressed" \
  "不再监听按键：真实桌面里按键根本不到 popup"
# 只禁"放行按键"那一种写法；WheelHandler 里的 event.accepted = true 是滚轮自己的
# 处理，和键盘无关，不在这一条的范围内。
expect_missing_code "$COMBO_QML" "event.accepted = false" \
  "不接管任何按键，Qt 的方向键行为保持原样"
expect_present "$COMBO_QML" "onHoveredChanged: {" \
  "指针进入某一行时记录 hover 状态并让位"
expect_present "$COMBO_QML" "control.pointerHovering = row.hovered" \
  "当前是否有 delegate 真的 hovered"
expect_present "$COMBO_QML" "onOpened: {" \
  "每次打开 popup 都复位"
expect_present "$COMBO_QML" "onClosed: {" \
  "关闭时也复位"

# --- 禁止再用 highlightedIndex 直接推断输入来源 ---
expect_missing "$COMBO_QML" "control.highlightedIndex === index" \
  "没有任何视觉分支直接拿 highlightedIndex 当状态"
combo_highlight_code_hits="$(grep -nF 'control.highlightedIndex' "$COMBO_QML" \
  | grep -vE '^[0-9]+:[[:space:]]*//' | wc -l)"
if [[ "$combo_highlight_code_hits" -eq 1 ]]; then
  record_pass "代码里只剩 Qt 自己的那一条 ListView 绑定用 highlightedIndex"
else
  record_fail "代码里有 $combo_highlight_code_hits 处在用 highlightedIndex（应当只有 1 处）"
fi
expect_present "$COMBO_QML" "currentIndex: control.highlightedIndex" \
  "剩下那一处是 ListView 的 currentIndex 绑定，不是视觉状态"
expect_present "$COMBO_QML" "control.keyboardRowIndex === index" \
  "键盘光标画在 popup ListView 的 currentIndex 那一行上"
# 键盘光标与"已选择项"可以叠加：keyboardHighlighted 不许再排除 isSelected。
keyboard_rule="$(grep -A3 'property bool keyboardHighlighted' "$COMBO_QML")"
if printf '%s' "$keyboard_rule" | grep -q "isSelected"; then
  record_fail "键盘光标把已选择项排除了（键盘导航回当前值时会看不见光标）"
else
  record_pass "键盘光标不排除已选择项，可以与勾号叠加"
fi

# --- 已选择项：勾号 + 强调色文字，不占底色 ---
expect_present "$COMBO_QML" 'objectName: "comboItemCheck"' \
  "已选择项有独立的勾号标记"
expect_present "$COMBO_QML" "row.isSelected ? Font.DemiBold : Font.Normal" \
  "已选择项用字重区分"
expect_present "$COMBO_QML" "row.isSelected ? theme.accent : theme.textPrimary" \
  "已选择项用强调色文字区分"

# --- 运行期：真实指针 + 真实焦点链上的方向键 ---
set +e
QT_QPA_PLATFORM=offscreen QSG_RHI_BACKEND=software timeout 240 \
  ./build/backup-gui-modern --combo-hover-test \
  --config-file "$TEST_STATE_DIR/combo-config.json" \
  --schedule-file "$TEST_STATE_DIR/combo-schedule.json" \
  --realtime-file "$TEST_STATE_DIR/combo-realtime.json" \
  > "$TEST_STATE_DIR/combo-hover.log" 2>&1
combo_status=$?
set -e
cat "$TEST_STATE_DIR/combo-hover.log" >> "$LOG_FILE"
if [[ "$combo_status" -eq 0 ]]; then
  record_pass "共享下拉状态自检通过（$(grep -c '   ok   ' "$TEST_STATE_DIR/combo-hover.log" || true) 项观测全部通过）"
else
  record_fail "共享下拉状态自检退出码 $combo_status"
  grep 'FAIL' "$TEST_STATE_DIR/combo-hover.log" | tail -8
fi
for pattern in "Mouse 1 打开 popup：无 hover 底色、无键盘光标、模式关闭" \
               "Mouse 1 已选择项（路径）只有勾号，没有底色" \
               "Mouse 2 只有“文件类型”有 hover 灰底，且没有误触发键盘模式" \
               "Mouse 3 指针离开后索引仍停在被划过的那一行" \
               "Mouse 3 索引留在那一行，但视觉上没有任何底色" \
               "Mouse 4 指针重新进入：hover 立刻接管" \
               "Keyboard 1 ↓ 让 popup 的 currentIndex 前移一项" \
               "Keyboard 1 ↓ 之后键盘模式打开（由导航结果推断）" \
               "Keyboard 1 键盘光标正好落在 Qt 移动到的那个 row 上" \
               "Keyboard 2 再 ↓：光标整体下移一行，上一行立刻熄灭" \
               "Keyboard 3 ↑ 把光标移回上一行" \
               "Keyboard 4 键盘光标落在已选择项上：光标与勾号同时可见" \
               "Mouse 5 键盘模式下移动鼠标：键盘光标立即消失、hover 接管" \
               "Mouse 6 指针离开后没有任何底色残留" \
               "Keyboard 5 Enter 采纳当前键盘行并关闭下拉" \
               "Keyboard 6 Esc 关闭下拉且不改动已选择的值" \
               "Reopen 重新打开下拉：没有 stale 键盘光标、没有 stale 灰底"; do
  if grep -qF -- "$pattern" "$TEST_STATE_DIR/combo-hover.log"; then
    record_pass "下拉状态自检：$pattern"
  else
    record_fail "下拉状态自检缺少：$pattern"
  fi
done
if grep -qF "qml-warning" "$TEST_STATE_DIR/combo-hover.log"; then
  record_fail "下拉状态自检期间出现了 QML 运行期告警"
else
  record_pass "下拉状态自检期间 0 QML 运行期告警"
fi

echo "[modern-gui] 23) 远程备份页（RemoteController + 真实 backup-server 进程）"
#
# 这一节不是 grep：它真的起一个 backup-server，再用页面背后的 RemoteController
# 走完 注册 -> 登录 -> 上传真实归档 -> 列表 -> 下载 -> 删除 -> 退出登录，
# 并断言密码回显模式、口令与 token 不落盘、忙碌时冲突请求被拒、页面提示不外泄、
# 列表行显示名称 / 大小 / 时间、删除必须确认、两套主题下控件几何正常。
mkdir -p "$TEST_STATE_DIR/remote"
set +e
QT_QPA_PLATFORM=offscreen QSG_RHI_BACKEND=software timeout 600 \
  ./build/backup-gui-modern --remote-test \
  --config-file "$TEST_STATE_DIR/remote/config.json" \
  --schedule-file "$TEST_STATE_DIR/remote/schedule.json" \
  --realtime-file "$TEST_STATE_DIR/remote/realtime.json" \
  > "$TEST_STATE_DIR/remote.log" 2>&1
remote_status=$?
set -e
sed 's/^/[modern-gui]     /' "$TEST_STATE_DIR/remote.log"
cat "$TEST_STATE_DIR/remote.log" >> "$LOG_FILE"
if [[ "$remote_status" -eq 0 ]]; then
  record_pass "远程备份页合同测试全部通过（$(grep -oE 'passed=[0-9]+ failed=[0-9]+' "$TEST_STATE_DIR/remote.log" | tail -1)）"
else
  record_fail "远程备份页合同测试失败（退出码 $remote_status）" \
    "$(grep -m3 'FAIL' "$TEST_STATE_DIR/remote.log" | tr '\n' ' ')"
fi
if grep -qF "qml-warning" "$TEST_STATE_DIR/remote.log"; then
  record_fail "远程备份页自检期间出现了 QML 运行期告警"
else
  record_pass "远程备份页自检期间 0 QML 运行期告警"
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

