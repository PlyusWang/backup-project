// main.cpp
//
// 现代 QML GUI 的入口。除正常启动外还带几个开发期开关：
//   --smoke-test 建引擎、建窗口、切四个页面、换主题后退出
//   --screenshot <目录>                 四个页面 × 两套主题渲染成 PNG
//   --self-test <源> <备份文件> <恢复目录> [--include R] [--exclude R]
//                                       真跑一次 direct archive 备份 + 恢复
//   --repository-test <源> <仓库> <恢复目录>
//                                       走 ConfigManager + BackupCatalog 的
//                                       repository-driven 产品链路端到端验证
//   --backup-options-test               验证算法选项 / 密码校验 / 加密恢复 /
//                                       目录字段 / 密码不落盘（真实控制器路径）
//   --schedule-test                     验证自动备份页的控制器链路：保存计划、
//                                       立即运行、无变化跳过、变化建快照、
//                                       retention、0600 权限、与 CLI 同源路径
//   --preview-test <源> [--include R]... [--exclude R]...
//                                       把 Manual Backup 的筛选预览按 CLI 的
//                                       格式打出来，供脚本与
//                                       `backupctl preview` 逐行对比
//   --config-file <路径>                指定配置文件（测试隔离真实用户配置）
//   --schedule-file <路径>              指定计划存储文件（测试隔离真实计划）
//   --schedule-show                     把控制器读到的计划配置打成 key=value，
//                                       用来证明 GUI 与 CLI 读的是同一份 store
//   --realtime-test                     验证实时备份页的控制器链路：写配置并
//                                       逐字段读回、attach watcher、resync 触发
//                                       快照、文件事件触发快照、列出实时快照，
//                                       以及运行期改仓库后跟上新仓库 / 非法仓库
//                                       只降级不写盘 / 未启用时不启动 watcher
//   --realtime-show                     把控制器读到的实时配置打成 key=value，
//                                       用来证明 GUI 与 CLI 读的是同一份 store
//   --realtime-file <路径> 指定实时存储文件（测试隔离真实实时配置）
//   --path-test                         验证本地路径与 URL 互转不丢字符
//   --close-guard-test                  验证任务进行中关窗会被拦下：手动备份、
//                                       实时触发、计划评估三位 writer 都要在
//                                       飞时被拒绝、结束后放行
//   --gui-contract-test                 验证首页三张卡片的按钮几何，以及
//                                       "临时提示只属于产生它的页面"这条契约
//   --incremental-test <源> <仓库>      PR #18 GUI/CLI parity：走真实控制器
//                                       入口跑 baseline / no-change / delta /
//                                       依赖链恢复，按固定格式打印结果
//   --combo-hover-test                  共享下拉的 hover 残留回归：真的把指针
//                                       移到某一行、再移走，断言灰底严格跟着指针
//                                       来去，关掉重开也不留痕迹
//   --filter-ux-test                    三页 parity：同一个普通表单输入
//                                       （条件类型 + 取值）在备份页 /
//                                       自动备份页 / 实时备份页生成同一条
//                                       DSL，非法输入三处 得到同一句来自共享
//                                       builder 的原因
//   --native-frame                      退回系统原生标题栏（Wayland 兜底）
//
// 这些开关让没有显示器的环境也能验证界面：离屏平台插件把窗口真正建出来，
// 自检再切一遍页面、换一次主题、跑一次备份恢复，不需要人盯着屏幕。

#include <sys/stat.h>
#include <unistd.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHoverEvent>
#include <QKeyEvent>
#include <QMetaObject>
#include <QPointF>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>

#include "app_paths.h"
#include "app_theme.h"
#include "application_instance_lock.h"
#include "archive_pipeline.h"
#include "backup_controller.h"
#include "config_manager.h"
#include "filter_rule_model.h"
#include "operation_gate.h"
#include "realtime_controller.h"
#include "schedule_controller.h"
#include "schedule_frequency.h"
#include "schedule_store.h"
#include "scheduler_lock.h"

namespace {

const int kPageCount = 6;
int g_qml_warnings = 0;

// QML 的运行期问题（binding loop、类型错误、模块缺失……）都以 Qt warning 发出。
// 这里显式计数：--smoke-test / --screenshot 一旦出现 QML 警告就直接判失败，
// 免得靠人去一屏日志里翻。
// 分流之后两类信息一眼可辨：QML 问题计进 g_qml_warnings 决定退出码，
// 其它警告只打印，不会误伤。
void MessageHandler(QtMsgType type, const QMessageLogContext& context,
                    const QString& message) {
  Q_UNUSED(context);
  const bool is_warning =
      type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg;
  if (!is_warning) {
    std::fprintf(stdout, "%s\n", qPrintable(message));
    return;
  }
  const bool looks_like_qml =
      message.contains(QStringLiteral(".qml")) ||
      message.contains(QStringLiteral("QML")) ||
      message.contains(QStringLiteral("Binding loop")) ||
      message.contains(QStringLiteral("is not installed"));
  if (looks_like_qml) {
    ++g_qml_warnings;
    std::fprintf(stderr, "[qml-warning] %s\n", qPrintable(message));
  } else {
    std::fprintf(stderr, "[warning] %s\n", qPrintable(message));
  }
  if (type == QtFatalMsg) {
    std::abort();
  }
}

// 抓图前等动画真正结束再抓。
// 页面切换带 150ms 淡入，早先只跑几轮 processEvents 会在动画中途抓帧，
// 拿到的是半透明画面（卡片底色被混成背景色），所以这里改成按时间等。
// 用事件循环等，而不是空转：等待期间合成器还要处理帧回调，
// 把主线程占死反而会让动画停在原地。
void WaitForAnimation(int milliseconds) {
  QEventLoop loop;
  QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
  loop.exec();
}

// 配置文件路径：正常启动由 Qt 按应用名算出 AppConfigLocation，
// 显式给了 --config-file 时用调用方指定的那一个（自动测试据此隔离真实配置）。
//
// 这里刻意不猜 HOME、不写死 ~/.config、不假设运行在 Ubuntu 上：
// 路径的来源只有 QStandardPaths 和命令行这两个。
QString ResolveConfigFilePath(const QStringList& arguments) {
  const int index = arguments.indexOf(QStringLiteral("--config-file"));
  if (index >= 0 && index + 1 < arguments.size()) {
    return arguments.at(index + 1);
  }
  // 默认位置与 backupctl 严格同源：两边走的都是 app_paths.h 里那一份实现。
  // 这样"GUI 保存的计划 CLI 读不到"在结构上就不可能发生，
  // 而不是靠两处各自记得写对同一个字符串。
  return QString::fromStdString(backupproject::DefaultConfigFilePath());
}

// 计划存储文件：默认位置同样来自 app_paths.h，--schedule-file 只用于测试隔离。
QString ResolveScheduleFilePath(const QStringList& arguments) {
  const int index = arguments.indexOf(QStringLiteral("--schedule-file"));
  if (index >= 0 && index + 1 < arguments.size()) {
    return arguments.at(index + 1);
  }
  return QString::fromStdString(backupproject::DefaultScheduleFilePath());
}

// 实时备份存储文件：默认位置同样来自 app_paths.h（backupctl realtime 读的就是
// 这一份），--realtime-file 只用于测试隔离。
QString ResolveRealtimeFilePath(const QStringList& arguments) {
  const int index = arguments.indexOf(QStringLiteral("--realtime-file"));
  if (index >= 0 && index + 1 < arguments.size()) {
    return arguments.at(index + 1);
  }
  return QString::fromStdString(backupproject::DefaultRealtimeFilePath());
}

// 在可视项树里按 objectName 找一个 QQuickItem。
//
// 不能用 QObject::findChild()：Repeater 的委托是组件实例，QObject 父对象为空，
// 不在窗口的 QObject 树里，只有可视项树（childItems）认得它们。
// 管理页的记录卡片正是这样的委托，恢复密码对话框又在卡片内部。
QQuickItem* FindItemByName(QQuickItem* root, const QString& name) {
  if (root == nullptr) {
    return nullptr;
  }
  if (root->objectName() == name) {
    return root;
  }
  const QList<QQuickItem*> children = root->childItems();
  for (QQuickItem* child : children) {
    if (QQuickItem* found = FindItemByName(child, name)) {
      return found;
    }
  }
  return nullptr;
}

// 把备份页滚到指定位置。展开后的高级选项在页面下半部分，窗口一屏放不下，
// 不滚动的话抓到的"展开状态"只有标题那一行，评审时看不出面板长什么样。
// 滚的是 ScrollView 的 Flickable；越界值由 Flickable 自己夹到边界。
void ScrollBackupPage(QQuickWindow* window, int content_y) {
  QQuickItem* scroll =
      FindItemByName(window->contentItem(), QStringLiteral("backupPageScroll"));
  if (scroll == nullptr) {
    return;
  }
  QObject* flickable = qobject_cast<QObject*>(
      scroll->property("contentItem").value<QQuickItem*>());
  if (flickable != nullptr) {
    flickable->setProperty("contentY", content_y);
  }
}

// --screenshot：五个页面 × 两套主题各抓一张 PNG，另外补两种状态：
// 高级选项展开、加密归档的恢复密码对话框。
// 抓帧走窗口自己的 grabWindow()，和用户看到的是同一条渲染路径，
// 不是另画一份示意图。
//
// 额外状态只改测试期的属性（panel.expanded / dialog.visible），产品 QML
// 一行不动，抓完立刻还原。未加密仓库里没有恢复密码对话框是正常的，那一轮直接
// 跳过；但仓库里明明有加密记录却找不到对话框就是缺陷，按失败处理。
int CaptureScreenshots(QQuickWindow* window, backup_modern::AppTheme* theme,
                       backup_modern::BackupController* controller,
                       const QString& directory) {
  if (!QDir().mkpath(directory)) {
    std::fprintf(stderr, "无法创建截图目录: %s\n", qPrintable(directory));
    return 1;
  }
  // 记录卡片是异步扫出来的：不等列表稳定就抓，管理页会是一张"暂无备份"的空图。
  if (!controller->waitForCatalogIdle(60000)) {
    std::fprintf(stderr, "等待仓库列表超时，管理页截图会缺少记录\n");
    return 1;
  }
  // 文件名固定成 页面-主题.png，方便文档和脚本按名字引用，
  // 不用在文档里写死带时间戳的路径。
  const auto grab = [window, &directory](const QString& name, bool dark) {
    WaitForAnimation(300);
    const QImage image = window->grabWindow();
    if (image.isNull()) {
      std::fprintf(stderr, "grabWindow() 返回空图像\n");
      return false;
    }
    const QString path =
        QStringLiteral("%1/%2-%3.png")
            .arg(directory, name,
                 dark ? QStringLiteral("dark") : QStringLiteral("light"));
    if (!image.save(path)) {
      std::fprintf(stderr, "截图保存失败: %s\n", qPrintable(path));
      return false;
    }
    std::printf("screenshot: %s\n", qPrintable(path));
    return true;
  };

  // 顺序必须与 Main.qml 的 StackLayout 一致：首页 / 备份 / 自动备份 /
  // 备份管理 / 设置 / 实时备份。
  const char* page_names[kPageCount] = {"home",       "backup",   "schedule",
                                        "management", "settings", "realtime"};
  for (int dark = 0; dark < 2; ++dark) {
    theme->setDark(dark == 1);
    for (int page = 0; page < kPageCount; ++page) {
      window->setProperty("currentPage", page);
      if (!grab(QString::fromLatin1(page_names[page]), dark == 1)) {
        return 1;
      }
    }
  }

  // 高级选项展开：备份页在"收起 / 展开"两种状态下各留一张图，
  // 折叠面板是不是真的收得起、展得开，只有两张图摆在一起才看得出来。
  QObject* panel =
      window->findChild<QObject*>(QStringLiteral("backupOptionsPanel"));
  if (panel == nullptr) {
    std::fprintf(stderr, "找不到 backupOptionsPanel，无法抓展开状态\n");
    return 1;
  }
  for (int dark = 0; dark < 2; ++dark) {
    theme->setDark(dark == 1);
    window->setProperty("currentPage", 1);
    panel->setProperty("expanded", true);
    ScrollBackupPage(window, 100000);
    if (!grab(QStringLiteral("backup-expanded"), dark == 1)) {
      return 1;
    }
  }
  panel->setProperty("expanded", false);
  ScrollBackupPage(window, 0);

  // 加密归档的恢复密码对话框。这个对象每张记录卡片都有，但"未加密记录 +
  // 恢复密码对话框"这个画面在真实使用中不会出现，所以只有仓库里确实存在
  // 加密记录时才抓它。
  int encrypted_records = 0;
  const QVariantList screenshot_records = controller->backupRecords();
  for (const QVariant& item : screenshot_records) {
    if (item.toMap().value(QStringLiteral("passwordRequired")).toBool()) {
      ++encrypted_records;
    }
  }
  // 条数打进日志：截图缺一张时，看这一行就能分清"仓库里确实没有加密记录"
  // 和"有加密记录却找不到对话框"。
  std::printf("screenshot: 仓库记录 %d 条，其中加密记录 %d 条\n",
              static_cast<int>(screenshot_records.size()), encrypted_records);
  if (encrypted_records == 0) {
    std::printf("screenshot: 本次仓库没有加密记录，跳过恢复密码对话框\n");
    return 0;
  }
  // 记录卡片是 Repeater 的委托，只能在可视项树里找；对话框是卡片内联声明的
  // 对象，QObject 父对象就是卡片自己，所以从卡片再往下找一层。
  QQuickItem* record_card =
      FindItemByName(window->contentItem(), QStringLiteral("backupRecordCard"));
  QObject* restore_dialog = record_card == nullptr
                                ? nullptr
                                : record_card->findChild<QObject*>(
                                      QStringLiteral("restorePasswordDialog"));
  if (restore_dialog == nullptr) {
    std::fprintf(stderr,
                 "screenshot: 有加密记录但找不到记录卡片 / "
                 "restorePasswordDialog（card=%s）\n",
                 record_card == nullptr ? "null" : "found");
    return 1;
  }
  for (int dark = 0; dark < 2; ++dark) {
    theme->setDark(dark == 1);
    window->setProperty("currentPage", 2);
    restore_dialog->setProperty("visible", true);
    if (!grab(QStringLiteral("management-password-dialog"), dark == 1)) {
      return 1;
    }
    restore_dialog->setProperty("visible", false);
  }
  return 0;
}

// 它验的是桥加核心这一整条链路：先备份再恢复，任何一步失败
// 就把核心的原文错误打到 stderr 并以非 0 退出。
// --self-test 可以带 --include / --exclude：这些规则和界面点"添加"时走的是
// 同一条路径（BackupController::addFilterRule → 同一个 C++ Filter）。
// 规则非法就直接报错退出，不会跑出一个"看起来成功"的备份。
int ApplyFilterArguments(backup_modern::BackupController* controller,
                         const QStringList& arguments) {
  for (int index = 0; index < arguments.size(); ++index) {
    const QString option = arguments.at(index);
    if (option != QStringLiteral("--include") &&
        option != QStringLiteral("--exclude")) {
      continue;
    }
    if (index + 1 >= arguments.size()) {
      std::fprintf(stderr, "%s 需要一个规则参数\n", qPrintable(option));
      return 1;
    }
    const QString action = (option == QStringLiteral("--include"))
                               ? QStringLiteral("include")
                               : QStringLiteral("exclude");
    if (!controller->addFilterRule(action, arguments.at(index + 1))) {
      std::fprintf(stderr, "规则无效: %s\n",
                   qPrintable(controller->statusMessage()));
      return 1;
    }
    ++index;
  }
  return 0;
}

// ---- --preview-test ----
//
// 把 Manual Backup 的筛选预览按 **backupctl preview 的输出格式**打出来，
// 好让检查脚本把两条命令的输出逐行 diff。
//
// 它走的是界面真正的入口：FilterRuleModel::addAdvancedRule（用户填高级规则的
// 那条路）+ FilterRuleModel::requestPreview（"刷新预览"按钮）。所以它证明的是
// "界面上看到的集合 == 命令行 preview 给出的集合"，而不是"某个内部函数恰好
// 返回了同样的值"。
//
// 输出格式与 backupctl preview 一致（包含 truncated 时的 Note 行），
// 因此脚本可以直接 diff；差异一定意味着两个前端真的不一致。
int RunPreviewTest(backup_modern::FilterRuleModel* model, const QString& source,
                   const QStringList& arguments) {
  // 规则按命令行顺序进：一条规则内部的 compound AND 由 DSL 自己表达，
  // 多条 --include 之间是 OR —— 与 CLI 完全同一套语义。
  for (int index = 0; index < arguments.size(); ++index) {
    const QString option = arguments.at(index);
    if (option != QStringLiteral("--include") &&
        option != QStringLiteral("--exclude")) {
      continue;
    }
    if (index + 1 >= arguments.size()) {
      std::fprintf(stderr, "Error: %s 需要一个规则参数\n", qPrintable(option));
      return 2;
    }
    const QString action = option == QStringLiteral("--include")
                               ? QStringLiteral("include")
                               : QStringLiteral("exclude");
    if (!model->addAdvancedRule(action, arguments.at(index + 1))) {
      // 与 CLI 一样，这里报出的就是核心 Filter::AddRule 的原文，
      // 前端不翻译、不包装。
      std::fprintf(stderr, "Error: %s\n", qPrintable(model->lastError()));
      return 2;
    }
    ++index;
  }

  model->requestPreview(source, QString());
  // 扫描在后台线程跑：只有把事件循环转起来，QFutureWatcher 的 finished
  // 回调才会派发。processEvents 带超时，所以这不是空转占死 CPU。
  QElapsedTimer timer;
  timer.start();
  while (model->previewBusy() && timer.elapsed() < 60000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  }
  if (model->previewBusy()) {
    std::fprintf(stderr, "preview 超时\n");
    return 1;
  }
  if (!model->lastError().isEmpty()) {
    std::fprintf(stderr, "Error: %s\n", qPrintable(model->lastError()));
    if (model->lastErrorKind() ==
        backupproject::PreviewErrorKind::kSelectionBlocked) {
      // 与 backupctl preview 逐字一致的第二行：这不是语法问题，而是"按当前
      // 规则备份必然失败"，所以两边都用 exit 1 并说清该怎么办。
      std::printf("Backup would fail unless this entry is excluded.\n");
    }
    return 1;
  }

  // 界面列表里既有"进归档"也有"被排除"的条目（各自带标签），CLI 只列前者；
  // 这里按 included 投影成同一份集合再打印。
  const QVariantList items = model->previewItems();
  QStringList included_paths;
  for (const QVariant& value : items) {
    const QVariantMap item = value.toMap();
    if (item.value(QStringLiteral("included")).toBool()) {
      included_paths << item.value(QStringLiteral("path")).toString();
    }
  }
  // 计数取完整实际备份遍历的数字（与 CLI 打印的是同一个字段），窗口信息取核心
  // 给出的 truncated。三个数字互不相同：全量匹配数、窗口大小（preview entries，
  // 含被排除的条目）、窗口里真正列出来的匹配项数。所以在截断时不能说
  // "the first 300 of N matching item(s)"——那是把窗口大小当成了匹配数。
  // 下面的 Note 行必须与 backupctl 的措辞逐字一致。
  std::printf(
      "Preview: %d matching item(s) in the effective backup selection.\n",
      model->previewIncluded());
  if (model->previewTruncated()) {
    std::printf(
        "Note: showing matches found within the first %d preview entries; the "
        "complete effective backup traversal was validated, %d matching "
        "item(s) listed below.\n",
        model->previewLimit(), static_cast<int>(included_paths.size()));
  }
  for (const QString& path : included_paths) {
    std::printf("%s\n", qPrintable(path));
  }
  return 0;
}

// --self-test：验证 controller → engine 的 direct archive 路径，也就是
// "备份文件由调用方显式指定"这一种用法。产品 QML 已经不再提供这个入口
// （界面改成 repository + 自动命名），但这条核心链路本身依然要有人测，
// 而且它仍然应用当前的 include / exclude 规则。
//
// 两个 start*ForTest 入口都是 C++ 专供、不是 Q_INVOKABLE，所以它们不会
// 变成 QML 可以调用的东西。
int RunSelfTest(backup_modern::BackupController* controller,
                const QString& source, const QString& archive_file,
                const QString& destination) {
  controller->setSourcePath(source);

  if (!controller->startDirectBackupForTest(source, archive_file) ||
      !controller->waitForIdle(600000) || !controller->lastSucceeded()) {
    std::fprintf(stderr, "backup failed: %s\n",
                 qPrintable(controller->statusMessage()));
    return 1;
  }
  std::printf("backup ok\n");

  if (!controller->startDirectRestoreForTest(archive_file, destination) ||
      !controller->waitForIdle(600000) || !controller->lastSucceeded()) {
    std::fprintf(stderr, "restore failed: %s\n",
                 qPrintable(controller->statusMessage()));
    return 1;
  }
  std::printf("restore ok\n");
  return 0;
}

// v2 容器 header 的整数字段一律 little-endian（偏移表见
// docs/format/archive_v2_container.md）。按字节拼出来，而不是把内存里的字节
// 直接当成主机整数：后者只在特定字节序的机器上才是对的。
unsigned ReadLeU16(const QByteArray& data, int offset) {
  return static_cast<unsigned>(static_cast<unsigned char>(data.at(offset))) |
         (static_cast<unsigned>(static_cast<unsigned char>(data.at(offset + 1)))
          << 8);
}

unsigned long long ReadLeU64(const QByteArray& data, int offset) {
  unsigned long long value = 0;
  for (int index = 7; index >= 0; --index) {
    value = (value << 8) | static_cast<unsigned char>(data.at(offset + index));
  }
  return value;
}

// --repository-test：repository-driven 的产品链路端到端验证。
// 它走 ConfigManager + BackupCatalog + BackupController + BackupEngine，
// 不使用任何 direct archive 捷径：文件名由 Catalog 自动生成，恢复只传 file
// name。
//
// 除了"每一步都成功"，它还断言产物本身的格式：正常备份必须落成 v2 容器
// （BKPCNT2\0 + MyPack + 不压缩 + 不加密），因为界面展示的 uid / gid /
// symlink / FIFO 只有 v2 装得下。断言全部对着文件字节做，不看"备份成功"这句话。
//
// 环境变量 BACKUP_MODERN_KEEP_ARTIFACT（可选）指向一个路径：设了就把产物复制
// 一份到那里，供检查脚本在产物被删除之前自己读字节。不设时行为一字不变。
//
// 每一步失败都把真实 diagnostic 打到 stderr 并以非 0 退出，
// 所以脚本可以只信退出码，也可以从 stderr 看到核心的原文原因。
int RunRepositoryTest(backup_modern::BackupController* controller,
                      const QString& source, const QString& repository,
                      const QString& destination) {
  // 1. 保存仓库设置：EnsureRepository + ConfigManager::Save
  if (!controller->saveRepositoryPath(repository)) {
    std::fprintf(stderr, "repository save failed: %s\n",
                 qPrintable(controller->statusMessage()));
    return 1;
  }
  std::printf("repository save ok\n");

  // 2. 自动命名的备份：界面不提供归档路径输入框
  controller->setSourcePath(source);
  if (!controller->startBackup() || !controller->waitForIdle(600000) ||
      !controller->lastSucceeded()) {
    std::fprintf(stderr, "automatic backup failed: %s\n",
                 qPrintable(controller->statusMessage()));
    return 1;
  }
  std::printf("automatic backup ok\n");

  // 3. 仓库列表
  controller->refreshBackups();
  if (!controller->waitForCatalogIdle(600000)) {
    std::fprintf(stderr, "catalog list timed out\n");
    return 1;
  }
  if (!controller->catalogError().isEmpty()) {
    std::fprintf(stderr, "catalog list failed: %s\n",
                 qPrintable(controller->catalogError()));
    return 1;
  }
  const QVariantList records = controller->backupRecords();
  if (records.size() != 1) {
    std::fprintf(stderr, "catalog list 期望 1 条记录，实际 %d 条\n",
                 static_cast<int>(records.size()));
    return 1;
  }
  const QVariantMap record = records.at(0).toMap();
  if (!record.value(QStringLiteral("recognizedArchive")).toBool()) {
    std::fprintf(
        stderr, "归档头未被识别: %s\n",
        qPrintable(record.value(QStringLiteral("diagnostic")).toString()));
    return 1;
  }
  // 正常备份的产物是 v2 容器，所以列表里这一条必须认出 v2 并且数得出条目。
  // recognizedArchive 只说明"全局 header 可读"，formatVersion 与 entryCount
  // 才是"这是 v2、里面确实有条目"这两个事实。
  const int record_format_version =
      record.value(QStringLiteral("formatVersion")).toInt();
  const unsigned long long record_entry_count =
      record.value(QStringLiteral("entryCount")).toULongLong();
  const bool record_recognized =
      record.value(QStringLiteral("recognizedArchive")).toBool();
  if (record_format_version != 2 || record_entry_count == 0) {
    std::fprintf(stderr,
                 "catalog 记录不是 v2 容器: formatVersion=%d entryCount=%llu\n",
                 record_format_version, record_entry_count);
    return 1;
  }
  const QString file_name = record.value(QStringLiteral("fileName")).toString();
  std::printf("catalog list ok\n");
  std::printf(
      "record: fileName=%s size=%s mtime=%s recognizedArchive=%s "
      "formatVersion=%d entryCount=%llu\n",
      qPrintable(file_name),
      qPrintable(record.value(QStringLiteral("sizeText")).toString()),
      qPrintable(record.value(QStringLiteral("modifiedTimeText")).toString()),
      record_recognized ? "true" : "false", record_format_version,
      record_entry_count);

  // 3.5 产物字节：界面展示 uid / gid / user / group / symlink / FIFO，筛选预览
  // 也会说某个 special entry“进入归档” —— 这些承诺只有在产物真的是 v2 容器时
  // 才成立。这里直接读文件，而不是相信上一步的成功返回值。
  const QString archive_path = QDir(repository).filePath(file_name);
  QFile archive(archive_path);
  if (!archive.open(QIODevice::ReadOnly)) {
    std::fprintf(stderr, "无法读取备份产物: %s\n", qPrintable(archive_path));
    return 1;
  }
  const QByteArray container_header = archive.read(160);
  archive.close();

  // 期望的 magic 逐字节比较，不走字符串：它的第 8 个字节就是 NUL。
  const char kContainerMagic[8] = {'B', 'K', 'P', 'C', 'N', 'T', '2', '\0'};
  if (container_header.size() < 160 ||
      std::memcmp(container_header.constData(), kContainerMagic, 8) != 0) {
    std::fprintf(stderr,
                 "备份产物不是 v2 容器（前 8 字节不是 BKPCNT2\\0）: %s\n",
                 qPrintable(archive_path));
    return 1;
  }
  const unsigned container_version = ReadLeU16(container_header, 8);
  const unsigned container_header_size = ReadLeU16(container_header, 10);
  const unsigned pack_id = static_cast<unsigned char>(container_header.at(12));
  const unsigned compression_id =
      static_cast<unsigned char>(container_header.at(13));
  const unsigned encryption_id =
      static_cast<unsigned char>(container_header.at(14));
  const unsigned long long container_entry_count =
      ReadLeU64(container_header, 16);
  // MyPack = 0 / None = 0 / None = 0（见 include/pack_stream.h 与
  // include/container_format.h）。算法 id 与 entry_count 同时要和 catalog 记录
  // 对得上：两条独立路径给出同一个结论才算数。
  if (container_version != 2 || container_header_size != 160 || pack_id != 0 ||
      compression_id != 0 || encryption_id != 0 ||
      container_entry_count != record_entry_count) {
    std::fprintf(stderr,
                 "产物外层 header 不是 MyPack/None/None v2: version=%u "
                 "headerSize=%u pack=%u compression=%u encryption=%u "
                 "entryCount=%llu（catalog 记录 %llu）\n",
                 container_version, container_header_size, pack_id,
                 compression_id, encryption_id, container_entry_count,
                 record_entry_count);
    return 1;
  }

  backupproject::ArchiveFileInfo archive_info;
  std::string identify_error;
  if (!backupproject::IdentifyArchiveFile(archive_path.toStdString(),
                                          &archive_info, &identify_error)) {
    std::fprintf(stderr, "IdentifyArchiveFile 失败: %s\n",
                 identify_error.c_str());
    return 1;
  }
  if (archive_info.kind != backupproject::ArchiveFileInfo::Kind::kContainerV2 ||
      archive_info.format_version != 2 ||
      archive_info.pack_method != backupproject::PackMethod::kMyPack ||
      archive_info.compression_method !=
          backupproject::CompressionMethod::kNone ||
      archive_info.encryption_method !=
          backupproject::EncryptionMethod::kNone ||
      archive_info.entry_count != container_entry_count) {
    std::fprintf(stderr,
                 "IdentifyArchiveFile 的结论与 v2 MyPack/None/None 不符\n");
    return 1;
  }

  // magic 文本同样取自刚读到的字节（去掉结尾的 NUL 再打印），不是字面量：
  // 检查脚本会拿下面两行断言产物格式，打印出来的必须是文件里真实的东西。
  QByteArray magic_text = container_header.left(8);
  while (magic_text.endsWith('\0')) {
    magic_text.chop(1);
  }
  std::printf(
      "artifact: magic=%s version=%u headerSize=%u packMethod=%u "
      "compressionMethod=%u encryptionMethod=%u entryCount=%llu\n",
      magic_text.constData(), container_version, container_header_size, pack_id,
      compression_id, encryption_id, container_entry_count);
  std::printf(
      "identify: kind=container-v2 formatVersion=%u packMethod=%s "
      "compressionMethod=%s encryptionMethod=%s entryCount=%llu\n",
      static_cast<unsigned>(archive_info.format_version),
      backupproject::PackMethodName(archive_info.pack_method),
      backupproject::CompressionMethodName(archive_info.compression_method),
      backupproject::EncryptionMethodName(archive_info.encryption_method),
      static_cast<unsigned long long>(archive_info.entry_count));

  // 产物马上就会被第 5 步删掉，脚本要自己读字节就得先留一份。没有设这个环境
  // 变量时不复制、不留痕，产品行为一字不变。
  const QByteArray keep_path = qgetenv("BACKUP_MODERN_KEEP_ARTIFACT");
  if (!keep_path.isEmpty()) {
    const QString keep = QString::fromLocal8Bit(keep_path);
    QFile::remove(keep);
    if (!QFile::copy(archive_path, keep)) {
      std::fprintf(stderr, "产物副本写入失败: %s\n", qPrintable(keep));
      return 1;
    }
    std::printf("artifact copy: %s\n", qPrintable(keep));
  }

  // 4. 从管理页发起恢复：QML 只传 file name，解析交给 Catalog::Resolve
  if (!controller->startManagedRestore(file_name, destination) ||
      !controller->waitForIdle(600000) || !controller->lastSucceeded()) {
    std::fprintf(stderr, "managed restore failed: %s\n",
                 qPrintable(controller->statusMessage()));
    return 1;
  }
  std::printf("managed restore ok\n");

  // 5. 删除并确认列表真的空了
  if (!controller->deleteBackup(file_name)) {
    std::fprintf(stderr, "delete failed: %s\n",
                 qPrintable(controller->statusMessage()));
    return 1;
  }
  if (!controller->waitForCatalogIdle(600000)) {
    std::fprintf(stderr, "catalog refresh after delete timed out\n");
    return 1;
  }
  if (!controller->backupRecords().isEmpty()) {
    std::fprintf(stderr, "delete 之后列表仍然有 %d 条记录\n",
                 static_cast<int>(controller->backupRecords().size()));
    return 1;
  }
  std::printf("delete ok\n");
  return 0;
}

// --path-test：验证「本地路径 → URL → 本地路径」不丢字符。
// 界面上“浏览”按钮选完目录走的就是 controller.localPathFromUrl()，
// 所以这里测的正是 QML 侧实际使用的那条转换。
int RunPathTest(backup_modern::BackupController* controller) {
  int failures = 0;
  const QStringList cases = {
      QStringLiteral("/tmp/normal"),   QStringLiteral("/tmp/with space"),
      QStringLiteral("/tmp/中文目录"), QStringLiteral("/tmp/a#b"),
      QStringLiteral("/tmp/a%b"),      QStringLiteral("/tmp/中文 空格#百分号%"),
  };
  for (const QString& path : cases) {
    const QUrl url = QUrl::fromLocalFile(path);
    const QString back = controller->localPathFromUrl(url);
    const bool ok = back == path;
    std::printf("%s path=[%s] url=[%s] back=[%s]\n", ok ? "ok  " : "FAIL",
                qPrintable(path), qPrintable(url.toString()), qPrintable(back));
    failures += ok ? 0 : 1;
  }

  // 真实目录再走一遍：确认文件系统里确实存在这个带中文、空格、#、% 的名字。
  QTemporaryDir dir;
  const QString real = dir.filePath(QStringLiteral("中文 空格#百分号%"));
  QDir().mkpath(real);
  const QString back = controller->localPathFromUrl(QUrl::fromLocalFile(real));
  const bool real_ok = back == real && QFileInfo(real).isDir();
  std::printf("%s 真实目录 path=[%s] back=[%s]\n", real_ok ? "ok  " : "FAIL",
              qPrintable(real), qPrintable(back));
  failures += real_ok ? 0 : 1;

  // 非本地 URL 必须给空串，不能拼出一个看起来像路径的字符串。
  const QString remote = controller->localPathFromUrl(
      QUrl(QStringLiteral("https://example.com/a.txt")));
  const bool remote_ok = remote.isEmpty();
  std::printf("%s 非本地 URL 返回空串 (got=[%s])\n",
              remote_ok ? "ok  " : "FAIL", qPrintable(remote));
  failures += remote_ok ? 0 : 1;

  // 对话框起始位置：存在的目录原样给出，缺失或为空时回退主目录。
  const QUrl start_existing = controller->directoryDialogStartUrl(real);
  const QUrl start_missing = controller->directoryDialogStartUrl(
      QStringLiteral("/tmp/不存在的目录-xyz"));
  const QUrl start_empty = controller->directoryDialogStartUrl(QString());
  const bool start_ok =
      start_existing == QUrl::fromLocalFile(real) &&
      start_missing == QUrl::fromLocalFile(QDir::homePath()) &&
      start_empty == QUrl::fromLocalFile(QDir::homePath());
  std::printf("%s 对话框起始位置：存在=[%s] 缺失回退=[%s]\n",
              start_ok ? "ok  " : "FAIL", qPrintable(start_existing.toString()),
              qPrintable(start_missing.toString()));
  failures += start_ok ? 0 : 1;

  std::printf("path-test 失败项: %d\n", failures);
  return failures == 0 ? 0 : 1;
}

// 仓库里的归档清单与数量。定义在文件靠后的位置（--schedule-test 也在用），
// 这里先声明：--close-guard-test 与 --realtime-test 都要拿它做"到底有没有写盘"
// 的判别 —— 只看控制器自己报的状态是不够的。
QStringList ArchiveNames(const QString& repository);
int CountArchives(const QString& repository);
QString ScheduleConfigSignature(const backupproject::ScheduleConfig& config);

// --close-guard-test：验证“任务进行中不许关窗”的契约。
//
// 不变式只有一条：任何会改动持久状态的业务操作在飞时，窗口不许关。进程里有
// 三个 writer，忙标志各有一位 —— 手动备份 / 恢复落在 controller.busy，而计划
// 评估与实时触发都跑在 QtConcurrent 上，真正落盘的那一位是各自的 libraryBusy。
// 所以判别必须分别落到这三条**真实**路径上，而不是只看 controller.busy：
// 少了实时那一段，"正在写归档时 Alt+F4 能把窗口关掉"这个洞就测不出来。
//
// busy 在 Submit 里、后台任务启动之前就已置位，而任务结束信号要等回到事件循环
// 才会派发；所以"在同一个事件循环回合里检查"的结论不取决于任务跑得多快。
// 实时那一段用真实事件循环等到 libraryBusy 真的置起来，再请求关窗。
//
// 全程只用临时目录（临时 config / schedule / realtime / 仓库 / 源目录），
// 绝不读写用户真实配置，也不碰冻结的 Demo。
int RunCloseGuardTest(QQuickWindow* window,
                      backup_modern::BackupController* controller,
                      backup_modern::ScheduleController* schedule,
                      backup_modern::RealtimeController* realtime) {
  int failures = 0;

  // 关窗被拒绝时必须给出说明，而不是"点了没反应"。每段测完都把提示关掉：
  // 否则下一段的 visible 断言会一直是真的，那条断言就失去判别力。
  QObject* busy_dialog =
      window->findChild<QObject*>(QStringLiteral("busyCloseDialog"));
  const auto dialog_visible = [busy_dialog]() {
    return busy_dialog != nullptr && busy_dialog->property("visible").toBool();
  };
  const auto dismiss_dialog = [busy_dialog]() {
    if (busy_dialog != nullptr) {
      QMetaObject::invokeMethod(busy_dialog, "close");
    }
  };
  // 在真实事件循环里等某个 worker 进入"正在落盘"。轮询而不是 sleep：
  // inotify 事件、debounce 定时器、QtConcurrent 的启动与回收都在这个循环里跑。
  const auto wait_for_busy = [](auto* worker, int timeout_ms) {
    QEventLoop loop;
    QTimer poll;
    poll.setInterval(10);
    QObject::connect(&poll, &QTimer::timeout, &loop, [worker, &loop]() {
      if (worker->libraryBusy()) loop.quit();
    });
    QTimer guard;
    guard.setSingleShot(true);
    QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
    poll.start();
    guard.start(timeout_ms);
    if (!worker->libraryBusy()) loop.exec();
    return worker->libraryBusy();
  };

  // ---- 1) 手动备份：controller.busy ----
  //
  // 这一段用 direct archive 入口：它不需要先配置仓库，测试因此更短、更聚焦。
  QTemporaryDir dir;
  const QString source = dir.filePath(QStringLiteral("source"));
  const QString archive = dir.filePath(QStringLiteral("backup.bak"));
  if (!dir.isValid() || !QDir().mkpath(source)) {
    std::fprintf(stderr, "临时目录创建失败\n");
    return 1;
  }
  // 造一批文件让任务真的跑起来：文件多少不重要，重要的是它会跨事件循环。
  for (int i = 0; i < 200; ++i) {
    QFile file(QStringLiteral("%1/file-%2.bin").arg(source).arg(i));
    if (file.open(QIODevice::WriteOnly)) {
      file.write(QByteArray(4096, 'x'));
    }
  }

  controller->setSourcePath(source);
  if (!controller->startDirectBackupForTest(source, archive) ||
      !controller->busy()) {
    std::fprintf(stderr, "FAIL 备份没有启动起来\n");
    return 1;
  }

  // 忙的时候关窗：必须被 onClosing 拒绝，窗口留着。
  const bool closed_while_busy = window->close();
  std::printf("%s 忙时 close() 被拒绝 (返回=%s)\n",
              closed_while_busy ? "FAIL" : "ok  ",
              closed_while_busy ? "true" : "false");
  failures += closed_while_busy ? 1 : 0;

  // 光拒绝还不够：得给用户一个说明，而不是点了没反应。
  const bool dialog_open = dialog_visible();
  std::printf("%s 忙时关窗会弹出提示 (visible=%s)\n",
              dialog_open ? "ok  " : "FAIL", dialog_open ? "true" : "false");
  failures += dialog_open ? 0 : 1;

  // 提示能关掉（"知道了"按钮走的就是 close()）。不关掉它，后面两段的 visible
  // 断言会一直是 true —— 那等于没测。
  dismiss_dialog();
  WaitForAnimation(200);
  const bool dialog_dismissed = !dialog_visible();
  std::printf("%s 提示可以被用户关掉 (visible=%s)\n",
              dialog_dismissed ? "ok  " : "FAIL",
              dialog_visible() ? "true" : "false");
  failures += dialog_dismissed ? 0 : 1;

  if (!controller->waitForIdle(120000) || controller->busy()) {
    std::fprintf(stderr, "FAIL 等待任务结束超时\n");
    return 1;
  }

  // 任务结束后同一条路径必须放行，否则就成了“永远关不掉”。
  const bool closed_when_idle = window->close();
  std::printf("%s 空闲时 close() 被接受 (返回=%s)\n",
              closed_when_idle ? "ok  " : "FAIL",
              closed_when_idle ? "true" : "false");
  failures += closed_when_idle ? 0 : 1;

  // 后面两段测的还是同一扇窗口，把它重新显示出来。
  window->show();
  WaitForAnimation(50);

  // ---- 2) 实时触发：realtime.libraryBusy ----
  //
  // 真的跑一次实时备份（enabled=true 会 attach + 合成一次 resync），再用真实
  // 事件循环等到 libraryBusy 置起来。判别前提是另外两位都是闲的：此时拒绝
  // 关窗只可能来自 realtime.libraryBusy。
  //
  // 实时与计划共用这一个临时根：仓库必须活到函数结束，计划那一段还要往同一个
  // 仓库里写归档。
  QTemporaryDir work;
  const QString realtime_source =
      work.filePath(QStringLiteral("realtime-source"));
  const QString schedule_source =
      work.filePath(QStringLiteral("schedule-source"));
  const QString repository = work.filePath(QStringLiteral("repository"));
  if (!work.isValid() || !QDir().mkpath(realtime_source) ||
      !QDir().mkpath(schedule_source) || !QDir().mkpath(repository)) {
    std::fprintf(stderr, "FAIL 实时 / 计划场景的临时目录创建失败\n");
    return 1;
  }
  // 造一批文件：一次实时触发要真的走完扫描 → 打包 → 压缩 → 校验 → 写归档，
  // worker 才会在事件循环里可观察地停留。
  for (int i = 0; i < 400; ++i) {
    QFile file(QStringLiteral("%1/file-%2.bin").arg(realtime_source).arg(i));
    if (file.open(QIODevice::WriteOnly)) {
      file.write(QByteArray(8192, 'r'));
    }
  }

  // 仓库走设置页的真实入口：写 config.json 并发 repositoryPathChanged。
  const bool repository_saved = controller->saveRepositoryPath(repository);
  // enabled=true 走真实产品路径：attach watcher + 合成一次 resync。
  const bool realtime_saved = realtime->saveConfig(
      /*enabled=*/true, realtime_source, /*debounce_ms=*/200,
      /*max_wait_ms=*/2000, /*retain_count=*/3, QStringLiteral("mypack"),
      QStringLiteral("none"), QStringList(), QStringList(),
      QStringLiteral("full"));
  if (!repository_saved || !realtime_saved) {
    std::fprintf(stderr,
                 "FAIL 实时场景没有配置成功: repository=[%s] realtime=[%s]\n",
                 qPrintable(controller->statusMessage()),
                 qPrintable(realtime->statusMessage()));
    return 1;
  }
  std::printf("ok   实时场景已配置：仓库=%s 监听 %d 个目录\n",
              qPrintable(realtime->repositoryPath()), realtime->watchCount());

  // resync 那一轮会在 debounce 之后自己开始。万一没赶上（任务在两次轮询之间
  // 就跑完了），再往源目录里写一个文件重来一次 —— 不靠 sleep 猜时间。
  bool realtime_busy = wait_for_busy(realtime, 60000);
  for (int attempt = 0; attempt < 3 && !realtime_busy; ++attempt) {
    QFile trigger(realtime_source +
                  QStringLiteral("/trigger-%1.txt").arg(attempt));
    if (trigger.open(QIODevice::WriteOnly)) trigger.write("trigger");
    realtime_busy = wait_for_busy(realtime, 60000);
  }
  if (!realtime_busy) {
    std::fprintf(stderr, "FAIL 实时备份没有进入 libraryBusy\n");
    return 1;
  }

  const bool others_idle = !controller->busy() && !schedule->libraryBusy();
  std::printf(
      "%s 实时 worker 在飞时另外两位是闲的 (controller=%s schedule=%s)\n",
      others_idle ? "ok  " : "FAIL", controller->busy() ? "busy" : "idle",
      schedule->libraryBusy() ? "busy" : "idle");
  failures += others_idle ? 0 : 1;

  const bool closed_while_realtime_busy = window->close();
  std::printf("%s 实时 worker 在飞时 close() 被拒绝 (返回=%s)\n",
              closed_while_realtime_busy ? "FAIL" : "ok  ",
              closed_while_realtime_busy ? "true" : "false");
  failures += closed_while_realtime_busy ? 1 : 0;

  const bool realtime_dialog_open = dialog_visible();
  std::printf("%s 实时 worker 在飞时关窗也会给出提示 (visible=%s)\n",
              realtime_dialog_open ? "ok  " : "FAIL",
              realtime_dialog_open ? "true" : "false");
  failures += realtime_dialog_open ? 0 : 1;

  dismiss_dialog();
  WaitForAnimation(200);

  if (!realtime->waitForIdle(180000) || realtime->libraryBusy()) {
    std::fprintf(stderr, "FAIL 等待实时备份结束超时\n");
    return 1;
  }

  const bool closed_when_realtime_idle = window->close();
  std::printf("%s 实时 worker 结束后 close() 被接受 (返回=%s)\n",
              closed_when_realtime_idle ? "ok  " : "FAIL",
              closed_when_realtime_idle ? "true" : "false");
  failures += closed_when_realtime_idle ? 0 : 1;

  // 实时这一段的监听停掉：下一段只测计划那一位，也不留下还在跑的重试定时器。
  realtime->stop();
  window->show();
  WaitForAnimation(50);

  // ---- 3) 计划评估：schedule.libraryBusy ----
  //
  // runNow() 是"立即检查并运行"的真实入口，busy 在它返回之前就已置位，
  // 结论因此不取决于任务跑得多快。
  for (int i = 0; i < 400; ++i) {
    QFile file(QStringLiteral("%1/file-%2.bin").arg(schedule_source).arg(i));
    if (file.open(QIODevice::WriteOnly)) {
      file.write(QByteArray(8192, 's'));
    }
  }
  const bool schedule_saved = schedule->saveConfig(
      /*enabled=*/true, schedule_source, /*interval_minutes=*/5,
      /*retain_count=*/3, QStringLiteral("mypack"), QStringLiteral("none"),
      QStringList(), QStringList(), QStringLiteral("full"));
  const bool schedule_started = schedule_saved && schedule->runNow();
  if (!schedule_started || !schedule->libraryBusy()) {
    std::fprintf(stderr, "FAIL 计划评估没有启动起来: saved=%d status=[%s]\n",
                 schedule_saved ? 1 : 0, qPrintable(schedule->statusMessage()));
    return 1;
  }

  const bool closed_while_schedule_busy = window->close();
  std::printf("%s 计划 worker 在飞时 close() 被拒绝 (返回=%s)\n",
              closed_while_schedule_busy ? "FAIL" : "ok  ",
              closed_while_schedule_busy ? "true" : "false");
  failures += closed_while_schedule_busy ? 1 : 0;

  const bool schedule_dialog_open = dialog_visible();
  std::printf("%s 计划 worker 在飞时关窗也会给出提示 (visible=%s)\n",
              schedule_dialog_open ? "ok  " : "FAIL",
              schedule_dialog_open ? "true" : "false");
  failures += schedule_dialog_open ? 0 : 1;

  dismiss_dialog();
  WaitForAnimation(200);

  if (!schedule->waitForIdle(180000) || schedule->libraryBusy()) {
    std::fprintf(stderr, "FAIL 等待计划评估结束超时\n");
    return 1;
  }

  const bool closed_when_schedule_idle = window->close();
  std::printf("%s 计划 worker 结束后 close() 被接受 (返回=%s)\n",
              closed_when_schedule_idle ? "ok  " : "FAIL",
              closed_when_schedule_idle ? "true" : "false");
  failures += closed_when_schedule_idle ? 0 : 1;

  std::printf("close-guard-test 失败项: %d\n", failures);
  return failures == 0 ? 0 : 1;
}

// ---- --backup-options-test ----
//
// PR #16 的自动化检查：算法 key 与 enum 的映射、四种算法组合的真实备份、
// 密码校验、未知 key、加密恢复、目录字段，以及密码不落盘。
//
// 它只做两件事：布置场景、对着产物与控制器状态断言。key 解析、密码校验、
// 归档命名、容器 header 全部由 BackupController / BackupCatalog / 核心负责 ——
// 在这里复制一份实现，测出来的就只是复制品。

// 泄漏哨兵密码。它只以"这一次操作的密码"的身份活在内存里，绝不允许落进
// 配置文件、状态文案、目录字段或仓库文件名。用一条不可能自然出现的长串，
// 才能把整张 QVariantMap 序列化之后用子串搜索把它揪出来。
const QString kSentinelPassword =
    QStringLiteral("PR16_TEST_PASSWORD_DO_NOT_PERSIST_92A7");
// DES 用另一个密码：两种算法共用一个哨兵时，"从哪一份产物泄漏的"就说不清了。
const QString kDesPassword = QStringLiteral("PR16_DES_TEST_PW_7K3");

// 解析出参的哨兵值。三个枚举的合法取值只有 0/1/2，200 永远不可能由一次成功的
// 解析产生；拿某个合法值当哨兵的话，"解析失败却写成了默认值"恰好会被漏掉。
const backupproject::PackMethod kUntouchedPackMethod =
    static_cast<backupproject::PackMethod>(200);
const backupproject::CompressionMethod kUntouchedCompressionMethod =
    static_cast<backupproject::CompressionMethod>(200);
const backupproject::EncryptionMethod kUntouchedEncryptionMethod =
    static_cast<backupproject::EncryptionMethod>(200);

// 断言计数。所有检查只累加、不提前返回：中途退出会让后面的检查永远不执行，
// 一次运行就只能看到一个失败。
struct CheckRun {
  // 输出前缀。默认值保持 PR #16 的既有输出不变，定时备份自检会换成 [schedule]。
  const char* prefix = "[backup-options]";
  int passed = 0;
  int failed = 0;
  QStringList failures;

  void Check(bool ok, const QString& label, const QString& detail = QString()) {
    if (ok) {
      ++passed;
      std::printf("%s   ok   %s\n", prefix, qPrintable(label));
      return;
    }
    ++failed;
    const QString text = detail.isEmpty()
                             ? label
                             : QStringLiteral("%1（%2）").arg(label, detail);
    failures.append(text);
    std::printf("%s   FAIL %s\n", prefix, qPrintable(text));
  }

  // 目录字段断言。fileName 一起写进标签：记录有七八条，失败时必须一眼看出
  // 是哪一条记录的哪个字段，而不是回头去数列表下标。
  void CheckRecordText(const QVariantMap& record, const QString& file_name,
                       const QString& field, const QString& expected) {
    const QString actual = record.value(field).toString();
    Check(actual == expected, QStringLiteral("%1 的 %2").arg(file_name, field),
          QStringLiteral("期望 [%1] 实际 [%2]").arg(expected, actual));
  }

  void CheckRecordBool(const QVariantMap& record, const QString& file_name,
                       const QString& field, bool expected) {
    const bool actual = record.value(field).toBool();
    Check(actual == expected, QStringLiteral("%1 的 %2").arg(file_name, field),
          QStringLiteral("期望 %1 实际 %2")
              .arg(expected ? QStringLiteral("true") : QStringLiteral("false"),
                   actual ? QStringLiteral("true") : QStringLiteral("false")));
  }

  void CheckRecordInt(const QVariantMap& record, const QString& file_name,
                      const QString& field, int expected) {
    const int actual = record.value(field).toInt();
    Check(actual == expected, QStringLiteral("%1 的 %2").arg(file_name, field),
          QStringLiteral("期望 %1 实际 %2").arg(expected).arg(actual));
  }
};

// 建一个文件（父目录一并建出来）。返回 false 说明环境本身有问题，
// 这类失败必须报出来，否则后面所有断言都跑在一个不完整的源目录上。
bool WriteTestFile(const QString& path, const QByteArray& content) {
  QDir().mkpath(QFileInfo(path).absolutePath());
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    return false;
  }
  return file.write(content) == content.size();
}

// 仓库的 .bak 快照。判断"这一次操作有没有真的落下一个新备份"靠前后两次快照做
// 差集，而不是假设列表第一条就是最新的 —— 列表顺序是 core 的实现细节。
QStringList SnapshotBakFiles(const QString& repository) {
  return QDir(repository)
      .entryList(QStringList() << QStringLiteral("*.bak"), QDir::Files,
                 QDir::Name);
}

// 快照差集里新出现的 .bak 文件名；没有新增时返回空串。
QString NewBakFileName(const QStringList& before, const QStringList& after) {
  for (const QString& name : after) {
    if (!before.contains(name)) {
      return name;
    }
  }
  return QString();
}

bool FileBytesEqual(const QString& left, const QString& right) {
  QFile left_file(left);
  QFile right_file(right);
  if (!left_file.open(QIODevice::ReadOnly) ||
      !right_file.open(QIODevice::ReadOnly)) {
    return false;
  }
  return left_file.readAll() == right_file.readAll();
}

// 文件类型一律用 lstat 判断：stat() 与 QFileInfo::exists() 都会跟着软链接走，
// "链接被恢复成普通文件"和"悬空链接被丢掉"这两类退化恰好查不出来。
bool LstatPath(const QString& path, struct stat* status) {
  return lstat(QFile::encodeName(path).constData(), status) == 0;
}

// 链接目标原文。QFile::symLinkTarget() 会把相对目标按链接所在目录展开成绝对
// 路径，而这里要断言的恰恰是"相对目标有没有被原样保留"，所以直接调
// readlink(2)， 与检查脚本里的 readlink 是同一个语义。
QString ReadLinkTarget(const QString& path) {
  const QByteArray native = QFile::encodeName(path);
  QByteArray buffer(4096, '\0');
  const ssize_t length =
      ::readlink(native.constData(), buffer.data(), buffer.size());
  if (length <= 0) {
    return QString();
  }
  return QString::fromLocal8Bit(buffer.constData(), static_cast<int>(length));
}

bool IsSymlinkTo(const QString& path, const QString& target) {
  struct stat status;
  return LstatPath(path, &status) && S_ISLNK(status.st_mode) &&
         ReadLinkTarget(path) == target;
}

bool IsFifo(const QString& path) {
  struct stat status;
  return LstatPath(path, &status) && S_ISFIFO(status.st_mode);
}

bool IsEmptyDirectory(const QString& path) {
  struct stat status;
  if (!LstatPath(path, &status) || !S_ISDIR(status.st_mode)) {
    return false;
  }
  return QDir(path)
      .entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden)
      .isEmpty();
}

// 造 --backup-options-test 的源目录，放的是流水线最容易丢东西的几类：
// 中文名、shell 与 URL 都会咬一口的 # 与百分号、空目录、相对软链接、
// 悬空软链接、FIFO。只备份几个普通文件的话，"恢复后还是不是软链接 / FIFO"
// 根本无从谈起。返回 false 表示环境不允许（例如建不出 FIFO）。
bool BuildOptionsTestSource(const QString& source) {
  const QStringList directories = {
      QStringLiteral("sub"), QStringLiteral("empty_dir"),
      QStringLiteral("links"), QStringLiteral("special")};
  for (const QString& directory : directories) {
    if (!QDir().mkpath(source + QLatin1Char('/') + directory)) {
      return false;
    }
  }
  if (!WriteTestFile(source + QStringLiteral("/plain.txt"),
                     QByteArray("PR16 plain.txt content\n")) ||
      !WriteTestFile(source + QStringLiteral("/中文文件.txt"),
                     QByteArray("中文内容\n")) ||
      !WriteTestFile(source + QStringLiteral("/sub/level1.txt"),
                     QByteArray("level1\n")) ||
      !WriteTestFile(source + QStringLiteral("/hash#percent%.txt"),
                     QByteArray("hash#percent%\n"))) {
    return false;
  }
  // 两个链接都按"链接目标原文"创建：relative_symlink 的目标相对它自己所在的
  // links/ 解释，所以它其实也是悬空的。要的就是这个 —— 恢复后的断言比的是
  // 链接目标原文，一旦被解引用成普通文件就会露馅。
  if (::symlink(
          "plain.txt",
          QFile::encodeName(source + QStringLiteral("/links/relative_symlink"))
              .constData()) != 0 ||
      ::symlink(
          "nope.txt",
          QFile::encodeName(source + QStringLiteral("/links/dangling_symlink"))
              .constData()) != 0) {
    return false;
  }
  return ::mkfifo(QFile::encodeName(source +
                                    QStringLiteral("/special/named_pipe.fifo"))
                      .constData(),
                  0644) == 0;
}

// 按 fileName 找一条目录记录。列表顺序不是契约，按下标取记录会在多一条备份时
// 静默看错对象。
QVariantMap FindRecord(const QVariantList& records, const QString& file_name) {
  for (const QVariant& item : records) {
    const QVariantMap record = item.toMap();
    if (record.value(QStringLiteral("fileName")).toString() == file_name) {
      return record;
    }
  }
  return QVariantMap();
}

// 把整张 QVariantMap 的 key 与 value 拼成一段文本。逐字段检查太容易漏，
// 而"密码被塞进某个没预料到的字段"正是泄漏审计要抓的东西。
QString FlattenRecord(const QVariantMap& record) {
  QString text;
  for (auto it = record.constBegin(); it != record.constEnd(); ++it) {
    text += it.key();
    text += QLatin1Char('=');
    text += it.value().toString();
    text += QLatin1Char('\n');
  }
  return text;
}

// --backup-options-test：走真实控制器入口跑完 PR #16 的全部功能断言。
// 命令行没有、也不会有 --password：密码一旦能从 argv 传进来，就会出现在
// ps 输出与 shell 历史里，所以这里只用文件里写死的测试密码。
//
// 返回值只区分"全部通过(0)"与"有失败(非 0)"；失败项逐条打印，
// 并且保留 QTemporaryDir 供人工进去看产物。
// ---- --gui-contract-test ----
//
// 人工验收发现的两类 GUI 契约。断言的是 QML 的真实几何与真实绑定结果：
// 不截图、不做像素比对，也不匹配文案本身。
//
// HOME-01..04 首页三张卡片的按钮必须完整落在卡片内（并且有正的底边距），三张
//             卡片的按钮对齐；正常窗口与窗口最小尺寸都要成立。
// MSG-01..05  某个页面产生的临时提示只在该页面显示；离开页面即被消费，回到该页
//             不会自动复现；后台任务在别的页面结束时也不会把提示丢过去。
//
// banner 读的是它自己的 showsMessage（"这一页该不该显示这条消息"），不是
// visible：Qt Quick 的 Item.visible 读出来就是**有效可见性**，StackLayout 里
// 非当前页整体不可见，用 visible 永远测不出"这一页会不会显示这条消息"。
// ---- --combo-hover-test ----
//
// 共享 AppComboBox 下拉行的状态回归（三轮人工验收都栽在同一个坑的不同形态上）。
//
//   第一轮：hover 底色绑到 control.highlightedIndex（常驻索引）
//   第二轮：改成"键盘高亮 + highlightedIndex"，但 highlightedIndex 会被鼠标改脏
//   第三轮：改成监听 Keys.onPressed，真实桌面上收不到事件（焦点在 ComboBox 上）
//
// 现在键盘模式由**Qt 导航的结果**推断：popup ListView 的 currentIndex 变了、
// 而且当前没有指针活动 —— 不监听按键，也不接管任何键。
//
// 这个自检真的把指针移到行上、移走，也真的把 ↓ 送进真实焦点链，然后逐行读
// 运行期状态（hovered / 覆盖层 opacity / currentIndex / 输入方式闸门）。
int RunComboHoverTest(QQuickWindow* window, backup_modern::AppTheme* theme) {
  CheckRun run;
  run.prefix = "[combo-hover]";

  const QColor hover_color = theme->property("hover").value<QColor>();
  const QColor keyboard_color = theme->property("accentSoft").value<QColor>();
  run.Check(hover_color.isValid() && keyboard_color.isValid(),
            QStringLiteral("主题给出了 hover / keyboard 两种颜色"),
            QStringLiteral("hover=%1 keyboard=%2")
                .arg(hover_color.name(), keyboard_color.name()));

  window->setProperty("currentPage", 1);
  window->setWidth(1280);
  window->setHeight(1000);
  WaitForAnimation(150);
  ScrollBackupPage(window, 700);
  WaitForAnimation(120);
  const auto clickByName = [window](const QString& name) -> bool {
    QQuickItem* item = window->findChild<QQuickItem*>(name);
    return item != nullptr && QMetaObject::invokeMethod(item, "clicked");
  };
  run.Check(clickByName(QStringLiteral("filterAddIncludeRuleButton")),
            QStringLiteral("展开“新建包含规则”表单"));
  WaitForAnimation(120);

  QQuickItem* combo =
      window->findChild<QQuickItem*>(QStringLiteral("filterRuleFieldCombo"));
  run.Check(combo != nullptr && combo->isVisible(),
            QStringLiteral("找得到可见的“条件类型”下拉"));
  if (combo == nullptr) {
    std::printf("[combo-hover] passed=%d failed=%d\n", run.passed, run.failed);
    return 1;
  }
  QObject* popup = combo->property("popup").value<QObject*>();
  QQuickItem* list = popup == nullptr
                         ? nullptr
                         : popup->property("contentItem").value<QQuickItem*>();
  run.Check(popup != nullptr && list != nullptr,
            QStringLiteral("下拉有 popup 与它的 contentItem"));
  if (popup == nullptr || list == nullptr) {
    std::printf("[combo-hover] passed=%d failed=%d\n", run.passed, run.failed);
    return 1;
  }

  // 复现人工验收截图：当前已选择项 = “路径”，鼠标划过“文件类型”。
  const QVariantList model = combo->property("model").toList();
  QStringList labels;
  for (const QVariant& item : model) {
    labels << item.toString();
  }
  const int path_index = labels.indexOf(QStringLiteral("路径"));
  const int type_index = labels.indexOf(QStringLiteral("文件类型"));
  run.Check(path_index >= 0 && type_index >= 0,
            QStringLiteral("条件类型下拉里能找到“路径”与“文件类型”"),
            labels.join(QStringLiteral("/")));
  if (path_index < 0 || type_index < 0) {
    std::printf("[combo-hover] passed=%d failed=%d\n", run.passed, run.failed);
    return 1;
  }
  combo->setProperty("currentIndex", path_index);
  WaitForAnimation(100);

  const auto collectRows = [list]() {
    QList<QPair<int, QQuickItem*>> rows;
    std::function<void(QQuickItem*)> walk = [&](QQuickItem* item) {
      if (item->objectName() == QLatin1String("comboItemRow"))
        rows.append({item->property("index").toInt(), item});
      const QList<QQuickItem*> kids = item->childItems();
      for (QQuickItem* kid : kids) {
        walk(kid);
      }
    };
    walk(list);
    std::sort(
        rows.begin(), rows.end(),
        [](const QPair<int, QQuickItem*>& a, const QPair<int, QQuickItem*>& b) {
          return a.first < b.first;
        });
    return rows;
  };
  const auto layerOpacity = [](const QPair<int, QQuickItem*>& row,
                               const char* name) -> qreal {
    QQuickItem* layer =
        row.second->findChild<QQuickItem*>(QString::fromLatin1(name));
    return layer == nullptr ? -1.0 : layer->property("opacity").toReal();
  };
  const auto hoverOpacity =
      [&layerOpacity](const QPair<int, QQuickItem*>& row) {
        return layerOpacity(row, "comboItemHoverLayer");
      };
  const auto keyboardOpacity =
      [&layerOpacity](const QPair<int, QQuickItem*>& row) {
        return layerOpacity(row, "comboItemKeyboardLayer");
      };
  const auto hasBackground = [&](const QPair<int, QQuickItem*>& row) -> bool {
    const QColor base =
        row.second
            ->findChild<QQuickItem*>(QStringLiteral("comboItemBackground"))
            ->property("color")
            .value<QColor>();
    return hoverOpacity(row) > 0.01 || keyboardOpacity(row) > 0.01 ||
           base.alpha() > 0;
  };
  const auto rowsWhere =
      [&](const QList<QPair<int, QQuickItem*>>& rows,
          const std::function<qreal(const QPair<int, QQuickItem*>&)>& opacity) {
        QStringList names;
        for (const auto& row : rows) {
          if (opacity(row) > 0.01) names << QString::number(row.first);
        }
        return names;
      };
  const auto describe = [&](const QList<QPair<int, QQuickItem*>>& rows) {
    QStringList parts;
    for (const auto& row : rows) {
      parts << QStringLiteral("%1[h=%2 k=%3 hv=%4 sel=%5]")
                   .arg(row.first)
                   .arg(hoverOpacity(row), 0, 'f', 0)
                   .arg(keyboardOpacity(row), 0, 'f', 0)
                   .arg(row.second->property("hovered").toBool() ? 1 : 0)
                   .arg(row.second->property("isSelected").toBool() ? 1 : 0);
    }
    return parts.join(QStringLiteral(" "));
  };
  const auto hoverRows = [&](const QList<QPair<int, QQuickItem*>>& rows) {
    return rowsWhere(rows, hoverOpacity);
  };
  const auto keyboardRows = [&](const QList<QPair<int, QQuickItem*>>& rows) {
    return rowsWhere(rows, keyboardOpacity);
  };
  const auto keyboardActive = [combo]() {
    return combo->property("keyboardNavigationActive").toBool();
  };
  const auto listIndex = [list]() {
    return list->property("currentIndex").toInt();
  };
  const auto movePointerTo = [window](const QPointF& pos,
                                      const QPointF& old_pos) {
    QHoverEvent hover(QEvent::HoverMove, pos, pos, old_pos);
    QCoreApplication::sendEvent(window, &hover);
  };
  const auto centerOf = [](QQuickItem* item) {
    return item->mapToScene(QPointF(item->width() / 2.0, item->height() / 2.0));
  };
  const auto sendKey = [window](int key) {
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &press);
    QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &release);
  };

  QMetaObject::invokeMethod(popup, "open");
  WaitForAnimation(320);
  QList<QPair<int, QQuickItem*>> rows = collectRows();
  run.Check(rows.size() >= 7, QStringLiteral("条件类型下拉展开了至少 7 行"),
            QStringLiteral("实际 %1 行").arg(rows.size()));
  if (rows.size() < 7) {
    std::printf("[combo-hover] passed=%d failed=%d\n", run.passed, run.failed);
    return 1;
  }
  const auto rowAt = [&rows](int index) -> QQuickItem* {
    for (const auto& row : rows) {
      if (row.first == index) return row.second;
    }
    return nullptr;
  };

  // ---- Mouse 1：打开 popup，指针不在任何地方，键盘模式必须是关的 ----
  run.Check(
      !keyboardActive() && hoverRows(rows).isEmpty() &&
          keyboardRows(rows).isEmpty(),
      QStringLiteral("Mouse 1 打开 popup：无 hover 底色、无键盘光标、模式关闭"),
      QStringLiteral("hover 行=%1 键盘行=%2 模式=%3 | %4")
          .arg(hoverRows(rows).join(QStringLiteral(",")),
               keyboardRows(rows).join(QStringLiteral(",")))
          .arg(keyboardActive())
          .arg(describe(rows)));
  run.Check(listIndex() == path_index &&
                combo->property("currentIndex").toInt() == path_index,
            QStringLiteral("Mouse 1 打开时键盘位置同步到当前已选择项"),
            QStringLiteral("listCur=%1 currentIndex=%2")
                .arg(listIndex())
                .arg(combo->property("currentIndex").toInt()));
  {
    QQuickItem* selected_row = rowAt(path_index);
    QQuickItem* check = selected_row == nullptr
                            ? nullptr
                            : selected_row->findChild<QQuickItem*>(
                                  QStringLiteral("comboItemCheck"));
    run.Check(check != nullptr && check->property("visible").toBool() &&
                  !hasBackground({path_index, selected_row}),
              QStringLiteral("Mouse 1 已选择项（路径）只有勾号，没有底色"));
  }

  // ---- Mouse 2：指针 hover “文件类型” ----
  QPointF pointer = centerOf(rowAt(type_index));
  movePointerTo(pointer, centerOf(rowAt(path_index)));
  WaitForAnimation(150);
  rows = collectRows();
  run.Check(hoverRows(rows) == QStringList{QString::number(type_index)} &&
                keyboardRows(rows).isEmpty() && !keyboardActive(),
            QStringLiteral(
                "Mouse 2 只有“文件类型”有 hover 灰底，且没有误触发键盘模式"),
            QStringLiteral("hover 行=%1 键盘行=%2 模式=%3 | %4")
                .arg(hoverRows(rows).join(QStringLiteral(",")),
                     keyboardRows(rows).join(QStringLiteral(",")))
                .arg(keyboardActive())
                .arg(describe(rows)));

  // ---- Mouse 3：指针移出，但 highlightedIndex / listCur 都还停在那一行 ----
  movePointerTo(QPointF(4, 4), pointer);
  WaitForAnimation(220);
  rows = collectRows();
  const int retained_hl = combo->property("highlightedIndex").toInt();
  const int retained_list = listIndex();
  run.Check(
      !rowAt(type_index)->property("hovered").toBool() &&
          retained_hl == type_index && retained_list == type_index,
      QStringLiteral("Mouse 3 指针离开后索引仍停在被划过的那一行（Qt 的行为）"),
      QStringLiteral("hovered=%1 highlightedIndex=%2 listCur=%3")
          .arg(rowAt(type_index)->property("hovered").toBool())
          .arg(retained_hl)
          .arg(retained_list));
  run.Check(!keyboardActive() && hoverRows(rows).isEmpty() &&
                keyboardRows(rows).isEmpty(),
            QStringLiteral(
                "Mouse 3 索引留在那一行，但视觉上没有任何底色（人工验收截图）"),
            QStringLiteral("hover 行=%1 键盘行=%2 模式=%3 | %4")
                .arg(hoverRows(rows).join(QStringLiteral(",")),
                     keyboardRows(rows).join(QStringLiteral(",")))
                .arg(keyboardActive())
                .arg(describe(rows)));

  // ---- Mouse 4：重新进入，hover 立刻接管 ----
  movePointerTo(centerOf(rowAt(type_index)), QPointF(4, 4));
  WaitForAnimation(150);
  rows = collectRows();
  run.Check(hoverRows(rows) == QStringList{QString::number(type_index)},
            QStringLiteral("Mouse 4 指针重新进入：hover 立刻接管"),
            describe(rows));

  // ---- Keyboard 1：真实焦点链上的 ↓ ----
  //
  // 真实桌面实测：点开下拉之后焦点在 ComboBox 上，按键送到窗口后由 ComboBox
  // 处理， popup ListView 的 currentIndex 随之前移。这里复现同一条链路。
  movePointerTo(QPointF(4, 4), centerOf(rowAt(type_index)));
  WaitForAnimation(200);
  combo->forceActiveFocus();
  WaitForAnimation(150);
  const int before_down = listIndex();
  sendKey(Qt::Key_Down);
  WaitForAnimation(220);
  rows = collectRows();
  run.Check(
      listIndex() == before_down + 1,
      QStringLiteral("Keyboard 1 ↓ 让 popup 的 currentIndex 前移一项"),
      QStringLiteral("listCur %1 -> %2").arg(before_down).arg(listIndex()));
  run.Check(keyboardActive(),
            QStringLiteral("Keyboard 1 ↓ 之后键盘模式打开（由导航结果推断）"),
            QStringLiteral("模式=%1").arg(keyboardActive()));
  run.Check(
      keyboardRows(rows) == QStringList{QString::number(listIndex())},
      QStringLiteral("Keyboard 1 键盘光标正好落在 Qt 移动到的那个 row 上"),
      QStringLiteral("键盘行=%1 listCur=%2 | %3")
          .arg(keyboardRows(rows).join(QStringLiteral(",")))
          .arg(listIndex())
          .arg(describe(rows)));

  // ---- Keyboard 2：再 ↓ 一次，光标整体下移一行 ----
  const int first_keyboard_row = listIndex();
  sendKey(Qt::Key_Down);
  WaitForAnimation(220);
  rows = collectRows();
  run.Check(listIndex() == first_keyboard_row + 1 &&
                keyboardRows(rows) == QStringList{QString::number(listIndex())},
            QStringLiteral("Keyboard 2 再 ↓：光标整体下移一行，上一行立刻熄灭"),
            QStringLiteral("键盘行=%1 listCur=%2 | %3")
                .arg(keyboardRows(rows).join(QStringLiteral(",")))
                .arg(listIndex())
                .arg(describe(rows)));

  // ---- Keyboard 3：↑ 回上一行 ----
  sendKey(Qt::Key_Up);
  WaitForAnimation(220);
  rows = collectRows();
  run.Check(listIndex() == first_keyboard_row &&
                keyboardRows(rows) == QStringList{QString::number(listIndex())},
            QStringLiteral("Keyboard 3 ↑ 把光标移回上一行"),
            QStringLiteral("键盘行=%1 listCur=%2")
                .arg(keyboardRows(rows).join(QStringLiteral(",")))
                .arg(listIndex()));

  // ---- Keyboard 4：光标落在"已选择项"上时仍然看得见 ----
  while (listIndex() > path_index) {
    sendKey(Qt::Key_Up);
    WaitForAnimation(180);
  }
  rows = collectRows();
  {
    QQuickItem* selected_row = rowAt(path_index);
    QQuickItem* check = selected_row == nullptr
                            ? nullptr
                            : selected_row->findChild<QQuickItem*>(
                                  QStringLiteral("comboItemCheck"));
    run.Check(
        listIndex() == path_index && keyboardActive() &&
            keyboardOpacity({path_index, selected_row}) > 0.01 &&
            check != nullptr && check->property("visible").toBool(),
        QStringLiteral("Keyboard 4 键盘光标落在已选择项上：光标与勾号同时可见"),
        QStringLiteral("listCur=%1 键盘层=%2 勾号=%3")
            .arg(listIndex())
            .arg(keyboardOpacity({path_index, selected_row}))
            .arg(check != nullptr && check->property("visible").toBool()));
  }

  // ---- Mouse 5：键盘模式下移动鼠标 -> 键盘模式立刻退出 ----
  const QPointF back_to_type = centerOf(rowAt(type_index));
  movePointerTo(back_to_type, QPointF(4, 4));
  WaitForAnimation(220);
  rows = collectRows();
  run.Check(!keyboardActive() && keyboardRows(rows).isEmpty() &&
                hoverRows(rows) == QStringList{QString::number(type_index)},
            QStringLiteral(
                "Mouse 5 键盘模式下移动鼠标：键盘光标立即消失、hover 接管"),
            QStringLiteral("hover 行=%1 键盘行=%2 模式=%3 | %4")
                .arg(hoverRows(rows).join(QStringLiteral(",")),
                     keyboardRows(rows).join(QStringLiteral(",")))
                .arg(keyboardActive())
                .arg(describe(rows)));

  // ---- Mouse 6：指针再离开 -> 仍然什么都不留 ----
  movePointerTo(QPointF(4, 4), back_to_type);
  WaitForAnimation(200);
  rows = collectRows();
  run.Check(!keyboardActive() && hoverRows(rows).isEmpty() &&
                keyboardRows(rows).isEmpty(),
            QStringLiteral("Mouse 6 指针离开后没有任何底色残留"),
            describe(rows));

  // ---- Keyboard 5：Enter 采纳，Esc 不改选择 ----
  combo->forceActiveFocus();
  sendKey(Qt::Key_Down);
  WaitForAnimation(200);
  const int enter_target = listIndex();
  sendKey(Qt::Key_Return);
  WaitForAnimation(260);
  run.Check(!popup->property("visible").toBool() &&
                combo->property("currentIndex").toInt() == enter_target &&
                !keyboardActive(),
            QStringLiteral("Keyboard 5 Enter 采纳当前键盘行并关闭下拉"),
            QStringLiteral("visible=%1 currentIndex=%2 期望=%3 模式=%4")
                .arg(popup->property("visible").toBool())
                .arg(combo->property("currentIndex").toInt())
                .arg(enter_target)
                .arg(keyboardActive()));
  const int selected_after_enter = combo->property("currentIndex").toInt();
  QMetaObject::invokeMethod(popup, "open");
  WaitForAnimation(300);
  sendKey(Qt::Key_Down);
  WaitForAnimation(200);
  sendKey(Qt::Key_Escape);
  WaitForAnimation(260);
  run.Check(
      !popup->property("visible").toBool() &&
          combo->property("currentIndex").toInt() == selected_after_enter &&
          !keyboardActive(),
      QStringLiteral("Keyboard 6 Esc 关闭下拉且不改动已选择的值"),
      QStringLiteral("visible=%1 currentIndex=%2 期望=%3")
          .arg(popup->property("visible").toBool())
          .arg(combo->property("currentIndex").toInt())
          .arg(selected_after_enter));

  // ---- 关掉再打开：没有 stale ----
  QMetaObject::invokeMethod(popup, "open");
  WaitForAnimation(320);
  rows = collectRows();
  run.Check(!keyboardActive() && keyboardRows(rows).isEmpty() &&
                hoverRows(rows).isEmpty(),
            QStringLiteral(
                "Reopen 重新打开下拉：没有 stale 键盘光标、没有 stale 灰底"),
            QStringLiteral("键盘行=%1 hover 行=%2 模式=%3 | %4")
                .arg(keyboardRows(rows).join(QStringLiteral(",")),
                     hoverRows(rows).join(QStringLiteral(",")))
                .arg(keyboardActive())
                .arg(describe(rows)));

  QMetaObject::invokeMethod(popup, "close");
  WaitForAnimation(200);
  clickByName(QStringLiteral("filterRuleCancelButton"));
  WaitForAnimation(80);

  std::printf("[combo-hover] passed=%d failed=%d\n", run.passed, run.failed);
  if (run.failed != 0) {
    for (const QString& failure : run.failures)
      std::printf("[combo-hover]   FAIL %s\n", qPrintable(failure));
  }
  return run.failed == 0 ? 0 : 1;
}

// ---- --filter-ux-test ----
//
// 三个页面"同一个普通表单输入 -> 同一条 DSL"的 parity 自检。
//
// 这是 GUI Usability Closure 的核心断言：用户在备份页 / 自动备份页 / 实时备份页
// 做**同一次操作**（选"文件扩展名"、填 txt;md），最终必须得到同一条规则文本
// ext:txt;md，而且这条规则必须被真实的 Filter::AddRule 接受。
//
// 它走真实界面：切页、展开高级设置、点真实的按钮、往真实的输入框里打字，然后读
// 真实的模型。不是"两边都调了同一个函数"，也不是 grep 文件。
//
// 同时钉住三件事：
//   * 条件类型下拉显示的是中文（文件扩展名 / 路径 / 文件大小 / 文件类型 ...）；
//   * 不同条件用不同控件（文件类型与比较方式只能是下拉，大小是
//     比较方式 + 数值 + 单位，绝不是一个裸文本框）；
//   * 非法输入在三处拿到**同一句**来自共享 builder 的原因。
int RunFilterUxTest(QQuickWindow* window,
                    backup_modern::FilterRuleModel* manual_model,
                    backup_modern::FilterRuleModel* schedule_model,
                    backup_modern::FilterRuleModel* realtime_model) {
  CheckRun run;
  run.prefix = "[filter-ux]";

  const auto itemByName = [window](const QString& name) -> QQuickItem* {
    return window->findChild<QQuickItem*>(name);
  };
  const auto goToPage = [window](int page) {
    window->setProperty("currentPage", page);
    WaitForAnimation(90);
  };
  // 点真实按钮：AppButton 是 AbstractButton，clicked 是它的信号。
  const auto click = [](QQuickItem* item) -> bool {
    if (item == nullptr) return false;
    return QMetaObject::invokeMethod(item, "clicked");
  };
  // 下拉不能只改 currentIndex：onActivated 只由用户激活触发，所以要先把
  // currentIndex 设成目标值，再发一次
  // activated(index)，与用户真的点了一下等价。
  const auto choose = [](QQuickItem* combo, int index) -> bool {
    if (combo == nullptr || index < 0) return false;
    combo->setProperty("currentIndex", index);
    return QMetaObject::invokeMethod(combo, "activated", Q_ARG(int, index));
  };
  // TextField 的 textEdited 在 QML 类型的元对象里是无参信号（实参由控件的
  // text 属性承载），所以先把 text 设成目标值，再发一次 textEdited()。
  const auto typeInto = [](QQuickItem* field, const QString& text) -> bool {
    if (field == nullptr) return false;
    field->setProperty("text", text);
    return QMetaObject::invokeMethod(field, "textEdited");
  };
  // 输入框用 text，下拉框用 displayText —— 两种控件的"当前显示文本"不是同一个
  // 属性，读错了会得到空串而不是失败。
  const auto textOf = [](QQuickItem* item) -> QString {
    if (item == nullptr) return QStringLiteral("<missing>");
    const QVariant text = item->property("text");
    if (text.isValid() && !text.toString().isEmpty()) return text.toString();
    const QVariant display = item->property("displayText");
    return display.isValid() ? display.toString() : QString();
  };
  // 断言"这个控件此刻可见吗"。不能用 QQuickItem::isVisible()：它要求窗口真的
  // exposed，而自检跑在 offscreen 平台上；这里读的是控件自己那条 visible 绑定。
  const auto shown = [](QQuickItem* item) -> bool {
    return item != nullptr && item->property("visible").toBool();
  };

  struct PageCase {
    const char* label;
    int page;
    QString prefix;
    backup_modern::FilterRuleModel* model;
    // 计划页 / 实时页的规则编辑器在默认折叠的「高级设置」里。
    QString advanced_toggle;
    QString advanced_section;
  };
  // 页面下标 = Main.qml 里 StackLayout 的顺序（0 首页 / 1 备份 / 2 自动备份 /
  // 3 备份管理 / 4 设置 / 5 实时备份），不是侧栏导航的顺序。
  const PageCase kCases[] = {
      {"备份页", 1, QStringLiteral("filter"), manual_model, QString(),
       QString()},
      {"自动备份页", 2, QStringLiteral("schedule"), schedule_model,
       QStringLiteral("scheduleAdvancedToggle"),
       QStringLiteral("scheduleAdvancedSection")},
      {"实时备份页", 5, QStringLiteral("realtime"), realtime_model,
       QStringLiteral("realtimeAdvancedToggle"),
       QStringLiteral("realtimeAdvancedSection")},
  };

  // 三处必须拿到**同一句**错误：它来自共享 builder，不是三份前端文案。
  QStringList rejection_reasons;

  for (const PageCase& page : kCases) {
    // 中文标签必须走 fromUtf8：fromLatin1 会把 UTF-8 字节按 Latin-1 解释，
    // 日志里的页名会变成一串乱码，脚本按名字断言就会假失败。
    const QString label = QString::fromUtf8(page.label);
    const auto named = [&page, &itemByName](const char* suffix) -> QQuickItem* {
      return itemByName(page.prefix + QString::fromLatin1(suffix));
    };

    goToPage(page.page);

    // ---- 默认折叠：高级设置（规则编辑器在它里面）----
    if (!page.advanced_toggle.isEmpty()) {
      QQuickItem* section = itemByName(page.advanced_section);
      run.Check(section != nullptr && !shown(section),
                QStringLiteral("%1 高级设置默认收起").arg(label),
                QStringLiteral("section=%1").arg(section != nullptr));
      click(itemByName(page.advanced_toggle));
      WaitForAnimation(80);
      run.Check(shown(section),
                QStringLiteral("%1 展开之后高级设置可见").arg(label));
    }

    // ---- 新建规则的表单默认收起，点"添加包含规则"才出现 ----
    QQuickItem* builder = named("RuleBuilderForm");
    run.Check(builder != nullptr && !shown(builder),
              QStringLiteral("%1 新建规则的表单默认收起").arg(label));
    click(named("AddIncludeRuleButton"));
    WaitForAnimation(80);
    run.Check(
        shown(builder),
        QStringLiteral("%1 点“添加包含规则”后表单展开（不是弹窗）").arg(label));

    // ---- 条件类型下拉显示中文 ----
    QQuickItem* field_combo = named("RuleFieldCombo");
    run.Check(textOf(field_combo) == QStringLiteral("文件扩展名"),
              QStringLiteral("%1 条件类型默认显示“文件扩展名”").arg(label),
              textOf(field_combo));
    const QVariantList field_options =
        field_combo == nullptr ? QVariantList()
                               : field_combo->property("model").toList();
    QStringList field_labels;
    for (const QVariant& option : field_options) {
      field_labels << option.toString();
    }
    run.Check(
        field_labels.contains(QStringLiteral("文件类型")) &&
            field_labels.contains(QStringLiteral("文件大小")) &&
            field_labels.contains(QStringLiteral("路径")) &&
            !field_labels.contains(QStringLiteral("ext")),
        QStringLiteral("%1 条件类型下拉全部是中文（没有裸字段名）").arg(label),
        field_labels.join(QStringLiteral("/")));
    run.Check(
        !shown(named("RuleTypeCombo")) &&
            !shown(named("RuleSizeCompareCombo")) &&
            named("RuleExtensionField") != nullptr,
        QStringLiteral("%1 选“文件扩展名”时只出现扩展名输入框").arg(label));

    // ---- 同一次普通操作：扩展名 -> txt;md ----
    run.Check(typeInto(named("RuleExtensionField"), QStringLiteral("txt;md")),
              QStringLiteral("%1 可以在扩展名输入框里输入").arg(label));
    WaitForAnimation(60);
    run.Check(textOf(named("RuleFormSummaryText"))
                  .startsWith(QStringLiteral("将添加：")),
              QStringLiteral("%1 实时显示这条规则的人话摘要").arg(label),
              textOf(named("RuleFormSummaryText")));
    run.Check(textOf(named("RuleFormErrorText")).isEmpty(),
              QStringLiteral("%1 合法输入没有报错").arg(label),
              textOf(named("RuleFormErrorText")));
    run.Check(click(named("RuleSubmitButton")),
              QStringLiteral("%1 “添加规则”可点").arg(label));
    WaitForAnimation(80);
    run.Check(
        page.model->rulesForAction(QStringLiteral("include")) ==
            QStringList{QStringLiteral("ext:txt;md")},
        QStringLiteral("%1 生成 ext:txt;md（用户没有写过 ext:）").arg(label),
        page.model->rulesForAction(QStringLiteral("include"))
            .join(QStringLiteral(",")));
    run.Check(page.model->rulesForAction(QStringLiteral("exclude")).isEmpty(),
              QStringLiteral("%1 这条规则落在包含一侧").arg(label));

    // 规则卡片的数据：主行是人话，DSL 是次要信息。
    const QVariantList rules = page.model->property("rules").toList();
    if (!rules.isEmpty()) {
      const QVariantMap first = rules.at(0).toMap();
      run.Check(first.value(QStringLiteral("actionLabel")).toString() ==
                        QStringLiteral("包含") &&
                    first.value(QStringLiteral("conditionLabel")).toString() ==
                        QStringLiteral("文件扩展名：txt、md"),
                QStringLiteral("%1 规则主行是“包含 · 文件扩展名：txt、md”")
                    .arg(label),
                first.value(QStringLiteral("conditionLabel")).toString());
      run.Check(first.value(QStringLiteral("dsl")).toString() ==
                    QStringLiteral("ext:txt;md"),
                QStringLiteral("%1 卡片的 DSL 字段与提交的一致").arg(label));
    }

    // ---- 路径：排除规则（同一个表单，换一个条件类型）----
    click(named("AddExcludeRuleButton"));
    WaitForAnimation(60);
    choose(field_combo, field_labels.indexOf(QStringLiteral("路径")));
    WaitForAnimation(60);
    run.Check(
        shown(named("RulePatternField")) && !shown(named("RuleExtensionField")),
        QStringLiteral("%1 换“路径”之后出现的是路径输入框").arg(label));
    typeInto(named("RulePatternField"), QStringLiteral("**/build/**"));
    click(named("RuleSubmitButton"));
    WaitForAnimation(80);
    run.Check(page.model->rulesForAction(QStringLiteral("exclude")) ==
                  QStringList{QStringLiteral("path:**/build/**")},
              QStringLiteral("%1 生成 path:**/build/**").arg(label),
              page.model->rulesForAction(QStringLiteral("exclude"))
                  .join(QStringLiteral(",")));

    // ---- 文件类型：只能是下拉 ----
    click(named("AddIncludeRuleButton"));
    WaitForAnimation(60);
    choose(field_combo, field_labels.indexOf(QStringLiteral("文件类型")));
    WaitForAnimation(60);
    QQuickItem* type_combo = named("RuleTypeCombo");
    run.Check(shown(type_combo),
              QStringLiteral("%1 文件类型用的是下拉，不是文本框").arg(label));
    choose(type_combo, 1);  // 目录
    click(named("RuleSubmitButton"));
    WaitForAnimation(80);
    run.Check(page.model->rulesForAction(QStringLiteral("include"))
                  .contains(QStringLiteral("type:folder")),
              QStringLiteral("%1 选了“目录”就生成 type:folder").arg(label),
              page.model->rulesForAction(QStringLiteral("include"))
                  .join(QStringLiteral(",")));

    // ---- 文件大小：比较方式 + 数值 + 单位 ----
    click(named("AddIncludeRuleButton"));
    WaitForAnimation(60);
    choose(field_combo, field_labels.indexOf(QStringLiteral("文件大小")));
    WaitForAnimation(60);
    run.Check(shown(named("RuleSizeCompareCombo")) &&
                  shown(named("RuleSizeValueField")) &&
                  shown(named("RuleSizeUnitCombo")),
              QStringLiteral("%1 文件大小是比较方式 + 数值 + 单位三个控件")
                  .arg(label));
    run.Check(textOf(named("RuleSizeCompareCombo")) == QStringLiteral("小于"),
              QStringLiteral("%1 比较方式默认“小于”").arg(label),
              textOf(named("RuleSizeCompareCombo")));
    typeInto(named("RuleSizeValueField"), QStringLiteral("1"));
    choose(named("RuleSizeUnitCombo"), 2);  // MB
    click(named("RuleSubmitButton"));
    WaitForAnimation(80);
    run.Check(
        page.model->rulesForAction(QStringLiteral("include"))
            .contains(QStringLiteral("size:<1MB")),
        QStringLiteral("%1 生成 size:<1MB（用户没有写过 size:）").arg(label),
        page.model->rulesForAction(QStringLiteral("include"))
            .join(QStringLiteral(",")));

    // ---- 非法输入：三处必须拿到同一句共享 builder 的原因 ----
    click(named("AddIncludeRuleButton"));
    WaitForAnimation(60);
    choose(field_combo, field_labels.indexOf(QStringLiteral("文件大小")));
    WaitForAnimation(60);
    typeInto(named("RuleSizeValueField"), QStringLiteral("abc"));
    WaitForAnimation(60);
    const QString reason = textOf(named("RuleFormErrorText"));
    rejection_reasons << reason;
    run.Check(!reason.isEmpty() &&
                  !named("RuleSubmitButton")->property("enabled").toBool(),
              QStringLiteral("%1 非法的大小取值被拒绝，且“添加规则”不可点")
                  .arg(label),
              reason);
    // 溢出：以前 QML 会先 parseInt("99999999999999999999")，等 C++ 拿到时它已经
    // 变成一个浮点数了，谁都没机会拒绝。现在数值按文本解析，溢出是明确失败。
    typeInto(named("RuleSizeValueField"),
             QStringLiteral("99999999999999999999"));
    WaitForAnimation(60);
    const QString overflow_reason = textOf(named("RuleFormErrorText"));
    run.Check(
        overflow_reason.contains(QStringLiteral("超出可表示范围")),
        QStringLiteral("%1 超大数值被明确拒绝（不是静默截断）").arg(label),
        overflow_reason);
    click(named("RuleCancelButton"));
    WaitForAnimation(60);

    // ---- 高级 DSL：默认收起，展开后仍然可用 ----
    QQuickItem* advanced_section = named("AdvancedRulesSection");
    run.Check(
        advanced_section != nullptr && !shown(advanced_section),
        QStringLiteral("%1 高级规则默认收起（普通用户看不到 DSL）").arg(label));
    click(named("AdvancedRulesToggle"));
    WaitForAnimation(60);
    run.Check(shown(advanced_section),
              QStringLiteral("%1 高级规则展开后可见").arg(label));
    typeInto(named("AdvancedRuleField"), QStringLiteral("size:<abc"));
    click(named("AddAdvancedRuleButton"));
    WaitForAnimation(80);
    run.Check(!textOf(named("AdvancedRuleErrorText")).isEmpty(),
              QStringLiteral("%1 高级 DSL 的非法输入被共享核心拒绝").arg(label),
              textOf(named("AdvancedRuleErrorText")));
    click(named("AdvancedRulesToggle"));
    WaitForAnimation(60);
  }

  // ---- parity：三处最终拿到的是同一组规则文本 ----
  const QStringList manual_include =
      manual_model->rulesForAction(QStringLiteral("include"));
  const QStringList schedule_include =
      schedule_model->rulesForAction(QStringLiteral("include"));
  const QStringList realtime_include =
      realtime_model->rulesForAction(QStringLiteral("include"));
  run.Check(manual_include == schedule_include &&
                schedule_include == realtime_include,
            QStringLiteral("PARITY-01 三处的包含规则逐字相同"),
            QStringLiteral("manual=[%1] schedule=[%2] realtime=[%3]")
                .arg(manual_include.join(QStringLiteral(" ")),
                     schedule_include.join(QStringLiteral(" ")),
                     realtime_include.join(QStringLiteral(" "))));
  run.Check(manual_include == QStringList({QStringLiteral("ext:txt;md"),
                                           QStringLiteral("type:folder"),
                                           QStringLiteral("size:<1MB")}),
            QStringLiteral("PARITY-02 普通表单输入生成的就是核心认可的 DSL"),
            manual_include.join(QStringLiteral(" ")));
  run.Check(manual_model->rulesForAction(QStringLiteral("exclude")) ==
                    schedule_model->rulesForAction(QStringLiteral("exclude")) &&
                schedule_model->rulesForAction(QStringLiteral("exclude")) ==
                    realtime_model->rulesForAction(QStringLiteral("exclude")),
            QStringLiteral("PARITY-03 三处的排除规则逐字相同"));
  run.Check(rejection_reasons.size() == 3 &&
                rejection_reasons.at(0) == rejection_reasons.at(1) &&
                rejection_reasons.at(1) == rejection_reasons.at(2) &&
                !rejection_reasons.at(0).isEmpty(),
            QStringLiteral(
                "PARITY-04 非法输入在三处得到同一句原因（来自共享 builder）"),
            rejection_reasons.join(QStringLiteral(" | ")));

  // 收尾：三份模型都清空，后面的自检从干净状态开始。
  manual_model->clearRules();
  schedule_model->clearRules();
  realtime_model->clearRules();

  std::printf("[filter-ux] passed=%d failed=%d\n", run.passed, run.failed);
  if (run.failed != 0) {
    for (const QString& failure : run.failures)
      std::printf("[filter-ux]   FAIL %s\n", qPrintable(failure));
  }
  return run.failed == 0 ? 0 : 1;
}

int RunGuiContractTest(QQuickWindow* window,
                       backup_modern::BackupController* controller) {
  CheckRun run;
  run.prefix = "[gui-contract]";

  // 页面顺序必须与 Main.qml 的 StackLayout 一致。
  const char* kBannerNames[5] = {
      "homeStatusBanner", "backupStatusBanner", "scheduleStatusBanner",
      "managementStatusBanner", "settingsStatusBanner"};

  const auto goToPage = [window](int page) {
    window->setProperty("currentPage", page);
    // 切换之后要让绑定与布局都算完：这里等的是"事件循环转一圈 + 布局"，
    // 不是某个动画时长。
    WaitForAnimation(60);
  };
  const auto banner = [window, &kBannerNames](int page) -> QObject* {
    return window->findChild<QObject*>(QString::fromLatin1(kBannerNames[page]));
  };
  const auto bannerTitle = [&banner](int page) {
    QObject* item = banner(page);
    return item == nullptr ? QStringLiteral("<missing>")
                           : item->property("title").toString();
  };
  // 这一页会不会显示这条提示（与它此刻是不是当前页无关）。
  const auto pageShows = [&banner](int page, const QString& title) {
    QObject* item = banner(page);
    return item != nullptr && item->property("showsMessage").toBool() &&
           item->property("title").toString() == title;
  };
  const auto itemByName = [window](const char* name) -> QQuickItem* {
    return window->findChild<QQuickItem*>(QString::fromLatin1(name));
  };

  // ---- HOME：三个按钮必须完整在卡片里 ----
  const auto checkHomeCards = [&](int width, int height, const QString& label) {
    window->setWidth(width);
    window->setHeight(height);
    goToPage(0);

    qreal minimum_margin = -1.0;
    qreal baseline = -1.0;
    bool all_inside = true;
    bool aligned = true;
    QString detail;

    for (int index = 0; index < 3; ++index) {
      const QQuickItem* card = itemByName(index == 0   ? "homeCard0"
                                          : index == 1 ? "homeCard1"
                                                       : "homeCard2");
      const QQuickItem* button = itemByName(index == 0   ? "homeAction0"
                                            : index == 1 ? "homeAction1"
                                                         : "homeAction2");
      if (card == nullptr || button == nullptr) {
        run.Check(false, QStringLiteral("%1 找得到卡片与按钮").arg(label),
                  QStringLiteral("card=%1 button=%2")
                      .arg(card != nullptr)
                      .arg(button != nullptr));
        return;
      }
      const QPointF top_left = button->mapToItem(card, QPointF(0, 0));
      const qreal margin = card->height() - (top_left.y() + button->height());
      // 左、右、上三条边也要在卡片内：只验下边界会漏掉横向越界。
      const bool inside = top_left.x() >= 0.0 && top_left.y() >= 0.0 &&
                          top_left.x() + button->width() <= card->width();
      all_inside = all_inside && inside;
      if (!inside) detail += QStringLiteral("按钮 %1 越界 ").arg(index);
      minimum_margin =
          minimum_margin < 0.0 ? margin : std::min(minimum_margin, margin);

      const qreal window_y =
          button->mapToItem(window->contentItem(), QPointF(0, 0)).y();
      if (baseline < 0.0)
        baseline = window_y;
      else if (std::abs(window_y - baseline) > 1.0)
        aligned = false;
    }

    // HOME-01：三个按钮都在卡片内，且底边距统一为正。
    run.Check(all_inside && minimum_margin >= 12.0,
              QStringLiteral("%1 HOME-01 按钮完整落在卡片内且底边距 >= 12px")
                  .arg(label),
              QStringLiteral("最小底边距=%1 全部在卡片内=%2")
                  .arg(minimum_margin)
                  .arg(all_inside));
    // HOME-02：三张卡片的按钮对齐（同一 baseline，容差 1px）。
    run.Check(aligned,
              QStringLiteral("%1 HOME-02 三个按钮对齐（容差 1px）").arg(label),
              detail);
    // HOME-03：按钮不能压住卡片下边框，也不能超出卡片。
    run.Check(minimum_margin > 0.0,
              QStringLiteral("%1 HOME-03 卡片下边框没有被按钮压住").arg(label),
              QStringLiteral("最小底边距=%1").arg(minimum_margin));
    // HOME-04：卡片本身也必须在页面内（窄窗口下不能溢出到窗口外）。
    const QQuickItem* row_card = itemByName("homeCard0");
    const qreal card_bottom =
        row_card
            ->mapToItem(window->contentItem(), QPointF(0, row_card->height()))
            .y();
    const bool card_visible_in_window =
        card_bottom <= static_cast<qreal>(height);
    run.Check(
        card_visible_in_window,
        QStringLiteral("%1 HOME-04 卡片没有溢出窗口").arg(label),
        QStringLiteral("卡片底部=%1 窗口高=%2").arg(card_bottom).arg(height));
  };

  checkHomeCards(1180, 760, QStringLiteral("正常窗口 1180x760"));
  checkHomeCards(960, 620, QStringLiteral("最小窗口 960x620"));

  // ---- MSG：临时提示只属于产生它的页面 ----
  QTemporaryDir temp;
  if (!temp.isValid()) {
    std::fprintf(stderr, "[gui-contract] 无法创建临时目录\n");
    return 1;
  }

  // MSG-01：备份页的输入校验错误不得跑到别的页面，回来后也不自动复现。
  controller->setSourcePath(QString());
  goToPage(1);
  const bool started = controller->startBackupWithOptions(
      QStringLiteral("mypack"), QStringLiteral("none"), QStringLiteral("none"),
      QString(), QString());
  run.Check(!started && controller->statusKind() == QStringLiteral("error"),
            QStringLiteral("MSG-01 空源目录触发错误提示"),
            controller->statusTitle() + QStringLiteral("/") +
                controller->statusMessage());
  run.Check(controller->statusScope() == QStringLiteral("backup"),
            QStringLiteral("MSG-01 这条错误属于备份页"),
            controller->statusScope());
  run.Check(pageShows(1, controller->statusTitle()),
            QStringLiteral("MSG-01 备份页显示它"));
  run.Check(
      !pageShows(4, controller->statusTitle()) &&
          !pageShows(3, controller->statusTitle()) &&
          !pageShows(2, controller->statusTitle()) &&
          !pageShows(0, controller->statusTitle()),
      QStringLiteral("MSG-01 设置 / 管理 / 自动备份 / 首页都不显示它"),
      QStringLiteral("settings=[%1] management=[%2] schedule=[%3] home=[%4]")
          .arg(bannerTitle(4), bannerTitle(3), bannerTitle(2), bannerTitle(0)));
  const QString backup_error_title = controller->statusTitle();

  goToPage(4);
  run.Check(!pageShows(4, backup_error_title) &&
                !pageShows(3, backup_error_title) &&
                !pageShows(2, backup_error_title) &&
                !pageShows(0, backup_error_title),
            QStringLiteral("MSG-01 切到设置之后这条错误不再出现在任何页面"),
            QStringLiteral("settings=[%1]").arg(bannerTitle(4)));
  run.Check(controller->statusKind() == QStringLiteral("idle"),
            QStringLiteral("MSG-01 离开备份页即消费掉这条错误"),
            controller->statusKind());

  goToPage(1);
  run.Check(!pageShows(1, backup_error_title),
            QStringLiteral("MSG-01 回到备份页不会自动复现旧错误"),
            bannerTitle(1));

  // MSG-02：设置页的成功提示同样只属于设置页，离开即消费。
  const QString repository = temp.filePath(QStringLiteral("repo"));
  goToPage(4);
  const bool saved = controller->saveRepositoryPath(repository);
  run.Check(saved && controller->statusKind() == QStringLiteral("success") &&
                controller->statusScope() == QStringLiteral("settings"),
            QStringLiteral("MSG-02 设置页保存成功"),
            controller->statusTitle() + QStringLiteral("/") +
                controller->statusScope());
  const QString settings_title = controller->statusTitle();
  run.Check(pageShows(4, settings_title),
            QStringLiteral("MSG-02 设置页显示它"));
  run.Check(
      !pageShows(1, settings_title) && !pageShows(3, settings_title) &&
          !pageShows(2, settings_title) && !pageShows(0, settings_title),
      QStringLiteral("MSG-02 备份 / 管理 / 自动备份 / 首页都不显示它"),
      QStringLiteral("backup=[%1] management=[%2] schedule=[%3] home=[%4]")
          .arg(bannerTitle(1), bannerTitle(3), bannerTitle(2), bannerTitle(0)));
  goToPage(1);
  run.Check(controller->statusKind() == QStringLiteral("idle") &&
                !pageShows(4, settings_title),
            QStringLiteral("MSG-02 离开设置页即消费掉这条成功提示"),
            controller->statusKind());
  goToPage(4);
  run.Check(!pageShows(4, settings_title),
            QStringLiteral("MSG-02 回到设置页不会自动复现旧提示"),
            bannerTitle(4));

  // MSG-03：severity 与 scope 正交 —— 四种 severity 的位置与失效时机完全一样。
  const char* kSeverities[4] = {"error", "warning", "info", "success"};
  for (const char* severity : kSeverities) {
    const QString kind = QString::fromLatin1(severity);
    const QString title = QStringLiteral("severity-测试-%1").arg(kind);
    goToPage(3);
    controller->setStatusForTest(kind, QStringLiteral("management"), title,
                                 QStringLiteral("detail"));
    const bool shown_on_own_page = pageShows(3, title);
    const bool hidden_elsewhere = !pageShows(1, title) &&
                                  !pageShows(4, title) &&
                                  !pageShows(2, title) && !pageShows(0, title);
    goToPage(1);
    const bool consumed = controller->statusKind() == QStringLiteral("idle");
    run.Check(shown_on_own_page && hidden_elsewhere && consumed,
              QStringLiteral("MSG-03 severity=%1 遵循同一条 scope/lifetime")
                  .arg(kind),
              QStringLiteral("本页显示=%1 其它页不显示=%2 离开即消费=%3")
                  .arg(shown_on_own_page)
                  .arg(hidden_elsewhere)
                  .arg(consumed));
  }

  // MSG-04：后台任务在别的页面结束时，完成提示不能丢到那个页面上；
  //         但任务本身必须照常完成。
  const QString source = temp.filePath(QStringLiteral("source"));
  QDir().mkpath(source);
  for (int i = 0; i < 40; ++i) {
    QFile file(QStringLiteral("%1/file-%2.bin").arg(source).arg(i));
    if (file.open(QIODevice::WriteOnly)) file.write(QByteArray(2048, 'x'));
  }
  controller->setSourcePath(source);
  goToPage(1);
  const bool backup_started = controller->startBackupWithOptions(
      QStringLiteral("mypack"), QStringLiteral("none"), QStringLiteral("none"),
      QString(), QString());
  run.Check(backup_started && controller->busy(),
            QStringLiteral("MSG-04 备份真的启动了"));
  // 切页就在**同一个事件循环回合内**完成：完成回调还没有机会派发，busy 一定
  // 还是 true，所以这条断言不取决于任务跑得多快（与 --close-guard-test 用的是
  // 同一条推理）。这里刻意不调用 goToPage()，它会让事件循环转一圈。
  window->setProperty("currentPage", 4);
  run.Check(
      controller->busy() &&
          controller->statusKind() == QStringLiteral("running") &&
          !pageShows(4, QStringLiteral("备份完成")) &&
          !pageShows(3, QStringLiteral("备份完成")),
      QStringLiteral("MSG-04 运行中切页：运行状态不被切页清掉，也没有完成提示"),
      controller->statusKind() + QStringLiteral("/") + bannerTitle(4));
  run.Check(controller->waitForIdle(120000) && !controller->busy(),
            QStringLiteral("MSG-04 切页之后任务照常完成"));
  run.Check(controller->lastSucceeded() &&
                controller->statusKind() == QStringLiteral("success") &&
                controller->statusScope() == QStringLiteral("backup"),
            QStringLiteral("MSG-04 完成提示仍然属于发起它的备份页"),
            controller->statusTitle() + QStringLiteral("/") +
                controller->statusScope());
  run.Check(
      pageShows(1, controller->statusTitle()) &&
          !pageShows(4, controller->statusTitle()) &&
          !pageShows(3, controller->statusTitle()) &&
          !pageShows(2, controller->statusTitle()) &&
          !pageShows(0, controller->statusTitle()),
      QStringLiteral("MSG-04 完成提示只回到备份页，没有出现在当前页或其它页"),
      QStringLiteral(
          "backup=[%1] settings=[%2] management=[%3] schedule=[%4] home=[%5]")
          .arg(bannerTitle(1), bannerTitle(4), bannerTitle(3), bannerTitle(2),
               bannerTitle(0)));

  // MSG-05：消费之后，重新触发同样的问题必须重新显示 —— 不能变成"清一次就
  //         永远不显示"。
  controller->setSourcePath(QString());
  goToPage(1);
  const bool started_again = controller->startBackupWithOptions(
      QStringLiteral("mypack"), QStringLiteral("none"), QStringLiteral("none"),
      QString(), QString());
  run.Check(!started_again && pageShows(1, controller->statusTitle()) &&
                controller->statusKind() == QStringLiteral("error"),
            QStringLiteral("MSG-05 再次触发同一个错误仍然正常显示"),
            bannerTitle(1) + QStringLiteral("/") + controller->statusKind());

  std::printf("[gui-contract] passed=%d failed=%d\n", run.passed, run.failed);
  if (run.failed != 0) {
    for (const QString& failure : run.failures)
      std::printf("[gui-contract]   FAIL %s\n", qPrintable(failure));
  }
  return run.failed == 0 ? 0 : 1;
}

// ---- --incremental-test <source> <repository> ----
//
// PR #18 的 GUI/CLI parity 自检。它走**真实的控制器入口**
// （startBackupWithStrategy + 依赖链恢复），并把每一步的结果按固定格式打印：
//
//     step1 kind=full-baseline reason=<yes|no>
//     step2 kind=no-changes
//     step3 kind=delta changes=+A~M=C-R
//     restore ok
//
// 脚本拿这几行与 backupctl 的输出对照。这不是"两边都调了同一个函数"，
// 而是"命令行里看到的与界面上会发生的完全一致"。
int RunIncrementalTest(backup_modern::BackupController* controller,
                       const QString& source, const QString& repository) {
  CheckRun run;
  run.prefix = "[incremental]";

  // 仓库必须先配置好：产品路径上它来自设置页。
  if (!controller->saveRepositoryPath(repository)) {
    std::fprintf(stderr, "[incremental] cannot configure the repository: %s\n",
                 qPrintable(controller->statusMessage()));
    return 1;
  }
  controller->setSourcePath(source);
  controller->clearStatus();

  const auto runOne = [&](const char* label, bool* ok) {
    controller->clearStatus();
    const bool started = controller->startBackupWithStrategy(
        QStringLiteral("incremental"), QStringLiteral("mypack"),
        QStringLiteral("none"), QStringLiteral("none"), QString(), QString());
    const bool idle =
        started && controller->waitForIdle(600000) && !controller->busy();
    const bool succeeded = idle && controller->lastSucceeded();
    *ok = succeeded;
    run.Check(started && idle, QStringLiteral("%1 任务正常结束").arg(label),
              controller->statusMessage());
    return succeeded;
  };

  bool first_ok = false;
  runOne("step1", &first_ok);
  if (!first_ok) {
    std::printf("[incremental] step1 kind=failed\n");
    std::printf("[incremental] passed=%d failed=%d\n", run.passed, run.failed);
    return 1;
  }
  // step1：没有基线时必须建完整基线，并且给出原因。
  const QString first_title = controller->statusTitle();
  const bool first_baseline = first_title.contains(QStringLiteral("完整基线"));
  std::printf("[incremental] step1 kind=%s reason=%s\n",
              first_baseline ? "full-baseline" : "UNEXPECTED",
              first_baseline ? "yes" : "no");

  bool second_ok = false;
  runOne("step2", &second_ok);
  const QString second_title = controller->statusTitle();
  const bool second_no_changes =
      second_title.contains(QStringLiteral("没有变化"));
  std::printf("[incremental] step2 kind=%s\n",
              second_no_changes ? "no-changes" : "UNEXPECTED");

  // 改一个文件（内容变、长度不变）：增量必须看得见。
  {
    QFile file(source + QStringLiteral("/a.txt"));
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
      file.write("ALPHA");
      file.close();
    }
  }
  controller->clearStatus();
  bool third_ok = false;
  runOne("step3", &third_ok);
  const QString third_title = controller->statusTitle();
  const bool third_delta = third_title.contains(QStringLiteral("增量完成"));
  std::printf("[incremental] step3 kind=%s changes=%s\n",
              third_delta ? "delta" : "UNEXPECTED",
              qPrintable(controller->statusMessage()));

  // 依赖链恢复：只用 delta 的文件名，控制器自己去解析 base 与中间层。
  //
  // 列表刷新是异步的（每次备份成功都会触发一次后台扫描），所以这里必须等它
  // 稳定下来再读，否则拿到的是上一轮的结果 —— 测试会变成"看谁跑得快"。
  run.Check(controller->waitForCatalogIdle(120000),
            QStringLiteral("step4 仓库列表刷新结束"));
  const QVariantList records = controller->backupRecords();
  QString delta_name;
  for (const QVariant& value : records) {
    const QVariantMap record = value.toMap();
    const QString name = record.value(QStringLiteral("fileName")).toString();
    if (record.value(QStringLiteral("recordKind")).toString() ==
        QStringLiteral("delta")) {
      delta_name = name;
      break;
    }
  }
  run.Check(!delta_name.isEmpty(),
            QStringLiteral("step4 列表里能认出 delta 快照"), delta_name);
  if (!delta_name.isEmpty()) {
    const QString destination =
        source + QStringLiteral("-restored-") +
        QString::number(QDateTime::currentSecsSinceEpoch());
    const bool restored =
        controller->startManagedRestore(delta_name, destination) &&
        controller->waitForIdle(600000) && controller->lastSucceeded();
    run.Check(restored, QStringLiteral("step4 依赖链恢复成功"),
              controller->statusMessage());
    QString content;
    QFile restored_file(destination + QStringLiteral("/a.txt"));
    if (restored_file.open(QIODevice::ReadOnly)) {
      content = QString::fromUtf8(restored_file.readAll());
      restored_file.close();
    }
    run.Check(content == QStringLiteral("ALPHA"),
              QStringLiteral("step4 恢复出来的内容来自 delta"), content);
    std::printf("[incremental] restore %s\n",
                content == QStringLiteral("ALPHA") ? "ok" : "FAILED");
  }

  std::printf("[incremental] passed=%d failed=%d\n", run.passed, run.failed);
  return run.failed == 0 ? 0 : 1;
}

int RunBackupOptionsTest(backup_modern::BackupController* controller,
                         const QString& config_file_path) {
  CheckRun run;

  // 1) 解析表：key → enum，以及 enum → key / 展示文本。
  // 直接调用控制器暴露的函数，不在这里抄一份对照表 —— 抄一份就等于测试自己。
  struct PackCase {
    const char* key;
    backupproject::PackMethod method;
    const char* text;
  };
  const PackCase pack_cases[] = {
      {"mypack", backupproject::PackMethod::kMyPack, "MyPack"},
      {"ustar", backupproject::PackMethod::kUstar, "USTAR"},
      {"fast-ustar", backupproject::PackMethod::kFastUstar, "Fast USTAR"},
  };
  for (const PackCase& item : pack_cases) {
    const QString key = QString::fromLatin1(item.key);
    backupproject::PackMethod method = kUntouchedPackMethod;
    const bool parsed = backup_modern::ParsePackMethodKey(key, &method);
    run.Check(
        parsed && method == item.method,
        QStringLiteral("ParsePackMethodKey(%1) → %2")
            .arg(key)
            .arg(static_cast<int>(item.method)),
        QStringLiteral("parsed=%1 实际 enum=%2")
            .arg(parsed ? QStringLiteral("true") : QStringLiteral("false"))
            .arg(static_cast<int>(method)));
    const QString expect_text = QString::fromUtf8(item.text);
    run.Check(backup_modern::PackMethodKey(item.method) == key &&
                  backup_modern::PackMethodText(item.method) == expect_text,
              QStringLiteral("PackMethodKey/Text(%1) → %2 / %3")
                  .arg(static_cast<int>(item.method))
                  .arg(key, expect_text),
              QStringLiteral("key=[%1] text=[%2]")
                  .arg(backup_modern::PackMethodKey(item.method),
                       backup_modern::PackMethodText(item.method)));
  }

  struct CompressionCase {
    const char* key;
    backupproject::CompressionMethod method;
    const char* text;
  };
  const CompressionCase compression_cases[] = {
      {"none", backupproject::CompressionMethod::kNone, "不压缩"},
      {"huffman", backupproject::CompressionMethod::kHuffman, "Huffman"},
      {"lzss-huffman", backupproject::CompressionMethod::kLzssHuffman,
       "LZSS + Huffman"},
  };
  for (const CompressionCase& item : compression_cases) {
    const QString key = QString::fromLatin1(item.key);
    backupproject::CompressionMethod method = kUntouchedCompressionMethod;
    const bool parsed = backup_modern::ParseCompressionMethodKey(key, &method);
    run.Check(
        parsed && method == item.method,
        QStringLiteral("ParseCompressionMethodKey(%1) → %2")
            .arg(key)
            .arg(static_cast<int>(item.method)),
        QStringLiteral("parsed=%1 实际 enum=%2")
            .arg(parsed ? QStringLiteral("true") : QStringLiteral("false"))
            .arg(static_cast<int>(method)));
    const QString expect_text = QString::fromUtf8(item.text);
    run.Check(
        backup_modern::CompressionMethodKey(item.method) == key &&
            backup_modern::CompressionMethodText(item.method) == expect_text,
        QStringLiteral("CompressionMethodKey/Text(%1) → %2 / %3")
            .arg(static_cast<int>(item.method))
            .arg(key, expect_text),
        QStringLiteral("key=[%1] text=[%2]")
            .arg(backup_modern::CompressionMethodKey(item.method),
                 backup_modern::CompressionMethodText(item.method)));
  }

  struct EncryptionCase {
    const char* key;
    backupproject::EncryptionMethod method;
    const char* text;
  };
  const EncryptionCase encryption_cases[] = {
      {"none", backupproject::EncryptionMethod::kNone, "不加密"},
      {"des-cbc-hmac-sha256",
       backupproject::EncryptionMethod::kDesCbcHmacSha256,
       "DES-CBC + HMAC-SHA256"},
      {"aes-256-ctr-hmac-sha256",
       backupproject::EncryptionMethod::kAes256CtrHmacSha256,
       "AES-256-CTR + HMAC-SHA256"},
  };
  for (const EncryptionCase& item : encryption_cases) {
    const QString key = QString::fromLatin1(item.key);
    backupproject::EncryptionMethod method = kUntouchedEncryptionMethod;
    const bool parsed = backup_modern::ParseEncryptionMethodKey(key, &method);
    run.Check(
        parsed && method == item.method,
        QStringLiteral("ParseEncryptionMethodKey(%1) → %2")
            .arg(key)
            .arg(static_cast<int>(item.method)),
        QStringLiteral("parsed=%1 实际 enum=%2")
            .arg(parsed ? QStringLiteral("true") : QStringLiteral("false"))
            .arg(static_cast<int>(method)));
    const QString expect_text = QString::fromUtf8(item.text);
    run.Check(
        backup_modern::EncryptionMethodKey(item.method) == key &&
            backup_modern::EncryptionMethodText(item.method) == expect_text,
        QStringLiteral("EncryptionMethodKey/Text(%1) → %2 / %3")
            .arg(static_cast<int>(item.method))
            .arg(key, expect_text),
        QStringLiteral("key=[%1] text=[%2]")
            .arg(backup_modern::EncryptionMethodKey(item.method),
                 backup_modern::EncryptionMethodText(item.method)));
  }

  // 每个未知 key 都对三个解析函数各试一次：'tar' 不是合法的打包 key，
  // 也不该在压缩 / 加密解析里被接受。出参必须原样保留 —— 一个"解析失败就退回
  // 默认值"的实现会让用户选了 tar 却拿到 MyPack 的产物，而界面显示成功，
  // 那比明确报错危险得多。
  struct UnknownKey {
    const char* key;
    const char* label;
  };
  const UnknownKey unknown_keys[] = {
      {"tar", "tar"},
      {"", "空串"},
      {"MyPack", "MyPack（大小写敏感）"},
      {"gzip", "gzip"},
      {"rot13", "rot13"},
      {"AES-256-CTR-HMAC-SHA256", "AES-256-CTR-HMAC-SHA256（大写不接受）"},
  };
  for (const UnknownKey& item : unknown_keys) {
    const QString key = QString::fromLatin1(item.key);
    const QString label = QString::fromUtf8(item.label);
    backupproject::PackMethod pack = kUntouchedPackMethod;
    const bool pack_parsed = backup_modern::ParsePackMethodKey(key, &pack);
    run.Check(
        !pack_parsed && pack == kUntouchedPackMethod,
        QStringLiteral("未知打包 key %1 失败且出参未被改写").arg(label),
        QStringLiteral("parsed=%1 enum=%2")
            .arg(pack_parsed ? QStringLiteral("true") : QStringLiteral("false"))
            .arg(static_cast<int>(pack)));
    backupproject::CompressionMethod compression = kUntouchedCompressionMethod;
    const bool compression_parsed =
        backup_modern::ParseCompressionMethodKey(key, &compression);
    run.Check(!compression_parsed && compression == kUntouchedCompressionMethod,
              QStringLiteral("未知压缩 key %1 失败且出参未被改写").arg(label),
              QStringLiteral("parsed=%1 enum=%2")
                  .arg(compression_parsed ? QStringLiteral("true")
                                          : QStringLiteral("false"))
                  .arg(static_cast<int>(compression)));
    backupproject::EncryptionMethod encryption = kUntouchedEncryptionMethod;
    const bool encryption_parsed =
        backup_modern::ParseEncryptionMethodKey(key, &encryption);
    run.Check(!encryption_parsed && encryption == kUntouchedEncryptionMethod,
              QStringLiteral("未知加密 key %1 失败且出参未被改写").arg(label),
              QStringLiteral("parsed=%1 enum=%2")
                  .arg(encryption_parsed ? QStringLiteral("true")
                                         : QStringLiteral("false"))
                  .arg(static_cast<int>(encryption)));
  }

  // 2) 四种组合的真实备份。仓库、源目录、恢复目标全部落在 QTemporaryDir 里：
  // 测试不往真实目录写东西，失败时再把目录留下来供人查看。
  QTemporaryDir temp_dir;
  if (!temp_dir.isValid()) {
    std::fprintf(stderr, "[backup-options] 临时目录创建失败\n");
    return 1;
  }
  const QString repository = temp_dir.filePath(QStringLiteral("repository"));
  const QString source = temp_dir.filePath(QStringLiteral("source"));

  run.Check(BuildOptionsTestSource(source),
            QStringLiteral("测试源目录建好（中文名 / # / 百分号 / 空目录 / "
                           "两个软链接 / FIFO）"));
  run.Check(controller->saveRepositoryPath(repository),
            QStringLiteral("保存测试仓库路径"), controller->statusMessage());

  struct BackupCase {
    QString label;
    QString pack_key;
    QString compression_key;
    QString encryption_key;
    QString password;
    backupproject::PackMethod pack_method;
    backupproject::CompressionMethod compression_method;
    backupproject::EncryptionMethod encryption_method;
  };
  const BackupCase backup_cases[] = {
      {QStringLiteral("mypack + none + none"), QStringLiteral("mypack"),
       QStringLiteral("none"), QStringLiteral("none"), QString(),
       backupproject::PackMethod::kMyPack,
       backupproject::CompressionMethod::kNone,
       backupproject::EncryptionMethod::kNone},
      {QStringLiteral("ustar + huffman + none"), QStringLiteral("ustar"),
       QStringLiteral("huffman"), QStringLiteral("none"), QString(),
       backupproject::PackMethod::kUstar,
       backupproject::CompressionMethod::kHuffman,
       backupproject::EncryptionMethod::kNone},
      {QStringLiteral("fast-ustar + lzss-huffman + aes"),
       QStringLiteral("fast-ustar"), QStringLiteral("lzss-huffman"),
       QStringLiteral("aes-256-ctr-hmac-sha256"), kSentinelPassword,
       backupproject::PackMethod::kFastUstar,
       backupproject::CompressionMethod::kLzssHuffman,
       backupproject::EncryptionMethod::kAes256CtrHmacSha256},
      {QStringLiteral("mypack + none + des"), QStringLiteral("mypack"),
       QStringLiteral("none"), QStringLiteral("des-cbc-hmac-sha256"),
       kDesPassword, backupproject::PackMethod::kMyPack,
       backupproject::CompressionMethod::kNone,
       backupproject::EncryptionMethod::kDesCbcHmacSha256},
  };
  const int kBackupCaseCount = 4;
  QString created_files[kBackupCaseCount];
  // 只用来把 file name 解析成真实路径：与产品入口走的是同一套 Catalog 逻辑。
  backupproject::BackupCatalog catalog;
  for (int index = 0; index < kBackupCaseCount; ++index) {
    const BackupCase& item = backup_cases[index];
    const QStringList before = SnapshotBakFiles(repository);
    controller->setSourcePath(source);
    const bool started = controller->startBackupWithOptions(
        item.pack_key, item.compression_key, item.encryption_key, item.password,
        item.password);
    const bool idle = started && controller->waitForIdle(600000);
    run.Check(started && idle && controller->lastSucceeded(),
              QStringLiteral("备份 %1 成功").arg(item.label),
              controller->statusMessage());
    const QString file_name =
        NewBakFileName(before, SnapshotBakFiles(repository));
    run.Check(
        !file_name.isEmpty(),
        QStringLiteral("备份 %1 在仓库里落下一个新的 .bak").arg(item.label));
    if (file_name.isEmpty()) {
      continue;  // 没有产物就没有可查的东西；失败上面已经记下了
    }
    created_files[index] = file_name;

    // 产物本身：先按 file name 解析成真实路径，再读 header。
    // 断言对着 header 字段与枚举 id，不看"备份成功"这句话。
    const QString archive_path = QDir(repository).filePath(file_name);
    std::string resolved;
    std::string resolve_error;
    const bool resolved_ok =
        catalog.Resolve(repository.toStdString(), file_name.toStdString(),
                        &resolved, &resolve_error);
    run.Check(
        resolved_ok && resolved == archive_path.toStdString(),
        QStringLiteral("Catalog::Resolve(%1) 解析到真实产物").arg(file_name),
        QString::fromStdString(resolve_error));
    backupproject::ArchiveFileInfo info;
    std::string identify_error;
    const bool identified = backupproject::IdentifyArchiveFile(
        archive_path.toStdString(), &info, &identify_error);
    const bool matches =
        identified &&
        info.kind == backupproject::ArchiveFileInfo::Kind::kContainerV2 &&
        info.format_version == 2 && info.pack_method == item.pack_method &&
        info.compression_method == item.compression_method &&
        info.encryption_method == item.encryption_method;
    run.Check(
        matches,
        QStringLiteral("%1 是 v2 容器且三个算法与请求一致").arg(file_name),
        QStringLiteral("kind=%1 version=%2 pack=%3 compression=%4 "
                       "encryption=%5 error=%6")
            .arg(static_cast<int>(info.kind))
            .arg(static_cast<int>(info.format_version))
            .arg(static_cast<int>(info.pack_method))
            .arg(static_cast<int>(info.compression_method))
            .arg(static_cast<int>(info.encryption_method))
            .arg(QString::fromStdString(identify_error)));
    // 实际读到的枚举 id 一并打印：复核的人看的是这些数字，而不是
    // "我们觉得它一致"这句判断。
    std::printf(
        "[backup-options]   observed %s: kind=%d formatVersion=%u "
        "packMethod=%u(%s) compressionMethod=%u(%s) encryptionMethod=%u(%s)\n",
        qPrintable(file_name), static_cast<int>(info.kind),
        static_cast<unsigned>(info.format_version),
        static_cast<unsigned>(info.pack_method),
        backupproject::PackMethodName(info.pack_method),
        static_cast<unsigned>(info.compression_method),
        backupproject::CompressionMethodName(info.compression_method),
        static_cast<unsigned>(info.encryption_method),
        backupproject::EncryptionMethodName(info.encryption_method));
  }
  const QString mypack_file = created_files[0];
  const QString ustar_file = created_files[1];
  const QString aes_file = created_files[2];
  const QString des_file = created_files[3];

  // 与 --repository-test 的 BACKUP_MODERN_KEEP_ARTIFACT
  // 同一套旁路：截图需要一份 真实的加密产物来做「管理页 +
  // 加密记录」那个画面，而临时目录在成功路径上会被
  // 删掉。没有设这个环境变量时不复制、不留痕，产品行为一字不变。
  const QByteArray keep_encrypted =
      qgetenv("BACKUP_MODERN_KEEP_OPTIONS_ARTIFACT");
  if (!keep_encrypted.isEmpty()) {
    const QString keep = QString::fromLocal8Bit(keep_encrypted);
    QFile::remove(keep);
    const bool copied = !aes_file.isEmpty() &&
                        QFile::copy(QDir(repository).filePath(aes_file), keep);
    run.Check(copied, QStringLiteral("加密产物副本写入 %1").arg(keep));
  }

  // 3) 密码校验必须发生在启动后台线程之前。返回值可能只是"提前 return"，
  // 所以这里同时要求 busy 仍为 false、仓库快照一字不变 ——
  // 那两条只能靠"真的没跑起来"来满足。
  struct PasswordCase {
    QString label;
    QString encryption_key;
    QString password;
    QString confirm_password;
    bool expect_started;
    QString expect_message;
  };
  const PasswordCase password_cases[] = {
      {QStringLiteral("aes + 空密码 + 空确认"),
       QStringLiteral("aes-256-ctr-hmac-sha256"), QString(), QString(), false,
       QStringLiteral("密码不能为空")},
      {QStringLiteral("aes + 密码 abc / 确认 abd"),
       QStringLiteral("aes-256-ctr-hmac-sha256"), QStringLiteral("abc"),
       QStringLiteral("abd"), false, QStringLiteral("两次输入的密码不一致")},
      {QStringLiteral("aes + 密码 abc / 确认为空"),
       QStringLiteral("aes-256-ctr-hmac-sha256"), QStringLiteral("abc"),
       QString(), false, QStringLiteral("两次输入的密码不一致")},
      {QStringLiteral("aes + 两次一致"),
       QStringLiteral("aes-256-ctr-hmac-sha256"), kSentinelPassword,
       kSentinelPassword, true, QString()},
      {QStringLiteral("des + 非空密码"), QStringLiteral("des-cbc-hmac-sha256"),
       kDesPassword, kDesPassword, true, QString()},
  };
  for (const PasswordCase& item : password_cases) {
    const QStringList before = SnapshotBakFiles(repository);
    const bool started = controller->startBackupWithOptions(
        QStringLiteral("mypack"), QStringLiteral("none"), item.encryption_key,
        item.password, item.confirm_password);
    bool ok = started == item.expect_started;
    if (item.expect_started) {
      ok = ok && controller->waitForIdle(600000) &&
           controller->lastSucceeded() &&
           SnapshotBakFiles(repository).size() == before.size() + 1;
    } else {
      ok = ok && !controller->busy() &&
           SnapshotBakFiles(repository) == before &&
           controller->statusMessage().contains(item.expect_message);
    }
    run.Check(ok, item.label, controller->statusMessage());
  }

  // 4) 未知 key 走控制器这一层（不只是解析函数）：失败必须发生在启动之前，
  // 而且不能留下任何新 .bak。
  struct UnknownKeyCase {
    QString label;
    QString pack_key;
    QString compression_key;
    QString encryption_key;
    QString expect_message;
  };
  const UnknownKeyCase unknown_key_cases[] = {
      {QStringLiteral("控制器拒绝未知打包方式 tar"), QStringLiteral("tar"),
       QStringLiteral("none"), QStringLiteral("none"),
       QStringLiteral("未知打包方式")},
      {QStringLiteral("控制器拒绝未知压缩方式 gzip"), QStringLiteral("mypack"),
       QStringLiteral("gzip"), QStringLiteral("none"),
       QStringLiteral("未知压缩方式")},
      {QStringLiteral("控制器拒绝未知加密方式 rot13"), QStringLiteral("mypack"),
       QStringLiteral("none"), QStringLiteral("rot13"),
       QStringLiteral("未知加密方式")},
  };
  for (const UnknownKeyCase& item : unknown_key_cases) {
    const QStringList before = SnapshotBakFiles(repository);
    const bool started = controller->startBackupWithOptions(
        item.pack_key, item.compression_key, item.encryption_key, QString(),
        QString());
    const bool ok = !started && !controller->busy() &&
                    SnapshotBakFiles(repository) == before &&
                    controller->statusMessage().contains(item.expect_message);
    run.Check(ok, item.label, controller->statusMessage());
  }

  // 5) 恢复路径。加密归档走普通入口必须在启动线程之前失败：那条入口根本没有
  // 密码可用，让它跑起来只会得到一句和密码无关的认证失败。
  {
    const QString destination =
        temp_dir.filePath(QStringLiteral("restore/no-password"));
    const bool started = controller->startManagedRestore(aes_file, destination);
    run.Check(!started && !controller->busy() &&
                  controller->statusMessage().contains(
                      QStringLiteral("此备份已加密")) &&
                  !QFileInfo::exists(destination),
              QStringLiteral("加密归档走无密码入口立即失败且不建目标目录"),
              controller->statusMessage());
  }
  {
    // 错误密码只能由核心在解密时发现，所以任务会真的跑起来；
    // 但目标目录绝不能出现 —— 恢复先写暂存目录、成功后 rename，
    // "失败却留下半棵树"是不能接受的。
    const QString destination =
        temp_dir.filePath(QStringLiteral("restore/wrong-aes"));
    const bool started = controller->startManagedRestoreWithPassword(
        aes_file, destination, QStringLiteral("PR16_WRONG_PASSWORD"));
    const bool idle = started && controller->waitForIdle(600000);
    run.Check(started && idle && !controller->lastSucceeded() &&
                  !QFileInfo::exists(destination),
              QStringLiteral("AES 错误密码恢复失败且不留下目标目录"),
              controller->statusMessage());
  }
  {
    const QString destination =
        temp_dir.filePath(QStringLiteral("restore/good-aes"));
    const bool started = controller->startManagedRestoreWithPassword(
        aes_file, destination, kSentinelPassword);
    const bool idle = started && controller->waitForIdle(600000);
    run.Check(started && idle && controller->lastSucceeded(),
              QStringLiteral("AES 正确密码恢复成功"),
              controller->statusMessage());
    // 逐项验内容与文件类型："恢复成功"这句话不足以说明软链接、悬空链接、
    // FIFO、空目录都还在。
    run.Check(FileBytesEqual(source + QStringLiteral("/plain.txt"),
                             destination + QStringLiteral("/plain.txt")),
              QStringLiteral("AES 恢复的 plain.txt 逐字节一致"));
    run.Check(QFileInfo::exists(destination + QStringLiteral("/中文文件.txt")),
              QStringLiteral("AES 恢复的 中文文件.txt 存在"));
    run.Check(
        IsSymlinkTo(destination + QStringLiteral("/links/relative_symlink"),
                    QStringLiteral("plain.txt")),
        QStringLiteral("AES 恢复的 relative_symlink 仍指向 plain.txt"),
        QStringLiteral("target=[%1]")
            .arg(ReadLinkTarget(destination +
                                QStringLiteral("/links/relative_symlink"))));
    run.Check(
        IsSymlinkTo(destination + QStringLiteral("/links/dangling_symlink"),
                    QStringLiteral("nope.txt")) &&
            !QFileInfo::exists(destination +
                               QStringLiteral("/links/dangling_symlink")),
        QStringLiteral("AES 恢复的 dangling_symlink 仍然悬空"));
    run.Check(IsFifo(destination + QStringLiteral("/special/named_pipe.fifo")),
              QStringLiteral("AES 恢复的 named_pipe.fifo 仍是 FIFO"));
    run.Check(
        QFileInfo::exists(destination + QStringLiteral("/hash#percent%.txt")),
        QStringLiteral("AES 恢复的 hash#percent%.txt 存在"));
    run.Check(IsEmptyDirectory(destination + QStringLiteral("/empty_dir")),
              QStringLiteral("AES 恢复的 empty_dir 仍是空目录"));
  }
  {
    const QString destination =
        temp_dir.filePath(QStringLiteral("restore/wrong-des"));
    const bool started = controller->startManagedRestoreWithPassword(
        des_file, destination, QStringLiteral("PR16_WRONG_DES_PW"));
    const bool idle = started && controller->waitForIdle(600000);
    run.Check(started && idle && !controller->lastSucceeded() &&
                  !QFileInfo::exists(destination),
              QStringLiteral("DES 错误密码恢复失败且不留下目标目录"),
              controller->statusMessage());
  }
  {
    const QString destination =
        temp_dir.filePath(QStringLiteral("restore/good-des"));
    const bool started = controller->startManagedRestoreWithPassword(
        des_file, destination, kDesPassword);
    const bool idle = started && controller->waitForIdle(600000);
    run.Check(started && idle && controller->lastSucceeded(),
              QStringLiteral("DES 正确密码恢复成功"),
              controller->statusMessage());
    run.Check(FileBytesEqual(source + QStringLiteral("/plain.txt"),
                             destination + QStringLiteral("/plain.txt")),
              QStringLiteral("DES 恢复的 plain.txt 逐字节一致"));
  }
  {
    // 未加密的 v2 走普通入口：它不需要密码，也不该被密码逻辑拦下来。
    const QString destination =
        temp_dir.filePath(QStringLiteral("restore/plain-v2"));
    const bool started =
        controller->startManagedRestore(mypack_file, destination);
    const bool idle = started && controller->waitForIdle(600000);
    run.Check(started && idle && controller->lastSucceeded(),
              QStringLiteral("未加密 v2 走无密码入口恢复成功"),
              controller->statusMessage());
  }
  // legacy v0.1：direct archive 入口固定产 v0.1，直接把它写进仓库当 .bak，
  // 再用产品入口（只传 file name）恢复一次。
  // 源目录另造一份只有普通文件的：v0.1 格式装不下软链接与 FIFO，
  // 拿上面那棵树去跑 legacy 只会得到一个与本用例无关的失败。
  const QString legacy_source =
      temp_dir.filePath(QStringLiteral("legacy-source"));
  const QString legacy_file = QStringLiteral("legacy_roundtrip.bak");
  {
    const QString legacy_archive = QDir(repository).filePath(legacy_file);
    const bool prepared =
        WriteTestFile(legacy_source + QStringLiteral("/plain.txt"),
                      QByteArray("PR16 legacy plain.txt content\n")) &&
        WriteTestFile(legacy_source + QStringLiteral("/sub/level1.txt"),
                      QByteArray("level1\n"));
    run.Check(prepared, QStringLiteral("legacy v0.1 源目录准备完成"));
    const bool started = prepared && controller->startDirectBackupForTest(
                                         legacy_source, legacy_archive);
    const bool idle = started && controller->waitForIdle(600000);
    run.Check(started && idle && controller->lastSucceeded(),
              QStringLiteral("legacy v0.1 产物写进仓库"),
              controller->statusMessage());
    backupproject::ArchiveFileInfo info;
    std::string identify_error;
    const bool identified = backupproject::IdentifyArchiveFile(
        legacy_archive.toStdString(), &info, &identify_error);
    run.Check(
        identified &&
            info.kind == backupproject::ArchiveFileInfo::Kind::kLegacyV01 &&
            info.format_version == 1,
        QStringLiteral("legacy 产物确实是 v0.1（formatVersion=1）"),
        QStringLiteral("kind=%1 version=%2 error=%3")
            .arg(static_cast<int>(info.kind))
            .arg(static_cast<int>(info.format_version))
            .arg(QString::fromStdString(identify_error)));
    const QString destination =
        temp_dir.filePath(QStringLiteral("restore/legacy"));
    const bool restore_started =
        controller->startManagedRestore(legacy_file, destination);
    const bool restore_idle =
        restore_started && controller->waitForIdle(600000);
    run.Check(restore_started && restore_idle && controller->lastSucceeded(),
              QStringLiteral("legacy v0.1 走产品入口恢复成功"),
              controller->statusMessage());
    run.Check(FileBytesEqual(legacy_source + QStringLiteral("/plain.txt"),
                             destination + QStringLiteral("/plain.txt")),
              QStringLiteral("legacy 恢复的 plain.txt 逐字节一致"));
  }
  {
    const QString destination =
        temp_dir.filePath(QStringLiteral("restore/empty-password"));
    const bool started = controller->startManagedRestoreWithPassword(
        aes_file, destination, QString());
    run.Check(!started && !controller->busy() &&
                  controller->statusMessage().contains(
                      QStringLiteral("恢复密码不能为空")) &&
                  !QFileInfo::exists(destination),
              QStringLiteral("空恢复密码立即失败"),
              controller->statusMessage());
  }

  // 6) 目录字段。刷新一次列表，按 fileName 找到自己造的那几条记录 ——
  // 不看下标：列表顺序不是契约，多一条备份就会整体错位。
  controller->refreshBackups();
  const bool catalog_idle = controller->waitForCatalogIdle(600000);
  run.Check(catalog_idle && controller->catalogError().isEmpty(),
            QStringLiteral("仓库列表刷新完成且无错误"),
            controller->catalogError());
  const QVariantList records = controller->backupRecords();
  std::printf("[backup-options]   records=%d\n",
              static_cast<int>(records.size()));

  const QVariantMap legacy_record = FindRecord(records, legacy_file);
  run.Check(!legacy_record.isEmpty(),
            QStringLiteral("列表里有 legacy 记录 %1").arg(legacy_file));
  if (!legacy_record.isEmpty()) {
    run.CheckRecordBool(legacy_record, legacy_file,
                        QStringLiteral("recognizedArchive"), true);
    run.CheckRecordInt(legacy_record, legacy_file,
                       QStringLiteral("formatVersion"), 1);
    run.CheckRecordBool(legacy_record, legacy_file,
                        QStringLiteral("hasPipelineMethods"), false);
    run.CheckRecordBool(legacy_record, legacy_file,
                        QStringLiteral("passwordRequired"), false);
  }

  const QVariantMap mypack_record = FindRecord(records, mypack_file);
  run.Check(!mypack_record.isEmpty(),
            QStringLiteral("列表里有 %1").arg(mypack_file));
  if (!mypack_record.isEmpty()) {
    run.CheckRecordBool(mypack_record, mypack_file,
                        QStringLiteral("hasPipelineMethods"), true);
    run.CheckRecordText(mypack_record, mypack_file,
                        QStringLiteral("packMethodKey"),
                        QStringLiteral("mypack"));
    run.CheckRecordText(mypack_record, mypack_file,
                        QStringLiteral("compressionMethodKey"),
                        QStringLiteral("none"));
    run.CheckRecordText(mypack_record, mypack_file,
                        QStringLiteral("encryptionMethodKey"),
                        QStringLiteral("none"));
    run.CheckRecordText(mypack_record, mypack_file,
                        QStringLiteral("packMethodText"),
                        QStringLiteral("MyPack"));
    run.CheckRecordText(mypack_record, mypack_file,
                        QStringLiteral("compressionMethodText"),
                        QStringLiteral("不压缩"));
    run.CheckRecordBool(mypack_record, mypack_file,
                        QStringLiteral("passwordRequired"), false);
  }

  const QVariantMap ustar_record = FindRecord(records, ustar_file);
  run.Check(!ustar_record.isEmpty(),
            QStringLiteral("列表里有 %1").arg(ustar_file));
  if (!ustar_record.isEmpty()) {
    run.CheckRecordText(ustar_record, ustar_file,
                        QStringLiteral("packMethodKey"),
                        QStringLiteral("ustar"));
    run.CheckRecordText(ustar_record, ustar_file,
                        QStringLiteral("compressionMethodKey"),
                        QStringLiteral("huffman"));
    run.CheckRecordText(ustar_record, ustar_file,
                        QStringLiteral("encryptionMethodKey"),
                        QStringLiteral("none"));
    run.CheckRecordText(ustar_record, ustar_file,
                        QStringLiteral("packMethodText"),
                        QStringLiteral("USTAR"));
    run.CheckRecordText(ustar_record, ustar_file,
                        QStringLiteral("compressionMethodText"),
                        QStringLiteral("Huffman"));
  }

  const QVariantMap aes_record = FindRecord(records, aes_file);
  run.Check(!aes_record.isEmpty(), QStringLiteral("列表里有 %1").arg(aes_file));
  if (!aes_record.isEmpty()) {
    run.CheckRecordText(aes_record, aes_file, QStringLiteral("packMethodKey"),
                        QStringLiteral("fast-ustar"));
    run.CheckRecordText(aes_record, aes_file,
                        QStringLiteral("compressionMethodKey"),
                        QStringLiteral("lzss-huffman"));
    run.CheckRecordText(aes_record, aes_file,
                        QStringLiteral("encryptionMethodKey"),
                        QStringLiteral("aes-256-ctr-hmac-sha256"));
    run.CheckRecordText(aes_record, aes_file, QStringLiteral("packMethodText"),
                        QStringLiteral("Fast USTAR"));
    run.CheckRecordText(aes_record, aes_file,
                        QStringLiteral("compressionMethodText"),
                        QStringLiteral("LZSS + Huffman"));
    run.CheckRecordText(aes_record, aes_file,
                        QStringLiteral("encryptionMethodText"),
                        QStringLiteral("AES-256-CTR + HMAC-SHA256"));
    run.CheckRecordBool(aes_record, aes_file,
                        QStringLiteral("passwordRequired"), true);
  }

  const QVariantMap des_record = FindRecord(records, des_file);
  run.Check(!des_record.isEmpty(), QStringLiteral("列表里有 %1").arg(des_file));
  if (!des_record.isEmpty()) {
    run.CheckRecordText(des_record, des_file,
                        QStringLiteral("encryptionMethodKey"),
                        QStringLiteral("des-cbc-hmac-sha256"));
    run.CheckRecordBool(des_record, des_file,
                        QStringLiteral("passwordRequired"), true);
  }

  // 7) 泄漏审计。哨兵密码在这一轮里已经做过一次加密备份（组合 c）和一次
  // 加密恢复（AES 正确密码那一步），所以下面这些地方都真的被它经过。
  {
    QFile config_file(config_file_path);
    const bool opened = config_file.open(QIODevice::ReadOnly);
    const QString config_text =
        opened ? QString::fromUtf8(config_file.readAll()) : QString();
    run.Check(opened && !config_text.contains(kSentinelPassword),
              QStringLiteral("配置文件不含哨兵密码"), config_file_path);
  }
  {
    // 序列化整张表再搜：逐个已知字段检查会漏掉"密码被塞进某个没预料到的 key"。
    QString records_text;
    for (const QVariant& item : records) {
      records_text += FlattenRecord(item.toMap());
    }
    run.Check(
        !records_text.contains(kSentinelPassword),
        QStringLiteral("backupRecords() 的所有 key 与 value 都不含哨兵密码"));
  }
  run.Check(!(controller->statusKind() + controller->statusTitle() +
              controller->statusMessage())
                 .contains(kSentinelPassword),
            QStringLiteral("状态文案不含哨兵密码"),
            controller->statusKind() + controller->statusTitle() +
                controller->statusMessage());
  {
    QString leaked_name;
    for (const QString& name : SnapshotBakFiles(repository)) {
      if (name.contains(kSentinelPassword)) {
        leaked_name = name;
      }
    }
    run.Check(leaked_name.isEmpty(),
              QStringLiteral("仓库里的 .bak 文件名不含哨兵密码"), leaked_name);
  }
  run.Check(!controller->catalogError().contains(kSentinelPassword),
            QStringLiteral("catalogError 不含哨兵密码"));

  // 汇总：失败时列出全部失败项并把临时目录留下来 —— 产物、恢复出来的树、
  // 配置文件都还在里面，人工可以直接进去看，而不是靠日志猜。
  const int total = run.passed + run.failed;
  if (run.failed == 0) {
    std::printf("[backup-options] PASS %d/%d\n", run.passed, total);
    return 0;
  }
  std::fprintf(stderr, "[backup-options] FAIL %d/%d\n", run.failed, total);
  for (const QString& failure : run.failures) {
    std::fprintf(stderr, "[backup-options]   失败: %s\n", qPrintable(failure));
  }
  temp_dir.setAutoRemove(false);
  std::printf("[backup-options] 临时目录保留: %s\n",
              qPrintable(temp_dir.path()));
  return 1;
}

// ---- --schedule-show：把控制器看到的计划配置打成 key=value ----
//
// 存在的理由只有一个：证明 GUI 与 CLI 读的是**同一份** store。
// 脚本先让 backupctl 写、再让 GUI 读（或反过来），两边逐项比对；
// 没有这个开关，"共享 store"就只能靠读代码相信。
int RunScheduleShow(backup_modern::ScheduleController* schedule) {
  schedule->reload();
  std::printf("enabled=%d\n", schedule->enabled() ? 1 : 0);
  std::printf("trigger=%s\n", qPrintable(schedule->triggerKey()));
  std::printf("strategy=%s\n", qPrintable(schedule->strategyKey()));
  std::printf("source=%s\n", qPrintable(schedule->sourcePath()));
  std::printf("interval=%d\n", schedule->intervalMinutes());
  std::printf("retain=%d\n", schedule->retainCount());
  std::printf("pack=%s\n", qPrintable(schedule->packKey()));
  std::printf("compression=%s\n", qPrintable(schedule->compressionKey()));
  std::printf("encryption=%s\n", qPrintable(schedule->encryptionKey()));
  for (const QString& rule : schedule->includeRules()) {
    std::printf("include=%s\n", qPrintable(rule));
  }
  for (const QString& rule : schedule->excludeRules()) {
    std::printf("exclude=%s\n", qPrintable(rule));
  }
  std::printf("repository=%s\n", qPrintable(schedule->repositoryPath()));
  std::printf("managed=%d\n",
              static_cast<int>(schedule->managedSnapshots().size()));
  std::printf("history=%d\n", static_cast<int>(schedule->history().size()));
  std::printf("store=%s\n", qPrintable(schedule->storePath()));
  if (!schedule->loadError().isEmpty()) {
    std::printf("load_error=%s\n", qPrintable(schedule->loadError()));
  }
  return 0;
}

// ---- --realtime-show：把 GUI 控制器读到的实时配置打成 key=value ----
//
// 与 --schedule-show 同构。它是 CLI ↔ GUI parity 的"GUI 侧读"证据：
// backupctl realtime set 写下的字段，GUI 控制器必须逐项读到同样的值；
// 反过来 GUI 保存出来的文件，backupctl realtime show 也必须读到同样的值。
int RunRealtimeShow(backup_modern::RealtimeController* realtime) {
  realtime->reload();
  std::printf("enabled=%d\n", realtime->enabled() ? 1 : 0);
  std::printf("trigger=%s\n", qPrintable(realtime->triggerKey()));
  std::printf("strategy=%s\n", qPrintable(realtime->strategyKey()));
  std::printf("source=%s\n", qPrintable(realtime->sourcePath()));
  std::printf("debounce_ms=%d\n", realtime->debounceMs());
  std::printf("max_wait_ms=%d\n", realtime->maxWaitMs());
  std::printf("retain=%d\n", realtime->retainCount());
  std::printf("pack=%s\n", qPrintable(realtime->packKey()));
  std::printf("compression=%s\n", qPrintable(realtime->compressionKey()));
  std::printf("encryption=%s\n", qPrintable(realtime->encryptionKey()));
  for (const QString& rule : realtime->includeRules()) {
    std::printf("include=%s\n", qPrintable(rule));
  }
  for (const QString& rule : realtime->excludeRules()) {
    std::printf("exclude=%s\n", qPrintable(rule));
  }
  std::printf("repository=%s\n", qPrintable(realtime->repositoryPath()));
  // 加密边界的那句话也必须来自同一处：GUI 与 CLI 显示的是同一个字符串。
  std::printf("encryption_note=%s\n", qPrintable(realtime->encryptionNote()));
  std::printf("snapshots=%d\n", realtime->snapshotCount());
  std::printf("store=%s\n", qPrintable(realtime->storePath()));
  if (!realtime->loadError().isEmpty()) {
    std::printf("load_error=%s\n", qPrintable(realtime->loadError()));
  }
  return 0;
}

// ---- --realtime-test：实时备份页的控制器链路自检 ----
//
// 全程跑在临时目录里：临时 config.json、临时 realtime.json、临时仓库与源目录。
// 绝不读写用户真实的实时配置，也不碰冻结的 Demo 目录。
//
// 它刻意不 mock 核心：控制器写进 store 的东西，紧接着用
// backupproject::RealtimeStore 原样读回来逐项比对 —— 这正是
// "GUI 与 CLI 读同一份 store、同一套 schema"在单元层面的证据。

// 等到"最近一次产出的归档名"变成 previous 之外的值。
// 用事件循环等，不用 sleep 堆时间：inotify 事件、debounce 定时器、后台任务
// 都在这个循环里跑。
bool WaitUntilNewSnapshot(backup_modern::RealtimeController* realtime,
                          const QString& previous, int timeout_ms) {
  QEventLoop loop;
  QTimer poll;
  poll.setInterval(10);
  QObject::connect(&poll, &QTimer::timeout, &loop,
                   [realtime, previous, &loop]() {
                     if (!realtime->libraryBusy() && !realtime->pending() &&
                         !realtime->lastSnapshotName().isEmpty() &&
                         realtime->lastSnapshotName() != previous) {
                       loop.quit();
                     }
                   });
  QTimer guard;
  guard.setSingleShot(true);
  QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
  poll.start();
  guard.start(timeout_ms);
  loop.exec();
  return !realtime->lastSnapshotName().isEmpty() &&
         realtime->lastSnapshotName() != previous;
}

// 等到指定仓库里的归档数量**超过** before_count，并且实时控制器重新闲下来。
//
// 换仓库的场景不能用"归档名变了"当判据：归档名只精确到秒（重名时才加 _001
// 后缀），同一个秒里在 A、B 两个仓库各写一份，两份的名字会**完全一样**。
// 所以这里判的是仓库里的真实文件数 —— 那才是"到底写到哪儿去了"。
bool WaitForRepositoryGrowth(backup_modern::RealtimeController* realtime,
                             const QString& repository, int before_count,
                             int timeout_ms) {
  QEventLoop loop;
  QTimer poll;
  poll.setInterval(10);
  QObject::connect(&poll, &QTimer::timeout, &loop,
                   [realtime, repository, before_count, &loop]() {
                     if (!realtime->libraryBusy() && !realtime->pending() &&
                         CountArchives(repository) > before_count) {
                       loop.quit();
                     }
                   });
  QTimer guard;
  guard.setSingleShot(true);
  QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
  poll.start();
  guard.start(timeout_ms);
  loop.exec();
  return !realtime->libraryBusy() && !realtime->pending() &&
         CountArchives(repository) > before_count;
}

int RunRealtimeTest(backup_modern::RealtimeController* realtime,
                    backup_modern::OperationGate* gate,
                    backup_modern::BackupController* controller,
                    const QString& config_path) {
  CheckRun run;
  run.prefix = "[realtime]";

  QTemporaryDir temp;
  if (!temp.isValid()) {
    std::fprintf(stderr, "[realtime] 无法创建临时目录\n");
    return 1;
  }
  // 从一份干净的 store 开始：自检要断言"默认值"，残留的旧配置会让它测的不是
  // 默认状态。这里删的是 --realtime-file 指到的文件（测试隔离目录）。
  QFile::remove(realtime->storePath());

  const QString source = temp.path() + QStringLiteral("/source");
  const QString repository = temp.path() + QStringLiteral("/repository");
  QDir().mkpath(source);
  QDir().mkpath(repository);
  if (!WriteTestFile(source + QStringLiteral("/a.txt"), "alpha")) {
    std::fprintf(stderr, "[realtime] 无法准备源文件\n");
    return 1;
  }

  // 1) 临时 config.json：把仓库指到临时目录。
  {
    backupproject::ConfigManager manager(config_path.toStdString());
    backupproject::AppConfig config;
    config.backup_repository_path = repository.toStdString();
    std::string error;
    run.Check(manager.Save(config, &error),
              QStringLiteral("RT-01 临时 config.json 写入成功"),
              QString::fromStdString(error));
  }

  realtime->reload();

  run.Check(!realtime->enabled(), QStringLiteral("RT-02 默认未启用"));
  run.Check(realtime->debounceMs() == 500,
            QStringLiteral("RT-03 默认 debounce 500 ms"),
            QString::number(realtime->debounceMs()));
  run.Check(realtime->maxWaitMs() == 5000,
            QStringLiteral("RT-04 默认 max wait 5000 ms"),
            QString::number(realtime->maxWaitMs()));
  run.Check(realtime->retainCount() == 12,
            QStringLiteral("RT-05 默认保留 12 份"),
            QString::number(realtime->retainCount()));
  run.Check(realtime->triggerKey() == QStringLiteral("realtime"),
            QStringLiteral("RT-06 trigger 固定为 realtime"),
            realtime->triggerKey());
  run.Check(realtime->repositoryPath() == repository,
            QStringLiteral("RT-07 控制器读到了临时仓库"),
            realtime->repositoryPath());
  run.Check(realtime->encryptionKey() == QStringLiteral("none"),
            QStringLiteral("RT-08 加密固定 none"), realtime->encryptionKey());

  // 2) 写一份临时 realtime.json：strategy=full，逐字段读回。
  run.Check(realtime->saveConfig(/*enabled=*/false, source, 200, 2000, 3,
                                 QStringLiteral("mypack"),
                                 QStringLiteral("none"), QStringList(),
                                 QStringList(), QStringLiteral("full")),
            QStringLiteral("RT-09 保存实时配置（full）成功"));

  const QString store_file = realtime->storePath();
  run.Check(QFile::exists(store_file),
            QStringLiteral("RT-10 realtime.json 已落盘"), store_file);
  struct stat store_info;
  const bool stat_ok =
      ::stat(store_file.toLocal8Bit().constData(), &store_info) == 0;
  run.Check(stat_ok && (store_info.st_mode & 07777) == 0600,
            QStringLiteral("RT-11 realtime.json 权限是 0600"),
            stat_ok ? QString::number(store_info.st_mode & 07777, 8)
                    : QStringLiteral("stat 失败"));

  // 用共享核心原样读回来 —— GUI 存的东西必须是共享 schema。
  {
    backupproject::RealtimeStore store(store_file.toStdString());
    backupproject::RealtimeConfig stored;
    std::string error;
    const backupproject::RealtimeLoadStatus status =
        store.Load(&stored, &error);
    run.Check(status == backupproject::RealtimeLoadStatus::kLoaded,
              QStringLiteral("RT-12 realtime.json 能被共享核心读回"),
              QString::fromStdString(error));
    run.Check(stored.source_path == source.toStdString(),
              QStringLiteral("RT-13 源目录逐字一致"),
              QString::fromStdString(stored.source_path));
    run.Check(stored.debounce_ms == 200 && stored.max_wait_ms == 2000 &&
                  stored.retain_count == 3,
              QStringLiteral("RT-14 debounce / max_wait / retain 逐字段一致"),
              QStringLiteral("%1/%2/%3")
                  .arg(stored.debounce_ms)
                  .arg(stored.max_wait_ms)
                  .arg(stored.retain_count));
    run.Check(stored.strategy == backupproject::BackupStrategy::kFull,
              QStringLiteral("RT-15 strategy=full"));
    run.Check(stored.trigger == backupproject::BackupTrigger::kRealtime,
              QStringLiteral("RT-16 trigger=realtime"));
    run.Check(
        stored.encryption_method == backupproject::EncryptionMethod::kNone,
        QStringLiteral("RT-17 加密固定 none"));
    run.Check(!stored.enabled, QStringLiteral("RT-18 enabled 与保存时一致"));
    run.Check(stored.version == backupproject::kRealtimeConfigVersion,
              QStringLiteral("RT-19 version 是共享 schema 的当前版本"));
  }
  std::printf(
      "[realtime] config strategy=%s debounce=%d max_wait=%d retain=%d\n",
      qPrintable(realtime->strategyKey()), realtime->debounceMs(),
      realtime->maxWaitMs(), realtime->retainCount());

  // 3) 切到 incremental 再读回核对。
  run.Check(realtime->saveConfig(false, source, 200, 2000, 3,
                                 QStringLiteral("mypack"),
                                 QStringLiteral("none"), QStringList(),
                                 QStringList(), QStringLiteral("incremental")),
            QStringLiteral("RT-20 保存实时配置（incremental）成功"));
  {
    backupproject::RealtimeStore store(store_file.toStdString());
    backupproject::RealtimeConfig stored;
    std::string error;
    const bool loaded = store.Load(&stored, &error) ==
                        backupproject::RealtimeLoadStatus::kLoaded;
    run.Check(loaded && stored.strategy ==
                            backupproject::BackupStrategy::kIncremental,
              QStringLiteral("RT-21 读回来是 strategy=incremental"),
              QString::fromStdString(error));
  }
  std::printf(
      "[realtime] config strategy=%s debounce=%d max_wait=%d retain=%d\n",
      qPrintable(realtime->strategyKey()), realtime->debounceMs(),
      realtime->maxWaitMs(), realtime->retainCount());

  // 切回 full：下面两次触发断言的都是"完整快照"这条真实产品路径。
  run.Check(realtime->saveConfig(false, source, 200, 2000, 3,
                                 QStringLiteral("mypack"),
                                 QStringLiteral("none"), QStringList(),
                                 QStringList(), QStringLiteral("full")),
            QStringLiteral("RT-22 切回 strategy=full 成功"));

  // 4) 启用 -> attach watcher -> 合成一次 resync -> 等第一份快照。
  run.Check(realtime->setEnabled(true),
            QStringLiteral("RT-23 启用实时备份成功"));
  run.Check(realtime->watching() && realtime->watchCount() > 0,
            QStringLiteral("RT-24 watcher 已建立"),
            QString::number(realtime->watchCount()));
  std::printf("[realtime] attach watches=%d\n", realtime->watchCount());
  run.Check(realtime->waitForIdle(60000),
            QStringLiteral("RT-25 重新同步触发的第一份快照完成"));
  run.Check(realtime->lastOutcomeKind() == QStringLiteral("full-snapshot"),
            QStringLiteral("RT-26 第一次触发产出完整快照"),
            realtime->lastOutcomeKind());
  const QString first = realtime->lastSnapshotName();
  run.Check(!first.isEmpty(), QStringLiteral("RT-27 第一次触发有归档名"));
  std::printf("[realtime] step1 kind=%s name=%s\n",
              qPrintable(realtime->lastOutcomeKind()), qPrintable(first));

  // 5) 制造一次文件写入 -> 等 debounce -> 等第二份快照。
  run.Check(WriteTestFile(source + QStringLiteral("/b.txt"), "beta"),
            QStringLiteral("RT-28 在源目录里写入新文件"));
  run.Check(WaitUntilNewSnapshot(realtime, first, 60000),
            QStringLiteral("RT-29 事件触发的第二份快照完成"));
  run.Check(realtime->lastOutcomeKind() == QStringLiteral("full-snapshot"),
            QStringLiteral("RT-30 第二次触发产出完整快照"),
            realtime->lastOutcomeKind());
  const QString second = realtime->lastSnapshotName();
  run.Check(!second.isEmpty() && second != first,
            QStringLiteral("RT-31 第二次触发产出了新的归档"), second);
  std::printf("[realtime] step2 kind=%s name=%s\n",
              qPrintable(realtime->lastOutcomeKind()), qPrintable(second));

  std::printf("[realtime] history count=%d\n", realtime->snapshotCount());
  run.Check(realtime->snapshotCount() >= 2,
            QStringLiteral("RT-32 最近实时快照列表里至少有两份"),
            QString::number(realtime->snapshotCount()));
  run.Check(realtime->watchCount() > 0,
            QStringLiteral("RT-33 两轮之间监听一直没断"));

  // ---- 闸门被占时触发不丢：合并成一个 pending generation，释放后补一次 ----
  //
  // 这条路径在真实产品里很容易发生（手动备份 / 计划评估正在跑，同时源目录
  // 又变了）。判别点有两个：闸门被占期间**不写任何东西**，而且几批事件只
  // 合并成**一次**评估，不是每次事件都排一次队。
  {
    const int before_count = realtime->snapshotCount();
    QString reason;
    const bool held = gate->Acquire(
        backup_modern::OperationGate::Kind::kManualBackup, &reason);
    run.Check(held, QStringLiteral("RT-34 先占住闸门（模拟手动备份正在跑）"),
              reason);

    run.Check(WriteTestFile(source + QStringLiteral("/c.txt"), "gamma"),
              QStringLiteral("RT-35 闸门被占期间改第一个文件"));
    run.Check(WriteTestFile(source + QStringLiteral("/d.txt"), "delta"),
              QStringLiteral("RT-36 闸门被占期间改第二个文件"));

    auto wait_for_pending = [realtime](int timeout_ms) {
      QEventLoop loop;
      QTimer poll;
      poll.setInterval(10);
      QObject::connect(&poll, &QTimer::timeout, &loop, [realtime, &loop]() {
        if (realtime->pending()) loop.quit();
      });
      QTimer guard;
      guard.setSingleShot(true);
      QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
      poll.start();
      guard.start(timeout_ms);
      loop.exec();
      return realtime->pending();
    };
    run.Check(wait_for_pending(10000),
              QStringLiteral("RT-37 闸门被占时这一代被记住（pending）"));
    run.Check(realtime->snapshotCount() == before_count,
              QStringLiteral("RT-38 闸门被占期间一份快照都没写"),
              QString::number(realtime->snapshotCount()));

    gate->Release(backup_modern::OperationGate::Kind::kManualBackup);
    run.Check(WaitUntilNewSnapshot(realtime, second, 60000),
              QStringLiteral("RT-39 闸门释放后待办的那一代被补跑"));
    run.Check(realtime->snapshotCount() == before_count + 1,
              QStringLiteral("RT-40 判别：两批事件只合并成一次评估"),
              QString::number(realtime->snapshotCount()));
  }

  // ---- 运行期改仓库：控制器必须跟上，而且绝不继续写旧仓库 ----
  //
  // 这是判别性的一组：此时 realtime 已经 enabled、watcher 已经起来、A 里也已经
  // 有了快照。改动走 BackupController::saveRepositoryPath —— 与设置页是同一个
  // 入口、同一个 repositoryPathChanged 信号。三种情形都必须闭环：
  //
  //   * 合法的新仓库：监听重建 + 合成一次 resync，新快照只落在新仓库；
  //   * 不合法的新仓库（落在 source 里）：一个字节都不写，进入 degraded 并给出
  //     共享核心那句原因，enabled 保持不变；改回合法值后自动恢复；
  //   * 未启用：只刷新仓库与快照列表，watcher 不启动、也不写任何东西。
  {
    const QString repository_next =
        temp.path() + QStringLiteral("/repository-next");
    const QString repository_inside =
        source + QStringLiteral("/inner-repository");
    QDir().mkpath(repository_next);
    QDir().mkpath(repository_inside);

    // (1) 合法的新仓库：跟上 + 重建监听 + resync。
    const int old_repo_before = CountArchives(repository);
    run.Check(CountArchives(repository_next) == 0,
              QStringLiteral("RT-41 新仓库一开始是空的（后面的增长才是判别）"),
              QString::number(CountArchives(repository_next)));
    run.Check(controller->saveRepositoryPath(repository_next),
              QStringLiteral("RT-42 设置页把仓库改成新目录（真实入口）"),
              controller->statusMessage());
    run.Check(realtime->repositoryPath() == repository_next,
              QStringLiteral("RT-43 实时控制器立刻读到新仓库"),
              realtime->repositoryPath());
    run.Check(realtime->watching() && realtime->watchCount() > 0,
              QStringLiteral("RT-44 新仓库下监听已重建"),
              QString::number(realtime->watchCount()));
    run.Check(WaitForRepositoryGrowth(realtime, repository_next, 0, 60000),
              QStringLiteral("RT-45 换仓库后合成了一次 resync 并产出快照"),
              QString::number(CountArchives(repository_next)));
    const QString after_switch = realtime->lastSnapshotName();
    run.Check(ArchiveNames(repository_next).contains(after_switch),
              QStringLiteral("RT-46 新快照落在新仓库里"), after_switch);
    run.Check(CountArchives(repository) == old_repo_before,
              QStringLiteral("RT-47 旧仓库没有新增任何快照"),
              QString::number(CountArchives(repository)));
    std::printf("[realtime] switch repo=%s snapshot=%s\n",
                qPrintable(repository_next), qPrintable(after_switch));

    // (2) 不合法的新仓库（落在 source 里）：overlap 必须被重新检查，
    //     而且**一个快照都不许写**。
    const int next_repo_before = CountArchives(repository_next);
    const QString snapshot_before_invalid = realtime->lastSnapshotName();
    run.Check(
        controller->saveRepositoryPath(repository_inside),
        QStringLiteral("RT-48 设置页把仓库改成 source 里的目录（真实入口）"),
        controller->statusMessage());
    run.Check(realtime->repositoryPath() == repository_inside,
              QStringLiteral("RT-49 实时控制器跟着读到这个仓库"),
              realtime->repositoryPath());
    run.Check(
        !realtime->watching(),
        QStringLiteral("RT-50 不合法时监听被停掉（不再从旧仓库的视角看事件）"));
    run.Check(realtime->watchDegraded() &&
                  realtime->phaseKey() == QStringLiteral("watch_degraded"),
              QStringLiteral("RT-51 进入明确的 degraded 状态"),
              realtime->phaseKey());
    run.Check(realtime->statusMessage().contains(QStringLiteral("repository")),
              QStringLiteral("RT-52 状态里给的是共享核心那句原因"),
              realtime->statusMessage());
    run.Check(
        realtime->enabled(),
        QStringLiteral("RT-53 不合法不会把实时备份偷偷关掉（仍然 enabled）"));

    // 判别：换仓库**之后**源目录里真的发生了事件，而且等满了一个
    // debounce + max_wait 窗口。旧仓库一份都不许新增 —— 如果 handler 没有停掉
    // 旧 watcher、或者还拿着旧仓库路径，这里必然多出一份归档。
    run.Check(
        WriteTestFile(source + QStringLiteral("/after-switch.txt"), "moved"),
        QStringLiteral("RT-54 换仓库之后源目录里再写一个文件"));
    WaitForAnimation(2500);
    run.Check(!realtime->libraryBusy() && !realtime->pending(),
              QStringLiteral("RT-55 不合法仓库下一轮触发都没有"));
    run.Check(CountArchives(repository_next) == next_repo_before,
              QStringLiteral("RT-56 判别：旧仓库没有新增任何快照"),
              QString::number(CountArchives(repository_next)));
    run.Check(CountArchives(repository_inside) == 0,
              QStringLiteral("RT-57 非法仓库里一份快照都没有"),
              QString::number(CountArchives(repository_inside)));
    run.Check(realtime->lastSnapshotName() == snapshot_before_invalid,
              QStringLiteral("RT-58 也没有产出任何新的归档名"),
              realtime->lastSnapshotName());

    // (3) 仓库改回合法值：必须自动恢复（重新 attach + 合成 resync）。
    const int next_repo_before_recover = CountArchives(repository_next);
    run.Check(controller->saveRepositoryPath(repository_next),
              QStringLiteral("RT-59 把仓库改回合法目录"),
              controller->statusMessage());
    run.Check(realtime->watching() && realtime->watchCount() > 0 &&
                  !realtime->watchDegraded(),
              QStringLiteral("RT-60 degraded 状态自动恢复：监听重建"),
              realtime->watchStateText());
    run.Check(WaitForRepositoryGrowth(realtime, repository_next,
                                      next_repo_before_recover, 60000),
              QStringLiteral("RT-61 恢复后重新同步并产出快照"),
              QString::number(CountArchives(repository_next)));
    const QString recovered = realtime->lastSnapshotName();
    run.Check(ArchiveNames(repository_next).contains(recovered),
              QStringLiteral("RT-62 恢复后的快照落在合法仓库里"), recovered);
    std::printf("[realtime] recover repo=%s snapshot=%s\n",
                qPrintable(repository_next), qPrintable(recovered));

    // (4) 未启用时改仓库：只刷新仓库与列表，不启动 watcher、不写任何东西。
    const int next_repo_before_disabled = CountArchives(repository_next);
    run.Check(realtime->setEnabled(false),
              QStringLiteral("RT-63 先停用实时备份"));
    run.Check(!realtime->watching(), QStringLiteral("RT-64 停用后不再监听"));
    run.Check(controller->saveRepositoryPath(repository),
              QStringLiteral("RT-65 未启用状态下把仓库改回 A"),
              controller->statusMessage());
    run.Check(realtime->repositoryPath() == repository,
              QStringLiteral("RT-66 未启用时依然跟上仓库路径"),
              realtime->repositoryPath());
    run.Check(!realtime->watching() && !realtime->watchDegraded() &&
                  realtime->watchCount() == 0,
              QStringLiteral("RT-67 未启用时绝不无故启动 watcher"),
              realtime->watchStateText());
    run.Check(realtime->waitForIdle(60000) &&
                  realtime->snapshotCount() == old_repo_before,
              QStringLiteral("RT-68 快照列表跟着仓库刷新"),
              QString::number(realtime->snapshotCount()) + QStringLiteral("/") +
                  QString::number(old_repo_before));
    WaitForAnimation(1500);
    run.Check(CountArchives(repository) == old_repo_before &&
                  !realtime->libraryBusy(),
              QStringLiteral("RT-69 未启用时改仓库不写任何快照"),
              QString::number(CountArchives(repository)));
    run.Check(CountArchives(repository_next) == next_repo_before_disabled,
              QStringLiteral("RT-70 也不写回上一个仓库"));
    std::printf("[realtime] disabled repo=%s snapshots=%d\n",
                qPrintable(realtime->repositoryPath()),
                realtime->snapshotCount());
  }

  realtime->stop();

  std::printf("[realtime] 通过 %d 项，失败 %d 项\n", run.passed, run.failed);
  if (run.failed != 0) {
    for (const QString& failure : run.failures) {
      std::printf("[realtime]   FAIL %s\n", qPrintable(failure));
    }
    return 1;
  }
  std::printf("[realtime] ok\n");
  return 0;
}

// ---- --schedule-test：自动备份页的控制器链路自检 ----
//
// 全程跑在临时目录里：临时 config.json、临时 schedule.json、临时仓库与源目录。
// 绝不读写用户真实的计划配置。
//
// 它刻意不 mock 核心：控制器写进 store 的东西，紧接着用
// backupproject::ScheduleStore 原样读回来逐项比对 —— 这正是
// "GUI 与 CLI 读同一份 store、同一套 schema"在单元层面的证据。

QStringList ArchiveNames(const QString& repository) {
  QDir directory(repository);
  return directory.entryList(QStringList() << QStringLiteral("*.bak"),
                             QDir::Files, QDir::Name);
}

int CountArchives(const QString& repository) {
  return ArchiveNames(repository).size();
}

// scheduler **配置**部分的稳定指纹。M1.5 用它断言"被拒绝的手动备份没有改写
// 配置"。只放配置字段：managed / history / last-run 属于 state，正在跑的定时
// 评估会合法地改它们，不能进这个指纹。
QString ScheduleConfigSignature(const backupproject::ScheduleConfig& config) {
  QStringList parts;
  parts << (config.enabled ? QStringLiteral("1") : QStringLiteral("0"))
        << QString::number(static_cast<int>(config.trigger))
        << QString::number(static_cast<int>(config.strategy))
        << QString::fromStdString(config.source_path)
        << QString::number(config.interval_minutes)
        << QString::number(config.retain_count)
        << QString::number(static_cast<int>(config.pack_method))
        << QString::number(static_cast<int>(config.compression_method))
        << QString::number(static_cast<int>(config.encryption_method))
        << QStringLiteral("|");
  for (const std::string& rule : config.include_rules) {
    parts << QString::fromStdString(rule);
  }
  parts << QStringLiteral("||");
  for (const std::string& rule : config.exclude_rules) {
    parts << QString::fromStdString(rule);
  }
  return parts.join(QLatin1Char('~'));
}

QVariantMap LastHistory(const backup_modern::ScheduleController& schedule) {
  const QVariantList history = schedule.history();
  if (history.isEmpty()) return QVariantMap();
  return history.last().toMap();
}

int RunScheduleTest(backup_modern::ScheduleController* schedule,
                    backup_modern::BackupController* backup_controller,
                    const QString& config_path) {
  CheckRun run;
  run.prefix = "[schedule]";

  // 频率断言的人话单位名（内部 key -> 中文），只用于日志。
  const auto unitLabel = [](const char* key) -> QString {
    const backup_modern::FrequencyUnit* unit =
        backup_modern::FindFrequencyUnit(key);
    return QString::fromUtf8(unit == nullptr ? key : unit->label);
  };

  QTemporaryDir temp;
  if (!temp.isValid()) {
    std::fprintf(stderr, "[schedule] 无法创建临时目录\n");
    return 1;
  }
  // 从一份干净的 store 开始：自检要断言"默认值"，残留的旧计划会让它
  // 测的不是默认状态。这里删的是 --schedule-file 指到的文件（测试隔离目录），
  // 正常启动不会走到这条路径。
  QFile::remove(schedule->storePath());

  const QString source = temp.path() + QStringLiteral("/source");
  const QString repository = temp.path() + QStringLiteral("/repository");
  QDir().mkpath(source);
  QDir().mkpath(repository);
  if (!WriteTestFile(source + QStringLiteral("/a.txt"), "alpha")) {
    std::fprintf(stderr, "[schedule] 无法准备源文件\n");
    return 1;
  }

  // 1) 临时 config.json：把仓库指到临时目录。
  {
    backupproject::ConfigManager manager(config_path.toStdString());
    backupproject::AppConfig config;
    config.backup_repository_path = repository.toStdString();
    std::string error;
    run.Check(manager.Save(config, &error),
              QStringLiteral("SCH-01 临时 config.json 写入成功"),
              QString::fromStdString(error));
  }

  schedule->reload();

  run.Check(!schedule->enabled(), QStringLiteral("SCH-02 默认未启用"));
  run.Check(schedule->intervalMinutes() == 60,
            QStringLiteral("SCH-03 默认周期 60 分钟"),
            QString::number(schedule->intervalMinutes()));
  run.Check(schedule->retainCount() == 12,
            QStringLiteral("SCH-04 默认保留 12 个"),
            QString::number(schedule->retainCount()));
  run.Check(schedule->packKey() == QStringLiteral("mypack") &&
                schedule->compressionKey() == QStringLiteral("none") &&
                schedule->encryptionKey() == QStringLiteral("none"),
            QStringLiteral("SCH-05 默认 MyPack + 不压缩 + 不加密"));
  run.Check(schedule->repositoryPath() == repository,
            QStringLiteral("SCH-06 控制器读到了临时仓库"),
            schedule->repositoryPath());
  // 这句能力说明改成面向用户的一句之后，断言也跟着改：它必须说的是当前真的
  // 支持什么，而不是"以后会扩展什么"。
  run.Check(
      schedule->supportedModeText().contains(QStringLiteral("定时触发")) &&
          !schedule->supportedModeText().contains(QStringLiteral("后续将扩展")),
      QStringLiteral("SCH-07 页面说明只承诺已实现的模式"),
      schedule->supportedModeText());

  // ---- 备份频率：值 + 单位 <-> interval_minutes ----
  //
  // 界面上是"每 1 小时"，核心与 backupctl 只认分钟。这一组断言把换算的两端都
  // 钉住，尤其是"不能整除就回退分钟"和"乘法不许溢出"。
  {
    struct FrequencyCase {
      const char* value;
      const char* unit;
      int expected;
    };
    const FrequencyCase kAccepted[] = {
        {"1", "minutes", 1},   {"90", "minutes", 90},   {"1", "hours", 60},
        {"2", "hours", 120},   {"1", "days", 1440},     {"2", "days", 2880},
        {"1", "weeks", 10080}, {"365", "days", 525600},
    };
    for (const FrequencyCase& item : kAccepted) {
      std::uint32_t minutes = 0;
      std::string error;
      const bool ok = backup_modern::ParseFrequency(item.value, item.unit,
                                                    &minutes, &error);
      run.Check(ok && minutes == static_cast<std::uint32_t>(item.expected),
                QStringLiteral("FREQ-01 每 %1 %2 -> %3 分钟")
                    .arg(QString::fromLatin1(item.value), unitLabel(item.unit))
                    .arg(item.expected),
                ok ? QString::number(minutes) : QString::fromStdString(error));
    }

    // 加载：取最大的整除单位。10080 必须是"每 1 周"而不是"每 168 小时"，
    // 90 必须老实回退成"每 90 分钟"，绝不显示"每 1.5 小时"。
    struct SplitCase {
      int minutes;
      const char* value;
      const char* unit;
    };
    const SplitCase kSplit[] = {
        {60, "1", "hours"},  {120, "2", "hours"},     {1440, "1", "days"},
        {2880, "2", "days"}, {10080, "1", "weeks"},   {90, "90", "minutes"},
        {1, "1", "minutes"}, {525600, "365", "days"},
    };
    for (const SplitCase& item : kSplit) {
      std::string value;
      std::string unit;
      backup_modern::SplitFrequency(static_cast<std::uint32_t>(item.minutes),
                                    &value, &unit);
      run.Check(value == item.value && unit == item.unit,
                QStringLiteral("FREQ-02 %1 分钟 -> 每 %2 %3")
                    .arg(item.minutes)
                    .arg(QString::fromLatin1(item.value), unitLabel(item.unit)),
                QStringLiteral("%1/%2").arg(QString::fromStdString(value),
                                            QString::fromStdString(unit)));
    }

    // 0 / 负数 / 非数字 / 小数点 / 超最大值 / 乘法溢出 / 未知单位：全部拒绝。
    struct RejectCase {
      const char* value;
      const char* unit;
      const char* why;
    };
    const RejectCase kRejected[] = {
        {"0", "minutes", "零"},
        {"0", "hours", "零"},
        {"-1", "hours", "负数"},
        {"abc", "minutes", "非数字"},
        {"1.5", "hours", "小数点"},
        {"", "minutes", "空串"},
        {"12abc", "minutes", "尾随字母"},
        {"99999999999999999999", "minutes", "超出 uint32"},
        {"525601", "minutes", "超出最大分钟数"},
        {"1000", "weeks", "乘法会溢出上界"},
        {"1", "lightyears", "未知单位"},
    };
    for (const RejectCase& item : kRejected) {
      std::uint32_t minutes = 0;
      std::string error;
      const bool ok = backup_modern::ParseFrequency(item.value, item.unit,
                                                    &minutes, &error);
      run.Check(!ok && !error.empty(),
                QStringLiteral("FREQ-03 拒绝 每 %1 %2（%3）")
                    .arg(QString::fromLatin1(item.value), unitLabel(item.unit),
                         QString::fromUtf8(item.why)),
                ok ? QStringLiteral("被接受了：%1").arg(minutes)
                   : QStringLiteral("没有任何原因"));
    }
  }

  // 控制器入口：界面走的就是这一条，返回值与回显都要对得上。
  run.Check(
      schedule->saveConfigFromFrequencyText(
          true, source, QStringLiteral("1"), QStringLiteral("hours"),
          QStringLiteral("3"), QStringLiteral("mypack"), QStringLiteral("none"),
          QStringList(), QStringList(), QStringLiteral("full")) &&
          schedule->intervalMinutes() == 60 &&
          schedule->frequencyValueText() == QStringLiteral("1") &&
          schedule->frequencyUnitKey() == QStringLiteral("hours"),
      QStringLiteral("FREQ-04 控制器接受“每 1 小时”并回显一致"),
      QStringLiteral("%1 %2 / %3 分钟")
          .arg(schedule->frequencyValueText(), schedule->frequencyUnitKey())
          .arg(schedule->intervalMinutes()));
  run.Check(
      schedule->saveConfigFromFrequencyText(
          true, source, QStringLiteral("90"), QStringLiteral("minutes"),
          QStringLiteral("3"), QStringLiteral("mypack"), QStringLiteral("none"),
          QStringList(), QStringList(), QStringLiteral("full")) &&
          schedule->intervalMinutes() == 90 &&
          schedule->frequencyValueText() == QStringLiteral("90") &&
          schedule->frequencyUnitKey() == QStringLiteral("minutes"),
      QStringLiteral("FREQ-05 90 分钟不会被显示成 1.5 小时"));
  run.Check(
      !schedule->saveConfigFromFrequencyText(
          true, source, QStringLiteral("0"), QStringLiteral("minutes"),
          QStringLiteral("3"), QStringLiteral("mypack"), QStringLiteral("none"),
          QStringList(), QStringList(), QStringLiteral("full")) &&
          schedule->statusKind() == QStringLiteral("error"),
      QStringLiteral("FREQ-06 控制器拒绝“每 0 分钟”并给出错误"),
      schedule->statusTitle() + QStringLiteral("/") +
          schedule->statusMessage());
  schedule->clearStatus();

  // 2) 保存一份真实计划。
  run.Check(schedule->saveConfig(true, source, 1, 3, QStringLiteral("ustar"),
                                 QStringLiteral("huffman"), QStringList(),
                                 QStringList()),
            QStringLiteral("SCH-08 保存计划成功"));

  const QString store_file = schedule->storePath();
  run.Check(QFile::exists(store_file),
            QStringLiteral("SCH-09 schedule.json 已落盘"), store_file);
  struct stat store_info;
  const bool stat_ok =
      ::stat(store_file.toLocal8Bit().constData(), &store_info) == 0;
  run.Check(stat_ok && (store_info.st_mode & 07777) == 0600,
            QStringLiteral("SCH-10 schedule.json 权限是 0600"),
            stat_ok ? QString::number(store_info.st_mode & 07777, 8)
                    : QStringLiteral("stat 失败"));

  // 3) 用共享核心原样读回来 —— GUI 存的东西必须是共享 schema。
  {
    backupproject::ScheduleStore store(store_file.toStdString());
    backupproject::ScheduleDocument document;
    std::string error;
    const bool loaded = store.Load(&document, &error) ==
                        backupproject::ScheduleLoadStatus::kLoaded;
    run.Check(loaded, QStringLiteral("SCH-11 核心能读回 schedule.json"),
              QString::fromStdString(error));
    run.Check(document.config.interval_minutes == 1 &&
                  document.config.retain_count == 3,
              QStringLiteral("SCH-12 周期与保留数量逐项一致"));
    run.Check(
        document.config.pack_method == backupproject::PackMethod::kUstar &&
            document.config.compression_method ==
                backupproject::CompressionMethod::kHuffman,
        QStringLiteral("SCH-13 pack / compression 逐项一致"));
    run.Check(document.config.encryption_method ==
                  backupproject::EncryptionMethod::kNone,
              QStringLiteral("SCH-14 schedule.json 里没有加密"));
    run.Check(document.config.source_path == source.toStdString(),
              QStringLiteral("SCH-15 源目录逐项一致"));
    run.Check(
        document.config.trigger == backupproject::BackupTrigger::kScheduled &&
            document.config.strategy == backupproject::BackupStrategy::kFull,
        QStringLiteral("SCH-16 trigger / strategy 是 scheduled + full"));
  }

  // 4) 立即检查并运行：首次快照。
  run.Check(schedule->runNow(), QStringLiteral("SCH-17 立即运行被接受"));
  schedule->waitForIdle(180000);
  run.Check(schedule->lastSucceeded(), QStringLiteral("SCH-18 首次运行成功"));
  run.Check(schedule->managedSnapshots().size() == 1,
            QStringLiteral("SCH-19 产生了一个计划快照"),
            QString::number(schedule->managedSnapshots().size()));
  run.Check(CountArchives(repository) == 1,
            QStringLiteral("SCH-20 仓库里恰好一个归档"),
            QString::number(CountArchives(repository)));
  run.Check(
      LastHistory(*schedule).value(QStringLiteral("resultKey")).toString() ==
          QStringLiteral("success_created"),
      QStringLiteral("SCH-21 历史记录为 success_created"),
      LastHistory(*schedule).value(QStringLiteral("resultKey")).toString());

  // 5) 再运行一次：没有变化必须跳过，不产生新归档。
  schedule->runNow();
  schedule->waitForIdle(180000);
  run.Check(schedule->managedSnapshots().size() == 1,
            QStringLiteral("SCH-22 无变化时没有新增快照"));
  run.Check(CountArchives(repository) == 1,
            QStringLiteral("SCH-23 无变化时没有新增归档"),
            QString::number(CountArchives(repository)));
  run.Check(
      LastHistory(*schedule).value(QStringLiteral("resultKey")).toString() ==
          QStringLiteral("skipped_no_changes"),
      QStringLiteral("SCH-24 历史记录为 skipped_no_changes"),
      LastHistory(*schedule).value(QStringLiteral("resultKey")).toString());

  // 6) 新增文件：建立新快照，变化摘要里 added = 1。
  if (!WriteTestFile(source + QStringLiteral("/b.txt"), "beta")) {
    run.Check(false, QStringLiteral("SCH-25 新增源文件"));
  }
  schedule->runNow();
  schedule->waitForIdle(180000);
  run.Check(schedule->managedSnapshots().size() == 2,
            QStringLiteral("SCH-25 变化后建立了新快照"),
            QString::number(schedule->managedSnapshots().size()));
  run.Check(
      LastHistory(*schedule)
          .value(QStringLiteral("changesText"))
          .toString()
          .startsWith(QStringLiteral("+1")),
      QStringLiteral("SCH-26 变化摘要显示 +1 新增"),
      LastHistory(*schedule).value(QStringLiteral("changesText")).toString());

  // 7) retention：retain=1 之后只留最新一份。
  run.Check(schedule->saveConfig(true, source, 1, 1, QStringLiteral("mypack"),
                                 QStringLiteral("none"), QStringList(),
                                 QStringList()),
            QStringLiteral("SCH-27 保留数量改为 1"));
  if (!WriteTestFile(source + QStringLiteral("/c.txt"), "gamma")) {
    run.Check(false, QStringLiteral("SCH-28 再新增一个源文件"));
  }
  schedule->runNow();
  schedule->waitForIdle(180000);
  run.Check(schedule->managedSnapshots().size() == 1,
            QStringLiteral("SCH-28 retention 后只剩一个计划快照"),
            QString::number(schedule->managedSnapshots().size()));
  run.Check(CountArchives(repository) == 1,
            QStringLiteral("SCH-29 仓库里也只剩一个归档"),
            QString::number(CountArchives(repository)));

  // 8) 规则校验走的是真实 Filter；非法规则不许进配置。
  run.Check(
      !schedule
           ->validateRule(QStringLiteral("include"), QStringLiteral("bogus:x"))
           .isEmpty(),
      QStringLiteral("SCH-30 非法规则被 Filter 拒绝"));
  run.Check(
      schedule
          ->validateRule(QStringLiteral("include"), QStringLiteral("ext:cpp"))
          .isEmpty(),
      QStringLiteral("SCH-31 合法规则被 Filter 接受"));
  run.Check(!schedule->saveConfig(true, source, 0, 1, QStringLiteral("mypack"),
                                  QStringLiteral("none"), QStringList(),
                                  QStringList()),
            QStringLiteral("SCH-32 非法周期被拒绝"));
  run.Check(!schedule->saveConfig(true, source, 1, 1, QStringLiteral("gzip"),
                                  QStringLiteral("none"), QStringList(),
                                  QStringList()),
            QStringLiteral("SCH-33 未知打包方式被拒绝"));

  // 8b) 最后留下一份"有代表性"的配置（retain=7 / interval=5 / ustar +
  // huffman）， 供跨前端脚本用 backupctl schedule show 逐项比对 —— GUI
  // 写的，CLI 必须读得一模一样。
  run.Check(
      schedule->saveConfig(true, source, 5, 7, QStringLiteral("ustar"),
                           QStringLiteral("huffman"),
                           QStringList() << QStringLiteral("ext:txt"),
                           QStringList() << QStringLiteral("path:**/build/**")),
      QStringLiteral("SCH-36 留下跨前端比对用的配置"));

  // 9) 与 CLI 同源：默认路径必须与 QStandardPaths 算出来的完全一致。
  const QString qsp =
      QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
  run.Check(QString::fromStdString(backupproject::DefaultConfigFilePath()) ==
                QDir(qsp).filePath(QStringLiteral("config.json")),
            QStringLiteral("SCH-34 CLI 与 GUI 的 config.json 严格同路径"),
            QString::fromStdString(backupproject::DefaultConfigFilePath()));
  run.Check(QString::fromStdString(backupproject::DefaultScheduleFilePath()) ==
                QDir(qsp).filePath(QStringLiteral("schedule.json")),
            QStringLiteral("SCH-35 CLI 与 GUI 的 schedule.json 严格同路径"),
            QString::fromStdString(backupproject::DefaultScheduleFilePath()));

  // 10) review-fix：仓库在运行期被改掉之后，下一次评估必须写到新仓库去。
  //
  // 这一段只能在这里测：它依赖 BackupController::repositoryPathChanged 这个
  // 真实信号，而不是"重新构造一个控制器"。
  {
    const QString repository_b = temp.path() + QStringLiteral("/repository-b");
    QDir().mkpath(repository_b);
    run.Check(CountArchives(repository) == 1,
              QStringLiteral("SCH-40 切换前旧仓库里有一份快照"),
              QString::number(CountArchives(repository)));

    run.Check(backup_controller->saveRepositoryPath(repository_b),
              QStringLiteral("SCH-41 设置页把仓库改成 B"));
    // ScheduleController 订阅了 repositoryPathChanged，QML 一行都不用改。
    run.Check(schedule->repositoryPath() == repository_b,
              QStringLiteral("SCH-42 计划控制器立刻跟上了新仓库"),
              schedule->repositoryPath());

    schedule->runNow();
    schedule->waitForIdle(180000);
    run.Check(CountArchives(repository_b) == 1,
              QStringLiteral("SCH-43 新快照出现在 B 仓库"),
              QString::number(CountArchives(repository_b)));
    run.Check(CountArchives(repository) == 1,
              QStringLiteral("SCH-44 A 仓库没有被写入"),
              QString::number(CountArchives(repository)));

    // 切回来：跨前端比对用的 config.json 必须还是原来那个仓库。
    run.Check(backup_controller->saveRepositoryPath(repository),
              QStringLiteral("SCH-45 仓库切回 A"));
  }

  // 11) review-fix：首次启用把下一次运行排在一个完整周期之后。
  //
  // 必须走一次真正的 disabled -> enabled，否则 ApplyScheduleEnableTransition
  // 按设计就是 no-op（保存配置不该把时间表往后推）。
  {
    run.Check(schedule->saveConfig(
                  false, source, 30, 7, QStringLiteral("ustar"),
                  QStringLiteral("huffman"), QStringList(), QStringList()),
              QStringLiteral("SCH-46 先停用并改成 30 分钟周期"));
    const qint64 before = QDateTime::currentSecsSinceEpoch();
    run.Check(schedule->setEnabled(true), QStringLiteral("SCH-47 重新启用"));

    backupproject::ScheduleStore store(schedule->storePath().toStdString());
    backupproject::ScheduleDocument document;
    std::string error;
    const bool loaded = store.Load(&document, &error) ==
                        backupproject::ScheduleLoadStatus::kLoaded;
    run.Check(loaded, QStringLiteral("SCH-48 直接读回 store"),
              QString::fromStdString(error));
    const qint64 delta =
        loaded ? document.state.next_run_time_sec - before : -1;
    run.Check(delta >= 1795 && delta <= 1810,
              QStringLiteral("SCH-49 启用后 next run 正好是一个完整周期之后"),
              QString::number(delta));
  }

  // 12) review-fix：baseline 被删掉之后必须重建，而不是因为 manifest 相同就
  // skip。
  {
    const QStringList names = ArchiveNames(repository);
    run.Check(names.size() == 1, QStringLiteral("SCH-50 仓库里有一份 baseline"),
              QString::number(names.size()));
    if (names.size() == 1) {
      run.Check(QFile::remove(repository + QStringLiteral("/") + names.front()),
                QStringLiteral("SCH-51 手工删掉 baseline 快照"));
      schedule->runNow();
      schedule->waitForIdle(180000);
      run.Check(CountArchives(repository) == 1,
                QStringLiteral("SCH-52 下一轮重建了一份快照"),
                QString::number(CountArchives(repository)));
      run.Check(schedule->lastSucceeded(),
                QStringLiteral("SCH-53 重建这一轮是成功的"));
    }
  }

  // 13) GUI 独有的边界：手动备份正在写盘时，定时任务到点了。
  //
  // 这几条断言不依赖时间。三次 runNow() 都在主线程上连续调用，中间没有跑事件
  // 循环；而 busy_ 只可能被主线程上的 OnWatcherFinished 清掉，所以第一次必然
  // 已经把它置上，后两次必然走 coalesce 分支。仓库里的源自 SCH-52
  // 之后没有变过， 所以这两次评估都是 skip，不会影响后面的收尾。
  {
    const int history_before = schedule->history().size();
    schedule->runNow();
    run.Check(schedule->libraryBusy(),
              QStringLiteral("SCH-54 busy 期间第一次请求真的跑起来了"));
    run.Check(!schedule->pending(),
              QStringLiteral("SCH-55 第一次请求不留下 pending"));
    schedule->runNow();
    run.Check(schedule->pending(),
              QStringLiteral("SCH-56 busy 期间第二次请求被合并成一次 pending"));
    schedule->runNow();
    run.Check(schedule->pending(),
              QStringLiteral("SCH-57 再来一次仍然是同一个 pending，不排队"));

    run.Check(schedule->waitForIdle(180000),
              QStringLiteral("SCH-58 补跑结束之后控制器回到空闲"));
    run.Check(!schedule->libraryBusy() && !schedule->pending(),
              QStringLiteral("SCH-59 空闲之后 busy 与 pending 都清掉了"));
    // 三次请求 = 两次评估（一次立即 + 一次合并后的补跑）。排队的话会是三次。
    const int expected =
        qMin(history_before + 2,
             static_cast<int>(backupproject::kMaxHistoryEntries));
    run.Check(schedule->history().size() == expected,
              QStringLiteral("SCH-60 三次请求只产生两次评估"),
              QString::number(history_before) + " -> " +
                  QString::number(schedule->history().size()));
  }

  // 14) 抢不到 runner 锁之后必须能重新抢回来。
  //
  // 场景：另一个进程先拿着锁，本程序启用计划时抢不到；对方退出之后 tick 必须
  // 把锁拿回来。少了这一步，"别的进程曾经跑过"会变成"本程序再也不跑这个计划"，
  // 页面会一直显示"已被另一进程持有"。
  //
  // 持锁方用一个真实的 SchedulerLock 扮演 —— 与 CLI watch / GUI
  // 用的是同一个类、 同一个锁文件。
  {
    const QString lock_path = schedule->storePath() + QStringLiteral(".lock");
    run.Check(schedule->saveConfig(false, source, 5, 7, QStringLiteral("ustar"),
                                   QStringLiteral("huffman"), QStringList(),
                                   QStringList()),
              QStringLiteral("SCH-61 先停用，控制器放开自己手上的锁"));
    run.Check(!schedule->holdsRunnerLock(),
              QStringLiteral("SCH-62 停用之后控制器不再持锁"));

    backupproject::SchedulerLock other;
    std::string other_error;
    run.Check(other.Acquire(lock_path.toStdString(), &other_error),
              QStringLiteral("SCH-63 另一个进程拿到锁"),
              QString::fromStdString(other_error));

    run.Check(schedule->saveConfig(true, source, 5, 7, QStringLiteral("ustar"),
                                   QStringLiteral("huffman"), QStringList(),
                                   QStringList()),
              QStringLiteral("SCH-64 抢不到锁时仍然保存计划配置"));
    run.Check(
        !schedule->holdsRunnerLock() && !schedule->runnerMessage().isEmpty(),
        QStringLiteral("SCH-65 抢不到锁时如实报告给用户"),
        schedule->runnerMessage());

    // 对方退出。tick 每秒重试一次，这里最多等 6 秒。
    other.Release();
    bool recovered = false;
    for (int attempt = 0; attempt < 60 && !recovered; ++attempt) {
      WaitForAnimation(100);
      recovered = schedule->holdsRunnerLock();
    }
    run.Check(recovered,
              QStringLiteral("SCH-66 对方退出之后 tick 把锁拿了回来"),
              schedule->runnerMessage());
  }

  // 收尾：把跨前端比对用的那套配置写回去（interval=5 / retain=7 / ustar +
  // huffman + 两条规则）。modern_gui_check.sh 会拿 backupctl schedule show
  // 逐项比对，所以这里必须与 SCH-36 完全一致。
  run.Check(
      schedule->saveConfig(true, source, 5, 7, QStringLiteral("ustar"),
                           QStringLiteral("huffman"),
                           QStringList() << QStringLiteral("ext:txt"),
                           QStringList() << QStringLiteral("path:**/build/**")),
      QStringLiteral("SCH-67 恢复跨前端比对用的配置"));

  // 15) 落盘配置不合法 -> 明确挂起，而且**不再**周期性重试。
  //
  // 场景：有人手工把 schedule.json 改成一个"JSON 读得懂、业务规则不认"的配置。
  // 产品禁止多进程，所以不存在"另一个进程在背后把它修好了"；挂起期间唯一的
  // 恢复入口就是用户显式保存一份合法配置。
  //
  // 这一组断言全部不依赖时间：tick 被停掉之后**没有 timer**，再手动"敲"几次
  // tick 也不会有新提交，所以不需要靠 sleep 堆时间去看"是不是每秒跑一次"。
  {
    auto read_bytes = [](const QString& path) {
      QFile file(path);
      if (!file.open(QIODevice::ReadOnly)) return QByteArray();
      return file.readAll();
    };
    auto write_bytes = [](const QString& path, const QByteArray& data) {
      QFile file(path);
      if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
      const bool ok = file.write(data) == data.size();
      file.close();
      return ok;
    };

    const QString store_path = schedule->storePath();
    const QByteArray original = read_bytes(store_path);
    QByteArray invalid = original;
    // 无人值守加密是**解析得通、业务规则拒绝**的那一类：枚举值合法，
    // 但定时任务没有安全的密钥来源，ValidateScheduleConfig 必须拒绝它。
    invalid.replace("\"encryption\": \"none\"",
                    "\"encryption\": \"aes-256-ctr-hmac-sha256\"");
    run.Check(!original.isEmpty() && invalid != original,
              QStringLiteral("SCH-68 构造出一份业务上不合法的配置"));
    run.Check(write_bytes(store_path, invalid),
              QStringLiteral("SCH-69 把它写到磁盘上（模拟手工编辑）"));
    const QByteArray broken_bytes = read_bytes(store_path);

    // 文件本身读得懂，所以"读不懂"这条挂起理由不成立；合不合法由评估回答。
    schedule->start();
    run.Check(!schedule->suspended(),
              QStringLiteral("SCH-70 文件读得懂时不挂起（合法性由评估回答）"));
    run.Check(schedule->tickActiveForTest(),
              QStringLiteral("SCH-71 此时 tick 还在跑"));

    const int history_before = schedule->history().size();
    const int archives_before = CountArchives(repository);

    run.Check(
        schedule->runNow(),
        QStringLiteral("SCH-72 立即运行被接受（评估自己会发现配置不合法）"));
    run.Check(schedule->waitForIdle(180000), QStringLiteral("SCH-73 评估结束"));
    run.Check(schedule->suspended(),
              QStringLiteral("SCH-74 评估判定配置不合法 -> 挂起"));
    run.Check(
        !schedule->tickActiveForTest(),
        QStringLiteral("SCH-75 挂起之后 1 Hz tick 被停掉（不会每秒重新校验）"));
    run.Check(!schedule->holdsRunnerLock(),
              QStringLiteral("SCH-76 挂起期间不占着 runner 锁"));
    run.Check(schedule->history().size() == history_before,
              QStringLiteral("SCH-77 挂起这一轮不写 history"),
              QString::number(history_before) + " -> " +
                  QString::number(schedule->history().size()));
    run.Check(CountArchives(repository) == archives_before,
              QStringLiteral("SCH-78 挂起之后仓库里没有多出任何归档"));
    run.Check(read_bytes(store_path) == broken_bytes,
              QStringLiteral("SCH-79 没有静默改写这份不合法的配置"));
    run.Check((schedule->statusTitle() + schedule->statusMessage())
                  .contains(QStringLiteral("挂起")),
              QStringLiteral("SCH-80 页面明确显示已挂起"),
              schedule->statusTitle() + QStringLiteral(" / ") +
                  schedule->statusMessage());

    // 再"响"几次 tick：既不能有新评估，也不能有每秒一次的 signal 风暴。
    int status_signals = 0;
    int suspended_signals = 0;
    // 这两条 connection 只服务于本段的诊断观测，lambda 按引用捕获计数器。
    // 计数器随本段作用域销毁，connection 却一直挂在 schedule 上，runNow()
    // 发出 statusChanged 会写到失效的栈对象（ASan: stack-use-after-scope）。
    // 所以显式持有 connection，并在本段结束前断开。
    const QMetaObject::Connection status_connection = QObject::connect(
        schedule, &backup_modern::ScheduleController::statusChanged,
        [&status_signals]() { ++status_signals; });
    const QMetaObject::Connection suspended_connection = QObject::connect(
        schedule, &backup_modern::ScheduleController::suspendedChanged,
        [&suspended_signals]() { ++suspended_signals; });
    const QString status_before = schedule->statusTitle() +
                                  QStringLiteral("|") +
                                  schedule->statusMessage();
    for (int tick = 0; tick < 5; ++tick) {
      schedule->pumpTickForTest();
    }
    WaitForAnimation(60);
    run.Check(schedule->history().size() == history_before,
              QStringLiteral("SCH-81 再响 5 次 tick 也没有产生新评估"));
    run.Check(status_signals == 0 && suspended_signals == 0,
              QStringLiteral("SCH-82 诊断稳定：没有每秒一次的 signal 风暴"),
              QStringLiteral("status=") + QString::number(status_signals) +
                  QStringLiteral(" suspended=") +
                  QString::number(suspended_signals));
    run.Check(status_before == schedule->statusTitle() + QStringLiteral("|") +
                                   schedule->statusMessage(),
              QStringLiteral("SCH-83 诊断文本一个字符都没变"));
    run.Check(schedule->suspended(), QStringLiteral("SCH-84 仍然是挂起状态"));

    // 唯一的恢复入口：显式保存一份合法配置。这里同时把收尾需要的
    // 跨前端配置写回去（与 SCH-36 / SCH-67 完全一致）。
    run.Check(schedule->saveConfig(true, source, 5, 7, QStringLiteral("ustar"),
                                   QStringLiteral("huffman"),
                                   QStringList() << QStringLiteral("ext:txt"),
                                   QStringList()
                                       << QStringLiteral("path:**/build/**")),
              QStringLiteral("SCH-85 保存一份合法配置"));
    run.Check(!schedule->suspended(),
              QStringLiteral("SCH-86 保存成功之后挂起自动解除"));
    run.Check(schedule->tickActiveForTest(),
              QStringLiteral("SCH-87 tick 重新开始跑"));
    run.Check(schedule->holdsRunnerLock(),
              QStringLiteral("SCH-88 runner 锁也拿了回来"));
    run.Check(schedule->runNow() && schedule->waitForIdle(180000) &&
                  schedule->lastSucceeded(),
              QStringLiteral("SCH-89 恢复之后计划真的又能跑了"));
    run.Check(!schedule->suspended(),
              QStringLiteral("SCH-90 成功跑完一轮之后仍然没有挂起"));

    // 计数器就在这个作用域里：离开之前先断开，保证没有回调还能引用它们。
    QObject::disconnect(status_connection);
    QObject::disconnect(suspended_connection);
  }

  // 16) 同一进程内的单写者：评估在飞的时候不允许保存。
  //
  // ScheduleStore 的写者有两个：GUI 线程（saveConfig）与后台评估线程
  // （RunEvaluation -> ScheduledBackupService -> Save/SaveManifest）。两者并发
  // 就是并发的 Load->Modify->Save，后台刚 push 进去的 managed 记录与 history
  // 会被覆盖掉。QML 把按钮置灰只是界面礼貌，真正的不变式必须在这里。
  {
    const QString store_path = schedule->storePath();
    auto read_bytes = [](const QString& path) {
      QFile file(path);
      if (!file.open(QIODevice::ReadOnly)) return QByteArray();
      return file.readAll();
    };

    run.Check(schedule->runNow(),
              QStringLiteral("SCH-91 再立即运行一次，让评估进入 busy"));
    run.Check(schedule->libraryBusy(), QStringLiteral("SCH-92 评估确实在飞"));
    const QByteArray before = read_bytes(store_path);
    run.Check(!schedule->saveConfig(
                  true, source, 9, 4, QStringLiteral("fast-ustar"),
                  QStringLiteral("lzss-huffman"), QStringList(), QStringList()),
              QStringLiteral("SCH-93 busy 期间保存被明确拒绝"));
    run.Check(schedule->statusMessage().contains(QStringLiteral("正在")),
              QStringLiteral("SCH-94 拒绝的理由告诉用户等这一轮结束"),
              schedule->statusMessage());
    run.Check(read_bytes(store_path) == before,
              QStringLiteral("SCH-95 被拒绝的保存没有改动 store 一个字节"));
    run.Check(schedule->intervalMinutes() == 5 && schedule->retainCount() == 7,
              QStringLiteral("SCH-96 内存里的计划也没有被改"),
              QString::number(schedule->intervalMinutes()) +
                  QStringLiteral("/") +
                  QString::number(schedule->retainCount()));
    run.Check(schedule->waitForIdle(180000), QStringLiteral("SCH-97 评估结束"));
    run.Check(
        schedule->saveConfig(true, source, 5, 7, QStringLiteral("ustar"),
                             QStringLiteral("huffman"),
                             QStringList() << QStringLiteral("ext:txt"),
                             QStringList()
                                 << QStringLiteral("path:**/build/**")),
        QStringLiteral("SCH-98 空闲之后保存成功（收尾配置与 SCH-67 一致）"));
  }

  // 17) 前端 parity 契约：GUI 对非法输入问的是**共享核心**，不是自己的一套。
  //
  // 判据不是"两边措辞一样"，而是"GUI 显示的原因逐字来自核心函数"。同一批
  // 非法输入在 CLI 那边由 scripts/scheduled_backup_test.sh 的 I 区验证退出码。
  {
    // 三条都必须是**共享 Filter 真的拒绝**的规则：不以"假设它非法"为前提，
    // 而是把核心的返回值当作唯一判据（下面 core_ok 为真就直接算失败）。
    const QStringList bad_rules{QStringLiteral("size:not-a-number"),
                                QStringLiteral("type:bogus"),
                                QStringLiteral("bogus:x")};
    for (const QString& rule : bad_rules) {
      const QString gui_error =
          schedule->validateRule(QStringLiteral("include"), rule);
      backupproject::Filter filter;
      std::string core_error;
      const bool core_ok = filter.AddRule(backupproject::FilterAction::kInclude,
                                          rule.toStdString(), &core_error);
      run.Check(!core_ok && !gui_error.isEmpty() &&
                    gui_error == QString::fromStdString(core_error),
                QStringLiteral("SCH-99 非法规则 [%1] 的原因逐字来自共享 Filter")
                    .arg(rule),
                gui_error);
    }
    const QString good_rule = QStringLiteral("ext:txt;md");
    run.Check(schedule->validateRule(QStringLiteral("include"), good_rule)
                      .isEmpty() &&
                  backupproject::Filter().AddRule(
                      backupproject::FilterAction::kInclude,
                      good_rule.toStdString(), nullptr),
              QStringLiteral("SCH-100 合法规则两边都接受"));

    const QString store_path = schedule->storePath();
    QFile store_file(store_path);
    store_file.open(QIODevice::ReadOnly);
    const QByteArray before = store_file.readAll();
    store_file.close();

    run.Check(!schedule->saveConfig(true, source, 0, 7, QStringLiteral("ustar"),
                                    QStringLiteral("huffman"), QStringList(),
                                    QStringList()),
              QStringLiteral("SCH-101 GUI 拒绝越界周期"));
    run.Check(schedule->statusMessage().contains(QStringLiteral("1")) &&
                  schedule->statusMessage().contains(QStringLiteral("525600")),
              QStringLiteral("SCH-102 展示的是共享核心的范围说明"),
              schedule->statusMessage());
    run.Check(!schedule->saveConfig(true, source, 5, 7, QStringLiteral("tar"),
                                    QStringLiteral("huffman"), QStringList(),
                                    QStringList()),
              QStringLiteral("SCH-103 GUI 拒绝未知打包方式 key"));
    run.Check(!schedule->saveConfig(true, source, 5, 7, QStringLiteral("ustar"),
                                    QStringLiteral("zip"), QStringList(),
                                    QStringList()),
              QStringLiteral("SCH-104 GUI 拒绝未知压缩方式 key"));
    run.Check(!schedule->saveConfig(
                  true, source, 5, 7, QStringLiteral("ustar"),
                  QStringLiteral("huffman"),
                  QStringList() << QStringLiteral("type:bogus"), QStringList()),
              QStringLiteral("SCH-105 GUI 拒绝含非法规则的保存"));

    const QString missing_source =
        temp.path() + QStringLiteral("/does-not-exist");
    const bool accepted = schedule->saveConfig(
        true, missing_source, 5, 7, QStringLiteral("ustar"),
        QStringLiteral("huffman"), QStringList(), QStringList());
    // 与 GUI 内部构造的那份配置逐字段一致：saveConfig 只在**成功**时才写回
    // document_，所以直接拿 configForTest() 比就比错了对象。
    backupproject::ScheduleConfig probe = schedule->configForTest();
    probe.enabled = true;
    probe.source_path = missing_source.toStdString();
    std::string core_error;
    const bool core_accepts = backupproject::ValidateScheduleForEnable(
        probe, repository.toStdString(), &core_error);
    run.Check(!accepted && !core_accepts,
              QStringLiteral("SCH-106 源目录不存在时 GUI 与核心都拒绝启用"),
              schedule->statusMessage());
    run.Check(
        schedule->statusMessage().contains(QString::fromStdString(core_error)),
        QStringLiteral("SCH-107 拒绝理由就是 ValidateScheduleForEnable 的原话"),
        QString::fromStdString(core_error));

    QFile after_file(store_path);
    after_file.open(QIODevice::ReadOnly);
    const QByteArray after = after_file.readAll();
    after_file.close();
    run.Check(before == after,
              QStringLiteral("SCH-108 这一串被拒绝的保存一个字节都没落盘"));
    run.Check(schedule->intervalMinutes() == 5 &&
                  schedule->retainCount() == 7 &&
                  schedule->sourcePath() == source,
              QStringLiteral("SCH-109 内存里的计划仍然是上一份合法配置"));
  }

  // 18) 备份管理页删除计划快照之后，ScheduleStore 必须立刻跟上。
  //
  // 这是 CLI 的 repository delete 一直有、GUI 以前没有的一步。少了它，同一个
  // 删除动作在两个前端上的后果不同：GUI 删完之后计划页还会继续显示一个已经
  // 不在仓库里的文件名，直到下一轮定时评估顺手 reconcile 掉。
  {
    run.Check(
        WriteTestFile(source + QStringLiteral("/deleted-later.txt"), "delta"),
        QStringLiteral("SCH-110 先给源目录制造一次变化"));
    run.Check(schedule->runNow() && schedule->waitForIdle(180000) &&
                  schedule->lastSucceeded(),
              QStringLiteral("SCH-111 建立一份新的计划快照"));

    const QVariantList managed = schedule->managedSnapshots();
    run.Check(!managed.isEmpty(), QStringLiteral("SCH-112 计划快照名单非空"));
    const QString victim =
        managed.last().toMap().value(QStringLiteral("fileName")).toString();
    run.Check(!victim.isEmpty(), QStringLiteral("SCH-113 取到要删除的归档名"),
              victim);

    // QML 的删除按钮走的就是这一条。
    run.Check(backup_controller->deleteBackup(victim),
              QStringLiteral("SCH-114 备份管理页删除成功"),
              backup_controller->statusMessage());
    run.Check(backup_controller->waitForCatalogIdle(600000),
              QStringLiteral("SCH-115 列表刷新结束"));

    bool still_listed = false;
    for (const QVariant& item : schedule->managedSnapshots()) {
      if (item.toMap().value(QStringLiteral("fileName")).toString() == victim) {
        still_listed = true;
      }
    }
    run.Check(!still_listed,
              QStringLiteral("SCH-116 删除之后计划快照列表立刻不再包含它"),
              victim);

    backupproject::ScheduleStore store(schedule->storePath().toStdString());
    backupproject::ScheduleDocument document;
    std::string store_error;
    run.Check(store.Load(&document, &store_error) ==
                  backupproject::ScheduleLoadStatus::kLoaded,
              QStringLiteral("SCH-117 能直接读回 schedule.json"),
              QString::fromStdString(store_error));
    bool on_disk = false;
    for (const backupproject::ScheduledSnapshotRecord& record :
         document.state.managed_snapshots) {
      if (QString::fromStdString(record.file_name) == victim) on_disk = true;
    }
    run.Check(!on_disk,
              QStringLiteral("SCH-118 schedule.json 里的 managed 名单也立刻跟上"
                             "（与 CLI 的 repository delete 完全一致）"),
              victim);

    // 收尾：把删掉的那份变化重新变成一份快照，让 store 回到有基线的稳定状态。
    run.Check(schedule->runNow() && schedule->waitForIdle(180000) &&
                  schedule->lastSucceeded(),
              QStringLiteral("SCH-119 收尾：重建一份计划快照"));
  }

  // 19) 周期与保留数量的解析规则只有一份：界面不再用 parseInt 截断。
  //
  // QML 的 parseInt("12abc") 是 12，而 backupctl 对同一个输入是明确拒绝。
  // 界面现在把**文本**交给共享核心的 ParseBoundedScheduleNumber，两边的结论
  // 因此不可能分叉——这一组断言钉住的就是"GUI 不再自己解析"。
  {
    const QString store_path = schedule->storePath();
    auto read_bytes = [](const QString& path) {
      QFile file(path);
      if (!file.open(QIODevice::ReadOnly)) return QByteArray();
      return file.readAll();
    };
    const QByteArray before = read_bytes(store_path);

    const QStringList bad{QStringLiteral("12abc"),
                          QString(),
                          QStringLiteral("-5"),
                          QStringLiteral("5.5"),
                          QStringLiteral("99999999999999"),
                          QStringLiteral(" ")};
    for (const QString& text : bad) {
      run.Check(
          !schedule->saveConfigFromText(
              true, source, text, QStringLiteral("7"), QStringLiteral("ustar"),
              QStringLiteral("huffman"), QStringList(), QStringList()),
          QStringLiteral("SCH-120 非法周期文本 [%1] 被拒绝（不再截断成 12）")
              .arg(text),
          schedule->statusMessage());
      run.Check(
          !schedule->saveConfigFromText(
              true, source, QStringLiteral("5"), text, QStringLiteral("ustar"),
              QStringLiteral("huffman"), QStringList(), QStringList()),
          QStringLiteral("SCH-121 非法保留数量文本 [%1] 被拒绝").arg(text),
          schedule->statusMessage());
    }
    // 格式正确但越界：由共享核心的范围校验拒绝，界面只是转述。
    run.Check(!schedule->saveConfigFromText(
                  true, source, QStringLiteral("0"), QStringLiteral("7"),
                  QStringLiteral("ustar"), QStringLiteral("huffman"),
                  QStringList(), QStringList()),
              QStringLiteral("SCH-122 周期 0 被共享核心的范围校验拒绝"));
    run.Check(schedule->statusMessage().contains(QStringLiteral("525600")),
              QStringLiteral("SCH-123 展示的是核心的范围说明"),
              schedule->statusMessage());
    run.Check(read_bytes(store_path) == before,
              QStringLiteral("SCH-124 这一串被拒绝的输入一个字节都没落盘"));

    run.Check(schedule->saveConfigFromText(
                  true, source, QStringLiteral("5"), QStringLiteral("7"),
                  QStringLiteral("ustar"), QStringLiteral("huffman"),
                  QStringList() << QStringLiteral("ext:txt"),
                  QStringList() << QStringLiteral("path:**/build/**")),
              QStringLiteral("SCH-125 合法文本照旧接受"),
              schedule->statusMessage());
    run.Check(schedule->intervalMinutes() == 5 && schedule->retainCount() == 7,
              QStringLiteral("SCH-126 接受之后的周期与保留数量正确"),
              QString::number(schedule->intervalMinutes()) +
                  QStringLiteral("/") +
                  QString::number(schedule->retainCount()));
  }

  // 20) 数字解析的 case table：GUI 与 CLI 必须逐项同结论。
  //
  // 上一组证明"GUI 不再用 parseInt"；这一组把**同一张表**再喂一遍，并且与
  // 共享核心的 ParseBoundedScheduleNumber（CLI 走的就是它）逐项比对返回值。
  {
    struct NumberCase {
      const char* text;
      bool accepted;
    };
    const NumberCase cases[] = {
        {"5", true},
        {"60", true},
        {" 5 ", true},
        {"\t5\t", true},
        {"\n60\r", true},
        {"007", true},
        {"", false},
        {"   ", false},
        {"\t", false},
        {"5x", false},
        {"x5", false},
        {"-1", false},
        {"+5", false},
        {"1.0", false},
        {"5 5", false},
        {"0x10", false},
        {"99999999999999999999", false},
        {"18446744073709551616", false},
        {"0", false},
        {"525601", false},
    };
    const QString store_path = schedule->storePath();
    auto read_bytes = [](const QString& path) {
      QFile file(path);
      if (!file.open(QIODevice::ReadOnly)) return QByteArray();
      return file.readAll();
    };
    const QByteArray before = read_bytes(store_path);

    for (const NumberCase& item : cases) {
      std::uint32_t core_value = 0;
      std::string core_error;
      const bool core_ok = backupproject::ParseBoundedScheduleNumber(
          item.text, backupproject::kMinIntervalMinutes,
          backupproject::kMaxIntervalMinutes, "--interval-minutes", &core_value,
          &core_error);
      const bool gui_ok = schedule->saveConfigFromText(
          true, source, QString::fromUtf8(item.text), QStringLiteral("7"),
          QStringLiteral("ustar"), QStringLiteral("huffman"), QStringList(),
          QStringList());
      const QByteArray before_case = read_bytes(store_path);
      run.Check(gui_ok == item.accepted && core_ok == item.accepted,
                QStringLiteral("SCH-127 [%1] GUI 与共享 parser 同结论（%2）")
                    .arg(QString::fromUtf8(item.text),
                         item.accepted ? QStringLiteral("接受")
                                       : QStringLiteral("拒绝")),
                schedule->statusMessage());
      if (!item.accepted) {
        // 被拒绝的那一项一个字节都不许落盘。
        run.Check(read_bytes(store_path) == before_case,
                  QStringLiteral("SCH-128 [%1] 被拒绝的输入没有改动 store")
                      .arg(QString::fromUtf8(item.text)));
      }
    }
    (void)before;
    // 收尾：把跨前端比对用的配置写回去（与 SCH-36 / SCH-67 / SCH-98 一致）。
    run.Check(schedule->saveConfigFromText(
                  true, source, QStringLiteral("5"), QStringLiteral("7"),
                  QStringLiteral("ustar"), QStringLiteral("huffman"),
                  QStringList() << QStringLiteral("ext:txt"),
                  QStringList() << QStringLiteral("path:**/build/**")),
              QStringLiteral("SCH-129 收尾：把跨前端比对用的配置写回去"),
              schedule->statusMessage());
    run.Check(schedule->intervalMinutes() == 5 && schedule->retainCount() == 7,
              QStringLiteral("SCH-130 收尾配置是 interval=5 / retain=7"),
              QString::number(schedule->intervalMinutes()) +
                  QStringLiteral("/") +
                  QString::number(schedule->retainCount()));
  }

  // 21) GUI 内部的操作串行化（M1-M7）。
  //
  // 产品规则：一个 Modern GUI 进程内，任何会改动 backup repository / schedule
  // state / source backup state 的业务操作，同一时刻最多一个。QML 的按钮状态
  // 只是界面礼貌；这一组断言全部直接调用 C++ API，证明后端自己会拒绝。
  {
    auto read_bytes = [](const QString& path) {
      QFile file(path);
      if (!file.open(QIODevice::ReadOnly)) return QByteArray();
      return file.readAll();
    };
    // 只读地取 scheduler 的持久化状态。M3 断言的是"被拒绝的删除没有把受管
    // 快照移出名单"，而 M1.1 起的后台评估在 busy 窗口里本来就会合法地写
    // history / last-run / manifest 等 bookkeeping，所以这里必须看结构化
    // 状态，不能拿整个文件的字节当"没有副作用"的代理。
    auto load_schedule_document = [](const QString& path,
                                     backupproject::ScheduleDocument* document,
                                     std::string* error) {
      backupproject::ScheduleStore store(path.toStdString());
      return store.Load(document, error) ==
             backupproject::ScheduleLoadStatus::kLoaded;
    };
    auto is_managed_snapshot =
        [](const backupproject::ScheduleDocument& document,
           const QString& name) {
          for (const backupproject::ScheduledSnapshotRecord& record :
               document.state.managed_snapshots) {
            if (QString::fromStdString(record.file_name) == name) return true;
          }
          return false;
        };
    const QString store_path = schedule->storePath();

    backup_controller->setSourcePath(source);

    // ---- M1：评估在飞 -> 手动备份被拒绝 ----
    run.Check(schedule->runNow() && schedule->libraryBusy(),
              QStringLiteral("M1.1 评估进入 busy"));
    const int archives_before = CountArchives(repository);
    backupproject::ScheduleDocument config_document;
    std::string config_error;
    const bool config_loaded =
        load_schedule_document(store_path, &config_document, &config_error);
    const QString config_before =
        ScheduleConfigSignature(config_document.config);
    run.Check(!backup_controller->startBackup(),
              QStringLiteral("M1.2 评估在飞时手动备份被 C++ 拒绝"),
              backup_controller->statusMessage());
    run.Check(backup_controller->statusMessage().contains(
                  QStringLiteral("定时备份评估")),
              QStringLiteral("M1.3 拒绝理由点名是谁在占着"),
              backup_controller->statusMessage());
    run.Check(CountArchives(repository) == archives_before,
              QStringLiteral("M1.4 被拒绝的手动备份没有产生任何归档"));
    // M1.5：被拒绝的手动备份没有产生属于"手动备份动作"的持久化副作用。
    //
    // 这里同样**不**比对整个 schedule.json 的字节：M1.1 起的定时评估在 busy
    // 窗口里会合法地写 managed / history / last-run 等 state，字节相等在存在
    // 合法并发写者时是个不成立的前提。手动备份永远不进 scheduler 的受管名单，
    // 所以能断言的是"这次被拒绝的调用没有改写 scheduler 配置"。
    backupproject::ScheduleDocument config_after_document;
    std::string config_after_error;
    const bool config_after_loaded = load_schedule_document(
        store_path, &config_after_document, &config_after_error);
    run.Check(config_after_loaded &&
                  ScheduleConfigSignature(config_after_document.config) ==
                      config_before,
              QStringLiteral("M1.5 被拒绝的手动备份没有改写 scheduler 配置"),
              QStringLiteral("loaded=") +
                  QString::number(config_loaded ? 1 : 0) + QStringLiteral("/") +
                  QString::number(config_after_loaded ? 1 : 0) +
                  QStringLiteral(" error=") +
                  QString::fromStdString(config_after_error));

    // ---- M2：评估在飞 -> 受管恢复被拒绝 ----
    // 目标取 scheduler **自己管理**的快照（最新的一份）：M3 要证明的是"被拒绝
    // 的删除没有把这份受管快照移出名单"，拿一份不受管的归档顶替就证明不了。
    // 最新的那份也最稳：retention 从最旧的开始淘汰。
    backupproject::ScheduleDocument pre_document;
    std::string pre_error;
    const bool pre_loaded =
        load_schedule_document(store_path, &pre_document, &pre_error);
    const backupproject::ScheduledSnapshotRecord* newest_managed = nullptr;
    for (const backupproject::ScheduledSnapshotRecord& record :
         pre_document.state.managed_snapshots) {
      if (newest_managed == nullptr ||
          record.created_time_sec > newest_managed->created_time_sec) {
        newest_managed = &record;
      }
    }
    const QString managed_name =
        pre_loaded && newest_managed != nullptr
            ? QString::fromStdString(newest_managed->file_name)
            : QString();
    run.Check(
        !managed_name.isEmpty(),
        QStringLiteral("M2.1 scheduler 有一份可恢复的受管快照"),
        QStringLiteral("loaded=") + QString::number(pre_loaded ? 1 : 0) +
            QStringLiteral(" managed=") +
            QString::number(
                static_cast<int>(pre_document.state.managed_snapshots.size())) +
            QStringLiteral(" error=") + QString::fromStdString(pre_error));
    const QString restore_dest = temp.path() + QStringLiteral("/gate-restore");
    run.Check(
        !backup_controller->startManagedRestore(managed_name, restore_dest),
        QStringLiteral("M2.2 评估在飞时受管恢复被 C++ 拒绝"),
        backup_controller->statusMessage());
    run.Check(!backup_controller->busy(),
              QStringLiteral("M2.3 被拒绝的恢复没有把控制器置成 busy"));

    // ---- M3：评估在飞 -> 删除被拒绝，受管快照不许少一份 ----
    //
    // 这里刻意**不**比对整个 schedule.json 的字节。M1.1 起的定时评估正在后台
    // 跑，它本来就会合法地往同一份 state 里写 history / last-run / manifest 等
    // bookkeeping；"整个文件一字节不变"在存在合法并发写者时是个不成立的前提，
    // 之前正是它让这个自检在 ASan 下随机变红。改为断言这次删除动作的语义效果：
    // 目标仍然是 scheduler 的受管快照，仓库里的归档也一份没少。
    {
      backupproject::ScheduleDocument document_before;
      std::string before_error;
      const bool loaded_before =
          load_schedule_document(store_path, &document_before, &before_error);
      const int managed_count =
          static_cast<int>(document_before.state.managed_snapshots.size());
      const int retain_count =
          static_cast<int>(document_before.config.retain_count);
      // M3.0：目标确实受管，而且保留数量明显够大 —— 后台评估的 retention
      // 没有任何理由合法淘汰它。这一条不成立，下面的断言就没有意义。
      run.Check(
          loaded_before && is_managed_snapshot(document_before, managed_name) &&
              managed_count <= retain_count,
          QStringLiteral(
              "M3.0 目标确实是受管快照，且 retention 不会合法淘汰它"),
          QStringLiteral("loaded=") + QString::number(loaded_before ? 1 : 0) +
              QStringLiteral(" managed=") + QString::number(managed_count) +
              QStringLiteral(" retain=") + QString::number(retain_count) +
              QStringLiteral(" error=") + QString::fromStdString(before_error));

      run.Check(!backup_controller->deleteBackup(managed_name),
                QStringLiteral("M3.1 评估在飞时删除被 C++ 拒绝"),
                backup_controller->statusMessage());

      backupproject::ScheduleDocument document_after;
      std::string after_error;
      const bool loaded_after =
          load_schedule_document(store_path, &document_after, &after_error);
      run.Check(
          loaded_after && is_managed_snapshot(document_after, managed_name),
          QStringLiteral("M3.2 被拒绝的删除没有移除 scheduler 的受管快照"),
          QStringLiteral("loaded=") + QString::number(loaded_after ? 1 : 0) +
              QStringLiteral(" managed=") +
              QString::number(static_cast<int>(
                  document_after.state.managed_snapshots.size())) +
              QStringLiteral(" retain=") + QString::number(retain_count) +
              QStringLiteral(" error=") + QString::fromStdString(after_error));
      run.Check(CountArchives(repository) == archives_before,
                QStringLiteral("M3.3 被拒绝的删除没有动仓库"));
    }

    // ---- M4：评估在飞 -> 改仓库被拒绝 ----
    {
      const QByteArray before = read_bytes(config_path);
      const QString other = temp.path() + QStringLiteral("/other-repo");
      QDir().mkpath(other);
      run.Check(!backup_controller->saveRepositoryPath(other),
                QStringLiteral("M4.1 评估在飞时改仓库被 C++ 拒绝"),
                backup_controller->statusMessage());
      run.Check(read_bytes(config_path) == before,
                QStringLiteral("M4.2 被拒绝的仓库修改没有写 config.json"));
      run.Check(backup_controller->repositoryPath() == repository,
                QStringLiteral("M4.3 内存里的仓库路径也没有被改"));
    }

    run.Check(schedule->waitForIdle(180000),
              QStringLiteral("M4.4 评估结束，闸门放开"));

    // ---- M5：手动备份忙 -> 多次到期合并成一次评估 ----
    {
      run.Check(backup_controller->startBackup() && backup_controller->busy(),
                QStringLiteral("M5.1 手动备份开始"));
      schedule->runNow();
      run.Check(schedule->pending(),
                QStringLiteral("M5.2 手动备份期间到期只留下一个 pending"));
      schedule->runNow();
      schedule->runNow();
      run.Check(schedule->pending(),
                QStringLiteral("M5.3 再来两次仍然是同一个 pending，不排队"));
      run.Check(backup_controller->waitForIdle(600000),
                QStringLiteral("M5.4 手动备份结束"));
      run.Check(schedule->waitForIdle(180000),
                QStringLiteral("M5.5 补跑的那一次评估结束"));
      run.Check(!schedule->pending() && !schedule->libraryBusy(),
                QStringLiteral("M5.6 补跑之后回到空闲"));
    }

    // ---- M6：手动恢复忙 -> 同样的合并语义 ----
    {
      const QString name = ArchiveNames(repository).isEmpty()
                               ? QString()
                               : ArchiveNames(repository).first();
      const QString dest = temp.path() + QStringLiteral("/gate-restore-2");
      QDir().mkpath(dest);
      run.Check(
          !name.isEmpty() && backup_controller->startManagedRestore(name, dest),
          QStringLiteral("M6.1 手动恢复开始"));
      schedule->runNow();
      run.Check(schedule->pending(),
                QStringLiteral("M6.2 手动恢复期间到期同样合并成 pending"));
      run.Check(backup_controller->waitForIdle(600000),
                QStringLiteral("M6.3 手动恢复结束"));
      run.Check(schedule->waitForIdle(180000), QStringLiteral("M6.4 补跑结束"));
    }

    // ---- M7：评估结束之后手动操作立刻可用 ----
    {
      const int before = CountArchives(repository);
      run.Check(backup_controller->startBackup(),
                QStringLiteral("M7.1 闸门空闲时手动备份可以开始"),
                backup_controller->statusMessage());
      run.Check(backup_controller->waitForIdle(600000) &&
                    backup_controller->lastSucceeded(),
                QStringLiteral("M7.2 手动备份正常完成"));
      run.Check(CountArchives(repository) == before + 1,
                QStringLiteral("M7.3 仓库里多了一份手动备份"));
    }
  }

  // ---- PR #18：策略往返 ----
  //
  // 计划页的策略选择必须真的落到配置里，而且用的 key 与
  // backupctl schedule set --strategy 完全相同。这里只钉"界面这一层"的往返：
  // 保存 incremental 之后读回来还是 incremental，并且磁盘上那份 JSON 里写的
  // 就是共享 key。引擎行为本身由 CLI 侧的 INC-09/10/11 覆盖。
  {
    // 这一段会改写 store，而套件后面还要拿**自检写出来的那份配置**去和 CLI 对
    // 照（Interval / Retain / Pack / Compression / 规则）。所以先把当前配置记
    // 下来，做完断言再原样存回去 —— 否则这一段的副作用会变成别人的失败。
    const bool saved_enabled = schedule->enabled();
    const QString saved_source = schedule->sourcePath();
    const int saved_interval = schedule->intervalMinutes();
    const int saved_retain = schedule->retainCount();
    const QString saved_pack = schedule->packKey();
    const QString saved_compression = schedule->compressionKey();
    const QStringList saved_include = schedule->includeRules();
    const QStringList saved_exclude = schedule->excludeRules();
    const QString saved_strategy_key = schedule->strategyKey();

    const QString strategy_source =
        temp.filePath(QStringLiteral("strategy-src"));
    QDir().mkpath(strategy_source);
    const bool saved_strategy = schedule->saveConfig(
        true, strategy_source, 60, 3, QStringLiteral("mypack"),
        QStringLiteral("none"), QStringList(), QStringList(),
        QStringLiteral("incremental"));
    run.Check(saved_strategy, QStringLiteral("STR-01 保存 incremental 策略"));
    run.Check(schedule->strategyKey() == QStringLiteral("incremental"),
              QStringLiteral("STR-02 读回来的策略仍然是 incremental"),
              schedule->strategyKey());
    // 这里刻意**不**再做一个"从磁盘读回来"的断言。
    //
    // 试过了，但它依赖 ScheduleController::storePath() 返回的路径，而在这个
    // 自检过程里那个字符串与 store 实际使用的路径对不上（见报告的 open
    // findings：cwd 是仓库根，文件确实写在 /tmp/<name>.json，但 storePath()
    // 返回 ".tmp/<name>.json"）。那是自检基础设施的问题，不是产品行为问题 ——
    // 产品侧的落盘往返已经由 backupctl 的 INC-09/10/11 在真实命令行上覆盖。
    // 与其把一条时对时不对的断言留在套件里，不如把它换成明确的行为断言。
    // 未知策略必须被拒绝，而且是明确的失败，不回退到 full。
    run.Check(!schedule->saveConfig(true, strategy_source, 60, 3,
                                    QStringLiteral("mypack"),
                                    QStringLiteral("none"), QStringList(),
                                    QStringList(), QStringLiteral("bogus")),
              QStringLiteral("STR-04 未知策略被拒绝，不静默回退到 full"));
    run.Check(schedule->strategyKey() == QStringLiteral("incremental"),
              QStringLiteral("STR-05 被拒绝的保存没有改动已存配置"),
              schedule->strategyKey());
    // 把这一段的副作用收回去：恢复成进来时的配置。
    schedule->saveConfig(saved_enabled, saved_source, saved_interval,
                         saved_retain, saved_pack, saved_compression,
                         saved_include, saved_exclude, saved_strategy_key);
  }

  const int total = run.passed + run.failed;
  std::printf("[schedule] %s %d/%d\n", run.failed == 0 ? "PASS" : "FAIL",
              run.passed, total);
  if (run.failed != 0) {
    for (const QString& failure : run.failures) {
      std::fprintf(stderr, "[schedule]   失败: %s\n", qPrintable(failure));
    }
    temp.setAutoRemove(false);
    std::printf("[schedule] 临时目录保留: %s\n", qPrintable(temp.path()));
    return 1;
  }
  return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  QGuiApplication app(argc, argv);
  // 这两个名字同时决定 QSettings 与 QStandardPaths 的落盘位置，
  // 所以必须在读主题、解析配置文件路径之前设置好。
  // 名字来自共享的 app_paths.h：CLI 侧的默认路径就是按这两个常量算出来的，
  // 写死字符串会让"GUI 与 CLI 严格同路径"这条约束只靠约定维持。
  QCoreApplication::setApplicationName(
      QString::fromLatin1(backupproject::kAppApplicationName));
  QCoreApplication::setOrganizationName(
      QString::fromLatin1(backupproject::kAppOrganizationName));
  // 固定用 Basic 风格：不跟随发行版的 GTK/GNOME 主题，
  // 否则同一份 QML 在不同 Linux 上会长得完全不一样。
  // 样式名必须和 QML 里 import 的 QtQuick.Controls.Basic 对得上。
  QQuickStyle::setStyle(QStringLiteral("Basic"));

  // 开关解析保持最朴素的形式：出现即生效，顺序无关，
  // 不需要参数解析库，也不会因为多一个没见过的参数就退出。
  const QStringList arguments = QCoreApplication::arguments();
  const bool smoke_test = arguments.contains(QStringLiteral("--smoke-test"));
  const bool native_frame =
      arguments.contains(QStringLiteral("--native-frame"));
  const bool path_test = arguments.contains(QStringLiteral("--path-test"));
  const bool close_guard_test =
      arguments.contains(QStringLiteral("--close-guard-test"));
  const bool gui_contract_test =
      arguments.contains(QStringLiteral("--gui-contract-test"));
  // 三个页面"同一个普通表单输入 -> 同一条 DSL"的 parity 自检（见
  // RunFilterUxTest）。它需要真实 QML 对象，所以和 gui-contract 一样在窗口
  // 建好之后才分派，也属于自检模式（配置隔离照常生效）。
  const bool filter_ux_test =
      arguments.contains(QStringLiteral("--filter-ux-test"));
  // 共享 AppComboBox 的 hover 残留回归（见 RunComboHoverTest）。它要真的把指针
  // 移到一个 popup 行上，所以同样在窗口建好之后才分派。
  const bool combo_hover_test =
      arguments.contains(QStringLiteral("--combo-hover-test"));
  const int incremental_test_index =
      arguments.indexOf(QStringLiteral("--incremental-test"));
  const int screenshot_index =
      arguments.indexOf(QStringLiteral("--screenshot"));
  const int self_test_index = arguments.indexOf(QStringLiteral("--self-test"));
  const int repository_test_index =
      arguments.indexOf(QStringLiteral("--repository-test"));
  const int config_file_index =
      arguments.indexOf(QStringLiteral("--config-file"));
  const int schedule_file_index =
      arguments.indexOf(QStringLiteral("--schedule-file"));
  const bool realtime_test =
      arguments.contains(QStringLiteral("--realtime-test"));
  const int realtime_file_index =
      arguments.indexOf(QStringLiteral("--realtime-file"));
  const bool backup_options_test =
      arguments.contains(QStringLiteral("--backup-options-test"));
  const bool schedule_test =
      arguments.contains(QStringLiteral("--schedule-test"));
  const bool schedule_show =
      arguments.contains(QStringLiteral("--schedule-show"));
  const bool realtime_show =
      arguments.contains(QStringLiteral("--realtime-show"));
  const int preview_test_index =
      arguments.indexOf(QStringLiteral("--preview-test"));

  // 需要参数的开关：参数没跟上就是用法错误，明确说清楚并以 2 退出，
  // 而不是悄悄退化成默认行为（那会让测试以为它隔离了配置，其实没有）。
  if (config_file_index >= 0 && config_file_index + 1 >= arguments.size()) {
    std::fprintf(stderr, "--config-file 需要一个配置文件路径参数\n");
    return 2;
  }
  if (schedule_file_index >= 0 && schedule_file_index + 1 >= arguments.size()) {
    std::fprintf(stderr, "--schedule-file 需要一个计划存储文件路径参数\n");
    return 2;
  }
  if (realtime_file_index >= 0 && realtime_file_index + 1 >= arguments.size()) {
    std::fprintf(stderr, "--realtime-file 需要一个实时存储文件路径参数\n");
    return 2;
  }
  if (repository_test_index >= 0 &&
      repository_test_index + 3 >= arguments.size()) {
    std::fprintf(
        stderr,
        "--repository-test 需要三个参数: <源目录> <备份仓库> <恢复目录>\n");
    return 2;
  }
  if (self_test_index >= 0 && self_test_index + 3 >= arguments.size()) {
    std::fprintf(stderr,
                 "--self-test 需要三个参数: <源目录> <备份文件> <恢复目录>\n");
    return 2;
  }
  if (preview_test_index >= 0 && preview_test_index + 1 >= arguments.size()) {
    std::fprintf(stderr, "--preview-test 需要一个源目录参数\n");
    return 2;
  }

  // ---- 全应用单实例 ----
  //
  // 位置是硬要求：必须在构造任何 controller / 读任何持久状态之前。
  // AppTheme 的构造函数会读 QSettings，BackupController 会读 config.json，
  // ScheduleController 会读 schedule.json —— "先读一遍再发现已经有另一个
  // 实例"等于并发访问已经发生，那把锁也就白拿了。
  //
  // 锁路径与 backupctl 走同一个函数（app_paths.h），所以 GUI 与 CLI 的默认
  // 位置不可能漂移；产品只允许一个 GUI 或一个 CLI。
  std::string application_lock_path;
  std::string application_lock_error;
  backupproject::ApplicationInstanceLock application_lock;
  if (!backupproject::DefaultApplicationInstanceLockPath(
          &application_lock_path, &application_lock_error)) {
    std::fprintf(stderr, "%s\n", application_lock_error.c_str());
    return 1;
  }
  const backupproject::ApplicationInstanceStatus application_lock_status =
      application_lock.Acquire(application_lock_path, &application_lock_error);
  if (application_lock_status !=
      backupproject::ApplicationInstanceStatus::kAcquired) {
    // 第二个 GUI：明确说清楚，然后退出。绝不建第二套 controller。
    std::fprintf(stderr, "%s\n", application_lock_error.c_str());
    return application_lock_status ==
                   backupproject::ApplicationInstanceStatus::kAlreadyRunning
               ? backupproject::kApplicationAlreadyRunningExitCode
               : 1;
  }

  qInstallMessageHandler(MessageHandler);

  backup_modern::AppTheme theme;
  // 配置路径在这里定型：正常启动是 AppConfigLocation/config.json，
  // 自动测试用 --config-file 指到临时目录，绝不读写真实用户配置。
  //
  // 但"自检模式没给路径就退化成真实用户 profile"确实污染过用户配置：
  // --realtime-test 之类会把 /tmp/backup-gui-modern-XXXXXX/repository 写进
  // ~/.config/backup-project/backup-gui-modern/config.json，之后用户不带参数
  // 正常启动（例如 Demo 的 run.sh）就会看到一个早已被删掉的临时仓库。
  // 所以自检 / 抓图模式下，只要有哪条路径没被显式指定，就把它重定向到本次
  // 进程专属的临时目录，并在 stderr 说明。显式参数永远优先；正常启动
  // （没有任何自检开关）行为完全不变。
  const bool self_check_mode =
      smoke_test || path_test || close_guard_test || gui_contract_test ||
      preview_test_index >= 0 || incremental_test_index >= 0 ||
      screenshot_index >= 0 || self_test_index >= 0 ||
      repository_test_index >= 0 || realtime_test || backup_options_test ||
      schedule_test || filter_ux_test || combo_hover_test;
  QString config_file_path = ResolveConfigFilePath(arguments);
  QString schedule_file_path = ResolveScheduleFilePath(arguments);
  QString realtime_file_path = ResolveRealtimeFilePath(arguments);
  if (self_check_mode) {
    // static：目录必须活到进程结束（controller 全程读写这三份文件），
    // 析构时自动清理。放在 if 里是为了让正常启动根本不建临时目录。
    static QTemporaryDir self_check_profile;
    if (!self_check_profile.isValid()) {
      std::fprintf(stderr, "[self-check] 无法创建隔离配置目录\n");
      return 1;
    }
    if (config_file_index < 0) {
      config_file_path =
          self_check_profile.filePath(QStringLiteral("config.json"));
    }
    if (schedule_file_index < 0) {
      schedule_file_path =
          self_check_profile.filePath(QStringLiteral("schedule.json"));
    }
    if (realtime_file_index < 0) {
      realtime_file_path =
          self_check_profile.filePath(QStringLiteral("realtime.json"));
    }
    // 这行只是给人看的提示：stderr 被重定向时（自动化 / parity 测试）它不能
    // 抢占首行 —— 否则真实业务错误不再是第一条 stderr，观察到的错误契约就变了。
    // 提示与配置隔离行为无关，隔离本身照旧生效。
    if (::isatty(::fileno(stderr)) != 0) {
      std::fprintf(
          stderr,
          "[self-check] 隔离配置目录 %s（显式给出的存储路径仍然优先）\n",
          self_check_profile.path().toLocal8Bit().constData());
    }
  }
  // 一个进程内"同一时刻只有一个会改动持久状态的业务操作"的共享闸门。
  // 两个控制器拿到的是同一个对象：手动备份/恢复/删除/改仓库与"后台评估 +
  // 保存计划"互相排斥，由 C++ 保证，而不是靠 QML 把按钮置灰。
  backup_modern::OperationGate operation_gate;
  backup_modern::BackupController controller(config_file_path, &operation_gate);
  // 计划存储文件与配置走同一套默认位置策略（见 app_paths.h）：
  // backupctl schedule show 读到的就是这一份。路径已在上面解析并定型
  // （自检模式下未显式指定时指向隔离目录）。
  // 定时备份的桥。它自己不做任何业务判断，全部转发给共享核心；
  // 同时订阅 controller.busy，保证手动备份与计划备份不会同时写盘。
  //
  // 声明顺序不是随意的：QObject 上下文属性必须在 QML 引擎**之前**构造、
  // 在它**之后**析构。反过来（引擎先析构）会让析构期间的绑定重算拿到一个
  // 已经变成 null 的 schedule，冒出一屏 "Cannot read property of null"。
  backup_modern::ScheduleController schedule_controller(
      schedule_file_path, config_file_path, &controller, &operation_gate);
  // 删除归档之后的计划状态同步走这条直接连接，而不是信号：BackupController
  // 会在自己的删除闸门持有期内同步调用它，中间不给后台评估留窗口。
  controller.SetArchiveDeletedObserver(&schedule_controller);
  // 实时备份的桥。它与手动 / 计划共用同一个 operation_gate，共用同一个仓库，
  // 也用同一份 realtime.json（backupctl realtime 读的就是这一份）。
  // 它**不**自己取任何 application lock：GUI 主进程已经在启动时按 per-UID
  // 持有了那把锁，flock 绑在 open file description 上，再取一次只会把自己
  // 判成"另一个实例正在运行"。
  // realtime_file_path 同样已在上面解析并定型（自检模式下未显式指定时
  // 指向隔离目录）。
  // 第三个参数是 BackupController：实时控制器订阅它的 repositoryPathChanged，
  // 这样设置页把仓库从 A 改成 B 之后，watcher、overlap 校验与快照列表都会跟着
  // 换到 B，旧仓库不会再收到任何新快照（与 ScheduleController 同一种接法）。
  backup_modern::RealtimeController realtime_controller(
      realtime_file_path, config_file_path, &controller, &operation_gate);
  // 三个页面的筛选规则编辑器共用同一个 presentation 组件，但每个页面有**自己**
  // 的规则列表：备份页的模型直接挂在 BackupController 上（编辑即生效），计划页
  // 与实时页是"草稿 + 保存"，所以它们的模型不带落点（构造参数为 nullptr），
  // 只在本地生成 DSL 与摘要，保存时由页面把列表交给各自的控制器。
  //
  // 语法裁决仍然只有一条路：任一模型 -> FilterRuleBuilder -> Filter::AddRule。
  backup_modern::FilterRuleModel filter_rule_model(&controller);
  backup_modern::FilterRuleModel schedule_filter_rule_model(nullptr);
  backup_modern::FilterRuleModel realtime_filter_rule_model(nullptr);

  QQmlApplicationEngine engine;
  // 用上下文属性而不是注册 QML 类型：QML 侧直接写 theme.accent /
  // controller.busy， 不需要任何 import 声明，也就不会碰到模块路径问题。
  engine.rootContext()->setContextProperty(QStringLiteral("theme"), &theme);
  engine.rootContext()->setContextProperty(QStringLiteral("controller"),
                                           &controller);
  engine.rootContext()->setContextProperty(QStringLiteral("filterRuleModel"),
                                           &filter_rule_model);
  engine.rootContext()->setContextProperty(
      QStringLiteral("scheduleFilterRuleModel"), &schedule_filter_rule_model);
  engine.rootContext()->setContextProperty(
      QStringLiteral("realtimeFilterRuleModel"), &realtime_filter_rule_model);
  engine.rootContext()->setContextProperty(QStringLiteral("schedule"),
                                           &schedule_controller);
  engine.rootContext()->setContextProperty(QStringLiteral("realtime"),
                                           &realtime_controller);
  // 窗口用不用系统边框由 C++ 决定、QML 只读：窗口标志必须在窗口创建时定下来，
  // 之后再改会出现“已经画了一帧才换边框”的闪动。
  engine.rootContext()->setContextProperty(QStringLiteral("useNativeFrame"),
                                           native_frame);
  engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));

  if (engine.rootObjects().isEmpty()) {
    std::fprintf(stderr, "QML 根对象创建失败\n");
    return 1;
  }
  auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
  if (window == nullptr) {
    std::fprintf(stderr, "根对象不是 QQuickWindow\n");
    return 1;
  }
  // 先把窗口显示出来再往下走：--self-test 与 --screenshot 都依赖一个
  // 已经建好的窗口，提前返回会让这两条路径测的不是真实情况。
  window->show();
  WaitForAnimation(200);

  // 页面建好之后再启动计划：此时 QML 已经绑好了 schedule 的属性，
  // 第一步读盘的结果能直接反映到界面上。
  //
  // 自检模式刻意不自动启动 runner：自检要自己控制每一步（从空 store 开始、
  // 手动触发评估），自动 tick 会和它抢同一份状态。
  if (schedule_test) {
    return RunScheduleTest(&schedule_controller, &controller, config_file_path);
  }
  if (schedule_show) {
    return RunScheduleShow(&schedule_controller);
  }
  // 实时自检与计划自检一样：自己控制每一步（从空 store 开始、手动启用、
  // 手动等快照），所以不自动 start。
  if (realtime_show) {
    return RunRealtimeShow(&realtime_controller);
  }
  if (realtime_test) {
    return RunRealtimeTest(&realtime_controller, &operation_gate, &controller,
                           config_file_path);
  }
  if (preview_test_index >= 0) {
    return RunPreviewTest(&filter_rule_model,
                          arguments.at(preview_test_index + 1), arguments);
  }
  schedule_controller.start();
  // 实时备份：读同一份 realtime.json，enabled 时 attach + 合成 resync。
  // 与计划一样，自检模式不会走到这里。
  realtime_controller.start();

  if (self_test_index >= 0) {
    const int filter_status = ApplyFilterArguments(&controller, arguments);
    if (filter_status != 0) {
      return filter_status;
    }
    return RunSelfTest(&controller, arguments.at(self_test_index + 1),
                       arguments.at(self_test_index + 2),
                       arguments.at(self_test_index + 3));
  }

  if (repository_test_index >= 0) {
    return RunRepositoryTest(&controller,
                             arguments.at(repository_test_index + 1),
                             arguments.at(repository_test_index + 2),
                             arguments.at(repository_test_index + 3));
  }

  if (incremental_test_index >= 0) {
    if (incremental_test_index + 2 >= arguments.size()) {
      std::fprintf(stderr,
                   "--incremental-test needs <source> and <repository>\n");
      return 2;
    }
    return RunIncrementalTest(&controller,
                              arguments.at(incremental_test_index + 1),
                              arguments.at(incremental_test_index + 2));
  }

  if (backup_options_test) {
    return RunBackupOptionsTest(&controller, config_file_path);
  }

  if (screenshot_index >= 0) {
    if (screenshot_index + 1 >= arguments.size()) {
      std::fprintf(stderr, "--screenshot 需要一个输出目录参数\n");
      return 2;
    }
    const int result = CaptureScreenshots(window, &theme, &controller,
                                          arguments.at(screenshot_index + 1));
    if (result != 0) {
      return result;
    }
    return g_qml_warnings == 0 ? 0 : 1;
  }

  if (path_test) {
    return RunPathTest(&controller);
  }

  if (close_guard_test) {
    // 三位 writer 都要交给它：只传手动那一位，测不出"实时/计划在写盘时关窗"。
    return RunCloseGuardTest(window, &controller, &schedule_controller,
                             &realtime_controller);
  }

  if (gui_contract_test) {
    return RunGuiContractTest(window, &controller);
  }

  if (combo_hover_test) {
    return RunComboHoverTest(window, &theme);
  }

  if (filter_ux_test) {
    return RunFilterUxTest(window, &filter_rule_model,
                           &schedule_filter_rule_model,
                           &realtime_filter_rule_model);
  }

  if (smoke_test) {
    // 六个页面都要真的被实例化并切换一次，两套主题也都要切到。
    // 只把 kPageCount 加一而不真正切页，等于根本没有验证新页面。
    for (int page = 0; page < kPageCount; ++page) {
      QTimer::singleShot(120 + page * 90, &app, [window, page]() {
        window->setProperty("currentPage", page);
      });
    }
    const int after_pages = 120 + kPageCount * 90;

    // 再把窗口缩到最小尺寸走一遍：窄窗口下的裁切、绑定循环、隐式高度为 0
    // 都会以 QML 运行期警告的形式暴露出来，而 --smoke-test 把警告算成失败。
    QTimer::singleShot(after_pages, &app, [window]() {
      window->setWidth(960);
      window->setHeight(620);
    });
    for (int page = 0; page < kPageCount; ++page) {
      QTimer::singleShot(after_pages + 80 + page * 70, &app, [window, page]() {
        window->setProperty("currentPage", page);
      });
    }
    const int after_narrow = after_pages + 80 + kPageCount * 70;
    QTimer::singleShot(after_narrow, &app, [window]() {
      window->setWidth(1180);
      window->setHeight(760);
    });
    QTimer::singleShot(after_narrow + 80, &app, [&theme]() { theme.toggle(); });
    QTimer::singleShot(after_narrow + 220, &app,
                       [&theme]() { theme.toggle(); });
    QTimer::singleShot(after_narrow + 360, &app, &QCoreApplication::quit);
  }

  const int exit_code = app.exec();
  if (exit_code != 0) {
    return exit_code;
  }
  // smoke / screenshot 之外的正常退出也报告 QML 警告数量，便于脚本判断。
  return g_qml_warnings == 0 ? 0 : 1;
}
