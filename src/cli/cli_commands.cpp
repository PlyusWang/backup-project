// cli_commands.cpp
//
// backupctl 各子命令的实现。
//
// 这一层的职责被刻意压到最小：解析参数、调用共享核心、把结果翻译成人能读的
// 输出与退出码。它不实现筛选语法、不拼归档路径、不算 next run、不做 retention，
// 也不读密码以外的任何输入。

#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>
#include <vector>

#include "archive_pipeline.h"
#include "backup_catalog.h"
#include "backup_engine.h"
#include "backup_option_keys.h"
#include "cli_app.h"
#include "config_manager.h"
#include "filter.h"
#include "schedule_store.h"
#include "scheduled_backup_service.h"
#include "scheduler_lock.h"
#include "terminal_secret.h"

namespace backupproject {
namespace {

void PrintError(const std::string& text) {
  std::cerr << "Error: " << text << '\n';
}

int UsageError(const CliContext& context, const std::string& text) {
  PrintError(text);
  std::cerr << '\n';
  PrintCliUsage(context.program_name, std::cerr);
  return kCliExitUsageError;
}

// 取一个选项的值。缺少值一律是用法错误，绝不退化成默认值。
bool TakeValue(const std::vector<std::string>& arguments, std::size_t* index,
               const std::string& option, std::string* value,
               std::string* error_message) {
  if (*index + 1 >= arguments.size()) {
    *error_message = option + " needs a value";
    return false;
  }
  *index += 1;
  *value = arguments[*index];
  return true;
}

bool ParseBoundedUint32(const std::string& text, std::uint32_t minimum,
                        std::uint32_t maximum, const std::string& option,
                        std::uint32_t* value, std::string* error_message) {
  if (text.empty()) {
    *error_message = option + " needs a number";
    return false;
  }
  std::uint64_t result = 0;
  for (const char character : text) {
    if (character < '0' || character > '9') {
      *error_message =
          option + " expects a plain non-negative integer, got '" + text + "'";
      return false;
    }
    result = result * 10u + static_cast<std::uint64_t>(character - '0');
    if (result > maximum) {
      *error_message = option + " is out of range: " + text + " (expected " +
                       std::to_string(minimum) + ".." +
                       std::to_string(maximum) + ")";
      return false;
    }
  }
  if (result < minimum) {
    *error_message = option + " is out of range: " + text + " (expected " +
                     std::to_string(minimum) + ".." + std::to_string(maximum) +
                     ")";
    return false;
  }
  *value = static_cast<std::uint32_t>(result);
  return true;
}

std::string FormatLocalTime(std::int64_t seconds) {
  if (seconds <= 0) return std::string("never");
  const std::time_t value = static_cast<std::time_t>(seconds);
  struct tm broken_down;
  if (::localtime_r(&value, &broken_down) == nullptr) return std::string("?");
  char buffer[64];
  if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S",
                    &broken_down) == 0) {
    return std::string("?");
  }
  return std::string(buffer);
}

std::string FormatSize(std::uint64_t bytes) {
  return std::to_string(bytes) + " B";
}

std::string JoinRules(const std::vector<std::string>& rules) {
  if (rules.empty()) return std::string("(none)");
  std::string joined;
  for (const std::string& rule : rules) {
    if (!joined.empty()) joined += ", ";
    joined += rule;
  }
  return joined;
}

// ---- 共享核心的薄封装 ----

bool LoadRepositoryPath(const CliContext& context, std::string* repository,
                        std::string* error_message) {
  repository->clear();
  if (context.config_file_path.empty()) {
    *error_message =
        "Cannot locate the application config file: neither XDG_CONFIG_HOME "
        "nor HOME is usable";
    return false;
  }
  ConfigManager manager(context.config_file_path);
  AppConfig config;
  std::string load_error;
  const ConfigLoadStatus status = manager.Load(&config, &load_error);
  if (status == ConfigLoadStatus::kError) {
    *error_message = load_error;
    return false;
  }
  if (status == ConfigLoadStatus::kMissing ||
      config.backup_repository_path.empty()) {
    *error_message =
        "No backup repository is configured. Run: " + context.program_name +
        " config repository set <path>";
    return false;
  }
  *repository = config.backup_repository_path;
  return true;
}

bool LoadScheduleDocument(const CliContext& context, ScheduleDocument* document,
                          ScheduleLoadStatus* status,
                          std::string* error_message) {
  if (context.schedule_file_path.empty()) {
    *error_message =
        "Cannot locate the schedule store: neither XDG_CONFIG_HOME nor HOME is "
        "usable";
    return false;
  }
  ScheduleStore store(context.schedule_file_path);
  const ScheduleLoadStatus loaded = store.Load(document, error_message);
  if (status != nullptr) *status = loaded;
  if (loaded == ScheduleLoadStatus::kError) return false;
  if (loaded == ScheduleLoadStatus::kMissing) {
    // 没配过计划不是错误：给一份默认（disabled）文档，界面与 CLI
    // 显示同一套默认值。
    *document = ScheduleDocument{};
  }
  return true;
}

bool SaveScheduleDocument(const CliContext& context,
                          const ScheduleDocument& document,
                          std::string* error_message) {
  ScheduleStore store(context.schedule_file_path);
  return store.Save(document, error_message);
}

const char* ConfigLoadStatusText(ConfigLoadStatus status) {
  switch (status) {
    case ConfigLoadStatus::kLoaded:
      return "loaded";
    case ConfigLoadStatus::kMissing:
      return "missing";
    case ConfigLoadStatus::kError:
      return "error";
  }
  return "error";
}

}  // namespace

// ---- 用法 ----

void PrintCliUsage(const std::string& program_name, std::ostream& output) {
  output
      << "Usage:\n"
      << "  " << program_name
      << " backup <source_directory> <backup_file> [--include <rule>]... "
         "[--exclude <rule>]...\n"
      << "  " << program_name
      << " restore <backup_file> <destination_directory>\n"
      << "  " << program_name
      << " schedule show | set | enable | disable | run | history | watch\n"
      << "    schedule run evaluates right now, ignoring the due time; a run "
         "with no changes is still skipped, so it never becomes a forced "
         "backup.\n"
      << "  " << program_name << " repository list | delete <file_name>\n"
      << "  " << program_name << " config repository show | set <path>\n"
      << "\n"
      << "Backup pipeline options (any of these switches the command to the "
         "v2\n"
      << "container; without them the legacy v0.1 archive format is used):\n"
      << "  --pack mypack|ustar|fast-ustar\n"
      << "  --compression none|huffman|lzss-huffman\n"
      << "  --encryption none|aes-256-ctr-hmac-sha256|des-cbc-hmac-sha256\n"
      << "    DES-CBC is educational / legacy only. A password is read "
         "interactively\n"
      << "    from /dev/tty; it is never taken from an argument, an "
         "environment\n"
      << "    variable or piped input.\n"
      << "\n"
      << "Global options:\n"
      << "  --config-file <path>    Override the application config.json.\n"
      << "  --schedule-file <path>  Override the schedule store.\n"
      << "\n"
      << "Filter rules (see docs/filter_usage.md):\n"
      << "  name: / path: / stem: / ext: / type:file|folder / size: / mtime:\n"
      << "  wildcards: * (no '/'), ? (one char, no '/'), ** (may cross '/')\n"
      << "  examples: --include 'ext:cpp;h' --exclude 'path:**/build/**'\n"
      << "\n"
      << "Options:\n"
      << "  --help, -h  Show this help message.\n";
}

// ---- backup ----

int RunBackupCommand(const CliContext& context,
                     const std::vector<std::string>& arguments) {
  if (arguments.size() < 2) {
    std::cerr << "Error: 'backup' expects <source_directory> and "
                 "<backup_file>.\n\n";
    PrintCliUsage(context.program_name, std::cerr);
    return kCliExitUsageError;
  }
  const std::string source_directory = arguments[0];
  const std::string backup_file = arguments[1];

  Filter filter;
  BackupOptions options;
  bool has_pipeline_option = false;
  bool encryption_requested = false;
  std::string error_message;

  for (std::size_t index = 2; index < arguments.size(); ++index) {
    const std::string option = arguments[index];
    std::string value;
    if (option == "--include" || option == "--exclude") {
      if (!TakeValue(arguments, &index, option, &value, &error_message)) {
        return UsageError(context, error_message);
      }
      const FilterAction action = option == "--include"
                                      ? FilterAction::kInclude
                                      : FilterAction::kExclude;
      if (!filter.AddRule(action, value, &error_message)) {
        return UsageError(context, error_message);
      }
      continue;
    }
    if (option == "--pack") {
      if (!TakeValue(arguments, &index, option, &value, &error_message)) {
        return UsageError(context, error_message);
      }
      if (!ParsePackMethodKey(value, &options.pack_method)) {
        return UsageError(context, "unknown pack method '" + value +
                                       "' (expected mypack, ustar or "
                                       "fast-ustar)");
      }
      has_pipeline_option = true;
      continue;
    }
    if (option == "--compression") {
      if (!TakeValue(arguments, &index, option, &value, &error_message)) {
        return UsageError(context, error_message);
      }
      if (!ParseCompressionMethodKey(value, &options.compression_method)) {
        return UsageError(context, "unknown compression method '" + value +
                                       "' (expected none, huffman or "
                                       "lzss-huffman)");
      }
      has_pipeline_option = true;
      continue;
    }
    if (option == "--encryption") {
      if (!TakeValue(arguments, &index, option, &value, &error_message)) {
        return UsageError(context, error_message);
      }
      if (!ParseEncryptionMethodKey(value, &options.encryption_method)) {
        return UsageError(context, "unknown encryption method '" + value +
                                       "' (expected none, "
                                       "aes-256-ctr-hmac-sha256 or "
                                       "des-cbc-hmac-sha256)");
      }
      has_pipeline_option = true;
      encryption_requested =
          options.encryption_method != EncryptionMethod::kNone;
      continue;
    }
    return UsageError(context, "unknown option '" + option + "'");
  }

  if (encryption_requested) {
    // 密码只在本进程内存里活着。两次不一致、不是交互终端、用户 Ctrl+C ——
    // 全部在这里明确失败，而不是"空密码也能加密"。
    std::string secret;
    if (!ReadSecretFromTerminalTwice("Backup password: ", "Confirm password: ",
                                     &secret, &error_message)) {
      PrintError(error_message);
      return kCliExitOperationFailed;
    }
    options.password = secret;
    // 用完之后立刻抹掉这一份副本；options.password 会在离开作用域时销毁。
    for (char& character : secret) character = '\0';
  }

  BackupEngine engine;
  const bool ok = has_pipeline_option
                      ? engine.Backup(source_directory, backup_file, filter,
                                      options, &error_message)
                      : engine.Backup(source_directory, backup_file, filter,
                                      &error_message);
  if (!ok) {
    PrintError(error_message);
    return kCliExitOperationFailed;
  }
  std::cout << "Backup completed successfully.\n";
  if (has_pipeline_option) {
    std::cout << "Pipeline: pack=" << PackMethodKey(options.pack_method)
              << " compression="
              << CompressionMethodKey(options.compression_method)
              << " encryption="
              << EncryptionMethodKey(options.encryption_method) << '\n';
  } else {
    std::cout
        << "Archive format: legacy v0.1 (no pipeline options were given)\n";
  }
  return kCliExitSuccess;
}

// ---- restore ----

int RunRestoreCommand(const CliContext& context,
                      const std::vector<std::string>& arguments) {
  if (arguments.size() != 2) {
    std::cerr << "Error: 'restore' expects <backup_file> and "
                 "<destination_directory>.\n\n";
    PrintCliUsage(context.program_name, std::cerr);
    return kCliExitUsageError;
  }
  const std::string backup_file = arguments[0];
  const std::string destination_directory = arguments[1];

  BackupEngine engine;
  std::string error_message;

  // 只有归档自己说"我需要密码"时才去问。问的方式还是 /dev/tty；
  // 识别失败时不在这里报错，交给真正的恢复路径给出更准确的诊断
  // （文件不存在、magic 不对、被截断……）。
  ArchiveFileInfo info;
  std::string identify_error;
  if (IdentifyArchiveFile(backup_file, &info, &identify_error) &&
      !info.password_hint.empty()) {
    std::string secret;
    if (!ReadSecretFromTerminal("Restore password: ", &secret,
                                &error_message)) {
      PrintError(error_message);
      return kCliExitOperationFailed;
    }
    RestoreOptions options;
    options.password = secret;
    for (char& character : secret) character = '\0';
    if (!engine.Restore(backup_file, destination_directory, options, nullptr,
                        &error_message)) {
      PrintError(error_message);
      return kCliExitOperationFailed;
    }
  } else if (!engine.Restore(backup_file, destination_directory,
                             &error_message)) {
    PrintError(error_message);
    return kCliExitOperationFailed;
  }
  std::cout << "Restore completed successfully.\n";
  return kCliExitSuccess;
}

// ---- schedule ----

namespace {

std::string ChangeSummaryText(const ChangeSummary& changes) {
  return "+" + std::to_string(changes.added) + " added, -" +
         std::to_string(changes.removed) + " removed, ~" +
         std::to_string(changes.modified) + " modified, " +
         std::to_string(changes.metadata_changed) + " metadata";
}

void PrintEvaluation(const ScheduleEvaluationResult& result,
                     std::int64_t now_sec) {
  std::cout << "Scheduled evaluation: "
            << ScheduleEvaluationStatusText(result.status) << "\n";
  if (result.status == ScheduleEvaluationStatus::kCreatedSnapshot ||
      result.status == ScheduleEvaluationStatus::kCreatedWithRetentionWarning ||
      result.status == ScheduleEvaluationStatus::kSkippedNoChanges) {
    std::cout << "  changes:   " << ChangeSummaryText(result.changes) << "\n";
  }
  if (!result.archive_file_name.empty()) {
    std::cout << "  archive:   " << result.archive_file_name << "\n";
  }
  if (result.first_snapshot) {
    // 首次快照与"相对上一版的变化"是两件事，输出里必须分得开。
    std::cout
        << "  note:      first snapshot (there was no previous manifest)\n";
  }
  if (result.retention_deleted > 0 || result.retention_failed > 0) {
    std::cout << "  retention: removed " << result.retention_deleted
              << ", failed " << result.retention_failed << "\n";
  }
  if (result.next_run_time_sec > 0) {
    std::cout << "  next run:  " << FormatLocalTime(result.next_run_time_sec)
              << " (at " << result.next_run_time_sec << ")\n";
  }
  if (!result.diagnostic.empty()) {
    std::cout << "  diagnostic: " << result.diagnostic << "\n";
  }
  (void)now_sec;
}

int ScheduleShow(const CliContext& context) {
  ScheduleDocument document;
  ScheduleLoadStatus status = ScheduleLoadStatus::kMissing;
  std::string error;
  if (!LoadScheduleDocument(context, &document, &status, &error)) {
    PrintError(error);
    return kCliExitOperationFailed;
  }
  const ScheduleConfig& config = document.config;
  const ScheduleState& state = document.state;

  std::cout << "Schedule store: " << context.schedule_file_path << "\n";
  std::cout << "Store status:   "
            << (status == ScheduleLoadStatus::kMissing ? "not created yet"
                                                       : "loaded")
            << "\n";
  std::cout << "Enabled:        " << (config.enabled ? "yes" : "no") << "\n";
  std::cout << "Trigger:        " << BackupTriggerKey(config.trigger) << "\n";
  std::cout << "Strategy:       " << BackupStrategyKey(config.strategy) << "\n";
  std::cout << "Source:         "
            << (config.source_path.empty() ? "(not set)" : config.source_path)
            << "\n";
  std::cout << "Interval:       " << config.interval_minutes << " minute(s)\n";
  std::cout << "Retain:         " << config.retain_count
            << " scheduled snapshot(s)\n";
  std::cout << "Pack:           " << PackMethodKey(config.pack_method) << "\n";
  std::cout << "Compression:    "
            << CompressionMethodKey(config.compression_method) << "\n";
  std::cout << "Encryption:     "
            << EncryptionMethodKey(config.encryption_method) << "\n";
  std::cout << "Include rules:  " << JoinRules(config.include_rules) << "\n";
  std::cout << "Exclude rules:  " << JoinRules(config.exclude_rules) << "\n";
  std::cout << "Last success:   "
            << FormatLocalTime(state.last_success_time_sec) << "\n";
  std::cout << "Next run:       " << FormatLocalTime(state.next_run_time_sec)
            << "\n";
  std::cout << "Managed:        " << state.managed_snapshots.size()
            << " scheduled snapshot(s)\n";
  for (const ScheduledSnapshotRecord& record : state.managed_snapshots) {
    std::cout << "  - " << record.file_name
              << "  created=" << FormatLocalTime(record.created_time_sec)
              << "  bytes=" << record.archive_size
              << "  entries=" << record.entry_count
              << "  pack=" << PackMethodKey(record.pack_method)
              << "  compression="
              << CompressionMethodKey(record.compression_method)
              << "  changes: " << ChangeSummaryText(record.changes) << "\n";
  }
  if (!state.history.empty()) {
    const ScheduleHistoryEntry& last = state.history.back();
    std::cout << "Last run:       " << FormatLocalTime(last.finished_at_sec)
              << "  result=" << ScheduleRunResultKey(last.result);
    if (!last.archive_file_name.empty()) {
      std::cout << "  archive=" << last.archive_file_name;
    }
    std::cout << "\n";
    if (!last.diagnostic.empty()) {
      std::cout << "Last diagnostic: " << last.diagnostic << "\n";
    }
  }
  return kCliExitSuccess;
}

int ScheduleSet(const CliContext& context,
                const std::vector<std::string>& arguments) {
  if (arguments.empty()) {
    return UsageError(context,
                      "'schedule set' expects at least one option, for example "
                      "--source <dir> --interval-minutes 60 --retain 12");
  }

  ScheduleDocument document;
  ScheduleLoadStatus status = ScheduleLoadStatus::kMissing;
  std::string error;
  if (!LoadScheduleDocument(context, &document, &status, &error)) {
    PrintError(error);
    return kCliExitOperationFailed;
  }
  ScheduleConfig config = document.config;

  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const std::string option = arguments[index];
    std::string value;
    if (option == "--source") {
      if (!TakeValue(arguments, &index, option, &value, &error)) {
        return UsageError(context, error);
      }
      config.source_path = value;
      continue;
    }
    if (option == "--interval-minutes") {
      if (!TakeValue(arguments, &index, option, &value, &error)) {
        return UsageError(context, error);
      }
      if (!ParseBoundedUint32(value, kMinIntervalMinutes, kMaxIntervalMinutes,
                              option, &config.interval_minutes, &error)) {
        return UsageError(context, error);
      }
      continue;
    }
    if (option == "--retain") {
      if (!TakeValue(arguments, &index, option, &value, &error)) {
        return UsageError(context, error);
      }
      if (!ParseBoundedUint32(value, kMinRetainCount, kMaxRetainCount, option,
                              &config.retain_count, &error)) {
        return UsageError(context, error);
      }
      continue;
    }
    if (option == "--pack") {
      if (!TakeValue(arguments, &index, option, &value, &error)) {
        return UsageError(context, error);
      }
      if (!ParsePackMethodKey(value, &config.pack_method)) {
        return UsageError(context, "unknown pack method '" + value + "'");
      }
      continue;
    }
    if (option == "--compression") {
      if (!TakeValue(arguments, &index, option, &value, &error)) {
        return UsageError(context, error);
      }
      if (!ParseCompressionMethodKey(value, &config.compression_method)) {
        return UsageError(context,
                          "unknown compression method '" + value + "'");
      }
      continue;
    }
    if (option == "--encryption") {
      if (!TakeValue(arguments, &index, option, &value, &error)) {
        return UsageError(context, error);
      }
      EncryptionMethod method = EncryptionMethod::kNone;
      if (!ParseEncryptionMethodKey(value, &method)) {
        return UsageError(context, "unknown encryption method '" + value + "'");
      }
      if (method != EncryptionMethod::kNone) {
        // 无人值守的定时任务没有安全的持久密钥来源。明确拒绝，绝不落盘明文密码，
        // 也绝不静默降级成不加密。
        return UsageError(
            context,
            "the scheduled backup supports --encryption none only: "
            "定时无人值守加密需要安全的密钥来源；当前版本不会持久化明文密码。");
      }
      config.encryption_method = method;
      continue;
    }
    if (option == "--include") {
      if (!TakeValue(arguments, &index, option, &value, &error)) {
        return UsageError(context, error);
      }
      if (config.include_rules.size() + config.exclude_rules.size() >=
          kMaxScheduleRules) {
        return UsageError(context, "too many filter rules (limit " +
                                       std::to_string(kMaxScheduleRules) + ")");
      }
      config.include_rules.push_back(value);
      continue;
    }
    if (option == "--exclude") {
      if (!TakeValue(arguments, &index, option, &value, &error)) {
        return UsageError(context, error);
      }
      if (config.include_rules.size() + config.exclude_rules.size() >=
          kMaxScheduleRules) {
        return UsageError(context, "too many filter rules (limit " +
                                       std::to_string(kMaxScheduleRules) + ")");
      }
      config.exclude_rules.push_back(value);
      continue;
    }
    return UsageError(context, "unknown option '" + option + "'");
  }

  // 规则语法在这里就用真实的 Filter 校验一遍：配置里存的规则与命令行的规则
  // 走的是同一套解析，写不进去的规则也读不出来。
  if (!ValidateScheduleConfig(config, &error)) {
    return UsageError(context, error);
  }

  document.config = config;
  if (!SaveScheduleDocument(context, document, &error)) {
    PrintError(error);
    return kCliExitOperationFailed;
  }
  std::cout << "Schedule updated. Enabled: " << (config.enabled ? "yes" : "no")
            << "\n";
  return kCliExitSuccess;
}

int ScheduleToggle(const CliContext& context, bool enabled) {
  ScheduleDocument document;
  ScheduleLoadStatus status = ScheduleLoadStatus::kMissing;
  std::string error;
  if (!LoadScheduleDocument(context, &document, &status, &error)) {
    PrintError(error);
    return kCliExitOperationFailed;
  }
  document.config.enabled = enabled;
  if (enabled && !ValidateScheduleForEnable(document.config, &error)) {
    // enable 之前必须完整校验：源目录不存在、没配源、规则非法，一律不许启用。
    PrintError(error);
    return kCliExitOperationFailed;
  }
  if (!SaveScheduleDocument(context, document, &error)) {
    PrintError(error);
    return kCliExitOperationFailed;
  }
  std::cout << "Scheduled backup " << (enabled ? "enabled" : "disabled")
            << ".\n";
  if (enabled) {
    std::cout
        << "It runs only while this program or 'backupctl schedule watch' "
           "is running.\n";
  }
  return kCliExitSuccess;
}

int ScheduleRun(const CliContext& context) {
  std::string repository;
  std::string error;
  if (!LoadRepositoryPath(context, &repository, &error)) {
    PrintError(error);
    return kCliExitOperationFailed;
  }
  if (context.schedule_file_path.empty()) {
    PrintError("Cannot locate the schedule store");
    return kCliExitOperationFailed;
  }

  ScheduleStore store(context.schedule_file_path);
  SchedulerLock lock;
  if (!lock.Acquire(store.lock_file_path(), &error)) {
    PrintError(error);
    return kCliExitOperationFailed;
  }

  ScheduledBackupService service(repository, &store);
  ScheduleEvaluationResult result;
  const std::int64_t now = static_cast<std::int64_t>(::time(nullptr));
  // "立即检查并运行"：跳过"还没到点"，但仍然做真实的变化检测。
  if (!service.EvaluateNow(now, &result, &error)) {
    PrintError(error);
    return kCliExitOperationFailed;
  }
  PrintEvaluation(result, now);
  return result.status == ScheduleEvaluationStatus::kFailed
             ? kCliExitOperationFailed
             : kCliExitSuccess;
}

int ScheduleHistory(const CliContext& context) {
  ScheduleDocument document;
  ScheduleLoadStatus status = ScheduleLoadStatus::kMissing;
  std::string error;
  if (!LoadScheduleDocument(context, &document, &status, &error)) {
    PrintError(error);
    return kCliExitOperationFailed;
  }
  if (document.state.history.empty()) {
    std::cout << "No scheduled run has been recorded yet.\n";
    return kCliExitSuccess;
  }
  std::cout << "FINISHED             RESULT                           +ADD "
               "-DEL ~MOD *META  ARCHIVE\n";
  for (const ScheduleHistoryEntry& entry : document.state.history) {
    std::cout << FormatLocalTime(entry.finished_at_sec) << "  "
              << ScheduleRunResultKey(entry.result);
    const std::string result_key = ScheduleRunResultKey(entry.result);
    for (std::size_t pad = result_key.size(); pad < 32; ++pad) std::cout << ' ';
    std::cout << " " << entry.changes.added << "    " << entry.changes.removed
              << "    " << entry.changes.modified << "     "
              << entry.changes.metadata_changed << "      "
              << (entry.archive_file_name.empty() ? "-"
                                                  : entry.archive_file_name);
    if (!entry.diagnostic.empty()) {
      std::cout << "  [" << entry.diagnostic << "]";
    }
    std::cout << "\n";
  }
  return kCliExitSuccess;
}

volatile sig_atomic_t g_watch_stop = 0;

void HandleWatchStop(int) { g_watch_stop = 1; }

int ScheduleWatch(const CliContext& context) {
  std::string repository;
  std::string error;
  if (!LoadRepositoryPath(context, &repository, &error)) {
    PrintError(error);
    return kCliExitOperationFailed;
  }

  ScheduleStore store(context.schedule_file_path);
  SchedulerLock lock;
  if (!lock.Acquire(store.lock_file_path(), &error)) {
    // "另一个进程正在跑"是正常状态，不是崩溃。说清楚，然后退出。
    PrintError(error);
    return kCliExitOperationFailed;
  }

  struct sigaction action;
  std::memset(&action, 0, sizeof(action));
  action.sa_handler = HandleWatchStop;
  sigemptyset(&action.sa_mask);
  sigaction(SIGINT, &action, nullptr);
  sigaction(SIGTERM, &action, nullptr);

  std::cout << "Watching the scheduled backup. Press Ctrl+C to stop.\n";
  std::cout.flush();

  ScheduledBackupService service(repository, &store);
  while (g_watch_stop == 0) {
    ScheduleEvaluationResult result;
    const std::int64_t now = static_cast<std::int64_t>(::time(nullptr));
    if (!service.Evaluate(now, &result, &error)) {
      PrintError(error);
      return kCliExitOperationFailed;
    }
    if (result.status != ScheduleEvaluationStatus::kNotDue) {
      PrintEvaluation(result, now);
      std::cout.flush();
    }

    // 睡到下一个时间点，但一次最多睡 30 秒：这样 Ctrl+C 最多 30 秒内响应，
    // 而且始终是"到点才算"，不是轮询着乱跑。
    std::int64_t wait_seconds = 30;
    if (result.status == ScheduleEvaluationStatus::kNotDue &&
        result.next_run_time_sec > now) {
      wait_seconds = result.next_run_time_sec - now;
    }
    if (wait_seconds < 1) wait_seconds = 1;
    if (wait_seconds > 30) wait_seconds = 30;
    ::sleep(static_cast<unsigned>(wait_seconds));
  }

  lock.Release();
  std::cout << "\nStopped watching the scheduled backup.\n";
  return kCliExitSuccess;
}

}  // namespace

int RunScheduleCommand(const CliContext& context,
                       const std::vector<std::string>& arguments) {
  if (arguments.empty()) {
    return UsageError(context,
                      "'schedule' expects a subcommand: show, set, enable, "
                      "disable, run, history or watch");
  }
  const std::string subcommand = arguments[0];
  const std::vector<std::string> rest(arguments.begin() + 1, arguments.end());

  if (subcommand == "show") return ScheduleShow(context);
  if (subcommand == "set") return ScheduleSet(context, rest);
  if (subcommand == "enable") return ScheduleToggle(context, true);
  if (subcommand == "disable") return ScheduleToggle(context, false);
  if (subcommand == "run") return ScheduleRun(context);
  if (subcommand == "history") return ScheduleHistory(context);
  if (subcommand == "watch") return ScheduleWatch(context);
  return UsageError(context,
                    "unknown schedule subcommand '" + subcommand + "'");
}

// ---- repository ----

namespace {

std::string KindText(const BackupRecord& record) {
  if (!record.recognized_archive) return "unreadable";
  return record.format_version == 2 ? "container-v2" : "legacy-v0.1";
}

std::string PipelineText(const BackupRecord& record) {
  if (!record.has_pipeline_methods) return "-";
  return std::string(PackMethodKey(record.pack_method)) + "/" +
         CompressionMethodKey(record.compression_method) + "/" +
         EncryptionMethodKey(record.encryption_method);
}

int RepositoryList(const CliContext& context) {
  std::string repository;
  std::string error;
  if (!LoadRepositoryPath(context, &repository, &error)) {
    PrintError(error);
    return kCliExitOperationFailed;
  }
  BackupCatalog catalog;
  std::vector<BackupRecord> records;
  if (!catalog.List(repository, &records, &error)) {
    PrintError(error);
    return kCliExitOperationFailed;
  }

  // origin 来自 ScheduleStore，而不是文件名解析：文件名永远不是 ownership 的
  // 真相来源。schedule 状态丢失时只退化成 manual，不影响归档本身。
  ScheduleDocument document;
  ScheduleLoadStatus status = ScheduleLoadStatus::kMissing;
  std::string schedule_error;
  LoadScheduleDocument(context, &document, &status, &schedule_error);

  std::cout << "Repository: " << repository << "\n";
  std::cout << "Archives:   " << records.size() << "\n";
  for (const BackupRecord& record : records) {
    bool managed = false;
    for (const ScheduledSnapshotRecord& snapshot :
         document.state.managed_snapshots) {
      if (snapshot.file_name == record.file_name) {
        managed = true;
        break;
      }
    }
    std::cout << "  " << record.file_name << "  "
              << FormatSize(record.archive_size) << "  "
              << FormatLocalTime(record.modified_time_sec) << "  "
              << KindText(record) << "  " << PipelineText(record)
              << "  origin=" << (managed ? "scheduled" : "manual");
    if (record.password_required) std::cout << "  password-required";
    if (!record.recognized_archive && !record.diagnostic.empty()) {
      std::cout << "  [" << record.diagnostic << "]";
    }
    std::cout << "\n";
  }
  return kCliExitSuccess;
}

int RepositoryDelete(const CliContext& context, const std::string& file_name) {
  std::string repository;
  std::string error;
  if (!LoadRepositoryPath(context, &repository, &error)) {
    PrintError(error);
    return kCliExitOperationFailed;
  }
  BackupCatalog catalog;
  if (!catalog.Delete(repository, file_name, &error)) {
    PrintError(error);
    return kCliExitOperationFailed;
  }

  // §19 的 A 路径：删除成功后立刻让 ScheduleStore 忘掉这条 managed record。
  // 依赖方向仍然是 ScheduleService -> BackupCatalog，Catalog 不知道 schedule
  // 存在。 这一步失败不影响删除本身——下一次 reconcile
  // 会自愈，所以只提示，不报失败。
  ScheduleDocument document;
  ScheduleLoadStatus status = ScheduleLoadStatus::kMissing;
  std::string schedule_error;
  if (LoadScheduleDocument(context, &document, &status, &schedule_error)) {
    const std::size_t before = document.state.managed_snapshots.size();
    ScheduledBackupService::ReconcileManagedSnapshots(repository, &document);
    if (document.state.managed_snapshots.size() != before) {
      std::string save_error;
      if (!SaveScheduleDocument(context, document, &save_error)) {
        std::cerr << "Warning: the archive was deleted, but the schedule store "
                     "could not be updated: "
                  << save_error << "\n";
      }
    }
  }

  std::cout << "Deleted " << file_name << ".\n";
  return kCliExitSuccess;
}

}  // namespace

int RunRepositoryCommand(const CliContext& context,
                         const std::vector<std::string>& arguments) {
  if (arguments.empty()) {
    return UsageError(context, "'repository' expects 'list' or 'delete'");
  }
  if (arguments[0] == "list") return RepositoryList(context);
  if (arguments[0] == "delete") {
    if (arguments.size() != 2) {
      return UsageError(context,
                        "'repository delete' expects exactly one <file_name>");
    }
    return RepositoryDelete(context, arguments[1]);
  }
  return UsageError(context,
                    "unknown repository subcommand '" + arguments[0] + "'");
}

// ---- config ----

namespace {

int ConfigRepositoryShow(const CliContext& context) {
  if (context.config_file_path.empty()) {
    PrintError("Cannot locate the application config file");
    return kCliExitOperationFailed;
  }
  ConfigManager manager(context.config_file_path);
  AppConfig config;
  std::string error;
  const ConfigLoadStatus status = manager.Load(&config, &error);
  if (status == ConfigLoadStatus::kError) {
    PrintError(error);
    return kCliExitOperationFailed;
  }
  std::cout << "Config file: " << context.config_file_path << "\n";
  std::cout << "Status:      " << ConfigLoadStatusText(status) << "\n";
  std::cout << "Repository:  "
            << (config.backup_repository_path.empty()
                    ? "(not set)"
                    : config.backup_repository_path)
            << "\n";
  std::cout << "Schedule:    " << context.schedule_file_path << "\n";
  std::cout << "Runs while:  this program or 'backupctl schedule watch' is "
               "running\n";
  return kCliExitSuccess;
}

int ConfigRepositorySet(const CliContext& context, const std::string& path) {
  if (context.config_file_path.empty()) {
    PrintError("Cannot locate the application config file");
    return kCliExitOperationFailed;
  }
  BackupCatalog catalog;
  std::string error;
  // 先确保仓库可用再落盘：和 GUI
  // 的顺序一致，避免"配置写成功但目录根本建不出来"。
  if (!catalog.EnsureRepository(path, &error)) {
    PrintError(error);
    return kCliExitOperationFailed;
  }
  ConfigManager manager(context.config_file_path);
  AppConfig config;
  config.backup_repository_path = path;
  if (!manager.Save(config, &error)) {
    PrintError(error);
    return kCliExitOperationFailed;
  }
  std::cout << "Repository set to " << path << ".\n";
  return kCliExitSuccess;
}

}  // namespace

int RunConfigCommand(const CliContext& context,
                     const std::vector<std::string>& arguments) {
  if (arguments.size() >= 2 && arguments[0] == "repository") {
    if (arguments[1] == "show") return ConfigRepositoryShow(context);
    if (arguments[1] == "set") {
      if (arguments.size() != 3) {
        return UsageError(context, "'config repository set' expects <path>");
      }
      return ConfigRepositorySet(context, arguments[2]);
    }
  }
  return UsageError(context,
                    "'config' expects 'repository show' or "
                    "'repository set <path>'");
}

}  // namespace backupproject
