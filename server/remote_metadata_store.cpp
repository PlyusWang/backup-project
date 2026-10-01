// src/network/remote_metadata_store.cpp

#include "remote_metadata_store.h"

#include <sqlite3.h>

#include <cstring>
#include <ctime>
#include <utility>

namespace backupproject {
namespace net {
namespace {

// WAL 让读写并发不至于互相阻塞；synchronous=FULL 保证 commit 之后掉电也还在。
// foundation 版本的写入量很小，用安全性换那点吞吐不值得反过来做。
constexpr const char* kPragmaStatements[] = {
    "PRAGMA journal_mode=WAL;",
    "PRAGMA synchronous=FULL;",
    "PRAGMA foreign_keys=ON;",
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

constexpr int kSchemaVersion = 1;

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
  return record;
}

constexpr const char* kSnapshotColumns =
    "id, user_id, display_name, size_bytes, sha256, created_at, storage_name";

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

bool RemoteMetadataStore::EnsureSchema(std::string* error_message) {
  for (const char* pragma : kPragmaStatements) {
    if (!Execute(pragma, error_message)) {
      return false;
    }
  }
  if (!Execute("BEGIN IMMEDIATE;", error_message)) {
    return false;
  }
  for (const char* statement : kSchemaStatements) {
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
  Statement statement;
  if (!Prepare("INSERT INTO snapshots"
               " (id, user_id, display_name, size_bytes, sha256, created_at,"
               "  storage_name) VALUES (?, ?, ?, ?, ?, ?, ?);",
               statement.out(), error_message)) {
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
  const int code = sqlite3_step(statement.get());
  if (code == SQLITE_CONSTRAINT) {
    return StoreResult::kAlreadyExists;
  }
  if (code != SQLITE_DONE) {
    if (error_message != nullptr) {
      *error_message = "cannot insert the snapshot row: " + LastError();
    }
    return StoreResult::kError;
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

StoreResult RemoteMetadataStore::ListUsers(
    std::vector<RemoteUserSummary>* out, std::string* error_message) {
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
  if (!Prepare(
          "SELECT u.id, u.username, u.created_at, COUNT(s.id),"
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
    if (!Prepare("SELECT COUNT(*), COALESCE(SUM(size_bytes), 0) FROM snapshots;",
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
        *error_message = "cannot write the deleted user tombstone: " +
                         LastError();
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
