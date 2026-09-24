// tree_scanner.cpp
//
// 见 tree_scanner.h。这里是唯一一份源目录树遍历。
//
// 与 v0.1 写入器的关系：v0.1 的 WriteDirectoryTree 只认目录和普通文件，
// 遇到软链接/FIFO/设备/socket 一律让整次备份失败。v2 的扫描器把前三类变成
// 一等公民，只保留 socket 的"要么被明确排除、要么整次失败"语义。
// 目录剪枝、排序、include/exclude 的判定顺序与 v0.1 完全一致。

#include "tree_scanner.h"

#include <dirent.h>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "archive_path.h"
#include "file_system.h"

namespace backupproject {

namespace {

void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) {
    *error_message = text;
  }
}

std::string Describe(int error_number, const std::string& action,
                     const std::string& path) {
  return action + ": " + path + ": " + std::strerror(error_number);
}

// ---- uid / gid -> 名字 ---------------------------------------------------
//
// 名字只是给 user:/group: 规则和 GUI 展示用的便利字段：解析失败（NSS 不可用、
// uid 没有对应账号、线程重入失败）时留空，数字 uid/gid 仍然完全可用，
// 既不报错也不崩。同一棵树里几千个文件常常只有几个 uid，所以顺手做个缓存。
class NameResolver {
 public:
  const std::string& UserName(std::uint32_t uid) {
    const auto found = users_.find(uid);
    if (found != users_.end()) {
      return found->second;
    }
    struct passwd entry;
    struct passwd* result = nullptr;
    std::vector<char> buffer(BufferSize());
    const int status = ::getpwuid_r(static_cast<uid_t>(uid), &entry,
                                    buffer.data(), buffer.size(), &result);
    const std::string name =
        (status == 0 && result != nullptr) ? std::string(entry.pw_name) : "";
    return users_.emplace(uid, name).first->second;
  }

  const std::string& GroupName(std::uint32_t gid) {
    const auto found = groups_.find(gid);
    if (found != groups_.end()) {
      return found->second;
    }
    struct group entry;
    struct group* result = nullptr;
    std::vector<char> buffer(BufferSize());
    const int status = ::getgrgid_r(static_cast<gid_t>(gid), &entry,
                                    buffer.data(), buffer.size(), &result);
    const std::string name =
        (status == 0 && result != nullptr) ? std::string(entry.gr_name) : "";
    return groups_.emplace(gid, name).first->second;
  }

 private:
  static std::size_t BufferSize() {
    const long hint = ::sysconf(_SC_GETPW_R_SIZE_MAX);
    // sysconf 返回 -1 表示"没有上限提示"，这时用一个保守的固定值。
    if (hint < 1024) {
      return 4096;
    }
    return static_cast<std::size_t>(hint);
  }

  std::map<std::uint32_t, std::string> users_;
  std::map<std::uint32_t, std::string> groups_;
};

// 同一次扫描里出现过的 inode。hardlink 检测只在"这一棵树内部"成立：
// 跨备份的 inode 复用没有意义，也不该被当成同一次复制。
struct InodeKey {
  std::uint64_t device = 0;
  std::uint64_t inode = 0;

  bool operator<(const InodeKey& other) const {
    if (device != other.device) {
      return device < other.device;
    }
    return inode < other.inode;
  }
};

// 单个条目的元数据快照：一次 lstat 拿全，之后不再重复 stat 同一个路径。
struct EntryFacts {
  EntryType type = EntryType::kRegularFile;
  std::uint32_t mode = 0;
  std::uint32_t uid = 0;
  std::uint32_t gid = 0;
  std::int64_t mtime_sec = 0;
  std::uint32_t mtime_nsec = 0;
  std::uint64_t size = 0;
  std::uint64_t device_id = 0;
  std::uint64_t inode = 0;
  std::uint32_t dev_major = 0;
  std::uint32_t dev_minor = 0;
  std::uint64_t link_count = 0;
};

bool FactsOf(const struct stat& info, EntryFacts* facts) {
  facts->mode = static_cast<std::uint32_t>(info.st_mode) & 07777u;
  facts->uid = static_cast<std::uint32_t>(info.st_uid);
  facts->gid = static_cast<std::uint32_t>(info.st_gid);
  facts->mtime_sec = static_cast<std::int64_t>(info.st_mtim.tv_sec);
  facts->mtime_nsec = static_cast<std::uint32_t>(info.st_mtim.tv_nsec);
  facts->device_id = static_cast<std::uint64_t>(info.st_dev);
  facts->inode = static_cast<std::uint64_t>(info.st_ino);
  facts->link_count = static_cast<std::uint64_t>(info.st_nlink);
  facts->size = 0;
  facts->dev_major = 0;
  facts->dev_minor = 0;

  if (S_ISDIR(info.st_mode)) {
    facts->type = EntryType::kDirectory;
  } else if (S_ISREG(info.st_mode)) {
    facts->type = EntryType::kRegularFile;
    facts->size = static_cast<std::uint64_t>(info.st_size);
  } else if (S_ISLNK(info.st_mode)) {
    facts->type = EntryType::kSymlink;
  } else if (S_ISFIFO(info.st_mode)) {
    facts->type = EntryType::kFifo;
  } else if (S_ISCHR(info.st_mode)) {
    facts->type = EntryType::kCharDevice;
    facts->dev_major = DeviceMajor(static_cast<std::uint64_t>(info.st_rdev));
    facts->dev_minor = DeviceMinor(static_cast<std::uint64_t>(info.st_rdev));
  } else if (S_ISBLK(info.st_mode)) {
    facts->type = EntryType::kBlockDevice;
    facts->dev_major = DeviceMajor(static_cast<std::uint64_t>(info.st_rdev));
    facts->dev_minor = DeviceMinor(static_cast<std::uint64_t>(info.st_rdev));
  } else if (S_ISSOCK(info.st_mode)) {
    facts->type = EntryType::kSocket;
  } else {
    return false;
  }
  return true;
}

// 软链接目标原文。readlink 不 follow，读到的是链接自己存的那串字节。
// 用 lstat 的 size 作为起始缓冲长度：对软链接来说它就是目标的字节数。
bool ReadLinkTarget(const std::string& disk_path, std::uint64_t hint,
                    std::string* target, std::string* error_message) {
  std::size_t size = static_cast<std::size_t>(hint);
  if (size == 0 || size > kMaxArchivePathLength) {
    size = kMaxArchivePathLength;
  }
  for (int attempt = 0; attempt < 4; ++attempt) {
    std::vector<char> buffer(size + 1);
    const ssize_t got = ::readlink(disk_path.c_str(), buffer.data(), size);
    if (got < 0) {
      SetError(error_message,
               Describe(errno, "Failed to read symbolic link", disk_path));
      return false;
    }
    if (static_cast<std::size_t>(got) < size) {
      target->assign(buffer.data(), static_cast<std::size_t>(got));
      return true;
    }
    // 缓冲被填满说明目标可能更长（竞争修改），加倍再来一次。
    size *= 2;
    if (size > 64 * 1024) {
      SetError(error_message, "Symbolic link target too long: " + disk_path);
      return false;
    }
  }
  SetError(error_message, "Symbolic link target too long: " + disk_path);
  return false;
}

class Scanner {
 public:
  Scanner(const Filter* filter, NameResolver* names,
          std::map<InodeKey, std::string>* seen_inodes)
      : filter_(filter), names_(names), seen_inodes_(seen_inodes) {}

  bool Scan(const std::string& source_directory,
            std::vector<ArchiveEntry>* entries, std::string* error_message) {
    entries_ = entries;
    // source root 永远保留：即使规则把内容全过滤掉，扫描结果仍然是一棵
    // 合法（只有一个根目录）的树，恢复出来就是空目录。
    return ScanDirectory(source_directory, ".", error_message);
  }

 private:
  // 把一次 lstat + Filter 决策的结果落成 ArchiveEntry。
  // is_root 只影响"根目录不进 Filter 判定"这一条。
  bool AppendEntry(const std::string& disk_path,
                   const std::string& archive_path, const struct stat& info,
                   bool is_root, std::string* error_message) {
    EntryFacts facts;
    if (!FactsOf(info, &facts)) {
      SetError(error_message, "Unsupported source entry type: " + disk_path);
      return false;
    }

    ArchiveEntry entry;
    entry.archive_path = archive_path;
    entry.source_path = disk_path;
    entry.type = facts.type;
    entry.mode = facts.mode;
    entry.uid = facts.uid;
    entry.gid = facts.gid;
    entry.mtime_sec = facts.mtime_sec;
    entry.mtime_nsec = facts.mtime_nsec;
    entry.size = facts.size;
    entry.dev_major = facts.dev_major;
    entry.dev_minor = facts.dev_minor;

    // 写侧也走读侧那一套路径规则：保证"自己能产出"蕴含"读侧能接受"。
    // is_directory 只在 path == "." 时起作用，其余路径不看它。
    if (!IsValidArchivePath(archive_path, is_root,
                            facts.type == EntryType::kDirectory,
                            kMaxArchivePathLength, error_message)) {
      return false;
    }

    switch (facts.type) {
      case EntryType::kDirectory:
        break;
      case EntryType::kRegularFile: {
        // hardlink：同一 (st_dev, st_ino) 第二次出现时只写一条"指向第一次
        // archive_path"的 hardlink 条目，绝不重复存一份 payload。
        if (facts.link_count > 1) {
          const InodeKey key{facts.device_id, facts.inode};
          const auto found = seen_inodes_->find(key);
          if (found != seen_inodes_->end() && found->second != archive_path) {
            entry.type = EntryType::kHardLink;
            entry.link_target = found->second;
            entry.size = 0;
            entry.source_path.clear();
            break;
          }
          seen_inodes_->emplace(key, archive_path);
        }
        break;
      }
      case EntryType::kSymlink: {
        std::string target;
        if (!ReadLinkTarget(disk_path, facts.size, &target, error_message)) {
          return false;
        }
        if (target.empty()) {
          SetError(error_message, "Empty symbolic link target: " + disk_path);
          return false;
        }
        entry.link_target = target;
        break;
      }
      case EntryType::kFifo:
      case EntryType::kCharDevice:
      case EntryType::kBlockDevice:
        break;
      case EntryType::kHardLink:
      case EntryType::kSocket:
        // FactsOf 不会产出这两种；放在这里是为了让 switch 完整。
        SetError(error_message, "Unsupported source entry type: " + disk_path);
        return false;
    }

    if (entry.type != EntryType::kSymlink) {
      entry.user_name = names_->UserName(facts.uid);
      entry.group_name = names_->GroupName(facts.gid);
    }
    entries_->push_back(std::move(entry));
    return true;
  }

  bool ScanDirectory(const std::string& disk_directory,
                     const std::string& archive_path,
                     std::string* error_message) {
    struct stat info;
    if (::lstat(disk_directory.c_str(), &info) != 0) {
      SetError(error_message,
               Describe(errno, "Failed to inspect directory", disk_directory));
      return false;
    }
    if (!S_ISDIR(info.st_mode)) {
      SetError(error_message, "Not a directory: " + disk_directory);
      return false;
    }
    if (!AppendEntry(disk_directory, archive_path, info, archive_path == ".",
                     error_message)) {
      return false;
    }

    DIR* raw_dir = ::opendir(disk_directory.c_str());
    if (raw_dir == nullptr) {
      SetError(error_message,
               Describe(errno, "Failed to open directory", disk_directory));
      return false;
    }
    std::vector<std::string> names;
    errno = 0;
    while (struct dirent* item = ::readdir(raw_dir)) {
      const std::string name = item->d_name;
      if (name == "." || name == "..") {
        continue;
      }
      names.push_back(name);
      errno = 0;
    }
    const int readdir_error = errno;
    ::closedir(raw_dir);
    if (readdir_error != 0) {
      SetError(
          error_message,
          Describe(readdir_error, "Failed to read directory", disk_directory));
      return false;
    }
    // readdir 的顺序由文件系统决定，排序后输出才稳定、才可复现。
    std::sort(names.begin(), names.end());

    for (const std::string& name : names) {
      const std::string child_disk = FileSystem::JoinPath(disk_directory, name);
      const std::string child_archive =
          (archive_path == ".") ? name : archive_path + "/" + name;
      if (child_archive.size() > kMaxArchivePathLength) {
        SetError(error_message, "Archive path too long: " + child_archive);
        return false;
      }

      struct stat child_info;
      // lstat：软链接不会被跟随，会原样暴露成 kSymlink。
      if (::lstat(child_disk.c_str(), &child_info) != 0) {
        SetError(error_message,
                 Describe(errno, "Failed to inspect path", child_disk));
        return false;
      }
      EntryFacts facts;
      if (!FactsOf(child_info, &facts)) {
        SetError(error_message, "Unsupported source entry type: " + child_disk);
        return false;
      }

      FilterEntry filter_entry;
      filter_entry.archive_path = child_archive;
      filter_entry.name = name;
      filter_entry.is_directory = facts.type == EntryType::kDirectory;
      filter_entry.type = facts.type;
      filter_entry.size = facts.size;
      filter_entry.mtime_sec = facts.mtime_sec;
      filter_entry.uid = facts.uid;
      filter_entry.gid = facts.gid;
      if (facts.type != EntryType::kSymlink) {
        filter_entry.user_name = names_->UserName(facts.uid);
        filter_entry.group_name = names_->GroupName(facts.gid);
      }

      if (facts.type == EntryType::kDirectory) {
        // 命中 exclude 的目录整棵剪掉：不再递归，子树里的 socket 之类
        // 也不再有"会不会进归档"的问题。
        if (filter_ != nullptr && filter_->ShouldPruneDirectory(filter_entry)) {
          continue;
        }
        if (!ScanDirectory(child_disk, child_archive, error_message)) {
          return false;
        }
        continue;
      }

      if (facts.type == EntryType::kSocket) {
        // socket 不作为可恢复备份。只有用户明确写了 exclude 才跳过它：
        // 静默跳过、跟随它、把它当普通文件复制都会让"备份成功"变成假话。
        if (filter_ != nullptr &&
            filter_->ShouldSkipSpecialEntry(filter_entry)) {
          continue;
        }
        SetError(error_message,
                 "Unsupported special type: socket: " + child_disk);
        return false;
      }

      // 软链接 / FIFO / 字符设备 / 块设备 / 普通文件走同一套 include/exclude
      // 判定：exclude 优先，且存在 include 规则时必须命中至少一条。
      // 新的 type: 规则正是靠这一步对特殊文件生效的。
      if (filter_ != nullptr && !filter_->ShouldIncludeFile(filter_entry)) {
        continue;
      }
      if (!AppendEntry(child_disk, child_archive, child_info, false,
                       error_message)) {
        return false;
      }
    }
    return true;
  }

  const Filter* filter_ = nullptr;
  NameResolver* names_ = nullptr;
  std::map<InodeKey, std::string>* seen_inodes_ = nullptr;
  std::vector<ArchiveEntry>* entries_ = nullptr;
};

}  // namespace

bool ScanSourceTree(const std::string& source_directory, const Filter* filter,
                    std::vector<ArchiveEntry>* entries,
                    std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  if (entries == nullptr) {
    SetError(error_message, "Internal error: null entry list");
    return false;
  }
  if (source_directory.empty()) {
    SetError(error_message, "Source directory is empty.");
    return false;
  }

  struct stat info;
  if (::lstat(source_directory.c_str(), &info) != 0) {
    SetError(error_message,
             Describe(errno, "Failed to inspect source directory",
                      source_directory));
    return false;
  }
  if (!S_ISDIR(info.st_mode)) {
    SetError(error_message, "Source is not a directory: " + source_directory);
    return false;
  }

  // "." 的合法性单独先把关一次：它是整个归档的第一条，级别最高。
  if (!IsValidArchivePath(".", true, true, kMaxArchivePathLength,
                          error_message)) {
    return false;
  }

  std::vector<ArchiveEntry> scanned;
  NameResolver names;
  std::map<InodeKey, std::string> seen_inodes;
  Scanner scanner(filter, &names, &seen_inodes);
  if (!scanner.Scan(source_directory, &scanned, error_message)) {
    return false;
  }
  *entries = std::move(scanned);
  return true;
}

}  // namespace backupproject
