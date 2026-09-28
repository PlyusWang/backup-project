// realtime_controller.cpp
//
// 见 realtime_controller.h。

#include "realtime_controller.h"

#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QSocketNotifier>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <string>
#include <utility>

#include "backup_option_keys.h"
#include "filter.h"

namespace backup_modern {

namespace {

const char kIdle[] = "idle";
const char kRunning[] = "running";
const char kSuccess[] = "success";
const char kWarning[] = "warning";
const char kError[] = "error";

// 状态机格子。字符串就是 QML 拿到的 key，断言与文案都以它为准。
const char kPhaseDisabled[] = "disabled";
const char kPhaseWatching[] = "watching";
const char kPhaseDebouncing[] = "debouncing";
const char kPhaseResync[] = "resync";
const char kPhaseSnapshotCreated[] = "snapshot_created";
const char kPhaseNoChanges[] = "no_changes";
const char kPhaseRetentionWarning[] = "retention_warning";
const char kPhaseWatchDegraded[] = "watch_degraded";
const char kPhaseWatchRecovered[] = "watch_recovered";
const char kPhaseConfigError[] = "config_error";
const char kPhaseFailed[] = "failed";

// 闸门被占用时的重试间隔。150 ms 足够快（用户感觉不到），又远不到 busy-spin：
// 一秒最多 7 次 Acquire，而且只在真有 pending generation 的时候才跑。
const int kGateRetryMs = 150;
// 监听降级之后的重建间隔。与 backupctl realtime watch 的 1000 ms 对齐。
const int kRecoveryMs = 1000;
// debounce 定时器最长一次睡多久：即使 WaitMs 报了很远的未来，也要周期性醒来
// 重新问一次 Due，免得配置/时钟变化把一次触发永久挂住。
const std::int64_t kDebounceTickCapMs = 200;

// 单调时钟。RealtimeDebouncer 用的是"注入的毫秒"，不能拿墙钟：用户改系统时间
// 不该让一次触发提前或永远不来。
std::int64_t SteadyMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

QString FormatLocalTime(std::int64_t seconds) {
  if (seconds <= 0) return QStringLiteral("时间未知");
  return QDateTime::fromSecsSinceEpoch(static_cast<qint64>(seconds))
      .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

QString FormatSize(std::uint64_t bytes) {
  const double value = static_cast<double>(bytes);
  if (bytes >= 1024ull * 1024ull) {
    return QStringLiteral("%1 MB").arg(value / (1024.0 * 1024.0), 0, 'f', 2);
  }
  if (bytes >= 1024ull) {
    return QStringLiteral("%1 KB").arg(value / 1024.0, 0, 'f', 1);
  }
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

// RealtimeOutcome::Kind -> 稳定的 key。与 backupctl realtime 打印的
// outcome= 用的是同一组词。
const char* OutcomeKindKey(backupproject::RealtimeOutcome::Kind kind) {
  switch (kind) {
    case backupproject::RealtimeOutcome::Kind::kFullSnapshot:
      return "full-snapshot";
    case backupproject::RealtimeOutcome::Kind::kFullBaseline:
      return "full-baseline";
    case backupproject::RealtimeOutcome::Kind::kDelta:
      return "delta";
    case backupproject::RealtimeOutcome::Kind::kNoChanges:
      return "no-changes";
    case backupproject::RealtimeOutcome::Kind::kFailed:
      return "failed";
  }
  return "failed";
}

QString OutcomeKindText(backupproject::RealtimeOutcome::Kind kind) {
  switch (kind) {
    case backupproject::RealtimeOutcome::Kind::kFullSnapshot:
      return QStringLiteral("已创建实时完整快照");
    case backupproject::RealtimeOutcome::Kind::kFullBaseline:
      return QStringLiteral("已创建实时增量基线（完整快照）");
    case backupproject::RealtimeOutcome::Kind::kDelta:
      return QStringLiteral("已写入实时增量备份");
    case backupproject::RealtimeOutcome::Kind::kNoChanges:
      return QStringLiteral("增量策略下没有变化，未写入任何内容");
    case backupproject::RealtimeOutcome::Kind::kFailed:
      return QStringLiteral("实时备份失败");
  }
  return QStringLiteral("实时备份失败");
}

// marker 里的 outcome_kind -> 给人看的"这一份是怎么来的"。
QString SnapshotKindText(const std::string& outcome_kind) {
  if (outcome_kind == "full") return QStringLiteral("完整快照");
  if (outcome_kind == "full-baseline") return QStringLiteral("增量基线");
  if (outcome_kind == "delta") return QStringLiteral("增量");
  if (outcome_kind == "no-changes") return QStringLiteral("无变化");
  if (outcome_kind.empty()) return QStringLiteral("未知");
  return QString::fromStdString(outcome_kind);
}

// baseline / delta：这一份快照在依赖链里扮演什么角色。
QString SnapshotBasisText(const std::string& outcome_kind) {
  if (outcome_kind == "delta") return QStringLiteral("delta");
  if (outcome_kind == "full" || outcome_kind == "full-baseline") {
    return QStringLiteral("baseline");
  }
  return QStringLiteral("-");
}

// 数字解析。与 backupctl realtime 的 ParseUint32 逐条一致：只收纯十进制数字
// （不做 trim、不收正负号、不收空格），累加过程中就夹住上界避免溢出。
// 范围本身**不在这里判断** —— 100..60000 / 500..300000 / 1..1000 是共享核心
// ValidateRealtimeConfig 的结论，前端不许有第二套边界。
bool ParseRealtimeNumber(const QString& text, const QString& label,
                         std::uint32_t* value, std::string* error_message) {
  if (text.isEmpty()) {
    *error_message = label.toStdString() + " needs a number";
    return false;
  }
  std::uint64_t result = 0;
  for (const QChar character : text) {
    const ushort code = static_cast<ushort>(character.unicode());
    if (code < static_cast<ushort>('0') || code > static_cast<ushort>('9')) {
      *error_message = label.toStdString() + " must be a decimal integer";
      return false;
    }
    result = result * 10u + static_cast<std::uint64_t>(code - '0');
    if (result > 100000000ull) {
      *error_message = label.toStdString() + " is out of range";
      return false;
    }
  }
  *value = static_cast<std::uint32_t>(result);
  return true;
}

}  // namespace

RealtimeController::RealtimeController(QString realtime_file_path,
                                       const QString& config_file_path,
                                       OperationGate* operation_gate,
                                       QObject* parent)
    : QObject(parent),
      realtime_file_path_(std::move(realtime_file_path)),
      config_manager_(config_file_path.toStdString()),
      store_(realtime_file_path_.toStdString()),
      operation_gate_(operation_gate) {
  // debounce 的推进只有一条路：一个单次定时器睡到 WaitMs 报的那一点，醒来问
  // 共享核心的 Due。界面不算时间、也不自己判断"稳定了没有"。
  debounce_timer_.setSingleShot(true);
  connect(&debounce_timer_, &QTimer::timeout, this,
          &RealtimeController::OnDebounceTimeout);

  // 闸门被占用时的轻量重试。刻意不是 0 间隔的 busy-spin。
  retry_timer_.setSingleShot(true);
  retry_timer_.setInterval(kGateRetryMs);
  connect(&retry_timer_, &QTimer::timeout, this,
          &RealtimeController::OnRetryTimeout);

  // 监听降级之后的重建。与 CLI watch 的 1000 ms 重试保持一致。
  recovery_timer_.setSingleShot(true);
  recovery_timer_.setInterval(kRecoveryMs);
  connect(&recovery_timer_, &QTimer::timeout, this,
          &RealtimeController::OnRecoveryTimeout);

  connect(&run_watcher_, &QFutureWatcher<RealtimeRunResult>::finished, this,
          &RealtimeController::OnRunFinished);
  connect(&list_watcher_, &QFutureWatcher<RealtimeRunResult>::finished, this,
          &RealtimeController::OnListFinished);
}

RealtimeController::~RealtimeController() {
  // 析构时先停表、再 detach：绝不留下一个还会回调进已析构对象的定时器。
  debounce_timer_.stop();
  retry_timer_.stop();
  recovery_timer_.stop();
  ClearNotifier();
  watcher_.Detach();
}

// ---- 配置装载 ----

void RealtimeController::LoadFromDisk() {
  backupproject::RealtimeConfig config;
  std::string error;
  const backupproject::RealtimeLoadStatus status = store_.Load(&config, &error);

  config_error_ = false;
  load_error_.clear();

  switch (status) {
    case backupproject::RealtimeLoadStatus::kLoaded:
      config_ = config;
      config_loaded_ = true;
      // 读得懂不等于合法。结构校验用共享核心那一个函数，前端不复刻规则。
      if (!backupproject::ValidateRealtimeConfig(config_, &error)) {
        config_error_ = true;
        load_error_ = QString::fromStdString(error);
      }
      break;
    case backupproject::RealtimeLoadStatus::kMissing:
      // 文件还不存在 = 用默认配置（未启用）。这不是错误。
      config_ = backupproject::RealtimeConfig();
      config_loaded_ = true;
      break;
    case backupproject::RealtimeLoadStatus::kError:
      // 文件在，但读不出来。绝不"自动恢复成默认值"——那是把用户的配置
      // 悄悄换掉。
      config_loaded_ = false;
      config_error_ = true;
      load_error_ = QString::fromStdString(error);
      break;
  }
  emit configChanged();
}

bool RealtimeController::RefreshRepository(std::string* error_message) {
  backupproject::AppConfig app_config;
  const backupproject::ConfigLoadStatus status =
      config_manager_.Load(&app_config, error_message);
  if (status == backupproject::ConfigLoadStatus::kError) {
    return false;
  }
  repository_path_ = QString::fromStdString(app_config.backup_repository_path);
  emit runtimeChanged();
  if (repository_path_.isEmpty()) {
    *error_message = "尚未配置备份仓库（请在设置页选择仓库目录）";
    return false;
  }
  return true;
}

void RealtimeController::start() {
  StopWatching();
  watch_degraded_ = false;
  watch_degraded_reason_.clear();
  SetPending(false);
  pending_generation_valid_ = false;

  LoadFromDisk();
  std::string repository_error;
  RefreshRepository(&repository_error);

  if (!config_loaded_ || config_error_) {
    SetPhase(QString::fromLatin1(kPhaseConfigError),
             QStringLiteral("实时配置不可用：%1").arg(load_error_));
    SetStatus(
        kError, QStringLiteral("实时备份配置不可用"),
        load_error_ + QStringLiteral(" 程序不会自动修改它，也不会自动重试；"
                                     "请在本页修正后重新保存。"));
    refreshSnapshots();
    return;
  }

  if (!config_.enabled) {
    SetPhase(QString::fromLatin1(kPhaseDisabled),
             QStringLiteral("实时备份未启用"));
    SetStatus(kIdle, QStringLiteral("等待启用实时备份"),
              QStringLiteral("启用后，本程序运行期间会监听源目录；"
                             "关掉程序就不再监听。"));
    refreshSnapshots();
    return;
  }

  BeginWatching();
  refreshSnapshots();
}

void RealtimeController::reload() { start(); }

void RealtimeController::stop() {
  StopWatching();
  SetPending(false);
  pending_generation_valid_ = false;
  SetPhase(QString::fromLatin1(kPhaseDisabled),
           QStringLiteral("实时备份已停止"));
  emit runtimeChanged();
}

void RealtimeController::clearStatus() {
  if (busy_) return;
  status_kind_ = QString::fromLatin1(kIdle);
  status_title_ = QStringLiteral("等待实时备份");
  status_message_.clear();
  emit statusChanged();
}

// ---- watcher 生命周期 ----

bool RealtimeController::TryStartWatching() {
  if (!config_loaded_ || config_error_ || !config_.enabled) return false;
  if (config_.source_path.empty()) {
    watch_degraded_ = true;
    watch_degraded_reason_ = QStringLiteral("配置里没有源目录");
    SetPhase(QString::fromLatin1(kPhaseWatchDegraded),
             QStringLiteral("监听已降级：配置里没有源目录"));
    SetStatus(kError, QStringLiteral("实时备份无法开始监听"),
              watch_degraded_reason_);
    return false;
  }

  std::string error;
  if (!RefreshRepository(&error)) {
    watch_degraded_ = true;
    watch_degraded_reason_ = QString::fromStdString(error);
    SetPhase(QString::fromLatin1(kPhaseWatchDegraded),
             QStringLiteral("监听已降级：%1").arg(watch_degraded_reason_));
    SetStatus(
        kError, QStringLiteral("实时备份无法运行"),
        watch_degraded_reason_ +
            QStringLiteral(" 程序每秒重试一次；仓库配置好后会自动恢复。"));
    return false;
  }

  // 源目录存在性 / 真实目录 / 不是软链接、仓库可解析、两者不重叠 —— 全部问
  // 共享核心的 ValidateRealtimeForEnable（backupctl realtime enable 用的是
  // 同一个函数），前端不另写一套判断。
  std::string identity;
  if (!backupproject::ValidateRealtimeForEnable(
          config_, repository_path_.toStdString(), &error, &identity)) {
    watch_degraded_ = true;
    watch_degraded_reason_ = QString::fromStdString(error);
    SetPhase(QString::fromLatin1(kPhaseWatchDegraded),
             QStringLiteral("监听已降级：%1").arg(watch_degraded_reason_));
    SetStatus(kError, QStringLiteral("实时备份暂时无法运行"),
              watch_degraded_reason_);
    return false;
  }
  repository_identity_ = identity;

  if (!watcher_.Attach(config_.source_path, &error)) {
    watch_degraded_ = true;
    watch_degraded_reason_ = QString::fromStdString(error);
    SetPhase(QString::fromLatin1(kPhaseWatchDegraded),
             QStringLiteral("监听已降级：%1").arg(watch_degraded_reason_));
    SetStatus(kError, QStringLiteral("实时备份无法开始监听"),
              watch_degraded_reason_);
    return false;
  }
  InstallNotifier();

  debouncer_ = std::make_unique<backupproject::RealtimeDebouncer>(
      config_.debounce_ms, config_.max_wait_ms);
  // 进程不运行期间没有事件，所以 attach 成功之后必须先合成一次 resync：
  // 否则"关掉再打开"那段时间里的变化会被永远漏掉。
  debouncer_->NoteResync(SteadyMs());
  watch_degraded_ = false;
  watch_degraded_reason_.clear();
  ArmDebounceTimer();
  SetPhase(QString::fromLatin1(kPhaseResync),
           QStringLiteral("已开始监听，正在做一次重新同步"));
  SetStatus(kSuccess, QStringLiteral("实时备份已启用"),
            QStringLiteral("正在监听 %1（%2 个目录）。关掉程序就不再监听。")
                .arg(QString::fromStdString(config_.source_path))
                .arg(static_cast<int>(watcher_.watch_count())));
  emit runtimeChanged();
  return true;
}

void RealtimeController::BeginWatching() {
  if (TryStartWatching()) return;
  if (config_.enabled && config_loaded_ && !config_error_) {
    recovery_timer_.start(kRecoveryMs);
  }
  emit runtimeChanged();
}

void RealtimeController::StopWatching() {
  debounce_timer_.stop();
  retry_timer_.stop();
  recovery_timer_.stop();
  ClearNotifier();
  watcher_.Detach();
  debouncer_.reset();
  emit runtimeChanged();
}

void RealtimeController::InstallNotifier() {
  ClearNotifier();
  if (watcher_.fd() < 0) return;
  // watcher 的 fd 在主线程读：QSocketNotifier 把"可读"变成一次槽调用，
  // 后台线程只有真正落盘的那一件事。
  notifier_ = new QSocketNotifier(watcher_.fd(), QSocketNotifier::Read, this);
  connect(notifier_, &QSocketNotifier::activated, this,
          &RealtimeController::OnWatcherReadable);
}

void RealtimeController::ClearNotifier() {
  if (notifier_ == nullptr) return;
  notifier_->setEnabled(false);
  notifier_->deleteLater();
  notifier_ = nullptr;
}

void RealtimeController::EnterDegraded(const QString& reason) {
  ClearNotifier();
  watcher_.Detach();
  debouncer_.reset();
  debounce_timer_.stop();
  watch_degraded_ = true;
  watch_degraded_reason_ = reason;
  SetPhase(QString::fromLatin1(kPhaseWatchDegraded),
           QStringLiteral("监听已降级：%1").arg(reason));
  SetStatus(
      kError, QStringLiteral("实时监听已降级"),
      reason + QStringLiteral(" 程序每秒重试一次；恢复后会合成一次重新同步，"
                              "不会漏掉这段时间里的变化。"));
  if (config_.enabled && config_loaded_ && !config_error_) {
    recovery_timer_.start(kRecoveryMs);
  }
  emit runtimeChanged();
}

void RealtimeController::OnRecoveryTimeout() {
  if (!config_.enabled || !config_loaded_ || config_error_) {
    recovery_timer_.stop();
    return;
  }
  if (TryStartWatching()) {
    SetPhase(QString::fromLatin1(kPhaseWatchRecovered),
             QStringLiteral("监听已恢复，并合成了一次重新同步"));
    SetStatus(kSuccess, QStringLiteral("实时监听已恢复"),
              QStringLiteral("重新建立了 %1 个目录的监听。")
                  .arg(static_cast<int>(watcher_.watch_count())));
    emit runtimeChanged();
    return;
  }
  // 仍然起不来：继续每秒重试。这不是 busy-spin —— 一次重试是几次 stat /
  // realpath，而且只在降级期间发生。
  recovery_timer_.start(kRecoveryMs);
}

// ---- 事件 ----

void RealtimeController::OnWatcherReadable() {
  if (!watcher_.attached() || debouncer_ == nullptr) return;

  backupproject::WatchBatch batch;
  std::string error;
  if (!watcher_.Drain(&batch, &error)) {
    EnterDegraded(QString::fromStdString(error));
    return;
  }
  if (!batch.any_event) return;

  const std::int64_t now = SteadyMs();
  // 事件只喂给共享核心的 debouncer。合并窗口的算法（trailing edge + max_wait
  // 硬上限）在 RealtimeDebouncer 里，界面不复刻。
  debouncer_->NoteEvents(batch.event_count, batch.structural, batch.overflow,
                         now);

  if (batch.structural || batch.overflow) {
    // 结构变化之后整体重建：新 fd 建成功之后才关旧的，不允许出现空窗。
    std::string rebuild_error;
    if (!watcher_.Rebuild(&rebuild_error)) {
      EnterDegraded(QString::fromStdString(rebuild_error));
      return;
    }
    InstallNotifier();
  }
  if (batch.root_lost) {
    EnterDegraded(QStringLiteral("源目录本身已消失（被删除或移动）"));
    return;
  }
  if (batch.overflow) {
    // 事件历史已经不可信，唯一正确的做法是重新观察当前源树。
    ++overflow_resync_count_;
    debouncer_->NoteResync(now);
    SetPhase(QString::fromLatin1(kPhaseResync),
             QStringLiteral("事件队列溢出，正在重新同步"));
  } else {
    SetPhase(QString::fromLatin1(kPhaseDebouncing),
             QStringLiteral("正在等待事件稳定（debounce %1 ms / 最长 %2 ms）")
                 .arg(config_.debounce_ms)
                 .arg(config_.max_wait_ms));
  }

  last_generation_ = debouncer_->generation();
  last_event_count_ = batch.event_count;
  last_overflow_ = batch.overflow;
  last_resync_ = batch.overflow;
  last_event_text_ =
      QStringLiteral("最近一批：%1 条事件（第 %2 代）%3%4")
          .arg(static_cast<qulonglong>(batch.event_count))
          .arg(static_cast<qulonglong>(debouncer_->generation()))
          .arg(batch.structural ? QStringLiteral("、含结构变化") : QString())
          .arg(batch.overflow ? QStringLiteral("、含溢出") : QString());
  ArmDebounceTimer();
  emit runtimeChanged();
}

void RealtimeController::ArmDebounceTimer() {
  if (debouncer_ == nullptr || !debouncer_->dirty()) {
    debounce_timer_.stop();
    return;
  }
  const std::int64_t wait = debouncer_->WaitMs(SteadyMs());
  std::int64_t interval = wait;
  if (interval < 1) interval = 1;
  if (interval > kDebounceTickCapMs) interval = kDebounceTickCapMs;
  debounce_timer_.start(static_cast<int>(interval));
}

void RealtimeController::OnDebounceTimeout() {
  if (debouncer_ == nullptr) return;
  const std::int64_t now = SteadyMs();
  if (!debouncer_->dirty()) return;
  if (!debouncer_->Due(now)) {
    // 还没到点：再睡一小段。Due 的判定在共享核心里，界面不算时间。
    ArmDebounceTimer();
    return;
  }

  const backupproject::RealtimeGeneration generation = debouncer_->Consume(now);
  if (generation.empty()) return;

  if (generation.resync) {
    SetPhase(QString::fromLatin1(kPhaseResync),
             QStringLiteral("正在重新同步当前源树"));
  }
  Submit(generation);
}

void RealtimeController::OnRetryTimeout() {
  if (!pending_generation_valid_) return;
  if (busy_) return;  // 回收路径会在任务结束时把 pending 交出去。
  // 闸门还被别人占着：原样留着那一代，再等 150 ms。绝不丢 trigger，
  // 也绝不排第二个队列。
  Submit(pending_generation_);
}

// ---- 提交 ----

void RealtimeController::MergePending(
    const backupproject::RealtimeGeneration& generation) {
  if (!pending_generation_valid_) {
    pending_generation_ = generation;
    pending_generation_valid_ = true;
  } else {
    // 一次只保留**一个** pending generation：多次到期在这里 coalesce。
    pending_generation_.event_count += generation.event_count;
    pending_generation_.overflow_seen =
        pending_generation_.overflow_seen || generation.overflow_seen;
    pending_generation_.structural_seen =
        pending_generation_.structural_seen || generation.structural_seen;
    pending_generation_.resync =
        pending_generation_.resync || generation.resync;
    pending_generation_.settled_ms =
        std::max(pending_generation_.settled_ms, generation.settled_ms);
    pending_generation_.generation =
        std::max(pending_generation_.generation, generation.generation);
  }
  SetPending(true);
  emit runtimeChanged();
}

void RealtimeController::SetPending(bool pending) {
  if (pending_generation_valid_ == pending) return;
  pending_generation_valid_ = pending;
  emit pendingChanged();
}

void RealtimeController::Submit(backupproject::RealtimeGeneration generation) {
  if (busy_) {
    // 后台任务在飞：合并成那一个 pending，等 OnRunFinished 再交出去。
    MergePending(generation);
    return;
  }
  if (!config_loaded_ || config_error_) {
    pending_generation_valid_ = false;
    emit pendingChanged();
    SetPhase(QString::fromLatin1(kPhaseConfigError),
             QStringLiteral("实时配置不可用，这次触发被丢弃"));
    return;
  }
  if (!config_.enabled) {
    pending_generation_valid_ = false;
    emit pendingChanged();
    return;
  }

  std::string repository_error;
  if (repository_path_.isEmpty() && !RefreshRepository(&repository_error)) {
    // 没有仓库就没有可写的地方。如实报错、丢掉这一代，而不是留一个永远
    // 交不出去的 pending 在那空转。
    pending_generation_valid_ = false;
    emit pendingChanged();
    SetPhase(QString::fromLatin1(kPhaseConfigError),
             QStringLiteral("没有可用的备份仓库，这次触发被丢弃"));
    SetStatus(kError, QStringLiteral("实时备份无法运行"),
              QString::fromStdString(repository_error));
    return;
  }

  // 闸门是真正的不变式：手动备份 / 恢复 / 删除 / 改仓库 / 保存计划正在进行时，
  // 这一轮提交不出去。抢不到就把这一代留着，150 ms 后再试。
  if (operation_gate_ != nullptr) {
    QString reason;
    if (!operation_gate_->Acquire(OperationGate::Kind::kRealtimeEvaluation,
                                  &reason)) {
      MergePending(generation);
      retry_timer_.start(kGateRetryMs);
      SetStatus(kWarning, QStringLiteral("另一个操作正在进行，已排队"),
                reason + QStringLiteral(" 触发不会丢：这一代已经留下，"
                                        "等它结束后立刻执行。"));
      return;
    }
  }

  pending_generation_valid_ = false;
  emit pendingChanged();

  last_succeeded_ = false;
  SetBusy(true);
  SetPhase(QString::fromLatin1(kPhaseResync),
           generation.resync ? QStringLiteral("正在执行重新同步触发的备份")
                             : QStringLiteral("正在执行事件触发的备份"));
  SetStatus(kRunning, QStringLiteral("实时备份运行中"),
            generation.resync
                ? QStringLiteral("重新同步：正在观察当前源树并建立快照…")
                : QStringLiteral("事件已经稳定（合并 %1 条），正在建立快照…")
                      .arg(static_cast<qulonglong>(generation.event_count)));
  run_watcher_.setFuture(QtConcurrent::run(
      &RealtimeController::RunOnce, config_, repository_path_.toStdString(),
      repository_identity_, generation));
}

void RealtimeController::DrainPending() {
  if (!pending_generation_valid_ || busy_) return;
  const backupproject::RealtimeGeneration generation = pending_generation_;
  // Submit 会自己决定是"交出去"还是"继续排队"。
  Submit(generation);
}

// ---- 后台任务 ----

RealtimeRunResult RealtimeController::RunOnce(
    backupproject::RealtimeConfig config, std::string repository_path,
    std::string repository_identity,
    const backupproject::RealtimeGeneration& generation) {
  RealtimeRunResult result;
  result.generation = generation.generation;
  result.event_count = generation.event_count;
  result.overflow = generation.overflow_seen;
  result.resync = generation.resync;

  backupproject::RealtimeEventSummary events;
  events.generation = generation.generation;
  events.event_count = generation.event_count;
  events.overflow = generation.overflow_seen;
  events.structural = generation.structural_seen;
  events.resync = generation.resync;

  std::string error;
  const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
  result.ok = backupproject::RunRealtimeBackupOnce(
      config, repository_path, repository_identity, events, now,
      &result.outcome, &error);
  result.error_message = error;

  // 快照列表在同一趟里取回来：界面线程不做文件 IO，也不做 archive 字节校验。
  std::string list_error;
  result.listed = backupproject::ListRealtimeSnapshots(
      repository_path, &result.snapshots, &list_error);
  result.list_error = list_error;
  return result;
}

RealtimeRunResult RealtimeController::ListOnly(std::string repository_path) {
  RealtimeRunResult result;
  std::string error;
  result.listed = backupproject::ListRealtimeSnapshots(
      repository_path, &result.snapshots, &error);
  result.list_error = error;
  return result;
}

void RealtimeController::ApplySnapshots(const RealtimeRunResult& result) {
  QVariantList items;
  for (const backupproject::RealtimeSnapshotRecord& record : result.snapshots) {
    QVariantMap item;
    item.insert(QStringLiteral("fileName"),
                QString::fromStdString(record.file_name));
    item.insert(QStringLiteral("archiveName"),
                QString::fromStdString(record.file_name));
    item.insert(QStringLiteral("createdText"),
                FormatLocalTime(record.created_time_sec));
    item.insert(
        QStringLiteral("strategyKey"),
        QString::fromLatin1(backupproject::BackupStrategyKey(record.strategy)));
    item.insert(QStringLiteral("strategyText"),
                record.strategy == backupproject::BackupStrategy::kFull
                    ? QStringLiteral("Full")
                    : QStringLiteral("Incremental"));
    item.insert(QStringLiteral("kindText"),
                SnapshotKindText(record.outcome_kind));
    item.insert(QStringLiteral("basisText"),
                SnapshotBasisText(record.outcome_kind));
    item.insert(
        QStringLiteral("packText"),
        QString::fromLatin1(backupproject::PackMethodKey(record.pack_method)));
    item.insert(QStringLiteral("compressionText"),
                QString::fromLatin1(backupproject::CompressionMethodKey(
                    record.compression_method)));
    item.insert(QStringLiteral("eventCount"),
                static_cast<qulonglong>(record.event_count));
    item.insert(QStringLiteral("changesText"),
                ChangeText(record.added, record.removed, record.modified,
                           record.metadata_changed));
    item.insert(QStringLiteral("sizeText"), FormatSize(record.archive_size));
    item.insert(QStringLiteral("verified"), record.verified);
    item.insert(QStringLiteral("verifiedText"),
                record.verified ? QStringLiteral("已校验")
                                : QStringLiteral("未通过校验"));
    item.insert(QStringLiteral("diagnostic"),
                QString::fromStdString(record.diagnostic));
    item.insert(QStringLiteral("overflowRecovery"), record.overflow_recovery);
    item.insert(QStringLiteral("resyncTrigger"), record.resync_trigger);
    items.append(item);
  }
  snapshot_items_ = items;
  emit snapshotsChanged();
}

void RealtimeController::refreshSnapshots() {
  if (repository_path_.isEmpty()) {
    std::string error;
    if (!RefreshRepository(&error)) return;
  }
  if (list_busy_) {
    // latest request wins：正在列的时候来的请求只记一下，等它回来再列一次。
    list_pending_ = true;
    return;
  }
  list_busy_ = true;
  list_watcher_.setFuture(QtConcurrent::run(&RealtimeController::ListOnly,
                                            repository_path_.toStdString()));
}

void RealtimeController::OnListFinished() {
  const RealtimeRunResult result = list_watcher_.result();
  list_busy_ = false;
  if (result.listed) ApplySnapshots(result);
  if (list_pending_) {
    list_pending_ = false;
    refreshSnapshots();
  }
}

void RealtimeController::ApplyRunResult(const RealtimeRunResult& result) {
  last_outcome_kind_ = QString::fromLatin1(OutcomeKindKey(result.outcome.kind));
  last_snapshot_name_ =
      QString::fromStdString(result.outcome.snapshot_file_name);
  last_succeeded_ =
      result.ok &&
      result.outcome.kind != backupproject::RealtimeOutcome::Kind::kFailed;

  if (result.listed) ApplySnapshots(result);

  if (!result.ok) {
    last_error_text_ = QString::fromStdString(result.error_message);
    SetPhase(QString::fromLatin1(kPhaseFailed),
             QStringLiteral("实时备份失败：%1").arg(last_error_text_));
    SetStatus(kError, QStringLiteral("实时备份失败"), last_error_text_);
    return;
  }

  const backupproject::RealtimeOutcome& outcome = result.outcome;
  last_error_text_ = QString::fromStdString(outcome.diagnostic);

  QString detail;
  if (!outcome.summary_text.empty()) {
    detail = QString::fromStdString(outcome.summary_text);
  }
  if (outcome.kind != backupproject::RealtimeOutcome::Kind::kNoChanges) {
    if (!detail.isEmpty()) detail += QStringLiteral(" · ");
    detail +=
        ChangeText(outcome.changes.added, outcome.changes.removed,
                   outcome.changes.modified, outcome.changes.metadata_changed);
  }
  if (!outcome.snapshot_file_name.empty()) {
    if (!detail.isEmpty()) detail += QStringLiteral(" · ");
    detail += QStringLiteral("归档 %1").arg(
        QString::fromStdString(outcome.snapshot_file_name));
  }
  if (!outcome.diagnostic.empty()) {
    if (!detail.isEmpty()) detail += QStringLiteral(" · ");
    detail += QString::fromStdString(outcome.diagnostic);
  }

  last_snapshot_text_ = QStringLiteral("%1 · %2").arg(
      FormatLocalTime(static_cast<std::int64_t>(std::time(nullptr))),
      OutcomeKindText(outcome.kind));
  if (!outcome.snapshot_file_name.empty()) {
    last_snapshot_text_ += QStringLiteral(" · %1").arg(
        QString::fromStdString(outcome.snapshot_file_name));
  }

  // "新快照成功、旧版本淘汰失败"不等于"备份失败"：两者必须分开报告。
  const bool retention_uncertain =
      outcome.retention_uncertain || outcome.marker_warning;
  if (outcome.kind == backupproject::RealtimeOutcome::Kind::kFailed) {
    SetPhase(QString::fromLatin1(kPhaseFailed), QStringLiteral("实时备份失败"));
    SetStatus(kError, QStringLiteral("实时备份失败"),
              outcome.summary_text.empty()
                  ? QStringLiteral("这一轮没有产出快照。")
                  : QString::fromStdString(outcome.summary_text));
    return;
  }

  switch (outcome.kind) {
    case backupproject::RealtimeOutcome::Kind::kNoChanges:
      SetPhase(QString::fromLatin1(kPhaseNoChanges),
               QStringLiteral("增量策略下没有变化，未写入任何内容"));
      SetStatus(kIdle, QStringLiteral("源目录没有变化，已跳过"),
                QStringLiteral("实时触发只创建有变化的快照，因此这一轮不生成"
                               "新归档，也不改动任何基线。"));
      return;
    case backupproject::RealtimeOutcome::Kind::kDelta:
      if (retention_uncertain) {
        SetPhase(QString::fromLatin1(kPhaseRetentionWarning),
                 QStringLiteral("增量已写入，但旧版本淘汰未完成"));
        SetStatus(kWarning, QStringLiteral("实时备份完成，但旧版本淘汰未完成"),
                  detail);
        return;
      }
      SetPhase(QString::fromLatin1(kPhaseSnapshotCreated),
               QStringLiteral("已写入实时增量备份"));
      SetStatus(kSuccess, QStringLiteral("已写入实时增量备份"), detail);
      return;
    case backupproject::RealtimeOutcome::Kind::kFullBaseline:
      if (retention_uncertain) {
        SetPhase(QString::fromLatin1(kPhaseRetentionWarning),
                 QStringLiteral("基线已建立，但旧版本淘汰未完成"));
        SetStatus(kWarning, QStringLiteral("实时备份完成，但旧版本淘汰未完成"),
                  detail);
        return;
      }
      SetPhase(QString::fromLatin1(kPhaseSnapshotCreated),
               QStringLiteral("已创建实时增量基线（完整快照）"));
      SetStatus(kSuccess, QStringLiteral("已创建实时增量基线（完整快照）"),
                detail);
      return;
    case backupproject::RealtimeOutcome::Kind::kFullSnapshot:
      if (retention_uncertain) {
        SetPhase(QString::fromLatin1(kPhaseRetentionWarning),
                 QStringLiteral("快照已创建，但旧版本淘汰未完成"));
        SetStatus(kWarning, QStringLiteral("实时备份完成，但旧版本淘汰未完成"),
                  detail);
        return;
      }
      SetPhase(QString::fromLatin1(kPhaseSnapshotCreated),
               QStringLiteral("已创建实时完整快照"));
      SetStatus(kSuccess, QStringLiteral("已创建实时完整快照"), detail);
      return;
    case backupproject::RealtimeOutcome::Kind::kFailed:
      break;
  }
  SetPhase(QString::fromLatin1(kPhaseFailed), QStringLiteral("实时备份失败"));
  SetStatus(kError, QStringLiteral("实时备份失败"), detail);
}

void RealtimeController::OnRunFinished() {
  const RealtimeRunResult result = run_watcher_.result();
  // 先放开闸门：下面的 DrainPending 可能立刻再提交一轮，而 SetBusy(false)
  // 之后 QML 才会看到"空闲"。
  if (operation_gate_ != nullptr) {
    operation_gate_->Release(OperationGate::Kind::kRealtimeEvaluation);
  }
  SetBusy(false);
  ApplyRunResult(result);
  last_generation_ = result.generation;
  last_event_count_ = result.event_count;
  last_overflow_ = result.overflow;
  last_resync_ = result.resync;
  emit operationFinished(last_succeeded_);
  DrainPending();
}

// ---- 保存配置 ----

bool RealtimeController::saveConfigFromText(
    bool enabled, const QString& source_path, const QString& debounce_text,
    const QString& max_wait_text, const QString& retain_text,
    const QString& pack_key, const QString& compression_key,
    const QStringList& include_rules, const QStringList& exclude_rules,
    const QString& strategy_key) {
  std::uint32_t debounce = 0;
  std::uint32_t max_wait = 0;
  std::uint32_t retain = 0;
  std::string error;
  // 界面不自己"解析"：数字只要不是纯十进制整数就在这里被拒绝，而**范围**
  // （100..60000 / 500..300000 且 >= debounce / 1..1000）由共享核心的
  // ValidateRealtimeConfig 裁决——backupctl realtime set 走的是同一套。
  if (!ParseRealtimeNumber(debounce_text, QStringLiteral("Debounce（ms）"),
                           &debounce, &error) ||
      !ParseRealtimeNumber(max_wait_text, QStringLiteral("Max wait（ms）"),
                           &max_wait, &error) ||
      !ParseRealtimeNumber(retain_text, QStringLiteral("保留数量"), &retain,
                           &error)) {
    SetStatus(kError, QStringLiteral("实时配置不合法"),
              QStringLiteral("Debounce / Max wait / 保留数量都必须是十进制整数"
                             "（不接受空格、正负号或其它字符）。") +
                  QStringLiteral(" ") + QString::fromStdString(error));
    return false;
  }
  return saveConfig(enabled, source_path, static_cast<int>(debounce),
                    static_cast<int>(max_wait), static_cast<int>(retain),
                    pack_key, compression_key, include_rules, exclude_rules,
                    strategy_key);
}

bool RealtimeController::saveConfig(bool enabled, const QString& source_path,
                                    int debounce_ms, int max_wait_ms,
                                    int retain_count, const QString& pack_key,
                                    const QString& compression_key,
                                    const QStringList& include_rules,
                                    const QStringList& exclude_rules,
                                    const QString& strategy_key) {
  // ---- 顺序是硬要求 ----
  //
  // 1) 先停 watcher：保存期间不再有新事件进来，也不会出现"监听的是旧源、
  //    写的是新配置"这种半截状态。
  const bool was_watching =
      watcher_.attached() || watch_degraded_ || debounce_timer_.isActive();
  StopWatching();

  // 保存失败时源配置没有被改动，恢复的是原来那一份；恢复时会合成一次
  // resync，所以这段空窗不会漏事件。
  auto restore_watch = [this, was_watching]() {
    if (was_watching && config_loaded_ && config_.enabled && !config_error_) {
      BeginWatching();
    }
  };

  // 2) 同一进程内的单写者：与手动备份 / 计划评估共用同一个闸门。
  OperationGuard guard(operation_gate_, OperationGate::Kind::kRealtimeConfig);
  if (operation_gate_ != nullptr && !guard.acquired()) {
    SetStatus(kWarning, QStringLiteral("另一个操作正在进行"), guard.reason());
    restore_watch();
    return false;
  }
  if (busy_) {
    SetStatus(kWarning, QStringLiteral("实时备份正在运行"),
              QStringLiteral("后台正在写盘，此时保存会与它并发改动同一份配置。"
                             "请等这一轮结束后再保存。"));
    restore_watch();
    return false;
  }

  backupproject::RealtimeConfig next = config_;
  next.version = backupproject::kRealtimeConfigVersion;
  next.enabled = enabled;
  next.trigger = backupproject::BackupTrigger::kRealtime;
  next.source_path = source_path.toStdString();
  next.debounce_ms =
      debounce_ms < 0 ? 0 : static_cast<std::uint32_t>(debounce_ms);
  next.max_wait_ms =
      max_wait_ms < 0 ? 0 : static_cast<std::uint32_t>(max_wait_ms);
  next.retain_count =
      retain_count < 0 ? 0 : static_cast<std::uint32_t>(retain_count);

  // 策略 / 算法 key 全部走共享核心那张表：解析失败不回退到默认值 ——
  // 用户明确选了增量，就必须拿到增量或明确的错误。
  if (!backupproject::ParseBackupStrategyKey(strategy_key.toStdString(),
                                             &next.strategy)) {
    SetStatus(kError, QStringLiteral("未知的备份策略"), strategy_key);
    restore_watch();
    return false;
  }
  if (!backupproject::ParsePackMethodKey(pack_key.toStdString(),
                                         &next.pack_method)) {
    SetStatus(kError, QStringLiteral("未知的打包方式"), pack_key);
    restore_watch();
    return false;
  }
  if (!backupproject::ParseCompressionMethodKey(compression_key.toStdString(),
                                                &next.compression_method)) {
    SetStatus(kError, QStringLiteral("未知的压缩方式"), compression_key);
    restore_watch();
    return false;
  }
  // 界面只提供"不加密"。无人值守的实时备份没有安全的持久密钥来源，
  // 这里不是"暂时隐藏"，而是真的没有任何地方可以存密码。
  next.encryption_method = backupproject::EncryptionMethod::kNone;

  next.include_rules.clear();
  for (const QString& rule : include_rules) {
    next.include_rules.push_back(rule.toStdString());
  }
  next.exclude_rules.clear();
  for (const QString& rule : exclude_rules) {
    next.exclude_rules.push_back(rule.toStdString());
  }

  std::string error;
  if (!backupproject::ValidateRealtimeConfig(next, &error)) {
    SetStatus(kError, QStringLiteral("实时配置不合法"),
              QString::fromStdString(error));
    restore_watch();
    return false;
  }
  if (next.enabled) {
    std::string repository_error;
    if (!RefreshRepository(&repository_error)) {
      SetStatus(kError, QStringLiteral("无法启用实时备份"),
                QString::fromStdString(repository_error));
      restore_watch();
      return false;
    }
    std::string identity;
    if (!backupproject::ValidateRealtimeForEnable(
            next, repository_path_.toStdString(), &error, &identity)) {
      // 与 backupctl realtime enable 调的是同一个函数：源目录、仓库、
      // "能不能启用"这三件事只有一份判断。
      SetStatus(kError, QStringLiteral("无法启用实时备份"),
                QString::fromStdString(error));
      restore_watch();
      return false;
    }
    repository_identity_ = identity;
  }

  if (!store_.Save(next, &error)) {
    SetStatus(kError, QStringLiteral("实时配置保存失败"),
              QString::fromStdString(error));
    restore_watch();
    return false;
  }

  config_ = next;
  config_loaded_ = true;
  config_error_ = false;
  load_error_.clear();
  emit configChanged();

  if (config_.enabled) {
    BeginWatching();
  } else {
    SetPhase(QString::fromLatin1(kPhaseDisabled),
             QStringLiteral("实时备份未启用"));
    SetStatus(kSuccess, QStringLiteral("实时配置已保存"),
              QStringLiteral("实时备份已停用，本程序不再监听源目录。"));
    emit runtimeChanged();
  }
  refreshSnapshots();
  return true;
}

bool RealtimeController::setEnabled(bool enabled) {
  return saveConfig(enabled, sourcePath(), debounceMs(), maxWaitMs(),
                    retainCount(), packKey(), compressionKey(), includeRules(),
                    excludeRules(), strategyKey());
}

QString RealtimeController::validateRule(const QString& action,
                                         const QString& rule) const {
  backupproject::Filter filter;
  const backupproject::FilterAction filter_action =
      action == QStringLiteral("exclude")
          ? backupproject::FilterAction::kExclude
          : backupproject::FilterAction::kInclude;
  std::string error;
  if (filter.AddRule(filter_action, rule.toStdString(), &error)) {
    return QString();
  }
  return QString::fromStdString(error);
}

// ---- 状态 ----

void RealtimeController::SetStatus(const QString& kind, const QString& title,
                                   const QString& message) {
  status_kind_ = kind;
  status_title_ = title;
  status_message_ = message;
  emit statusChanged();
}

void RealtimeController::SetPhase(const QString& key, const QString& text) {
  if (phase_key_ == key && phase_text_ == text) return;
  phase_key_ = key;
  phase_text_ = text;
  emit runtimeChanged();
}

void RealtimeController::SetBusy(bool busy) {
  if (busy_ == busy) return;
  busy_ = busy;
  emit busyChanged();
}

// ---- 只读访问器 ----

QString RealtimeController::triggerKey() const {
  return QString::fromLatin1(backupproject::BackupTriggerKey(config_.trigger));
}

QString RealtimeController::strategyKey() const {
  return QString::fromLatin1(
      backupproject::BackupStrategyKey(config_.strategy));
}

QString RealtimeController::sourcePath() const {
  return QString::fromStdString(config_.source_path);
}

int RealtimeController::debounceMs() const {
  return static_cast<int>(config_.debounce_ms);
}

int RealtimeController::maxWaitMs() const {
  return static_cast<int>(config_.max_wait_ms);
}

int RealtimeController::retainCount() const {
  return static_cast<int>(config_.retain_count);
}

QString RealtimeController::packKey() const {
  return QString::fromLatin1(backupproject::PackMethodKey(config_.pack_method));
}

QString RealtimeController::compressionKey() const {
  return QString::fromLatin1(
      backupproject::CompressionMethodKey(config_.compression_method));
}

QString RealtimeController::encryptionKey() const {
  return QString::fromLatin1(
      backupproject::EncryptionMethodKey(config_.encryption_method));
}

QStringList RealtimeController::includeRules() const {
  QStringList list;
  for (const std::string& rule : config_.include_rules) {
    list.append(QString::fromStdString(rule));
  }
  return list;
}

QStringList RealtimeController::excludeRules() const {
  QStringList list;
  for (const std::string& rule : config_.exclude_rules) {
    list.append(QString::fromStdString(rule));
  }
  return list;
}

QString RealtimeController::supportedModeText() const {
  return QStringLiteral(
      "当前支持：实时触发（文件事件）+ 完整快照 / 增量策略。\n"
      "实时触发只决定什么时候跑，策略与存储仍然由与 backupctl 共用的同一份"
      "核心执行。");
}

QString RealtimeController::encryptionNote() const {
  return QStringLiteral("实时无人值守备份当前不保存密码，因此不启用加密。");
}

bool RealtimeController::watching() const { return watcher_.attached(); }

int RealtimeController::watchCount() const {
  return static_cast<int>(watcher_.watch_count());
}

int RealtimeController::pendingEventCount() const {
  std::uint64_t count = debouncer_ != nullptr ? debouncer_->event_count() : 0;
  if (pending_generation_valid_) count += pending_generation_.event_count;
  return static_cast<int>(std::min<std::uint64_t>(count, 1000000000ull));
}

QString RealtimeController::watchStateText() const {
  if (watch_degraded_) {
    return QStringLiteral("监听已降级：%1").arg(watch_degraded_reason_);
  }
  if (!watcher_.attached()) {
    return config_.enabled ? QStringLiteral("尚未开始监听")
                           : QStringLiteral("未启用，未在监听");
  }
  return QStringLiteral("正在监听 · %1 个目录")
      .arg(static_cast<int>(watcher_.watch_count()));
}

QString RealtimeController::pendingStateText() const {
  const int pending_events = pendingEventCount();
  if (pending_generation_valid_) {
    return QStringLiteral(
               "有 1 代触发在排队（%1 条事件），等当前操作结束后补跑")
        .arg(static_cast<int>(pending_generation_.event_count));
  }
  if (debouncer_ != nullptr && debouncer_->dirty()) {
    return QStringLiteral(
               "等待事件稳定：已合并 %1 条事件（debounce %2 ms / "
               "最长 %3 ms）")
        .arg(static_cast<qulonglong>(debouncer_->event_count()))
        .arg(config_.debounce_ms)
        .arg(config_.max_wait_ms);
  }
  if (pending_events > 0) {
    return QStringLiteral("待处理事件：%1").arg(pending_events);
  }
  return QStringLiteral("没有待处理事件");
}

QString RealtimeController::overflowStateText() const {
  QString text = QStringLiteral("溢出重同步：%1 次")
                     .arg(static_cast<qulonglong>(overflow_resync_count_));
  if (last_resync_) {
    text += QStringLiteral(" · 最近一次触发是重新同步");
  } else if (last_overflow_) {
    text += QStringLiteral(" · 最近一批事件里有溢出");
  } else {
    text += QStringLiteral(" · 没有发生过溢出");
  }
  return text;
}

QString RealtimeController::localPathFromUrl(const QUrl& url) const {
  return url.toLocalFile();
}

QUrl RealtimeController::directoryDialogStartUrl(const QString& path) const {
  const QFileInfo info(path);
  if (!path.isEmpty() && info.exists() && info.isDir()) {
    return QUrl::fromLocalFile(info.absoluteFilePath());
  }
  return QUrl::fromLocalFile(QDir::homePath());
}

bool RealtimeController::waitForIdle(int timeout_ms) {
  QEventLoop loop;
  QTimer poll;
  poll.setInterval(10);
  QObject::connect(&poll, &QTimer::timeout, &loop, [this, &loop]() {
    if (!busy_ && !pending_generation_valid_ && !list_busy_ &&
        !retry_timer_.isActive()) {
      loop.quit();
    }
  });
  QTimer guard;
  guard.setSingleShot(true);
  QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
  poll.start();
  guard.start(timeout_ms);
  if (!busy_ && !pending_generation_valid_ && !list_busy_ &&
      !retry_timer_.isActive()) {
    return true;
  }
  loop.exec();
  return !busy_ && !pending_generation_valid_ && !list_busy_ &&
         !retry_timer_.isActive();
}

}  // namespace backup_modern
