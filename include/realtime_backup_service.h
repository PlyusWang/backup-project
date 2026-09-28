// realtime_backup_service.h
//
// PR #19：一次"已经稳定的 realtime 触发"如何执行，以及 realtime 快照的归属。
//
// 三条硬规则：
//
//   1. **Realtime 只决定什么时候触发**。Full 继续走 BackupEngine，Incremental
//      继续走 RunIncrementalBackup；这里不复制任何一个策略的实现。
//   2. **归属不用中央可变 state**。每份 realtime 快照配一个 per-snapshot marker
//      sidecar `<snapshot>.realtime`（BPREALTIME1），内容机器写机器读。没有
//      "每次备份都改写"的 realtime history JSON，也就没有"监听 home 时状态文件
//      自己触发自己"的问题。
//   3. **marker 必须绑定实际 archive bytes**：写 marker 之前用 PR #18 的
//      LoadVerifiedSnapshotIdentity 拿到验证过的 snapshot id，只信实际字节，
//      不信 header / envelope 的声明值。
//
// retention 直接复用 PR #18 的依赖感知计划与 descendants-first 删除；跨 Trigger
// 的祖先（Manual / Scheduled 建的）绝不被 realtime retention 误删。
//
// 本文件是纯 C++17：不依赖 Qt，也不依赖任何第三方库。

#ifndef BACKUP_PROJECT_INCLUDE_REALTIME_BACKUP_SERVICE_H_
#define BACKUP_PROJECT_INCLUDE_REALTIME_BACKUP_SERVICE_H_

#include <cstdint>
#include <string>
#include <vector>

#include "backup_catalog.h"
#include "incremental_backup.h"
#include "realtime_store.h"

namespace backupproject {

// ---- marker ----
inline constexpr const char* kRealtimeMarkerSuffix = ".realtime";
inline constexpr const char* kRealtimeMarkerHeader = "BPREALTIME1\n";
inline constexpr std::size_t kMaxRealtimeMarkerBytes = 64u * 1024u;

struct RealtimeMarker {
  std::string snapshot_file_name;
  std::string snapshot_id;  // 实际 archive bytes 的 verified id
  std::int64_t created_time_sec = 0;
  std::string job_identity;
  std::string source_identity;
  std::string filter_identity;
  BackupStrategy strategy = BackupStrategy::kFull;
  PackMethod pack_method = PackMethod::kMyPack;
  CompressionMethod compression_method = CompressionMethod::kNone;
  std::uint64_t event_count = 0;
  bool overflow_recovery = false;
  bool resync_trigger = false;
  // full / full-baseline / delta / no-changes
  std::string outcome_kind;
  std::uint64_t added = 0;
  std::uint64_t removed = 0;
  std::uint64_t modified = 0;
  std::uint64_t metadata_changed = 0;
};

// <snapshot>.realtime
std::string RealtimeMarkerFileName(const std::string& snapshot_file_name);
bool IsRealtimeMarkerFileName(const std::string& file_name,
                              std::string* snapshot_file_name);

std::string SerializeRealtimeMarker(const RealtimeMarker& marker);
bool ParseRealtimeMarker(const std::string& text, RealtimeMarker* marker,
                         std::string* error_message);
// temp + fsync + rename。
bool WriteRealtimeMarker(const std::string& repository_directory,
                         const RealtimeMarker& marker,
                         std::string* error_message);
// 只读一个 marker；失败时 error_message 说明原因（调用方据此拒绝"信任"它）。
bool LoadRealtimeMarker(const std::string& repository_directory,
                        const std::string& snapshot_file_name,
                        RealtimeMarker* marker, std::string* error_message);

// ---- 发现 / history ----
struct RealtimeSnapshotRecord {
  std::string file_name;
  std::string marker_file_name;
  std::string snapshot_id;
  std::string job_identity;
  std::string outcome_kind;
  std::int64_t created_time_sec = 0;
  std::uint64_t event_count = 0;
  bool overflow_recovery = false;
  bool resync_trigger = false;
  BackupStrategy strategy = BackupStrategy::kFull;
  PackMethod pack_method = PackMethod::kMyPack;
  CompressionMethod compression_method = CompressionMethod::kNone;
  std::uint64_t added = 0;
  std::uint64_t removed = 0;
  std::uint64_t modified = 0;
  std::uint64_t metadata_changed = 0;
  std::uint64_t archive_size = 0;
  std::int64_t archive_mtime_sec = 0;
  // marker 与 .bak 都对得上（marker 自洽 + archive 身份与 marker 一致）
  bool verified = false;
  std::string diagnostic;
};

// 扫描 repository 里的 *.bak.realtime，strict parse，Resolve .bak，验证实际
// 身份，再与 BackupCatalog 记录 JOIN。按 created_time_sec、file_name 排序。
// 无效 marker 不删、不当作可信记录，只带 diagnostic。
bool ListRealtimeSnapshots(const std::string& repository_directory,
                           std::vector<RealtimeSnapshotRecord>* records,
                           std::string* error_message);

// ---- 一次触发 ----
struct RealtimeEventSummary {
  std::uint64_t generation = 0;
  std::uint64_t event_count = 0;
  bool overflow = false;
  bool structural = false;
  bool resync = false;
};

struct RealtimeOutcome {
  enum class Kind {
    kFullSnapshot,  // Realtime + Full 写出一份完整快照
    kFullBaseline,  // Realtime + Incremental 建了新的完整基线
    kDelta,         // Realtime + Incremental 写了 delta
    kNoChanges,     // 有效备份集合没变，什么都没写
    kFailed,
  };
  Kind kind = Kind::kNoChanges;
  std::string snapshot_file_name;
  std::string summary_text;
  std::string diagnostic;
  ChangeSummary changes;
  // marker 是否写成；没写成时 marker_warning = true，而且**不执行**破坏性
  // realtime retention（archive 保留、退化成普通快照）。
  bool marker_written = false;
  bool marker_warning = false;
  std::uint64_t retention_deleted = 0;
  std::uint64_t retention_kept_ancestors = 0;
  bool retention_uncertain = false;
};

// 执行一次已经稳定的 realtime trigger。events 只描述事件层面发生了什么；
// 它不会被当作"改了几个文件"。
bool RunRealtimeBackupOnce(const RealtimeConfig& config,
                           const std::string& repository_path,
                           const std::string& repository_identity,
                           const RealtimeEventSummary& events,
                           std::int64_t now_sec, RealtimeOutcome* outcome,
                           std::string* error_message);

// ---- retention ----
struct RealtimeRetentionResult {
  std::uint64_t deleted = 0;
  std::uint64_t kept_visible = 0;
  std::uint64_t kept_ancestors = 0;
  bool uncertain = false;
  std::string reason;
  std::vector<std::string> diagnostics;
};

// 只淘汰 job_identity 与当前配置相同的 realtime 快照；候选最旧在前；真正删除
// 交给 DeleteSnapshots（descendants-first）。任何一步无法验证 -> 不删 +
// warning。
bool RunRealtimeRetention(const std::string& repository_directory,
                          const std::string& job_identity,
                          std::uint32_t retain_count,
                          RealtimeRetentionResult* result,
                          std::string* error_message);

// ---- 测试接缝 ----
// 让下一次 marker 发布失败（archive 已经成功）。
void SetRealtimeMarkerWriteFailureForTesting(bool fail);

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_REALTIME_BACKUP_SERVICE_H_
