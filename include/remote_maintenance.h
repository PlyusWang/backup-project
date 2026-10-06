// include/remote_maintenance.h
//
// PR #20（closure）：服务端与 ECS 本地管理工具**共用**的数据维护原语。
//
// 为什么必须要有这一层：管理工具需要"删除某个 snapshot"和"删除某个账户及其
// 全部云端数据"。如果它自己拼 SQL + rm，安全删除的顺序（先让数据不可见、再动
// 元数据、最后物理删除；任何一步失败都回滚）就会有两份实现，而两份实现迟早
// 会分叉——分叉的那一天，管理工具就变成了绕过服务端语义的后门。
//
// 所以 backup-server 的 DELETE / DELETE_ACCOUNT 与 backup-server-admin 的删除
// 动作调用的是**同一批函数**。这一层不认识 socket，也不认识协议：它只处理
// "数据目录 + 元数据库"。
//
// 失败一致性（账户注销）：
//
//   1. <root>/users/<id> 整体 rename 到
//   <root>/trash/account-<id>.<随机>.deleted
//      —— 这一步是原子的，做完之后这些字节对任何客户端都不再可见；
//   2. 元数据库在**一个事务**里删 snapshots 行 + users 行 + 写墓碑；
//   3. 事务失败 -> 把目录改回原名字（数据完好如初），返回失败；
//   4. 事务成功 -> 物理删除 quarantine 目录。这一步失败只记告警：数据已经在
//      trash 里且元数据已经不存在，不会出现"用户没了、blob 还挂在正常位置"
//      这种半成功状态。
//
// 账户注销与"snapshot 删除"用的是同一套顺序，只是粒度从单个文件变成整个目录。

#ifndef BACKUP_PROJECT_INCLUDE_REMOTE_MAINTENANCE_H_
#define BACKUP_PROJECT_INCLUDE_REMOTE_MAINTENANCE_H_

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

#include "remote_metadata_store.h"

namespace backupproject {
namespace net {

class RemoteMaintenance {
 public:
  using LogFunction = std::function<void(const std::string&)>;

  RemoteMaintenance() = default;
  // store 由调用方持有：服务端复用它已经打开的那一个 SQLite 连接（绝不为了
  // 管理动作在同一进程里再开第二个连接），管理工具打开自己的那一个。
  RemoteMaintenance(RemoteMetadataStore* store, std::string root_directory)
      : store_(store), root_directory_(std::move(root_directory)) {}

  void set_log(LogFunction log) { log_ = std::move(log); }
  bool ready() const { return store_ != nullptr && !root_directory_.empty(); }
  const std::string& root_directory() const { return root_directory_; }

  // 磁盘路径永远由服务端生成：数字 user id + 服务端生成的 storage_name。
  // 客户端给的用户名与显示名一次都不参与拼接。
  std::string UserDirectory(std::int64_t user_id) const;

  // 删除单个 snapshot。返回值与 RemoteServer::HandleDelete 原来的语义逐字一致：
  //   kOk       删除完成（trash 里的残留 unlink 失败只记告警）
  //   kNotFound 该用户的这一行不存在，或 blob 已经不在磁盘上
  //   kError    任何一步失败。正常情况下它会先把已经动过的那一步回滚掉，
  //            但回滚本身也可能失败（例如把 blob 从 trash 改名回去时磁盘出错）。
  //            那种情况下数据会停在 quarantine / trash 里，元数据行仍然存在，
  //            并且会写一条明确的诊断日志。**不要把 kError 读成“磁盘与元数据一定完全没动过”**。
  StoreResult DeleteSnapshot(std::int64_t user_id,
                             const std::string& snapshot_id,
                             RemoteSnapshotRecord* removed,
                             std::string* error_message);

  // 删除一个账户的全部数据（见文件头的顺序说明）。removed_* 是事务里统计到的
  // 元数据量，供调用方记账。
  StoreResult DeleteAccount(std::int64_t user_id,
                            std::uint64_t* removed_snapshots,
                            std::uint64_t* removed_bytes,
                            std::string* error_message);

  // 数据目录上的共享锁文件。backup-server 在 Start() 时独占持有它，"破坏性
  // 管理操作"也必须先抢到它——这不是 pgrep 那种提示性检查，而是内核持有的
  // flock：进程崩溃、被 SIGKILL 都会自动释放，不留 stale 状态。
  static std::string LockFilePath(const std::string& root_directory);
  // 只读锁文件里的提示文本（"pid=… started_at=…"）。读不到时返回空串。
  // 真相永远在 flock 上，这里的内容只用于告诉用户"是谁在跑"。
  static std::string ReadLockHint(const std::string& root_directory);

  // 递归删除一个目录（不跟随符号链接：链接本身被 unlink，不递归进去）。
  static bool RemoveDirectoryTree(const std::string& path,
                                  std::string* error_message);

 private:
  void Log(const std::string& message);
  static bool EnsureDirectory(const std::string& path,
                              std::string* error_message);

  RemoteMetadataStore* store_ = nullptr;
  std::string root_directory_;
  LogFunction log_;
};

}  // namespace net
}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_REMOTE_MAINTENANCE_H_
