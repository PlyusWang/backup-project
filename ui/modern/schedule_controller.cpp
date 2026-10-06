// schedule_controller.cpp

#include "schedule_controller.h"

#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QVariantMap>
#include <QtConcurrent/QtConcurrentRun>
#include <cstdint>
#include <ctime>
#include <string>
#include <utility>

#include "backup_catalog.h"
#include "backup_mode.h"
#include "backup_option_keys.h"
#include "filter.h"
#include "format_bytes.h"
#include "schedule_frequency.h"

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

// 计划历史里的大小与其他页面同一套规则：以前这里只报裸字节。
QString FormatSize(std::uint64_t bytes) {
  return QString::fromStdString(backupproject::FormatByteSize(bytes));
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
                                       OperationGate* operation_gate,
                                       QObject* parent)
    : QObject(parent),
      schedule_file_path_(std::move(schedule_file_path)),
      backup_controller_(backup_controller),
      config_manager_(config_file_path.toStdString()),
      store_(schedule_file_path_.toStdString()),
      operation_gate_(operation_gate) {
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
    // 备份管理页删掉一份归档之后的计划状态同步不走信号：那一步必须发生在
    // BackupController 的删除闸门持有期内（见 OnArchiveDeleted 的声明）。
    // 由 main.cpp 通过 SetArchiveDeletedObserver(this) 建立这条直接连接。
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
    // 文件读不懂 = 计划不能用：明确挂起，而不是显示成"未启用"。
    SetConfigInvalid(true);
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
  // 文件本身读得懂，所以"读不懂"这条挂起理由不成立。配置**内容**是否合法
  // 由 Evaluate 回答（见 OnEvaluationFinished）：它能看见 Filter、仓库与
  // 无人值守加密边界，比这里的一次结构检查更完整。
  SetConfigInvalid(false);
  emit configChanged();
  emit stateChanged();
}

// ---- runner 锁 ----

void ScheduleController::ApplyRunnerLock() {
  const bool should_run =
      document_.config.enabled && config_loaded_ && !config_invalid_;

  if (!should_run) {
    if (lock_.held()) {
      lock_.Release();
      tick_.stop();
    }
    // 挂起与"未启用"是两件事：前者是配置坏了，用户需要看到不同的说法。
    if (config_invalid_) {
      SetRunnerMessage(QStringLiteral("计划配置不合法，定时备份已挂起。"));
    } else {
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

void ScheduleController::SetConfigInvalid(bool invalid) {
  if (config_invalid_ == invalid) return;
  config_invalid_ = invalid;
  if (invalid) {
    // 关键的一步：把 1 Hz 的 timer 停掉。只把状态标成"挂起"而让 timer 继续
    // 跑，Tick 仍然每秒被调用一次——虽然会被上面的提前返回挡住，但"停掉
    // timer"才是"不再周期性重试"这句话的直接实现。
    tick_.stop();
    // 挂起期间也不该占着 runner 锁：我们不会跑任何东西，占着它只会让
    // "这份计划现在由谁在跑"得到一个假答案。锁随挂起释放，恢复时
    // ApplyRunnerLock 会重新抢回来。
    if (lock_.held()) lock_.Release();
  }
  emit suspendedChanged();
}

void ScheduleController::SetRunnerMessage(const QString& text) {
  if (runner_message_ == text) return;
  runner_message_ = text;
  emit runnerChanged();
}

// ---- tick ----

void ScheduleController::Tick() {
  // 挂起时 tick 已经被停掉，这里再挡一次：即使 timer 真的又响了一次，
  // 也绝不会有第二次完整校验，更不会有第二次备份。
  if (config_invalid_) return;
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

void ScheduleController::OnArchiveDeleted(const QString& file_name) {
  Q_UNUSED(file_name);
  // 调用约定（见头文件与 operation_gate.h）：这一次调用发生在
  // BackupController 持有 kManualDelete 闸门的**同一持有期内**，所以这里
  // 不能再申请闸门，也不能启动后台任务——只做一次同步的 reconcile + 保存。
  // 读不懂 / 已挂起时一概不动：document_ 不是一份可信的 state，reconcile 之后
  // 保存也一定失败，写下去只会把一份坏配置覆盖成另一份坏配置。
  if (!config_loaded_ || config_invalid_) return;

  const std::size_t before = document_.state.managed_snapshots.size();
  // 与 CLI 的 repository delete 调的是同一个函数、同一个仓库。
  backupproject::ScheduledBackupService::ReconcileManagedSnapshots(
      repository_path_.toStdString(), &document_);
  if (document_.state.managed_snapshots.size() == before) {
    // 删掉的不是计划快照：一个字节都不写。
    return;
  }
  std::string error;
  if (!store_.Save(document_, &error)) {
    // 归档已经删掉了，这一步失败不能让"删除"这个动作看起来失败。如实提示，
    // 下一轮定时评估会再 reconcile 一次。
    SetStatus(kWarning, QStringLiteral("备份已删除，但计划状态没有更新"),
              QString::fromStdString(error));
    return;
  }
  emit stateChanged();
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

  // 闸门是真正的不变式：手动备份/恢复/删除/改仓库正在进行时，这一轮评估
  // 提交不出去，按"忙"处理（合并成一次 pending）。QML 的按钮状态不参与判断。
  OperationGate::Kind acquired = OperationGate::Kind::kNone;
  if (operation_gate_ != nullptr) {
    QString reason;
    if (!operation_gate_->Acquire(OperationGate::Kind::kScheduleEvaluation,
                                  &reason)) {
      if (!pending_) {
        pending_ = true;
        emit pendingChanged();
      }
      pending_force_ = pending_force_ || force;
      return;
    }
    acquired = OperationGate::Kind::kScheduleEvaluation;
  }

  const QString store_path = schedule_file_path_;
  const QString repository = repository_path_;
  last_succeeded_ = false;
  SetBusy(true);
  // 启动后台任务失败不是这里的路径（QtConcurrent 不会失败），所以持有期就是
  // 整个评估：OnEvaluationFinished 里释放。
  (void)acquired;
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
    case backupproject::ScheduleEvaluationStatus::kConfigInvalid:
      // 配置不合法：这一轮什么都没写。控制器据此挂起，不再周期性地重试。
      outcome.succeeded = false;
      outcome.config_invalid = true;
      break;
  }
  return outcome;
}

void ScheduleController::OnEvaluationFinished() {
  const ScheduleOutcome outcome = watcher_.result();
  last_succeeded_ = outcome.succeeded;
  // 先放开评估闸门：下面的 ApplyRunnerLock / DrainPending 都可能再次提交，
  // 而 SetBusy(false) 会触发 BackupController 侧的补跑。
  if (operation_gate_ != nullptr) {
    operation_gate_->Release(OperationGate::Kind::kScheduleEvaluation);
  }
  SetBusy(false);

  if (!outcome.error_message.isEmpty()) {
    SetStatus(kError, QStringLiteral("计划任务无法运行"),
              outcome.error_message);
  } else if (outcome.config_invalid) {
    // 挂起不是"这一次失败"：磁盘上的配置不合法，我们既不修也不猜。
    // 页面必须明确说清楚，并给出唯一的恢复入口。
    SetStatus(
        kError, QStringLiteral("计划配置不合法，定时备份已挂起"),
        (outcome.diagnostic.isEmpty() ? outcome.status_text
                                      : outcome.diagnostic) +
            QStringLiteral(" 这一轮没有写入任何内容，也不会自动重试。"
                           "请在本页修正后重新保存，保存成功即自动恢复。"));
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
  if (outcome.config_invalid) {
    // 顺序很重要：LoadFromDisk
    // 会清掉"文件读不懂"那条挂起理由（文件确实读得懂），
    // 所以评估级别的挂起必须在这之后重新立起来，而且**不再**调用
    // ApplyRunnerLock —— 那会把刚停掉的 tick 又启动回来。
    SetConfigInvalid(true);
  } else {
    ApplyRunnerLock();
  }
  emit operationFinished(last_succeeded_);
  // 挂起期间积压的 pending 没有意义：配置不合法，补跑一次也只是再失败一次。
  if (!config_invalid_) DrainPending();
}

// ---- 对外入口 ----

void ScheduleController::start() {
  // 显式（重新）启动是恢复挂起的入口之一：用户点了"重新载入"，就该老实
  // 重新读一次盘。LoadFromDisk 会按文件的实际状态重新决定是否挂起。
  SetConfigInvalid(false);
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

QString ScheduleController::frequencyValueText() const {
  std::string value;
  std::string unit;
  SplitFrequency(document_.config.interval_minutes, &value, &unit);
  return QString::fromStdString(value);
}

QString ScheduleController::frequencyUnitKey() const {
  std::string value;
  std::string unit;
  SplitFrequency(document_.config.interval_minutes, &value, &unit);
  return QString::fromStdString(unit);
}

QVariantList ScheduleController::frequencyUnits() const {
  QVariantList units;
  for (int i = 0; i < FrequencyUnitCount(); ++i) {
    const FrequencyUnit& unit = FrequencyUnitAt(i);
    QVariantMap option;
    option.insert(QStringLiteral("key"), QString::fromLatin1(unit.key));
    option.insert(QStringLiteral("label"), QString::fromUtf8(unit.label));
    units.push_back(option);
  }
  return units;
}

bool ScheduleController::saveConfigFromFrequencyText(
    bool enabled, const QString& source_path, const QString& value_text,
    const QString& unit_key, const QString& retain_text,
    const QString& pack_key, const QString& compression_key,
    const QStringList& include_rules, const QStringList& exclude_rules,
    const QString& strategy_key) {
  std::uint32_t interval = 0;
  std::string error;
  // 值 × 单位 -> 分钟。范围与溢出都在 schedule_frequency.cpp 里判，
  // 数值文本本身仍然由共享核心的 ParseBoundedScheduleNumber 解析。
  if (!ParseFrequency(value_text.toStdString(), unit_key.toStdString(),
                      &interval, &error)) {
    SetStatus(kError, QStringLiteral("备份频率不合法"),
              QStringLiteral(
                  "备份频率必须是正整数，判断规则与 backupctl 完全一致。") +
                  QStringLiteral(" ") + QString::fromStdString(error));
    return false;
  }
  return saveConfigFromText(enabled, source_path, QString::number(interval),
                            retain_text, pack_key, compression_key,
                            include_rules, exclude_rules, strategy_key);
}

bool ScheduleController::saveConfigFromText(
    bool enabled, const QString& source_path, const QString& interval_text,
    const QString& retain_text, const QString& pack_key,
    const QString& compression_key, const QStringList& include_rules,
    const QStringList& exclude_rules, const QString& strategy_key) {
  std::uint32_t interval = 0;
  std::uint32_t retain = 0;
  std::string error;
  // 与 backupctl schedule set --interval-minutes / --retain 是同一个函数、
  // 同一套规则；界面不再做任何自己的"解析"。
  if (!backupproject::ParseBoundedScheduleNumber(
          interval_text.toStdString(), backupproject::kMinIntervalMinutes,
          backupproject::kMaxIntervalMinutes,
          QStringLiteral("周期（分钟）").toStdString(), &interval, &error) ||
      !backupproject::ParseBoundedScheduleNumber(
          retain_text.toStdString(), backupproject::kMinRetainCount,
          backupproject::kMaxRetainCount,
          QStringLiteral("保留数量").toStdString(), &retain, &error)) {
    SetStatus(kError, QStringLiteral("计划配置不合法"),
              QStringLiteral("周期与保留数量都必须是十进制整数，判断规则与 "
                             "backupctl 完全一致。") +
                  QStringLiteral(" ") + QString::fromStdString(error));
    return false;
  }
  return saveConfig(enabled, source_path, static_cast<int>(interval),
                    static_cast<int>(retain), pack_key, compression_key,
                    include_rules, exclude_rules, strategy_key);
}

bool ScheduleController::saveConfig(bool enabled, const QString& source_path,
                                    int interval_minutes, int retain_count,
                                    const QString& pack_key,
                                    const QString& compression_key,
                                    const QStringList& include_rules,
                                    const QStringList& exclude_rules,
                                    const QString& strategy_key) {
  // ---- 同一进程内的单写者规则 ----
  //
  // ScheduleStore 的写者有两个：本函数（GUI 线程）与后台评估线程
  // （RunEvaluation -> ScheduledBackupService ->
  // ScheduleStore::Save/SaveManifest）。 两者同时写就是并发的
  // Load->Modify->Save：后台那一路刚 push 进去的 managed 记录与 history
  // 会被这一路整体覆盖掉，归档从此脱离 retention 管理。
  //
  // 产品只允许一个进程，但进程内仍然有两个线程，所以这条规则必须由 C++ 保证，
  // 而不是只靠 QML 把按钮置灰（那是界面礼貌，不是不变式）。评估在飞的时候
  // 直接拒绝保存，用户等它结束再点。
  //
  // busy_ 只看本控制器；闸门看的是整个 GUI：手动备份/恢复/删除/改仓库正在
  // 进行时，保存计划同样必须被拒绝——那几步都会改动持久状态。
  OperationGuard guard(operation_gate_, OperationGate::Kind::kScheduleConfig);
  if (operation_gate_ != nullptr && !guard.acquired()) {
    SetStatus(kWarning, QStringLiteral("另一个操作正在进行"), guard.reason());
    return false;
  }
  if (busy_) {
    SetStatus(
        kWarning, QStringLiteral("计划任务正在运行"),
        QStringLiteral(
            "定时备份正在评估/写盘，此时保存会与它并发写入同一份计划状态。"
            "请等这一轮结束后再保存。"));
    return false;
  }
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

  // 策略同样是共享核心的 key：与 backupctl schedule set --strategy 逐字一致。
  // 解析失败不回退到 full —— 用户明确选了增量，就必须拿到增量或明确的错误。
  if (!backupproject::ParseBackupStrategyKey(strategy_key.toStdString(),
                                             &config.strategy)) {
    SetStatus(kError, QStringLiteral("未知的备份策略"), strategy_key);
    return false;
  }
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
  // 保存成功 = 磁盘上现在有一份合法配置 = 挂起理由消失。这是唯一的自动恢复点。
  SetConfigInvalid(false);
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
  if (config_invalid_) {
    SetStatus(kError, QStringLiteral("计划配置不合法，定时备份已挂起"),
              QStringLiteral("请先在本页修正并保存计划配置。"));
    return false;
  }
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
  // 面向用户的一句话能力说明。以前那句还写着"后续将扩展增量策略"，那是 PR #18
  // 之前的实情；现在增量已经是计划路径上真实支持的一种方式，继续留着就是误导。
  return QStringLiteral("当前支持：定时触发；备份方式可选完整备份或增量备份。");
}

QString ScheduleController::encryptionNote() const {
  // 唯一来源：核心那句"无人值守为什么不加密"。GUI 不复制一份字面量。
  return QString::fromStdString(
      backupproject::UnattendedEncryptionDisabledReason(
          backupproject::BackupTrigger::kScheduled));
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
