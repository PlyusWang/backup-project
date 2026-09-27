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
//
// **产品层的多进程问题已经不由这把锁负责了**：整个产品只允许一个进程
// （见 application_instance_lock.h），GUI 与 backupctl 在进入任何业务逻辑之前
// 就已经互相排斥。这把锁因此退化成 scheduler 内部的 defense in depth：
//
//   * 它守的是"同一个进程里如果将来出现第二个 runner，也不会并发写仓库"；
//   * 单元测试仍然直接用它来验证"抢不到锁就不跑"，那是它的真实语义；
//   * 跨进程的 schedule watch vs schedule watch 现在**首先**被
//     ApplicationInstanceLock 拒绝，看到的是"已有另一个实例"，不是这条消息。
//
// 锁顺序固定：ApplicationInstanceLock -> SchedulerLock。
//
// 锁机制（flock / O_NOFOLLOW / fstat 普通文件）统一实现在 file_lock.h，
// 这里只负责 scheduler 自己的措辞与"谁在跑"的含义。
//
// 本文件是纯 C++17 + POSIX：不依赖 Qt。

#ifndef BACKUP_PROJECT_INCLUDE_SCHEDULER_LOCK_H_
#define BACKUP_PROJECT_INCLUDE_SCHEDULER_LOCK_H_

#include <string>

#include "file_lock.h"

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

  bool held() const { return lock_.held(); }

  // 锁文件里的提示文本（"pid=… started_at=…"）。仅供展示。
  std::string ReadOwnerHint() const { return lock_.ReadOwnerHint(); }

  void Release() { lock_.Release(); }

 private:
  FileLock lock_;
};

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_SCHEDULER_LOCK_H_
