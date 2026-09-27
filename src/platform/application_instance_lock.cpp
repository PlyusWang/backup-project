// application_instance_lock.cpp
//
// 见 application_instance_lock.h。

#include "application_instance_lock.h"

#include <string>

#include "app_paths.h"
#include "file_io.h"

namespace backupproject {
namespace {

void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) *error_message = text;
}

// 锁文件名。固定一个常量，GUI 与 CLI 共用一个字面量。
constexpr const char* kApplicationLockFileName = "app.lock";

}  // namespace

ApplicationInstanceLock::~ApplicationInstanceLock() { Release(); }

ApplicationInstanceStatus ApplicationInstanceLock::Acquire(
    const std::string& lock_file_path, std::string* error_message) {
  if (error_message != nullptr) error_message->clear();

  if (lock_file_path.empty()) {
    SetError(error_message, "Application lock file path is empty");
    return ApplicationInstanceStatus::kError;
  }

  // 正常启动时配置目录可能还不存在（全新机器上第一次跑 GUI 或 CLI）。
  // 这里按项目现有策略创建它（0700），而不是因为"目录还没建"就拒绝启动：
  // 锁失败必须是"已经有实例"，不能是"配置文件目录刚好还没建"。
  if (!EnsurePrivateDirectoryFor(lock_file_path, error_message)) {
    return ApplicationInstanceStatus::kError;
  }

  std::string lock_error;
  switch (lock_.Acquire(lock_file_path, &lock_error)) {
    case FileLockStatus::kAcquired:
      return ApplicationInstanceStatus::kAcquired;
    case FileLockStatus::kBusy:
      // 产品规则说得越直白越好：用户看到的不是"scheduler 被别人占了"，
      // 而是"这个程序只允许开一个"。
      SetError(error_message,
               "Another backup-project instance is already running. This "
               "program allows only one GUI or CLI process at a time "
               "(application lock: " +
                   lock_file_path +
                   "). Close the other instance and try again.");
      return ApplicationInstanceStatus::kAlreadyRunning;
    case FileLockStatus::kError:
      break;
  }
  SetError(error_message, lock_error);
  return ApplicationInstanceStatus::kError;
}

bool DefaultApplicationInstanceLockPath(std::string* path,
                                        std::string* error_message) {
  if (path == nullptr) {
    SetError(error_message, "Application lock path output must not be null");
    return false;
  }
  path->clear();
  // AppConfigFilePath 会拒绝含 '/' 的文件名，也保证 GUI / CLI 同源。
  if (!AppConfigFilePath(kApplicationLockFileName, path, error_message)) {
    return false;
  }
  return true;
}

}  // namespace backupproject
