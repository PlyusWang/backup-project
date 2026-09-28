// realtime_store.h
//
// PR #19：Realtime Trigger 的配置存储。
//
// 为什么不是 ScheduleStore 的第三个模式：Scheduled 是**时间**触发的状态机，
// Realtime 是**文件事件**触发的状态机。两者共享 Strategy / Catalog / Filter /
// retention 这些 primitive，但不共享"触发状态"这份可变状态——把它们塞进同一个
// schedule.json 只会让"这一轮是谁触发的"变成一个需要猜的字段。
//
// 严格遵守项目既有约定：
//   * 解析是严格的：未知 key、重复 key、类型不符、超界、NUL、超长一律拒绝；
//   * 损坏的文件**不会**被"自动恢复成默认值"——那是把用户的配置悄悄换掉；
//   * 保存是原子的：唯一临时文件 + fsync + rename；
//   * 范围冻结（100 <= debounce <= 60000；500 <= max_wait <= 300000；
//     max_wait >= debounce；1 <= retain <= 1000）；
//   * 支持矩阵只有一份：Realtime 的合法性问 backup_mode 的那张真值表。
//
// 本文件是纯 C++17：不依赖 Qt，也不依赖任何第三方库。

#ifndef BACKUP_PROJECT_INCLUDE_REALTIME_STORE_H_
#define BACKUP_PROJECT_INCLUDE_REALTIME_STORE_H_

#include <cstdint>
#include <string>
#include <vector>

#include "backup_mode.h"
#include "container_format.h"
#include "pack_stream.h"

namespace backupproject {

inline constexpr std::size_t kMaxRealtimeStringBytes = 4096;
inline constexpr std::size_t kMaxRealtimeRules = 1024;
inline constexpr std::size_t kMaxRealtimeFileBytes = 4u * 1024u * 1024u;

inline constexpr std::uint32_t kRealtimeConfigVersion = 1;
inline constexpr std::uint32_t kDefaultRealtimeDebounceMs = 500;
inline constexpr std::uint32_t kDefaultRealtimeMaxWaitMs = 5000;
inline constexpr std::uint32_t kDefaultRealtimeRetainCount = 12;
inline constexpr std::uint32_t kMinRealtimeDebounceMs = 100;
inline constexpr std::uint32_t kMaxRealtimeDebounceMs = 60000;
inline constexpr std::uint32_t kMinRealtimeMaxWaitMs = 500;
inline constexpr std::uint32_t kMaxRealtimeMaxWaitMs = 300000;
inline constexpr std::uint32_t kMinRealtimeRetainCount = 1;
inline constexpr std::uint32_t kMaxRealtimeRetainCount = 1000;

// Realtime 配置。字段与 schedule.json 的 config 有意保持同形（strategy / pack /
// compression / rules），这样前端与用户看到的词汇不会因为触发方式而变。
struct RealtimeConfig {
  std::uint32_t version = kRealtimeConfigVersion;
  bool enabled = false;
  BackupTrigger trigger = BackupTrigger::kRealtime;
  std::string source_path;
  std::uint32_t debounce_ms = kDefaultRealtimeDebounceMs;
  std::uint32_t max_wait_ms = kDefaultRealtimeMaxWaitMs;
  std::uint32_t retain_count = kDefaultRealtimeRetainCount;
  BackupStrategy strategy = BackupStrategy::kFull;
  PackMethod pack_method = PackMethod::kMyPack;
  CompressionMethod compression_method = CompressionMethod::kNone;
  EncryptionMethod encryption_method = EncryptionMethod::kNone;
  std::vector<std::string> include_rules;
  std::vector<std::string> exclude_rules;
};

// ---- 路径边界 ----
//
// Realtime 的硬边界：**source 与 repository 完全不重叠**。否则"备份写出的
// .bak 又变成下一次触发的事件"，用户会看到一个自己喂养自己的循环。
//
// 判断必须基于 canonical（realpath 之后）的**路径组件**，不是字符串前缀：
// /home/a 与 /home/abc 不是祖孙关系。
enum class RealtimePathOverlap {
  kNone,
  kEqual,
  kRepositoryInsideSource,
  kSourceInsideRepository,
  kUnknown,
};

// 两个路径都不要求存在；不存在时退化为"按组件比较绝对路径"。
RealtimePathOverlap ClassifyPathOverlap(const std::string& source_path,
                                        const std::string& repository_path,
                                        std::string* detail);

// ---- 校验 ----
//
// 结构校验：范围、策略/算法组合（问共享真值表）、字段长度、NUL。
// 不访问文件系统，因此 disabled 状态下允许保存一个暂时不存在的 source。
bool ValidateRealtimeConfig(const RealtimeConfig& config,
                            std::string* error_message);

// enable 前的完整校验：source 存在、是真实目录、root 不是软链接；repository
// 已配置、可解析、不是软链接；两者不重叠。
bool ValidateRealtimeForEnable(const RealtimeConfig& config,
                               const std::string& repository_path,
                               std::string* error_message,
                               std::string* repository_identity);

// ---- 存储 ----
enum class RealtimeLoadStatus { kLoaded, kMissing, kError };

class RealtimeStore {
 public:
  explicit RealtimeStore(std::string file_path);

  // kMissing 表示文件还不存在（等价于"用默认配置"）；kError 表示文件存在但
  // 读不进来或解析失败——调用方必须把它当成错误，不要当成默认值。
  RealtimeLoadStatus Load(RealtimeConfig* config,
                          std::string* error_message) const;

  // 原子保存：temp + fsync + rename。写出去的必须读得回来。
  bool Save(const RealtimeConfig& config, std::string* error_message) const;

  const std::string& file_path() const { return file_path_; }

 private:
  std::string file_path_;
};

// ---- Realtime job identity ----
//
// "这一套 realtime 配置"的稳定摘要：源 / 仓库 / 规则 / 策略 / pack / compression
// / encryption / trigger。marker 记录它，retention 只淘汰与当前 job 相同的快照；
// 换了源或换了算法之后，旧 realtime 快照继续显示为实时快照，但不再被新 job
// 自动淘汰（用户仍可手工删除）。
std::string RealtimeJobIdentityDigest(const RealtimeConfig& config,
                                      const std::string& repository_identity,
                                      const std::string& source_path);

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_REALTIME_STORE_H_
