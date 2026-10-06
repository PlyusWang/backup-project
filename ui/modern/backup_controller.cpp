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
#include <cstddef>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "backup_engine.h"
#include "backup_option_keys.h"
#include "format_bytes.h"
#include "incremental_backup.h"
#include "incremental_restore.h"

namespace backup_modern {

namespace {

// 这四个字符串本身也是接口的一部分：改名要同时改 StatusBanner.qml。
// 状态种类，QML 用它决定 banner 的颜色和图标。
const char kIdle[] = "idle";
const char kRunning[] = "running";
const char kSuccess[] = "success";
const char kError[] = "error";

// 状态消息的归属页面，取值与 QML 里各页 StatusBanner 的 pageScope 一一对应。
// 与 severity 正交：同一个 scope 下 idle / running / success / error 走完全
// 相同的生命周期。空串表示全局（启动期失败、空闲基线），每一页都能显示。
const char kScopeBackup[] = "backup";
const char kScopeSettings[] = "settings";
const char kScopeManagement[] = "management";

// 文件大小的展示文本。格式规则只有一份（include/format_bytes.h），
// Qt 这层只把 std::string 转成 QString；QML 不实现单位换算。
QString FormatSize(std::uint64_t bytes) {
  return QString::fromStdString(backupproject::FormatByteSize(bytes));
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

  // ---- v2 pipeline 方法 ----
  // legacy v0.1 归档没有这三种方法，key 与 text 一律留空：界面不能给一份根本
  // 没有 pipeline 的归档标上 MyPack / Huffman。
  // 这些值全部来自归档 header 的声明，只说明"这份归档说它用了什么"，
  // 不代表归档完整，也不代表密码正确。
  item.insert(QStringLiteral("hasPipelineMethods"),
              record.has_pipeline_methods);
  // 显式写 backup_modern::：核心里的同名函数会因为参数类型触发 ADL 一起进来，
  // 不限定的话这里就是二义性调用。
  item.insert(QStringLiteral("packMethodKey"),
              record.has_pipeline_methods
                  ? backup_modern::PackMethodKey(record.pack_method)
                  : QString());
  item.insert(QStringLiteral("packMethodText"),
              record.has_pipeline_methods
                  ? backup_modern::PackMethodText(record.pack_method)
                  : QString());
  item.insert(QStringLiteral("compressionMethodKey"),
              record.has_pipeline_methods ? backup_modern::CompressionMethodKey(
                                                record.compression_method)
                                          : QString());
  item.insert(
      QStringLiteral("compressionMethodText"),
      record.has_pipeline_methods
          ? backup_modern::CompressionMethodText(record.compression_method)
          : QString());
  item.insert(QStringLiteral("encryptionMethodKey"),
              record.has_pipeline_methods
                  ? backup_modern::EncryptionMethodKey(record.encryption_method)
                  : QString());
  item.insert(QStringLiteral("encryptionMethodText"),
              record.has_pipeline_methods ? backup_modern::EncryptionMethodText(
                                                record.encryption_method)
                                          : QString());
  // 只表示"恢复这份归档需要密码"。列表阶段没有、也不该有密码。
  item.insert(QStringLiteral("passwordRequired"), record.password_required);
  // 快照种类与依赖链状态。界面据此显示"完整 / 增量"、父快照，
  // 以及在链断掉时如实说明"这份现在恢复不了"，而不是等用户点了才失败。
  item.insert(QStringLiteral("recordKind"), record.incremental_delta
                                                ? QStringLiteral("delta")
                                                : QStringLiteral("full"));
  item.insert(QStringLiteral("isDelta"), record.incremental_delta);
  item.insert(QStringLiteral("parentFileName"),
              QString::fromStdString(record.parent_file_name));
  item.insert(QStringLiteral("chainRestorable"),
              record.chain_restorable || !record.incremental_delta);
  item.insert(QStringLiteral("chainDiagnostic"),
              QString::fromStdString(record.chain_diagnostic));
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

// ---- 展示文案表：只回答"界面上怎么叫" ----
//
// 这三张表曾经同时承担"key <-> enum 映射"和"中文展示文案"两件事，
// 于是 CLI 想用同一套 key 就只能再抄一遍。现在映射提取到 Qt 无关的
// 共享核心 include/backup_option_keys.h：
//
//   CLI、ScheduleStore、Modern GUI 读的是同一张表。
//
// 所以这里剩下的只有本地化文案，一条 enum 取值都不再出现。加一种算法时，
// 映射只在核心改一次，这一层最多补一行中文。
struct DisplayLabel {
  const char* key;
  const char* label;
};

const DisplayLabel kCompressionLabels[] = {
    {"none", "不压缩"},
    {"huffman", "Huffman"},
    {"lzss-huffman", "LZSS + Huffman"},
};

const DisplayLabel kEncryptionLabels[] = {
    {"none", "不加密"},
    {"des-cbc-hmac-sha256", "DES-CBC + HMAC-SHA256"},
    {"aes-256-ctr-hmac-sha256", "AES-256-CTR + HMAC-SHA256"},
};

QString LabelForKey(const DisplayLabel* table, std::size_t count,
                    const char* key) {
  for (std::size_t index = 0; index < count; ++index) {
    if (std::strcmp(table[index].key, key) == 0) {
      return QString::fromUtf8(table[index].label);
    }
  }
  return QString();
}

}  // namespace

// ---- GUI 稳定 key 与核心 enum 的唯一映射 ----
//
// 全部转发到共享核心。GUI 在这里不再拥有任何映射知识，只负责 QString 的
// 编码转换；展示文案单独查上面那张本地化表。

bool ParsePackMethodKey(const QString& key, backupproject::PackMethod* method) {
  if (method == nullptr) return false;
  return backupproject::ParsePackMethodKey(key.toStdString(), method);
}

QString PackMethodKey(backupproject::PackMethod method) {
  return QString::fromLatin1(backupproject::PackMethodKey(method));
}

QString PackMethodText(backupproject::PackMethod method) {
  return QString::fromUtf8(backupproject::PackMethodDisplayName(method));
}

bool ParseCompressionMethodKey(const QString& key,
                               backupproject::CompressionMethod* method) {
  if (method == nullptr) return false;
  return backupproject::ParseCompressionMethodKey(key.toStdString(), method);
}

QString CompressionMethodKey(backupproject::CompressionMethod method) {
  return QString::fromLatin1(backupproject::CompressionMethodKey(method));
}

QString CompressionMethodText(backupproject::CompressionMethod method) {
  return LabelForKey(kCompressionLabels,
                     sizeof(kCompressionLabels) / sizeof(kCompressionLabels[0]),
                     backupproject::CompressionMethodKey(method));
}

bool ParseEncryptionMethodKey(const QString& key,
                              backupproject::EncryptionMethod* method) {
  if (method == nullptr) return false;
  return backupproject::ParseEncryptionMethodKey(key.toStdString(), method);
}

QString EncryptionMethodKey(backupproject::EncryptionMethod method) {
  return QString::fromLatin1(backupproject::EncryptionMethodKey(method));
}

QString EncryptionMethodText(backupproject::EncryptionMethod method) {
  return LabelForKey(kEncryptionLabels,
                     sizeof(kEncryptionLabels) / sizeof(kEncryptionLabels[0]),
                     backupproject::EncryptionMethodKey(method));
}

BackupController::BackupController(const QString& config_file_path,
                                   OperationGate* operation_gate,
                                   QObject* parent)
    : QObject(parent),
      config_manager_(config_file_path.toStdString()),
      operation_gate_(operation_gate) {
  // 两个 watcher 都以 this 为上下文：对象销毁时连接自动断开，后台任务即使还在
  // 跑也不会回调到已经释放的控制器上。
  connect(
      &watcher_, &QFutureWatcher<OperationOutcome>::finished, this, [this]() {
        const OperationOutcome outcome = watcher_.result();
        const Kind kind = active_kind_;
        const QString file_name = active_file_name_;
        // 完成提示沿用发起这次操作时记录的 scope：即使用户已经切到别的页面，
        // 这条消息也只会在原来的页面上显示。
        status_scope_ = active_scope_;
        last_succeeded_ = outcome.succeeded;
        // 先放开闸门再清 busy_：busyChanged 会触发 ScheduleController 的补跑，
        // 那一步必须看到闸门已经空了，否则补跑又要排一次 pending。
        if (operation_gate_ != nullptr &&
            active_gate_kind_ != OperationGate::Kind::kNone) {
          operation_gate_->Release(active_gate_kind_);
          active_gate_kind_ = OperationGate::Kind::kNone;
        }
        SetBusy(false);
        if (outcome.succeeded) {
          if (kind == Kind::kBackup && outcome.incremental) {
            // 增量策略必须说清"这一轮到底做了什么"：什么都没写、建了基线、
            // 还是写了 delta。三种情况用户看到的话必须是不一样的。
            if (outcome.no_changes) {
              SetStatus(
                  QString::fromLatin1(kSuccess), QStringLiteral("没有变化"),
                  QStringLiteral("自上次快照以来没有有效变化，未创建新快照。"));
            } else if (outcome.created_baseline) {
              SetStatus(
                  QString::fromLatin1(kSuccess),
                  QStringLiteral("备份完成（完整基线）"),
                  QStringLiteral("增量策略：本轮没有可信基线，已建立完整基线"
                                 "快照。原因：%1")
                      .arg(outcome.baseline_reason));
              refreshBackups();
            } else {
              SetStatus(QString::fromLatin1(kSuccess),
                        QStringLiteral("增量完成"),
                        QStringLiteral("已保存为 %1；变化：+%2 ~%3 =%4 -%5")
                            .arg(file_name)
                            .arg(outcome.added)
                            .arg(outcome.modified)
                            .arg(outcome.metadata_changed)
                            .arg(outcome.removed));
              refreshBackups();
            }
          } else if (kind == Kind::kBackup) {
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
// 选项显示名：手动 / 自动 / 实时三个页面共用这一份。以前各页自己
// 写一份，而且手动页漏了“推荐 / 兼容格式”这两条取舍提示。
//
// 顺序与 QML 侧列出的 key 一一对应：strategy 对 full / incremental，
// pack 对 mypack / ustar / fast-ustar，compression 对 none / huffman /
// lzss-huffman， encryption 对 none / aes-256-ctr-hmac-sha256 /
// des-cbc-hmac-sha256。
QStringList BackupController::optionStrategyLabels() const {
  return {QStringLiteral("完整备份"), QStringLiteral("增量备份")};
}

QStringList BackupController::optionPackLabels() const {
  // “推荐 / 兼容格式”是给用户看的取舍提示：MyPack 是本项目自有格式
  // （增量链只支持它），另两种是通用格式。三个页面都必须看到同一条提示。
  return {QStringLiteral("MyPack（推荐）"), QStringLiteral("USTAR（兼容格式）"),
          QStringLiteral("Fast USTAR（兼容格式）")};
}

QStringList BackupController::optionCompressionLabels() const {
  return {QStringLiteral("不压缩"), QStringLiteral("Huffman"),
          QStringLiteral("LZSS + Huffman")};
}

QStringList BackupController::optionEncryptionLabels() const {
  // 不加密 -> AES -> DES。DES 后面必须挂着“教学 / 旧算法”标记，
  // 免得有人在真实数据上误选它。
  return {QStringLiteral("不加密"), QStringLiteral("AES-256-CTR + HMAC-SHA256"),
          QStringLiteral("DES-CBC + HMAC-SHA256（教学 / 旧算法）")};
}

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
  // 规则编辑器只存在于备份页，所以它产生的提示也只属于备份页。
  status_scope_ = QString::fromLatin1(kScopeBackup);
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
  // 启动期读配置失败是全局状态：那时还没有"当前页面"这回事。
  status_scope_.clear();
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
  // 保存仓库是设置页的动作。
  status_scope_ = QString::fromLatin1(kScopeSettings);
  // 改仓库会写 config.json，并让之后所有备份落到另一个地方。它同样是一个
  // "会改动持久状态"的业务操作，所以在评估在飞的时候必须被拒绝。
  OperationGuard guard(operation_gate_, OperationGate::Kind::kRepositoryChange);
  if (operation_gate_ != nullptr && !guard.acquired()) {
    SetStatus(QString::fromLatin1(kError), QStringLiteral("无法保存"),
              guard.reason());
    return false;
  }
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

// 旧入口 = MyPack + 不压缩 + 不加密。保留它是为了让既有的自动测试、
// main.cpp 的自测链路、以及任何还没更新的调用方一字不改地继续工作。
bool BackupController::startBackup() {
  return startBackupWithOptions(QStringLiteral("mypack"),
                                QStringLiteral("none"), QStringLiteral("none"),
                                QString(), QString());
}

// 旧入口 = full 策略。行为与带算法选择的入口完全一致。
bool BackupController::startBackupWithOptions(const QString& pack_key,
                                              const QString& compression_key,
                                              const QString& encryption_key,
                                              const QString& password,
                                              const QString& confirm_password) {
  return StartBackupWithStrategy(BackupStrategy::kFull, pack_key,
                                 compression_key, encryption_key, password,
                                 confirm_password);
}

bool BackupController::startBackupWithStrategy(
    const QString& strategy_key, const QString& pack_key,
    const QString& compression_key, const QString& encryption_key,
    const QString& password, const QString& confirm_password) {
  // 解析失败绝不回退到 full：用户明确选了增量，就必须拿到增量或明确报错。
  BackupStrategy strategy = BackupStrategy::kFull;
  status_scope_ = QString::fromLatin1(kScopeBackup);
  if (!ParseBackupStrategyKey(strategy_key.toStdString(), &strategy)) {
    SetStatus(QString::fromLatin1(kError), QStringLiteral("无法备份"),
              QStringLiteral("未知备份策略：%1").arg(strategy_key));
    return false;
  }
  // 支持矩阵与选项组合是唯一答案来源：界面能点到的组合与核心接受的组合
  // 必须是同一个集合。这一条同时盖住 programmatic call——界面把某个选项置灰
  // 只是提示，真正的边界在这里。
  backupproject::PackMethod pack_method = backupproject::PackMethod::kMyPack;
  backupproject::CompressionMethod compression_method =
      backupproject::CompressionMethod::kNone;
  backupproject::EncryptionMethod encryption_method =
      backupproject::EncryptionMethod::kNone;
  const bool pack_ok = ParsePackMethodKey(pack_key, &pack_method);
  const bool compression_ok =
      ParseCompressionMethodKey(compression_key, &compression_method);
  const bool encryption_ok =
      ParseEncryptionMethodKey(encryption_key, &encryption_method);
  if (pack_ok && compression_ok && encryption_ok) {
    backupproject::BackupOptionCombination combination;
    combination.trigger = BackupTrigger::kManual;
    combination.strategy = strategy;
    combination.pack_method = pack_method;
    combination.compression_method = compression_method;
    combination.encryption_method = encryption_method;
    if (!backupproject::IsSupportedBackupOptionCombination(combination)) {
      SetStatus(QString::fromLatin1(kError), QStringLiteral("无法备份"),
                QString::fromStdString(
                    backupproject::UnsupportedBackupOptionCombinationReason(
                        combination)));
      return false;
    }
  } else if (!IsSupportedBackupMode(BackupTrigger::kManual, strategy)) {
    // key 解析失败的详细报错由后面那条路径负责；这里只保证产品矩阵先被判掉。
    SetStatus(QString::fromLatin1(kError), QStringLiteral("无法备份"),
              QString::fromStdString(UnsupportedBackupModeReason(
                  BackupTrigger::kManual, strategy)));
    return false;
  }
  return StartBackupWithStrategy(strategy, pack_key, compression_key,
                                 encryption_key, password, confirm_password);
}

bool BackupController::StartBackupWithStrategy(
    BackupStrategy strategy, const QString& pack_key,
    const QString& compression_key, const QString& encryption_key,
    const QString& password, const QString& confirm_password) {
  // 备份页的动作：成功 / 失败 / 校验提示都只属于备份页。
  status_scope_ = QString::fromLatin1(kScopeBackup);
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
  // 未知 key 一律明确失败，绝不回退默认值：用户明确选了某个算法却拿到另一个
  // 算法的产物，比明确报错危险得多。
  backupproject::BackupOptions options;
  if (!ParsePackMethodKey(pack_key, &options.pack_method)) {
    SetStatus(QString::fromLatin1(kError), QStringLiteral("无法备份"),
              QStringLiteral("未知打包方式：%1").arg(pack_key));
    return false;
  }
  if (!ParseCompressionMethodKey(compression_key,
                                 &options.compression_method)) {
    SetStatus(QString::fromLatin1(kError), QStringLiteral("无法备份"),
              QStringLiteral("未知压缩方式：%1").arg(compression_key));
    return false;
  }
  if (!ParseEncryptionMethodKey(encryption_key, &options.encryption_method)) {
    SetStatus(QString::fromLatin1(kError), QStringLiteral("无法备份"),
              QStringLiteral("未知加密方式：%1").arg(encryption_key));
    return false;
  }

  // 密码规则只有两条：非空、两次一致。刻意不加长度与复杂度要求 —— 那是产品
  // 策略，不是这一层该发明的规则。QML 也会即时提示同样的两条，但真正的判定
  // 在这里：QML 不是安全边界。
  if (options.encryption_method != backupproject::EncryptionMethod::kNone) {
    if (password.isEmpty()) {
      SetStatus(QString::fromLatin1(kError), QStringLiteral("无法备份"),
                QStringLiteral("密码不能为空。"));
      return false;
    }
    if (password != confirm_password) {
      SetStatus(QString::fromLatin1(kError), QStringLiteral("无法备份"),
                QStringLiteral("两次输入的密码不一致。"));
      return false;
    }
    options.password = password.toStdString();
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
  OperationRequest request;
  request.kind = Kind::kBackup;
  request.backup_flavor = BackupFlavor::kModernV2;
  request.first_path = source_path_;
  request.second_path = archive;
  request.filter = filter;
  request.backup_options = options;
  request.strategy = strategy;
  if (strategy == BackupStrategy::kIncremental) {
    // 增量在后台自己决定 baseline / delta / 不写，所以它需要仓库本身、
    // 快照名与规则原文（规则是链 identity 的一部分）。
    request.repository_directory = repository_path_;
    request.repository_identity =
        QString::fromStdString(backupproject::RepositoryIdentity(repository));
    request.snapshot_file_name = QFileInfo(archive).fileName();
    for (const QString& rule : include_rules_) {
      request.include_rules.push_back(rule.toStdString());
    }
    for (const QString& rule : exclude_rules_) {
      request.exclude_rules.push_back(rule.toStdString());
    }
  }
  // file name 只用于状态提示，来自 Catalog 生成的归档名，与密码无关。
  return Start(request, QFileInfo(archive).fileName());
}

bool BackupController::startManagedRestore(const QString& file_name,
                                           const QString& destination_path) {
  // 恢复是"备份管理"页里的动作。
  status_scope_ = QString::fromLatin1(kScopeManagement);
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
  // QML 的 passwordRequired 只是它从列表里读到的一句 header 声明，不能当事实。
  // 这里自己再辨认一次：普通恢复入口遇到加密的 v2 必须在启动后台线程之前失败，
  // 否则后台会拿空密码去试，用户只会看到一句和密码无关的认证失败。
  backupproject::ArchiveFileInfo info;
  const bool identified =
      backupproject::IdentifyArchiveFile(archive_path, &info, nullptr);
  if (identified &&
      info.kind == backupproject::ArchiveFileInfo::Kind::kContainerV2 &&
      info.encryption_method != backupproject::EncryptionMethod::kNone) {
    SetStatus(QString::fromLatin1(kError), QStringLiteral("无法恢复"),
              QStringLiteral("此备份已加密，请输入恢复密码。"));
    return false;
  }
  // 辨认失败不在这里拦：那个坏文件应该由真正的恢复路径给出它自己的诊断，
  // 免得同一个文件出现两套措辞。保持既有行为不变。
  OperationRequest request;
  request.kind = Kind::kRestore;
  request.first_path = QString::fromStdString(archive_path);
  request.second_path = destination_path;
  // 依赖链恢复需要仓库与快照名：目标是完整快照时走的是同一条路径，
  // 行为与以前完全一致；是 delta 时自动解析 base 与中间层。
  request.repository_directory = repository_path_;
  request.snapshot_file_name = file_name;
  return Start(request, file_name);
}

bool BackupController::startManagedRestoreWithPassword(
    const QString& file_name, const QString& destination_path,
    const QString& password) {
  // 与不带密码的那个入口同属"备份管理"页。
  status_scope_ = QString::fromLatin1(kScopeManagement);
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
  if (password.isEmpty()) {
    SetStatus(QString::fromLatin1(kError), QStringLiteral("无法恢复"),
              QStringLiteral("恢复密码不能为空。"));
    return false;
  }

  std::string archive_path;
  std::string error_message;
  if (!catalog_.Resolve(repository_path_.toStdString(), file_name.toStdString(),
                        &archive_path, &error_message)) {
    SetStatus(QString::fromLatin1(kError), QStringLiteral("恢复失败"),
              QString::fromStdString(error_message));
    return false;
  }

  // 是否需要密码由归档自己的 header 决定，不信 QML 传进来的任何标志。
  backupproject::ArchiveFileInfo info;
  const bool identified =
      backupproject::IdentifyArchiveFile(archive_path, &info, nullptr);
  OperationRequest request;
  request.kind = Kind::kRestore;
  request.first_path = QString::fromStdString(archive_path);
  request.second_path = destination_path;
  request.repository_directory = repository_path_;
  request.snapshot_file_name = file_name;
  if (identified &&
      info.kind == backupproject::ArchiveFileInfo::Kind::kContainerV2 &&
      info.encryption_method != backupproject::EncryptionMethod::kNone) {
    request.restore_is_v2 = true;
    request.restore_options.password = password.toStdString();
  }
  // legacy 与未加密的 v2 都走按 magic 分流的旧入口：它们根本不需要密码，
  // 硬塞一个 RestoreOptions 进去只会让代码看起来像在用密码。
  return Start(request, file_name);
}

bool BackupController::deleteBackup(const QString& file_name) {
  // 删除也是"备份管理"页里的动作。
  status_scope_ = QString::fromLatin1(kScopeManagement);
  // 删除会同时改两处持久状态：仓库里的文件与 ScheduleStore 的 managed 名单。
  // 整个动作（含最后那步 reconcile）都在同一持有期内完成，所以后台评估不可能
  // 在这中间写 store。
  OperationGuard guard(operation_gate_, OperationGate::Kind::kManualDelete);
  if (operation_gate_ != nullptr && !guard.acquired()) {
    SetStatus(QString::fromLatin1(kError), QStringLiteral("无法删除"),
              guard.reason());
    return false;
  }
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
  std::vector<std::string> diagnostics;
  if (!catalog_.Delete(repository_path_.toStdString(), file_name.toStdString(),
                       &diagnostics, &error_message)) {
    SetStatus(QString::fromLatin1(kError), QStringLiteral("删除失败"),
              QString::fromStdString(error_message));
    return false;
  }
  QString message = QStringLiteral("%1 已从备份仓库中移除。").arg(file_name);
  // 副文件清理失败不是"删除失败"，但要说出来：否则用户以为仓库干净了，
  // 而 <name>.manifest / <name>.identity 还留在那里。
  for (const std::string& note : diagnostics) {
    message += QStringLiteral("\n") + QString::fromStdString(note);
  }
  SetStatus(QString::fromLatin1(kSuccess), QStringLiteral("备份已删除"),
            message);
  refreshBackups();
  // 删成功之后才通知：计划状态要跟着这份仓库的实际内容走，而不是跟着"用户点了
  // 删除"走。失败时什么都没变，也就不该有人去改 schedule。
  //
  // 这一步是同步的、且在闸门持有期内完成——它取代了以前那个信号：信号是排队
  // 投递的，投递到 ScheduleController 时"删除"这个操作可能已经结束，闸门也
  // 已经放开，后台评估就有机会插进来。
  if (archive_deleted_observer_ != nullptr) {
    archive_deleted_observer_->OnArchiveDeleted(file_name);
  }
  return true;
}

// ---- 仅供 main.cpp 自测的 direct archive 入口（不是 Q_INVOKABLE）----

bool BackupController::startDirectBackupForTest(const QString& source,
                                                const QString& archive_file) {
  // 自测入口没有页面，按它语义上对应的页面归属，免得留下上一次的 scope。
  status_scope_ = QString::fromLatin1(kScopeBackup);
  Filter filter;
  std::string filter_error;
  if (!BuildFilter(&filter, &filter_error)) {
    SetStatus(QString::fromLatin1(kError), QStringLiteral("规则无效"),
              QString::fromStdString(filter_error));
    return false;
  }
  // 走的是同一个 Start()，direct backup 也照样应用当前 include / exclude 规则；
  // 但它固定产 legacy v0.1，作为旧格式的回归入口。
  OperationRequest request;
  request.kind = Kind::kBackup;
  request.backup_flavor = BackupFlavor::kLegacyV01;
  request.first_path = source;
  request.second_path = archive_file;
  request.filter = filter;
  return Start(request, QFileInfo(archive_file).fileName());
}

bool BackupController::startDirectRestoreForTest(const QString& archive_file,
                                                 const QString& destination) {
  status_scope_ = QString::fromLatin1(kScopeManagement);
  // 恢复不需要筛选：归档里有什么就恢复什么，和 CLI 的语义一致。
  // 恢复也不看 flavor：格式由归档自己的 magic 决定。
  OperationRequest request;
  request.kind = Kind::kRestore;
  request.first_path = archive_file;
  request.second_path = destination;
  return Start(request, QFileInfo(archive_file).fileName());
}

// ---- 任务启动 ----

// 先置忙再启动线程：QML 收到 busyChanged 之后才会禁用按钮，
// 顺序反过来的话，线程已经跑起来而界面还允许再点一次。
// 忙的时候直接返回 false，不排队——界面上的按钮本来就是禁用的。
bool BackupController::Start(const OperationRequest& request,
                             const QString& file_name) {
  if (busy_) {
    // 双保险：QML 侧已经用 busy
    // 禁用了按钮，但快捷键或程序化调用仍可能走到这里。
    return false;
  }
  // 闸门是真正的不变式：定时备份评估在跑的时候，手动备份/恢复必须在这里被
  // 拒绝，而不是靠按钮被置灰。请求的 kind 决定占哪一种。
  const OperationGate::Kind gate_kind =
      request.kind == Kind::kBackup ? OperationGate::Kind::kManualBackup
                                    : OperationGate::Kind::kManualRestore;
  if (operation_gate_ != nullptr) {
    QString reason;
    if (!operation_gate_->Acquire(gate_kind, &reason)) {
      SetStatus(QString::fromLatin1(kError),
                request.kind == Kind::kBackup ? QStringLiteral("无法开始备份")
                                              : QStringLiteral("无法开始恢复"),
                reason);
      return false;
    }
  }
  active_gate_kind_ =
      operation_gate_ != nullptr ? gate_kind : OperationGate::Kind::kNone;
  active_kind_ = request.kind;
  active_file_name_ = file_name;
  // 记住这次操作属于哪一页：完成提示要落回发起它的页面，而不是用户此刻
  // 正在看的页面。
  active_scope_ = status_scope_;
  SetBusy(true);
  // "正在备份 / 正在恢复"是**全局运行状态**，不属于任何一页：切页不该把它清掉，
  // 每一页都该看得到"现在有活在干"。归属页面只作用于结果提示。
  status_scope_.clear();
  SetStatus(QString::fromLatin1(kRunning),
            request.kind == Kind::kBackup ? QStringLiteral("正在备份……")
                                          : QStringLiteral("正在恢复……"),
            QStringLiteral("正在复制目录，期间界面仍可正常操作。"));
  // 函数指针 + 值拷贝的参数：后台线程拿到的是自己的副本，不需要加锁。
  // request（含密码）按值拷进后台任务，本函数返回后本地副本立即销毁。
  watcher_.setFuture(
      QtConcurrent::run(&BackupController::RunOperation, request));
  return true;
}

// 每次调用都新建一个 BackupEngine：核心没有全局状态，
// 一个任务一个实例最省心，也不存在后台线程共享对象的问题。
// QString 到 std::string 走的是 UTF-8，中文路径能原样传给核心。
OperationOutcome BackupController::RunOperation(OperationRequest request) {
  // 这个函数跑在后台线程：只创建引擎、调一次接口，绝不触碰任何 QML 对象。
  // request 是值拷贝，随本次调用结束一起销毁 —— 密码的生命周期到此为止。
  backupproject::BackupEngine engine;
  std::string error_message;
  const std::string first = request.first_path.toStdString();
  const std::string second = request.second_path.toStdString();

  OperationOutcome outcome;
  if (request.kind == Kind::kBackup &&
      request.strategy == BackupStrategy::kIncremental) {
    // 增量：baseline / delta / 无变化三选一，由共享引擎决定。
    // 界面只负责把结果如实说出来，不自己判断"这算不算成功"。
    backupproject::IncrementalOutcome incremental;
    const bool ok = backupproject::RunIncrementalBackup(
        first, request.repository_directory.toStdString(),
        request.snapshot_file_name.toStdString(),
        request.repository_identity.toStdString(), request.filter,
        request.backup_options, request.include_rules, request.exclude_rules,
        std::string(), &incremental, &error_message);
    outcome.succeeded = ok;
    outcome.incremental = true;
    if (ok) {
      outcome.no_changes = incremental.kind ==
                           backupproject::IncrementalOutcome::Kind::kNoChanges;
      outcome.created_baseline =
          incremental.kind ==
          backupproject::IncrementalOutcome::Kind::kFullBaseline;
      outcome.snapshot_kind =
          outcome.no_changes
              ? QStringLiteral("no changes")
              : (outcome.created_baseline ? QStringLiteral("full baseline")
                                          : QStringLiteral("delta"));
      outcome.baseline_reason =
          QString::fromStdString(incremental.baseline_reason);
      outcome.added = incremental.summary.added;
      outcome.modified = incremental.summary.modified;
      outcome.metadata_changed = incremental.summary.metadata_changed;
      outcome.removed = incremental.summary.removed;
    }
  } else if (request.kind == Kind::kBackup) {
    if (request.backup_flavor == BackupFlavor::kModernV2) {
      // 三项都由用户在界面上选择，这里不再写死任何一项。
      outcome.succeeded = engine.Backup(first, second, request.filter,
                                        request.backup_options, &error_message);
    } else {
      // legacy v0.1：旧格式始终保留一条被真实执行的回归入口。
      outcome.succeeded =
          engine.Backup(first, second, request.filter, &error_message);
    }
  } else if (request.kind == Kind::kRestore &&
             request.repository_directory.size() > 0 &&
             // 只有 v2 container 与 BKPINC1 delta 才属于依赖链；legacy v0.1
             // 等格式没有链的概念，继续走下面那条按 magic 分流的既有入口
             // （产品一直能恢复历史 v0.1，这条能力不因为增量而消失）。
             backupproject::ClassifySnapshotFile(first, nullptr) !=
                 backupproject::SnapshotFileKind::kUnknown) {
    // GUI 的恢复也走依赖链入口 —— 目标是一份完整快照时行为与以前
    // 完全一致，是 delta 时自动把 base 与中间层一起应用。
    backupproject::RestoreReport report;
    outcome.succeeded = backupproject::RestoreSnapshotChain(
        request.repository_directory.toStdString(),
        request.snapshot_file_name.toStdString(), second,
        request.restore_options, &report, &error_message);
  } else if (request.restore_is_v2) {
    // 只有用户真的输入了恢复密码才会走这里。
    outcome.succeeded =
        engine.Restore(first, second, request.restore_options, &error_message);
  } else {
    // legacy v0.1 与未加密的 v2 共用这条按 magic 分流的入口。
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

void BackupController::setStatusForTest(const QString& kind,
                                        const QString& scope,
                                        const QString& title,
                                        const QString& message) {
  status_scope_ = scope;
  SetStatus(kind, title, message);
}

void BackupController::SetStatus(const QString& kind, const QString& title,
                                 const QString& message) {
  status_kind_ = kind;
  status_title_ = title;
  status_message_ = message;
  // 空闲基线是全局状态：它没有"属于哪一页"这一说，所以在这里归一化掉。
  // 其余消息保留调用方设置的 scope，由 QML 按页面过滤。
  if (kind == QString::fromLatin1(kIdle)) {
    status_scope_.clear();
  }
  emit statusChanged();
}

// 离开页面时的"消费"。busy 判定与 clearStatus() 完全一致：正在跑的操作属于
// 全局运行状态，不因为切页就消失。
void BackupController::dismissPageStatus(const QString& scope) {
  if (busy_ || scope.isEmpty()) {
    return;
  }
  if (status_scope_ != scope) {
    return;
  }
  clearStatus();
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
