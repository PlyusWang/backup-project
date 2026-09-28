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
  // 结构事件之后的重建必须能容忍"目录在这一瞬间被删掉"：那不是错误，只是这
  // 一层没有东西可看。
  struct stat info;
  if (::lstat(path.c_str(), &info) != 0) {
    if (errno == ENOENT) return true;
    SetError(error_message,
             "Cannot inspect directory " + path + ": " + ErrnoText(errno));
    return false;
  }
  if (!S_ISDIR(info.st_mode))
    return true;  // 软链接 / 普通文件：不 follow，跳过

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
    if (errno == ENOENT) return true;
    SetError(error_message,
             "Cannot open directory " + path + ": " + ErrnoText(errno));
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
    if (::lstat(child.c_str(), &child_info) != 0) continue;  // 刚好被删掉
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
  root_wd_ = -1;
  return AddDirectory(fd, root, true, watches, index, error_message);
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
  if (!BuildWatches(fd, root, &watches, &index, error_message)) {
    // 半成品必须清理：不留 fd，也不留任何 watch。
    ::close(fd);
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
