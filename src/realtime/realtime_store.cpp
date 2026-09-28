// realtime_store.cpp
//
// 见 include/realtime_store.h。

#include "realtime_store.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdio>
#include <string>
#include <vector>

#include "backup_catalog.h"
#include "backup_option_keys.h"
#include "incremental_delta.h"
#include "simple_json.h"
#include "source_digest.h"

namespace backupproject {

namespace {

void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) *error_message = text;
}

std::string ErrnoText(int error_number) {
  const char* text = ::strerror(error_number);
  return text == nullptr ? std::string("errno ") + std::to_string(error_number)
                         : std::string(text);
}

bool IsBoundedString(const std::string& value) {
  if (value.size() > kMaxRealtimeStringBytes) return false;
  return value.find('\0') == std::string::npos;
}

// 去掉尾部 '/'（根目录本身除外）。只处理分隔符，不解析 "." / ".."：那是
// canonicalize 的活，而 canonicalize 可能把路径带到别处去。
std::string StripTrailingSlashes(const std::string& path) {
  if (path.empty()) return path;
  std::string out = path;
  while (out.size() > 1 && out.back() == '/') out.pop_back();
  return out;
}

// 存在时用 realpath 归一化；不存在时退化成"去掉尾部斜杠的绝对路径"。
// 两种情况下都不做任何猜测。
std::string CanonicalOrAbsolute(const std::string& path) {
  if (path.empty()) return path;
  char resolved[PATH_MAX];
  if (::realpath(path.c_str(), resolved) != nullptr) {
    return StripTrailingSlashes(std::string(resolved));
  }
  return StripTrailingSlashes(path);
}

bool IsSameOrDescendant(const std::string& candidate, const std::string& root) {
  if (candidate == root) return true;
  if (root == "/") return !candidate.empty() && candidate[0] == '/';
  if (candidate.size() <= root.size()) return false;
  if (candidate.compare(0, root.size(), root) != 0) return false;
  return candidate[root.size()] == '/';
}

bool LstatIsDirectory(const std::string& path, bool* is_directory,
                      bool* is_symlink, std::string* error_message) {
  struct stat info;
  if (::lstat(path.c_str(), &info) != 0) {
    SetError(error_message, "Cannot inspect " + path + ": " + ErrnoText(errno));
    return false;
  }
  *is_directory = S_ISDIR(info.st_mode);
  *is_symlink = S_ISLNK(info.st_mode);
  return true;
}

std::string KeyOfRuleList(const std::vector<std::string>& rules) {
  std::string out;
  for (const std::string& rule : rules) {
    out += rule;
    out.push_back('\n');
  }
  return out;
}

}  // namespace

RealtimePathOverlap ClassifyPathOverlap(const std::string& source_path,
                                        const std::string& repository_path,
                                        std::string* detail) {
  if (detail != nullptr) detail->clear();
  if (source_path.empty() || repository_path.empty()) {
    if (detail != nullptr) *detail = "one of the two paths is empty";
    return RealtimePathOverlap::kUnknown;
  }
  const std::string source = CanonicalOrAbsolute(source_path);
  const std::string repository = CanonicalOrAbsolute(repository_path);
  if (source == repository) {
    if (detail != nullptr) *detail = source;
    return RealtimePathOverlap::kEqual;
  }
  if (IsSameOrDescendant(repository, source)) {
    if (detail != nullptr) *detail = repository + " is inside " + source;
    return RealtimePathOverlap::kRepositoryInsideSource;
  }
  if (IsSameOrDescendant(source, repository)) {
    if (detail != nullptr) *detail = source + " is inside " + repository;
    return RealtimePathOverlap::kSourceInsideRepository;
  }
  return RealtimePathOverlap::kNone;
}

bool ValidateRealtimeConfig(const RealtimeConfig& config,
                            std::string* error_message) {
  if (config.version != kRealtimeConfigVersion) {
    SetError(error_message, "Unsupported realtime config version: " +
                                std::to_string(config.version));
    return false;
  }
  if (config.trigger != BackupTrigger::kRealtime) {
    SetError(error_message, "A realtime config must have trigger = realtime");
    return false;
  }
  if (!IsBoundedString(config.source_path)) {
    SetError(error_message,
             "Realtime source path is not usable (too long or contains NUL)");
    return false;
  }
  if (config.debounce_ms < kMinRealtimeDebounceMs ||
      config.debounce_ms > kMaxRealtimeDebounceMs) {
    SetError(error_message,
             "Realtime debounce must be between " +
                 std::to_string(kMinRealtimeDebounceMs) + " and " +
                 std::to_string(kMaxRealtimeDebounceMs) + " ms, got " +
                 std::to_string(config.debounce_ms));
    return false;
  }
  if (config.max_wait_ms < kMinRealtimeMaxWaitMs ||
      config.max_wait_ms > kMaxRealtimeMaxWaitMs) {
    SetError(error_message,
             "Realtime max wait must be between " +
                 std::to_string(kMinRealtimeMaxWaitMs) + " and " +
                 std::to_string(kMaxRealtimeMaxWaitMs) + " ms, got " +
                 std::to_string(config.max_wait_ms));
    return false;
  }
  if (config.max_wait_ms < config.debounce_ms) {
    SetError(error_message,
             "Realtime max wait must not be smaller than the debounce window");
    return false;
  }
  if (config.retain_count < kMinRealtimeRetainCount ||
      config.retain_count > kMaxRealtimeRetainCount) {
    SetError(error_message, "Realtime retain count must be between " +
                                std::to_string(kMinRealtimeRetainCount) +
                                " and " +
                                std::to_string(kMaxRealtimeRetainCount) +
                                ", got " + std::to_string(config.retain_count));
    return false;
  }
  if (config.include_rules.size() + config.exclude_rules.size() >
      kMaxRealtimeRules) {
    SetError(error_message, "Too many realtime filter rules: " +
                                std::to_string(config.include_rules.size() +
                                               config.exclude_rules.size()));
    return false;
  }
  for (const std::string& rule : config.include_rules) {
    if (!IsBoundedString(rule)) {
      SetError(error_message, "Realtime include rule is not usable");
      return false;
    }
  }
  for (const std::string& rule : config.exclude_rules) {
    if (!IsBoundedString(rule)) {
      SetError(error_message, "Realtime exclude rule is not usable");
      return false;
    }
  }
  // 策略 / 算法组合只有一份答案来源。Realtime 不需要自己的第二张表。
  BackupOptionCombination combination;
  combination.trigger = config.trigger;
  combination.strategy = config.strategy;
  combination.pack_method = config.pack_method;
  combination.compression_method = config.compression_method;
  combination.encryption_method = config.encryption_method;
  if (!IsSupportedBackupOptionCombination(combination)) {
    SetError(error_message,
             UnsupportedBackupOptionCombinationReason(combination));
    return false;
  }
  return true;
}

bool ValidateRealtimeForEnable(const RealtimeConfig& config,
                               const std::string& repository_path,
                               std::string* error_message,
                               std::string* repository_identity) {
  if (!ValidateRealtimeConfig(config, error_message)) return false;
  if (config.source_path.empty()) {
    SetError(error_message, "Realtime source directory must not be empty");
    return false;
  }
  bool is_directory = false;
  bool is_symlink = false;
  if (!LstatIsDirectory(config.source_path, &is_directory, &is_symlink,
                        error_message)) {
    return false;
  }
  if (is_symlink) {
    SetError(error_message, "Realtime source must not be a symbolic link: " +
                                config.source_path);
    return false;
  }
  if (!is_directory) {
    SetError(error_message,
             "Realtime source must be a directory: " + config.source_path);
    return false;
  }

  BackupCatalog catalog;
  std::string normalized_repository;
  if (!catalog.EnsureRepository(repository_path, error_message)) {
    return false;
  }
  if (!LstatIsDirectory(repository_path, &is_directory, &is_symlink,
                        error_message)) {
    return false;
  }
  if (is_symlink || !is_directory) {
    SetError(error_message,
             "Realtime repository must be a real directory (not a symlink): " +
                 repository_path);
    return false;
  }

  // 硬边界：源与仓库绝不重叠，从根上消灭"备份产生的 .bak 又触发自己"。
  std::string overlap_detail;
  const RealtimePathOverlap overlap =
      ClassifyPathOverlap(config.source_path, repository_path, &overlap_detail);
  switch (overlap) {
    case RealtimePathOverlap::kNone:
      break;
    case RealtimePathOverlap::kEqual:
      SetError(
          error_message,
          "Realtime source and repository must not be the same directory: " +
              overlap_detail);
      return false;
    case RealtimePathOverlap::kRepositoryInsideSource:
      SetError(error_message,
               "The realtime repository must not live inside the source "
               "directory: " +
                   overlap_detail);
      return false;
    case RealtimePathOverlap::kSourceInsideRepository:
      SetError(error_message,
               "The realtime source must not live inside the repository: " +
                   overlap_detail);
      return false;
    case RealtimePathOverlap::kUnknown:
      SetError(error_message,
               "Cannot compare the realtime source and repository paths");
      return false;
  }

  if (repository_identity != nullptr) {
    *repository_identity = RepositoryIdentity(repository_path);
  }
  return true;
}

RealtimeStore::RealtimeStore(std::string file_path)
    : file_path_(std::move(file_path)) {}

namespace {

bool ReadWholeFile(const std::string& path, std::string* text,
                   std::string* error_message) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    SetError(error_message, "Cannot open " + path + ": " + ErrnoText(errno));
    return false;
  }
  struct stat info;
  if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
    ::close(fd);
    SetError(error_message, "Not a regular file: " + path);
    return false;
  }
  if (static_cast<std::uint64_t>(info.st_size) > kMaxRealtimeFileBytes) {
    ::close(fd);
    SetError(error_message, "Realtime config file is too large: " + path);
    return false;
  }
  text->clear();
  text->resize(static_cast<std::size_t>(info.st_size));
  std::size_t filled = 0;
  while (filled < text->size()) {
    const ssize_t got = ::read(fd, &(*text)[filled], text->size() - filled);
    if (got < 0) {
      if (errno == EINTR) continue;
      const std::string message = ErrnoText(errno);
      ::close(fd);
      SetError(error_message, "Cannot read " + path + ": " + message);
      return false;
    }
    if (got == 0) break;
    filled += static_cast<std::size_t>(got);
  }
  if (::close(fd) != 0) {
    SetError(error_message, "Cannot close " + path + ": " + ErrnoText(errno));
    return false;
  }
  if (filled != text->size()) {
    SetError(error_message, "Realtime config is truncated: " + path);
    return false;
  }
  return true;
}

bool RealtimeRequireString(const JsonValue& object, const char* key,
                           const std::string& what, std::string* value,
                           std::string* error_message) {
  const JsonValue* field = object.Find(key);
  if (field == nullptr || !field->is_string()) {
    SetError(error_message, what + ": '" + key + "' must be a string");
    return false;
  }
  if (!IsBoundedString(field->text)) {
    SetError(error_message, what + ": '" + key + "' is not usable");
    return false;
  }
  *value = field->text;
  return true;
}

bool RealtimeRequireBool(const JsonValue& object, const char* key,
                         const std::string& what, bool* value,
                         std::string* error_message) {
  const JsonValue* field = object.Find(key);
  if (field == nullptr || !field->is_bool()) {
    SetError(error_message, what + ": '" + key + "' must be a boolean");
    return false;
  }
  *value = field->boolean;
  return true;
}

bool RealtimeRequireUint(const JsonValue& object, const char* key,
                         const std::string& what, std::uint32_t maximum,
                         std::uint32_t* value, std::string* error_message) {
  const JsonValue* field = object.Find(key);
  if (field == nullptr || !field->is_number()) {
    SetError(error_message, what + ": '" + key + "' must be an integer");
    return false;
  }
  if (field->number < 0 ||
      static_cast<std::uint64_t>(field->number) > maximum) {
    SetError(error_message, what + ": '" + key + "' is out of range");
    return false;
  }
  *value = static_cast<std::uint32_t>(field->number);
  return true;
}

bool RealtimeRequireRuleList(const JsonValue& object, const char* key,
                             const std::string& what,
                             std::vector<std::string>* rules,
                             std::string* error_message) {
  const JsonValue* field = object.Find(key);
  if (field == nullptr || !field->is_array()) {
    SetError(error_message, what + ": '" + key + "' must be an array");
    return false;
  }
  rules->clear();
  if (field->array.size() > kMaxRealtimeRules) {
    SetError(error_message, what + ": '" + key + "' has too many entries");
    return false;
  }
  for (const JsonValue& item : field->array) {
    if (!item.is_string() || !IsBoundedString(item.text)) {
      SetError(error_message,
               what + ": '" + key + "' must contain only usable strings");
      return false;
    }
    rules->push_back(item.text);
  }
  return true;
}

void AppendStringField(std::string* out, const char* key,
                       const std::string& value) {
  *out += "    \"";
  *out += key;
  *out += "\": \"";
  for (const char character : value) {
    switch (character) {
      case '\\':
        *out += "\\\\";
        break;
      case '"':
        *out += "\\\"";
        break;
      case '\n':
        *out += "\\n";
        break;
      case '\r':
        *out += "\\r";
        break;
      case '\t':
        *out += "\\t";
        break;
      default:
        out->push_back(character);
    }
  }
  *out += "\"";
}

void AppendRules(std::string* out, const char* key,
                 const std::vector<std::string>& rules, bool last) {
  *out += "    \"";
  *out += key;
  *out += "\": [";
  for (std::size_t index = 0; index < rules.size(); ++index) {
    if (index > 0) *out += ", ";
    *out += "\"";
    for (const char character : rules[index]) {
      if (character == '\\' || character == '"') out->push_back('\\');
      if (character == '\n') {
        *out += "\\n";
        continue;
      }
      out->push_back(character);
    }
    *out += "\"";
  }
  *out += "]";
  if (!last) *out += ",";
  *out += "\n";
}

}  // namespace

RealtimeLoadStatus RealtimeStore::Load(RealtimeConfig* config,
                                       std::string* error_message) const {
  if (config == nullptr) {
    SetError(error_message, "Realtime config output must not be null");
    return RealtimeLoadStatus::kError;
  }
  if (error_message != nullptr) error_message->clear();
  *config = RealtimeConfig{};

  struct stat info;
  if (::lstat(file_path_.c_str(), &info) != 0) {
    if (errno == ENOENT) return RealtimeLoadStatus::kMissing;
    SetError(error_message,
             "Cannot inspect " + file_path_ + ": " + ErrnoText(errno));
    return RealtimeLoadStatus::kError;
  }

  std::string text;
  if (!ReadWholeFile(file_path_, &text, error_message)) {
    return RealtimeLoadStatus::kError;
  }
  JsonValue root;
  std::string json_error;
  if (!ParseJson(text, &root, &json_error)) {
    SetError(error_message, "Invalid realtime config: " + json_error);
    return RealtimeLoadStatus::kError;
  }
  if (!RequireExactFields(
          root,
          {"version", "enabled", "trigger", "source_path", "debounce_ms",
           "max_wait_ms", "retain_count", "strategy", "pack", "compression",
           "encryption", "include_rules", "exclude_rules"},
          "realtime config", error_message)) {
    return RealtimeLoadStatus::kError;
  }

  std::uint32_t number = 0;
  std::string text_value;
  const std::string what = "realtime config";
  if (!RealtimeRequireUint(root, "version", what, 1000, &number,
                           error_message)) {
    return RealtimeLoadStatus::kError;
  }
  config->version = number;
  if (!RealtimeRequireBool(root, "enabled", what, &config->enabled,
                           error_message)) {
    return RealtimeLoadStatus::kError;
  }
  if (!RealtimeRequireString(root, "trigger", what, &text_value,
                             error_message)) {
    return RealtimeLoadStatus::kError;
  }
  if (!ParseBackupTriggerKey(text_value, &config->trigger) ||
      config->trigger != BackupTrigger::kRealtime) {
    SetError(error_message, "realtime config: unknown trigger '" + text_value +
                                "' (expected realtime)");
    return RealtimeLoadStatus::kError;
  }
  if (!RealtimeRequireString(root, "source_path", what, &config->source_path,
                             error_message)) {
    return RealtimeLoadStatus::kError;
  }
  if (!RealtimeRequireUint(root, "debounce_ms", what, 60000000u, &number,
                           error_message)) {
    return RealtimeLoadStatus::kError;
  }
  config->debounce_ms = number;
  if (!RealtimeRequireUint(root, "max_wait_ms", what, 60000000u, &number,
                           error_message)) {
    return RealtimeLoadStatus::kError;
  }
  config->max_wait_ms = number;
  if (!RealtimeRequireUint(root, "retain_count", what, 1000000u, &number,
                           error_message)) {
    return RealtimeLoadStatus::kError;
  }
  config->retain_count = number;
  if (!RealtimeRequireString(root, "strategy", what, &text_value,
                             error_message)) {
    return RealtimeLoadStatus::kError;
  }
  if (!ParseBackupStrategyKey(text_value, &config->strategy)) {
    SetError(error_message,
             "realtime config: unknown strategy '" + text_value + "'");
    return RealtimeLoadStatus::kError;
  }
  if (!RealtimeRequireString(root, "pack", what, &text_value, error_message)) {
    return RealtimeLoadStatus::kError;
  }
  if (!ParsePackMethodKey(text_value, &config->pack_method)) {
    SetError(error_message,
             "realtime config: unknown pack method '" + text_value + "'");
    return RealtimeLoadStatus::kError;
  }
  if (!RealtimeRequireString(root, "compression", what, &text_value,
                             error_message)) {
    return RealtimeLoadStatus::kError;
  }
  if (!ParseCompressionMethodKey(text_value, &config->compression_method)) {
    SetError(error_message, "realtime config: unknown compression method '" +
                                text_value + "'");
    return RealtimeLoadStatus::kError;
  }
  if (!RealtimeRequireString(root, "encryption", what, &text_value,
                             error_message)) {
    return RealtimeLoadStatus::kError;
  }
  if (!ParseEncryptionMethodKey(text_value, &config->encryption_method)) {
    SetError(error_message,
             "realtime config: unknown encryption method '" + text_value + "'");
    return RealtimeLoadStatus::kError;
  }
  if (!RealtimeRequireRuleList(root, "include_rules", what,
                               &config->include_rules, error_message)) {
    return RealtimeLoadStatus::kError;
  }
  if (!RealtimeRequireRuleList(root, "exclude_rules", what,
                               &config->exclude_rules, error_message)) {
    return RealtimeLoadStatus::kError;
  }
  if (!ValidateRealtimeConfig(*config, error_message)) {
    return RealtimeLoadStatus::kError;
  }
  return RealtimeLoadStatus::kLoaded;
}

bool RealtimeStore::Save(const RealtimeConfig& config,
                         std::string* error_message) const {
  if (error_message != nullptr) error_message->clear();
  if (!ValidateRealtimeConfig(config, error_message)) return false;

  std::string out = "{\n";
  out += "    \"version\": " + std::to_string(config.version) + ",\n";
  out += std::string("    \"enabled\": ") +
         (config.enabled ? "true" : "false") + ",\n";
  out += "    \"trigger\": \"" + std::string(BackupTriggerKey(config.trigger)) +
         "\",\n";
  AppendStringField(&out, "source_path", config.source_path);
  out += ",\n";
  out += "    \"debounce_ms\": " + std::to_string(config.debounce_ms) + ",\n";
  out += "    \"max_wait_ms\": " + std::to_string(config.max_wait_ms) + ",\n";
  out += "    \"retain_count\": " + std::to_string(config.retain_count) + ",\n";
  out += "    \"strategy\": \"" +
         std::string(BackupStrategyKey(config.strategy)) + "\",\n";
  out += "    \"pack\": \"" + std::string(PackMethodKey(config.pack_method)) +
         "\",\n";
  out += "    \"compression\": \"" +
         std::string(CompressionMethodKey(config.compression_method)) + "\",\n";
  out += "    \"encryption\": \"" +
         std::string(EncryptionMethodKey(config.encryption_method)) + "\",\n";
  AppendRules(&out, "include_rules", config.include_rules, false);
  AppendRules(&out, "exclude_rules", config.exclude_rules, true);
  out += "}\n";

  const std::string temp = file_path_ + ".tmp";
  ::unlink(temp.c_str());
  const int fd =
      ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) {
    SetError(error_message, "Cannot create " + temp + ": " + ErrnoText(errno));
    return false;
  }
  std::size_t written = 0;
  while (written < out.size()) {
    const ssize_t got = ::write(fd, out.data() + written, out.size() - written);
    if (got < 0) {
      if (errno == EINTR) continue;
      const std::string message = ErrnoText(errno);
      ::close(fd);
      ::unlink(temp.c_str());
      SetError(error_message, "Cannot write " + temp + ": " + message);
      return false;
    }
    written += static_cast<std::size_t>(got);
  }
  if (::fsync(fd) != 0 || ::close(fd) != 0) {
    const std::string message = ErrnoText(errno);
    ::unlink(temp.c_str());
    SetError(error_message, "Cannot flush " + temp + ": " + message);
    return false;
  }
  if (::rename(temp.c_str(), file_path_.c_str()) != 0) {
    const std::string message = ErrnoText(errno);
    ::unlink(temp.c_str());
    SetError(error_message, "Cannot publish " + file_path_ + ": " + message);
    return false;
  }
  return true;
}

std::string RealtimeJobIdentityDigest(const RealtimeConfig& config,
                                      const std::string& repository_identity,
                                      const std::string& source_path) {
  std::string canonical = "BPREALTIMEJOB1\n";
  canonical += "trigger=realtime\n";
  canonical +=
      "source=" + SourceIdentityDigest(source_path, repository_identity) + "\n";
  canonical += "repository=" + repository_identity + "\n";
  canonical +=
      "filter=" +
      FilterIdentityDigest(config.include_rules, config.exclude_rules) + "\n";
  canonical +=
      "strategy=" + std::string(BackupStrategyKey(config.strategy)) + "\n";
  canonical +=
      "pipeline=" +
      StrategyIdentityDigest(config.pack_method, config.compression_method,
                             config.encryption_method) +
      "\n";
  canonical += "include=" + KeyOfRuleList(config.include_rules) + "\n";
  canonical += "exclude=" + KeyOfRuleList(config.exclude_rules) + "\n";
  return ContentDigestOfBytes(canonical);
}

}  // namespace backupproject
