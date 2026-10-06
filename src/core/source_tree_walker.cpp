// source_tree_walker.cpp
//
// 见 include/source_tree_walker.h。
//
// 这份实现是从 src/core/tree_scanner.cpp 里原样搬出来的遍历部分：同样的
// lstat、同样的 opendir/readdir 与 errno 处理、同样的 lexical 排序、同样的
// 路径长度校验、同样的剪枝与 socket 语义。搬动时只做了一件事——把
// "把事实变成 ArchiveEntry" 留给消费者。

// 本文件只负责"遍历 + 判定 + 报错"：不读任何文件正文、不编码 hardlink、
// 不构造 ArchiveEntry、不决定预览窗口与展示方式——那些留给消费者，于是
// Preview 与 Backup 共享同一份事实（决策顺序是合同，见头文件的顺序图）。
//
// 单线程、无全局可变状态；Walker 持有的 filter / names / visitor / faults
// 全是非拥有指针，生命周期只覆盖一次 WalkSourceTree 调用。
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

// 失败文案的唯一格式："<动作>: <磁盘路径>: <strerror>"。预览与真实备份必须
// 逐字一致，调用方（GUI / CLI）直接转述 failure.message、不重新拼装，
// 否则同一个磁盘错误在两条路径上会变成两种说法。
std::string Describe(int error_number, const std::string& action,
                     const std::string& path) {
  return action + ": " + path + ": " + std::strerror(error_number);
}

// 测试注入：返回 0 表示照常调用真实 syscall。
// 只在测试里非空。注入的失败与真实 syscall 失败在下游走完全同一条路径：
// 调用方不能、也不需要区分"这次是注入的"，一旦能区分，测的就不是生产路径。
// 生产代码一律用默认的 nullptr。
int InjectedErrno(const SourceWalkFaults* faults, SourceWalkSyscall call,
                  const std::string& disk_path) {
  if (faults == nullptr || faults->fail_syscall == nullptr) return 0;
  return faults->fail_syscall(call, disk_path, faults->context);
}

// lstat 的 st_mode -> EntryType，连同其余 metadata 一起快照。
// 失败只可能是"这个类型我们表示不了"（kUnsupportedType）。
// mode 只保留 07777：setuid / setgid / sticky 是归档必须保存的事实，文件
// 类型位不进 facts——类型由 EntryType 表达，两处都存会让"mode 说是目录、
// type 说是文件"这种矛盾状态成为可能。
//
// size 只对普通文件填充：软链接的 st_size 是链接目标的长度，不是内容长
// 度。消费侧（tree_scanner 的 ReadLinkTarget）把 0 当作"未知"并回退到上限。
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

  // 设备号只在字符 / 块设备上解析：DeviceMajor / DeviceMinor 是写入侧与恢复
  // 侧共用的同一份拆分逻辑，其它类型保持 0，避免出现"看起来有设备号"的假事实。
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

// 遍历器本体。一次 Walk 对应一个实例：不 new、不持有 fd、不缓存目录内容，
// 失败一律用返回值 + SourceWalkFailure 表达，不抛异常、不跨调用复用状态——
// 每次遍历都重新问一次文件系统，两次结果不同只能是磁盘变了。
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
      return Fail(failure, SourceWalkFailureKind::kInvalidArchivePath,
                  root_path_error, source_directory, ".");
    }
    // source root 永远保留：即使规则把内容全过滤掉，扫描结果仍然是一棵合法
    // （只有一个根目录）的树，恢复出来就是空目录。
    return WalkDirectory(source_directory, ".", failure);
  }

 private:
  // 统一填 failure 并返回 false，于是调用点可以写 "return Fail(...)"：
  // 原因只有一处装配，不会有某条分支忘了填。failure 允许为 nullptr（只关心
  // 返回值的调用方），此时仍然返回 false，绝不静默变成成功。
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

  // 一条 entry 已经确定**会进入归档**之后，才要求它的 archive path 满足完整
  // grammar。
  //
  // 时机很关键：把完整 IsValidArchivePath 提到 Filter 之前，会让
  // "本来会被规则排除、因此根本不会进归档"的条目也提前阻塞整次备份/预览——
  // 那是新的语义，不是历史语义。历史 Backup 是"Filter 先决定它进不进，
  // 进了才校验路径"。
  //
  // 调用的是 Backup 与读侧共用的那一个 IsValidArchivePath：长度、反斜杠、
  // 盘符、绝对路径、结尾 '/'、空 component、"." / ".." component、NUL 全在
  // 这一份 grammar 里。所以 Linux 允许、归档不允许的名字（a\b.txt）在预览与
  // 备份里会得到同一个结论、同一句原文。
  //
  // is_first_entry 恒为 false：child 永远不是第一条；"."（source root）在
  // Walk() 里用 (true, true) 单独校验。is_directory 传 lstat 得到的真实类型，
  // 参数语义与 Backup 完全一致。
  bool CheckIncludedArchivePath(const std::string& disk_path,
                                const std::string& archive_path,
                                const SourceEntryFacts& facts,
                                SourceWalkFailure* failure) {
    std::string path_error;
    if (IsValidArchivePath(archive_path, false,
                           facts.type == EntryType::kDirectory,
                           kMaxArchivePathLength, &path_error)) {
      return true;
    }
    return Fail(failure, SourceWalkFailureKind::kInvalidArchivePath, path_error,
                disk_path, archive_path);
  }

  // 交给消费者的那一次调用。消费者失败时把它的原文原样带出去。
  // visitor_ 已在 WalkSourceTree 挡过 nullptr，这里可以直接解引用。消费者
  // 的原文原样带出、不加任何前缀：那句话会被直接显示给用户。
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

    // 先把名字全部收进内存再递归：目录流必须在递归之前关闭，否则每深一层就
    // 多占一个 DIR*，深目录树会把 fd 耗光。errno 先清零、readdir 返回 nullptr
    // 时再看 errno，是区分"已读完"与"读失败"的唯一可靠写法。
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

      // 历史语义：路径长度是**遍历/构造**阶段的硬边界，在 lstat 与 Filter 之前
      // 就已经判死。一个超长的 child path 即使后来会被规则排除，真实 Backup
      // 历史上也会在这里失败——这里保持既有语义，不做改动。
      if (child_archive.size() > kMaxArchivePathLength) {
        return Fail(failure, SourceWalkFailureKind::kInvalidArchivePath,
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
        // 只有**真的会进归档**的条目才需要满足完整 archive grammar。被剪枝的
        // 目录在上一行已经 continue，走不到这里。
        if (!CheckIncludedArchivePath(child_disk, child_archive, facts,
                                      failure)) {
          return false;
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
        // **优先级刻意保持历史语义**：没有被排除的 socket 先报"这个类型不能
        // 归档"，而不是先报它的文件名违反了 archive grammar。哪怕 socket 的
        // 名字里带反斜杠，真实 Backup 历史上报的也是这一句。
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
      // 到这里这条 entry 已经确定要进归档了，现在才要求它满足完整 grammar。
      if (!CheckIncludedArchivePath(child_disk, child_archive, facts,
                                    failure)) {
        return false;
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

  // 四个非拥有指针：filter 可以为空（等价于没有任何规则），names / visitor /
  // faults 必须由调用方保证在这次 Walk 期间存活。Walker 不可拷贝、不跨线程。
  const Filter* filter_ = nullptr;
  UserDirectoryCache* names_ = nullptr;
  SourceTreeVisitor* visitor_ = nullptr;
  const SourceWalkFaults* faults_ = nullptr;
};

}  // namespace

// 唯一入口。failure 先被整体清零：调用方复用一个结构体反复调用时，不会读到
// 上一次留下的 kind / message。visitor 为空是内部错误（kConsumerFailed），
// 不是"什么都不做就成功"——静默成功会让调用方以为树里本来就没有条目。
//
// UserDirectoryCache 每次调用新建：uid / gid 到名字的映射不跨调用缓存，宁可
// 在同一次遍历里重复解析，也不让两次备份对同一棵树给出不同的名字。
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
