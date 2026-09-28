// schedule_controller.h
//
// QML 与"定时备份共享核心"之间唯一的桥。
//
// 依赖方向：
//
//   QML ──► ScheduleController ──► ScheduledBackupService / ScheduleStore
//                                        │
//                                        └──► BackupEngine / BackupCatalog
//
// QML 只做四件事：展示、编辑配置、点启停/立即运行、展示 history。
// 它**不算** next run、**不**扫描文件树、**不**做 retention、**不**删归档、
// **不**对比 manifest、**不**判断 due、也**不**自己维护计划快照列表 ——
// 这些全部在共享核心里，CLI 用的是同一份。
//
// 时间只由一个 1 秒粒度的 QTimer 提供"醒来一次"这个动作；"到没到点"仍然问
// 核心的 IsScheduleDue。这样 GUI 与 backupctl schedule watch 的判定不可能分叉。
//
// 与手动 backup / restore 的并发边界：本控制器不自己写盘，所有评估都提交给
// 后台线程；提交之前先看 BackupController::busy()。忙的时候只记一个 pending
// 标记（多次到期 coalesce 成一次），等手动操作结束后补跑一次，绝不并发写盘、
// 也不 busy-spin。

#ifndef BACKUP_PROJECT_UI_MODERN_SCHEDULE_CONTROLLER_H_
#define BACKUP_PROJECT_UI_MODERN_SCHEDULE_CONTROLLER_H_

#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantList>

#include "backup_controller.h"
#include "config_manager.h"
#include "operation_gate.h"
#include "schedule_store.h"
#include "scheduled_backup_service.h"
#include "scheduler_lock.h"

namespace backup_modern {

// 后台评估的结果。全部是值类型：跨线程只发生一次拷贝，后台线程不读控制器的
// 任何成员，也就不需要加锁。
struct ScheduleOutcome {
  bool succeeded = false;
  bool ran = false;
  bool skipped_no_changes = false;
  bool created = false;
  bool retention_warning = false;
  bool first_snapshot = false;
  // 以前记过 baseline，但它已经不可信了（快照被删 / 换了仓库 / 换了源）。
  // 与 first_snapshot 一样会产出一份完整快照，只是原因不同，界面要说清楚。
  bool baseline_reset = false;
  bool due = true;
  // 落盘配置不合法：这一轮什么都没写，而且调度器应当**挂起**，不再周期性
  // 重试。与普通的 failed 分开：failed 下一轮还会照常再试，挂起不会。
  bool config_invalid = false;

  QString status_key;
  QString status_text;
  QString error_message;
  QString archive_file_name;
  QString diagnostic;

  qulonglong added = 0;
  qulonglong removed = 0;
  qulonglong modified = 0;
  qulonglong metadata_changed = 0;
  qlonglong next_run_sec = 0;
};

class ScheduleController : public QObject,
                           public BackupController::ArchiveDeletedObserver {
  Q_OBJECT

  // ---- 配置（可编辑）----
  Q_PROPERTY(bool configLoaded READ configLoaded NOTIFY configChanged)
  Q_PROPERTY(QString loadError READ loadError NOTIFY configChanged)
  Q_PROPERTY(QString storePath READ storePath NOTIFY configChanged)
  Q_PROPERTY(bool enabled READ enabled NOTIFY configChanged)
  Q_PROPERTY(QString triggerKey READ triggerKey NOTIFY configChanged)
  Q_PROPERTY(QString strategyKey READ strategyKey NOTIFY configChanged)
  Q_PROPERTY(QString sourcePath READ sourcePath NOTIFY configChanged)
  Q_PROPERTY(int intervalMinutes READ intervalMinutes NOTIFY configChanged)
  Q_PROPERTY(int retainCount READ retainCount NOTIFY configChanged)
  // PR #18：strategyKey 这一条 PR #17 就已经留好了，本轮只是让它真的能被
  // 界面选择与保存（下面两个 saveConfig* 各多一个 strategy_key）。
  Q_PROPERTY(QString packKey READ packKey NOTIFY configChanged)
  Q_PROPERTY(QString compressionKey READ compressionKey NOTIFY configChanged)
  Q_PROPERTY(QString encryptionKey READ encryptionKey NOTIFY configChanged)
  Q_PROPERTY(QStringList includeRules READ includeRules NOTIFY configChanged)
  Q_PROPERTY(QStringList excludeRules READ excludeRules NOTIFY configChanged)
  // 当前真实支持的模式。QML 只显示这一行，不做"未实现的按钮"。
  Q_PROPERTY(QString supportedModeText READ supportedModeText CONSTANT)

  // ---- 运行状态 ----
  Q_PROPERTY(bool libraryBusy READ libraryBusy NOTIFY busyChanged)
  Q_PROPERTY(bool pending READ pending NOTIFY pendingChanged)
  // 定时备份是否因为"落盘配置不合法"而挂起。挂起期间：不评估、不写盘、
  // 不重试；用户在页面上重新保存一份合法的计划即可自动恢复。
  Q_PROPERTY(bool suspended READ suspended NOTIFY suspendedChanged)
  Q_PROPERTY(bool holdsRunnerLock READ holdsRunnerLock NOTIFY runnerChanged)
  Q_PROPERTY(QString runnerMessage READ runnerMessage NOTIFY runnerChanged)
  Q_PROPERTY(QString statusKind READ statusKind NOTIFY statusChanged)
  Q_PROPERTY(QString statusTitle READ statusTitle NOTIFY statusChanged)
  Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusChanged)

  Q_PROPERTY(QString lastRunText READ lastRunText NOTIFY stateChanged)
  Q_PROPERTY(QString nextRunText READ nextRunText NOTIFY stateChanged)
  Q_PROPERTY(QString lastResultText READ lastResultText NOTIFY stateChanged)
  Q_PROPERTY(
      QVariantList managedSnapshots READ managedSnapshots NOTIFY stateChanged)
  Q_PROPERTY(QVariantList history READ history NOTIFY stateChanged)
  Q_PROPERTY(QString repositoryPath READ repositoryPath NOTIFY stateChanged)
  Q_PROPERTY(
      bool repositoryConfigured READ repositoryConfigured NOTIFY stateChanged)

 public:
  // schedule_file_path 由 main.cpp 显式给出（正常启动来自 app_paths.h，
  // 与 backupctl 的默认位置严格同源；测试用 --schedule-file 覆盖）。
  // backup_controller 用来查询/等待手动操作的 busy 边界，可以为空。
  // operation_gate 与 BackupController 用的是同一个对象：它保证"手动操作"
  // 与"后台评估"不会同时改动持久状态。
  ScheduleController(QString schedule_file_path,
                     const QString& config_file_path,
                     BackupController* backup_controller,
                     OperationGate* operation_gate, QObject* parent = nullptr);
  ~ScheduleController() override;

  // BackupController::ArchiveDeletedObserver
  //
  // 由 BackupController::deleteBackup 在**持有 kManualDelete 闸门期间**同步
  // 调用，所以这里绝不能再申请闸门、也不能启动后台任务：整个"删除 + 计划状态
  // 同步"是一个操作，中间不允许别的 writer 插进来。
  void OnArchiveDeleted(const QString& file_name) override;

  bool configLoaded() const { return config_loaded_; }
  QString loadError() const { return load_error_; }
  QString storePath() const { return schedule_file_path_; }
  bool enabled() const { return document_.config.enabled; }
  QString triggerKey() const;
  QString strategyKey() const;
  QString sourcePath() const;
  int intervalMinutes() const;
  int retainCount() const;
  QString packKey() const;
  QString compressionKey() const;
  QString encryptionKey() const;
  QStringList includeRules() const;
  QStringList excludeRules() const;
  QString supportedModeText() const;

  bool libraryBusy() const { return busy_; }
  bool pending() const { return pending_; }
  bool suspended() const { return config_invalid_; }
  bool holdsRunnerLock() const { return lock_.held(); }
  QString runnerMessage() const { return runner_message_; }
  QString statusKind() const { return status_kind_; }
  QString statusTitle() const { return status_title_; }
  QString statusMessage() const { return status_message_; }

  QString lastRunText() const;
  QString nextRunText() const;
  QString lastResultText() const;
  QVariantList managedSnapshots() const;
  QVariantList history() const;
  QString repositoryPath() const { return repository_path_; }
  bool repositoryConfigured() const { return !repository_path_.isEmpty(); }

  // 目录对话框的 URL 转换与 BackupController 保持同一套实现。
  Q_INVOKABLE QString localPathFromUrl(const QUrl& url) const;
  Q_INVOKABLE QUrl directoryDialogStartUrl(const QString& path) const;

  // 启动：读 store、必要时抢 runner 锁并启动 tick。可以重复调用（幂等）。
  Q_INVOKABLE void start();
  Q_INVOKABLE void reload();

  // 保存计划配置。校验失败时返回 false 并把原因写进状态条，绝不落盘半份配置。
  //
  // 启用前的校验调用的是共享核心的 ValidateScheduleForEnable（CLI 用的是同一
  // 个函数），并带上当前仓库；首次启用还会把 next_run 推成一个完整周期之后。
  // QML 真正调用的入口：周期与保留数量在界面上是**文本框**，所以按文本传进来，
  // 由共享核心的 ParseBoundedScheduleNumber 裁决。
  //
  // 为什么不在这里用 QML 的 parseInt：它会把 "12abc" 悄悄变成 12，而 backupctl
  // 明确拒绝同一个输入。同一个输入两个前端给出不同结论，正是 parity 要收掉的
  // 东西；顺带还避免了 "99999999999999" 转 int 的溢出。
  // strategy_key 默认 "full"：不传的调用方（旧 QML、旧自测）行为一字不变，
  // 传了的调用方拿到的是与 backupctl --strategy 完全相同的一个 key。
  Q_INVOKABLE bool saveConfigFromText(
      bool enabled, const QString& source_path, const QString& interval_text,
      const QString& retain_text, const QString& pack_key,
      const QString& compression_key, const QStringList& include_rules,
      const QStringList& exclude_rules,
      const QString& strategy_key = QStringLiteral("full"));

  // 已经解析好的整数入口：C++ 侧的自动化测试与 saveConfigFromText 用它。
  Q_INVOKABLE bool saveConfig(
      bool enabled, const QString& source_path, int interval_minutes,
      int retain_count, const QString& pack_key, const QString& compression_key,
      const QStringList& include_rules, const QStringList& exclude_rules,
      const QString& strategy_key = QStringLiteral("full"));
  // 只改 enabled。启用前会做完整校验。
  Q_INVOKABLE bool setEnabled(bool enabled);
  // 备份管理页的 JOIN：file_name -> 来源 / 计划变化摘要。
  //
  // 依赖方向仍然是 ScheduleService -> BackupCatalog：这里只读 ScheduleStore
  // 自己的 managed 名单，BackupCatalog 完全不知道 scheduler 存在。
  // 来源判断**不解析文件名** —— 文件名永远不是 ownership 的真相来源。
  Q_INVOKABLE QString originForFile(const QString& file_name) const;
  Q_INVOKABLE QString changesForFile(const QString& file_name) const;

  // 用真实的 Filter 校验一条规则；返回空串表示合法。
  // QML 用它做即时反馈，真正的边界仍然是保存时的 ValidateScheduleConfig——
  // 页面里没有、也不会有第二套规则解析。
  Q_INVOKABLE QString validateRule(const QString& action,
                                   const QString& rule) const;
  // "立即检查并运行"：跳过"还没到点"，但仍然做真实的变化检测。
  Q_INVOKABLE bool runNow();
  // 结束进程时释放锁。
  Q_INVOKABLE void stop();
  Q_INVOKABLE void clearStatus();

  // ---- 仅供 main.cpp 的自动化测试使用，刻意不是 Q_INVOKABLE ----
  bool waitForIdle(int timeout_ms);
  bool lastSucceeded() const { return last_succeeded_; }
  // 控制器当前内存里的配置原样交出去。parity 测试要用它调用共享核心的
  // ValidateScheduleForEnable，证明 GUI 拒绝启用时给的理由就是核心那句话，
  // 而不是界面自己另写的一套判断。
  backupproject::ScheduleConfig configForTest() const {
    return document_.config;
  }
  // tick 是否还活着。挂起之后它必须是 false：没有 timer 就不可能再有
  // "每秒重新校验一次"。
  bool tickActiveForTest() const { return tick_.isActive(); }
  // 直接跑一次 Tick，"假装 timer 又响了一次"。用来证明即使 tick 真的再响，
  // 挂起状态下也不会有任何新提交——不需要靠 sleep 堆时间去等 1 Hz。
  void pumpTickForTest() { Tick(); }

 signals:
  void configChanged();
  void stateChanged();
  void statusChanged();
  void busyChanged();
  void pendingChanged();
  void suspendedChanged();
  void runnerChanged();
  void operationFinished(bool succeeded);

 private:
  void LoadFromDisk();
  void ApplyRunnerLock();
  void Tick();
  void Submit(bool force);
  void DrainPending();
  // runner_message_ 的写入口：文本没变就不发信号。tick 会周期性地重试抢锁，
  // 少了这一层，"抢不到"会变成每秒一次的 runnerChanged 抖动。
  void SetRunnerMessage(const QString& text);
  void OnEvaluationFinished();
  void OnBackupBusyChanged();
  // 仓库在设置页被改掉之后，本控制器必须立刻跟上：下一次评估用的是新仓库。
  void OnRepositoryPathChanged();
  void SetStatus(const QString& kind, const QString& title,
                 const QString& message);
  // 挂起开关的唯一入口。挂起时停掉 tick：没有 timer 就没有周期性重校验。
  void SetConfigInvalid(bool invalid);
  void SetBusy(bool busy);

  static ScheduleOutcome RunEvaluation(const QString& store_path,
                                       const QString& repository, bool force);

  QString schedule_file_path_;
  // 手动 backup / restore 的 busy 边界来自这里。只是订阅，不复制它的逻辑。
  BackupController* backup_controller_ = nullptr;
  backupproject::ConfigManager config_manager_;
  backupproject::ScheduleStore store_;
  backupproject::ScheduleDocument document_;
  bool config_loaded_ = false;
  QString load_error_;

  QString repository_path_;

  // 本进程是否是这一份计划任务的 active runner。抢不到锁时页面要如实说明。
  backupproject::SchedulerLock lock_;
  QString runner_message_;
  // 与 BackupController 共用的闸门。可以为空（只有自检路径会这样用）。
  OperationGate* operation_gate_ = nullptr;

  QTimer tick_;
  QFutureWatcher<ScheduleOutcome> watcher_;

  // 落盘配置不合法 -> 挂起。见 SetConfigInvalid。
  bool config_invalid_ = false;
  bool busy_ = false;
  bool pending_ = false;
  // pending 那次是"立即运行"还是普通到期检查：两者都要在手动操作结束后补跑，
  // 但只有前者要跳过 due 判定。
  bool pending_force_ = false;
  bool last_succeeded_ = false;

  QString status_kind_ = QStringLiteral("idle");
  QString status_title_ = QStringLiteral("等待计划任务");
  QString status_message_;
};

}  // namespace backup_modern

#endif  // BACKUP_PROJECT_UI_MODERN_SCHEDULE_CONTROLLER_H_
