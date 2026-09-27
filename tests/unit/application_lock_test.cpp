// application_lock_test.cpp
//
// 锁的专项测试：file_lock / scheduler_lock / application_instance_lock。
//
// 为什么这些用例不能只靠 shell 里的多进程测试：
//   * "锁路径是符号链接 / 目录 / FIFO 时必须 fail closed，而且绝不 truncate
//     目标" 这件事要在**拿到答案的同时**检查受害者文件没被动过；
//   * "锁文件可以被留下、但绝不能靠删文件来释放锁" 需要检查 inode 而不是 exit
//   code；
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
    test_support::Check(
        first.Acquire(path, &error) == bp::FileLockStatus::kAcquired,
        "AL-01 空路径之外的新锁文件可以拿到", error);
    test_support::Check(first.held(), "AL-02 拿到之后 held() 为真");

    std::uint32_t mode = 0;
    test_support::Check(ModeOf(path, &mode) && mode == 0600,
                        "AL-03 锁文件权限是 0600（不受 umask 影响）",
                        test_support::Octal(mode));

    // 同一个进程里第二次 open 是另一个 open file description，flock 照样互斥。
    bp::FileLock second;
    const std::string hint = first.ReadOwnerHint();
    test_support::Check(
        second.Acquire(path, &error) == bp::FileLockStatus::kBusy,
        "AL-04 第二个持有者拿到的是 kBusy（不是 kError）", error);
    test_support::Check(!second.held(), "AL-05 kBusy 之后不持有任何 fd");
    test_support::Check(
        Contains(hint, "pid=" + std::to_string(static_cast<long>(::getpid()))),
        "AL-06 锁文件里的提示带本进程 pid（仅供人看）", hint);

    first.Release();
    test_support::Check(!first.held(), "AL-07 Release 之后不再持有");
    test_support::Check(
        second.Acquire(path, &error) == bp::FileLockStatus::kAcquired,
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
  test_support::Section(
      "AL 2. 锁路径不安全时 fail closed，且绝不 truncate 目标");
  {
    const std::string directory = LockDir();
    const std::string victim = directory + "/victim.txt";
    const std::string content = "do not truncate me";
    test_support::WriteFile(victim, content, 0644);

    const std::string link = directory + "/symlink.lock";
    test_support::CreateSymlink(victim, link);
    bp::FileLock lock;
    std::string error;
    test_support::Check(
        lock.Acquire(link, &error) == bp::FileLockStatus::kError,
        "AL-11 锁路径是符号链接 -> kError", error);
    test_support::Check(Contains(error, "symbolic link"),
                        "AL-12 报错明确说这是符号链接", error);
    std::string after;
    test_support::ReadFile(victim, &after);
    test_support::Check(after == content,
                        "AL-13 被指向的文件一个字节都没被改（O_NOFOLLOW）",
                        after);
    std::uint32_t victim_mode = 0;
    test_support::Check(ModeOf(victim, &victim_mode) && victim_mode == 0644,
                        "AL-14 受害者权限也没被改",
                        test_support::Octal(victim_mode));

    const std::string subdir = directory + "/dir.lock";
    ::mkdir(subdir.c_str(), 0755);
    test_support::Check(
        lock.Acquire(subdir, &error) == bp::FileLockStatus::kError,
        "AL-15 锁路径是目录 -> kError", error);

    const std::string fifo = directory + "/fifo.lock";
    test_support::CreateFifo(fifo, 0600);
    // FIFO 用 O_RDWR 打开不会阻塞，所以这里不会挂住；关键是它必须被拒绝。
    test_support::Check(
        lock.Acquire(fifo, &error) == bp::FileLockStatus::kError,
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
        Contains(error,
                 "The scheduled backup is already held by another process"),
        "AL-20 SchedulerLock 报的是计划任务被别人持有", error);

    bp::SchedulerLock empty;
    test_support::Check(!empty.Acquire("", &error), "AL-21 空路径被拒绝");
    test_support::Check(Contains(error, "lock file path is empty"),
                        "AL-22 空路径的报错是它自己的措辞", error);
  }

  // ---- 全应用单实例锁 ----
  test_support::Section(
      "AL 4. ApplicationInstanceLock：整个产品只允许一个实例");
  {
    const std::string directory = LockDir();
    const std::string path = directory + "/app.lock";

    bp::ApplicationInstanceLock first;
    std::string error;
    test_support::Check(
        first.Acquire(path, &error) == bp::ApplicationInstanceStatus::kAcquired,
        "AL-23 第一个实例拿到锁", error);
    test_support::Check(first.held(), "AL-24 held() 为真");

    bp::ApplicationInstanceLock second;
    test_support::Check(second.Acquire(path, &error) ==
                            bp::ApplicationInstanceStatus::kAlreadyRunning,
                        "AL-25 第二个实例得到 kAlreadyRunning（不是 kError）",
                        error);
    test_support::Check(Contains(error, "only one GUI or CLI process"),
                        "AL-26 报错把产品规则说白：只允许一个 GUI 或 CLI",
                        error);
    test_support::Check(!second.held(), "AL-27 被拒绝的一方不持有 fd");

    first.Release();
    test_support::Check(second.Acquire(path, &error) ==
                            bp::ApplicationInstanceStatus::kAcquired,
                        "AL-28 释放之后第二个实例能拿到", error);
    second.Release();

    test_support::Section("AL 5. 全局锁路径只依赖 UID");
    // 产品锁不能依赖 HOME / XDG_CONFIG_HOME：依赖它，同一个用户换一个环境变量
    // 就能开第二个实例。这里把解析函数摊开成 (uid, runtime_root, fallback_root)
    // 三个显式参数，因此每一种分支都能被覆盖，不需要伪造 /run/user
    // 或第二个用户。
    const std::string root = LockDir();
    const std::string runtime_root = root + "/run-user";
    const std::string fallback_root = root + "/tmp";
    test_support::Mkdir(runtime_root, 0755);
    test_support::Mkdir(fallback_root, 0777);
    const uid_t uid = ::getuid();
    const std::string uid_text =
        std::to_string(static_cast<unsigned long long>(uid));
    const std::string runtime_dir = runtime_root + "/" + uid_text;
    const std::string expected_fallback =
        fallback_root + "/backup-project-" + uid_text + ".lock";

    std::string resolved;
    // 1) runtime 目录不存在 -> fallback
    test_support::Check(
        bp::ResolveApplicationLockPath(uid, runtime_root, fallback_root,
                                       &resolved, &error) &&
            resolved == expected_fallback,
        "AL-29 runtime 目录不存在 -> UID 专属 fallback", resolved);

    // 2) runtime 目录可用 -> 用它
    test_support::Mkdir(runtime_dir, 0700);
    test_support::Check(
        bp::ResolveApplicationLockPath(uid, runtime_root, fallback_root,
                                       &resolved, &error) &&
            resolved == runtime_dir + "/backup-project.lock",
        "AL-30 runtime 目录可用 -> 优先用它", resolved);

    // 3) runtime 目录本身是符号链接 -> 不跟随，退 fallback
    const std::string real_runtime_dir =
        runtime_root + "/" + uid_text + ".real";
    test_support::Check(
        ::rename(runtime_dir.c_str(), real_runtime_dir.c_str()) == 0,
        "AL-31a runtime 目录改名备好");
    test_support::Check(
        test_support::CreateSymlink(real_runtime_dir, runtime_dir),
        "AL-31b 在 runtime 路径上放一个符号链接");
    test_support::Check(
        bp::ResolveApplicationLockPath(uid, runtime_root, fallback_root,
                                       &resolved, &error) &&
            resolved == expected_fallback,
        "AL-31 runtime 目录是符号链接时不跟随 -> fallback", resolved);
    test_support::Check(
        ::unlink(runtime_dir.c_str()) == 0 &&
            ::rename(real_runtime_dir.c_str(), runtime_dir.c_str()) == 0,
        "AL-31c 还原成真目录");

    // 4) runtime 目录存在但不是目录 -> fallback
    const std::string not_a_dir = runtime_root + "/file-dir";
    test_support::WriteFile(not_a_dir, "x", 0644);
    {
      // 用一个"UID 就是 file-dir 这个名字"的假 uid 来命中最直接：
      // 直接把 runtime_root 指到那个普通文件的父目录并换一个 uid 文本即可。
      // 这里改成让 runtime_dir 本身成为一个普通文件：
      const std::string other_uid_text = "4242";
      const std::string file_runtime = runtime_root + "/" + other_uid_text;
      test_support::WriteFile(file_runtime, "not a dir", 0644);
      test_support::Check(
          bp::ResolveApplicationLockPath(4242, runtime_root, fallback_root,
                                         &resolved, &error) &&
              resolved == fallback_root + "/backup-project-4242.lock",
          "AL-32 runtime 路径是普通文件 -> fallback", resolved);
      test_support::Check(!test_support::Exists(resolved),
                          "AL-33 解析本身仍然不创建任何文件");
    }

    // 5) runtime 目录属主不是这个 uid -> fallback
    test_support::Check(
        bp::ResolveApplicationLockPath(uid + 1, runtime_root, fallback_root,
                                       &resolved, &error) &&
            resolved ==
                fallback_root + "/backup-project-" +
                    std::to_string(static_cast<unsigned long long>(uid + 1)) +
                    ".lock",
        "AL-34 runtime 目录属主不是该 uid -> fallback", resolved);

    // 6) runtime 目录不可写 -> fallback
    test_support::Check(::chmod(runtime_dir.c_str(), 0500) == 0,
                        "AL-35 runtime 目录改为只读");
    test_support::Check(
        bp::ResolveApplicationLockPath(uid, runtime_root, fallback_root,
                                       &resolved, &error) &&
            resolved == expected_fallback,
        "AL-36 runtime 目录不可写 -> fallback", resolved);
    (void)::chmod(runtime_dir.c_str(), 0700);

    // 7) 产品默认函数不读 HOME /
    // XDG_CONFIG_HOME：换环境变量必须得到同一个路径。
    {
      SavedEnvironment saved_xdg("XDG_CONFIG_HOME");
      SavedEnvironment saved_home("HOME");
      std::string first;
      std::string second;
      ::setenv("XDG_CONFIG_HOME", (root + "/xdg-a").c_str(), 1);
      test_support::Check(
          bp::DefaultApplicationInstanceLockPath(&first, &error),
          "AL-37 能解析出默认锁路径", error);
      ::setenv("XDG_CONFIG_HOME", (root + "/xdg-b").c_str(), 1);
      ::setenv("HOME", (root + "/home-b").c_str(), 1);
      test_support::Check(
          bp::DefaultApplicationInstanceLockPath(&second, &error),
          "AL-38 换环境变量之后仍然能解析", error);
      test_support::Check(!first.empty() && first == second,
                          "AL-39 默认锁路径与 XDG_CONFIG_HOME / HOME 无关",
                          first + " vs " + second);
      test_support::Check(
          first == "/run/user/" + uid_text + "/backup-project.lock" ||
              first == "/tmp/backup-project-" + uid_text + ".lock",
          "AL-40 默认锁路径是 runtime 目录或 UID 专属 fallback", first);
      test_support::Check(first.find(uid_text) != std::string::npos,
                          "AL-41 默认锁路径里带着当前 uid", first);
    }

    // 8) 不安全路径必须报 kError，不能伪装成"已有实例"。
    const std::string victim = root + "/victim2.txt";
    test_support::WriteFile(victim, "keep", 0644);
    const std::string link = root + "/link.lock";
    test_support::CreateSymlink(victim, link);
    bp::ApplicationInstanceLock unsafe;
    test_support::Check(
        unsafe.Acquire(link, &error) == bp::ApplicationInstanceStatus::kError,
        "AL-42 锁路径是符号链接 -> kError，不是 kAlreadyRunning", error);
    std::string after;
    test_support::ReadFile(victim, &after);
    test_support::Check(after == "keep", "AL-43 被指向的文件仍然没被动过",
                        after);

    // 9) 自动建目录：fallback 根不存在时按 0700 建出来
    {
      const std::string deep_fallback = root + "/deep/tmp";
      bp::ApplicationInstanceLock deep;
      test_support::Check(
          bp::ResolveApplicationLockPath(uid, root + "/no-such-run",
                                         deep_fallback, &resolved, &error) &&
              deep.Acquire(resolved, &error) ==
                  bp::ApplicationInstanceStatus::kAcquired,
          "AL-44 fallback 根不存在时也能拿锁（自动建 0700 目录）", error);
      std::uint32_t mode = 0;
      test_support::Check(ModeOf(deep_fallback, &mode) && mode == 0700,
                          "AL-45 自动建出来的 fallback 目录是 0700",
                          test_support::Octal(mode));
      deep.Release();
    }
    test_support::Check(
        ::getenv("XDG_CONFIG_HOME") == nullptr ||
            std::string(::getenv("XDG_CONFIG_HOME")) != root + "/xdg-b",
        "AL-46 测试结束后环境变量已还原");

    // ---- R1..R8：runtime 目录的安全性判定 ----
    //
    // 在目录里创建 / 解析 "backup-project.lock" 需要两件事：对目录**可写**
    // （创建目录项）和**可进入 / 可搜索**（穿过目录访问它下面的名字）。
    // 只查写权限会把 mode 0600 的目录判成可用，然后在真正 open 子路径时才
    // EACCES —— 那时错误看起来像配置损坏，而不是"runtime 目录不可用，
    // 该走 fallback 了"。这一组用例把两个位都钉住。
    //
    // 全部走 ResolveApplicationLockPath 这个 seam：uid 与两个根目录都是显式
    // 参数，所以每一种 mode / owner / 节点类型都能被测到，不需要 sudo，
    // 也不会去动真实的 /run/user/<uid>。
    test_support::Section("AL 5b. runtime 目录判定：R1..R8");
    const std::string r_root = root + "/r-run";
    const std::string r_fallback = root + "/r-tmp";
    test_support::Mkdir(r_root, 0755);
    test_support::Mkdir(r_fallback, 0700);
    const std::string r_dir = r_root + "/" + uid_text;
    const std::string r_expected_fallback =
        r_fallback + "/backup-project-" + uid_text + ".lock";

    // R1：属主正确 + 0700 -> 可用
    test_support::Mkdir(r_dir, 0700);
    test_support::Check(
        bp::ResolveApplicationLockPath(uid, r_root, r_fallback, &resolved,
                                       &error) &&
            resolved == r_dir + "/backup-project.lock",
        "R1 属主是本人且 mode 0700 -> runtime 目录可用", resolved);

    // R2：属主正确 + 0600（可写但**不可搜索**）-> 不可用
    test_support::Check(::chmod(r_dir.c_str(), 0600) == 0,
                        "R2 runtime 目录改为 0600");
    test_support::Check(
        bp::ResolveApplicationLockPath(uid, r_root, r_fallback, &resolved,
                                       &error) &&
            resolved == r_expected_fallback,
        "R2 mode 0600（缺 S_IXUSR）-> 不可用，退 fallback", resolved);

    // R3：属主正确 + 0500（可搜索但**不可写**）-> 不可用
    test_support::Check(::chmod(r_dir.c_str(), 0500) == 0,
                        "R3 runtime 目录改为 0500");
    test_support::Check(
        bp::ResolveApplicationLockPath(uid, r_root, r_fallback, &resolved,
                                       &error) &&
            resolved == r_expected_fallback,
        "R3 mode 0500（缺 S_IWUSR）-> 不可用，退 fallback", resolved);
    test_support::Check(::chmod(r_dir.c_str(), 0700) == 0,
                        "R3b runtime 目录恢复 0700");

    // R4：属主不是本人 + 0777 -> 仍然不可用（世界可写不等于可以用）
    const std::string r_foreign = r_root + "/4242";
    test_support::Mkdir(r_foreign, 0777);
    test_support::Check(
        bp::ResolveApplicationLockPath(4242, r_root, r_fallback, &resolved,
                                       &error) &&
            resolved == r_fallback + "/backup-project-4242.lock",
        "R4 属主不是该 uid 时，0777 也不算可用 -> fallback", resolved);

    // R5：节点不是目录 -> 不可用
    const std::string r_file = r_root + "/5150";
    test_support::WriteFile(r_file, "not a directory", 0644);
    test_support::Check(
        bp::ResolveApplicationLockPath(5150, r_root, r_fallback, &resolved,
                                       &error) &&
            resolved == r_fallback + "/backup-project-5150.lock",
        "R5 runtime 路径是普通文件 -> fallback", resolved);

    // R6：节点是符号链接 -> 不跟随，退 fallback
    const std::string r_real = r_root + "/6000.real";
    test_support::Mkdir(r_real, 0700);
    test_support::Check(test_support::CreateSymlink(r_real, r_root + "/6000"),
                        "R6 在 runtime 路径上放一个指向真目录的符号链接");
    test_support::Check(
        bp::ResolveApplicationLockPath(6000, r_root, r_fallback, &resolved,
                                       &error) &&
            resolved == r_fallback + "/backup-project-6000.lock",
        "R6 runtime 路径是符号链接 -> 不跟随，退 fallback", resolved);

    // R7：runtime 不可用时选出来的 fallback 必须真的在 fallback 根下，
    // 不能是"看起来换了名字、其实还在 runtime 目录里"。
    test_support::Check(::chmod(r_dir.c_str(), 0600) == 0,
                        "R7 runtime 目录改为 0600");
    test_support::Check(
        bp::ResolveApplicationLockPath(uid, r_root, r_fallback, &resolved,
                                       &error) &&
            resolved == r_expected_fallback &&
            resolved.rfind(r_root + "/", 0) != 0,
        "R7 判定不可用时选出的路径确实落在 fallback 根下", resolved);
    test_support::Check(::chmod(r_dir.c_str(), 0700) == 0,
                        "R7b runtime 目录恢复 0700");

    // R8：fallback 锁本身的安全属性不能因为"退到 fallback"而放松。
    //   0600 / O_NOFOLLOW / flock 独占都在这里直接验；
    //   "锁文件属主必须是本人"需要第二个 UID 才能造出来（见下面的 NOTE）。
    {
      test_support::Check(!test_support::Exists(r_expected_fallback),
                          "R8a fallback 锁文件一开始不存在");
      bp::ApplicationInstanceLock fallback_lock;
      test_support::Check(
          fallback_lock.Acquire(r_expected_fallback, &error) ==
              bp::ApplicationInstanceStatus::kAcquired,
          "R8b fallback 锁可以拿到", error);
      std::uint32_t fallback_mode = 0;
      test_support::Check(
          ModeOf(r_expected_fallback, &fallback_mode) && fallback_mode == 0600,
          "R8b fallback 锁文件是 0600", test_support::Octal(fallback_mode));

      bp::ApplicationInstanceLock fallback_second;
      test_support::Check(
          fallback_second.Acquire(r_expected_fallback, &error) ==
              bp::ApplicationInstanceStatus::kAlreadyRunning,
          "R8c fallback 锁仍然是 flock 独占（第二个实例被拒）", error);
      fallback_lock.Release();

      // O_NOFOLLOW：目标名字上是符号链接时 kError，且被指向的文件一个字节
      // 都不能被动过 —— "退到 fallback" 不是放松路径安全检查的理由。
      const std::string r_victim = root + "/r-victim.txt";
      test_support::WriteFile(r_victim, "do not touch", 0644);
      const std::string r_link = r_fallback + "/backup-project-7777.lock";
      test_support::Check(test_support::CreateSymlink(r_victim, r_link),
                          "R8d 在 fallback 锁路径上放一个符号链接");
      bp::ApplicationInstanceLock fallback_unsafe;
      test_support::Check(
          fallback_unsafe.Acquire(r_link, &error) ==
              bp::ApplicationInstanceStatus::kError,
          "R8d fallback 路径是符号链接 -> kError（不是 kAlreadyRunning）",
          error);
      std::string r_after;
      test_support::ReadFile(r_victim, &r_after);
      test_support::Check(r_after == "do not touch",
                          "R8d 被指向的文件没有被 truncate 或被覆盖", r_after);

      // R8e：同一个位置上如果是目录，同样 fail closed。
      const std::string r_dir_lock = r_fallback + "/backup-project-8888.lock";
      test_support::Mkdir(r_dir_lock, 0755);
      bp::ApplicationInstanceLock fallback_dir;
      test_support::Check(
          fallback_dir.Acquire(r_dir_lock, &error) ==
              bp::ApplicationInstanceStatus::kError,
          "R8e fallback 路径是目录 -> kError", error);

      test_support::Note(
          "R8f 锁文件属主必须是本人（st_uid != geteuid() 一律 fail closed）"
          "需要第二个 UID 才能构造，无法在无 sudo 的单元测试里造出那个文件；"
          "该判定在 src/platform/file_lock.cpp 里，属于拒绝路径而不是接受路径。");
    }
  }

  // ---- 原子替换写入（ScheduleStore / ConfigManager 共用）----
  test_support::Section("AL 6. WriteFileAtomicallyReplacing：同名目标反复替换");
  {
    const std::string directory = LockDir();
    const std::string path = directory + "/state.json";
    std::string error;
    test_support::Check(bp::WriteFileAtomicallyReplacing(path, "first", &error),
                        "AL-47 首次写入成功", error);
    std::string content;
    test_support::ReadFile(path, &content);
    test_support::Check(content == "first", "AL-48 内容正确", content);

    test_support::Check(
        bp::WriteFileAtomicallyReplacing(path, "second", &error),
        "AL-49 覆盖写入成功", error);
    test_support::ReadFile(path, &content);
    test_support::Check(content == "second", "AL-50 覆盖之后是新的完整内容",
                        content);

    std::uint32_t mode = 0;
    test_support::Check(ModeOf(path, &mode) && mode == 0600,
                        "AL-51 目标权限是 0600", test_support::Octal(mode));

    // 临时文件必须被清理干净：目录里只剩目标文件本身。
    const std::vector<std::string> entries =
        test_support::DirEntries(directory);
    bool leftovers = false;
    for (const std::string& name : entries) {
      if (name.find(".tmp-") != std::string::npos) leftovers = true;
    }
    test_support::Check(!leftovers, "AL-52 成功之后没有留下任何临时文件");

    // 目标是一个符号链接时，写入必须替换**链接本身**而不是跟随它写穿过去。
    // rename 天然满足这一点（它作用在目录项上），这里把它钉住。
    const std::string victim = directory + "/victim3.txt";
    test_support::WriteFile(victim, "untouched", 0644);
    const std::string link = directory + "/linked-state.json";
    test_support::CreateSymlink(victim, link);
    test_support::Check(
        bp::WriteFileAtomicallyReplacing(link, "replaced", &error),
        "AL-53 目标名字上是符号链接时也能替换目录项", error);
    std::string victim_content;
    test_support::ReadFile(victim, &victim_content);
    test_support::Check(victim_content == "untouched",
                        "AL-54 链接指向的文件没有被写穿", victim_content);
    std::string link_content;
    test_support::ReadFile(link, &link_content);
    test_support::Check(link_content == "replaced",
                        "AL-55 那个名字现在是一个装着新内容的普通文件",
                        link_content);

    test_support::Check(!bp::WriteFileAtomicallyReplacing("", "x", &error),
                        "AL-56 空路径明确失败");
    test_support::Check(Contains(error, "path is empty"),
                        "AL-57 空路径的报错说清楚原因", error);
  }

  return test_support::Finish("application_lock_test");
}
