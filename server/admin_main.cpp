// server/admin_main.cpp
//
// backup-server-admin：**只能在服务器本机运行**的管理工具。
//
// ---- 安全边界（这一段是产品约束，不是注释）----
//
//   * 它不监听任何端口：本文件里没有 socket()、bind()、listen()、accept()；
//   * 它不是 BPNET1 的一部分：普通客户端（RemoteArchiveClient / backupctl
//     remote / Modern GUI）里没有、也不会有任何路径能调到它；
//   * 它不开 HTTP，不要求浏览器，不依赖任何 Web 框架；
//   * 正确用法只有一种：先 SSH 登录到 ECS，再在 ECS 本机执行它。
//
// ---- 它做什么 ----
//
//   用户管理（列表 / 详情 / 删除）、备份文件管理（列表 / 详情 / 删除）、
//   存储概览。破坏性操作复用服务端同一份 RemoteMaintenance，并且多两道闸门：
//
//     1. 命令行必须给出 --confirm，内容与目标一致（不是 y/n 这种一按就过的
//        确认）；
//     2. 必须抢到数据目录的独占锁。backup-server 在运行时一直持有它，于是
//        管理工具**拒绝**删除并提示"请先停止 backup-server"，而不是与正在
//        写的服务端竞态。这不是 pgrep 猜一下然后 rm，而是内核持有的 flock。
//
// 只读操作（列表 / 详情 / 概览）在服务端运行时照常可用。
//
// ---- 它不打印什么 ----
//
// 口令 salt / hash / token secret 一次都不出现在输出里。这不是"记得别打印"：
// 本工具用到的查询（RemoteMetadataStore::ListUsers）根本不选那些列，而
// 用户名/快照 id 之外的输入永远不会被拼进文件系统路径。

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "file_lock.h"
#include "network_protocol.h"
#include "remote_maintenance.h"
#include "remote_metadata_store.h"

namespace {

using backupproject::FileLock;
using backupproject::FileLockStatus;
using backupproject::net::IsValidSnapshotId;
using backupproject::net::IsValidUsername;
using backupproject::net::RemoteMaintenance;
using backupproject::net::RemoteMetadataStore;
using backupproject::net::RemoteSnapshotRecord;
using backupproject::net::RemoteStorageOverview;
using backupproject::net::RemoteUserRecord;
using backupproject::net::RemoteUserSummary;
using backupproject::net::StoreResult;

void PrintUsage(std::FILE* out, const char* program) {
  std::fprintf(
      out,
      "用法: %s --root <数据目录> --db <数据库文件> <命令> [参数]\n"
      "\n"
      "只读命令（backup-server 运行时也可以用）：\n"
      "  status                                   服务状态与总量\n"
      "  list-users                               用户列表\n"
      "  show-user <用户 id 或用户名>             用户详情\n"
      "  list-snapshots <用户 id 或用户名>        某个用户的备份列表\n"
      "  show-snapshot <快照 id>                  备份详情（含完整 SHA-256）\n"
      "  overview                                 存储概览\n"
      "\n"
      "破坏性命令（要求 backup-server 已停止，且 --confirm 与目标一致）：\n"
      "  delete-snapshot <快照 id> --user <用户 id 或用户名> --confirm <快照 id>\n"
      "  delete-user <用户 id 或用户名> --confirm \"DELETE <用户名>\"\n"
      "\n"
      "退出码: 0 成功 / 1 失败或被拒绝 / 2 用法错误\n",
      program);
}

std::string FormatTime(std::int64_t unix_seconds) {
  if (unix_seconds <= 0) {
    return "-";
  }
  std::time_t when = static_cast<std::time_t>(unix_seconds);
  std::tm parts;
  std::memset(&parts, 0, sizeof(parts));
  if (::localtime_r(&when, &parts) == nullptr) {
    return "-";
  }
  char buffer[32];
  if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &parts) == 0) {
    return "-";
  }
  return std::string(buffer);
}

std::string FormatSize(std::uint64_t bytes) {
  static const char* kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB"};
  double value = static_cast<double>(bytes);
  std::size_t unit = 0;
  while (value >= 1024.0 && unit + 1 < sizeof(kUnits) / sizeof(kUnits[0])) {
    value /= 1024.0;
    ++unit;
  }
  char buffer[64];
  if (unit == 0) {
    std::snprintf(buffer, sizeof(buffer), "%llu B",
                  static_cast<unsigned long long>(bytes));
  } else {
    std::snprintf(buffer, sizeof(buffer), "%.1f %s", value, kUnits[unit]);
  }
  return std::string(buffer);
}

std::string HostName() {
  char buffer[256];
  if (::gethostname(buffer, sizeof(buffer)) != 0) {
    return "(unknown)";
  }
  buffer[sizeof(buffer) - 1] = '\0';
  return std::string(buffer);
}

struct Options {
  std::string root_directory;
  std::string database_path;
  std::string command;
  std::vector<std::string> positional;
  std::string user_selector;
  std::string confirm;
};

bool ParseOptions(int argc, char* argv[], Options* options,
                  std::string* error_message) {
  for (int index = 1; index < argc; ++index) {
    const std::string name = argv[index];
    if (name == "--help" || name == "-h") {
      return false;
    }
    if (name == "--root" || name == "--db" || name == "--user" ||
        name == "--confirm") {
      if (index + 1 >= argc) {
        *error_message = name + " needs a value";
        return false;
      }
      const std::string value = argv[++index];
      if (name == "--root") {
        options->root_directory = value;
      } else if (name == "--db") {
        options->database_path = value;
      } else if (name == "--user") {
        options->user_selector = value;
      } else {
        options->confirm = value;
      }
      continue;
    }
    if (!name.empty() && name[0] == '-' && options->command.empty()) {
      *error_message = "unknown option " + name;
      return false;
    }
    if (options->command.empty()) {
      options->command = name;
    } else {
      options->positional.push_back(name);
    }
  }
  if (options->command.empty()) {
    *error_message = "no command given";
    return false;
  }
  if (options->root_directory.empty() || options->database_path.empty()) {
    *error_message = "--root and --db are both required";
    return false;
  }
  return true;
}

// 只读地把数据目录的锁试一遍：抢到就说明没有 backup-server 持有它（随即释放），
// 抢不到就是 kBusy。锁文件里的 pid/时间只是给人看的提示。
bool DescribeServerState(const std::string& root_directory, std::string* text) {
  FileLock probe;
  std::string error;
  const FileLockStatus status =
      probe.Acquire(RemoteMaintenance::LockFilePath(root_directory), &error);
  if (status == FileLockStatus::kAcquired) {
    probe.Release();
    *text = "未运行（没有进程持有数据目录锁）";
    return true;
  }
  if (status == FileLockStatus::kBusy) {
    const std::string hint = RemoteMaintenance::ReadLockHint(root_directory);
    *text = "backup-server 正在运行" +
            (hint.empty() ? std::string() : "（" + hint + "）");
    return false;
  }
  *text = "无法判断（" + error + "）";
  return false;
}

// 破坏性操作的闸门：真抢锁并**持有**它。
bool AcquireDestructiveLock(const std::string& root_directory, FileLock* lock,
                            std::string* error_message) {
  std::string lock_error;
  const FileLockStatus status =
      lock->Acquire(RemoteMaintenance::LockFilePath(root_directory),
                    &lock_error);
  if (status == FileLockStatus::kAcquired) {
    return true;
  }
  if (status == FileLockStatus::kBusy) {
    const std::string hint = RemoteMaintenance::ReadLockHint(root_directory);
    *error_message =
        "backup-server 正在运行" +
        (hint.empty() ? std::string() : "（" + hint + "）") +
        "，拒绝执行删除操作。请先在服务器上停止 backup-server，再重新运行"
        "本命令。";
    return false;
  }
  *error_message = "无法获取数据目录锁：" + lock_error;
  return false;
}

bool ResolveUser(RemoteMetadataStore* store, const std::string& selector,
                 std::int64_t* user_id, std::string* username,
                 std::string* error_message) {
  if (selector.empty()) {
    *error_message = "缺少用户参数（用户 id 或用户名）";
    return false;
  }
  RemoteUserRecord record;
  std::string store_error;
  StoreResult result = StoreResult::kNotFound;
  bool numeric = true;
  for (const char character : selector) {
    if (character < '0' || character > '9') {
      numeric = false;
      break;
    }
  }
  if (numeric && selector.size() <= 18) {
    result = store->FindUserById(
        static_cast<std::int64_t>(std::strtoll(selector.c_str(), nullptr, 10)),
        &record, &store_error);
  } else {
    // 用户名先过共享校验器：它只允许 [A-Za-z0-9_.-]，而且**永远不会**被拼进
    // 文件系统路径（磁盘上一律用数字 id）。
    std::string validation_error;
    if (!IsValidUsername(selector, &validation_error)) {
      *error_message = "用户参数既不像是 id，也不是合法用户名：" +
                       validation_error;
      return false;
    }
    result = store->FindUser(selector, &record, &store_error);
  }
  if (result == StoreResult::kNotFound) {
    *error_message = "没有这个用户：" + selector;
    return false;
  }
  if (result != StoreResult::kOk) {
    *error_message = "读取用户记录失败：" + store_error;
    return false;
  }
  *user_id = record.user_id;
  *username = record.username;
  return true;
}

int OpenAll(const Options& options, RemoteMetadataStore* store,
            RemoteMaintenance* maintenance, std::string* error_message) {
  if (!store->Open(options.database_path, error_message)) {
    return -1;
  }
  *maintenance =
      RemoteMaintenance(store, options.root_directory);
  if (!maintenance->ready()) {
    *error_message = "维护层没有配置好";
    return -1;
  }
  return 0;
}

void PrintStatus(RemoteMetadataStore* store, const Options& options) {
  std::printf("服务器：%s\n", HostName().c_str());
  std::printf("数据目录：%s\n", options.root_directory.c_str());
  std::printf("数据库：%s\n", options.database_path.c_str());
  std::string state;
  DescribeServerState(options.root_directory, &state);
  std::printf("服务状态：%s\n", state.c_str());
  RemoteStorageOverview overview;
  std::string error;
  if (store->StorageOverview(&overview, &error) == StoreResult::kOk) {
    std::printf("用户数：%llu　快照数：%llu　blob 总大小：%s　已注销账户：%llu\n",
                static_cast<unsigned long long>(overview.user_count),
                static_cast<unsigned long long>(overview.snapshot_count),
                FormatSize(overview.total_bytes).c_str(),
                static_cast<unsigned long long>(overview.deleted_user_count));
  } else {
    std::printf("统计失败：%s\n", error.c_str());
  }
}

int CommandListUsers(RemoteMetadataStore* store) {
  std::vector<RemoteUserSummary> users;
  std::string error;
  if (store->ListUsers(&users, &error) != StoreResult::kOk) {
    std::fprintf(stderr, "读取用户列表失败：%s\n", error.c_str());
    return 1;
  }
  if (users.empty()) {
    std::printf("还没有任何用户。\n");
    return 0;
  }
  std::printf("%-6s %-24s %-20s %10s %14s\n", "ID", "用户名", "创建时间",
              "备份数", "占用空间");
  for (const RemoteUserSummary& user : users) {
    std::printf("%-6lld %-24s %-20s %10llu %14s\n",
                static_cast<long long>(user.user_id), user.username.c_str(),
                FormatTime(user.created_at).c_str(),
                static_cast<unsigned long long>(user.snapshot_count),
                FormatSize(user.total_bytes).c_str());
  }
  std::printf("共 %llu 个用户。\n",
              static_cast<unsigned long long>(users.size()));
  return 0;
}

int CommandShowUser(RemoteMetadataStore* store, const std::string& selector) {
  std::int64_t user_id = 0;
  std::string username;
  std::string error;
  if (!ResolveUser(store, selector, &user_id, &username, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  RemoteUserRecord record;
  std::string store_error;
  if (store->FindUserById(user_id, &record, &store_error) != StoreResult::kOk) {
    std::fprintf(stderr, "读取用户记录失败：%s\n", store_error.c_str());
    return 1;
  }
  std::uint64_t count = 0;
  if (store->CountSnapshots(user_id, &count, &store_error) !=
      StoreResult::kOk) {
    std::fprintf(stderr, "统计备份数量失败：%s\n", store_error.c_str());
    return 1;
  }
  std::vector<RemoteSnapshotRecord> snapshots;
  if (store->ListSnapshots(user_id, &snapshots, &store_error) !=
      StoreResult::kOk) {
    std::fprintf(stderr, "读取备份列表失败：%s\n", store_error.c_str());
    return 1;
  }
  std::uint64_t total = 0;
  for (const RemoteSnapshotRecord& snapshot : snapshots) {
    total += snapshot.size_bytes;
  }
  std::printf("用户 ID：%lld\n", static_cast<long long>(user_id));
  std::printf("用户名：%s\n", username.c_str());
  std::printf("创建时间：%s\n", FormatTime(record.created_at).c_str());
  std::printf("备份数量：%llu\n", static_cast<unsigned long long>(count));
  std::printf("占用空间：%s\n", FormatSize(total).c_str());
  // 口令字段（salt / hash / 迭代次数）只存在于数据库里：本工具不打印它们，
  // 也没有任何开关能把它们打开。
  return 0;
}

int PrintSnapshotTable(const std::vector<RemoteSnapshotRecord>& snapshots) {
  if (snapshots.empty()) {
    std::printf("这个用户还没有云端备份。\n");
    return 0;
  }
  std::printf("%-34s %-28s %12s %-20s\n", "快照 ID", "显示名", "大小",
              "创建时间");
  for (const RemoteSnapshotRecord& snapshot : snapshots) {
    std::printf("%-34s %-28s %12s %-20s\n", snapshot.snapshot_id.c_str(),
                snapshot.display_name.c_str(),
                FormatSize(snapshot.size_bytes).c_str(),
                FormatTime(snapshot.created_at).c_str());
  }
  std::printf("共 %llu 个备份。\n",
              static_cast<unsigned long long>(snapshots.size()));
  return 0;
}

int CommandListSnapshots(RemoteMetadataStore* store,
                         const std::string& selector) {
  std::int64_t user_id = 0;
  std::string username;
  std::string error;
  if (!ResolveUser(store, selector, &user_id, &username, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  std::vector<RemoteSnapshotRecord> snapshots;
  std::string store_error;
  if (store->ListSnapshots(user_id, &snapshots, &store_error) !=
      StoreResult::kOk) {
    std::fprintf(stderr, "读取备份列表失败：%s\n", store_error.c_str());
    return 1;
  }
  std::printf("用户：%s（id=%lld）\n", username.c_str(),
              static_cast<long long>(user_id));
  return PrintSnapshotTable(snapshots);
}

int CommandShowSnapshot(RemoteMetadataStore* store,
                        const std::string& snapshot_id) {
  std::string validation_error;
  if (!IsValidSnapshotId(snapshot_id, &validation_error)) {
    std::fprintf(stderr, "快照 id 不合法：%s\n", validation_error.c_str());
    return 1;
  }
  // 快照 id 全局唯一，但要按用户过滤才能查到：这里逐个用户查，避免为管理
  // 工具新增一条"无 user_id 过滤"的查询路径。
  std::vector<RemoteUserSummary> users;
  std::string error;
  if (store->ListUsers(&users, &error) != StoreResult::kOk) {
    std::fprintf(stderr, "读取用户列表失败：%s\n", error.c_str());
    return 1;
  }
  for (const RemoteUserSummary& user : users) {
    RemoteSnapshotRecord snapshot;
    std::string store_error;
    if (store->FindSnapshot(user.user_id, snapshot_id, &snapshot,
                            &store_error) == StoreResult::kOk) {
      std::printf("快照 ID：%s\n", snapshot.snapshot_id.c_str());
      std::printf("归属用户：%s（id=%lld）\n", user.username.c_str(),
                  static_cast<long long>(user.user_id));
      std::printf("显示名：%s\n", snapshot.display_name.c_str());
      std::printf("大小：%s（%llu 字节）\n",
                  FormatSize(snapshot.size_bytes).c_str(),
                  static_cast<unsigned long long>(snapshot.size_bytes));
      std::printf("创建时间：%s\n", FormatTime(snapshot.created_at).c_str());
      std::printf("SHA-256：%s\n", snapshot.sha256.c_str());
      std::printf("磁盘文件：%s\n", snapshot.storage_name.c_str());
      return 0;
    }
  }
  std::fprintf(stderr, "没有这个快照：%s\n", snapshot_id.c_str());
  return 1;
}

int CommandOverview(RemoteMetadataStore* store,
                    const std::string& root_directory) {
  RemoteStorageOverview overview;
  std::string error;
  if (store->StorageOverview(&overview, &error) != StoreResult::kOk) {
    std::fprintf(stderr, "读取存储概览失败：%s\n", error.c_str());
    return 1;
  }
  std::printf("用户数：%llu\n",
              static_cast<unsigned long long>(overview.user_count));
  std::printf("快照总数：%llu\n",
              static_cast<unsigned long long>(overview.snapshot_count));
  std::printf("blob 总大小：%s（%llu 字节）\n",
              FormatSize(overview.total_bytes).c_str(),
              static_cast<unsigned long long>(overview.total_bytes));
  std::printf("已注销账户：%llu\n",
              static_cast<unsigned long long>(overview.deleted_user_count));
  std::string state;
  DescribeServerState(root_directory, &state);
  std::printf("服务状态：%s\n", state.c_str());
  // 按占用空间排序的前几名：这是"谁把磁盘吃掉了"最直接的答案。
  std::vector<RemoteUserSummary> users;
  if (store->ListUsers(&users, &error) == StoreResult::kOk) {
    for (std::size_t i = 0; i < users.size(); ++i) {
      for (std::size_t j = i + 1; j < users.size(); ++j) {
        if (users[j].total_bytes > users[i].total_bytes) {
          const RemoteUserSummary swap = users[i];
          users[i] = users[j];
          users[j] = swap;
        }
      }
    }
    const std::size_t limit = users.size() < 5 ? users.size() : 5;
    if (limit > 0) {
      std::printf("占用最多的用户：\n");
      for (std::size_t index = 0; index < limit; ++index) {
        std::printf("  %-24s %10s（%llu 个备份）\n",
                    users[index].username.c_str(),
                    FormatSize(users[index].total_bytes).c_str(),
                    static_cast<unsigned long long>(
                        users[index].snapshot_count));
      }
    }
  }
  return 0;
}

int CommandDeleteSnapshot(RemoteMetadataStore* store,
                          RemoteMaintenance* maintenance,
                          const Options& options, const std::string& snapshot_id,
                          std::string* error_message) {
  std::string validation_error;
  if (!IsValidSnapshotId(snapshot_id, &validation_error)) {
    *error_message = "快照 id 不合法：" + validation_error;
    return 1;
  }
  if (options.confirm != snapshot_id) {
    *error_message =
        "确认字符串与快照 id 不一致。要真的删除，请加 "
        "--confirm <快照 id>。";
    return 1;
  }
  std::int64_t user_id = 0;
  std::string username;
  if (!ResolveUser(store, options.user_selector, &user_id, &username,
                   error_message)) {
    return 1;
  }
  RemoteSnapshotRecord removed;
  std::string delete_error;
  const StoreResult result =
      maintenance->DeleteSnapshot(user_id, snapshot_id, &removed,
                                  &delete_error);
  if (result != StoreResult::kOk) {
    *error_message = "删除失败（磁盘与元数据都保持原样）：" + delete_error;
    return 1;
  }
  std::printf("已删除快照 %s（用户 %s，%s，SHA-256 %s）\n",
              removed.snapshot_id.c_str(), username.c_str(),
              FormatSize(removed.size_bytes).c_str(), removed.sha256.c_str());
  return 0;
}

int CommandDeleteUser(RemoteMetadataStore* store,
                      RemoteMaintenance* maintenance,
                      const Options& options, const std::string& selector,
                      std::string* error_message) {
  std::int64_t user_id = 0;
  std::string username;
  if (!ResolveUser(store, selector, &user_id, &username, error_message)) {
    return 1;
  }
  // 二次确认必须写出完整的 "DELETE <用户名>"：一个 y 键按不出不可逆的删除。
  const std::string expected = "DELETE " + username;
  if (options.confirm != expected) {
    *error_message = "确认字符串不正确。要真的删除这个账户及其全部云端备份，"
                     "请加 --confirm \"" +
                     expected + "\"。";
    return 1;
  }
  std::uint64_t removed_snapshots = 0;
  std::uint64_t removed_bytes = 0;
  std::string delete_error;
  const StoreResult result =
      maintenance->DeleteAccount(user_id, &removed_snapshots, &removed_bytes,
                                 &delete_error);
  if (result != StoreResult::kOk) {
    *error_message = "删除账户失败（数据与元数据都保持原样）：" + delete_error;
    return 1;
  }
  std::printf("已删除账户 %s（id=%lld）：%llu 个备份、%s 已从磁盘清除。\n",
              username.c_str(), static_cast<long long>(user_id),
              static_cast<unsigned long long>(removed_snapshots),
              FormatSize(removed_bytes).c_str());
  return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  const char* program =
      (argc > 0 && argv[0] != nullptr) ? argv[0] : "backup-server-admin";
  Options options;
  std::string parse_error;
  if (!ParseOptions(argc, argv, &options, &parse_error)) {
    if (parse_error.empty()) {
      PrintUsage(stdout, program);
      return 0;
    }
    std::fprintf(stderr, "错误：%s\n\n", parse_error.c_str());
    PrintUsage(stderr, program);
    return 2;
  }

  const bool destructive = options.command == "delete-snapshot" ||
                           options.command == "delete-user";

  // 破坏性操作先抢数据目录锁，再打开数据库：服务端在跑的时候要尽早拒绝，
  // 而且绝不能在"以为安全"的状态下动一个正在被写的目录。
  FileLock lock;
  if (destructive) {
    std::string lock_error;
    if (!AcquireDestructiveLock(options.root_directory, &lock, &lock_error)) {
      std::fprintf(stderr, "拒绝执行：%s\n", lock_error.c_str());
      return 1;
    }
  }

  RemoteMetadataStore store;
  RemoteMaintenance maintenance;
  std::string open_error;
  if (OpenAll(options, &store, &maintenance, &open_error) != 0) {
    std::fprintf(stderr, "无法打开数据目录：%s\n", open_error.c_str());
    return 1;
  }
  // 维护层的日志只写 stderr：管理工具的输出是给人看的表格，不该被日志混进去。
  maintenance.set_log([](const std::string& message) {
    std::fprintf(stderr, "[admin] %s\n", message.c_str());
  });

  const auto require_argument = [&options](std::size_t index,
                                           std::string* out) {
    if (options.positional.size() <= index) {
      return false;
    }
    *out = options.positional[index];
    return true;
  };

  if (options.command == "status") {
    PrintStatus(&store, options);
    return 0;
  }
  if (options.command == "list-users") {
    return CommandListUsers(&store);
  }
  if (options.command == "overview") {
    return CommandOverview(&store, options.root_directory);
  }
  if (options.command == "show-user" || options.command == "list-snapshots") {
    std::string selector;
    if (!require_argument(0, &selector)) {
      std::fprintf(stderr, "错误：%s 需要一个用户参数。\n",
                   options.command.c_str());
      return 2;
    }
    return options.command == "show-user" ? CommandShowUser(&store, selector)
                                          : CommandListSnapshots(&store, selector);
  }
  if (options.command == "show-snapshot") {
    std::string snapshot_id;
    if (!require_argument(0, &snapshot_id)) {
      std::fprintf(stderr, "错误：show-snapshot 需要一个快照 id。\n");
      return 2;
    }
    return CommandShowSnapshot(&store, snapshot_id);
  }
  if (options.command == "delete-snapshot") {
    std::string snapshot_id;
    if (!require_argument(0, &snapshot_id)) {
      std::fprintf(stderr, "错误：delete-snapshot 需要一个快照 id。\n");
      return 2;
    }
    std::string error;
    const int status =
        CommandDeleteSnapshot(&store, &maintenance, options, snapshot_id,
                              &error);
    if (status != 0) {
      std::fprintf(stderr, "错误：%s\n", error.c_str());
    }
    return status;
  }
  if (options.command == "delete-user") {
    std::string selector;
    if (!require_argument(0, &selector)) {
      std::fprintf(stderr, "错误：delete-user 需要一个用户参数。\n");
      return 2;
    }
    std::string error;
    const int status =
        CommandDeleteUser(&store, &maintenance, options, selector, &error);
    if (status != 0) {
      std::fprintf(stderr, "错误：%s\n", error.c_str());
    }
    return status;
  }

  std::fprintf(stderr, "错误：未知命令 %s\n\n", options.command.c_str());
  PrintUsage(stderr, program);
  return 2;
}
