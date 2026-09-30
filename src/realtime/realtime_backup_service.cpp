// realtime_backup_service.cpp
//
// 见 include/realtime_backup_service.h。

#include "realtime_backup_service.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "backup_engine.h"
#include "backup_option_keys.h"
#include "incremental_delta.h"
#include "source_digest.h"
#include "source_manifest.h"

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

std::string JoinPath(const std::string& directory, const std::string& name) {
  if (directory.empty()) return name;
  if (directory.back() == '/') return directory + name;
  return directory + "/" + name;
}

bool g_marker_write_failure_for_testing = false;

std::string EscapeField(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (const char character : value) {
    switch (character) {
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(character);
    }
  }
  return out;
}

bool UnescapeField(const std::string& text, std::string* value) {
  value->clear();
  value->reserve(text.size());
  for (std::size_t index = 0; index < text.size(); ++index) {
    if (text[index] != '\\') {
      value->push_back(text[index]);
      continue;
    }
    if (index + 1 >= text.size()) return false;
    switch (text[++index]) {
      case '\\':
        value->push_back('\\');
        break;
      case 'n':
        value->push_back('\n');
        break;
      case 'r':
        value->push_back('\r');
        break;
      case 't':
        value->push_back('\t');
        break;
      default:
        return false;
    }
  }
  return true;
}

void AppendField(std::string* out, const char* key, const std::string& value) {
  *out += key;
  *out += "=";
  *out += EscapeField(value);
  *out += "\n";
}

void AppendNumber(std::string* out, const char* key, std::uint64_t value) {
  AppendField(out, key, std::to_string(value));
}

bool ParseUint64(const std::string& text, std::uint64_t* value) {
  if (text.empty() || text.size() > 20) return false;
  std::uint64_t result = 0;
  for (const char character : text) {
    if (character < '0' || character > '9') return false;
    const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
    if (result > (UINT64_MAX - digit) / 10) return false;
    result = result * 10 + digit;
  }
  *value = result;
  return true;
}

bool ParseInt64(const std::string& text, std::int64_t* value) {
  if (text.empty()) return false;
  bool negative = false;
  std::string digits = text;
  if (digits[0] == '-') {
    negative = true;
    digits = digits.substr(1);
  }
  std::uint64_t magnitude = 0;
  if (!ParseUint64(digits, &magnitude)) return false;
  if (magnitude > static_cast<std::uint64_t>(INT64_MAX)) return false;
  *value = negative ? -static_cast<std::int64_t>(magnitude)
                    : static_cast<std::int64_t>(magnitude);
  return true;
}

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
  if (static_cast<std::uint64_t>(info.st_size) > kMaxRealtimeMarkerBytes) {
    ::close(fd);
    SetError(error_message, "Marker is too large: " + path);
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
  ::close(fd);
  if (filled != text->size()) {
    SetError(error_message, "Marker is truncated: " + path);
    return false;
  }
  return true;
}

}  // namespace

void SetRealtimeMarkerWriteFailureForTesting(bool fail) {
  g_marker_write_failure_for_testing = fail;
}

std::string RealtimeMarkerFileName(const std::string& snapshot_file_name) {
  return snapshot_file_name + kRealtimeMarkerSuffix;
}

bool IsRealtimeMarkerFileName(const std::string& file_name,
                              std::string* snapshot_file_name) {
  const std::size_t suffix_len = ::strlen(kRealtimeMarkerSuffix);
  if (file_name.size() <= suffix_len) return false;
  if (file_name.compare(file_name.size() - suffix_len, suffix_len,
                        kRealtimeMarkerSuffix) != 0) {
    return false;
  }
  const std::string base = file_name.substr(0, file_name.size() - suffix_len);
  // 只有"受管理的 .bak 名字 + .realtime"才可能是项目拥有的 marker：
  // notes.realtime / report.realtime / foo.txt.realtime 都不是。
  if (!IsManagedBackupFileName(base)) return false;
  if (snapshot_file_name != nullptr) *snapshot_file_name = base;
  return true;
}

std::string SerializeRealtimeMarker(const RealtimeMarker& marker) {
  std::string out = kRealtimeMarkerHeader;
  AppendField(&out, "snapshot_file_name", marker.snapshot_file_name);
  AppendField(&out, "snapshot_id", marker.snapshot_id);
  AppendNumber(&out, "created_time_sec",
               static_cast<std::uint64_t>(marker.created_time_sec));
  AppendField(&out, "job_identity", marker.job_identity);
  AppendField(&out, "source_identity", marker.source_identity);
  AppendField(&out, "filter_identity", marker.filter_identity);
  AppendField(&out, "strategy", BackupStrategyKey(marker.strategy));
  AppendField(&out, "pack", PackMethodKey(marker.pack_method));
  AppendField(&out, "compression",
              CompressionMethodKey(marker.compression_method));
  AppendNumber(&out, "event_count", marker.event_count);
  AppendField(&out, "overflow_recovery", marker.overflow_recovery ? "1" : "0");
  AppendField(&out, "resync_trigger", marker.resync_trigger ? "1" : "0");
  AppendField(&out, "outcome_kind", marker.outcome_kind);
  AppendNumber(&out, "added", marker.added);
  AppendNumber(&out, "removed", marker.removed);
  AppendNumber(&out, "modified", marker.modified);
  AppendNumber(&out, "metadata_changed", marker.metadata_changed);
  return out;
}

bool ParseRealtimeMarker(const std::string& text, RealtimeMarker* marker,
                         std::string* error_message) {
  if (marker == nullptr) {
    SetError(error_message, "Marker output must not be null");
    return false;
  }
  *marker = RealtimeMarker{};
  if (text.size() > kMaxRealtimeMarkerBytes) {
    SetError(error_message, "Marker is too large");
    return false;
  }
  const std::string header = kRealtimeMarkerHeader;
  if (text.compare(0, std::min(header.size(), text.size()), header) != 0) {
    SetError(error_message, "Invalid realtime marker: wrong header");
    return false;
  }
  static const char* kRequired[] = {"snapshot_file_name",
                                    "snapshot_id",
                                    "created_time_sec",
                                    "job_identity",
                                    "source_identity",
                                    "filter_identity",
                                    "strategy",
                                    "pack",
                                    "compression",
                                    "event_count",
                                    "overflow_recovery",
                                    "resync_trigger",
                                    "outcome_kind",
                                    "added",
                                    "removed",
                                    "modified",
                                    "metadata_changed"};
  std::map<std::string, int> seen;
  std::size_t position = header.size();
  while (position < text.size()) {
    const std::size_t newline = text.find('\n', position);
    if (newline == std::string::npos) {
      SetError(error_message, "Invalid realtime marker: truncated line");
      return false;
    }
    const std::string line = text.substr(position, newline - position);
    position = newline + 1;
    if (line.empty()) continue;
    const std::size_t equals = line.find('=');
    if (equals == std::string::npos) {
      SetError(error_message, "Invalid realtime marker: a line has no '='");
      return false;
    }
    const std::string key = line.substr(0, equals);
    std::string value;
    if (!UnescapeField(line.substr(equals + 1), &value)) {
      SetError(error_message,
               "Invalid realtime marker: bad escape in key '" + key + "'");
      return false;
    }
    if (++seen[key] > 1) {
      SetError(error_message,
               "Invalid realtime marker: duplicate key '" + key + "'");
      return false;
    }

    std::uint64_t number = 0;
    std::int64_t signed_number = 0;
    if (key == "snapshot_file_name") {
      marker->snapshot_file_name = value;
    } else if (key == "snapshot_id") {
      if (!IsContentDigest(value)) {
        SetError(error_message, "Invalid realtime marker: bad snapshot id");
        return false;
      }
      marker->snapshot_id = value;
    } else if (key == "created_time_sec") {
      if (!ParseInt64(value, &signed_number)) {
        SetError(error_message, "Invalid realtime marker: bad created time");
        return false;
      }
      marker->created_time_sec = signed_number;
    } else if (key == "job_identity") {
      if (!IsContentDigest(value)) {
        SetError(error_message, "Invalid realtime marker: bad job identity");
        return false;
      }
      marker->job_identity = value;
    } else if (key == "source_identity") {
      marker->source_identity = value;
    } else if (key == "filter_identity") {
      marker->filter_identity = value;
    } else if (key == "strategy") {
      if (!ParseBackupStrategyKey(value, &marker->strategy)) {
        SetError(error_message,
                 "Invalid realtime marker: unknown strategy '" + value + "'");
        return false;
      }
    } else if (key == "pack") {
      if (!ParsePackMethodKey(value, &marker->pack_method)) {
        SetError(error_message,
                 "Invalid realtime marker: unknown pack '" + value + "'");
        return false;
      }
    } else if (key == "compression") {
      if (!ParseCompressionMethodKey(value, &marker->compression_method)) {
        SetError(
            error_message,
            "Invalid realtime marker: unknown compression '" + value + "'");
        return false;
      }
    } else if (key == "event_count") {
      if (!ParseUint64(value, &number)) {
        SetError(error_message, "Invalid realtime marker: bad event count");
        return false;
      }
      marker->event_count = number;
    } else if (key == "overflow_recovery" || key == "resync_trigger") {
      if (value != "0" && value != "1") {
        SetError(error_message,
                 "Invalid realtime marker: '" + key + "' must be 0 or 1");
        return false;
      }
      const bool flag = value == "1";
      if (key == "overflow_recovery") {
        marker->overflow_recovery = flag;
      } else {
        marker->resync_trigger = flag;
      }
    } else if (key == "outcome_kind") {
      marker->outcome_kind = value;
    } else if (key == "added" || key == "removed" || key == "modified" ||
               key == "metadata_changed") {
      if (!ParseUint64(value, &number)) {
        SetError(error_message,
                 "Invalid realtime marker: bad count for '" + key + "'");
        return false;
      }
      if (key == "added") {
        marker->added = number;
      } else if (key == "removed") {
        marker->removed = number;
      } else if (key == "modified") {
        marker->modified = number;
      } else {
        marker->metadata_changed = number;
      }
    } else {
      SetError(error_message,
               "Invalid realtime marker: unknown key '" + key + "'");
      return false;
    }
  }
  for (const char* key : kRequired) {
    if (seen[key] != 1) {
      SetError(
          error_message,
          std::string("Invalid realtime marker: missing key '") + key + "'");
      return false;
    }
  }
  if (!IsManagedBackupFileName(marker->snapshot_file_name)) {
    SetError(error_message,
             "Invalid realtime marker: snapshot_file_name is not a managed "
             "backup name");
    return false;
  }
  if (marker->outcome_kind != "full" &&
      marker->outcome_kind != "full-baseline" &&
      marker->outcome_kind != "delta" && marker->outcome_kind != "no-changes") {
    SetError(error_message, "Invalid realtime marker: unknown outcome kind '" +
                                marker->outcome_kind + "'");
    return false;
  }
  return true;
}

bool WriteRealtimeMarker(const std::string& repository_directory,
                         const RealtimeMarker& marker,
                         std::string* error_message) {
  if (g_marker_write_failure_for_testing) {
    SetError(error_message,
             "Injected realtime marker publish failure (for testing)");
    return false;
  }
  const std::string text = SerializeRealtimeMarker(marker);
  RealtimeMarker verified;
  std::string parse_error;
  if (!ParseRealtimeMarker(text, &verified, &parse_error)) {
    SetError(error_message,
             "Refusing to write an invalid realtime marker: " + parse_error);
    return false;
  }
  const std::string path = JoinPath(
      repository_directory, RealtimeMarkerFileName(marker.snapshot_file_name));
  const std::string temp = path + ".tmp";
  ::unlink(temp.c_str());
  const int fd =
      ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) {
    SetError(error_message, "Cannot create " + temp + ": " + ErrnoText(errno));
    return false;
  }
  std::size_t written = 0;
  while (written < text.size()) {
    const ssize_t got =
        ::write(fd, text.data() + written, text.size() - written);
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
  if (::rename(temp.c_str(), path.c_str()) != 0) {
    const std::string message = ErrnoText(errno);
    ::unlink(temp.c_str());
    SetError(error_message, "Cannot publish " + path + ": " + message);
    return false;
  }
  return true;
}

bool LoadRealtimeMarker(const std::string& repository_directory,
                        const std::string& snapshot_file_name,
                        RealtimeMarker* marker, std::string* error_message) {
  if (!IsManagedBackupFileName(snapshot_file_name)) {
    SetError(error_message, "Not a managed backup name: " + snapshot_file_name);
    return false;
  }
  const std::string path = JoinPath(repository_directory,
                                    RealtimeMarkerFileName(snapshot_file_name));
  std::string text;
  if (!ReadWholeFile(path, &text, error_message)) return false;
  RealtimeMarker parsed;
  if (!ParseRealtimeMarker(text, &parsed, error_message)) return false;
  if (parsed.snapshot_file_name != snapshot_file_name) {
    SetError(error_message, "The realtime marker belongs to '" +
                                parsed.snapshot_file_name + "'");
    return false;
  }
  *marker = parsed;
  return true;
}

bool ListRealtimeSnapshots(const std::string& repository_directory,
                           std::vector<RealtimeSnapshotRecord>* records,
                           std::string* error_message) {
  if (records == nullptr) {
    SetError(error_message, "Realtime record output must not be null");
    return false;
  }
  records->clear();

  BackupCatalog catalog;
  std::vector<BackupRecord> listed;
  if (!catalog.List(repository_directory, &listed, error_message)) return false;
  std::map<std::string, const BackupRecord*> by_name;
  for (const BackupRecord& record : listed) {
    by_name[record.file_name] = &record;
  }

  DIR* raw = ::opendir(repository_directory.c_str());
  if (raw == nullptr) {
    SetError(error_message, "Cannot open the repository directory " +
                                repository_directory + ": " + ErrnoText(errno));
    return false;
  }
  std::vector<std::string> marker_names;
  while (true) {
    errno = 0;
    struct dirent* item = ::readdir(raw);
    if (item == nullptr) {
      if (errno != 0) {
        const std::string text = ErrnoText(errno);
        ::closedir(raw);
        SetError(error_message,
                 "Cannot read the repository directory: " + text);
        return false;
      }
      break;
    }
    const std::string name = item->d_name;
    if (name == "." || name == "..") continue;
    marker_names.push_back(name);
  }
  ::closedir(raw);
  std::sort(marker_names.begin(), marker_names.end());

  for (const std::string& marker_name : marker_names) {
    std::string base;
    if (!IsRealtimeMarkerFileName(marker_name, &base)) continue;
    RealtimeSnapshotRecord record;
    record.file_name = base;
    record.marker_file_name = marker_name;

    RealtimeMarker marker;
    std::string marker_error;
    if (!LoadRealtimeMarker(repository_directory, base, &marker,
                            &marker_error)) {
      record.diagnostic = marker_error;
      records->push_back(record);
      continue;
    }
    record.snapshot_id = marker.snapshot_id;
    record.job_identity = marker.job_identity;
    record.outcome_kind = marker.outcome_kind;
    record.created_time_sec = marker.created_time_sec;
    record.event_count = marker.event_count;
    record.overflow_recovery = marker.overflow_recovery;
    record.resync_trigger = marker.resync_trigger;
    record.strategy = marker.strategy;
    record.pack_method = marker.pack_method;
    record.compression_method = marker.compression_method;
    record.added = marker.added;
    record.removed = marker.removed;
    record.modified = marker.modified;
    record.metadata_changed = marker.metadata_changed;

    // marker 必须与磁盘上的实际归档一致：这是"只信实际字节"的那道门。
    SnapshotIdentity identity;
    std::string identity_error;
    if (!LoadVerifiedSnapshotIdentity(repository_directory, base, &identity,
                                      nullptr, &identity_error)) {
      record.diagnostic = "the snapshot cannot be verified: " + identity_error;
      records->push_back(record);
      continue;
    }
    if (identity.snapshot_id != marker.snapshot_id) {
      record.diagnostic =
          "the realtime marker does not match the actual archive bytes";
      records->push_back(record);
      continue;
    }
    const auto found = by_name.find(base);
    if (found != by_name.end()) {
      record.archive_size = found->second->archive_size;
      record.archive_mtime_sec = found->second->modified_time_sec;
    }
    record.verified = true;
    records->push_back(record);
  }

  std::sort(records->begin(), records->end(),
            [](const RealtimeSnapshotRecord& left,
               const RealtimeSnapshotRecord& right) {
              if (left.created_time_sec != right.created_time_sec) {
                return left.created_time_sec < right.created_time_sec;
              }
              return left.file_name < right.file_name;
            });
  return true;
}

bool RunRealtimeRetention(const std::string& repository_directory,
                          const std::string& job_identity,
                          std::uint32_t retain_count,
                          RealtimeRetentionResult* result,
                          std::string* error_message) {
  if (result == nullptr) {
    SetError(error_message, "Realtime retention result must not be null");
    return false;
  }
  *result = RealtimeRetentionResult{};
  if (error_message != nullptr) error_message->clear();

  std::vector<RealtimeSnapshotRecord> records;
  std::string list_error;
  if (!ListRealtimeSnapshots(repository_directory, &records, &list_error)) {
    result->uncertain = true;
    result->reason = "cannot list realtime snapshots: " + list_error;
    return true;  // 不删 + warning，不把整次备份判成失败
  }

  // 任何一份 marker 坏到无法判断归属 -> 本轮 destructive retention 直接 no-op。
  for (const RealtimeSnapshotRecord& record : records) {
    if (!record.verified) {
      result->uncertain = true;
      result->reason = "a realtime marker cannot be verified (" +
                       record.marker_file_name + "): " + record.diagnostic;
      return true;
    }
  }

  std::vector<std::string> candidates_oldest_first;
  for (const RealtimeSnapshotRecord& record : records) {
    if (record.job_identity != job_identity) continue;  // 旧 job：不自动淘汰
    candidates_oldest_first.push_back(record.file_name);
  }
  if (candidates_oldest_first.size() <= retain_count) {
    result->kept_visible = candidates_oldest_first.size();
    return true;
  }

  RetentionPlan plan;
  std::string plan_error;
  if (!PlanDependencyAwareRetention(repository_directory,
                                    candidates_oldest_first, retain_count,
                                    &plan, &plan_error)) {
    result->uncertain = true;
    result->reason = "cannot plan realtime retention: " + plan_error;
    return true;
  }
  result->kept_visible = plan.keep_visible.size();
  result->kept_ancestors = plan.keep_ancestors.size();
  // 只有**真的不确定**才算 uncertain。
  //
  // 健康的增量链（例如 F0 -> D1 -> D2 -> D3、retain = 1）会出现
  // remove 为空但并非不确定的状态：最新恢复点依赖全部祖先，所以这一轮没有
  // 可删的东西。那是正常的 dependency retention，不是"依赖链读不出来"，
  // 不能报成 warning 让用户以为哪里坏了。
  if (plan.dependency_uncertain) {
    result->uncertain = true;
    result->reason = plan.uncertainty_reason;
    return true;
  }
  if (plan.remove.empty()) {
    // success：deleted = 0，kept_visible / kept_ancestors 保持真实计数，无
    // warning。
    return true;
  }

  BackupCatalog catalog;
  std::vector<std::string> removed;
  std::vector<std::string> diagnostics;
  std::string delete_error;
  if (!catalog.DeleteSnapshots(repository_directory, plan.remove, &removed,
                               &diagnostics, &delete_error)) {
    // 有不在删除集合里的 live descendant，或单个 unlink 失败：链优先。
    result->uncertain = true;
    result->reason = delete_error;
    result->diagnostics = diagnostics;
    result->deleted = removed.size();
    return true;
  }
  result->deleted = removed.size();
  result->diagnostics = diagnostics;
  return true;
}

bool RunRealtimeBackupOnce(const RealtimeConfig& config,
                           const std::string& repository_path,
                           const std::string& repository_identity,
                           const RealtimeEventSummary& events,
                           std::int64_t now_sec, RealtimeOutcome* outcome,
                           std::string* error_message) {
  if (outcome == nullptr) {
    SetError(error_message, "Realtime outcome output must not be null");
    return false;
  }
  *outcome = RealtimeOutcome{};
  if (error_message != nullptr) error_message->clear();
  if (!ValidateRealtimeConfig(config, error_message)) {
    outcome->kind = RealtimeOutcome::Kind::kFailed;
    return false;
  }

  const std::string source_identity =
      SourceIdentityDigest(config.source_path, repository_identity);
  const std::string filter_identity =
      FilterIdentityDigest(config.include_rules, config.exclude_rules);
  const std::string job_identity = RealtimeJobIdentityDigest(
      config, repository_identity, config.source_path);

  // 规则编译只有一处实现（共享 Filter::AddRule，见 BuildRealtimeFilter）：
  // ValidateRealtimeConfig 已经在保存 / 启用前编译过一遍，这里是运行时的防御
  // 路径，用的是同一个函数，因此错误逐字一致。
  Filter filter;
  if (!BuildRealtimeFilter(config, &filter, error_message)) {
    outcome->kind = RealtimeOutcome::Kind::kFailed;
    return false;
  }

  BackupCatalog catalog;
  if (!catalog.EnsureRepository(repository_path, error_message)) {
    outcome->kind = RealtimeOutcome::Kind::kFailed;
    return false;
  }

  BackupOptions options;
  options.pack_method = config.pack_method;
  options.compression_method = config.compression_method;
  options.encryption_method = EncryptionMethod::kNone;  // 自动触发不加密

  std::string snapshot_file_name;
  if (config.strategy == BackupStrategy::kFull) {
    // Full：完全走既有 BackupEngine + 既有扫描/打包/压缩流水线。
    std::string archive_path;
    if (!catalog.BuildArchivePath(repository_path, config.source_path, now_sec,
                                  &archive_path, error_message)) {
      outcome->kind = RealtimeOutcome::Kind::kFailed;
      return false;
    }
    snapshot_file_name = archive_path.substr(archive_path.rfind('/') + 1);
    BackupEngine engine;
    if (!engine.Backup(config.source_path, archive_path, filter, options,
                       error_message)) {
      outcome->kind = RealtimeOutcome::Kind::kFailed;
      return false;
    }
    outcome->kind = RealtimeOutcome::Kind::kFullSnapshot;
    // 面向用户的那句话只有一处：CLI 与 GUI 都显示它。
    outcome->summary_text = "创建完整实时快照";
  } else {
    // Incremental：完全走 RunIncrementalBackup（baseline / delta / no-change
    // 三选一由它决定，这里一个字都不复制）。
    std::string archive_path;
    if (!catalog.BuildArchivePath(repository_path, config.source_path, now_sec,
                                  &archive_path, error_message)) {
      outcome->kind = RealtimeOutcome::Kind::kFailed;
      return false;
    }
    snapshot_file_name = archive_path.substr(archive_path.rfind('/') + 1);
    IncrementalOutcome incremental;
    if (!RunIncrementalBackup(
            config.source_path, repository_path, snapshot_file_name,
            repository_identity, filter, options, config.include_rules,
            config.exclude_rules, std::string(), &incremental, error_message)) {
      outcome->kind = RealtimeOutcome::Kind::kFailed;
      return false;
    }
    outcome->changes = incremental.summary;
    switch (incremental.kind) {
      case IncrementalOutcome::Kind::kFullBaseline:
        outcome->kind = RealtimeOutcome::Kind::kFullBaseline;
        outcome->summary_text = "实时增量策略建立了新的完整基线";
        break;
      case IncrementalOutcome::Kind::kDelta:
        outcome->kind = RealtimeOutcome::Kind::kDelta;
        outcome->summary_text = "实时增量快照已创建";
        break;
      case IncrementalOutcome::Kind::kNoChanges:
        outcome->kind = RealtimeOutcome::Kind::kNoChanges;
        outcome->summary_text =
            "检测到文件系统事件，但有效备份集合没有变化；未创建快照";
        return true;  // 什么都没写：marker 与 retention 都不需要
    }
    if (incremental.snapshot_file_name == snapshot_file_name) {
      // 引擎可能已经因为"没有可信基线"而写了完整基线：名字一致。
    }
  }

  // 快照真的写出来了才填名字：调用方（CLI / GUI）要用它去 list / restore。
  outcome->snapshot_file_name = snapshot_file_name;

  // ---- marker：必须绑定**实际 archive bytes** ----
  SnapshotIdentity identity;
  std::string identity_error;
  if (!LoadVerifiedSnapshotIdentity(repository_path, snapshot_file_name,
                                    &identity, nullptr, &identity_error)) {
    // 归档已经写好了，但身份验证不过：这是硬错误（不能把不可信的东西写成
    // realtime 快照），但**不删**已经产生的归档——它仍可 list / restore /
    // manual delete。
    outcome->diagnostic =
        "the new snapshot could not be verified: " + identity_error;
    outcome->kind = RealtimeOutcome::Kind::kFailed;
    SetError(error_message, outcome->diagnostic);
    return false;
  }

  RealtimeMarker marker;
  marker.snapshot_file_name = snapshot_file_name;
  marker.snapshot_id = identity.snapshot_id;
  marker.created_time_sec = now_sec;
  marker.job_identity = job_identity;
  marker.source_identity = source_identity;
  marker.filter_identity = filter_identity;
  marker.strategy = config.strategy;
  marker.pack_method = config.pack_method;
  marker.compression_method = config.compression_method;
  marker.event_count = events.event_count;
  marker.overflow_recovery = events.overflow;
  marker.resync_trigger = events.resync;
  switch (outcome->kind) {
    case RealtimeOutcome::Kind::kFullSnapshot:
      marker.outcome_kind = "full";
      break;
    case RealtimeOutcome::Kind::kFullBaseline:
      marker.outcome_kind = "full-baseline";
      break;
    case RealtimeOutcome::Kind::kDelta:
      marker.outcome_kind = "delta";
      break;
    default:
      marker.outcome_kind = "no-changes";
      break;
  }
  marker.added = outcome->changes.added;
  marker.removed = outcome->changes.removed;
  marker.modified = outcome->changes.modified;
  marker.metadata_changed = outcome->changes.metadata_changed;

  std::string marker_error;
  if (!WriteRealtimeMarker(repository_path, marker, &marker_error)) {
    // archive 保留；本轮退化成"成功但 ownership 有警告"，而且**不执行**破坏性
    // realtime retention（没有 marker 就没有可信的归属，更不能猜着删）。
    outcome->marker_written = false;
    outcome->marker_warning = true;
    outcome->diagnostic =
        "the snapshot was created, but its realtime marker could not be "
        "written: " +
        marker_error;
    return true;
  }
  outcome->marker_written = true;

  // ---- retention：复用 PR #18 的依赖感知计划 + descendants-first 删除 ----
  RealtimeRetentionResult retention;
  std::string retention_error;
  if (RunRealtimeRetention(repository_path, job_identity, config.retain_count,
                           &retention, &retention_error)) {
    outcome->retention_deleted = retention.deleted;
    outcome->retention_kept_ancestors = retention.kept_ancestors;
    outcome->retention_uncertain = retention.uncertain;
    if (retention.uncertain && !retention.reason.empty() &&
        retention.reason != "nothing to remove") {
      if (!outcome->diagnostic.empty()) outcome->diagnostic += " ";
      outcome->diagnostic += "realtime retention warning: " + retention.reason;
    }
  } else if (!retention_error.empty()) {
    if (!outcome->diagnostic.empty()) outcome->diagnostic += " ";
    outcome->diagnostic += "realtime retention failed: " + retention_error;
  }
  return true;
}

}  // namespace backupproject
