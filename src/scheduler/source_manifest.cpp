// source_manifest.cpp

#include "source_manifest.h"

#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "source_digest.h"
#include "tree_scanner.h"

namespace backupproject {
namespace {

void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) *error_message = text;
}

// ---- 数字解析：不抛异常，逐位检查溢出 ----
//
// 刻意不用 std::stoull：它会抛 std::out_of_range，"解析失败"与"内部错误"
// 就分不开了。这里全部返回 bool。

bool ParseUnsigned(const std::string& text, std::uint64_t* value) {
  if (text.empty() || text.size() > 20) return false;
  // 拒绝前导零：manifest 由本模块自己写，规范形式就是唯一的合法形式。
  if (text.size() > 1 && text[0] == '0') return false;
  std::uint64_t result = 0;
  for (const char character : text) {
    if (character < '0' || character > '9') return false;
    const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
    if (result > (UINT64_MAX - digit) / 10u) return false;
    result = result * 10u + digit;
  }
  *value = result;
  return true;
}

bool ParseSigned(const std::string& text, std::int64_t* value) {
  if (text.empty()) return false;
  bool negative = false;
  std::string digits = text;
  if (digits[0] == '-') {
    negative = true;
    digits.erase(0, 1);
  }
  std::uint64_t magnitude = 0;
  if (!ParseUnsigned(digits, &magnitude)) return false;
  constexpr std::uint64_t kPositiveLimit = 9223372036854775807ull;
  constexpr std::uint64_t kNegativeLimit = 9223372036854775808ull;
  if (negative) {
    if (magnitude > kNegativeLimit) return false;
    if (magnitude == kNegativeLimit) {
      *value = INT64_MIN;
    } else {
      *value = -static_cast<std::int64_t>(magnitude);
    }
    return true;
  }
  if (magnitude > kPositiveLimit) return false;
  *value = static_cast<std::int64_t>(magnitude);
  return true;
}

// ---- 字符串字段转义 ----
//
// 只转义反斜杠、TAB、换行、回车：路径里出现 TAB 或换行在 Linux 上是合法的，
// 不转义就会把一个字段切成两半；不转义反斜杠则会产生二义性（"\t" 到底是
// 一个 TAB 还是一个反斜杠加 t）。

std::string EscapeField(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const char character : value) {
    switch (character) {
      case '\\':
        escaped += "\\\\";
        break;
      case '\t':
        escaped += "\\t";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      default:
        escaped.push_back(character);
        break;
    }
  }
  return escaped;
}

bool UnescapeField(const std::string& text, std::string* value) {
  value->clear();
  value->reserve(text.size());
  for (std::size_t index = 0; index < text.size(); ++index) {
    const char character = text[index];
    if (character != '\\') {
      value->push_back(character);
      continue;
    }
    if (index + 1 >= text.size()) return false;
    const char escaped = text[++index];
    switch (escaped) {
      case '\\':
        value->push_back('\\');
        break;
      case 't':
        value->push_back('\t');
        break;
      case 'n':
        value->push_back('\n');
        break;
      case 'r':
        value->push_back('\r');
        break;
      default:
        return false;
    }
  }
  return true;
}

bool SplitFields(const std::string& line, std::vector<std::string>* fields) {
  fields->clear();
  std::size_t start = 0;
  while (true) {
    const std::size_t tab = line.find('\t', start);
    if (tab == std::string::npos) {
      fields->push_back(line.substr(start));
      return true;
    }
    fields->push_back(line.substr(start, tab - start));
    start = tab + 1;
    if (fields->size() > 64) return false;  // 字段数上界，防病态输入
  }
}

bool IsValidArchivePath(const std::string& path) {
  if (path.empty()) return false;
  if (path[0] == '/') return false;
  if (path.find('\0') != std::string::npos) return false;
  if (path.size() >= 2 && path.compare(0, 2, "./") == 0) return false;
  if (path.back() == '/') return false;
  return true;
}

bool LessByArchivePath(const ManifestEntry& left, const ManifestEntry& right) {
  return left.archive_path < right.archive_path;
}

// binding 里那份快照名字的**结构**检查：必须是一个单组件文件名——不带路径
// 分隔符、不是 "." / ".."、不含 NUL。".bak 后缀"这条更具体的归档命名规则属于
// BackupCatalog / ScheduleStore 那一层，由 ScheduleStore::LoadManifest 再校验
// 一次；两层各守自己的规则，谁也不替谁放宽。
bool IsPlainSingleComponentName(const std::string& name) {
  if (name.empty()) return false;
  if (name == "." || name == "..") return false;
  if (name.find('/') != std::string::npos) return false;
  if (name.find('\\') != std::string::npos) return false;
  if (name.find('\0') != std::string::npos) return false;
  return true;
}

bool IsValidManifestBinding(const ManifestBinding& binding,
                            std::string* error_message) {
  if (binding.snapshot_file_name.empty()) {
    SetError(error_message,
             "Invalid source manifest: the baseline snapshot file name is "
             "empty");
    return false;
  }
  if (binding.repository_identity.empty()) {
    SetError(error_message,
             "Invalid source manifest: the baseline repository identity is "
             "empty");
    return false;
  }
  if (binding.source_path.empty()) {
    SetError(error_message,
             "Invalid source manifest: the baseline source path is empty");
    return false;
  }
  if (binding.snapshot_file_name.size() > kMaxManifestBindingBytes ||
      binding.repository_identity.size() > kMaxManifestBindingBytes ||
      binding.source_path.size() > kMaxManifestBindingBytes) {
    SetError(error_message,
             "Invalid source manifest: a baseline binding field is too long");
    return false;
  }
  if (binding.repository_identity.find('\0') != std::string::npos ||
      binding.source_path.find('\0') != std::string::npos) {
    SetError(error_message,
             "Invalid source manifest: a baseline binding field contains a NUL "
             "byte");
    return false;
  }
  if (!IsPlainSingleComponentName(binding.snapshot_file_name)) {
    SetError(error_message,
             "Invalid source manifest: the baseline snapshot file name is not "
             "a single path component");
    return false;
  }
  return true;
}

// 条目正文。v1 与 v2 的正文格式完全一样，只有头行不同。
void AppendManifestEntries(const std::vector<ManifestEntry>& entries,
                           std::string* out) {
  for (const ManifestEntry& entry : entries) {
    *out += std::to_string(static_cast<unsigned>(entry.type));
    *out += '\t';
    *out += std::to_string(entry.size);
    *out += '\t';
    *out += std::to_string(entry.mtime_sec);
    *out += '\t';
    *out += std::to_string(entry.mtime_nsec);
    *out += '\t';
    *out += std::to_string(entry.mode);
    *out += '\t';
    *out += std::to_string(entry.uid);
    *out += '\t';
    *out += std::to_string(entry.gid);
    *out += '\t';
    *out += std::to_string(entry.dev_major);
    *out += '\t';
    *out += std::to_string(entry.dev_minor);
    *out += '\t';
    *out += std::to_string(entry.hardlink_degree);
    *out += '\t';
    *out += EscapeField(entry.archive_path);
    *out += '\t';
    *out += EscapeField(entry.link_target);
    *out += '\n';
  }
}

// version 3 的条目行：v2 的 12 个字段之后追加内容摘要（第 13 个字段）。
// 摘要本身是十六进制，转义只是让"字段"这个概念保持统一。
void AppendManifestEntriesV3(const std::vector<ManifestEntry>& entries,
                             std::string* out) {
  for (const ManifestEntry& entry : entries) {
    *out += std::to_string(static_cast<unsigned>(entry.type));
    *out += '\t';
    *out += std::to_string(entry.size);
    *out += '\t';
    *out += std::to_string(entry.mtime_sec);
    *out += '\t';
    *out += std::to_string(entry.mtime_nsec);
    *out += '\t';
    *out += std::to_string(entry.mode);
    *out += '\t';
    *out += std::to_string(entry.uid);
    *out += '\t';
    *out += std::to_string(entry.gid);
    *out += '\t';
    *out += std::to_string(entry.dev_major);
    *out += '\t';
    *out += std::to_string(entry.dev_minor);
    *out += '\t';
    *out += std::to_string(entry.hardlink_degree);
    *out += '\t';
    *out += EscapeField(entry.archive_path);
    *out += '\t';
    *out += EscapeField(entry.link_target);
    *out += '\t';
    *out += EscapeField(entry.content_digest);
    *out += '\n';
  }
}

// 把一次 ScanSourceTree 的结果映射成 manifest 条目。
// source_path 一并带上：强化版要拿它去读正文算摘要。
void ManifestFromScannedEntries(const std::vector<ArchiveEntry>& scanned,
                                std::vector<ManifestEntry>* entries) {
  std::unordered_map<std::string, std::uint32_t> hardlink_degree;
  for (const ArchiveEntry& entry : scanned) {
    if (entry.type != EntryType::kHardLink) continue;
    ++hardlink_degree[entry.link_target];
  }

  entries->clear();
  entries->reserve(scanned.size());
  for (const ArchiveEntry& scanned_entry : scanned) {
    ManifestEntry entry;
    entry.archive_path = scanned_entry.archive_path;
    entry.source_path = scanned_entry.source_path;
    entry.type = scanned_entry.type;
    entry.size = scanned_entry.size;
    entry.mtime_sec = scanned_entry.mtime_sec;
    entry.mtime_nsec = scanned_entry.mtime_nsec;
    entry.mode = scanned_entry.mode;
    entry.uid = scanned_entry.uid;
    entry.gid = scanned_entry.gid;
    entry.link_target = scanned_entry.link_target;
    entry.dev_major = scanned_entry.dev_major;
    entry.dev_minor = scanned_entry.dev_minor;
    const auto found = hardlink_degree.find(entry.archive_path);
    entry.hardlink_degree = found == hardlink_degree.end() ? 0u : found->second;
    entries->push_back(std::move(entry));
  }
}

}  // namespace

std::uint64_t ChangeSummaryTotal(const ChangeSummary& summary) {
  return summary.added + summary.removed + summary.modified +
         summary.metadata_changed;
}

bool BuildSourceManifest(const std::string& source_directory,
                         const Filter* filter,
                         std::vector<ManifestEntry>* entries,
                         std::string* error_message) {
  if (entries == nullptr) {
    SetError(error_message, "Manifest output must not be null");
    return false;
  }
  entries->clear();

  std::vector<ArchiveEntry> scanned;
  // 复用备份自己的扫描器：集合、类型判定、filter 剪枝、socket 规则都只有一份。
  if (!ScanSourceTree(source_directory, filter, &scanned, error_message)) {
    return false;
  }
  if (scanned.size() > kMaxManifestEntries) {
    SetError(error_message, "Source manifest is too large: " +
                                std::to_string(scanned.size()) + " entries");
    return false;
  }

  ManifestFromScannedEntries(scanned, entries);
  return true;
}

bool BuildStrongSourceManifest(const std::string& source_directory,
                               const Filter* filter,
                               std::vector<ManifestEntry>* entries,
                               std::string* error_message) {
  if (entries == nullptr) {
    SetError(error_message, "Manifest output must not be null");
    return false;
  }
  entries->clear();

  std::vector<ArchiveEntry> scanned;
  if (!ScanSourceTree(source_directory, filter, &scanned, error_message)) {
    return false;
  }
  if (scanned.size() > kMaxManifestEntries) {
    SetError(error_message, "Source manifest is too large: " +
                                std::to_string(scanned.size()) + " entries");
    return false;
  }
  ManifestFromScannedEntries(scanned, entries);

  // 全量哈希。第一版不做任何 size+mtime 摘要缓存：缓存的失效判断本身就是
  // correctness 问题，而"漏掉一次变化"在增量链上是会被后代继承的错误。
  for (ManifestEntry& entry : *entries) {
    if (entry.type == EntryType::kRegularFile) {
      std::string digest;
      if (!ContentDigestOfFile(entry.source_path, &digest, error_message)) {
        return false;
      }
      entry.content_digest = std::move(digest);
    } else if (entry.type == EntryType::kSymlink) {
      // 软链接没有"正文"，它的内容就是目标字符串的字节。
      entry.content_digest = ContentDigestOfBytes(entry.link_target);
    }
  }

  // 读正文期间源不许变。变了就整次失败：否则 manifest 会把"读到的内容"与
  // "扫描时记下的元数据"拼成一个从未真实存在过的版本，而增量链会把它当成
  // 一个可信的祖先。
  for (const ManifestEntry& entry : *entries) {
    if (entry.type != EntryType::kRegularFile) continue;
    struct stat info;
    if (::lstat(entry.source_path.c_str(), &info) != 0) {
      SetError(error_message,
               "Source entry disappeared while hashing: " + entry.source_path);
      return false;
    }
    const std::uint64_t size = static_cast<std::uint64_t>(info.st_size);
    const std::int64_t mtime_sec =
        static_cast<std::int64_t>(info.st_mtim.tv_sec);
    const std::uint32_t mtime_nsec =
        static_cast<std::uint32_t>(info.st_mtim.tv_nsec);
    if (!S_ISREG(info.st_mode) || size != entry.size ||
        mtime_sec != entry.mtime_sec || mtime_nsec != entry.mtime_nsec) {
      SetError(error_message,
               "Source changed while hashing: " + entry.source_path);
      return false;
    }
  }
  return true;
}

bool HasContentDigests(const std::vector<ManifestEntry>& entries) {
  for (const ManifestEntry& entry : entries) {
    if (entry.type != EntryType::kRegularFile &&
        entry.type != EntryType::kSymlink) {
      continue;
    }
    if (!IsContentDigest(entry.content_digest)) return false;
  }
  return true;
}

bool DiffManifests(const std::vector<ManifestEntry>& previous,
                   const std::vector<ManifestEntry>& current,
                   ChangeSummary* summary,
                   std::vector<std::string>* changed_paths,
                   std::string* error_message) {
  if (summary == nullptr) {
    SetError(error_message, "Change summary output must not be null");
    return false;
  }
  *summary = ChangeSummary{};
  if (changed_paths != nullptr) changed_paths->clear();

  std::vector<const ManifestEntry*> left;
  std::vector<const ManifestEntry*> right;
  left.reserve(previous.size());
  right.reserve(current.size());
  for (const ManifestEntry& entry : previous) left.push_back(&entry);
  for (const ManifestEntry& entry : current) right.push_back(&entry);
  const auto less = [](const ManifestEntry* a, const ManifestEntry* b) {
    return LessByArchivePath(*a, *b);
  };
  std::sort(left.begin(), left.end(), less);
  std::sort(right.begin(), right.end(), less);

  std::size_t i = 0;
  std::size_t j = 0;
  while (i < left.size() || j < right.size()) {
    if (j >= right.size() ||
        (i < left.size() && left[i]->archive_path < right[j]->archive_path)) {
      ++summary->removed;
      if (changed_paths != nullptr)
        changed_paths->push_back(left[i]->archive_path);
      ++i;
      continue;
    }
    if (i >= left.size() || right[j]->archive_path < left[i]->archive_path) {
      ++summary->added;
      if (changed_paths != nullptr)
        changed_paths->push_back(right[j]->archive_path);
      ++j;
      continue;
    }

    const ManifestEntry& old_entry = *left[i];
    const ManifestEntry& new_entry = *right[j];
    bool is_modified = false;
    bool is_metadata_changed = false;

    if (old_entry.type != new_entry.type) {
      is_modified = true;
    } else {
      switch (new_entry.type) {
        case EntryType::kRegularFile:
          if (old_entry.size != new_entry.size ||
              old_entry.mtime_sec != new_entry.mtime_sec ||
              old_entry.mtime_nsec != new_entry.mtime_nsec) {
            is_modified = true;
          } else if (!old_entry.content_digest.empty() ||
                     !new_entry.content_digest.empty()) {
            // 元数据一致时，内容身份才说话：same-size + same-mtime 的人为
            // in-place rewrite 只有摘要看得出来。两边都有摘要时比摘要；
            // 只有一边有（拿 v3 与 v2 比，例如刚升级完）时无法证明相等，
            // 按变化处理——宁可多重一份完整快照，绝不错误跳过。
            if (old_entry.content_digest != new_entry.content_digest) {
              is_modified = true;
            }
          }
          break;
        case EntryType::kSymlink:
        case EntryType::kHardLink:
          if (old_entry.link_target != new_entry.link_target)
            is_modified = true;
          break;
        case EntryType::kCharDevice:
        case EntryType::kBlockDevice:
          if (old_entry.dev_major != new_entry.dev_major ||
              old_entry.dev_minor != new_entry.dev_minor) {
            is_modified = true;
          }
          break;
        case EntryType::kDirectory:
        case EntryType::kFifo:
        case EntryType::kSocket:
          break;
      }

      if (!is_modified) {
        if (new_entry.type == EntryType::kHardLink) {
          // hardlink 条目的内容身份只有 link_target；inode 上的其它字段由
          // leader 那条记录负责，这里再看一遍就是重复计数。
          is_metadata_changed = false;
        } else if (old_entry.mode != new_entry.mode ||
                   old_entry.uid != new_entry.uid ||
                   old_entry.gid != new_entry.gid) {
          is_metadata_changed = true;
        } else if (new_entry.type != EntryType::kRegularFile &&
                   new_entry.type != EntryType::kDirectory &&
                   (old_entry.mtime_sec != new_entry.mtime_sec ||
                    old_entry.mtime_nsec != new_entry.mtime_nsec)) {
          // FIFO / 软链接 / 设备的 mtime 变化统一算 metadata_changed。
          // 目录刻意不在此列：子项的任何增删都会顺带改掉父目录的 mtime，
          // 把它算进来会让"新增一个被 filter 排除的文件"也触发一次完整快照，
          // 而实际备份集合并没有变。代价是单独 touch 目录看不出来——已知盲区。
          is_metadata_changed = true;
        } else if (old_entry.hardlink_degree != new_entry.hardlink_degree) {
          is_metadata_changed = true;
        }
      }
    }

    if (is_modified) {
      ++summary->modified;
      if (changed_paths != nullptr)
        changed_paths->push_back(new_entry.archive_path);
    } else if (is_metadata_changed) {
      ++summary->metadata_changed;
      if (changed_paths != nullptr)
        changed_paths->push_back(new_entry.archive_path);
    }
    ++i;
    ++j;
  }
  return true;
}

std::string SerializeManifest(const std::vector<ManifestEntry>& entries,
                              const ManifestBinding& binding) {
  // 归属不完整就什么都不写。调用方必须在写盘前拿到一个明确的失败，
  // 而不是一份"看起来正常、其实不知道属于谁"的 manifest。
  std::string binding_error;
  if (!IsValidManifestBinding(binding, &binding_error)) return std::string();

  std::string out;
  out += "BPMANIFEST2 ";
  out += std::to_string(entries.size());
  out += '\t';
  out += EscapeField(binding.snapshot_file_name);
  out += '\t';
  out += EscapeField(binding.repository_identity);
  out += '\t';
  out += EscapeField(binding.source_path);
  out += '\n';
  AppendManifestEntries(entries, &out);
  return out;
}

std::string SerializeManifestV3(const std::vector<ManifestEntry>& entries,
                                const ManifestBinding& binding) {
  std::string binding_error;
  if (!IsValidManifestBinding(binding, &binding_error)) return std::string();

  // 自称 v3 就必须带齐内容身份：普通文件与软链接缺摘要时什么都不写。
  // 写出半份 v3 等于给了增量链一个看起来可用、实际无法校验的基线。
  for (const ManifestEntry& entry : entries) {
    if (entry.type != EntryType::kRegularFile &&
        entry.type != EntryType::kSymlink) {
      continue;
    }
    if (!IsContentDigest(entry.content_digest)) return std::string();
  }

  std::string out;
  out += "BPMANIFEST3 ";
  out += std::to_string(entries.size());
  out += '\t';
  out += EscapeField(binding.snapshot_file_name);
  out += '\t';
  out += EscapeField(binding.repository_identity);
  out += '\t';
  out += EscapeField(binding.source_path);
  out += '\n';
  AppendManifestEntriesV3(entries, &out);
  return out;
}

std::string ManifestDigest(const std::vector<ManifestEntry>& entries) {
  // 先按 archive_path 排序：摘要必须只取决于"源是什么样"，不取决于遍历
  // 恰好以什么顺序产出条目。排序后的顺序就是规范顺序。
  std::vector<const ManifestEntry*> ordered;
  ordered.reserve(entries.size());
  for (const ManifestEntry& entry : entries) ordered.push_back(&entry);
  std::sort(ordered.begin(), ordered.end(),
            [](const ManifestEntry* left, const ManifestEntry* right) {
              return LessByArchivePath(*left, *right);
            });
  std::vector<ManifestEntry> canonical;
  canonical.reserve(ordered.size());
  for (const ManifestEntry* entry : ordered) canonical.push_back(*entry);

  std::string text;
  text += "BPMANIFEST3 ";
  text += std::to_string(canonical.size());
  text += '\n';
  AppendManifestEntriesV3(canonical, &text);
  return ContentDigestOfBytes(text);
}

std::string SerializeManifestV1(const std::vector<ManifestEntry>& entries) {
  std::string out;
  out += "BPMANIFEST1 ";
  out += std::to_string(entries.size());
  out += '\n';
  AppendManifestEntries(entries, &out);
  return out;
}

bool ParseManifest(const std::string& text, std::vector<ManifestEntry>* entries,
                   ManifestBinding* binding, std::string* error_message) {
  if (entries == nullptr) {
    SetError(error_message, "Manifest output must not be null");
    return false;
  }
  if (binding == nullptr) {
    SetError(error_message, "Manifest binding output must not be null");
    return false;
  }
  entries->clear();
  *binding = ManifestBinding{};

  if (text.size() > kMaxManifestBytes) {
    SetError(error_message, "Source manifest is too large: " +
                                std::to_string(text.size()) + " bytes");
    return false;
  }

  // 两个版本头都要认。v1 只是"读得出来"——它的 binding 会留空，调用方据此
  // 判定这是不可信基线，走重建。升级语义因此是单向安全的。
  const std::string header_v3 = "BPMANIFEST3 ";
  const std::string header_v2 = "BPMANIFEST2 ";
  const std::string header_v1 = "BPMANIFEST1 ";
  // is_version_2 的含义是"头行带 binding"，v3 同样带，只是条目多一个字段。
  bool is_version_2 = false;
  bool is_version_3 = false;
  std::size_t header_size = 0;
  if (text.compare(0, std::min(header_v3.size(), text.size()), header_v3) ==
      0) {
    is_version_3 = true;
    is_version_2 = true;
    header_size = header_v3.size();
  } else if (text.compare(0, std::min(header_v2.size(), text.size()),
                          header_v2) == 0) {
    is_version_2 = true;
    header_size = header_v2.size();
  } else if (text.compare(0, std::min(header_v1.size(), text.size()),
                          header_v1) == 0) {
    header_size = header_v1.size();
  } else {
    SetError(error_message,
             "Invalid source manifest: missing or wrong version header");
    return false;
  }
  const std::size_t first_newline = text.find('\n');
  if (first_newline == std::string::npos) {
    SetError(error_message, "Invalid source manifest: truncated header");
    return false;
  }
  const std::string header_rest =
      text.substr(header_size, first_newline - header_size);
  if (header_rest.size() > kMaxManifestLineBytes) {
    SetError(error_message,
             "Invalid source manifest: the header line is too long");
    return false;
  }

  std::uint64_t declared_count = 0;
  if (is_version_2) {
    // 头行结构固定：<count>\t<snapshot>\t<repository>\t<source>，正好四个
    // 字段。少一个、多一个、多一个 TAB 都算坏文件——manifest 是机器写的，
    // 不规范的字节只能说明状态坏了。
    std::vector<std::string> header_fields;
    if (!SplitFields(header_rest, &header_fields) ||
        header_fields.size() != 4) {
      SetError(error_message,
               "Invalid source manifest: a version 2 header must have exactly "
               "four fields");
      return false;
    }
    if (!ParseUnsigned(header_fields[0], &declared_count) ||
        declared_count > kMaxManifestEntries) {
      SetError(error_message, "Invalid source manifest: bad entry count '" +
                                  header_fields[0] + "'");
      return false;
    }
    ManifestBinding parsed_binding;
    if (!UnescapeField(header_fields[1], &parsed_binding.snapshot_file_name) ||
        !UnescapeField(header_fields[2], &parsed_binding.repository_identity) ||
        !UnescapeField(header_fields[3], &parsed_binding.source_path)) {
      SetError(error_message,
               "Invalid source manifest: bad baseline binding escape");
      return false;
    }
    if (!IsValidManifestBinding(parsed_binding, error_message)) return false;
    *binding = std::move(parsed_binding);
  } else {
    if (!ParseUnsigned(header_rest, &declared_count) ||
        declared_count > kMaxManifestEntries) {
      SetError(error_message, "Invalid source manifest: bad entry count '" +
                                  header_rest + "'");
      return false;
    }
  }

  entries->reserve(static_cast<std::size_t>(declared_count));
  std::unordered_set<std::string> seen_paths;
  std::size_t position = first_newline + 1;
  for (std::uint64_t index = 0; index < declared_count; ++index) {
    const std::size_t newline = text.find('\n', position);
    if (newline == std::string::npos) {
      SetError(error_message,
               "Invalid source manifest: fewer entries than the header "
               "declares");
      return false;
    }
    const std::string line = text.substr(position, newline - position);
    position = newline + 1;
    if (line.size() > kMaxManifestLineBytes) {
      SetError(error_message,
               "Invalid source manifest: entry line is too long");
      return false;
    }

    std::vector<std::string> fields;
    const std::size_t expected_fields = is_version_3 ? 13u : 12u;
    if (!SplitFields(line, &fields) || fields.size() != expected_fields) {
      SetError(error_message,
               "Invalid source manifest: an entry does not have " +
                   std::to_string(expected_fields) + " fields");
      return false;
    }

    std::uint64_t type_id = 0;
    if (!ParseUnsigned(fields[0], &type_id) || type_id < 1 || type_id > 7) {
      SetError(error_message,
               "Invalid source manifest: bad entry type '" + fields[0] + "'");
      return false;
    }

    ManifestEntry entry;
    entry.type = static_cast<EntryType>(type_id);

    if (!ParseUnsigned(fields[1], &entry.size) || entry.size > (1ull << 62)) {
      SetError(error_message,
               "Invalid source manifest: bad size '" + fields[1] + "'");
      return false;
    }
    if (!ParseSigned(fields[2], &entry.mtime_sec)) {
      SetError(error_message, "Invalid source manifest: bad mtime seconds '" +
                                  fields[2] + "'");
      return false;
    }
    std::uint64_t mtime_nsec = 0;
    if (!ParseUnsigned(fields[3], &mtime_nsec) || mtime_nsec >= 1000000000ull) {
      SetError(
          error_message,
          "Invalid source manifest: bad mtime nanoseconds '" + fields[3] + "'");
      return false;
    }
    entry.mtime_nsec = static_cast<std::uint32_t>(mtime_nsec);

    std::uint64_t mode = 0;
    if (!ParseUnsigned(fields[4], &mode) || mode > 07777ull) {
      SetError(error_message,
               "Invalid source manifest: bad mode '" + fields[4] + "'");
      return false;
    }
    entry.mode = static_cast<std::uint32_t>(mode);

    std::uint64_t uid = 0;
    std::uint64_t gid = 0;
    if (!ParseUnsigned(fields[5], &uid) || uid > 0xFFFFFFFFull) {
      SetError(error_message,
               "Invalid source manifest: bad uid '" + fields[5] + "'");
      return false;
    }
    if (!ParseUnsigned(fields[6], &gid) || gid > 0xFFFFFFFFull) {
      SetError(error_message,
               "Invalid source manifest: bad gid '" + fields[6] + "'");
      return false;
    }
    entry.uid = static_cast<std::uint32_t>(uid);
    entry.gid = static_cast<std::uint32_t>(gid);

    std::uint64_t dev_major = 0;
    std::uint64_t dev_minor = 0;
    if (!ParseUnsigned(fields[7], &dev_major) || dev_major > 0xFFFFFFFFull) {
      SetError(error_message,
               "Invalid source manifest: bad device major '" + fields[7] + "'");
      return false;
    }
    if (!ParseUnsigned(fields[8], &dev_minor) || dev_minor > 0xFFFFFFFFull) {
      SetError(error_message,
               "Invalid source manifest: bad device minor '" + fields[8] + "'");
      return false;
    }
    entry.dev_major = static_cast<std::uint32_t>(dev_major);
    entry.dev_minor = static_cast<std::uint32_t>(dev_minor);

    std::uint64_t degree = 0;
    if (!ParseUnsigned(fields[9], &degree) || degree > kMaxManifestEntries) {
      SetError(error_message, "Invalid source manifest: bad hardlink degree '" +
                                  fields[9] + "'");
      return false;
    }
    entry.hardlink_degree = static_cast<std::uint32_t>(degree);

    if (!UnescapeField(fields[10], &entry.archive_path) ||
        !IsValidArchivePath(entry.archive_path)) {
      SetError(error_message,
               "Invalid source manifest: bad archive path in an entry");
      return false;
    }
    if (!UnescapeField(fields[11], &entry.link_target)) {
      SetError(error_message,
               "Invalid source manifest: bad link target escape");
      return false;
    }
    if (is_version_3) {
      if (!UnescapeField(fields[12], &entry.content_digest)) {
        SetError(error_message,
                 "Invalid source manifest: bad content digest escape");
        return false;
      }
      // v3 自称带内容身份，就必须真的带：普通文件与软链接缺摘要说明这份
      // manifest 不能当增量基线；反过来，其它类型带摘要说明写入方不懂格式。
      if (entry.type == EntryType::kRegularFile ||
          entry.type == EntryType::kSymlink) {
        if (!IsContentDigest(entry.content_digest)) {
          SetError(error_message,
                   "Invalid source manifest: a version 3 entry is missing its "
                   "content digest");
          return false;
        }
      } else if (!entry.content_digest.empty()) {
        SetError(error_message,
                 "Invalid source manifest: this entry type must not carry a "
                 "content digest");
        return false;
      }
    }
    if (!seen_paths.insert(entry.archive_path).second) {
      SetError(error_message, "Invalid source manifest: duplicate path '" +
                                  entry.archive_path + "'");
      return false;
    }
    entries->push_back(std::move(entry));
  }

  if (position != text.size()) {
    SetError(error_message,
             "Invalid source manifest: unexpected data after the last entry");
    return false;
  }
  return true;
}

}  // namespace backupproject
