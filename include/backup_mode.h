// backup_mode.h
//
// 备份的两个正交维度：触发方式（Trigger）与策略（Strategy）。
//
// 老师给出的产品矩阵是 2 x 3 六种组合：
//
//   Trigger:  Manual / Scheduled / Realtime
//   Strategy: Full / Incremental
//
// 本版本真正实现并对外承诺的三种：
//
//   Manual    + Full           （PR #15 / #16）
//   Scheduled + Full           （PR #17）
//   Manual    + Incremental    （PR #18：delta 格式 + 依赖链恢复 + 共享引擎）
//
// Scheduled + Incremental 仍然被拒绝，理由写在真值表旁边：计划路径的 retention
// 还是"按时间删最旧"，用在依赖链上会删掉祖先、让后代全部不可恢复。Realtime
// 只在 enum 里存在，没有任何产品入口。
//
// 产品入口一律显式拒绝不支持的组合，不做 silent fallback
// （"选了增量就偷偷按全量跑"是最危险的那种降级）。
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
// kFull：每次都写出一份完整、自包含的归档。
// kIncremental：第一次（或基线不可信时）写完整基线，之后写 delta，
//   没有任何有效变化时什么都不写。恢复靠依赖链自动解析。
enum class BackupStrategy : std::uint8_t {
  kFull = 0,
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
