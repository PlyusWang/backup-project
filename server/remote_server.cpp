// server/remote_server.cpp
//
// backup-server 的监听、帧循环与连接状态机。见 include/remote_server.h。
//
// 这一层刻意不碰归档格式：它只管"把一段有名字、有长度、有 SHA-256 的字节
// 安全地存下来"。归档语义属于 BackupEngine，不属于这里。

// 状态机（ConnectionContext 一个连接一份，不跨连接共享）：
//
//   CONNECTED --REGISTER--> CONNECTED（注册只建账户，不建立会话）
//   CONNECTED --LOGIN / RESUME--> AUTHENTICATED
//   AUTHENTICATED --UPLOAD_BEGIN--> UPLOAD_IN_PROGRESS
//   UPLOAD_IN_PROGRESS --UPLOAD_END--> AUTHENTICATED
//   UPLOAD_IN_PROGRESS --任何失败 / 断连--> AUTHENTICATED（AbortUpload）
//   AUTHENTICATED --DOWNLOAD_BEGIN--> DOWNLOAD_IN_PROGRESS
//   DOWNLOAD_IN_PROGRESS --DOWNLOAD_END / 失败--> AUTHENTICATED
//   AUTHENTICATED --LOGOUT / 账户被注销--> CONNECTED
//
// 状态不匹配的请求一律回 kInvalidState，不做"猜调用方本来想干什么"的兼容。
//
// 线程模型：主线程跑 accept（Run），worker_count 个固定线程各自服务一条连接；
// 一条连接从握手到关闭只由同一个 worker 线程处理，所以 ConnectionContext 与
// 加密通道都不需要加锁。跨线程共享的只有：SQLite（store_ 内部一把互斥锁）、
// 登录节流表（login_throttle_mutex_）、日志（log_mutex_）、任务队列
// （work_mutex_ / work_ready_ / slot_free_）与几个 atomic 标志。
//
// 磁盘布局（每一段都由服务端生成，客户端字符串从不参与拼接）：
//
//   <root>/.backup-server.lock                  数据目录独占 flock
//   <root>/users/<uid>/<snapshot-id>.bak        已发布的不可变 blob
//   <root>/users/<uid>/tmp/<snapshot-id>.part   上传中的临时文件，0600
//   <root>/users/<uid>/trash/...                待物理删除的 blob
//   <root>/trash/account-<uid>.<rand>.deleted   账户注销的隔离目录
//
// 失败语义：处理函数返回 false = 这条连接必须断开（帧流或资源已经不可信）；
// 返回 true = 错误帧已经发出，帧循环可以继续。错误响应的 payload 恒为空，
// 具体原因只进服务端日志，免得把内部路径与 errno 泄漏给客户端。
#include "remote_server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

#include "bpcert.h"
#include "crypto.h"
#include "file_lock.h"
#include "remote_auth.h"
#include "remote_maintenance.h"
#include "remote_metadata_store.h"
#include "secure_transport.h"

namespace backupproject {
namespace net {
namespace {

// listen backlog：worker 全忙时排在这里，超出的连接由内核拒绝。
constexpr int kListenBacklog = 64;
// 最大 worker 数。超过这个数就不再是"foundation 的有界模型"了。
constexpr std::size_t kMaxWorkers = 64;

// 这条 worker 线程正在服务的连接所用的 BPSEC1 通道。
//
// 一个连接从握手到关闭全程只由**同一个** worker 线程处理（见 WorkerLoop），
// 所以放在 thread_local 里是安全的；这样 SendError / SendStatus 这类只拿得到
// fd 的深层函数不必逐个改签名就能走加密层。产品路径上它一定非空：
// ServeConnection 在进入帧循环之前先完成握手，握手失败直接断连。
// g_connection_channel == nullptr 时**不发任何字节**（fail closed），
// 不存在"退回明文 BPNET1"的代码路径。
thread_local SecureChannel* g_connection_channel = nullptr;
// Run() 的轮询间隔：让 Stop() 最迟 200 ms 内生效，同时不空转。
constexpr int kPollIntervalMs = 200;

// 服务端的墙钟秒（Unix epoch）。token 过期、快照时间戳、登录锁定时长都用它，
// 因此它受系统时钟调整影响：把时钟往回调会让已经过期的 token 重新可用。
// 单调时钟（steady_clock）只用于超时等待，不用于任何对外可见的时间戳。
std::uint64_t NowSeconds() {
  return static_cast<std::uint64_t>(::time(nullptr));
}

// 逐级创建目录（等价于 mkdir -p），权限一律 0700。
//
// 边界：它不解析符号链接、不做 realpath、也不清理路径里的 ".."；传进来的路径
// 必须是服务端自己拼出来的（见 UserDirectory）。EEXIST 视为成功，是为了容忍
// 两个线程同时创建同一级目录；但路径已存在且不是目录时必须失败——把这种情况
// 当成功，错误现场会被推迟到后面的 open()，离真正的原因更远。
// 返回 false 时 error_message 一定被填，调用方据此记日志并回 kInternalError。
bool EnsureDirectory(const std::string& path, std::string* error_message) {
  if (path.empty()) {
    if (error_message != nullptr) {
      *error_message = "directory path is empty";
    }
    return false;
  }
  // 逐级创建。已经存在但不是目录时必须失败，不能"当作成功"。
  std::string current;
  std::size_t index = 0;
  if (path[0] == '/') {
    current = "/";
    index = 1;
  }
  while (index <= path.size()) {
    const std::size_t slash = path.find('/', index);
    const std::string part = path.substr(
        index, slash == std::string::npos ? std::string::npos : slash - index);
    if (!part.empty()) {
      if (current.empty() || current == "/") {
        current += part;
      } else {
        current += "/" + part;
      }
      struct stat info;
      if (::stat(current.c_str(), &info) == 0) {
        if (!S_ISDIR(info.st_mode)) {
          if (error_message != nullptr) {
            *error_message = current + " exists but is not a directory";
          }
          return false;
        }
      } else if (::mkdir(current.c_str(), 0700) != 0 && errno != EEXIST) {
        if (error_message != nullptr) {
          *error_message =
              "cannot create " + current + ": " + std::strerror(errno);
        }
        return false;
      }
    }
    if (slash == std::string::npos) {
      break;
    }
    index = slash + 1;
  }
  return true;
}

// secrets.env 的大小上限：它是几行配置，不是数据文件。没有上限就等于给
// "读一个巨大的文件"留门（非普通文件在下面已经被拒，但普通文件也可能是 10
// GiB）。
constexpr std::size_t kMaxSecretFileBytes = 1024 * 1024;

// 读 secrets.env 里的 BACKUP_TOKEN_SECRET。
//
// 硬规则：函数只把值交给调用方，**任何**日志、错误信息、返回值里都不出现
// 值本身，最多出现长度。文件缺失、权限不对、没有这个键、值太短，都明确失败。
//
// "权限不对"必须是**真的检查过**的，而且检查与读取必须作用于同一个 inode：
//   * open(O_NOFOLLOW)：符号链接直接失败。否则别人只要把 secrets.env 指向
//     别处，"这是一个 0600 的 secret 文件"这句承诺就只是关于链接本身的；
//   * fstat(fd)，而不是 stat(path) 之后再重新打开：没有 check/use 分离，
//     中间不会被换成另一个文件（没有 TOCTOU）；
//   * 必须是普通文件；
//   * group / other 位一个都不能有：0600 与 0400 都接受，0640 / 0644 / 0660 /
//     0666 一律拒绝——secret 落在别人的可读范围里就等于泄漏。
// secrets.env 的格式是极简的 KEY=VALUE：逐行找第一个
// BACKUP_TOKEN_SECRET=，容忍 CRLF，不做引号 / 转义 / 变量展开，也不认
// "export " 前缀。它只被这一个进程读，因此不需要通用 env 语法。
bool ReadSecretFile(const std::string& path, std::string* secret,
                    std::string* error_message) {
  if (path.empty()) {
    if (error_message != nullptr) {
      *error_message = "--secret-file is required";
    }
    return false;
  }
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    if (error_message != nullptr) {
      if (errno == ELOOP) {
        *error_message = "the secret file must not be a symbolic link: " + path;
      } else {
        *error_message =
            "cannot open the secret file " + path + ": " + std::strerror(errno);
      }
    }
    return false;
  }
  struct stat info;
  if (::fstat(fd, &info) != 0) {
    const std::string reason = std::strerror(errno);
    ::close(fd);
    if (error_message != nullptr) {
      *error_message = "cannot inspect the secret file " + path + ": " + reason;
    }
    return false;
  }
  if (!S_ISREG(info.st_mode)) {
    ::close(fd);
    if (error_message != nullptr) {
      *error_message = "the secret file must be a regular file: " + path;
    }
    return false;
  }
  if ((info.st_mode & 0077) != 0) {
    char mode[16];
    std::snprintf(mode, sizeof(mode), "%04o",
                  static_cast<unsigned>(info.st_mode & 07777));
    ::close(fd);
    if (error_message != nullptr) {
      *error_message =
          "the secret file must not be readable or writable by group or others"
          " (expected mode 0600; 0400 is also accepted): " +
          path + " is " + mode;
    }
    return false;
  }
  // 从**同一个 fd** 读完整份内容，之后只解析内存里的这一份。
  std::string content;
  char buffer[4096];
  for (;;) {
    const ssize_t got = ::read(fd, buffer, sizeof(buffer));
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      const std::string reason = std::strerror(errno);
      ::close(fd);
      if (error_message != nullptr) {
        *error_message = "cannot read the secret file " + path + ": " + reason;
      }
      return false;
    }
    if (got == 0) {
      break;
    }
    content.append(buffer, static_cast<std::size_t>(got));
    if (content.size() > kMaxSecretFileBytes) {
      ::close(fd);
      if (error_message != nullptr) {
        *error_message = "the secret file is larger than 1 MiB: " + path;
      }
      return false;
    }
  }
  ::close(fd);
  std::string value;
  std::size_t start = 0;
  while (start <= content.size()) {
    const std::size_t end = content.find('\n', start);
    std::string line = content.substr(
        start, end == std::string::npos ? std::string::npos : end - start);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const std::string key = "BACKUP_TOKEN_SECRET=";
    if (line.compare(0, key.size(), key) == 0) {
      value = line.substr(key.size());
      break;
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  if (value.empty()) {
    if (error_message != nullptr) {
      *error_message = "the secret file does not define BACKUP_TOKEN_SECRET";
    }
    return false;
  }
  if (value.size() < 16) {
    if (error_message != nullptr) {
      *error_message = "BACKUP_TOKEN_SECRET is shorter than 16 bytes";
    }
    return false;
  }
  *secret = value;
  return true;
}

}  // namespace

// 状态名只用于日志与诊断：它是观测文本，不是协议字段，客户端从不解析。
// 新增状态时必须在这里补分支，否则会打印出 UNKNOWN_STATE。
const char* ConnectionStateName(ConnectionState state) {
  switch (state) {
    case ConnectionState::kConnected:
      return "CONNECTED";
    case ConnectionState::kAuthenticated:
      return "AUTHENTICATED";
    case ConnectionState::kUploadInProgress:
      return "UPLOAD_IN_PROGRESS";
    case ConnectionState::kDownloadInProgress:
      return "DOWNLOAD_IN_PROGRESS";
  }
  return "UNKNOWN_STATE";
}

// request_counts_ 是"收到的请求数"的测试观测点：按 opcode 下标索引的 256 个
// 原子计数器。产品路径只写不读，测试用它证明客户端不会静默重发请求。
RemoteServer::RemoteServer() {
  // std::atomic 的默认构造在 C++17 里不保证清零：显式初始化。
  for (std::size_t index = 0; index < 256; ++index) {
    request_counts_[index].store(0);
  }
}

std::uint64_t RemoteServer::request_count_for_testing(
    std::uint16_t opcode) const {
  if (opcode > 255) {
    return 0;
  }
  return request_counts_[opcode].load();
}

// 析构走 Stop()，这也是"Run() 还在跑时销毁对象是 UB"的由来：Stop() 会关掉
// Run() 正在使用的 listener。Stop() 内部的 run_in_progress_ 检查只是一道防
// 误用的闸门，那种情况下它什么都不拆，资源要靠 Run() 自己收。
RemoteServer::~RemoteServer() { Stop(); }

// 配置校验：纯检查，不产生副作用（唯一的写入是最后那句 config_ = config），
// 因此可以反复调用，也只有它成功之后 Start() 才有意义。
//
// 这里拒绝的每一条都是"启动之后就没法安全运行"的配置：空 bind、非
// dotted-quad 的地址、空 root / db / secret / transport key、worker 数越界、
// IO 超时越界、上传上限为 0。校验失败时给的是可以直接照做的原因，
// 而不是一句 "invalid config"。
bool RemoteServer::Configure(const RemoteServerConfig& config,
                             std::string* error_message) {
  if (config.bind_address.empty()) {
    if (error_message != nullptr) {
      *error_message = "--bind must not be empty";
    }
    return false;
  }
  in_addr probe;
  if (::inet_pton(AF_INET, config.bind_address.c_str(), &probe) != 1) {
    if (error_message != nullptr) {
      *error_message = "--bind must be a dotted-quad IPv4 address";
    }
    return false;
  }
  // 只允许监听 127.0.0.1，而且是**相等**判断（127.0.0.2 之类同样拒绝）。
  //
  // BPNET1 的每一个字节都由 BPSEC1 加密（见 secure_transport.h），
  // 机密性不再依赖 SSH 隧道；隧道降级为部署层的纵深防御。监听地址仍然只能是
  // 环回地址——这是纵深防御的一部分，不是机密性的前提：把只该由隧道访问的
  // 端口直接暴露在共享网络上没有任何好处，所以这里继续 fail closed，
  // 不提供 --insecure / --allow-public 之类的开关。
  // 默认规则不变（非回环一律拒绝），但多了一条**显式**例外：
  // 官方云端要让用户不开隧道，就必须把 18765 暴露在公网上。这个例外必须
  // 同时满足三件事，缺一不可：
  //   1. 显式给 --allow-public-bind <理由>，理由是给日志和事后审计看的；
  //   2. 配置了 BPSEC2 身份证书 —— 公网监听的正当性完全建立在「客户端能用
  //      证书确认对端是谁」之上，没有证书就只是把端口裸奔出去；
  //   3. 理由不能是空串（形式上的「我知道我在做什么」）。
  // 没有这个开关时，0.0.0.0 / 127.0.0.2 / 192.168.x 的拒绝行为与过去逐字
  // 相同，既有测试仍然断言这一点。
  if (config.bind_address != "127.0.0.1") {
    if (!config.allow_public_bind) {
      if (error_message != nullptr) {
        *error_message =
            "--bind must be 127.0.0.1, not " + config.bind_address +
            ": this version has no native TLS, so the server only accepts"
            " loopback connections; reach a remote instance through an SSH"
            " tunnel (ssh -N -L 18765:127.0.0.1:18765 <host>)."
            " To serve the official cloud directly on a public address,"
            " pass --allow-public-bind <reason> together with"
            " --bpsec2-cert-file.";
      }
      return false;
    }
    if (config.certificate_file_path.empty()) {
      if (error_message != nullptr) {
        *error_message =
            "--allow-public-bind requires --bpsec2-cert-file: a public"
            " listener must authenticate itself with a signed identity,"
            " otherwise clients have no way to tell who they are talking to";
      }
      return false;
    }
    if (!config.require_bpsec2) {
      // 公网监听只允许签名身份，而且要**只**允许签名身份：只开证书却仍接受
      // BPSEC1 的 pin 客户端，等于在公网上保留一条"人工指纹"的旧路。红队
      // 复核用真实二进制验证过：不加这一条，pin 客户端在公网监听上仍能 ping
      // 通。
      if (error_message != nullptr) {
        *error_message =
            "--allow-public-bind requires --require-bpsec2: a public listener"
            " must accept signed-identity clients only, otherwise the legacy"
            " pin path stays reachable on the internet";
      }
      return false;
    }
    if (config.public_bind_reason.empty()) {
      if (error_message != nullptr) {
        *error_message =
            "--allow-public-bind requires a non-empty reason"
            " (it is written to the startup log)";
      }
      return false;
    }
  }
  if (config.root_directory.empty()) {
    if (error_message != nullptr) {
      *error_message = "--root must not be empty";
    }
    return false;
  }
  if (config.database_path.empty()) {
    if (error_message != nullptr) {
      *error_message = "--db must not be empty";
    }
    return false;
  }
  if (config.transport_key_file_path.empty()) {
    if (error_message != nullptr) {
      *error_message =
          "--transport-key-file must not be empty: BPSEC1 needs a server"
          " identity key, and this server has no plaintext mode (generate one"
          " with backup-server-keygen --output <path>)";
    }
    return false;
  }
  if (config.worker_count == 0 || config.worker_count > kMaxWorkers) {
    if (error_message != nullptr) {
      *error_message = "--workers must be between 1 and 64";
    }
    return false;
  }
  if (config.io_timeout_seconds <= 0 || config.io_timeout_seconds > 3600) {
    if (error_message != nullptr) {
      *error_message = "--io-timeout must be between 1 and 3600 seconds";
    }
    return false;
  }
  if (config.max_upload_bytes == 0) {
    if (error_message != nullptr) {
      *error_message = "--max-upload-bytes must be greater than zero";
    }
    return false;
  }
  config_ = config;
  return true;
}

// 把服务端从"未启动"推进到"可以 Run()"：建目录 -> 抢数据目录锁 -> 读 secret
// 与身份密钥 -> 开元数据库 -> 建维护层 -> 建监听 socket。
//
// 顺序不能换：锁必须先于任何写盘拿到（否则两个进程会同时写同一批文件），
// 元数据库必须先于 maintenance_ 打开（维护层复用的就是这一个连接）。
// 任何一步失败都从当前位置往回释放已经拿到的资源并返回 false：Start() 要么
// 完整成功，要么不留下半开状态，调用方不需要也无法做部分回滚。
// 它不 accept 任何连接：接受连接是 Run() 的事。
bool RemoteServer::Start(std::string* error_message) {
  if (running()) {
    if (error_message != nullptr) {
      *error_message = "server is already started";
    }
    return false;
  }
  // 停止标志在这里复位，而不是在 Run() 里：如果信号在 Start() 之后、
  // Run() 之前到达，在 Run() 里复位会把这次停止请求吞掉，进程就再也
  // 停不下来了。复位必须发生在"开始接受停止请求之前"。
  stop_requested_.store(false);
  if (!EnsureDirectory(config_.root_directory, error_message)) {
    return false;
  }
  if (!EnsureDirectory(config_.root_directory + "/users", error_message)) {
    return false;
  }
  // 账户注销的隔离区：与 users/ 同级，不属于任何用户目录。
  if (!EnsureDirectory(config_.root_directory + "/trash", error_message)) {
    return false;
  }
  // 数据目录独占锁。抢不到就是"这个数据目录已经有写者了"：如实拒绝启动，
  // 而不是两个进程同时写同一个 SQLite 与同一批 blob。
  if (!AcquireDataLock(error_message)) {
    return false;
  }
  const std::size_t slash = config_.database_path.find_last_of('/');
  if (slash != std::string::npos && slash > 0) {
    if (!EnsureDirectory(config_.database_path.substr(0, slash),
                         error_message)) {
      return false;
    }
  }
  if (!LoadSecret(error_message)) {
    data_lock_.reset();
    return false;
  }
  if (!LoadTransportIdentityKey(error_message)) {
    data_lock_.reset();
    return false;
  }
  // store_ 是进程内唯一的 SQLite 连接：worker 线程通过它内部那把互斥锁串行
  // 访问，所以这里没有连接池，也不存在写-写竞争。
  store_.reset(new RemoteMetadataStore());
  if (!store_->Open(config_.database_path, error_message)) {
    store_.reset();
    data_lock_.reset();
    return false;
  }
  Log("metadata database opened at " + config_.database_path);
  // 维护层复用**这一个** store 连接：管理动作与协议处理看到的是同一个
  // SQLite 连接与同一把互斥锁，不存在第二个写者。
  maintenance_.reset(
      new RemoteMaintenance(store_.get(), config_.root_directory));
  maintenance_->set_log([this](const std::string& message) { Log(message); });
  if (!OpenListener(error_message)) {
    maintenance_.reset();
    store_->Close();
    store_.reset();
    data_lock_.reset();
    return false;
  }
  if (config_.bind_address != "127.0.0.1") {
    // 公网监听必须在日志里留痕：出了事要能一眼看出当时是谁、以什么理由
    // 把它开出去的。理由是启动参数里那句话，不是脚本猜的。
    std::ostringstream public_line;
    public_line << "WARNING: listening on a PUBLIC address "
                << config_.bind_address << ":" << bound_port_
                << " reason=" << config_.public_bind_reason
                << " identity=bpsec2-certificate" << " require_bpsec2="
                << (config_.require_bpsec2 ? "yes" : "no");
    Log(public_line.str());
  }
  {
    std::ostringstream line;
    line << "listening on " << config_.bind_address << ":" << bound_port_
         << " root=" << config_.root_directory
         << " workers=" << config_.worker_count
         << " max_upload=" << config_.max_upload_bytes << " bspec1=on"
         << " transport_fingerprint="
         << crypto::X25519Fingerprint(transport_identity_.public_key);
    Log(line.str());
  }
  return true;
}

// secret_ 常驻进程内存：签发与校验 token 的每一次 HMAC 都要用它，因此不做
// "用完即清"。它来自 0600 的 secrets.env，绝不进日志、绝不进错误信息。
bool RemoteServer::LoadSecret(std::string* error_message) {
  if (!ReadSecretFile(config_.secret_file_path, &secret_, error_message)) {
    return false;
  }
  // 只记长度，不记内容。
  std::ostringstream line;
  line << "token secret loaded (" << secret_.size() << " bytes)";
  Log(line.str());
  return true;
}

namespace {

// 读整个文件。BPSEC2 的证书是几十到几百字节的公开材料，一次读完最简单。
// 注意：4096 的检查发生在整份内容已经在内存里之后——它挡住的是"把任意大的
// 文件交给 BPCERT1 解析器"，而不是内存峰值。证书是公开材料且只有几百字节，
// 这个取舍是刻意的（真要限制峰值就得先 fstat 再按大小分配）。
bool ReadWholeFile(const std::string& path, std::string* out,
                   std::string* error_message) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    if (error_message != nullptr) {
      *error_message = "打不开文件：" + path;
    }
    return false;
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  *out = buffer.str();
  // 上限先于解析：BPCERT1 本身有 4096 字节硬上限，先卡住长度就不必把一个
  // 任意大的文件读进内存再被解析器拒绝（红队复核的 nit）。
  if (out->size() > 4096) {
    if (error_message != nullptr) {
      *error_message = "文件超过 4096 字节上限：" + path;
    }
    return false;
  }
  if (out->empty()) {
    if (error_message != nullptr) {
      *error_message = "文件是空的：" + path;
    }
    return false;
  }
  return true;
}

}  // namespace

// 组装服务端身份：BPSEC1 长期私钥（必填）+ 可选的 BPSEC2 身份证书。
//
// 证书不是另一把密钥，它是对同一把 transport key 的签名声明，所以必须检查
// 证书里的公钥与 transport.key 一致：不一致时每个客户端都会在握手阶段拒绝
// 本机，真正的原因却只有启动日志看得到——宁可在启动时直接失败。
// require_bpsec2 与证书是一组："只收签名身份"却没有证书等于谁都连不上。
bool RemoteServer::LoadTransportIdentityKey(std::string* error_message) {
  if (!LoadTransportIdentity(config_.transport_key_file_path,
                             &transport_identity_, error_message)) {
    return false;
  }
  // 只记指纹（公开信息），不记私钥。
  Log("BPSEC1 transport identity loaded, fingerprint=" +
      crypto::X25519Fingerprint(transport_identity_.public_key));

  // BPSEC2：按配置加载服务器身份证书（里面只有公钥材料）。
  if (!config_.certificate_file_path.empty()) {
    std::string raw;
    if (!ReadWholeFile(config_.certificate_file_path, &raw, error_message)) {
      return false;
    }
    crypto::Bpcert1 certificate;
    const crypto::Bpcert1Error parsed = crypto::Bpcert1Parse(raw, &certificate);
    if (parsed != crypto::Bpcert1Error::kOk) {
      if (error_message != nullptr) {
        *error_message = std::string("服务器身份证书不合法：") +
                         crypto::Bpcert1ErrorName(parsed);
      }
      return false;
    }
    // 这张证书必须**就是**给本机这把身份密钥签的。否则服务端会拿着一把对
    // 不上的证书去握手，每个客户端都会拒绝，而真正的原因要到线上才看得出来
    // —— 宁可在启动时直接失败。
    if (certificate.server_public_key != transport_identity_.public_key) {
      if (error_message != nullptr) {
        *error_message =
            "服务器身份证书里的公钥与本机 transport.key 不一致"
            "（这张证书不是给这把密钥签的）";
      }
      return false;
    }
    transport_identity_.certificate = raw;
    Log("BPSEC2 identity certificate loaded, server_id=" +
        certificate.server_id + " issuer=" + certificate.issuer_id +
        " serial=" + std::to_string(certificate.serial_number) +
        " sha256=" + crypto::Bpcert1Fingerprint(raw));
  }
  if (config_.require_bpsec2 && transport_identity_.certificate.empty()) {
    if (error_message != nullptr) {
      *error_message =
          "--require-bpsec2 需要同时用 --bpsec2-cert-file 给出服务器身份证书";
    }
    return false;
  }
  return true;
}

// 只在 Start() 里被调用一次（单线程），因此 listener_fd_ / bound_port_ 的写入
// 不需要同步。SO_REUSEADDR 是为了让刚重启的服务端能立刻重绑处于 TIME_WAIT 的
// 端口，它不允许两个进程同时监听同一个地址。
// port == 0 时端口由内核分配，getsockname 把真实端口回填到 bound_port_，测试
// 靠它拿临时端口。任何一步失败都 close(fd) 后返回，不留半开的 socket。
bool RemoteServer::OpenListener(std::string* error_message) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    if (error_message != nullptr) {
      *error_message = std::string("socket() failed: ") + std::strerror(errno);
    }
    return false;
  }
  const int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  sockaddr_in address;
  std::memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(config_.port);
  if (::inet_pton(AF_INET, config_.bind_address.c_str(), &address.sin_addr) !=
      1) {
    ::close(fd);
    if (error_message != nullptr) {
      *error_message = "invalid bind address";
    }
    return false;
  }
  if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    const std::string reason = std::strerror(errno);
    ::close(fd);
    if (error_message != nullptr) {
      // 端口被占用时给出可执行的判断依据，而不是只说"失败"。
      *error_message = "cannot bind " + config_.bind_address + ":" +
                       std::to_string(config_.port) + ": " + reason;
    }
    return false;
  }
  if (::listen(fd, kListenBacklog) != 0) {
    const std::string reason = std::strerror(errno);
    ::close(fd);
    if (error_message != nullptr) {
      *error_message = std::string("listen() failed: ") + reason;
    }
    return false;
  }
  sockaddr_in actual;
  std::memset(&actual, 0, sizeof(actual));
  socklen_t actual_size = sizeof(actual);
  bound_port_ = config_.port;
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&actual), &actual_size) ==
      0) {
    bound_port_ = ntohs(actual.sin_port);
  }
  listener_fd_ = fd;
  return true;
}

void RemoteServer::Stop() {
  // 生命周期合同（见头文件）：Stop() 不得与 Run() 并发。Run() 自己会
  // shutdown pending 连接并 join 全部 worker，而 Stop() 会把 store_ /
  // listener_fd_ 拆掉。 两者交叉的话，worker 会在用着 store_ / listener
  // 的时候被拆掉。 Run() 还在跑就调 Stop()
  // 是调用方的错：这里直接返回，不做任何拆除， 让 Run() 自己把 worker
  // 收干净（RequestStop() 仍然是唯一线程安全的停止入口）。
  if (run_in_progress_.load()) {
    return;
  }
  RequestStop();
  // 释放顺序是刻意的：先让 accept 停下来（关 listener），再释放 store_；
  // pending_ 清空与 worker join 放在中间，因为走到这里要么 Run() 已经返回
  // （它自己 join 过全部 worker），要么 Run() 从未运行过。
  // 维护层与数据目录锁最后释放：锁一放，backup-server-admin 就能动这个目录，
  // 所以必须在确认没有任何 worker 之后。
  if (listener_fd_ >= 0) {
    ::close(listener_fd_);
    listener_fd_ = -1;
  }
  if (store_ != nullptr) {
    store_->Close();
    store_.reset();
  }
  {
    std::lock_guard<std::mutex> guard(work_mutex_);
    pending_.clear();
  }
  for (std::thread& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  workers_.clear();
  // 维护层与数据目录锁最后释放：锁一放，管理工具就可以动这个目录了，
  // 所以必须在所有 worker 都停下来之后。
  maintenance_.reset();
  data_lock_.reset();
}

// 唯一允许与 Run() 并发调用的入口（例如另一个线程或信号处理路径）：只置一个
// atomic 标志，不碰 fd、不碰线程、不碰数据库。它只是"请求"，真正的收尾在
// Run() 的返回路径上完成。
void RemoteServer::RequestStop() { stop_requested_.store(true); }

// store_ 可能还没建起来（测试可以在 Start() 之前调用），所以先判空。
void RemoteServer::FailNextAccountDeleteForTesting() {
  if (store_ != nullptr) {
    store_->FailNextDeleteUserForTesting();
  }
}

// 错误帧回填请求的 opcode（未知 opcode 才用 kError）：客户端的 Request 正是靠
// request_id 与本字段把响应和请求配对，配不上就不会去猜是哪条命令失败了。
// 返回 false 表示连错误帧都发不出去，帧循环必须据此断开连接。
bool RemoteServer::SendError(int fd, std::uint16_t opcode,
                             std::uint64_t request_id, Status status,
                             std::string* error_message) {
  // 错误响应的 payload 永远是空的：原因只写服务端日志，不回给客户端，
  // 免得把内部路径或 errno 漏出去。
  if (g_connection_channel == nullptr) {
    // 没有加密通道时一个字节都不发：没有明文回退路径。
    if (error_message != nullptr) {
      *error_message = "refusing to send a frame without a BPSEC1 channel";
    }
    return false;
  }
  if (!g_connection_channel->SendFrame(
          fd, opcode, static_cast<std::uint32_t>(status), request_id,
          std::string(), error_message)) {
    return false;
  }
  return true;
}

// 成功响应的统一出口：opcode 用请求的 opcode（客户端据此知道这是哪条命令的
// 答复），request_id 原样回填。未识别的 opcode 统一落成 ERROR 帧，避免把一个
// 任意数值原样回显给对端。payload 由调用方保证不超过 1 MiB（帧层还会再查）。
bool RemoteServer::SendStatus(int fd, const FrameHeader& request, Status status,
                              const std::string& payload,
                              std::string* error_message) {
  const std::uint16_t opcode = IsKnownOpcode(request.opcode)
                                   ? request.opcode
                                   : static_cast<std::uint16_t>(Opcode::kError);
  if (g_connection_channel == nullptr) {
    // 同上：没有加密通道就不发。
    if (error_message != nullptr) {
      *error_message = "refusing to send a frame without a BPSEC1 channel";
    }
    return false;
  }
  return g_connection_channel->SendFrame(
      fd, opcode, static_cast<std::uint32_t>(status), request.request_id,
      payload, error_message);
}

// PING 是唯一既不需要会话也不需要口令的操作码：HandleFrame 在状态检查之前就
// 处理它，只要求 payload 为空。用途是探活与版本协商（协议版本、服务端软件名、
// 服务端墙钟），所以它不查账户、不碰数据库、不产生任何持久化副作用。
// 响应字段顺序即线上顺序：string software, u16 version, u64 server_time。
bool RemoteServer::HandlePing(int fd, const FrameHeader& header,
                              std::string* error_message) {
  PayloadBuilder builder;
  std::string build_error;
  if (!builder.AppendString(kServerSoftwareName, kMaxDisplayNameBytes,
                            &build_error)) {
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInternalError, error_message);
  }
  builder.AppendU16(kProtocolVersion);
  builder.AppendU64(NowSeconds());
  return SendStatus(fd, header, Status::kOk, builder.data(), error_message);
}

namespace {

// 解 REGISTER / LOGIN 的 payload：用户名 + 口令，且不允许尾部多余字节。
// 尾部有垃圾说明客户端与服务端的字段理解已经不一致，必须明确拒绝，
// 而不是"读到自己要的就当成功"。
// REGISTER 与 LOGIN 共用它，因此两者的字段布局永远是同一个：解码顺序即
// username, password（u16 长度前缀 + 原始字节）。口令在这里只被搬进内存，
// 绝不进日志；上限来自协议层的 kMaxUsernameBytes / kMaxPasswordBytes。
bool DecodeCredentials(const std::string& payload, std::string* username,
                       std::string* password, std::string* error_message) {
  PayloadReader reader(payload);
  if (!reader.ReadString(kMaxUsernameBytes, username)) {
    if (error_message != nullptr) {
      *error_message =
          "cannot read the username field: " + reader.error_message();
    }
    return false;
  }
  if (!reader.ReadString(kMaxPasswordBytes, password)) {
    if (error_message != nullptr) {
      *error_message =
          "cannot read the password field: " + reader.error_message();
    }
    return false;
  }
  if (!reader.AtEnd()) {
    if (error_message != nullptr) {
      *error_message = "credentials frame has trailing bytes";
    }
    return false;
  }
  return true;
}

}  // namespace

// 语义：建账户。它不建立会话——成功后客户端还要再走一次 LOGIN 才拿到 token，
// 所以这里不改 ConnectionContext 的 state。
//
// 口令只以 PBKDF2 记录（salt + hash + iterations）落库，服务端无法反推出口令，
// 迭代次数由服务端决定，客户端没有任何字段能影响它。用户名唯一性由数据库的
// UNIQUE 约束兜底：并发注册同一个名字时输的一方得到 kAlreadyExists，
// 而不是覆盖前一个账户的口令。
bool RemoteServer::HandleRegister(int fd, const FrameHeader& header,
                                  const std::string& payload,
                                  ConnectionContext* context,
                                  std::string* error_message) {
  if (context->state != ConnectionState::kConnected) {
    Log("rejecting REGISTER while a session is already established");
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInvalidState, error_message);
  }
  std::string username;
  std::string password;
  std::string decode_error;
  if (!DecodeCredentials(payload, &username, &password, &decode_error)) {
    Log("rejecting a malformed REGISTER: " + decode_error);
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInvalidRequest, error_message);
  }
  std::string validation_error;
  if (!IsValidUsername(username, &validation_error)) {
    Log("rejecting REGISTER with an invalid username: " + validation_error);
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInvalidRequest, error_message);
  }
  if (password.size() < kMinPasswordBytes ||
      password.size() > kMaxPasswordBytes) {
    Log("rejecting REGISTER with a password outside 8..256 bytes");
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInvalidRequest, error_message);
  }
  PasswordRecord record;
  std::string hash_error;
  if (!HashPassword(password, &record, &hash_error)) {
    Log("cannot derive a password hash: " + hash_error);
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInternalError, error_message);
  }
  std::int64_t user_id = 0;
  std::string store_error;
  const StoreResult result = store_->CreateUser(
      username, record, static_cast<std::int64_t>(NowSeconds()), &user_id,
      &store_error);
  if (result == StoreResult::kAlreadyExists) {
    Log("rejecting REGISTER for a username that already exists");
    return SendError(fd, header.opcode, header.request_id,
                     Status::kAlreadyExists, error_message);
  }
  if (result != StoreResult::kOk) {
    Log("cannot create the user row: " + store_error);
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInternalError, error_message);
  }
  Log("registered user id=" + std::to_string(user_id));
  return SendStatus(fd, header, Status::kOk, std::string(), error_message);
}

// ---- §33：登录失败节流 ----
//
// 目标只有一个：让在线口令猜测变得不划算。三条设计约束：
//   1. 计数按**用户名字符串**，而不是按 user_id —— 不存在的用户名也必须被
//      限速，否则「这个用户名被限速了」本身就泄漏了「这个用户名存在」；
//   2. 锁定期间**即使口令正确也拒绝** —— 否则攻击者只要在锁定窗口里碰对一次
//      就绕过了节流；
//   3. 表的大小必须有上界：攻击者可以用海量不同的用户名把内存撑爆，
//      所以超过上限时先清理已过期的条目，仍然满就不再记录（降级而不是崩）。
// 只读查询：返回剩余锁定秒数，0 表示没锁（或节流被配置关掉了）。
// 调用方无论锁没锁都回同一句 kUnauthorized 文案，所以"这个用户名正在被锁定"
// 不会通过响应内容泄漏出去。
std::int64_t RemoteServer::LoginLockRemainingSeconds(
    const std::string& username) {
  if (config_.max_login_failures <= 0 || config_.login_lockout_seconds <= 0) {
    return 0;
  }
  const std::int64_t now = NowSeconds();
  std::lock_guard<std::mutex> guard(login_throttle_mutex_);
  const auto found = login_throttle_.find(username);
  if (found == login_throttle_.end()) {
    return 0;
  }
  return found->second.locked_until > now ? found->second.locked_until - now
                                          : 0;
}

// 记一次失败：连续失败达到 max_login_failures 时置 locked_until 并把计数清零
// （窗口结束后重新从 0 开始计数，而不是"解锁即再锁"）。表满时先清掉已过期的
// 条目，仍然满就不记录——宁可少限速一个名字，也不能让攻击者用海量用户名把
// 服务端内存撑爆。
void RemoteServer::RecordLoginFailure(const std::string& username) {
  if (config_.max_login_failures <= 0 || config_.login_lockout_seconds <= 0) {
    return;
  }
  const std::int64_t now = NowSeconds();
  std::lock_guard<std::mutex> guard(login_throttle_mutex_);
  constexpr std::size_t kMaxTracked = 4096;
  if (login_throttle_.find(username) == login_throttle_.end() &&
      login_throttle_.size() >= kMaxTracked) {
    for (auto it = login_throttle_.begin(); it != login_throttle_.end();) {
      if (it->second.locked_until <= now) {
        it = login_throttle_.erase(it);
      } else {
        ++it;
      }
    }
    if (login_throttle_.size() >= kMaxTracked) {
      Log("login throttle table is full, not tracking this username");
      return;
    }
  }
  LoginThrottle& entry = login_throttle_[username];
  ++entry.consecutive_failures;
  if (entry.consecutive_failures >= config_.max_login_failures) {
    entry.locked_until = now + config_.login_lockout_seconds;
    entry.consecutive_failures = 0;
    Log("login throttle engaged for the supplied username for " +
        std::to_string(config_.login_lockout_seconds) + "s");
  }
}

// 登录成功即删条目：节流只针对连续失败，成功一次就把计数清零。
void RemoteServer::ClearLoginFailures(const std::string& username) {
  std::lock_guard<std::mutex> guard(login_throttle_mutex_);
  login_throttle_.erase(username);
}
// 登录成功 = 服务端签发一枚带 HMAC 与过期时间的 token（12 小时），并把这条
// 连接推进到 kAuthenticated。服务端不保存会话表：token 的有效性完全由签名与
// 时间决定，所以"账户被注销"要靠 RejectIfAccountMissing 这类回查兜底。
//
// 三条路径必须给客户端不可区分的答案：用户名不存在、口令错误、被节流，全部回
// kUnauthorized；前两者还要付出同等代价的 PBKDF2（对不存在的用户名用全零的
// salt/hash 假记录跑一次），让响应时间不泄漏账户是否存在。
// token 只出现在这一条响应里：不写日志、不进 last_error、不落盘。
bool RemoteServer::HandleLogin(int fd, const FrameHeader& header,
                               const std::string& payload,
                               ConnectionContext* context,
                               std::string* error_message) {
  if (context->state != ConnectionState::kConnected) {
    Log("rejecting LOGIN while a session is already established");
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInvalidState, error_message);
  }
  std::string username;
  std::string password;
  std::string decode_error;
  if (!DecodeCredentials(payload, &username, &password, &decode_error)) {
    Log("rejecting a malformed LOGIN: " + decode_error);
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInvalidRequest, error_message);
  }
  // §33：先看节流，而且放在查库**之前** —— 存在与不存在的用户名走同一条
  // 限速路径，限速本身就不会变成「这个用户名存在吗」的探针。
  const std::int64_t lock_remaining = LoginLockRemainingSeconds(username);
  if (lock_remaining > 0) {
    Log("login throttled: too many consecutive failures for the supplied"
        " username, " +
        std::to_string(lock_remaining) + "s remaining");
    return SendError(fd, header.opcode, header.request_id,
                     Status::kUnauthorized, error_message);
  }
  RemoteUserRecord user;
  std::string store_error;
  const StoreResult result = store_->FindUser(username, &user, &store_error);
  if (result == StoreResult::kNotFound) {
    // 不让"这个用户名存不存在"从响应时间上泄漏出去：照样做一次同等代价的
    // PBKDF2。响应码与口令错误完全一致。
    PasswordRecord dummy;
    dummy.salt = std::string(kPasswordSaltBytes, '\0');
    dummy.hash = std::string(kPasswordHashBytes, '\0');
    dummy.iterations = kPasswordIterations;
    bool ignored = false;
    VerifyPassword(password, dummy, &ignored, nullptr);
    RecordLoginFailure(username);
    Log("login rejected: unknown user");
    return SendError(fd, header.opcode, header.request_id,
                     Status::kUnauthorized, error_message);
  }
  if (result != StoreResult::kOk) {
    Log("cannot read the user row: " + store_error);
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInternalError, error_message);
  }
  bool matches = false;
  std::string verify_error;
  if (!VerifyPassword(password, user.password, &matches, &verify_error)) {
    Log("cannot verify the stored password: " + verify_error);
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInternalError, error_message);
  }
  if (!matches) {
    RecordLoginFailure(username);
    Log("login rejected: wrong password for user id=" +
        std::to_string(user.user_id));
    return SendError(fd, header.opcode, header.request_id,
                     Status::kUnauthorized, error_message);
  }
  std::string token;
  std::string token_error;
  if (!IssueToken(secret_, static_cast<std::uint64_t>(user.user_id),
                  NowSeconds(), &token, &token_error)) {
    Log("cannot issue a token: " + token_error);
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInternalError, error_message);
  }
  ClearLoginFailures(username);
  context->state = ConnectionState::kAuthenticated;
  context->user_id = static_cast<std::uint64_t>(user.user_id);
  context->username = user.username;
  // token 只出现在这一条响应里，绝不写日志。
  PayloadBuilder builder;
  std::string build_error;
  if (!builder.AppendString(token, kMaxTokenBytes, &build_error)) {
    Log("cannot encode the token response: " + build_error);
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInternalError, error_message);
  }
  Log("login accepted for user id=" + std::to_string(user.user_id));
  return SendStatus(fd, header, Status::kOk, builder.data(), error_message);
}

// LOGOUT 是"这条连接上的会话结束"：服务端没有会话表，它清掉的是内存里的
// ConnectionContext（state 回 kConnected，user_id / username 归零）。
// 它无法让那枚 token 在别处失效——真正的失效机制是 token 的 12 小时有效期，
// 以及账户注销之后的回查。
bool RemoteServer::HandleLogout(int fd, const FrameHeader& header,
                                ConnectionContext* context,
                                std::string* error_message) {
  if (context->state != ConnectionState::kAuthenticated) {
    Log("rejecting LOGOUT without a session");
    return SendError(fd, header.opcode, header.request_id,
                     Status::kUnauthorized, error_message);
  }
  // 服务端不保存会话表，所以 LOGOUT 就是把这条连接的内存会话清掉。
  // 真正让 token 失效的是它的 12 小时有效期（见 KNOWN-LIMITATIONS）。
  context->state = ConnectionState::kConnected;
  context->user_id = 0;
  context->username.clear();
  return SendStatus(fd, header, Status::kOk, std::string(), error_message);
}

// 断线重连：客户端拿旧 token 在一条新连接上换回会话。校验包含 HMAC 与过期
// 时间（VerifyToken），通过之后不续期、不换发新 token，只把连接状态推进到
// kAuthenticated；这枚 token 还能用多久仍然由它自己的过期时间决定。
bool RemoteServer::HandleResume(int fd, const FrameHeader& header,
                                const std::string& payload,
                                ConnectionContext* context,
                                std::string* error_message) {
  if (context->state != ConnectionState::kConnected) {
    Log("rejecting RESUME while a session is already established");
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInvalidState, error_message);
  }
  PayloadReader reader(payload);
  std::string token;
  if (!reader.ReadString(kMaxTokenBytes, &token) || !reader.AtEnd()) {
    Log("rejecting a malformed RESUME: " + reader.error_message());
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInvalidRequest, error_message);
  }
  TokenPayload parsed;
  std::string verify_error;
  if (!VerifyToken(secret_, token, NowSeconds(), &parsed, &verify_error)) {
    // 失败原因只写日志：里面既没有 token 内容，也没有 secret。
    Log("resume rejected: " + verify_error);
    return SendError(fd, header.opcode, header.request_id,
                     Status::kUnauthorized, error_message);
  }
  // token 的签名说明"这是我们签发的"，但**不**说明"这个账户还在"：注销过的
  // 账户必须在这里被挡住。这一条正是"注销之后旧 token 立刻失效"的实现。
  RemoteUserRecord user;
  std::string store_error;
  const StoreResult found = store_->FindUserById(
      static_cast<std::int64_t>(parsed.user_id), &user, &store_error);
  if (found == StoreResult::kNotFound) {
    Log("resume rejected: the account no longer exists");
    return SendError(fd, header.opcode, header.request_id,
                     Status::kUnauthorized, error_message);
  }
  if (found != StoreResult::kOk) {
    Log("cannot read the user row: " + store_error);
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInternalError, error_message);
  }
  context->state = ConnectionState::kAuthenticated;
  context->user_id = parsed.user_id;
  context->username = user.username;
  Log("session resumed for user id=" + std::to_string(parsed.user_id));
  return SendStatus(fd, header, Status::kOk, std::string(), error_message);
}

// 列出当前用户的全部快照（按服务端元数据的顺序，不分页）。
// 响应布局即线上顺序：u32 count，然后每条 snapshot_id, display_name,
// sha256, size_bytes, created_at, kind, generation, parent_id, lineage
// （与 network_protocol.h 里 kList 的定义一致）。
//
// 链关系只被原样搬运，服务端不替客户端解释父链：客户端要自己沿 parent 走。
// 两条上限缺一不可：条目数超过 kMaxListEntries 直接拒绝；逐条累加后一旦整帧
// 会超过 1 MiB 也拒绝——绝不发一个超限的帧让对端去截断。
bool RemoteServer::HandleList(int fd, const FrameHeader& header,
                              ConnectionContext* context,
                              std::string* error_message) {
  if (context->state != ConnectionState::kAuthenticated) {
    Log("rejecting LIST without a session");
    return SendError(fd, header.opcode, header.request_id,
                     Status::kUnauthorized, error_message);
  }
  if (RejectIfAccountMissing(fd, header, context, error_message)) {
    return true;
  }
  std::vector<RemoteSnapshotRecord> records;
  std::string store_error;
  const StoreResult result = store_->ListSnapshots(
      static_cast<std::int64_t>(context->user_id), &records, &store_error);
  if (result != StoreResult::kOk) {
    Log("cannot list the snapshots: " + store_error);
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInternalError, error_message);
  }
  if (records.size() > kMaxListEntries) {
    Log("refusing to build a LIST response with more than " +
        std::to_string(kMaxListEntries) + " entries");
    return SendError(fd, header.opcode, header.request_id, Status::kTooLarge,
                     error_message);
  }
  PayloadBuilder builder;
  builder.AppendU32(static_cast<std::uint32_t>(records.size()));
  std::string build_error;
  for (const RemoteSnapshotRecord& record : records) {
    if (!builder.AppendString(record.snapshot_id, kMaxSnapshotIdBytes,
                              &build_error) ||
        !builder.AppendString(record.display_name, kMaxDisplayNameBytes,
                              &build_error) ||
        !builder.AppendString(record.sha256, kSha256HexBytes, &build_error)) {
      Log("cannot encode a LIST entry: " + build_error);
      return SendError(fd, header.opcode, header.request_id,
                       Status::kInternalError, error_message);
    }
    builder.AppendU64(record.size_bytes);
    builder.AppendU64(static_cast<std::uint64_t>(record.created_at));
    // 链元数据。客户端据此自己走父链，服务端不替它解释链。
    builder.AppendU16(record.snapshot_kind);
    builder.AppendU64(record.generation);
    if (!builder.AppendString(record.parent_id, kMaxSnapshotIdBytes,
                              &build_error) ||
        !builder.AppendString(record.lineage, kMaxLineageBytes, &build_error)) {
      Log("cannot encode a LIST entry: " + build_error);
      return SendError(fd, header.opcode, header.request_id,
                       Status::kInternalError, error_message);
    }
    // 一帧装不下就明确拒绝，绝不发一个超限的帧。
    if (builder.size() > kMaxPayloadBytes) {
      Log("the LIST response would exceed the 1 MiB frame limit");
      return SendError(fd, header.opcode, header.request_id, Status::kTooLarge,
                       error_message);
    }
  }
  return SendStatus(fd, header, Status::kOk, builder.data(), error_message);
}

namespace {

// rename 的持久性要靠父目录 fsync 才算完整：只 fsync 文件本身，
// 掉电后可能留下"文件内容在、目录项没落盘"的状态。
// 失败被刻意忽略：调用点都在 rename 成功之后，此刻把一次目录 fsync 失败升级
// 成致命错误，只会让一个已经可见的文件被回滚；它影响的仅仅是掉电后的持久性
// 窗口（可能丢刚发布的那条目录项），不影响运行期的一致性。
void FsyncDirectory(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd < 0) {
    return;
  }
  ::fsync(fd);
  ::close(fd);
}

}  // namespace

// 路径布局的唯一定义处：<root>/users/<十进制 uid>。子目录 tmp/ 放上传临时
// 文件、trash/ 放待物理删除的 blob，都由 EnsureUserDirectory 一次建好。
// 这里只做字符串拼接，所以路径里不可能出现 ".." 或用户提供的任何片段。
std::string RemoteServer::UserDirectory(std::int64_t user_id) const {
  // 磁盘路径永远只由服务端生成：数字 user id + 服务端生成的 snapshot id。
  // 客户端给的用户名与显示名一次都不参与拼接。
  return config_.root_directory + "/users/" + std::to_string(user_id);
}

// 幂等：UPLOAD_BEGIN / DOWNLOAD_BEGIN 每次都会调用它，重复调用只是多几次 stat。
// tmp/ 与 trash/ 是上传与删除两条路径各自的中间态目录，必须先于任何 open()
// 存在，否则第一次上传就会以 ENOENT 失败。
bool RemoteServer::EnsureUserDirectory(std::int64_t user_id,
                                       std::string* directory,
                                       std::string* error_message) {
  const std::string base = UserDirectory(user_id);
  if (!EnsureDirectory(base, error_message) ||
      !EnsureDirectory(base + "/tmp", error_message) ||
      !EnsureDirectory(base + "/trash", error_message)) {
    return false;
  }
  if (directory != nullptr) {
    *directory = base;
  }
  return true;
}

// 128 位 CSPRNG 随机数转 32 位十六进制：不可猜测，因此它不能当枚举 id 的
// 起点；唯一性靠两层兜底——临时文件用 O_EXCL 创建，元数据表对
// (user_id, snapshot_id) 有唯一约束。
bool RemoteServer::GenerateSnapshotId(std::string* snapshot_id,
                                      std::string* error_message) {
  std::string raw;
  std::string random_error;
  if (!crypto::RandomBytes(16, &raw, &random_error)) {
    if (error_message != nullptr) {
      *error_message = "cannot generate a snapshot id: " + random_error;
    }
    return false;
  }
  *snapshot_id = crypto::ToHex(
      reinterpret_cast<const unsigned char*>(raw.data()), raw.size());
  return true;
}

// 循环写直到写完：write() 允许短写，返回 0 在普通文件上意味着磁盘或文件系统
// 出了问题，两者都不能当成"写完了"；EINTR 只重试不报错。
// 返回 false 时调用方必须走 AbortUpload（删掉临时文件），不能让半截 blob 留下。
// fail_next_write_ 是测试注入点，产品代码不设置它。
bool RemoteServer::WriteAll(int fd, const char* data, std::size_t size,
                            std::string* error_message) {
  if (fail_next_write_) {
    fail_next_write_ = false;
    if (error_message != nullptr) {
      *error_message = "injected blob write failure (test seam)";
    }
    return false;
  }
  std::size_t written = 0;
  while (written < size) {
    const ssize_t step = ::write(fd, data + written, size - written);
    if (step < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (error_message != nullptr) {
        *error_message = std::string("write failed: ") + std::strerror(errno);
      }
      return false;
    }
    if (step == 0) {
      if (error_message != nullptr) {
        *error_message = "write returned zero";
      }
      return false;
    }
    written += static_cast<std::size_t>(step);
  }
  return true;
}

void RemoteServer::FailNextMetadataInsertForTesting() {
  if (store_ != nullptr) {
    store_->FailNextInsertForTesting();
  }
}

// 只清内存状态，不碰磁盘：发布成功之后临时文件已经被 rename 成正式名字，这时
// 再删就等于删掉刚发布的 blob。失败路径要的是 AbortUpload。
// 状态回到 kAuthenticated 而不是 kConnected：会话仍然有效，只是这次传输结束。
// 摘要器用整体赋值复位（Sha256 只能 Final 一次，没有 reset）。
void RemoteServer::ResetUploadState(ConnectionContext* context) {
  context->upload_display_name.clear();
  context->upload_declared_size = 0;
  context->upload_sha256.clear();
  context->upload_snapshot_id.clear();
  context->upload_temp_path.clear();
  context->upload_received = 0;
  context->upload_fd = -1;
  context->upload_hasher = crypto::Sha256();
  context->state = ConnectionState::kAuthenticated;
}

// 顺序：先关 fd 再 unlink（让"文件已经不可写"立刻成立），最后复位状态。
// unlink 失败只记一行警告：错误已经发生，删不掉临时文件不该掩盖原始错误。
void RemoteServer::AbortUpload(ConnectionContext* context) {
  if (context->upload_fd >= 0) {
    ::close(context->upload_fd);
    context->upload_fd = -1;
  }
  if (!context->upload_temp_path.empty()) {
    // 失败路径绝不留下半个文件：临时文件在这里被删掉。
    if (::unlink(context->upload_temp_path.c_str()) != 0 && errno != ENOENT) {
      Log("warning: could not remove the upload temp file");
    }
  }
  ResetUploadState(context);
}

// 只关句柄与清下载状态，token / 会话一概不动：下载失败不是"退出登录"。
// 状态只在 kDownloadInProgress 时回落，避免把正在上传的连接错误地拉回
// kAuthenticated（上传与下载共用同一个 state 字段）。
void RemoteServer::CloseDownload(ConnectionContext* context) {
  if (context->download_fd >= 0) {
    ::close(context->download_fd);
    context->download_fd = -1;
  }
  context->download_snapshot_id.clear();
  context->download_size = 0;
  context->download_sha256.clear();
  context->download_sent = 0;
  if (context->state == ConnectionState::kDownloadInProgress) {
    context->state = ConnectionState::kAuthenticated;
  }
}

// 它只碰 ConnectionContext，不关 fd：fd 的 owner 是 worker（见 WorkerLoop），
// 这样"连接没了"与"会话状态清了"两件事各自只有一个负责人。
void RemoteServer::CleanupConnection(ConnectionContext* context) {
  // 两个都要做：客户端半路断开时上传要删临时文件、下载要关句柄。
  if (context->state == ConnectionState::kUploadInProgress ||
      context->upload_fd >= 0) {
    AbortUpload(context);
  }
  CloseDownload(context);
}

// 一次上传的协商阶段：定下名字、长度、摘要与链关系，并创建临时文件。真正的
// 数据在 UPLOAD_CHUNK 里来，发布在 UPLOAD_END 里做。
//
// 字段解码顺序必须与客户端 PayloadBuilder 的顺序逐字一致：
//   display_name, declared_size, declared_sha256,
//   snapshot_kind, parent_snapshot_id, lineage
// 少一个或多一个字节都算 malformed：尾部多出字段说明两端的字段理解已经分叉，
// 这时"读到自己要的就当成功"只会把错误推到更难查的地方。
//
// 这里不信任客户端声明的任何东西：display_name / sha256 / lineage 过校验器；
// parent 必须是本用户的、已存在的快照（不存在的父与别人的父回同一个 kNotFound，
// 因此这个接口不能用来探测别人的 id）；generation 完全由服务端按
// parent.generation + 1 推导，客户端没有字段可以自己填。
//
// 状态迁移只在最后一步发生：前面每一条失败路径都还没创建文件、也还没改 state，
// 因此不需要回滚。
bool RemoteServer::HandleUploadBegin(int fd, const FrameHeader& header,
                                     const std::string& payload,
                                     ConnectionContext* context,
                                     std::string* error_message) {
  if (context->state != ConnectionState::kAuthenticated) {
    Log("rejecting UPLOAD_BEGIN outside an authenticated session");
    return SendError(fd, header.opcode, header.request_id,
                     Status::kUnauthorized, error_message);
  }
  if (RejectIfAccountMissing(fd, header, context, error_message)) {
    return true;
  }
  PayloadReader reader(payload);
  std::string display_name;
  std::uint64_t declared_size = 0;
  std::string declared_sha256;
  std::uint16_t snapshot_kind = 0;
  std::string parent_snapshot_id;
  std::string lineage;
  if (!reader.ReadString(kMaxDisplayNameBytes, &display_name) ||
      !reader.ReadU64(&declared_size) ||
      !reader.ReadString(kSha256HexBytes, &declared_sha256) ||
      !reader.ReadU16(&snapshot_kind) ||
      !reader.ReadString(kMaxSnapshotIdBytes, &parent_snapshot_id) ||
      !reader.ReadString(kMaxLineageBytes, &lineage) || !reader.AtEnd()) {
    // 旧客户端的 UPLOAD_BEGIN 少了后面三个字段：这里会因为
    // 读不到 / 有尾巴而拒绝。这是刻意的——加密与链元数据一起上线，
    // 不存在"勉强接受旧格式"的分支。
    Log("rejecting a malformed UPLOAD_BEGIN: " + reader.error_message());
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInvalidRequest, error_message);
  }
  std::string validation_error;
  if (!IsValidDisplayName(display_name, &validation_error) ||
      !IsValidSha256Hex(declared_sha256, &validation_error)) {
    Log("rejecting UPLOAD_BEGIN metadata: " + validation_error);
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInvalidRequest, error_message);
  }
  // lineage：完整快照**允许为空**——空串表示这一份不属于任何链（旧版上传
  // 的数据与低层 remote upload 都是这一类）。非空时必须是 64 位十六进
  // 制；增量则**必须**带一个合法的链标识：没有链标识的增量在语义上不存在。
  if (!IsKnownSnapshotKind(snapshot_kind)) {
    Log("rejecting UPLOAD_BEGIN with an unknown snapshot kind");
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInvalidRequest, error_message);
  }
  if (!lineage.empty() && !IsValidSha256Hex(lineage, &validation_error)) {
    Log("rejecting UPLOAD_BEGIN with a malformed lineage: " + validation_error);
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInvalidRequest, error_message);
  }
  if (snapshot_kind == static_cast<std::uint16_t>(SnapshotKind::kIncremental) &&
      lineage.empty()) {
    Log("rejecting an incremental snapshot without a lineage");
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInvalidRequest, error_message);
  }
  // ---- 链关系校验。全部由服务端做，客户端说什么都要在这里过一遍 ----
  //
  //   full          parent 必须为空、generation 恒为 0（新链的根）
  //   incremental   parent 必须已经存在、属于**同一个用户**、lineage 相同；
  //                 generation = parent.generation + 1（服务端算的）
  //
  // 因为 parent 只能指向**已经存在**的不可变快照，所以不可能出现指向未来的
  // 环（self parent 也一样：自己的 id 是在这之后才生成的）。
  // 客户端根本没有 generation 字段可填，所以"代数跳跃"在这条路径上不存在。
  std::uint64_t generation = 0;
  if (snapshot_kind == static_cast<std::uint16_t>(SnapshotKind::kFull)) {
    if (!parent_snapshot_id.empty()) {
      Log("rejecting a full snapshot that declares a parent");
      return SendError(fd, header.opcode, header.request_id,
                       Status::kInvalidRequest, error_message);
    }
  } else {
    if (parent_snapshot_id.empty()) {
      Log("rejecting an incremental snapshot without a parent");
      return SendError(fd, header.opcode, header.request_id,
                       Status::kInvalidRequest, error_message);
    }
    std::string parent_validation;
    if (!IsValidSnapshotId(parent_snapshot_id, &parent_validation)) {
      Log("rejecting an incremental snapshot with a malformed parent id");
      return SendError(fd, header.opcode, header.request_id,
                       Status::kInvalidRequest, error_message);
    }
    RemoteSnapshotRecord parent;
    std::string parent_error;
    const StoreResult parent_result =
        store_->FindSnapshot(static_cast<std::int64_t>(context->user_id),
                             parent_snapshot_id, &parent, &parent_error);
    if (parent_result == StoreResult::kNotFound) {
      // 不存在的父与"别人的父"是同一个答案：不允许按 id 探测别人的快照。
      Log("rejecting an incremental upload: the parent snapshot does not "
          "exist");
      return SendError(fd, header.opcode, header.request_id, Status::kNotFound,
                       error_message);
    }
    if (parent_result != StoreResult::kOk) {
      Log("cannot read the parent snapshot row: " + parent_error);
      return SendError(fd, header.opcode, header.request_id,
                       Status::kInternalError, error_message);
    }
    if (parent.lineage != lineage) {
      Log("rejecting an incremental upload across lineages");
      return SendError(fd, header.opcode, header.request_id,
                       Status::kInvalidRequest, error_message);
    }
    generation = parent.generation + 1;
  }
  if (declared_size == 0) {
    // 0 字节的归档不是合法归档。产品明确拒绝，而不是存一个空文件——
    // 否则"上传成功"会掩盖客户端读文件读空了这件事。
    Log("rejecting UPLOAD_BEGIN with a declared size of zero");
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInvalidRequest, error_message);
  }
  if (declared_size > config_.max_upload_bytes) {
    Log("rejecting UPLOAD_BEGIN above the configured upload limit");
    return SendError(fd, header.opcode, header.request_id, Status::kTooLarge,
                     error_message);
  }
  std::string directory;
  if (!EnsureUserDirectory(static_cast<std::int64_t>(context->user_id),
                           &directory, error_message)) {
    Log("cannot create the user directory: " + *error_message);
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInternalError, error_message);
  }
  std::string snapshot_id;
  std::string id_error;
  if (!GenerateSnapshotId(&snapshot_id, &id_error)) {
    Log("cannot generate a snapshot id: " + id_error);
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInternalError, error_message);
  }
  const std::string temp_path = directory + "/tmp/" + snapshot_id + ".part";
  const int temp_fd =
      ::open(temp_path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (temp_fd < 0) {
    Log(std::string("cannot create the upload temp file: ") +
        std::strerror(errno));
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInternalError, error_message);
  }
  context->state = ConnectionState::kUploadInProgress;
  context->upload_display_name = display_name;
  context->upload_declared_size = declared_size;
  context->upload_sha256 = declared_sha256;
  context->upload_snapshot_id = snapshot_id;
  context->upload_temp_path = temp_path;
  context->upload_received = 0;
  context->upload_fd = temp_fd;
  context->upload_hasher = crypto::Sha256();
  context->upload_kind = snapshot_kind;
  context->upload_parent_id = parent_snapshot_id;
  context->upload_generation = generation;
  context->upload_lineage = lineage;
  return SendStatus(fd, header, Status::kOk, std::string(), error_message);
}

// 数据块：写进临时文件，同时把同一份字节喂给摘要器（顺序读一遍就同时完成
// 落盘与校验）。写得进去不等于被接受——累计长度一旦超过声明的 size，立刻
// AbortUpload 并结束这次上传：临时文件删掉、连接回到 kAuthenticated，之后的
// UPLOAD_END 会因为状态不对被拒。
bool RemoteServer::HandleUploadChunk(int fd, const FrameHeader& header,
                                     const std::string& payload,
                                     ConnectionContext* context,
                                     std::string* error_message) {
  if (context->state != ConnectionState::kUploadInProgress) {
    Log("rejecting UPLOAD_CHUNK without an active upload");
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInvalidState, error_message);
  }
  if (payload.empty()) {
    // 空块没有任何意义，而且会掩盖客户端的边界 bug：明确拒绝。
    Log("rejecting an empty UPLOAD_CHUNK");
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInvalidRequest, error_message);
  }
  if (payload.size() > kTransferChunkBytes) {
    Log("rejecting an UPLOAD_CHUNK larger than the chunk limit");
    return SendError(fd, header.opcode, header.request_id, Status::kTooLarge,
                     error_message);
  }
  if (context->upload_received + payload.size() >
      context->upload_declared_size) {
    Log("upload exceeded its declared size; aborting");
    AbortUpload(context);
    return SendError(fd, header.opcode, header.request_id, Status::kTooLarge,
                     error_message);
  }
  std::string write_error;
  if (!WriteAll(context->upload_fd, payload.data(), payload.size(),
                &write_error)) {
    Log("cannot write the upload temp file: " + write_error);
    AbortUpload(context);
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInternalError, error_message);
  }
  context->upload_hasher.Update(payload.data(), payload.size());
  context->upload_received += payload.size();
  return SendStatus(fd, header, Status::kOk, std::string(), error_message);
}

// 发布阶段：先自证收到的东西是对的（长度与 SHA-256 都要与 UPLOAD_BEGIN 声明的
// 一致），再按 fsync -> close -> rename -> 目录 fsync -> SQLite 的顺序落盘。
//
// 崩溃一致性不变量：blob 先可见、元数据后写。反过来会出现"记录在、文件不在"，
// 那种状态会让 LIST 出来的快照下载失败；而先写 blob 的窗口里崩溃只会留下一个
// 没有任何记录指向的孤儿文件——它不会被 LIST 列出，只占磁盘。所以 DB 写失败时
// 要把已经 rename 出去的 blob 删掉（并再 fsync 一次目录），而那次 unlink 失败
// 只记警告：一致性已经由元数据侧保证了。
//
// 摘要器只能 Final 一次，所以这里在副本上收尾：失败路径还要靠原对象复位状态。
bool RemoteServer::HandleUploadEnd(int fd, const FrameHeader& header,
                                   const std::string& payload,
                                   ConnectionContext* context,
                                   std::string* error_message) {
  if (context->state != ConnectionState::kUploadInProgress) {
    Log("rejecting UPLOAD_END without an active upload");
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInvalidState, error_message);
  }
  if (!payload.empty()) {
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInvalidRequest, error_message);
  }
  // 上传期间账户可能已经被另一个连接注销了：发布之前再确认一次账户还在，
  // 否则一份已注销账户的 blob 会被写进磁盘并留下元数据行。
  if (RejectIfAccountMissing(fd, header, context, error_message)) {
    return true;
  }
  const std::uint64_t received = context->upload_received;
  const std::uint64_t declared = context->upload_declared_size;
  const std::string snapshot_id = context->upload_snapshot_id;
  const std::string display_name = context->upload_display_name;
  const std::string declared_sha256 = context->upload_sha256;
  const std::string temp_path = context->upload_temp_path;
  const int temp_fd = context->upload_fd;
  // 链关系在 UPLOAD_BEGIN 时就已经校验并定下来；这里只把它落到记录与响应里。
  const std::uint16_t snapshot_kind = context->upload_kind;
  const std::string parent_snapshot_id = context->upload_parent_id;
  const std::uint64_t generation = context->upload_generation;
  const std::string lineage = context->upload_lineage;

  // 摘要器只能 Final 一次，所以先在副本上收尾，失败路径还要继续用它清场。
  crypto::Sha256 hasher = context->upload_hasher;
  unsigned char digest[crypto::kSha256DigestSize];
  hasher.Final(digest);
  const std::string actual_sha256 =
      crypto::ToHex(digest, crypto::kSha256DigestSize);

  if (received != declared) {
    Log("upload size mismatch: got " + std::to_string(received) + " of " +
        std::to_string(declared) + " bytes");
    AbortUpload(context);
    return SendError(fd, header.opcode, header.request_id,
                     Status::kIntegrityMismatch, error_message);
  }
  if (actual_sha256 != declared_sha256) {
    Log("upload hash mismatch; the blob was not published");
    AbortUpload(context);
    return SendError(fd, header.opcode, header.request_id,
                     Status::kIntegrityMismatch, error_message);
  }

  // ---- 发布顺序：fsync -> close -> rename -> 目录 fsync -> SQLite ----
  if (::fsync(temp_fd) != 0) {
    Log(std::string("fsync failed: ") + std::strerror(errno));
    AbortUpload(context);
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInternalError, error_message);
  }
  if (::close(temp_fd) != 0) {
    Log(std::string("close failed: ") + std::strerror(errno));
    context->upload_fd = -1;
    AbortUpload(context);
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInternalError, error_message);
  }
  context->upload_fd = -1;

  const std::string directory =
      UserDirectory(static_cast<std::int64_t>(context->user_id));
  const std::string final_path = directory + "/" + snapshot_id + ".bak";
  if (::rename(temp_path.c_str(), final_path.c_str()) != 0) {
    Log(std::string("cannot publish the blob: ") + std::strerror(errno));
    AbortUpload(context);
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInternalError, error_message);
  }
  FsyncDirectory(directory);

  const std::int64_t now = static_cast<std::int64_t>(NowSeconds());
  RemoteSnapshotRecord record;
  record.snapshot_id = snapshot_id;
  record.user_id = static_cast<std::int64_t>(context->user_id);
  record.display_name = display_name;
  record.size_bytes = declared;
  record.sha256 = actual_sha256;
  record.created_at = now;
  record.storage_name = snapshot_id + ".bak";
  record.snapshot_kind = snapshot_kind;
  record.parent_id = parent_snapshot_id;
  record.generation = generation;
  record.lineage = lineage;

  std::string store_error;
  const StoreResult result = store_->InsertSnapshot(record, &store_error);
  if (result != StoreResult::kOk) {
    // DB 写失败：把已经 rename 出去的 blob 删掉，绝不留"文件在、记录不在"。
    Log("cannot record the snapshot; rolling the published blob back: " +
        store_error);
    if (::unlink(final_path.c_str()) != 0) {
      Log("warning: the rollback unlink failed; an orphan blob remains");
    }
    FsyncDirectory(directory);
    ResetUploadState(context);
    Status status = Status::kInternalError;
    if (result == StoreResult::kAlreadyExists) {
      status = Status::kAlreadyExists;
    } else if (result == StoreResult::kNotFound) {
      // 父在 UPLOAD_BEGIN 之后被删掉了：如实告诉客户端"链已经变了"。
      status = Status::kChainConflict;
    } else if (result == StoreResult::kChainConflict) {
      status = Status::kChainConflict;
    }
    return SendError(fd, header.opcode, header.request_id, status,
                     error_message);
  }

  ResetUploadState(context);
  PayloadBuilder builder;
  std::string build_error;
  if (!builder.AppendString(snapshot_id, kMaxSnapshotIdBytes, &build_error) ||
      !builder.AppendString(actual_sha256, kSha256HexBytes, &build_error)) {
    Log("cannot encode the UPLOAD_END response: " + build_error);
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInternalError, error_message);
  }
  builder.AppendU64(declared);
  builder.AppendU64(static_cast<std::uint64_t>(now));
  builder.AppendU16(snapshot_kind);
  builder.AppendU64(generation);
  if (!builder.AppendString(parent_snapshot_id, kMaxSnapshotIdBytes,
                            &build_error)) {
    Log("cannot encode the UPLOAD_END response: " + build_error);
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInternalError, error_message);
  }
  Log("published snapshot " + snapshot_id + " (" + std::to_string(declared) +
      " bytes, kind=" + std::to_string(snapshot_kind) +
      " generation=" + std::to_string(generation) + ")");
  return SendStatus(fd, header, Status::kOk, builder.data(), error_message);
}

// 下载的协商阶段：只回元数据（显示名、SHA-256、长度），一个数据字节都不发；
// 数据由客户端用 DOWNLOAD_CHUNK 一份一份来取。这样"客户端要下到哪"与"服务端
// 怎么读盘"解耦，客户端可以随时中止而不需要服务端猜。
//
// 打开 blob 之前有三道检查：storage_name 必须是规范文件名（纵深防御，防路径
// 拼接）、文件必须存在、大小必须与元数据一致。任何一条不过都回 kNotFound——
// 对客户端来说"记录在但文件不在"属于服务端内部问题，没必要也不应该区分。
// 用 fstat 而不是 stat：检查与读取必须针对同一个 fd，中间不会被换掉。
bool RemoteServer::HandleDownloadBegin(int fd, const FrameHeader& header,
                                       const std::string& payload,
                                       ConnectionContext* context,
                                       std::string* error_message) {
  if (context->state != ConnectionState::kAuthenticated) {
    Log("rejecting DOWNLOAD_BEGIN outside an authenticated session");
    return SendError(fd, header.opcode, header.request_id,
                     Status::kUnauthorized, error_message);
  }
  if (RejectIfAccountMissing(fd, header, context, error_message)) {
    return true;
  }
  PayloadReader reader(payload);
  std::string snapshot_id;
  if (!reader.ReadString(kMaxSnapshotIdBytes, &snapshot_id) ||
      !reader.AtEnd()) {
    Log("rejecting a malformed DOWNLOAD_BEGIN: " + reader.error_message());
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInvalidRequest, error_message);
  }
  std::string validation_error;
  if (!IsValidSnapshotId(snapshot_id, &validation_error)) {
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInvalidRequest, error_message);
  }
  // 查询自带 user_id 过滤：别人的快照与不存在的快照返回同一个答案，
  // 因此这个接口不能被用来探测"某个 id 是否存在"。
  RemoteSnapshotRecord record;
  std::string store_error;
  const StoreResult result =
      store_->FindSnapshot(static_cast<std::int64_t>(context->user_id),
                           snapshot_id, &record, &store_error);
  if (result == StoreResult::kNotFound) {
    Log("download rejected: no such snapshot for this user");
    return SendError(fd, header.opcode, header.request_id, Status::kNotFound,
                     error_message);
  }
  if (result != StoreResult::kOk) {
    Log("cannot read the snapshot row: " + store_error);
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInternalError, error_message);
  }
  // 纵深防御：storage_name 是服务端自己写进去的，但仍然确认它是个纯文件名。
  if (record.storage_name.empty() ||
      record.storage_name.find('/') != std::string::npos ||
      record.storage_name.find('\\') != std::string::npos ||
      record.storage_name != record.snapshot_id + ".bak") {
    Log("refusing to open a snapshot whose storage name is not canonical");
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInternalError, error_message);
  }
  const std::string path =
      UserDirectory(record.user_id) + "/" + record.storage_name;
  const int blob_fd = ::open(path.c_str(), O_RDONLY);
  if (blob_fd < 0) {
    // 记录在、文件不在：这是服务端内部不一致，对客户端只说"没找到"。
    Log("metadata row has no blob on disk (internal inconsistency)");
    return SendError(fd, header.opcode, header.request_id, Status::kNotFound,
                     error_message);
  }
  struct stat info;
  std::memset(&info, 0, sizeof(info));
  if (::fstat(blob_fd, &info) != 0 || !S_ISREG(info.st_mode) ||
      static_cast<std::uint64_t>(info.st_size) != record.size_bytes) {
    Log("the blob on disk does not match its metadata; refusing to serve it");
    ::close(blob_fd);
    return SendError(fd, header.opcode, header.request_id, Status::kNotFound,
                     error_message);
  }
  context->state = ConnectionState::kDownloadInProgress;
  context->download_snapshot_id = record.snapshot_id;
  context->download_size = record.size_bytes;
  context->download_sha256 = record.sha256;
  context->download_sent = 0;
  context->download_fd = blob_fd;

  PayloadBuilder builder;
  std::string build_error;
  if (!builder.AppendString(record.display_name, kMaxDisplayNameBytes,
                            &build_error) ||
      !builder.AppendString(record.sha256, kSha256HexBytes, &build_error)) {
    CloseDownload(context);
    Log("cannot encode the DOWNLOAD_BEGIN response: " + build_error);
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInternalError, error_message);
  }
  builder.AppendU64(record.size_bytes);
  return SendStatus(fd, header, Status::kOk, builder.data(), error_message);
}

// 顺序读一个块（不超过 256 KiB）并原样装进响应 payload。请求 payload 必须为
// 空：偏移量由服务端自己维护（download_sent），客户端不能指定——没有随机读，
// 也就没有"用偏移量去探测别人的数据"这条路。
// 空 payload 是流结束的信号（读到的字节数为 0），客户端据此停止循环；累计发送
// 数超过元数据声明的长度说明磁盘上的文件被换过，直接中止这次下载。
bool RemoteServer::HandleDownloadChunk(int fd, const FrameHeader& header,
                                       const std::string& payload,
                                       ConnectionContext* context,
                                       std::string* error_message) {
  if (context->state != ConnectionState::kDownloadInProgress) {
    Log("rejecting DOWNLOAD_CHUNK without an active download");
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInvalidState, error_message);
  }
  if (!payload.empty()) {
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInvalidRequest, error_message);
  }
  std::string chunk(kTransferChunkBytes, '\0');
  ssize_t got = 0;
  for (;;) {
    got = ::read(context->download_fd, &chunk[0], chunk.size());
    if (got < 0 && errno == EINTR) {
      continue;
    }
    break;
  }
  if (got < 0) {
    Log(std::string("cannot read the blob: ") + std::strerror(errno));
    CloseDownload(context);
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInternalError, error_message);
  }
  if (got == 0) {
    // 空 payload = 流结束。客户端随后发 DOWNLOAD_END 收尾。
    chunk.clear();
  } else {
    chunk.resize(static_cast<std::size_t>(got));
  }
  context->download_sent += chunk.size();
  if (context->download_sent > context->download_size) {
    Log("the blob on disk is longer than its metadata; aborting the download");
    CloseDownload(context);
    return SendError(fd, header.opcode, header.request_id, Status::kNotFound,
                     error_message);
  }
  return SendStatus(fd, header, Status::kOk, chunk, error_message);
}

// 客户端说"我不要了，收尾吧"。sent != size 是允许的（客户端可以提前中止），
// 只记一行日志：真实原因在对端，服务端这里没有可执行的补救动作。
// 服务端不复核摘要——摘要在 BEGIN 里已经给出，由客户端在发布之前自己验。
bool RemoteServer::HandleDownloadEnd(int fd, const FrameHeader& header,
                                     const std::string& payload,
                                     ConnectionContext* context,
                                     std::string* error_message) {
  if (context->state != ConnectionState::kDownloadInProgress) {
    Log("rejecting DOWNLOAD_END without an active download");
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInvalidState, error_message);
  }
  if (!payload.empty()) {
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInvalidRequest, error_message);
  }
  const std::uint64_t sent = context->download_sent;
  const std::uint64_t size = context->download_size;
  CloseDownload(context);
  if (sent != size) {
    // 客户端提前收手：对端自己知道，这里只记一行日志。
    Log("download ended before the whole blob was sent");
  }
  return SendStatus(fd, header, Status::kOk, std::string(), error_message);
}

// 删一个快照。真正的顺序（先挪进 trash 变不可见 -> 删元数据行 -> 失败改回
// 名字 -> 物理 unlink）全在 RemoteMaintenance 里，因为 backup-server-admin
// 走的是同一份实现：管理员没有第二条能绕过依赖检查的删除路径。
// 还有后代的快照回 kInvalidState 而不是内部错误：这是"当前状态不允许"。
bool RemoteServer::HandleDelete(int fd, const FrameHeader& header,
                                const std::string& payload,
                                ConnectionContext* context,
                                std::string* error_message) {
  if (context->state != ConnectionState::kAuthenticated) {
    Log("rejecting DELETE outside an authenticated session");
    return SendError(fd, header.opcode, header.request_id,
                     Status::kUnauthorized, error_message);
  }
  if (RejectIfAccountMissing(fd, header, context, error_message)) {
    return true;
  }
  PayloadReader reader(payload);
  std::string snapshot_id;
  if (!reader.ReadString(kMaxSnapshotIdBytes, &snapshot_id) ||
      !reader.AtEnd()) {
    Log("rejecting a malformed DELETE: " + reader.error_message());
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInvalidRequest, error_message);
  }
  std::string validation_error;
  if (!IsValidSnapshotId(snapshot_id, &validation_error)) {
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInvalidRequest, error_message);
  }
  const std::int64_t user_id = static_cast<std::int64_t>(context->user_id);
  RemoteSnapshotRecord removed;
  std::string delete_error;
  // 删除的全部顺序（先挪成不可见、再删元数据、失败回滚）在共享的
  // RemoteMaintenance 里：ECS 本地的 backup-server-admin 走的是同一条路径，
  // 不存在"管理工具另有一套删除逻辑"这种分叉。
  const StoreResult deleted = maintenance_->DeleteSnapshot(
      user_id, snapshot_id, &removed, &delete_error);
  if (deleted != StoreResult::kOk) {
    Log("delete failed for snapshot " + snapshot_id + ": " + delete_error);
    if (deleted == StoreResult::kHasDependents) {
      // 链不能从中间断开：这个快照还有增量后代，必须先删后代。
      // 管理工具走的是同一个 RemoteMaintenance，所以管理员也绕不过这条规则。
      return SendError(fd, header.opcode, header.request_id,
                       Status::kInvalidState, error_message);
    }
    return SendError(fd, header.opcode, header.request_id,
                     deleted == StoreResult::kNotFound ? Status::kNotFound
                                                       : Status::kInternalError,
                     error_message);
  }
  Log("deleted snapshot " + snapshot_id);
  return SendStatus(fd, header, Status::kOk, std::string(), error_message);
}

// 注销账户：不可逆，所以要口令二次确认——一个被捡到的 token 不足以删号。
// 目标账户只能是 token 所属的那个 user_id：载荷里只有口令，协议里没有任何
// 字段能指定"删谁"，删别人的账户在这条路径上不可表达。
//
// 服务端动作分三步：把 users/<uid>/ 原子改名成隔离目录（一步就让全部字节
// 不可见）-> 在一个事务里删掉该用户的全部元数据 -> 物理删除隔离目录。
// 失败时隔离动作会被回滚，数据与元数据都还在，所以这里如实回 kInternalError，
// 绝不假装删除成功。成功之后这条连接的会话立刻失效；别的连接上那些签名仍然
// 有效的旧 token 会在下一次操作时被 RejectIfAccountMissing 挡掉。
bool RemoteServer::HandleDeleteAccount(int fd, const FrameHeader& header,
                                       const std::string& payload,
                                       ConnectionContext* context,
                                       std::string* error_message) {
  if (context->state != ConnectionState::kAuthenticated) {
    Log("rejecting DELETE_ACCOUNT outside an authenticated session");
    return SendError(fd, header.opcode, header.request_id,
                     Status::kUnauthorized, error_message);
  }
  // 载荷只有口令。目标账户永远是 token 自己所属的那个 user id：协议里没有
  // 任何字段可以让调用方指定"删谁"，删别人的账户在这条路径上不可表达。
  PayloadReader reader(payload);
  std::string password;
  if (!reader.ReadString(kMaxPasswordBytes, &password) || !reader.AtEnd()) {
    Log("rejecting a malformed DELETE_ACCOUNT: " + reader.error_message());
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInvalidRequest, error_message);
  }
  std::string validation_error;
  if (!IsValidPassword(password, &validation_error)) {
    Log("rejecting DELETE_ACCOUNT with an invalid password field");
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInvalidRequest, error_message);
  }
  // 一个有效的 token 不足以注销账户：token 可能被别人捡到，而注销不可逆。
  // 服务端重新校验一次当前口令。
  RemoteUserRecord user;
  std::string store_error;
  const StoreResult found = store_->FindUserById(
      static_cast<std::int64_t>(context->user_id), &user, &store_error);
  if (found == StoreResult::kNotFound) {
    Log("delete-account rejected: the account no longer exists");
    context->state = ConnectionState::kConnected;
    context->user_id = 0;
    context->username.clear();
    return SendError(fd, header.opcode, header.request_id,
                     Status::kUnauthorized, error_message);
  }
  if (found != StoreResult::kOk) {
    Log("cannot read the user row: " + store_error);
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInternalError, error_message);
  }
  bool matches = false;
  std::string verify_error;
  if (!VerifyPassword(password, user.password, &matches, &verify_error)) {
    Log("cannot verify the stored password: " + verify_error);
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInternalError, error_message);
  }
  if (!matches) {
    // 口令本身绝不进日志——连长度都不写。
    Log("delete-account rejected: wrong password for user id=" +
        std::to_string(user.user_id));
    return SendError(fd, header.opcode, header.request_id,
                     Status::kUnauthorized, error_message);
  }
  const std::int64_t user_id = static_cast<std::int64_t>(user.user_id);
  std::uint64_t removed_snapshots = 0;
  std::uint64_t removed_bytes = 0;
  const StoreResult deleted = maintenance_->DeleteAccount(
      user_id, &removed_snapshots, &removed_bytes, &store_error);
  if (deleted != StoreResult::kOk) {
    // 失败时数据与元数据都还在（隔离动作已经被回滚）：如实回错误，
    // 不假装删除成功。
    Log("delete-account failed for user id=" + std::to_string(user_id) + ": " +
        store_error);
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInternalError, error_message);
  }
  // 这条连接上的会话立即失效。别的连接上那些签名仍然有效的旧 token 会在
  // 每次操作前被 RejectIfAccountMissing 挡掉——账户已经不存在了。
  context->state = ConnectionState::kConnected;
  context->user_id = 0;
  context->username.clear();
  {
    std::ostringstream line;
    line << "account deleted: user id=" << user_id
         << " snapshots=" << removed_snapshots << " bytes=" << removed_bytes;
    Log(line.str());
  }
  return SendStatus(fd, header, Status::kOk, std::string(), error_message);
}

// 代价是每个数据操作一次按主键的 SQLite 点查，换来的是"注销立即生效"。
// 返回 true 时错误帧已经发出，调用方只能直接 return true：不能继续做事，
// 也不能再发第二帧。
bool RemoteServer::RejectIfAccountMissing(int fd, const FrameHeader& header,
                                          ConnectionContext* context,
                                          std::string* error_message) {
  RemoteUserRecord user;
  std::string store_error;
  const StoreResult result = store_->FindUserById(
      static_cast<std::int64_t>(context->user_id), &user, &store_error);
  if (result == StoreResult::kOk) {
    return false;
  }
  if (result == StoreResult::kNotFound) {
    // 账户注销之后，之前签发的 token 在密码学上依然有效（服务端不保存会话
    // 表），所以"账户还在不在"必须每次回查。这里就是那道闸门。
    Log("rejecting an operation for an account that no longer exists");
    context->state = ConnectionState::kConnected;
    context->user_id = 0;
    context->username.clear();
    SendError(fd, header.opcode, header.request_id, Status::kUnauthorized,
              error_message);
    return true;
  }
  Log("cannot read the user row: " + store_error);
  SendError(fd, header.opcode, header.request_id, Status::kInternalError,
            error_message);
  return true;
}

// flock 绑定在打开的 fd 上：进程崩溃或被杀时内核自动释放，所以不会留下需要
// 人工清理的 stale 锁文件（这正是它比 pid 文件可靠的地方）。
// 抢不到时把锁文件里的提示（谁、什么时候、在做什么）附在错误里，让运维一眼
// 看出是"服务端正在跑"还是"管理工具正在做破坏性操作"。
bool RemoteServer::AcquireDataLock(std::string* error_message) {
  data_lock_.reset(new backupproject::FileLock());
  const std::string path =
      RemoteMaintenance::LockFilePath(config_.root_directory);
  std::string lock_error;
  const backupproject::FileLockStatus status =
      data_lock_->Acquire(path, &lock_error);
  if (status == backupproject::FileLockStatus::kAcquired) {
    Log("data directory lock acquired: " + path);
    return true;
  }
  data_lock_.reset();
  if (status == backupproject::FileLockStatus::kBusy) {
    const std::string hint =
        RemoteMaintenance::ReadLockHint(config_.root_directory);
    if (error_message != nullptr) {
      *error_message = "another process is already using the data directory " +
                       config_.root_directory +
                       (hint.empty() ? std::string() : " (" + hint + ")");
    }
    return false;
  }
  if (error_message != nullptr) {
    *error_message = "cannot lock the data directory: " + lock_error;
  }
  return false;
}

// 单帧的分发点。返回值是连接级语义：true = 这一帧处理完了，帧循环继续；
// false = 帧流或资源已经不可信，ServeConnection 必须断开这条连接。
//
// 进 switch 之前的三道帧级校验对每个 opcode 都成立：opcode 必须已知、请求帧的
// status 字段必须为 0（它是响应字段）、PING 的 payload 必须为空。业务状态检查
// 由各个处理函数自己做（例如"没登录就不能 UPLOAD_BEGIN"），所以这里的 switch
// 只是一张路由表。
bool RemoteServer::HandleFrame(int fd, const FrameHeader& header,
                               const std::string& payload,
                               ConnectionContext* context,
                               std::string* error_message) {
  if (context == nullptr) {
    if (error_message != nullptr) {
      *error_message = "connection context is null";
    }
    return false;
  }
  if (!IsKnownOpcode(header.opcode)) {
    Log("rejecting unknown opcode " + std::to_string(header.opcode));
    return SendError(fd, static_cast<std::uint16_t>(Opcode::kError),
                     header.request_id, Status::kInvalidRequest, error_message);
  }
  // 请求帧里的 status 必须为 0：它是响应字段。
  if (header.status != 0) {
    Log("rejecting a request frame with a non-zero status field");
    return SendError(fd, header.opcode, header.request_id,
                     Status::kInvalidRequest, error_message);
  }
  if (header.opcode == static_cast<std::uint16_t>(Opcode::kPing)) {
    if (!payload.empty()) {
      return SendError(fd, header.opcode, header.request_id,
                       Status::kInvalidRequest, error_message);
    }
    return HandlePing(fd, header, error_message);
  }

  switch (static_cast<Opcode>(header.opcode)) {
    case Opcode::kRegister:
      return HandleRegister(fd, header, payload, context, error_message);
    case Opcode::kLogin:
      return HandleLogin(fd, header, payload, context, error_message);
    case Opcode::kLogout:
      return HandleLogout(fd, header, context, error_message);
    case Opcode::kResume:
      return HandleResume(fd, header, payload, context, error_message);
    case Opcode::kList:
      return HandleList(fd, header, context, error_message);
    case Opcode::kUploadBegin:
      return HandleUploadBegin(fd, header, payload, context, error_message);
    case Opcode::kUploadChunk:
      return HandleUploadChunk(fd, header, payload, context, error_message);
    case Opcode::kUploadEnd:
      return HandleUploadEnd(fd, header, payload, context, error_message);
    case Opcode::kDownloadBegin:
      return HandleDownloadBegin(fd, header, payload, context, error_message);
    case Opcode::kDownloadChunk:
      return HandleDownloadChunk(fd, header, payload, context, error_message);
    case Opcode::kDownloadEnd:
      return HandleDownloadEnd(fd, header, payload, context, error_message);
    case Opcode::kDelete:
      return HandleDelete(fd, header, payload, context, error_message);
    case Opcode::kDeleteAccount:
      return HandleDeleteAccount(fd, header, payload, context, error_message);
    default:
      break;
  }
  // 走到这里说明这个操作码连"已知"都不是（HandleFrame 开头已经挡掉了
  // 未知操作码），保留一条防御性的答复。
  Log(std::string("opcode ") + OpcodeName(header.opcode) +
      " is not implemented (state=" + ConnectionStateName(context->state) +
      ")");
  return SendError(fd, header.opcode, header.request_id, Status::kUnsupported,
                   error_message);
}

// 这是"一个 worker 线程的全部工作"：完成 BPSEC1/BPSEC2 握手，然后一直处理帧
// 直到对端关闭或出错。它不关 fd（worker 负责），但保证在返回之前把会话状态
// 收干净——finish 包住了每一条返回路径。
//
// 读结果的处置分三类：kClosed 是正常结束（返回 true）；kCorruptStream 与
// kIoError 说明帧流或连接已经不可信（返回 false，断开）；kInvalidFrame 则是
// "magic 与长度自洽、内容不合法"，流位置仍然完好，所以回一个错误帧继续服务。
// 客户端收到 kUnsupportedVersion / kMalformedFrame 之后能自己决定是否降级，
// 而不必整条连接重来。
//
// 握手与业务帧走的是同一个加密通道：g_connection_channel 在这条连接上一直是
// 它，没有任何一处会退回明文。
bool RemoteServer::ServeConnection(int fd, std::string* error_message) {
  // 慢连接保护：读写在 io_timeout_seconds 之后超时返回，因此一个挂着不动的
  // 客户端最多占用一个 worker 这么久，不会永久占用。
  //
  // 审查轮更正：SO_RCVTIMEO 是**每次 recv** 的超时，对"每个超时周期挤 1 个
  // 字节"的对端无效——那样一条未认证的连接可以把 worker 占住约 112 个超时
  // 周期（默认 io_timeout 30 秒 → 接近一小时）。握手因此额外有一个整体预算，
  // 见下面的 SetHandshakeTimeoutMs。
  timeval timeout;
  timeout.tv_sec = config_.io_timeout_seconds;
  timeout.tv_usec = 0;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

  ConnectionContext context;
  // 这条连接的加密通道。必须比 finish 活得久，所以在这里声明。
  SecureChannel channel;

  // ServeConnection 的每一条返回路径都要收尾：客户端半路断开时，
  // 未完成的上传必须删掉临时文件，下载必须关掉句柄；同时把 thread-local
  // 的通道指针复位，免得被下一条连接误用。
  const auto finish = [this, &context, &channel](bool result) {
    if (g_connection_channel == &channel) {
      g_connection_channel = nullptr;
    }
    CleanupConnection(&context);
    return result;
  };

  // 任何业务帧之前先完成 BPSEC1 握手。
  //
  // 失败就关连接：口令、token、用户名、快照元数据一个字节都不会以明文出现在
  // 网络上，也没有"握手失败就退回明文 BPNET1"的分支。
  // 握手一共只有 112 个字节的往返，正常网络下几秒内一定完成；给两倍 IO 超时
  // （下限 30 秒）作为整体预算，超了就按 IO 错误关连接。
  std::uint64_t handshake_budget_seconds =
      static_cast<std::uint64_t>(config_.io_timeout_seconds) * 2;
  if (handshake_budget_seconds < 30) {
    handshake_budget_seconds = 30;
  }
  channel.SetHandshakeTimeoutMs(handshake_budget_seconds * 1000);
  // BPSEC2：配置成只接受签名身份时，服务端连 BPSEC1 的 ClientHello 都不接。
  const bool handshake_ok =
      config_.require_bpsec2
          ? channel.HandshakeServerRequireCertificate(fd, transport_identity_,
                                                      error_message)
          : channel.HandshakeServer(fd, transport_identity_, error_message);
  if (!handshake_ok) {
    Log(std::string("BPSEC1/BPSEC2 handshake failed: ") +
        (error_message != nullptr && !error_message->empty()
             ? *error_message
             : std::string("unknown reason")));
    return finish(false);
  }
  g_connection_channel = &channel;

  for (;;) {
    FrameHeader header;
    std::string payload;
    std::string read_error;
    const FrameReadStatus status =
        channel.ReceiveFrame(fd, &header, &payload, &read_error);
    if (status == FrameReadStatus::kClosed) {
      Log("client closed the connection");
      return finish(true);
    }
    if (status == FrameReadStatus::kCorruptStream) {
      Log("framing is corrupt, dropping the connection: " + read_error);
      return finish(false);
    }
    if (status == FrameReadStatus::kIoError) {
      Log("connection I/O error: " + read_error);
      return finish(false);
    }
    if (status == FrameReadStatus::kInvalidFrame) {
      // 流位置完好（magic 与长度都自洽），所以可以回一个错误帧继续服务。
      Status reply = Status::kMalformedFrame;
      if (header.version != kProtocolVersion) {
        reply = Status::kUnsupportedVersion;
      } else if (!IsKnownOpcode(header.opcode)) {
        reply = Status::kInvalidRequest;
      }
      Log(std::string("rejecting an invalid frame: ") + read_error);
      if (!SendError(fd, static_cast<std::uint16_t>(Opcode::kError),
                     header.request_id, reply, error_message)) {
        return finish(false);
      }
      continue;
    }
    // 按操作码计数：**收到就算**（哪怕接下来就断开、根本没有处理）。
    // 测试用它证明客户端不会在失败之后偷偷重发一次请求。
    if (header.opcode < 256) {
      request_counts_[header.opcode].fetch_add(1);
    }
    // 测试专用：模拟"这一帧读进来了，但服务端没回应就断了"（进程被 kill、
    // 隧道重启都属于这一类）。客户端必须如实报告失败，并且**不重发**。
    // 产品代码从不设置这个标志。
    if (fail_next_response_.exchange(false)) {
      Log("injected: dropping the connection before answering a frame (test "
          "seam)");
      return finish(false);
    }
    if (!HandleFrame(fd, header, payload, &context, error_message)) {
      if (error_message != nullptr) {
        Log("connection terminated: " + *error_message);
      }
      return finish(false);
    }
  }
}

// 主线程的循环：accept + 把新连接排进 pending_，由固定数量的 worker 消费。
// 并发上限就是 worker_count，不存在"每来一个连接新建一个线程"的路径。
//
// 背压：busy_workers_ 已经等于 worker_count 时，accept 循环在 slot_free_ 上
// 等待，新连接留在 listen backlog 里（超出的由内核拒绝），而不是无限建线程
// 或者把连接堆在内存里。
//
// 停止序列（顺序有意义）：置 stop_requested_ -> shutdown 掉 pending_ 里还没被
// 取走的连接（让阻塞在 recv 的 worker 也能退出）-> notify_all 唤醒所有 worker
// -> join -> 清 workers_ -> 清 run_in_progress_。之后 Stop() 才能安全拆除。
bool RemoteServer::Run(std::string* error_message) {
  if (!running()) {
    if (error_message != nullptr) {
      *error_message = "Run() called before a successful Start()";
    }
    return false;
  }
  // 标记“Run() 正在执行”：Stop() 在这段时间里不做任何拆除（见头文件的
  // 生命周期合同）。下面的收尾路径与异常路径都要清掉它。
  run_in_progress_.store(true);
  worker_count_ = config_.worker_count;
  for (std::size_t index = 0; index < worker_count_; ++index) {
    workers_.push_back(std::thread(&RemoteServer::WorkerLoop, this));
  }

  bool ok = true;
  while (!stop_requested_.load()) {
    // 用 poll 而不是阻塞 accept：这样 Stop() 最迟 kPollIntervalMs 之后生效，
    // 并且监听 fd 被关掉时不会永久卡在 accept 上。
    pollfd entry;
    entry.fd = listener_fd_;
    entry.events = POLLIN;
    entry.revents = 0;
    const int ready = ::poll(&entry, 1, kPollIntervalMs);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (error_message != nullptr) {
        *error_message = std::string("poll() failed: ") + std::strerror(errno);
      }
      ok = false;
      break;
    }
    if (ready == 0 || (entry.revents & POLLIN) == 0) {
      continue;
    }
    const int client = ::accept(listener_fd_, nullptr, nullptr);
    if (client < 0) {
      if (errno == EINTR || errno == ECONNABORTED) {
        continue;
      }
      if (errno == EMFILE || errno == ENFILE) {
        // 描述符耗尽时不要忙等。
        Log("accept() ran out of file descriptors; backing off");
        ::poll(nullptr, 0, kPollIntervalMs);
        continue;
      }
      if (error_message != nullptr) {
        *error_message =
            std::string("accept() failed: ") + std::strerror(errno);
      }
      ok = false;
      break;
    }
    // 交给空闲 worker。worker 全忙时在这里等待——新连接留在 listen backlog
    // 里，而不是新建线程。
    std::unique_lock<std::mutex> lock(work_mutex_);
    while (busy_workers_ >= worker_count_ && !stop_requested_.load()) {
      slot_free_.wait_for(lock, std::chrono::milliseconds(kPollIntervalMs));
    }
    if (stop_requested_.load()) {
      lock.unlock();
      ::close(client);
      break;
    }
    busy_workers_ += 1;
    pending_.push_back(client);
    lock.unlock();
    work_ready_.notify_one();
  }

  {
    std::lock_guard<std::mutex> guard(work_mutex_);
    stop_requested_.store(true);
    for (const int fd : pending_) {
      ::shutdown(fd, SHUT_RDWR);
    }
  }
  work_ready_.notify_all();
  for (std::thread& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  workers_.clear();
  // 到这里 worker 已经全部停下来，Stop() 可以安全地做最终清理了。
  run_in_progress_.store(false);
  if (!ok && error_message != nullptr) {
    Log("run loop stopped: " + *error_message);
  }
  return ok;
}

// 取任务用的是带超时的 wait_for 而不是 wait：即使某次 notify 丢了，worker 也会
// 在 kPollIntervalMs 之后自己醒一次，从而保证停止请求最多延迟这么久生效。
// pending_ 为空且没有停止请求时继续等，有停止请求就退出。
//
// busy_workers_ 与 slot_free_ 是一对：每个连接在开始服务之前 +1，服务结束之后
// -1 并 notify 一次，Run() 正是靠这个计数实现背压。ServeConnection 的返回值只
// 表示"这条连接是怎么结束的"，不影响 worker 继续取下一个任务。
void RemoteServer::WorkerLoop() {
  for (;;) {
    int client = -1;
    {
      std::unique_lock<std::mutex> lock(work_mutex_);
      work_ready_.wait_for(
          lock, std::chrono::milliseconds(kPollIntervalMs),
          [this] { return !pending_.empty() || stop_requested_.load(); });
      if (pending_.empty()) {
        if (stop_requested_.load()) {
          return;
        }
        continue;
      }
      client = pending_.front();
      pending_.pop_front();
    }
    std::string error;
    ServeConnection(client, &error);
    ::close(client);
    {
      std::lock_guard<std::mutex> guard(work_mutex_);
      if (busy_workers_ > 0) {
        busy_workers_ -= 1;
      }
    }
    slot_free_.notify_one();
  }
}

// 日志格式：[backup-server <墙钟秒>] message。stderr 用 fputs + fflush 立即刷出
// （崩溃时最后几行还在），日志文件每次追加都重新 open：不长驻句柄，也就不会
// 因为句柄泄漏而静默丢掉后续日志。整条日志在 log_mutex_ 内组装并写出，因此
// 多线程并发写不会互相穿插。纪律：调用方只传已经脱敏的内容，secret / token /
// 口令一律不进来。
void RemoteServer::Log(const std::string& message) {
  std::ostringstream line;
  line << "[backup-server " << NowSeconds() << "] " << message << "\n";
  const std::string text = line.str();
  std::lock_guard<std::mutex> guard(log_mutex_);
  if (!config_.quiet) {
    std::fputs(text.c_str(), stderr);
    std::fflush(stderr);
  }
  if (!config_.log_file_path.empty()) {
    std::ofstream output(config_.log_file_path.c_str(),
                         std::ios::out | std::ios::app);
    if (output) {
      output << text;
    }
  }
}

}  // namespace net
}  // namespace backupproject
