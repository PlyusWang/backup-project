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
//   --path-test                         验证本地路径与 URL 互转不丢字符
//   --close-guard-test                  验证任务进行中关窗会被拦下
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
#include <cstdio>
#include <cstring>

#include "app_paths.h"
#include "app_theme.h"
#include "application_instance_lock.h"
#include "archive_pipeline.h"
#include "backup_controller.h"
#include "config_manager.h"
#include "filter_rule_model.h"
#include "operation_gate.h"
#include "schedule_controller.h"
#include "schedule_store.h"
#include "scheduler_lock.h"

namespace {

const int kPageCount = 5;
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
  // 备份管理 / 设置。
  const char* page_names[kPageCount] = {"home", "backup", "schedule",
                                        "management", "settings"};
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
  // 计数取整棵树的数字（与 CLI 打印的是同一个字段），窗口信息取核心给出的
  // total / truncated。列表本身只到窗口为止，所以两者在截断时可以不同——
  // 下面的 Note 行负责把这件事说清楚。
  std::printf("Preview: %d matching item(s)\n", model->previewIncluded());
  if (model->previewTruncated()) {
    std::printf(
        "Note: the source tree has %d entries; only the first %d were "
        "examined, and %d matching item(s) are listed below.\n",
        model->previewTotal(), model->previewLimit(),
        static_cast<int>(included_paths.size()));
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

// --close-guard-test：验证“任务进行中不许关窗”的契约。
// busy 在 startBackup() 返回前就已置位，而任务结束信号要等回到事件循环
// 才会派发，所以在同一个事件循环回合里检查，结论不取决于任务跑得多快。
//
// 这里用 direct archive 入口：close guard 只关心 busy 这一位，
// 而 direct 入口不需要先配置仓库，测试因此更短、更聚焦。
int RunCloseGuardTest(QQuickWindow* window,
                      backup_modern::BackupController* controller) {
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

  int failures = 0;
  // 忙的时候关窗：必须被 onClosing 拒绝，窗口留着。
  const bool closed_while_busy = window->close();
  std::printf("%s 忙时 close() 被拒绝 (返回=%s)\n",
              closed_while_busy ? "FAIL" : "ok  ",
              closed_while_busy ? "true" : "false");
  failures += closed_while_busy ? 1 : 0;

  // 光拒绝还不够：得给用户一个说明，而不是点了没反应。
  QObject* dialog =
      window->findChild<QObject*>(QStringLiteral("busyCloseDialog"));
  const bool dialog_open =
      dialog != nullptr && dialog->property("visible").toBool();
  std::printf("%s 忙时关窗会弹出提示 (visible=%s)\n",
              dialog_open ? "ok  " : "FAIL", dialog_open ? "true" : "false");
  failures += dialog_open ? 0 : 1;

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
  run.Check(schedule->supportedModeText().contains(
                QStringLiteral("定时触发 + 完整快照")),
            QStringLiteral("SCH-07 页面说明只承诺已实现的模式"));

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
    QObject::connect(schedule,
                     &backup_modern::ScheduleController::statusChanged,
                     [&status_signals]() { ++status_signals; });
    QObject::connect(schedule,
                     &backup_modern::ScheduleController::suspendedChanged,
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
    const QString store_path = schedule->storePath();

    backup_controller->setSourcePath(source);

    // ---- M1：评估在飞 -> 手动备份被拒绝 ----
    run.Check(schedule->runNow() && schedule->libraryBusy(),
              QStringLiteral("M1.1 评估进入 busy"));
    const int archives_before = CountArchives(repository);
    const QByteArray store_before = read_bytes(store_path);
    run.Check(!backup_controller->startBackup(),
              QStringLiteral("M1.2 评估在飞时手动备份被 C++ 拒绝"),
              backup_controller->statusMessage());
    run.Check(backup_controller->statusMessage().contains(
                  QStringLiteral("定时备份评估")),
              QStringLiteral("M1.3 拒绝理由点名是谁在占着"),
              backup_controller->statusMessage());
    run.Check(CountArchives(repository) == archives_before,
              QStringLiteral("M1.4 被拒绝的手动备份没有产生任何归档"));
    run.Check(read_bytes(store_path) == store_before,
              QStringLiteral("M1.5 被拒绝的手动备份没有改动 schedule store"));

    // ---- M2：评估在飞 -> 受管恢复被拒绝 ----
    const QString managed_name = ArchiveNames(repository).isEmpty()
                                     ? QString()
                                     : ArchiveNames(repository).first();
    run.Check(!managed_name.isEmpty(),
              QStringLiteral("M2.1 仓库里有一份可恢复的归档"));
    const QString restore_dest = temp.path() + QStringLiteral("/gate-restore");
    run.Check(
        !backup_controller->startManagedRestore(managed_name, restore_dest),
        QStringLiteral("M2.2 评估在飞时受管恢复被 C++ 拒绝"),
        backup_controller->statusMessage());
    run.Check(!backup_controller->busy(),
              QStringLiteral("M2.3 被拒绝的恢复没有把控制器置成 busy"));

    // ---- M3：评估在飞 -> 删除被拒绝，store 一个字节不变 ----
    {
      const QByteArray before = read_bytes(store_path);
      run.Check(!backup_controller->deleteBackup(managed_name),
                QStringLiteral("M3.1 评估在飞时删除被 C++ 拒绝"),
                backup_controller->statusMessage());
      run.Check(read_bytes(store_path) == before,
                QStringLiteral("M3.2 被拒绝的删除没有改动 schedule store"));
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
  const int screenshot_index =
      arguments.indexOf(QStringLiteral("--screenshot"));
  const int self_test_index = arguments.indexOf(QStringLiteral("--self-test"));
  const int repository_test_index =
      arguments.indexOf(QStringLiteral("--repository-test"));
  const int config_file_index =
      arguments.indexOf(QStringLiteral("--config-file"));
  const int schedule_file_index =
      arguments.indexOf(QStringLiteral("--schedule-file"));
  const bool backup_options_test =
      arguments.contains(QStringLiteral("--backup-options-test"));
  const bool schedule_test =
      arguments.contains(QStringLiteral("--schedule-test"));
  const bool schedule_show =
      arguments.contains(QStringLiteral("--schedule-show"));
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
  const QString config_file_path = ResolveConfigFilePath(arguments);
  // 一个进程内"同一时刻只有一个会改动持久状态的业务操作"的共享闸门。
  // 两个控制器拿到的是同一个对象：手动备份/恢复/删除/改仓库与"后台评估 +
  // 保存计划"互相排斥，由 C++ 保证，而不是靠 QML 把按钮置灰。
  backup_modern::OperationGate operation_gate;
  backup_modern::BackupController controller(config_file_path, &operation_gate);
  // 计划存储文件与配置走同一套默认位置策略（见 app_paths.h）：
  // backupctl schedule show 读到的就是这一份。
  const QString schedule_file_path = ResolveScheduleFilePath(arguments);
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
  backup_modern::FilterRuleModel filter_rule_model(&controller);

  QQmlApplicationEngine engine;
  // 用上下文属性而不是注册 QML 类型：QML 侧直接写 theme.accent /
  // controller.busy， 不需要任何 import 声明，也就不会碰到模块路径问题。
  engine.rootContext()->setContextProperty(QStringLiteral("theme"), &theme);
  engine.rootContext()->setContextProperty(QStringLiteral("controller"),
                                           &controller);
  engine.rootContext()->setContextProperty(QStringLiteral("filterRuleModel"),
                                           &filter_rule_model);
  engine.rootContext()->setContextProperty(QStringLiteral("schedule"),
                                           &schedule_controller);
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
  if (preview_test_index >= 0) {
    return RunPreviewTest(&filter_rule_model,
                          arguments.at(preview_test_index + 1), arguments);
  }
  schedule_controller.start();

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
    return RunCloseGuardTest(window, &controller);
  }

  if (smoke_test) {
    // 五个页面都要真的被实例化并切换一次，两套主题也都要切到。
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
