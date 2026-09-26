// schedule_store.h
//
// 定时备份的持久化：config + runtime state + history + managed snapshot 名录。
//
// 为什么不做进 ConfigManager：ConfigManager 的 schema 已经定型且干净
// （version 1 + backup_repository_path），把计划任务塞进去会让它变成大杂烩，
// 也会把"改一个字段"和"改整个应用的配置版本"绑在一起。所以这里是**独立**的
// versioned store，ConfigManager 的 schema / version 一个字都不动。
//
// 落盘布局（两个文件，都在同一个目录下，由 schedule_file_path 派生）：
//
//   <dir>/schedule.json          配置 + 运行状态 + 名录 + history
//   <dir>/schedule-manifest.dat  上一份成功快照的源清单（可能很大）
//   <dir>/schedule.json.lock     运行期 single-runner 锁（flock）
//
// 为什么不把 manifest 塞进 JSON：源树几万条时，每轮都要解析/序列化一个巨大的
// JSON 对象；而且 state 每次保存都会连带重写它。拆开之后两个文件各自原子替换，
// state 引用关系只有"上一份成功 manifest 就是这个固定路径"这一条，
// 崩溃后自愈规则也简单：manifest 不见了/坏了 = 没有上一份快照 =
// 走首次快照语义， 多建一份完整备份，绝不漏变化。
//
// 崩溃一致性（§36）的落点在这里：
//   * Save() 永远是 temp + fsync + rename + 目录
//   fsync，读者永远看不到半个文件；
//   * archive 先发布、manifest 再落盘、state 最后落盘；
//   * state 里只存单组件 file_name，绝不存绝对路径——真正的 resolve/delete
//     继续走 BackupCatalog，路径安全边界不在这里开口子；
//   * state 丢失也绝不影响 archive 的正确性：它最多让一份 .bak 变成"没人认领的
//     普通备份"，照样可以列出、恢复、删除。
//
// 本文件是纯 C++17：不依赖 Qt，不依赖任何第三方 JSON 库。

#ifndef BACKUP_PROJECT_INCLUDE_SCHEDULE_STORE_H_
#define BACKUP_PROJECT_INCLUDE_SCHEDULE_STORE_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "backup_mode.h"
#include "container_format.h"
#include "filter.h"
#include "pack_stream.h"
#include "source_manifest.h"

namespace backupproject {

inline constexpr std::uint32_t kDefaultIntervalMinutes = 60;
inline constexpr std::uint32_t kDefaultRetainCount = 12;
inline constexpr std::uint32_t kMinIntervalMinutes = 1;
// 一年。再大就不是"定时"而是"手动"了，而且 interval*60 的溢出必须先被范围
// 检查拦掉，不能靠 int64 的巧合。
inline constexpr std::uint32_t kMaxIntervalMinutes = 525600;
inline constexpr std::uint32_t kMinRetainCount = 1;
inline constexpr std::uint32_t kMaxRetainCount = 1000;
// history 只保留最近这么多条，超出就丢最旧的。它**不是**当前文件列表，
// 只是"曾经跑过什么"的日志。
inline constexpr std::size_t kMaxHistoryEntries = 32;
inline constexpr std::size_t kMaxScheduleStringBytes = 4096;
inline constexpr std::size_t kMaxScheduleRules = 1024;
inline constexpr std::size_t kMaxScheduleFileBytes = 4u * 1024u * 1024u;

// 一条计划配置。默认值就是"没配过"：disabled、每小时、留 12 份、
// MyPack + 不压缩 + 不加密、没有筛选规则。
struct ScheduleConfig {
  bool enabled = false;

  BackupTrigger trigger = BackupTrigger::kScheduled;
  BackupStrategy strategy = BackupStrategy::kFull;

  std::string source_path;

  std::uint32_t interval_minutes = kDefaultIntervalMinutes;
  std::uint32_t retain_count = kDefaultRetainCount;

  PackMethod pack_method = PackMethod::kMyPack;
  CompressionMethod compression_method = CompressionMethod::kNone;
  // 本 PR 的无人值守计划只允许 kNone。理由见 ValidateScheduleConfig。
  EncryptionMethod encryption_method = EncryptionMethod::kNone;

  std::vector<std::string> include_rules;
  std::vector<std::string> exclude_rules;
};

// 把配置里的规则编成 Filter。每条规则都走真实的 Filter::AddRule，
// 因此"配置里存的规则"与"CLI 敲进去的规则"是同一套语法、同一套报错。
bool BuildScheduleFilter(const ScheduleConfig& config, Filter* filter,
                         std::string* error_message);

// 结构校验：trigger / strategy / interval / retain / 加密边界 / 规则语法。
// 不含任何文件系统探测。
bool ValidateScheduleConfig(const ScheduleConfig& config,
                            std::string* error_message);

// 启用前的完整校验：结构校验 + source_path
// 非空、存在、是真实目录（不是软链接）。
bool ValidateScheduleForEnable(const ScheduleConfig& config,
                               std::string* error_message);

// 一个由本 scheduler 自己创建、并且仍然归它管理的快照。
//
// 刻意不持久 archive 的绝对路径：只留单组件 file_name，真正的解析与删除
// 一律走 BackupCatalog——路径越界的口子只在那一处收口。
struct ScheduledSnapshotRecord {
  std::string file_name;
  std::int64_t created_time_sec = 0;

  ChangeSummary changes;

  std::uint64_t entry_count = 0;
  std::uint64_t archive_size = 0;

  PackMethod pack_method = PackMethod::kMyPack;
  CompressionMethod compression_method = CompressionMethod::kNone;
};

enum class ScheduleRunResult {
  kSuccessCreated,
  kSkippedNoChanges,
  kFailed,
  kSuccessWithRetentionWarning,
};

const char* ScheduleRunResultKey(ScheduleRunResult result);
const char* ScheduleRunResultText(ScheduleRunResult result);
bool ParseScheduleRunResultKey(const std::string& key,
                               ScheduleRunResult* result);

struct ScheduleHistoryEntry {
  std::int64_t scheduled_at_sec = 0;
  std::int64_t started_at_sec = 0;
  std::int64_t finished_at_sec = 0;

  ScheduleRunResult result = ScheduleRunResult::kFailed;

  // 只有真的产出了归档时非空。
  std::string archive_file_name;

  ChangeSummary changes;

  // 诊断原文（核心给出的错误文本，或 retention 的失败原因）。
  // 绝不允许出现密码：本 PR 的计划任务不接受加密，这条是硬约束而非约定。
  std::string diagnostic;
};

struct ScheduleState {
  // 下一次应该运行的时间（Unix epoch 秒）。0 表示"还没算过"。
  std::int64_t next_run_time_sec = 0;
  std::int64_t last_success_time_sec = 0;
  std::uint64_t last_manifest_entry_count = 0;

  // 只有 scheduler 自己创建的快照才会出现在这里。用户手动备份、GUI 手动备份
  // 永远不进这个列表，所以自动淘汰不可能删到它们。
  std::vector<ScheduledSnapshotRecord> managed_snapshots;

  std::vector<ScheduleHistoryEntry> history;
};

struct ScheduleDocument {
  ScheduleConfig config;
  ScheduleState state;
};

enum class ScheduleLoadStatus { kLoaded, kMissing, kError };

// 定时备份持久化。构造函数只接受显式路径：core 不猜 HOME、不读 XDG、
// 不碰 QSettings，默认位置由 app_paths.h 在应用层算好再传进来。
class ScheduleStore {
 public:
  explicit ScheduleStore(std::string schedule_file_path);

  const std::string& schedule_file_path() const { return schedule_file_path_; }
  // schedule.json -> schedule-manifest.dat
  std::string manifest_file_path() const;
  // schedule.json.lock
  std::string lock_file_path() const;

  // kMissing 表示文件不存在（不是错误）：调用方据此走"默认 disabled 配置"。
  // kError 表示文件存在但读不懂，此时 *document 保持默认值，error_message
  // 里是具体原因——绝不静默回退到默认配置。
  ScheduleLoadStatus Load(ScheduleDocument* document,
                          std::string* error_message) const;

  // 原子保存：临时文件 -> fsync -> rename -> 目录 fsync。
  // 新文件权限固定 0600（不受 umask 影响）：里面虽然没有密码，
  // 但它记录了用户的目录结构，没有理由是 0666。
  bool Save(const ScheduleDocument& document, std::string* error_message) const;

  // 上一份成功快照的源清单。
  //
  // kMissing 同时覆盖"从来没写过"和"文件被删掉了"两种情况——两者的处理方式
  // 完全一样：没有可比对的基线，走首次快照语义。kError（文件在但坏了）也
  // 按同一语义处理，但调用方应当把 error_message 记进诊断里，不要假装无事发生。
  enum class ManifestLoadStatus { kLoaded, kMissing, kError };

  ManifestLoadStatus LoadManifest(std::vector<ManifestEntry>* entries,
                                  std::string* error_message) const;
  bool SaveManifest(const std::vector<ManifestEntry>& entries,
                    std::string* error_message) const;
  bool RemoveManifest(std::string* error_message) const;

 private:
  std::string schedule_file_path_;
};

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_SCHEDULE_STORE_H_
