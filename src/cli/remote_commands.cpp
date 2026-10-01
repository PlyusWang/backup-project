// src/cli/remote_commands.cpp

#include "remote_commands.h"

#include <libgen.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>
#include <vector>

#include "incremental_backup.h"
#include "remote_backup_client.h"
#include "terminal_secret.h"

namespace backupproject {
namespace {

namespace net = backupproject::net;

// 命令行的解析结果。endpoint 与身份分开，便于 "--host/--port 出现在任何位置"。
struct RemoteOptions {
  net::RemoteEndpoint endpoint;
  std::string username;
  std::string display_name;
  std::string repository_directory;
  // 注销账户的二次确认：必须逐字等于 --user 给的用户名。
  std::string confirm_username;
  bool force = false;
  std::vector<std::string> positional;
};

void PrintRemoteUsageTo(std::ostream& output) {
  output << "  backupctl remote ping [--host <地址>] [--port <端口>]\n"
            "  backupctl remote register --user <用户名> [--host] [--port]\n"
            "  backupctl remote login --user <用户名> [--host] [--port]\n"
            "  backupctl remote list --user <用户名> [--host] [--port]\n"
            "  backupctl remote upload <本地归档> --user <用户名>\n"
            "      [--name <显示名>] [--repository <仓库目录>]\n"
            "  backupctl remote download <快照ID> <目标路径> --user <用户名>\n"
            "      [--force]\n"
            "  backupctl remote delete <快照ID> --user <用户名>\n"
            "  backupctl remote delete-account --user <用户名> --confirm "
            "<用户名>\n"
            "      （永久删除该账户与它的全部云端备份，不可撤销）\n"
            "  口令只从终端读取；自动测试用 BACKUP_REMOTE_PASSWORD 提供，\n"
            "  两者都不会被打印。默认端点 127.0.0.1:18765。\n";
}

bool TakeValue(const std::vector<std::string>& arguments, std::size_t* index,
               const std::string& name, std::string* value,
               std::string* error_message) {
  if (*index + 1 >= arguments.size()) {
    *error_message = name + " 需要一个值";
    return false;
  }
  if (!value->empty()) {
    *error_message = name + " 出现了不止一次";
    return false;
  }
  *value = arguments[*index + 1];
  *index += 1;
  return true;
}

bool ParseOptions(const std::vector<std::string>& arguments,
                  RemoteOptions* options, std::string* error_message) {
  std::string host;
  std::string port;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const std::string& token = arguments[index];
    if (token == "--host") {
      if (!TakeValue(arguments, &index, token, &host, error_message)) {
        return false;
      }
    } else if (token == "--port") {
      if (!TakeValue(arguments, &index, token, &port, error_message)) {
        return false;
      }
    } else if (token == "--user") {
      if (!TakeValue(arguments, &index, token, &options->username,
                     error_message)) {
        return false;
      }
    } else if (token == "--name") {
      if (!TakeValue(arguments, &index, token, &options->display_name,
                     error_message)) {
        return false;
      }
    } else if (token == "--repository") {
      if (!TakeValue(arguments, &index, token, &options->repository_directory,
                     error_message)) {
        return false;
      }
    } else if (token == "--confirm") {
      if (!TakeValue(arguments, &index, token, &options->confirm_username,
                     error_message)) {
        return false;
      }
    } else if (token == "--force") {
      options->force = true;
    } else if (token.size() > 2 && token[0] == '-' && token[1] == '-') {
      *error_message = "未知选项 " + token;
      return false;
    } else {
      options->positional.push_back(token);
    }
  }
  if (!host.empty()) {
    options->endpoint.host = host;
  }
  if (!port.empty()) {
    std::uint64_t value = 0;
    for (const char character : port) {
      if (character < '0' || character > '9') {
        *error_message = "--port 必须是数字";
        return false;
      }
      value = value * 10 + static_cast<std::uint64_t>(character - '0');
      if (value > 65535) {
        *error_message = "--port 必须在 0..65535 之间";
        return false;
      }
    }
    if (value == 0) {
      *error_message = "--port 不能是 0";
      return false;
    }
    options->endpoint.port = static_cast<std::uint16_t>(value);
  }
  return true;
}

// 口令来源优先级：测试专用的环境变量 -> 终端交互。
// 两者都不打印口令；命令行里没有 --password 这条路径。
bool ObtainPassword(const std::string& username, bool confirm,
                    std::string* password, std::string* error_message) {
  const char* from_environment = std::getenv("BACKUP_REMOTE_PASSWORD");
  if (from_environment != nullptr && from_environment[0] != '\0') {
    *password = from_environment;
    return true;
  }
  const std::string prompt = "请输入远程备份口令（" + username + "）: ";
  if (confirm) {
    return ReadSecretFromTerminalTwice(prompt, "请再输入一次: ", password,
                                       error_message);
  }
  return ReadSecretFromTerminal(prompt, password, error_message);
}

std::string BaseName(const std::string& path) {
  const std::size_t slash = path.find_last_of('/');
  if (slash == std::string::npos) {
    return path;
  }
  return path.substr(slash + 1);
}

std::string FormatTime(std::uint64_t seconds) {
  const std::time_t when = static_cast<std::time_t>(seconds);
  std::tm parts;
  std::memset(&parts, 0, sizeof(parts));
  if (::localtime_r(&when, &parts) == nullptr) {
    return "-";
  }
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &parts);
  return std::string(buffer);
}

void PrintProgress(const net::RemoteTransferProgress& progress) {
  if (progress.bytes_total == 0) {
    return;
  }
  const int percent =
      static_cast<int>((progress.bytes_done * 100) / progress.bytes_total);
  std::fprintf(stderr, "[remote] %s %llu/%llu 字节（%d%%）\n",
               progress.phase.c_str(),
               static_cast<unsigned long long>(progress.bytes_done),
               static_cast<unsigned long long>(progress.bytes_total), percent);
}

// 连接（并在需要时登录）。失败时打印原因并返回 false。
bool ConnectAndLogin(const RemoteOptions& options, bool need_login,
                     net::RemoteArchiveClient* client,
                     std::string* error_message) {
  if (!client->Connect(options.endpoint, error_message)) {
    return false;
  }
  if (!need_login) {
    return true;
  }
  if (options.username.empty()) {
    *error_message = "这个命令需要 --user";
    return false;
  }
  std::string password;
  if (!ObtainPassword(options.username, /*confirm=*/false, &password,
                      error_message)) {
    return false;
  }
  const bool ok = client->Login(options.username, password, error_message);
  // 口令用完立刻丢掉，不留在栈上更久。
  password.assign(password.size(), '\0');
  password.clear();
  return ok;
}

int Fail(const std::string& message) {
  std::cerr << "Error: " << message << "\n";
  return kCliExitOperationFailed;
}

}  // namespace

void PrintRemoteUsage(std::ostream& output) { PrintRemoteUsageTo(output); }

int RunRemoteCommand(const CliContext& context,
                     const std::vector<std::string>& arguments) {
  (void)context;
  if (arguments.empty()) {
    std::cerr << "Error: remote 需要一个子命令。\n\n";
    PrintRemoteUsageTo(std::cerr);
    return kCliExitUsageError;
  }
  const std::string subcommand = arguments[0];
  const std::vector<std::string> rest(arguments.begin() + 1, arguments.end());

  RemoteOptions options;
  std::string parse_error;
  if (!ParseOptions(rest, &options, &parse_error)) {
    std::cerr << "Error: " << parse_error << "\n\n";
    PrintRemoteUsageTo(std::cerr);
    return kCliExitUsageError;
  }

  net::RemoteArchiveClient client;
  std::string error;

  if (subcommand == "ping") {
    if (!options.positional.empty()) {
      std::cerr << "Error: remote ping 不接受位置参数。\n";
      return kCliExitUsageError;
    }
    if (!client.Connect(options.endpoint, &error)) {
      return Fail(error);
    }
    std::string software;
    std::uint16_t version = 0;
    std::uint64_t server_time = 0;
    if (!client.Ping(&software, &version, &server_time, &error)) {
      return Fail(error);
    }
    std::cout << "PING 正常: " << software << " 协议版本 " << version
              << " 服务端时间 " << FormatTime(server_time) << "\n";
    std::cout << "端点: " << options.endpoint.host << ":"
              << options.endpoint.port << "\n";
    return kCliExitSuccess;
  }

  if (subcommand == "register") {
    if (!options.positional.empty()) {
      std::cerr << "Error: remote register 不接受位置参数。\n";
      return kCliExitUsageError;
    }
    if (options.username.empty()) {
      std::cerr << "Error: remote register 需要 --user。\n";
      return kCliExitUsageError;
    }
    if (!client.Connect(options.endpoint, &error)) {
      return Fail(error);
    }
    std::string password;
    if (!ObtainPassword(options.username, /*confirm=*/true, &password,
                        &error)) {
      return Fail(error);
    }
    const bool ok = client.Register(options.username, password, &error);
    password.assign(password.size(), '\0');
    password.clear();
    if (!ok) {
      return Fail(error);
    }
    std::cout << "已注册用户 " << options.username
              << "（口令不会被打印或保存）\n";
    return kCliExitSuccess;
  }

  if (subcommand == "login") {
    if (!options.positional.empty()) {
      std::cerr << "Error: remote login 不接受位置参数。\n";
      return kCliExitUsageError;
    }
    if (!ConnectAndLogin(options, /*need_login=*/true, &client, &error)) {
      return Fail(error);
    }
    // token 只在这个进程里存在，命令结束即丢弃。
    std::cout << "登录成功: " << options.username << " @ "
              << options.endpoint.host << ":" << options.endpoint.port << "\n";
    return kCliExitSuccess;
  }

  if (subcommand == "list") {
    if (!options.positional.empty()) {
      std::cerr << "Error: remote list 不接受位置参数。\n";
      return kCliExitUsageError;
    }
    if (!ConnectAndLogin(options, /*need_login=*/true, &client, &error)) {
      return Fail(error);
    }
    std::vector<net::RemoteSnapshotInfo> snapshots;
    if (!client.List(&snapshots, &error)) {
      return Fail(error);
    }
    std::cout << "共 " << snapshots.size() << " 个远程快照\n";
    std::printf("%-34s %-12s %-20s %-14s %s\n", "SNAPSHOT_ID", "SIZE",
                "CREATED", "SHA256", "NAME");
    for (const net::RemoteSnapshotInfo& info : snapshots) {
      std::printf("%-34s %-12llu %-20s %-14s %s\n", info.snapshot_id.c_str(),
                  static_cast<unsigned long long>(info.size_bytes),
                  FormatTime(info.created_at).c_str(),
                  info.sha256.substr(0, 12).c_str(), info.display_name.c_str());
    }
    return kCliExitSuccess;
  }

  if (subcommand == "upload") {
    if (options.positional.size() != 1) {
      std::cerr << "Error: remote upload 需要一个本地归档路径。\n";
      return kCliExitUsageError;
    }
    const std::string local_path = options.positional[0];
    if (!ConnectAndLogin(options, /*need_login=*/true, &client, &error)) {
      return Fail(error);
    }
    // 如果调用方给了仓库目录，就先用产品自己的身份校验器证明这份归档
    // 确实是"实际字节与声明一致"的快照，再上传。没有给仓库目录时只做
    // 文件级校验（普通文件、非空、长度与 SHA-256 由传输本身保证）。
    if (!options.repository_directory.empty()) {
      SnapshotIdentity identity;
      std::string verify_error;
      if (!LoadVerifiedSnapshotIdentity(options.repository_directory,
                                        BaseName(local_path), &identity,
                                        nullptr, &verify_error)) {
        return Fail("归档身份校验失败，拒绝上传: " + verify_error);
      }
      std::cout << "[remote] 归档身份已校验（payload SHA-256 "
                << identity.payload_sha256.substr(0, 16) << "...）\n";
    }
    const std::string display_name = options.display_name.empty()
                                         ? BaseName(local_path)
                                         : options.display_name;
    net::RemoteSnapshotInfo uploaded;
    if (!client.UploadArchiveFile(local_path, display_name, PrintProgress,
                                  &uploaded, &error)) {
      return Fail(error);
    }
    std::cout << "上传完成\n";
    std::cout << "  快照 ID: " << uploaded.snapshot_id << "\n";
    std::cout << "  显示名:  " << uploaded.display_name << "\n";
    std::cout << "  长度:    " << uploaded.size_bytes << " 字节\n";
    std::cout << "  SHA-256: " << uploaded.sha256 << "\n";
    return kCliExitSuccess;
  }

  if (subcommand == "download") {
    if (options.positional.size() != 2) {
      std::cerr << "Error: remote download 需要 <快照ID> <目标路径>。\n";
      return kCliExitUsageError;
    }
    const std::string snapshot_id = options.positional[0];
    const std::string target = options.positional[1];
    if (!ConnectAndLogin(options, /*need_login=*/true, &client, &error)) {
      return Fail(error);
    }
    net::RemoteSnapshotInfo downloaded;
    if (!client.DownloadArchiveFile(snapshot_id, target, options.force,
                                    PrintProgress, &downloaded, &error)) {
      return Fail(error);
    }
    std::cout << "下载完成: " << target << "\n";
    std::cout << "  快照 ID: " << downloaded.snapshot_id << "\n";
    std::cout << "  长度:    " << downloaded.size_bytes << " 字节\n";
    std::cout << "  SHA-256: " << downloaded.sha256 << "\n";
    return kCliExitSuccess;
  }

  if (subcommand == "delete") {
    if (options.positional.size() != 1) {
      std::cerr << "Error: remote delete 需要一个快照 ID。\n";
      return kCliExitUsageError;
    }
    if (!ConnectAndLogin(options, /*need_login=*/true, &client, &error)) {
      return Fail(error);
    }
    if (!client.Delete(options.positional[0], &error)) {
      return Fail(error);
    }
    std::cout << "已删除远程快照 " << options.positional[0] << "\n";
    return kCliExitSuccess;
  }

  if (subcommand == "delete-account") {
    if (!options.positional.empty()) {
      std::cerr << "Error: remote delete-account 不接受位置参数。\n";
      return kCliExitUsageError;
    }
    if (options.username.empty()) {
      std::cerr << "Error: remote delete-account 需要 --user。\n";
      return kCliExitUsageError;
    }
    // 二次确认必须逐字给出账户名：注销是不可撤销的服务端删除，不能是
    // "回车一下"就完成的事。
    if (options.confirm_username != options.username) {
      std::cerr << "Error: 需要 --confirm " << options.username
                << " 才能注销账户（这会永久删除该账户与它的全部云端备份）。\n";
      return kCliExitUsageError;
    }
    if (!client.Connect(options.endpoint, &error)) {
      return Fail(error);
    }
    // 这里刻意不走 ConnectAndLogin：注销**必须**把同一个口令再交给服务端
    // 校验一次，所以口令要在手里多留一会儿，用完立刻擦掉。
    std::string password;
    if (!ObtainPassword(options.username, /*confirm=*/false, &password,
                        &error)) {
      return Fail(error);
    }
    bool ok = client.Login(options.username, password, &error);
    if (ok) {
      ok = client.DeleteAccount(password, &error);
    }
    password.assign(password.size(), '\0');
    password.clear();
    if (!ok) {
      return Fail(error);
    }
    std::cout << "已注销账户 " << options.username
              << "（服务端已删除该账户以及它的全部云端备份，此操作不可撤销）\n";
    return kCliExitSuccess;
  }

  std::cerr << "Error: 未知的 remote 子命令 '" << subcommand << "'。\n\n";
  PrintRemoteUsageTo(std::cerr);
  return kCliExitUsageError;
}

}  // namespace backupproject
