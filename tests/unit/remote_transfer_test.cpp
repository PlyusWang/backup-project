// tests/unit/remote_transfer_test.cpp
//
// PR #20：远程上传 / 下载 / 删除的端到端测试（真实 TCP 环回 + 真实 SQLite +
// 真实文件系统）。
//
// 这个文件里**没有 mock**：服务端是真的 RemoteServer 对象，客户端是真的
// socket，元数据真的写进 SQLite，blob 真的落在磁盘上，测试结束之后真的去
// 读磁盘确认。
//
// 覆盖三件事：
//   1. 正常往返（含 1 字节 / 块边界 / 块边界 +-1 / 5 MiB / 32 MiB 流式）；
//   2. 归属隔离（B 看不到、下不到、删不掉 A 的东西）；
//   3. 失败路径（声明长度不符、摘要不符、写盘失败、DB 写失败、半路断开）
//      之后**磁盘上不能留下任何已发布的 blob 或临时文件**。

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "crypto.h"
#include "network_protocol.h"
#include "remote_auth.h"
#include "remote_server.h"
#include "remote_test_support.h"
#include "test_support.h"

// 消毒剂构建的判别宏。
//
// "流式实现有没有把整份文件读进内存"和"ASan/UBSan 有没有发现内存错误"是两个
// 不同的测试目标。ASan 会自己把地址空间放大好几倍（shadow memory + 隔离区），
// 进程的 VmHWM 因而不是产品 buffering 的度量：拿生产阈值去判一个消毒剂进程，
// 只会把消毒剂运行时的开销误判成产品的内存回归。
//
// 所以 XFER T8 的 RSS 阈值只在非消毒剂构建里断言；消毒剂构建里照常跑完
// 整份 32 MiB 的传输与 SHA-256 校验，并在输出里明确写"RSS 阈值在这里不适用"。
// 消毒剂自己的零报告由 scripts/network_test.sh 扫描，与本文件无关。
#if defined(__SANITIZE_ADDRESS__)
#define XFER_SANITIZED_BUILD 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define XFER_SANITIZED_BUILD 1
#endif
#endif
#ifndef XFER_SANITIZED_BUILD
#define XFER_SANITIZED_BUILD 0
#endif

namespace bp = backupproject;
namespace crypto = backupproject::crypto;
namespace net = backupproject::net;

namespace {

constexpr int kClientTimeoutSeconds = 60;
constexpr std::uint64_t kBigTransferBytes = 32ull * 1024 * 1024;

std::string RandomHex(std::size_t bytes) {
  std::string raw;
  std::string error;
  if (!crypto::RandomBytes(bytes, &raw, &error)) {
    return std::string();
  }
  return crypto::ToHex(reinterpret_cast<const unsigned char*>(raw.data()),
                       raw.size());
}

std::string Sha256Of(const std::string& data) {
  unsigned char digest[crypto::kSha256DigestSize];
  crypto::Sha256::Hash(data.data(), data.size(), digest);
  return crypto::ToHex(digest, crypto::kSha256DigestSize);
}

// 进程峰值 RSS（VmHWM）。用来证明"上传 32 MiB 不会把整份文件读进内存"。
std::uint64_t PeakRssKb() {
  std::ifstream input("/proc/self/status");
  std::string line;
  while (std::getline(input, line)) {
    if (line.compare(0, 6, "VmHWM:") == 0) {
      return std::strtoull(line.c_str() + 6, nullptr, 10);
    }
  }
  return 0;
}

// 确定性的伪随机块生成器：只占 64 KiB 内存，可以产出任意长度的内容。
// 用它代替"先造一个 32 MiB 的字符串"，否则测试自己就把内存吃掉了。
class PatternGenerator {
 public:
  PatternGenerator() {
    std::string seed;
    std::string error;
    crypto::RandomBytes(32, &seed, &error);
    block_.resize(64 * 1024);
    unsigned char digest[crypto::kSha256DigestSize];
    crypto::Sha256::Hash(seed.data(), seed.size(), digest);
    std::uint64_t state = 0;
    for (std::size_t index = 0; index < 8; ++index) {
      state = (state << 8) | digest[index];
    }
    for (std::size_t index = 0; index < block_.size(); ++index) {
      // xorshift64*：确定性、够用，不用于任何安全目的。
      state ^= state >> 12;
      state ^= state << 25;
      state ^= state >> 27;
      block_[index] = static_cast<char>((state * 2685821657736338717ull) >> 56);
    }
  }

  void Fill(char* out, std::size_t size) {
    std::size_t written = 0;
    while (written < size) {
      const std::size_t take = std::min(size - written, block_.size());
      std::memcpy(out + written, block_.data(), take);
      written += take;
    }
  }

 private:
  std::string block_;
};

struct Fixture {
  net::RemoteServerConfig config;
  std::string base;
  std::string root;
  std::string database;
  std::string secret_file;
  // PR #21：BPSEC1 的服务端身份私钥与客户端 pin 文本（同一次生成）。
  std::string transport_key_file;
  std::string pin;
};

bool SetupFixture(Fixture* fixture, const std::string& name) {
  fixture->base = test_support::FreshDir(name);
  if (fixture->base.empty()) {
    return false;
  }
  fixture->root = fixture->base + "/data";
  fixture->database = fixture->base + "/state/metadata.sqlite3";
  fixture->secret_file = fixture->base + "/secrets.env";
  const std::string secret = RandomHex(32);
  if (secret.empty() ||
      !test_support::WriteFile(fixture->secret_file,
                               "BACKUP_TOKEN_SECRET=" + secret + "\n", 0600)) {
    return false;
  }
  // BPSEC1 身份密钥：服务端在 ServeConnection 的第一步就要用它；缺了它
  // Start() 直接失败，所以每个 fixture 都得有自己的一套。
  fixture->transport_key_file = fixture->base + "/transport.key";
  net::TransportIdentity identity;
  std::string identity_error;
  if (!remote_test_support::PrepareTransportIdentity(
          fixture->transport_key_file, &identity, &fixture->pin,
          &identity_error)) {
    return false;
  }
  fixture->config.bind_address = "127.0.0.1";
  fixture->config.port = 0;
  fixture->config.root_directory = fixture->root;
  fixture->config.database_path = fixture->database;
  fixture->config.secret_file_path = fixture->secret_file;
  fixture->config.transport_key_file_path = fixture->transport_key_file;
  fixture->config.quiet = true;
  return true;
}

int ConnectToLoopback(std::uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  timeval timeout;
  timeout.tv_sec = kClientTimeoutSeconds;
  timeout.tv_usec = 0;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  sockaddr_in address;
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = 0;
  ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) !=
      0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

std::thread ServeOneConnection(net::RemoteServer* server) {
  return std::thread([server] {
    const int client = ::accept(server->listener_fd(), nullptr, nullptr);
    if (client < 0) {
      return;
    }
    std::string error;
    server->ServeConnection(client, &error);
    ::close(client);
  });
}

struct Session {
  int fd = -1;
  // BPSEC1 会话密钥 / 序号：一条连接一个，握手完成后所有帧都走它。
  net::SecureChannel channel;
  std::uint64_t next_request_id = 1;
};

// 客户端侧握手。失败时必须显式 shutdown(SHUT_RDWR)：服务端线程这时还等在
// ServeConnection 的握手读上，不关连接的话 worker.join() 会永远卡住。
bool HandshakeOrShutdown(Session* session, const std::string& pin,
                         std::string* error) {
  if (remote_test_support::HandshakeTestClient(session->fd, pin,
                                               &session->channel, error)) {
    return true;
  }
  ::shutdown(session->fd, SHUT_RDWR);
  return false;
}

net::FrameReadStatus Request(Session* session, net::Opcode opcode,
                             const std::string& payload, net::FrameHeader* header,
                             std::string* response, std::string* error) {
  const std::uint64_t request_id = session->next_request_id++;
  if (!remote_test_support::SendTestFrame(
          &session->channel, session->fd, static_cast<std::uint16_t>(opcode), 0,
          request_id, payload, error)) {
    return net::FrameReadStatus::kIoError;
  }
  return remote_test_support::ReceiveTestFrame(&session->channel, session->fd,
                                               header, response, error);
}

std::string Credentials(const std::string& username,
                        const std::string& password) {
  net::PayloadBuilder builder;
  std::string error;
  builder.AppendString(username, net::kMaxUsernameBytes, &error);
  builder.AppendString(password, net::kMaxPasswordBytes, &error);
  return builder.data();
}

bool Register(Session* session, const std::string& username,
              const std::string& password, std::uint32_t* status) {
  net::FrameHeader header;
  std::string response;
  std::string error;
  const net::FrameReadStatus result =
      Request(session, net::Opcode::kRegister, Credentials(username, password),
              &header, &response, &error);
  if (status != nullptr) {
    *status = header.status;
  }
  return result == net::FrameReadStatus::kOk;
}

bool Login(Session* session, const std::string& username,
           const std::string& password, std::uint32_t* status) {
  net::FrameHeader header;
  std::string response;
  std::string error;
  const net::FrameReadStatus result =
      Request(session, net::Opcode::kLogin, Credentials(username, password),
              &header, &response, &error);
  if (status != nullptr) {
    *status = header.status;
  }
  return result == net::FrameReadStatus::kOk;
}

// 完整的"注册 + 登录"流程（新连接上用同一个账号时只走登录）。
bool OpenSession(net::RemoteServer* server, const std::string& username,
                 const std::string& password, const std::string& pin,
                 Session* session, std::string* error) {
  session->fd = ConnectToLoopback(server->bound_port());
  if (session->fd < 0) {
    *error = "cannot connect";
    return false;
  }
  // 服务端在业务帧之前先握手，客户端必须跟着做第一步，否则整套序列都会
  // 停在"服务端等 ClientHello、客户端等响应"上。
  if (!HandshakeOrShutdown(session, pin, error)) {
    return false;
  }
  std::uint32_t status = 0;
  if (!Register(session, username, password, &status) ||
      status != static_cast<std::uint32_t>(net::Status::kAlreadyExists)) {
    if (status != static_cast<std::uint32_t>(net::Status::kOk)) {
      *error = "register returned " + std::string(net::StatusName(status));
      return false;
    }
  }
  if (!Login(session, username, password, &status) ||
      status != static_cast<std::uint32_t>(net::Status::kOk)) {
    *error = "login returned " + std::string(net::StatusName(status));
    return false;
  }
  return true;
}

// 远端链元数据：UPLOAD_BEGIN 的字段顺序是
//   display_name -> size -> sha256 -> kind -> parent_id -> lineage
// 服务端不给缺字段的请求做默认值补齐，所以这三个字段必须显式带上。
//
// 这个文件里上传的都是**独立完整快照**（与低层 remote upload 的语义一致）：
// kind = kFull、没有父、lineage 为空串（空串表示"不属于任何链"）。整套用例
// 都不碰增量链，链本身由专门的链测试覆盖。所以这里发的字节与产品 CLI 在
// remote upload 下发的完全一致。
std::string UploadBeginPayload(const std::string& display_name,
                               std::uint64_t size,
                               const std::string& sha256) {
  net::PayloadBuilder builder;
  std::string error;
  builder.AppendString(display_name, net::kMaxDisplayNameBytes, &error);
  builder.AppendU64(size);
  builder.AppendString(sha256, net::kSha256HexBytes, &error);
  builder.AppendU16(static_cast<std::uint16_t>(net::SnapshotKind::kFull));
  builder.AppendString(std::string(), net::kMaxSnapshotIdBytes, &error);
  builder.AppendString(std::string(), net::kMaxLineageBytes, &error);
  return builder.data();
}

std::string SnapshotIdPayload(const std::string& snapshot_id) {
  net::PayloadBuilder builder;
  std::string error;
  builder.AppendString(snapshot_id, net::kMaxSnapshotIdBytes, &error);
  return builder.data();
}

// 上传内存里的一份数据。成功时把 UPLOAD_END 响应解出来的 snapshot id 写出去。
bool UploadBuffer(Session* session, const std::string& display_name,
                  const std::string& data, std::string* snapshot_id,
                  std::uint32_t* status, std::string* error) {
  net::FrameHeader header;
  std::string response;
  if (Request(session, net::Opcode::kUploadBegin,
              UploadBeginPayload(display_name, data.size(), Sha256Of(data)),
              &header, &response, error) != net::FrameReadStatus::kOk) {
    return false;
  }
  if (status != nullptr) {
    *status = header.status;
  }
  if (header.status != static_cast<std::uint32_t>(net::Status::kOk)) {
    return true;
  }
  std::size_t offset = 0;
  while (offset < data.size()) {
    const std::size_t take =
        std::min(net::kTransferChunkBytes, data.size() - offset);
    if (Request(session, net::Opcode::kUploadChunk, data.substr(offset, take),
                &header, &response, error) != net::FrameReadStatus::kOk) {
      return false;
    }
    if (header.status != static_cast<std::uint32_t>(net::Status::kOk)) {
      if (status != nullptr) {
        *status = header.status;
      }
      return true;
    }
    offset += take;
  }
  if (Request(session, net::Opcode::kUploadEnd, std::string(), &header,
              &response, error) != net::FrameReadStatus::kOk) {
    return false;
  }
  if (status != nullptr) {
    *status = header.status;
  }
  if (header.status != static_cast<std::uint32_t>(net::Status::kOk)) {
    return true;
  }
  // UPLOAD_END 响应：snapshot_id -> sha256 -> size -> created_at -> kind ->
  // generation -> parent_id（链特性之后多了后三个字段）。
  net::PayloadReader reader(response);
  std::string sha;
  std::uint64_t size = 0;
  std::uint64_t created_at = 0;
  std::uint16_t kind = 0;
  std::uint64_t generation = 0;
  std::string parent_id;
  if (!reader.ReadString(net::kMaxSnapshotIdBytes, snapshot_id) ||
      !reader.ReadString(net::kSha256HexBytes, &sha) ||
      !reader.ReadU64(&size) || !reader.ReadU64(&created_at) ||
      !reader.ReadU16(&kind) || !reader.ReadU64(&generation) ||
      !reader.ReadString(net::kMaxSnapshotIdBytes, &parent_id) ||
      !reader.AtEnd()) {
    *error = "cannot decode the UPLOAD_END response";
    return false;
  }
  // full 快照必须是链根：没有父、代数 0。
  if (kind != static_cast<std::uint16_t>(net::SnapshotKind::kFull) ||
      generation != 0 || !parent_id.empty()) {
    *error = "the server did not report this upload as a full chain root";
    return false;
  }
  *error = sha + "|" + std::to_string(size) + "|" + std::to_string(created_at);
  return true;
}

// 流式上传：数据由生成器现产现发，内存里只留一个块。
bool UploadStream(Session* session, const std::string& display_name,
                  std::uint64_t total, PatternGenerator* generator,
                  std::string* snapshot_id, std::uint32_t* status,
                  std::string* error) {
  net::FrameHeader header;
  std::string response;
  crypto::Sha256 hasher;
  std::vector<char> buffer(net::kTransferChunkBytes);
  std::uint64_t remaining = total;
  while (remaining > 0) {
    const std::size_t take = static_cast<std::size_t>(
        std::min<std::uint64_t>(remaining, buffer.size()));
    generator->Fill(buffer.data(), take);
    hasher.Update(buffer.data(), take);
    remaining -= take;
  }
  unsigned char digest[crypto::kSha256DigestSize];
  crypto::Sha256 finish = hasher;
  finish.Final(digest);
  const std::string sha = crypto::ToHex(digest, crypto::kSha256DigestSize);

  if (Request(session, net::Opcode::kUploadBegin,
              UploadBeginPayload(display_name, total, sha), &header, &response,
              error) != net::FrameReadStatus::kOk) {
    return false;
  }
  if (status != nullptr) {
    *status = header.status;
  }
  if (header.status != static_cast<std::uint32_t>(net::Status::kOk)) {
    return true;
  }
  // 重新生成一遍并发送（生成器是确定性的，所以内容与上面哈希的一致）。
  PatternGenerator sender;
  sender = *generator;
  remaining = total;
  while (remaining > 0) {
    const std::size_t take = static_cast<std::size_t>(
        std::min<std::uint64_t>(remaining, buffer.size()));
    sender.Fill(buffer.data(), take);
    if (Request(session, net::Opcode::kUploadChunk,
                std::string(buffer.data(), take), &header, &response,
                error) != net::FrameReadStatus::kOk) {
      return false;
    }
    if (header.status != static_cast<std::uint32_t>(net::Status::kOk)) {
      if (status != nullptr) {
        *status = header.status;
      }
      return true;
    }
    remaining -= take;
  }
  if (Request(session, net::Opcode::kUploadEnd, std::string(), &header,
              &response, error) != net::FrameReadStatus::kOk) {
    return false;
  }
  if (status != nullptr) {
    *status = header.status;
  }
  if (header.status != static_cast<std::uint32_t>(net::Status::kOk)) {
    return true;
  }
  net::PayloadReader reader(response);
  std::string returned_sha;
  std::uint64_t size = 0;
  std::uint64_t created_at = 0;
  std::uint16_t kind = 0;
  std::uint64_t generation = 0;
  std::string parent_id;
  if (!reader.ReadString(net::kMaxSnapshotIdBytes, snapshot_id) ||
      !reader.ReadString(net::kSha256HexBytes, &returned_sha) ||
      !reader.ReadU64(&size) || !reader.ReadU64(&created_at) ||
      !reader.ReadU16(&kind) || !reader.ReadU64(&generation) ||
      !reader.ReadString(net::kMaxSnapshotIdBytes, &parent_id)) {
    *error = "cannot decode the UPLOAD_END response";
    return false;
  }
  *error = returned_sha + "|" + std::to_string(size);
  return true;
}

// 下载：把整份内容写进 *out，同时校验服务端声明的长度。
bool DownloadBuffer(Session* session, const std::string& snapshot_id,
                    std::string* out, std::uint64_t* declared_size,
                    std::string* declared_sha, std::uint32_t* status,
                    std::string* error) {
  net::FrameHeader header;
  std::string response;
  if (Request(session, net::Opcode::kDownloadBegin,
              SnapshotIdPayload(snapshot_id), &header, &response,
              error) != net::FrameReadStatus::kOk) {
    return false;
  }
  if (status != nullptr) {
    *status = header.status;
  }
  if (header.status != static_cast<std::uint32_t>(net::Status::kOk)) {
    return true;
  }
  net::PayloadReader reader(response);
  std::string display_name;
  if (!reader.ReadString(net::kMaxDisplayNameBytes, &display_name) ||
      !reader.ReadString(net::kSha256HexBytes, declared_sha) ||
      !reader.ReadU64(declared_size)) {
    *error = "cannot decode the DOWNLOAD_BEGIN response";
    return false;
  }
  out->clear();
  for (;;) {
    if (Request(session, net::Opcode::kDownloadChunk, std::string(), &header,
                &response, error) != net::FrameReadStatus::kOk) {
      return false;
    }
    if (header.status != static_cast<std::uint32_t>(net::Status::kOk)) {
      if (status != nullptr) {
        *status = header.status;
      }
      return true;
    }
    if (response.empty()) {
      break;
    }
    out->append(response);
  }
  if (Request(session, net::Opcode::kDownloadEnd, std::string(), &header,
              &response, error) != net::FrameReadStatus::kOk) {
    return false;
  }
  if (status != nullptr) {
    *status = header.status;
  }
  return true;
}

// 只统计长度与摘要，不保留内容（32 MiB 用）。
bool DownloadAndHash(Session* session, const std::string& snapshot_id,
                     std::uint64_t* total, std::string* sha,
                     std::uint32_t* status, std::string* error) {
  net::FrameHeader header;
  std::string response;
  if (Request(session, net::Opcode::kDownloadBegin,
              SnapshotIdPayload(snapshot_id), &header, &response,
              error) != net::FrameReadStatus::kOk) {
    return false;
  }
  if (status != nullptr) {
    *status = header.status;
  }
  if (header.status != static_cast<std::uint32_t>(net::Status::kOk)) {
    return true;
  }
  crypto::Sha256 hasher;
  std::uint64_t received = 0;
  for (;;) {
    if (Request(session, net::Opcode::kDownloadChunk, std::string(), &header,
                &response, error) != net::FrameReadStatus::kOk) {
      return false;
    }
    if (header.status != static_cast<std::uint32_t>(net::Status::kOk)) {
      return true;
    }
    if (response.empty()) {
      break;
    }
    hasher.Update(response.data(), response.size());
    received += response.size();
  }
  unsigned char digest[crypto::kSha256DigestSize];
  hasher.Final(digest);
  *sha = crypto::ToHex(digest, crypto::kSha256DigestSize);
  *total = received;
  if (Request(session, net::Opcode::kDownloadEnd, std::string(), &header,
              &response, error) != net::FrameReadStatus::kOk) {
    return false;
  }
  return true;
}

bool ListSnapshots(Session* session,
                   std::vector<net::PayloadReader>* /*unused*/,
                   std::vector<std::string>* ids, std::uint32_t* status,
                   std::string* error) {
  net::FrameHeader header;
  std::string response;
  if (Request(session, net::Opcode::kList, std::string(), &header, &response,
              error) != net::FrameReadStatus::kOk) {
    return false;
  }
  if (status != nullptr) {
    *status = header.status;
  }
  if (header.status != static_cast<std::uint32_t>(net::Status::kOk)) {
    return true;
  }
  net::PayloadReader reader(response);
  std::uint32_t count = 0;
  if (!reader.ReadU32(&count)) {
    *error = "cannot read the LIST count";
    return false;
  }
  ids->clear();
  for (std::uint32_t index = 0; index < count; ++index) {
    std::string id;
    std::string display_name;
    std::string sha;
    std::uint64_t size = 0;
    std::uint64_t created_at = 0;
    // 链元数据：kind -> generation -> parent_id -> lineage。
    std::uint16_t kind = 0;
    std::uint64_t generation = 0;
    std::string parent_id;
    std::string lineage;
    if (!reader.ReadString(net::kMaxSnapshotIdBytes, &id) ||
        !reader.ReadString(net::kMaxDisplayNameBytes, &display_name) ||
        !reader.ReadString(net::kSha256HexBytes, &sha) ||
        !reader.ReadU64(&size) || !reader.ReadU64(&created_at) ||
        !reader.ReadU16(&kind) || !reader.ReadU64(&generation) ||
        !reader.ReadString(net::kMaxSnapshotIdBytes, &parent_id) ||
        !reader.ReadString(net::kMaxLineageBytes, &lineage)) {
      *error = "cannot decode a LIST entry";
      return false;
    }
    ids->push_back(id);
  }
  return true;
}

bool DeleteSnapshot(Session* session, const std::string& snapshot_id,
                    std::uint32_t* status, std::string* error) {
  net::FrameHeader header;
  std::string response;
  if (Request(session, net::Opcode::kDelete, SnapshotIdPayload(snapshot_id),
              &header, &response, error) != net::FrameReadStatus::kOk) {
    return false;
  }
  if (status != nullptr) {
    *status = header.status;
  }
  return true;
}

// 统计一个目录里剩下多少条目（上传临时文件 / trash 里的残留都用它检查）。
std::size_t CountEntries(const std::string& path) {
  const std::vector<std::string> entries = test_support::DirEntries(path);
  std::size_t count = 0;
  for (const std::string& entry : entries) {
    if (entry != "." && entry != "..") {
      count += 1;
    }
  }
  return count;
}

// 统计用户目录里已发布的 blob（*.bak）。用户目录本身还有 tmp / trash
// 两个子目录，所以不能直接数条目：要数的是"有没有不该存在的归档文件"。
std::size_t CountBlobs(const std::string& path) {
  const std::vector<std::string> entries = test_support::DirEntries(path);
  std::size_t count = 0;
  for (const std::string& entry : entries) {
    if (entry.size() > 4 && entry.compare(entry.size() - 4, 4, ".bak") == 0) {
      count += 1;
    }
  }
  return count;
}

// 用户目录：<root>/users/<id>。测试用第 1 个注册用户，所以 id 是 1。
std::string UserDirectory(const Fixture& fixture) {
  return fixture.root + "/users/1";
}

bool WaitForEmptyDirectory(const std::string& path, int attempts) {
  for (int index = 0; index < attempts; ++index) {
    if (CountEntries(path) == 0) {
      return true;
    }
    ::usleep(50000);
  }
  return CountEntries(path) == 0;
}

}  // namespace

int main() {
  test_support::Section("XFER 1. 往返：上传 -> 落盘 -> 列表 -> 下载 -> 删除");
  Fixture fixture;
  test_support::Check(SetupFixture(&fixture, "xfer-main"), "XFER T1 夹具就绪");
  net::RemoteServer server;
  std::string error;
  test_support::Check(
      server.Configure(fixture.config, &error) && server.Start(&error),
      "XFER T1 服务端启动", error);
  std::thread worker = ServeOneConnection(&server);
  const std::string username = "night-" + RandomHex(4);
  const std::string password = RandomHex(16);
  Session session;
  std::string session_error;
  test_support::Check(
      OpenSession(&server, username, password, fixture.pin, &session,
                  &session_error),
      "XFER T1 注册并登录", session_error);

  const std::string data = RandomHex(512);  // 1024 字节
  const std::string sha = Sha256Of(data);
  std::string snapshot_id;
  std::uint32_t status = 0;
  std::string note;
  const bool uploaded =
      UploadBuffer(&session, "first.bak", data, &snapshot_id, &status, &note);
  test_support::Check(uploaded && status == static_cast<std::uint32_t>(
                                          net::Status::kOk),
                      "XFER T1 上传成功", net::StatusName(status));
  test_support::Check(net::IsValidSnapshotId(snapshot_id, &error),
                      "XFER T1 服务端生成的 snapshot id 合法");
  test_support::Check(note.compare(0, sha.size() + 6, sha + "|1024|") == 0,
                      "XFER T1 判别：UPLOAD_END 回的摘要与长度就是文件事实",
                      note);

  const std::string user_directory = UserDirectory(fixture);
  const std::string blob_path = user_directory + "/" + snapshot_id + ".bak";
  std::string on_disk;
  test_support::Check(
      test_support::ReadFile(blob_path, &on_disk) && on_disk == data,
      "XFER T1 判别：磁盘 blob 与上传内容逐字节一致");
  test_support::Check(CountEntries(user_directory + "/tmp") == 0,
                      "XFER T1 判别：tmp 目录没有残留");
  test_support::Check(CountEntries(user_directory + "/trash") == 0,
                      "XFER T1 判别：trash 目录没有残留");

  std::vector<std::string> ids;
  status = 0;
  test_support::Check(ListSnapshots(&session, nullptr, &ids, &status, &error) &&
                          status == static_cast<std::uint32_t>(net::Status::kOk) &&
                          ids.size() == 1 && ids[0] == snapshot_id,
                      "XFER T1 列表里有且只有这一条", net::StatusName(status));

  std::string downloaded;
  std::uint64_t declared_size = 0;
  std::string declared_sha;
  status = 0;
  test_support::Check(
      DownloadBuffer(&session, snapshot_id, &downloaded, &declared_size,
                     &declared_sha, &status, &error) &&
          status == static_cast<std::uint32_t>(net::Status::kOk) &&
          downloaded == data && declared_size == data.size() &&
          declared_sha == sha,
      "XFER T1 判别：下载内容与上传内容逐字节一致", net::StatusName(status));

  status = 0;
  std::string missing;
  std::uint64_t missing_size = 0;
  std::string missing_sha;
  DownloadBuffer(&session, std::string(32, 'f'), &missing, &missing_size,
                 &missing_sha, &status, &error);
  test_support::Check(status == static_cast<std::uint32_t>(net::Status::kNotFound),
                      "XFER T1 不存在的 snapshot id = NOT_FOUND",
                      net::StatusName(status));

  status = 0;
  DeleteSnapshot(&session, snapshot_id, &status, &error);
  test_support::Check(status == static_cast<std::uint32_t>(net::Status::kOk),
                      "XFER T1 删除成功", net::StatusName(status));
  test_support::Check(!test_support::Exists(blob_path),
                      "XFER T1 判别：删除之后磁盘上的 blob 消失");
  test_support::Check(CountEntries(user_directory + "/trash") == 0,
                      "XFER T1 判别：删除之后 trash 里没有残留");
  ids.clear();
  status = 0;
  ListSnapshots(&session, nullptr, &ids, &status, &error);
  test_support::Check(ids.empty(), "XFER T1 判别：删除之后列表为空");
  status = 0;
  DeleteSnapshot(&session, snapshot_id, &status, &error);
  test_support::Check(status == static_cast<std::uint32_t>(net::Status::kNotFound),
                      "XFER T1 再删一次 = NOT_FOUND");

  test_support::Section("XFER 2. 块边界：1 / chunk-1 / chunk / chunk+1 / 5 MiB");
  {
    const std::size_t chunk = net::kTransferChunkBytes;
    const std::size_t sizes[] = {1, chunk - 1, chunk, chunk + 1,
                                 5u * 1024u * 1024u};
    bool all_ok = true;
    std::string details;
    for (const std::size_t size : sizes) {
      const std::string payload = RandomHex(size / 2 + (size % 2));
      const std::string body = payload.substr(0, size);
      std::string id;
      std::uint32_t upload_status = 0;
      std::string upload_note;
      const bool sent = UploadBuffer(&session, "size.bak", body, &id,
                                     &upload_status, &upload_note);
      std::string back;
      std::uint64_t back_size = 0;
      std::string back_sha;
      std::uint32_t download_status = 0;
      const bool got = DownloadBuffer(&session, id, &back, &back_size,
                                      &back_sha, &download_status, &error);
      if (!sent || !got ||
          upload_status != static_cast<std::uint32_t>(net::Status::kOk) ||
          download_status != static_cast<std::uint32_t>(net::Status::kOk) ||
          back != body || back_size != body.size() ||
          back_sha != Sha256Of(body)) {
        all_ok = false;
        details += std::to_string(size) + " ";
      }
      DeleteSnapshot(&session, id, &status, &error);
    }
    test_support::Check(all_ok,
                        "XFER T2 判别：五种长度都往返一致（失败长度: " + details +
                            "）");
  }

  test_support::Section("XFER 3. 状态机：没有 BEGIN 就没有 CHUNK / END");
  {
    net::FrameHeader header;
    std::string response;
    status = 0;
    Request(&session, net::Opcode::kUploadChunk, "data", &header, &response,
            &error);
    test_support::Check(
        header.status == static_cast<std::uint32_t>(net::Status::kInvalidState),
        "XFER T3 没有 UPLOAD_BEGIN 就发 CHUNK = INVALID_STATE",
        net::StatusName(header.status));
    status = 0;
    Request(&session, net::Opcode::kUploadEnd, std::string(), &header,
            &response, &error);
    test_support::Check(
        header.status == static_cast<std::uint32_t>(net::Status::kInvalidState),
        "XFER T3 没有 UPLOAD_BEGIN 就发 END = INVALID_STATE");
    Request(&session, net::Opcode::kDownloadChunk, std::string(), &header,
            &response, &error);
    test_support::Check(
        header.status == static_cast<std::uint32_t>(net::Status::kInvalidState),
        "XFER T3 没有 DOWNLOAD_BEGIN 就发 DOWNLOAD_CHUNK = INVALID_STATE");
    Request(&session, net::Opcode::kDownloadEnd, std::string(), &header,
            &response, &error);
    test_support::Check(
        header.status == static_cast<std::uint32_t>(net::Status::kInvalidState),
        "XFER T3 没有 DOWNLOAD_BEGIN 就发 DOWNLOAD_END = INVALID_STATE");
  }

  test_support::Section("XFER 4. UPLOAD_BEGIN 的元数据校验");
  {
    net::FrameHeader header;
    std::string response;
    Request(&session, net::Opcode::kUploadBegin,
            UploadBeginPayload("zero.bak", 0, std::string(64, 'a')), &header,
            &response, &error);
    test_support::Check(
        header.status ==
            static_cast<std::uint32_t>(net::Status::kInvalidRequest),
        "XFER T4 声明 0 字节 = INVALID_REQUEST（0 字节不是合法归档）");
    Request(&session, net::Opcode::kUploadBegin,
            UploadBeginPayload("bad\nname.bak", 10, std::string(64, 'a')),
            &header, &response, &error);
    test_support::Check(
        header.status ==
            static_cast<std::uint32_t>(net::Status::kInvalidRequest),
        "XFER T4 含控制字符的显示名 = INVALID_REQUEST");
    Request(&session, net::Opcode::kUploadBegin,
            UploadBeginPayload("huge.bak", 9ull * 1024 * 1024 * 1024,
                               std::string(64, 'a')),
            &header, &response, &error);
    test_support::Check(
        header.status == static_cast<std::uint32_t>(net::Status::kTooLarge),
        "XFER T4 超过上传上限 = TOO_LARGE（上限检查在任何分配之前）");
    Request(&session, net::Opcode::kUploadBegin,
            UploadBeginPayload("badsha.bak", 10, std::string(63, 'a')), &header,
            &response, &error);
    test_support::Check(
        header.status ==
            static_cast<std::uint32_t>(net::Status::kInvalidRequest),
        "XFER T4 摘要文本长度不对 = INVALID_REQUEST");
  }

  test_support::Section("XFER 5. 失败路径：磁盘上不能留下任何东西");
  {
    const std::string body = RandomHex(256);  // 512 字节
    const std::string good_sha = Sha256Of(body);
    net::FrameHeader header;
    std::string response;

    // 5a. 声明比实际小：最后一个块越界 -> TOO_LARGE + 上传被中止
    Request(&session, net::Opcode::kUploadBegin,
            UploadBeginPayload("small.bak", body.size() - 1, good_sha), &header,
            &response, &error);
    std::uint32_t upload_status = header.status;
    Request(&session, net::Opcode::kUploadChunk, body, &header, &response,
            &error);
    test_support::Check(
        upload_status == static_cast<std::uint32_t>(net::Status::kOk) &&
            header.status == static_cast<std::uint32_t>(net::Status::kTooLarge),
        "XFER T5 声明长度小于实际 = TOO_LARGE");
    test_support::Check(CountEntries(user_directory + "/tmp") == 0,
                        "XFER T5 判别：越界之后临时文件被删掉");
    test_support::Check(CountBlobs(user_directory) == 0,
                        "XFER T5 判别：用户目录里没有已发布的 blob");

    // 5b. 声明比实际大：END 时长度不符 -> INTEGRITY_MISMATCH
    Request(&session, net::Opcode::kUploadBegin,
            UploadBeginPayload("big.bak", body.size() + 10, good_sha), &header,
            &response, &error);
    Request(&session, net::Opcode::kUploadChunk, body, &header, &response,
            &error);
    Request(&session, net::Opcode::kUploadEnd, std::string(), &header,
            &response, &error);
    test_support::Check(header.status == static_cast<std::uint32_t>(
                                            net::Status::kIntegrityMismatch),
                        "XFER T5 声明长度大于实际 = INTEGRITY_MISMATCH");
    test_support::Check(CountEntries(user_directory + "/tmp") == 0,
                        "XFER T5 判别：长度不符之后临时文件被删掉");

    // 5c. 摘要不符：长度对、内容对不上声明
    const std::string wrong_sha = Sha256Of(body + "x");
    Request(&session, net::Opcode::kUploadBegin,
            UploadBeginPayload("hash.bak", body.size(), wrong_sha), &header,
            &response, &error);
    Request(&session, net::Opcode::kUploadChunk, body, &header, &response,
            &error);
    Request(&session, net::Opcode::kUploadEnd, std::string(), &header,
            &response, &error);
    test_support::Check(header.status == static_cast<std::uint32_t>(
                                            net::Status::kIntegrityMismatch),
                        "XFER T5 摘要不符 = INTEGRITY_MISMATCH");
    test_support::Check(CountEntries(user_directory + "/tmp") == 0 &&
                            CountBlobs(user_directory) == 0,
                        "XFER T5 判别：摘要不符之后没有发布任何 blob");

    // 5d. 空块被拒绝，但上传仍然可以正常继续
    Request(&session, net::Opcode::kUploadBegin,
            UploadBeginPayload("empty-chunk.bak", body.size(), good_sha),
            &header, &response, &error);
    Request(&session, net::Opcode::kUploadChunk, std::string(), &header,
            &response, &error);
    test_support::Check(
        header.status ==
            static_cast<std::uint32_t>(net::Status::kInvalidRequest),
        "XFER T5 空块 = INVALID_REQUEST");
    Request(&session, net::Opcode::kUploadChunk, body, &header, &response,
            &error);
    Request(&session, net::Opcode::kUploadEnd, std::string(), &header,
            &response, &error);
    test_support::Check(header.status ==
                            static_cast<std::uint32_t>(net::Status::kOk),
                        "XFER T5 判别：被拒绝的帧没有打断正在进行的上传");
    net::PayloadReader reader(response);
    std::string id;
    std::string returned_sha;
    std::uint64_t size = 0;
    std::uint64_t created_at = 0;
    reader.ReadString(net::kMaxSnapshotIdBytes, &id);
    reader.ReadString(net::kSha256HexBytes, &returned_sha);
    reader.ReadU64(&size);
    reader.ReadU64(&created_at);
    DeleteSnapshot(&session, id, &status, &error);

    // 5e. 超过块上限的帧（256 KiB < 300 KiB <= 1 MiB）-> TOO_LARGE。
    // 这一帧只是被拒绝：它里面的字节一个都没写进去，上传还能继续。
    const std::string big_body(300 * 1024, 'x');
    Request(&session, net::Opcode::kUploadBegin,
            UploadBeginPayload("bigchunk.bak", big_body.size(),
                               Sha256Of(big_body)),
            &header, &response, &error);
    Request(&session, net::Opcode::kUploadChunk,
            std::string(300 * 1024, 'x'), &header, &response, &error);
    test_support::Check(header.status == static_cast<std::uint32_t>(
                                            net::Status::kTooLarge),
                        "XFER T5 单个块超过 256 KiB = TOO_LARGE");
    Request(&session, net::Opcode::kUploadChunk,
            big_body.substr(0, net::kTransferChunkBytes), &header, &response,
            &error);
    Request(&session, net::Opcode::kUploadChunk,
            big_body.substr(net::kTransferChunkBytes), &header, &response,
            &error);
    Request(&session, net::Opcode::kUploadEnd, std::string(), &header,
            &response, &error);
    test_support::Check(header.status ==
                            static_cast<std::uint32_t>(net::Status::kOk),
                        "XFER T5 判别：被拒绝的超大块没有污染上传内容");
    net::PayloadReader big_reader(response);
    std::string big_id;
    std::string big_sha;
    std::uint64_t big_size = 0;
    std::uint64_t big_created = 0;
    big_reader.ReadString(net::kMaxSnapshotIdBytes, &big_id);
    big_reader.ReadString(net::kSha256HexBytes, &big_sha);
    big_reader.ReadU64(&big_size);
    big_reader.ReadU64(&big_created);
    std::uint16_t big_kind = 0;
    std::uint64_t big_generation = 0;
    std::string big_parent;
    big_reader.ReadU16(&big_kind);
    big_reader.ReadU64(&big_generation);
    big_reader.ReadString(net::kMaxSnapshotIdBytes, &big_parent);
    test_support::Check(big_sha == Sha256Of(big_body) &&
                            big_size == big_body.size(),
                        "XFER T5 判别：最终 blob 就是完整的 300 KiB");
    DeleteSnapshot(&session, big_id, &status, &error);

    // 5f. 注入磁盘写失败
    server.FailNextBlobWriteForTesting();
    Request(&session, net::Opcode::kUploadBegin,
            UploadBeginPayload("writefail.bak", body.size(), good_sha), &header,
            &response, &error);
    Request(&session, net::Opcode::kUploadChunk, body, &header, &response,
            &error);
    test_support::Check(
        header.status ==
            static_cast<std::uint32_t>(net::Status::kInternalError),
        "XFER T5 注入的写盘失败 = INTERNAL_ERROR");
    test_support::Check(CountEntries(user_directory + "/tmp") == 0 &&
                            CountBlobs(user_directory) == 0,
                        "XFER T5 判别：写盘失败之后没有留下任何文件");

    // 5g. 注入元数据插入失败：blob 已经 rename 出去，必须被回滚
    server.FailNextMetadataInsertForTesting();
    std::string injected_id;
    std::uint32_t injected_status = 0;
    std::string injected_note;
    UploadBuffer(&session, "rollback.bak", body, &injected_id,
                 &injected_status, &injected_note);
    test_support::Check(
        injected_status == static_cast<std::uint32_t>(net::Status::kInternalError),
        "XFER T5 注入的元数据写入失败 = INTERNAL_ERROR",
        net::StatusName(injected_status));
    test_support::Check(CountBlobs(user_directory) == 0 &&
                            CountEntries(user_directory + "/tmp") == 0,
                        "XFER T5 判别：元数据失败之后已发布的 blob 被回滚");
    ids.clear();
    status = 0;
    ListSnapshots(&session, nullptr, &ids, &status, &error);
    test_support::Check(ids.empty(),
                        "XFER T5 判别：失败的上传没有留下任何元数据记录");
  }

  test_support::Section("XFER 6. 客户端半路断开：服务端必须清掉临时文件");
  {
    // 换一条连接（上一条仍然服务于主用例）。
    const std::string body = RandomHex(400);  // 800 字节
    std::thread second_worker = ServeOneConnection(&server);
    Session second;
    std::string second_error;
    test_support::Check(
        OpenSession(&server, username, password, fixture.pin, &second,
                    &second_error),
        "XFER T6 第二条连接登录", second_error);
    net::FrameHeader header;
    std::string response;
    Request(&second, net::Opcode::kUploadBegin,
            UploadBeginPayload("drop.bak", body.size(), Sha256Of(body)),
            &header, &response, &error);
    test_support::Check(header.status ==
                            static_cast<std::uint32_t>(net::Status::kOk),
                        "XFER T6 第二条连接开始上传");
    Request(&second, net::Opcode::kUploadChunk, body.substr(0, 100), &header,
            &response, &error);
    test_support::Check(CountEntries(user_directory + "/tmp") == 1,
                        "XFER T6 判别：上传中确实存在一个临时文件");
    // 不发 UPLOAD_END，直接断开。
    ::close(second.fd);
    test_support::Check(
        WaitForEmptyDirectory(user_directory + "/tmp", 100),
        "XFER T6 判别：断开之后服务端删掉了临时文件");
    test_support::Check(CountBlobs(user_directory) == 0,
                        "XFER T6 判别：断开没有发布任何 blob");
    second_worker.join();
  }

  test_support::Section("XFER 7. 服务端文件被改：客户端能发现（长度不变）");
  {
    const std::string body = RandomHex(300);
    std::string id;
    std::uint32_t upload_status = 0;
    std::string upload_note;
    UploadBuffer(&session, "tamper.bak", body, &id, &upload_status,
                 &upload_note);
    const std::string blob = user_directory + "/" + id + ".bak";
    test_support::Check(upload_status ==
                            static_cast<std::uint32_t>(net::Status::kOk) &&
                            test_support::WriteFile(blob, std::string(body.size(), 'Z'),
                                                    0600),
                        "XFER T7 夹具：把磁盘上的 blob 换成同长度的另一份内容");
    std::string back;
    std::uint64_t declared_size = 0;
    std::string declared_sha;
    std::uint32_t download_status = 0;
    DownloadBuffer(&session, id, &back, &declared_size, &declared_sha,
                   &download_status, &error);
    test_support::Check(
        download_status == static_cast<std::uint32_t>(net::Status::kOk) &&
            declared_size == body.size() && Sha256Of(back) != declared_sha,
        "XFER T7 判别：字节被改过，客户端按声明的 SHA-256 一定能发现"
        "（服务端只校验长度，完整性由客户端判定）");
    DeleteSnapshot(&session, id, &status, &error);
  }

  ::close(session.fd);
  worker.join();
  server.Stop();

  test_support::Section("XFER 8. 32 MiB 流式：RSS 不随文件大小线性增长");
  {
    Fixture big;
    test_support::Check(SetupFixture(&big, "xfer-big"), "XFER T8 夹具就绪");
    net::RemoteServer big_server;
    std::string big_error;
    test_support::Check(big_server.Configure(big.config, &big_error) &&
                            big_server.Start(&big_error),
                        "XFER T8 服务端启动", big_error);
    std::thread big_worker = ServeOneConnection(&big_server);
    Session big_session;
    std::string big_session_error;
    const std::string big_user = "big-" + RandomHex(4);
    const std::string big_password = RandomHex(16);
    test_support::Check(OpenSession(&big_server, big_user, big_password,
                                    big.pin, &big_session, &big_session_error),
                        "XFER T8 注册并登录", big_session_error);

    const std::uint64_t rss_before = PeakRssKb();
    PatternGenerator generator;
    std::string big_id;
    std::uint32_t upload_status = 0;
    std::string upload_note;
    const bool sent =
        UploadStream(&big_session, "big.bak", kBigTransferBytes, &generator,
                     &big_id, &upload_status, &upload_note);
    const std::uint64_t rss_after_upload = PeakRssKb();
    test_support::Check(sent && upload_status ==
                                     static_cast<std::uint32_t>(
                                         net::Status::kOk),
                        "XFER T8 32 MiB 上传成功", net::StatusName(upload_status));

    std::uint64_t received = 0;
    std::string download_sha;
    std::uint32_t download_status = 0;
    const bool got = DownloadAndHash(&big_session, big_id, &received,
                                     &download_sha, &download_status,
                                     &big_error);
    const std::uint64_t rss_after_download = PeakRssKb();
    test_support::Check(got && download_status ==
                                    static_cast<std::uint32_t>(
                                        net::Status::kOk) &&
                            received == kBigTransferBytes,
                        "XFER T8 32 MiB 下载成功且长度一致",
                        net::StatusName(download_status));
    // upload_note 就是 "服务端回的摘要|长度"，直接比文本即可
    // （不能对它再做一次 SHA-256，那是摘要的摘要）。
    test_support::Check(
        download_sha == upload_note.substr(0, upload_note.find('|')),
        "XFER T8 判别：32 MiB 往返之后 SHA-256 一致");

    const std::uint64_t growth = rss_after_download - rss_before;
    test_support::Note("32 MiB 传输：VmHWM " + std::to_string(rss_before) +
                       " KB -> " + std::to_string(rss_after_upload) +
                       " KB -> " + std::to_string(rss_after_download) +
                       " KB（增长 " + std::to_string(growth) + " KB）");
#if XFER_SANITIZED_BUILD
    // 消毒剂构建：传输与校验照跑（上面已经断言完），只是不拿生产阈值判定
    // 消毒剂进程的内存水位。把三行写清楚，免得看日志的人以为这条断言被删了。
    std::printf("RSS_BOUND: non-sanitized = N/A (this build is sanitized)\n");
    std::printf("ASAN: transfer executed\n");
    std::printf("ASAN: RSS threshold = N/A under sanitizer\n");
#else
    std::printf("RSS_BOUND: non-sanitized = %s\n",
                growth < 16 * 1024 ? "PASS" : "FAIL");
    test_support::Check(growth < 16 * 1024,
                        "XFER T8 判别：32 MiB 文件没有进内存（峰值增长 < 16 MiB）");
#endif

    const std::string big_blob = big.root + "/users/1/" + big_id + ".bak";
    std::ifstream blob(big_blob.c_str(), std::ios::binary | std::ios::ate);
    std::streamoff blob_size = -1;
    if (blob) {
      blob_size = static_cast<std::streamoff>(blob.tellg());
    }
    test_support::Check(blob_size == static_cast<std::streamoff>(kBigTransferBytes),
                        "XFER T8 判别：磁盘上的 blob 就是 32 MiB");
    DeleteSnapshot(&big_session, big_id, &status, &big_error);
    ::close(big_session.fd);
    big_worker.join();
    big_server.Stop();
  }

  test_support::Section("XFER 9. 跨用户隔离（socket 层）");
  {
    Fixture shared;
    test_support::Check(SetupFixture(&shared, "xfer-users"), "XFER T9 夹具就绪");
    net::RemoteServer shared_server;
    std::string shared_error;
    test_support::Check(shared_server.Configure(shared.config, &shared_error) &&
                            shared_server.Start(&shared_error),
                        "XFER T9 服务端启动", shared_error);
    std::thread alice_worker = ServeOneConnection(&shared_server);
    const std::string alice_password = RandomHex(16);
    Session alice;
    std::string alice_error;
    test_support::Check(OpenSession(&shared_server, "alice-01", alice_password,
                                    shared.pin, &alice, &alice_error),
                        "XFER T9 用户 A 注册并登录", alice_error);
    std::thread bob_worker = ServeOneConnection(&shared_server);
    const std::string bob_password = RandomHex(16);
    Session bob;
    std::string bob_error;
    test_support::Check(OpenSession(&shared_server, "bob-01", bob_password,
                                    shared.pin, &bob, &bob_error),
                        "XFER T9 用户 B 注册并登录", bob_error);

    const std::string body = RandomHex(200);
    std::string alice_id;
    std::uint32_t upload_status = 0;
    std::string upload_note;
    UploadBuffer(&alice, "alice.bak", body, &alice_id, &upload_status,
                 &upload_note);
    test_support::Check(upload_status ==
                            static_cast<std::uint32_t>(net::Status::kOk),
                        "XFER T9 A 上传成功");

    std::vector<std::string> bob_ids;
    status = 0;
    ListSnapshots(&bob, nullptr, &bob_ids, &status, &shared_error);
    test_support::Check(bob_ids.empty(),
                        "XFER T9 判别：B 的列表看不到 A 的快照");

    std::string back;
    std::uint64_t declared_size = 0;
    std::string declared_sha;
    status = 0;
    DownloadBuffer(&bob, alice_id, &back, &declared_size, &declared_sha, &status,
                   &shared_error);
    test_support::Check(status == static_cast<std::uint32_t>(net::Status::kNotFound),
                        "XFER T9 判别：B 下载 A 的快照 = NOT_FOUND",
                        net::StatusName(status));
    status = 0;
    DeleteSnapshot(&bob, alice_id, &status, &shared_error);
    test_support::Check(status == static_cast<std::uint32_t>(net::Status::kNotFound),
                        "XFER T9 判别：B 删除 A 的快照 = NOT_FOUND");

    std::vector<std::string> alice_ids;
    status = 0;
    ListSnapshots(&alice, nullptr, &alice_ids, &status, &shared_error);
    test_support::Check(alice_ids.size() == 1 && alice_ids[0] == alice_id,
                        "XFER T9 判别：越权尝试之后 A 的快照仍在");
    status = 0;
    DownloadBuffer(&alice, alice_id, &back, &declared_size, &declared_sha,
                   &status, &shared_error);
    test_support::Check(status == static_cast<std::uint32_t>(net::Status::kOk) &&
                            back == body,
                        "XFER T9 判别：A 自己下载成功且内容一致");
    const std::string alice_blob = shared.root + "/users/1/" + alice_id + ".bak";
    test_support::Check(test_support::Exists(alice_blob),
                        "XFER T9 判别：B 的越权删除没有动到 A 的文件");

    ::close(alice.fd);
    ::close(bob.fd);
    alice_worker.join();
    bob_worker.join();
    shared_server.Stop();
  }

  return test_support::Finish("remote_transfer_test");
}
