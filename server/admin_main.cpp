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
// ---- 它显示什么（PR #21 UI closure：问题 A）----
//
// 每次运行都会打印一行 "传输身份指纹: sha256:…"。它来自**当前真正在服役的
// 那把传输身份私钥**：优先用正在运行的 backup-server 命令行里那个
// --transport-key-file（只读 /proc/<pid>/cmdline，不发信号、不连端口），
// 读不到才退到部署约定路径 <server-root>/state/transport.key。读回私钥用的是
// 服务端与 backup-server-keygen 共用的同一份 LoadTransportIdentity，指纹用的是
// 同一份 X25519Fingerprint——这里不重新实现任何一套编码。
//
// 它**只读**：不启动 / 不停止服务，不生成 / 不修改 / 不移动任何密钥文件。
// 读失败只是一行"不可用 + 原因"，菜单与其余输出照常。
//
// ---- 它不打印什么 ----
//
// 私钥内容、口令 salt / hash / token secret 一次都不出现在输出里。这不是
// "记得别打印"：本工具用到的查询（RemoteMetadataStore::ListUsers）根本不选
// 那些列，而用户名/快照 id 之外的输入永远不会被拼进文件系统路径；身份私钥
// 只被读进内存算公钥，算完立即清零。

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "admin_selector.h"
#include "file_lock.h"
#include "network_protocol.h"
#include "remote_maintenance.h"
#include "remote_metadata_store.h"
#include "secure_transport.h"
#include "x25519.h"

namespace {

using backupproject::FileLock;
using backupproject::FileLockStatus;
using backupproject::admin::AmbiguityMessage;
using backupproject::admin::DecideUserResolution;
using backupproject::admin::IsAllDigits;
using backupproject::admin::ParseUserSelector;
using backupproject::admin::UserResolution;
using backupproject::admin::UserSelector;
using backupproject::admin::UserSelectorKind;
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
      "      backup-server-admin --server-root <部署根> [--root ...] [--db "
      "...] ...\n"
      "\n"
      "  缺省布局（部署脚本安装的样子）：\n"
      "    <server-root>/data                 数据根（--root）\n"
      "    <server-root>/state/metadata.sqlite3  元数据库（--db）\n"
      "    <server-root>/state/transport.key  "
      "传输身份私钥（--transport-key-file，\n"
      "                                       只读；默认自动定位，见下）\n"
      "  数据库必须**已经存在**：管理工具不会创建空库，路径不对就报错退出。\n"
      "\n"
      "只读命令（backup-server 运行时也可以用）：\n"
      "  status                                   服务状态与总量\n"
      "  list-users                               用户列表\n"
      "  show-user <用户选择器>                   用户详情\n"
      "  list-snapshots <用户选择器>              某个用户的备份列表\n"
      "  show-snapshot <快照 id>                  备份详情（含完整 SHA-256）\n"
      "  overview                                 存储概览\n"
      "  transport-identity                       "
      "传输身份公钥与指纹（首次配置用）\n"
      "\n"
      "传输身份指纹:\n"
      "  每条命令的输出开头都会打印一行 \"传输身份指纹: "
      "sha256:…\"，它就是客户端要填的\n"
      "  “服务器身份指纹（server-key）”。读取顺序：--transport-key-file -> "
      "正在运行的\n"
      "  backup-server 实际加载的那把私钥（只读 /proc/<pid>/cmdline）-> "
      "部署约定路径。\n"
      "  读不到只显示 \"不可用 + "
      "原因\"，不影响任何命令；私钥内容永远不会被打印。\n"
      "\n"
      "用户选择器（**不会替你猜**）：\n"
      "  id:<编号>        只按编号找，例如 id:23\n"
      "  name:<用户名>    只按用户名找，例如 name:23（用户名允许是纯数字）\n"
      "  裸输入           只有不产生歧义时才被接受：同时命中 id "
      "与用户名就拒绝，\n"
      "                   并告诉你应该写成 id:23 还是 name:23。\n"
      "\n"
      "破坏性命令（要求 backup-server 已停止，且 --confirm 与目标一致）：\n"
      "  这两条命令的用户选择器**必须**写成 id:<编号> 或 name:<用户名>。\n"
      "  delete-snapshot <快照 id> --user id:<编号>|name:<用户名> --confirm "
      "<快照 id>\n"
      "  delete-user id:<编号>|name:<用户名> --confirm \"DELETE "
      "<用户名>#<编号>\"\n"
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

// 把路径变成"可以直接粘贴进终端"的绝对路径：存在时解析符号链接，不存在时
// 退化成"目录的 realpath + 文件名"。所有面向用户的路径都过这一层，避免界面上
// 出现 "data"/"state/metadata.sqlite3" 这种脱离上下文的相对路径——人工验收正是
// 因为看不出"这是哪一个库"而把空库读成了"ECS 里没有用户"。
std::string AbsolutePath(const std::string& path) {
  if (path.empty()) {
    return path;
  }
  char resolved[PATH_MAX];
  if (::realpath(path.c_str(), resolved) != nullptr) {
    return std::string(resolved);
  }
  const std::size_t slash = path.find_last_of('/');
  if (slash == std::string::npos) {
    char cwd[PATH_MAX];
    if (::getcwd(cwd, sizeof(cwd)) == nullptr) {
      return path;
    }
    return std::string(cwd) + "/" + path;
  }
  const std::string directory = path.substr(0, slash == 0 ? 1 : slash);
  const std::string name = path.substr(slash + 1);
  std::string resolved_directory = directory;
  if (::realpath(directory.c_str(), resolved) != nullptr) {
    resolved_directory = resolved;
  }
  if (name.empty()) {
    return resolved_directory;
  }
  return resolved_directory + "/" + name;
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
  // 部署根目录（<server-root>/bin 的上一级）。用于在界面上说清"我在看哪个
  // 实例"：--root 默认是 <server-root>/data，--db 默认是
  // <server-root>/state/metadata.sqlite3。
  std::string server_root;
  std::string root_directory;
  std::string database_path;
  // 传输身份私钥（BPSEC1 server identity）。留空时按"正在运行的服务端 ->
  // 部署约定路径"的顺序自动定位，见 LoadTransportIdentityForDisplay。
  std::string transport_key_file;
  std::string command;
  std::vector<std::string> positional;
  std::string user_selector;
  std::string confirm;
};

// 这个实例的部署根：显式给了就用，否则按约定从数据目录推（<server-root>/data）。
// 定义放在 Options 之后：它要用到那个结构体。
std::string ServerRootOf(const Options& options) {
  if (!options.server_root.empty()) {
    return AbsolutePath(options.server_root);
  }
  const std::string data_root = AbsolutePath(options.root_directory);
  const std::size_t slash = data_root.find_last_of('/');
  if (slash == std::string::npos || slash == 0) {
    return data_root;
  }
  return data_root.substr(0, slash);
}

// ---- 传输身份（BPSEC1 server identity）：只读地显示指纹 ----
//
// 这一段存在的唯一理由是"首次配置流程"：客户端的"服务器身份指纹（server-key）"
// 是必填项，用户必须能从服务器本机把它抄走。以前 admin 首页只有 Host /
// Server root / Data root / Metadata DB / Service 五项，用户拿不到指纹，
// 流程在第一步就断了。
//
// 显示的指纹必须**就是当前服务端在用的那一个身份**，因此读取顺序是：
//
//   1. 命令行显式给出的 --transport-key-file（调用方说了算）；
//   2. 正在运行的 backup-server 真正加载的那把私钥——从 /proc/<pid>/cmdline
//      里取出它的 --root 与 --transport-key-file，并要求 --root 与本实例的
//      数据根一致。这是"屏幕上这一行 == 服务端现在用的身份"最直接的证据；
//   3. 部署约定 <server-root>/state/transport.key（deploy 脚本的固定位置）。
//
// 全程只读：不开端口、不发信号、不启动/停止服务、不生成也不修改密钥文件。
struct TransportIdentityReport {
  bool available = false;
  std::string key_path;  // 实际读的那份私钥文件
  std::string source;  // 这个路径是怎么定下来的（给人看的说明）
  std::string fingerprint;  // 64 位小写十六进制（available 时有效）
  std::string public_key_hex;  // 32 字节公钥的十六进制（available 时有效）
  std::string error;           // available 为假时的原因
};

std::string BaseNameOf(const std::string& path) {
  const std::size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

// /proc 里的小文件：读失败或超过上界都返回 false，绝不部分相信。
bool ReadSmallFile(const std::string& path, std::size_t limit,
                   std::string* out) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  std::string data;
  char buffer[4096];
  for (;;) {
    const ssize_t count = ::read(fd, buffer, sizeof(buffer));
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      ::close(fd);
      return false;
    }
    if (count == 0) {
      break;
    }
    if (data.size() + static_cast<std::size_t>(count) > limit) {
      ::close(fd);
      return false;
    }
    data.append(buffer, static_cast<std::size_t>(count));
  }
  ::close(fd);
  *out = data;
  return true;
}

bool ReadProcessArguments(long pid, std::vector<std::string>* arguments) {
  std::string raw;
  if (!ReadSmallFile("/proc/" + std::to_string(pid) + "/cmdline", 256 * 1024,
                     &raw)) {
    return false;
  }
  arguments->clear();
  std::string current;
  for (const char character : raw) {
    if (character == '\0') {
      if (!current.empty()) {
        arguments->push_back(current);
        current.clear();
      }
      continue;
    }
    current.push_back(character);
  }
  if (!current.empty()) {
    arguments->push_back(current);
  }
  return !arguments->empty();
}

// 找正在运行的 backup-server，并取出它加载的身份私钥路径。只读 /proc。
bool FindRunningServerTransportKey(const std::string& data_root, long* pid_out,
                                   std::string* key_path,
                                   std::string* root_text) {
  DIR* proc = ::opendir("/proc");
  if (proc == nullptr) {
    return false;
  }
  const std::string wanted_root = AbsolutePath(data_root);
  bool found = false;
  while (struct dirent* entry = ::readdir(proc)) {
    const std::string name = entry->d_name;
    if (name.empty() || !IsAllDigits(name)) {
      continue;
    }
    const long pid = std::strtol(name.c_str(), nullptr, 10);
    if (pid <= 0) {
      continue;
    }
    std::vector<std::string> arguments;
    if (!ReadProcessArguments(pid, &arguments) || arguments.empty()) {
      continue;
    }
    if (BaseNameOf(arguments[0]) != "backup-server") {
      continue;
    }
    std::string process_root;
    std::string process_key;
    for (std::size_t index = 1; index < arguments.size(); ++index) {
      if (arguments[index] == "--root" && index + 1 < arguments.size()) {
        process_root = arguments[++index];
      } else if (arguments[index] == "--transport-key-file" &&
                 index + 1 < arguments.size()) {
        process_key = arguments[++index];
      }
    }
    if (process_root.empty() || process_key.empty()) {
      continue;
    }
    if (AbsolutePath(process_root) != wanted_root) {
      continue;
    }
    *pid_out = pid;
    *key_path = process_key;
    *root_text = process_root;
    found = true;
    break;
  }
  ::closedir(proc);
  return found;
}

// 读一次传输身份，只为了显示。永远不返回"猜"的结果：读不到就 available=false
// 并把原因带出来，调用方照着打印一行"不可用"。
TransportIdentityReport LoadTransportIdentityForDisplay(
    const Options& options) {
  TransportIdentityReport report;
  if (!options.transport_key_file.empty()) {
    report.key_path = options.transport_key_file;
    report.source = "命令行给出的 --transport-key-file";
  } else {
    long pid = 0;
    std::string process_key;
    std::string process_root;
    if (FindRunningServerTransportKey(options.root_directory, &pid,
                                      &process_key, &process_root)) {
      report.key_path = process_key;
      report.source = "正在运行的 backup-server（pid " + std::to_string(pid) +
                      "）加载的身份私钥";
    } else {
      report.key_path = ServerRootOf(options) + "/state/transport.key";
      report.source =
          "部署约定路径 <server-root>/state/transport.key（没有发现正在运行的 "
          "backup-server）";
    }
  }
  backupproject::net::TransportIdentity identity;
  std::string load_error;
  if (!backupproject::net::LoadTransportIdentity(report.key_path, &identity,
                                                 &load_error)) {
    report.error = load_error;
    return report;
  }
  report.available = true;
  report.fingerprint =
      backupproject::crypto::X25519Fingerprint(identity.public_key);
  report.public_key_hex =
      backupproject::crypto::X25519FormatKeyHex(identity.public_key);
  // 私钥材料用完即弃：它没有被打印、没有被写进任何文件，也不留在内存里。
  for (char& byte : identity.private_key) {
    byte = '\0';
  }
  return report;
}

bool ParseOptions(int argc, char* argv[], Options* options,
                  std::string* error_message) {
  for (int index = 1; index < argc; ++index) {
    const std::string name = argv[index];
    if (name == "--help" || name == "-h") {
      return false;
    }
    if (name == "--server-root" || name == "--root" || name == "--db" ||
        name == "--user" || name == "--confirm" ||
        name == "--transport-key-file") {
      if (index + 1 >= argc) {
        *error_message = name + " needs a value";
        return false;
      }
      const std::string value = argv[++index];
      if (name == "--server-root") {
        options->server_root = value;
      } else if (name == "--root") {
        options->root_directory = value;
      } else if (name == "--db") {
        options->database_path = value;
      } else if (name == "--user") {
        options->user_selector = value;
      } else if (name == "--transport-key-file") {
        options->transport_key_file = value;
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
  // 只给 --server-root 也能按部署约定推出另外两个；显式给出的永远优先。
  if (options->server_root.empty() && options->root_directory.empty() &&
      options->database_path.empty()) {
    *error_message = "--root and --db are both required";
    return false;
  }
  if (options->root_directory.empty()) {
    if (options->server_root.empty()) {
      *error_message = "--root is required when --server-root is not given";
      return false;
    }
    options->root_directory = options->server_root + "/data";
  }
  if (options->database_path.empty()) {
    if (options->server_root.empty()) {
      *error_message = "--db is required when --server-root is not given";
      return false;
    }
    options->database_path = options->server_root + "/state/metadata.sqlite3";
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
  const FileLockStatus status = lock->Acquire(
      RemoteMaintenance::LockFilePath(root_directory), &lock_error);
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

// 快照 id 允许写成 snapshot:<32 位十六进制>：前缀只是为了和用户选择器看起来
// 一致，去掉之后仍然按 32 位十六进制校验，规则不放松。
std::string StripSnapshotPrefix(const std::string& text) {
  const std::string prefix = "snapshot:";
  if (text.rfind(prefix, 0) == 0) {
    return text.substr(prefix.size());
  }
  return text;
}

// 把用户输入变成一个**确切的**账户。三种写法：
//
//   id:<编号>      只按 id 找
//   name:<用户名>  只按用户名找
//   裸输入         两个候选都查一遍：同时命中就拒绝（绝不猜），只命中一个就用
//                  那一个，并在 stderr 上说明这次是按哪一种解析的
//
// require_explicit（删除操作用 true）：裸输入一律拒绝——删除是不可逆的，不能让
// 一个"23 到底是编号还是用户名"的疑问决定删掉哪个账户。
bool ResolveUser(RemoteMetadataStore* store, const std::string& selector_text,
                 bool require_explicit, std::int64_t* user_id,
                 std::string* username, std::string* error_message) {
  UserSelector selector;
  std::string parse_error;
  if (!ParseUserSelector(selector_text, &selector, &parse_error)) {
    *error_message = parse_error;
    return false;
  }
  if (require_explicit && selector.kind == UserSelectorKind::kBare) {
    *error_message =
        "这是删除操作，必须明确指定用户：请写成 id:<编号> 或 name:<用户名>。"
        "裸输入 " +
        selector.text +
        " 既可能是编号也可能是用户名，管理工具不会替你猜"
        "（可以先用 show-user 看一眼再删）。";
    return false;
  }
  // 两个候选各查一次：只有把两个结果都摆出来，才谈得上"不猜"。
  RemoteUserRecord record;
  bool has_id_match = false;
  bool has_name_match = false;
  std::string store_error;
  if (selector.kind != UserSelectorKind::kName && IsAllDigits(selector.text) &&
      selector.text.size() <= 18) {
    RemoteUserRecord by_id;
    const StoreResult result =
        store->FindUserById(selector.id, &by_id, &store_error);
    if (result == StoreResult::kOk) {
      record = by_id;
      has_id_match = true;
    } else if (result != StoreResult::kNotFound) {
      *error_message = "读取用户记录失败：" + store_error;
      return false;
    }
  }
  if (selector.kind != UserSelectorKind::kId) {
    // 用户名先过共享校验器：它只允许 [A-Za-z0-9_.-]，而且**永远不会**被拼进
    // 文件系统路径（磁盘上一律用数字 id）。
    std::string validation_error;
    if (IsValidUsername(selector.text, &validation_error)) {
      RemoteUserRecord by_name;
      const StoreResult result =
          store->FindUser(selector.text, &by_name, &store_error);
      if (result == StoreResult::kOk) {
        record = by_name;
        has_name_match = true;
      } else if (result != StoreResult::kNotFound) {
        *error_message = "读取用户记录失败：" + store_error;
        return false;
      }
    }
  }
  switch (DecideUserResolution(selector, has_id_match, has_name_match)) {
    case UserResolution::kAmbiguous:
      *error_message = AmbiguityMessage(selector.text);
      return false;
    case UserResolution::kNotFound:
      if (selector.kind == UserSelectorKind::kId) {
        *error_message = "没有 id=" + selector.text + " 的用户";
      } else if (selector.kind == UserSelectorKind::kName) {
        *error_message = "没有叫 " + selector.text + " 的用户";
      } else {
        *error_message = "没有这个用户：" + selector.text;
      }
      return false;
    case UserResolution::kUseId:
      if (selector.kind == UserSelectorKind::kBare) {
        std::fprintf(stderr,
                     "提示：%s 这次按编号解析（id=%lld）。写成 id:%s 就不会有"
                     "歧义。\n",
                     selector.text.c_str(), static_cast<long long>(selector.id),
                     selector.text.c_str());
      }
      break;
    case UserResolution::kUseName:
      if (selector.kind == UserSelectorKind::kBare) {
        std::fprintf(stderr,
                     "提示：%s 这次按用户名解析。写成 name:%s 就不会有歧义。\n",
                     selector.text.c_str(), selector.text.c_str());
      }
      break;
  }
  *user_id = record.user_id;
  *username = record.username;
  return true;
}

int OpenAll(const Options& options, bool writable, RemoteMetadataStore* store,
            RemoteMaintenance* maintenance, std::string* error_message) {
  // **fail closed**：只打开已经存在的数据库。
  //
  // 管理工具没有"初始化一个新实例"的语义。以前这里用的是 store->Open()，它带
  // SQLITE_OPEN_CREATE：路径写错时 SQLite 会悄悄建一个空库，于是"这个实例还
  // 没有用户"和"你看的是另一个实例"在屏幕上完全一样——人工验收因此得出了
  // "ECS 上没有任何用户"的错误结论（而 GUI 显示的 Wjy 已登录其实是真的）。
  //
  // 打开方式由命令决定，不共用一个"能读也能写"的连接：
  //   * 只读命令走 SQLITE_OPEN_READONLY：backup-server 正在运行也能安全查询，
  //     而且这条连接根本写不了库（以前它会在打开时 EnsureSchema——BEGIN
  //     IMMEDIATE + CREATE TABLE + PRAGMA user_version，那是写事务）；
  //   * 破坏性命令走可写连接：调用方**已经**先拿到数据目录锁，证明服务端已停止。
  // 两条路都不 CREATE、不建表。
  const bool opened =
      writable
          ? store->OpenExistingReadWrite(options.database_path, error_message)
          : store->OpenExistingReadOnly(options.database_path, error_message);
  if (!opened) {
    return -1;
  }
  *maintenance = RemoteMaintenance(store, options.root_directory);
  if (!maintenance->ready()) {
    *error_message = "维护层没有配置好";
    return -1;
  }
  return 0;
}

// 首页那一行指纹。成功时它**就是**客户端要填进"服务器身份指纹（server-key）"
// 的那一串（sha256:<64 位十六进制>），用户可以直接抄。
void PrintTransportFingerprintLine(const Options& options) {
  const TransportIdentityReport identity =
      LoadTransportIdentityForDisplay(options);
  if (identity.available) {
    std::printf("传输身份指纹: sha256:%s\n", identity.fingerprint.c_str());
    std::printf("              来源: %s\n", identity.source.c_str());
    return;
  }
  // 读不到指纹不是"管理工具坏了"：菜单、用户列表、存储概览都必须照常。
  std::printf("传输身份指纹: 不可用\n");
  std::printf("              原因: %s\n", identity.error.c_str());
  std::printf(
      "              期望的身份私钥: %s（可用 --transport-key-file "
      "显式指定）\n",
      AbsolutePath(identity.key_path).c_str());
}

// 每次运行都先把"我在看哪个实例"说清楚：主机、部署根、数据根、元数据库、
// 服务状态（含 PID）、传输身份指纹。人工验收时这一块必须和下面的列表出现在
// 同一屏里——脱离上下文的"还没有任何用户"是这次 P0 的直接诱因。
//
// 传输身份指纹（PR #21 UI closure，问题 A）放在这里就等于放在菜单首页上：
// backup-server-admin.sh 的 banner 调用的正是 status。它只读、只显示公钥指纹，
// 读不到也只是一行"不可用 + 原因"。
void PrintIdentity(const Options& options, bool lock_held_by_this_run) {
  const std::string data_root = AbsolutePath(options.root_directory);
  std::printf("Host:        %s\n", HostName().c_str());
  std::printf("Server root: %s\n", ServerRootOf(options).c_str());
  std::printf("Data root:   %s\n", data_root.c_str());
  std::printf("Metadata DB: %s\n", AbsolutePath(options.database_path).c_str());
  std::string state;
  if (lock_held_by_this_run) {
    // 破坏性操作会**持有**数据目录锁（见 AcquireDestructiveLock），这时候再去
    // 试探这把锁一定是 busy——而 busy 的正是本次操作自己拿的锁。直接说清楚：
    // 谎称"backup-server 正在运行"会让用户以为自己的服务端没停干净。
    state = "未运行（数据目录锁由本次管理操作持有）";
  } else {
    DescribeServerState(data_root, &state);
  }
  std::printf("Service:     %s\n", state.c_str());
  PrintTransportFingerprintLine(options);
}

// 服务器身份信息页：fingerprint + public key。**永远不打印私钥**：这里读的是
// LoadTransportIdentity 解出来的公钥，私钥那份材料在函数返回前就被清零。
int CommandTransportIdentity(const Options& options) {
  const TransportIdentityReport identity =
      LoadTransportIdentityForDisplay(options);
  std::printf("BPSEC1 Server Identity\n");
  std::printf("身份私钥文件: %s\n", AbsolutePath(identity.key_path).c_str());
  std::printf("来源:         %s\n", identity.source.c_str());
  std::printf("\n");
  if (!identity.available) {
    std::printf("Fingerprint:  不可用\n");
    std::printf("Public key:   不可用\n");
    std::fprintf(stderr, "读取传输身份失败：%s\n", identity.error.c_str());
    std::fprintf(stderr,
                 "提示：用 --transport-key-file <文件> 指定这个实例的身份私钥；"
                 "本命令不会创建或修改任何密钥。\n");
    return 1;
  }
  std::printf("Fingerprint:\n  sha256:%s\n\n", identity.fingerprint.c_str());
  std::printf("Public key:\n  hex:%s\n\n", identity.public_key_hex.c_str());
  std::printf(
      "客户端 pin（二选一，填进客户端的“服务器身份指纹（server-key）”）:\n");
  std::printf("  sha256:%s\n", identity.fingerprint.c_str());
  std::printf("  hex:%s\n\n", identity.public_key_hex.c_str());
  std::printf(
      "提示：把上面的 Fingerprint 复制到 Modern GUI 的“服务器身份指纹"
      "（server-key）”，点“应用”之后再登录或注册。\n");
  std::printf(
      "私钥不会显示：本命令只读公钥与指纹，不打印私钥内容，也不写任何文件。\n");
  return 0;
}

// 统计块。实例身份由 main()
// 统一在最前面打印一次：这一版**不再**在这里重复打印， 否则
// status（菜单首页就是它）会把身份块显示两遍。
void PrintStatus(RemoteMetadataStore* store) {
  RemoteStorageOverview overview;
  std::string error;
  if (store->StorageOverview(&overview, &error) == StoreResult::kOk) {
    std::printf(
        "用户数：%llu　快照数：%llu　blob 总大小：%s　已注销账户：%llu\n",
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
  if (!ResolveUser(store, selector, /*require_explicit=*/false, &user_id,
                   &username, &error)) {
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
  if (!ResolveUser(store, selector, /*require_explicit=*/false, &user_id,
                   &username, &error)) {
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
                        const std::string& snapshot_id_text) {
  // 允许写成 snapshot:<32 位十六进制>：前缀只是为了和用户选择器看起来一致，
  // 去掉之后仍然按 32 位十六进制校验，规则不放松。
  const std::string snapshot_id = StripSnapshotPrefix(snapshot_id_text);
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
        std::printf(
            "  %-24s %10s（%llu 个备份）\n", users[index].username.c_str(),
            FormatSize(users[index].total_bytes).c_str(),
            static_cast<unsigned long long>(users[index].snapshot_count));
      }
    }
  }
  return 0;
}

int CommandDeleteSnapshot(RemoteMetadataStore* store,
                          RemoteMaintenance* maintenance,
                          const Options& options,
                          const std::string& snapshot_id_text,
                          std::string* error_message) {
  const std::string snapshot_id = StripSnapshotPrefix(snapshot_id_text);
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
  if (!ResolveUser(store, options.user_selector, /*require_explicit=*/true,
                   &user_id, &username, error_message)) {
    return 1;
  }
  RemoteSnapshotRecord removed;
  std::string delete_error;
  const StoreResult result = maintenance->DeleteSnapshot(
      user_id, snapshot_id, &removed, &delete_error);
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
                      RemoteMaintenance* maintenance, const Options& options,
                      const std::string& selector, std::string* error_message) {
  std::int64_t user_id = 0;
  std::string username;
  if (!ResolveUser(store, selector, /*require_explicit=*/true, &user_id,
                   &username, error_message)) {
    return 1;
  }
  // 二次确认必须写出完整的 "DELETE <用户名>#<编号>"：一个 y 键按不出不可逆的
  // 删除，而且用户名与编号都要出现——同名的两个账户在看这一行的时候就能分清。
  const std::string expected = "DELETE " + username + "#" +
                               std::to_string(static_cast<long long>(user_id));
  if (options.confirm != expected) {
    *error_message =
        "确认字符串不正确。这会不可逆地删除账户 \"" + username +
        "\"（id=" + std::to_string(static_cast<long long>(user_id)) +
        "）以及它的全部云端备份，请加 --confirm \"" + expected + "\"。";
    return 1;
  }
  std::uint64_t removed_snapshots = 0;
  std::uint64_t removed_bytes = 0;
  std::string delete_error;
  const StoreResult result = maintenance->DeleteAccount(
      user_id, &removed_snapshots, &removed_bytes, &delete_error);
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

  const bool destructive =
      options.command == "delete-snapshot" || options.command == "delete-user";

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
  if (OpenAll(options, destructive, &store, &maintenance, &open_error) != 0) {
    std::fprintf(stderr,
                 "ERROR: 未找到服务器状态数据库：\n"
                 "  %s\n"
                 "\n"
                 "请确认正在管理正确的 backup-server 实例：\n"
                 "  Server root: %s\n"
                 "  Data root:   %s\n"
                 "  Metadata DB: %s\n"
                 "\n"
                 "（原因：%s）\n"
                 "管理工具只读取**已经存在**的数据库，不会创建空库。\n",
                 AbsolutePath(options.database_path).c_str(),
                 ServerRootOf(options).c_str(),
                 AbsolutePath(options.root_directory).c_str(),
                 AbsolutePath(options.database_path).c_str(),
                 open_error.c_str());
    return 1;
  }
  // 每一条命令都先打印实例身份，再打印结果。
  // destructive 为真时锁已经在本次进程手里：身份块要如实这么说，而不是
  // 把"自己持有锁"显示成"backup-server 正在运行"。
  PrintIdentity(options, destructive);
  std::printf("\n");
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
    PrintStatus(&store);
    return 0;
  }
  if (options.command == "list-users") {
    return CommandListUsers(&store);
  }
  if (options.command == "overview") {
    return CommandOverview(&store, options.root_directory);
  }
  // 服务器身份信息：只读，和 status 一样在服务端运行时可用。
  if (options.command == "transport-identity" ||
      options.command == "show-identity") {
    return CommandTransportIdentity(options);
  }
  if (options.command == "show-user" || options.command == "list-snapshots") {
    std::string selector;
    if (!require_argument(0, &selector)) {
      std::fprintf(stderr, "错误：%s 需要一个用户参数。\n",
                   options.command.c_str());
      return 2;
    }
    return options.command == "show-user"
               ? CommandShowUser(&store, selector)
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
    const int status = CommandDeleteSnapshot(&store, &maintenance, options,
                                             snapshot_id, &error);
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
