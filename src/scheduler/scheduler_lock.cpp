// scheduler_lock.cpp
//
// 见 scheduler_lock.h。锁机制本身在 file_lock.cpp，这里只做含义与措辞。
// 上层看到的是同一个返回值 false：kBusy（别人正在跑）与 kError（真故障）
// 只能靠 error_message 区分，所以下面两句话必须写得让用户直接读懂。

#include "scheduler_lock.h"

#include <string>

namespace backupproject {
namespace {

void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) *error_message = text;
}

}  // namespace

// RAII 兜底：析构一定释放，所以 scheduler 的 tick 结束时不需要显式 Release，
// 提前 return 的路径也不会漏掉（锁文件本身留在磁盘上，见 file_lock.h）。
SchedulerLock::~SchedulerLock() { Release(); }

// 先 Release 再取：同一个对象可以换一个路径重新取锁，不留上一个 fd。
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
