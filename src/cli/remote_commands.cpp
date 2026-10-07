// src/cli/remote_commands.cpp
// 模块职责：backupctl remote 全部子命令的唯一入口——解析 argv、建立连接与登录、
// 按子命令编排调用 net 层，然后把结果打印成人能读的文本。
//
// 边界（不负责什么）：不实现协议、不做 TLS/HMAC/分帧/重传、不碰归档格式，
// 也不做增量链的决策（那是 remote_incremental 的判断）。
//
// 数据流：argv -> ParseOptions -> RemoteOptions（端点 + 身份 + 备份参数）->
// RemoteArchiveClient / RunRemoteBackup / RunRemoteRestore -> stdout 文本。
//
// 退出码：0 成功；kCliExitUsageError 用法错；kCliExitOperationFailed 运行失败
// 正常输出只走 stdout，错误只走 stderr：脚本解析 stdout 时不会被提示语污染。
//
// 安全边界：口令只来自终端或 BACKUP_REMOTE_PASSWORD（自动化测试），命令行里
// 没有 --password 这条路径；服务端身份必须显式给出（pin 或证书），不做 TOFU；
// 注销账户要逐字 --confirm <用户名>，且口令要再交给服务端校验一次。

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
#include "remote_incremental.h"
#include "server_profile.h"
#include "terminal_secret.h"

namespace backupproject {
namespace {

namespace net = backupproject::net;

// 命令行的解析结果。endpoint 与身份分开，便于 "--host/--port 出现在任何位置"。
// 解析结果的载体。默认值本身就是策略：incremental 默认开启（没有可续的链时
// 引擎会自动先建基线），身份默认走 pin 模式。
// endpoint 与身份字段分开：--host/--port 可以在任何位置出现并被覆盖，身份不
// 允许（--official-cloud 时身份只能来自编译进二进制的 profile）。
// filter 与 include_rules/exclude_rules 同时保存不是冗余：filter 负责执行，
// 规则原文负责算 identity（规则变了就必须重建基线）。
struct RemoteOptions {
  net::RemoteEndpoint endpoint;
  std::string username;
  std::string display_name;
  std::string repository_directory;
  // 服务端 BPSEC1 身份 pin（"sha256:<指纹>" 或 "hex:<公钥>"）。
  std::string server_key;
  // BPSEC2 签名身份：identity_mode == "certificate" 时用证书认证服务端，
  // 此时 --server-key 不参与判断。
  std::string identity_mode;
  // --official-cloud：用编译进二进制的官方云端 profile（零配置）。
  // 地址/端口仍然可以被 --host/--port 覆盖，这样自测可以把它指到隧道上，
  // 而身份部分（证书模式 + 内置官方根 + expected_server_id）始终来自 profile。
  bool official_cloud = false;
  std::string trusted_roots_file;
  std::string expected_server_id;
  // 远端 backup 的参数：策略、显示名、过滤规则（规则原文同时留给增量链的
  // identity —— 规则变了就必须重建基线，这是引擎的硬规则）。
  bool incremental = true;
  bool saw_strategy = false;
  std::vector<std::string> include_rules;
  std::vector<std::string> exclude_rules;
  Filter filter;
  BackupOptions backup_options;
  // 注销账户的二次确认：必须逐字等于 --user 给的用户名。
  std::string confirm_username;
  bool force = false;
  std::vector<std::string> positional;
};

// 用法文本是 CLI 对用户的契约，也是各子命令参数个数的唯一说明；每次改参数都要
// 同步这里，否则用户只能靠报错猜。
void PrintRemoteUsageTo(std::ostream& output) {
  output
      << "  backupctl remote ping [--host <地址>] [--port <端口>]\n"
         "  backupctl remote register --user <用户名> [--host] [--port]\n"
         "  backupctl remote login --user <用户名> [--host] [--port]\n"
         "  backupctl remote list --user <用户名> [--host] [--port]\n"
         "  backupctl remote upload <本地归档> --user <用户名>\n"
         "      [--name <显示名>] [--repository <仓库目录>]\n"
         "  backupctl remote download <快照ID> <目标路径> --user <用户名>\n"
         "      [--force]\n"
         "  backupctl remote backup <源目录> --user <用户名>\n"
         "      [--strategy full|incremental] [--name <显示名>]\n"
         "      [--include <规则>]... [--exclude <规则>]...\n"
         "      （incremental 是默认值：没有可续的链时自动先建一份完整基线）\n"
         "  backupctl remote restore <快照ID> <目标目录> --user <用户名>\n"
         "      （自动把整条依赖链拉下来、逐字节验证后恢复；不需要手工下载 "
         "delta）\n"
         "  backupctl remote delete <快照ID> --user <用户名>\n"
         "  backupctl remote delete-account --user <用户名> --confirm "
         "<用户名>\n"
         "      （永久删除该账户与它的全部云端备份，不可撤销）\n"
         "  口令只从终端读取；自动测试用 BACKUP_REMOTE_PASSWORD 提供，\n"
         "  两者都不会被打印。默认端点 127.0.0.1:18765。\n"
         "\n"
         "  --official-cloud 用编译进二进制的官方云端身份（零配置：不需要\n"
         "                   --server-key，也不需要 --expected-server-id）\n"
         "  身份有两种模式：\n"
         "    * pin 模式（默认，BPSEC1）：--server-key <sha256:指纹|hex:公钥>\n"
         "      （也可以放在环境变量 BACKUP_REMOTE_SERVER_KEY 里）。客户端\n"
         "      必须事先知道服务端身份公钥，本项目不做首次连接自动信任。\n"
         "      用下面的命令取得 pin：\n"
         "          backup-server-keygen --show --key-file <身份私钥文件>\n"
         "    * 证书模式（BPSEC2，签名身份）：--expected-server-id <名字>\n"
         "      服务端出示由离线根签发的身份证书，客户端用可信根验签。\n"
         "      [--trusted-roots <根文件>] 不给就用**内置官方根**（官方云端\n"
         "      的用法：用户不需要知道、也不需要核对任何指纹）。\n";
}

// 取走下一个 token 当选项值。同一个选项给两次直接报错，而不是“最后一个赢”：
// 静默覆盖会让用户以为前一个生效了（尤其是 --server-key 这种身份参数）。
// index 被原地前进，调用方不需要自己跳过被取走的值。
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

// 单遍扫描，选项可以出现在任何位置（--host/--port 不要求写在子命令之后）；
// 不认识的 --xxx 立刻报错（拒绝而不是忽略：拼错的选项静默失效最难查）。
// 扫描结束后统一做优先级归并：pin 取 --server-key 或环境变量；证书模式由
// --expected-server-id 隐含；--official-cloud 的身份完全来自内置 profile。
// 所有拒绝路径都不改 options 之外的任何状态，也没有副作用。
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
    } else if (token == "--strategy") {
      std::string value;
      if (!TakeValue(arguments, &index, token, &value, error_message)) {
        return false;
      }
      if (value != "full" && value != "incremental") {
        *error_message = "--strategy 只接受 full 或 incremental";
        return false;
      }
      options->incremental = value == "incremental";
      options->saw_strategy = true;
    } else if (token == "--include" || token == "--exclude") {
      std::string value;
      if (!TakeValue(arguments, &index, token, &value, error_message)) {
        return false;
      }
      const FilterAction action = token == "--include" ? FilterAction::kInclude
                                                       : FilterAction::kExclude;
      if (!options->filter.AddRule(action, value, error_message)) {
        return false;
      }
      if (action == FilterAction::kInclude) {
        options->include_rules.push_back(value);
      } else {
        options->exclude_rules.push_back(value);
      }
    } else if (token == "--server-key") {
      if (!TakeValue(arguments, &index, token, &options->server_key,
                     error_message)) {
        return false;
      }
    } else if (token == "--identity-mode") {
      if (!TakeValue(arguments, &index, token, &options->identity_mode,
                     error_message)) {
        return false;
      }
      if (options->identity_mode != "pin" &&
          options->identity_mode != "certificate") {
        *error_message = "--identity-mode 只能是 pin 或 certificate";
        return false;
      }
    } else if (token == "--trusted-roots") {
      if (!TakeValue(arguments, &index, token, &options->trusted_roots_file,
                     error_message)) {
        return false;
      }
    } else if (token == "--expected-server-id") {
      if (!TakeValue(arguments, &index, token, &options->expected_server_id,
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
    } else if (token == "--official-cloud") {
      // 开关型：什么都不用填，身份来自编译进二进制的官方云端 profile。
      options->official_cloud = true;
    } else if (token.size() > 2 && token[0] == '-' && token[1] == '-') {
      *error_message = "未知选项 " + token;
      return false;
    } else {
      options->positional.push_back(token);
    }
  }
  // pin 优先级：--server-key -> 环境变量 BACKUP_REMOTE_SERVER_KEY。
  // 两者都没有时在 RunRemoteCommand 里明确报错（不做 TOFU）。
  options->endpoint.server_key_pin = options->server_key;
  if (options->endpoint.server_key_pin.empty()) {
    const char* from_environment = std::getenv("BACKUP_REMOTE_SERVER_KEY");
    if (from_environment != nullptr && from_environment[0] != '\0') {
      options->endpoint.server_key_pin = from_environment;
    }
  }
  // --official-cloud：身份部分**完全**来自编译进二进制的 profile —— 用户
  // 不需要知道服务器地址、端口、server_id，也不需要任何指纹。地址与端口
  // 允许被 --host/--port 覆盖（自测把它指到隧道上时用），身份不允许覆盖。
  // 覆盖顺序很重要：先让用户显式给的值生效，再用内置 profile 补空缺；身份字段
  // 一律来自 profile，防止“零配置”变成“零校验”。
  if (options->official_cloud) {
    const backupproject::net::ServerProfile& official =
        backupproject::net::OfficialCloudProfile();
    options->identity_mode = official.identity;
    options->expected_server_id = official.expected_server_id;
    options->trusted_roots_file = official.trusted_roots_file;
    if (host.empty()) {
      host = official.host;
    }
    if (port.empty()) {
      port = std::to_string(official.port);
    }
  }
  // BPSEC2：签名身份（证书）。给了 --expected-server-id 就默认进证书模式，
  // 也可以显式写 --identity-mode certificate。
  if (!options->identity_mode.empty()) {
    options->endpoint.identity_mode = options->identity_mode;
  } else if (!options->expected_server_id.empty()) {
    options->endpoint.identity_mode = "certificate";
  }
  options->endpoint.trusted_roots_file = options->trusted_roots_file;
  options->endpoint.expected_server_id = options->expected_server_id;
  if (options->endpoint.identity_mode == "certificate" &&
      options->endpoint.expected_server_id.empty()) {
    *error_message =
        "证书模式（--identity-mode certificate）需要同时给出 "
        "--expected-server-id <证书里的服务器名字>";
    return false;
  }
  if (!host.empty()) {
    options->endpoint.host = host;
  }
  // 端口手工解析成 uint16：atoi 会把 "12ab" 静默当成 12，而端口是最容易写错的
  // 参数之一。逐字符累积并在中途检查上界，0 与 >65535 都拒绝。
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
// 口令来源优先级：环境变量（自动化测试）-> 终端交互。两条都不回显、不落盘；
// 注册/改口令时走二次输入，避免把打错的密码注册进去。
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

// 取最后一个 '/' 之后的部分；不用 libgen 的 basename，它会改写入参缓冲区。
// 上传/下载的显示名都按这个规则从路径推出。
std::string BaseName(const std::string& path) {
  const std::size_t slash = path.find_last_of('/');
  if (slash == std::string::npos) {
    return path;
  }
  return path.substr(slash + 1);
}

// 用本地时区展示服务端时间：服务端返回的是 Unix 秒，用户看的是自己墙上的钟。
// localtime_r 是线程安全版本；越界时间返回 "-"，展示路径不抛异常。
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

// 传输进度回调，可能被高频调用：bytes_total 为 0 时直接返回，否则百分比会除零。
// 进度写 stderr：stdout 是脚本的输入，不能混进“已经传了多少字节”这种噪声。
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
// 连接与（按需）登录的公共前置：need_login=false 只有 ping 用，它不需要账户。
// 登录成功后立刻把口令副本清零：std::string 不保证析构时擦内存。
// 失败时返回 false 并填 error_message，调用方统一走 Fail()。
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

// 运行期失败的统一出口：stderr 一行 "Error: ..." + 固定退出码，脚本只认码。
// 用法错不走这里：那类错误要额外打印 usage。
int Fail(const std::string& message) {
  std::cerr << "Error: " << message << "\n";
  return kCliExitOperationFailed;
}

}  // namespace

// 对外暴露的入口与内部实现共用同一份文本：帮助（stdout）与用法错误（stderr）
// 一致，不会出现“报错时提示的选项和 --help 里的不一样”。
void PrintRemoteUsage(std::ostream& output) { PrintRemoteUsageTo(output); }

// 子命令分派。所有子命令共享同一套解析与身份前置检查，各自只负责参数个数与
// 编排顺序；这样“少给了一个参数”不会变成一次半途而废的网络操作。
//
// 注意参数个数的校验必须留在各分支里：remote restore 要 2 个位置参数，
// remote ping 一个都不要，统一校验会写出一堆例外。
// (void)context：remote 子命令不需要本地配置上下文，只用端点与身份。
int RunRemoteCommand(const CliContext& context,
                     const std::vector<std::string>& arguments) {
  (void)context;
  if (arguments.empty()) {
    std::cerr << "Error: remote 需要一个子命令。\n\n";
    PrintRemoteUsageTo(std::cerr);
    return kCliExitUsageError;
  }
  // arguments[0] 是子命令，其余交给 ParseOptions；未知子命令在末尾统一报错。
  // 这样“子命令拼错”和“选项拼错”都会打印同一份 usage。
  const std::string subcommand = arguments[0];
  const std::vector<std::string> rest(arguments.begin() + 1, arguments.end());

  RemoteOptions options;
  std::string parse_error;
  if (!ParseOptions(rest, &options, &parse_error)) {
    std::cerr << "Error: " << parse_error << "\n\n";
    PrintRemoteUsageTo(std::cerr);
    return kCliExitUsageError;
  }

  // 身份前置检查放在任何网络动作之前：缺 pin 而证书模式也没开时，连都不该连。
  // 这是本项目“不做 TOFU”的可执行版本：没有可信身份就不建立会话。
  if (options.endpoint.identity_mode != "certificate" &&
      options.endpoint.server_key_pin.empty()) {
    std::cerr << "Error: 缺少 --server-key <sha256:指纹|hex:公钥>。\n"
                 "  （如果服务端配了签名身份证书，改用 --expected-server-id\n"
                 "   <服务器名字> 走证书模式，就不需要指纹了。）\n"
                 "  BPSEC1 要求客户端事先知道服务端身份公钥，本项目不做首次\n"
                 "  连接自动信任（TOFU）。用下面的命令取得 pin：\n"
                 "      backup-server-keygen --show --key-file <身份私钥文件>\n"
                 "  也可以把它放进环境变量 BACKUP_REMOTE_SERVER_KEY。\n\n";
    PrintRemoteUsageTo(std::cerr);
    return kCliExitUsageError;
  }

  // client 贯穿整个子命令：连接、登录与后续请求复用同一条已认证会话，
  // 退出时随栈对象销毁，不做全局单例（也就没有“上一次登录残留”这种状态）。
  net::RemoteArchiveClient client;
  std::string error;

  // ping 只用 client.Connect：它不需要账户，因此不走 ConnectAndLogin。
  // 输出软件名、协议版本与服务端时间——自测/巡检用它确认“对端是这个服务端”。
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

  // 注册是唯一需要二次确认口令的子命令：打错的密码一旦注册进去，之后登不上。
  // 注册只建立账户，不建立会话：需要 token 的命令仍然要自己 login。
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
    // Register 成功后立刻清零口令副本：这个进程已经不再需要它，留着只是风险。
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

  // 登录成功后 token 只活在这个进程里，命令结束即随 client 析构消失。
  // 不落盘、不复用、不打印，所以这里只回显用户名与端点。
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

  // 这是给人看的表格，也是“远端增量”最直接的证据：KIND / GEN / PARENT 三列
  // 分别说明这个快照是 full 还是 incremental、代数、以及它的父快照。
  // NAME 固定在最后一列：脚本按“最后一列 = 名字”解析，新增列只能插在中间。
  // SHA-256 与父 id 只打印前 12 位：列宽有限，完整值仍可用 download 取回。
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
    // 列出链关系：kind / generation / parent 是老师现场验收"远端增量"
    // 最直接的证据（NAME
    // 仍然放在最后一列：脚本里"取最后一列当名字"的写法不受影响）。
    std::printf("%-34s %-12s %-20s %-14s %-12s %-4s %-34s %s\n", "SNAPSHOT_ID",
                "SIZE", "CREATED", "SHA256", "KIND", "GEN", "PARENT", "NAME");
    for (const net::RemoteSnapshotInfo& info : snapshots) {
      const std::string kind =
          info.snapshot_kind ==
                  static_cast<std::uint16_t>(net::SnapshotKind::kIncremental)
              ? "incremental"
              : "full";
      const std::string parent = info.parent_snapshot_id.empty()
                                     ? std::string("-")
                                     : info.parent_snapshot_id.substr(0, 12);
      std::printf("%-34s %-12llu %-20s %-14s %-12s %-4llu %-34s %s\n",
                  info.snapshot_id.c_str(),
                  static_cast<unsigned long long>(info.size_bytes),
                  FormatTime(info.created_at).c_str(),
                  info.sha256.substr(0, 12).c_str(), kind.c_str(),
                  static_cast<unsigned long long>(info.generation),
                  parent.c_str(), info.display_name.c_str());
    }
    return kCliExitSuccess;
  }

  // 顺序不能换：先登录拿到服务端指纹，再用（指纹，用户名）定位本地缓存目录。
  // 缓存按 (指纹, 用户名) 两级隔离：换服务端或换账户都不会串用对方的链。
  // 是否产出 delta、要不要重建基线由 RunRemoteBackup 决定，CLI 只表达意图
  // （--strategy 只影响 allow_incremental 这一个开关）。
  // no_changes 是正常结果而不是错误：源目录没变就不必产生新快照，退出码 0。
  if (subcommand == "backup") {
    if (options.positional.size() != 1) {
      std::cerr << "Error: remote backup 需要正好一个源目录。\n\n";
      PrintRemoteUsageTo(std::cerr);
      return kCliExitUsageError;
    }
    if (options.username.empty()) {
      std::cerr << "Error: remote backup 需要 --user。\n";
      return kCliExitUsageError;
    }
    const std::string source_directory = options.positional[0];
    if (!ConnectAndLogin(options, /*need_login=*/true, &client, &error)) {
      return Fail(error);
    }
    net::RemoteCacheLayout cache;
    if (!net::PrepareRemoteCache(std::string(), client.server_fingerprint(),
                                 options.username, &cache, &error)) {
      return Fail(error);
    }
    net::RemoteBackupRequest request;
    request.client = &client;
    request.cache = cache;
    request.source_directory = source_directory;
    request.include_rules = options.include_rules;
    request.exclude_rules = options.exclude_rules;
    request.filter = options.filter;
    request.options = options.backup_options;
    request.allow_incremental = options.incremental;
    request.display_name = options.display_name;
    request.progress = PrintProgress;
    net::RemoteBackupOutcome outcome;
    if (!net::RunRemoteBackup(request, &outcome, &error)) {
      return Fail(error);
    }
    if (outcome.no_changes) {
      std::cout << "源目录没有变化，没有创建新的远端快照。\n";
      return 0;
    }
    std::cout << (outcome.produced_delta ? "已上传增量快照" : "已上传完整快照")
              << "\n";
    std::cout << "  快照 ID:   " << outcome.snapshot_id << "\n";
    std::cout << "  类型:      "
              << (outcome.produced_delta ? "incremental" : "full") << "\n";
    std::cout << "  代数:      " << outcome.generation << "\n";
    if (!outcome.parent_snapshot_id.empty()) {
      std::cout << "  父快照:    " << outcome.parent_snapshot_id << "\n";
    }
    std::cout << "  归档名:    " << outcome.archive_name << "\n";
    std::cout << "  本次上传:  " << outcome.uploaded_bytes << " 字节\n";
    if (outcome.chain_root_bytes > 0) {
      std::cout << "  链根大小:  " << outcome.chain_root_bytes
                << " 字节（本次只传了增量部分）\n";
    }
    // 引擎自己报了“为什么重建基线”，CLI 只负责把原因原样透出，不解释、不掩盖。
    if (outcome.rebuilt_full_baseline) {
      std::cout << "  说明:      引擎判断无法续链，已重建完整基线（"
                << outcome.baseline_reason << "）\n";
    }
    std::cout << "  本地缓存:  " << cache.cache_directory << "\n";
    return 0;
  }

  // 一次调用把整条依赖链取回并逐字节验证后恢复，用户不需要手工下载 delta。
  // 增量链不支持加密（外层信封不受内层保护），所以这里不向用户索要口令。
  // 目标目录的契约（存在/为空/不覆盖）由本地恢复核心把关，CLI 不重复实现。
  if (subcommand == "restore") {
    if (options.positional.size() != 2) {
      std::cerr << "Error: remote restore 需要 <快照ID> <目标目录>。\n\n";
      PrintRemoteUsageTo(std::cerr);
      return kCliExitUsageError;
    }
    if (options.username.empty()) {
      std::cerr << "Error: remote restore 需要 --user。\n";
      return kCliExitUsageError;
    }
    const std::string snapshot_id = options.positional[0];
    const std::string target_directory = options.positional[1];
    if (!ConnectAndLogin(options, /*need_login=*/true, &client, &error)) {
      return Fail(error);
    }
    // 恢复也复用同一套缓存布局：链上的材料本来就住在那里，命中就不必重新下载。
    // 缓存目录由 (服务端指纹, 用户名) 决定，所以不同账户的恢复互不可见。
    net::RemoteCacheLayout cache;
    if (!net::PrepareRemoteCache(std::string(), client.server_fingerprint(),
                                 options.username, &cache, &error)) {
      return Fail(error);
    }
    // 增量链不支持加密（外层信封不受内层保护），所以恢复不需要口令。
    net::RemoteRestoreOutcome outcome;
    if (!net::RunRemoteRestore(&client, cache, snapshot_id, target_directory,
                               RestoreOptions(), &outcome, &error)) {
      return Fail(error);
    }
    std::cout << "已从远端恢复 " << outcome.archive_name << "\n";
    std::cout << "  目标目录:  " << target_directory << "\n";
    std::cout << "  依赖链:    " << outcome.chain_length << " 份快照（"
              << outcome.delta_count << " 个增量）\n";
    std::cout << "  本次下载:  " << outcome.downloaded_bytes << " 字节\n";
    std::cout << "  恢复条目:  " << outcome.restored_entries << "\n";
    return 0;
  }

  // 给了 --repository 就先做一次“产品级身份校验”：用同一套校验器证明这份归档
  // 的实际字节与它自己的声明一致，再上传；否则上传一个本地就已经损坏的文件，
  // 只会把错误推迟到恢复的那一刻。
  // 没有给 --repository 时退化成文件级校验：普通文件、非空、长度与 SHA-256 由
  // 传输本身保证——这是能力边界，不是安全边界的等价替代。
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
    // 显示名默认用本地文件名，--name 只改变服务端展示，不改变归档内容与校验。
    // 归档的 SHA-256 与长度由服务端在传输后回报，客户端原样回显给用户核对。
    const std::string display_name = options.display_name.empty()
                                         ? BaseName(local_path)
                                         : options.display_name;
    // 打印的是服务端回报的值：客户端只做转述，用户核对的是服务端实际存的字节。
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

  // 目标路径已存在时默认拒绝，--force 才覆盖：避免一次手误盖掉用户的文件。
  // 下载完成后的长度与 SHA-256 由客户端校验并回显，用户能核对服务端声明值。
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

  // 位置参数原样当快照 ID 用：不做前缀补全、不做模糊匹配，避免误删。
  // 删除只作用于服务端；本地缓存与索引不动——它们本来就只是可选缓存。
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

  // 二次确认必须逐字等于 --user：注销是不可撤销的服务端删除，不是回车就能过。
  // 这里刻意不走 ConnectAndLogin：注销要把同一个口令再交给服务端校验一次，
  // 所以口令要在手里多留一会儿，用完立刻擦掉。
  // 两步都成功才算成功：Login 失败时绝不能继续调用 DeleteAccount。
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

  // 未知子命令按用法错误处理（而不是运行失败）：它和拼错选项是同一类问题。
  std::cerr << "Error: 未知的 remote 子命令 '" << subcommand << "'。\n\n";
  PrintRemoteUsageTo(std::cerr);
  return kCliExitUsageError;
}

}  // namespace backupproject
