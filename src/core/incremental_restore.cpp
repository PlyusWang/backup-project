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

// O_PATH 是 Linux 2.6.39+ 的原语：只把目录项**绑定**成对象，不打开内容。
// 本文件用它来"先拿到对象、再判类型、最后才决定要不要真的打开"——这是让
// "标记名上放了一个 FIFO / 设备文件"不再把恢复拖死（也不会产生设备副作用）
// 的关键。万一某个构建环境没定义，就退化成 0，下面的用法会显式报错而不是
// 悄悄降级成会阻塞的 open。
#ifndef O_PATH
#define O_PATH 0
#endif

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

// ---- 临时树的安全删除与来源证明 -------------------------------------------
//
// 这一节只做"删除"，所以每一处都必须回答同一个问题：**我删的到底是不是我刚
// 打开、刚验证过的那个对象？**
//
// PR #34 第一版的实现用 lstat + chmod + opendir + rmdir 这一套**路径**操作，
// 有两个真实缺陷（本轮修复）：
//   1. lstat 只保证"这一次查的时候不是软链接"。查完之后路径会被重新解析：
//      另一个进程（同 UID 即可）把目录项换成指向别处的软链接，后面的 chmod /
//      opendir / rmdir 就会落到链接目标上。这是 TOCTOU，"全程 lstat 所以不会
//      follow"这个结论并不成立。
//   2. 回收旧残留只看"名字像 <dest>.<pid>.<kind> + 同父目录 + 同 UID + pid
//   已死"。
//      这四条都不能证明目录是**本软件建的**：用户自己在同一目录下建一个恰好
//      同名的普通目录、里面放重要文件，就会被当成残留递归删掉。
//
// 现在两条都收口：
//   * 删除全部改成**基于已打开目录 fd 的相对操作**（openat / fstatat /
//     fdopendir / unlinkat / fchmod）。打开之后不再用路径，所有验证都在 fd 上
//     做（fstat），于是"验证的对象"和"操作的对象"是同一个内核对象。
//   * 跨进程回收增加**来源证明**：本进程创建临时树时会在同一父目录留下所有权
//     标记文件，回收时必须同时满足"名字规则 + 标记内容匹配 + 属主是自己 +
//     pid 已死"。**没有标记的目录一律不碰**——宁可留下磁盘残留，也不删无法
//     证明来源的数据。

// destination 的词法规范化：只去掉**尾部**多余的 '/'，'/' 本身保持 '/'。
//
// 为什么需要：临时路径是按 "<destination>.<pid>.<kind>"
// 直接拼出来的。destination 若带尾斜杠（"/tmp/x/restore/"），临时树就会落进
// destination **内部**，发布那一 步的 rmdir(destination) 必然
// ENOTEMPTY，恢复整体失败——这是实测到的真实缺陷。
//
// 只做这一件事：不解析 "." / ".."、不碰软链接、不合并不同路径。调用方给的
// destination 必须保持"同一个路径"的语义，任何额外解析都可能把两条不同路径
// 错误地当成一条。
std::string NormalizeDestination(const std::string& destination) {
  std::string trimmed = destination;
  while (trimmed.size() > 1 && trimmed.back() == '/') {
    trimmed.pop_back();
  }
  return trimmed;
}

// 把 destination 拆成"父目录 + 基名"。没有斜杠时父目录取 "."。
// 复用同一套词法规范化，保证"回收用的父目录/基名"与"构造临时路径用的
// destination"永远一致。
void SplitDestination(const std::string& destination, std::string* parent,
                      std::string* base) {
  const std::string trimmed = NormalizeDestination(destination);
  const std::size_t slash = trimmed.find_last_of('/');
  if (slash == std::string::npos) {
    *parent = ".";
    *base = trimmed;
    return;
  }
  *parent = (slash == 0) ? "/" : trimmed.substr(0, slash);
  *base = trimmed.substr(slash + 1);
}

// 所有权标记的魔数与文件名。标记文件与临时树同级，永远不会被发布进
// destination。
const char kTempOwnerMagic[] = "BPRESTORE-TMP-OWNER/1";

std::string TempOwnerFileName(const std::string& destination_base,
                              long long owner_pid) {
  return destination_base + "." + std::to_string(owner_pid) + ".owner";
}

std::string TempOwnerPayload(long long owner_pid) {
  return std::string(kTempOwnerMagic) + "\n" + std::to_string(owner_pid) + "\n";
}

// 递归删除 name（相对 parent_fd 解析）。
//
// force_owner_access：目录缺少宿主 rwx 位时先 fchmod 补上再删。只对**本进程
// 自己刚创建的临时树**使用——那棵树按定义全是半成品，永远不会被发布。
//
// 返回 false = "确实有东西没删掉"（不是"不存在"）。
bool RemoveTreeAt(int parent_fd, const std::string& name,
                  bool force_owner_access) {
  // O_NOFOLLOW|O_DIRECTORY：软链接以 ELOOP 失败、普通文件以 ENOTDIR 失败，
  // 两者都走下面的 unlinkat 分支——只摘掉这个目录项本身，绝不 follow。
  const int dir_fd = ::openat(parent_fd, name.c_str(),
                              O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (dir_fd < 0) {
    if (errno == ENOENT) return true;  // 幂等
    if (errno == ELOOP || errno == ENOTDIR) {
      if (::unlinkat(parent_fd, name.c_str(), 0) == 0) return true;
      return errno == ENOENT;
    }
    if (errno == EACCES && force_owner_access) {
      // 目录打不开（例如 0000 权限）：先用 **O_PATH** 把目录项绑定成内核对象，
      // 再对**已绑定的那个对象**补权限。
      //
      // 修复前这里是 fstatat + 按路径 fchmodat：两者之间目录项可以被换成
      // 软链接，而 Linux 的 fchmodat 没有 AT_SYMLINK_NOFOLLOW（glibc 传这个
      // 标志会返回 ENOTSUP，实际等价于 follow）——软件就可能去改一个跟本次
      // 恢复毫无关系的文件的权限位。
      //
      // O_PATH 只要求父目录可搜索、不要求目标自身的任何权限，所以 0000 的
      // 目录也能拿到 fd；/proc/self/fd/N 是内核 magic link，chmod 解析到的
      // 就是那个已经绑定的 inode，于是"检查的对象"与"修改的对象"必然是同一个。
      // 拿不到对象（例如 /proc 没挂载）就**保守失败**：宁可留下磁盘残留，也
      // 绝不按路径去猜一个对象改权限。
      if (O_PATH == 0) return false;
      const int path_fd =
          ::openat(parent_fd, name.c_str(),
                   O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (path_fd < 0) {
        if (errno == ENOENT) return true;  // 幂等
        if (errno == ELOOP || errno == ENOTDIR) {
          if (::unlinkat(parent_fd, name.c_str(), 0) == 0) return true;
          return errno == ENOENT;
        }
        return false;
      }
      struct stat info;
      if (::fstat(path_fd, &info) != 0 || !S_ISDIR(info.st_mode)) {
        ::close(path_fd);
        return false;
      }
      const std::string bound = "/proc/self/fd/" + std::to_string(path_fd);
      const int chmod_rc =
          ::chmod(bound.c_str(), (info.st_mode & 07777) | S_IRWXU);
      ::close(path_fd);
      if (chmod_rc != 0) return false;  // 补不了权限就如实报告"没删掉"
      return RemoveTreeAt(parent_fd, name, force_owner_access);
    }
    return false;
  }

  struct stat info;
  if (::fstat(dir_fd, &info) != 0) {
    ::close(dir_fd);
    return false;
  }
  if (force_owner_access) {
    const mode_t wanted = S_IRUSR | S_IWUSR | S_IXUSR;
    if ((info.st_mode & wanted) != wanted) {
      // fchmod 作用在**已打开的 fd** 上：不可能改到链接目标。
      ::fchmod(dir_fd, (info.st_mode & 07777) | wanted);
    }
  }

  DIR* directory = ::fdopendir(dir_fd);  // 接管 fd 所有权
  if (directory == nullptr) {
    ::close(dir_fd);
    return false;
  }
  bool ok = true;
  while (struct dirent* item = ::readdir(directory)) {
    const std::string child = item->d_name;
    if (child == "." || child == "..") continue;
    if (!RemoveTreeAt(::dirfd(directory), child, force_owner_access))
      ok = false;
  }
  ::closedir(directory);
  // 摘目录项之前再做一次对象身份确认：fd 绑定的对象（fstat，上面的 info）
  // 必须仍然就是目录项**此刻**指向的对象（fstatat + AT_SYMLINK_NOFOLLOW）。
  //
  // Linux 没有"按 fd 摘目录项"的系统调用，所以这里**不可能**把窗口收成零，
  // 如实记录剩余限制：
  //   * 递归删除子项全部基于已经打开的 fd，替换影响不到"已经删掉的内容"；
  //   * AT_REMOVEDIR 对非空目录返回 ENOTEMPTY，所以最坏情况只是摘掉一个
  //     **空**目录项，不会连带删除数据；
  //   * 身份不一致时一律保守失败（ok = false），绝不猜。
  struct stat entry_now;
  const bool same_object = ::fstatat(parent_fd, name.c_str(), &entry_now,
                                     AT_SYMLINK_NOFOLLOW) == 0 &&
                           entry_now.st_dev == info.st_dev &&
                           entry_now.st_ino == info.st_ino;
  if (!same_object || ::unlinkat(parent_fd, name.c_str(), AT_REMOVEDIR) != 0) {
    ok = false;
  }
  return ok;
}

// 按路径打开父目录，然后把 name 交给 RemoveTreeAt。父目录的解析只发生一次，
// name 之后的所有操作都相对这个 fd。
bool RemoveTreeForcingOwnerAccess(const std::string& path) {
  std::string parent;
  std::string name;
  SplitDestination(path, &parent, &name);
  if (name.empty() || name == "." || name == ".." || name == "/") return false;
  const int parent_fd =
      ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (parent_fd < 0) return false;
  const bool ok = RemoveTreeAt(parent_fd, name, /*force_owner_access=*/true);
  ::close(parent_fd);
  return ok;
}

// 读所有权标记并校验：必须是普通文件、属主是自己、不带 group/other 写位、
// 内容与 pid 完全匹配。openat 用 O_NOFOLLOW，指向别处的软链接读不进来。
bool TempOwnerMarkerMatches(int parent_fd, const std::string& name,
                            long long expected_pid) {
  // 第一步只用 O_PATH 绑定对象并判类型：O_PATH 既不读也不写，**不会阻塞**，
  // 也不会触发字符设备的打开副作用。标记名上如果放的是 FIFO，直接用
  // O_RDONLY 打开会一直等一个写者——那是"恢复卡死"而不是"恢复失败"。
  if (O_PATH == 0) return false;
  const int probe_fd =
      ::openat(parent_fd, name.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
  if (probe_fd < 0) return false;
  struct stat probe_info;
  const bool regular = ::fstat(probe_fd, &probe_info) == 0 &&
                       S_ISREG(probe_info.st_mode) &&
                       probe_info.st_uid == ::geteuid() &&
                       (probe_info.st_mode & (S_IWGRP | S_IWOTH)) == 0;
  ::close(probe_fd);
  if (!regular) return false;

  // 类型/属主/权限都确认过是"普通文件"之后，才真的打开来读。
  const int fd =
      ::openat(parent_fd, name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return false;
  struct stat info;
  if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
    ::close(fd);
    return false;
  }
  if (info.st_uid != ::geteuid()) {
    ::close(fd);
    return false;
  }
  if ((info.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
    ::close(fd);
    return false;
  }
  char buffer[128] = {0};
  const ssize_t got = ::read(fd, buffer, sizeof(buffer) - 1);
  ::close(fd);
  if (got <= 0) return false;
  return std::string(buffer, static_cast<std::size_t>(got)) ==
         TempOwnerPayload(expected_pid);
}

// 所有权标记的创建结果。调用方必须区分"这个标记是我建的 / 是上一轮同 pid 的
// 我留下的 / 这个名字被别人占着"，因为三者的安全语义完全不同。
enum class TempMarkerResult {
  kCreated,  // 本次调用排他创建成功
  kReusedOurs,  // 已经存在，且内容与 pid 完全匹配 = 上一轮同 pid 崩溃残留
  kConflict,  // 已经存在，但不是本程序留下的标记 —— 调用方必须 fail-closed
  kFailed,  // 写不了（例如父目录不可写）：不致命，但本轮不做"被回收方"
};

// 写下本进程的所有权标记。
//
// 只做**排他创建**，这是 P0 的核心不变量：执行一次恢复，绝不修改或删除一个
// 本来不属于本次恢复的文件。修复前的实现是
//     openat(..., O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600)
// 它有三个真实后果（都已被 RED 用例实测复现）：
//   * O_TRUNC：同名普通文件被截断；同名**硬链接**会把另一个名字下的真实文件
//     一起毁掉（实测：受害文件内容被替换成标记文本）；
//   * O_WRONLY 打开同名 FIFO 且没有 O_NONBLOCK：恢复不是失败而是**永久阻塞**；
//   * 无论创建成功与否，收尾还会无条件 unlink 这个路径，把同名软链接 /
//     只读文件直接摘掉。
// O_CREAT|O_EXCL 把这三条一次解决：名字已被占用时内核直接返回 EEXIST，
// 既不会打开、不会截断、也不会阻塞；此时只有"内容确实是本 pid 的标记"才被
// 认作自己上一轮的残留，其余一律报 kConflict 让调用方 fail-closed。
TempMarkerResult CreateTempOwnerMarker(int parent_fd, const std::string& name,
                                       long long owner_pid) {
  if (parent_fd < 0) return TempMarkerResult::kFailed;
  const int fd =
      ::openat(parent_fd, name.c_str(),
               O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) {
    if (errno != EEXIST) return TempMarkerResult::kFailed;
    // 名字被占着：只有"就是本程序上一轮同 pid 留下的标记"才算自己的。
    // 这个判定全程走 O_PATH + fstat，不会阻塞、不会 follow。
    return TempOwnerMarkerMatches(parent_fd, name, owner_pid)
               ? TempMarkerResult::kReusedOurs
               : TempMarkerResult::kConflict;
  }
  const std::string payload = TempOwnerPayload(owner_pid);
  std::size_t written = 0;
  bool ok = true;
  while (written < payload.size()) {
    const ssize_t got =
        ::write(fd, payload.data() + written, payload.size() - written);
    if (got < 0) {
      if (errno == EINTR) continue;
      ok = false;
      break;
    }
    written += static_cast<std::size_t>(got);
  }
  ::close(fd);
  if (!ok) {
    // 半截标记只可能是刚由本次调用创建的（O_EXCL 保证了这一点），所以可以
    // 安全地撤销，不给下一次回收留下"内容不匹配的垃圾"。
    ::unlinkat(parent_fd, name.c_str(), 0);
    return TempMarkerResult::kFailed;
  }
  return TempMarkerResult::kCreated;
}

// 回收**已经死掉的旧进程**留下的临时条目。
//
// 进入时的清理只认"当前 pid 的同名残留"，跨 pid 的旧残留永远不会被再看一眼，
// 这是残留无上限累积的第二个原因。这里做一次回收，但**必须先证明来源**：
//   1. 名字必须精确等于 <destination 基名>.<十进制 pid>.<kind>，kind 只能是
//      staging / overlay / container，且与 destination 同父目录；
//   2. **同一 pid 必须还有一个所有权标记** <基名>.<pid>.owner：普通文件、属主
//      是自己、不带 group/other 写位、内容与魔数和 pid 完全匹配。
//      没有标记就一律不碰 —— 用户自建的"名字恰好相似"的目录因此绝对安全，
//      这正是"只靠同 UID 不能替代来源证明"的落点；
//   3. 条目属主必须是自己；
//   4. pid 不能是自己；
//   5. pid 必须已经不存在（kill(pid,0) 以 ESRCH 失败）。活着的 pid 一律跳过
//      —— 那可能是另一个进程正在用的中间数据。
//
// 所有条目操作都相对**已打开的父目录 fd**（openat/fstatat/unlinkat），扫描之后
// 不再重新解析路径；条目本身用 O_NOFOLLOW 打开，软链接不可能被 follow。
//
// 并发前提：客户端用 ApplicationInstanceLock 保证一个 UID 同时只有一个
// backupctl， 所以"同一 destination 上还有另一个活着的恢复"在锁语义下不成立；第
// 5 条再补 一道运行时检查。已知残余窗口：第 5 条与随后删除之间 pid
// 可能被复用，而新进程 恰好正在恢复同一个 destination ——
// 窗口极窄且被单实例锁覆盖，如实记录。
void ReclaimStaleTempDirs(const std::string& destination_directory) {
  std::string parent;
  std::string base;
  SplitDestination(destination_directory, &parent, &base);
  if (base.empty()) return;
  const int parent_fd =
      ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (parent_fd < 0) return;
  // fdopendir 会接管 fd 的所有权，所以给扫描单独 dup 一个，parent_fd 留给
  // 后面的 openat / fstatat / unlinkat 用。
  const int scan_fd = ::dup(parent_fd);
  DIR* directory = (scan_fd >= 0) ? ::fdopendir(scan_fd) : nullptr;
  if (directory == nullptr) {
    if (scan_fd >= 0) ::close(scan_fd);
    ::close(parent_fd);
    return;
  }

  struct Candidate {
    long long owner_pid;
    std::string name;
  };
  std::vector<Candidate> candidates;
  const std::string prefix = base + ".";
  while (struct dirent* item = ::readdir(directory)) {
    const std::string name = item->d_name;
    if (name.size() <= prefix.size() ||
        name.compare(0, prefix.size(), prefix) != 0) {
      continue;
    }
    std::string rest = name.substr(prefix.size());
    bool matched_kind = false;
    for (const char* kind : {".staging", ".overlay", ".container"}) {
      const std::string suffix(kind);
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
    if (::kill(static_cast<pid_t>(owner_pid), 0) == 0 || errno != ESRCH) {
      continue;
    }
    candidates.push_back(Candidate{owner_pid, name});
  }
  ::closedir(directory);  // 关掉的是 scan_fd

  // 每个 pid 的来源证明只查一次，并且**先全部查完再动任何东西**：宁可一个都不
  // 回收，也不要在半信半疑的状态下开删。
  std::map<long long, bool> marker_ok;
  for (const Candidate& candidate : candidates) {
    if (marker_ok.find(candidate.owner_pid) != marker_ok.end()) continue;
    marker_ok[candidate.owner_pid] = TempOwnerMarkerMatches(
        parent_fd, TempOwnerFileName(base, candidate.owner_pid),
        candidate.owner_pid);
  }

  for (const Candidate& candidate : candidates) {
    if (!marker_ok[candidate.owner_pid]) continue;  // 没标记：不碰
    // 删除前再核一次属主，仍然走 fd 相对查询，不重新解析路径。
    struct stat info;
    if (::fstatat(parent_fd, candidate.name.c_str(), &info,
                  AT_SYMLINK_NOFOLLOW) != 0) {
      continue;
    }
    if (info.st_uid != ::geteuid()) continue;
    RemoveTreeAt(parent_fd, candidate.name, /*force_owner_access=*/true);
  }
  // 标记文件最后再删：先删它会让"条目还在、证明没了"，反而更脏。
  for (const auto& entry : marker_ok) {
    if (!entry.second) continue;
    const std::string marker = TempOwnerFileName(base, entry.first);
    ::unlinkat(parent_fd, marker.c_str(), 0);
  }
  ::close(parent_fd);
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
  // 词法规范化：去掉尾部多余的 '/'，且必须在**任何**路径构造与预检之前做。
  // 否则 "<dest>.<pid>.staging" 会落进 dest 内部，发布那一步的 rmdir 必然
  // ENOTEMPTY —— 这就是尾斜杠 destination 恢复失败的真实根因。
  // 只去尾斜杠：不解析 "." / ".."、不碰软链接、不合并不同路径。
  const std::string destination = NormalizeDestination(destination_directory);
  if (destination.empty()) {
    SetError(error_message, "The destination directory must not be empty");
    return false;
  }

  // destination 必须不存在或为空——与既有 restore 的对外承诺一致。
  struct stat info;
  if (::lstat(destination.c_str(), &info) == 0) {
    if (!S_ISDIR(info.st_mode)) {
      SetError(error_message,
               "The destination exists and is not a directory: " + destination);
      return false;
    }
    bool empty = false;
    if (!IsDirectoryEmpty(destination, &empty, error_message)) {
      return false;
    }
    if (!empty) {
      SetError(error_message,
               "The destination directory is not empty: " + destination);
      return false;
    }
  } else if (errno != ENOENT) {
    SetError(error_message, "Cannot stat the destination: " + ErrnoText(errno));
    return false;
  }

  const std::string suffix =
      "." + std::to_string(static_cast<unsigned long>(::getpid()));
  const std::string staging = destination + suffix + ".staging";
  const std::string overlay = destination + suffix + ".overlay";
  const std::string inner_container = destination + suffix + ".container";

  std::string destination_parent;
  std::string destination_base;
  SplitDestination(destination, &destination_parent, &destination_base);
  const long long self_pid = static_cast<long long>(::getpid());
  const std::string temp_owner_name =
      TempOwnerFileName(destination_base, self_pid);

  // 父目录只解析一次：后面所有"这个名字在不在 / 是谁的"都在这个 fd 上做，
  // 不再重新解析路径。
  struct ParentDirGuard {
    int fd;
    ~ParentDirGuard() {
      if (fd >= 0) ::close(fd);
    }
  };
  ParentDirGuard destination_parent_dir{
      ::open(destination_parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
  if (destination_parent_dir.fd < 0) {
    SetError(error_message, "Cannot open the destination parent directory " +
                                destination_parent + ": " + ErrnoText(errno));
    return false;
  }
  const int parent_fd = destination_parent_dir.fd;

  // 进入时这个名字上**已经**有一个属于本 pid 的合法标记吗？
  //   有   => 上一轮同 pid 的进程崩在中途留下的，这批名字归本程序；
  //   没有 => 下面任何"同名条目已存在"都只能说明名字被别人的对象占着。
  const bool marker_pre_existing =
      TempOwnerMarkerMatches(parent_fd, temp_owner_name, self_pid);

  bool marker_owned = false;
  switch (CreateTempOwnerMarker(parent_fd, temp_owner_name, self_pid)) {
    case TempMarkerResult::kCreated:
    case TempMarkerResult::kReusedOurs:
      marker_owned = true;
      break;
    case TempMarkerResult::kConflict:
      // 名字被一个**不是本程序建的**对象占着（普通文件 / 硬链接 / 软链接 /
      // FIFO / 目录）。不打开、不截断、不删除，直接 fail-closed。
      SetError(error_message,
               "Refusing to touch " +
                   JoinPath(destination_parent, temp_owner_name) +
                   ": it already exists and was not created by this program");
      return false;
    case TempMarkerResult::kFailed:
      // 写不了标记（例如父目录不可写）：不致命，只是本轮崩溃后不会被自动
      // 回收。如实记进 report，不改变恢复本身的结果。
      AddNote(report, "Could not write the temporary-owner marker " +
                          JoinPath(destination_parent, temp_owner_name) +
                          "; a crash would leave unreclaimable temporary data");
      break;
  }

  // 1) 先收**别的（已死）进程**留下、且能证明来源的临时条目。
  ReclaimStaleTempDirs(destination);

  // 2) 本 pid 的同名条目。这里就是 P0-B 的落点：**本进程的 pid 不能单独证明
  //    路径归属**——标记也可能是本进程刚刚写的，它证明不了"这个同名目录是
  //    我建的"。
  //      * marker_pre_existing == true：它们确实是本程序上一轮崩溃的残留
  //        （pid 被复用而上次没清干净），按 R-01 的原有语义强制清理；
  //      * 否则：同名条目**不是**本程序建的。修复前这里是
  //        RemoveTreeForcingOwnerAccess(staging/overlay) + unlink(container)，
  //        会把用户自己恰好同名的目录（pid 可复用、名字可预测）递归强删，
  //        0500 的还会先被 chmod 成 0700。现在一律 fail-closed：绝不
  //        "先删除再重新创建"。
  if (marker_pre_existing) {
    RemoveTreeForcingOwnerAccess(staging);
    RemoveTreeForcingOwnerAccess(overlay);
    ::unlink(inner_container.c_str());
  } else {
    const std::string claimed[3] = {destination_base + suffix + ".staging",
                                    destination_base + suffix + ".overlay",
                                    destination_base + suffix + ".container"};
    for (const std::string& name : claimed) {
      struct stat occupied;
      if (::fstatat(parent_fd, name.c_str(), &occupied, AT_SYMLINK_NOFOLLOW) !=
          0) {
        continue;  // ENOENT：这个名字是干净的
      }
      if (marker_owned) {
        ::unlinkat(parent_fd, temp_owner_name.c_str(), 0);
      }
      SetError(error_message,
               "Refusing to restore: the temporary path " +
                   JoinPath(destination_parent, name) +
                   " already exists and was not created by this program");
      return false;
    }
  }

  // 3) 排他认领 staging / overlay：mkdir 的原子性把"检查存在"与"创建"之间的
  //    窗口收掉。下游 RunRestorePipeline 的 MakeDirectories 容忍 EEXIST，
  //    所以这里的认领就是最终归属；认领失败说明有人抢在前面放了一个同名对象，
  //    同样是 fail-closed。
  if (::mkdir(staging.c_str(), 0700) != 0 ||
      ::mkdir(overlay.c_str(), 0700) != 0) {
    const int claim_errno = errno;
    RemoveTreeForcingOwnerAccess(staging);
    RemoveTreeForcingOwnerAccess(overlay);
    if (marker_owned) {
      ::unlinkat(parent_fd, temp_owner_name.c_str(), 0);
    }
    SetError(error_message, "Cannot claim the temporary directories for " +
                                destination + ": " + ErrnoText(claim_errno));
    return false;
  }

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
  // 中间容器是**本程序**在 ExtractDeltaPayload 里创建的。只有确实创建过才在
  // 收尾时删除它——这样"恢复结束时无条件 unlink 一个路径"就彻底不存在了。
  bool container_present = false;
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
      container_present = true;
      RemoveTreeForcingOwnerAccess(overlay);
      // 每个 delta 的 overlay 也走排他认领：删不掉又占着名字时宁可在这里
      // 失败，也不要往一棵来路不明的目录里写。
      if (::mkdir(overlay.c_str(), 0700) != 0) {
        SetError(error_message, "Cannot claim the delta overlay directory " +
                                    overlay + ": " + ErrnoText(errno));
        delta_failed = true;
        break;
      }
      if (!RunRestorePipeline(inner_container, overlay, options, report,
                              error_message)) {
        delta_failed = true;
        break;
      }
      if (::unlink(inner_container.c_str()) == 0 || errno == ENOENT) {
        container_present = false;
      }

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
    if (::lstat(destination.c_str(), &target_info) == 0) {
      // destination 存在时上面已确认它是空目录：先 rmdir 再 rename，替换才是
      // 纯粹的一次 rename（对非空目录 rename 会得到 ENOTEMPTY）。
      if (::rmdir(destination.c_str()) != 0) {
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
    if (!PublishReplacing(staging, destination, error_message)) {
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
  if (container_present) {
    ::unlink(inner_container.c_str());
  }
  // 自己的临时树已经收干净，撤掉所有权标记：它只用于"证明这批条目是同类"，
  // 留着一个没有条目的标记只会让下一次回收做无用功。
  //
  // 只撤**本程序**的标记：kConflict 已经在上面 return 了，kFailed 时这个
  // 名字上根本没有我们的东西——绝不能去 unlink 别人的对象。这正是修复前
  // "无论标记是否创建成功都无条件 unlink 这个路径"的反面。
  if (marker_owned) {
    ::unlinkat(parent_fd, temp_owner_name.c_str(), 0);
  }
  return ok;
}

}  // namespace backupproject
