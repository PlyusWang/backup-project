// src/network/remote_backup_client.cpp

#include "remote_backup_client.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

#include "crypto.h"
#include "file_io.h"

namespace backupproject {
namespace net {
namespace {

std::string StrerrorText() { return std::string(std::strerror(errno)); }

// 路径里最后一个 '/' 之前的部分。
// "a" -> "."、"/a" -> "/"。
// 下载的临时文件必须落在目标所在目录里
// （只有同一个文件系统上才能原子发布）。
std::string ParentDirectoryOf(const std::string& path) {
  const std::size_t slash = path.rfind('/');
  if (slash == std::string::npos) {
    return std::string(".");
  }
  if (slash == 0) {
    return std::string("/");
  }
  return path.substr(0, slash);
}

std::string BaseNameOf(const std::string& path) {
  const std::size_t slash = path.rfind('/');
  if (slash == std::string::npos) {
    return path;
  }
  return path.substr(slash + 1);
}

bool StatRegularFile(const std::string& path, std::uint64_t* size,
                     std::string* error_message) {
  struct stat info;
  if (::stat(path.c_str(), &info) != 0) {
    if (error_message != nullptr) {
      *error_message = "cannot stat " + path + ": " + StrerrorText();
    }
    return false;
  }
  if (!S_ISREG(info.st_mode)) {
    if (error_message != nullptr) {
      *error_message = path + " is not a regular file";
    }
    return false;
  }
  if (size != nullptr) {
    *size = static_cast<std::uint64_t>(info.st_size);
  }
  return true;
}

// 第一遍：流式算 SHA-256（只占一个块的内存）。
bool HashFile(const std::string& path, std::string* sha256_hex,
              std::string* error_message) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    if (error_message != nullptr) {
      *error_message = "cannot open " + path + ": " + StrerrorText();
    }
    return false;
  }
  crypto::Sha256 hasher;
  std::vector<char> buffer(kTransferChunkBytes);
  for (;;) {
    const ssize_t got = ::read(fd, buffer.data(), buffer.size());
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (error_message != nullptr) {
        *error_message = "cannot read " + path + ": " + StrerrorText();
      }
      ::close(fd);
      return false;
    }
    if (got == 0) {
      break;
    }
    hasher.Update(buffer.data(), static_cast<std::size_t>(got));
  }
  ::close(fd);
  unsigned char digest[crypto::kSha256DigestSize];
  hasher.Final(digest);
  *sha256_hex = crypto::ToHex(digest, crypto::kSha256DigestSize);
  return true;
}

// 这条连接的对端是不是已经关了？只做零等待的探测（poll + MSG_PEEK）。
//
// 为什么必须有这一步：服务端会在 io_timeout 之后主动关掉空闲连接，而客户端
// 手里的 fd 依然"有效"——往里写不会立刻报错，响应却永远不会来。人工验收看到
// 的"奇数次失败、偶数次有响应"就是它：失败那一次之后客户端把 token 一起丢了，
// 下一次只能重新登录，于是又"好"了一次。
bool SocketLooksClosed(int fd) {
  pollfd entry;
  entry.fd = fd;
  entry.events = POLLIN;
  entry.revents = 0;
  const int ready = ::poll(&entry, 1, 0);
  if (ready <= 0) {
    // 0 = 没有可读事件（正常情况）；< 0 = poll 自己出错，留给后面的收发去报。
    return false;
  }
  if ((entry.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
    return true;
  }
  if ((entry.revents & POLLIN) == 0) {
    return false;
  }
  char byte = 0;
  const ssize_t got = ::recv(fd, &byte, 1, MSG_PEEK);
  if (got == 0) {
    return true;  // 对端干净关闭
  }
  if (got < 0) {
    return errno == ECONNRESET || errno == ENOTCONN || errno == EBADF;
  }
  return false;
}

}  // namespace

std::string RemoteStatusMessage(std::uint32_t status) {
  switch (static_cast<Status>(status)) {
    case Status::kOk:
      return "成功";
    case Status::kInvalidRequest:
      return "请求不合法（字段格式或长度不对）";
    case Status::kUnauthorized:
      return "未登录或凭据不正确";
    case Status::kForbidden:
      return "没有权限";
    case Status::kNotFound:
      return "服务端找不到这个快照";
    case Status::kAlreadyExists:
      return "名字已经被占用";
    case Status::kInvalidState:
      return "当前连接状态不允许这个操作";
    case Status::kTooLarge:
      return "超过服务端允许的大小";
    case Status::kIntegrityMismatch:
      return "内容校验失败（长度或 SHA-256 与声明不符）";
    case Status::kInternalError:
      return "服务端内部错误（细节见服务端日志）";
    case Status::kUnsupportedVersion:
      return "服务端不接受这个协议版本";
    case Status::kMalformedFrame:
      return "服务端认为这个帧不合法";
    case Status::kUnsupported:
      return "这个操作在当前构建里还不被支持";
  }
  return "未知错误";
}

RemoteArchiveClient::RemoteArchiveClient() = default;

RemoteArchiveClient::~RemoteArchiveClient() { Disconnect(); }

void RemoteArchiveClient::Fail(const std::string& reason) {
  last_error_ = reason;
}

void RemoteArchiveClient::DisconnectSocket() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  // 连接没了，这条连接上的会话自然也没了；但 token 还在手里，可以在新连接
  // 上恢复（Authenticate/PrepareConnection 会做这件事）。
  authenticated_ = false;
}

void RemoteArchiveClient::Disconnect() {
  DisconnectSocket();
  // token 只活在内存里：明确放弃会话时就丢掉，绝不写文件。
  if (!token_.empty()) {
    token_.clear();
  }
}

bool RemoteArchiveClient::Connect(const RemoteEndpoint& endpoint,
                                  std::string* error_message) {
  // 只换连接，不动 token：重连之后可能还要用它恢复会话。
  // 想彻底放弃会话的调用方应该显式调用 Disconnect()。
  DisconnectSocket();
  if (endpoint.host.empty() || endpoint.port == 0) {
    if (error_message != nullptr) {
      *error_message = "endpoint host and port must be set";
    }
    return false;
  }
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    if (error_message != nullptr) {
      *error_message = std::string("socket() failed: ") + StrerrorText();
    }
    return false;
  }
  timeval timeout;
  timeout.tv_sec = endpoint.timeout_seconds;
  timeout.tv_usec = 0;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

  sockaddr_in address;
  std::memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(endpoint.port);
  if (::inet_pton(AF_INET, endpoint.host.c_str(), &address.sin_addr) != 1) {
    ::close(fd);
    if (error_message != nullptr) {
      *error_message = "endpoint host must be a dotted-quad IPv4 address";
    }
    return false;
  }
  if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) !=
      0) {
    const std::string reason = StrerrorText();
    ::close(fd);
    if (error_message != nullptr) {
      *error_message = "cannot connect to " + endpoint.host + ":" +
                       std::to_string(endpoint.port) + ": " + reason;
    }
    return false;
  }
  fd_ = fd;
  endpoint_ = endpoint;
  next_request_id_ = 1;
  authenticated_ = false;
  return true;
}

bool RemoteArchiveClient::PrepareConnection(std::string* error_message) {
  if (fd_ >= 0 && !SocketLooksClosed(fd_)) {
    return true;
  }
  // 走到这里说明连接不可用（或者本来就没连）。重连，并在手里还有 token 时先用
  // token 恢复会话。**这一刻还没有发送任何请求字节**，所以这不是"失败之后偷偷
  // 重发一次"：重发在本项目里是被明确禁止的。
  const RemoteEndpoint endpoint = endpoint_;
  if (endpoint.host.empty() || endpoint.port == 0) {
    if (error_message != nullptr) {
      *error_message = "not connected";
    }
    return false;
  }
  DisconnectSocket();
  std::string connect_error;
  if (!Connect(endpoint, &connect_error)) {
    if (error_message != nullptr) {
      *error_message = connect_error;
    }
    return false;
  }
  if (token_.empty()) {
    return true;
  }
  std::string resume_error;
  if (!ResumeSession(&resume_error)) {
    if (error_message != nullptr) {
      *error_message = resume_error;
    }
    return false;
  }
  return true;
}

bool RemoteArchiveClient::ResumeSession(std::string* error_message) {
  if (token_.empty()) {
    if (error_message != nullptr) {
      *error_message = "there is no saved session to resume";
    }
    return false;
  }
  PayloadBuilder builder;
  std::string build_error;
  if (!builder.AppendString(token_, kMaxTokenBytes, &build_error)) {
    if (error_message != nullptr) {
      *error_message = build_error;
    }
    return false;
  }
  FrameHeader header;
  std::string response;
  std::string resume_error;
  if (Request(Opcode::kResume, builder.data(), &header, &response,
              &resume_error)) {
    authenticated_ = true;
    return true;
  }
  if (last_status_ == static_cast<std::uint32_t>(Status::kUnauthorized)) {
    // 服务端明确拒绝：token 过期、被轮换，或者账户已经注销。这才是真的失效，
    // 只有这一种情况允许丢掉 token。
    Disconnect();
    if (error_message != nullptr) {
      // 用**共享的状态文案**，而不是再编一句英文：控制器靠它反查状态码，
      // 从而把这种情况归类成"会话失效"（清会话、提示重新登录），
      // 而不是"网络抖动"（保留会话）。
      *error_message = RemoteStatusMessage(
          static_cast<std::uint32_t>(Status::kUnauthorized));
    }
    return false;
  }
  if (error_message != nullptr) {
    *error_message = resume_error;
  }
  return false;
}

bool RemoteArchiveClient::Request(Opcode opcode, const std::string& payload,
                                  FrameHeader* header, std::string* response,
                                  std::string* error_message) {
  last_status_ = 0;
  std::string prepare_error;
  if (!PrepareConnection(&prepare_error)) {
    Fail(prepare_error);
    if (error_message != nullptr) {
      *error_message = prepare_error;
    }
    return false;
  }
  const std::uint64_t request_id = next_request_id_++;
  std::string io_error;
  if (!SendFrame(fd_, static_cast<std::uint16_t>(opcode), 0, request_id,
                 payload, &io_error)) {
    Fail(io_error);
    if (error_message != nullptr) {
      *error_message = "cannot send the request: " + io_error;
    }
    // 写失败了：这条连接不可信。**不重发**——这个请求有可能已经被对端收到了。
    DisconnectSocket();
    return false;
  }
  const FrameReadStatus status = ReceiveFrame(fd_, header, response, &io_error);
  if (status != FrameReadStatus::kOk) {
    Fail(io_error);
    if (error_message != nullptr) {
      *error_message = "cannot read the response: " + io_error;
    }
    // 帧流已经不可信：关掉这条连接，但**保留 token**。网络抖动不等于退出登录，
    // 下一次操作会用 token 在新连接上恢复会话。同样**不重发**本次请求。
    DisconnectSocket();
    return false;
  }
  if (header->request_id != request_id) {
    if (error_message != nullptr) {
      *error_message = "the response does not match the request";
    }
    Disconnect();
    return false;
  }
  if (header->status != static_cast<std::uint32_t>(Status::kOk)) {
    last_status_ = header->status;
    Fail(std::string(OpcodeName(header->opcode)) + " -> " +
         StatusName(header->status));
    if (error_message != nullptr) {
      *error_message = RemoteStatusMessage(header->status);
    }
    return false;
  }
  return true;
}

bool RemoteArchiveClient::RequireAuthenticated(const std::string& what,
                                               std::string* error_message) {
  // 这条 TCP 连接上的会话可能已经随着连接一起没了，但 token 还在：只要手里有
  // token 就允许继续，Request 会先重连并恢复会话。
  if (authenticated_ || !token_.empty()) {
    return true;
  }
  if (error_message != nullptr) {
    *error_message =
        "this command needs a session; " + what + " was called before login";
  }
  return false;
}

bool RemoteArchiveClient::Ping(std::string* software,
                               std::uint16_t* protocol_version,
                               std::uint64_t* server_time,
                               std::string* error_message) {
  FrameHeader header;
  std::string response;
  if (!Request(Opcode::kPing, std::string(), &header, &response,
               error_message)) {
    return false;
  }
  PayloadReader reader(response);
  std::uint16_t version = 0;
  std::uint64_t when = 0;
  std::string name;
  if (!reader.ReadString(kMaxDisplayNameBytes, &name) ||
      !reader.ReadU16(&version) || !reader.ReadU64(&when) || !reader.AtEnd()) {
    if (error_message != nullptr) {
      *error_message = "cannot decode the PING response";
    }
    return false;
  }
  if (software != nullptr) {
    *software = name;
  }
  if (protocol_version != nullptr) {
    *protocol_version = version;
  }
  if (server_time != nullptr) {
    *server_time = when;
  }
  return true;
}

bool RemoteArchiveClient::Register(const std::string& username,
                                   const std::string& password,
                                   std::string* error_message) {
  // 同 Login：注册是"重新开始"，先丢掉旧会话，避免服务端回 INVALID_STATE。
  if (authenticated_ || !token_.empty()) {
    Disconnect();
  }
  PayloadBuilder builder;
  std::string build_error;
  if (!builder.AppendString(username, kMaxUsernameBytes, &build_error) ||
      !builder.AppendString(password, kMaxPasswordBytes, &build_error)) {
    if (error_message != nullptr) {
      *error_message = build_error;
    }
    return false;
  }
  FrameHeader header;
  std::string response;
  return Request(Opcode::kRegister, builder.data(), &header, &response,
                 error_message);
}

bool RemoteArchiveClient::Login(const std::string& username,
                                const std::string& password,
                                std::string* error_message) {
  // 显式登录意味着"重新开始"：先丢掉旧会话（如果有）。否则服务端会按
  // "这条连接上已经有会话了"拒绝（INVALID_STATE），用户看到的会是一句
  // 与登录无关的错误。
  if (authenticated_ || !token_.empty()) {
    Disconnect();
  }
  PayloadBuilder builder;
  std::string build_error;
  if (!builder.AppendString(username, kMaxUsernameBytes, &build_error) ||
      !builder.AppendString(password, kMaxPasswordBytes, &build_error)) {
    if (error_message != nullptr) {
      *error_message = build_error;
    }
    return false;
  }
  FrameHeader header;
  std::string response;
  if (!Request(Opcode::kLogin, builder.data(), &header, &response,
               error_message)) {
    return false;
  }
  PayloadReader reader(response);
  if (!reader.ReadString(kMaxTokenBytes, &token_) || !reader.AtEnd()) {
    if (error_message != nullptr) {
      *error_message = "cannot decode the LOGIN response";
    }
    return false;
  }
  authenticated_ = true;
  return true;
}

bool RemoteArchiveClient::Logout(std::string* error_message) {
  if (!RequireAuthenticated("logout", error_message)) {
    return false;
  }
  FrameHeader header;
  std::string response;
  const bool ok = Request(Opcode::kLogout, std::string(), &header, &response,
                          error_message);
  authenticated_ = false;
  token_.clear();
  return ok;
}

bool RemoteArchiveClient::List(std::vector<RemoteSnapshotInfo>* snapshots,
                               std::string* error_message) {
  if (!RequireAuthenticated("list", error_message)) {
    return false;
  }
  FrameHeader header;
  std::string response;
  if (!Request(Opcode::kList, std::string(), &header, &response,
               error_message)) {
    return false;
  }
  PayloadReader reader(response);
  std::uint32_t count = 0;
  if (!reader.ReadU32(&count)) {
    if (error_message != nullptr) {
      *error_message = "cannot decode the LIST response";
    }
    return false;
  }
  std::vector<RemoteSnapshotInfo> parsed;
  for (std::uint32_t index = 0; index < count; ++index) {
    RemoteSnapshotInfo info;
    if (!reader.ReadString(kMaxSnapshotIdBytes, &info.snapshot_id) ||
        !reader.ReadString(kMaxDisplayNameBytes, &info.display_name) ||
        !reader.ReadString(kSha256HexBytes, &info.sha256) ||
        !reader.ReadU64(&info.size_bytes) ||
        !reader.ReadU64(&info.created_at)) {
      if (error_message != nullptr) {
        *error_message = "cannot decode a LIST entry";
      }
      return false;
    }
    parsed.push_back(info);
  }
  if (!reader.AtEnd()) {
    if (error_message != nullptr) {
      *error_message = "the LIST response has trailing bytes";
    }
    return false;
  }
  if (snapshots != nullptr) {
    *snapshots = parsed;
  }
  return true;
}

bool RemoteArchiveClient::UploadArchiveFile(
    const std::string& local_path, const std::string& display_name,
    const RemoteProgressCallback& progress, RemoteSnapshotInfo* uploaded,
    std::string* error_message) {
  if (!RequireAuthenticated("upload", error_message)) {
    return false;
  }
  std::uint64_t size = 0;
  if (!StatRegularFile(local_path, &size, error_message)) {
    return false;
  }
  if (size == 0) {
    if (error_message != nullptr) {
      *error_message = local_path + " is empty; refusing to upload it";
    }
    return false;
  }
  if (size > kDefaultMaxUploadBytes) {
    if (error_message != nullptr) {
      *error_message = "the file is larger than the foundation upload limit";
    }
    return false;
  }
  std::string sha256;
  // 第一遍：算摘要。UPLOAD_BEGIN 必须在发送任何字节之前给出它。
  if (!HashFile(local_path, &sha256, error_message)) {
    return false;
  }
  const int source = ::open(local_path.c_str(), O_RDONLY);
  if (source < 0) {
    if (error_message != nullptr) {
      *error_message = "cannot open " + local_path + ": " + StrerrorText();
    }
    return false;
  }

  PayloadBuilder begin;
  std::string build_error;
  // 字段顺序必须与服务端的解码顺序一致：显示名 -> 长度 -> SHA-256。
  if (!begin.AppendString(display_name, kMaxDisplayNameBytes, &build_error)) {
    ::close(source);
    if (error_message != nullptr) {
      *error_message = build_error;
    }
    return false;
  }
  begin.AppendU64(size);
  if (!begin.AppendString(sha256, kSha256HexBytes, &build_error)) {
    ::close(source);
    if (error_message != nullptr) {
      *error_message = build_error;
    }
    return false;
  }

  FrameHeader header;
  std::string response;
  if (!Request(Opcode::kUploadBegin, begin.data(), &header, &response,
               error_message)) {
    ::close(source);
    return false;
  }

  // UPLOAD_BEGIN 已经被服务端接受：服务端现在处于 UPLOAD_IN_PROGRESS。
  // 这一版协议没有 UPLOAD_ABORT，所以本地失败一律用**连接**当事务边界：
  // 关掉 socket 之后服务端读到 EOF，会走 CleanupConnection -> AbortUpload，
  // 删掉上传临时文件，连接状态也不会留在"上传中"。
  // 用 DisconnectSocket() 而不是 Disconnect()：token 必须留着。本地文件出的
  // 错就是本地错误，不能顺手把用户"退出登录"；下一次操作会自己重连并用
  // RESUME 恢复会话。这不是"自动重试上传"：这一次上传已经失败，重连只发生在
  // 用户下一次主动操作的时候。
  const auto abort_upload_transaction = [&]() {
    ::close(source);
    DisconnectSocket();
  };

  // 第二遍：分块发送。内存里只有一个块。
  std::vector<char> buffer(kTransferChunkBytes);
  std::uint64_t sent = 0;
  if (progress) {
    progress(RemoteTransferProgress{"upload", 0, size});
  }
  while (sent < size) {
    ssize_t got = 0;
    for (;;) {
      got = ::read(source, buffer.data(), buffer.size());
      if (got < 0 && errno == EINTR) {
        continue;
      }
      break;
    }
    if (got < 0) {
      // 先记下本地错误（StrerrorText 要在 close 之前读 errno），再终止事务：
      // 收尾不能覆盖调用方看到的原始原因。
      if (error_message != nullptr) {
        *error_message = "cannot read " + local_path + ": " + StrerrorText();
      }
      abort_upload_transaction();
      return false;
    }
    if (got == 0) {
      // 文件在算完摘要之后被截短了：同样是本地错误，事务同样要终止。
      if (error_message != nullptr) {
        *error_message = "the file shrank while it was being uploaded";
      }
      abort_upload_transaction();
      return false;
    }
    if (!Request(Opcode::kUploadChunk,
                 std::string(buffer.data(), static_cast<std::size_t>(got)),
                 &header, &response, error_message)) {
      // 发送失败时 Request 已经自己关过连接（token 保留）；这里只是让
      // "UPLOAD_BEGIN 之后失败"这条路径只有一个出口。
      abort_upload_transaction();
      return false;
    }
    sent += static_cast<std::uint64_t>(got);
    if (progress) {
      progress(RemoteTransferProgress{"upload", sent, size});
    }
  }
  ::close(source);

  if (!Request(Opcode::kUploadEnd, std::string(), &header, &response,
               error_message)) {
    return false;
  }
  PayloadReader reader(response);
  RemoteSnapshotInfo info;
  std::uint64_t created_at = 0;
  if (!reader.ReadString(kMaxSnapshotIdBytes, &info.snapshot_id) ||
      !reader.ReadString(kSha256HexBytes, &info.sha256) ||
      !reader.ReadU64(&info.size_bytes) || !reader.ReadU64(&created_at) ||
      !reader.AtEnd()) {
    if (error_message != nullptr) {
      *error_message = "cannot decode the UPLOAD_END response";
    }
    return false;
  }
  info.display_name = display_name;
  info.created_at = created_at;
  if (uploaded != nullptr) {
    *uploaded = info;
  }
  return true;
}

bool RemoteArchiveClient::DownloadArchiveFile(
    const std::string& snapshot_id, const std::string& target_path,
    bool allow_overwrite, const RemoteProgressCallback& progress,
    RemoteSnapshotInfo* downloaded, std::string* error_message) {
  if (!RequireAuthenticated("download", error_message)) {
    return false;
  }
  if (target_path.empty()) {
    if (error_message != nullptr) {
      *error_message = "the download target path is empty";
    }
    return false;
  }
  // 默认不覆盖已经存在的目标。
  // 这一步只是**尽早**失败（连 DOWNLOAD_BEGIN 都不发）。
  // 它不是"不覆盖"的保证：
  // 真正的保证来自最后那一步原子的 PublishNoReplace()。
  // 目标即使在这次下载开始之后才被别的进程创建，
  // 也绝不会有字节被写进去。
  if (!allow_overwrite && ::access(target_path.c_str(), F_OK) == 0) {
    if (error_message != nullptr) {
      *error_message = target_path + " already exists (use --force to replace)";
    }
    return false;
  }
  PayloadBuilder request;
  std::string build_error;
  if (!request.AppendString(snapshot_id, kMaxSnapshotIdBytes, &build_error)) {
    if (error_message != nullptr) {
      *error_message = build_error;
    }
    return false;
  }
  FrameHeader header;
  std::string response;
  if (!Request(Opcode::kDownloadBegin, request.data(), &header, &response,
               error_message)) {
    return false;
  }
  // 服务端从这里开始处于 DOWNLOAD_IN_PROGRESS。之后不管哪一步在本地失败
  // （响应解码、创建临时文件、本地写、服务端多发字节），都必须显式收尾一次：
  // 服务端允许客户端提前结束（HandleDownloadEnd 接受 sent != declared），
  // 会关掉句柄并把连接放回"已认证"。
  // 判断依据是"事务开始了没有"，不是"数据收完了没有"：本地失败时数据往往还
  // 没收完，但 socket 完全可能还是好的。
  bool download_active = true;
  // 收尾请求用一个**独立的**错误变量：调用方拿到手的永远是那个原始本地错误，
  // 不会被收尾的结果覆盖。收尾本身失败不影响已经发布的文件：如实报告（Fail）
  // 但不回滚。收尾请求自己失败时 Request 会关掉这条连接（token 保留），
  // 下一次操作照常重连 + RESUME。
  // 连接已经不可用就不用（也不该）为它重连一次：服务端在 EOF 时已经清过
  // 上传 / 下载状态了。
  const auto end_download_transaction = [&]() {
    if (!download_active) {
      return;
    }
    download_active = false;
    if (fd_ < 0 || SocketLooksClosed(fd_)) {
      return;
    }
    std::string finish_error;
    if (!Request(Opcode::kDownloadEnd, std::string(), &header, &response,
                 &finish_error)) {
      Fail(finish_error);
    }
  };
  RemoteSnapshotInfo info;
  std::uint64_t declared_size = 0;
  PayloadReader reader(response);
  if (!reader.ReadString(kMaxDisplayNameBytes, &info.display_name) ||
      !reader.ReadString(kSha256HexBytes, &info.sha256) ||
      !reader.ReadU64(&declared_size) || !reader.AtEnd()) {
    if (error_message != nullptr) {
      *error_message = "cannot decode the DOWNLOAD_BEGIN response";
    }
    // 请求已经被接受了：解码失败也是"事务已经开始"之后失败。
    end_download_transaction();
    return false;
  }
  info.snapshot_id = snapshot_id;
  info.size_bytes = declared_size;

  // 中间产物是**唯一命名的**临时文件，而不是固定的 target + ".part"：
  //   * 固定名字会 O_TRUNC 掉用户本来就在那里的同名文件，而那是用户的数据；
  //   * mkstemp 出来的名字带随机后缀、0600、O_CREAT|O_EXCL，所以既不会截断
  //     别人预放的文件，也不会跟随别人预放的符号链接；
  //   * 必须与目标同目录：只有同一个文件系统上才能原子发布。
  const std::string target_directory = ParentDirectoryOf(target_path);
  std::string leaf = BaseNameOf(target_path);
  // 模板要留出后缀的位置（NAME_MAX 是 255），
  // 临时文件叫什么并不重要。
  if (leaf.size() > 40) {
    leaf.resize(40);
  }
  if (leaf.empty()) {
    leaf = "download";
  }
  FileSink sink;
  if (!sink.OpenTemp(target_directory, leaf + ".part-", error_message)) {
    // 临时文件都没建起来，但服务端那边的下载事务已经开始了：收尾。
    end_download_transaction();
    return false;
  }
  const std::string part_path = sink.path();
  crypto::Sha256 hasher;
  std::uint64_t received = 0;
  if (progress) {
    progress(RemoteTransferProgress{"download", 0, declared_size});
  }
  bool ok = true;
  for (;;) {
    if (!Request(Opcode::kDownloadChunk, std::string(), &header, &response,
                 error_message)) {
      ok = false;
      break;
    }
    if (response.empty()) {
      // 服务端发来一个空 chunk 就是"发完了"。
      break;
    }
    if (!sink.Write(response.data(), response.size(), error_message)) {
      ok = false;
      break;
    }
    hasher.Update(response.data(), response.size());
    received += response.size();
    if (received > declared_size) {
      if (error_message != nullptr) {
        *error_message = "the server sent more bytes than it declared";
      }
      ok = false;
      break;
    }
    if (progress) {
      progress(RemoteTransferProgress{"download", received, declared_size});
    }
  }
  unsigned char digest[crypto::kSha256DigestSize];
  hasher.Final(digest);
  const std::string actual_sha256 =
      crypto::ToHex(digest, crypto::kSha256DigestSize);

  if (ok && (received != declared_size || actual_sha256 != info.sha256)) {
    if (error_message != nullptr) {
      *error_message =
          "the downloaded archive does not match the declared size or"
          " SHA-256; nothing was published";
    }
    ok = false;
  }
  // flush -> fsync -> close 三步全部成功，才算"临时文件已经完整落盘"。
  if (ok && !sink.Close(error_message)) {
    ok = false;
  }
  if (!ok) {
    // 失败绝不发布目标文件：只删掉自己这个唯一命名的临时文件，
    // 然后无论失败发生在哪一步都收尾（连接已经坏了的话这一步自动跳过）。
    sink.Abandon();
    end_download_transaction();
    return false;
  }
  // 发布是**一步原子操作**，"不覆盖"由内核保证（link 已存在返回 EEXIST；
  // 不支持 link 时退到 renameat2(RENAME_NOREPLACE)；
  // 两个都不可用就 fail closed，绝不退回普通 rename）。
  // 所以目标文件是这次下载开始之后才被创建的也好、
  // 是早就存在的也好，只要不允许覆盖，
  // 就不可能有一个字节被写进去。
  // allow_overwrite 为真时才做原子替换：那是用户明确同意的覆盖。
  std::string publish_error;
  const bool published =
      allow_overwrite
          ? PublishReplacing(part_path, target_path, &publish_error)
          : PublishNoReplace(part_path, target_path, &publish_error);
  if (!published) {
    // 两种发布原语都保证失败时目标文件一个字节都没被碰过；这里只清理自己的
    // 临时文件（Close() 成功之后 FileSink 认为路径已提交，不会再替我们删）。
    ::unlink(part_path.c_str());
    sink.Abandon();
    if (error_message != nullptr) {
      if (!allow_overwrite && ::access(target_path.c_str(), F_OK) == 0) {
        *error_message = target_path +
                         " already exists (use --force to replace); nothing was"
                         " published";
      } else {
        *error_message = publish_error.empty()
                             ? ("cannot publish " + target_path)
                             : publish_error;
      }
    }
    end_download_transaction();
    return false;
  }
  end_download_transaction();
  if (downloaded != nullptr) {
    *downloaded = info;
  }
  return true;
}

bool RemoteArchiveClient::Delete(const std::string& snapshot_id,
                                 std::string* error_message) {
  if (!RequireAuthenticated("delete", error_message)) {
    return false;
  }
  PayloadBuilder request;
  std::string build_error;
  if (!request.AppendString(snapshot_id, kMaxSnapshotIdBytes, &build_error)) {
    if (error_message != nullptr) {
      *error_message = build_error;
    }
    return false;
  }
  FrameHeader header;
  std::string response;
  return Request(Opcode::kDelete, request.data(), &header, &response,
                 error_message);
}

bool RemoteArchiveClient::DeleteAccount(const std::string& password,
                                        std::string* error_message) {
  if (!RequireAuthenticated("delete-account", error_message)) {
    return false;
  }
  PayloadBuilder request;
  std::string build_error;
  if (!request.AppendString(password, kMaxPasswordBytes, &build_error)) {
    if (error_message != nullptr) {
      *error_message = build_error;
    }
    return false;
  }
  FrameHeader header;
  std::string response;
  const bool ok = Request(Opcode::kDeleteAccount, request.data(), &header,
                          &response, error_message);
  if (ok) {
    // 账户已经不存在了：这条连接上的会话没有任何意义，token 立刻丢掉。
    authenticated_ = false;
    token_.clear();
  }
  return ok;
}

}  // namespace net
}  // namespace backupproject
