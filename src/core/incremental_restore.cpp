// incremental_restore.cpp
//
// 见 include/incremental_restore.h。

// 本文件实现"恢复到任意一个 restore point"：解析 Full → Δ1 → … → ΔN 的依赖
// 链，按顺序应用，最后原子地发布到 destination。
//
// 职责边界（刻意不越界）：
//   * 不解析归档 / 容器的字节格式：base 与每个 delta 的 payload 都交给既有的
//     RunRestorePipeline，本文件只处理"已经落地的两棵目录树"；
//   * 不做认证：加密的 delta 在解析阶段就被拒绝，而不是"先解开再验"；
//   * 不合并进活跃目录：destination 必须不存在或为空。
//
// 失败语义：任何一步失败都返回 false 且 destination 一个字节都不动；中间产物
// 在退出前尽力删除。error_message 只是允许为 nullptr 的诊断出参，绝不能当状态
// 机用（见 RestoreSnapshotChain 的 delta_failed）。本文件是同步单线程代码，
// 调用方保证同一个 destination 不会被并发恢复。
#include "incremental_restore.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utime.h>

#include <algorithm>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "archive_path.h"
#include "backup_catalog.h"
#include "container_format.h"
#include "file_io.h"
#include "incremental_backup.h"

namespace backupproject {

namespace {

// error_message 是可选出参：诊断文本永远不决定控制流，失败与否只看返回值。
void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) *error_message = text;
}

// strerror 对未知 errno 可能返回 nullptr，直接拼字符串会在最需要信息的崩溃
// 现场再崩一次，所以这里兜底成 "errno N"。
std::string ErrnoText(int error_number) {
  const char* text = ::strerror(error_number);
  return text == nullptr ? std::string("errno ") + std::to_string(error_number)
                         : std::string(text);
}

// notes 是"尽力而为但没做到"的清单（chown / chmod / 设备节点…）：给用户核对
// 用，不影响返回码，也不会把一次成功的恢复算成失败。
void AddNote(RestoreReport* report, const std::string& note) {
  if (report != nullptr) report->notes.push_back(note);
}

std::string JoinPath(const std::string& directory, const std::string& name) {
  if (directory.empty()) return name;
  if (directory.back() == '/') return directory + name;
  return directory + "/" + name;
}

// 递归删除（staging / overlay 的清理用）。不存在视为成功。
// 全程 lstat，绝不 follow：指向 / 的软链接只会被 unlink 掉自己，不会被递归
// 进去。ENOENT 视为成功，所以清理是幂等的——重跑不会因为"上一轮已经删干净"
// 而失败。
//
// 失败是尽力而为的：继续删完剩下的条目，最后返回 false 让调用方知道有残留。
bool RemoveTree(const std::string& path) {
  struct stat info;
  if (::lstat(path.c_str(), &info) != 0) {
    return errno == ENOENT;
  }
  if (!S_ISDIR(info.st_mode)) {
    return ::unlink(path.c_str()) == 0;
  }
  DIR* directory = ::opendir(path.c_str());
  if (directory == nullptr) return false;
  bool ok = true;
  while (struct dirent* item = ::readdir(directory)) {
    const std::string name = item->d_name;
    if (name == "." || name == "..") continue;
    if (!RemoveTree(JoinPath(path, name))) ok = false;
  }
  ::closedir(directory);
  if (::rmdir(path.c_str()) != 0) ok = false;
  return ok;
}

// 强制删除**本进程自己的**临时树。与 RemoveTree 的唯一区别是：目录缺少宿主
// (owner) 的读/写/执行位时，先把这三个位补上再继续删。
//
// 为什么需要它：staging 会忠实保留源树的权限（例如源里有一个 0500 的 ro/）。
// 一旦这一轮恢复在合并阶段失败，RemoveTree 就会卡在"ro/ 里的文件删不掉、
// ro/ 本身又因为非空 rmdir 不掉"，于是整棵临时树原样留在 destination 旁边
// ——这就是实测到的"失败恢复留下删不掉的中间目录"的根因。
//
// 为什么这样改是安全的：
//   * 只用于 <destination>.<pid>.{staging,overlay,container}：这三个路径是
//     本进程刚创建的，按定义全是半成品，永远不会被发布；
//   * 全程 lstat：软链接一律 unlink 自身，绝不 follow（不会去动链接目标）；
//   * 只**增加**宿主 rwx 位，不动 group/other，也不减少任何已有位；
//   * 不做任何"按后缀扫目录"的动作——那是 ReclaimStaleTempDirs 的职责，
//     并且它有自己的边界。
bool RemoveTreeForcingOwnerAccess(const std::string& path) {
  struct stat info;
  if (::lstat(path.c_str(), &info) != 0) {
    return errno == ENOENT;
  }
  if (!S_ISDIR(info.st_mode)) {
    return ::unlink(path.c_str()) == 0;
  }
  const mode_t wanted = S_IRUSR | S_IWUSR | S_IXUSR;
  if ((info.st_mode & wanted) != wanted) {
    // 补权限失败也继续尝试删除：父目录可写时 rmdir 本来就够用。
    ::chmod(path.c_str(), (info.st_mode & 07777) | wanted);
  }
  DIR* directory = ::opendir(path.c_str());
  if (directory == nullptr) return false;
  bool ok = true;
  while (struct dirent* item = ::readdir(directory)) {
    const std::string name = item->d_name;
    if (name == "." || name == "..") continue;
    if (!RemoveTreeForcingOwnerAccess(JoinPath(path, name))) ok = false;
  }
  ::closedir(directory);
  if (::rmdir(path.c_str()) != 0) ok = false;
  return ok;
}

// 把 destination 拆成"父目录 + 基名"。没有斜杠时父目录取 "."。
void SplitDestination(const std::string& destination, std::string* parent,
                      std::string* base) {
  const std::size_t slash = destination.find_last_of('/');
  if (slash == std::string::npos) {
    *parent = ".";
    *base = destination;
    return;
  }
  *parent = (slash == 0) ? "/" : destination.substr(0, slash);
  *base = destination.substr(slash + 1);
}

// 回收**已经死掉的旧进程**留在 destination 旁边的临时目录。
//
// 进入时的清理只认"当前 pid 的同名残留"，跨 pid 的旧残留永远不会被再看一眼，
// 这是残留无上限累积的第二个原因。这里做一次边界严格的回收，任何一条不满足
// 就跳过——**绝不按后缀猜**：
//   1. 名字必须精确等于 <destination 基名>.<十进制 pid>.<kind>，kind 只能是
//      staging / overlay / container，而且必须与 destination 同父目录；
//   2. lstat 必须是真正的目录：软链接一律跳过（避免被指到别处去删）；
//   3. 属主必须等于当前 euid：绝不碰别人的文件；
//   4. pid 必须不等于自己；
//   5. pid 必须已经不存在（kill(pid,0) 失败且 errno == ESRCH）。活着的 pid
//      一律跳过——那可能是另一个进程正在用的中间数据。
//
// 并发前提：客户端用 ApplicationInstanceLock 保证一个 UID 同时只有一个
// backupctl 进程，所以"同一个 destination 上还有另一个活着的恢复"在锁语义下
// 不成立；第 5 条再补一道运行时检查。
//
// 已知残余窗口（如实记录，不在本轮消除）：第 5 条与随后的删除之间，内核可能
// 把该 pid 复用给一个新进程，而它恰好正在恢复同一个 destination。窗口极窄，
// 且被单实例锁覆盖。
void ReclaimStaleTempDirs(const std::string& destination_directory) {
  std::string parent;
  std::string base;
  SplitDestination(destination_directory, &parent, &base);
  if (base.empty()) return;
  DIR* directory = ::opendir(parent.c_str());
  if (directory == nullptr) return;
  const std::string prefix = base + ".";
  std::vector<std::string> stale;
  while (struct dirent* item = ::readdir(directory)) {
    const std::string name = item->d_name;
    if (name.size() <= prefix.size() ||
        name.compare(0, prefix.size(), prefix) != 0) {
      continue;
    }
    std::string rest = name.substr(prefix.size());
    bool matched_kind = false;
    for (const char* candidate : {".staging", ".overlay", ".container"}) {
      const std::string suffix(candidate);
      if (rest.size() > suffix.size() &&
          rest.compare(rest.size() - suffix.size(), suffix.size(), suffix) ==
              0) {
        rest = rest.substr(0, rest.size() - suffix.size());
        matched_kind = true;
        break;
      }
    }
    if (!matched_kind || rest.empty()) continue;
    bool digits_only = true;
    for (char c : rest) {
      if (c < '0' || c > '9') {
        digits_only = false;
        break;
      }
    }
    if (!digits_only) continue;
    const long long owner_pid = ::strtoll(rest.c_str(), nullptr, 10);
    if (owner_pid <= 0 || owner_pid == static_cast<long long>(::getpid())) {
      continue;
    }
    const std::string path = JoinPath(parent, name);
    struct stat info;
    if (::lstat(path.c_str(), &info) != 0) continue;
    // 不要求"必须是目录"：非目录（含软链接）由下面的强制删除用 unlink 摘掉
    // 条目本身——unlink 只作用于这个目录项，绝不会 follow 到链接目标。
    if (info.st_uid != ::geteuid()) continue;
    if (::kill(static_cast<pid_t>(owner_pid), 0) == 0 || errno != ESRCH) {
      continue;
    }
    stale.push_back(path);
  }
  ::closedir(directory);
  // 排序只为让回收顺序可复现（顺序不影响结果）。
  std::sort(stale.begin(), stale.end());
  for (const std::string& path : stale) {
    RemoveTreeForcingOwnerAccess(path);
  }
}

// destination 的预检之一。打不开目录是**错误**，不是"当作空"：把非空目录误判
// 成空会让后面的合并 / rename 直接覆盖用户数据，这里必须 fail-closed。
bool IsDirectoryEmpty(const std::string& path, bool* empty,
                      std::string* error_message) {
  DIR* directory = ::opendir(path.c_str());
  if (directory == nullptr) {
    SetError(error_message, "Cannot open " + path + ": " + ErrnoText(errno));
    return false;
  }
  *empty = true;
  while (struct dirent* item = ::readdir(directory)) {
    const std::string name = item->d_name;
    if (name == "." || name == "..") continue;
    *empty = false;
    break;
  }
  ::closedir(directory);
  return true;
}

// 把一段路径按深度排序（深的在前）。tombstone 必须深的先删，否则父目录先没了
// 之后子路径的删除会变成"路径不存在"，看起来像成功，实际留下不一致。
void SortDeepestFirst(std::vector<std::string>* paths) {
  std::stable_sort(paths->begin(), paths->end(),
                   [](const std::string& left, const std::string& right) {
                     const std::size_t left_depth = static_cast<std::size_t>(
                         std::count(left.begin(), left.end(), '/'));
                     const std::size_t right_depth = static_cast<std::size_t>(
                         std::count(right.begin(), right.end(), '/'));
                     if (left_depth != right_depth)
                       return left_depth > right_depth;
                     return left.size() > right.size();
                   });
}

// ---- staging 合并 ----
//
// delta 的 payload 先被恢复成一个独立的 overlay 目录（复用现有 restore，得到
// 正确的路径、正文、软链接、hardlink 拓扑与 metadata），再合并进 staging。
//
// 这一步与"从归档恢复"是不同的操作：这里合并的是两棵**已经落地**的目录树，
// 所以它不需要、也不应该重新实现归档格式里的任何东西。

// 一次 delta 合并的上下文，生命周期 = 一个 delta：每轮循环新建，hardlink 表
// 随之重建。也就是说跨 delta 共享的 inode 不保证被重新链接起来，这是刻意的
// 边界——要保证它就得读完整条链的 inode 表，代价远大于收益。
//
// report 是非拥有指针，允许为 nullptr；其余字段只由本线程访问。
struct MergeContext {
  RestoreReport* report = nullptr;
  // (st_dev, st_ino) -> 已经合并过去的目标路径。用来保持 hardlink 拓扑：
  // overlay 里两个共享 inode 的条目，合并之后必须仍然共享一个 inode。
  std::map<std::pair<std::uint64_t, std::uint64_t>, std::string> inodes;
  std::uint64_t merged_entries = 0;
};

// 逐块复制普通文件正文。用 O_NOFOLLOW 打开源：overlay 是我们自己刚恢复出来
// 的树，这里"理论上"不可能是软链接，但理论上不是安全边界——真被塞进一个链接
// 时，宁可打开失败也不要跟着它去读别的文件。
//
// 读写都处理 EINTR 与短写；close 的错误也要报——延迟写错误只在 close 时浮现，
// 漏掉它等于把一次写失败报成成功。失败时目标可能只写了一半，但调用链会中止
// 整次恢复并丢弃 staging，不会有半截文件被发布出去。
bool CopyFileContents(const std::string& from, const std::string& to,
                      std::string* error_message) {
  const int in_fd = ::open(from.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (in_fd < 0) {
    SetError(error_message, "Cannot open " + from + ": " + ErrnoText(errno));
    return false;
  }
  const int out_fd =
      ::open(to.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (out_fd < 0) {
    const std::string text = ErrnoText(errno);
    ::close(in_fd);
    SetError(error_message, "Cannot create " + to + ": " + text);
    return false;
  }
  std::vector<char> buffer(64u * 1024u);
  bool ok = true;
  for (;;) {
    const ssize_t got = ::read(in_fd, buffer.data(), buffer.size());
    if (got < 0) {
      if (errno == EINTR) continue;
      SetError(error_message, "Cannot read " + from + ": " + ErrnoText(errno));
      ok = false;
      break;
    }
    if (got == 0) break;
    std::size_t remaining = static_cast<std::size_t>(got);
    const char* cursor = buffer.data();
    while (remaining > 0) {
      const ssize_t written = ::write(out_fd, cursor, remaining);
      if (written < 0) {
        if (errno == EINTR) continue;
        SetError(error_message, "Cannot write " + to + ": " + ErrnoText(errno));
        ok = false;
        break;
      }
      cursor += written;
      remaining -= static_cast<std::size_t>(written);
    }
    if (!ok) break;
  }
  if (::close(in_fd) != 0 && ok) {
    SetError(error_message, "Cannot close " + from + ": " + ErrnoText(errno));
    ok = false;
  }
  if (::close(out_fd) != 0 && ok) {
    SetError(error_message, "Cannot close " + to + ": " + ErrnoText(errno));
    ok = false;
  }
  return ok;
}

// 尽力而为地把 metadata 搬过去。ownership 搬不动不是失败：非 root 进程本来
// 就改不了任意属主，既有的 recover 路径也是这么处理的（如实记录，不假装）。
// "尽力而为"是合同的一部分：本函数没有返回值，任何一步失败都不会中断恢复
// ——用户要的是把文件拿回来，不是把 mode 一位不差地拿回来。
//
// 两类失败分开处理：EPERM 是当前权限模型不允许（非 root 改属主），属于预期
// 之内，计进 report->skipped_ownership；其它 errno 是异常，进 notes 供排查。
void ApplyMetadataBestEffort(const std::string& path, const struct stat& info,
                             bool is_symlink, MergeContext* context) {
  if (is_symlink) {
    if (::lchown(path.c_str(), info.st_uid, info.st_gid) != 0 &&
        errno != EPERM) {
      AddNote(context->report,
              "lchown failed for " + path + ": " + ErrnoText(errno));
    }
    // 软链接自己的时间戳也只能用 AT_SYMLINK_NOFOLLOW 贴：跟随会把时间写到
    // 目标上，那是另一个条目的事。
    struct timespec link_times[2];
    link_times[0].tv_sec = info.st_atim.tv_sec;
    link_times[0].tv_nsec = info.st_atim.tv_nsec;
    link_times[1].tv_sec = info.st_mtim.tv_sec;
    link_times[1].tv_nsec = info.st_mtim.tv_nsec;
    if (::utimensat(AT_FDCWD, path.c_str(), link_times, AT_SYMLINK_NOFOLLOW) !=
        0) {
      AddNote(context->report,
              "utimensat failed for " + path + ": " + ErrnoText(errno));
    }
    return;
  }
  if (::chmod(path.c_str(), info.st_mode & 07777) != 0) {
    AddNote(context->report,
            "chmod failed for " + path + ": " + ErrnoText(errno));
  }
  if (::chown(path.c_str(), info.st_uid, info.st_gid) != 0) {
    if (errno == EPERM) {
      if (context->report != nullptr) ++context->report->skipped_ownership;
    } else {
      AddNote(context->report,
              "chown failed for " + path + ": " + ErrnoText(errno));
    }
  }
  struct timespec times[2];
  times[0].tv_sec = info.st_atim.tv_sec;
  times[0].tv_nsec = info.st_atim.tv_nsec;
  times[1].tv_sec = info.st_mtim.tv_sec;
  times[1].tv_nsec = info.st_mtim.tv_nsec;
  if (::utimensat(AT_FDCWD, path.c_str(), times, 0) != 0) {
    AddNote(context->report,
            "utimensat failed for " + path + ": " + ErrnoText(errno));
  }
}

bool EnsureRemoved(const std::string& path, std::string* error_message) {
  struct stat info;
  if (::lstat(path.c_str(), &info) != 0) {
    return errno == ENOENT;
  }
  if (S_ISDIR(info.st_mode)) {
    if (!RemoveTree(path)) {
      SetError(error_message, "Cannot remove directory " + path);
      return false;
    }
    return true;
  }
  if (::unlink(path.c_str()) != 0) {
    SetError(error_message, "Cannot remove " + path + ": " + ErrnoText(errno));
    return false;
  }
  return true;
}

bool MergeNode(const std::string& source_path, const std::string& target_path,
               MergeContext* context, std::string* error_message);

// 把一个目录合并进 staging。mkdir 用 0700 只是**临时**权限：真实 mode 由最后
// 那次 ApplyMetadataBestEffort 贴回去，中间这段时间里内容不该被别的用户读到。
// 子项先合、目录 metadata 最后贴（贴早了会被子项创建改掉 mtime）。递归深度受
// 归档路径长度上限间接约束（每层至少 '/' + 1 字节），不会无界增长。
bool MergeDirectory(const std::string& source_path,
                    const std::string& target_path, const struct stat& info,
                    MergeContext* context, std::string* error_message) {
  struct stat target_info;
  if (::lstat(target_path.c_str(), &target_info) == 0) {
    if (!S_ISDIR(target_info.st_mode)) {
      // 类型变化：文件/软链接 → 目录。必须先删掉旧表示再建目录，
      // 直接覆盖是做不到的。
      if (!EnsureRemoved(target_path, error_message)) return false;
      if (::mkdir(target_path.c_str(), 0700) != 0) {
        SetError(error_message, "Cannot create directory " + target_path +
                                    ": " + ErrnoText(errno));
        return false;
      }
    }
  } else if (errno == ENOENT) {
    if (::mkdir(target_path.c_str(), 0700) != 0) {
      SetError(error_message, "Cannot create directory " + target_path + ": " +
                                  ErrnoText(errno));
      return false;
    }
  } else {
    SetError(error_message,
             "Cannot stat " + target_path + ": " + ErrnoText(errno));
    return false;
  }

  DIR* directory = ::opendir(source_path.c_str());
  if (directory == nullptr) {
    SetError(error_message,
             "Cannot open " + source_path + ": " + ErrnoText(errno));
    return false;
  }
  bool ok = true;
  while (struct dirent* item = ::readdir(directory)) {
    const std::string name = item->d_name;
    if (name == "." || name == "..") continue;
    if (!MergeNode(JoinPath(source_path, name), JoinPath(target_path, name),
                   context, error_message)) {
      ok = false;
      break;
    }
  }
  ::closedir(directory);
  if (!ok) return false;
  // 目录自己的 metadata 最后写：写早了会被子项的创建改掉 mtime。
  ApplyMetadataBestEffort(target_path, info, false, context);
  return true;
}

// 单个条目的合并：按 lstat 的真实类型分派，绝不 follow 软链接。每种类型的失败
// 语义都是明确写出来的，不是笼统的 false：
//   * 目录 / 软链接 / 普通文件 / FIFO 失败 → 返回 false，整次恢复中止；
//   * 设备节点 mknod 失败 → 记一条 note 并返回 true：非 root 本来就造不出设备
//     节点，为此失败等于把"把用户数据拿回来"这件事一起否掉；
//   * 未知类型 → 记 note 跳过，返回 true。
//
// 普通文件先查 hardlink 表：st_nlink > 1 且这对 (st_dev, st_ino) 已经合并过，
// 就直接 link 到第一个已合并的路径，原来共享 inode 的条目之后仍然共享。新建
// 任何目标之前都先 EnsureRemoved，因为它可能是另一种类型。
bool MergeNode(const std::string& source_path, const std::string& target_path,
               MergeContext* context, std::string* error_message) {
  struct stat info;
  if (::lstat(source_path.c_str(), &info) != 0) {
    SetError(error_message,
             "Cannot stat " + source_path + ": " + ErrnoText(errno));
    return false;
  }

  if (S_ISDIR(info.st_mode)) {
    return MergeDirectory(source_path, target_path, info, context,
                          error_message);
  }

  if (S_ISLNK(info.st_mode)) {
    char buffer[4096];
    const ssize_t size =
        ::readlink(source_path.c_str(), buffer, sizeof(buffer));
    if (size < 0) {
      SetError(error_message,
               "Cannot read link " + source_path + ": " + ErrnoText(errno));
      return false;
    }
    if (!EnsureRemoved(target_path, error_message)) return false;
    const std::string target(buffer, static_cast<std::size_t>(size));
    if (::symlink(target.c_str(), target_path.c_str()) != 0) {
      SetError(error_message, "Cannot create symlink " + target_path + ": " +
                                  ErrnoText(errno));
      return false;
    }
    ApplyMetadataBestEffort(target_path, info, true, context);
    ++context->merged_entries;
    return true;
  }

  if (S_ISREG(info.st_mode)) {
    // hardlink 拓扑：同一个 inode 的第二个条目直接 link 到第一个已合并的路径。
    if (info.st_nlink > 1) {
      const std::pair<std::uint64_t, std::uint64_t> key(
          static_cast<std::uint64_t>(info.st_dev),
          static_cast<std::uint64_t>(info.st_ino));
      const auto found = context->inodes.find(key);
      if (found != context->inodes.end()) {
        if (!EnsureRemoved(target_path, error_message)) return false;
        if (::link(found->second.c_str(), target_path.c_str()) != 0) {
          SetError(error_message,
                   "Cannot link " + target_path + ": " + ErrnoText(errno));
          return false;
        }
        ++context->merged_entries;
        return true;
      }
      context->inodes[key] = target_path;
    }
    if (!EnsureRemoved(target_path, error_message)) return false;
    if (!CopyFileContents(source_path, target_path, error_message))
      return false;
    ApplyMetadataBestEffort(target_path, info, false, context);
    ++context->merged_entries;
    return true;
  }

  if (S_ISFIFO(info.st_mode)) {
    if (!EnsureRemoved(target_path, error_message)) return false;
    if (::mkfifo(target_path.c_str(), info.st_mode & 07777) != 0) {
      SetError(error_message,
               "Cannot create fifo " + target_path + ": " + ErrnoText(errno));
      return false;
    }
    ApplyMetadataBestEffort(target_path, info, false, context);
    ++context->merged_entries;
    return true;
  }

  if (S_ISCHR(info.st_mode) || S_ISBLK(info.st_mode)) {
    if (!EnsureRemoved(target_path, error_message)) return false;
    if (::mknod(target_path.c_str(), info.st_mode, info.st_rdev) != 0) {
      // 非 root 造不出设备节点：如实记录并跳过，不让整次恢复失败。
      AddNote(context->report, "Cannot create device node " + target_path +
                                   ": " + ErrnoText(errno));
      return true;
    }
    ApplyMetadataBestEffort(target_path, info, false, context);
    ++context->merged_entries;
    return true;
  }

  AddNote(context->report, "Skipped unsupported node type: " + source_path);
  return true;
}

// 遍历 overlay 的顶层子项，逐个 MergeNode 到 staging。overlay 根自己的
// metadata 不在这里处理——它对应"源根"，由调用方在合并结束后单独贴一次。
bool MergeTree(const std::string& source_directory,
               const std::string& target_directory, MergeContext* context,
               std::string* error_message) {
  DIR* directory = ::opendir(source_directory.c_str());
  if (directory == nullptr) {
    SetError(error_message,
             "Cannot open " + source_directory + ": " + ErrnoText(errno));
    return false;
  }
  bool ok = true;
  while (struct dirent* item = ::readdir(directory)) {
    const std::string name = item->d_name;
    if (name == "." || name == "..") continue;
    if (!MergeNode(JoinPath(source_directory, name),
                   JoinPath(target_directory, name), context, error_message)) {
      ok = false;
      break;
    }
  }
  ::closedir(directory);
  return ok;
}

}  // namespace

// 解析 target 的依赖链：从目标出发沿 parent_file_name 上溯，遇到完整快照
// （kContainer）即停止，返回 base 在前的顺序。
//
// 输入信任级别：target_file_name 来自界面 / CLI，parent_file_name 来自不可信
// 信封，而两者落地的方式只有一条——BackupCatalog::Resolve（单组件名字、仓库
// 直接子项、普通文件、非软链接）。名字合法**不等于**它是这个仓库里的一份快照。
//
// 三条硬性不变量：
//   1) 链上不允许重复文件（环 / 自指）：visited 线性查重，链长有上界所以不必
//      上哈希表；
//   2) 父绑定必须三件事同时成立：文件身份、snapshot_id、manifest_digest；
//   3) 整条链属于同一个 generation：底部的完整快照必须就是 delta 声明的那个。
bool ResolveSnapshotChain(const std::string& repository_directory,
                          const std::string& target_file_name,
                          SnapshotChain* chain, std::string* error_message) {
  if (chain == nullptr) {
    SetError(error_message, "Snapshot chain output must not be null");
    return false;
  }
  *chain = SnapshotChain{};

  BackupCatalog catalog;
  std::vector<std::string> reversed_files;
  std::vector<std::string> reversed_names;
  std::vector<std::string> visited;
  std::string current = target_file_name;
  std::string expected_parent_id;
  std::string expected_parent_manifest_digest;

  // depth 的定义：目标自己是 0，每往上一跳加一。所以一条含 N 个 delta 的链，
  // 链底的完整快照落在 depth = N 上；"depth > kMaxDeltaChainDepth" 意味着
  // "64 个 delta 允许、65 个拒绝"，边界由测试钉死（INC-C BND-01/02）。
  for (std::size_t depth = 0;; ++depth) {
    if (depth > kMaxDeltaChainDepth) {
      SetError(error_message, "The snapshot chain is deeper than " +
                                  std::to_string(kMaxDeltaChainDepth) +
                                  " levels");
      return false;
    }
    if (std::find(visited.begin(), visited.end(), current) != visited.end()) {
      SetError(error_message,
               "Snapshot chain contains a cycle at '" + current + "'");
      return false;
    }
    visited.push_back(current);

    // 每一跳都走 BackupCatalog::Resolve：单组件名字、仓库的直接子项、普通文件、
    // 非软链接。parent_file_name 来自不可信信封，这里是它落地前的唯一入口——
    // 名字合法**不等于**它是这个仓库里的一份快照。
    std::string path;
    if (!catalog.Resolve(repository_directory, current, &path, error_message)) {
      const std::string reason =
          error_message == nullptr ? std::string() : *error_message;
      SetError(error_message,
               expected_parent_id.empty()
                   ? "The snapshot '" + current + "' cannot be used: " + reason
                   : "The parent snapshot '" + current +
                         "' is missing or unusable: " + reason);
      return false;
    }

    SnapshotIdentity identity;
    std::string identity_error;
    if (!LoadVerifiedSnapshotIdentity(repository_directory, current, &identity,
                                      nullptr, &identity_error)) {
      SetError(error_message, identity_error.empty()
                                  ? "Unknown snapshot file: " + current
                                  : identity_error);
      return false;
    }
    if (identity.kind == SnapshotFileKind::kUnknown) {
      SetError(error_message, "Unknown snapshot file: " + current);
      return false;
    }

    // 父绑定必须**三件事同时成立**：名字解析到这份文件、它的归档身份对得上、
    // 它的 manifest 摘要也对得上。少一条都不是合法链。
    if (!expected_parent_id.empty()) {
      if (!identity.sidecars_verified) {
        SetError(error_message,
                 "The parent snapshot '" + current +
                     "' has no verified manifest/identity sidecar: " +
                     identity.sidecar_diagnostic);
        return false;
      }
      if (identity.snapshot_id != expected_parent_id) {
        SetError(error_message, "Snapshot '" + current +
                                    "' does not match the parent identity "
                                    "recorded by its child (the file was "
                                    "replaced or the chain is broken)");
        return false;
      }
      if (identity.manifest_digest != expected_parent_manifest_digest) {
        SetError(error_message,
                 "Snapshot '" + current +
                     "' does not match the parent manifest digest recorded "
                     "by its child (the file was replaced or the chain is "
                     "broken)");
        return false;
      }
    }

    // 上溯的顺序是"目标在前、base 在后"，所以先收集再整体反转；names 与 files
    // 一一对应，必须同步反转，否则错误信息会指向错误的文件名。
    reversed_files.push_back(path);
    reversed_names.push_back(current);

    if (identity.kind == SnapshotFileKind::kContainer) {
      // 链的底必须真的是这份 delta 声明的 generation：否则这条链会被应用在
      // 一个不是它祖先的完整快照上，结果是一个从未存在过的目录树。
      if (!chain->base_generation_id.empty() &&
          chain->base_generation_id != identity.snapshot_id) {
        SetError(error_message,
                 "The full snapshot at the bottom of this chain is not the "
                 "generation the deltas were built against");
        return false;
      }
      chain->base_generation_id = identity.snapshot_id;
      break;
    }

    // 读侧与写侧是同一条加密合同：加密的 delta 一律不接受。它的明文外层信封
    // （parent / tombstones）不受内层 HMAC 覆盖，接受它等于接受一组未经认证的
    // 路径指令。旧版本写出来的这种 delta 因此也不再可恢复。
    ContainerHeader payload_header;
    std::string payload_error;
    if (!InspectDeltaPayloadHeader(path, &payload_header, &payload_error)) {
      SetError(error_message, payload_error);
      return false;
    }
    if (payload_header.encryption_method !=
        static_cast<std::uint8_t>(EncryptionMethod::kNone)) {
      SetError(error_message, UnsupportedIncrementalEncryptionReason());
      return false;
    }

    const DeltaEnvelope& envelope = identity.envelope;
    if (reversed_files.size() == 1) {
      chain->target_manifest_digest = envelope.current_manifest_digest;
      chain->base_generation_id = envelope.base_generation_id;
    } else if (envelope.base_generation_id != chain->base_generation_id) {
      SetError(error_message, "Snapshot chain mixes two generations; '" +
                                  current +
                                  "' belongs to a different full baseline");
      return false;
    }
    if (envelope.parent_file_name.empty()) {
      SetError(error_message, "Delta '" + current + "' has no parent");
      return false;
    }
    expected_parent_id = envelope.parent_snapshot_id;
    expected_parent_manifest_digest = envelope.parent_manifest_digest;
    current = envelope.parent_file_name;
  }

  std::reverse(reversed_files.begin(), reversed_files.end());
  std::reverse(reversed_names.begin(), reversed_names.end());
  chain->files = std::move(reversed_files);
  chain->file_names = std::move(reversed_names);
  chain->delta_count = chain->files.size() - 1;
  return true;
}

// 面向调用方的唯一入口：解析链 → 恢复 base → 逐个应用 delta → 原子发布。
//
// destination 的预检是"不存在，或存在但是空目录"。这不是洁癖：只有一次 rename
// 才能保证"要么旧内容原样、要么新内容完整"，合并进一个非空目录做不到这点。
//
// 全过程只写三个临时路径（staging / overlay / inner_container），名字带 pid
// 后缀且与 destination 同目录：既保证 rename 落在同一文件系统内，又让不同进程
// 不会互相覆盖。
//
// 临时文件生命周期：进入时先回收**已死进程**留下的同 destination 残留
// （ReclaimStaleTempDirs），再清自己的同名残留；退出时不管成功失败都用强制
// 版本删掉自己的三个临时路径（RemoveTreeForcingOwnerAccess）。因此正常与失败
// 两种路径都不再留下本进程的中间目录。
bool RestoreSnapshotChain(const std::string& repository_directory,
                          const std::string& target_file_name,
                          const std::string& destination_directory,
                          const RestoreOptions& options, RestoreReport* report,
                          std::string* error_message) {
  SnapshotChain chain;
  if (!ResolveSnapshotChain(repository_directory, target_file_name, &chain,
                            error_message)) {
    return false;
  }
  if (destination_directory.empty()) {
    SetError(error_message, "The destination directory must not be empty");
    return false;
  }

  // destination 必须不存在或为空——与既有 restore 的对外承诺一致。
  struct stat info;
  if (::lstat(destination_directory.c_str(), &info) == 0) {
    if (!S_ISDIR(info.st_mode)) {
      SetError(error_message,
               "The destination exists and is not a directory: " +
                   destination_directory);
      return false;
    }
    bool empty = false;
    if (!IsDirectoryEmpty(destination_directory, &empty, error_message)) {
      return false;
    }
    if (!empty) {
      SetError(error_message, "The destination directory is not empty: " +
                                  destination_directory);
      return false;
    }
  } else if (errno != ENOENT) {
    SetError(error_message, "Cannot stat the destination: " + ErrnoText(errno));
    return false;
  }

  const std::string suffix =
      "." + std::to_string(static_cast<unsigned long>(::getpid()));
  const std::string staging = destination_directory + suffix + ".staging";
  const std::string overlay = destination_directory + suffix + ".overlay";
  const std::string inner_container =
      destination_directory + suffix + ".container";
  // 1) 先把**别的（已死）进程**留在同一个 destination 旁边的临时目录收掉：
  //    名字/属主/死 pid 三条都要满足，见 ReclaimStaleTempDirs 的边界说明。
  // 2) 再清自己的同名残留：上一次同 pid 的进程崩在中途（pid 后来被复用）时
  //    会遇到，普通 RemoveTree 可能删不掉只读子树。
  ReclaimStaleTempDirs(destination_directory);
  RemoveTreeForcingOwnerAccess(staging);
  RemoveTreeForcingOwnerAccess(overlay);
  ::unlink(inner_container.c_str());

  // do/while(false) 只用来做"带 break 的单出口"：任何一步失败都跳到末尾的统一
  // 清理，最终只有 ok == true 才代表 destination 真的被发布了。
  bool ok = false;
  // “这一轮 delta 失败了”必须由这个布尔量表达，**不能**去读 error_message：
  // error_message 是可选的诊断出参（允许 nullptr），拿它当状态机会在
  // 传 nullptr 时失效。
  //
  // 不变量：delta 循环里**每一个**致命分支都要先置位再 break。只 break 只能
  // 跳出内层 for —— 循环之后 delta_failed 还是 false 的话，代码会继续走到
  // 发布，于是一次失败的恢复被报成成功，目标里是一棵没应用完增量的树。
  bool delta_failed = false;
  do {
    // 1) base：走既有的完整恢复路径（它自己也是 staging + 原子发布的写法）。
    if (!RunRestorePipeline(chain.files.front(), staging, options, report,
                            error_message)) {
      break;
    }
    // 2) 逐个 delta 应用。
    // index 从 1 开始：files[0] 是 base，已经整棵恢复进 staging。每个 delta 都
    // 解到一个全新的 overlay 再合并，绝不在 staging 上直接解包——这样"解到一半
    // 失败"不会污染已经正确的部分，tombstone 看到的也一定是 base 应用完的真实
    // 状态。
    for (std::size_t index = 1; index < chain.files.size(); ++index) {
      const std::string& delta = chain.files[index];
      DeltaEnvelope envelope;
      if (!ReadDeltaEnvelope(delta, &envelope, error_message)) {
        delta_failed = true;
        break;
      }
      if (!ExtractDeltaPayload(delta, inner_container, error_message)) {
        delta_failed = true;
        break;
      }
      RemoveTreeForcingOwnerAccess(overlay);
      if (!RunRestorePipeline(inner_container, overlay, options, report,
                              error_message)) {
        delta_failed = true;
        break;
      }
      ::unlink(inner_container.c_str());

      // 2a) tombstone：深的先删，而且**不允许穿过软链接祖先**。
      std::vector<std::string> tombstones = envelope.tombstones;
      SortDeepestFirst(&tombstones);
      for (const std::string& relative : tombstones) {
        // 语法在解析信封时就验过了（IsValidDeltaTombstone）；这里再验一次是
        // 纵深防御：应用路径不该假设"调用方一定先解析过"。
        if (!IsValidDeltaTombstone(relative, error_message)) {
          delta_failed = true;
          break;
        }
        std::string path;
        bool exists = false;
        if (!ResolveUnderRootNoSymlinkAncestors(staging, relative, &path,
                                                &exists, error_message)) {
          delta_failed = true;
          break;
        }
        // 中间组件不存在 = 这条路径现在不存在，没有东西要删。
        if (!exists) continue;
        // final node 按 lstat 语义处理：软链接删链接本身（不碰目标），
        // FIFO / 设备删节点，目录递归删（RemoveTree 全程 lstat，不 follow）。
        if (!RemoveTree(path)) {
          SetError(error_message, "Cannot apply a tombstone: " + path);
          delta_failed = true;
          break;
        }
      }
      if (delta_failed) break;

      // 2b) 覆盖新增/修改/类型变化。
      // 每个 delta 一个全新的 MergeContext：hardlink 表只在这一次合并内有意义；
      // merged_entries 则累加进 report，让"这次恢复写了多少条目"是个总量。
      MergeContext context;
      context.report = report;
      if (!MergeTree(overlay, staging, &context, error_message)) {
        delta_failed = true;
        break;
      }
      // 2c) 源根自己的 metadata：overlay 的根就是源根，它的 mode/uid/gid/mtime
      //     由 delta 的源根条目负责。合并只处理了子项，根要单独贴一次，
      //     否则"往根里写了东西"之后的根 mtime 就没人负责了。
      struct stat overlay_root;
      if (::lstat(overlay.c_str(), &overlay_root) == 0) {
        ApplyMetadataBestEffort(staging, overlay_root, false, &context);
      }
      if (report != nullptr) {
        report->restored_entries += context.merged_entries;
      }
      RemoveTreeForcingOwnerAccess(overlay);
    }
    if (delta_failed) break;

    // 3) 发布。destination 已存在（空目录）时先删掉，保证 rename 是原子的。
    struct stat target_info;
    if (::lstat(destination_directory.c_str(), &target_info) == 0) {
      // destination 存在时上面已确认它是空目录：先 rmdir 再 rename，替换才是
      // 纯粹的一次 rename（对非空目录 rename 会得到 ENOTEMPTY）。
      if (::rmdir(destination_directory.c_str()) != 0) {
        SetError(error_message,
                 "Cannot replace the destination: " + ErrnoText(errno));
        break;
      }
    }
    // 发布走项目共用的原子替换原语：单次 rename（目标已经在上面处理
    // 成不存在），成功之后同步父目录，与 PublishNoReplace /
    // WriteFileAtomicallyReplacing 保持同一条 durability 策略。
    //
    // rename 一旦成功，destination 就已经真实发布了：这里不做任何
    // “失败就把 destination 删掉”的补救，那只会把一次成功的恢复变成
    // 数据丢失。
    if (!PublishReplacing(staging, destination_directory, error_message)) {
      break;
    }
    ok = true;
  } while (false);

  // 成功与失败都走这里。staging 在发布成功后已被 rename 搬走；overlay 与中间
  // 容器一定还在。
  //
  // 这里必须用**强制**版本：这棵树里可能带着 0500 之类的只读目录（staging 会
  // 忠实保留源树权限），普通 RemoveTree 会卡在"里面删不掉、目录又非空"而把整棵
  // 留下——这正是实测到的残留来源。三个路径都是本进程刚建的临时树，放开宿主
  // 权限再删是安全的（见 RemoveTreeForcingOwnerAccess）。
  RemoveTreeForcingOwnerAccess(staging);
  RemoveTreeForcingOwnerAccess(overlay);
  ::unlink(inner_container.c_str());
  return ok;
}

}  // namespace backupproject
