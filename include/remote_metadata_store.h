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
  // PR #21：这个快照还被别的快照当作父引用着，删掉它会让那些子快照
  // 永远无法恢复。删除必须依赖感知：先删叶子，再删祖先。
  kHasDependents,
  // PR #21 审查修复：链的边界条件不成立。包括"父不存在（见 kNotFound）"
  // 之外的四种：lineage 不同、代数不是父+1、父已经有活着的孩子（本产品是
  // **线性链**，不允许分叉）、代数超过本地增量引擎能恢复的上限。
  kChainConflict,
};

// 远端链允许的最大代数定义在 include/network_protocol.h（协议层面的常量，
// 客户端与服务端共用同一个值；这里使用它但不再重复定义）。

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

  // ---- PR #21：远端增量链 ----
  //
  // 0 = full（链根，generation 0、没有父），1 = incremental（必须有父）。
  std::uint16_t snapshot_kind = 0;
  // 父快照的 id；full 恒为空串。**服务端校验过**：父必须存在、属于同一个
  // 用户、lineage 相同；generation 由服务端按 parent.generation + 1 推导，
  // 客户端根本没有机会自己填一个数。
  std::string parent_id;
  // 距离链根的代数：full = 0，每挂一层 +1。
  std::uint64_t generation = 0;
  // 链的归属摘要（64 个小写十六进制字符）。同一个源 + 同一个远端仓库身份
  // 得到同一个 lineage；不同 lineage 之间不允许建立父子关系。
  std::string lineage;
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

  // 打开（必要时创建）元数据库。服务端用它：一个新实例本来就该被初始化。
  bool Open(const std::string& path, std::string* error_message);
  // 只打开**已经存在**的数据库文件，绝不创建。
  //
  // 管理与诊断工具用它：路径写错时必须明确失败，而不是让 SQLite 悄悄建一个
  // 空库——那样"这个实例还没有任何用户"和"你指的是另一个实例"在界面上一模一样，
  // 人工验收会得出完全错误的结论（本轮 P0 就是这么发生的）。
  //
  // 打开方式显式分成两个入口，因为"只读"与"可写"是两种相反的产品承诺，
  // 不能藏在同一个 OpenExisting() 里：
  //   * OpenExistingReadOnly：管理工具的只读命令（status / list-users /
  //     show-user / list-snapshots / overview）用它。连接是
  //     SQLITE_OPEN_READONLY（外加 PRAGMA query_only），所以不可能建表、
  //     不可能写 user_version、不可能开写事务——backup-server 正在运行时它
  //     也只是读者，这正是"服务端在跑也能安全查看"的兑现方式；
  //   * OpenExistingReadWrite：破坏性命令（delete-user / delete-snapshot）
  //     用它，调用方必须**已经**持有数据目录锁（证明服务端已停止）才允许走到
  //     这里。
  // 两者都不 CREATE、不建表：schema 必须已经存在而且版本正确，否则明确失败。
  bool OpenExistingReadOnly(const std::string& path,
                            std::string* error_message);
  bool OpenExistingReadWrite(const std::string& path,
                             std::string* error_message);
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

  // 插入一行快照。**链的边界条件在这个函数内部、与 INSERT 同一个互斥区间里
  // 重新校验**（不是在调用方）：
  //   * 增量：父必须仍然存在、属于同一个用户、lineage 相同、
  //     generation == parent.generation + 1、不超过 kMaxRemoteChainGeneration，
  //     并且父**还没有活着的孩子**（线性链，不允许分叉）；
  //   * 完整快照：parent 必须为空、generation 必须为 0。
  // 调用方在 UPLOAD_BEGIN 时也会校验一次，但那只是"早点给用户一个说法"：
  // 真正的权威校验必须与写入原子，否则删除与上传并发时会留下"父没了、子还在"
  // 的孤儿（TOCTOU）。返回 kNotFound 表示父不存在，kChainConflict
  // 表示其余冲突。
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
  // 有多少个快照把这个 id 当作父。删除前的依赖检查用它：非 0 就不许删。
  StoreResult CountSnapshotChildren(std::int64_t user_id,
                                    const std::string& snapshot_id,
                                    std::uint64_t* out,
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
  // 让迁移在**补完指定列之后**立刻失败（用于证明"迁移到一半"会被整体回滚）。
  // 传入列名（snapshot_kind / parent_id / generation / lineage）；产品代码里
  // 没有任何地方调用它。
  void FailMigrationAfterColumnForTesting(const std::string& column_name) {
    fail_migration_after_column_ = column_name;
  }
  // 让下一次 DeleteUser 在提交之前失败：事务整体回滚，磁盘上的隔离动作
  // 必须由调用方撤回来。产品代码里没有任何地方调用它。
  void FailNextDeleteUserForTesting() { fail_next_delete_user_ = true; }

 private:
  bool Execute(const std::string& sql, std::string* error_message);
  bool Prepare(const std::string& sql, sqlite3_stmt** statement,
               std::string* error_message);
  bool EnsureSchema(std::string* error_message);
  // snapshots 表当前有哪些列（PRAGMA table_info）。迁移据此只补缺的列，
  // 所以它是幂等的。只在服务端的可写打开路径上调用。
  bool SnapshotColumnsPresent(std::vector<std::string>* columns,
                              std::string* error_message);
  // 只读 / 可写两种"打开已经存在的库"的公共实现（writable 决定连接标志）。
  bool OpenExistingWithMode(const std::string& path, bool writable,
                            std::string* error_message);
  // 校验一个已经存在的库：schema 版本对得上、必要的表都在、并且真的能读。
  // 只读，绝不改写——它代替了以前"打开管理工具就顺手 EnsureSchema"的行为。
  bool VerifyExistingSchema(std::string* error_message);
  std::string LastError() const;

  sqlite3* database_ = nullptr;
  std::string path_;
  bool fail_next_insert_ = false;
  bool fail_next_delete_user_ = false;
  std::string fail_migration_after_column_;
  // 保护 database_ 与 fail_next_insert_：worker 线程会并发进来。
  mutable std::mutex mutex_;
};

}  // namespace net
}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_REMOTE_METADATA_STORE_H_
