// src/network/remote_backup_client.cpp

#include "remote_backup_client.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

#include "crypto.h"

namespace backupproject {
namespace net {
namespace {

std::string StrerrorText() { return std::string(std::strerror(errno)); }

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

bool WriteWholeFile(int fd, const char* data, std::size_t size,
                    std::string* error_message) {
  std::size_t written = 0;
  while (written < size) {
    const ssize_t step = ::write(fd, data + written, size - written);
    if (step < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (error_message != nullptr) {
        *error_message = std::string("cannot write: ") + StrerrorText();
      }
      return false;
    }
    written += static_cast<std::size_t>(step);
  }
  return true;
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

void RemoteArchiveClient::Disconnect() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  authenticated_ = false;
  // token 只活在内存里：断开就丢掉，绝不写文件。
  if (!token_.empty()) {
    token_.clear();
  }
}

bool RemoteArchiveClient::Connect(const RemoteEndpoint& endpoint,
                                  std::string* error_message) {
  Disconnect();
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

bool RemoteArchiveClient::Request(Opcode opcode, const std::string& payload,
                                  FrameHeader* header, std::string* response,
                                  std::string* error_message) {
  if (fd_ < 0) {
    if (error_message != nullptr) {
      *error_message = "not connected";
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
    return false;
  }
  const FrameReadStatus status = ReceiveFrame(fd_, header, response, &io_error);
  if (status != FrameReadStatus::kOk) {
    Fail(io_error);
    if (error_message != nullptr) {
      *error_message = "cannot read the response: " + io_error;
    }
    // 帧流已经不可信：这条连接不能再用了。
    Disconnect();
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
  if (authenticated_) {
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
      ::close(source);
      if (error_message != nullptr) {
        *error_message = "cannot read " + local_path + ": " + StrerrorText();
      }
      return false;
    }
    if (got == 0) {
      ::close(source);
      if (error_message != nullptr) {
        *error_message = "the file shrank while it was being uploaded";
      }
      return false;
    }
    if (!Request(Opcode::kUploadChunk,
                 std::string(buffer.data(), static_cast<std::size_t>(got)),
                 &header, &response, error_message)) {
      ::close(source);
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
  // 默认不覆盖已经存在的目标：先判，再动任何字节。
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
  RemoteSnapshotInfo info;
  std::uint64_t declared_size = 0;
  PayloadReader reader(response);
  if (!reader.ReadString(kMaxDisplayNameBytes, &info.display_name) ||
      !reader.ReadString(kSha256HexBytes, &info.sha256) ||
      !reader.ReadU64(&declared_size) || !reader.AtEnd()) {
    if (error_message != nullptr) {
      *error_message = "cannot decode the DOWNLOAD_BEGIN response";
    }
    return false;
  }
  info.snapshot_id = snapshot_id;
  info.size_bytes = declared_size;

  const std::string part_path = target_path + ".part";
  const int output =
      ::open(part_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (output < 0) {
    if (error_message != nullptr) {
      *error_message = "cannot create " + part_path + ": " + StrerrorText();
    }
    return false;
  }
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
      break;
    }
    if (!WriteWholeFile(output, response.data(), response.size(),
                        error_message)) {
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
  if (ok && ::fsync(output) != 0) {
    if (error_message != nullptr) {
      *error_message = std::string("fsync failed: ") + StrerrorText();
    }
    ok = false;
  }
  ::close(output);
  if (!ok) {
    // 失败绝不发布目标文件：只删掉自己的 .part。
    ::unlink(part_path.c_str());
    return false;
  }
  if (::rename(part_path.c_str(), target_path.c_str()) != 0) {
    if (error_message != nullptr) {
      *error_message = "cannot publish " + target_path + ": " + StrerrorText();
    }
    ::unlink(part_path.c_str());
    return false;
  }
  // 下载流收尾。这一步失败不影响已经校验并发布的文件，只是让服务端早点
  // 释放句柄；如实报告但不回滚。
  std::string finish_error;
  if (!Request(Opcode::kDownloadEnd, std::string(), &header, &response,
               &finish_error)) {
    Fail(finish_error);
  }
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
