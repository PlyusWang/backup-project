// file_lock.cpp
//
// 见 file_lock.h。

#include "file_lock.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <ctime>
#include <string>

namespace backupproject {
namespace {

void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) *error_message = text;
}

std::string Describe(int error_number, const std::string& action,
                     const std::string& path) {
  return action + ": " + path + ": " + std::strerror(error_number);
}

// 把 st_mode 翻译成人话。锁被拒绝时用户需要知道"到底是个什么东西占了
// 这个名字"，只报 EISDIR/ENOTDIR 之类的 errno 说不清楚。
std::string FileTypeText(const struct stat& status) {
  if (S_ISDIR(status.st_mode)) return "a directory";
  if (S_ISLNK(status.st_mode)) return "a symbolic link";
  if (S_ISFIFO(status.st_mode)) return "a FIFO";
  if (S_ISCHR(status.st_mode)) return "a character device";
  if (S_ISBLK(status.st_mode)) return "a block device";
  if (S_ISSOCK(status.st_mode)) return "a socket";
  return "not a regular file";
}

}  // namespace

FileLock::~FileLock() { Release(); }

FileLockStatus FileLock::Acquire(const std::string& lock_file_path,
                                 std::string* error_message) {
  if (error_message != nullptr) error_message->clear();
  Release();

  if (lock_file_path.empty()) {
    SetError(error_message, "Lock file path is empty");
    return FileLockStatus::kError;
  }

  // 0600：锁文件里没有秘密，但也没有理由让别的用户看到这台机器上谁在跑备份。
  // O_NOFOLLOW：这个名字是符号链接时**打开本身**就失败（ELOOP），
  // 而不是跟到链接目标上去加锁——那会把锁加到别的文件上。
  const int fd = ::open(lock_file_path.c_str(),
                        O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) {
    const int saved_errno = errno;
    if (saved_errno == ELOOP) {
      SetError(error_message,
               "Refusing to use the lock file because it is a symbolic link: " +
                   lock_file_path);
      return FileLockStatus::kError;
    }
    SetError(
        error_message,
        Describe(saved_errno, "Failed to open the lock file", lock_file_path));
    return FileLockStatus::kError;
  }

  // fstat 而不是 lstat：O_NOFOLLOW 已经挡住了"打开的那一刻它是符号链接"，
  // 这里要确认的是"打开的确实是普通文件"。目录在 Linux 上根本打不开
  // （EISDIR），但 FIFO 会——对 FIFO flock 的行为不是我们要的东西。
  struct stat status;
  if (::fstat(fd, &status) != 0) {
    const int saved_errno = errno;
    ::close(fd);
    SetError(error_message,
             Describe(saved_errno, "Failed to inspect the lock file",
                      lock_file_path));
    return FileLockStatus::kError;
  }
  if (!S_ISREG(status.st_mode)) {
    ::close(fd);
    SetError(error_message, "Refusing to use the lock file because it is " +
                                FileTypeText(status) + ": " + lock_file_path);
    return FileLockStatus::kError;
  }
  // 属主必须是本人。产品锁可能落在 sticky 的 /tmp 那类目录里：别的用户完全
  // 可以先创建同名文件。那时正确的行为是 fail closed——绝不去 flock 一个
  // 别人控制的 inode（对方可以随时删掉它让锁失效），更不会去覆盖它。
  if (status.st_uid != ::geteuid()) {
    ::close(fd);
    SetError(
        error_message,
        "Refusing to use the lock file because it is owned by uid " +
            std::to_string(static_cast<unsigned long long>(status.st_uid)) +
            " instead of this user (uid " +
            std::to_string(static_cast<unsigned long long>(::geteuid())) +
            "): " + lock_file_path);
    return FileLockStatus::kError;
  }

  // LOCK_NB 是刻意的：等待会把 GUI 的事件循环或 watch 的 tick 卡住，
  // 而"另一个进程正在跑"本来就是需要如实报告的一种正常状态。
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    const int saved_errno = errno;
    ::close(fd);
    if (saved_errno == EWOULDBLOCK || saved_errno == EAGAIN) {
      return FileLockStatus::kBusy;
    }
    SetError(
        error_message,
        Describe(saved_errno, "Failed to lock the lock file", lock_file_path));
    return FileLockStatus::kError;
  }

  fd_ = fd;
  lock_file_path_ = lock_file_path;

  // 抢到锁之后写一份给人看的提示。写失败不影响正确性——真相在 flock 上。
  const std::string hint =
      "pid=" + std::to_string(static_cast<long>(::getpid())) +
      " started_at=" + std::to_string(static_cast<long long>(::time(nullptr))) +
      "\n";
  if (::ftruncate(fd_, 0) == 0) {
    ssize_t ignored = ::write(fd_, hint.data(), hint.size());
    (void)ignored;
  }
  return FileLockStatus::kAcquired;
}

std::string FileLock::ReadOwnerHint() const {
  if (lock_file_path_.empty()) return std::string();
  const int fd = ::open(lock_file_path_.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return std::string();
  std::string hint;
  char buffer[256];
  const ssize_t count = ::read(fd, buffer, sizeof(buffer));
  if (count > 0) hint.assign(buffer, static_cast<std::size_t>(count));
  ::close(fd);
  while (!hint.empty() && (hint.back() == '\n' || hint.back() == '\r')) {
    hint.pop_back();
  }
  return hint;
}

void FileLock::Release() {
  if (fd_ < 0) return;
  // close() 会连带释放 flock。显式 LOCK_UN 一次只是让意图写在代码里，
  // 不依赖读代码的人知道这条 POSIX 细节。
  ::flock(fd_, LOCK_UN);
  ::close(fd_);
  fd_ = -1;
}

}  // namespace backupproject
