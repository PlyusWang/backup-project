// backup_controller.cpp
//
// 桥的实现。刻意保持很薄：真正的备份/恢复只有一行调用，其余代码都在处理
// “界面该显示什么”，以及把 repository 相关的核心调用按正确顺序编排起来。

#include "backup_controller.h"

#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <string>
#include <utility>
#include <vector>

#include "backup_engine.h"

namespace backup_modern {

namespace {

// 这四个字符串本身也是接口的一部分：改名要同时改 StatusBanner.qml。
// 状态种类，QML 用它决定 banner 的颜色和图标。
const char kIdle[] = "idle";
const char kRunning[] = "running";
const char kSuccess[] = "success";
const char kError[] = "error";

// 文件大小的展示文本。格式化放在 C++ 这层，QML 不需要自己实现一套单位换算。
QString FormatSize(std::uint64_t bytes) {
  constexpr double kKilo = 1024.0;
  constexpr double kMega = kKilo * 1024.0;
  constexpr double kGiga = kMega * 1024.0;
  const double value = static_cast<double>(bytes);
  if (value < kKilo) {
    return QStringLiteral("%1 B").arg(static_cast<qulonglong>(bytes));
  }
  if (value < kMega) {
    return QStringLiteral("%1 KB").arg(value / kKilo, 0, 'f', 1);
  }
  if (value < kGiga) {
    return QStringLiteral("%1 MB").arg(value / kMega, 0, 'f', 1);
  }
  return QStringLiteral("%1 GB").arg(value / kGiga, 0, 'f', 2);
}

// 归档文件自身的 mtime。archive v0.1 里没有 created_at 这类字段，
// 所以界面上只能、也只允许把它叫作“文件修改时间”。
QString FormatModifiedTime(std::int64_t seconds) {
  return QDateTime::fromSecsSinceEpoch(static_cast<qint64>(seconds))
      .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

// BackupRecord -> QML 用的 QVariantMap。
//
// 刻意不包含 archive_path：管理页上的恢复与删除都只传 file name，再由
// BackupCatalog::Resolve / Delete 去解析并校验。给 QML 一个完整路径，
// 等于把一份可以绕过校验直接喂给核心的输入交到界面层。
QVariantMap RecordToVariant(const backupproject::BackupRecord& record) {
  QVariantMap item;
  item.insert(QStringLiteral("fileName"),
              QString::fromStdString(record.file_name));
  item.insert(QStringLiteral("sizeBytes"),
              static_cast<qulonglong>(record.archive_size));
  item.insert(QStringLiteral("sizeText"), FormatSize(record.archive_size));
  item.insert(QStringLiteral("modifiedTimeSec"),
              static_cast<qlonglong>(record.modified_time_sec));
  item.insert(QStringLiteral("modifiedTimeText"),
              FormatModifiedTime(record.modified_time_sec));
  // recognizedArchive 只表示“全局 header 被当前实现认得”，
  // 不代表归档完整、更不代表能恢复成功。界面上不要把它写成“校验通过”。
  item.insert(QStringLiteral("recognizedArchive"), record.recognized_archive);
  item.insert(QStringLiteral("formatVersion"),
              static_cast<int>(record.format_version));
  item.insert(QStringLiteral("entryCount"),
              static_cast<qulonglong>(record.entry_count));
  item.insert(QStringLiteral("diagnostic"),
              QString::fromStdString(record.diagnostic));
  return item;
}

QVariantList RecordsToVariantList(
    const std::vector<backupproject::BackupRecord>& records) {
  QVariantList list;
  list.reserve(static_cast<int>(records.size()));
  for (const backupproject::BackupRecord& record : records) {
    list.append(RecordToVariant(record));
  }
  return list;
}

}  // namespace

BackupController::BackupController(const QString& config_file_path,
                                   QObject* parent)
    : QObject(parent), config_manager_(config_file_path.toStdString()) {
  // 两个 watcher 都以 this 为上下文：对象销毁时连接自动断开，后台任务即使还在
  // 跑也不会回调到已经释放的控制器上。
  connect(
      &watcher_, &QFutureWatcher<OperationOutcome>::finished, this, [this]() {
        const OperationOutcome outcome = watcher_.result();
        const Kind kind = active_kind_;
        const QString file_name = active_file_name_;
        last_succeeded_ = outcome.succeeded;
        SetBusy(false);
        if (outcome.succeeded) {
          if (kind == Kind::kBackup) {
            SetStatus(QString::fromLatin1(kSuccess), QStringLiteral("备份完成"),
                      file_name.isEmpty()
                          ? QStringLiteral("备份文件已写入备份仓库。")
                          : QStringLiteral("已保存为 %1").arg(file_name));
            // 仓库里多了一个文件，管理页必须能看到它。
            refreshBackups();
          } else {
            SetStatus(
                QString::fromLatin1(kSuccess), QStringLiteral("恢复完成"),
                file_name.isEmpty()
                    ? QStringLiteral("目录已恢复到目标位置。")
                    : QStringLiteral("%1 已恢复到目标目录。").arg(file_name));
            // 恢复不会改变仓库内容，不需要刷新列表。
          }
        } else {
          // 核心给的错误信息原样展示：里面写清了是哪个路径、什么原因，
          // 在桥这层改写成“操作失败，请重试”会把最有用的部分丢掉。
          SetStatus(QString::fromLatin1(kError),
                    kind == Kind::kBackup ? QStringLiteral("备份失败")
                                          : QStringLiteral("恢复失败"),
                    outcome.error_message);
        }
        active_file_name_.clear();
        emit operationFinished(outcome.succeeded);
      });

  connect(&catalog_watcher_, &QFutureWatcher<CatalogOutcome>::finished, this,
          [this]() {
            const CatalogOutcome outcome = catalog_watcher_.result();
            // 结果属于哪个仓库，决定了它还算不算数：扫描 A 的途中用户改了 B，
            // A 迟到回来的结果绝不能覆盖 B。
            const bool stale = outcome.repository_path != repository_path_;
            if (!stale) {
              if (outcome.succeeded) {
                backup_records_ = RecordsToVariantList(outcome.records);
                catalog_error_.clear();
              } else {
                // 列表失败只写 catalogError，不碰 status banner：
                // 管理页自己的错误不该覆盖一次刚成功的备份/恢复提示。
                backup_records_.clear();
                catalog_error_ = outcome.error_message;
              }
              emit backupRecordsChanged();
              emit catalogStateChanged();
            }

            const bool rescan =
                (stale || catalog_pending_) && !repository_path_.isEmpty();
            catalog_pending_ = false;
            if (rescan) {
              // 还有更新的仓库要扫：保持 busy 连续，不先发一次“空闲”，
              // 否则等 idle 的调用方会在两次扫描之间误判成已经稳定。
              catalog_watcher_.setFuture(QtConcurrent::run(
                  &BackupController::RunCatalogList, repository_path_));
              return;
            }
            SetCatalogBusy(false);
          });

  LoadConfig();
}

// 相等就不发信号：QML 的双向绑定会把输入框的值再写回来一次，
// 少了这个判断会来回触发，形成绑定环。
void BackupController::setSourcePath(const QString& path) {
  if (source_path_ == path) {
    return;
  }
  // 路径原样保存：不做 trim、不补斜杠、不 lowercase，
  // Linux 下空格和大小写都是文件名的一部分。
  source_path_ = path;
  emit sourcePathChanged();
}

QString BackupController::localPathFromUrl(const QUrl& url) const {
  // 非本地 URL（远端、qrc 等）返回空串，由调用方决定怎么办，
  // 而不是硬拼出一个看起来像路径的字符串。
  return url.isLocalFile() ? url.toLocalFile() : QString();
}

QUrl BackupController::directoryDialogStartUrl(const QString& path) const {
  const QFileInfo info(path);
  if (path.isEmpty() || !info.exists() || !info.isDir()) {
    return QUrl::fromLocalFile(QDir::homePath());
  }
  return QUrl::fromLocalFile(path);
}

bool BackupController::addFilterRule(const QString& action,
                                     const QString& rule) {
  FilterAction filter_action = FilterAction::kInclude;
  QStringList* target = nullptr;
  if (action == QStringLiteral("include")) {
    filter_action = FilterAction::kInclude;
    target = &include_rules_;
  } else if (action == QStringLiteral("exclude")) {
    filter_action = FilterAction::kExclude;
    target = &exclude_rules_;
  } else {
    SetStatus(QString::fromLatin1(kError), QStringLiteral("规则无效"),
              QStringLiteral("筛选动作只能是 include 或 exclude。"));
    return false;
  }

  // 语法判断只做一次，而且用的是和 CLI、归档层完全相同的 Filter 实现：
  // 界面不复制一套解析规则，两边不会漂移。
  Filter probe;
  std::string error_message;
  if (!probe.AddRule(filter_action, rule.toStdString(), &error_message)) {
    SetStatus(QString::fromLatin1(kError), QStringLiteral("规则无效"),
              QString::fromStdString(error_message));
    return false;
  }
  target->append(rule);
  emit filtersChanged();
  clearStatus();
  return true;
}

bool BackupController::removeFilterRule(int index) {
  if (index < 0) {
    return false;
  }
  if (index < include_rules_.size()) {
    include_rules_.removeAt(index);
    emit filtersChanged();
    return true;
  }
  const int exclude_index = index - include_rules_.size();
  if (exclude_index >= 0 && exclude_index < exclude_rules_.size()) {
    exclude_rules_.removeAt(exclude_index);
    emit filtersChanged();
    return true;
  }
  return false;
}

void BackupController::clearFilterRules() {
  if (include_rules_.isEmpty() && exclude_rules_.isEmpty()) {
    return;
  }
  include_rules_.clear();
  exclude_rules_.clear();
  emit filtersChanged();
}

bool BackupController::BuildFilter(Filter* filter,
                                   std::string* error_message) const {
  for (const QString& rule : include_rules_) {
    if (!filter->AddRule(FilterAction::kInclude, rule.toStdString(),
                         error_message)) {
      return false;
    }
  }
  for (const QString& rule : exclude_rules_) {
    if (!filter->AddRule(FilterAction::kExclude, rule.toStdString(),
                         error_message)) {
      return false;
    }
  }
  return true;
}

// ---- 启动时读配置 ----

void BackupController::LoadConfig() {
  backupproject::AppConfig config;
  std::string error_message;
  switch (config_manager_.Load(&config, &error_message)) {
    case backupproject::ConfigLoadStatus::kMissing:
      // 首次运行没有配置文件是正常的：不显示错误，也不替用户猜一个默认仓库。
      return;
    case backupproject::ConfigLoadStatus::kLoaded:
      if (config.backup_repository_path.empty()) {
        // 配置存在但没设仓库，等价于“尚未配置”。
        return;
      }
      repository_path_ = QString::fromStdString(config.backup_repository_path);
      emit repositoryPathChanged();
      refreshBackups();
      return;
    case backupproject::ConfigLoadStatus::kError:
      // 配置坏掉时如实报告，并把 ConfigManager 的原始诊断原样带出来。
      // 不自动改写、不自动删除、不猜默认值 ——
      // 用户的配置只有用户能决定怎么处理。
      SetStatus(QString::fromLatin1(kError), QStringLiteral("配置读取失败"),
                QString::fromStdString(error_message));
      return;
  }
}

// ---- 仓库列表 ----

void BackupController::SetCatalogBusy(bool busy) {
  if (catalog_busy_ == busy) {
    return;
  }
  catalog_busy_ = busy;
  emit catalogStateChanged();
}

void BackupController::StartCatalogList() {
  catalog_pending_ = false;
  SetCatalogBusy(true);
  catalog_watcher_.setFuture(
      QtConcurrent::run(&BackupController::RunCatalogList, repository_path_));
}

void BackupController::refreshBackups() {
  if (repository_path_.isEmpty()) {
    // 没有仓库就没有“列表”这回事：清干净即可，不启动后台扫描。
    const bool records_changed = !backup_records_.isEmpty();
    const bool error_changed = !catalog_error_.isEmpty();
    backup_records_.clear();
    catalog_error_.clear();
    catalog_pending_ = false;
    if (records_changed) {
      emit backupRecordsChanged();
    }
    if (error_changed) {
      emit catalogStateChanged();
    }
    return;
  }
  if (catalog_busy_) {
    // latest request wins：不排队第二次扫描，只记一位；当前这次结束后会立刻
    // 用最新的 repository 重扫。
    catalog_pending_ = true;
    return;
  }
  StartCatalogList();
}

// 后台线程：自己建一个 BackupCatalog，不与 GUI 线程共享任何对象。
CatalogOutcome BackupController::RunCatalogList(const QString& repository) {
  backupproject::BackupCatalog catalog;
  CatalogOutcome outcome;
  outcome.repository_path = repository;
  std::string error_message;
  outcome.succeeded =
      catalog.List(repository.toStdString(), &outcome.records, &error_message);
  if (!outcome.succeeded) {
    outcome.error_message = QString::fromStdString(error_message);
  }
  return outcome;
}

// ---- 保存 repository 设置 ----

bool BackupController::saveRepositoryPath(const QString& path) {
  if (busy_) {
    SetStatus(QString::fromLatin1(kError), QStringLiteral("无法保存"),
              QStringLiteral("备份或恢复正在进行，请等它结束后再改设置。"));
    return false;
  }
  if (path.isEmpty()) {
    SetStatus(QString::fromLatin1(kError), QStringLiteral("无法保存"),
              QStringLiteral("备份仓库路径不能为空。"));
    return false;
  }

  // 路径原样使用：不 trim、不 canonicalize、不 lowercase。Linux 下空格与大小写
  // 都是路径的一部分，清洗只会把用户真实的选择改坏。
  std::string error_message;
  if (!catalog_.EnsureRepository(path.toStdString(), &error_message)) {
    SetStatus(QString::fromLatin1(kError), QStringLiteral("设置保存失败"),
              QString::fromStdString(error_message));
    return false;
  }

  backupproject::AppConfig config;
  config.backup_repository_path = path.toStdString();
  if (!config_manager_.Save(config, &error_message)) {
    // EnsureRepository 可能已经把目录建出来了，这里刻意不回滚：删掉一个刚创建
    // 的空目录只会让用户更困惑。真正的保证是 repositoryPath_ 保持旧值，
    // 于是“配置里写的”和“界面上显示的”始终一致。
    SetStatus(QString::fromLatin1(kError), QStringLiteral("设置保存失败"),
              QString::fromStdString(error_message));
    return false;
  }

  if (repository_path_ != path) {
    repository_path_ = path;
    emit repositoryPathChanged();
  }
  // 换了仓库，旧列表和旧错误都不再对应当前 repository：先清干净再刷新，
  // 免得界面上短暂出现“B 的路径 + A 的列表”。
  backup_records_.clear();
  catalog_error_.clear();
  emit backupRecordsChanged();
  emit catalogStateChanged();
  refreshBackups();

  SetStatus(QString::fromLatin1(kSuccess), QStringLiteral("设置已保存"),
            QStringLiteral("备份仓库已更新。"));
  return true;
}

// ---- 备份 / 恢复 / 删除 ----

bool BackupController::startBackup() {
  if (busy_) {
    return false;
  }
  if (source_path_.isEmpty()) {
    SetStatus(QString::fromLatin1(kError), QStringLiteral("无法备份"),
              QStringLiteral("请先选择要备份的源目录。"));
    return false;
  }
  if (!repositoryConfigured()) {
    SetStatus(QString::fromLatin1(kError), QStringLiteral("无法备份"),
              QStringLiteral("请先在设置中选择备份仓库。"));
    return false;
  }
  // 规则只有全部合法才会走到这里（添加时已经验证过），
  // 这里再编一次是为了把规则随任务一起交给后台线程。
  Filter filter;
  std::string filter_error;
  if (!BuildFilter(&filter, &filter_error)) {
    SetStatus(QString::fromLatin1(kError), QStringLiteral("规则无效"),
              QString::fromStdString(filter_error));
    return false;
  }

  const std::string repository = repository_path_.toStdString();
  const std::string source = source_path_.toStdString();
  std::string error_message;
  // 配置里记着仓库，不代表它此刻还在：用户完全可能自己把目录删掉。
  // 这里重新确保一次，走的是和设置页保存时完全相同的核心接口。
  if (!catalog_.EnsureRepository(repository, &error_message)) {
    SetStatus(QString::fromLatin1(kError), QStringLiteral("备份失败"),
              QString::fromStdString(error_message));
    return false;
  }
  // 命名规则完全属于 BackupCatalog：<source-base>_YYYYMMDD_HHMMSS.bak，
  // 冲突时由它自己追加 _001…_999。这里一行都没有复制那套规则。
  std::string archive_path;
  if (!catalog_.BuildArchivePath(
          repository, source,
          static_cast<std::int64_t>(QDateTime::currentSecsSinceEpoch()),
          &archive_path, &error_message)) {
    SetStatus(QString::fromLatin1(kError), QStringLiteral("备份失败"),
              QString::fromStdString(error_message));
    return false;
  }
  // 只取最后一段用于提示。它的来源仍然是 Catalog 给的路径，
  // 而不是控制器重新拼一遍。
  const QString archive = QString::fromStdString(archive_path);
  // 正常备份走 v2：界面上的 uid / gid / symlink / FIFO 说明必须与产物一致。
  return Start(Kind::kBackup, BackupFlavor::kModernV2, source_path_, archive,
               QFileInfo(archive).fileName(), filter);
}

bool BackupController::startManagedRestore(const QString& file_name,
                                           const QString& destination_path) {
  if (busy_) {
    return false;
  }
  if (!repositoryConfigured()) {
    SetStatus(QString::fromLatin1(kError), QStringLiteral("无法恢复"),
              QStringLiteral("请先在设置中选择备份仓库。"));
    return false;
  }
  if (file_name.isEmpty()) {
    SetStatus(QString::fromLatin1(kError), QStringLiteral("无法恢复"),
              QStringLiteral("请先选择要恢复的备份文件。"));
    return false;
  }
  if (destination_path.isEmpty()) {
    SetStatus(QString::fromLatin1(kError), QStringLiteral("无法恢复"),
              QStringLiteral("请先选择恢复目录。"));
    return false;
  }

  // QML 只传 file name；把它解析成仓库里的真实文件是 Catalog 的职责，
  // 界面既拿不到也不允许拼 archive 的完整路径。
  std::string archive_path;
  std::string error_message;
  if (!catalog_.Resolve(repository_path_.toStdString(), file_name.toStdString(),
                        &archive_path, &error_message)) {
    SetStatus(QString::fromLatin1(kError), QStringLiteral("恢复失败"),
              QString::fromStdString(error_message));
    return false;
  }
  // 列表里的 recognizedArchive 只说明全局 header 可读，不构成“能恢复”的证据。
  // 这里刻意不信任它：真正的完整校验依然发生在
  // BackupEngine::Restore → ArchiveReader 的 preflight 里。
  // 恢复的格式由归档自身的 magic 决定（BackupEngine::Restore 按 magic 分流），
  // flavor 在这里不参与判断。
  return Start(Kind::kRestore, BackupFlavor::kLegacyV01,
               QString::fromStdString(archive_path), destination_path,
               file_name, Filter());
}

bool BackupController::deleteBackup(const QString& file_name) {
  if (busy_) {
    SetStatus(QString::fromLatin1(kError), QStringLiteral("无法删除"),
              QStringLiteral("备份或恢复正在进行，请等它结束后再试。"));
    return false;
  }
  if (catalog_busy_) {
    SetStatus(QString::fromLatin1(kError), QStringLiteral("无法删除"),
              QStringLiteral("备份列表正在刷新，请稍后再试。"));
    return false;
  }
  if (!repositoryConfigured()) {
    SetStatus(QString::fromLatin1(kError), QStringLiteral("无法删除"),
              QStringLiteral("请先在设置中选择备份仓库。"));
    return false;
  }
  if (file_name.isEmpty()) {
    SetStatus(QString::fromLatin1(kError), QStringLiteral("无法删除"),
              QStringLiteral("请先选择要删除的备份文件。"));
    return false;
  }

  // 删除不要求 InspectHeader 成功：坏掉的 .bak 同样是普通文件，必须删得掉，
  // 否则它会在列表里永远留着。界面上那层确认对话框不是安全边界，
  // 文件名与目标类型的校验始终由 Catalog 自己完成。
  std::string error_message;
  if (!catalog_.Delete(repository_path_.toStdString(), file_name.toStdString(),
                       &error_message)) {
    SetStatus(QString::fromLatin1(kError), QStringLiteral("删除失败"),
              QString::fromStdString(error_message));
    return false;
  }
  SetStatus(QString::fromLatin1(kSuccess), QStringLiteral("备份已删除"),
            QStringLiteral("%1 已从备份仓库中移除。").arg(file_name));
  refreshBackups();
  return true;
}

// ---- 仅供 main.cpp 自测的 direct archive 入口（不是 Q_INVOKABLE）----

bool BackupController::startDirectBackupForTest(const QString& source,
                                                const QString& archive_file) {
  Filter filter;
  std::string filter_error;
  if (!BuildFilter(&filter, &filter_error)) {
    SetStatus(QString::fromLatin1(kError), QStringLiteral("规则无效"),
              QString::fromStdString(filter_error));
    return false;
  }
  // 走的是同一个 Start()，direct backup 也照样应用当前 include / exclude 规则；
  // 但它固定产 legacy v0.1，作为旧格式的回归入口。
  return Start(Kind::kBackup, BackupFlavor::kLegacyV01, source, archive_file,
               QFileInfo(archive_file).fileName(), filter);
}

bool BackupController::startDirectRestoreForTest(const QString& archive_file,
                                                 const QString& destination) {
  // 恢复不需要筛选：归档里有什么就恢复什么，和 CLI 的语义一致。
  // 恢复也不看 flavor：格式由归档自己的 magic 决定。
  return Start(Kind::kRestore, BackupFlavor::kLegacyV01, archive_file,
               destination, QFileInfo(archive_file).fileName(), Filter());
}

// ---- 任务启动 ----

// 先置忙再启动线程：QML 收到 busyChanged 之后才会禁用按钮，
// 顺序反过来的话，线程已经跑起来而界面还允许再点一次。
// 忙的时候直接返回 false，不排队——界面上的按钮本来就是禁用的。
bool BackupController::Start(Kind kind, BackupFlavor flavor,
                             const QString& first_path,
                             const QString& second_path,
                             const QString& file_name, const Filter& filter) {
  if (busy_) {
    // 双保险：QML 侧已经用 busy
    // 禁用了按钮，但快捷键或程序化调用仍可能走到这里。
    return false;
  }
  active_kind_ = kind;
  active_file_name_ = file_name;
  SetBusy(true);
  SetStatus(QString::fromLatin1(kRunning),
            kind == Kind::kBackup ? QStringLiteral("正在备份……")
                                  : QStringLiteral("正在恢复……"),
            QStringLiteral("正在复制目录，期间界面仍可正常操作。"));
  // 函数指针 + 值拷贝的参数：后台线程拿到的是自己的副本，不需要加锁。
  // Filter 按值一起拷进后台任务：后台线程有自己的副本，不需要加锁。
  watcher_.setFuture(QtConcurrent::run(&BackupController::RunOperation, kind,
                                       flavor, first_path, second_path,
                                       filter));
  return true;
}

// 每次调用都新建一个 BackupEngine：核心没有全局状态，
// 一个任务一个实例最省心，也不存在后台线程共享对象的问题。
// QString 到 std::string 走的是 UTF-8，中文路径能原样传给核心。
OperationOutcome BackupController::RunOperation(Kind kind, BackupFlavor flavor,
                                                const QString& first_path,
                                                const QString& second_path,
                                                const Filter& filter) {
  // 这个函数跑在后台线程：只创建引擎、调一次接口，绝不触碰任何 QML 对象。
  backupproject::BackupEngine engine;
  std::string error_message;
  const std::string first = first_path.toStdString();
  const std::string second = second_path.toStdString();

  OperationOutcome outcome;
  if (kind == Kind::kBackup) {
    if (flavor == BackupFlavor::kModernV2) {
      // 三项都取"不做额外加工"的取值：打包用 MyPack，不压缩、不加密。
      // 加密需要密码，而界面没有、也不应该有密码输入框 —— 悄悄用空密码或者
      // 写死一个密码，比不加密更糟。
      backupproject::BackupOptions options;
      options.pack_method = backupproject::PackMethod::kMyPack;
      options.compression_method = backupproject::CompressionMethod::kNone;
      options.encryption_method = backupproject::EncryptionMethod::kNone;
      outcome.succeeded =
          engine.Backup(first, second, filter, options, &error_message);
    } else {
      outcome.succeeded = engine.Backup(first, second, filter, &error_message);
    }
  } else {
    outcome.succeeded = engine.Restore(first, second, &error_message);
  }
  if (!outcome.succeeded) {
    outcome.error_message = QString::fromStdString(error_message);
  }
  return outcome;
}

// 只在真正变化时发信号，避免多余的界面重算。
void BackupController::SetBusy(bool busy) {
  if (busy_ == busy) {
    return;
  }
  busy_ = busy;
  emit busyChanged();
}

void BackupController::SetStatus(const QString& kind, const QString& title,
                                 const QString& message) {
  status_kind_ = kind;
  status_title_ = title;
  status_message_ = message;
  emit statusChanged();
}

// 用户一动输入就把上一次的结果提示收回去；任务进行中不清，
// 否则会把“正在备份”这条提示提前抹掉。
void BackupController::clearStatus() {
  if (busy_) {
    return;
  }
  SetStatus(QString::fromLatin1(kIdle), QStringLiteral("等待操作"), QString());
}

// busyChanged 先到就立即返回；超时则返回 false，让调用方知道任务可能还在跑。
// --self-test / --repository-test 用它替代 QML
// 的事件循环，不起窗口也能等任务结束。
bool BackupController::waitForIdle(int timeout_ms) {
  QEventLoop loop;
  QTimer timer;
  timer.setSingleShot(true);
  connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
  connect(this, &BackupController::busyChanged, &loop, [&loop, this]() {
    if (!busy_) {
      loop.quit();
    }
  });
  if (!busy_) {
    return true;
  }
  timer.start(timeout_ms);
  loop.exec();
  return !busy_;
}

// 与 waitForIdle 同样的结构，但看的是仓库列表这一路：
// pending 也算“没稳定”，否则会在两次扫描之间误判成已经结束。
bool BackupController::waitForCatalogIdle(int timeout_ms) {
  QEventLoop loop;
  QTimer timer;
  timer.setSingleShot(true);
  connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
  connect(this, &BackupController::catalogStateChanged, &loop, [&loop, this]() {
    if (!catalog_busy_ && !catalog_pending_) {
      loop.quit();
    }
  });
  if (!catalog_busy_ && !catalog_pending_) {
    return true;
  }
  timer.start(timeout_ms);
  loop.exec();
  return !catalog_busy_ && !catalog_pending_;
}

}  // namespace backup_modern
