// source_tree_walker.cpp
//
// 见 include/source_tree_walker.h。
//
// 这份实现是从 src/core/tree_scanner.cpp 里原样搬出来的遍历部分：同样的
// lstat、同样的 opendir/readdir 与 errno 处理、同样的 lexical 排序、同样的
// 路径长度校验、同样的剪枝与 socket 语义。搬动时只做了一件事——把
// "把事实变成 ArchiveEntry" 留给消费者。

#include "source_tree_walker.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "archive_path.h"
#include "file_system.h"
#include "user_directory.h"

namespace backupproject {
namespace {

std::string Describe(int error_number, const std::string& action,
                     const std::string& path) {
  return action + ": " + path + ": " + std::strerror(error_number);
}

// 测试注入：返回 0 表示照常调用真实 syscall。
int InjectedErrno(const SourceWalkFaults* faults, SourceWalkSyscall call,
                  const std::string& disk_path) {
  if (faults == nullptr || faults->fail_syscall == nullptr) return 0;
  return faults->fail_syscall(call, disk_path, faults->context);
}

// lstat 的 st_mode -> EntryType，连同其余 metadata 一起快照。
// 失败只可能是"这个类型我们表示不了"（kUnsupportedType）。
bool FactsOf(const struct stat& info, SourceEntryFacts* facts) {
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

class Walker {
 public:
  Walker(const Filter* filter, UserDirectoryCache* names,
         SourceTreeVisitor* visitor, const SourceWalkFaults* faults)
      : filter_(filter), names_(names), visitor_(visitor), faults_(faults) {}

  bool Walk(const std::string& source_directory, SourceWalkFailure* failure) {
    // source root 先单独把一把关：它是整棵树里级别最高的一条，报错也要报得
    // 最具体（"源目录不可用"而不是"某个目录打不开"）。
    struct stat info;
    const int injected_root =
        InjectedErrno(faults_, SourceWalkSyscall::kLstat, source_directory);
    if (injected_root != 0) {
      return Fail(failure, SourceWalkFailureKind::kSourceRoot,
                  Describe(injected_root, "Failed to inspect source directory",
                           source_directory),
                  source_directory, ".");
    }
    if (::lstat(source_directory.c_str(), &info) != 0) {
      return Fail(failure, SourceWalkFailureKind::kSourceRoot,
                  Describe(errno, "Failed to inspect source directory",
                           source_directory),
                  source_directory, ".");
    }
    if (!S_ISDIR(info.st_mode)) {
      return Fail(failure, SourceWalkFailureKind::kSourceRoot,
                  "Source is not a directory: " + source_directory,
                  source_directory, ".");
    }
    // "." 的合法性单独先把关一次：它是整个归档的第一条，级别最高。
    std::string root_path_error;
    if (!IsValidArchivePath(".", true, true, kMaxArchivePathLength,
                            &root_path_error)) {
      return Fail(failure, SourceWalkFailureKind::kConsumerFailed,
                  root_path_error, source_directory, ".");
    }
    // source root 永远保留：即使规则把内容全过滤掉，扫描结果仍然是一棵合法
    // （只有一个根目录）的树，恢复出来就是空目录。
    return WalkDirectory(source_directory, ".", failure);
  }

 private:
  bool Fail(SourceWalkFailure* failure, SourceWalkFailureKind kind,
            const std::string& message, const std::string& disk_path,
            const std::string& archive_path) {
    if (failure != nullptr) {
      failure->kind = kind;
      failure->message = message;
      failure->disk_path = disk_path;
      failure->archive_path = archive_path;
    }
    return false;
  }

  // 交给消费者的那一次调用。消费者失败时把它的原文原样带出去。
  bool Visit(const std::string& disk_path, const std::string& archive_path,
             const SourceEntryFacts& facts, SourceEntryDecision decision,
             SourceWalkFailure* failure) {
    std::string consumer_error;
    if (visitor_->OnEntry(disk_path, archive_path, facts, decision,
                          &consumer_error)) {
      return true;
    }
    return Fail(failure, SourceWalkFailureKind::kConsumerFailed, consumer_error,
                disk_path, archive_path);
  }

  bool WalkDirectory(const std::string& disk_directory,
                     const std::string& archive_path,
                     SourceWalkFailure* failure) {
    struct stat info;
    const int injected_dir =
        InjectedErrno(faults_, SourceWalkSyscall::kLstat, disk_directory);
    if (injected_dir != 0) {
      return Fail(
          failure, SourceWalkFailureKind::kInspect,
          Describe(injected_dir, "Failed to inspect directory", disk_directory),
          disk_directory, archive_path);
    }
    if (::lstat(disk_directory.c_str(), &info) != 0) {
      return Fail(
          failure, SourceWalkFailureKind::kInspect,
          Describe(errno, "Failed to inspect directory", disk_directory),
          disk_directory, archive_path);
    }
    if (!S_ISDIR(info.st_mode)) {
      return Fail(failure, SourceWalkFailureKind::kInspect,
                  "Not a directory: " + disk_directory, disk_directory,
                  archive_path);
    }
    SourceEntryFacts dir_facts;
    if (!FactsOf(info, &dir_facts)) {
      return Fail(failure, SourceWalkFailureKind::kUnsupportedType,
                  "Unsupported source entry type: " + disk_directory,
                  disk_directory, archive_path);
    }
    FillNames(&dir_facts);
    if (!Visit(disk_directory, archive_path, dir_facts,
               SourceEntryDecision::kIncluded, failure)) {
      return false;
    }

    const int injected_open = InjectedErrno(
        faults_, SourceWalkSyscall::kOpenDirectory, disk_directory);
    DIR* raw_dir = nullptr;
    int open_errno = injected_open;
    if (open_errno == 0) {
      raw_dir = ::opendir(disk_directory.c_str());
      open_errno = (raw_dir == nullptr) ? errno : 0;
    }
    if (open_errno != 0) {
      return Fail(
          failure, SourceWalkFailureKind::kDirectoryRead,
          Describe(open_errno, "Failed to open directory", disk_directory),
          disk_directory, archive_path);
    }

    std::vector<std::string> names;
    int readdir_error = 0;
    while (true) {
      // 注入点插在每次 readdir 之前：模拟"读目录读到一半失败"。
      const int injected_read = InjectedErrno(
          faults_, SourceWalkSyscall::kReadDirectory, disk_directory);
      if (injected_read != 0) {
        readdir_error = injected_read;
        break;
      }
      errno = 0;
      struct dirent* item = ::readdir(raw_dir);
      if (item == nullptr) {
        readdir_error = errno;
        break;
      }
      const std::string name = item->d_name;
      if (name == "." || name == "..") {
        continue;
      }
      names.push_back(name);
    }
    ::closedir(raw_dir);
    if (readdir_error != 0) {
      return Fail(
          failure, SourceWalkFailureKind::kDirectoryRead,
          Describe(readdir_error, "Failed to read directory", disk_directory),
          disk_directory, archive_path);
    }
    // readdir 的顺序由文件系统决定，排序后输出才稳定、才可复现。
    // 这条排序是**合同**：预览的前 N 项必须与备份扫描的前 N 项是同一批。
    std::sort(names.begin(), names.end());

    for (const std::string& name : names) {
      const std::string child_disk = FileSystem::JoinPath(disk_directory, name);
      const std::string child_archive =
          (archive_path == ".") ? name : archive_path + "/" + name;
      if (child_archive.size() > kMaxArchivePathLength) {
        return Fail(failure, SourceWalkFailureKind::kPathTooLong,
                    "Archive path too long: " + child_archive, child_disk,
                    child_archive);
      }

      SourceEntryFacts facts;
      const int injected_child =
          InjectedErrno(faults_, SourceWalkSyscall::kLstat, child_disk);
      if (injected_child == 0) {
        struct stat child_info;
        // lstat：软链接不会被跟随，会原样暴露成 kSymlink。
        if (::lstat(child_disk.c_str(), &child_info) != 0) {
          return Fail(failure, SourceWalkFailureKind::kInspect,
                      Describe(errno, "Failed to inspect path", child_disk),
                      child_disk, child_archive);
        }
        if (!FactsOf(child_info, &facts)) {
          return Fail(failure, SourceWalkFailureKind::kUnsupportedType,
                      "Unsupported source entry type: " + child_disk,
                      child_disk, child_archive);
        }
      } else {
        // 枚举完成之后、stat 之前条目消失（或注入的同类失败），与真实 lstat
        // 失败走同一条路：一律 fail closed，不 continue、不 break、不报成功。
        return Fail(
            failure, SourceWalkFailureKind::kInspect,
            Describe(injected_child, "Failed to inspect path", child_disk),
            child_disk, child_archive);
      }
      FillNames(&facts);

      FilterEntry filter_entry;
      filter_entry.archive_path = child_archive;
      filter_entry.name = name;
      filter_entry.is_directory = facts.type == EntryType::kDirectory;
      filter_entry.type = facts.type;
      filter_entry.size = facts.size;
      filter_entry.mtime_sec = facts.mtime_sec;
      filter_entry.uid = facts.uid;
      filter_entry.gid = facts.gid;
      // 软链接同样填名字：预览与真实扫描必须给出同一份元数据，否则
      // include user:<自己> 会在预览里命中、真实备份却把链接漏掉。
      filter_entry.user_name = facts.user_name;
      filter_entry.group_name = facts.group_name;

      if (facts.type == EntryType::kDirectory) {
        // 命中 exclude 的目录整棵剪掉：不再递归，子树里的 socket 之类
        // 也不再有"会不会进归档"的问题。
        if (filter_ != nullptr && filter_->ShouldPruneDirectory(filter_entry)) {
          if (!Visit(child_disk, child_archive, facts,
                     SourceEntryDecision::kDirectoryPruned, failure)) {
            return false;
          }
          continue;
        }
        if (!WalkDirectory(child_disk, child_archive, failure)) {
          return false;
        }
        continue;
      }

      if (facts.type == EntryType::kSocket) {
        // socket 不作为可恢复备份。只有用户明确写了 exclude 才跳过它：
        // 静默跳过、跟随它、把它当普通文件复制都会让"备份成功"变成假话。
        if (filter_ != nullptr &&
            filter_->ShouldSkipSpecialEntry(filter_entry)) {
          if (!Visit(child_disk, child_archive, facts,
                     SourceEntryDecision::kExcludedByRule, failure)) {
            return false;
          }
          continue;
        }
        return Fail(failure, SourceWalkFailureKind::kSocket,
                    "Unsupported special type: socket: " + child_disk,
                    child_disk, child_archive);
      }

      // 软链接 / FIFO / 字符设备 / 块设备 / 普通文件走同一套 include/exclude
      // 判定：exclude 优先，且存在 include 规则时必须命中至少一条。
      // 新的 type: 规则正是靠这一步对特殊文件生效的。
      if (filter_ != nullptr && !filter_->ShouldIncludeFile(filter_entry)) {
        if (!Visit(child_disk, child_archive, facts,
                   SourceEntryDecision::kExcludedByRule, failure)) {
          return false;
        }
        continue;
      }
      if (!Visit(child_disk, child_archive, facts,
                 SourceEntryDecision::kIncluded, failure)) {
        return false;
      }
    }
    return true;
  }

  // 所有类型都解析属主 / 属组名字，软链接也在内：uid / gid 来自 lstat，
  // 本来就是链接自己的属主，解析名字不涉及 follow，没有"跟过去"的风险。
  // 解析失败留空（Filter 视为不匹配），不报错也不崩。
  void FillNames(SourceEntryFacts* facts) {
    facts->user_name = names_->UserName(facts->uid);
    facts->group_name = names_->GroupName(facts->gid);
  }

  const Filter* filter_ = nullptr;
  UserDirectoryCache* names_ = nullptr;
  SourceTreeVisitor* visitor_ = nullptr;
  const SourceWalkFaults* faults_ = nullptr;
};

}  // namespace

bool WalkSourceTree(const std::string& source_directory, const Filter* filter,
                    SourceTreeVisitor* visitor, SourceWalkFailure* failure,
                    const SourceWalkFaults* faults) {
  if (failure != nullptr) {
    *failure = SourceWalkFailure{};
  }
  if (visitor == nullptr) {
    if (failure != nullptr) {
      failure->kind = SourceWalkFailureKind::kConsumerFailed;
      failure->message = "Internal error: null visitor";
    }
    return false;
  }
  if (source_directory.empty()) {
    if (failure != nullptr) {
      failure->kind = SourceWalkFailureKind::kSourceRoot;
      failure->message = "Source directory is empty.";
      failure->disk_path = source_directory;
      failure->archive_path = ".";
    }
    return false;
  }

  UserDirectoryCache names;
  Walker walker(filter, &names, visitor, faults);
  return walker.Walk(source_directory, failure);
}

}  // namespace backupproject
