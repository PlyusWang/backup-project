// server/main.cpp
//
// backup-server 的进程入口。
//
// 它只做四件事：解析参数、检查 PID 文件、把信号变成一次优雅停止、跑服务循环。
// 所有协议与存储逻辑都在 src/network/ 里，这个文件不碰 socket、不碰 SQLite。
//
// 与桌面前端的边界：backup-server 是**独立服务进程**，不参与 desktop 的
// 单实例锁（那把锁约束的是 backupctl / Classic GUI / Modern GUI 三个前端），
// 但它自己有 PID 文件：同一个 root/db/port 上不允许起第二个实例。
// 进程模型：单进程、多线程——main 线程只负责启动、等待与收尾，连接处理在
// RemoteServer 的 worker 线程里，所以这里的全局状态都遵循"启动前初始化、
// 停止后失效"。
//
// 安全边界：本文件只决定"监听参数是什么"。监听地址、证书与是否强制
// BPSEC2 的裁决在 RemoteServer::Configure 里——只有那里能看到密钥与
// 证书的加载结果，所以 --allow-public-bind 的"缺一不可"不在这里校验。
//
// 不变量：每个 root/db/port 组合最多一个活着的实例（PID 文件 + /proc 复核）；
// 只有 Run() 成功返回才可能返回 0；任何失败路径都不会留下 PID 文件。
// 失败语义：所有错误都走 stderr + 非 0 退出码，不抛异常——进程入口没有
// 上层可以承接异常，异常逃出 main 只会变成 std::terminate 与 core dump。

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "remote_server.h"

namespace {

// 信号处理器与主线程之间唯一的共享状态。处理器只调用 RequestStop()（写一个
// 原子标志），不碰其它成员；Run() 返回后主线程立刻清空它，避免停止流程里
// 再来的信号访问到已经析构的 server。
backupproject::net::RemoteServer* g_server = nullptr;

// 信号处理器里只做一次原子写：不分配、不打印、不碰文件系统。
void HandleStopSignal(int /*signal_number*/) {
  if (g_server != nullptr) {
    g_server->RequestStop();
  }
}

void PrintUsage(std::FILE* out, const char* program) {
  std::fprintf(
      out,
      "用法: %s [选项]\n"
      "\n"
      "  --bind <地址>           监听地址，本版本固定 127.0.0.1\n"
      "  --port <端口>           监听端口，默认 18765（0 = 由内核分配）\n"
      "  --root <目录>           blob 存储根目录\n"
      "  --db <文件>             SQLite 元数据库文件\n"
      "  --secret-file <文件>    含 BACKUP_TOKEN_SECRET 的 secrets.env\n"
      "  --transport-key-file <文件>\n"
      "                          BPSEC1 传输身份私钥（0600，32 字节）；必填，\n"
      "                          用 backup-server-keygen 生成\n"
      "  --bpsec2-cert-file <文件>\n"
      "                          服务器身份证书（BPCERT1，由离线根签发）；\n"
      "                          给出后客户端可以用签名身份（BPSEC2）连接，\n"
      "                          不再需要人工核对指纹\n"
      "  --require-bpsec2        只接受 BPSEC2（签名身份）客户端：收到 BPSEC1\n"
      "                          的握手直接拒绝，不做降级。需要同时给出证书\n"
      "  --allow-public-bind <理由>\n"
      "                          允许监听非回环地址（官方云端直连用）。默认\n"
      "                          仍然只允许 127.0.0.1；打开时必须同时给出\n"
      "                          --bpsec2-cert-file 与 "
      "--require-bpsec2（缺一不可），\n"
      "                          理由是给日志与事后审计的\n"
      "  --max-login-failures <n>    同一个用户名连续失败多少次后限速（默认 "
      "5，0 = 关闭）\n"
      "  --login-lockout-seconds <n> 限速窗口秒数（默认 60）\n"
      "  --log-file <文件>       追加日志文件（默认只写 stderr）\n"
      "  --pid-file <文件>       PID 文件（同一个 root/db/port 只允许一个）\n"
      "  --workers <数量>        并发 worker 数，默认 4（1..64）\n"
      "  --io-timeout <秒>       单连接读写超时，默认 30\n"
      "  --max-upload-bytes <n>  单次上传上限，默认 8 GiB\n"
      "  --quiet                 不往 stderr 打日志\n"
      "  --help                  显示这份用法\n"
      "\n"
      "BPNET1 的全部流量由 BPSEC1 加密（X25519 + HKDF-SHA256 + AES-256-CTR +\n"
      "HMAC-SHA256）；SSH 隧道仍然是部署层的纵深防御。监听地址只能是\n"
      "127.0.0.1（--bind 给任何别的地址都会直接拒绝启动）。远程使用：\n"
      "  ssh -N -L 18765:127.0.0.1:18765 <ecs-host>\n"
      "\n"
      "退出码: 0 正常停止 / 1 运行期失败 / 2 用法错误\n",
      program);
}

// 严格解析无符号十进制：不接受空串、符号、前后缀垃圾、溢出。
// 上界判断写成 maximum - digit 的形式，避免 value * 10 + digit 先溢出；
// 命令行是不可信输入，这里解析出的数值随后会当作尺寸/上界使用，所以宁可
// 拒绝也不能截断。
bool ParseUnsigned(const std::string& text, std::uint64_t maximum,
                   std::uint64_t* out) {
  if (text.empty() || text.size() > 20) {
    return false;
  }
  std::uint64_t value = 0;
  for (const char character : text) {
    if (character < '0' || character > '9') {
      return false;
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
    if (value > (maximum - digit) / 10) {
      return false;
    }
    value = value * 10 + digit;
  }
  *out = value;
  return true;
}

// PID 文件是普通文件，内容可能陈旧、被别人覆盖或只写了一半，所以一律按
// "尽力解析"处理：读不到、不是数字、不是正数都返回 -1，表示"没有可用的
// pid 线索"而不是"发生错误"。真正的存活判定交给 IsRunningServer，它只看
// /proc，不信任这个数字。
long ReadPidFile(const std::string& path) {
  std::FILE* file = std::fopen(path.c_str(), "r");
  if (file == nullptr) {
    return -1;
  }
  char buffer[64];
  const char* got = std::fgets(buffer, sizeof(buffer), file);
  std::fclose(file);
  if (got == nullptr) {
    return -1;
  }
  char* end = nullptr;
  const long pid = std::strtol(buffer, &end, 10);
  if (end == buffer || pid <= 0) {
    return -1;
  }
  return pid;
}

// 判断一个 pid 是不是还活着的 backup-server。
// 只读取 /proc/<pid>/comm，不发送任何信号——绝不"随便杀一个 PID"。
// 判不准时一律偏向"还活着"：进程存在但名字读不到、或 comm 读空都返回
// true。代价是极端情况下拒绝启动，收益是绝不会有两个进程同时写同一个
// SQLite 元数据库和同一个 root 目录。
bool IsRunningServer(long pid) {
  const std::string directory = "/proc/" + std::to_string(pid);
  struct stat info;
  if (::stat(directory.c_str(), &info) != 0) {
    return false;
  }
  std::FILE* file = std::fopen((directory + "/comm").c_str(), "r");
  if (file == nullptr) {
    // 进程存在但读不到名字：保守地认为"有人在用这个 PID"。
    return true;
  }
  char buffer[128];
  const char* got = std::fgets(buffer, sizeof(buffer), file);
  std::fclose(file);
  if (got == nullptr) {
    return true;
  }
  return std::strstr(buffer, "backup-server") != nullptr;
}

// 写入 "<pid>\n" 后故意不关 fd：fd 一直开到进程退出，由 main 统一 close +
// unlink。PID 文件只是诊断手段与软互斥（"读 + 查 /proc + 写"之间有固有
// TOCTOU 窗口），真正的并发一致性由服务层的 SQLite/root 语义保证。
bool WritePidFile(const std::string& path, int* out_fd, std::string* error) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    if (error != nullptr) {
      *error = "cannot write the pid file " + path;
    }
    return false;
  }
  const std::string text = std::to_string(::getpid()) + "\n";
  if (::write(fd, text.data(), text.size()) !=
      static_cast<ssize_t>(text.size())) {
    ::close(fd);
    if (error != nullptr) {
      *error = "cannot write the pid file " + path;
    }
    return false;
  }
  *out_fd = fd;
  return true;
}

}  // namespace

// 进程入口。结构固定为四段：解析参数 → 处理 PID 文件 → Configure/Start →
// Run 到停止再收尾。任何一段失败都走同一条清理路径（关 fd、删 PID 文件），
// 所以每个 return 都必须自己保证"没有留下半启动的 server"。
//
// 退出码：0 = 正常停止；1 = 运行期失败（含"已有实例在跑"）；2 = 用法错误。
int main(int argc, char* argv[]) {
  const char* program =
      (argc > 0 && argv[0] != nullptr) ? argv[0] : "backup-server";
  backupproject::net::RemoteServerConfig config;
  std::string pid_file_path;
  bool have_root = false;
  bool have_db = false;
  bool have_secret = false;
  bool have_transport_key = false;

  // 手写解析而不用 getopt：需要区分"开关型选项"与"取值型选项"，而且未知
  // 选项必须明确失败而不是被静默忽略——静默忽略会让打错的
  // --require-bpsec2 变成"服务照常启动但不强制签名身份"，那是安全降级。
  for (int index = 1; index < argc; ++index) {
    const std::string name = argv[index];
    if (name == "--help" || name == "-h") {
      PrintUsage(stdout, program);
      return 0;
    }
    if (name == "--quiet") {
      // 开关型选项：不吃后面的参数。
      config.quiet = true;
      continue;
    }
    if (name == "--require-bpsec2") {
      // 开关型选项：只接受签名身份（证书）客户端。
      config.require_bpsec2 = true;
      continue;
    }
    if (index + 1 >= argc) {
      std::fprintf(stderr, "Error: %s needs a value.\n\n", name.c_str());
      PrintUsage(stderr, program);
      return 2;
    }
    const std::string value = argv[++index];
    std::uint64_t number = 0;
    if (name == "--bind") {
      config.bind_address = value;
    } else if (name == "--port") {
      if (!ParseUnsigned(value, 65535, &number)) {
        std::fprintf(stderr, "Error: --port must be 0..65535.\n");
        return 2;
      }
      config.port = static_cast<std::uint16_t>(number);
    } else if (name == "--root") {
      config.root_directory = value;
      have_root = true;
    } else if (name == "--db") {
      config.database_path = value;
      have_db = true;
    } else if (name == "--secret-file") {
      config.secret_file_path = value;
      have_secret = true;
    } else if (name == "--transport-key-file") {
      config.transport_key_file_path = value;
      have_transport_key = true;
    } else if (name == "--bpsec2-cert-file") {
      config.certificate_file_path = value;
    } else if (name == "--max-login-failures") {
      if (!ParseUnsigned(value, 1000000, &number)) {
        std::fprintf(stderr, "Error: --max-login-failures must be a number.");
        return 2;
      }
      config.max_login_failures = static_cast<int>(number);
    } else if (name == "--login-lockout-seconds") {
      if (!ParseUnsigned(value, 86400, &number)) {
        std::fprintf(stderr,
                     "Error: --login-lockout-seconds must be a number.");
        return 2;
      }
      config.login_lockout_seconds = static_cast<int>(number);
    } else if (name == "--allow-public-bind") {
      // 显式公网绑定：值是「一句话理由」，会写进启动日志与事后审计。
      config.allow_public_bind = true;
      config.public_bind_reason = value;
    } else if (name == "--log-file") {
      config.log_file_path = value;
    } else if (name == "--pid-file") {
      pid_file_path = value;
    } else if (name == "--workers") {
      if (!ParseUnsigned(value, 64, &number) || number == 0) {
        std::fprintf(stderr, "Error: --workers must be 1..64.\n");
        return 2;
      }
      config.worker_count = static_cast<std::size_t>(number);
    } else if (name == "--io-timeout") {
      if (!ParseUnsigned(value, 3600, &number) || number == 0) {
        std::fprintf(stderr, "Error: --io-timeout must be 1..3600 seconds.\n");
        return 2;
      }
      config.io_timeout_seconds = static_cast<int>(number);
    } else if (name == "--max-upload-bytes") {
      if (!ParseUnsigned(value, 1ull << 40, &number) || number == 0) {
        std::fprintf(stderr, "--max-upload-bytes must be 1..2^40.\n");
        return 2;
      }
      config.max_upload_bytes = number;
    } else {
      std::fprintf(stderr, "Error: unknown option '%s'.\n\n", name.c_str());
      PrintUsage(stderr, program);
      return 2;
    }
  }

  // 四个必填项用独立的 bool 记录，而不是看字符串是否为空：空串是合法取值，
  // 只有"压根没给这个选项"才算缺参。缺参按用法错误处理（退出码 2），并且
  // 在写 PID 文件之前就返回，不会留下垃圾文件。
  if (!have_root || !have_db || !have_secret || !have_transport_key) {
    std::fprintf(stderr,
                 "Error: --root, --db, --secret-file and --transport-key-file"
                 " are all required.\n\n");
    PrintUsage(stderr, program);
    return 2;
  }

  // PID 文件：先确认没有另一个还在跑的 backup-server，再写自己的。
  // 顺序不能颠倒：先读旧文件 → 用 /proc 复核 → 才写自己的。反过来会在
  // "另一个实例正在启动"时覆盖掉对方的 pid，让真正的占用者失去诊断线索。
  int pid_fd = -1;
  if (!pid_file_path.empty()) {
    const long existing = ReadPidFile(pid_file_path);
    if (existing > 0 && IsRunningServer(existing)) {
      std::fprintf(stderr,
                   "Error: another backup-server (pid %ld) is already"
                   " running with pid file %s.\n",
                   existing, pid_file_path.c_str());
      return 1;
    }
    std::string pid_error;
    if (!WritePidFile(pid_file_path, &pid_fd, &pid_error)) {
      std::fprintf(stderr, "Error: %s\n", pid_error.c_str());
      return 1;
    }
  }

  // 生命周期分两步是为了区分"参数/密钥错误"（退出码 2）与"运行期失败"
  // （退出码 1）：Configure 只做校验与准备（加载传输私钥、证书、打开元数据
  // 库），Start 才 bind 端口、起 worker 线程。任一失败都必须回滚 PID 文件，
  // 否则下次启动会被自己留下的文件挡住。
  backupproject::net::RemoteServer server;
  g_server = &server;
  std::string error;
  if (!server.Configure(config, &error)) {
    std::fprintf(stderr, "Error: %s\n", error.c_str());
    if (pid_fd >= 0) ::close(pid_fd);
    if (!pid_file_path.empty()) ::unlink(pid_file_path.c_str());
    return 2;
  }
  if (!server.Start(&error)) {
    std::fprintf(stderr, "Error: %s\n", error.c_str());
    if (pid_fd >= 0) ::close(pid_fd);
    if (!pid_file_path.empty()) ::unlink(pid_file_path.c_str());
    return 1;
  }

  // 信号只在 Start 之后安装：启动过程中（还在加载密钥/建库）收到 SIGTERM
  // 时希望进程按默认语义直接终止，而不是进入"半初始化的优雅停止"。处理器
  // 只做一次原子写，真正的停止发生在 Run() 返回之后的主线程里。
  struct sigaction action;
  std::memset(&action, 0, sizeof(action));
  action.sa_handler = HandleStopSignal;
  ::sigemptyset(&action.sa_mask);
  // 不设 SA_RESTART：让阻塞中的系统调用被打断，停止更及时。
  action.sa_flags = 0;
  ::sigaction(SIGTERM, &action, nullptr);
  ::sigaction(SIGINT, &action, nullptr);
  // 对端已经关掉的连接上继续写会得到 SIGPIPE：忽略它，让 send() 返回 EPIPE。
  ::signal(SIGPIPE, SIG_IGN);

  // Run 阻塞到 RequestStop()（信号或内部致命错误）为止；返回后必须无条件
  // Stop()——它是幂等的，负责 join worker 并释放端口。
  const bool ok = server.Run(&error);
  server.Stop();
  g_server = nullptr;
  if (!ok) {
    std::fprintf(stderr, "Error: %s\n", error.c_str());
  }
  if (pid_fd >= 0) {
    ::close(pid_fd);
  }
  if (!pid_file_path.empty()) {
    ::unlink(pid_file_path.c_str());
  }
  std::fprintf(stderr, "backup-server stopped.\n");
  return ok ? 0 : 1;
}
