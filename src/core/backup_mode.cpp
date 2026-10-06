// backup_mode.cpp
//
// Trigger / Strategy 的 key 解析与展示文本。表驱动而不是 if 链：
// 加一个维度取值时只改这一张表，解析与反查不会走散。

// 与 backup_option_keys.cpp 同构：key 进配置（schedule.json 的 trigger /
// strategy 字段），text 只进界面。正查函数与解析函数必须成对维护，缺一个
// 就会出现"写进去的读不出来"。
#include "backup_mode.h"

#include "incremental_backup.h"

namespace backupproject {
namespace {

struct TriggerEntry {
  const char* key;
  BackupTrigger trigger;
  const char* text;
};

// 触发方式：manual（点按钮）/ scheduled（计划器）/ realtime（文件监视）。
// 它只回答"什么时候触发"，不决定"保存什么"。
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

// 策略：full（每次一份完整归档）/ incremental（基线 + delta 链）。
// 它与 pack_method、encryption_method 一起才构成一次备份的完整选项。
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
    // Manual + Incremental：delta 格式、依赖链恢复、baseline/delta/no-change
    // 决策都在共享核心里，CLI 与 GUI 走同一条路径。
    {BackupTrigger::kManual, BackupStrategy::kIncremental, true},
    {BackupTrigger::kScheduled, BackupStrategy::kFull, true},
    // Scheduled + Incremental 成立的前提有两件事，缺一不可：
    //   * 计划路径把增量决策交给共享引擎（内容身份，而不是 metadata-first）；
    //   * retention 变成 dependency-aware，不会为了"删最旧"而删掉某个 delta
    //     的祖先。
    {BackupTrigger::kScheduled, BackupStrategy::kIncremental, true},
    {BackupTrigger::kManual, BackupStrategy::kFull, true},
    // Realtime 是第三个 Trigger。它只决定“什么时候触发”，
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

// 给用户的拒绝理由。注意它把支持矩阵**又写了一遍**（真值表在
// IsSupportedBackupMode 里）：表变了这句话就会撒谎，两者必须一起改。
// 返回值不含上下文，调用方需要时自己加前缀（见组合版本）。
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

// 空串表示"这个触发方式没有无人值守加密的限制"。调用方不得把空串当成可
// 展示的理由，否则界面上会出现一条没有内容的错误。
std::string UnattendedEncryptionDisabledReason(BackupTrigger trigger) {
  if (trigger == BackupTrigger::kScheduled) {
    return std::string(
        "定时无人值守加密需要安全的密钥来源；当前版本不会持久化明文密码。");
  }
  if (trigger == BackupTrigger::kRealtime) {
    return std::string("实时无人值守备份当前不保存密码，因此不启用加密。");
  }
  return std::string();
}

// 组合是否支持的**唯一**判定在 IsSupportedBackupOptionCombination 里，所以
// 这个函数必须按同样的顺序检查同样的谓词：顺序一旦错位，就可能给用户一个
// 与真实拒绝原因不同的解释。返回空串表示"这个组合是支持的"。
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
  if ((combination.trigger == BackupTrigger::kScheduled ||
       combination.trigger == BackupTrigger::kRealtime) &&
      combination.encryption_method != EncryptionMethod::kNone) {
    // 前缀说明"这是哪条边界"，句子本体来自唯一来源。
    const std::string prefix =
        combination.trigger == BackupTrigger::kScheduled
            ? "Unattended scheduled encryption is not supported: "
            : "Unattended realtime encryption is not supported: ";
    return prefix + UnattendedEncryptionDisabledReason(combination.trigger);
  }
  if (combination.strategy == BackupStrategy::kIncremental &&
      !IsSupportedIncrementalEncryption(combination.encryption_method)) {
    return UnsupportedIncrementalEncryptionReason();
  }
  // 走到这里说明组合是支持的；返回空串而不是编一句"不支持"。
  return std::string();
}

}  // namespace backupproject
