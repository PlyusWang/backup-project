// schedule_controller.cpp

#include "schedule_controller.h"

#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QtConcurrent/QtConcurrentRun>
#include <ctime>
#include <string>
#include <utility>

#include "backup_catalog.h"
#include "backup_option_keys.h"
#include "filter.h"

namespace backup_modern {

namespace {

const char kIdle[] = "idle";
const char kRunning[] = "running";
const char kSuccess[] = "success";
const char kWarning[] = "warning";
const char kError[] = "error";

QString FormatLocalTime(std::int64_t seconds) {
  if (seconds <= 0) return QStringLiteral("尚未运行");
  return QDateTime::fromSecsSinceEpoch(static_cast<qint64>(seconds))
      .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

QString FormatSize(std::uint64_t bytes) {
  return QStringLiteral("%1 B").arg(static_cast<qulonglong>(bytes));
}

QString ChangeText(qulonglong added, qulonglong removed, qulonglong modified,
                   qulonglong metadata_changed) {
  return QStringLiteral("+%1 新增 · ~%2 修改 · -%3 删除 · %4 元数据变化")
      .arg(added)
      .arg(modified)
      .arg(removed)
      .arg(metadata_changed);
}

}  // namespace

ScheduleController::ScheduleController(QString schedule_file_path,
                                       const QString& config_file_path,
                                       BackupController* backup_controller,
                                       QObject* parent)
    : QObject(parent),
      schedule_file_path_(std::move(schedule_file_path)),
      backup_controller_(backup_controller),
      config_manager_(config_file_path.toStdString()),
      store_(schedule_file_path_.toStdString()) {
  // tick 只有 1 秒粒度，而且只负责"醒来一次"。"到没到点"问的是共享核心的
  // IsScheduleDue —— QTimer 不参与任何业务判断。
  tick_.setInterval(1000);
  connect(&tick_, &QTimer::timeout, this, &ScheduleController::Tick);

  connect(&watcher_, &QFutureWatcher<ScheduleOutcome>::finished, this,
          &ScheduleController::OnEvaluationFinished);

  // 手动 backup / restore 的 busy 边界。控制器不复制这套并发逻辑，
  // 只是订阅它：手动操作一结束就把积压的那一次到期检查补上。
  if (backup_controller != nullptr) {
    connect(backup_controller, &BackupController::busyChanged, this,
            &ScheduleController::OnBackupBusyChanged);
    // 仓库可以在运行期被改掉（设置页 / CLI）。不订阅它，scheduler 就会一直
    // 拿启动时读到的那个仓库继续写——静默地把备份写进旧位置。
    connect(backup_controller, &BackupController::repositoryPathChanged, this,
            &ScheduleController::OnRepositoryPathChanged);
  }
}

ScheduleController::~ScheduleController() {
  // 析构时释放 runner 锁：flock 随 fd 关闭自动释放，这里显式做一次，
  // 让"谁持有锁"在代码里也是清楚的。
  lock_.Release();
}

// ---- 读盘 ----

void ScheduleController::LoadFromDisk() {
  config_loaded_ = false;
  load_error_.clear();

  // repository 来自 ConfigManager（与手动备份同一个 config.json）。
  repository_path_.clear();
  backupproject::AppConfig config;
  std::string config_error;
  const backupproject::ConfigLoadStatus status =
      config_manager_.Load(&config, &config_error);
  if (status == backupproject::ConfigLoadStatus::kError) {
    load_error_ = QString::fromStdString(config_error);
  } else if (status == backupproject::ConfigLoadStatus::kLoaded) {
    repository_path_ = QString::fromStdString(config.backup_repository_path);
  }

  backupproject::ScheduleDocument document;
  std::string error;
  const backupproject::ScheduleLoadStatus loaded =
      store_.Load(&document, &error);
  if (loaded == backupproject::ScheduleLoadStatus::kError) {
    // 坏配置只如实报告，不自动改写、不自动删除、不猜默认值。
    load_error_ = QString::fromStdString(error);
    document_ = backupproject::ScheduleDocument{};
    emit configChanged();
    emit stateChanged();
    return;
  }
  if (loaded == backupproject::ScheduleLoadStatus::kMissing) {
    // 从来没配过不是错误：给一份默认（disabled）配置，界面显示的是同一套默认值。
    document_ = backupproject::ScheduleDocument{};
  } else {
    document_ = std::move(document);
  }
  config_loaded_ = true;
  emit configChanged();
  emit stateChanged();
}

// ---- runner 锁 ----

void ScheduleController::ApplyRunnerLock() {
  const bool should_run = document_.config.enabled && config_loaded_;

  if (!should_run) {
    if (lock_.held()) {
      lock_.Release();
      tick_.stop();
      SetRunnerMessage(QStringLiteral("当前未启用。"));
    } else if (runner_message_.isEmpty()) {
      SetRunnerMessage(QStringLiteral("当前未启用。"));
    }
    return;
  }

  if (!lock_.held()) {
    std::string error;
    if (!lock_.Acquire(store_.lock_file_path(), &error)) {
      // 另一个进程正在跑：这一轮不执行，页面如实显示。
      //
      // 但 timer 必须继续跑下去。它此刻的角色变成"等锁释放"的轮询：Tick 每秒
      // 试一次非阻塞 flock，对方退出之后立刻把锁拿回来。停掉 timer 就等于
      // "别的进程跑过一次之后，本程序再也不跑这个计划了" —— 页面会一直显示
      // "已被另一进程持有"，直到用户改配置或重启。
      SetRunnerMessage(QString::fromStdString(error));
      if (!tick_.isActive()) tick_.start();
      return;
    }
  }
  SetRunnerMessage(QStringLiteral("本程序正在运行该计划（锁：%1）")
                       .arg(QString::fromStdString(store_.lock_file_path())));
  if (!tick_.isActive()) tick_.start();
}

void ScheduleController::SetRunnerMessage(const QString& text) {
  if (runner_message_ == text) return;
  runner_message_ = text;
  emit runnerChanged();
}

// ---- tick ----

void ScheduleController::Tick() {
  if (!config_loaded_ || !document_.config.enabled) return;
  if (!lock_.held()) {
    // 锁可能只是**暂时**在别人手里：上一个持有者退出之后必须能重新抢回来。
    // 少了这一步，"另一个进程曾经跑过"会变成"本程序从此再也不跑这个计划" ——
    // 页面会一直显示"已被另一进程持有"，直到用户改配置或重启。
    // tick 本来就是 1 Hz，一次非阻塞 flock 的代价可以忽略；抢不到时
    // SetRunnerMessage 不会重复发信号，页面也不会每秒重绘。
    ApplyRunnerLock();
    if (!lock_.held()) return;
  }

  const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
  // "到没到点"由共享核心回答，QML 与 CLI 用的是同一个函数。
  if (!backupproject::IsScheduleDue(now, document_.state.next_run_time_sec,
                                    document_.config.interval_minutes)) {
    return;
  }
  Submit(/*force=*/false);
}

void ScheduleController::OnBackupBusyChanged() { DrainPending(); }

void ScheduleController::OnRepositoryPathChanged() {
  // 只重读配置：下一次 Submit 会把当时的 repository_path_ 拷进后台任务，
  // 所以新值天然只影响之后的提交，不会影响已经在跑的那一轮。
  const QString previous = repository_path_;
  LoadFromDisk();
  ApplyRunnerLock();
  if (repository_path_ != previous) {
    // 换了仓库：积压的那一次到期检查必须落到新仓库上。
    DrainPending();
  }
}

void ScheduleController::Submit(bool force) {
  if (busy_) {
    // 忙碌期间来的多个到期事件 coalesce 成一次，绝不排无限队列。
    if (!pending_) {
      pending_ = true;
      emit pendingChanged();
    }
    pending_force_ = pending_force_ || force;
    return;
  }
  if (schedule_file_path_.isEmpty()) {
    SetStatus(kError, QStringLiteral("无法定位计划存储文件"),
              QStringLiteral("应用配置目录不可用，定时备份无法运行。"));
    return;
  }
  if (repository_path_.isEmpty()) {
    // 没配仓库：记成 pending，等用户在设置页配好之后自然会被补跑。
    if (!pending_) {
      pending_ = true;
      emit pendingChanged();
    }
    pending_force_ = pending_force_ || force;
    return;
  }
  // 手动 backup / restore 正在写盘：绝不并发。只记一个 pending，
  // 等 BackupController 的 busyChanged 之后再补跑一次。
  if (backup_controller_ != nullptr && backup_controller_->busy()) {
    if (!pending_) {
      pending_ = true;
      emit pendingChanged();
    }
    pending_force_ = pending_force_ || force;
    return;
  }

  const QString store_path = schedule_file_path_;
  const QString repository = repository_path_;
  last_succeeded_ = false;
  SetBusy(true);
  SetStatus(kRunning, QStringLiteral("计划任务运行中"),
            force ? QStringLiteral("正在立即检查并运行…")
                  : QStringLiteral("正在执行到期的定时备份…"));
  watcher_.setFuture(QtConcurrent::run(&ScheduleController::RunEvaluation,
                                       store_path, repository, force));
}

ScheduleOutcome ScheduleController::RunEvaluation(const QString& store_path,
                                                  const QString& repository,
                                                  bool force) {
  ScheduleOutcome outcome;
  backupproject::ScheduleStore store(store_path.toStdString());
  backupproject::ScheduledBackupService service(repository.toStdString(),
                                                &store);

  backupproject::ScheduleEvaluationResult result;
  std::string error;
  const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
  const bool ok = force ? service.EvaluateNow(now, &result, &error)
                        : service.Evaluate(now, &result, &error);
  if (!ok) {
    outcome.error_message = QString::fromStdString(error);
    outcome.status_key = QStringLiteral("failed");
    outcome.status_text = outcome.error_message;
    return outcome;
  }

  outcome.succeeded = true;
  outcome.ran = true;
  outcome.first_snapshot = result.first_snapshot;
  outcome.baseline_reset = result.baseline_reset;
  outcome.status_key = QString::fromLatin1(
      backupproject::ScheduleEvaluationStatusKey(result.status));
  outcome.status_text = QString::fromUtf8(
      backupproject::ScheduleEvaluationStatusText(result.status));
  outcome.archive_file_name = QString::fromStdString(result.archive_file_name);
  outcome.diagnostic = QString::fromStdString(result.diagnostic);
  outcome.added = result.changes.added;
  outcome.removed = result.changes.removed;
  outcome.modified = result.changes.modified;
  outcome.metadata_changed = result.changes.metadata_changed;
  outcome.next_run_sec = result.next_run_time_sec;

  switch (result.status) {
    case backupproject::ScheduleEvaluationStatus::kDisabled:
    case backupproject::ScheduleEvaluationStatus::kNotDue:
      outcome.due = false;
      break;
    case backupproject::ScheduleEvaluationStatus::kSkippedNoChanges:
      outcome.skipped_no_changes = true;
      break;
    case backupproject::ScheduleEvaluationStatus::kCreatedSnapshot:
      outcome.created = true;
      break;
    case backupproject::ScheduleEvaluationStatus::kCreatedWithRetentionWarning:
      outcome.created = true;
      outcome.retention_warning = true;
      break;
    case backupproject::ScheduleEvaluationStatus::kFailed:
      outcome.succeeded = false;
      break;
  }
  return outcome;
}

void ScheduleController::OnEvaluationFinished() {
  const ScheduleOutcome outcome = watcher_.result();
  last_succeeded_ = outcome.succeeded;
  SetBusy(false);

  if (!outcome.error_message.isEmpty()) {
    SetStatus(kError, QStringLiteral("计划任务无法运行"),
              outcome.error_message);
  } else if (!outcome.due) {
    SetStatus(kIdle, QStringLiteral("还没到时间"),
              QStringLiteral("下一次运行时间未到，本轮不执行。"));
  } else if (outcome.skipped_no_changes) {
    SetStatus(kIdle, QStringLiteral("源目录没有变化，已跳过"),
              QStringLiteral(
                  "按计划只创建有变化的完整快照，因此这一轮不生成新归档。"));
  } else if (outcome.retention_warning) {
    SetStatus(
        kWarning, QStringLiteral("计划备份成功，但旧版本淘汰失败"),
        QStringLiteral("新快照已经创建并可恢复；删不掉的那一份会保留下来，"
                       "下一轮会再次尝试。") +
            (outcome.diagnostic.isEmpty()
                 ? QString()
                 : QStringLiteral(" 原因：") + outcome.diagnostic));
  } else if (outcome.created) {
    SetStatus(
        kSuccess,
        outcome.baseline_reset ? QStringLiteral("已重建基线快照")
                               : QStringLiteral("已创建新的完整快照"),
        (outcome.first_snapshot
             ? QStringLiteral("首次快照：") +
                   ChangeText(outcome.added, outcome.removed, outcome.modified,
                              outcome.metadata_changed)
         : outcome.baseline_reset
             ? QStringLiteral("原来那份基线快照已经不在仓库里（被删除、"
                              "换了仓库或换了源目录），因此重新建立了一份"
                              "完整快照：") +
                   ChangeText(outcome.added, outcome.removed, outcome.modified,
                              outcome.metadata_changed)
             : QStringLiteral("变化：") +
                   ChangeText(outcome.added, outcome.removed, outcome.modified,
                              outcome.metadata_changed)) +
            QStringLiteral(" · 归档 %1").arg(outcome.archive_file_name));
  } else {
    SetStatus(kError, QStringLiteral("计划备份失败"), outcome.status_text);
  }

  // 重新读盘：managed 列表、history、next run 都以磁盘上的状态为准，
  // 界面不自己维护第二份。
  LoadFromDisk();
  ApplyRunnerLock();
  emit operationFinished(last_succeeded_);
  DrainPending();
}

// ---- 对外入口 ----

void ScheduleController::start() {
  LoadFromDisk();
  ApplyRunnerLock();
}

void ScheduleController::reload() { start(); }

void ScheduleController::stop() {
  tick_.stop();
  lock_.Release();
  emit runnerChanged();
}

void ScheduleController::clearStatus() {
  if (busy_) return;
  status_kind_ = QStringLiteral("idle");
  status_title_ = QStringLiteral("等待计划任务");
  status_message_.clear();
  emit statusChanged();
}

bool ScheduleController::saveConfig(bool enabled, const QString& source_path,
                                    int interval_minutes, int retain_count,
                                    const QString& pack_key,
                                    const QString& compression_key,
                                    const QStringList& include_rules,
                                    const QStringList& exclude_rules) {
  if (!config_loaded_) {
    SetStatus(kError, QStringLiteral("计划配置不可用"), load_error_);
    return false;
  }

  backupproject::ScheduleConfig config = document_.config;
  config.enabled = enabled;
  config.source_path = source_path.toStdString();
  config.interval_minutes =
      interval_minutes < 0 ? 0 : static_cast<std::uint32_t>(interval_minutes);
  config.retain_count =
      retain_count < 0 ? 0 : static_cast<std::uint32_t>(retain_count);

  // 算法 key 的解析用的是共享核心那张表，与 CLI 完全一致。
  if (!backupproject::ParsePackMethodKey(pack_key.toStdString(),
                                         &config.pack_method)) {
    SetStatus(kError, QStringLiteral("未知的打包方式"), pack_key);
    return false;
  }
  if (!backupproject::ParseCompressionMethodKey(compression_key.toStdString(),
                                                &config.compression_method)) {
    SetStatus(kError, QStringLiteral("未知的压缩方式"), compression_key);
    return false;
  }
  // 界面只提供"不加密"。无人值守的定时任务没有安全的持久密钥来源，
  // 这里不是"暂时隐藏"，而是真的没有任何地方可以存密码。
  config.encryption_method = backupproject::EncryptionMethod::kNone;

  config.include_rules.clear();
  for (const QString& rule : include_rules) {
    config.include_rules.push_back(rule.toStdString());
  }
  config.exclude_rules.clear();
  for (const QString& rule : exclude_rules) {
    config.exclude_rules.push_back(rule.toStdString());
  }

  std::string error;
  if (!backupproject::ValidateScheduleConfig(config, &error)) {
    SetStatus(kError, QStringLiteral("计划配置不合法"),
              QString::fromStdString(error));
    return false;
  }
  if (config.enabled && !backupproject::ValidateScheduleForEnable(
                            config, repository_path_.toStdString(), &error)) {
    // 与 backupctl schedule enable 调的是同一个函数：源目录、仓库、"能不能
    // 启用"这三件事只有一份判断，不会一边松一边紧。
    SetStatus(kError, QStringLiteral("无法启用定时备份"),
              QString::fromStdString(error));
    return false;
  }

  backupproject::ScheduleDocument next = document_;
  next.config = config;
  // 首次启用：下一次运行排在一个完整周期之后，而不是下一个 tick 就开跑。
  // 已经启用时这个函数是 no-op —— 保存配置不该把时间表往后推。
  backupproject::ApplyScheduleEnableTransition(
      &next, document_.config.enabled,
      static_cast<std::int64_t>(std::time(nullptr)));
  if (!store_.Save(next, &error)) {
    SetStatus(kError, QStringLiteral("计划保存失败"),
              QString::fromStdString(error));
    return false;
  }

  document_ = next;
  emit configChanged();
  ApplyRunnerLock();
  SetStatus(kSuccess, QStringLiteral("计划已保存"),
            config.enabled
                ? QStringLiteral("定时备份已启用。它只在本程序或 "
                                 "backupctl schedule watch 运行期间执行。")
                : QStringLiteral("定时备份已停用。"));
  return true;
}

bool ScheduleController::setEnabled(bool enabled) {
  return saveConfig(enabled, sourcePath(), intervalMinutes(), retainCount(),
                    packKey(), compressionKey(), includeRules(),
                    excludeRules());
}

QString ScheduleController::originForFile(const QString& file_name) const {
  if (file_name.isEmpty()) return QString();
  for (const backupproject::ScheduledSnapshotRecord& record :
       document_.state.managed_snapshots) {
    if (QString::fromStdString(record.file_name) == file_name) {
      return QStringLiteral("定时备份");
    }
  }
  // 不在 managed 名单里就是手动备份。schedule 状态丢失时这里会全部退化成
  // "手动备份" —— 那正是我们想要的降级：归档本身仍然可以列出、恢复、删除。
  return QStringLiteral("手动备份");
}

QString ScheduleController::changesForFile(const QString& file_name) const {
  if (file_name.isEmpty()) return QString();
  for (const backupproject::ScheduledSnapshotRecord& record :
       document_.state.managed_snapshots) {
    if (QString::fromStdString(record.file_name) != file_name) continue;
    // 变化摘要只对 managed 的计划快照有意义：手动备份没有"相对上一版"这回事。
    return ChangeText(record.changes.added, record.changes.removed,
                      record.changes.modified, record.changes.metadata_changed);
  }
  return QString();
}

QString ScheduleController::validateRule(const QString& action,
                                         const QString& rule) const {
  Filter filter;
  const FilterAction filter_action =
      action == QStringLiteral("exclude")
          ? backupproject::FilterAction::kExclude
          : backupproject::FilterAction::kInclude;
  std::string error;
  if (filter.AddRule(filter_action, rule.toStdString(), &error)) {
    return QString();
  }
  return QString::fromStdString(error);
}

bool ScheduleController::runNow() {
  if (!config_loaded_) {
    SetStatus(kError, QStringLiteral("计划配置不可用"), load_error_);
    return false;
  }
  if (!document_.config.enabled) {
    SetStatus(kError, QStringLiteral("定时备份尚未启用"),
              QStringLiteral("请先保存并启用计划，再执行“立即检查并运行”。"));
    return false;
  }
  if (!lock_.held()) {
    SetStatus(kError, QStringLiteral("计划任务已由另一进程持有"),
              runner_message_);
    return false;
  }
  Submit(/*force=*/true);
  return true;
}

void ScheduleController::DrainPending() {
  if (!pending_) return;
  if (busy_) return;
  // 手动操作还在跑就继续等。这不是 busy-spin：只有 busyChanged 才会再进来。
  if (backup_controller_ != nullptr && backup_controller_->busy()) return;
  const bool force = pending_force_;
  pending_ = false;
  pending_force_ = false;
  emit pendingChanged();
  Submit(force);
}

void ScheduleController::SetBusy(bool busy) {
  if (busy_ == busy) return;
  busy_ = busy;
  emit busyChanged();
}

void ScheduleController::SetStatus(const QString& kind, const QString& title,
                                   const QString& message) {
  status_kind_ = kind;
  status_title_ = title;
  status_message_ = message;
  emit statusChanged();
}

bool ScheduleController::waitForIdle(int timeout_ms) {
  QEventLoop loop;
  QTimer poll;
  poll.setInterval(10);
  QObject::connect(&poll, &QTimer::timeout, &loop, [this, &loop]() {
    if (!busy_ && !pending_) loop.quit();
  });
  QTimer guard;
  guard.setSingleShot(true);
  QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
  poll.start();
  guard.start(timeout_ms);
  if (!busy_ && !pending_) return true;
  loop.exec();
  return !busy_ && !pending_;
}

QString ScheduleController::localPathFromUrl(const QUrl& url) const {
  return url.toLocalFile();
}

QUrl ScheduleController::directoryDialogStartUrl(const QString& path) const {
  const QFileInfo info(path);
  if (!path.isEmpty() && info.exists() && info.isDir()) {
    return QUrl::fromLocalFile(info.absoluteFilePath());
  }
  return QUrl::fromLocalFile(QDir::homePath());
}

// ---- 只读访问器 ----

QString ScheduleController::triggerKey() const {
  return QString::fromLatin1(
      backupproject::BackupTriggerKey(document_.config.trigger));
}

QString ScheduleController::strategyKey() const {
  return QString::fromLatin1(
      backupproject::BackupStrategyKey(document_.config.strategy));
}

QString ScheduleController::sourcePath() const {
  return QString::fromStdString(document_.config.source_path);
}

int ScheduleController::intervalMinutes() const {
  return static_cast<int>(document_.config.interval_minutes);
}

int ScheduleController::retainCount() const {
  return static_cast<int>(document_.config.retain_count);
}

QString ScheduleController::packKey() const {
  return QString::fromLatin1(
      backupproject::PackMethodKey(document_.config.pack_method));
}

QString ScheduleController::compressionKey() const {
  return QString::fromLatin1(
      backupproject::CompressionMethodKey(document_.config.compression_method));
}

QString ScheduleController::encryptionKey() const {
  return QString::fromLatin1(
      backupproject::EncryptionMethodKey(document_.config.encryption_method));
}

QStringList ScheduleController::includeRules() const {
  QStringList list;
  for (const std::string& rule : document_.config.include_rules) {
    list.append(QString::fromStdString(rule));
  }
  return list;
}

QStringList ScheduleController::excludeRules() const {
  QStringList list;
  for (const std::string& rule : document_.config.exclude_rules) {
    list.append(QString::fromStdString(rule));
  }
  return list;
}

QString ScheduleController::supportedModeText() const {
  return QStringLiteral(
      "当前支持：定时触发 + 完整快照\n后续将扩展增量策略与实时触发。");
}

QString ScheduleController::lastRunText() const {
  return FormatLocalTime(document_.state.last_success_time_sec);
}

QString ScheduleController::nextRunText() const {
  return FormatLocalTime(document_.state.next_run_time_sec);
}

QString ScheduleController::lastResultText() const {
  if (document_.state.history.empty()) return QStringLiteral("尚无运行记录");
  const backupproject::ScheduleHistoryEntry& last =
      document_.state.history.back();
  QString text = QStringLiteral("%1 · %2").arg(
      FormatLocalTime(last.finished_at_sec),
      QString::fromUtf8(backupproject::ScheduleRunResultText(last.result)));
  if (!last.archive_file_name.empty()) {
    text += QStringLiteral(" · %1").arg(
        QString::fromStdString(last.archive_file_name));
  }
  if (!last.diagnostic.empty()) {
    text += QStringLiteral(" · ") + QString::fromStdString(last.diagnostic);
  }
  return text;
}

QVariantList ScheduleController::managedSnapshots() const {
  QVariantList list;
  for (const backupproject::ScheduledSnapshotRecord& record :
       document_.state.managed_snapshots) {
    QVariantMap item;
    item.insert(QStringLiteral("fileName"),
                QString::fromStdString(record.file_name));
    item.insert(QStringLiteral("createdText"),
                FormatLocalTime(record.created_time_sec));
    item.insert(QStringLiteral("sizeText"), FormatSize(record.archive_size));
    item.insert(QStringLiteral("entryCount"),
                static_cast<qulonglong>(record.entry_count));
    item.insert(QStringLiteral("packText"),
                QString::fromUtf8(
                    backupproject::PackMethodDisplayName(record.pack_method)));
    item.insert(QStringLiteral("compressionText"),
                QString::fromUtf8(backupproject::CompressionMethodDisplayName(
                    record.compression_method)));
    item.insert(
        QStringLiteral("changesText"),
        ChangeText(record.changes.added, record.changes.removed,
                   record.changes.modified, record.changes.metadata_changed));
    list.append(item);
  }
  return list;
}

QVariantList ScheduleController::history() const {
  QVariantList list;
  for (const backupproject::ScheduleHistoryEntry& entry :
       document_.state.history) {
    QVariantMap item;
    item.insert(QStringLiteral("timeText"),
                FormatLocalTime(entry.finished_at_sec));
    item.insert(
        QStringLiteral("resultKey"),
        QString::fromLatin1(backupproject::ScheduleRunResultKey(entry.result)));
    item.insert(
        QStringLiteral("resultText"),
        QString::fromUtf8(backupproject::ScheduleRunResultText(entry.result)));
    item.insert(
        QStringLiteral("changesText"),
        ChangeText(entry.changes.added, entry.changes.removed,
                   entry.changes.modified, entry.changes.metadata_changed));
    item.insert(QStringLiteral("archiveName"),
                QString::fromStdString(entry.archive_file_name));
    item.insert(QStringLiteral("diagnostic"),
                QString::fromStdString(entry.diagnostic));
    list.append(item);
  }
  return list;
}

}  // namespace backup_modern
