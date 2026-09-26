// scheduled_backup_service.cpp

#include "scheduled_backup_service.h"

#include <sys/stat.h>
#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "backup_catalog.h"
#include "backup_engine.h"
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

// 最旧的 managed snapshot 下标。时间相同再按 file_name 升序，
// 保证"删哪一个"是确定的，不依赖容器里的偶然顺序。
std::size_t OldestManagedIndex(
    const std::vector<ScheduledSnapshotRecord>& records) {
  std::size_t oldest = 0;
  for (std::size_t index = 1; index < records.size(); ++index) {
    if (records[index].created_time_sec < records[oldest].created_time_sec ||
        (records[index].created_time_sec == records[oldest].created_time_sec &&
         records[index].file_name < records[oldest].file_name)) {
      oldest = index;
    }
  }
  return oldest;
}

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
                                          std::string* error_message) const {
  if (deleted != nullptr) *deleted = 0;
  if (failed != nullptr) *failed = 0;
  if (error_message != nullptr) error_message->clear();
  if (document == nullptr) {
    SetError(error_message, "Schedule document must not be null");
    return false;
  }

  BackupCatalog catalog;
  const std::size_t retain = document->config.retain_count;
  while (document->state.managed_snapshots.size() > retain) {
    const std::size_t oldest =
        OldestManagedIndex(document->state.managed_snapshots);
    // 先拷出来：下面会 erase，引用立刻失效。
    const std::string file_name =
        document->state.managed_snapshots[oldest].file_name;

    std::string delete_error;
    // 删除永远走 BackupCatalog：它是唯一实现"file_name 必须是仓库直接子项、
    // 必须是普通文件、不是软链接"这条路径安全边界的地方。
    // 绝不写成 std::filesystem::remove(repository + "/" + file_name)。
    if (!catalog.Delete(repository_path_, file_name, &delete_error)) {
      if (failed != nullptr) *failed += 1;
      SetError(error_message,
               "Failed to remove the oldest scheduled snapshot '" + file_name +
                   "': " + delete_error);
      // 删不掉的记录保留在 managed 列表里，下一轮再试。
      // 新快照已经成功，绝不能因为淘汰失败就把它当成整体失败。
      return false;
    }
    document->state.managed_snapshots.erase(
        document->state.managed_snapshots.begin() +
        static_cast<std::ptrdiff_t>(oldest));
    if (deleted != nullptr) *deleted += 1;
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
  // 基本只有"有人手改了 schedule.json"。此时不写盘——写了也存不回去。
  std::string config_error;
  if (!ValidateScheduleConfig(document.config, &config_error)) {
    result->status = ScheduleEvaluationStatus::kFailed;
    result->diagnostic = config_error;
    return true;
  }

  Filter filter;
  if (!BuildScheduleFilter(document.config, &filter, &config_error)) {
    result->status = ScheduleEvaluationStatus::kFailed;
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

  if (baseline_usable) {
    std::string diff_error;
    if (!DiffManifests(previous, current, &result->changes, nullptr,
                       &diff_error)) {
      return finish_failed(diff_error);
    }
  } else {
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

  std::string archive_path;
  if (!catalog.BuildArchivePath(repository_path_, document.config.source_path,
                                now_sec, &archive_path, &work_error)) {
    return finish_failed(work_error);
  }

  BackupOptions options;
  options.pack_method = document.config.pack_method;
  options.compression_method = document.config.compression_method;
  // 无人值守计划不接受加密；配置层已经拒过一次，这里再钉一遍：
  // 没有任何密码参数能从这条路径进来。
  options.encryption_method = EncryptionMethod::kNone;

  BackupEngine engine;
  if (!engine.Backup(document.config.source_path, archive_path, filter, options,
                     &work_error)) {
    return finish_failed(work_error);
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

  // ★ 崩溃一致性的顺序（§36）：archive 已发布 -> manifest -> state -> retention
  //   -> history -> state。
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
        "The snapshot was created, but the source manifest could not be saved "
        "(the next run will be treated as a first snapshot): " +
        persist_error + " ";
  }
  if (!store_->Save(document, &persist_error)) {
    result->diagnostic +=
        "The snapshot was created, but the schedule state could not be "
        "saved: " +
        persist_error + " ";
    result->status = ScheduleEvaluationStatus::kCreatedSnapshot;
    return true;
  }

  // ---- retention：只在**这次真的创建成功之后**执行 ----
  std::uint64_t deleted = 0;
  std::uint64_t failed = 0;
  std::string retention_error;
  const bool retention_ok =
      RunRetention(&document, &deleted, &failed, &retention_error);
  result->retention_deleted = deleted;
  result->retention_failed = failed;
  if (!retention_ok) {
    result->diagnostic += retention_error + " ";
  }

  result->status = StatusForRetention(retention_ok);

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
