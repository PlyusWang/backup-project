// application_lock_test.cpp
//
// 锁的专项测试：file_lock / scheduler_lock / application_instance_lock。
//
// 为什么这些用例不能只靠 shell 里的多进程测试：
//   * "锁路径是符号链接 / 目录 / FIFO 时必须 fail closed，而且绝不 truncate
//     目标" 这件事要在**拿到答案的同时**检查受害者文件没被动过；
//   * "锁文件可以被留下、但绝不能靠删文件来释放锁" 需要检查 inode 而不是 exit code；
//   * 抢不到锁的**原因**必须能被区分（kBusy vs kError），否则一个环境问题会被
//     汇报成并发问题。
//
// flock 的锁属于 open file description：同一个进程里两次 open 同一路径同样会
// 互斥，所以这些用例不需要真的开子进程。

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdlib>
#include <string>

#include "application_instance_lock.h"
#include "file_io.h"
#include "file_lock.h"
#include "scheduler_lock.h"
#include "test_support.h"

namespace bp = backupproject;

namespace {

std::string LockDir() { return test_support::FreshDir("app-lock"); }

bool ModeOf(const std::string& path, std::uint32_t* mode) {
  struct stat info;
  if (::lstat(path.c_str(), &info) != 0) return false;
  *mode = static_cast<std::uint32_t>(info.st_mode & 07777);
  return true;
}

bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

// 环境变量还原器：单实例锁的路径来自 XDG_CONFIG_HOME / HOME，测试要自己摆布
// 它们，同时保证退出时不留副作用。
class SavedEnvironment {
 public:
  explicit SavedEnvironment(const char* name) : name_(name) {
    const char* value = std::getenv(name);
    if (value != nullptr) {
      had_value_ = true;
      value_ = value;
    }
  }
  ~SavedEnvironment() {
    if (had_value_) {
      ::setenv(name_.c_str(), value_.c_str(), 1);
    } else {
      ::unsetenv(name_.c_str());
    }
  }

 private:
  std::string name_;
  std::string value_;
  bool had_value_ = false;
};

}  // namespace

int main() {
  // ---- FileLock 的机械语义 ----
  test_support::Section("AL 1. FileLock：抢锁 / 互斥 / 释放");
  {
    const std::string directory = LockDir();
    const std::string path = directory + "/test.lock";

    bp::FileLock first;
    std::string error;
    test_support::Check(first.Acquire(path, &error) == bp::FileLockStatus::kAcquired,
                        "AL-01 空路径之外的新锁文件可以拿到", error);
    test_support::Check(first.held(), "AL-02 拿到之后 held() 为真");

    std::uint32_t mode = 0;
    test_support::Check(ModeOf(path, &mode) && mode == 0600,
                        "AL-03 锁文件权限是 0600（不受 umask 影响）",
                        test_support::Octal(mode));

    // 同一个进程里第二次 open 是另一个 open file description，flock 照样互斥。
    bp::FileLock second;
    const std::string hint = first.ReadOwnerHint();
    test_support::Check(second.Acquire(path, &error) == bp::FileLockStatus::kBusy,
                        "AL-04 第二个持有者拿到的是 kBusy（不是 kError）", error);
    test_support::Check(!second.held(), "AL-05 kBusy 之后不持有任何 fd");
    test_support::Check(
        Contains(hint, "pid=" + std::to_string(static_cast<long>(::getpid()))),
        "AL-06 锁文件里的提示带本进程 pid（仅供人看）", hint);

    first.Release();
    test_support::Check(!first.held(), "AL-07 Release 之后不再持有");
    test_support::Check(second.Acquire(path, &error) ==
                            bp::FileLockStatus::kAcquired,
                        "AL-08 释放之后第二个能拿到", error);
    second.Release();
    test_support::Check(test_support::Exists(path),
                        "AL-09 释放锁**不删除**锁文件（删文件会让双方各持一个 "
                        "inode，锁就失效了）");

    bp::FileLock empty;
    test_support::Check(empty.Acquire("", &error) == bp::FileLockStatus::kError,
                        "AL-10 空锁路径是 kError", error);
  }

  // ---- 不安全的锁路径必须 fail closed ----
  test_support::Section("AL 2. 锁路径不安全时 fail closed，且绝不 truncate 目标");
  {
    const std::string directory = LockDir();
    const std::string victim = directory + "/victim.txt";
    const std::string content = "do not truncate me";
    test_support::WriteFile(victim, content, 0644);

    const std::string link = directory + "/symlink.lock";
    test_support::CreateSymlink(victim, link);
    bp::FileLock lock;
    std::string error;
    test_support::Check(lock.Acquire(link, &error) == bp::FileLockStatus::kError,
                        "AL-11 锁路径是符号链接 -> kError", error);
    test_support::Check(Contains(error, "symbolic link"),
                        "AL-12 报错明确说这是符号链接", error);
    std::string after;
    test_support::ReadFile(victim, &after);
    test_support::Check(after == content,
                        "AL-13 被指向的文件一个字节都没被改（O_NOFOLLOW）", after);
    std::uint32_t victim_mode = 0;
    test_support::Check(ModeOf(victim, &victim_mode) && victim_mode == 0644,
                        "AL-14 受害者权限也没被改", test_support::Octal(victim_mode));

    const std::string subdir = directory + "/dir.lock";
    ::mkdir(subdir.c_str(), 0755);
    test_support::Check(lock.Acquire(subdir, &error) == bp::FileLockStatus::kError,
                        "AL-15 锁路径是目录 -> kError", error);

    const std::string fifo = directory + "/fifo.lock";
    test_support::CreateFifo(fifo, 0600);
    // FIFO 用 O_RDWR 打开不会阻塞，所以这里不会挂住；关键是它必须被拒绝。
    test_support::Check(lock.Acquire(fifo, &error) == bp::FileLockStatus::kError,
                        "AL-16 锁路径是 FIFO -> kError（而不是挂住）", error);
    test_support::Check(Contains(error, "FIFO"),
                        "AL-17 报错说清楚占名字的是 FIFO", error);
  }

  // ---- SchedulerLock 的措辞与语义 ----
  test_support::Section("AL 3. SchedulerLock 仍然只回答 quota runner 的问题");
  {
    const std::string directory = LockDir();
    const std::string path = directory + "/scheduler.lock";
    bp::SchedulerLock first;
    std::string error;
    test_support::Check(first.Acquire(path, &error),
                        "AL-18 SchedulerLock 能抢到锁", error);

    bp::SchedulerLock second;
    test_support::Check(!second.Acquire(path, &error),
                        "AL-19 第二个 SchedulerLock 抢不到");
    test_support::Check(
        Contains(error, "The scheduled backup is already held by another process"),
        "AL-20 SchedulerLock 报的是计划任务被别人持有", error);

    bp::SchedulerLock empty;
    test_support::Check(!empty.Acquire("", &error), "AL-21 空路径被拒绝");
    test_support::Check(Contains(error, "lock file path is empty"),
                        "AL-22 空路径的报错是它自己的措辞", error);
  }

  // ---- 全应用单实例锁 ----
  test_support::Section("AL 4. ApplicationInstanceLock：整个产品只允许一个实例");
  {
    const std::string directory = LockDir();
    const std::string path = directory + "/app.lock";

    bp::ApplicationInstanceLock first;
    std::string error;
    test_support::Check(first.Acquire(path, &error) ==
                            bp::ApplicationInstanceStatus::kAcquired,
                        "AL-23 第一个实例拿到锁", error);
    test_support::Check(first.held(), "AL-24 held() 为真");

    bp::ApplicationInstanceLock second;
    test_support::Check(second.Acquire(path, &error) ==
                            bp::ApplicationInstanceStatus::kAlreadyRunning,
                        "AL-25 第二个实例得到 kAlreadyRunning（不是 kError）", error);
    test_support::Check(Contains(error, "only one GUI or CLI process"),
                        "AL-26 报错把产品规则说白：只允许一个 GUI 或 CLI", error);
    test_support::Check(!second.held(), "AL-27 被拒绝的一方不持有 fd");

    first.Release();
    test_support::Check(second.Acquire(path, &error) ==
                            bp::ApplicationInstanceStatus::kAcquired,
                        "AL-28 释放之后第二个实例能拿到", error);
    second.Release();

    // 产品路径必须由 app_paths.h 解析出来，且与 repository / --config-file /
    // --schedule-file 无关。这里验证它跟着配置根走，而不是跟着别的东西。
    test_support::Section("AL 5. 默认锁路径与配置根同源");
    const std::string xdg = LockDir() + "/xdg";
    test_support::Mkdir(xdg, 0755);
    {
      SavedEnvironment saved_xdg("XDG_CONFIG_HOME");
      SavedEnvironment saved_home("HOME");
      ::setenv("XDG_CONFIG_HOME", xdg.c_str(), 1);
      std::string resolved;
      test_support::Check(
          bp::DefaultApplicationInstanceLockPath(&resolved, &error),
          "AL-29 能解析出默认锁路径", error);
      test_support::Check(
          resolved == xdg + "/backup-project/backup-gui-modern/app.lock",
          "AL-30 默认锁路径 = <AppConfigDirectory>/app.lock", resolved);

      // 解析只算路径，不碰文件系统。
      test_support::Check(!test_support::Exists(resolved),
                          "AL-31 只解析路径：解析本身不创建任何文件");

      // 父目录不存在时 Acquire 要按项目策略建出来（0700），而不是拒绝启动。
      const std::string nested = xdg + "/deep/er/app.lock";
      bp::ApplicationInstanceLock deep;
      test_support::Check(deep.Acquire(nested, &error) ==
                              bp::ApplicationInstanceStatus::kAcquired,
                          "AL-32 配置目录不存在时也能拿锁（会自动建 0700 目录）",
                          error);
      std::uint32_t mode = 0;
      test_support::Check(ModeOf(xdg + "/deep/er", &mode) && mode == 0700,
                          "AL-33 自动建出来的目录是 0700",
                          test_support::Octal(mode));
      deep.Release();

      // 不安全路径必须报 kError，不能伪装成"已有实例"：那会把环境问题
      // 说成并发问题，排障方向直接跑偏。
      const std::string victim = xdg + "/victim2.txt";
      test_support::WriteFile(victim, "keep", 0644);
      const std::string link = xdg + "/link.lock";
      test_support::CreateSymlink(victim, link);
      bp::ApplicationInstanceLock unsafe;
      test_support::Check(unsafe.Acquire(link, &error) ==
                              bp::ApplicationInstanceStatus::kError,
                          "AL-34 锁路径是符号链接 -> kError，不是 kAlreadyRunning",
                          error);
      std::string after;
      test_support::ReadFile(victim, &after);
      test_support::Check(after == "keep",
                          "AL-35 被指向的文件仍然没被动过", after);
    }
    test_support::Check(::getenv("XDG_CONFIG_HOME") == nullptr ||
                            std::string(::getenv("XDG_CONFIG_HOME")) != xdg,
                        "AL-36 测试结束后环境变量已还原");
  }

  // ---- 原子替换写入（ScheduleStore / ConfigManager 共用）----
  test_support::Section("AL 6. WriteFileAtomicallyReplacing：同名目标反复替换");
  {
    const std::string directory = LockDir();
    const std::string path = directory + "/state.json";
    std::string error;
    test_support::Check(
        bp::WriteFileAtomicallyReplacing(path, "first", &error),
        "AL-37 首次写入成功", error);
    std::string content;
    test_support::ReadFile(path, &content);
    test_support::Check(content == "first", "AL-38 内容正确", content);

    test_support::Check(
        bp::WriteFileAtomicallyReplacing(path, "second", &error),
        "AL-39 覆盖写入成功", error);
    test_support::ReadFile(path, &content);
    test_support::Check(content == "second", "AL-40 覆盖之后是新的完整内容",
                        content);

    std::uint32_t mode = 0;
    test_support::Check(ModeOf(path, &mode) && mode == 0600,
                        "AL-41 目标权限是 0600", test_support::Octal(mode));

    // 临时文件必须被清理干净：目录里只剩目标文件本身。
    const std::vector<std::string> entries = test_support::DirEntries(directory);
    bool leftovers = false;
    for (const std::string& name : entries) {
      if (name.find(".tmp-") != std::string::npos) leftovers = true;
    }
    test_support::Check(!leftovers,
                        "AL-42 成功之后没有留下任何临时文件");

    // 目标是一个符号链接时，写入必须替换**链接本身**而不是跟随它写穿过去。
    // rename 天然满足这一点（它作用在目录项上），这里把它钉住。
    const std::string victim = directory + "/victim3.txt";
    test_support::WriteFile(victim, "untouched", 0644);
    const std::string link = directory + "/linked-state.json";
    test_support::CreateSymlink(victim, link);
    test_support::Check(
        bp::WriteFileAtomicallyReplacing(link, "replaced", &error),
        "AL-43 目标名字上是符号链接时也能替换目录项", error);
    std::string victim_content;
    test_support::ReadFile(victim, &victim_content);
    test_support::Check(victim_content == "untouched",
                        "AL-44 链接指向的文件没有被写穿", victim_content);
    std::string link_content;
    test_support::ReadFile(link, &link_content);
    test_support::Check(link_content == "replaced",
                        "AL-45 那个名字现在是一个装着新内容的普通文件",
                        link_content);

    test_support::Check(
        !bp::WriteFileAtomicallyReplacing("", "x", &error),
        "AL-46 空路径明确失败");
    test_support::Check(Contains(error, "path is empty"),
                        "AL-47 空路径的报错说清楚原因", error);
  }

  return test_support::Finish("application_lock_test");
}
