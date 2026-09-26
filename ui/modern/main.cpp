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
//   --config-file <路径>                指定配置文件（测试隔离真实用户配置）
//   --path-test                         验证本地路径与 URL 互转不丢字符
//   --close-guard-test                  验证任务进行中关窗会被拦下
//   --native-frame                      退回系统原生标题栏（Wayland 兜底）
//
// 这些开关让没有显示器的环境也能验证界面：离屏平台插件把窗口真正建出来，
// 自检再切一遍页面、换一次主题、跑一次备份恢复，不需要人盯着屏幕。

#include <sys/stat.h>
#include <unistd.h>

#include <QDir>
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

#include "app_theme.h"
#include "archive_pipeline.h"
#include "backup_controller.h"
#include "filter_rule_model.h"

namespace {

const int kPageCount = 4;
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
  const QString directory =
      QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
  if (directory.isEmpty()) {
    return QString();
  }
  return QDir(directory).filePath(QStringLiteral("config.json"));
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

// --screenshot：四个页面 × 两套主题各抓一张 PNG，另外补两种状态：
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

  const char* page_names[kPageCount] = {"home", "backup", "management",
                                        "settings"};
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
  int passed = 0;
  int failed = 0;
  QStringList failures;

  void Check(bool ok, const QString& label, const QString& detail = QString()) {
    if (ok) {
      ++passed;
      std::printf("[backup-options]   ok   %s\n", qPrintable(label));
      return;
    }
    ++failed;
    const QString text = detail.isEmpty()
                             ? label
                             : QStringLiteral("%1（%2）").arg(label, detail);
    failures.append(text);
    std::printf("[backup-options]   FAIL %s\n", qPrintable(text));
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

}  // namespace

int main(int argc, char* argv[]) {
  QGuiApplication app(argc, argv);
  // 这两个名字同时决定 QSettings 与 QStandardPaths 的落盘位置，
  // 所以必须在读主题、解析配置文件路径之前设置好。
  QCoreApplication::setApplicationName("backup-gui-modern");
  QCoreApplication::setOrganizationName("backup-project");
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
  const bool backup_options_test =
      arguments.contains(QStringLiteral("--backup-options-test"));

  // 需要参数的开关：参数没跟上就是用法错误，明确说清楚并以 2 退出，
  // 而不是悄悄退化成默认行为（那会让测试以为它隔离了配置，其实没有）。
  if (config_file_index >= 0 && config_file_index + 1 >= arguments.size()) {
    std::fprintf(stderr, "--config-file 需要一个配置文件路径参数\n");
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

  qInstallMessageHandler(MessageHandler);

  backup_modern::AppTheme theme;
  // 配置路径在这里定型：正常启动是 AppConfigLocation/config.json，
  // 自动测试用 --config-file 指到临时目录，绝不读写真实用户配置。
  const QString config_file_path = ResolveConfigFilePath(arguments);
  backup_modern::BackupController controller(config_file_path);

  QQmlApplicationEngine engine;
  // 用上下文属性而不是注册 QML 类型：QML 侧直接写 theme.accent /
  // controller.busy， 不需要任何 import 声明，也就不会碰到模块路径问题。
  engine.rootContext()->setContextProperty(QStringLiteral("theme"), &theme);
  backup_modern::FilterRuleModel filter_rule_model(&controller);
  engine.rootContext()->setContextProperty(QStringLiteral("controller"),
                                           &controller);
  engine.rootContext()->setContextProperty(QStringLiteral("filterRuleModel"),
                                           &filter_rule_model);
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
    // 四个页面都要真的被实例化并切换一次，两套主题也都要切到。
    // 只把 kPageCount 改成 4 而不真正切页，等于根本没有验证新页面。
    for (int page = 0; page < kPageCount; ++page) {
      QTimer::singleShot(120 + page * 90, &app, [window, page]() {
        window->setProperty("currentPage", page);
      });
    }
    const int after_pages = 120 + kPageCount * 90;
    QTimer::singleShot(after_pages, &app, [&theme]() { theme.toggle(); });
    QTimer::singleShot(after_pages + 120, &app, [&theme]() { theme.toggle(); });
    QTimer::singleShot(after_pages + 260, &app, &QCoreApplication::quit);
  }

  const int exit_code = app.exec();
  if (exit_code != 0) {
    return exit_code;
  }
  // smoke / screenshot 之外的正常退出也报告 QML 警告数量，便于脚本判断。
  return g_qml_warnings == 0 ? 0 : 1;
}
