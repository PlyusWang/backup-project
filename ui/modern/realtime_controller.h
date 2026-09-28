// realtime_controller.h
//
// QML 与"实时触发共享核心"之间唯一的桥。
//
// 依赖方向：
//
//   QML ──► RealtimeController ──► RealtimeStore / InotifyWatcher /
//                                  RealtimeDebouncer /
//                                  RunRealtimeBackupOnce /
//                                  ListRealtimeSnapshots
//
// QML 只做四件事：展示、编辑配置、点启停、展示最近快照。
// 它**不**直接调 inotify、**不**拼 repository 路径、**不**自己解析 Filter、
// **不**自己判断 option support、**不**自己执行 retention —— 全部经这里，
// 而这里全部转发给与 backupctl realtime 共用的同一份核心。
//
// ---- 线程模型 ----
//
//   * watcher 的 fd 在主线程用 QSocketNotifier 读；
//   * debouncer 在主线程，只被"事件到达"与"定时器到期"两件事推进；
//   * RunRealtimeBackupOnce 用 QtConcurrent::run 在后台跑；后台线程只返回一个
//     值类型结果（RealtimeRunResult），不碰任何 QObject / QML 状态。
//
// ---- 闸门 ----
//
// 提交后台任务之前必须 Acquire(kRealtimeEvaluation)。抢不到时**不丢 trigger**：
// 把那一代合并进**一个** pending generation，用 ~150 ms 的轻量 retry timer
// 重试；不做 busy-spin，也不排无限队列。
//
// 保存配置的顺序是硬要求：先停 watcher → Acquire(kRealtimeConfig) → 严格校验 +
// 原子保存 → 若 enabled 则重新 attach + 合成 resync。
//
// 本文件只依赖 Qt 与共享核心，不含任何业务判定。

#ifndef BACKUP_PROJECT_UI_MODERN_REALTIME_CONTROLLER_H_
#define BACKUP_PROJECT_UI_MODERN_REALTIME_CONTROLLER_H_

#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "config_manager.h"
#include "operation_gate.h"
#include "realtime_backup_service.h"
#include "realtime_debouncer.h"
#include "realtime_store.h"
#include "realtime_watcher.h"

class QSocketNotifier;

namespace backup_modern {

// 一次后台任务的返回值。
//
// 刻意只用值类型：跨线程只发生一次拷贝，后台线程不读控制器的任何成员，也就
// 不需要加锁，更不可能碰到 QObject / QML 状态。
struct RealtimeRunResult {
  // RunRealtimeBackupOnce 本身是否走完（false 时 error_message 是原因）。
  bool ok = false;
  std::string error_message;
  backupproject::RealtimeOutcome outcome;

  // 这一次顺带取回的实时快照列表（同一个后台线程里做，界面线程不做文件 IO）。
  bool listed = false;
  std::string list_error;
  std::vector<backupproject::RealtimeSnapshotRecord> snapshots;

  // 这一代的事件层面信息，用来如实显示"这一轮是被什么触发的"。
  std::uint64_t generation = 0;
  std::uint64_t event_count = 0;
  bool overflow = false;
  bool resync = false;
};

class RealtimeController : public QObject {
  Q_OBJECT

  // ---- 配置（可编辑）----
  Q_PROPERTY(bool configLoaded READ configLoaded NOTIFY configChanged)
  Q_PROPERTY(QString loadError READ loadError NOTIFY configChanged)
  Q_PROPERTY(QString storePath READ storePath NOTIFY configChanged)
  Q_PROPERTY(bool enabled READ enabled NOTIFY configChanged)
  Q_PROPERTY(QString triggerKey READ triggerKey NOTIFY configChanged)
  Q_PROPERTY(QString strategyKey READ strategyKey NOTIFY configChanged)
  Q_PROPERTY(QString sourcePath READ sourcePath NOTIFY configChanged)
  Q_PROPERTY(int debounceMs READ debounceMs NOTIFY configChanged)
  Q_PROPERTY(int maxWaitMs READ maxWaitMs NOTIFY configChanged)
  Q_PROPERTY(int retainCount READ retainCount NOTIFY configChanged)
  Q_PROPERTY(QString packKey READ packKey NOTIFY configChanged)
  Q_PROPERTY(QString compressionKey READ compressionKey NOTIFY configChanged)
  Q_PROPERTY(QString encryptionKey READ encryptionKey NOTIFY configChanged)
  Q_PROPERTY(QStringList includeRules READ includeRules NOTIFY configChanged)
  Q_PROPERTY(QStringList excludeRules READ excludeRules NOTIFY configChanged)
  // 当前真实支持的模式。QML 只显示这一行，不做"未实现的按钮"。
  Q_PROPERTY(QString supportedModeText READ supportedModeText CONSTANT)
  // 加密说明：只有 none。这句话是页面文案的唯一来源，不在 QML 里再写一遍。
  Q_PROPERTY(QString encryptionNote READ encryptionNote CONSTANT)

  // ---- 运行状态 ----
  Q_PROPERTY(bool libraryBusy READ libraryBusy NOTIFY busyChanged)
  Q_PROPERTY(bool pending READ pending NOTIFY pendingChanged)
  Q_PROPERTY(bool watching READ watching NOTIFY runtimeChanged)
  Q_PROPERTY(bool watchDegraded READ watchDegraded NOTIFY runtimeChanged)
  Q_PROPERTY(int watchCount READ watchCount NOTIFY runtimeChanged)
  Q_PROPERTY(int pendingEventCount READ pendingEventCount NOTIFY runtimeChanged)
  // 状态机当前停在哪一格：watching / debouncing / resync / snapshot_created /
  // no_changes / retention_warning / watch_degraded / watch_recovered /
  // config_error / disabled / failed。
  Q_PROPERTY(QString phaseKey READ phaseKey NOTIFY runtimeChanged)
  Q_PROPERTY(QString phaseText READ phaseText NOTIFY runtimeChanged)
  Q_PROPERTY(QString watchStateText READ watchStateText NOTIFY runtimeChanged)
  Q_PROPERTY(
      QString pendingStateText READ pendingStateText NOTIFY runtimeChanged)
  Q_PROPERTY(
      QString overflowStateText READ overflowStateText NOTIFY runtimeChanged)
  Q_PROPERTY(QString lastEventText READ lastEventText NOTIFY runtimeChanged)
  Q_PROPERTY(
      QString lastSnapshotText READ lastSnapshotText NOTIFY runtimeChanged)
  Q_PROPERTY(QString repositoryPath READ repositoryPath NOTIFY runtimeChanged)
  Q_PROPERTY(
      bool repositoryConfigured READ repositoryConfigured NOTIFY runtimeChanged)
  Q_PROPERTY(QString statusKind READ statusKind NOTIFY statusChanged)
  Q_PROPERTY(QString statusTitle READ statusTitle NOTIFY statusChanged)
  Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusChanged)
  Q_PROPERTY(QVariantList snapshots READ snapshots NOTIFY snapshotsChanged)

 public:
  // realtime_file_path 由 main.cpp 显式给出（正常启动来自 app_paths.h，与
  // backupctl 的默认位置严格同源；测试用 --realtime-file 覆盖）。
  // operation_gate 必须与 BackupController / ScheduleController
  // 用的是**同一个** 对象：它才是"同一时刻只有一个 writer"这条不变式的载体。
  RealtimeController(QString realtime_file_path,
                     const QString& config_file_path,
                     OperationGate* operation_gate, QObject* parent = nullptr);
  ~RealtimeController() override;

  // ---- 只读访问器（配置）----
  bool configLoaded() const { return config_loaded_; }
  QString loadError() const { return load_error_; }
  QString storePath() const { return realtime_file_path_; }
  bool enabled() const { return config_.enabled; }
  QString triggerKey() const;
  QString strategyKey() const;
  QString sourcePath() const;
  int debounceMs() const;
  int maxWaitMs() const;
  int retainCount() const;
  QString packKey() const;
  QString compressionKey() const;
  QString encryptionKey() const;
  QStringList includeRules() const;
  QStringList excludeRules() const;
  QString supportedModeText() const;
  QString encryptionNote() const;

  // ---- 只读访问器（运行状态）----
  bool libraryBusy() const { return busy_; }
  bool pending() const { return pending_generation_valid_; }
  bool watching() const;
  bool watchDegraded() const { return watch_degraded_; }
  int watchCount() const;
  int pendingEventCount() const;
  QString phaseKey() const { return phase_key_; }
  QString phaseText() const { return phase_text_; }
  QString watchStateText() const;
  QString pendingStateText() const;
  QString overflowStateText() const;
  QString lastEventText() const { return last_event_text_; }
  QString lastSnapshotText() const { return last_snapshot_text_; }
  QString repositoryPath() const { return repository_path_; }
  bool repositoryConfigured() const { return !repository_path_.isEmpty(); }
  QString statusKind() const { return status_kind_; }
  QString statusTitle() const { return status_title_; }
  QString statusMessage() const { return status_message_; }
  QVariantList snapshots() const { return snapshot_items_; }

  // 目录对话框的 URL 转换与其它页面保持同一套实现。
  Q_INVOKABLE QString localPathFromUrl(const QUrl& url) const;
  Q_INVOKABLE QUrl directoryDialogStartUrl(const QString& path) const;

  // 启动：读配置、必要时 attach watcher 并合成一次 resync，可以重复调用。
  Q_INVOKABLE void start();
  Q_INVOKABLE void reload();
  // 结束进程时停掉 watcher 与全部定时器。
  Q_INVOKABLE void stop();
  Q_INVOKABLE void clearStatus();
  // 重新列一遍仓库里的实时快照（只读，不取闸门）。
  Q_INVOKABLE void refreshSnapshots();

  // 保存实时配置。校验失败时返回 false 并把原因写进状态条，绝不落盘半份配置。
  //
  // debounce / max_wait / retain 在界面上是**文本框**，所以按文本传进来：
  // QML 的 parseInt 会把 "12abc" 悄悄变成 12，而 backupctl 明确拒绝同一个输入。
  Q_INVOKABLE bool saveConfigFromText(
      bool enabled, const QString& source_path, const QString& debounce_text,
      const QString& max_wait_text, const QString& retain_text,
      const QString& pack_key, const QString& compression_key,
      const QStringList& include_rules, const QStringList& exclude_rules,
      const QString& strategy_key = QStringLiteral("full"));
  // 已经解析好的整数入口：C++ 侧的自检用它。
  Q_INVOKABLE bool saveConfig(
      bool enabled, const QString& source_path, int debounce_ms,
      int max_wait_ms, int retain_count, const QString& pack_key,
      const QString& compression_key, const QStringList& include_rules,
      const QStringList& exclude_rules,
      const QString& strategy_key = QStringLiteral("full"));
  // 只改 enabled。启用前会做完整校验。
  Q_INVOKABLE bool setEnabled(bool enabled);

  // 用真实的 Filter 校验一条规则；返回空串表示合法。
  // 真正的边界仍然是保存时的 ValidateRealtimeConfig —— 页面里没有、也不会有
  // 第二套规则解析。
  Q_INVOKABLE QString validateRule(const QString& action,
                                   const QString& rule) const;

  // ---- 仅供 main.cpp 的自动化测试使用，刻意不是 Q_INVOKABLE ----
  bool waitForIdle(int timeout_ms);
  bool lastSucceeded() const { return last_succeeded_; }
  // 最近一次任务的结论 key：full-snapshot / full-baseline / delta /
  // no-changes / failed / ""（还没跑过）。
  QString lastOutcomeKind() const { return last_outcome_kind_; }
  QString lastSnapshotName() const { return last_snapshot_name_; }
  int snapshotCount() const { return snapshot_items_.size(); }
  QString lastErrorText() const { return last_error_text_; }
  backupproject::RealtimeConfig configForTest() const { return config_; }
  // 直接推进一次 debounce 判定，不依赖真实时钟等待。
  void pumpDebounceForTest() { OnDebounceTimeout(); }

 signals:
  void configChanged();
  void runtimeChanged();
  void statusChanged();
  void busyChanged();
  void pendingChanged();
  void snapshotsChanged();
  void operationFinished(bool succeeded);

 private:
  // ---- 配置 / 仓库 ----
  void LoadFromDisk();
  // 读 config.json 拿到仓库路径。返回 false 时 error 里是原因。
  bool RefreshRepository(std::string* error_message);
  // attach + 装 QSocketNotifier + 合成 resync。失败时进入 degraded 并排重试。
  bool TryStartWatching();
  void StopWatching();
  void InstallNotifier();
  void ClearNotifier();
  // TryStartWatching 的调用方：失败时按需要排一次 1000 ms 的重建重试。
  void BeginWatching();

  // ---- 事件 ----
  void OnWatcherReadable();
  void OnRecoveryTimeout();
  void ArmDebounceTimer();
  void OnDebounceTimeout();
  void OnRetryTimeout();

  // ---- 提交 / 回收 ----
  void Submit(backupproject::RealtimeGeneration generation);
  void MergePending(const backupproject::RealtimeGeneration& generation);
  void DrainPending();
  void OnRunFinished();
  void OnListFinished();
  void ApplyRunResult(const RealtimeRunResult& result);
  void ApplySnapshots(const RealtimeRunResult& result);

  void SetStatus(const QString& kind, const QString& title,
                 const QString& message);
  void SetPhase(const QString& key, const QString& text);
  void SetBusy(bool busy);
  void SetPending(bool pending);
  void EnterDegraded(const QString& reason);

  static RealtimeRunResult RunOnce(
      backupproject::RealtimeConfig config, std::string repository_path,
      std::string repository_identity,
      const backupproject::RealtimeGeneration& generation);
  static RealtimeRunResult ListOnly(std::string repository_path);
  static std::int64_t SteadyNowMs();

  QString realtime_file_path_;
  backupproject::ConfigManager config_manager_;
  backupproject::RealtimeStore store_;
  backupproject::RealtimeConfig config_;
  bool config_loaded_ = false;
  // 落盘配置读不出来 / 结构不合法。与"这一轮失败"不同：它是一个持续状态，
  // 期间不 attach、不触发，也不自动重试。
  bool config_error_ = false;
  QString load_error_;

  QString repository_path_;
  std::string repository_identity_;

  backupproject::InotifyWatcher watcher_;
  std::unique_ptr<backupproject::RealtimeDebouncer> debouncer_;
  QSocketNotifier* notifier_ = nullptr;

  OperationGate* operation_gate_ = nullptr;

  QTimer debounce_timer_;
  QTimer retry_timer_;
  QTimer recovery_timer_;
  QFutureWatcher<RealtimeRunResult> run_watcher_;
  QFutureWatcher<RealtimeRunResult> list_watcher_;

  bool busy_ = false;
  bool list_busy_ = false;
  bool list_pending_ = false;

  // 闸门被占用时保留的**唯一**一个 pending generation。多个到期事件在这里
  // coalesce，不排队。
  bool pending_generation_valid_ = false;
  backupproject::RealtimeGeneration pending_generation_;

  bool watch_degraded_ = false;
  QString watch_degraded_reason_;
  std::uint64_t overflow_resync_count_ = 0;

  std::uint64_t last_generation_ = 0;
  std::uint64_t last_event_count_ = 0;
  bool last_overflow_ = false;
  bool last_resync_ = false;
  bool last_succeeded_ = false;
  QString last_outcome_kind_;
  QString last_snapshot_name_;
  QString last_error_text_;
  QString last_event_text_;
  QString last_snapshot_text_;

  QVariantList snapshot_items_;

  QString status_kind_ = QStringLiteral("idle");
  QString status_title_ = QStringLiteral("等待实时备份");
  QString status_message_;
  QString phase_key_ = QStringLiteral("disabled");
  QString phase_text_ = QStringLiteral("实时备份未启用");
};

}  // namespace backup_modern

#endif  // BACKUP_PROJECT_UI_MODERN_REALTIME_CONTROLLER_H_
