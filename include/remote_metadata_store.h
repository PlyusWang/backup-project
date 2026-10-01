// include/remote_metadata_store.h
//
// PR #20：远程快照的 SQLite 元数据库。
//
// 只存**元数据**，不存字节：blob 永远是磁盘上的文件，SQLite 里只有它的
// 名字、长度、SHA-256 和归属。这样"文件在、记录不在"和"记录在、文件不在"
// 这两种不一致都能被单独发现和处理（见 src/network/remote_server.cpp 里的
// publish / delete 顺序）。
//
// 硬规则：
//   * 所有 SQL 都是预编译语句 + 绑定参数，没有字符串拼接的查询；
//   * disk path 永远由服务端自己生成（storage_name），绝不来自客户端给的
//     显示名；
//   * 一把互斥锁把写串行化，加上 busy_timeout，因此并发上传不会随机得到
//     "database is locked"；
//   * database is locked 之类的内部错误只写服务端日志，不回给客户端；
//   * user id 显式分配且**永不重用**：注销一个账户会在 deleted_users 里留下
//     一条墓碑，下一个 id 一定大于所有历史 id。否则 rowid 会在删掉最大 id
//     之后被重用，一个注销前签发的 token 就会命中新注册的账户。
//
// 头文件刻意不包含 sqlite3.h：只有服务端的 .cpp 需要它，
// 桌面端（backupctl / GUI）根本不链接 SQLite。

#ifndef BACKUP_PROJECT_INCLUDE_REMOTE_METADATA_STORE_H_
#define BACKUP_PROJECT_INCLUDE_REMOTE_METADATA_STORE_H_

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "remote_auth.h"

// 前向声明：头文件不包含 sqlite3.h，只有实现文件需要它。
struct sqlite3;
struct sqlite3_stmt;

namespace backupproject {
namespace net {

// 存储层的返回码。与协议状态码一一对应，因为它们表达的是同一件事。
enum class StoreResult {
  kOk,
  kNotFound,
  kAlreadyExists,
  kError,
};

const char* StoreResultName(StoreResult result);

struct RemoteUserRecord {
  std::int64_t user_id = 0;
  std::string username;
  PasswordRecord password;
  std::int64_t created_at = 0;
};

struct RemoteSnapshotRecord {
  std::string snapshot_id;
  std::int64_t user_id = 0;
  std::string display_name;
  std::uint64_t size_bytes = 0;
  std::string sha256;
  std::int64_t created_at = 0;
  // 服务端生成的磁盘文件名（<snapshot_id>.bak）。永远不来自客户端。
  std::string storage_name;
};

// 管理视图用的每用户汇总。
//
// 这里刻意只有 id / 名字 / 创建时间 / 数量 / 字节数：**没有** salt、hash 或
// 迭代次数。管理工具因此不是"保证不打印口令字段"，而是根本取不到它们。
struct RemoteUserSummary {
  std::int64_t user_id = 0;
  std::string username;
  std::int64_t created_at = 0;
  std::uint64_t snapshot_count = 0;
  std::uint64_t total_bytes = 0;
};

// 存储概览：四个标量，全部由 SQL 聚合出来，不在应用层遍历文件系统。
struct RemoteStorageOverview {
  std::uint64_t user_count = 0;
  std::uint64_t snapshot_count = 0;
  std::uint64_t total_bytes = 0;
  // 已经注销的账户数（墓碑表里的行数）。
  std::uint64_t deleted_user_count = 0;
};

class RemoteMetadataStore {
 public:
  RemoteMetadataStore();
  ~RemoteMetadataStore();

  RemoteMetadataStore(const RemoteMetadataStore&) = delete;
  RemoteMetadataStore& operator=(const RemoteMetadataStore&) = delete;

  bool Open(const std::string& path, std::string* error_message);
  void Close();
  bool IsOpen() const { return database_ != nullptr; }

  StoreResult CreateUser(const std::string& username,
                         const PasswordRecord& password,
                         std::int64_t created_at, std::int64_t* out_user_id,
                         std::string* error_message);
  StoreResult FindUser(const std::string& username, RemoteUserRecord* out,
                       std::string* error_message);
  StoreResult FindUserById(std::int64_t user_id, RemoteUserRecord* out,
                           std::string* error_message);

  StoreResult InsertSnapshot(const RemoteSnapshotRecord& record,
                             std::string* error_message);
  StoreResult ListSnapshots(std::int64_t user_id,
                            std::vector<RemoteSnapshotRecord>* out,
                            std::string* error_message);
  StoreResult FindSnapshot(std::int64_t user_id, const std::string& snapshot_id,
                           RemoteSnapshotRecord* out,
                           std::string* error_message);
  // 删除并返回被删掉的那一行（删除顺序需要它的 storage_name）。
  StoreResult DeleteSnapshot(std::int64_t user_id,
                             const std::string& snapshot_id,
                             RemoteSnapshotRecord* removed,
                             std::string* error_message);
  StoreResult CountSnapshots(std::int64_t user_id, std::uint64_t* out,
                             std::string* error_message);

  // ---- 管理视图（服务端与 ECS 本地管理工具共用）----
  StoreResult ListUsers(std::vector<RemoteUserSummary>* out,
                        std::string* error_message);
  StoreResult StorageOverview(RemoteStorageOverview* out,
                              std::string* error_message);

  // 删除一个用户的**全部元数据**：snapshots 行 + users 行 + 一条墓碑。
  //
  // 三步在同一个事务里，所以不存在"用户行没了、快照行还在"或者反过来的
  // 中间态：要么全部生效，要么一行都没动（返回 kError）。磁盘上的 blob 由
  // 调用方按 trash/quarantine 顺序处理，见 remote_maintenance.h。
  StoreResult DeleteUser(std::int64_t user_id, std::uint64_t* removed_snapshots,
                         std::uint64_t* removed_bytes,
                         std::string* error_message);

  // ---- 测试专用故障注入 ----
  //
  // "DB 写入失败时最终 blob 必须被回滚"这条路径必须能真的被触发一次，
  // 而不是只写在注释里。产品代码里没有任何地方调用它。
  void FailNextInsertForTesting() { fail_next_insert_ = true; }
  // 让下一次 DeleteUser 在提交之前失败：事务整体回滚，磁盘上的隔离动作
  // 必须由调用方撤回来。产品代码里没有任何地方调用它。
  void FailNextDeleteUserForTesting() { fail_next_delete_user_ = true; }

 private:
  bool Execute(const std::string& sql, std::string* error_message);
  bool Prepare(const std::string& sql, sqlite3_stmt** statement,
               std::string* error_message);
  bool EnsureSchema(std::string* error_message);
  std::string LastError() const;

  sqlite3* database_ = nullptr;
  std::string path_;
  bool fail_next_insert_ = false;
  bool fail_next_delete_user_ = false;
  // 保护 database_ 与 fail_next_insert_：worker 线程会并发进来。
  mutable std::mutex mutex_;
};

}  // namespace net
}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_REMOTE_METADATA_STORE_H_
