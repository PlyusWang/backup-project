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

ScheduledBackupService::ScheduledBackupService(std::string repository_path,
                                               ScheduleStore* store)
    : repository_path_(std::move(repository_path)), store_(store) {}

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

  // 自愈发生在所有判断之前：用户手动删掉的快照，从 managed 名单里消失。
  // history 里"曾经创建过"的记录保留——history 不是当前文件列表。
  ReconcileManagedSnapshots(repository_path_, &document);

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

  // ---- 与上一份成功 manifest 比较 ----
  std::vector<ManifestEntry> previous;
  const ScheduleStore::ManifestLoadStatus manifest_status =
      store_->LoadManifest(&previous, &manifest_error);
  if (manifest_status == ScheduleStore::ManifestLoadStatus::kLoaded) {
    std::string diff_error;
    if (!DiffManifests(previous, current, &result->changes, nullptr,
                       &diff_error)) {
      return finish_failed(diff_error);
    }
  } else {
    // 没有上一份可比对的基线：首次快照。manifest 坏了也走这条路——
    // 多建一份完整备份，绝不漏变化。
    result->first_snapshot = true;
    result->changes = ChangeSummary{};
    result->changes.added = current.size();
    if (manifest_status == ScheduleStore::ManifestLoadStatus::kError) {
      result->diagnostic =
          "The previous source manifest could not be read, so this run was "
          "treated as the first snapshot: " +
          manifest_error;
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

  // ---- 有变化：创建一份完整独立的 v2 快照 ----
  BackupCatalog catalog;
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

  // ★ 崩溃一致性的顺序（§36）：archive 已发布 -> manifest -> state -> retention
  //   -> history -> state。
  //
  //   如果在这里崩：archive 已经是一个完整可恢复的 .bak，而 state 里还没有
  //   它。它最多变成一个"没人认领的普通备份"，下次 List 照样列得出来、
  //   恢复得了、删得掉。state 永远不会让 archive 的正确性依赖它。
  std::string persist_error;
  if (!store_->SaveManifest(current, &persist_error)) {
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
