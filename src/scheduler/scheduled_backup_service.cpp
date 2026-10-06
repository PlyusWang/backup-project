// scheduled_backup_service.cpp

#include "scheduled_backup_service.h"

#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "backup_catalog.h"
#include "backup_engine.h"
#include "incremental_backup.h"
#include "source_manifest.h"

namespace backupproject {
namespace {

void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) *error_message = text;
}

std::string BaseNameOf(const std::string& path) {
  const std::size_t slash = path.rfind('/');
  if (slash == std::string::npos) return path;
  return path.substr(slash + 1);
}

// 饱和加法：now 来自系统时钟，interval 来自配置，两者相加不允许溢出。
std::int64_t SaturatingAdd(std::int64_t left, std::int64_t right) {
  if (right > 0 && left > INT64_MAX - right) return INT64_MAX;
  if (right < 0 && left < INT64_MIN - right) return INT64_MIN;
  return left + right;
}

void AppendHistoryEntry(ScheduleState* state,
                        const ScheduleHistoryEntry& entry) {
  state->history.push_back(entry);
  // history 必须 bounded：它是一段日志，不是当前文件列表，没有理由无限增长。
  while (state->history.size() > kMaxHistoryEntries) {
    state->history.erase(state->history.begin());
  }
}

// 用一份**已经列好的**仓库记录来裁剪 managed 名单。
//
// 单独抽出来是因为 EvaluateInternal 已经为了"仓库到底能不能读"先 List 过一次，
// 再调一次 ReconcileManagedSnapshots 就是第二次目录扫描 + 第二次逐个归档
// InspectHeader —— 那是纯浪费，而且两次结果之间还有竞态。
void ReconcileAgainstRecords(const std::vector<BackupRecord>& records,
                             ScheduleDocument* document) {
  if (document == nullptr) return;
  if (document->state.managed_snapshots.empty()) return;

  std::unordered_set<std::string> present;
  present.reserve(records.size());
  for (const BackupRecord& record : records) present.insert(record.file_name);

  std::vector<ScheduledSnapshotRecord> kept;
  kept.reserve(document->state.managed_snapshots.size());
  for (const ScheduledSnapshotRecord& record :
       document->state.managed_snapshots) {
    if (present.find(record.file_name) != present.end()) {
      kept.push_back(record);
    }
  }
  document->state.managed_snapshots = std::move(kept);
}

// baseline 记录的那份快照还在不在 managed 名单里。
bool BaselineIsManaged(const ScheduleDocument& document) {
  const std::string& file_name = document.state.baseline.snapshot_file_name;
  if (file_name.empty()) return false;
  for (const ScheduledSnapshotRecord& record :
       document.state.managed_snapshots) {
    if (record.file_name == file_name) return true;
  }
  return false;
}

// 注意：原来这里有一个 OldestManagedIndex()，retention 直接用它挑"最旧的一份"。
// PR #18 之后"删哪一份"不再是一个局部决定——必须先算出依赖安全的删除集合
// （见 PlanDependencyAwareRetention），所以那条"找最旧"的逻辑搬进了计划函数，
// 排序规则（时间相同按 file_name）一字未变。这里刻意不再留一个没人用的副本：
// 两处排序规则共存，早晚会有一处先改。

}  // namespace

const char* ScheduleEvaluationStatusKey(ScheduleEvaluationStatus status) {
  switch (status) {
    case ScheduleEvaluationStatus::kDisabled:
      return "disabled";
    case ScheduleEvaluationStatus::kNotDue:
      return "not_due";
    case ScheduleEvaluationStatus::kSkippedNoChanges:
      return "skipped_no_changes";
    case ScheduleEvaluationStatus::kCreatedSnapshot:
      return "created_snapshot";
    case ScheduleEvaluationStatus::kCreatedWithRetentionWarning:
      return "created_with_retention_warning";
    case ScheduleEvaluationStatus::kFailed:
      return "failed";
    case ScheduleEvaluationStatus::kConfigInvalid:
      return "config_invalid";
  }
  return "failed";
}

const char* ScheduleEvaluationStatusText(ScheduleEvaluationStatus status) {
  switch (status) {
    case ScheduleEvaluationStatus::kDisabled:
      return "Scheduled backup is disabled";
    case ScheduleEvaluationStatus::kNotDue:
      return "Not due yet";
    case ScheduleEvaluationStatus::kSkippedNoChanges:
      return "Skipped: the source has not changed since the last snapshot";
    case ScheduleEvaluationStatus::kCreatedSnapshot:
      return "Created a new full snapshot";
    case ScheduleEvaluationStatus::kCreatedWithRetentionWarning:
      return "Snapshot created, but an old scheduled snapshot could not be "
             "removed";
    case ScheduleEvaluationStatus::kFailed:
      return "Scheduled backup failed";
    case ScheduleEvaluationStatus::kConfigInvalid:
      return "Invalid schedule configuration: the schedule is suspended";
  }
  return "Scheduled backup failed";
}

std::int64_t ScheduleNextRunTime(std::int64_t now_sec,
                                 std::uint32_t interval_minutes) {
  // interval_minutes 已经被范围检查夹在 [1, 525600]，乘 60 不可能溢出。
  const std::int64_t interval_seconds =
      static_cast<std::int64_t>(interval_minutes) * 60;
  return SaturatingAdd(now_sec, interval_seconds);
}

bool IsScheduleDue(std::int64_t now_sec, std::int64_t next_run_time_sec,
                   std::uint32_t interval_minutes) {
  if (next_run_time_sec <= 0) return true;
  if (now_sec >= next_run_time_sec) return true;
  // 无符号相减：next_run > now 时得到的就是真实差值，且不会因极端值有符号
  // 溢出。差值大到离谱就说明这个 next_run 不可信（时钟被往回调过），
  // 按到点处理。
  const std::uint64_t delta = static_cast<std::uint64_t>(next_run_time_sec) -
                              static_cast<std::uint64_t>(now_sec);
  const std::uint64_t interval_seconds =
      static_cast<std::uint64_t>(interval_minutes) * 60u;
  return delta > interval_seconds;
}

ScheduleEvaluationStatus StatusForRetention(bool retention_ok) {
  return retention_ok ? ScheduleEvaluationStatus::kCreatedSnapshot
                      : ScheduleEvaluationStatus::kCreatedWithRetentionWarning;
}

const char* ScheduleBaselineStatusKey(ScheduleBaselineStatus status) {
  switch (status) {
    case ScheduleBaselineStatus::kMissing:
      return "missing";
    case ScheduleBaselineStatus::kRepositoryChanged:
      return "repository_changed";
    case ScheduleBaselineStatus::kSourceChanged:
      return "source_changed";
    case ScheduleBaselineStatus::kNotManaged:
      return "not_managed";
    case ScheduleBaselineStatus::kSnapshotGone:
      return "snapshot_gone";
    case ScheduleBaselineStatus::kValid:
      return "valid";
  }
  return "missing";
}

std::string ScheduleBaselineStatusText(ScheduleBaselineStatus status) {
  switch (status) {
    case ScheduleBaselineStatus::kMissing:
      return "No baseline snapshot has been recorded yet";
    case ScheduleBaselineStatus::kRepositoryChanged:
      return "The recorded baseline belongs to a different repository";
    case ScheduleBaselineStatus::kSourceChanged:
      return "The recorded baseline belongs to a different source directory";
    case ScheduleBaselineStatus::kNotManaged:
      return "The baseline snapshot is no longer a managed scheduled snapshot";
    case ScheduleBaselineStatus::kSnapshotGone:
      return "The baseline snapshot can no longer be resolved in the "
             "repository";
    case ScheduleBaselineStatus::kValid:
      return "The baseline snapshot is present in this repository";
  }
  return "No baseline snapshot has been recorded yet";
}

ScheduleBaselineStatus EvaluateScheduleBaseline(
    const ScheduleDocument& document, const std::string& repository_path,
    std::string* error_message) {
  if (error_message != nullptr) error_message->clear();

  const ScheduleBaseline& baseline = document.state.baseline;
  if (baseline.snapshot_file_name.empty()) {
    SetError(error_message,
             ScheduleBaselineStatusText(ScheduleBaselineStatus::kMissing));
    return ScheduleBaselineStatus::kMissing;
  }

  // 三个纯字符串比较先做完，任何文件系统动作都排在后面。
  const std::string identity = RepositoryIdentity(repository_path);
  if (baseline.repository_identity != identity) {
    SetError(error_message, ScheduleBaselineStatusText(
                                ScheduleBaselineStatus::kRepositoryChanged) +
                                ": recorded '" + baseline.repository_identity +
                                "', current '" + identity + "'");
    return ScheduleBaselineStatus::kRepositoryChanged;
  }
  if (baseline.source_path != document.config.source_path) {
    SetError(
        error_message,
        ScheduleBaselineStatusText(ScheduleBaselineStatus::kSourceChanged) +
            ": recorded '" + baseline.source_path + "', current '" +
            document.config.source_path + "'");
    return ScheduleBaselineStatus::kSourceChanged;
  }
  if (!BaselineIsManaged(document)) {
    SetError(error_message,
             ScheduleBaselineStatusText(ScheduleBaselineStatus::kNotManaged) +
                 ": '" + baseline.snapshot_file_name + "'");
    return ScheduleBaselineStatus::kNotManaged;
  }

  // 名字必须在**当前**仓库里能被安全解析成真实文件。这一步同时覆盖
  // "文件被外部删掉"和"名字被改成越界的东西"两种情况。
  BackupCatalog catalog;
  std::string archive_path;
  std::string resolve_error;
  if (!catalog.Resolve(repository_path, baseline.snapshot_file_name,
                       &archive_path, &resolve_error)) {
    SetError(error_message,
             ScheduleBaselineStatusText(ScheduleBaselineStatus::kSnapshotGone) +
                 ": '" + baseline.snapshot_file_name + "': " + resolve_error);
    return ScheduleBaselineStatus::kSnapshotGone;
  }
  return ScheduleBaselineStatus::kValid;
}

void ApplyScheduleEnableTransition(ScheduleDocument* document, bool was_enabled,
                                   std::int64_t now_sec) {
  if (document == nullptr) return;
  if (!document->config.enabled) return;  // 停用不动时间表
  if (was_enabled) return;                // 已经启用：不重置
  document->state.next_run_time_sec =
      ScheduleNextRunTime(now_sec, document->config.interval_minutes);
}

ScheduledBackupService::ScheduledBackupService(std::string repository_path,
                                               ScheduleStore* store)
    : repository_path_(std::move(repository_path)), store_(store) {}

void ScheduledBackupService::SetRepositoryPath(std::string repository_path) {
  repository_path_ = std::move(repository_path);
}

void ScheduledBackupService::ReconcileManagedSnapshots(
    const std::string& repository_path, ScheduleDocument* document) {
  if (document == nullptr) return;
  if (document->state.managed_snapshots.empty()) return;

  BackupCatalog catalog;
  std::vector<BackupRecord> records;
  std::string error;
  // 仓库不可用（没挂载、路径被删、不是目录）时**什么都不做**。
  // 把"读不到"当成"不存在"会把 ownership 名单清空，那些旧快照就再也不会
  // 被 retention 回收了。
  if (!catalog.List(repository_path, &records, &error)) return;
  ReconcileAgainstRecords(records, document);
}

bool ScheduledBackupService::RunRetention(ScheduleDocument* document,
                                          std::uint64_t* deleted,
                                          std::uint64_t* failed,
                                          std::uint64_t* dependency_retained,
                                          std::uint64_t* unreadable,
                                          std::string* error_message) const {
  if (deleted != nullptr) *deleted = 0;
  if (failed != nullptr) *failed = 0;
  if (dependency_retained != nullptr) *dependency_retained = 0;
  if (unreadable != nullptr) *unreadable = 0;
  if (error_message != nullptr) error_message->clear();
  if (document == nullptr) {
    SetError(error_message, "Schedule document must not be null");
    return false;
  }

  BackupCatalog catalog;
  const std::size_t retain = document->config.retain_count;

  // PR #18：删除集合必须先过依赖检查。
  //
  // "删最旧的"对 Full 是安全的，对依赖链不是：删掉某个 delta 的祖先会让它
  // 以及它所有后代都无法恢复，而列表上看起来只是"少了一份旧快照"。
  // 计划函数只回答"哪些能删"，具体删除仍然只走 catalog.Delete。
  std::vector<std::string> managed_oldest_first;
  {
    std::vector<ScheduledSnapshotRecord> ordered =
        document->state.managed_snapshots;
    std::sort(ordered.begin(), ordered.end(),
              [](const ScheduledSnapshotRecord& left,
                 const ScheduledSnapshotRecord& right) {
                if (left.created_time_sec != right.created_time_sec) {
                  return left.created_time_sec < right.created_time_sec;
                }
                return left.file_name < right.file_name;
              });
    for (const ScheduledSnapshotRecord& item : ordered) {
      managed_oldest_first.push_back(item.file_name);
    }
  }
  RetentionPlan plan;
  std::string plan_error;
  if (!PlanDependencyAwareRetention(repository_path_, managed_oldest_first,
                                    retain, &plan, &plan_error)) {
    SetError(error_message,
             "Failed to plan a dependency-safe retention pass: " + plan_error);
    return false;
  }
  if (plan.dependency_uncertain) {
    // fail closed：依赖不确定 -> 这一轮什么都不删。
    //
    // 报成"带警告的成功"而不是硬失败：新快照已经建好、状态也自洽，只是没有
    // 回收旧快照。但必须说出来——否则"为什么仓库一直在长"没有答案。
    if (unreadable != nullptr) {
      *unreadable = static_cast<std::uint64_t>(plan.unreadable.size());
    }
    SetError(
        error_message,
        "Retention removed nothing because a dependency chain could not be "
        "verified: " +
            plan.uncertainty_reason);
    return false;
  }

  // 计划里的名字必须是"本计划管理的快照"，否则不删（别人的东西不动）。
  std::vector<std::string> to_remove;
  for (const std::string& planned : plan.remove) {
    for (const ScheduledSnapshotRecord& item :
         document->state.managed_snapshots) {
      if (item.file_name == planned) {
        to_remove.push_back(planned);
        break;
      }
    }
  }

  if (!to_remove.empty()) {
    std::vector<std::string> removed;
    std::vector<std::string> diagnostics;
    std::string delete_error;
    // 删除永远走 BackupCatalog：它是唯一实现"file_name 必须是仓库直接子项、
    // 必须是普通文件、不是软链接"这条路径安全边界的地方，也是唯一实现
    // "还有后代活着就不许删"的地方。
    // 绝不写成 std::filesystem::remove(repository + "/" + file_name)。
    //
    // 整批一起交进去是刻意的：retention 的删除集合是**整条链一起**，
    // 而"删祖先"单独看必须被拒绝。集合级的规则正好同时表达这两件事。
    const bool all_removed = catalog.DeleteSnapshots(
        repository_path_, to_remove, &removed, &diagnostics, &delete_error);

    // 只有**真的删掉了**的记录才从 managed 列表里去掉：单个 unlink 失败时
    // 剩下的记录留在名单里，下一轮再试。
    for (const std::string& file_name : removed) {
      for (std::size_t index = 0;
           index < document->state.managed_snapshots.size(); ++index) {
        if (document->state.managed_snapshots[index].file_name != file_name) {
          continue;
        }
        document->state.managed_snapshots.erase(
            document->state.managed_snapshots.begin() +
            static_cast<std::ptrdiff_t>(index));
        break;
      }
    }
    if (deleted != nullptr) {
      *deleted += static_cast<std::uint64_t>(removed.size());
    }
    if (failed != nullptr) {
      *failed += static_cast<std::uint64_t>(diagnostics.size());
    }
    if (!all_removed) {
      // DeleteSnapshots 在第一个 unlink 失败处就返回，后面的名字**根本没被尝试**。
      // 两种情形必须分开记，否则计数会撒谎：
      //   * removed 为空（例如依赖排序阶段就拒绝了）：整批都没尝试，
      //     按整批计；
      //   * removed 非空：恰好一个 unlink 真正失败（该函数遇到
      //     失败立即返回），只计这一个。
      if (failed != nullptr) {
        *failed += removed.empty()
                       ? static_cast<std::uint64_t>(to_remove.size())
                       : 1u;
      }
      SetError(
          error_message,
          "Failed to remove the oldest scheduled snapshots: " + delete_error);
      return false;
    }
  }

  // 副文件生命周期：catalog 会带走每一份被删快照自己的副文件，但仓库里
  // 历史遗留的孤儿副文件（.bak 早就不在了）只能在这里显式清一次。
  // 清理失败如实计数，不静默。
  {
    std::vector<std::string> removed_sidecars;
    std::vector<std::string> sidecar_diagnostics;
    std::string cleanup_error;
    if (!CleanOrphanSidecars(repository_path_, &removed_sidecars,
                             &sidecar_diagnostics, &cleanup_error)) {
      if (failed != nullptr) *failed += 1;
      SetError(error_message,
               "Failed to clean up orphan snapshot sidecars: " + cleanup_error);
      return false;
    }
    if (failed != nullptr) {
      *failed += static_cast<std::uint64_t>(sidecar_diagnostics.size());
    }
  }
  // 被依赖而保留下来的祖先、以及读不出依赖因此不敢删的快照，
  // 都如实计数：否则"为什么还留着这么旧的快照"在日志和界面上都说不清。
  if (dependency_retained != nullptr) {
    *dependency_retained = plan.keep_ancestors.size();
  }
  if (unreadable != nullptr) {
    *unreadable = plan.unreadable.size();
  }

  // 不变式：retention 结束后，baseline 记录必须仍然指向一份存在的快照。
  //
  // 正常路径下永远碰不到——淘汰的是最旧的，baseline 是刚创建的那份最新的。
  // 但 retain_count 被调小、或者 state 曾经被手工改过时是有可能的。真发生了
  // 就在这里把记录清掉：下一轮 Evaluate 会因为 kMissing 重建一份完整基线
  // 快照，而不是拿着一个指向空气的 baseline 继续判 skip。
  if (!document->state.baseline.snapshot_file_name.empty() &&
      !BaselineIsManaged(*document)) {
    document->state.baseline = ScheduleBaseline{};
  }
  return true;
}

bool ScheduledBackupService::Evaluate(std::int64_t now_sec,
                                      ScheduleEvaluationResult* result,
                                      std::string* error_message) {
  return EvaluateInternal(now_sec, /*force=*/false, result, error_message);
}

bool ScheduledBackupService::EvaluateNow(std::int64_t now_sec,
                                         ScheduleEvaluationResult* result,
                                         std::string* error_message) {
  return EvaluateInternal(now_sec, /*force=*/true, result, error_message);
}

bool ScheduledBackupService::EvaluateInternal(std::int64_t now_sec, bool force,
                                              ScheduleEvaluationResult* result,
                                              std::string* error_message) {
  if (error_message != nullptr) error_message->clear();
  if (result == nullptr) {
    SetError(error_message, "Schedule evaluation result must not be null");
    return false;
  }
  *result = ScheduleEvaluationResult{};
  if (store_ == nullptr) {
    SetError(error_message, "Schedule store must not be null");
    return false;
  }
  if (repository_path_.empty()) {
    SetError(error_message,
             "Cannot run the scheduled backup: no repository is configured");
    return false;
  }

  ScheduleDocument document;
  const ScheduleLoadStatus load_status = store_->Load(&document, error_message);
  if (load_status == ScheduleLoadStatus::kError) return false;
  if (load_status == ScheduleLoadStatus::kMissing) {
    SetError(error_message, "The scheduled backup is not configured: " +
                                store_->schedule_file_path());
    return false;
  }

  const std::uint32_t interval = document.config.interval_minutes;
  result->next_run_time_sec = document.state.next_run_time_sec;

  if (!document.config.enabled) {
    result->status = ScheduleEvaluationStatus::kDisabled;
    return true;
  }
  if (!force &&
      !IsScheduleDue(now_sec, document.state.next_run_time_sec, interval)) {
    result->status = ScheduleEvaluationStatus::kNotDue;
    return true;
  }

  auto finish_failed = [&](const std::string& diagnostic) {
    result->status = ScheduleEvaluationStatus::kFailed;
    result->diagnostic = diagnostic;
    // 失败也要推进 next_run：否则一个坏配置会让计划每次 tick 都重试一遍。
    document.state.next_run_time_sec = ScheduleNextRunTime(now_sec, interval);
    result->next_run_time_sec = document.state.next_run_time_sec;
    ScheduleHistoryEntry entry;
    entry.scheduled_at_sec = now_sec;
    entry.started_at_sec = now_sec;
    entry.finished_at_sec = now_sec;
    entry.result = ScheduleRunResult::kFailed;
    entry.changes = result->changes;
    entry.diagnostic = diagnostic;
    AppendHistoryEntry(&document.state, entry);
    std::string save_error;
    if (!store_->Save(document, &save_error)) {
      result->diagnostic +=
          " (and the schedule state could not be saved: " + save_error + ")";
    }
    return true;
  };

  // ---- 自愈 + 仓库可用性 ----
  //
  // 两件事共用这一次 List：
  //   1. 用户手动删掉的快照从 managed 名单里消失（history 保留——它不是当前
  //      文件列表）；
  //   2. 判断"仓库到底能不能读"。这一点在修 baseline 之前不重要，现在很关键：
  //      如果只是仓库没挂载，Resolve 一定会失败，而把"读不到"当成"baseline
  //      失效"会让程序在一个空的挂载点里新建一份备份。所以有 baseline 记录
  //      却读不出仓库时，直接失败、什么都不写。
  BackupCatalog catalog;
  std::vector<BackupRecord> listed;
  std::string list_error;
  if (catalog.List(repository_path_, &listed, &list_error)) {
    ReconcileAgainstRecords(listed, &document);
  } else if (!document.state.baseline.snapshot_file_name.empty()) {
    return finish_failed(
        "The backup repository could not be listed, so the recorded baseline "
        "was kept and nothing was written: " +
        list_error);
  }

  // 配置本身必须合法。合法化发生在 set / enable 时，所以走到这里还能不合法
  // 基本只有"有人手改了 schedule.json"。
  //
  // 这两条是**挂起**，不是"这一次失败"：
  //   * 一个字节都不写——写下去也存不回来（Save 会拒绝同一份非法配置），
  //     而"静默把用户手改的文件改成我们能接受的样子"更不可以；
  //   * 不推进 next_run、不记 history。kFailed 走的是 finish_failed，它会推进
  //     next_run，那在这里是错的：next_run 只活在内存里，磁盘上的值没变，
  //     下一轮读到旧值仍然"到点"，于是变成每秒一次的完整校验 + 一个新线程。
  //   * 调用方（GUI / watch）收到 kConfigInvalid 后进入明确的挂起状态并停止
  //     周期性重试，恢复只能靠用户显式保存一份合法配置。
  std::string config_error;
  if (!ValidateScheduleConfig(document.config, &config_error)) {
    result->status = ScheduleEvaluationStatus::kConfigInvalid;
    result->diagnostic = config_error;
    return true;
  }

  Filter filter;
  if (!BuildScheduleFilter(document.config, &filter, &config_error)) {
    result->status = ScheduleEvaluationStatus::kConfigInvalid;
    result->diagnostic = config_error;
    return true;
  }

  // ---- 扫描：与备份用的是同一个扫描器，集合不可能漂移 ----
  std::vector<ManifestEntry> current;
  std::string manifest_error;
  if (!BuildSourceManifest(document.config.source_path, &filter, &current,
                           &manifest_error)) {
    return finish_failed(manifest_error);
  }

  // ---- 能不能拿"上一份 manifest"当基线？----
  //
  // 三份材料缺一不可：
  //   * state 里记着 baseline 快照，而且它**现在仍然真实存在于当前仓库里、
  //     仍然归本 scheduler 管理**（EvaluateScheduleBaseline）；
  //   * 那份 manifest 文件本身还读得出来；
  //   * **manifest 自己声明的归属与 state 记录的 baseline 完全一致**。
  //
  // 第三条是这条不变式的最后一环，也是最容易被忽略的一环。"baseline 存在"与
  // "manifest 与当前源相同"这两条各自成立，并不蕴含"那份 baseline 装的就是
  // 当前源"。反例正是崩溃留下的中间状态：archive 与 manifest 都已经换成了新的
  // （M2 / S2），state 却还没来得及保存（仍然是 S1）。下一轮 S1 依然存在且
  // managed，manifest 也依然等于当前源，于是错误地跳过一轮——而仓库里根本没有
  // 任何一份快照装得下 M2。三个文件无法原子一起提交，所以必须让 manifest 带着
  // 自己的归属，靠"这一对是否配套"来判定，而不是靠"两边分别看起来都还行"。
  //
  // 配套不上一律按"没有可信基线"处理：重建一份完整快照。多建一份，绝不错误跳过。
  std::string baseline_error;
  const ScheduleBaselineStatus baseline_status =
      EvaluateScheduleBaseline(document, repository_path_, &baseline_error);

  std::vector<ManifestEntry> previous;
  ManifestBinding manifest_binding;
  // manifest_error 复用上面扫描用的那个：两处报错都只在一轮之内用一次。
  const ScheduleStore::ManifestLoadStatus manifest_status =
      store_->LoadManifest(&previous, &manifest_binding, &manifest_error);

  std::string baseline_reason;
  bool baseline_usable = false;
  if (baseline_status != ScheduleBaselineStatus::kValid) {
    baseline_reason = ScheduleBaselineStatusText(baseline_status);
    if (!baseline_error.empty()) baseline_reason += " (" + baseline_error + ")";
  } else if (manifest_status == ScheduleStore::ManifestLoadStatus::kError) {
    baseline_reason = "The previous source manifest could not be read (" +
                      manifest_error + ")";
  } else if (manifest_status == ScheduleStore::ManifestLoadStatus::kMissing) {
    baseline_reason = "The previous source manifest file is gone";
  } else if (manifest_binding.empty()) {
    // version 1 的 manifest：没有归属信息。绝不猜它属于 state 里那份 baseline。
    baseline_reason =
        "The previous source manifest was written by an older version and does "
        "not record which snapshot it belongs to";
  } else if (!SameBaselineBinding(document.state.baseline, manifest_binding)) {
    baseline_reason =
        "The previous source manifest belongs to a different snapshot than the "
        "recorded baseline (manifest belongs to '" +
        manifest_binding.snapshot_file_name + "', the recorded baseline is '" +
        document.state.baseline.snapshot_file_name + "')";
  } else {
    baseline_usable = true;
  }

  // ---- PR #18：增量策略的结论由共享增量引擎给出 ----
  //
  // 这里刻意**不**用 metadata-first 的比较来决定增量要不要写：内容身份必须
  // 是真实摘要，否则 same-size + same-mtime 的改写会被漏掉，而漏掉的那一次
  // 变化会成为所有后代的错误祖先。
  //
  // 引擎可能已经写好了一份快照（完整基线或 delta），也可能什么都没写；
  // 两种结果都被翻译成下面那条**共用的**尾巴所期待的几个变量，
  // 所以登记 / baseline 绑定 / manifest / retention / state / history 的
  // 语义在三种路径上完全一致。
  const bool incremental_mode =
      document.config.strategy == BackupStrategy::kIncremental;
  bool snapshot_already_written = false;
  std::string incremental_snapshot_path;

  if (incremental_mode) {
    std::string work_error;
    if (!catalog.EnsureRepository(repository_path_, &work_error)) {
      return finish_failed(work_error);
    }
    // 命名规则仍然只属于 BackupCatalog。这里只是**预留**一个名字：
    // 引擎若判定"没有变化"，这个名字不会被用到（BuildArchivePath 不创建文件）。
    std::string candidate_path;
    if (!catalog.BuildArchivePath(repository_path_, document.config.source_path,
                                  now_sec, &candidate_path, &work_error)) {
      return finish_failed(work_error);
    }

    BackupOptions incremental_options;
    incremental_options.pack_method = document.config.pack_method;
    incremental_options.compression_method = document.config.compression_method;
    // 无人值守计划不接受加密：配置层已经拒过一次，这里不再提供入口。
    incremental_options.encryption_method = EncryptionMethod::kNone;

    IncrementalOutcome outcome;
    if (!RunIncrementalBackup(
            document.config.source_path, repository_path_,
            BaseNameOf(candidate_path), RepositoryIdentity(repository_path_),
            filter, incremental_options, document.config.include_rules,
            document.config.exclude_rules, std::string(), &outcome,
            &work_error)) {
      return finish_failed(work_error);
    }

    result->changes = outcome.summary;
    const bool had_baseline =
        !document.state.baseline.snapshot_file_name.empty();
    if (outcome.kind == IncrementalOutcome::Kind::kNoChanges) {
      // 什么都没写：下面的 skip 分支会因为 changes 为空而接管，
      // 连"不更新 manifest、不建空文件"这些细节都走同一条代码。
    } else {
      snapshot_already_written = true;
      incremental_snapshot_path =
          repository_path_ + "/" + outcome.snapshot_file_name;
      result->archive_file_name = outcome.snapshot_file_name;
      if (outcome.kind == IncrementalOutcome::Kind::kFullBaseline) {
        result->first_snapshot = !had_baseline;
        result->baseline_reset = had_baseline;
        result->diagnostic +=
            "Requested strategy = incremental, but this run created a full "
            "baseline snapshot. Reason: " +
            outcome.baseline_reason + ". ";
      } else {
        result->diagnostic +=
            "Incremental delta on top of '" + outcome.parent_file_name + "'. ";
      }
    }
  }

  if (!incremental_mode && baseline_usable) {
    std::string diff_error;
    if (!DiffManifests(previous, current, &result->changes, nullptr,
                       &diff_error)) {
      return finish_failed(diff_error);
    }
  } else if (!incremental_mode) {
    // 没有可信基线：这一轮产出一份**完整基线快照**。
    // 多建一份完整备份，绝不漏变化——这正是本 PR 的核心语义。
    const bool had_baseline =
        !document.state.baseline.snapshot_file_name.empty();
    result->first_snapshot = !had_baseline;
    result->baseline_reset = had_baseline;
    result->changes = ChangeSummary{};
    result->changes.added = current.size();
    if (had_baseline) {
      result->diagnostic =
          "The recorded baseline is no longer usable, so this run created a "
          "new baseline snapshot. Reason: " +
          baseline_reason + ". ";
    }
  }

  // ---- 没变化：skip ----
  //
  // 这里什么都不做才是正确的：不调用 BackupEngine、不创建 .bak、
  // 不更新 last successful manifest、不搞"删一个旧文件再建一个一样的新文件"
  // 的伪轮换。
  if (!result->first_snapshot && result->changes.empty()) {
    result->status = ScheduleEvaluationStatus::kSkippedNoChanges;
    document.state.next_run_time_sec = ScheduleNextRunTime(now_sec, interval);
    result->next_run_time_sec = document.state.next_run_time_sec;

    ScheduleHistoryEntry entry;
    entry.scheduled_at_sec = now_sec;
    entry.started_at_sec = now_sec;
    entry.finished_at_sec = now_sec;
    entry.result = ScheduleRunResult::kSkippedNoChanges;
    entry.changes = result->changes;
    AppendHistoryEntry(&document.state, entry);

    std::string save_error;
    if (!store_->Save(document, &save_error)) {
      return finish_failed(
          "The run was skipped, but the schedule state could "
          "not be saved: " +
          save_error);
    }
    return true;
  }

  // ---- 有变化（或基线失效）：创建一份完整独立的 v2 快照 ----
  std::string work_error;
  if (!catalog.EnsureRepository(repository_path_, &work_error)) {
    return finish_failed(work_error);
  }

  BackupOptions options;
  options.pack_method = document.config.pack_method;
  options.compression_method = document.config.compression_method;
  // 无人值守计划不接受加密；配置层已经拒过一次，这里再钉一遍：
  // 没有任何密码参数能从这条路径进来。
  options.encryption_method = EncryptionMethod::kNone;

  std::string archive_path;
  if (snapshot_already_written) {
    // 增量引擎已经把它写好了（并且写好了它的 manifest / identity 副文件）。
    archive_path = incremental_snapshot_path;
  } else {
    if (!catalog.BuildArchivePath(repository_path_, document.config.source_path,
                                  now_sec, &archive_path, &work_error)) {
      return finish_failed(work_error);
    }
    BackupEngine engine;
    if (!engine.Backup(document.config.source_path, archive_path, filter,
                       options, &work_error)) {
      return finish_failed(work_error);
    }
  }

  // 归档已经成功发布。从这一刻起，这一轮就是"成功"——后面的登记、淘汰、
  // 写日志失败都只能降级成诊断，不能把它说成备份失败。
  const std::string file_name = BaseNameOf(archive_path);
  result->archive_file_name = file_name;

  std::uint64_t archive_size = 0;
  struct stat archive_status;
  if (::lstat(archive_path.c_str(), &archive_status) == 0 &&
      S_ISREG(archive_status.st_mode)) {
    archive_size = static_cast<std::uint64_t>(archive_status.st_size);
  }

  ScheduledSnapshotRecord record;
  record.file_name = file_name;
  record.created_time_sec = now_sec;
  record.changes = result->changes;
  record.entry_count = current.size();
  record.archive_size = archive_size;
  record.pack_method = options.pack_method;
  record.compression_method = options.compression_method;
  document.state.managed_snapshots.push_back(record);
  document.state.last_success_time_sec = now_sec;
  document.state.last_manifest_entry_count = current.size();
  document.state.next_run_time_sec = ScheduleNextRunTime(now_sec, interval);
  result->next_run_time_sec = document.state.next_run_time_sec;

  // ★ 这个 manifest 从此属于**这一份**快照。三者一起写下去，下一轮才有资格
  //   用"manifest 相同"来判定"源没变"。
  document.state.baseline.snapshot_file_name = file_name;
  document.state.baseline.repository_identity =
      RepositoryIdentity(repository_path_);
  document.state.baseline.source_path = document.config.source_path;

  // ★ 崩溃一致性的顺序（§36）：archive 已发布 -> manifest -> retention
  //   -> state -> history -> state。
  //
  //   如果在这里崩：archive 已经是一个完整可恢复的 .bak，而 state 里还没有
  //   它。它最多变成一个"没人认领的普通备份"，下次 List 照样列得出来、
  //   恢复得了、删得掉。state 永远不会让 archive 的正确性依赖它。
  // manifest 写下去时带着**与 state 完全相同**的归属。两份文件因此各自
  // 自描述，任何"只写成功了一半"的中间状态都会在下一轮被识别成不配套。
  const ManifestBinding binding = BindingOf(document.state.baseline);
  std::string persist_error;
  if (!store_->SaveManifest(current, binding, &persist_error)) {
    result->diagnostic +=
        "The snapshot was created, but the source manifest could not be "
        "saved: " +
        persist_error + " ";
  }

  // ---- retention：只在**这次真的创建成功之后**执行 ----
  //
  // 它必须排在第一次 Save **之前**。这不是顺序偏好，而是"写出去的东西必须
  // 读得回来"：上一段刚把这一份新快照 push 进名单，长度是 retain + 1，而
  // ScheduleStore 只接受 kMaxRetainCount 条，retain_count 又允许取到
  // kMaxRetainCount。于是"先 Save 再淘汰"在满额时会要求写出一份自己都读不
  // 回来的 state：Save 拒绝，这一轮在写盘处提前返回，retention 永远轮不到，
  // 下一轮再建一份……仓库无上限增长，而且每一轮都报成功、退出码 0。
  //
  // 先淘汰再落盘，落盘的那份长度就恒 <= retain_count <= 文件上界，
  // "能写出去的都能读回来"由构造保证。
  //
  // 淘汰删的是最旧的那些，而 baseline 刚被设成**最新**的这一份，
  // 所以 retention 不可能删掉 baseline —— 这一点与改动前完全一致。
  std::uint64_t deleted = 0;
  std::uint64_t failed = 0;
  std::uint64_t dependency_retained = 0;
  std::uint64_t retention_unreadable = 0;
  std::string retention_error;
  const bool retention_ok =
      RunRetention(&document, &deleted, &failed, &dependency_retained,
                   &retention_unreadable, &retention_error);
  result->retention_dependency_retained = dependency_retained;
  result->retention_unreadable = retention_unreadable;
  result->retention_deleted = deleted;
  result->retention_failed = failed;
  result->status = StatusForRetention(retention_ok);
  if (!retention_ok) {
    result->diagnostic += retention_error + " ";
  }

  if (!store_->Save(document, &persist_error)) {
    result->diagnostic +=
        "The snapshot was created, but the schedule state could not be "
        "saved: " +
        persist_error + " ";
    result->status = ScheduleEvaluationStatus::kCreatedSnapshot;
    return true;
  }

  ScheduleHistoryEntry entry;
  entry.scheduled_at_sec = now_sec;
  entry.started_at_sec = now_sec;
  entry.finished_at_sec = now_sec;
  entry.result = retention_ok ? ScheduleRunResult::kSuccessCreated
                              : ScheduleRunResult::kSuccessWithRetentionWarning;
  entry.archive_file_name = file_name;
  entry.changes = result->changes;
  entry.diagnostic = result->diagnostic;
  AppendHistoryEntry(&document.state, entry);

  std::string final_save_error;
  if (!store_->Save(document, &final_save_error)) {
    // 归档、那份 manifest、以及 managed 名单都已经落盘了；只有 history 这一条
    // 没写进去。仍然是 kCreatedSnapshot，把原因挂在诊断里。
    result->diagnostic +=
        "The snapshot was created, but the run history could not be saved: " +
        final_save_error;
  }
  return true;
}

}  // namespace backupproject
