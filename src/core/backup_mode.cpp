// backup_mode.cpp
//
// Trigger / Strategy 的 key 解析与展示文本。表驱动而不是 if 链：
// 加一个维度取值时只改这一张表，解析与反查不会走散。

#include "backup_mode.h"

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
  return trigger == BackupTrigger::kManual ||
         (trigger == BackupTrigger::kScheduled &&
          strategy == BackupStrategy::kFull);
}

std::string UnsupportedBackupModeReason(BackupTrigger trigger,
                                        BackupStrategy strategy) {
  return std::string("Unsupported backup mode: ") + BackupTriggerText(trigger) +
         " + " + BackupStrategyText(strategy) +
         ". This version implements only Manual + Full and Scheduled + Full.";
}

}  // namespace backupproject
