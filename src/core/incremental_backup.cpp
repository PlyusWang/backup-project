// incremental_backup.cpp
//
// 见 include/incremental_backup.h。

#include "incremental_backup.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include "backup_catalog.h"
#include "incremental_delta.h"
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

bool IsPlainSingleComponentName(const std::string& name) {
  if (name.empty() || name == "." || name == "..") return false;
  if (name.find('/') != std::string::npos) return false;
  if (name.find('\\') != std::string::npos) return false;
  if (name.find('\0') != std::string::npos) return false;
  return true;
}

std::string JoinPath(const std::string& directory, const std::string& name) {
  if (directory.empty()) return name;
  if (directory.back() == '/') return directory + name;
  return directory + "/" + name;
}

bool FileExists(const std::string& path) {
  struct stat info;
  return ::lstat(path.c_str(), &info) == 0;
}

// 只读整个文件（manifest 副文件有大小上界）。
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
  if (static_cast<std::uint64_t>(info.st_size) > kMaxManifestBytes) {
    ::close(fd);
    SetError(error_message, "Manifest is too large: " + path);
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
    SetError(error_message, "Manifest is truncated: " + path);
    return false;
  }
  return true;
}

// 写 manifest 副文件：唯一临时文件 + fsync + rename，权限 0600。
bool WriteManifestFile(const std::string& path, const std::string& text,
                       std::string* error_message) {
  const std::string temp =
      path + "." + std::to_string(static_cast<unsigned long>(::getpid())) +
      ".part";
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

// 把 manifest 条目映射成"要写进 payload 的 ArchiveEntry"。路径与元数据来自
// 这一次的真实扫描（manifest 是同一套扫描器产出的）。
bool BuildChangedEntries(const std::string& source_directory,
                         const std::vector<std::string>& changed_paths,
                         const std::vector<ManifestEntry>& current,
                         std::vector<ArchiveEntry>* entries,
                         std::string* error_message) {
  entries->clear();
  // 源根永远进 payload：delta 应用之后根目录自己的 metadata 要有人负责。
  bool has_root = false;
  for (const ManifestEntry& entry : current) {
    if (entry.archive_path == ".") {
      ArchiveEntry root;
      root.archive_path = ".";
      root.source_path = source_directory;
      root.type = EntryType::kDirectory;
      root.mode = entry.mode;
      root.uid = entry.uid;
      root.gid = entry.gid;
      root.mtime_sec = entry.mtime_sec;
      root.mtime_nsec = entry.mtime_nsec;
      entries->push_back(root);
      has_root = true;
      break;
    }
  }
  if (!has_root) {
    SetError(error_message, "The scanned manifest has no source root entry");
    return false;
  }

  // 变化路径的**所有祖先目录**也要进 payload。
  //
  // 理由不是"顺手带上"，而是正确性：应用 delta 时会往这些目录里写子项，
  // 而写子项必然改掉目录自己的 mtime/大小。如果目录的权威 metadata 不在
  // delta 里，合并阶段就只能拿一个"刚刚被创建出来的目录"的 metadata 顶上去，
  // 结果链恢复出来的目录 mtime 与完整恢复不一致。
  std::vector<std::string> wanted = changed_paths;
  for (const std::string& path : changed_paths) {
    std::size_t slash = path.rfind('/');
    while (slash != std::string::npos) {
      const std::string parent = path.substr(0, slash);
      if (std::find(wanted.begin(), wanted.end(), parent) == wanted.end()) {
        wanted.push_back(parent);
      }
      slash = parent.rfind('/');
    }
  }

  for (const ManifestEntry& entry : current) {
    if (entry.archive_path == ".") continue;
    if (std::find(wanted.begin(), wanted.end(), entry.archive_path) ==
        wanted.end()) {
      continue;
    }
    ArchiveEntry out;
    out.archive_path = entry.archive_path;
    out.source_path = entry.source_path;
    out.type = entry.type;
    out.mode = entry.mode;
    out.uid = entry.uid;
    out.gid = entry.gid;
    out.mtime_sec = entry.mtime_sec;
    out.mtime_nsec = entry.mtime_nsec;
    out.size = entry.size;
    out.link_target = entry.link_target;
    out.dev_major = entry.dev_major;
    out.dev_minor = entry.dev_minor;
    entries->push_back(out);
  }
  return true;
}

}  // namespace

bool IsSupportedIncrementalPack(PackMethod pack) {
  return pack == PackMethod::kMyPack;
}

std::string UnsupportedIncrementalPackReason() {
  return "Incremental backup currently supports the MyPack pack method only: "
         "USTAR cannot express tombstones or parent dependencies, so an "
         "incremental chain built on it could not be applied correctly.";
}

std::string SnapshotManifestFileName(const std::string& snapshot_file_name) {
  return snapshot_file_name + ".manifest";
}

std::string SnapshotIdentityFileName(const std::string& snapshot_file_name) {
  return snapshot_file_name + ".identity";
}

namespace {

// 身份副文件：行式 key=value，严格解析（多了少了都算坏）。
std::string SerializeSnapshotIdentity(const std::string& source_identity,
                                      const std::string& filter_identity,
                                      const std::string& strategy_identity) {
  std::string out = "BPIDENT1\n";
  out += "source=" + source_identity + "\n";
  out += "filter=" + filter_identity + "\n";
  out += "strategy=" + strategy_identity + "\n";
  return out;
}

bool ParseSnapshotIdentity(const std::string& text,
                           std::string* source_identity,
                           std::string* filter_identity,
                           std::string* strategy_identity,
                           std::string* error_message) {
  const std::string header = "BPIDENT1\n";
  if (text.compare(0, std::min(header.size(), text.size()), header) != 0) {
    SetError(error_message, "Invalid snapshot identity: wrong header");
    return false;
  }
  int seen[3] = {0, 0, 0};
  std::size_t position = header.size();
  while (position < text.size()) {
    const std::size_t newline = text.find('\n', position);
    if (newline == std::string::npos) {
      SetError(error_message, "Invalid snapshot identity: truncated line");
      return false;
    }
    const std::string line = text.substr(position, newline - position);
    position = newline + 1;
    if (line.empty()) continue;
    const std::size_t equals = line.find('=');
    if (equals == std::string::npos) {
      SetError(error_message, "Invalid snapshot identity: a line has no '='");
      return false;
    }
    const std::string key = line.substr(0, equals);
    const std::string value = line.substr(equals + 1);
    if (!IsContentDigest(value)) {
      SetError(error_message,
               "Invalid snapshot identity: bad digest for '" + key + "'");
      return false;
    }
    if (key == "source") {
      ++seen[0];
      if (source_identity != nullptr) *source_identity = value;
    } else if (key == "filter") {
      ++seen[1];
      if (filter_identity != nullptr) *filter_identity = value;
    } else if (key == "strategy") {
      ++seen[2];
      if (strategy_identity != nullptr) *strategy_identity = value;
    } else {
      SetError(error_message,
               "Invalid snapshot identity: unknown key '" + key + "'");
      return false;
    }
  }
  if (seen[0] != 1 || seen[1] != 1 || seen[2] != 1) {
    SetError(error_message, "Invalid snapshot identity: missing key");
    return false;
  }
  return true;
}

}  // namespace

bool LoadSnapshotManifest(const std::string& repository_directory,
                          const std::string& snapshot_file_name,
                          std::vector<ManifestEntry>* entries,
                          std::string* error_message) {
  if (!IsPlainSingleComponentName(snapshot_file_name)) {
    SetError(error_message, "A snapshot file name must be a single component");
    return false;
  }
  const std::string path = JoinPath(
      repository_directory, SnapshotManifestFileName(snapshot_file_name));
  std::string text;
  if (!ReadWholeFile(path, &text, error_message)) return false;
  ManifestBinding binding;
  return ParseManifest(text, entries, &binding, error_message);
}

bool FindIncrementalBaseline(const std::string& repository_directory,
                             const std::string& source_path,
                             const std::string& repository_identity,
                             const std::string& filter_identity,
                             const std::string& strategy_identity,
                             std::string* baseline_file_name,
                             std::string* reason) {
  if (baseline_file_name != nullptr) baseline_file_name->clear();
  if (reason != nullptr) reason->clear();

  std::string catalog_error;
  std::vector<BackupRecord> listed;
  BackupCatalog catalog;
  if (!catalog.List(repository_directory, &listed, &catalog_error)) {
    if (reason != nullptr) {
      *reason = "cannot list the repository: " + catalog_error;
    }
    return false;
  }

  // 文件名倒序 = 最新的在前（catalog 的名字带时间戳）。
  std::vector<std::string> names;
  for (const BackupRecord& record : listed) names.push_back(record.file_name);
  std::sort(names.begin(), names.end(), std::greater<std::string>());

  for (const std::string& name : names) {
    const std::string manifest_path =
        JoinPath(repository_directory, SnapshotManifestFileName(name));
    if (!FileExists(manifest_path)) continue;
    std::vector<ManifestEntry> entries;
    ManifestBinding binding;
    std::string text;
    std::string error;
    if (!ReadWholeFile(manifest_path, &text, &error)) continue;
    if (!ParseManifest(text, &entries, &binding, &error)) continue;
    if (!HasContentDigests(entries)) {
      if (reason != nullptr) {
        *reason = "the newest candidate snapshot has no content identity";
      }
      continue;
    }
    if (binding.snapshot_file_name != name ||
        binding.repository_identity != repository_identity ||
        binding.source_path != source_path) {
      if (reason != nullptr) {
        *reason =
            "the newest candidate snapshot belongs to a different "
            "repository, source or generation";
      }
      continue;
    }
    // 身份副文件：源的 identity 与 binding 必须自洽；filter / strategy 变了
    // 就必须重新建基线，绝不跨参数继续链。
    std::string identity_text;
    std::string identity_error;
    std::string recorded_source;
    std::string recorded_filter;
    std::string recorded_strategy;
    if (!ReadWholeFile(
            JoinPath(repository_directory, SnapshotIdentityFileName(name)),
            &identity_text, &identity_error) ||
        !ParseSnapshotIdentity(identity_text, &recorded_source,
                               &recorded_filter, &recorded_strategy,
                               &identity_error)) {
      if (reason != nullptr) {
        *reason =
            "the newest candidate snapshot has no usable identity "
            "record (" +
            identity_error + ")";
      }
      continue;
    }
    if (recorded_source !=
        SourceIdentityDigest(source_path, repository_identity)) {
      if (reason != nullptr) {
        *reason = "the recorded source identity does not match this source";
      }
      continue;
    }
    if (recorded_filter != filter_identity) {
      if (reason != nullptr) {
        *reason = "the filter rules changed since the last snapshot";
      }
      continue;
    }
    if (recorded_strategy != strategy_identity) {
      if (reason != nullptr) {
        *reason =
            "the pack / compression / encryption settings changed since "
            "the last snapshot";
      }
      continue;
    }
    if (baseline_file_name != nullptr) *baseline_file_name = name;
    return true;
  }
  if (reason != nullptr && reason->empty()) {
    *reason = "no snapshot with a usable content manifest was found";
  }
  return false;
}

bool SnapshotParentOf(const std::string& repository_directory,
                      const std::string& snapshot_file_name,
                      std::string* parent_file_name,
                      std::string* error_message) {
  if (parent_file_name == nullptr) {
    SetError(error_message, "Parent output must not be null");
    return false;
  }
  parent_file_name->clear();
  if (!IsPlainSingleComponentName(snapshot_file_name)) {
    SetError(error_message, "A snapshot file name must be a single component");
    return false;
  }
  const std::string path = JoinPath(repository_directory, snapshot_file_name);
  const SnapshotFileKind kind = ClassifySnapshotFile(path, error_message);
  if (kind == SnapshotFileKind::kContainer) return true;
  if (kind == SnapshotFileKind::kUnknown) return false;
  DeltaEnvelope envelope;
  if (!ReadDeltaEnvelope(path, &envelope, error_message)) return false;
  *parent_file_name = envelope.parent_file_name;
  return true;
}

bool PlanDependencyAwareRetention(
    const std::string& repository_directory,
    const std::vector<std::string>& candidates_oldest_first,
    std::size_t retain_count, RetentionPlan* plan, std::string* error_message) {
  if (plan == nullptr) {
    SetError(error_message, "Retention plan output must not be null");
    return false;
  }
  *plan = RetentionPlan{};

  const std::size_t total = candidates_oldest_first.size();
  // 可见集合 = 最近 retain_count 个（候选里最旧在前，所以取尾部）。
  const std::size_t visible_count = retain_count < total ? retain_count : total;
  const std::size_t visible_begin = total - visible_count;
  for (std::size_t index = visible_begin; index < total; ++index) {
    plan->keep_visible.push_back(candidates_oldest_first[index]);
  }

  // 依赖闭包：从可见集合出发，沿 parent 往上走，只认候选集合里的名字。
  // visited 兼作环保护：坏链最多让某个名字被访问一次。
  std::vector<std::string> keep = plan->keep_visible;
  std::vector<std::string> visited = keep;
  for (std::size_t cursor = 0; cursor < keep.size(); ++cursor) {
    const std::string current = keep[cursor];
    // depth 由 visited 的数量天然限制：候选集合有限，且每个只访问一次。
    if (visited.size() > kMaxDeltaChainDepth * 64 &&
        keep.size() > candidates_oldest_first.size()) {
      SetError(error_message, "Retention dependency walk did not terminate");
      return false;
    }
    std::string parent;
    if (!SnapshotParentOf(repository_directory, current, &parent,
                          error_message)) {
      return false;
    }
    if (parent.empty()) continue;
    if (std::find(candidates_oldest_first.begin(),
                  candidates_oldest_first.end(),
                  parent) == candidates_oldest_first.end()) {
      // 祖先不归本计划管理：不动它，也不需要继续往上走。
      continue;
    }
    if (std::find(visited.begin(), visited.end(), parent) != visited.end()) {
      continue;
    }
    visited.push_back(parent);
    keep.push_back(parent);
    plan->keep_ancestors.push_back(parent);
  }

  for (const std::string& name : candidates_oldest_first) {
    if (std::find(keep.begin(), keep.end(), name) != keep.end()) continue;
    plan->remove.push_back(name);
  }
  return true;
}

bool RunIncrementalBackup(const std::string& source_directory,
                          const std::string& repository_directory,
                          const std::string& snapshot_file_name,
                          const std::string& repository_identity,
                          const Filter& filter, const BackupOptions& options,
                          const std::vector<std::string>& include_rules,
                          const std::vector<std::string>& exclude_rules,
                          const std::string& baseline_snapshot_name,
                          IncrementalOutcome* outcome,
                          std::string* error_message) {
  if (outcome == nullptr) {
    SetError(error_message, "Incremental outcome output must not be null");
    return false;
  }
  *outcome = IncrementalOutcome{};
  if (!IsPlainSingleComponentName(snapshot_file_name)) {
    SetError(error_message, "A snapshot file name must be a single component");
    return false;
  }

  // 这一次的身份：源 / 仓库 / 规则。任何一个变了都不沿用旧链。
  const std::string source_identity =
      SourceIdentityDigest(source_directory, repository_identity);
  const std::string filter_identity =
      FilterIdentityDigest(include_rules, exclude_rules);
  const std::string strategy_identity =
      StrategyIdentityDigest(options.pack_method, options.compression_method,
                             options.encryption_method);

  // 1) 当前源树的强 manifest（内容身份）。
  std::vector<ManifestEntry> current;
  if (!BuildStrongSourceManifest(source_directory, &filter, &current,
                                 error_message)) {
    return false;
  }
  const std::string current_digest = ManifestDigest(current);

  // 2) 找基线。
  std::string baseline = baseline_snapshot_name;
  std::string reason;
  if (!baseline.empty()) {
    if (!FileExists(JoinPath(repository_directory, baseline))) {
      baseline.clear();
      reason = "the requested baseline snapshot does not exist";
    }
  } else {
    FindIncrementalBaseline(repository_directory, source_directory,
                            repository_identity, filter_identity,
                            strategy_identity, &baseline, &reason);
  }

  std::vector<ManifestEntry> previous;
  if (!baseline.empty()) {
    std::string load_error;
    ManifestBinding binding;
    std::vector<ManifestEntry> loaded;
    const std::string path =
        JoinPath(repository_directory, SnapshotManifestFileName(baseline));
    std::string text;
    bool usable = ReadWholeFile(path, &text, &load_error) &&
                  ParseManifest(text, &loaded, &binding, &load_error) &&
                  HasContentDigests(loaded) &&
                  binding.snapshot_file_name == baseline &&
                  binding.repository_identity == repository_identity &&
                  binding.source_path == source_directory;
    if (!usable) {
      reason =
          "the recorded baseline is no longer trustworthy (" + load_error + ")";
      baseline.clear();
    } else {
      previous = std::move(loaded);
    }
  }

  // 3) 没有可信基线 -> 建一份完整 baseline。绝不写一个指向不存在父亲的 delta。
  if (baseline.empty()) {
    const std::string target =
        JoinPath(repository_directory, snapshot_file_name);
    // 完整基线 = 当前整棵有效源树的条目（与真实备份看到的集合完全一致）。
    std::vector<std::string> every;
    every.reserve(current.size());
    for (const ManifestEntry& entry : current) {
      every.push_back(entry.archive_path);
    }
    std::vector<ArchiveEntry> all;
    if (!BuildChangedEntries(source_directory, every, current, &all,
                             error_message)) {
      return false;
    }
    if (!RunBackupPipelineFromEntries(all, target, options, error_message)) {
      return false;
    }
    // 快照发布成功之后才写 manifest：反过来会留下"基线存在但没有快照"的状态。
    ManifestBinding binding;
    binding.snapshot_file_name = snapshot_file_name;
    binding.repository_identity = repository_identity;
    binding.source_path = source_directory;
    const std::string text = SerializeManifestV3(current, binding);
    if (text.empty()) {
      SetError(error_message,
               "Cannot serialize the baseline manifest for this snapshot");
      return false;
    }
    if (!WriteManifestFile(
            JoinPath(repository_directory,
                     SnapshotManifestFileName(snapshot_file_name)),
            text, error_message)) {
      // 快照已经发布、但它的副文件写不出去：这份快照谁也不认识，留着只会
      // 变成"看起来像备份、其实没有任何东西指向它"的孤儿。撤回它，
      // 并且明确报告失败。
      ::unlink(target.c_str());
      return false;
    }
    if (!WriteManifestFile(
            JoinPath(repository_directory,
                     SnapshotIdentityFileName(snapshot_file_name)),
            SerializeSnapshotIdentity(source_identity, filter_identity,
                                      strategy_identity),
            error_message)) {
      ::unlink(target.c_str());
      ::unlink(JoinPath(repository_directory,
                        SnapshotManifestFileName(snapshot_file_name))
                   .c_str());
      return false;
    }
    outcome->kind = IncrementalOutcome::Kind::kFullBaseline;
    outcome->snapshot_file_name = snapshot_file_name;
    outcome->baseline_reason = reason;
    outcome->summary.added = current.size();
    outcome->summary_text =
        "Requested strategy = Incremental, but no trustworthy baseline was "
        "available, so a full baseline snapshot was created";
    return true;
  }

  // 4) 与基线比较。
  ChangeSummary summary;
  std::vector<std::string> changed_paths;
  if (!DiffManifests(previous, current, &summary, &changed_paths,
                     error_message)) {
    return false;
  }
  if (summary.empty()) {
    outcome->kind = IncrementalOutcome::Kind::kNoChanges;
    outcome->parent_file_name = baseline;
    outcome->summary = summary;
    outcome->summary_text = "No effective changes; no new snapshot was created";
    return true;
  }

  // 5) 写 delta。removed 变成 tombstone，其余进 payload。
  std::vector<ArchiveEntry> changed_entries;
  if (!BuildChangedEntries(source_directory, changed_paths, current,
                           &changed_entries, error_message)) {
    return false;
  }
  std::vector<std::string> tombstones;
  for (const ManifestEntry& entry : previous) {
    const std::string& path = entry.archive_path;
    if (path == ".") continue;
    const bool still_there = std::any_of(current.begin(), current.end(),
                                         [&path](const ManifestEntry& item) {
                                           return item.archive_path == path;
                                         });
    if (!still_there) tombstones.push_back(path);
  }

  DeltaEnvelope envelope;
  // 父身份：文件名 + 身份摘要 + 父的 manifest digest。
  envelope.parent_file_name = baseline;
  std::string parent_id;
  if (!SnapshotIdOfFile(JoinPath(repository_directory, baseline), &parent_id,
                        error_message)) {
    return false;
  }
  envelope.parent_snapshot_id = parent_id;
  envelope.parent_manifest_digest = ManifestDigest(previous);
  envelope.source_identity = source_identity;
  envelope.filter_identity = filter_identity;
  envelope.strategy_identity = strategy_identity;
  envelope.current_manifest_digest = current_digest;
  envelope.created_unix_seconds = static_cast<std::int64_t>(::time(nullptr));
  envelope.added = summary.added;
  envelope.modified = summary.modified;
  envelope.metadata_changed = summary.metadata_changed;
  envelope.removed = summary.removed;
  envelope.tombstones = tombstones;
  for (const ManifestEntry& entry : current) {
    if (entry.type == EntryType::kDirectory) {
      envelope.affected_directories.push_back(entry.archive_path);
    }
  }
  // generation 必须指向**这一条链的底**，而且是身份而不是名字：
  // 父是完整快照时链底就是它自己（parent_id 已经是它的内容身份），
  // 父是 delta 时沿用父记录的 generation。
  if (ClassifySnapshotFile(JoinPath(repository_directory, baseline), nullptr) ==
      SnapshotFileKind::kDelta) {
    DeltaEnvelope parent_envelope;
    std::string read_error;
    if (!ReadDeltaEnvelope(JoinPath(repository_directory, baseline),
                           &parent_envelope, &read_error)) {
      SetError(error_message, read_error);
      return false;
    }
    envelope.base_generation_id = parent_envelope.base_generation_id;
  } else {
    envelope.base_generation_id = parent_id;
  }

  if (!WriteDeltaFile(JoinPath(repository_directory, snapshot_file_name),
                      envelope, changed_entries, options, error_message)) {
    return false;
  }
  ManifestBinding binding;
  binding.snapshot_file_name = snapshot_file_name;
  binding.repository_identity = repository_identity;
  binding.source_path = source_directory;
  const std::string text = SerializeManifestV3(current, binding);
  const std::string delta_path =
      JoinPath(repository_directory, snapshot_file_name);
  if (text.empty() ||
      !WriteManifestFile(JoinPath(repository_directory,
                                  SnapshotManifestFileName(snapshot_file_name)),
                         text, error_message)) {
    if (error_message != nullptr && error_message->empty()) {
      SetError(error_message, "Cannot serialize the delta manifest");
    }
    // 与完整基线同样处理：不留下没人认识的孤儿快照。
    ::unlink(delta_path.c_str());
    return false;
  }
  if (!WriteManifestFile(
          JoinPath(repository_directory,
                   SnapshotIdentityFileName(snapshot_file_name)),
          SerializeSnapshotIdentity(source_identity, filter_identity,
                                    strategy_identity),
          error_message)) {
    ::unlink(delta_path.c_str());
    ::unlink(JoinPath(repository_directory,
                      SnapshotManifestFileName(snapshot_file_name))
                 .c_str());
    return false;
  }

  outcome->kind = IncrementalOutcome::Kind::kDelta;
  outcome->snapshot_file_name = snapshot_file_name;
  outcome->parent_file_name = baseline;
  outcome->summary = summary;
  outcome->summary_text = "Incremental delta written on top of " + baseline;
  return true;
}

}  // namespace backupproject
