// src/network/remote_metadata_store.cpp

#include "remote_metadata_store.h"

#include <sqlite3.h>

#include <cstring>
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
  Statement statement;
  if (!Prepare("INSERT INTO users"
               " (username, password_salt, password_hash, password_iterations,"
               "  created_at) VALUES (?, ?, ?, ?, ?);",
               statement.out(), error_message)) {
    return StoreResult::kError;
  }
  sqlite3_bind_text(statement.get(), 1, username.c_str(),
                    static_cast<int>(username.size()), SQLITE_TRANSIENT);
  sqlite3_bind_blob(statement.get(), 2, password.salt.data(),
                    static_cast<int>(password.salt.size()), SQLITE_TRANSIENT);
  sqlite3_bind_blob(statement.get(), 3, password.hash.data(),
                    static_cast<int>(password.hash.size()), SQLITE_TRANSIENT);
  sqlite3_bind_int64(statement.get(), 4,
                     static_cast<sqlite3_int64>(password.iterations));
  sqlite3_bind_int64(statement.get(), 5,
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
    *out_user_id =
        static_cast<std::int64_t>(sqlite3_last_insert_rowid(database_));
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

}  // namespace net
}  // namespace backupproject
