// file_lock.h
//
// 进程级 flock 的唯一实现。
//
// 项目里有两个"同一时刻只能有一个持有者"的需求，它们问的问题完全不同：
//
//   SchedulerLock           谁来跑这份计划任务
//   ApplicationInstanceLock 这台机器上是否已经有一个 backup-project 进程在跑
//
// 但"怎么安全地拿一把内核锁"只有一套正确答案，所以机械部分单独成模块。
// 两个上层类型各自决定**含义**与**给人看的消息**，open/flock/fstat 一律走
// 这里——两份各自演化的锁代码迟早会在其中一份上漏掉某条安全边界。
//
// 三条硬性要求（都是安全边界，不是风格）：
//   * flock(LOCK_EX | LOCK_NB)：锁由内核持有，进程正常退出、崩溃、被 SIGKILL
//     都会自动释放。绝不用"文件存在即上锁"——崩溃会留下 stale 文件；
//   * 打开时 O_NOFOLLOW：锁路径本身是符号链接一律拒绝，绝不跟随；
//   * 打开后 fstat 必须是普通文件：目录、FIFO、设备文件一律拒绝。对它们
//     flock 的语义各不相同，而且打开一个目录在 Linux 上会成功。
//
// 刻意不做的事：
//   * 不做 pidfile 清理协议、不做超时踢锁——flock 随 fd 关闭释放，这正是选它
//     而不是"写一个 pid 文件"的原因；
//   * 不把锁文件内容当成真相。里面的 pid/时间只是给人看的提示，权限判断永远
//     来自 flock 本身；
//   * 不删除锁文件来"释放锁"。锁文件可以永久留在磁盘上：unlink 一个正在被
//     别人 flock 的文件会让双方各自持有**不同的 inode**，锁因此形同虚设。
//
// 本文件是纯 C++17 + POSIX：不依赖 Qt。

#ifndef BACKUP_PROJECT_INCLUDE_FILE_LOCK_H_
#define BACKUP_PROJECT_INCLUDE_FILE_LOCK_H_

#include <string>

namespace backupproject {

enum class FileLockStatus {
  // 拿到了。fd 归本对象所有，Release() 或析构时关闭。
  kAcquired,
  // 别人正持有。这是正常状态，不是错误：调用方按"别人在跑"处理。
  kBusy,
  // 打不开、锁不上、或者锁路径不安全。error_message 里是具体原因。
  kError,
};

class FileLock {
 public:
  FileLock() = default;
  ~FileLock();

  // 不可拷贝：它持有一个 fd，复制会让"谁负责释放"变得没有答案。
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;

  // 非阻塞抢锁。可以重复调用：已经持有的那把锁会先被释放。
  FileLockStatus Acquire(const std::string& lock_file_path,
                         std::string* error_message);

  bool held() const { return fd_ >= 0; }

  // 锁文件里的提示文本（"pid=… started_at=…"）。仅供展示。
  std::string ReadOwnerHint() const;

  void Release();

 private:
  int fd_ = -1;
  std::string lock_file_path_;
};

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_FILE_LOCK_H_
