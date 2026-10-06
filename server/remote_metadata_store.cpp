// server/remote_metadata_store.cpp

// 实现层职责：把 RemoteMetadataStore 的每个公开方法翻译成一条或一组 SQLite
// 语句，并负责"元数据与磁盘 blob 的一致性"里属于元数据的那一半。
//
// 本文件**不负责**：
//   * 生成或校验 storage_name —— 磁盘文件名由服务端上层决定，这里只当字符串
//     存取；
//   * blob 的写入与删除顺序 —— 那是 remote_server / remote_maintenance 的
//     publish / quarantine 流程，这里只保证行级事实的原子性；
//   * 口令的哈希与比对 —— salt / hash / iterations 原样存取，校验在
//     remote_auth.cpp；
//   * 协议编解码 —— StoreResult 到协议状态码的映射在 remote_server.cpp。
//
// 数据流：客户端 UPLOAD_END -> 上层构造 RemoteSnapshotRecord -> 本文件的
// InsertSnapshot（链约束的权威校验点）-> snapshots 表；列表 / 详情 / 删除
// 反向读回同一组列。
//
// 关键不变量：
//   1. 列顺序就是磁盘布局。kSnapshotColumns 的书写顺序、ReadSnapshotRow 的
//      下标、CREATE TABLE 与 ALTER TABLE 的列序三者必须一致；只改其中一处不会
//      编译报错，只会让读回来的字段整体错位。
//   2. SQL 一律预编译加绑定参数。全文没有把**值**拼进 SQL 文本；拼接只用于
//      编译期常量（列名、表名、pragma）。
//   3. 实例方法必须在 Open 成功之后调用；未打开时统一返回 kError 并写明
//      "the metadata store is not open"，绝不空指针解引用。
//   4. 公开方法在入口取同一把 mutex_，因此进程内的"查-改-写"序列是串行的；
//      跨进程的竞争交给 SQLite 的写锁加 busy_timeout。
//
// 失败语义：可预期的业务结果用 StoreResult 表达（kNotFound / kAlreadyExists /
// kHasDependents / kChainConflict），只有"内部坏了"才是 kError。error_message
// 仅在 kError 时有意义，它的文案只进服务端日志，不回给客户端——里面可能带
// 磁盘路径与 SQLite 细节。
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
// 下面这组 pragma 在每次打开连接时整体重放：除 journal_mode 外都是连接级
// 设置，不写数据库文件，重放因此是幂等的；"这条连接的语义"也就只有一处定义，
// 服务端路径与只读管理工具路径共享同一份。
constexpr const char* kPragmaStatements[] = {
    // busy_timeout 必须排在**最前面**：并发首次打开时，第二条连接若还没装上
    // busy_timeout 就先执行 journal_mode=WAL（需要写锁），它会立刻拿到
    // SQLITE_BUSY，表现为随机的 "database is locked" 假失败（审查轮实测
    // 2 线程首次并发 Open 可复现，重试即成功）。
    "PRAGMA busy_timeout=5000;", "PRAGMA journal_mode=WAL;",
    "PRAGMA synchronous=FULL;",  "PRAGMA foreign_keys=ON;",
    "PRAGMA busy_timeout=5000;",
};

// 建表语句按"父表先于子表"排列：users 必须先于 snapshots 建好，否则
// foreign_keys=ON 之下那条外键会指向一个还不存在的表。整个数组在同一个事务里
// 顺序执行，中间任何一条失败都会回滚，因此不会留下半张 schema。
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
    // 链元数据。parent_id 用空串表示"没有父"，不用 NULL：
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
    // 列表查询固定按 (user_id, created_at) 过滤加排序，这个复合索引让它走索引
    // 扫描而不是全表扫。id 排在最后是为了让同一毫秒创建的两行也有确定顺序，
    // 列表因此稳定、可复现——分页或对比两次刷新时才不会莫名换位。
    "  ON snapshots(user_id, created_at, id);",
};

// 依赖感知删除要按 (user_id, parent_id) 找子节点。这个索引引用后加的列，
// 所以**必须**等列补齐之后再建：旧库上先建索引会以
// "no such column: parent_id" 失败，整个迁移就会回滚。
constexpr const char* kChainIndexStatements[] = {
    "CREATE INDEX IF NOT EXISTS snapshots_by_parent"
    "  ON snapshots(user_id, parent_id);",
};

// schema 1 -> 2 加入远端增量链元数据（snapshot_kind / parent_id /
// generation / lineage）。版本号就是 PRAGMA user_version。
//
// 迁移规则（这也是"绝不 DROP TABLE、绝不清库"的兑现方式）：
//   * 只做 ALTER TABLE ADD COLUMN 与 CREATE INDEX IF NOT EXISTS；
//   * 旧版写出的行一律留成"legacy standalone full"：snapshot_kind=0、
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

// 把一列的文本取成 std::string 的**拷贝**。必须拷贝：语句一旦 finalize 或者
// 再 step 一次，sqlite3_column_text 返回的指针就失效了。
// NULL 与零长度在这里被归成同一个空串——本 schema 里两者语义相同（旧行升级后
// parent_id / lineage 是空串，NOT NULL 列也不允许 NULL），因此不再区分。
std::string ColumnText(sqlite3_stmt* statement, int index) {
  const unsigned char* text = sqlite3_column_text(statement, index);
  const int bytes = sqlite3_column_bytes(statement, index);
  if (text == nullptr || bytes <= 0) {
    return std::string();
  }
  return std::string(reinterpret_cast<const char*>(text),
                     static_cast<std::size_t>(bytes));
}

// 与 ColumnText 同构，只是走 blob 接口：salt / hash 是二进制，任何按文本读取
// 都会在第一个 0 字节处截断。空 blob 同样映射成空串。
std::string ColumnBlob(sqlite3_stmt* statement, int index) {
  const void* data = sqlite3_column_blob(statement, index);
  const int bytes = sqlite3_column_bytes(statement, index);
  if (data == nullptr || bytes <= 0) {
    return std::string();
  }
  return std::string(static_cast<const char*>(data),
                     static_cast<std::size_t>(bytes));
}

// 全文件唯一的行解码点：SELECT 出来的列按下标映射到结构体字段，下标与
// kSnapshotColumns 的书写顺序一一对应（0=id …… 10=lineage）。两边任何一处
// 改动都必须同时改另一处，否则不会报错、只会整体错位。
// size_bytes / generation 是无符号语义，这里借 int64 中转：SQLite 的 INTEGER
// 本身就是有符号 64 位，而本产品的存储上限远达不到 2^63 字节。
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

// 所有读路径共用一份列清单：列表、详情、删除前回读必须投影出**同样的列序**，
// 否则 ReadSnapshotRow 的下标就不再成立。新增列只能追加到末尾。
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
  // out() 是给 sqlite3_prepare_v2 写回句柄用的接缝，get() 用于 bind / step。
  // 语句不可拷贝，也不提供 reset 复用：本文件的语句都是一次性的，
  // 复用会让"上一次绑定的参数残留"变成静默的错误来源。
  sqlite3_stmt** out() { return &statement_; }
  sqlite3_stmt* get() const { return statement_; }

 private:
  sqlite3_stmt* statement_ = nullptr;
};

}  // namespace

// 只给日志与测试断言用。返回的是稳定的英文标识串，不要本地化、不要改字面量
// （测试按这些字符串匹配）。switch 覆盖了当前全部枚举值，末尾的 UNKNOWN 是给
// "以后新增了枚举却忘了改这里"留的兜底，而不是可达的正常路径。
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

// 析构只做一件事：Close()。Close 是幂等的（见它的定义），所以"从没打开过就
// 析构"与"正常关闭后再析构"走同一条路，析构函数因此不必检查状态、也不会抛。
RemoteMetadataStore::~RemoteMetadataStore() { Close(); }

// 取**本连接**最近一次失败的英文原因。sqlite3_errmsg 的返回值只在下一次调用
// 同一连接上的 SQLite API 之前有效，所以这里立刻拷成 std::string；调用方必须
// 在持有 mutex_ 的区间内调用它，否则读到的是别的线程留下的原因。
std::string RemoteMetadataStore::LastError() const {
  if (database_ == nullptr) {
    return "database is not open";
  }
  const char* message = sqlite3_errmsg(database_);
  return message != nullptr ? std::string(message) : std::string("unknown");
}

// 编译一条 SQL。长度传 -1 表示"读到字符串结尾"，尾指针传 nullptr 表示不关心
// 剩余部分：因此调用方必须保证 sql 里只有一条语句，多余的会被静默忽略。
// 返回的句柄在 database_ 关闭之前一直有效，由 Statement 负责 finalize；
// 这里不校验绑定参数的个数，绑错位置要到 step 时才会暴露。
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

// 执行不需要结果集的语句：pragma、DDL、BEGIN / COMMIT / ROLLBACK。
// sqlite3_exec 的错误串由 SQLite 自己分配，必须用 sqlite3_free 释放；
// 下面所有分支都覆盖到了，包括成功路径上 message 仍是 nullptr 的情况。
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
// 输出参数 columns 先清空再填充：调用方会复用它，残留的旧列名会让迁移误判
// "这一列已经存在"，从而跳过本该执行的 ALTER。
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

// 建库与迁移的唯一入口，只在服务端的可写打开路径上被调用。
// 版本 0 = 全新库（建表即可），1 = 旧版写出的库（补 4 个链列），
// 2 = 当前版本；其它版本一律拒绝打开，而不是"尽力而为"——用未知 schema 读写
// 比直接失败危险得多。
// 事务用 BEGIN IMMEDIATE 而不是 BEGIN：立刻拿写锁，这样两个进程同时首次打开
// 时只有一个能执行迁移，另一个要么等待、要么在 busy_timeout 之后明确失败。
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
    // ALTER TABLE ... ADD COLUMN 带 NOT NULL 时必须给 DEFAULT：SQLite 会用
    // 默认值回填已有行，旧行立刻满足新列的非空约束，不需要额外的 UPDATE，
    // 也就不存在"迁移做到一半、半张表是旧值"的中间态。
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
  // 版本号与 schema 变更在**同一个事务**里提交：user_version 一旦变成 2，
  // 四个列和那个索引就一定都已经在了。反过来，回滚会把版本号一起退回，
  // 于是下次启动会重试整个迁移（迁移本身幂等）。
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
// 这里的表名是编译期常量、不含任何用户输入，所以字符串拼接不构成注入面；
// 用户输入永远只走绑定参数。
constexpr const char* kRequiredTables[] = {"users", "snapshots",
                                           "deleted_users"};

bool RemoteMetadataStore::VerifyExistingSchema(std::string* error_message) {
  // 版本不匹配就明确失败，而不是"顺手"把库升级成新 schema：那是一次写操作，
  // 而只读命令完全可能正在 backup-server 运行时执行。
  // 迁移只发生在服务端启动路径（EnsureSchema）；管理工具面对一个
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
  // 只 Prepare 加 step 一次 count(*)：能编译、能执行就说明表存在且可读。
  // 不比较行数，也不做任何写入——走到这条路径的读者可能正与服务端并发。
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

// 服务端启动路径：打开（不存在就创建）并确保 schema 是当前版本，这里是本文件
// 唯一允许建库的地方。任何一步失败都会把这半开的连接关掉、把 database_ 复位成
// nullptr，因此返回 false 之后对象仍处于"未打开"这个合法状态，
// 调用方可以安全地重试或者直接析构。
// SQLITE_OPEN_FULLMUTEX 是第二层保险：即便将来有人在锁外误用连接，
// SQLite 自己也会串行化，而不是产生数据竞争。
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

// 两个具名入口只做一件事：把 writable 这个布尔量固化成两条**产品承诺**。
// 调用方读代码时看到的是"我只是看看"或者"我要改数据"，而不是一个裸的 true。
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
  // 先 stat 再 open 之间有一个理论上的 TOCTOU 窗口，但这里的后果可控：
  // 文件若在这两步之间被删掉，下面的 open 只会失败（刻意不带 CREATE），
  // 不会有任何写操作落到错误的位置。
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

// 幂等：可以在任何状态下反复调用（析构、显式关闭、失败清理都走它）。
// 这里只调 sqlite3_close 而不检查返回值，因为本文件所有语句都由 Statement 在
// 离开作用域时 finalize，不存在未释放的句柄；反过来说，若真有句柄泄漏，
// sqlite3_close 会返回 SQLITE_BUSY 并保留连接，那种情况必须让测试暴露出来，
// 而不是靠这里的重试掩盖。
void RemoteMetadataStore::Close() {
  std::lock_guard<std::mutex> guard(mutex_);
  if (database_ != nullptr) {
    sqlite3_close(database_);
    database_ = nullptr;
  }
  path_.clear();
}

// 注册一个新账户。前置条件：password.salt / hash 的长度必须等于 remote_auth
// 约定的长度——长度不对说明上游被改坏了，这里直接拒绝而不是"先存进去再说"，
// 因为一条长度错误的记录会让这个账户以后所有登录都失败。
// kAlreadyExists 同时覆盖 UNIQUE(username) 与主键冲突两种情况：对调用方来说
// "这个名字已存在"和"这一次没写进去"是同一件事。
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
  // 所有绑定都用 SQLITE_TRANSIENT：SQLite 会自己拷一份数据，调用方传入的
  // std::string 在 step 之前析构也不会留下悬垂指针（SQLITE_STATIC 才要求
  // 调用方保证生命周期，这里没有那个必要，也不值得冒那个风险）。
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

// 按用户名查账户（登录路径）。kNotFound 是**正常结果**而不是错误：用户不存在
// 与口令不对必须由调用方给出同一个外部答复，否则就成了用户名枚举接口。
// out 允许为 nullptr（只做存在性探测）；命中时密码字段原样返回，是否匹配由
// remote_auth 用 constant-time 比较判定，本层不做任何判断。
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

// 按 id 查账户：会话令牌里带的是 subject（id），不是用户名。与 FindUser 分成
// 两个入口，是因为它们查的是不同的唯一键——用户名可能被管理员重建，
// 而 id 一旦分配就永不重用（见 CreateUser 的分配规则与 deleted_users 墓碑）。
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

// 写入一行快照，并在写入的同一临界区里重新校验整条链的约束。
//
// 事务边界刻意分成两种：
//   * 增量：BEGIN IMMEDIATE -> 读父 -> 查兄弟 -> INSERT -> COMMIT 是一个
//     事务，任何一步失败都 ROLLBACK，于是"并发删除把父删掉"和"并发上传挂
//     第二个孩子"都会被写锁挡住，不会留下孤儿或分叉；
//   * 完整快照：只有一条 INSERT，SQLite 的隐式事务本身就保证原子性，
//     不需要显式 BEGIN / COMMIT（多开一个事务只会多一次 fsync）。
//
// 读父与查兄弟都用 user_id 与 id 一起做条件：别人的快照在这里与不存在的快照
// 得到完全相同的答案，客户端无法凭返回码的差别去探测别的租户有哪些 id。
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
  if (!Prepare(
          "INSERT INTO snapshots"
          " (id, user_id, display_name, size_bytes, sha256, created_at,"
          "  storage_name, snapshot_kind, parent_id, generation, lineage)"
          // 绑定顺序必须与上面的列清单及其类型一一对应：SQLite 的类型是动态的，
          // 把一个 11
          // 个占位符的语句绑错位置通常不会报错，只会静默写进错误的值。
          // generation 与 size_bytes 都是无符号语义，绑定前显式转成 int64。
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
    // COMMIT 也可能失败（磁盘满、fsync 失败）：那时事务仍然开着，
    // 必须显式 ROLLBACK，否则这条连接会一直持有写锁，让后续所有调用超时。
    if (!Execute("COMMIT;", error_message)) {
      Execute("ROLLBACK;", nullptr);
      return StoreResult::kError;
    }
  }
  return StoreResult::kOk;
}

// 列出某个用户的全部快照，按 (created_at, id) 升序——这是**展示顺序**，
// 也保证了父一定排在子前面，调用方据此渲染链。
// 注意：这里直接往 out 里 push_back，中途失败时 out 里会留下已经读到的部分
// 内容，调用方在 kError 时必须丢弃 out（ListUsers 用局部 vector 保证了失败时
// 不动 out，两者的失败语义并不相同）。
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

// 按 (user_id, id) 取一行。条件里同时带 user_id 有两个作用：让"别人的 id"与
// "不存在的 id"返回同一个 kNotFound（不可探测），以及让查询走上
// snapshots_by_user 的索引前缀，避免任何跨租户的读路径。
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

// 删除一行快照，并把被删掉的那行回填给调用方——磁盘上的 blob 要靠它的
// storage_name 才能定位。
// 顺序是：回读整行 -> 查有没有子节点 -> DELETE。回读与删除在同一个 mutex_ 区间
// 里，因此不存在"查到 A 的行、删掉 B 的行"这种窗口；即使本进程之外的写者在
// 这两步之间抢先删掉了同一行，最后那条 DELETE 也只影响 0 行，由
// sqlite3_changes() 捕获并报 kError，绝不假装成功。
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
  // 依赖感知删除的**最后一道闸门**。调用方（RemoteMaintenance）在动
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
  // 能走到这里说明这一行一定还在（本进程内由 mutex_ 串行化），changes() 不是
  // 1 就意味着有本进程之外的写者动过它——这是"数据被并发的管理工具改动"的
  // 明确信号，必须报错让上层停下来，而不是当成"已经删过了"。
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

// 删除前的依赖检查。DeleteSnapshot 内部也会查一次，这里是给**调用方**用的：
// 运维工具需要在动磁盘之前就告诉操作者"删不了，先删这 N 个后代"，
// 而不是先把 blob 挪进隔离区、失败了再回滚回来。
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

// 某个用户当前的快照条数。用于配额检查和"删除前先告诉用户会删掉多少"，
// 不参与链的正确性判断（链的约束在 InsertSnapshot 与 DeleteSnapshot 里）。
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

// 管理视图：每个账户一行汇总。LEFT JOIN 保证"一个快照都没有的用户"也会出现
// 在结果里（COUNT 为 0、SUM 由 COALESCE 兜成 0），否则管理员会以为账户没建上。
// 结果先攒在局部 vector 里，全部读完才赋给 out：中途失败时 out 保持原样，
// 不会留下半张列表。
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

// 三个互相独立的 COUNT / SUM 查询拼出概览。刻意**不**放进一个事务：这是只读的
// 展示数据，允许三次查询之间恰好有一次提交落进来（总数与字节数因此可能差一条
// 记录），为此去拿写锁反而会干扰正在上传的客户端。
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
      // 只在 Prepare 没写过错的时候补写错误：一旦 Prepare 失败，
      // error_message 里已经有更具体的原因，不能被这里的通用文案覆盖掉。
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

// 注销账户：同一个事务里先删该用户的全部 snapshots 行，再删 users 行，
// 最后写一条 deleted_users 墓碑。顺序不能反——foreign_keys=ON 之下先删 users
// 会撞上外键约束，那条外键就是这个顺序的护栏。
// 墓碑有两个作用：审计（这个 id 确实注销过）与"id 永不重用"的依据。
// 事务之外没有副作用，任何一步失败都只是 ROLLBACK；磁盘上的 blob 由调用方在
// 拿到 kOk 之后才去处理。
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
    // 先聚合再删：回给调用方的"删掉了多少条 / 多少字节"必须是**删除前**的
    // 事实，删完之后再统计就什么都查不到了。
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
    // 用户行不存在时整个事务回滚（连上面已经删掉的 snapshots 行一起退回）：
    // 一次"看起来删掉了一半"的部分成功，会让调用方误以为磁盘上的隔离也该
    // 保留。
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
    // INSERT OR REPLACE：墓碑表的主键就是 user id，对同一个 id 重复注销
    // （理论上不该发生，id 只在分配后使用一次）也必须幂等而不是报错。
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
  // 测试接缝放在 COMMIT 之前：注入的失败必须走**和真实失败同一条**回滚路径，
  // 否则被测试到的那个分支就不是产品分支。
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
