// realtime_commands.cpp
//
// 见 include/realtime_commands.h。

// 模块职责：realtime 子命令的 CLI 前端（show / set / enable / disable / watch /
// history），是"配置校验 -> 落盘 -> 长期监听 -> 触发一次备份"这条链的入口。
//
// 边界：本文件不实现任何实时语义。监听在 InotifyWatcher，合并窗口在
// RealtimeDebouncer，一次触发怎么执行在 RunRealtimeBackupOnce，配置结构校验在
// RealtimeStore，规则语法在 Filter::AddRule。这里只做参数解析、把校验结果翻译
// 成退出码与人话、以及驱动 watch 主循环。
//
// 数据流（改配置）：argv -> RealtimeStore::Load（文件缺失时是默认值）-> 逐项
// 覆盖 -> IsSupportedBackupOptionCombination + ValidateRealtimeConfig ->
// store.Save（原子写）。enable 额外做文件系统级校验，全过才写 enabled=true。
//
// 数据流（watch）：Load -> 校验 -> Attach -> poll/Drain -> Debouncer 判定
// "这一代稳定了" -> RunRealtimeBackupOnce -> 打印 outcome。
//
// 失败语义：所有路径都返回 kCliExit* 退出码并往 stderr 写 "Error: ..."；参数与
// 配置错误用 kCliExitUsageError（并补打 usage），运行期错误用
// kCliExitOperationFailed。不抛异常、不 abort。
//
// 并发：单线程事件循环 + 同步备份；"同一时刻只有一个产品进程"由入口的
// ApplicationInstanceLock 保证，因此这里的"读-改-写"不会被别的进程插队。
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

// 两种时钟分工明确：MONOTONIC 只用于 debounce 判定与退避计时，不受系统时间调整
// 影响（wall clock 回拨会让等待窗口永不到期）；写快照时间戳用 wall clock。
std::int64_t SteadyNowMs() {
  struct timespec now;
  ::clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<std::int64_t>(now.tv_sec) * 1000 +
         static_cast<std::int64_t>(now.tv_nsec) / 1000000;
}

std::int64_t WallNowSec() { return static_cast<std::int64_t>(::time(nullptr)); }

// usage 里的取值范围（debounce 100..60000、max-wait 500..300000 且
// >= debounce、retain 1..1000）必须与 RealtimeStore 的校验常量一致，
// 否则照提示填的值会被拒绝。
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

// 用法错误的统一出口：专门退出码 + 整篇 usage。脚本据此把"参数写错了"和"运行时
// 失败了"分开处理，所以两者不能用同一个退出码。
int UsageError(const CliContext& context, const std::string& message) {
  std::cerr << "Error: " << message << "\n\n";
  PrintUsage(context);
  return kCliExitUsageError;
}

// 运行期错误只打一行，不刷 usage：此时用户需要的是原因，不是用法。
void PrintError(const std::string& message) {
  std::cerr << "Error: " << message << "\n";
}

// 配置文件位置：测试注入（context.realtime_file_path）优先，否则产品默认路径。
// 返回空串表示连 HOME / XDG_CONFIG_HOME 都拿不到——调用方必须报错退出，绝不能
// 退化成"写当前目录"，否则会在用户没预期的地方留下配置文件。
std::string RealtimePathOf(const CliContext& context) {
  if (!context.realtime_file_path.empty()) return context.realtime_file_path;
  return DefaultRealtimeFilePath();
}

// 仓库路径的唯一来源：应用配置里的 backup_repository_path。realtime 命令不接受
// 命令行传仓库，避免一条命令就把备份写到别处，也避免 CLI 与 GUI 看到不同仓库。
// kError 直接透传（配置文件坏了要修）；kMissing 与空路径同等对待，都表示"还没配
// 仓库"，提示用户跑 config repository set。
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

// 文案唯一来源是 backup_option_keys：CLI 输出与 GUI 显示共用同一套键。
std::string StrategyText(BackupStrategy strategy) {
  return BackupStrategyKey(strategy);
}

// 字段名固定、每行一项，方便用户对照文档与 diff；但不承诺机器可解析（机器接口是
// RealtimeStore 落盘的 JSON）。enabled_hint 用来区分 show（要提示"现在没在跑"）
// 与 set / enable（刚改完，再提示没有意义）。
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

// 只读 arguments[index + 1]，不改变 index：跳过一个取值由调用方自己 ++index
// 完成，这样无值选项（--clear-filters）与有值选项能共用同一个解析循环。
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

// 手写十进制解析而不是 strtoul：不接受符号、空白、0x 前缀与前导零。每步都检查
// 上界 1e8，所以 result 最多约 1e9，远低于 uint64 上限、不可能回绕；真正的业务
// 范围（100..60000 等）留给 ValidateRealtimeConfig，避免范围常量写两份。
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
// 信号与主循环之间唯一通信：volatile sig_atomic_t 是唯一保证能被信号处理器安全
// 读写的类型。处理器只置标志、不做清理——close / detach 都不是
// async-signal-safe，收尾一律回到主循环的正常路径上做。
volatile sig_atomic_t g_realtime_stop = 0;

void HandleStopSignal(int /*signal_number*/) { g_realtime_stop = 1; }

// SIGINT（Ctrl+C）与 SIGTERM（systemd stop / kill）共用同一个处理器。sa_flags
// 刻意留 0（不加 SA_RESTART）：这样 poll 会立刻以 EINTR 返回，主循环能马上看到
// 停止标志，而不是等到下一次事件或超时。
void InstallStopHandlers() {
  struct sigaction action;
  ::memset(&action, 0, sizeof(action));
  action.sa_handler = HandleStopSignal;
  ::sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  ::sigaction(SIGINT, &action, nullptr);
  ::sigaction(SIGTERM, &action, nullptr);
}

// 这些字符串与 marker 里的 outcome_kind 同源（full / full-baseline / delta /
// no-changes / failed），history 打印的就是它。
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

// watch 主循环。前置：config 已通过 ValidateRealtimeForEnable，repository 与
// identity 已解析——本函数只处理运行期故障，配置错误在这里无法恢复。
// 状态机：Attach -> Running <-> Degraded（1s 退避重试）-> Stopped；停止标志只在
// 循环条件处检查，因此最坏延迟是 poll 的上限 200ms。
// 单线程、无后台线程：备份是同步跑的，备份期间的新事件会堆在内核队列里，回到
// 循环后一次 Drain 全部读走并合成下一代（队列满则 overflow -> resync）。
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
  // 从 debouncer 的角度看，resync 是"立刻到期的一代"：它不依赖事件计数，因为
  // overflow 之后事件历史已经不可信，重新观察当前源树是唯一的恢复路径。
  std::cout << "[realtime] resync: observing the current source tree\n";

  bool degraded = false;
  std::int64_t next_reattach_ms = 0;

  while (g_realtime_stop == 0) {
    // 退化：watcher 不可信（poll / drain / rebuild 失败或 root 丢失），退避 1s
    // 后重新 Attach。睡眠切成 100ms 片，是为了让 Ctrl+C 的响应不被 1s 挡住。
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
    // 即使 debouncer 说还要等更久，poll 也最多等 200ms：停止标志与 root 丢失
    // 状态因此最多 200ms 就被重查一次（WaitMs 返回 -1 也退化成 200ms 轮询）。

    struct pollfd descriptor;
    descriptor.fd = watcher.fd();
    descriptor.events = POLLIN;
    descriptor.revents = 0;
    // ready < 0 且 errno == EINTR 是正常的（信号处理器刚跑过），继续循环即可；
    // 其它错误说明这个 fd 已不可靠，只能整体重建。
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
        // 顺序有意为之：先 NoteEvents 把 overflow 计入这一代的诊断信息，再
        // NoteResync 让它立即到期。两者都会置 dirty，但 resync 标记告诉调用方
        // "这次不是因为文件变了，而是因为事件历史丢了"。
        if (batch.overflow) debouncer.NoteResync(SteadyNowMs());
      }
    }

    const std::int64_t due_now = SteadyNowMs();
    if (!debouncer.Due(due_now)) continue;

    // 只有在真的要把这一代交给备份之前才 Consume：它会清 dirty，清了就不会再
    // 触发。注意失败时直接 continue、不补 resync，所以失败窗口里的变化
    // 要等下一个事件才可能被覆盖（如需"失败也重试"，应在这里补 NoteResync）。
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
    // 单次失败不终止监听：长期守护进程的可用性优先，打印错误后继续等下一代；
    // outcome 只在返回 true 时可信。
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

  // 正常退出路径：显式 Detach（析构也会做一次，这里是收尾）。Ctrl+C 停止不是
  // 错误，所以退出码始终是成功——systemd 与脚本据此区分"用户停了"与"跑挂了"。
  watcher.Detach();
  std::cout << "[realtime] stopped\n";
  return kCliExitSuccess;
}

}  // namespace

// 子命令分派。每个分支自己 Load 一次配置，再做"改 -> 校验 -> Save"，没有
// 跨子命令共享的可变状态；进程级 ApplicationInstanceLock 保证这段"读-改-写"
// 不会被另一个产品进程（GUI 或另一个 CLI）插在中间；未知子命令报 usage 错误。
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

  // set 是增量修改：先 Load 当前配置（文件缺失时得默认值），只覆盖命令行提到
  // 的字段。--clear-filters 与 --include/--exclude 按顺序生效：先清空再追加，
  // 结果只保留清空之后写的规则。任何一项非法都在落盘前返回：宁可不保存，也不
  // 保存一份"部分生效"的配置。
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
    // 先问真值表、再问结构校验：组合本身不成立时，用户需要知道"为什么这个组合
    // 不行"（UnsupportedBackupOptionCombinationReason 给出原因），而不是先被
    // 一堆范围错误淹没。
    if (!IsSupportedBackupOptionCombination(combination)) {
      return UsageError(context,
                        UnsupportedBackupOptionCombinationReason(combination));
    }
    error.clear();
    // 结构校验（范围 / 组合 / 长度 / NUL）不访问文件系统，所以允许保存一个暂时
    // 不存在的 source；"源目录必须真的存在"留到 enable 时再查。
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

  // enable 与 disable 共用一个分支，差别只有一步：disable 永远允许（关掉实时
  // 备份不该有前置条件），enable 必须先过文件系统级校验。
  // enable 的顺序：仓库已配置 -> ValidateRealtimeForEnable（source 存在、是真实
  // 目录、非软链接，repository 可解析，两者不重叠）-> 全过才写 enabled=true。
  if (command == "enable" || command == "disable") {
    RealtimeConfig config;
    std::string error;
    const RealtimeLoadStatus status = store.Load(&config, &error);
    if (status == RealtimeLoadStatus::kError) {
      PrintError(error);
      return kCliExitOperationFailed;
    }
    // disable 不做校验：配置坏掉时，用户更需要能把它关掉。
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
    // identity 是仓库指纹，会作为 marker 的 job_identity 写进快照：同一次
    // enable 与之后的 watch 必须看到同一个仓库，否则归属判断会错乱。
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

  // watch：要求 enabled 已为真，并再校验一次（enable 之后源目录可能被删、换成
  // 普通文件或软链接）。校验失败以 "fatal config error:" 前缀退出而不是进入
  // degraded——配置错误不会自己恢复，反复重试只会刷日志。
  // 前台运行、Ctrl+C 退出，不 daemon 化：进程生命周期交给 systemd 管。
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
    // 因此这里可以安全地做"读配置 -> 校验 -> 长时间运行"，不必担心另一个实例在
    // 运行期间改掉同一份 realtime.json。
    InstallStopHandlers();
    return RunWatch(config, repository, identity);
  }

  return UsageError(context, "unknown realtime command '" + command + "'");
}

}  // namespace backupproject
