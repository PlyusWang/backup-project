// backup_mode.h
//
// 备份的两个正交维度：触发方式（Trigger）与策略（Strategy）。
//
// 老师给出的产品矩阵是 2 x 3 六种组合：
//
//   Trigger:  Manual / Scheduled / Realtime
//   Strategy: Full / Incremental
//
// 本版本真正实现并对外承诺的只有两种：
//
//   Manual    + Full   （PR #15 / #16 已有）
//   Scheduled + Full   （本 PR）
//
// 其余四种组合在本文件里只有 enum 取值，没有任何产品入口会接受它们。
// 这是刻意的：先把维度留出来，避免将来把接口命名写死成
// "FullScheduleOnlyForever"，同时又绝不写空壳假实现——产品入口一律显式拒绝，
// 不做 silent fallback（"选了增量就偷偷按全量跑"是最危险的那种降级）。
//
// 本文件是纯 C++17：不依赖 Qt，CLI / GUI / scheduler core 都可以 include。

#ifndef BACKUP_PROJECT_INCLUDE_BACKUP_MODE_H_
#define BACKUP_PROJECT_INCLUDE_BACKUP_MODE_H_

#include <cstdint>
#include <string>

namespace backupproject {

// 触发方式。数值不写进任何归档格式，只用于持久化文本 key 与内存比较，
// 因此可以扩展。
enum class BackupTrigger : std::uint8_t {
  kManual = 0,
  kScheduled = 1,
  // 已定义、未实现。没有 inotify，没有 watcher 线程，没有任何产品入口。
  kRealtime = 2,
};

// 备份策略。
//
// kIncremental 在这里只是"未来维度"的占位：本 PR 的 change detection 是
// "有变化才生成完整独立快照"，不是增量存储，两者不能混为一谈。
enum class BackupStrategy : std::uint8_t {
  kFull = 0,
  // 已定义、未实现。没有 delta 格式，没有 baseline 依赖链。
  kIncremental = 1,
};

// 稳定文本 key：写进 schedule.json，也被 CLI 接受。
// 与 BackupTriggerKey 拼写固定为 "manual" / "scheduled" / "realtime"。
const char* BackupTriggerKey(BackupTrigger trigger);
const char* BackupStrategyKey(BackupStrategy strategy);

// 展示文本。ASCII，供 CLI 与日志用；GUI 需要本地化文案时自己映射，
// 不在这层塞界面语言。
const char* BackupTriggerText(BackupTrigger trigger);
const char* BackupStrategyText(BackupStrategy strategy);

// 解析失败一律返回 false，绝不回退到默认值：用户/配置文件明确写了
// "incremental"，就必须看到明确报错，而不是拿到一份 full 快照。
bool ParseBackupTriggerKey(const std::string& key, BackupTrigger* trigger);
bool ParseBackupStrategyKey(const std::string& key, BackupStrategy* strategy);

// 本版本的产品支持矩阵。只对当前真正实现了的组合返回 true。
bool IsSupportedBackupMode(BackupTrigger trigger, BackupStrategy strategy);

// 不支持时的完整说明。GUI / CLI 直接显示原文，不各自拼一句话。
std::string UnsupportedBackupModeReason(BackupTrigger trigger,
                                        BackupStrategy strategy);

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_BACKUP_MODE_H_
