// realtime_watcher.cpp
//
// 见 include/realtime_watcher.h。

#include "realtime_watcher.h"

#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

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

// 出了竞态时要能说清"它现在是什么"：只报"不是目录"没法排查。
const char* DirectoryTypeText(mode_t mode) {
  if (S_ISLNK(mode)) return "a symbolic link";
  if (S_ISREG(mode)) return "a regular file";
  if (S_ISFIFO(mode)) return "a FIFO";
  if (S_ISSOCK(mode)) return "a socket";
  if (S_ISCHR(mode)) return "a character device";
  if (S_ISBLK(mode)) return "a block device";
  return "not a directory";
}

// 读事件用的缓冲：inotify 一次最多返回这么多字节。内核要求至少 sizeof(struct
// inotify_event) + NAME_MAX + 1，64 KiB 足够一次读完一批常见事件。
constexpr std::size_t kReadBufferBytes = 64u * 1024u;

// 监听的掩码。目录与普通条目共用一个掩码：多收一点事件没有代价（调用方只做
// "有没有可能影响源树"的判断），少收一个才是 bug。
constexpr std::uint32_t kWatchMask =
    IN_CREATE | IN_DELETE | IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB |
    IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT |
    IN_EXCL_UNLINK;

// 目录 watch 的附加标志：只接受目录，且不 follow 软链接。
constexpr std::uint32_t kDirectoryFlags = IN_ONLYDIR | IN_DONT_FOLLOW;

}  // namespace

InotifyWatcher::~InotifyWatcher() { Detach(); }

void InotifyWatcher::ReleaseState() {
  watches_.clear();
  index_.clear();
  root_wd_ = -1;
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  root_.clear();
}

void InotifyWatcher::Detach() { ReleaseState(); }

bool InotifyWatcher::AddDirectory(int fd, const std::string& path, bool is_root,
                                  std::vector<WatchTarget>* watches,
                                  std::unordered_map<int, std::size_t>* index,
                                  std::string* error_message) {
  // child 目录：结构事件之后的重建必须能容忍"目录在这一瞬间被删掉"——那
  // 不是错误，只是这一层没有东西可看。
  //
  // root 相反：Attach 的预检刚刚确认过它是真实目录，这里再看不到它（或者它
  // 已经不是目录了）说明它在这两步之间被删掉 / 移走 / 换成了普通文件或软链接。
  // 这种情况**必须硬失败**：如果按 child 那样 benign skip，BuildWatches 会返回
  // 成功、Attach 会返回成功，而实例其实是 watch_count == 0 的空壳——之后 root
  // 重建也不会有任何 watch 通知它，等于永久漏监听。
  struct stat info;
  if (::lstat(path.c_str(), &info) != 0) {
    const int saved_errno = errno;
    if (is_root) {
      SetError(error_message, "The watch root cannot be watched: " + path +
                                  ": " + ErrnoText(saved_errno) +
                                  (saved_errno == ENOENT
                                       ? " (it disappeared after the initial "
                                         "check)"
                                       : ""));
      return false;
    }
    if (saved_errno == ENOENT) return true;
    SetError(error_message, "Cannot inspect directory " + path + ": " +
                                ErrnoText(saved_errno));
    return false;
  }
  if (!S_ISDIR(info.st_mode)) {
    if (!is_root) return true;  // 软链接 / 普通文件：不 follow，跳过
    SetError(error_message,
             "The watch root is no longer a directory: " + path + " (it is " +
                 DirectoryTypeText(info.st_mode) +
                 " now; the root must be a real directory and must not be a "
                 "symbolic link)");
    return false;
  }

  if (injected_add_watch_errno_ != 0) {
    const int injected = injected_add_watch_errno_;
    injected_add_watch_errno_ = 0;
    SetError(error_message, "inotify_add_watch(" + path +
                                ") failed: " + ErrnoText(injected) +
                                " (injected for testing)");
    return false;
  }

  const int wd =
      ::inotify_add_watch(fd, path.c_str(), kWatchMask | kDirectoryFlags);
  if (wd < 0) {
    SetError(error_message,
             "Cannot watch " + path + ": " + ErrnoText(errno) +
                 (errno == ENOSPC ? " (inotify watch limit reached: check "
                                    "fs.inotify.max_user_watches)"
                                  : ""));
    return false;
  }
  WatchTarget target;
  target.wd = wd;
  target.path = path;
  target.is_root = is_root;
  if (index->find(wd) != index->end()) {
    SetError(error_message, "Duplicate inotify watch descriptor for " + path);
    return false;
  }
  (*index)[wd] = watches->size();
  watches->push_back(std::move(target));
  if (is_root && root_wd_ < 0) root_wd_ = wd;

  DIR* directory = ::opendir(path.c_str());
  if (directory == nullptr) {
    const int saved_errno = errno;
    if (is_root) {
      // root 的 watch 已经加上、目录却打不开了：同样的"看着健康其实没有内容"
      // 状态，一律硬失败。
      SetError(error_message, "Cannot open the watch root " + path + ": " +
                                  ErrnoText(saved_errno));
      return false;
    }
    if (saved_errno == ENOENT) return true;
    SetError(error_message,
             "Cannot open directory " + path + ": " + ErrnoText(saved_errno));
    return false;
  }
  std::vector<std::string> children;
  while (true) {
    errno = 0;
    struct dirent* item = ::readdir(directory);
    if (item == nullptr) {
      if (errno != 0) {
        const std::string text = ErrnoText(errno);
        ::closedir(directory);
        SetError(error_message, "Cannot read directory " + path + ": " + text);
        return false;
      }
      break;
    }
    const std::string name = item->d_name;
    if (name == "." || name == "..") continue;
    children.push_back(name);
  }
  ::closedir(directory);

  if (injected_readdir_errno_ != 0) {
    const int injected = injected_readdir_errno_;
    injected_readdir_errno_ = 0;
    SetError(error_message, "readdir(" + path +
                                ") failed: " + ErrnoText(injected) +
                                " (injected for testing)");
    return false;
  }

  // 排序只是为了让 watch 顺序确定、便于测试与排查；正确性不依赖它。
  std::sort(children.begin(), children.end());
  for (const std::string& name : children) {
    const std::string child = JoinPath(path, name);
    struct stat child_info;
    if (::lstat(child.c_str(), &child_info) != 0) {
      // 只有 ENOENT 才是“这一层在这一瞬间被删了”这种可以忽略的竞态。
      // EACCES / ELOOP / ENAMETOOLONG / EIO 等一律硬失败：把它当成“刚好被删”
      // 会让整棵子树静默地不被 watch，而 BuildWatches 仍然报成功——
      // 正是头文件承诺要避免的“假装健康”。
      const int saved_errno = errno;
      if (saved_errno != ENOENT) {
        SetError(error_message, "Cannot inspect " + child + ": " +
                                    ErrnoText(saved_errno));
        return false;
      }
      continue;
    }
    if (!S_ISDIR(child_info.st_mode)) continue;  // 软链接不 follow
    if (!AddDirectory(fd, child, false, watches, index, error_message)) {
      return false;
    }
  }
  return true;
}

bool InotifyWatcher::BuildWatches(int fd, const std::string& root,
                                  std::vector<WatchTarget>* watches,
                                  std::unordered_map<int, std::size_t>* index,
                                  std::string* error_message) {
  // 测试接缝：正好落在"root 预检已经通过"与"AddDirectory 的第二次 lstat"
  // 之间。产品路径下这两个指针永远是 nullptr。
  if (root_precheck_hook_ != nullptr) {
    root_precheck_hook_(root_precheck_hook_context_);
  }
  root_wd_ = -1;
  return AddDirectory(fd, root, true, watches, index, error_message);
}

void InotifyWatcher::SetRootPrecheckHookForTesting(void (*hook)(void* context),
                                                   void* context) {
  root_precheck_hook_ = hook;
  root_precheck_hook_context_ = context;
}

bool InotifyWatcher::Attach(const std::string& root,
                            std::string* error_message) {
  if (error_message != nullptr) error_message->clear();
  if (root.empty()) {
    SetError(error_message, "A watch root must not be empty");
    return false;
  }
  struct stat info;
  if (::lstat(root.c_str(), &info) != 0) {
    SetError(error_message,
             "Cannot inspect the watch root " + root + ": " + ErrnoText(errno));
    return false;
  }
  if (S_ISLNK(info.st_mode)) {
    SetError(error_message,
             "The watch root must not be a symbolic link: " + root);
    return false;
  }
  if (!S_ISDIR(info.st_mode)) {
    SetError(error_message, "The watch root must be a directory: " + root);
    return false;
  }

  const int fd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (fd < 0) {
    SetError(error_message, "inotify_init1 failed: " + ErrnoText(errno));
    return false;
  }
  std::vector<WatchTarget> watches;
  std::unordered_map<int, std::size_t> index;
  // BuildWatches 会把 root_wd_ 清成 -1；失败时旧状态必须原样保留（包括旧
  // root 的 wd —— 它是"源根丢了没有"的唯一判据）。
  const int previous_root_wd = root_wd_;
  if (!BuildWatches(fd, root, &watches, &index, error_message)) {
    // 半成品必须清理：不留 fd，也不留任何 watch。
    ::close(fd);
    root_wd_ = previous_root_wd;
    return false;
  }
  // 后置不变量：不允许返回一个 root 根本没被 watch 上的"成功"实例。
  // 只靠预检挡不住 precheck 与 AddDirectory 之间的那个窗口。
  if (!RootWatchBuilt(watches)) {
    SetError(error_message,
             "The watch root has no inotify watch after a successful build: " +
                 root +
                 " (the root disappeared or changed type while it was being "
                 "watched)");
    ::close(fd);
    root_wd_ = previous_root_wd;
    return false;
  }
  // 成功之后才替换旧状态（Attach 也可以用来换 root）。
  // 注意：ReleaseState() 会把 root_wd_ 清成 -1，所以先把它记下来再恢复——
  // 根 watch 的 wd 是"源根丢了没有"的唯一判据，丢了它 root_lost 永远不会触发。
  const int built_root_wd = root_wd_;
  ReleaseState();
  fd_ = fd;
  root_ = root;
  watches_ = std::move(watches);
  index_ = std::move(index);
  root_wd_ = built_root_wd;
  return true;
}

bool InotifyWatcher::Rebuild(std::string* error_message) {
  if (error_message != nullptr) error_message->clear();
  if (root_.empty()) {
    SetError(error_message, "Cannot rebuild before a successful attach");
    return false;
  }
  const int fd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (fd < 0) {
    SetError(error_message, "inotify_init1 failed: " + ErrnoText(errno));
    return false;
  }
  std::vector<WatchTarget> watches;
  std::unordered_map<int, std::size_t> index;
  const int previous_root_wd = root_wd_;
  if (!BuildWatches(fd, root_, &watches, &index, error_message)) {
    ::close(fd);
    root_wd_ = previous_root_wd;
    return false;  // 旧 fd 原样保留：不允许出现"拆了旧的、新的没建起来"
  }
  if (!RootWatchBuilt(watches)) {
    SetError(
        error_message,
        "The watch root has no inotify watch after a successful rebuild: " +
            root_ +
            " (the root disappeared or changed type while it was "
            "being watched)");
    ::close(fd);
    root_wd_ = previous_root_wd;
    return false;
  }
  if (fd_ >= 0) ::close(fd_);
  fd_ = fd;
  watches_ = std::move(watches);
  index_ = std::move(index);
  return true;
}

bool InotifyWatcher::Drain(WatchBatch* batch, std::string* error_message) {
  if (batch == nullptr) {
    SetError(error_message, "Watch batch output must not be null");
    return false;
  }
  *batch = WatchBatch{};
  if (error_message != nullptr) error_message->clear();
  if (fd_ < 0) {
    SetError(error_message, "Drain called before a successful attach");
    return false;
  }

  if (injected_overflow_) {
    injected_overflow_ = false;
    batch->overflow = true;
    batch->any_event = true;
    batch->event_count += 1;
  }

  std::vector<unsigned char> buffer(kReadBufferBytes);
  while (true) {
    const ssize_t got = ::read(fd_, buffer.data(), buffer.size());
    if (got < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      SetError(error_message,
               "Cannot read inotify events: " + ErrnoText(errno));
      return false;
    }
    if (got == 0) break;

    std::size_t offset = 0;
    const std::size_t total = static_cast<std::size_t>(got);
    while (offset + sizeof(struct inotify_event) <= total) {
      struct inotify_event event;
      ::memcpy(&event, buffer.data() + offset, sizeof(event));
      if (event.len > total - offset - sizeof(struct inotify_event)) {
        SetError(error_message,
                 "Malformed inotify event (len exceeds the read buffer)");
        return false;
      }
      offset += sizeof(struct inotify_event) + event.len;

      batch->any_event = true;
      batch->event_count += 1;

      const std::uint32_t mask = event.mask;
      if ((mask & IN_Q_OVERFLOW) != 0) batch->overflow = true;
      if ((mask & IN_ISDIR) != 0 &&
          (mask & (IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO)) != 0) {
        batch->structural = true;
      }
      if ((mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT)) != 0) {
        batch->structural = true;
        batch->root_lost = batch->root_lost || event.wd == root_wd_;
      }
      if ((mask & IN_IGNORED) != 0) {
        batch->structural = true;
        if (event.wd == root_wd_) batch->root_lost = true;
      }
      if ((mask & IN_MOVED_FROM) != 0 && event.cookie != 0) {
        batch->last_rename_cookie = event.cookie;
      }
      if ((mask & IN_MOVED_TO) != 0 && event.cookie != 0 &&
          event.cookie == batch->last_rename_cookie) {
        batch->rename_pairs += 1;
      }
    }
    if (offset != total) {
      SetError(error_message, "Truncated inotify event batch");
      return false;
    }
  }
  return true;
}

void InotifyWatcher::InjectOverflowForTesting() { injected_overflow_ = true; }

void InotifyWatcher::InjectAddWatchFailureForTesting(int error_number) {
  injected_add_watch_errno_ = error_number;
}

void InotifyWatcher::InjectReaddirFailureForTesting(int error_number) {
  injected_readdir_errno_ = error_number;
}

}  // namespace backupproject
