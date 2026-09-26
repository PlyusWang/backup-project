// scheduler_lock.cpp

#include "scheduler_lock.h"

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

}  // namespace

SchedulerLock::~SchedulerLock() { Release(); }

bool SchedulerLock::Acquire(const std::string& lock_file_path,
                            std::string* error_message) {
  if (error_message != nullptr) error_message->clear();
  Release();

  if (lock_file_path.empty()) {
    SetError(error_message,
             "Cannot acquire the scheduler lock: lock file path "
             "is empty");
    return false;
  }

  // 0600：锁文件里没有秘密，但也没有理由让别的用户看到这台机器上谁在跑备份。
  const int fd =
      ::open(lock_file_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (fd < 0) {
    SetError(error_message, "Failed to open scheduler lock file: " +
                                lock_file_path + ": " + std::strerror(errno));
    return false;
  }

  // LOCK_NB 是刻意的：等待会把 GUI 的事件循环或 watch 的 tick 卡住，
  // 而"另一个进程正在跑"本来就是我们需要如实报告的一种正常状态。
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    const int saved_errno = errno;
    ::close(fd);
    if (saved_errno == EWOULDBLOCK || saved_errno == EAGAIN) {
      SetError(error_message,
               "The scheduled backup is already held by another process "
               "(lock file: " +
                   lock_file_path + ")");
      return false;
    }
    SetError(error_message,
             "Failed to lock scheduler lock file: " + lock_file_path + ": " +
                 std::strerror(saved_errno));
    return false;
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
  return true;
}

std::string SchedulerLock::ReadOwnerHint() const {
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

void SchedulerLock::Release() {
  if (fd_ < 0) return;
  // close() 会连带释放 flock。显式 LOCK_UN 一次只是让意图写在代码里，
  // 不依赖读代码的人知道这条 POSIX 细节。
  ::flock(fd_, LOCK_UN);
  ::close(fd_);
  fd_ = -1;
}

}  // namespace backupproject
