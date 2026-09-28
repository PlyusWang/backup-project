// backup_mode.cpp
//
// Trigger / Strategy 的 key 解析与展示文本。表驱动而不是 if 链：
// 加一个维度取值时只改这一张表，解析与反查不会走散。

#include "backup_mode.h"

#include "incremental_backup.h"

namespace backupproject {
namespace {

struct TriggerEntry {
  const char* key;
  BackupTrigger trigger;
  const char* text;
};

const TriggerEntry kTriggerEntries[] = {
    {"manual", BackupTrigger::kManual, "Manual"},
    {"scheduled", BackupTrigger::kScheduled, "Scheduled"},
    {"realtime", BackupTrigger::kRealtime, "Realtime"},
};

struct StrategyEntry {
  const char* key;
  BackupStrategy strategy;
  const char* text;
};

const StrategyEntry kStrategyEntries[] = {
    {"full", BackupStrategy::kFull, "Full"},
    {"incremental", BackupStrategy::kIncremental, "Incremental"},
};

// 产品支持矩阵。写成显式的 3 x 2 真值表，而不是 if 链：
// 每一条组合都必须在这里表态，不会因为漏写一个条件而"默认可用"。
//
// 这张表是**唯一**的答案来源：ValidateScheduleConfig、CLI、GUI 都只问
// IsSupportedBackupMode，任何一处都不许自己再写一遍 trigger / strategy 判断。
struct ModeEntry {
  BackupTrigger trigger;
  BackupStrategy strategy;
  bool supported;
};

const ModeEntry kModeEntries[] = {
    // PR #18：Manual + Incremental 现在是真的了 —— delta 格式、依赖链恢复、
    // baseline/delta/no-change 决策都在共享核心里，CLI 与 GUI 走同一条路径。
    {BackupTrigger::kManual, BackupStrategy::kIncremental, true},
    {BackupTrigger::kScheduled, BackupStrategy::kFull, true},
    // PR #18：Scheduled + Incremental 现在也是真的了。它成立的前提有两件事，
    // 缺一不可，而且都已经落地：
    //   * 计划路径把增量决策交给共享引擎（内容身份，而不是 metadata-first）；
    //   * retention 变成 dependency-aware，不会为了"删最旧"而删掉某个 delta
    //     的祖先。
    {BackupTrigger::kScheduled, BackupStrategy::kIncremental, true},
    {BackupTrigger::kManual, BackupStrategy::kFull, true},
    // PR #19：Realtime 成为第三个 Trigger。它只决定"什么时候触发"，
    // 保存什么仍然完全交给既有 Strategy（Full → BackupEngine，
    // Incremental → RunIncrementalBackup）。
    {BackupTrigger::kRealtime, BackupStrategy::kFull, true},
    {BackupTrigger::kRealtime, BackupStrategy::kIncremental, true},
};

const char* kUnknown = "unknown";

}  // namespace

const char* BackupTriggerKey(BackupTrigger trigger) {
  for (const TriggerEntry& entry : kTriggerEntries) {
    if (entry.trigger == trigger) return entry.key;
  }
  return kUnknown;
}

const char* BackupStrategyKey(BackupStrategy strategy) {
  for (const StrategyEntry& entry : kStrategyEntries) {
    if (entry.strategy == strategy) return entry.key;
  }
  return kUnknown;
}

const char* BackupTriggerText(BackupTrigger trigger) {
  for (const TriggerEntry& entry : kTriggerEntries) {
    if (entry.trigger == trigger) return entry.text;
  }
  return kUnknown;
}

const char* BackupStrategyText(BackupStrategy strategy) {
  for (const StrategyEntry& entry : kStrategyEntries) {
    if (entry.strategy == strategy) return entry.text;
  }
  return kUnknown;
}

bool ParseBackupTriggerKey(const std::string& key, BackupTrigger* trigger) {
  if (trigger == nullptr) return false;
  for (const TriggerEntry& entry : kTriggerEntries) {
    if (key == entry.key) {
      *trigger = entry.trigger;
      return true;
    }
  }
  return false;
}

bool ParseBackupStrategyKey(const std::string& key, BackupStrategy* strategy) {
  if (strategy == nullptr) return false;
  for (const StrategyEntry& entry : kStrategyEntries) {
    if (key == entry.key) {
      *strategy = entry.strategy;
      return true;
    }
  }
  return false;
}

bool IsSupportedBackupMode(BackupTrigger trigger, BackupStrategy strategy) {
  // 必须**同时**匹配 trigger 与 strategy。只判断 trigger 会把
  // Manual + Incremental 误判成 supported——那正是"选了增量却按全量跑"
  // 这类静默降级的入口。
  for (const ModeEntry& entry : kModeEntries) {
    if (entry.trigger == trigger && entry.strategy == strategy) {
      return entry.supported;
    }
  }
  // 表里没有的组合（将来新增的枚举取值）一律视为不支持：fail closed。
  return false;
}

std::string UnsupportedBackupModeReason(BackupTrigger trigger,
                                        BackupStrategy strategy) {
  return std::string("Unsupported backup mode: ") + BackupTriggerText(trigger) +
         " + " + BackupStrategyText(strategy) +
         ". This version implements Manual + Full, Manual + Incremental, "
         "Scheduled + Full, Scheduled + Incremental, Realtime + Full and "
         "Realtime + Incremental.";
}

bool IsSupportedBackupOptionCombination(
    const BackupOptionCombination& combination) {
  if (!IsSupportedBackupMode(combination.trigger, combination.strategy)) {
    return false;
  }
  if (combination.strategy == BackupStrategy::kIncremental &&
      !IsSupportedIncrementalPack(combination.pack_method)) {
    return false;
  }
  // 计划路径从来没有"口令"这个东西：无人值守的加密需要安全的密钥来源，
  // 本版本一律拒绝（与 ValidateScheduleConfig 逐字一致的那句话）。
  if ((combination.trigger == BackupTrigger::kScheduled ||
       combination.trigger == BackupTrigger::kRealtime) &&
      combination.encryption_method != EncryptionMethod::kNone) {
    return false;
  }
  if (combination.strategy == BackupStrategy::kIncremental &&
      !IsSupportedIncrementalEncryption(combination.encryption_method)) {
    return false;
  }
  return true;
}

std::string UnsupportedBackupOptionCombinationReason(
    const BackupOptionCombination& combination) {
  if (!IsSupportedBackupMode(combination.trigger, combination.strategy)) {
    return UnsupportedBackupModeReason(combination.trigger,
                                       combination.strategy);
  }
  if (combination.strategy == BackupStrategy::kIncremental &&
      !IsSupportedIncrementalPack(combination.pack_method)) {
    return UnsupportedIncrementalPackReason();
  }
  if (combination.trigger == BackupTrigger::kScheduled &&
      combination.encryption_method != EncryptionMethod::kNone) {
    return std::string(
        "Unattended scheduled encryption is not supported: "
        "定时无人值守加密需要安全的密钥来源；当前版本不会持久化明文密码。");
  }
  if (combination.trigger == BackupTrigger::kRealtime &&
      combination.encryption_method != EncryptionMethod::kNone) {
    return std::string(
        "Unattended realtime encryption is not supported: "
        "实时无人值守备份当前不保存密码，因此不启用加密。");
  }
  if (combination.strategy == BackupStrategy::kIncremental &&
      !IsSupportedIncrementalEncryption(combination.encryption_method)) {
    return UnsupportedIncrementalEncryptionReason();
  }
  // 走到这里说明组合是支持的；返回空串而不是编一句"不支持"。
  return std::string();
}

}  // namespace backupproject
