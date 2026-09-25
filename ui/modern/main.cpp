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
//   --config-file <路径>                指定配置文件（测试隔离真实用户配置）
//   --path-test                         验证本地路径与 URL 互转不丢字符
//   --close-guard-test                  验证任务进行中关窗会被拦下
//   --native-frame                      退回系统原生标题栏（Wayland 兜底）
//
// 这些开关让没有显示器的环境也能验证界面：离屏平台插件把窗口真正建出来，
// 自检再切一遍页面、换一次主题、跑一次备份恢复，不需要人盯着屏幕。

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
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

// --screenshot：四个页面 × 两套主题各抓一张 PNG。
// 抓帧走窗口自己的 grabWindow()，和用户看到的是同一条渲染路径，
// 不是另画一份示意图。
int CaptureScreenshots(QQuickWindow* window, backup_modern::AppTheme* theme,
                       const QString& directory) {
  if (!QDir().mkpath(directory)) {
    std::fprintf(stderr, "无法创建截图目录: %s\n", qPrintable(directory));
    return 1;
  }
  // 文件名固定成 页面-主题.png，方便文档和脚本按名字引用，
  // 不用在文档里写死带时间戳的路径。
  const char* page_names[kPageCount] = {"home", "backup", "management",
                                        "settings"};
  for (int dark = 0; dark < 2; ++dark) {
    theme->setDark(dark == 1);
    for (int page = 0; page < kPageCount; ++page) {
      window->setProperty("currentPage", page);
      WaitForAnimation(300);
      const QImage image = window->grabWindow();
      if (image.isNull()) {
        std::fprintf(stderr, "grabWindow() 返回空图像\n");
        return 1;
      }
      const QString path =
          QStringLiteral("%1/%2-%3.png")
              .arg(
                  directory, QString::fromLatin1(page_names[page]),
                  dark == 1 ? QStringLiteral("dark") : QStringLiteral("light"));
      if (!image.save(path)) {
        std::fprintf(stderr, "截图保存失败: %s\n", qPrintable(path));
        return 1;
      }
      std::printf("screenshot: %s\n", qPrintable(path));
    }
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

  if (screenshot_index >= 0) {
    if (screenshot_index + 1 >= arguments.size()) {
      std::fprintf(stderr, "--screenshot 需要一个输出目录参数\n");
      return 2;
    }
    const int result =
        CaptureScreenshots(window, &theme, arguments.at(screenshot_index + 1));
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
