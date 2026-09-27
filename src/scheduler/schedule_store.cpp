// schedule_store.cpp

#include "schedule_store.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "backup_catalog.h"
#include "backup_option_keys.h"
#include "file_io.h"
#include "simple_json.h"

namespace backupproject {
namespace {

void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) *error_message = text;
}

std::string Describe(int error_number, const std::string& action,
                     const std::string& path) {
  return action + ": " + path + ": " + std::strerror(error_number);
}

bool ContainsNul(const std::string& value) {
  return value.find('\0') != std::string::npos;
}

// 本文件自己的父目录解析。"a" -> "."、"/a" -> "/"、"/" -> "/"。
//
// 刻意不共用 file_io.h 里那个同名函数：它在"没有 '/' 时返回空串"，语义与
// 这里不同（空串会让 mkdir -p 走到当前目录之外的判断上）。真正共享的是
// 原子的文件替换与目录创建入口，不是这个六行的字符串切分。
std::string ParentDirectoryOf(const std::string& path) {
  const std::size_t slash = path.rfind('/');
  if (slash == std::string::npos) return std::string(".");
  if (slash == 0) return std::string("/");
  return path.substr(0, slash);
}

// 递归创建目录，等价于 mkdir -p。
//
// 什么时候会真的用到它：用户把 --schedule-file 指到一个还不存在的深层路径
// （首次运行、或者应用配置目录整个被清空过）。此时"保存计划"不该因为父目录
// 不在就失败——那是保存路径的问题，不是用户的输入错误。
//
// 目录权限 0700：里面放的是 schedule.json + manifest，前者已经是 0600，
// 目录没有理由是 0755 让同机器上任何人都能列出来。
bool MakeDirectories(const std::string& path, std::string* error_message) {
  if (path.empty() || path == "." || path == "/") return true;

  struct stat status;
  if (::lstat(path.c_str(), &status) == 0) {
    if (S_ISDIR(status.st_mode)) return true;
    SetError(error_message,
             "Cannot create the schedule directory: not a directory: " + path);
    return false;
  }
  if (errno != ENOENT) {
    SetError(error_message, Describe(errno, "Failed to inspect", path));
    return false;
  }
  if (!MakeDirectories(ParentDirectoryOf(path), error_message)) return false;
  if (::mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) {
    SetError(error_message,
             Describe(errno, "Failed to create the schedule directory", path));
    return false;
  }
  return true;
}

bool ReadWholeFile(const std::string& path, std::size_t maximum_bytes,
                   std::string* data, bool* missing,
                   std::string* error_message) {
  data->clear();
  *missing = false;

  struct stat status;
  if (::lstat(path.c_str(), &status) != 0) {
    if (errno == ENOENT) {
      *missing = true;
      return true;
    }
    SetError(error_message, Describe(errno, "Failed to inspect", path));
    return false;
  }
  if (!S_ISREG(status.st_mode)) {
    SetError(error_message, "Not a regular file: " + path);
    return false;
  }
  if (static_cast<std::uint64_t>(status.st_size) > maximum_bytes) {
    SetError(error_message, "File is too large to read: " + path + " (" +
                                std::to_string(status.st_size) + " bytes)");
    return false;
  }

  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    SetError(error_message, Describe(errno, "Failed to open", path));
    return false;
  }
  std::string contents;
  contents.reserve(static_cast<std::size_t>(status.st_size));
  char buffer[65536];
  while (true) {
    const ssize_t count = ::read(fd, buffer, sizeof(buffer));
    if (count < 0) {
      if (errno == EINTR) continue;
      const int saved_errno = errno;
      ::close(fd);
      SetError(error_message, Describe(saved_errno, "Failed to read", path));
      return false;
    }
    if (count == 0) break;
    contents.append(buffer, static_cast<std::size_t>(count));
    if (contents.size() > maximum_bytes) {
      ::close(fd);
      SetError(error_message, "File is too large to read: " + path);
      return false;
    }
  }
  ::close(fd);
  *data = std::move(contents);
  return true;
}

// 与 BackupCatalog 的规则保持一致：不含 '/' 与 '\\'、不是 "." / ".."、
// 不含 NUL、以 .bak 结尾。Catalog 仍然会在 Resolve / Delete 时再校验一次，
// 这里提前拦是为了"一个坏掉的 state 不会变成一串奇怪的系统调用"。
bool IsSingleComponentArchiveName(const std::string& file_name) {
  if (file_name.empty()) return false;
  if (file_name == "." || file_name == "..") return false;
  if (file_name.find('/') != std::string::npos) return false;
  if (file_name.find('\\') != std::string::npos) return false;
  if (ContainsNul(file_name)) return false;
  if (file_name.size() <= 4) return false;
  return file_name.compare(file_name.size() - 4, 4, ".bak") == 0;
}

bool IsBoundedString(const std::string& value, std::size_t maximum) {
  return value.size() <= maximum && !ContainsNul(value);
}

}  // namespace

bool BuildScheduleFilter(const ScheduleConfig& config, Filter* filter,
                         std::string* error_message) {
  if (filter == nullptr) {
    SetError(error_message, "Filter output must not be null");
    return false;
  }
  if (config.include_rules.size() + config.exclude_rules.size() >
      kMaxScheduleRules) {
    SetError(error_message, "Too many schedule filter rules: " +
                                std::to_string(config.include_rules.size() +
                                               config.exclude_rules.size()));
    return false;
  }
  for (const std::string& rule : config.include_rules) {
    if (!filter->AddRule(FilterAction::kInclude, rule, error_message)) {
      return false;
    }
  }
  for (const std::string& rule : config.exclude_rules) {
    if (!filter->AddRule(FilterAction::kExclude, rule, error_message)) {
      return false;
    }
  }
  return true;
}

bool ValidateScheduleConfig(const ScheduleConfig& config,
                            std::string* error_message) {
  if (!IsSupportedBackupMode(config.trigger, config.strategy)) {
    SetError(error_message,
             UnsupportedBackupModeReason(config.trigger, config.strategy));
    return false;
  }
  if (config.interval_minutes < kMinIntervalMinutes ||
      config.interval_minutes > kMaxIntervalMinutes) {
    SetError(error_message, "Schedule interval must be between " +
                                std::to_string(kMinIntervalMinutes) + " and " +
                                std::to_string(kMaxIntervalMinutes) +
                                " minutes, got " +
                                std::to_string(config.interval_minutes));
    return false;
  }
  if (config.retain_count < kMinRetainCount ||
      config.retain_count > kMaxRetainCount) {
    SetError(error_message, "Schedule retain count must be between " +
                                std::to_string(kMinRetainCount) + " and " +
                                std::to_string(kMaxRetainCount) + ", got " +
                                std::to_string(config.retain_count));
    return false;
  }
  if (config.encryption_method != EncryptionMethod::kNone) {
    // 无人值守的定时任务没有安全的持久密钥来源，所以本版本一律拒绝。
    // 明确拒绝而不是"存下来但运行时报错"，也不是静默降级成不加密。
    SetError(
        error_message,
        "Unattended scheduled encryption is not supported: "
        "定时无人值守加密需要安全的密钥来源；当前版本不会持久化明文密码。");
    return false;
  }
  if (!IsBoundedString(config.source_path, kMaxScheduleStringBytes)) {
    SetError(error_message,
             "Schedule source path is not usable (too long or contains a NUL "
             "byte)");
    return false;
  }
  for (const std::string& rule : config.include_rules) {
    if (!IsBoundedString(rule, kMaxScheduleStringBytes)) {
      SetError(error_message,
               "A schedule include rule is not usable (too long "
               "or contains a NUL byte)");
      return false;
    }
  }
  for (const std::string& rule : config.exclude_rules) {
    if (!IsBoundedString(rule, kMaxScheduleStringBytes)) {
      SetError(error_message,
               "A schedule exclude rule is not usable (too long "
               "or contains a NUL byte)");
      return false;
    }
  }
  Filter filter;
  return BuildScheduleFilter(config, &filter, error_message);
}

namespace {

// 一个"必须已经存在、必须是真实目录、且不能是软链接"的路径。
//
// 两条路径（源目录、仓库）用的是同一段判断，报错文案只换主语：绝不写两遍，
// 免得某一天只修好了其中一份。
bool RequireRealDirectory(const std::string& path, const char* what,
                          std::string* error_message) {
  if (path.empty()) {
    SetError(error_message, std::string(what) + " must be set before enabling");
    return false;
  }
  struct stat status;
  if (::lstat(path.c_str(), &status) != 0) {
    SetError(error_message, Describe(errno, what, path));
    return false;
  }
  if (S_ISLNK(status.st_mode)) {
    SetError(error_message,
             std::string(what) + " must not be a symbolic link: " + path);
    return false;
  }
  if (!S_ISDIR(status.st_mode)) {
    SetError(error_message, std::string(what) + " is not a directory: " + path);
    return false;
  }
  return true;
}

}  // namespace

bool ValidateScheduleForEnable(const ScheduleConfig& config,
                               const std::string& repository_path,
                               std::string* error_message) {
  if (!ValidateScheduleConfig(config, error_message)) return false;

  // 仓库先查：仓库没配好时，"源目录也不对"不是用户现在最需要看到的信息。
  if (repository_path.empty()) {
    SetError(error_message,
             "No backup repository is configured. Set one before enabling the "
             "scheduled backup");
    return false;
  }
  if (!RequireRealDirectory(repository_path, "The backup repository",
                            error_message)) {
    return false;
  }
  return RequireRealDirectory(config.source_path, "Schedule source directory",
                              error_message);
}

const char* ScheduleRunResultKey(ScheduleRunResult result) {
  switch (result) {
    case ScheduleRunResult::kSuccessCreated:
      return "success_created";
    case ScheduleRunResult::kSkippedNoChanges:
      return "skipped_no_changes";
    case ScheduleRunResult::kFailed:
      return "failed";
    case ScheduleRunResult::kSuccessWithRetentionWarning:
      return "success_with_retention_warning";
  }
  return "failed";
}

const char* ScheduleRunResultText(ScheduleRunResult result) {
  switch (result) {
    case ScheduleRunResult::kSuccessCreated:
      return "Created a new full snapshot";
    case ScheduleRunResult::kSkippedNoChanges:
      return "Skipped: no changes since the last snapshot";
    case ScheduleRunResult::kFailed:
      return "Failed";
    case ScheduleRunResult::kSuccessWithRetentionWarning:
      return "Snapshot created, but retention could not delete an old one";
  }
  return "Failed";
}

bool ParseScheduleRunResultKey(const std::string& key,
                               ScheduleRunResult* result) {
  if (result == nullptr) return false;
  if (key == "success_created") {
    *result = ScheduleRunResult::kSuccessCreated;
    return true;
  }
  if (key == "skipped_no_changes") {
    *result = ScheduleRunResult::kSkippedNoChanges;
    return true;
  }
  if (key == "failed") {
    *result = ScheduleRunResult::kFailed;
    return true;
  }

  if (key == "success_with_retention_warning") {
    *result = ScheduleRunResult::kSuccessWithRetentionWarning;
    return true;
  }
  return false;
}

// ---- 序列化 ----

namespace {

void AppendIndent(std::string* out, int depth) {
  out->append(static_cast<std::size_t>(depth) * 2, ' ');
}

void AppendNumberField(std::string* out, const char* name, std::uint64_t value,
                       bool last) {
  *out += '"';
  *out += name;
  *out += "\": ";
  *out += std::to_string(value);
  *out += last ? "\n" : ",\n";
}

void AppendSignedField(std::string* out, const char* name, std::int64_t value,
                       bool last) {
  *out += '"';
  *out += name;
  *out += "\": ";
  *out += std::to_string(value);
  *out += last ? "\n" : ",\n";
}

void AppendStringField(std::string* out, const char* name,
                       const std::string& value, bool last) {
  *out += '"';
  *out += name;
  *out += "\": ";
  WriteJsonString(out, value);
  *out += last ? "\n" : ",\n";
}

void AppendBoolField(std::string* out, const char* name, bool value,
                     bool last) {
  *out += '"';
  *out += name;
  *out += "\": ";
  *out += value ? "true" : "false";
  *out += last ? "\n" : ",\n";
}

void AppendStringArrayField(std::string* out, const char* name,
                            const std::vector<std::string>& values, int depth,
                            bool last) {
  AppendIndent(out, depth);
  *out += '"';
  *out += name;
  *out += "\": [";
  if (values.empty()) {
    *out += last ? "]\n" : "],\n";
    return;
  }
  *out += '\n';
  for (std::size_t index = 0; index < values.size(); ++index) {
    AppendIndent(out, depth + 1);
    WriteJsonString(out, values[index]);
    *out += (index + 1 == values.size()) ? "\n" : ",\n";
  }
  AppendIndent(out, depth);
  *out += last ? "]\n" : "],\n";
}

void AppendChangeFields(std::string* out, const ChangeSummary& changes,
                        int depth) {
  AppendIndent(out, depth);
  AppendNumberField(out, "added", changes.added, false);
  AppendIndent(out, depth);
  AppendNumberField(out, "removed", changes.removed, false);
  AppendIndent(out, depth);
  AppendNumberField(out, "modified", changes.modified, false);
  AppendIndent(out, depth);
  AppendNumberField(out, "metadata_changed", changes.metadata_changed, false);
}

void AppendSnapshotField(std::string* out,
                         const ScheduledSnapshotRecord& record, int depth,
                         bool last) {
  AppendIndent(out, depth);
  out->append("{\n");
  AppendIndent(out, depth + 1);
  AppendStringField(out, "file_name", record.file_name, false);
  AppendIndent(out, depth + 1);
  AppendSignedField(out, "created_time_sec", record.created_time_sec, false);
  AppendChangeFields(out, record.changes, depth + 1);
  AppendIndent(out, depth + 1);
  AppendNumberField(out, "entry_count", record.entry_count, false);
  AppendIndent(out, depth + 1);
  AppendNumberField(out, "archive_size", record.archive_size, false);
  AppendIndent(out, depth + 1);
  AppendStringField(out, "pack", PackMethodKey(record.pack_method), false);
  AppendIndent(out, depth + 1);
  AppendStringField(out, "compression",
                    CompressionMethodKey(record.compression_method), true);
  AppendIndent(out, depth);
  *out += last ? "}\n" : "},\n";
}

void AppendHistoryField(std::string* out, const ScheduleHistoryEntry& entry,
                        int depth, bool last) {
  AppendIndent(out, depth);
  out->append("{\n");
  AppendIndent(out, depth + 1);
  AppendSignedField(out, "scheduled_at_sec", entry.scheduled_at_sec, false);
  AppendIndent(out, depth + 1);
  AppendSignedField(out, "started_at_sec", entry.started_at_sec, false);
  AppendIndent(out, depth + 1);
  AppendSignedField(out, "finished_at_sec", entry.finished_at_sec, false);
  AppendIndent(out, depth + 1);
  AppendStringField(out, "result", ScheduleRunResultKey(entry.result), false);
  AppendIndent(out, depth + 1);
  AppendStringField(out, "archive_file_name", entry.archive_file_name, false);
  AppendChangeFields(out, entry.changes, depth + 1);
  AppendIndent(out, depth + 1);
  AppendStringField(out, "diagnostic", entry.diagnostic, true);
  AppendIndent(out, depth);
  *out += last ? "}\n" : "},\n";
}

}  // namespace

std::string SerializeScheduleDocument(const ScheduleDocument& document) {
  std::string out;
  out += "{\n";
  AppendIndent(&out, 1);
  AppendNumberField(&out, "version", 1, false);

  AppendIndent(&out, 1);
  out += "\"config\": {\n";
  AppendIndent(&out, 2);
  AppendBoolField(&out, "enabled", document.config.enabled, false);
  AppendIndent(&out, 2);
  AppendStringField(&out, "trigger", BackupTriggerKey(document.config.trigger),
                    false);
  AppendIndent(&out, 2);
  AppendStringField(&out, "strategy",
                    BackupStrategyKey(document.config.strategy), false);
  AppendIndent(&out, 2);
  AppendStringField(&out, "source_path", document.config.source_path, false);
  AppendIndent(&out, 2);
  AppendNumberField(&out, "interval_minutes", document.config.interval_minutes,
                    false);
  AppendIndent(&out, 2);
  AppendNumberField(&out, "retain_count", document.config.retain_count, false);
  AppendIndent(&out, 2);
  AppendStringField(&out, "pack", PackMethodKey(document.config.pack_method),
                    false);
  AppendIndent(&out, 2);
  AppendStringField(&out, "compression",
                    CompressionMethodKey(document.config.compression_method),
                    false);
  AppendIndent(&out, 2);
  AppendStringField(&out, "encryption",
                    EncryptionMethodKey(document.config.encryption_method),
                    false);
  AppendStringArrayField(&out, "include_rules", document.config.include_rules,
                         2, false);
  AppendStringArrayField(&out, "exclude_rules", document.config.exclude_rules,
                         2, true);
  AppendIndent(&out, 1);
  out += "},\n";

  AppendIndent(&out, 1);
  out += "\"state\": {\n";
  AppendIndent(&out, 2);
  AppendSignedField(&out, "next_run_time_sec", document.state.next_run_time_sec,
                    false);
  AppendIndent(&out, 2);
  AppendSignedField(&out, "last_success_time_sec",
                    document.state.last_success_time_sec, false);
  AppendIndent(&out, 2);
  AppendNumberField(&out, "last_manifest_entry_count",
                    document.state.last_manifest_entry_count, false);

  // baseline 三件套总是写出来（空串也写）。读侧把它们当成可选字段，
  // 所以旧版本写出的、没有这三行的 schedule.json 仍然读得进来。
  AppendIndent(&out, 2);
  AppendStringField(&out, "baseline_snapshot_file_name",
                    document.state.baseline.snapshot_file_name, false);
  AppendIndent(&out, 2);
  AppendStringField(&out, "baseline_repository_identity",
                    document.state.baseline.repository_identity, false);
  AppendIndent(&out, 2);
  AppendStringField(&out, "baseline_source_path",
                    document.state.baseline.source_path, false);

  AppendIndent(&out, 2);
  out += "\"managed_snapshots\": [";
  if (document.state.managed_snapshots.empty()) {
    out += "],\n";
  } else {
    out += '\n';
    for (std::size_t index = 0; index < document.state.managed_snapshots.size();
         ++index) {
      AppendSnapshotField(&out, document.state.managed_snapshots[index], 3,
                          index + 1 == document.state.managed_snapshots.size());
    }
    AppendIndent(&out, 2);
    out += "],\n";
  }

  AppendIndent(&out, 2);
  out += "\"history\": [";
  if (document.state.history.empty()) {
    out += "]\n";
  } else {
    out += '\n';
    for (std::size_t index = 0; index < document.state.history.size();
         ++index) {
      AppendHistoryField(&out, document.state.history[index], 3,
                         index + 1 == document.state.history.size());
    }
    AppendIndent(&out, 2);
    out += "]\n";
  }

  AppendIndent(&out, 1);
  out += "}\n";
  out += "}\n";
  return out;
}

// ---- 解析 ----
//
// 字段集合是"完全相等"的：少一个报 missing，多一个报 unknown。
// 现场手改出来的、我们不认识的字段不会被静默忽略。

namespace {

const std::vector<const char*> kRootFields = {"version", "config", "state"};
const std::vector<const char*> kConfigFields = {
    "enabled",          "trigger",       "strategy",     "source_path",
    "interval_minutes", "retain_count",  "pack",         "compression",
    "encryption",       "include_rules", "exclude_rules"};
const std::vector<const char*> kStateFields = {
    "next_run_time_sec", "last_success_time_sec", "last_manifest_entry_count",
    "managed_snapshots", "history"};
// baseline 是在 review-fix 这一轮追加的。上一版写出的 schedule.json 没有这
// 三个字段，它必须仍然读得进来（缺 baseline = 不知道 manifest 属于哪份快照
// = 下一轮重建基线快照，语义上恰好就是安全的那个默认）。但"不在这两个列表
// 里的 key"照样报 unknown，"kStateFields 里少一个"照样报 missing。
const std::vector<const char*> kStateOptionalFields = {
    "baseline_snapshot_file_name", "baseline_repository_identity",
    "baseline_source_path"};
const std::vector<const char*> kSnapshotFields = {
    "file_name", "created_time_sec", "added",       "removed",
    "modified",  "metadata_changed", "entry_count", "archive_size",
    "pack",      "compression"};
const std::vector<const char*> kHistoryFields = {
    "scheduled_at_sec",  "started_at_sec", "finished_at_sec", "result",
    "archive_file_name", "added",          "removed",         "modified",
    "metadata_changed",  "diagnostic"};

bool BadField(const std::string& what, const char* key,
              const std::string& detail, std::string* error_message) {
  SetError(error_message, "Invalid schedule store: " + what + ": field '" +
                              key + "' " + detail);
  return false;
}

// 可选字符串字段：完全不出现就保持默认（空串），出现了就必须是合法字符串。
// 只有 kStateOptionalFields 里的字段会走这里。
bool RequireOptionalString(const JsonValue& object, const char* key,
                           const std::string& what, std::string* out,
                           std::string* error_message) {
  if (object.Find(key) == nullptr) return true;
  if (!RequireString(object, key, what, out, error_message)) return false;
  if (!IsBoundedString(*out, kMaxScheduleStringBytes)) {
    return BadField(what, key, "is unusable (too long or contains a NUL byte)",
                    error_message);
  }
  return true;
}

bool ParseChangeFields(const JsonValue& object, const std::string& what,
                       ChangeSummary* changes, std::string* error_message) {
  if (!RequireUint64(object, "added", what, kMaxManifestEntries,
                     &changes->added, error_message)) {
    return false;
  }
  if (!RequireUint64(object, "removed", what, kMaxManifestEntries,
                     &changes->removed, error_message)) {
    return false;
  }
  if (!RequireUint64(object, "modified", what, kMaxManifestEntries,
                     &changes->modified, error_message)) {
    return false;
  }
  return RequireUint64(object, "metadata_changed", what, kMaxManifestEntries,
                       &changes->metadata_changed, error_message);
}

bool ParseConfig(const JsonValue& root, ScheduleConfig* config,
                 std::string* error_message) {
  const JsonValue* object = nullptr;
  if (!RequireObject(root.Find("config"), "config", &object, error_message)) {
    return false;
  }
  if (!RequireExactFields(*object, kConfigFields, "config", error_message)) {
    return false;
  }
  if (!RequireBool(*object, "enabled", "config", &config->enabled,
                   error_message)) {
    return false;
  }

  std::string trigger_key;
  if (!RequireString(*object, "trigger", "config", &trigger_key,
                     error_message)) {
    return false;
  }
  if (!ParseBackupTriggerKey(trigger_key, &config->trigger)) {
    return BadField("config", "trigger",
                    "is not a known trigger: '" + trigger_key + "'",
                    error_message);
  }
  std::string strategy_key;
  if (!RequireString(*object, "strategy", "config", &strategy_key,
                     error_message)) {
    return false;
  }
  if (!ParseBackupStrategyKey(strategy_key, &config->strategy)) {
    return BadField("config", "strategy",
                    "is not a known strategy: '" + strategy_key + "'",
                    error_message);
  }
  if (!RequireString(*object, "source_path", "config", &config->source_path,
                     error_message)) {
    return false;
  }
  if (!RequireUint32(*object, "interval_minutes", "config", kMinIntervalMinutes,
                     kMaxIntervalMinutes, &config->interval_minutes,
                     error_message)) {
    return false;
  }
  if (!RequireUint32(*object, "retain_count", "config", kMinRetainCount,
                     kMaxRetainCount, &config->retain_count, error_message)) {
    return false;
  }

  std::string pack_key;
  if (!RequireString(*object, "pack", "config", &pack_key, error_message)) {
    return false;
  }
  if (!ParsePackMethodKey(pack_key, &config->pack_method)) {
    return BadField("config", "pack",
                    "is not a known pack method: '" + pack_key + "'",
                    error_message);
  }
  std::string compression_key;
  if (!RequireString(*object, "compression", "config", &compression_key,
                     error_message)) {
    return false;
  }
  if (!ParseCompressionMethodKey(compression_key,
                                 &config->compression_method)) {
    return BadField(
        "config", "compression",
        "is not a known compression method: '" + compression_key + "'",
        error_message);
  }
  std::string encryption_key;
  if (!RequireString(*object, "encryption", "config", &encryption_key,
                     error_message)) {
    return false;
  }
  // 这里只解析，不做"必须等于 none"的产品判断——那是 ValidateScheduleConfig
  // 的职责。分开之后 schedule show 还能把一份非法配置原样显示出来。
  if (!ParseEncryptionMethodKey(encryption_key, &config->encryption_method)) {
    return BadField(
        "config", "encryption",
        "is not a known encryption method: '" + encryption_key + "'",
        error_message);
  }

  if (!RequireStringArray(*object, "include_rules", "config",
                          &config->include_rules, error_message)) {
    return false;
  }
  if (!RequireStringArray(*object, "exclude_rules", "config",
                          &config->exclude_rules, error_message)) {
    return false;
  }
  if (config->include_rules.size() + config->exclude_rules.size() >
      kMaxScheduleRules) {
    return BadField("config", "include_rules",
                    "contains too many rules (limit " +
                        std::to_string(kMaxScheduleRules) + ")",
                    error_message);
  }
  for (const std::string& rule : config->include_rules) {
    if (!IsBoundedString(rule, kMaxScheduleStringBytes)) {
      return BadField("config", "include_rules",
                      "contains an unusable rule (too long or with a NUL byte)",
                      error_message);
    }
  }
  for (const std::string& rule : config->exclude_rules) {
    if (!IsBoundedString(rule, kMaxScheduleStringBytes)) {
      return BadField("config", "exclude_rules",
                      "contains an unusable rule (too long or with a NUL byte)",
                      error_message);
    }
  }
  if (!IsBoundedString(config->source_path, kMaxScheduleStringBytes)) {
    return BadField("config", "source_path",
                    "is unusable (too long or contains a NUL byte)",
                    error_message);
  }
  return true;
}

bool ParseSnapshot(const JsonValue& value, const std::string& what,
                   ScheduledSnapshotRecord* record,
                   std::string* error_message) {
  if (!value.is_object()) {
    SetError(error_message,
             "Invalid schedule store: " + what + ": must be an object");
    return false;
  }
  if (!RequireExactFields(value, kSnapshotFields, what, error_message)) {
    return false;
  }
  if (!RequireString(value, "file_name", what, &record->file_name,
                     error_message)) {
    return false;
  }
  if (!IsSingleComponentArchiveName(record->file_name)) {
    return BadField(
        what, "file_name",
        "must be a single .bak file name: '" + record->file_name + "'",
        error_message);
  }
  if (!RequireInt64(value, "created_time_sec", what, &record->created_time_sec,
                    error_message)) {
    return false;
  }
  if (!ParseChangeFields(value, what, &record->changes, error_message)) {
    return false;
  }
  if (!RequireUint64(value, "entry_count", what, 1ull << 62,
                     &record->entry_count, error_message)) {
    return false;
  }
  if (!RequireUint64(value, "archive_size", what, 1ull << 62,
                     &record->archive_size, error_message)) {
    return false;
  }
  std::string pack_key;
  if (!RequireString(value, "pack", what, &pack_key, error_message)) {
    return false;
  }
  if (!ParsePackMethodKey(pack_key, &record->pack_method)) {
    return BadField(what, "pack",
                    "is not a known pack method: '" + pack_key + "'",
                    error_message);
  }
  std::string compression_key;
  if (!RequireString(value, "compression", what, &compression_key,
                     error_message)) {
    return false;
  }
  if (!ParseCompressionMethodKey(compression_key,
                                 &record->compression_method)) {
    return BadField(
        what, "compression",
        "is not a known compression method: '" + compression_key + "'",
        error_message);
  }
  return true;
}

bool ParseHistoryEntry(const JsonValue& value, const std::string& what,
                       ScheduleHistoryEntry* entry,
                       std::string* error_message) {
  if (!value.is_object()) {
    SetError(error_message,
             "Invalid schedule store: " + what + ": must be an object");
    return false;
  }
  if (!RequireExactFields(value, kHistoryFields, what, error_message)) {
    return false;
  }
  if (!RequireInt64(value, "scheduled_at_sec", what, &entry->scheduled_at_sec,
                    error_message)) {
    return false;
  }
  if (!RequireInt64(value, "started_at_sec", what, &entry->started_at_sec,
                    error_message)) {
    return false;
  }
  if (!RequireInt64(value, "finished_at_sec", what, &entry->finished_at_sec,
                    error_message)) {
    return false;
  }
  std::string result_key;
  if (!RequireString(value, "result", what, &result_key, error_message)) {
    return false;
  }
  if (!ParseScheduleRunResultKey(result_key, &entry->result)) {
    return BadField(what, "result",
                    "is not a known run result: '" + result_key + "'",
                    error_message);
  }
  if (!RequireString(value, "archive_file_name", what,
                     &entry->archive_file_name, error_message)) {
    return false;
  }
  if (!entry->archive_file_name.empty() &&
      !IsSingleComponentArchiveName(entry->archive_file_name)) {
    return BadField(what, "archive_file_name",
                    "must be empty or a single .bak file name: '" +
                        entry->archive_file_name + "'",
                    error_message);
  }
  if (!ParseChangeFields(value, what, &entry->changes, error_message)) {
    return false;
  }
  if (!RequireString(value, "diagnostic", what, &entry->diagnostic,
                     error_message)) {
    return false;
  }
  if (!IsBoundedString(entry->diagnostic, kMaxScheduleStringBytes)) {
    return BadField(what, "diagnostic",
                    "is unusable (too long or contains a NUL byte)",
                    error_message);
  }
  return true;
}

bool ParseState(const JsonValue& root, ScheduleState* state,
                std::string* error_message) {
  const JsonValue* object = nullptr;
  if (!RequireObject(root.Find("state"), "state", &object, error_message)) {
    return false;
  }
  if (!RequireExactFields(*object, kStateFields, kStateOptionalFields, "state",
                          error_message)) {
    return false;
  }
  if (!RequireInt64(*object, "next_run_time_sec", "state",
                    &state->next_run_time_sec, error_message)) {
    return false;
  }
  if (!RequireInt64(*object, "last_success_time_sec", "state",
                    &state->last_success_time_sec, error_message)) {
    return false;
  }
  if (!RequireUint64(*object, "last_manifest_entry_count", "state",
                     kMaxManifestEntries, &state->last_manifest_entry_count,
                     error_message)) {
    return false;
  }

  // baseline 三件套：全部可选，缺失就是空串（= 没有记录过 baseline）。
  // file_name 存在时必须是合法的单组件归档名——它是从仓库里 Resolve 出来的
  // 依据，绝不允许出现带 '/' 或 ".." 的东西。
  if (!RequireOptionalString(*object, "baseline_snapshot_file_name", "state",
                             &state->baseline.snapshot_file_name,
                             error_message)) {
    return false;
  }
  if (!state->baseline.snapshot_file_name.empty() &&
      !IsSingleComponentArchiveName(state->baseline.snapshot_file_name)) {
    return BadField("state", "baseline_snapshot_file_name",
                    "is not a plain .bak file name", error_message);
  }
  if (!RequireOptionalString(*object, "baseline_repository_identity", "state",
                             &state->baseline.repository_identity,
                             error_message)) {
    return false;
  }
  if (!RequireOptionalString(*object, "baseline_source_path", "state",
                             &state->baseline.source_path, error_message)) {
    return false;
  }

  const JsonValue* snapshots = nullptr;
  if (!RequireArray(*object, "managed_snapshots", "state", &snapshots,
                    error_message)) {
    return false;
  }
  if (snapshots->array.size() > kMaxRetainCount) {
    return BadField("state", "managed_snapshots",
                    "holds more records than the maximum retain count (" +
                        std::to_string(kMaxRetainCount) + ")",
                    error_message);
  }
  state->managed_snapshots.clear();
  state->managed_snapshots.reserve(snapshots->array.size());
  for (std::size_t index = 0; index < snapshots->array.size(); ++index) {
    ScheduledSnapshotRecord record;
    const std::string what =
        "state.managed_snapshots[" + std::to_string(index) + "]";
    if (!ParseSnapshot(snapshots->array[index], what, &record, error_message)) {
      return false;
    }
    state->managed_snapshots.push_back(std::move(record));
  }

  const JsonValue* history = nullptr;
  if (!RequireArray(*object, "history", "state", &history, error_message)) {
    return false;
  }
  if (history->array.size() > kMaxHistoryEntries) {
    return BadField("state", "history",
                    "holds more entries than the bound (" +
                        std::to_string(kMaxHistoryEntries) + ")",
                    error_message);
  }
  state->history.clear();
  state->history.reserve(history->array.size());
  for (std::size_t index = 0; index < history->array.size(); ++index) {
    ScheduleHistoryEntry entry;
    const std::string what = "state.history[" + std::to_string(index) + "]";
    if (!ParseHistoryEntry(history->array[index], what, &entry,
                           error_message)) {
      return false;
    }
    state->history.push_back(std::move(entry));
  }
  return true;
}

bool ParseDocument(const std::string& text, ScheduleDocument* document,
                   std::string* error_message) {
  JsonValue root;
  if (!ParseJson(text, &root, error_message)) return false;
  const JsonValue* object = nullptr;
  if (!RequireObject(&root, "the schedule store", &object, error_message)) {
    return false;
  }
  if (!RequireExactFields(*object, kRootFields, "the schedule store",
                          error_message)) {
    return false;
  }
  std::int64_t version = 0;
  if (!RequireInt64(*object, "version", "the schedule store", &version,
                    error_message)) {
    return false;
  }
  if (version != 1) {
    SetError(error_message,
             "Unsupported schedule store version: " + std::to_string(version));
    return false;
  }
  ScheduleDocument loaded;
  if (!ParseConfig(*object, &loaded.config, error_message)) return false;
  if (!ParseState(*object, &loaded.state, error_message)) return false;
  *document = std::move(loaded);
  return true;
}

}  // namespace

// ---- ScheduleStore ----

ScheduleStore::ScheduleStore(std::string schedule_file_path)
    : schedule_file_path_(std::move(schedule_file_path)) {}

std::string ScheduleStore::manifest_file_path() const {
  std::string base = schedule_file_path_;
  const std::string suffix = ".json";
  if (base.size() > suffix.size() &&
      base.compare(base.size() - suffix.size(), suffix.size(), suffix) == 0) {
    base.erase(base.size() - suffix.size());
  }
  return base + "-manifest.dat";
}

std::string ScheduleStore::lock_file_path() const {
  return schedule_file_path_ + ".lock";
}

ScheduleLoadStatus ScheduleStore::Load(ScheduleDocument* document,
                                       std::string* error_message) const {
  if (error_message != nullptr) error_message->clear();
  if (document == nullptr) {
    SetError(error_message, "Schedule document output must not be null");
    return ScheduleLoadStatus::kError;
  }
  *document = ScheduleDocument{};
  // 空路径必须在碰文件系统之前拦掉：否则 lstat("") 给出的错误跟"配置还不存在"
  // 完全不是一回事，调用方也就没法正确分流。
  if (schedule_file_path_.empty()) {
    SetError(error_message,
             "Cannot load schedule: schedule file path is empty");
    return ScheduleLoadStatus::kError;
  }

  std::string text;
  bool missing = false;
  if (!ReadWholeFile(schedule_file_path_, kMaxScheduleFileBytes, &text,
                     &missing, error_message)) {
    return ScheduleLoadStatus::kError;
  }
  if (missing) return ScheduleLoadStatus::kMissing;

  ScheduleDocument loaded;
  if (!ParseDocument(text, &loaded, error_message)) {
    return ScheduleLoadStatus::kError;
  }
  *document = std::move(loaded);
  return ScheduleLoadStatus::kLoaded;
}

bool ScheduleStore::Save(const ScheduleDocument& document,
                         std::string* error_message) const {
  if (error_message != nullptr) error_message->clear();
  if (schedule_file_path_.empty()) {
    SetError(error_message,
             "Cannot save schedule: schedule file path is empty");
    return false;
  }
  // 保存前先做结构校验：一份"写下去就读不回来"的配置，比一次明确的失败糟得多。
  if (!ValidateScheduleConfig(document.config, error_message)) return false;
  if (document.state.managed_snapshots.size() > kMaxRetainCount) {
    SetError(error_message,
             "Cannot save schedule: managed snapshot list exceeds " +
                 std::to_string(kMaxRetainCount) + " records");
    return false;
  }
  if (document.state.history.size() > kMaxHistoryEntries) {
    SetError(error_message, "Cannot save schedule: history exceeds " +
                                std::to_string(kMaxHistoryEntries) +
                                " entries");
    return false;
  }
  for (const ScheduledSnapshotRecord& record :
       document.state.managed_snapshots) {
    if (!IsSingleComponentArchiveName(record.file_name)) {
      SetError(error_message,
               "Cannot save schedule: managed snapshot record has an invalid "
               "file name: '" +
                   record.file_name + "'");
      return false;
    }
  }
  // baseline 指向的也必须是一个合法的单组件归档名。空串是合法的：那表示
  // "还没有 baseline"，不是错误。
  if (!document.state.baseline.snapshot_file_name.empty() &&
      !IsSingleComponentArchiveName(
          document.state.baseline.snapshot_file_name)) {
    SetError(error_message,
             "Cannot save schedule: the baseline snapshot has an invalid "
             "file name: '" +
                 document.state.baseline.snapshot_file_name + "'");
    return false;
  }
  if (!IsBoundedString(document.state.baseline.repository_identity,
                       kMaxScheduleStringBytes) ||
      !IsBoundedString(document.state.baseline.source_path,
                       kMaxScheduleStringBytes)) {
    SetError(error_message,
             "Cannot save schedule: the baseline record holds an unusable "
             "path (too long or contains a NUL byte)");
    return false;
  }

  // 父目录不存在就先建出来：首次运行、或者应用配置目录被清空过时，
  // "保存计划"不该因为路径还不存在而失败。
  if (!MakeDirectories(ParentDirectoryOf(schedule_file_path_), error_message)) {
    return false;
  }
  // 原子替换的实现在 file_io.cpp：临时文件名由 mkstemp 生成（唯一 + O_EXCL），
  // 因此既不会跟随别人预放的符号链接，也不会截断别人预放的文件。ConfigManager
  // 用的是同一个函数——两份各自演化的临时文件写法迟早会有一份漏掉某条边界。
  return WriteFileAtomicallyReplacing(
      schedule_file_path_, SerializeScheduleDocument(document), error_message);
}

ManifestBinding BindingOf(const ScheduleBaseline& baseline) {
  ManifestBinding binding;
  binding.snapshot_file_name = baseline.snapshot_file_name;
  binding.repository_identity = baseline.repository_identity;
  binding.source_path = baseline.source_path;
  return binding;
}

ScheduleBaseline BaselineOf(const ManifestBinding& binding) {
  ScheduleBaseline baseline;
  baseline.snapshot_file_name = binding.snapshot_file_name;
  baseline.repository_identity = binding.repository_identity;
  baseline.source_path = binding.source_path;
  return baseline;
}

bool SameBaselineBinding(const ScheduleBaseline& baseline,
                         const ManifestBinding& binding) {
  // version 1 的 manifest 没有归属信息。空 binding 与空 baseline 都是
  // "不知道属于谁"，一律判为不一致——这里绝不猜。
  if (binding.empty()) return false;
  if (baseline.snapshot_file_name.empty()) return false;
  return baseline.snapshot_file_name == binding.snapshot_file_name &&
         baseline.repository_identity == binding.repository_identity &&
         baseline.source_path == binding.source_path;
}

ScheduleStore::ManifestLoadStatus ScheduleStore::LoadManifest(
    std::vector<ManifestEntry>* entries, ManifestBinding* binding,
    std::string* error_message) const {
  if (error_message != nullptr) error_message->clear();
  if (entries == nullptr) {
    SetError(error_message, "Manifest output must not be null");
    return ManifestLoadStatus::kError;
  }
  if (binding == nullptr) {
    SetError(error_message, "Manifest binding output must not be null");
    return ManifestLoadStatus::kError;
  }
  entries->clear();
  *binding = ManifestBinding{};
  if (schedule_file_path_.empty()) {
    SetError(error_message,
             "Cannot load source manifest: schedule file path is empty");
    return ManifestLoadStatus::kError;
  }

  std::string text;
  bool missing = false;
  if (!ReadWholeFile(manifest_file_path(), kMaxManifestBytes, &text, &missing,
                     error_message)) {
    return ManifestLoadStatus::kError;
  }
  if (missing) return ManifestLoadStatus::kMissing;
  if (!ParseManifest(text, entries, binding, error_message)) {
    return ManifestLoadStatus::kError;
  }
  // 第二层校验：binding 里那份快照名还必须是一个**本仓库能管理的归档名**。
  // 结构检查（单组件、无 NUL、有界）在 ParseManifest 里已经做过一次，
  // 这里补上归档命名规则这一条——两份 manifest 规则各自守自己的边界，
  // 谁也不替谁放宽。
  if (!binding->empty() &&
      !IsSingleComponentArchiveName(binding->snapshot_file_name)) {
    SetError(
        error_message,
        "Invalid source manifest: the baseline snapshot file name is not a "
        "manageable archive name: '" +
            binding->snapshot_file_name + "'");
    return ManifestLoadStatus::kError;
  }
  return ManifestLoadStatus::kLoaded;
}

bool ScheduleStore::SaveManifest(const std::vector<ManifestEntry>& entries,
                                 const ManifestBinding& binding,
                                 std::string* error_message) const {
  if (error_message != nullptr) error_message->clear();
  if (schedule_file_path_.empty()) {
    SetError(error_message,
             "Cannot save source manifest: schedule file path is empty");
    return false;
  }
  if (entries.size() > kMaxManifestEntries) {
    SetError(error_message,
             "Cannot save source manifest: " + std::to_string(entries.size()) +
                 " entries exceeds the bound");
    return false;
  }
  // 归属必须完整，而且必须是一个 Catalog 认得出来的归档名。
  if (!IsSingleComponentArchiveName(binding.snapshot_file_name)) {
    SetError(error_message,
             "Cannot save source manifest: the baseline snapshot file name is "
             "not a manageable archive name: '" +
                 binding.snapshot_file_name + "'");
    return false;
  }
  if (!IsBoundedString(binding.repository_identity, kMaxScheduleStringBytes) ||
      !IsBoundedString(binding.source_path, kMaxScheduleStringBytes) ||
      binding.repository_identity.empty() || binding.source_path.empty()) {
    SetError(error_message,
             "Cannot save source manifest: the baseline binding holds an "
             "unusable repository or source path");
    return false;
  }

  const std::string text = SerializeManifest(entries, binding);
  if (text.empty()) {
    SetError(error_message,
             "Cannot save source manifest: the baseline binding is incomplete");
    return false;
  }
  if (text.size() > kMaxManifestBytes) {
    SetError(error_message,
             "Cannot save source manifest: " + std::to_string(text.size()) +
                 " bytes exceeds the bound");
    return false;
  }
  return WriteFileAtomicallyReplacing(manifest_file_path(), text,
                                      error_message);
}

bool ScheduleStore::RemoveManifest(std::string* error_message) const {
  if (error_message != nullptr) error_message->clear();
  if (schedule_file_path_.empty()) {
    SetError(error_message,
             "Cannot remove source manifest: schedule file path is empty");
    return false;
  }
  const std::string path = manifest_file_path();
  if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
    SetError(error_message, Describe(errno, "Failed to remove", path));
    return false;
  }
  return true;
}

}  // namespace backupproject
