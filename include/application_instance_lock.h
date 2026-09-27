// application_instance_lock.h
//
// 全应用单实例锁：**整个产品同一时刻只允许一个进程**。
//
// 它回答的问题只有一个：
//
//   "backup-project 这个产品是否已经有一个实例在跑？"
//
// 而不是"这份计划任务现在由谁来跑"——那是 SchedulerLock 的问题。两者刻意
// 分开：单实例是**产品层**的决定（GUI+GUI / GUI+CLI / CLI+CLI / CLI+GUI 全部
// 拒绝），SchedulerLock 退化成 scheduler 内部的 defense in depth。
//
// 锁路径的三条约束（少一条就等于没有单实例）：
//   * 与 repository 无关：换仓库不换锁；
//   * 与 --config-file / --schedule-file 无关：换存储位置不换锁，
//     否则"两个进程带不同参数启动"就成了绕过手段；
//   * GUI 与 backupctl 解析出的默认路径**完全相同**（都走 app_paths.h），
//     因为"GUI 存的东西 CLI 读不到"这类最难查的 bug 就是路径漂移造成的。
//
// 锁协议是 flock，见 file_lock.h：进程退出、崩溃、SIGKILL 都由内核自动释放，
// 磁盘上留一个 stale 锁文件不会把产品永久锁死。
//
// 锁顺序固定为：
//
//   ApplicationInstanceLock -> SchedulerLock
//
// 不能反过来：单实例锁永远是外层，先拿它再谈 scheduler 内部的事，
// 将来加锁也不会出现环。

#ifndef BACKUP_PROJECT_INCLUDE_APPLICATION_INSTANCE_LOCK_H_
#define BACKUP_PROJECT_INCLUDE_APPLICATION_INSTANCE_LOCK_H_

#include <sys/types.h>

#include <string>

#include "file_lock.h"

namespace backupproject {

// "已经有另一个实例在跑"的退出码。GUI 与 backupctl 用**同一个**数字：
// 这是同一条产品规则，两个前端不该各自发明一套。
inline constexpr int kApplicationAlreadyRunningExitCode = 3;

enum class ApplicationInstanceStatus {
  // 拿到了。此后本进程是这台机器上唯一的 backup-project。
  kAcquired,
  // 已经有另一个实例在跑：产品规则下的正常拒绝，不是错误。
  kAlreadyRunning,
  // 锁本身出了问题（路径不安全、打不开、HOME 不可用……）。**不能**当成
  // "已有实例"报出去：那会把一个环境问题伪装成并发问题，排障方向直接跑偏。
  kError,
};

class ApplicationInstanceLock {
 public:
  ApplicationInstanceLock() = default;
  ~ApplicationInstanceLock();

  ApplicationInstanceLock(const ApplicationInstanceLock&) = delete;
  ApplicationInstanceLock& operator=(const ApplicationInstanceLock&) = delete;

  // 调用方必须在**碰任何业务状态之前**调用它（读配置也算）。
  // 失败时把给人看的一句话写进 error_message。
  ApplicationInstanceStatus Acquire(const std::string& lock_file_path,
                                    std::string* error_message);

  bool held() const { return lock_.held(); }

  // 锁文件里的 pid/时间提示。仅供展示，不是判断依据。
  std::string ReadOwnerHint() const { return lock_.ReadOwnerHint(); }

  void Release() { lock_.Release(); }

 private:
  FileLock lock_;
};

// 产品全局实例锁的路径解析。
//
// 只依赖**当前 Unix UID + 固定产品身份**，不依赖任何可变的环境或参数：
//
//   * HOME / XDG_CONFIG_HOME —— 依赖它，同一个用户换一个环境变量就能开第二个
//     实例，那就不叫"整个产品只允许一个进程"了；
//   * repository / --config-file / --schedule-file / cwd —— 同理。
//
// 依次尝试：
//   1. <runtime_root>/<uid>/backup-project.lock   例如
//   /run/user/1000/backup-project.lock
//      只在该目录存在、是真目录（不是符号链接）、属主是 uid、属主**可写**且
//      属主**可进入/可搜索**（S_IWUSR 与 S_IXUSR 都必须有）时使用：
//      在目录里创建并解析锁文件名，两个位缺一不可，只查写权限会让 mode 0600
//      的目录一路走到 open 才失败；
//   2. <fallback_root>/backup-project-<uid>.lock  例如
//   /tmp/backup-project-1000.lock
//      fallback 目录通常是 sticky 的 /tmp：别的用户可能抢先占住这个文件名，
//      这种情况由 FileLock 的"属主必须是本人"检查 fail closed，绝不跟随/覆盖。
//
// uid 与两个根目录都是显式参数，所以这条决策可以被单元测试完整覆盖，
// 不需要伪造 /run/user 或第二个用户。
bool ResolveApplicationLockPath(uid_t uid, const std::string& runtime_root,
                                const std::string& fallback_root,
                                std::string* path, std::string* error_message);

// 产品默认：ResolveApplicationLockPath(getuid(), "/run/user", "/tmp")。
bool DefaultApplicationInstanceLockPath(std::string* path,
                                        std::string* error_message);

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_APPLICATION_INSTANCE_LOCK_H_
