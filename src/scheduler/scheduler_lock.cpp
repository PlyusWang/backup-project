// scheduler_lock.cpp
//
// 见 scheduler_lock.h。锁机制本身在 file_lock.cpp，这里只做含义与措辞。

#include "scheduler_lock.h"

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

  std::string lock_error;
  switch (lock_.Acquire(lock_file_path, &lock_error)) {
    case FileLockStatus::kAcquired:
      return true;
    case FileLockStatus::kBusy:
      SetError(error_message,
               "The scheduled backup is already held by another process "
               "(lock file: " +
                   lock_file_path + ")");
      return false;
    case FileLockStatus::kError:
      break;
  }
  // 措辞按 scheduler 的语境重写一遍：file_lock 不知道这把锁是给谁用的，
  // 直接把它那句通用的话透出去，用户会读不出这是计划任务的锁。
  SetError(error_message,
           "Failed to acquire the scheduler lock: " + lock_error);
  return false;
}

}  // namespace backupproject
