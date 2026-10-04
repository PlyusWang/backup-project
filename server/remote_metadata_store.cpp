// src/network/remote_metadata_store.cpp

#include "remote_metadata_store.h"

#include <sqlite3.h>
#include <sys/stat.h>

#include <cstring>
#include <ctime>
#include <utility>

#include "network_protocol.h"

namespace backupproject {
namespace net {
namespace {

// WAL 让读写并发不至于互相阻塞；synchronous=FULL 保证 commit 之后掉电也还在。
// foundation 版本的写入量很小，用安全性换那点吞吐不值得反过来做。
constexpr const char* kPragmaStatements[] = {
    // busy_timeout 必须排在**最前面**：并发首次打开时，第二条连接若还没装上
    // busy_timeout 就先执行 journal_mode=WAL（需要写锁），它会立刻拿到
    // SQLITE_BUSY，表现为随机的 "database is locked" 假失败（审查轮实测
    // 2 线程首次并发 Open 可复现，重试即成功）。
    "PRAGMA busy_timeout=5000;", "PRAGMA journal_mode=WAL;",
    "PRAGMA synchronous=FULL;",  "PRAGMA foreign_keys=ON;",
    "PRAGMA busy_timeout=5000;",
};

constexpr const char* kSchemaStatements[] = {
    "CREATE TABLE IF NOT EXISTS users ("
    "  id INTEGER PRIMARY KEY,"
    "  username TEXT UNIQUE NOT NULL,"
    "  password_salt BLOB NOT NULL,"
    "  password_hash BLOB NOT NULL,"
    "  password_iterations INTEGER NOT NULL,"
    "  created_at INTEGER NOT NULL"
    ");",
    "CREATE TABLE IF NOT EXISTS snapshots ("
    "  id TEXT PRIMARY KEY,"
    "  user_id INTEGER NOT NULL,"
    "  display_name TEXT NOT NULL,"
    "  size_bytes INTEGER NOT NULL,"
    "  sha256 TEXT NOT NULL,"
    "  created_at INTEGER NOT NULL,"
    "  storage_name TEXT NOT NULL,"
    // PR #21 的链元数据。parent_id 用空串表示"没有父"，不用 NULL：
    // 少一种边界状态，SQL 与绑定参数都少一处分支。
    "  snapshot_kind INTEGER NOT NULL DEFAULT 0,"
    "  parent_id TEXT NOT NULL DEFAULT '',"
    "  generation INTEGER NOT NULL DEFAULT 0,"
    "  lineage TEXT NOT NULL DEFAULT '',"
    "  FOREIGN KEY(user_id) REFERENCES users(id)"
    ");",
    // 注销墓碑：user id 永不重用的依据，也是"这个账户确实注销过"的审计行。
    // 它不存用户名、不存口令、不存任何 blob 引用，所以留着它不构成隐私面。
    "CREATE TABLE IF NOT EXISTS deleted_users ("
    "  id INTEGER PRIMARY KEY,"
    "  deleted_at INTEGER NOT NULL"
    ");",
    "CREATE INDEX IF NOT EXISTS snapshots_by_user"
    "  ON snapshots(user_id, created_at, id);",
};

// 依赖感知删除要按 (user_id, parent_id) 找子节点。这个索引引用 PR #21 才加的
// 列，所以**必须**等列补齐之后再建：旧库上先建索引会以
// "no such column: parent_id" 失败，整个迁移就会回滚。
constexpr const char* kChainIndexStatements[] = {
    "CREATE INDEX IF NOT EXISTS snapshots_by_parent"
    "  ON snapshots(user_id, parent_id);",
};

// PR #21：schema 1 -> 2 加入远端增量链元数据（snapshot_kind / parent_id /
// generation / lineage）。版本号就是 PRAGMA user_version。
//
// 迁移规则（这也是"绝不 DROP TABLE、绝不清库"的兑现方式）：
//   * 只做 ALTER TABLE ADD COLUMN 与 CREATE INDEX IF NOT EXISTS；
//   * 旧的 PR #20 行一律留成"legacy standalone full"：snapshot_kind=0、
//     parent_id=''、generation=0、lineage=''——DEFAULT 子句就是它们的值，
//     不需要 UPDATE，也就不存在"迁移一半改了半张表"的中间态；
//   * 整个迁移在一个 BEGIN IMMEDIATE 事务里，任何一步失败都 ROLLBACK；
//   * 幂等：列已经存在时不再 ALTER，所以重复执行（包括服务器被 kill 之后
//     重启）都是安全的；
//   * **只有服务端的可写打开路径会调用它**。只读的管理工具走
//     VerifyExistingSchema，版本不对就明确失败，绝不偷偷升级。
constexpr int kSchemaVersion = 2;
constexpr int kLegacySchemaVersion = 1;

// 每个新列：名字 + 建表片段。顺序固定，方便审计。
constexpr const char* kChainColumnNames[] = {"snapshot_kind", "parent_id",
                                             "generation", "lineage"};

std::string ColumnText(sqlite3_stmt* statement, int index) {
  const unsigned char* text = sqlite3_column_text(statement, index);
  const int bytes = sqlite3_column_bytes(statement, index);
  if (text == nullptr || bytes <= 0) {
    return std::string();
  }
  return std::string(reinterpret_cast<const char*>(text),
                     static_cast<std::size_t>(bytes));
}

std::string ColumnBlob(sqlite3_stmt* statement, int index) {
  const void* data = sqlite3_column_blob(statement, index);
  const int bytes = sqlite3_column_bytes(statement, index);
  if (data == nullptr || bytes <= 0) {
    return std::string();
  }
  return std::string(static_cast<const char*>(data),
                     static_cast<std::size_t>(bytes));
}

RemoteSnapshotRecord ReadSnapshotRow(sqlite3_stmt* statement) {
  RemoteSnapshotRecord record;
  record.snapshot_id = ColumnText(statement, 0);
  record.user_id = sqlite3_column_int64(statement, 1);
  record.display_name = ColumnText(statement, 2);
  record.size_bytes =
      static_cast<std::uint64_t>(sqlite3_column_int64(statement, 3));
  record.sha256 = ColumnText(statement, 4);
  record.created_at = sqlite3_column_int64(statement, 5);
  record.storage_name = ColumnText(statement, 6);
  record.snapshot_kind =
      static_cast<std::uint16_t>(sqlite3_column_int(statement, 7));
  record.parent_id = ColumnText(statement, 8);
  record.generation =
      static_cast<std::uint64_t>(sqlite3_column_int64(statement, 9));
  record.lineage = ColumnText(statement, 10);
  return record;
}

constexpr const char* kSnapshotColumns =
    "id, user_id, display_name, size_bytes, sha256, created_at, storage_name,"
    " snapshot_kind, parent_id, generation, lineage";

// RAII：语句用完一定 finalize，异常路径也不例外。
class Statement {
 public:
  Statement() = default;
  ~Statement() {
    if (statement_ != nullptr) {
      sqlite3_finalize(statement_);
    }
  }
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;
  sqlite3_stmt** out() { return &statement_; }
  sqlite3_stmt* get() const { return statement_; }

 private:
  sqlite3_stmt* statement_ = nullptr;
};

}  // namespace

const char* StoreResultName(StoreResult result) {
  switch (result) {
    case StoreResult::kOk:
      return "OK";
    case StoreResult::kNotFound:
      return "NOT_FOUND";
    case StoreResult::kAlreadyExists:
      return "ALREADY_EXISTS";
    case StoreResult::kError:
      return "ERROR";
    case StoreResult::kHasDependents:
      return "HAS_DEPENDENTS";
    case StoreResult::kChainConflict:
      return "CHAIN_CONFLICT";
  }
  return "UNKNOWN";
}

RemoteMetadataStore::RemoteMetadataStore() = default;

RemoteMetadataStore::~RemoteMetadataStore() { Close(); }

std::string RemoteMetadataStore::LastError() const {
  if (database_ == nullptr) {
    return "database is not open";
  }
  const char* message = sqlite3_errmsg(database_);
  return message != nullptr ? std::string(message) : std::string("unknown");
}

bool RemoteMetadataStore::Prepare(const std::string& sql,
                                  sqlite3_stmt** statement,
                                  std::string* error_message) {
  const int code =
      sqlite3_prepare_v2(database_, sql.c_str(), -1, statement, nullptr);
  if (code != SQLITE_OK) {
    if (error_message != nullptr) {
      *error_message = "cannot prepare a statement: " + LastError();
    }
    return false;
  }
  return true;
}

bool RemoteMetadataStore::Execute(const std::string& sql,
                                  std::string* error_message) {
  char* message = nullptr;
  const int code =
      sqlite3_exec(database_, sql.c_str(), nullptr, nullptr, &message);
  if (code != SQLITE_OK) {
    if (error_message != nullptr) {
      *error_message = "sqlite exec failed: " +
                       std::string(message != nullptr ? message : "unknown");
    }
    if (message != nullptr) {
      sqlite3_free(message);
    }
    return false;
  }
  return true;
}

// snapshots 表当前实际有哪些列。迁移只补**缺的**列，因此可以重复执行。
bool RemoteMetadataStore::SnapshotColumnsPresent(
    std::vector<std::string>* columns, std::string* error_message) {
  columns->clear();
  Statement statement;
  if (!Prepare("PRAGMA table_info(snapshots);", statement.out(),
               error_message)) {
    return false;
  }
  for (;;) {
    const int code = sqlite3_step(statement.get());
    if (code == SQLITE_DONE) {
      break;
    }
    if (code != SQLITE_ROW) {
      if (error_message != nullptr) {
        *error_message =
            "cannot read the snapshots table layout: " + LastError();
      }
      return false;
    }
    // table_info 的第 1 列（下标 1）是列名。
    columns->push_back(ColumnText(statement.get(), 1));
  }
  return true;
}

bool RemoteMetadataStore::EnsureSchema(std::string* error_message) {
  for (const char* pragma : kPragmaStatements) {
    if (!Execute(pragma, error_message)) {
      return false;
    }
  }
  int version = 0;
  {
    Statement statement;
    if (!Prepare("PRAGMA user_version;", statement.out(), error_message)) {
      return false;
    }
    if (sqlite3_step(statement.get()) != SQLITE_ROW) {
      if (error_message != nullptr) {
        *error_message = "cannot read the schema version: " + LastError();
      }
      return false;
    }
    version = sqlite3_column_int(statement.get(), 0);
  }
  if (version != 0 && version != kLegacySchemaVersion &&
      version != kSchemaVersion) {
    if (error_message != nullptr) {
      *error_message = "the metadata database has schema version " +
                       std::to_string(version) +
                       ", which this build cannot open (supported: 1 -> 2)";
    }
    return false;
  }

  // 整个迁移是一个事务：建表、补列、写版本号要么全部生效，要么全部回滚。
  if (!Execute("BEGIN IMMEDIATE;", error_message)) {
    return false;
  }
  for (const char* statement : kSchemaStatements) {
    if (!Execute(statement, error_message)) {
      Execute("ROLLBACK;", nullptr);
      return false;
    }
  }
  std::vector<std::string> columns;
  if (!SnapshotColumnsPresent(&columns, error_message)) {
    Execute("ROLLBACK;", nullptr);
    return false;
  }
  // 旧库（version 1）缺这四列；新库在建表时就已经有了。按列名判断而不是按
  // 版本号判断，迁移因此是幂等的：重复执行不会 ALTER 第二次。
  for (const char* name : kChainColumnNames) {
    bool present = false;
    for (const std::string& column : columns) {
      if (column == name) {
        present = true;
        break;
      }
    }
    if (present) {
      continue;
    }
    std::string statement = "ALTER TABLE snapshots ADD COLUMN ";
    if (std::string(name) == "snapshot_kind") {
      statement += "snapshot_kind INTEGER NOT NULL DEFAULT 0;";
    } else if (std::string(name) == "parent_id") {
      statement += "parent_id TEXT NOT NULL DEFAULT '';";
    } else if (std::string(name) == "generation") {
      statement += "generation INTEGER NOT NULL DEFAULT 0;";
    } else {
      statement += "lineage TEXT NOT NULL DEFAULT '';";
    }
    if (!Execute(statement, error_message)) {
      Execute("ROLLBACK;", nullptr);
      return false;
    }
    // 测试接缝：在补完这一列之后立刻失败，用来证明整个迁移（列 + 版本号 +
    // 索引）会被一起回滚，而不是留下"加了一半列"的库。
    if (!fail_migration_after_column_.empty() &&
        fail_migration_after_column_ == name) {
      fail_migration_after_column_.clear();
      if (error_message != nullptr) {
        *error_message = "injected migration failure after column " +
                         std::string(name) + " (test seam)";
      }
      Execute("ROLLBACK;", nullptr);
      return false;
    }
  }
  for (const char* statement : kChainIndexStatements) {
    if (!Execute(statement, error_message)) {
      Execute("ROLLBACK;", nullptr);
      return false;
    }
  }
  if (!Execute("PRAGMA user_version=" + std::to_string(kSchemaVersion) + ";",
               error_message) ||
      !Execute("COMMIT;", error_message)) {
    Execute("ROLLBACK;", nullptr);
    return false;
  }
  return true;
}

// 一个已经存在的库必须有的表。只验证它们**能读**，绝不 CREATE：
// 管理工具在任何模式下都不负责建库、建表或升级 schema。
constexpr const char* kRequiredTables[] = {"users", "snapshots",
                                           "deleted_users"};

bool RemoteMetadataStore::VerifyExistingSchema(std::string* error_message) {
  // 版本不匹配就明确失败，而不是"顺手"把库升级成新 schema：那是一次写操作，
  // 而只读命令完全可能正在 backup-server 运行时执行。
  // PR #21 的迁移只发生在服务端启动路径（EnsureSchema）；管理工具面对一个
  // 还没迁移过的旧库时必须拒绝工作，而不是自己动手升级。
  {
    Statement statement;
    if (!Prepare("PRAGMA user_version;", statement.out(), error_message)) {
      return false;
    }
    if (sqlite3_step(statement.get()) != SQLITE_ROW) {
      if (error_message != nullptr) {
        *error_message = "cannot read the schema version: " + LastError();
      }
      return false;
    }
    const int version = sqlite3_column_int(statement.get(), 0);
    if (version != kSchemaVersion) {
      if (error_message != nullptr) {
        *error_message = "the metadata database has schema version " +
                         std::to_string(version) + ", expected " +
                         std::to_string(kSchemaVersion);
      }
      return false;
    }
  }
  for (const char* table : kRequiredTables) {
    Statement statement;
    const std::string sql = std::string("SELECT count(*) FROM ") + table + ";";
    if (!Prepare(sql, statement.out(), nullptr)) {
      if (error_message != nullptr) {
        *error_message = "the metadata database is missing the " +
                         std::string(table) + " table: " + LastError();
      }
      return false;
    }
    if (sqlite3_step(statement.get()) != SQLITE_ROW) {
      if (error_message != nullptr) {
        *error_message =
            "cannot read the " + std::string(table) + " table: " + LastError();
      }
      return false;
    }
  }
  return true;
}

bool RemoteMetadataStore::Open(const std::string& path,
                               std::string* error_message) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (database_ != nullptr) {
    if (error_message != nullptr) {
      *error_message = "the metadata store is already open";
    }
    return false;
  }
  if (path.empty()) {
    if (error_message != nullptr) {
      *error_message = "the metadata database path is empty";
    }
    return false;
  }
  sqlite3* database = nullptr;
  const int code = sqlite3_open_v2(
      path.c_str(), &database,
      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
      nullptr);
  if (code != SQLITE_OK) {
    const std::string reason =
        database != nullptr ? sqlite3_errmsg(database) : "unknown";
    if (database != nullptr) {
      sqlite3_close(database);
    }
    if (error_message != nullptr) {
      *error_message = "cannot open the metadata database: " + reason;
    }
    return false;
  }
  database_ = database;
  path_ = path;
  if (!EnsureSchema(error_message)) {
    sqlite3_close(database_);
    database_ = nullptr;
    path_.clear();
    return false;
  }
  return true;
}

bool RemoteMetadataStore::OpenExistingReadOnly(const std::string& path,
                                               std::string* error_message) {
  return OpenExistingWithMode(path, /*writable=*/false, error_message);
}

bool RemoteMetadataStore::OpenExistingReadWrite(const std::string& path,
                                                std::string* error_message) {
  return OpenExistingWithMode(path, /*writable=*/true, error_message);
}

bool RemoteMetadataStore::OpenExistingWithMode(const std::string& path,
                                               bool writable,
                                               std::string* error_message) {
  if (path.empty()) {
    if (error_message != nullptr) {
      *error_message = "the metadata database path is empty";
    }
    return false;
  }
  // 先自己看一眼：文件必须存在、必须是普通文件。这样错误信息能说清"是哪个
  // 路径不对"，而不是把 SQLite 的 "unable to open database file" 原样抛出去。
  struct stat info;
  if (::stat(path.c_str(), &info) != 0) {
    if (error_message != nullptr) {
      *error_message = "the metadata database does not exist: " + path;
    }
    return false;
  }
  if (!S_ISREG(info.st_mode)) {
    if (error_message != nullptr) {
      *error_message = "the metadata database is not a regular file: " + path;
    }
    return false;
  }
  std::lock_guard<std::mutex> guard(mutex_);
  if (database_ != nullptr) {
    if (error_message != nullptr) {
      *error_message = "the metadata store is already open";
    }
    return false;
  }
  sqlite3* database = nullptr;
  // 刻意不带 SQLITE_OPEN_CREATE：即使上面那个 stat 与这里之间文件被删掉，
  // 这一次打开也只会失败，不会创建一个空库。
  //
  // 只读命令更是连"可写"都不申请：那条连接在 SQLite 层面就是读者，
  // "这条命令不会写库"因此不是约定，而是连接本身的性质。
  const int flags = (writable ? SQLITE_OPEN_READWRITE : SQLITE_OPEN_READONLY) |
                    SQLITE_OPEN_FULLMUTEX;
  const int code = sqlite3_open_v2(path.c_str(), &database, flags, nullptr);
  if (code != SQLITE_OK) {
    const std::string reason =
        database != nullptr ? sqlite3_errmsg(database) : "unknown";
    if (database != nullptr) {
      sqlite3_close(database);
    }
    if (error_message != nullptr) {
      *error_message =
          "cannot open the metadata database " + path + ": " + reason;
    }
    return false;
  }
  database_ = database;
  path_ = path;
  // 下面两条都是**连接级** pragma，不写数据库文件本身。journal_mode 才是写
  // 操作（它会把库改成 WAL 并写进文件头），这里刻意不设：管理工具不修改它
  // 打开的库。busy_timeout 让"服务端正在写"时读到的是等待，而不是立刻
  // SQLITE_BUSY；query_only 是只读路径的第二道锁——以后就算有人在这一层手滑
  // 写了 SQL，也会被直接拒绝，而不是悄悄改掉数据。
  //
  // foreign_keys 只在可写连接上打开：破坏性删除要按"先 snapshots 后 users"
  // 的顺序做，这条外键就是那个顺序的护栏，和服务端连接上的语义保持一致。
  std::string pragma_error;
  if (!Execute("PRAGMA busy_timeout=5000;", &pragma_error) ||
      !Execute(writable ? "PRAGMA foreign_keys=ON;" : "PRAGMA query_only=ON;",
               &pragma_error)) {
    sqlite3_close(database_);
    database_ = nullptr;
    path_.clear();
    if (error_message != nullptr) {
      *error_message = pragma_error;
    }
    return false;
  }
  // 只验证，不建表：管理工具不是"初始化实例"的地方。
  if (!VerifyExistingSchema(error_message)) {
    sqlite3_close(database_);
    database_ = nullptr;
    path_.clear();
    return false;
  }
  return true;
}

void RemoteMetadataStore::Close() {
  std::lock_guard<std::mutex> guard(mutex_);
  if (database_ != nullptr) {
    sqlite3_close(database_);
    database_ = nullptr;
  }
  path_.clear();
}

StoreResult RemoteMetadataStore::CreateUser(const std::string& username,
                                            const PasswordRecord& password,
                                            std::int64_t created_at,
                                            std::int64_t* out_user_id,
                                            std::string* error_message) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (database_ == nullptr) {
    if (error_message != nullptr) {
      *error_message = "the metadata store is not open";
    }
    return StoreResult::kError;
  }
  if (password.salt.size() != kPasswordSaltBytes ||
      password.hash.size() != kPasswordHashBytes) {
    if (error_message != nullptr) {
      *error_message = "refusing to store a malformed password record";
    }
    return StoreResult::kError;
  }
  // user id 显式分配：下一个 id 严格大于 users 与 deleted_users 里的最大值。
  // 用 INSERT 的隐式 rowid 会在"删掉最大的那一行"之后把 id 还给下一个注册者，
  // 于是一个注销前签发的 token（签名仍然有效）就命中了别人的新账户。
  // 整个分配 + 插入在同一个互斥区间里，进程内因此没有竞态。
  std::int64_t next_user_id = 0;
  {
    Statement allocate;
    if (!Prepare(
            "SELECT MAX(users_max, deleted_max) + 1 FROM ("
            "  SELECT COALESCE((SELECT MAX(id) FROM users), 0) AS users_max,"
            "         COALESCE((SELECT MAX(id) FROM deleted_users), 0)"
            "           AS deleted_max);",
            allocate.out(), error_message)) {
      return StoreResult::kError;
    }
    if (sqlite3_step(allocate.get()) != SQLITE_ROW) {
      if (error_message != nullptr) {
        *error_message = "cannot allocate the next user id: " + LastError();
      }
      return StoreResult::kError;
    }
    next_user_id = sqlite3_column_int64(allocate.get(), 0);
  }
  Statement statement;
  if (!Prepare("INSERT INTO users"
               " (id, username, password_salt, password_hash,"
               "  password_iterations, created_at) VALUES (?, ?, ?, ?, ?, ?);",
               statement.out(), error_message)) {
    return StoreResult::kError;
  }
  sqlite3_bind_int64(statement.get(), 1,
                     static_cast<sqlite3_int64>(next_user_id));
  sqlite3_bind_text(statement.get(), 2, username.c_str(),
                    static_cast<int>(username.size()), SQLITE_TRANSIENT);
  sqlite3_bind_blob(statement.get(), 3, password.salt.data(),
                    static_cast<int>(password.salt.size()), SQLITE_TRANSIENT);
  sqlite3_bind_blob(statement.get(), 4, password.hash.data(),
                    static_cast<int>(password.hash.size()), SQLITE_TRANSIENT);
  sqlite3_bind_int64(statement.get(), 5,
                     static_cast<sqlite3_int64>(password.iterations));
  sqlite3_bind_int64(statement.get(), 6,
                     static_cast<sqlite3_int64>(created_at));
  const int code = sqlite3_step(statement.get());
  if (code == SQLITE_CONSTRAINT) {
    return StoreResult::kAlreadyExists;
  }
  if (code != SQLITE_DONE) {
    if (error_message != nullptr) {
      *error_message = "cannot insert the user row: " + LastError();
    }
    return StoreResult::kError;
  }
  if (out_user_id != nullptr) {
    *out_user_id = next_user_id;
  }
  return StoreResult::kOk;
}

StoreResult RemoteMetadataStore::FindUser(const std::string& username,
                                          RemoteUserRecord* out,
                                          std::string* error_message) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (database_ == nullptr) {
    if (error_message != nullptr) {
      *error_message = "the metadata store is not open";
    }
    return StoreResult::kError;
  }
  Statement statement;
  if (!Prepare(
          "SELECT id, username, password_salt, password_hash,"
          " password_iterations, created_at FROM users WHERE username = ?;",
          statement.out(), error_message)) {
    return StoreResult::kError;
  }
  sqlite3_bind_text(statement.get(), 1, username.c_str(),
                    static_cast<int>(username.size()), SQLITE_TRANSIENT);
  const int code = sqlite3_step(statement.get());
  if (code == SQLITE_DONE) {
    return StoreResult::kNotFound;
  }
  if (code != SQLITE_ROW) {
    if (error_message != nullptr) {
      *error_message = "cannot read the user row: " + LastError();
    }
    return StoreResult::kError;
  }
  if (out != nullptr) {
    out->user_id = sqlite3_column_int64(statement.get(), 0);
    out->username = ColumnText(statement.get(), 1);
    out->password.salt = ColumnBlob(statement.get(), 2);
    out->password.hash = ColumnBlob(statement.get(), 3);
    out->password.iterations =
        static_cast<std::uint32_t>(sqlite3_column_int64(statement.get(), 4));
    out->created_at = sqlite3_column_int64(statement.get(), 5);
  }
  return StoreResult::kOk;
}

StoreResult RemoteMetadataStore::FindUserById(std::int64_t user_id,
                                              RemoteUserRecord* out,
                                              std::string* error_message) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (database_ == nullptr) {
    if (error_message != nullptr) {
      *error_message = "the metadata store is not open";
    }
    return StoreResult::kError;
  }
  Statement statement;
  if (!Prepare("SELECT id, username, password_salt, password_hash,"
               " password_iterations, created_at FROM users WHERE id = ?;",
               statement.out(), error_message)) {
    return StoreResult::kError;
  }
  sqlite3_bind_int64(statement.get(), 1, static_cast<sqlite3_int64>(user_id));
  const int code = sqlite3_step(statement.get());
  if (code == SQLITE_DONE) {
    return StoreResult::kNotFound;
  }
  if (code != SQLITE_ROW) {
    if (error_message != nullptr) {
      *error_message = "cannot read the user row: " + LastError();
    }
    return StoreResult::kError;
  }
  if (out != nullptr) {
    out->user_id = sqlite3_column_int64(statement.get(), 0);
    out->username = ColumnText(statement.get(), 1);
    out->password.salt = ColumnBlob(statement.get(), 2);
    out->password.hash = ColumnBlob(statement.get(), 3);
    out->password.iterations =
        static_cast<std::uint32_t>(sqlite3_column_int64(statement.get(), 4));
    out->created_at = sqlite3_column_int64(statement.get(), 5);
  }
  return StoreResult::kOk;
}

StoreResult RemoteMetadataStore::InsertSnapshot(
    const RemoteSnapshotRecord& record, std::string* error_message) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (database_ == nullptr) {
    if (error_message != nullptr) {
      *error_message = "the metadata store is not open";
    }
    return StoreResult::kError;
  }
  if (fail_next_insert_) {
    fail_next_insert_ = false;
    if (error_message != nullptr) {
      *error_message = "injected insert failure (test seam)";
    }
    return StoreResult::kError;
  }
  // ---- 链的边界条件：与 INSERT 在同一个互斥区间里重新校验 ----
  //
  // 服务端在 UPLOAD_BEGIN 时验过一次父，但写入发生在 UPLOAD_END——中间这段
  // 时间里父可能被另一个连接删掉，或者另一个连接给它挂了孩子。只有把校验放到
  // 这里（与 INSERT 同一个临界区、同一个事务），才能保证：
  //   * 不会出现"父已删除、子还在"的孤儿（删除侧在同一把锁里查子节点）；
  //   * 同一个父最多只有一个活着的孩子（线性链，不允许分叉）；
  //   * 不会存下代数控过、本地引擎恢复不了的快照。
  if (record.snapshot_kind ==
      static_cast<std::uint16_t>(SnapshotKind::kIncremental)) {
    if (record.parent_id.empty() || record.generation == 0) {
      if (error_message != nullptr) {
        *error_message =
            "an incremental snapshot needs a parent and a"
            " generation above zero";
      }
      return StoreResult::kChainConflict;
    }
    if (record.generation > kMaxRemoteChainGeneration) {
      if (error_message != nullptr) {
        *error_message = "generation " + std::to_string(record.generation) +
                         " is beyond the restorable chain limit of " +
                         std::to_string(kMaxRemoteChainGeneration);
      }
      return StoreResult::kChainConflict;
    }
    if (!Execute("BEGIN IMMEDIATE;", error_message)) {
      return StoreResult::kError;
    }
    std::uint16_t parent_kind = 0;
    std::uint64_t parent_generation = 0;
    std::string parent_lineage;
    {
      Statement parent;
      if (!Prepare("SELECT snapshot_kind, generation, lineage FROM snapshots"
                   " WHERE id = ? AND user_id = ?;",
                   parent.out(), error_message)) {
        Execute("ROLLBACK;", nullptr);
        return StoreResult::kError;
      }
      sqlite3_bind_text(parent.get(), 1, record.parent_id.c_str(),
                        static_cast<int>(record.parent_id.size()),
                        SQLITE_TRANSIENT);
      sqlite3_bind_int64(parent.get(), 2,
                         static_cast<sqlite3_int64>(record.user_id));
      const int parent_code = sqlite3_step(parent.get());
      if (parent_code == SQLITE_DONE) {
        // 父不存在（或不属于这个用户，两者故意同一个答案）。
        Execute("ROLLBACK;", nullptr);
        if (error_message != nullptr) {
          *error_message = "the parent snapshot no longer exists";
        }
        return StoreResult::kNotFound;
      }
      if (parent_code != SQLITE_ROW) {
        if (error_message != nullptr) {
          *error_message = "cannot read the parent snapshot: " + LastError();
        }
        Execute("ROLLBACK;", nullptr);
        return StoreResult::kError;
      }
      parent_kind =
          static_cast<std::uint16_t>(sqlite3_column_int(parent.get(), 0));
      parent_generation =
          static_cast<std::uint64_t>(sqlite3_column_int64(parent.get(), 1));
      parent_lineage = ColumnText(parent.get(), 2);
    }
    if (parent_kind != static_cast<std::uint16_t>(SnapshotKind::kFull) &&
        parent_kind != static_cast<std::uint16_t>(SnapshotKind::kIncremental)) {
      Execute("ROLLBACK;", nullptr);
      if (error_message != nullptr) {
        *error_message = "the parent row has an unknown snapshot kind";
      }
      return StoreResult::kChainConflict;
    }
    if (parent_lineage != record.lineage) {
      Execute("ROLLBACK;", nullptr);
      if (error_message != nullptr) {
        *error_message = "the parent belongs to a different lineage";
      }
      return StoreResult::kChainConflict;
    }
    if (parent_generation + 1 != record.generation) {
      Execute("ROLLBACK;", nullptr);
      if (error_message != nullptr) {
        *error_message =
            "the generation must be the parent generation plus one";
      }
      return StoreResult::kChainConflict;
    }
    {
      // 线性链：父已经有孩子就不许再挂一个。
      Statement children;
      if (!Prepare("SELECT count(*) FROM snapshots"
                   " WHERE user_id = ? AND parent_id = ?;",
                   children.out(), error_message)) {
        Execute("ROLLBACK;", nullptr);
        return StoreResult::kError;
      }
      sqlite3_bind_int64(children.get(), 1,
                         static_cast<sqlite3_int64>(record.user_id));
      sqlite3_bind_text(children.get(), 2, record.parent_id.c_str(),
                        static_cast<int>(record.parent_id.size()),
                        SQLITE_TRANSIENT);
      if (sqlite3_step(children.get()) != SQLITE_ROW) {
        if (error_message != nullptr) {
          *error_message = "cannot count the children: " + LastError();
        }
        Execute("ROLLBACK;", nullptr);
        return StoreResult::kError;
      }
      if (sqlite3_column_int64(children.get(), 0) > 0) {
        Execute("ROLLBACK;", nullptr);
        if (error_message != nullptr) {
          *error_message =
              "the parent already has a child; this build keeps"
              " remote lineages linear";
        }
        return StoreResult::kChainConflict;
      }
    }
  } else {
    // 完整快照：必须是链根。
    if (!record.parent_id.empty() || record.generation != 0) {
      if (error_message != nullptr) {
        *error_message =
            "a full snapshot must not declare a parent and must"
            " have generation zero";
      }
      return StoreResult::kChainConflict;
    }
  }

  Statement statement;
  if (!Prepare("INSERT INTO snapshots"
               " (id, user_id, display_name, size_bytes, sha256, created_at,"
               "  storage_name, snapshot_kind, parent_id, generation, lineage)"
               " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);",
               statement.out(), error_message)) {
    if (record.snapshot_kind ==
        static_cast<std::uint16_t>(SnapshotKind::kIncremental)) {
      Execute("ROLLBACK;", nullptr);
    }
    return StoreResult::kError;
  }
  sqlite3_bind_text(statement.get(), 1, record.snapshot_id.c_str(),
                    static_cast<int>(record.snapshot_id.size()),
                    SQLITE_TRANSIENT);
  sqlite3_bind_int64(statement.get(), 2,
                     static_cast<sqlite3_int64>(record.user_id));
  sqlite3_bind_text(statement.get(), 3, record.display_name.c_str(),
                    static_cast<int>(record.display_name.size()),
                    SQLITE_TRANSIENT);
  sqlite3_bind_int64(statement.get(), 4,
                     static_cast<sqlite3_int64>(record.size_bytes));
  sqlite3_bind_text(statement.get(), 5, record.sha256.c_str(),
                    static_cast<int>(record.sha256.size()), SQLITE_TRANSIENT);
  sqlite3_bind_int64(statement.get(), 6,
                     static_cast<sqlite3_int64>(record.created_at));
  sqlite3_bind_text(statement.get(), 7, record.storage_name.c_str(),
                    static_cast<int>(record.storage_name.size()),
                    SQLITE_TRANSIENT);
  sqlite3_bind_int(statement.get(), 8, static_cast<int>(record.snapshot_kind));
  sqlite3_bind_text(statement.get(), 9, record.parent_id.c_str(),
                    static_cast<int>(record.parent_id.size()),
                    SQLITE_TRANSIENT);
  sqlite3_bind_int64(statement.get(), 10,
                     static_cast<sqlite3_int64>(record.generation));
  sqlite3_bind_text(statement.get(), 11, record.lineage.c_str(),
                    static_cast<int>(record.lineage.size()), SQLITE_TRANSIENT);
  const int code = sqlite3_step(statement.get());
  if (code != SQLITE_DONE) {
    if (record.snapshot_kind ==
        static_cast<std::uint16_t>(SnapshotKind::kIncremental)) {
      Execute("ROLLBACK;", nullptr);
    }
    if (code == SQLITE_CONSTRAINT) {
      return StoreResult::kAlreadyExists;
    }
    if (error_message != nullptr) {
      *error_message = "cannot insert the snapshot row: " + LastError();
    }
    return StoreResult::kError;
  }
  if (record.snapshot_kind ==
      static_cast<std::uint16_t>(SnapshotKind::kIncremental)) {
    // 校验与写入在同一个事务里：提交失败也要如实报错。
    if (!Execute("COMMIT;", error_message)) {
      Execute("ROLLBACK;", nullptr);
      return StoreResult::kError;
    }
  }
  return StoreResult::kOk;
}

StoreResult RemoteMetadataStore::ListSnapshots(
    std::int64_t user_id, std::vector<RemoteSnapshotRecord>* out,
    std::string* error_message) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (database_ == nullptr) {
    if (error_message != nullptr) {
      *error_message = "the metadata store is not open";
    }
    return StoreResult::kError;
  }
  Statement statement;
  const std::string sql = std::string("SELECT ") + kSnapshotColumns +
                          " FROM snapshots WHERE user_id = ?"
                          " ORDER BY created_at, id;";
  if (!Prepare(sql, statement.out(), error_message)) {
    return StoreResult::kError;
  }
  sqlite3_bind_int64(statement.get(), 1, static_cast<sqlite3_int64>(user_id));
  for (;;) {
    const int code = sqlite3_step(statement.get());
    if (code == SQLITE_DONE) {
      break;
    }
    if (code != SQLITE_ROW) {
      if (error_message != nullptr) {
        *error_message = "cannot read the snapshot rows: " + LastError();
      }
      return StoreResult::kError;
    }
    if (out != nullptr) {
      out->push_back(ReadSnapshotRow(statement.get()));
    }
  }
  return StoreResult::kOk;
}

StoreResult RemoteMetadataStore::FindSnapshot(std::int64_t user_id,
                                              const std::string& snapshot_id,
                                              RemoteSnapshotRecord* out,
                                              std::string* error_message) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (database_ == nullptr) {
    if (error_message != nullptr) {
      *error_message = "the metadata store is not open";
    }
    return StoreResult::kError;
  }
  Statement statement;
  const std::string sql = std::string("SELECT ") + kSnapshotColumns +
                          " FROM snapshots WHERE id = ? AND user_id = ?;";
  if (!Prepare(sql, statement.out(), error_message)) {
    return StoreResult::kError;
  }
  sqlite3_bind_text(statement.get(), 1, snapshot_id.c_str(),
                    static_cast<int>(snapshot_id.size()), SQLITE_TRANSIENT);
  sqlite3_bind_int64(statement.get(), 2, static_cast<sqlite3_int64>(user_id));
  const int code = sqlite3_step(statement.get());
  if (code == SQLITE_DONE) {
    // 别人的快照与不存在的快照在这里是同一个答案：不允许按 id 探测存在性。
    return StoreResult::kNotFound;
  }
  if (code != SQLITE_ROW) {
    if (error_message != nullptr) {
      *error_message = "cannot read the snapshot row: " + LastError();
    }
    return StoreResult::kError;
  }
  if (out != nullptr) {
    *out = ReadSnapshotRow(statement.get());
  }
  return StoreResult::kOk;
}

StoreResult RemoteMetadataStore::DeleteSnapshot(std::int64_t user_id,
                                                const std::string& snapshot_id,
                                                RemoteSnapshotRecord* removed,
                                                std::string* error_message) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (database_ == nullptr) {
    if (error_message != nullptr) {
      *error_message = "the metadata store is not open";
    }
    return StoreResult::kError;
  }
  // 先查再删，两步在同一个互斥区间里，所以不存在"查到 A 的行、删掉 B 的行"
  // 这种窗口；返回的 storage_name 一定属于刚被删掉的那一行。
  RemoteSnapshotRecord existing;
  {
    Statement select;
    const std::string sql = std::string("SELECT ") + kSnapshotColumns +
                            " FROM snapshots WHERE id = ? AND user_id = ?;";
    if (!Prepare(sql, select.out(), error_message)) {
      return StoreResult::kError;
    }
    sqlite3_bind_text(select.get(), 1, snapshot_id.c_str(),
                      static_cast<int>(snapshot_id.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(select.get(), 2, static_cast<sqlite3_int64>(user_id));
    const int code = sqlite3_step(select.get());
    if (code == SQLITE_DONE) {
      return StoreResult::kNotFound;
    }
    if (code != SQLITE_ROW) {
      if (error_message != nullptr) {
        *error_message = "cannot read the snapshot row: " + LastError();
      }
      return StoreResult::kError;
    }
    existing = ReadSnapshotRow(select.get());
  }
  // PR #21：依赖感知删除的**最后一道闸门**。调用方（RemoteMaintenance）在动
  // 磁盘之前就已经查过一次子节点；这里再查一次，是为了让"绕过调用方直接删"
  // 也不可能造成断链——删除一个还有子节点的快照会让那些子快照永远无法恢复。
  {
    Statement children;
    if (!Prepare("SELECT count(*) FROM snapshots"
                 " WHERE user_id = ? AND parent_id = ?;",
                 children.out(), error_message)) {
      return StoreResult::kError;
    }
    sqlite3_bind_int64(children.get(), 1, static_cast<sqlite3_int64>(user_id));
    sqlite3_bind_text(children.get(), 2, snapshot_id.c_str(),
                      static_cast<int>(snapshot_id.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(children.get()) != SQLITE_ROW) {
      if (error_message != nullptr) {
        *error_message = "cannot count the dependent snapshots: " + LastError();
      }
      return StoreResult::kError;
    }
    const sqlite3_int64 dependents = sqlite3_column_int64(children.get(), 0);
    if (dependents > 0) {
      if (error_message != nullptr) {
        *error_message = "snapshot " + snapshot_id + " still has " +
                         std::to_string(dependents) +
                         " dependent incremental snapshot(s); delete the"
                         " descendants first";
      }
      return StoreResult::kHasDependents;
    }
  }
  Statement remove;
  if (!Prepare("DELETE FROM snapshots WHERE id = ? AND user_id = ?;",
               remove.out(), error_message)) {
    return StoreResult::kError;
  }
  sqlite3_bind_text(remove.get(), 1, snapshot_id.c_str(),
                    static_cast<int>(snapshot_id.size()), SQLITE_TRANSIENT);
  sqlite3_bind_int64(remove.get(), 2, static_cast<sqlite3_int64>(user_id));
  const int code = sqlite3_step(remove.get());
  if (code != SQLITE_DONE) {
    if (error_message != nullptr) {
      *error_message = "cannot delete the snapshot row: " + LastError();
    }
    return StoreResult::kError;
  }
  if (sqlite3_changes(database_) != 1) {
    if (error_message != nullptr) {
      *error_message = "the snapshot row disappeared during the delete";
    }
    return StoreResult::kError;
  }
  if (removed != nullptr) {
    *removed = existing;
  }
  return StoreResult::kOk;
}

StoreResult RemoteMetadataStore::CountSnapshotChildren(
    std::int64_t user_id, const std::string& snapshot_id, std::uint64_t* out,
    std::string* error_message) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (database_ == nullptr) {
    if (error_message != nullptr) {
      *error_message = "the metadata store is not open";
    }
    return StoreResult::kError;
  }
  Statement statement;
  if (!Prepare("SELECT count(*) FROM snapshots"
               " WHERE user_id = ? AND parent_id = ?;",
               statement.out(), error_message)) {
    return StoreResult::kError;
  }
  sqlite3_bind_int64(statement.get(), 1, static_cast<sqlite3_int64>(user_id));
  sqlite3_bind_text(statement.get(), 2, snapshot_id.c_str(),
                    static_cast<int>(snapshot_id.size()), SQLITE_TRANSIENT);
  if (sqlite3_step(statement.get()) != SQLITE_ROW) {
    if (error_message != nullptr) {
      *error_message = "cannot count the dependent snapshots: " + LastError();
    }
    return StoreResult::kError;
  }
  if (out != nullptr) {
    *out = static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 0));
  }
  return StoreResult::kOk;
}

StoreResult RemoteMetadataStore::CountSnapshots(std::int64_t user_id,
                                                std::uint64_t* out,
                                                std::string* error_message) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (database_ == nullptr) {
    if (error_message != nullptr) {
      *error_message = "the metadata store is not open";
    }
    return StoreResult::kError;
  }
  Statement statement;
  if (!Prepare("SELECT COUNT(*) FROM snapshots WHERE user_id = ?;",
               statement.out(), error_message)) {
    return StoreResult::kError;
  }
  sqlite3_bind_int64(statement.get(), 1, static_cast<sqlite3_int64>(user_id));
  const int code = sqlite3_step(statement.get());
  if (code != SQLITE_ROW) {
    if (error_message != nullptr) {
      *error_message = "cannot count the snapshot rows: " + LastError();
    }
    return StoreResult::kError;
  }
  if (out != nullptr) {
    *out = static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 0));
  }
  return StoreResult::kOk;
}

StoreResult RemoteMetadataStore::ListUsers(std::vector<RemoteUserSummary>* out,
                                           std::string* error_message) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (database_ == nullptr) {
    if (error_message != nullptr) {
      *error_message = "the metadata store is not open";
    }
    return StoreResult::kError;
  }
  // 列是逐个写出来的：口令相关的列一次都不出现在这条 SQL 里。管理工具
  // 因此不是"记得不要打印 hash"，而是根本拿不到 hash。
  Statement statement;
  if (!Prepare("SELECT u.id, u.username, u.created_at, COUNT(s.id),"
               " COALESCE(SUM(s.size_bytes), 0) FROM users u"
               " LEFT JOIN snapshots s ON s.user_id = u.id"
               " GROUP BY u.id, u.username, u.created_at ORDER BY u.id;",
               statement.out(), error_message)) {
    return StoreResult::kError;
  }
  std::vector<RemoteUserSummary> parsed;
  for (;;) {
    const int code = sqlite3_step(statement.get());
    if (code == SQLITE_DONE) {
      break;
    }
    if (code != SQLITE_ROW) {
      if (error_message != nullptr) {
        *error_message = "cannot read the user rows: " + LastError();
      }
      return StoreResult::kError;
    }
    RemoteUserSummary summary;
    summary.user_id = sqlite3_column_int64(statement.get(), 0);
    summary.username = ColumnText(statement.get(), 1);
    summary.created_at = sqlite3_column_int64(statement.get(), 2);
    summary.snapshot_count =
        static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 3));
    summary.total_bytes =
        static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 4));
    parsed.push_back(summary);
  }
  if (out != nullptr) {
    *out = parsed;
  }
  return StoreResult::kOk;
}

StoreResult RemoteMetadataStore::StorageOverview(RemoteStorageOverview* out,
                                                 std::string* error_message) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (database_ == nullptr) {
    if (error_message != nullptr) {
      *error_message = "the metadata store is not open";
    }
    return StoreResult::kError;
  }
  RemoteStorageOverview overview;
  {
    Statement statement;
    if (!Prepare("SELECT COUNT(*) FROM users;", statement.out(),
                 error_message) ||
        sqlite3_step(statement.get()) != SQLITE_ROW) {
      if (error_message != nullptr && error_message->empty()) {
        *error_message = "cannot count the user rows: " + LastError();
      }
      return StoreResult::kError;
    }
    overview.user_count =
        static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 0));
  }
  {
    Statement statement;
    if (!Prepare(
            "SELECT COUNT(*), COALESCE(SUM(size_bytes), 0) FROM snapshots;",
            statement.out(), error_message) ||
        sqlite3_step(statement.get()) != SQLITE_ROW) {
      if (error_message != nullptr && error_message->empty()) {
        *error_message = "cannot aggregate the snapshot rows: " + LastError();
      }
      return StoreResult::kError;
    }
    overview.snapshot_count =
        static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 0));
    overview.total_bytes =
        static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 1));
  }
  {
    Statement statement;
    if (!Prepare("SELECT COUNT(*) FROM deleted_users;", statement.out(),
                 error_message) ||
        sqlite3_step(statement.get()) != SQLITE_ROW) {
      if (error_message != nullptr && error_message->empty()) {
        *error_message = "cannot count the deleted user rows: " + LastError();
      }
      return StoreResult::kError;
    }
    overview.deleted_user_count =
        static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), 0));
  }
  if (out != nullptr) {
    *out = overview;
  }
  return StoreResult::kOk;
}

StoreResult RemoteMetadataStore::DeleteUser(std::int64_t user_id,
                                            std::uint64_t* removed_snapshots,
                                            std::uint64_t* removed_bytes,
                                            std::string* error_message) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (database_ == nullptr) {
    if (error_message != nullptr) {
      *error_message = "the metadata store is not open";
    }
    return StoreResult::kError;
  }
  if (!Execute("BEGIN IMMEDIATE;", error_message)) {
    return StoreResult::kError;
  }
  const auto rollback = [this]() { Execute("ROLLBACK;", nullptr); };

  std::uint64_t snapshots = 0;
  std::uint64_t bytes = 0;
  {
    Statement select;
    if (!Prepare("SELECT COUNT(*), COALESCE(SUM(size_bytes), 0) FROM snapshots"
                 " WHERE user_id = ?;",
                 select.out(), error_message)) {
      rollback();
      return StoreResult::kError;
    }
    sqlite3_bind_int64(select.get(), 1, static_cast<sqlite3_int64>(user_id));
    if (sqlite3_step(select.get()) != SQLITE_ROW) {
      if (error_message != nullptr) {
        *error_message = "cannot aggregate the snapshot rows: " + LastError();
      }
      rollback();
      return StoreResult::kError;
    }
    snapshots =
        static_cast<std::uint64_t>(sqlite3_column_int64(select.get(), 0));
    bytes = static_cast<std::uint64_t>(sqlite3_column_int64(select.get(), 1));
  }
  {
    Statement remove;
    if (!Prepare("DELETE FROM snapshots WHERE user_id = ?;", remove.out(),
                 error_message)) {
      rollback();
      return StoreResult::kError;
    }
    sqlite3_bind_int64(remove.get(), 1, static_cast<sqlite3_int64>(user_id));
    if (sqlite3_step(remove.get()) != SQLITE_DONE) {
      if (error_message != nullptr) {
        *error_message = "cannot delete the snapshot rows: " + LastError();
      }
      rollback();
      return StoreResult::kError;
    }
  }
  {
    Statement remove;
    if (!Prepare("DELETE FROM users WHERE id = ?;", remove.out(),
                 error_message)) {
      rollback();
      return StoreResult::kError;
    }
    sqlite3_bind_int64(remove.get(), 1, static_cast<sqlite3_int64>(user_id));
    if (sqlite3_step(remove.get()) != SQLITE_DONE) {
      if (error_message != nullptr) {
        *error_message = "cannot delete the user row: " + LastError();
      }
      rollback();
      return StoreResult::kError;
    }
    if (sqlite3_changes(database_) != 1) {
      if (error_message != nullptr) {
        *error_message = "no such user row";
      }
      // 账户不在了（或者从来不存在）：整个事务回滚，调用方按 NOT_FOUND 处理，
      // 磁盘上的隔离动作也会被撤回。
      rollback();
      return StoreResult::kNotFound;
    }
  }
  {
    // 墓碑：id 从此不再被分配。时间戳只是给人看的审计信息。
    Statement mark;
    if (!Prepare("INSERT OR REPLACE INTO deleted_users (id, deleted_at)"
                 " VALUES (?, ?);",
                 mark.out(), error_message)) {
      rollback();
      return StoreResult::kError;
    }
    sqlite3_bind_int64(mark.get(), 1, static_cast<sqlite3_int64>(user_id));
    sqlite3_bind_int64(mark.get(), 2,
                       static_cast<sqlite3_int64>(std::time(nullptr)));
    if (sqlite3_step(mark.get()) != SQLITE_DONE) {
      if (error_message != nullptr) {
        *error_message =
            "cannot write the deleted user tombstone: " + LastError();
      }
      rollback();
      return StoreResult::kError;
    }
  }
  if (fail_next_delete_user_) {
    fail_next_delete_user_ = false;
    if (error_message != nullptr) {
      *error_message = "injected delete failure (test seam)";
    }
    rollback();
    return StoreResult::kError;
  }
  if (!Execute("COMMIT;", error_message)) {
    rollback();
    return StoreResult::kError;
  }
  if (removed_snapshots != nullptr) {
    *removed_snapshots = snapshots;
  }
  if (removed_bytes != nullptr) {
    *removed_bytes = bytes;
  }
  return StoreResult::kOk;
}

}  // namespace net
}  // namespace backupproject
