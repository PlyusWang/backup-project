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
#      以及 repository-driven 架构约束（四页结构、没有 standalone 恢复页、
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
#  12. 截图（写进 tests/output/，评审产物不进仓库）：两套主题 × 四页 +
#      高级选项展开 + 加密恢复密码对话框。
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
    BEGIN { prev_panel_allowed = 0 }
    # 上一条诊断是否“已精确放行且来自 FilterEditorPanel.qml”。
    # 只用来放行紧跟其后的那条 companion Info，遇到任何别的诊断立即清空。
    function MarkAllowed(msg) {
      print "ALLOWED\t" msg
      prev_panel_allowed = (msg ~ /FilterEditorPanel\.qml/) ? 1 : 0
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
      # PR #12：紧随上述已放行主诊断的 companion Info（qmllint 不给它文件路径）。
      # 只认这一句精确文本，且只在直接前一条是 FilterEditorPanel.qml 的已放行诊断时才放行；
      # 放行后立刻清状态，避免变成“全局允许某类提示”。
      if (msg ~ /^Info: ruleModel is a member of a parent element\.?$/ && prev_panel_allowed == 1) {
        print "ALLOWED\t" msg
        prev_panel_allowed = 0
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

# 页面结构：首页 / 备份 / 备份管理 / 设置 四页。
# 恢复已经不是独立页面，而是备份管理页里的一个动作 —— 这几条断言把结构钉死，
# 免得日后又长回一个"恢复页"。
expect_count "$QML_DIR/Main.qml" "NavItem {" 4 \
  "侧栏有四个导航项（首页 / 备份 / 备份管理 / 设置）"
expect_count "$QML_DIR/Main.qml" "opacity: root.currentPage === " 4 \
  "StackLayout 里四页各自绑定可见性"
expect_count_re "$QML_DIR/Main.qml" "^[[:space:]]*currentIndex: root.currentPage" 1 \
  "StackLayout 跟随 root.currentPage"
if grep -rq 'OperationPage' "$QML_DIR" "$RESOURCE_FILE"; then
  record_fail "仍然存在 standalone OperationPage（恢复应当是管理页里的动作）"
else
  record_pass "没有 standalone OperationPage"
fi
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
# 备份页从 PR #16 起走带算法选项的入口；startBackup() 在控制器内部就是
# 它的 mypack + none + none 等价形式，产品界面不再直接调用那个名字。
expect_count_re "$QML_DIR/pages/BackupPage.qml" 'controller\.startBackupWithOptions\(' 1 \
  "备份页调用 controller.startBackupWithOptions()"
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

# 筛选编辑器：刷新、添加 Include、添加 Exclude、清空、上移、下移、删除、
# 添加规则 = 8 处。仍然钉死数量，防止漏绑 busy 或复制粘贴出多余按钮。
expect_count "$QML_DIR/components/FilterEditorPanel.qml" "enabled: !controller.busy" 5 \
  "筛选编辑器忙碌时禁用输入与按钮"
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
# 错误正文要能选中复制，核心给的长路径才有可能贴出来。
expect_count_re "$QML_DIR/components/StatusBanner.qml" "selectByMouse:[[:space:]]*true" 1 \
  "状态栏正文可鼠标选中"
# 运行期契约：忙时拒绝关闭并提示，任务结束后放行。
set +e
QT_QPA_PLATFORM=offscreen QSG_RHI_BACKEND=software \
  ./build/backup-gui-modern --close-guard-test \
  --config-file "$TEST_CONFIG_FILE" > /tmp/modern-gui-guard.log 2>&1
guard_status=$?
set -e
sed 's/^/[modern-gui]     /' /tmp/modern-gui-guard.log
cat /tmp/modern-gui-guard.log >> "$LOG_FILE"
if [[ "$guard_status" -eq 0 ]]; then
  record_pass "忙时关窗被拒绝并给出提示，任务结束后可正常关闭"
else
  record_fail "关闭守卫行为与预期不符"
fi

echo "[modern-gui] 8) 文件筛选在 GUI 路径上生效"
# 界面只负责收集规则文本，解析与匹配都在 C++ Filter 里：
# 下面先做静态确认，再用 --self-test 走一遍真实控制器路径。
# 可视化编辑器的链路固定为：面板 -> FilterRuleModel -> BackupController -> 真实 Filter。
# 面板只跟 model 打交道，model 才调用控制器，所以断言按这个真实结构落在两处。
expect_count_re "$QML_DIR/components/FilterEditorPanel.qml" "resetForm\(\"include\"\)" 1 \
  "Modern GUI 提供 Include 添加入口"
expect_count_re "$QML_DIR/components/FilterEditorPanel.qml" "resetForm\(\"exclude\"\)" 1 \
  "Modern GUI 提供 Exclude 添加入口"
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
if grep -q -- '"uid", "gid", "user", "group"' "$QML_DIR/components/FilterEditorPanel.qml"; then
  record_pass "筛选编辑器字段下拉含 uid / gid / user / group"
else
  record_fail "筛选编辑器字段下拉缺 uid / gid / user / group"
fi
if grep -q -- '"symlink", "fifo", "char",' "$QML_DIR/components/FilterEditorPanel.qml"; then
  record_pass "type 下拉含 symlink / fifo / char / block / socket"
else
  record_fail "type 下拉缺新的 type 取值"
fi
if grep -q -- '"eq", "lt", "le", "gt", "ge", "range"' "$QML_DIR/components/FilterEditorPanel.qml"; then
  record_pass "uid / gid 比较运算符含 eq / lt / le / gt / ge / range"
else
  record_fail "uid / gid 比较运算符缺项"
fi
if grep -q -- '"uid_high": panel.formUidHighText' "$QML_DIR/components/FilterEditorPanel.qml"; then
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
# 预览必须继续复用真实 Filter，并且用 lstat（不跟随软链接）。
if grep -qF 'filter.ShouldIncludeFile(' "$ROOT_DIR/ui/modern/filter_rule_model.cpp" \
   && grep -qF 'filter.ShouldPruneDirectory(' "$ROOT_DIR/ui/modern/filter_rule_model.cpp" \
   && grep -qF 'filter.ShouldSkipSpecialEntry(' "$ROOT_DIR/ui/modern/filter_rule_model.cpp"; then
  record_pass "预览沿用真实 Filter 的三条判定（include / 剪枝 / 特殊文件）"
else
  record_fail "预览没有走真实 Filter 判定"
fi
if grep -qF '::lstat(' "$ROOT_DIR/ui/modern/filter_rule_model.cpp"; then
  record_pass "预览用 lstat 取元数据（不跟随软链接）"
else
  record_fail "预览没有用 lstat"
fi
# 预览要能把各类条目分开说清楚，并且点出 socket 的后果。
for tag in '符号链接' 'FIFO' '字符设备' '块设备' 'socket（不支持归档）'; do
  if grep -qF "QStringLiteral(\"${tag}\")" "$ROOT_DIR/ui/modern/filter_rule_model.cpp"; then
    record_pass "预览能标注 ${tag}"
  else
    record_fail "预览缺 ${tag} 标注"
  fi
done
if grep -qF '不支持的 socket（会导致备份失败）' "$ROOT_DIR/ui/modern/filter_rule_model.cpp" \
   && grep -qF '被规则排除' "$ROOT_DIR/ui/modern/filter_rule_model.cpp"; then
  record_pass "预览区分被规则排除与不支持的 socket"
else
  record_fail "预览缺排除 / socket 提示"
fi

# mtime 的 5 种形态：字段下拉、类型键、天数与两个日期都要真的接到模型上。
if grep -q -- '"uid", "gid", "user", "group", "mtime"' "$QML_DIR/components/FilterEditorPanel.qml"; then
  record_pass "筛选编辑器字段下拉含 mtime"
else
  record_fail "筛选编辑器字段下拉缺 mtime"
fi
if grep -q -- '"today", "yesterday", "last_days", "day",' "$QML_DIR/components/FilterEditorPanel.qml"; then
  record_pass "mtime 类型下拉含 today / yesterday / last_days / day / day_range"
else
  record_fail "mtime 类型下拉缺项"
fi
if grep -q -- '"mtime_kind": panel.formMtimeKind' "$QML_DIR/components/FilterEditorPanel.qml" \
   && grep -q -- '"days_back": panel.formDaysBackText' "$QML_DIR/components/FilterEditorPanel.qml" \
   && grep -q -- '"date_low": panel.formDateLow' "$QML_DIR/components/FilterEditorPanel.qml" \
   && grep -q -- '"date_high": panel.formDateHigh' "$QML_DIR/components/FilterEditorPanel.qml"; then
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
# 收起时的一行摘要：三段算法名用「 · 」连接，正好两个分隔符。
expect_count "$QML_DIR/components/BackupOptionsPanel.qml" '" · "' 2 \
  "摘要用「 · 」连接三段算法名"
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
  'controller\.startBackupWithOptions\(' 1 \
  "备份页调用 controller.startBackupWithOptions()"
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
    | grep -qE 'const started = controller\.startBackupWithOptions\([^)]*\) if \(started\) panel\.clearPasswords\(\)'; then
  record_pass "备份页检查 startBackupWithOptions() 的返回值，且只有成功才 panel.clearPasswords()"
else
  record_fail "备份页没有把 startBackupWithOptions() 的返回值与 clearPasswords() 关联（同步校验失败时会误清密码）"
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

# 一轮截图 = 八张固定状态（四页 × 两主题）+ 高级选项展开两张（两主题）
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
  expected="$expected management-light management-dark"
  expected="$expected settings-light settings-dark"
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
    | grep -qE 'onClicked: \{ panel\.requestPasswordValidation\(\) if \(!panel\.passwordAcceptable\) return const started = controller\.startBackupWithOptions\([^)]*\) if \(started\) panel\.clearPasswords\(\) \}'; then
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

echo "[modern-gui] 通过 $PASS_COUNT 项，失败 $FAIL_COUNT 项"
echo "[modern-gui] 日志: $LOG_FILE"
if [[ "$FAIL_COUNT" -eq 0 ]]; then
  # 全部通过时才打 PASS 并以 0 退出。
echo "[modern-gui] PASS"
else
  echo "[modern-gui] FAIL"
  exit 1
fi

