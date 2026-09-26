// scheduler_lock.h
//
// 窄范围的 single-runner 锁。
//
// 它回答的问题只有一个：现在到底由谁来跑这个计划任务。
//
//   GUI 已经开着 scheduler timer
//   同时另一个终端在跑 backupctl schedule watch
//
// 这两个进程都觉得自己该跑，结果就是同一时刻两份备份在写同一个仓库。
// 本 PR 不要求做完整的分布式锁，但也不能对多进程边界完全无感，所以：
//
//   * schedule store 本身是 atomic write（ScheduleStore 负责）；
//   * scheduler 每次运行前先抢这把 flock；
//   * 抢不到的一方明确显示"计划任务已由另一进程持有"，而不是也去备份一遍。
//
// 刻意不做的事：
//   * 不做 pidfile 清理协议、不做超时踢锁——flock 随进程退出（甚至崩溃）自动
//     释放，这正是选它而不是选"写一个 pid 文件"的原因；
//   * 不把锁文件内容当成真相。里面的 pid/时间只是给人看的提示，
//     权限判断永远来自 flock 本身。
//
// 本文件是纯 C++17 + POSIX：不依赖 Qt。

#ifndef BACKUP_PROJECT_INCLUDE_SCHEDULER_LOCK_H_
#define BACKUP_PROJECT_INCLUDE_SCHEDULER_LOCK_H_

#include <string>

namespace backupproject {

class SchedulerLock {
 public:
  SchedulerLock() = default;
  ~SchedulerLock();

  // 不可拷贝：它持有一个 fd，复制会让"谁负责释放"变得没有答案。
  SchedulerLock(const SchedulerLock&) = delete;
  SchedulerLock& operator=(const SchedulerLock&) = delete;

  // 非阻塞抢锁。抢不到（EWOULDBLOCK）不是"出错了"而是"别人正在跑"，
  // 但两种情况的返回值都是 false——调用方看 error_message 区分。
  bool Acquire(const std::string& lock_file_path, std::string* error_message);

  bool held() const { return fd_ >= 0; }

  // 锁文件里的提示文本（"pid=… started_at=…"）。仅供展示。
  std::string ReadOwnerHint() const;

  void Release();

 private:
  int fd_ = -1;
  std::string lock_file_path_;
};

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_SCHEDULER_LOCK_H_
