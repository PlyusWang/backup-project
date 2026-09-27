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
#include "backup_mode.h"
#include "backup_option_keys.h"
#include "backup_preview.h"
#include "cli_app.h"
#include "config_manager.h"
#include "filter.h"
#include "incremental_backup.h"
#include "incremental_restore.h"
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

// 单值选项的重复检测。
//
// "--retain 1 --retain 2" 不是"后者覆盖前者"：用户对同一件事说了两遍，静默取
// 一个会让真正想指定的那个被丢掉，而且脚本里的拼接错误会一路静默通过。
//
// 刻意不覆盖三类：
//   * --include / --exclude 是真·可重复选项，重复是它的正常用法；
//   * --clear-filters 是幂等的布尔开关，说两遍和不说一样，没有歧义；
//   * 位置参数由各自的"参数个数"检查负责。
bool MarkSingleOption(bool* seen, const std::string& option,
                      std::string* error_message) {
  if (*seen) {
    *error_message = option + " was given more than once";
    return false;
  }
  *seen = true;
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

// 路径的最后一段。"a/b.bak" -> "b.bak"、"b.bak" -> "b.bak"。
// 只用于把 Catalog 给出的路径缩成一个可读的文件名，不参与任何安全判断。
std::string BaseNameOf(const std::string& path) {
  const std::size_t slash = path.rfind('/');
  if (slash == std::string::npos) return path;
  return path.substr(slash + 1);
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

std::int64_t NowSeconds() { return static_cast<std::int64_t>(::time(nullptr)); }

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
      << " backup <source_directory> [--include <rule>]... [--exclude "
         "<rule>]...\n"
      << "    The archive is written into the configured repository, with a "
         "name the\n"
      << "    program generates; there is no way to give it a path.\n"
      << "  " << program_name
      << " preview <source_directory> [--include <rule>]... [--exclude "
         "<rule>]...\n"
      << "    Read-only: lists the entries a backup with these filter rules "
         "would\n"
      << "    put into the archive. Creates no archive, needs no repository "
         "and\n"
      << "    writes no state. Same rules, same selection and the same "
         "preview\n"
      << "    window as the Manual Backup preview in the Modern GUI. The "
         "full\n"
      << "    effective backup traversal is always validated; at most "
      << kPreviewEntryLimit << " entries\n"
      << "    are listed, and truncation is reported. Exits 1 when the "
         "selection\n"
      << "    could not be backed up (for example an un-excluded socket).\n"
      << "  " << program_name
      << " restore <file_name> <destination_directory>\n"
      << "    <file_name> must be a single-component .bak name inside the "
         "configured\n"
      << "    repository (see 'repository list').\n"
      << "  " << program_name
      << " schedule show | set | enable | disable | run | history | watch\n"
      << "    schedule set takes --source --interval-minutes --retain "
         "--strategy\n"
      << "      --pack --compression --encryption none --include --exclude, "
         "plus\n"
      << "      --clear-filters (drop the stored rules first, then add the "
         "ones\n"
      << "      given on this command line).\n"
      << "    schedule run evaluates right now, ignoring the due time; a run "
         "with no changes is still skipped, so it never becomes a forced "
         "backup.\n"
      << "    schedule enable requires a configured repository and a real "
         "source\n"
      << "      directory; the first run is scheduled one full interval away.\n"
      << "  " << program_name << " repository list | delete <file_name>\n"
      << "  " << program_name << " config repository show | set <path>\n"
      << "\n"
      << "Backup pipeline options (the product archive is always a v2 "
         "container;\n"
      << "these choose how it is packed, compressed and encrypted):\n"
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
      << "Every command consumes all of its arguments: an unexpected\n"
      << "positional argument, an unknown option, a missing option value, a\n"
      << "repeated single-value option and an out-of-range number are all\n"
      << "usage errors. --include / --exclude may be repeated.\n"
      << "\n"
      << "Only one instance of this program may run at a time. The Modern GUI\n"
      << "and this command share one application lock; when the other one is\n"
      << "already running, every business command exits with code 3 and does\n"
      << "not touch the configuration, the repository or the schedule.\n"
      << "\n"
      << "Exit codes:\n"
      << "  0  success\n"
      << "  1  operation failed\n"
      << "  2  command line usage error\n"
      << "  3  another backup-project instance is already running\n"
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
//
// 与 Modern GUI **完全同一套业务模型**：归档由 BackupCatalog 在**配置好的
// 仓库**里命名（<source-base>_YYYYMMDD_HHMMSS.bak），调用方不能指定路径。
//
// 任意路径的 direct archive（以及它默认产出的 legacy v0.1）不是产品功能：
// 那是归档格式的测试夹具（tests/tools/archive_cli.cpp）负责的事。产品 CLI
// 与产品 GUI 都不再暴露"把备份写到哪就是哪"这个能力。
int RunBackupCommand(const CliContext& context,
                     const std::vector<std::string>& arguments) {
  if (arguments.empty()) {
    std::cerr << "Error: 'backup' expects <source_directory>.\n\n";
    PrintCliUsage(context.program_name, std::cerr);
    return kCliExitUsageError;
  }
  const std::string source_directory = arguments[0];

  Filter filter;
  BackupOptions options;
  // 策略默认 full：不写 --strategy 的既有用法行为一字不变。
  BackupStrategy strategy = BackupStrategy::kFull;
  bool encryption_requested = false;
  bool saw_pack = false;
  bool saw_compression = false;
  bool saw_encryption = false;
  bool saw_strategy = false;
  std::string error_message;
  // 规则原文按 include / exclude 分开留着：它是增量链的 identity 之一
  // （规则变了就必须重新建基线），所以不能只留下编好的 Filter。
  std::vector<std::string> include_rules;
  std::vector<std::string> exclude_rules;

  for (std::size_t index = 1; index < arguments.size(); ++index) {
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
      if (action == FilterAction::kInclude) {
        include_rules.push_back(value);
      } else {
        exclude_rules.push_back(value);
      }
      continue;
    }
    if (option == "--strategy") {
      if (!MarkSingleOption(&saw_strategy, option, &error_message)) {
        return UsageError(context, error_message);
      }
      if (!TakeValue(arguments, &index, option, &value, &error_message)) {
        return UsageError(context, error_message);
      }
      // 解析失败绝不回退到 full：用户明确说了 incremental，就必须拿到明确的
      // 结果或明确的错误。
      if (!ParseBackupStrategyKey(value, &strategy)) {
        return UsageError(context, "unknown backup strategy '" + value +
                                       "' (expected full or incremental)");
      }
      continue;
    }
    if (option == "--pack") {
      if (!MarkSingleOption(&saw_pack, option, &error_message)) {
        return UsageError(context, error_message);
      }
      if (!TakeValue(arguments, &index, option, &value, &error_message)) {
        return UsageError(context, error_message);
      }
      if (!ParsePackMethodKey(value, &options.pack_method)) {
        return UsageError(context, "unknown pack method '" + value +
                                       "' (expected mypack, ustar or "
                                       "fast-ustar)");
      }
      continue;
    }
    if (option == "--compression") {
      if (!MarkSingleOption(&saw_compression, option, &error_message)) {
        return UsageError(context, error_message);
      }
      if (!TakeValue(arguments, &index, option, &value, &error_message)) {
        return UsageError(context, error_message);
      }
      if (!ParseCompressionMethodKey(value, &options.compression_method)) {
        return UsageError(context, "unknown compression method '" + value +
                                       "' (expected none, huffman or "
                                       "lzss-huffman)");
      }
      continue;
    }
    if (option == "--encryption") {
      if (!MarkSingleOption(&saw_encryption, option, &error_message)) {
        return UsageError(context, error_message);
      }
      if (!TakeValue(arguments, &index, option, &value, &error_message)) {
        return UsageError(context, error_message);
      }
      if (!ParseEncryptionMethodKey(value, &options.encryption_method)) {
        return UsageError(context, "unknown encryption method '" + value +
                                       "' (expected none, "
                                       "aes-256-ctr-hmac-sha256 or "
                                       "des-cbc-hmac-sha256)");
      }
      encryption_requested =
          options.encryption_method != EncryptionMethod::kNone;
      continue;
    }
    return UsageError(context, "unknown option '" + option + "'");
  }

  // 仓库是业务模型的一部分：没有仓库就没有"产品备份"这回事。
  std::string repository;
  if (!LoadRepositoryPath(context, &repository, &error_message)) {
    PrintError(error_message);
    return kCliExitOperationFailed;
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
    for (char& character : secret) character = '\0';
  }

  BackupCatalog catalog;
  if (!catalog.EnsureRepository(repository, &error_message)) {
    PrintError(error_message);
    return kCliExitOperationFailed;
  }
  // 命名规则完全属于 BackupCatalog：这里一行都没有复制那套规则。
  std::string archive_path;
  if (!catalog.BuildArchivePath(repository, source_directory, NowSeconds(),
                                &archive_path, &error_message)) {
    PrintError(error_message);
    return kCliExitOperationFailed;
  }

  // 支持矩阵是唯一答案来源，而且要在做任何写盘动作**之前**问它：
  // "argv 收下了、校验再拒绝"这种半吊子状态最容易让人以为命令成功了。
  if (!IsSupportedBackupMode(BackupTrigger::kManual, strategy)) {
    PrintError(UnsupportedBackupModeReason(BackupTrigger::kManual, strategy));
    return kCliExitOperationFailed;
  }
  // 打包方式与策略的组合也要在**写任何东西之前**拒绝：增量第一版只支持
  // MyPack，等到第一次 delta 才失败会把用户留在一份"看起来可用"的基线上。
  if (strategy == BackupStrategy::kIncremental &&
      !IsSupportedIncrementalPack(options.pack_method)) {
    return UsageError(context, UnsupportedIncrementalPackReason());
  }

  const std::string file_name = BaseNameOf(archive_path);
  if (strategy == BackupStrategy::kIncremental) {
    // 增量：baseline / delta / 无变化三选一，由共享引擎决定并如实报告。
    IncrementalOutcome outcome;
    if (!RunIncrementalBackup(source_directory, repository, file_name,
                              RepositoryIdentity(repository), filter, options,
                              include_rules, exclude_rules, "", &outcome,
                              &error_message)) {
      PrintError(error_message);
      return kCliExitOperationFailed;
    }
    if (outcome.kind == IncrementalOutcome::Kind::kNoChanges) {
      std::cout << "No effective changes since the last snapshot; nothing was "
                   "written.\n";
      std::cout << "Repository: " << repository << "\n";
      std::cout << "Parent:     " << outcome.parent_file_name << "\n";
      return kCliExitSuccess;
    }
    std::cout << "Backup completed successfully.\n";
    std::cout << "Repository: " << repository << "\n";
    std::cout << "Archive:    " << file_name << "\n";
    std::cout << "Strategy:   " << BackupStrategyKey(strategy) << "\n";
    if (outcome.kind == IncrementalOutcome::Kind::kFullBaseline) {
      // 用户要的是增量，这一轮建的却是完整基线：必须说出来，还要说清为什么。
      std::cout << "Kind:       full baseline (no trustworthy baseline: "
                << outcome.baseline_reason << ")\n";
    } else {
      std::cout << "Kind:       delta\n";
      std::cout << "Parent:     " << outcome.parent_file_name << "\n";
      std::cout << "Changes:    added=" << outcome.summary.added
                << " modified=" << outcome.summary.modified
                << " metadata=" << outcome.summary.metadata_changed
                << " removed=" << outcome.summary.removed << "\n";
    }
    std::cout << "Pipeline: pack=" << PackMethodKey(options.pack_method)
              << " compression="
              << CompressionMethodKey(options.compression_method)
              << " encryption="
              << EncryptionMethodKey(options.encryption_method) << '\n';
    return kCliExitSuccess;
  }

  BackupEngine engine;
  if (!engine.Backup(source_directory, archive_path, filter, options,
                     &error_message)) {
    PrintError(error_message);
    return kCliExitOperationFailed;
  }
  std::cout << "Backup completed successfully.\n";
  std::cout << "Repository: " << repository << "\n";
  std::cout << "Archive:    " << file_name << "\n";
  std::cout << "Strategy:   " << BackupStrategyKey(strategy) << "\n";
  std::cout << "Pipeline: pack=" << PackMethodKey(options.pack_method)
            << " compression="
            << CompressionMethodKey(options.compression_method)
            << " encryption=" << EncryptionMethodKey(options.encryption_method)
            << '\n';
  return kCliExitSuccess;
}

// ---- preview ----
//
// Manual Backup 的"填规则 -> 预览"这一步。它与 Modern GUI 走的是**同一个函数**
// （backupproject::PreviewBackupSelection），所以两边给出的条目集合、排序与
// 截断标志必然一致；真实 backup 用的又是同一个 Filter，于是
// "GUI 预览 == CLI 预览 == 实际归档条目"是结构上的结论，不是约定。
//
// 它刻意**不需要仓库**：预览不写归档、不碰 repository / config / schedule /
// history，也不创建任何临时文件。用户可以先把规则调好，再决定备份到哪。
//
// 只接受 --include / --exclude。--pack / --compression / --encryption 属于
// "怎么写归档"，与"选哪些条目"无关：允许它们只会让人以为这条命令会执行备份。
int RunPreviewCommand(const CliContext& context,
                      const std::vector<std::string>& arguments) {
  if (arguments.empty()) {
    std::cerr << "Error: 'preview' expects <source_directory>.\n\n";
    PrintCliUsage(context.program_name, std::cerr);
    return kCliExitUsageError;
  }
  const std::string source_directory = arguments[0];

  std::vector<FilterRuleDraft> rules;
  std::string error_message;
  for (std::size_t index = 1; index < arguments.size(); ++index) {
    const std::string option = arguments[index];
    std::string value;
    if (option != "--include" && option != "--exclude") {
      return UsageError(context, "unknown option '" + option + "'");
    }
    if (!TakeValue(arguments, &index, option, &value, &error_message)) {
      return UsageError(context, error_message);
    }
    FilterRuleDraft rule;
    rule.action =
        option == "--include" ? FilterAction::kInclude : FilterAction::kExclude;
    rule.raw_dsl = value;
    // 语法裁决只有一处：ValidateRule 内部的 Filter::AddRule。预览既不会接受
    // 备份会拒绝的规则，也不会拒绝备份会接受的规则。
    if (!ValidateRule(rule, &error_message)) {
      return UsageError(context, error_message);
    }
    rules.push_back(rule);
  }

  // 到这里为止没有碰过任何持久状态：解析失败(2) 一定发生在扫描之前。
  const PreviewResult preview = PreviewBackupSelection(source_directory, rules);
  if (!preview.error.empty()) {
    // 消息就是共享核心（也就是真实 Backup）报出的那一句，一个字都不改：
    // "源目录不可用"、"遍历失败"、"这份选择无法被成功备份"这三件事由结构化
    // 的 error_kind 区分，判断不在这一层重做。
    PrintError(preview.error);
    if (preview.error_kind == PreviewErrorKind::kSelectionBlocked) {
      // 这不是命令行用法错误（那是 2），而是"按当前规则备份必然失败"（1）。
      std::cout << "Backup would fail unless this entry is excluded.\n";
    }
    return kCliExitOperationFailed;
  }

  // 三个数字必须分开说，混起来就说不准：
  //   * included_count：完整实际备份遍历里的匹配数（全量，不是窗口里的）；
  //   * kPreviewEntryLimit：展示窗口的大小，单位是 preview entries（含被排除的
  //     与被剪枝的条目，遍历在窗口满了之后继续）；
  //   * listed_matching_count：窗口里真正列出来的匹配项数。
  // 旧的 Note 把前两个说成 "the first 300 of N matching item(s)"，等于宣称
  // 窗口装的是匹配项——当匹配项都排在窗口之外时它直接自相矛盾。
  std::size_t listed_matching_count = 0;
  for (const PreviewItem& item : preview.items) {
    if (item.included) ++listed_matching_count;
  }
  std::cout << "Preview: " << preview.included_count
            << " matching item(s) in the effective backup selection.\n";
  if (preview.truncated) {
    // 明确说出来，不静默截断。术语用"effective backup traversal"而不是
    // "the whole source tree"：被规则剪枝的子树不会递归，真实备份也一样。
    std::cout << "Note: showing matches found within the first "
              << kPreviewEntryLimit
              << " preview entries; the complete effective backup traversal "
                 "was validated, "
              << listed_matching_count << " matching item(s) listed below.\n";
  }
  for (const PreviewItem& item : preview.items) {
    if (!item.included) continue;
    std::cout << item.archive_path << '\n';
  }
  return kCliExitSuccess;
}

// ---- restore ----
//
// file_name 是仓库内的**单组件**名字，路径解析交给 BackupCatalog::Resolve：
// 拒绝空串、"."、".."、含 '/' 或 '\\'、内嵌 NUL、不以 .bak 结尾，并且要求
// 解析结果确实是仓库的直接子项、普通文件、非软链接。产品 CLI 不再接受任意
// 绝对路径——那同样属于测试夹具。
int RunRestoreCommand(const CliContext& context,
                      const std::vector<std::string>& arguments) {
  if (arguments.size() != 2) {
    std::cerr << "Error: 'restore' expects <file_name> and "
                 "<destination_directory>.\n\n";
    PrintCliUsage(context.program_name, std::cerr);
    return kCliExitUsageError;
  }
  const std::string file_name = arguments[0];
  const std::string destination_directory = arguments[1];

  std::string repository;
  std::string error_message;
  if (!LoadRepositoryPath(context, &repository, &error_message)) {
    PrintError(error_message);
    return kCliExitOperationFailed;
  }
  BackupCatalog catalog;
  std::string archive_path;
  if (!catalog.Resolve(repository, file_name, &archive_path, &error_message)) {
    PrintError(error_message);
    return kCliExitOperationFailed;
  }

  ArchiveFileInfo info;
  std::string identify_error;
  const bool wants_password =
      IdentifyArchiveFile(archive_path, &info, &identify_error) &&
      !info.password_hint.empty();

  RestoreOptions options;
  std::string secret;
  if (wants_password) {
    // 只有归档自己说"我需要密码"时才去问。问的方式还是 /dev/tty；
    // 识别失败时不在这里报错，交给真正的恢复路径给出更准确的诊断。
    if (!ReadSecretFromTerminal("Restore password: ", &secret,
                                &error_message)) {
      PrintError(error_message);
      return kCliExitOperationFailed;
    }
    options.password = secret;
  }

  // 先按 magic 分类再选路径：
  //
  //   * v2 container / BKPINC1 delta —— 依赖链入口。目标是一份完整快照时
  //     内部走的就是同一个恢复路径，行为与以前一致；是 delta 时自动把 base
  //     与中间层一起应用，用户只需要选 restore point。
  //   * 其它（legacy v0.1 等）—— 这些格式没有"链"的概念，走按 magic 分流的
  //     既有入口。它们的行为与 PR #17 一字不变：产品 CLI 一直能恢复历史 v0.1
  //     归档，这条能力不能因为新增了增量而消失。
  const SnapshotFileKind snapshot_kind =
      ClassifySnapshotFile(archive_path, nullptr);
  bool restored = false;
  RestoreReport report;
  if (snapshot_kind == SnapshotFileKind::kUnknown) {
    BackupEngine engine;
    restored = wants_password
                   ? engine.Restore(archive_path, destination_directory,
                                    options, nullptr, &error_message)
                   : engine.Restore(archive_path, destination_directory,
                                    &error_message);
  } else {
    restored =
        RestoreSnapshotChain(repository, file_name, destination_directory,
                             options, &report, &error_message);
  }
  for (char& character : secret) character = '\0';
  if (!restored) {
    PrintError(error_message);
    return kCliExitOperationFailed;
  }
  std::cout << "Restore completed successfully.\n";
  if (!report.notes.empty()) {
    // 尽力而为的步骤（例如非 root 改不了属主）如实说出来，不假装完整。
    std::cout << "Notes:      " << report.notes.size()
              << " (ownership or metadata steps were skipped)\n";
  }
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
  if (result.baseline_reset) {
    // 与 first_snapshot 分开报告：用户需要知道"这一轮为什么又建了一份"，
    // 而不是把它当成一次普通的变化。
    std::cout << "  note:      baseline reset (the recorded baseline no longer "
                 "matches a live snapshot)\n";
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
  // 这一次命令行上给的规则先收在这里，等选项全部解析完再决定怎么合并：
  // 这样 --clear-filters 写在 --include 的前面还是后面，结果都一样。
  std::vector<std::string> include_rules;
  std::vector<std::string> exclude_rules;
  bool clear_filters = false;
  // 单值选项各只能出现一次。--include / --exclude 是重复有意义的可重复选项，
  // --clear-filters 是幂等开关，三者都不在这里。
  bool saw_source = false;
  bool saw_interval = false;
  bool saw_retain = false;
  bool saw_strategy = false;
  bool saw_pack = false;
  bool saw_compression = false;
  bool saw_encryption = false;

  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const std::string option = arguments[index];
    std::string value;
    if (option == "--source") {
      if (!MarkSingleOption(&saw_source, option, &error)) {
        return UsageError(context, error);
      }
      if (!TakeValue(arguments, &index, option, &value, &error)) {
        return UsageError(context, error);
      }
      config.source_path = value;
      continue;
    }
    if (option == "--interval-minutes") {
      if (!MarkSingleOption(&saw_interval, option, &error)) {
        return UsageError(context, error);
      }
      if (!TakeValue(arguments, &index, option, &value, &error)) {
        return UsageError(context, error);
      }
      if (!ParseBoundedScheduleNumber(value, kMinIntervalMinutes,
                                      kMaxIntervalMinutes, option,
                                      &config.interval_minutes, &error)) {
        return UsageError(context, error);
      }
      continue;
    }
    if (option == "--retain") {
      if (!MarkSingleOption(&saw_retain, option, &error)) {
        return UsageError(context, error);
      }
      if (!TakeValue(arguments, &index, option, &value, &error)) {
        return UsageError(context, error);
      }
      if (!ParseBoundedScheduleNumber(value, kMinRetainCount, kMaxRetainCount,
                                      option, &config.retain_count, &error)) {
        return UsageError(context, error);
      }
      continue;
    }
    if (option == "--strategy") {
      // PR #18：计划也支持增量策略。解析失败不回退：写进配置的必须正是用户
      // 要的那一个，而"支不支持这个组合"由共享的 ValidateScheduleConfig
      // （也就是 IsSupportedBackupMode 那张真值表）在后面统一回答。
      if (!MarkSingleOption(&saw_strategy, option, &error)) {
        return UsageError(context, error);
      }
      if (!TakeValue(arguments, &index, option, &value, &error)) {
        return UsageError(context, error);
      }
      if (!ParseBackupStrategyKey(value, &config.strategy)) {
        return UsageError(context, "unknown backup strategy '" + value +
                                       "' (expected full or incremental)");
      }
      continue;
    }
    if (option == "--pack") {
      if (!MarkSingleOption(&saw_pack, option, &error)) {
        return UsageError(context, error);
      }
      if (!TakeValue(arguments, &index, option, &value, &error)) {
        return UsageError(context, error);
      }
      if (!ParsePackMethodKey(value, &config.pack_method)) {
        return UsageError(context, "unknown pack method '" + value + "'");
      }
      continue;
    }
    if (option == "--compression") {
      if (!MarkSingleOption(&saw_compression, option, &error)) {
        return UsageError(context, error);
      }
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
      if (!MarkSingleOption(&saw_encryption, option, &error)) {
        return UsageError(context, error);
      }
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
    if (option == "--include" || option == "--exclude") {
      if (!TakeValue(arguments, &index, option, &value, &error)) {
        return UsageError(context, error);
      }
      if (include_rules.size() + exclude_rules.size() >= kMaxScheduleRules) {
        return UsageError(context, "too many filter rules (limit " +
                                       std::to_string(kMaxScheduleRules) + ")");
      }
      if (option == "--include") {
        include_rules.push_back(value);
      } else {
        exclude_rules.push_back(value);
      }
      continue;
    }
    if (option == "--clear-filters") {
      clear_filters = true;
      continue;
    }
    return UsageError(context, "unknown option '" + option + "'");
  }

  // --clear-filters：先把**存下来的**规则整批丢掉，再把这次命令行上给的规则
  // 放进去。之前 CLI 只能不断 append，GUI 里删掉的规则在 CLI 侧永远清不掉。
  if (clear_filters) {
    config.include_rules.clear();
    config.exclude_rules.clear();
  }
  config.include_rules.insert(config.include_rules.end(), include_rules.begin(),
                              include_rules.end());
  config.exclude_rules.insert(config.exclude_rules.end(), exclude_rules.begin(),
                              exclude_rules.end());
  if (config.include_rules.size() + config.exclude_rules.size() >
      kMaxScheduleRules) {
    return UsageError(context, "too many filter rules (limit " +
                                   std::to_string(kMaxScheduleRules) + ")");
  }

  // 规则语法在这里就用真实的 Filter 校验一遍：配置里存的规则与命令行的规则
  // 走的是同一套解析，写不进去的规则也读不出来。
  if (!ValidateScheduleConfig(config, &error)) {
    return UsageError(context, error);
  }

  if (config.enabled) {
    // 已经启用的计划：**任何**一次修改之后都必须仍然"真的能跑"。
    // 否则用户会得到一份 enabled 但一跑就失败的配置，而失败要等到下一次
    // 定时触发才会暴露出来。校验不过就一个字节都不写，旧配置原样保留。
    std::string repository;
    if (!LoadRepositoryPath(context, &repository, &error) ||
        !ValidateScheduleForEnable(config, repository, &error)) {
      PrintError(error +
                 " The scheduled backup is still enabled, so the stored "
                 "configuration was left unchanged.");
      return kCliExitOperationFailed;
    }
  }

  document.config = config;
  if (!SaveScheduleDocument(context, document, &error)) {
    PrintError(error);
    return kCliExitOperationFailed;
  }
  std::cout << "Schedule updated. Enabled: " << (config.enabled ? "yes" : "no")
            << "\n";
  std::cout << "Strategy: " << BackupStrategyKey(config.strategy) << "\n";
  if (config.strategy == BackupStrategy::kIncremental &&
      !IsSupportedIncrementalPack(config.pack_method)) {
    // 增量只支持 MyPack：在配置这一层就说清楚，而不是等第一次 delta 才发现。
    std::cout << "Note: " << UnsupportedIncrementalPackReason() << "\n";
  }
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
  const bool was_enabled = document.config.enabled;
  document.config.enabled = enabled;
  if (enabled) {
    std::string repository;
    if (!LoadRepositoryPath(context, &repository, &error)) {
      // 没配仓库就"启用"是假启用：页面/命令会说 enabled，第一次到点却直接失败。
      PrintError(error);
      return kCliExitOperationFailed;
    }
    // enable 之前必须完整校验：源目录不存在、没配源、规则非法、仓库不可用，
    // 一律不许启用。用的是 GUI 调用的同一个函数。
    if (!ValidateScheduleForEnable(document.config, repository, &error)) {
      PrintError(error);
      return kCliExitOperationFailed;
    }
    // 首次启用把下一次运行排在一个完整周期之后：勾上"启用"下一秒就开跑是
    // 反直觉的，想立刻跑有明确入口（schedule run）。
    ApplyScheduleEnableTransition(&document, was_enabled, NowSeconds());
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

  // "立即运行"的前提是这份计划**确实被启用了**。以前这里会走到
  // EvaluateNow -> kDisabled 然后退出 0：脚本会把"其实什么都没做"当成成功，
  // 而 GUI 的 runNow() 在同样的情况下是明确的拒绝。同一件事两个前端给不同
  // 结论，正是本轮要收掉的边界，所以判断放在这里、结论是失败。
  {
    ScheduleDocument document;
    ScheduleLoadStatus status = ScheduleLoadStatus::kMissing;
    if (!LoadScheduleDocument(context, &document, &status, &error)) {
      PrintError(error);
      return kCliExitOperationFailed;
    }
    if (!document.config.enabled) {
      PrintError(
          "The scheduled backup is disabled, so there is nothing to run now. "
          "Enable it first: " +
          context.program_name + " schedule enable");
      return kCliExitOperationFailed;
    }
  }

  ScheduleStore store(context.schedule_file_path);
  SchedulerLock lock;
  if (!lock.Acquire(store.lock_file_path(), &error)) {
    PrintError(error);
    return kCliExitOperationFailed;
  }

  ScheduledBackupService service(repository, &store);
  ScheduleEvaluationResult result;
  const std::int64_t now = NowSeconds();
  // "立即检查并运行"：跳过"还没到点"，但仍然做真实的变化检测。
  if (!service.EvaluateNow(now, &result, &error)) {
    PrintError(error);
    return kCliExitOperationFailed;
  }
  PrintEvaluation(result, now);
  // 配置不合法（计划被挂起）与"这一轮失败"是两回事，但对命令的调用者来说
  // 都是"这次运行没有成功"，所以退出码相同：非 0。
  return (result.status == ScheduleEvaluationStatus::kFailed ||
          result.status == ScheduleEvaluationStatus::kConfigInvalid)
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
    // 每一轮真正评估**之前**重新读一次当前仓库。
    //
    // 长运行进程绝不能把仓库缓存一整个生命周期：另一个 CLI 或 GUI 执行
    // "config repository set B" 之后，这一轮必须写到 B 去，而不是继续往启动
    // 时那份配置里的旧仓库写。重读的代价只是一次小文件读取，而且只发生在
    // 真正要评估之前，不是每秒轮询。
    std::string current_repository;
    if (!LoadRepositoryPath(context, &current_repository, &error)) {
      PrintError(error);
      lock.Release();
      return kCliExitOperationFailed;
    }
    service.SetRepositoryPath(current_repository);

    ScheduleEvaluationResult result;
    const std::int64_t now = NowSeconds();
    if (!service.Evaluate(now, &result, &error)) {
      PrintError(error);
      return kCliExitOperationFailed;
    }
    if (result.status == ScheduleEvaluationStatus::kConfigInvalid) {
      // 落盘配置不合法 == 计划已挂起。继续 watch 只会每 30 秒重做一遍同样的
      // 完整校验，而且产品只允许一个进程——用户在 watch 运行期间根本没法去改
      // 那份文件。明确退出，把控制权交回用户。
      PrintEvaluation(result, now);
      PrintError(
          "The stored schedule configuration is not usable, so the scheduled "
          "backup is suspended. Nothing was written. Fix it with "
          "'" +
          context.program_name +
          " schedule set ...' and start watching again.");
      lock.Release();
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

  const bool known = subcommand == "show" || subcommand == "set" ||
                     subcommand == "enable" || subcommand == "disable" ||
                     subcommand == "run" || subcommand == "history" ||
                     subcommand == "watch";
  if (!known) {
    return UsageError(context,
                      "unknown schedule subcommand '" + subcommand + "'");
  }
  // 每个子命令都必须**明确消费全部 argv**。除了 set（它自己解析 rest），
  // 其余子命令都是零参数：'schedule show extra' 静默忽略 extra 会让脚本里
  // 一个拼错的参数看起来完全成功，而用户以为自己换了一种运行方式。
  if (subcommand != "set" && !rest.empty()) {
    return UsageError(context, "'schedule " + subcommand +
                                   "' does not take any argument, but got '" +
                                   rest[0] + "'");
  }

  if (subcommand == "show") return ScheduleShow(context);
  if (subcommand == "set") return ScheduleSet(context, rest);
  if (subcommand == "enable") return ScheduleToggle(context, true);
  if (subcommand == "disable") return ScheduleToggle(context, false);
  if (subcommand == "run") return ScheduleRun(context);
  if (subcommand == "history") return ScheduleHistory(context);
  return ScheduleWatch(context);
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
  if (arguments[0] == "list") {
    if (arguments.size() != 1) {
      return UsageError(context,
                        "'repository list' does not take any argument, but got "
                        "'" +
                            arguments[1] + "'");
    }
    return RepositoryList(context);
  }
  if (arguments[0] == "delete") {
    if (arguments.size() < 2) {
      return UsageError(context,
                        "'repository delete' expects exactly one <file_name>");
    }
    if (arguments.size() > 2) {
      return UsageError(context,
                        "'repository delete' expects exactly one <file_name>, "
                        "but got " +
                            std::to_string(arguments.size() - 1) + " of them");
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
  if (arguments[0] == "repository" && arguments.size() >= 2) {
    if (arguments[1] == "show") {
      if (arguments.size() != 2) {
        return UsageError(context,
                          "'config repository show' does not take any "
                          "argument, but got '" +
                              arguments[2] + "'");
      }
      return ConfigRepositoryShow(context);
    }
    if (arguments[1] == "set") {
      if (arguments.size() < 3) {
        return UsageError(context, "'config repository set' expects <path>");
      }
      if (arguments.size() > 3) {
        return UsageError(context,
                          "'config repository set' expects exactly one <path>, "
                          "but got " +
                              std::to_string(arguments.size() - 2) +
                              " of them");
      }
      return ConfigRepositorySet(context, arguments[2]);
    }
    return UsageError(
        context, "unknown config repository subcommand '" + arguments[1] + "'");
  }
  return UsageError(context,
                    "'config' expects 'repository show' or "
                    "'repository set <path>'");
}

}  // namespace backupproject
