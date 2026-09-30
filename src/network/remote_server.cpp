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
#include "remote_auth.h"
#include "remote_metadata_store.h"

namespace backupproject {
namespace net {
namespace {

// listen backlog：worker 全忙时排在这里，超出的连接由内核拒绝。
constexpr int kListenBacklog = 64;
// 最大 worker 数。超过这个数就不再是"foundation 的有界模型"了。
constexpr std::size_t kMaxWorkers = 64;
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

// 读 secrets.env 里的 BACKUP_TOKEN_SECRET。
//
// 硬规则：函数只把值交给调用方，**任何**日志、错误信息、返回值里都不出现
// 值本身，最多出现长度。文件缺失、权限不对、没有这个键、值太短，都明确失败。
bool ReadSecretFile(const std::string& path, std::string* secret,
                    std::string* error_message) {
  if (path.empty()) {
    if (error_message != nullptr) {
      *error_message = "--secret-file is required";
    }
    return false;
  }
  std::ifstream input(path.c_str());
  if (!input) {
    if (error_message != nullptr) {
      *error_message = "cannot open the secret file " + path;
    }
    return false;
  }
  std::string line;
  std::string value;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const std::string key = "BACKUP_TOKEN_SECRET=";
    if (line.compare(0, key.size(), key) == 0) {
      value = line.substr(key.size());
      break;
    }
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

RemoteServer::RemoteServer() = default;

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
  if (!EnsureDirectory(config_.root_directory, error_message)) {
    return false;
  }
  if (!EnsureDirectory(config_.root_directory + "/users", error_message)) {
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
    return false;
  }
  store_.reset(new RemoteMetadataStore());
  if (!store_->Open(config_.database_path, error_message)) {
    store_.reset();
    return false;
  }
  Log("metadata database opened at " + config_.database_path);
  if (!OpenListener(error_message)) {
    store_->Close();
    store_.reset();
    return false;
  }
  {
    std::ostringstream line;
    line << "listening on " << config_.bind_address << ":" << bound_port_
         << " root=" << config_.root_directory
         << " workers=" << config_.worker_count
         << " max_upload=" << config_.max_upload_bytes;
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
}

void RemoteServer::RequestStop() { stop_requested_.store(true); }

bool RemoteServer::SendError(int fd, std::uint16_t opcode,
                             std::uint64_t request_id, Status status,
                             std::string* error_message) {
  // 错误响应的 payload 永远是空的：原因只写服务端日志，不回给客户端，
  // 免得把内部路径或 errno 漏出去。
  if (!SendFrame(fd, opcode, static_cast<std::uint32_t>(status), request_id,
                 std::string(), error_message)) {
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
  return SendFrame(fd, opcode, static_cast<std::uint32_t>(status),
                   request.request_id, payload, error_message);
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

bool RemoteServer::HandleList(int fd, const FrameHeader& header,
                              ConnectionContext* context,
                              std::string* error_message) {
  if (context->state != ConnectionState::kAuthenticated) {
    Log("rejecting LIST without a session");
    return SendError(fd, header.opcode, header.request_id,
                     Status::kUnauthorized, error_message);
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
    // 一帧装不下就明确拒绝，绝不发一个超限的帧。
    if (builder.size() > kMaxPayloadBytes) {
      Log("the LIST response would exceed the 1 MiB frame limit");
      return SendError(fd, header.opcode, header.request_id, Status::kTooLarge,
                       error_message);
    }
  }
  return SendStatus(fd, header, Status::kOk, builder.data(), error_message);
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
    case Opcode::kList:
      return HandleList(fd, header, context, error_message);
    default:
      break;
  }
  // 传输类操作码在 PR20 的第三个 commit 里接入。在那之前这里**如实**回答
  // "当前构建还不支持"，而不是假装成功。
  Log(std::string("opcode ") + OpcodeName(header.opcode) +
      " is not supported by this build (state=" +
      ConnectionStateName(context->state) + ")");
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
  for (;;) {
    FrameHeader header;
    std::string payload;
    std::string read_error;
    const FrameReadStatus status =
        ReceiveFrame(fd, &header, &payload, &read_error);
    if (status == FrameReadStatus::kClosed) {
      Log("client closed the connection");
      return true;
    }
    if (status == FrameReadStatus::kCorruptStream) {
      Log("framing is corrupt, dropping the connection: " + read_error);
      return false;
    }
    if (status == FrameReadStatus::kIoError) {
      Log("connection I/O error: " + read_error);
      return false;
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
        return false;
      }
      continue;
    }
    if (!HandleFrame(fd, header, payload, &context, error_message)) {
      Log("connection terminated: " + *error_message);
      return false;
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
  stop_requested_.store(false);
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
