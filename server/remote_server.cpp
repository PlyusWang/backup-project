// src/network/remote_server.cpp
//
// backup-server 的监听、帧循环与连接状态机。见 include/remote_server.h。
//
// 这一层刻意不碰归档格式：它只管"把一段有名字、有长度、有 SHA-256 的字节
// 安全地存下来"。归档语义属于 BackupEngine，不属于这里。

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

std::uint64_t NowSeconds() {
  return static_cast<std::uint64_t>(::time(nullptr));
}

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
// "读一个巨大的文件"留门（非普通文件在下面已经被拒，但普通文件也可能是 10 GiB）。
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
        *error_message = "cannot open the secret file " + path + ": " +
                         std::strerror(errno);
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

RemoteServer::~RemoteServer() { Stop(); }

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
  // PR #21 起，BPNET1 的每一个字节都由 BPSEC1 加密（见 secure_transport.h），
  // 机密性不再依赖 SSH 隧道；隧道降级为部署层的纵深防御。监听地址仍然只能是
  // 环回地址——这是纵深防御的一部分，不是机密性的前提：把只该由隧道访问的
  // 端口直接暴露在共享网络上没有任何好处，所以这里继续 fail closed，
  // 不提供 --insecure / --allow-public 之类的开关。
  if (config.bind_address != "127.0.0.1") {
    if (error_message != nullptr) {
      *error_message =
          "--bind must be 127.0.0.1, not " + config.bind_address +
          ": this version has no native TLS, so the server only accepts"
          " loopback connections; reach a remote instance through an SSH"
          " tunnel (ssh -N -L 18765:127.0.0.1:18765 <host>)";
    }
    return false;
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
  {
    std::ostringstream line;
    line << "listening on " << config_.bind_address << ":" << bound_port_
         << " root=" << config_.root_directory
         << " workers=" << config_.worker_count
         << " max_upload=" << config_.max_upload_bytes
         << " bspec1=on"
         << " transport_fingerprint="
         << crypto::X25519Fingerprint(transport_identity_.public_key);
    Log(line.str());
  }
  return true;
}

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

bool RemoteServer::LoadTransportIdentityKey(std::string* error_message) {
  if (!LoadTransportIdentity(config_.transport_key_file_path,
                             &transport_identity_, error_message)) {
    return false;
  }
  // 只记指纹（公开信息），不记私钥。
  Log("BPSEC1 transport identity loaded, fingerprint=" +
      crypto::X25519Fingerprint(transport_identity_.public_key));
  return true;
}

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
  RequestStop();
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

void RemoteServer::RequestStop() { stop_requested_.store(true); }

void RemoteServer::FailNextAccountDeleteForTesting() {
  if (store_ != nullptr) {
    store_->FailNextDeleteUserForTesting();
  }
}

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
  if (!g_connection_channel->SendFrame(fd, opcode,
                                       static_cast<std::uint32_t>(status),
                                       request_id, std::string(),
                                       error_message)) {
    return false;
  }
  return true;
}

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
  return g_connection_channel->SendFrame(fd, opcode,
                                         static_cast<std::uint32_t>(status),
                                         request.request_id, payload,
                                         error_message);
}

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
    return SendError(fd, header.opcode, header.request_id, Status::kUnauthorized,
                     error_message);
  }
  // token 的签名说明"这是我们签发的"，但**不**说明"这个账户还在"：注销过的
  // 账户必须在这里被挡住。这一条正是"注销之后旧 token 立刻失效"的实现。
  RemoteUserRecord user;
  std::string store_error;
  const StoreResult found = store_->FindUserById(
      static_cast<std::int64_t>(parsed.user_id), &user, &store_error);
  if (found == StoreResult::kNotFound) {
    Log("resume rejected: the account no longer exists");
    return SendError(fd, header.opcode, header.request_id, Status::kUnauthorized,
                     error_message);
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
    // PR #21：链元数据。客户端据此自己走父链，服务端不替它解释链。
    builder.AppendU16(record.snapshot_kind);
    builder.AppendU64(record.generation);
    if (!builder.AppendString(record.parent_id, kMaxSnapshotIdBytes,
                              &build_error) ||
        !builder.AppendString(record.lineage, kMaxLineageBytes,
                              &build_error)) {
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
void FsyncDirectory(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd < 0) {
    return;
  }
  ::fsync(fd);
  ::close(fd);
}

}  // namespace

std::string RemoteServer::UserDirectory(std::int64_t user_id) const {
  // 磁盘路径永远只由服务端生成：数字 user id + 服务端生成的 snapshot id。
  // 客户端给的用户名与显示名一次都不参与拼接。
  return config_.root_directory + "/users/" + std::to_string(user_id);
}

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

void RemoteServer::CleanupConnection(ConnectionContext* context) {
  // 两个都要做：客户端半路断开时上传要删临时文件、下载要关句柄。
  if (context->state == ConnectionState::kUploadInProgress ||
      context->upload_fd >= 0) {
    AbortUpload(context);
  }
  CloseDownload(context);
}

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
    // 旧客户端（PR #20）的 UPLOAD_BEGIN 少了后面三个字段：这里会因为
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
  // lineage：完整快照**允许为空**——空串表示这一份不属于任何链（PR #20 时代
  // 上传的旧数据与低层 remote upload 都是这一类）。非空时必须是 64 位十六进
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
  // ---- PR #21：链关系校验。全部由服务端做，客户端说什么都要在这里过一遍 ----
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
    const StoreResult parent_result = store_->FindSnapshot(
        static_cast<std::int64_t>(context->user_id), parent_snapshot_id,
        &parent, &parent_error);
    if (parent_result == StoreResult::kNotFound) {
      // 不存在的父与"别人的父"是同一个答案：不允许按 id 探测别人的快照。
      Log("rejecting an incremental upload: the parent snapshot does not exist");
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
      *error_message =
          "another process is already using the data directory " +
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

bool RemoteServer::ServeConnection(int fd, std::string* error_message) {
  // 慢连接保护：读写在 io_timeout_seconds 之后超时返回，因此一个挂着不动的
  // 客户端最多占用一个 worker 这么久，不会永久占用。
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

  // PR #21：任何业务帧之前先完成 BPSEC1 握手。
  //
  // 失败就关连接：口令、token、用户名、快照元数据一个字节都不会以明文出现在
  // 网络上，也没有"握手失败就退回明文 BPNET1"的分支。
  if (!channel.HandshakeServer(fd, transport_identity_, error_message)) {
    Log(std::string("BPSEC1 handshake failed: ") +
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
      Log("injected: dropping the connection before answering a frame (test seam)");
      return finish(false);
    }
    if (!HandleFrame(fd, header, payload, &context, error_message)) {
      Log("connection terminated: " + *error_message);
      return finish(false);
    }
  }
}

bool RemoteServer::Run(std::string* error_message) {
  if (!running()) {
    if (error_message != nullptr) {
      *error_message = "Run() called before a successful Start()";
    }
    return false;
  }
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
  if (!ok && error_message != nullptr) {
    Log("run loop stopped: " + *error_message);
  }
  return ok;
}

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
