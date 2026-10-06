// src/network/remote_backup_client.cpp

// 模块职责：把"一次远程操作"翻译成 BPNET1 的请求序列，并守住三条不变量：
//   * 网络上没有明文 —— 每条 TCP 连接先做 BPSEC1/BPSEC2 握手，失败即断开；
//   * 不重发 —— 请求一旦写出，失败就如实上报，绝不自动重试（写失败时对端
//     完全可能已经执行了它）；
//   * 不破坏本地已有文件 —— 下载先写唯一命名的临时文件，长度与 SHA-256 都对
//     得上才原子发布。
//
// 它不负责：归档格式（.bak 的字节由上层引擎产生）、增量链的计算、凭据持久化。
// 本类只认"名字 + 长度 + SHA-256"，服务端在这一层的理解与它一致。
//
// 数据流（上传）：HashFile 算摘要 -> UPLOAD_BEGIN(名字, 长度, 摘要, 链关系) ->
// UPLOAD_CHUNK x N -> UPLOAD_END -> 用响应填 RemoteSnapshotInfo。
// 数据流（下载）：DOWNLOAD_BEGIN -> DOWNLOAD_CHUNK 直到空 payload -> FileSink
// 临时文件 -> 校验长度与摘要 -> PublishNoReplace / PublishReplacing ->
// DOWNLOAD_END。
//
// 线程与生命周期：一条连接一个实例，实例内的状态（fd_ / channel_ / token_）
// 全部不做同步，调用方必须串行调用；要并发就各自持有实例。token 只活在内存
// 里，Disconnect() 是唯一会主动丢弃它的本地动作。
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

// 与 ParentDirectoryOf 配对使用，同样只做字符串切分：不解析 ".."、不解析符号
// 链接、不做 realpath。目标路径来自用户参数，这里只用它算出"临时文件该放在
// 哪个目录、用什么前缀"，真正的原子发布由内核的 link / renameat2 保证。
std::string BaseNameOf(const std::string& path) {
  const std::size_t slash = path.rfind('/');
  if (slash == std::string::npos) {
    return path;
  }
  return path.substr(slash + 1);
}

// 只接受普通文件：目录、FIFO、设备文件这类"看起来能读"的东西会把后面的流式
// 读取拖住，或者给出一个不稳定的长度。这里取到的大小随后作为 UPLOAD_BEGIN 的
// declared_size，由服务端逐字节对照。
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
// 第一遍读文件：只算摘要，不写任何东西，内存占用固定为一个块。
// 这一遍与真正发送的那一遍用的是两个独立的 fd，因此两遍之间文件被改写是可能
// 的；兜底在服务端——它按收到的字节重算 SHA-256，对不上就回
// kIntegrityMismatch 并删掉临时文件，不会有半个快照被发布。
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
// 手里的 fd 依然“有效”——往里写不会立刻报错，响应却永远不会来。表现出来
// 就是“奇数次失败、偶数次有响应”：失败那一次之后客户端把 token 一起丢了，
// 下一次只能重新登录，于是又“好”了一次。
// MSG_PEEK 只"看"不取：这次探测不会吃掉后续帧的任何字节，所以它可以在任意
// 两次请求之间安全调用。判不准时一律返回 false（当成连接可用），把结论留给
// 接下来的收发——那样至少还能拿到一个 errno。
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

// 协议状态码到用户可见文案的唯一来源（CLI 与 GUI 都走这里）。
// 注意：上层的会话管理靠这段文本反查状态码来区分"会话失效"与"网络故障"
// （见 ResumeSession），所以改动文案会牵动控制器的判断逻辑与相关测试。
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
      return "当前状态不允许这个操作（例如这个快照还有增量快照依赖它，"
             "必须先删后代）";
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
    case Status::kChainConflict:
      return "远端增量链状态冲突（父快照已被删除、已经有别的增量挂在它下面，"
             "或代数超过了可恢复的上限）";
  }
  return "未知错误";
}

RemoteArchiveClient::RemoteArchiveClient() = default;

RemoteArchiveClient::~RemoteArchiveClient() { Disconnect(); }

// last_error_ 保留最近一次失败的协议层原始原因（英文），供日志与测试使用；
// 它不会被主动清空，只有下一次失败会覆盖它。
void RemoteArchiveClient::Fail(const std::string& reason) {
  last_error_ = reason;
}

// 关掉 TCP 连接并把加密通道复位：会话密钥与记录序号随连接一起作废，下一条
// 连接必须重新握手（每次连接一套新密钥，不复用、不续用）。fd_ 置 -1 是"没有
// 连接"的唯一表示，PrepareConnection 依赖它判断要不要重连。
void RemoteArchiveClient::DisconnectSocket() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  // 加密通道的会话密钥与记录序号随连接一起作废：下一条 TCP 连接必须重新
  // 握手（每次连接一套新的密钥，不复用、不续用）。
  channel_.Reset();
  // 连接没了，这条连接上的会话自然也没了；但 token 还在手里，可以在新连接
  // 上恢复（Authenticate/PrepareConnection 会做这件事）。
  authenticated_ = false;
}

// 明确放弃会话：连接与 token 一起丢掉。token 只存在于内存里，所以这里没有
// 任何与磁盘有关的清理动作。
void RemoteArchiveClient::Disconnect() {
  DisconnectSocket();
  // token 只活在内存里：明确放弃会话时就丢掉，绝不写文件。
  if (!token_.empty()) {
    token_.clear();
  }
}

// 建连 + 握手。它不动 token：调用方可能只是想换一条连接继续用同一个会话
// （PrepareConnection 会在返回之后自己发 RESUME）。
// 超时设的是 SO_RCVTIMEO / SO_SNDTIMEO，也就是每次 recv/send 的超时，而不是
// 整份文件传输的整体预算；真正有整体预算的只有握手（channel_ 内部 deadline）。
// 两条身份路径（pin 与 certificate）互斥：一旦选定证书模式就不会再看 pin。
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

  // TCP 连上之后的第一件事是 BPSEC1 握手，之后才谈 BPNET1 业务帧。
  //
  // 两条失败路径都是"直接失败"，没有第三条：
  //   * pin 没配置或格式不对 -> 拒绝连接（不做 TOFU）；
  //   * 握手失败（身份不符 / Finished 校验失败 / 对端根本不说 BPSEC1）
  //     -> 关连接，**绝不**退回明文 BPNET1。
  channel_.Reset();
  channel_.SetHandshakeTimeoutMs(60000);

  // BPSEC2：签名身份模式。证书与 pin 是两条互斥的路，这里一旦选定证书，
  // 就不会再去看 pin（失败也不会回退）。
  if (endpoint.identity_mode == "certificate") {
    net::ServerIdentityPolicy policy;
    if (!endpoint.trusted_roots_file.empty()) {
      std::string roots_error;
      if (!crypto::TrustedRootStore::LoadFromFile(
              endpoint.trusted_roots_file, &policy.roots, &roots_error)) {
        const std::string reason = "无法加载可信根文件：" + roots_error;
        ::close(fd_);
        fd_ = -1;
        if (error_message != nullptr) {
          *error_message = reason;
        }
        Fail(reason);
        return false;
      }
    } else {
      // 官方云端：根是编译进客户端的内置常量，用户侧零配置，也不存在
      // "在旁边放一个根文件就能改信任"这种可能。
      policy.roots = crypto::TrustedRootStore::OfficialCloudStore();
    }
    policy.expected_server_id = endpoint.expected_server_id;
    std::string certificate_error;
    if (!channel_.HandshakeClientWithCertificate(fd_, policy,
                                                 &certificate_error)) {
      const std::string reason =
          std::string("BPSEC2 签名身份握手失败（") +
          SecureTransportErrorName(channel_.last_error()) + "）：" +
          certificate_error;
      ::close(fd_);
      fd_ = -1;
      if (error_message != nullptr) {
        *error_message = reason;
      }
      Fail(reason);
      return false;
    }
    return true;
  }

  std::string pin_error;
  if (!ParseServerKeyPin(endpoint.server_key_pin, &server_key_pin_,
                         &pin_error)) {
    const std::string reason = "无法使用服务端传输身份 pin：" + pin_error;
    ::close(fd_);
    fd_ = -1;
    if (error_message != nullptr) {
      *error_message = reason;
    }
    Fail(reason);
    return false;
  }
  // 客户端侧握手整体预算：60 秒（上面已经设过）。对端即使持有正确的身份
  // 私钥，慢慢滴水同样能把客户端挂住（审查轮缺陷 C）。
  std::string handshake_error;
  if (!channel_.HandshakeClient(fd_, server_key_pin_, &handshake_error)) {
    const std::string reason = std::string("BPSEC1 握手失败（") +
                               SecureTransportErrorName(channel_.last_error()) +
                               "）：" + handshake_error;
    ::close(fd_);
    fd_ = -1;
    if (error_message != nullptr) {
      *error_message = reason;
    }
    Fail(reason);
    return false;
  }
  return true;
}

void RemoteArchiveClient::SetReconnectEndpoint(const RemoteEndpoint& endpoint) {
  // 只改"下次重连的参数"。连接、会话、token 都不动（见头文件里的说明）。
  endpoint_ = endpoint;
}

// 幂等：连接可用时几乎零成本（一次 poll）。重连之后如果手里还有 token，就先
// RESUME 恢复会话，然后才发调用方真正想发的那个请求。
// 这里发生的所有 I/O 都在"本次请求的第一个字节发出之前"，所以它不违反
// no-retry：没有任何一个用户请求会被发第二遍。
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

// 只有服务端明确回 kUnauthorized 才丢 token（那才是"会话真的失效了"）；
// 其他失败（超时、断连、对端重启）一律保留 token，让用户的下一次操作自己
// 重连。失败时给出的文案用的是共享的状态文案，控制器据此分类。
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

// 所有请求的唯一出口：prepare -> 分配 request_id -> 发送 -> 收响应 ->
// 校验配对。
//
// 失败分级（每一条都决定连接与 token 的命运）：
//   * 发送失败 / 读响应失败：连接不可信，关掉连接但保留 token，并且不重发——
//     已经写出去的那半个请求，对端可能已经执行了；
//   * 响应的 request_id 与请求不符：协议级错乱或串话，直接 Disconnect()
//     （连 token 一起丢），因为无法判断对端到底是谁；
//   * 对端回了非 kOk：记录 last_status_，连接与 token 都保留，由调用方
//     （例如 RESUME 的调用者）自己决定要不要丢会话。
// 成功时 response 是服务端 payload 的原始字节，由各个方法自己解码。
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
  if (!channel_.SendFrame(fd_, static_cast<std::uint16_t>(opcode), 0,
                          request_id, payload, &io_error)) {
    Fail(io_error);
    if (error_message != nullptr) {
      *error_message = "cannot send the request: " + io_error;
    }
    // 写失败了：这条连接不可信。**不重发**——这个请求有可能已经被对端收到了。
    DisconnectSocket();
    return false;
  }
  const FrameReadStatus status =
      channel_.ReceiveFrame(fd_, header, response, &io_error);
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

// 不需要会话（服务端在状态检查之前就处理它），因此它是"服务端还活着吗、它说的
// 是哪一版协议"的探针。响应必须完整解码到末尾，多一个字节都算失败：版本协商
// 不能建在"读到自己要的就算成功"上。
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

// 口令只作为请求 payload 的一部分经过加密通道：不进日志、不进 last_error，
// 返回值里也没有任何与口令相关的内容。
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

// 语义上是"本地会话结束"：无论服务端是否接受，本地 token 都会被丢掉。这样即使
// 服务端已经不可达，用户也能明确地退出登录。云端数据不受影响——真正删除云端
// 数据的是 DeleteAccount。
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

// 逐条严格解码：条目数由服务端给（上限 kMaxListEntries），任何一条字段缺失、
// 长度超限或 kind 未知，整次调用就失败——不做"跳过坏条目继续"，那样会把一份
// 不完整的列表当成完整的交给用户。解析结果先放进局部 vector，全部成功之后才
// 写入 *snapshots，失败不会留下半份列表。
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
        !reader.ReadU64(&info.created_at) ||
        !reader.ReadU16(&info.snapshot_kind) ||
        !reader.ReadU64(&info.generation) ||
        !reader.ReadString(kMaxSnapshotIdBytes, &info.parent_snapshot_id) ||
        !reader.ReadString(kMaxLineageBytes, &info.lineage)) {
      if (error_message != nullptr) {
        *error_message = "cannot decode a LIST entry";
      }
      return false;
    }
    if (!IsKnownSnapshotKind(info.snapshot_kind)) {
      if (error_message != nullptr) {
        *error_message = "the LIST entry declares an unknown snapshot kind";
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
  // 低层 upload 命令保持原语义：一个独立的完整快照（full / generation 0）。
  return UploadSnapshotFile(local_path, display_name, RemoteUploadOptions(),
                            progress, uploaded, error_message);
}

// 两遍读是协议决定的：UPLOAD_BEGIN 必须在发送任何数据之前给出长度与摘要，所以
// 第一遍只能扫描算 SHA-256，第二遍才分块发送。代价是读两次盘，换来的是上传
// 期间内存里只有一个块，而不是整份归档。
//
// 事务边界是连接：UPLOAD_BEGIN 被接受之后，任何本地失败（读文件出错、文件被
// 截短、发块失败）都关掉 socket，让服务端从这里读到 EOF 并删掉上传临时文件。
// token 保留——本地文件出错不是"退出登录"。
//
// 大小有两道彼此独立的闸：客户端是 kDefaultMaxUploadBytes（发送任何字节之前的
// 早退），服务端是 --max-upload-bytes（权威值）。progress 在开始、每个块之后
// 被调用，bytes_done 单调不减。
bool RemoteArchiveClient::UploadSnapshotFile(
    const std::string& local_path, const std::string& display_name,
    const RemoteUploadOptions& options, const RemoteProgressCallback& progress,
    RemoteSnapshotInfo* uploaded, std::string* error_message) {
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
  // 链关系。字段顺序必须与服务端的解码顺序一致：
  //   display_name -> size -> sha256 -> kind -> parent_id -> lineage
  begin.AppendU16(options.snapshot_kind);
  if (!begin.AppendString(options.parent_snapshot_id, kMaxSnapshotIdBytes,
                          &build_error) ||
      !begin.AppendString(options.lineage, kMaxLineageBytes, &build_error)) {
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
      !reader.ReadU16(&info.snapshot_kind) ||
      !reader.ReadU64(&info.generation) ||
      !reader.ReadString(kMaxSnapshotIdBytes, &info.parent_snapshot_id) ||
      !reader.AtEnd()) {
    if (error_message != nullptr) {
      *error_message = "cannot decode the UPLOAD_END response";
    }
    return false;
  }
  info.lineage = options.lineage;
  info.display_name = display_name;
  info.created_at = created_at;
  if (uploaded != nullptr) {
    *uploaded = info;
  }
  return true;
}

// 下载事务：BEGIN 之后服务端处于 DOWNLOAD_IN_PROGRESS，客户端无论在哪一步失败
// 都要显式发一次 DOWNLOAD_END（服务端允许提前结束），或者直接断连——两条路径
// 服务端都会把状态清回"已认证"。download_active 记录的就是"事务开始了没有"，
// 而不是"数据收完了没有"。
//
// 中间产物是目标目录里唯一命名的临时文件（FileSink::OpenTemp，mkstemp 0600），
// 校验（长度 + SHA-256）发生在 fsync/close 之后、发布之前，所以只要发布成功，
// 目标文件一定是完整的。失败时的收尾顺序固定：先 Abandon() 删掉自己的临时
// 文件，再 end_download_transaction()；收尾用的错误变量是独立的，调用方拿到
// 的永远是那个原始本地错误。
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

// 服务端会拒绝删除还有增量后代的快照（kInvalidState），并在文案里说明要先删
// 后代。客户端不做本地预判，把判断权留给唯一的权威。
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

// 只有成功才丢会话：失败（例如口令不对）时连接与会话都保持原样，用户可以重试
// 或继续用别的命令。
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
