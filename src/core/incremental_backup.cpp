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
#include <map>
#include <string>
#include <utility>
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
  // fsync 与 close 必须**各自**尝试：`fsync(fd) != 0 || close(fd) != 0` 会在
  // fsync 失败时短路掉 close，每失败一次泄漏一个 fd（ENOSPC/EIO 正是会连续
  // 失败的那种场景）。saved_error 只记第一次失败的原因，后面的清理步骤不得
  // 改写它。
  int saved_error = 0;
  if (::fsync(fd) != 0) {
    saved_error = errno;
  }
  if (::close(fd) != 0 && saved_error == 0) {
    saved_error = errno;
  }
  if (saved_error != 0) {
    const std::string message = ErrnoText(saved_error);
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
    // 内容绑定：manifest 的摘要是"期望值"，写侧边写边算、写完核对。
    // 这一条只在增量路径上生效——完整备份不需要，也不该因此变慢。
    out.expected_content_digest = entry.content_digest;
    // FIFO / 设备没有正文摘要，靠最后一次 lstat 比对兜住。
    out.expect_source_unchanged = true;
    entries->push_back(out);
  }
  return true;
}

}  // namespace

namespace {

// ---- hardlink group 的成员模型 ----
//
// 强 manifest 对一组硬链接的记录方式是：leader 那条是普通文件（正文摘要在它
// 身上），其余成员是指向 leader 的 hardlink 条目。这个模型必须在"判变化"和
// "选 payload"两处都被当成**一个整体**：
//
//     leader + peer 共享一个 inode，只改内容、不动链接关系
//       → 旧行为：只有 leader 被算成 modified，delta 里只有它
//       → 恢复 overlay 里 leader 是新 inode、peer 还是老 inode
//       → 合并之后 group 被拆开，peer 留着老内容
//
// 所以"组里任何一个成员变了"必须扩张成"整组成员都进 changed set"。
std::vector<std::string> HardlinkGroupOf(
    const std::vector<ManifestEntry>& entries,
    const std::string& archive_path) {
  std::vector<std::string> members;
  std::string leader = archive_path;
  for (const ManifestEntry& entry : entries) {
    if (entry.archive_path == archive_path &&
        entry.type == EntryType::kHardLink && !entry.link_target.empty()) {
      leader = entry.link_target;
      break;
    }
  }
  members.push_back(leader);
  for (const ManifestEntry& entry : entries) {
    if (entry.type != EntryType::kHardLink) continue;
    if (entry.link_target != leader) continue;
    if (std::find(members.begin(), members.end(), entry.archive_path) ==
        members.end()) {
      members.push_back(entry.archive_path);
    }
  }
  return members;
}

// 把 changed set 扩张到"所有被触及的 hardlink group 的全部当前成员"。
void ExpandHardlinkGroups(const std::vector<ManifestEntry>& current,
                          std::vector<std::string>* changed_paths) {
  std::vector<std::string> expanded = *changed_paths;
  for (const std::string& path : *changed_paths) {
    const bool present = std::any_of(current.begin(), current.end(),
                                     [&path](const ManifestEntry& entry) {
                                       return entry.archive_path == path;
                                     });
    if (!present) continue;
    for (const std::string& member : HardlinkGroupOf(current, path)) {
      const bool in_current =
          std::any_of(current.begin(), current.end(),
                      [&member](const ManifestEntry& entry) {
                        return entry.archive_path == member;
                      });
      if (!in_current) continue;
      if (std::find(expanded.begin(), expanded.end(), member) ==
          expanded.end()) {
        expanded.push_back(member);
      }
    }
  }
  std::sort(expanded.begin(), expanded.end());
  *changed_paths = std::move(expanded);
}

// 身份副文件的序列化只有一份实现，定义在本文件后面的匿名命名空间里；
// 这里先用一次声明，避免把那份实现搬到别处去。
std::string SerializeSnapshotIdentity(const std::string& snapshot_file_name,
                                      const std::string& snapshot_id,
                                      const std::string& manifest_digest,
                                      const std::string& source_identity,
                                      const std::string& filter_identity,
                                      const std::string& strategy_identity);

// 快照已经写出来之后，两个副文件必须一起成功；任何一个失败就把本轮写下的
// 东西全部撤回。只撤自己刚创建的那些：这里不碰任何既有文件。
bool PublishSnapshotSidecars(
    const std::string& repository_directory,
    const std::string& snapshot_file_name, const std::string& snapshot_path,
    const std::string& snapshot_id, const std::string& manifest_digest,
    const std::vector<ManifestEntry>& entries,
    const std::string& repository_identity, const std::string& source_directory,
    const std::string& source_identity, const std::string& filter_identity,
    const std::string& strategy_identity, std::string* error_message) {
  const std::string manifest_path = JoinPath(
      repository_directory, SnapshotManifestFileName(snapshot_file_name));
  const std::string identity_path = JoinPath(
      repository_directory, SnapshotIdentityFileName(snapshot_file_name));

  ManifestBinding binding;
  binding.snapshot_file_name = snapshot_file_name;
  binding.repository_identity = repository_identity;
  binding.source_path = source_directory;
  const std::string text = SerializeManifestV3(entries, binding);
  if (text.empty()) {
    SetError(error_message, "Cannot serialize the manifest for this snapshot");
    ::unlink(snapshot_path.c_str());
    return false;
  }
  if (!WriteManifestFile(manifest_path, text, error_message)) {
    ::unlink(snapshot_path.c_str());
    return false;
  }
  const std::string identity_text = SerializeSnapshotIdentity(
      snapshot_file_name, snapshot_id, manifest_digest, source_identity,
      filter_identity, strategy_identity);
  if (!WriteManifestFile(identity_path, identity_text, error_message)) {
    ::unlink(manifest_path.c_str());
    ::unlink(snapshot_path.c_str());
    return false;
  }
  return true;
}

// ---- 测试接缝（进程内、默认空）----
void (*g_manifest_built_hook)(void*) = nullptr;
void* g_manifest_built_hook_context = nullptr;

}  // namespace

void SetIncrementalManifestBuiltHookForTesting(void (*hook)(void* context),
                                               void* context) {
  g_manifest_built_hook = hook;
  g_manifest_built_hook_context = context;
}

bool IsSupportedIncrementalEncryption(EncryptionMethod encryption) {
  return encryption == EncryptionMethod::kNone;
}

std::string UnsupportedIncrementalEncryptionReason() {
  return std::string(
      "Incremental v1 does not support encryption: the incremental envelope "
      "is not authenticated by the encrypted inner container, so its parent "
      "and tombstone fields would be unprotected. Use the full strategy for "
      "encrypted backups.");
}

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
//
//   BPIDENT1                      （旧格式：只有源 / 规则 / 策略，没有绑定）
//   source=<64 hex>
//   filter=<64 hex>
//   strategy=<64 hex>
//
//   BPIDENT2                      （当前格式：多三件绑定 + 一个版本号）
//   version=2
//   snapshot_file_name=<escaped>
//   snapshot_id=<64 hex>
//   manifest_digest=<64 hex>
//   source=<64 hex>
//   filter=<64 hex>
//   strategy=<64 hex>
//
// BPIDENT1 仍然读得出来，但结果是"没有绑定"——调用方必须把它当成不可信基线
// （多建一份完整快照，绝不错误跳过）。升级语义因此是单向安全的。
constexpr const char* kIdentityV1Header = "BPIDENT1\n";
constexpr const char* kIdentityV2Header = "BPIDENT2\n";
constexpr std::uint64_t kIdentityVersion2 = 2;

// 字符串字段的转义只作用于反斜杠、换行、回车、TAB：这几个字符出现在文件名里
// 会让行式格式失去意义，其余字节（含 UTF-8）原样保留。
std::string EscapeIdentityField(const std::string& value) {
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

bool UnescapeIdentityField(const std::string& text, std::string* value) {
  value->clear();
  value->reserve(text.size());
  for (std::size_t index = 0; index < text.size(); ++index) {
    if (text[index] != '\\') {
      value->push_back(text[index]);
      continue;
    }
    if (index + 1 >= text.size()) return false;
    const char escaped = text[++index];
    switch (escaped) {
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

struct SnapshotIdentityRecord {
  std::uint64_t version = 1;
  // version 2 才有意义的三件绑定。
  std::string snapshot_file_name;
  std::string snapshot_id;
  std::string manifest_digest;
  // 两个版本都有。
  std::string source_identity;
  std::string filter_identity;
  std::string strategy_identity;
};

std::string SerializeSnapshotIdentity(const std::string& snapshot_file_name,
                                      const std::string& snapshot_id,
                                      const std::string& manifest_digest,
                                      const std::string& source_identity,
                                      const std::string& filter_identity,
                                      const std::string& strategy_identity) {
  std::string out = kIdentityV2Header;
  out += "version=" + std::to_string(kIdentityVersion2) + "\n";
  out += "snapshot_file_name=" + EscapeIdentityField(snapshot_file_name) + "\n";
  out += "snapshot_id=" + snapshot_id + "\n";
  out += "manifest_digest=" + manifest_digest + "\n";
  out += "source=" + source_identity + "\n";
  out += "filter=" + filter_identity + "\n";
  out += "strategy=" + strategy_identity + "\n";
  return out;
}

bool ParseSnapshotIdentity(const std::string& text,
                           SnapshotIdentityRecord* record,
                           std::string* error_message) {
  if (record == nullptr) {
    SetError(error_message, "Snapshot identity output must not be null");
    return false;
  }
  *record = SnapshotIdentityRecord{};
  const bool is_v2 =
      text.compare(0,
                   std::min(std::string(kIdentityV2Header).size(), text.size()),
                   kIdentityV2Header) == 0;
  const bool is_v1 =
      text.compare(0,
                   std::min(std::string(kIdentityV1Header).size(), text.size()),
                   kIdentityV1Header) == 0;
  if (!is_v1 && !is_v2) {
    SetError(error_message, "Invalid snapshot identity: wrong header");
    return false;
  }
  const std::string header = is_v2 ? kIdentityV2Header : kIdentityV1Header;
  record->version = is_v2 ? kIdentityVersion2 : 1;

  int seen_version = 0;
  int seen_name = 0;
  int seen_id = 0;
  int seen_manifest = 0;
  int seen_source = 0;
  int seen_filter = 0;
  int seen_strategy = 0;
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
    const std::string raw = line.substr(equals + 1);
    std::string value;
    if (key == "snapshot_file_name") {
      ++seen_name;
      if (!UnescapeIdentityField(raw, &value)) {
        SetError(error_message,
                 "Invalid snapshot identity: bad escape in the snapshot name");
        return false;
      }
      if (value.empty() || value.size() > kMaxManifestBindingBytes) {
        SetError(error_message,
                 "Invalid snapshot identity: implausible snapshot name");
        return false;
      }
      record->snapshot_file_name = value;
      continue;
    }
    if (!UnescapeIdentityField(raw, &value)) {
      SetError(error_message,
               "Invalid snapshot identity: bad escape in key '" + key + "'");
      return false;
    }
    if (key == "version") {
      ++seen_version;
      std::uint64_t number = 0;
      for (const char character : value) {
        if (character < '0' || character > '9') {
          SetError(error_message,
                   "Invalid snapshot identity: bad version field");
          return false;
        }
        number = number * 10 + static_cast<std::uint64_t>(character - '0');
        if (number > 1000000) {
          SetError(error_message,
                   "Invalid snapshot identity: bad version field");
          return false;
        }
      }
      if (number != kIdentityVersion2) {
        SetError(
            error_message,
            "Invalid snapshot identity: unsupported version '" + value + "'");
        return false;
      }
      continue;
    }
    // 其余字段都是摘要。
    if (!IsContentDigest(value)) {
      SetError(error_message,
               "Invalid snapshot identity: bad digest for '" + key + "'");
      return false;
    }
    if (key == "snapshot_id") {
      ++seen_id;
      record->snapshot_id = value;
    } else if (key == "manifest_digest") {
      ++seen_manifest;
      record->manifest_digest = value;
    } else if (key == "source") {
      ++seen_source;
      record->source_identity = value;
    } else if (key == "filter") {
      ++seen_filter;
      record->filter_identity = value;
    } else if (key == "strategy") {
      ++seen_strategy;
      record->strategy_identity = value;
    } else {
      SetError(error_message,
               "Invalid snapshot identity: unknown key '" + key + "'");
      return false;
    }
  }
  if (seen_source != 1 || seen_filter != 1 || seen_strategy != 1) {
    SetError(error_message, "Invalid snapshot identity: missing key");
    return false;
  }
  if (is_v2) {
    if (seen_version != 1 || seen_name != 1 || seen_id != 1 ||
        seen_manifest != 1) {
      SetError(error_message,
               "Invalid snapshot identity: missing binding key in a "
               "version 2 record");
      return false;
    }
  } else if (seen_version != 0 || seen_name != 0 || seen_id != 0 ||
             seen_manifest != 0) {
    SetError(error_message,
             "Invalid snapshot identity: a version 1 record carries binding "
             "keys");
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

namespace {

// 读一份快照的身份副文件。缺失 / 解析失败都只是"没有绑定"：返回 false，
// 原因写进 reason。它不是错误——旧仓库里本来就可能没有这个文件。
bool ReadIdentityRecord(const std::string& repository_directory,
                        const std::string& snapshot_file_name,
                        SnapshotIdentityRecord* record, std::string* reason) {
  std::string text;
  std::string error;
  if (!ReadWholeFile(JoinPath(repository_directory,
                              SnapshotIdentityFileName(snapshot_file_name)),
                     &text, &error)) {
    if (reason != nullptr) {
      *reason = "the identity sidecar is missing or unreadable (" + error + ")";
    }
    return false;
  }
  if (!ParseSnapshotIdentity(text, record, &error)) {
    if (reason != nullptr) *reason = error;
    return false;
  }
  return true;
}

}  // namespace

bool LoadVerifiedSnapshotIdentity(const std::string& repository_directory,
                                  const std::string& snapshot_file_name,
                                  SnapshotIdentity* identity,
                                  std::vector<ManifestEntry>* manifest_entries,
                                  std::string* error_message) {
  if (identity == nullptr) {
    SetError(error_message, "Snapshot identity output must not be null");
    return false;
  }
  *identity = SnapshotIdentity{};
  identity->snapshot_file_name = snapshot_file_name;

  // 路径解析走 Catalog：单组件名字、仓库的直接子项、普通文件、非软链接。
  // delta 信封里的 parent_file_name 是不可信输入，这里就是它落地前的那道门。
  BackupCatalog catalog;
  std::string path;
  if (!catalog.Resolve(repository_directory, snapshot_file_name, &path,
                       error_message)) {
    return false;
  }
  identity->archive_path = path;

  identity->kind = ClassifySnapshotFile(path, error_message);
  if (identity->kind == SnapshotFileKind::kUnknown) {
    if (error_message != nullptr && error_message->empty()) {
      SetError(error_message, "Unknown snapshot file: " + snapshot_file_name);
    }
    return false;
  }
  // ---- 实际字节验证：身份只有在这一步之后才算成立 ----
  //
  // 只读 header / 只解析信封拿到的是**声明值**：把 payload 换掉（header 与信封
  // 都不动）它照样成立，而"照样成立"正是 stale/broken 快照被当成可信身份的
  // 来源。所以先证明磁盘上的实际 payload 字节与声明一致，再谈身份。
  if (identity->kind == SnapshotFileKind::kDelta) {
    if (!VerifyDeltaPayload(path, error_message)) return false;
  } else if (!VerifyFullSnapshotPayload(path, &identity->payload_sha256,
                                        error_message)) {
    return false;
  }
  if (!SnapshotIdOfFile(path, &identity->snapshot_id, error_message)) {
    return false;
  }
  if (identity->kind == SnapshotFileKind::kDelta) {
    if (!ReadDeltaEnvelope(path, &identity->envelope, error_message)) {
      return false;
    }
    // 上一步已经证明实际 payload 就是这个摘要。
    identity->payload_sha256 = identity->envelope.payload_sha256;
    identity->parent_file_name = identity->envelope.parent_file_name;
    identity->parent_snapshot_id = identity->envelope.parent_snapshot_id;
    identity->parent_manifest_digest =
        identity->envelope.parent_manifest_digest;
    identity->base_generation_id = identity->envelope.base_generation_id;
    identity->manifest_digest = identity->envelope.current_manifest_digest;
  }

  // ---- manifest 副文件 ----
  std::vector<ManifestEntry> entries;
  ManifestBinding binding;
  std::string manifest_text;
  std::string manifest_error;
  if (!ReadWholeFile(JoinPath(repository_directory,
                              SnapshotManifestFileName(snapshot_file_name)),
                     &manifest_text, &manifest_error) ||
      !ParseManifest(manifest_text, &entries, &binding, &manifest_error) ||
      !HasContentDigests(entries) ||
      binding.snapshot_file_name != snapshot_file_name) {
    identity->sidecar_diagnostic =
        "the manifest sidecar is missing or unusable (" + manifest_error + ")";
    return true;
  }
  identity->manifest_binding = binding;
  const std::string actual_manifest_digest = ManifestDigest(entries);

  // ---- 身份副文件 ----
  SnapshotIdentityRecord record;
  std::string identity_error;
  if (!ReadIdentityRecord(repository_directory, snapshot_file_name, &record,
                          &identity_error)) {
    identity->sidecar_diagnostic = identity_error;
    return true;
  }
  if (record.version != kIdentityVersion2) {
    identity->sidecar_diagnostic =
        "the identity sidecar is an old version without a snapshot binding";
    return true;
  }
  if (record.snapshot_file_name != snapshot_file_name) {
    identity->sidecar_diagnostic =
        "the identity sidecar belongs to '" + record.snapshot_file_name + "'";
    return true;
  }
  if (record.snapshot_id != identity->snapshot_id) {
    identity->sidecar_diagnostic =
        "the identity sidecar records a different archive identity: the "
        "snapshot file was replaced after it was written";
    return true;
  }
  if (record.manifest_digest != actual_manifest_digest) {
    identity->sidecar_diagnostic =
        "the identity sidecar and the manifest sidecar disagree about the "
        "source state";
    return true;
  }
  if (identity->kind == SnapshotFileKind::kDelta &&
      identity->envelope.current_manifest_digest != actual_manifest_digest) {
    identity->sidecar_diagnostic =
        "the delta envelope and the manifest sidecar disagree about the "
        "source state";
    return true;
  }
  identity->source_identity = record.source_identity;
  identity->filter_identity = record.filter_identity;
  identity->strategy_identity = record.strategy_identity;
  identity->manifest_digest = actual_manifest_digest;
  identity->sidecars_verified = true;
  if (manifest_entries != nullptr) *manifest_entries = std::move(entries);
  return true;
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
    // 候选的信任完全建立在"磁盘上的事实"上：归档身份（来自字节）、manifest
    // 摘要（来自副文件内容）、以及副文件自己声明的归属，三者必须自洽。
    // 只"能解析 + 文件存在 + 参数对得上"不再算数——那正是"换掉 .bak、留着旧
    // 副文件"能骗过去的路径。
    SnapshotIdentity identity;
    std::string load_error;
    if (!LoadVerifiedSnapshotIdentity(repository_directory, name, &identity,
                                      nullptr, &load_error)) {
      continue;
    }
    if (!identity.sidecars_verified) {
      if (reason != nullptr) {
        *reason = "the newest candidate snapshot is not trustworthy: " +
                  identity.sidecar_diagnostic;
      }
      continue;
    }
    if (identity.manifest_binding.repository_identity != repository_identity ||
        identity.manifest_binding.source_path != source_path) {
      if (reason != nullptr) {
        *reason =
            "the newest candidate snapshot belongs to a different "
            "repository or source";
      }
      continue;
    }
    if (identity.source_identity !=
        SourceIdentityDigest(source_path, repository_identity)) {
      if (reason != nullptr) {
        *reason = "the recorded source identity does not match this source";
      }
      continue;
    }
    if (identity.filter_identity != filter_identity) {
      if (reason != nullptr) {
        *reason = "the filter rules changed since the last snapshot";
      }
      continue;
    }
    if (identity.strategy_identity != strategy_identity) {
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

bool SnapshotDeltaDepth(const std::string& repository_directory,
                        const std::string& snapshot_file_name,
                        std::size_t* depth, std::string* error_message) {
  if (depth == nullptr) {
    SetError(error_message, "Depth output must not be null");
    return false;
  }
  *depth = 0;
  std::vector<std::string> visited;
  std::string current = snapshot_file_name;
  while (true) {
    if (std::find(visited.begin(), visited.end(), current) != visited.end()) {
      SetError(error_message,
               "Snapshot chain contains a cycle at '" + current + "'");
      return false;
    }
    visited.push_back(current);
    std::string parent;
    if (!SnapshotParentOf(repository_directory, current, &parent,
                          error_message)) {
      return false;
    }
    if (parent.empty()) return true;
    *depth += 1;
    if (*depth > kMaxDeltaChainDepth) {
      // 已经超过上界就没有必要继续走下去：调用方只需要知道"越界了"。
      return true;
    }
    current = parent;
  }
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

  const auto is_candidate =
      [&candidates_oldest_first](const std::string& name) {
        return std::find(candidates_oldest_first.begin(),
                         candidates_oldest_first.end(),
                         name) != candidates_oldest_first.end();
      };

  // ---- 依赖闭包：从可见集合出发，沿 parent 往上走 ----
  //
  // 每一步都走 **LoadVerifiedSnapshotIdentity**（实际字节 +
  // 副文件绑定都验过），
  // 而不是"读得出来就算数"。只要有一个必须保留的点没法把依赖链完整走完——
  // 自己坏了、payload 与声明不符、父缺失、父的绑定无效、成环——就进入
  // fail-closed：remove 清空，这一轮什么都不淘汰。
  //
  // 理由：那种情况下"哪些更老的候选可能是它的祖先"没有答案，而删除不可逆。
  // 宁可不回收，也不删掉一条恢复链的祖先。
  std::vector<std::string> keep = plan->keep_visible;
  std::vector<std::string> traversal = plan->keep_visible;
  std::vector<std::string> visited = traversal;
  std::map<std::string, std::string> parent_of;
  bool uncertain = false;
  for (std::size_t cursor = 0; cursor < traversal.size() && !uncertain;
       ++cursor) {
    const std::string current = traversal[cursor];
    SnapshotIdentity identity;
    std::string verify_error;
    if (!LoadVerifiedSnapshotIdentity(repository_directory, current, &identity,
                                      nullptr, &verify_error)) {
      plan->unreadable.push_back(current);
      uncertain = true;
      plan->uncertainty_reason =
          "the dependency chain cannot be verified at '" + current +
          "': " + verify_error;
      break;
    }
    if (identity.parent_file_name.empty()) continue;  // 完整快照 = 链底
    const std::string parent = identity.parent_file_name;
    parent_of[current] = parent;
    if (std::find(visited.begin(), visited.end(), parent) != visited.end()) {
      // 已经走过（菱形依赖）或者成环：环在下面的深度检查里被抓住。
      continue;
    }
    visited.push_back(parent);
    traversal.push_back(parent);
    if (is_candidate(parent) &&
        std::find(keep.begin(), keep.end(), parent) == keep.end()) {
      keep.push_back(parent);
      plan->keep_ancestors.push_back(parent);
    }
  }

  // 深度/成环检查：每一条走过的链都必须在 kMaxDeltaChainDepth 步内到达链底。
  // 只靠 visited 去重会把环"走成一条有限的路"，那样祖先集合就是错的。
  if (!uncertain) {
    for (const std::string& start : traversal) {
      std::string node = start;
      std::size_t steps = 0;
      while (true) {
        const auto found = parent_of.find(node);
        if (found == parent_of.end()) break;  // 走到没记过的一跳 = 到顶
        if (found->second.empty()) break;     // 完整快照
        if (++steps > kMaxDeltaChainDepth) {
          uncertain = true;
          plan->uncertainty_reason =
              "the dependency chain through '" + start +
              "' is longer than the supported depth or contains a cycle";
          break;
        }
        node = found->second;
      }
      if (uncertain) break;
    }
  }

  if (uncertain) {
    // fail closed：remove 保持为空。
    plan->dependency_uncertain = true;
    plan->remove.clear();
    return true;
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
  // 打包方式与加密方式的边界在核心里也要拦一次：前端已经报过同一句话，
  // 但"只有前端拦"意味着任何直接调用核心的路径都能产出格式上不该存在的
  // 组合。这里拒绝，且不写任何文件。
  if (!IsSupportedIncrementalPack(options.pack_method)) {
    SetError(error_message, UnsupportedIncrementalPackReason());
    return false;
  }
  if (options.encryption_method != EncryptionMethod::kNone) {
    SetError(error_message, UnsupportedIncrementalEncryptionReason());
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

  // 测试接缝：强 manifest 已经建好、payload 还没读。见头文件里的说明。
  if (g_manifest_built_hook != nullptr) {
    g_manifest_built_hook(g_manifest_built_hook_context);
  }

  // 2) 找基线。
  std::string baseline = baseline_snapshot_name;
  std::string reason;
  if (!baseline.empty()) {
    if (!IsManagedBackupFileName(baseline)) {
      baseline.clear();
      reason = "the requested baseline snapshot name is not a managed name";
    }
  } else {
    FindIncrementalBaseline(repository_directory, source_directory,
                            repository_identity, filter_identity,
                            strategy_identity, &baseline, &reason);
  }

  // 基线必须**重新验一遍**，哪怕它是 FindIncrementalBaseline 刚挑出来的、
  // 或者是调用方明确指定的：副文件与归档内容的绑定要逐项对上磁盘事实，
  // 参数身份也要与这一次相同。"文件还在"远远不够。
  std::vector<ManifestEntry> previous;
  if (!baseline.empty()) {
    SnapshotIdentity identity;
    std::string load_error;
    std::vector<ManifestEntry> loaded;
    const bool loaded_ok = LoadVerifiedSnapshotIdentity(
        repository_directory, baseline, &identity, &loaded, &load_error);
    bool usable =
        loaded_ok && identity.sidecars_verified &&
        identity.manifest_binding.repository_identity == repository_identity &&
        identity.manifest_binding.source_path == source_directory &&
        identity.source_identity == source_identity &&
        identity.filter_identity == filter_identity &&
        identity.strategy_identity == strategy_identity;
    if (usable && identity.kind == SnapshotFileKind::kUnknown) usable = false;
    if (!usable) {
      reason = loaded_ok
                   ? "the recorded baseline is no longer trustworthy (" +
                         identity.sidecar_diagnostic + ")"
                   : "the recorded baseline is not usable (" + load_error + ")";
      baseline.clear();
    } else {
      // 再挂一个 delta 会不会越过恢复侧的上界？会的话就不挂：与其产出
      // "创建成功、恢复才发现链太深"的快照，不如老实再建一份完整基线。
      std::size_t parent_depth = 0;
      std::string depth_error;
      if (!SnapshotDeltaDepth(repository_directory, baseline, &parent_depth,
                              &depth_error)) {
        reason = "the recorded baseline is not usable (" + depth_error + ")";
        baseline.clear();
      } else if (parent_depth >= kMaxDeltaChainDepth) {
        reason = "the dependency chain has reached the maximum depth of " +
                 std::to_string(kMaxDeltaChainDepth) +
                 " deltas; a new full baseline was created";
        baseline.clear();
      } else {
        previous = std::move(loaded);
      }
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
    // 快照发布成功之后才写副文件：反过来会留下"基线存在但没有快照"的状态。
    // 副文件里的 snapshot_id 必须来自**刚写出来的这个文件**，这样"副文件属于
    // 这一份 .bak"才有内容是事实而不是声明。
    std::string snapshot_id;
    if (!SnapshotIdOfFile(target, &snapshot_id, error_message)) {
      ::unlink(target.c_str());
      return false;
    }
    if (!PublishSnapshotSidecars(repository_directory, snapshot_file_name,
                                 target, snapshot_id, current_digest, current,
                                 repository_identity, source_directory,
                                 source_identity, filter_identity,
                                 strategy_identity, error_message)) {
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

  // 4b) hardlink group 扩张：只把 leader 放进 delta 会把 group 拆坏
  // （peer 会留在老 inode 上）。整组成员必须一起进 payload。
  ExpandHardlinkGroups(current, &changed_paths);

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
  const std::string delta_path =
      JoinPath(repository_directory, snapshot_file_name);
  std::string snapshot_id;
  if (!SnapshotIdOfFile(delta_path, &snapshot_id, error_message)) {
    ::unlink(delta_path.c_str());
    return false;
  }
  if (!PublishSnapshotSidecars(
          repository_directory, snapshot_file_name, delta_path, snapshot_id,
          current_digest, current, repository_identity, source_directory,
          source_identity, filter_identity, strategy_identity, error_message)) {
    return false;
  }

  outcome->kind = IncrementalOutcome::Kind::kDelta;
  outcome->snapshot_file_name = snapshot_file_name;
  outcome->parent_file_name = baseline;
  outcome->summary = summary;
  outcome->summary_text = "Incremental delta written on top of " + baseline;
  return true;
}

// ---- 依赖图 ----

bool FindReachableDescendants(const std::string& repository_directory,
                              const std::string& snapshot_file_name,
                              std::vector<std::string>* descendants,
                              std::string* error_message) {
  if (descendants == nullptr) {
    SetError(error_message, "Descendant output must not be null");
    return false;
  }
  descendants->clear();

  BackupCatalog catalog;
  std::vector<BackupRecord> listed;
  if (!catalog.List(repository_directory, &listed, error_message)) return false;

  // parent -> children。只从"读得出来的直接子项快照"建边：坏文件、缺失的父、
  // 环都不会让整张图失败，它们只是不贡献边。
  std::map<std::string, std::vector<std::string>> children;
  for (const BackupRecord& record : listed) {
    std::string parent;
    std::string read_error;
    if (!SnapshotParentOf(repository_directory, record.file_name, &parent,
                          &read_error)) {
      continue;
    }
    if (parent.empty()) continue;
    children[parent].push_back(record.file_name);
  }

  // BFS + visited 兼作环保护。
  std::vector<std::string> visited{snapshot_file_name};
  std::vector<std::string> queue{snapshot_file_name};
  for (std::size_t cursor = 0; cursor < queue.size(); ++cursor) {
    const auto found = children.find(queue[cursor]);
    if (found == children.end()) continue;
    for (const std::string& child : found->second) {
      if (std::find(visited.begin(), visited.end(), child) != visited.end()) {
        continue;
      }
      visited.push_back(child);
      queue.push_back(child);
      descendants->push_back(child);
    }
  }
  std::sort(descendants->begin(), descendants->end());
  descendants->erase(std::unique(descendants->begin(), descendants->end()),
                     descendants->end());
  return true;
}

// ---- 副文件生命周期 ----

std::vector<std::string> SnapshotSidecarFileNames(
    const std::string& snapshot_file_name) {
  // PR #19：realtime marker 也是这份快照拥有的 sidecar，删除时跟着走。
  // 常量在这里写字面量而不是 include realtime_backup_service.h，是为了让
  // incremental_backup 不反向依赖 realtime 模块；两边都只认 ".realtime"。
  return {SnapshotManifestFileName(snapshot_file_name),
          SnapshotIdentityFileName(snapshot_file_name),
          snapshot_file_name + ".realtime"};
}

namespace {

constexpr const char* kManifestSuffix = ".manifest";
constexpr const char* kIdentitySuffix = ".identity";
// PR #19：realtime marker（BPREALTIME1）。只有 <managed .bak>.realtime 才算。
constexpr const char* kRealtimeSuffix = ".realtime";

// name 是不是**本项目的**一份快照的副文件名；是的话把主文件名写进 *base。
//
// 只认后缀是不够的：那样 notes.manifest / report.identity 这种普通用户文件
// 只要没有同名 base 就会被当成孤儿删掉——而仓库的边界是"只管自己的 backup
// artifacts"。所以剥掉后缀之后，base 还必须过 BackupCatalog 的唯一命名规则
// （单组件 + .bak 结尾）。
bool SplitSidecarName(const std::string& name, std::string* base) {
  const std::size_t manifest_len = ::strlen(kManifestSuffix);
  const std::size_t identity_len = ::strlen(kIdentitySuffix);
  const std::size_t realtime_len = ::strlen(kRealtimeSuffix);
  std::string candidate;
  if (name.size() > manifest_len &&
      name.compare(name.size() - manifest_len, manifest_len, kManifestSuffix) ==
          0) {
    candidate = name.substr(0, name.size() - manifest_len);
  } else if (name.size() > identity_len &&
             name.compare(name.size() - identity_len, identity_len,
                          kIdentitySuffix) == 0) {
    candidate = name.substr(0, name.size() - identity_len);
  } else if (name.size() > realtime_len &&
             name.compare(name.size() - realtime_len, realtime_len,
                          kRealtimeSuffix) == 0) {
    candidate = name.substr(0, name.size() - realtime_len);
  } else {
    return false;
  }
  if (!IsManagedBackupFileName(candidate)) return false;
  *base = candidate;
  return true;
}

// 主文件还在吗（直接子项、普通文件、非软链接）。
bool SnapshotFileExists(const std::string& repository_directory,
                        const std::string& base_name) {
  if (base_name.empty() || base_name == "." || base_name == "..") return false;
  struct stat info;
  const std::string path = JoinPath(repository_directory, base_name);
  if (::lstat(path.c_str(), &info) != 0) return false;
  return S_ISREG(info.st_mode);
}

}  // namespace

bool FindOrphanSidecars(const std::string& repository_directory,
                        std::vector<std::string>* orphan_file_names,
                        std::string* error_message) {
  if (orphan_file_names == nullptr) {
    SetError(error_message, "Orphan output must not be null");
    return false;
  }
  orphan_file_names->clear();

  DIR* raw = ::opendir(repository_directory.c_str());
  if (raw == nullptr) {
    SetError(error_message, "Cannot open the repository directory " +
                                repository_directory + ": " + ErrnoText(errno));
    return false;
  }
  while (true) {
    // readdir 用 nullptr 同时表示"读完"和"出错"，只能靠 errno 区分：把读错误
    // 当 EOF 会让一次扫描悄悄漏掉一部分副文件（甚至把该清理的当成不存在）。
    errno = 0;
    struct dirent* item = ::readdir(raw);
    if (item == nullptr) {
      if (errno != 0) {
        const std::string text = ErrnoText(errno);
        ::closedir(raw);
        SetError(error_message,
                 "Cannot read the repository directory while looking for "
                 "orphan sidecars: " +
                     text);
        return false;
      }
      break;
    }
    const std::string name = item->d_name;
    if (name == "." || name == "..") continue;
    std::string base;
    if (!SplitSidecarName(name, &base)) continue;
    if (SnapshotFileExists(repository_directory, base)) continue;
    orphan_file_names->push_back(name);
  }
  ::closedir(raw);
  std::sort(orphan_file_names->begin(), orphan_file_names->end());
  return true;
}

bool CleanOrphanSidecars(const std::string& repository_directory,
                         std::vector<std::string>* removed_file_names,
                         std::vector<std::string>* diagnostics,
                         std::string* error_message) {
  if (removed_file_names != nullptr) removed_file_names->clear();
  std::vector<std::string> orphans;
  if (!FindOrphanSidecars(repository_directory, &orphans, error_message)) {
    return false;
  }
  bool all_removed = true;
  for (const std::string& name : orphans) {
    const std::string path = JoinPath(repository_directory, name);
    if (::unlink(path.c_str()) != 0) {
      all_removed = false;
      if (diagnostics != nullptr) {
        diagnostics->push_back("Cannot remove the orphan sidecar " + path +
                               ": " + ErrnoText(errno));
      }
      continue;
    }
    if (removed_file_names != nullptr) removed_file_names->push_back(name);
  }
  // 单个文件删不掉不算整次调用失败：清理是尽力而为，失败已经在 diagnostics 里
  // 如实报出来了。返回 false 只留给"连仓库都打不开"。
  (void)all_removed;
  return true;
}

}  // namespace backupproject
