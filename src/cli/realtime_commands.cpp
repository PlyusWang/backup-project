// realtime_commands.cpp
//
// 见 include/realtime_commands.h。

#include "realtime_commands.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "app_paths.h"
#include "application_instance_lock.h"
#include "backup_catalog.h"
#include "backup_mode.h"
#include "backup_option_keys.h"
#include "config_manager.h"
#include "realtime_backup_service.h"
#include "realtime_debouncer.h"
#include "realtime_store.h"
#include "realtime_watcher.h"

namespace backupproject {

namespace {

std::int64_t SteadyNowMs() {
  struct timespec now;
  ::clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<std::int64_t>(now.tv_sec) * 1000 +
         static_cast<std::int64_t>(now.tv_nsec) / 1000000;
}

std::int64_t WallNowSec() { return static_cast<std::int64_t>(::time(nullptr)); }

void PrintUsage(const CliContext& context) {
  std::cerr
      << "Usage: " << context.program_name << " realtime <command>\n"
      << "\n"
      << "Commands:\n"
      << "  show                      打印实时备份配置\n"
      << "  set [options]             修改配置（严格校验，原子保存）\n"
      << "  enable                    校验并启用（源存在、仓库可用、不重叠）\n"
      << "  disable                   停用\n"
      << "  watch                     前台监听源目录并实时备份（Ctrl+C 退出）\n"
      << "  history                   列出实时快照\n"
      << "\n"
      << "set options:\n"
      << "  --source DIR             源目录\n"
      << "  --debounce-ms N          合并窗口（100..60000，默认 500）\n"
      << "  --max-wait-ms N          最长等待（500..300000，且 >= debounce）\n"
      << "  --retain N               保留的实时还原点（1..1000）\n"
      << "  --strategy full|incremental\n"
      << "  --pack NAME / --compression NAME / --encryption NAME\n"
      << "  --include RULE / --exclude RULE\n";
}

int UsageError(const CliContext& context, const std::string& message) {
  std::cerr << "Error: " << message << "\n\n";
  PrintUsage(context);
  return kCliExitUsageError;
}

void PrintError(const std::string& message) {
  std::cerr << "Error: " << message << "\n";
}

std::string RealtimePathOf(const CliContext& context) {
  if (!context.realtime_file_path.empty()) return context.realtime_file_path;
  return DefaultRealtimeFilePath();
}

bool LoadRepositoryPath(const CliContext& context, std::string* repository,
                        std::string* error_message) {
  repository->clear();
  if (context.config_file_path.empty()) {
    *error_message =
        "Cannot locate the application config file (neither XDG_CONFIG_HOME "
        "nor HOME is usable)";
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
        " config repository set <directory>";
    return false;
  }
  *repository = config.backup_repository_path;
  return true;
}

std::string StrategyText(BackupStrategy strategy) {
  return BackupStrategyKey(strategy);
}

void PrintConfig(const CliContext& context, const RealtimeConfig& config,
                 bool enabled_hint) {
  std::cout << "Realtime file:  " << RealtimePathOf(context) << "\n";
  std::cout << "Enabled:        " << (config.enabled ? "yes" : "no") << "\n";
  std::cout << "Source:         "
            << (config.source_path.empty() ? std::string("(not set)")
                                           : config.source_path)
            << "\n";
  std::cout << "Debounce:       " << config.debounce_ms << " ms\n";
  std::cout << "Max wait:       " << config.max_wait_ms << " ms\n";
  std::cout << "Retain:         " << config.retain_count
            << " realtime restore point(s)\n";
  std::cout << "Strategy:       " << StrategyText(config.strategy) << "\n";
  std::cout << "Pack:           " << PackMethodKey(config.pack_method) << "\n";
  std::cout << "Compression:    "
            << CompressionMethodKey(config.compression_method) << "\n";
  std::cout << "Encryption:     "
            << EncryptionMethodKey(config.encryption_method) << "\n";
  std::cout << "Include rules:";
  if (config.include_rules.empty()) {
    std::cout << "  (none)\n";
  } else {
    std::cout << "\n";
    for (const std::string& rule : config.include_rules) {
      std::cout << "  " << rule << "\n";
    }
  }
  std::cout << "Exclude rules:";
  if (config.exclude_rules.empty()) {
    std::cout << "  (none)\n";
  } else {
    std::cout << "\n";
    for (const std::string& rule : config.exclude_rules) {
      std::cout << "  " << rule << "\n";
    }
  }
  if (enabled_hint && !config.enabled) {
    std::cout << "Note:           realtime watch is not running\n";
  }
}

bool RequireValue(const std::vector<std::string>& arguments, std::size_t index,
                  const std::string& option, std::string* value,
                  std::string* error_message) {
  if (index + 1 >= arguments.size()) {
    *error_message = option + " requires a value";
    return false;
  }
  *value = arguments[index + 1];
  return true;
}

bool ParseUint32(const std::string& text, std::uint32_t* value,
                 const std::string& option, std::string* error_message) {
  if (text.empty()) {
    *error_message = option + " requires a number";
    return false;
  }
  std::uint64_t result = 0;
  for (const char character : text) {
    if (character < '0' || character > '9') {
      *error_message = option + " must be a decimal integer";
      return false;
    }
    result = result * 10 + static_cast<std::uint64_t>(character - '0');
    if (result > 100000000u) {
      *error_message = option + " is out of range";
      return false;
    }
  }
  *value = static_cast<std::uint32_t>(result);
  return true;
}

// ---- watch ----
volatile sig_atomic_t g_realtime_stop = 0;

void HandleStopSignal(int /*signal_number*/) { g_realtime_stop = 1; }

void InstallStopHandlers() {
  struct sigaction action;
  ::memset(&action, 0, sizeof(action));
  action.sa_handler = HandleStopSignal;
  ::sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  ::sigaction(SIGINT, &action, nullptr);
  ::sigaction(SIGTERM, &action, nullptr);
}

const char* OutcomeKindText(RealtimeOutcome::Kind kind) {
  switch (kind) {
    case RealtimeOutcome::Kind::kFullSnapshot:
      return "full";
    case RealtimeOutcome::Kind::kFullBaseline:
      return "full-baseline";
    case RealtimeOutcome::Kind::kDelta:
      return "delta";
    case RealtimeOutcome::Kind::kNoChanges:
      return "no-changes";
    case RealtimeOutcome::Kind::kFailed:
      return "failed";
  }
  return "unknown";
}

int RunWatch(const RealtimeConfig& config, const std::string& repository,
             const std::string& identity) {
  InotifyWatcher watcher;
  std::string error;
  if (!watcher.Attach(config.source_path, &error)) {
    PrintError(std::string("fatal config error: ") + error);
    return kCliExitOperationFailed;
  }
  std::cout << "[realtime] watching " << config.source_path
            << " (watches=" << watcher.watch_count()
            << ", repository=" << repository << ")\n";

  RealtimeDebouncer debouncer(config.debounce_ms, config.max_wait_ms);
  // 进程不运行期间没有事件，所以 attach 成功之后必须先合成一次 resync：
  // 否则"关掉再打开"的那段时间里的变化会被永远漏掉。
  debouncer.NoteResync(SteadyNowMs());
  std::cout << "[realtime] resync: observing the current source tree\n";

  bool degraded = false;
  std::int64_t next_reattach_ms = 0;

  while (g_realtime_stop == 0) {
    if (degraded) {
      const std::int64_t now = SteadyNowMs();
      if (now >= next_reattach_ms) {
        std::string attach_error;
        if (watcher.Attach(config.source_path, &attach_error)) {
          degraded = false;
          std::cout << "[realtime] watch recovered (watches="
                    << watcher.watch_count() << ")\n";
          debouncer.NoteResync(now);
          std::cout << "[realtime] resync: observing the current source tree\n";
        } else {
          next_reattach_ms = now + 1000;
        }
      }
      ::usleep(100 * 1000);
      continue;
    }

    const std::int64_t now = SteadyNowMs();
    std::int64_t wait_ms = debouncer.WaitMs(now);
    if (wait_ms < 0) wait_ms = 200;
    if (wait_ms > 200) wait_ms = 200;

    struct pollfd descriptor;
    descriptor.fd = watcher.fd();
    descriptor.events = POLLIN;
    descriptor.revents = 0;
    const int ready = ::poll(&descriptor, 1, static_cast<int>(wait_ms));
    if (ready < 0 && errno != EINTR) {
      PrintError(std::string("watch degraded: poll failed: ") +
                 ::strerror(errno));
      degraded = true;
      next_reattach_ms = SteadyNowMs() + 1000;
      continue;
    }

    if (ready > 0) {
      WatchBatch batch;
      std::string drain_error;
      if (!watcher.Drain(&batch, &drain_error)) {
        std::cout << "[realtime] watch degraded: " << drain_error << "\n";
        degraded = true;
        next_reattach_ms = SteadyNowMs() + 1000;
        continue;
      }
      if (batch.any_event) {
        if (batch.overflow) {
          std::cout
              << "[realtime] overflow: rebuilding watches and resyncing\n";
        }
        debouncer.NoteEvents(batch.event_count, batch.structural,
                             batch.overflow, SteadyNowMs());
        if (batch.any_event && !batch.overflow) {
          std::cout << "[realtime] debouncing " << batch.event_count
                    << " event(s)\n";
        }
        if (batch.structural || batch.overflow) {
          std::string rebuild_error;
          if (!watcher.Rebuild(&rebuild_error)) {
            std::cout << "[realtime] watch degraded: " << rebuild_error << "\n";
            degraded = true;
            next_reattach_ms = SteadyNowMs() + 1000;
            continue;
          }
        }
        if (batch.root_lost) {
          std::cout
              << "[realtime] watch degraded: the source root disappeared\n";
          degraded = true;
          next_reattach_ms = SteadyNowMs() + 1000;
          continue;
        }
        if (batch.overflow) debouncer.NoteResync(SteadyNowMs());
      }
    }

    const std::int64_t due_now = SteadyNowMs();
    if (!debouncer.Due(due_now)) continue;

    const RealtimeGeneration generation = debouncer.Consume(due_now);
    RealtimeEventSummary summary;
    summary.generation = generation.generation;
    summary.event_count = generation.event_count;
    summary.overflow = generation.overflow_seen;
    summary.structural = generation.structural_seen;
    summary.resync = generation.resync;
    if (generation.resync) {
      std::cout << "[realtime] resync trigger: capturing the current source "
                   "tree\n";
    } else {
      std::cout << "[realtime] settled generation #" << generation.generation
                << " (" << generation.event_count << " event(s))\n";
    }

    RealtimeOutcome outcome;
    std::string run_error;
    if (!RunRealtimeBackupOnce(config, repository, identity, summary,
                               WallNowSec(), &outcome, &run_error)) {
      PrintError(run_error);
      continue;
    }
    std::cout << "[realtime] outcome=" << OutcomeKindText(outcome.kind);
    if (!outcome.snapshot_file_name.empty()) {
      std::cout << " snapshot=" << outcome.snapshot_file_name;
    }
    std::cout << "\n";
    // 面向用户的那句话直接来自共享核心：GUI 显示的是同一句，CLI 不另写一份。
    if (!outcome.summary_text.empty()) {
      std::cout << "[realtime] " << outcome.summary_text << "\n";
    }
    if (!outcome.diagnostic.empty()) {
      std::cout << "[realtime] retention warning: " << outcome.diagnostic
                << "\n";
    }
  }

  watcher.Detach();
  std::cout << "[realtime] stopped\n";
  return kCliExitSuccess;
}

}  // namespace

int RunRealtimeCommand(const CliContext& context,
                       const std::vector<std::string>& arguments) {
  if (arguments.empty()) return UsageError(context, "realtime needs a command");
  const std::string command = arguments[0];
  const std::string path = RealtimePathOf(context);
  if (path.empty()) {
    PrintError(
        "Cannot locate the realtime config file (neither XDG_CONFIG_HOME nor "
        "HOME is usable)");
    return kCliExitOperationFailed;
  }
  RealtimeStore store(path);

  if (command == "show") {
    RealtimeConfig config;
    std::string error;
    const RealtimeLoadStatus status = store.Load(&config, &error);
    if (status == RealtimeLoadStatus::kError) {
      PrintError(error);
      return kCliExitOperationFailed;
    }
    PrintConfig(context, config, /*enabled_hint=*/true);
    return kCliExitSuccess;
  }

  if (command == "history") {
    std::string repository;
    std::string error;
    if (!LoadRepositoryPath(context, &repository, &error)) {
      PrintError(error);
      return kCliExitOperationFailed;
    }
    std::vector<RealtimeSnapshotRecord> records;
    if (!ListRealtimeSnapshots(repository, &records, &error)) {
      PrintError(error);
      return kCliExitOperationFailed;
    }
    std::cout << "Repository: " << repository << "\n";
    std::cout << "Realtime snapshots: " << records.size() << "\n";
    for (const RealtimeSnapshotRecord& record : records) {
      std::cout << "  " << record.file_name << "  ";
      if (!record.verified) {
        std::cout << "[unverified: " << record.diagnostic << "]\n";
        continue;
      }
      std::cout << "strategy=" << StrategyText(record.strategy)
                << "  kind=" << record.outcome_kind
                << "  events=" << record.event_count
                << "  pack=" << PackMethodKey(record.pack_method)
                << "  compression="
                << CompressionMethodKey(record.compression_method)
                << "  created=" << record.created_time_sec;
      if (record.resync_trigger) std::cout << "  resync";
      if (record.overflow_recovery) std::cout << "  overflow-recovery";
      std::cout << "\n";
    }
    return kCliExitSuccess;
  }

  if (command == "set") {
    RealtimeConfig config;
    std::string error;
    const RealtimeLoadStatus status = store.Load(&config, &error);
    if (status == RealtimeLoadStatus::kError) {
      PrintError(error);
      return kCliExitOperationFailed;
    }
    bool reset_rules = false;
    for (std::size_t index = 1; index < arguments.size(); ++index) {
      const std::string& option = arguments[index];
      std::string value;
      if (option == "--clear-filters") {
        reset_rules = true;
        config.include_rules.clear();
        config.exclude_rules.clear();
        continue;
      }
      if (option == "--include" || option == "--exclude") {
        if (!RequireValue(arguments, index, option, &value, &error)) {
          return UsageError(context, error);
        }
        ++index;
        if (reset_rules) {
          reset_rules = false;
        }
        if (option == "--include") {
          config.include_rules.push_back(value);
        } else {
          config.exclude_rules.push_back(value);
        }
        continue;
      }
      if (!RequireValue(arguments, index, option, &value, &error)) {
        return UsageError(context, error);
      }
      ++index;
      if (option == "--source") {
        config.source_path = value;
      } else if (option == "--debounce-ms") {
        if (!ParseUint32(value, &config.debounce_ms, option, &error)) {
          return UsageError(context, error);
        }
      } else if (option == "--max-wait-ms") {
        if (!ParseUint32(value, &config.max_wait_ms, option, &error)) {
          return UsageError(context, error);
        }
      } else if (option == "--retain") {
        if (!ParseUint32(value, &config.retain_count, option, &error)) {
          return UsageError(context, error);
        }
      } else if (option == "--strategy") {
        if (!ParseBackupStrategyKey(value, &config.strategy)) {
          return UsageError(context, "unknown strategy '" + value +
                                         "' (expected full or incremental)");
        }
      } else if (option == "--pack") {
        if (!ParsePackMethodKey(value, &config.pack_method)) {
          return UsageError(context, "unknown pack method '" + value + "'");
        }
      } else if (option == "--compression") {
        if (!ParseCompressionMethodKey(value, &config.compression_method)) {
          return UsageError(context,
                            "unknown compression method '" + value + "'");
        }
      } else if (option == "--encryption") {
        if (!ParseEncryptionMethodKey(value, &config.encryption_method)) {
          return UsageError(context,
                            "unknown encryption method '" + value + "'");
        }
      } else {
        return UsageError(context, "unknown option '" + option + "'");
      }
    }

    // 共享真值表是唯一答案来源：不支持就明确拒绝，绝不静默降级。
    BackupOptionCombination combination;
    combination.trigger = config.trigger;
    combination.strategy = config.strategy;
    combination.pack_method = config.pack_method;
    combination.compression_method = config.compression_method;
    combination.encryption_method = config.encryption_method;
    if (!IsSupportedBackupOptionCombination(combination)) {
      return UsageError(context,
                        UnsupportedBackupOptionCombinationReason(combination));
    }
    error.clear();
    if (!ValidateRealtimeConfig(config, &error)) {
      return UsageError(context, error);
    }
    if (!store.Save(config, &error)) {
      PrintError(error);
      return kCliExitOperationFailed;
    }
    std::cout << "Realtime configuration saved.\n";
    PrintConfig(context, config, /*enabled_hint=*/false);
    return kCliExitSuccess;
  }

  if (command == "enable" || command == "disable") {
    RealtimeConfig config;
    std::string error;
    const RealtimeLoadStatus status = store.Load(&config, &error);
    if (status == RealtimeLoadStatus::kError) {
      PrintError(error);
      return kCliExitOperationFailed;
    }
    if (command == "disable") {
      config.enabled = false;
      if (!store.Save(config, &error)) {
        PrintError(error);
        return kCliExitOperationFailed;
      }
      std::cout << "Realtime backup disabled.\n";
      return kCliExitSuccess;
    }
    std::string repository;
    if (!LoadRepositoryPath(context, &repository, &error)) {
      PrintError(error);
      return kCliExitOperationFailed;
    }
    std::string identity;
    error.clear();
    if (!ValidateRealtimeForEnable(config, repository, &error, &identity)) {
      PrintError(error);
      return kCliExitOperationFailed;
    }
    config.enabled = true;
    if (!store.Save(config, &error)) {
      PrintError(error);
      return kCliExitOperationFailed;
    }
    std::cout << "Realtime backup enabled.\n";
    PrintConfig(context, config, /*enabled_hint=*/true);
    return kCliExitSuccess;
  }

  if (command == "watch") {
    RealtimeConfig config;
    std::string error;
    const RealtimeLoadStatus status = store.Load(&config, &error);
    if (status == RealtimeLoadStatus::kError) {
      PrintError(error);
      return kCliExitOperationFailed;
    }
    if (!config.enabled) {
      PrintError("Realtime backup is not enabled. Run: " +
                 context.program_name + " realtime enable");
      return kCliExitOperationFailed;
    }
    std::string repository;
    if (!LoadRepositoryPath(context, &repository, &error)) {
      PrintError(error);
      return kCliExitOperationFailed;
    }
    std::string identity;
    if (!ValidateRealtimeForEnable(config, repository, &error, &identity)) {
      PrintError(std::string("fatal config error: ") + error);
      return kCliExitOperationFailed;
    }

    // 那把 per-UID application lock 由 CLI 入口（app/backupctl.cpp 的 main）
    // 在整个进程生命周期内持有。这里**不能**再抢一次：flock 是绑在 open file
    // description 上的，同一进程第二次 open 同一路径再 LOCK_EX|LOCK_NB 一样会
    // EWOULDBLOCK，结果是自己把自己判成"另一个实例正在运行"。
    // 换句话说，"同一时刻只有一个产品进程"这条规则已经由入口保证了。
    InstallStopHandlers();
    return RunWatch(config, repository, identity);
  }

  return UsageError(context, "unknown realtime command '" + command + "'");
}

}  // namespace backupproject
