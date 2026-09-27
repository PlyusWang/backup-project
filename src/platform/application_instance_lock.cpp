// application_instance_lock.cpp
//
// 见 application_instance_lock.h。

#include "application_instance_lock.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <string>

#include "file_io.h"

namespace backupproject {
namespace {

void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) *error_message = text;
}

// 锁文件名。固定一个常量，两个前端共用一个字面量。
constexpr const char* kApplicationLockFileName = "backup-project.lock";

std::string UidText(uid_t uid) {
  return std::to_string(static_cast<unsigned long long>(uid));
}

// per-user runtime 目录（通常是 /run/user/<uid>）能不能安全地放产品锁。
//
// 每一条都要满足，任何一条不满足就退到 UID 专属的 fallback：
//   * 存在，而且是**真目录**（lstat 先挡住符号链接，stat 再确认目录本身）；
//   * 属主是当前 uid（别人的目录我们不能写，也不该往里面放东西）；
//   * 属主**可写**（S_IWUSR）：在目录里创建锁文件需要写权限；
//   * 属主**可进入 / 可搜索**（S_IXUSR）：在目录里解析"backup-project.lock"
//     这个名字需要 x 位 —— 目录的 x 位就是"能穿过它访问里面的条目"。
//
// 最后一条不是理论问题：mode 0600 的目录是合法存在的，只查 S_IWUSR 会把它判成
// 可用，然后在真正 open 子路径时才 EACCES。那时错误已经不是"runtime 目录不可
// 用"，而是一个看起来像配置损坏的 I/O 失败；正确的行为是在这里就判不可用、
// 干净地退到 fallback。
//
// 判定刻意用 mode/owner 而不是 access(W_OK|X_OK)：
//   * access() 走的是真实 uid/gid（还会被 ACL、只读挂载影响），而这条规则要的是
//     "这个目录节点的 mode 与属主是否安全"，两者不是一回事；
//   * access() 会跟随符号链接，而这里的 lstat 分支已经明确拒绝符号链接。
// 换句话说，这里要的是安全属性，不是"现在能不能写进去"。
bool IsUsableRuntimeDirectory(const std::string& path, uid_t uid) {
  struct stat link_status;
  if (::lstat(path.c_str(), &link_status) != 0) return false;
  if (S_ISLNK(link_status.st_mode)) return false;
  if (!S_ISDIR(link_status.st_mode)) return false;
  if (link_status.st_uid != uid) return false;
  if ((link_status.st_mode & S_IWUSR) == 0) return false;
  if ((link_status.st_mode & S_IXUSR) == 0) return false;

  struct stat real_status;
  if (::stat(path.c_str(), &real_status) != 0) return false;
  if (!S_ISDIR(real_status.st_mode)) return false;
  if (real_status.st_uid != uid) return false;
  return true;
}

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

bool ResolveApplicationLockPath(uid_t uid, const std::string& runtime_root,
                                const std::string& fallback_root,
                                std::string* path, std::string* error_message) {
  if (path == nullptr) {
    SetError(error_message, "Application lock path output must not be null");
    return false;
  }
  path->clear();
  if (runtime_root.empty() || fallback_root.empty()) {
    SetError(error_message, "Application lock roots must not be empty");
    return false;
  }

  const std::string runtime_dir = runtime_root + "/" + UidText(uid);
  if (IsUsableRuntimeDirectory(runtime_dir, uid)) {
    *path = runtime_dir + "/" + kApplicationLockFileName;
    return true;
  }

  // fallback：UID 专属文件名，放在 sticky 的 fallback 根目录下。
  //
  // 这里**不**用 mkstemp 那种随机名：锁必须有一个所有进程都能算出来的固定
  // 位置，否则两个进程各锁各的。安全性由两件事保证：
  //   * FileLock 打开时 O_NOFOLLOW + fstat 普通文件；
  //   * 打开之后属主必须等于 geteuid()——别的用户抢先占住这个名字时直接
  //     fail closed，既不跟随也不覆盖。
  if (!EnsurePrivateDirectory(fallback_root, error_message)) return false;
  *path = fallback_root + "/backup-project-" + UidText(uid) + ".lock";
  return true;
}

bool DefaultApplicationInstanceLockPath(std::string* path,
                                        std::string* error_message) {
  // 只认 uid：不读 HOME、不读 XDG_CONFIG_HOME、不看仓库与任何命令行参数。
  return ResolveApplicationLockPath(::getuid(), "/run/user", "/tmp", path,
                                    error_message);
}

}  // namespace backupproject
