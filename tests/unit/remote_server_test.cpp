// tests/unit/remote_server_test.cpp
//
// PR #20：backup-server 的监听、帧循环与连接状态机测试。
//
// 这里是**真实 TCP 环回**：真的 bind、真的 accept、真的收发帧，
// 不是把 handler 直接调一遍。覆盖四类东西：
//
//   1. 启动参数与 secret 文件的失败路径；
//   2. PING 往返；
//   3. 未认证 / 未实现操作码的如实回答；
//   4. 帧损坏（假 magic、超大长度）必须断开连接，而不是继续解析。

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <thread>

#include "crypto.h"
#include "network_protocol.h"
#include "remote_auth.h"
#include "remote_server.h"
#include "remote_test_support.h"
#include "test_support.h"

namespace bp = backupproject;
namespace net = backupproject::net;

namespace {

constexpr int kClientTimeoutSeconds = 10;

std::string RandomSecretHex() {
  std::string raw;
  std::string error;
  if (!bp::crypto::RandomBytes(32, &raw, &error)) {
    return std::string();
  }
  return bp::crypto::ToHex(
      reinterpret_cast<const unsigned char*>(raw.data()), raw.size());
}

struct Fixture {
  net::RemoteServerConfig config;
  std::string root;
  std::string database;
  std::string secret_file;
  // PR #21：BPSEC1 的服务端长期身份私钥，以及客户端握手要用的 pin 文本
  // （"sha256:<指纹>"）。两者来自同一次生成，所以 pin 一定对得上。
  std::string transport_key_file;
  std::string pin;
};

// 隔离的临时目录：root / db / secret 都在里面。
bool SetupFixture(Fixture* fixture, const std::string& name) {
  const std::string base = test_support::FreshDir(name);
  if (base.empty()) {
    return false;
  }
  fixture->root = base + "/data";
  fixture->database = base + "/state/metadata.sqlite3";
  fixture->secret_file = base + "/secrets.env";
  const std::string secret = RandomSecretHex();
  if (secret.empty()) {
    return false;
  }
  // 每次运行换一个随机 secret：测试里不存在"固定密钥"。
  if (!test_support::WriteFile(fixture->secret_file,
                               "BACKUP_TOKEN_SECRET=" + secret + "\n", 0600)) {
    return false;
  }
  // BPSEC1 身份密钥：每个 fixture 一套新的，同时拿到 identity 与 pin 文本。
  // 服务端在 ServeConnection 的第一步就要用它，缺了它 Start() 直接失败。
  fixture->transport_key_file = base + "/transport.key";
  net::TransportIdentity identity;
  std::string identity_error;
  if (!remote_test_support::PrepareTransportIdentity(
          fixture->transport_key_file, &identity, &fixture->pin,
          &identity_error)) {
    return false;
  }
  fixture->config.bind_address = "127.0.0.1";
  fixture->config.port = 0;  // 内核分配端口，测试之间不抢
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

// 后台 accept 一个连接并服务它，直到对端关闭。
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

// 对端关闭返回 true；超时等其它情况返回 false。
bool PeerClosed(int fd) {
  char buffer[128];
  const ssize_t got = ::recv(fd, buffer, sizeof(buffer), 0);
  return got == 0;
}

// 客户端侧 BPSEC1 握手。
//
// 失败时必须显式 shutdown(SHUT_RDWR)：服务端这时还等在 ServeConnection 的
// 握手读上，不把这条连接关掉，worker.join() 会永远卡住。
bool HandshakeClientOrShutdown(int fd, const std::string& pin,
                               net::SecureChannel* channel,
                               std::string* error) {
  if (remote_test_support::HandshakeTestClient(fd, pin, channel, error)) {
    return true;
  }
  ::shutdown(fd, SHUT_RDWR);
  return false;
}

// 一次请求 / 响应往返。语义与 PR #20 的 net::SendFrame + net::ReceiveFrame
// 完全相同，只是帧现在整个走在 BPSEC1 加密记录里。
net::FrameReadStatus RoundTrip(net::SecureChannel* channel, int fd,
                               std::uint16_t opcode, std::uint64_t request_id,
                               const std::string& payload,
                               net::FrameHeader* header,
                               std::string* response, std::string* error) {
  if (!remote_test_support::SendTestFrame(channel, fd, opcode, 0, request_id,
                                          payload, error)) {
    return net::FrameReadStatus::kIoError;
  }
  return remote_test_support::ReceiveTestFrame(channel, fd, header, response,
                                               error);
}

std::string CredentialsPayload(const std::string& username,
                                const std::string& password) {
  net::PayloadBuilder builder;
  std::string error;
  builder.AppendString(username, net::kMaxUsernameBytes, &error);
  builder.AppendString(password, net::kMaxPasswordBytes, &error);
  return builder.data();
}

}  // namespace

int main() {
  test_support::Section("SRV 1. 启动参数校验");
  {
    net::RemoteServer server;
    std::string error;
    net::RemoteServerConfig config;
    config.root_directory = "/tmp/x";
    config.database_path = "/tmp/y.sqlite3";
    config.secret_file_path = "/tmp/z.env";
    config.bind_address = "";
    test_support::Check(!server.Configure(config, &error),
                        "SRV T1 bind 为空被拒绝");
    config.bind_address = "0.0.0.0.example";
    error.clear();
    test_support::Check(!server.Configure(config, &error),
                        "SRV T1 非 IPv4 字面量被拒绝", error);
    // 本版本没有原生 TLS：任何非环回地址都必须 fail closed。0.0.0.0 是"监听
    // 所有网卡"，私网 / 公网地址同样会把明文协议暴露在共享网络上，127.0.0.2
    // 也不接受——规则是**相等**判断，不是"127/8 都行"。
    for (const char* address :
         {"0.0.0.0", "127.0.0.2", "192.168.1.10", "8.8.8.8", "10.1.2.3"}) {
      config.bind_address = address;
      error.clear();
      test_support::Check(!server.Configure(config, &error),
                          std::string("SRV T1 非环回地址 ") + address +
                              " 被拒绝");
      test_support::Check(error.find("127.0.0.1") != std::string::npos,
                          std::string("SRV T1 判别：") + address +
                              " 的拒绝理由里点名了 127.0.0.1");
    }
    // IPv6 字面量同样不接受：这一版只解析 IPv4。
    config.bind_address = "::1";
    error.clear();
    test_support::Check(!server.Configure(config, &error),
                        "SRV T1 IPv6 字面量被拒绝");
    config.bind_address = "127.0.0.1";
    config.root_directory = "";
    error.clear();
    test_support::Check(!server.Configure(config, &error),
                        "SRV T1 root 为空被拒绝");
    config.root_directory = "/tmp/x";
    config.worker_count = 0;
    error.clear();
    test_support::Check(!server.Configure(config, &error),
                        "SRV T1 workers=0 被拒绝");
    config.worker_count = 65;
    error.clear();
    test_support::Check(!server.Configure(config, &error),
                        "SRV T1 workers=65 被拒绝");
    config.worker_count = 4;
    config.io_timeout_seconds = 0;
    error.clear();
    test_support::Check(!server.Configure(config, &error),
                        "SRV T1 io-timeout=0 被拒绝");
    config.io_timeout_seconds = 30;
    // PR #21：BPSEC1 的服务端身份密钥是**必填项**。空值必须被拒绝——产品里
    // 没有"不配密钥就退回明文 BPNET1"的分支，所以这里也不能有默认值。
    config.transport_key_file_path.clear();
    error.clear();
    test_support::Check(!server.Configure(config, &error),
                        "SRV T1 transport-key-file 为空被拒绝");
    test_support::Check(error.find("transport-key-file") != std::string::npos,
                        "SRV T1 判别：拒绝理由里点名了 --transport-key-file",
                        error);
    config.transport_key_file_path = "/tmp/transport.key";
    error.clear();
    test_support::Check(server.Configure(config, &error),
                        "SRV T1 合法配置被接受", error);
  }

  test_support::Section("SRV 2. secret 文件：缺失 / 缺键 / 过短都拒绝启动");
  {
    Fixture fixture;
    test_support::Check(SetupFixture(&fixture, "srv-secret"), "SRV T2 夹具就绪");
    {
      net::RemoteServer server;
      net::RemoteServerConfig config = fixture.config;
      config.secret_file_path = fixture.root + "/missing.env";
      std::string error;
      const bool configured = server.Configure(config, &error);
      test_support::Check(configured && !server.Start(&error) &&
                              !server.running(),
                          "SRV T2 secret 文件不存在时拒绝启动", error);
    }
    {
      test_support::WriteFile(fixture.secret_file, "OTHER_KEY=1\n", 0600);
      net::RemoteServer server;
      std::string error;
      const bool configured = server.Configure(fixture.config, &error);
      test_support::Check(configured && !server.Start(&error) &&
                              !server.running(),
                          "SRV T2 缺 BACKUP_TOKEN_SECRET 时拒绝启动", error);
    }
    {
      test_support::WriteFile(fixture.secret_file,
                              "BACKUP_TOKEN_SECRET=short\n", 0600);
      net::RemoteServer server;
      std::string error;
      const bool configured = server.Configure(fixture.config, &error);
      test_support::Check(configured && !server.Start(&error) &&
                              !server.running(),
                          "SRV T2 secret 过短时拒绝启动", error);
    }
  }

  test_support::Section("SRV 2b. secret 文件：权限 / 类型 / 符号链接都 fail closed");
  {
    Fixture fixture;
    test_support::Check(SetupFixture(&fixture, "srv-secret-mode"),
                        "SRV T2b 夹具就绪");
    std::string secret_text;
    test_support::ReadFile(fixture.secret_file, &secret_text);
    const std::string key = "BACKUP_TOKEN_SECRET=";
    const std::size_t at = secret_text.find(key);
    std::string secret_value =
        at == std::string::npos ? std::string() : secret_text.substr(at + key.size());
    while (!secret_value.empty() && (secret_value.back() == '\n' ||
                                     secret_value.back() == '\r')) {
      secret_value.pop_back();
    }
    test_support::Check(!secret_value.empty(),
                        "SRV T2b 夹具：夹具 secret 非空（用来做泄漏判别）");

    // 起一次就停：Start() 成功即证明这份 secret 文件被接受。
    const auto starts_with_secret = [&fixture](std::string* error) {
      net::RemoteServer server;
      if (!server.Configure(fixture.config, error)) {
        return false;
      }
      if (!server.Start(error)) {
        return false;
      }
      server.Stop();
      return true;
    };

    // 每一轮都要先恢复可写：上一轮可能把文件留成 0400（owner 只读），
    // 那样下一次 O_WRONLY 打开会直接 EACCES，测的就不是"服务端拒绝了"。
    const auto rewrite_secret = [&fixture, &secret_text](std::uint32_t mode) {
      (void)::chmod(fixture.secret_file.c_str(), 0600);
      return test_support::WriteFile(fixture.secret_file, secret_text, mode);
    };

    std::string error;
    test_support::Check(rewrite_secret(0600) && starts_with_secret(&error),
                        "SRV T2b 0600 的 secret 文件被接受", error);
    error.clear();
    test_support::Check(rewrite_secret(0400) && starts_with_secret(&error),
                        "SRV T2b 0400（只有 owner 能读）也被接受", error);
    for (const std::uint32_t mode : {0644u, 0660u, 0640u, 0604u, 0666u}) {
      test_support::Check(rewrite_secret(mode),
                          "SRV T2b 夹具：secret 文件已改成 " +
                              test_support::Octal(mode));
      error.clear();
      const bool started = starts_with_secret(&error);
      test_support::Check(!started,
                          "SRV T2b 模式 " + test_support::Octal(mode) +
                              " 的 secret 文件被拒绝");
      test_support::Check(error.find(secret_value) == std::string::npos,
                          "SRV T2b 判别：拒绝信息里没有 secret 的值", error);
    }

    // 符号链接：拒绝（O_NOFOLLOW），哪怕它指向一个 0600 的正规文件。
    test_support::Check(rewrite_secret(0600),
                        "SRV T2b 夹具：secret 文件恢复成 0600");
    const std::string link_path = fixture.secret_file + ".link";
    test_support::Check(
        test_support::CreateSymlink(fixture.secret_file, link_path),
        "SRV T2b 夹具：指向 secret 的符号链接已创建");
    {
      net::RemoteServer server;
      net::RemoteServerConfig config = fixture.config;
      config.secret_file_path = link_path;
      error.clear();
      test_support::Check(!server.Configure(config, &error) ||
                              !server.Start(&error),
                          "SRV T2b 指向 secret 的符号链接被拒绝", error);
    }
    // 目录：拒绝（不是普通文件）。
    {
      net::RemoteServer server;
      net::RemoteServerConfig config = fixture.config;
      config.secret_file_path = fixture.root;
      error.clear();
      test_support::Check(!server.Configure(config, &error) ||
                              !server.Start(&error),
                          "SRV T2b 目录当 secret 文件被拒绝", error);
    }
    // 合法文件重新被接受：证明前面的拒绝都来自"那一份坏文件"，不是环境坏了。
    error.clear();
    test_support::Check(starts_with_secret(&error),
                        "SRV T2b 判别：换回 0600 正规文件之后又能启动", error);
  }

  test_support::Section("SRV 3. PING 往返（真实 TCP 环回）");
  {
    Fixture fixture;
    SetupFixture(&fixture, "srv-ping");
    net::RemoteServer server;
    std::string error;
    test_support::Check(server.Configure(fixture.config, &error) &&
                            server.Start(&error),
                        "SRV T3 服务端启动", error);
    test_support::Check(server.bound_port() != 0,
                        "SRV T3 内核分配的端口非 0");

    std::thread worker = ServeOneConnection(&server);
    const int client = ConnectToLoopback(server.bound_port());
    test_support::Check(client >= 0, "SRV T3 客户端连上监听端口");

    // 服务端在说第一句 BPNET1 之前必须先完成 BPSEC1 握手；客户端在**主线程**
    // 握手，服务端在 worker 线程里跑 ServeConnection，两边不会互相等死。
    net::SecureChannel channel;
    test_support::Check(
        HandshakeClientOrShutdown(client, fixture.pin, &channel, &error),
        "SRV T3 BPSEC1 握手完成", error);

    net::FrameHeader header;
    std::string response;
    const net::FrameReadStatus status =
        RoundTrip(&channel, client, static_cast<std::uint16_t>(net::Opcode::kPing),
                  7, std::string(), &header, &response, &error);
    test_support::Check(status == net::FrameReadStatus::kOk,
                        "SRV T3 PING 收到响应", error);
    test_support::Check(header.opcode ==
                                static_cast<std::uint16_t>(net::Opcode::kPing) &&
                            header.request_id == 7 &&
                            header.status ==
                                static_cast<std::uint32_t>(net::Status::kOk),
                        "SRV T3 判别：操作码 / request_id / status 回填正确");
    net::PayloadReader reader(response);
    std::string software;
    std::uint16_t version = 0;
    std::uint64_t server_time = 0;
    const bool decoded = reader.ReadString(net::kMaxDisplayNameBytes,
                                           &software) &&
                         reader.ReadU16(&version) &&
                         reader.ReadU64(&server_time) && reader.AtEnd();
    test_support::Check(decoded && software == net::kServerSoftwareName &&
                            version == net::kProtocolVersion && server_time > 0,
                        "SRV T3 判别：PING payload 是软件名 + 协议版本 + 时间",
                        reader.error_message());
    ::close(client);
    worker.join();
    server.Stop();
  }

  test_support::Section("SRV 4. 未认证 / 未实现的操作码如实回答");
  {
    Fixture fixture;
    SetupFixture(&fixture, "srv-ops");
    net::RemoteServer server;
    std::string error;
    server.Configure(fixture.config, &error);
    server.Start(&error);
    std::thread worker = ServeOneConnection(&server);
    const int client = ConnectToLoopback(server.bound_port());
    net::SecureChannel channel;
    test_support::Check(
        HandshakeClientOrShutdown(client, fixture.pin, &channel, &error),
        "SRV T4 BPSEC1 握手完成", error);

    net::FrameHeader header;
    std::string response;
    net::FrameReadStatus status = RoundTrip(
        &channel, client, static_cast<std::uint16_t>(net::Opcode::kList), 11,
        std::string(), &header, &response, &error);
    test_support::Check(status == net::FrameReadStatus::kOk &&
                            header.status == static_cast<std::uint32_t>(
                                                net::Status::kUnauthorized),
                        "SRV T4 没登录的 LIST = UNAUTHORIZED",
                        net::StatusName(header.status));

    error.clear();
    net::PayloadBuilder credentials;
    credentials.AppendString("night-user", net::kMaxUsernameBytes, &error);
    credentials.AppendString("secret-password", net::kMaxPasswordBytes, &error);
    status = RoundTrip(&channel, client, static_cast<std::uint16_t>(net::Opcode::kLogin),
                       12, credentials.data(), &header, &response, &error);
    test_support::Check(status == net::FrameReadStatus::kOk &&
                            header.status == static_cast<std::uint32_t>(
                                                net::Status::kUnauthorized),
                        "SRV T4 不存在的账号 LOGIN = UNAUTHORIZED",
                        net::StatusName(header.status));

    error.clear();
    // 传输类操作码在第三个 commit 之前**如实**回答"不支持"。
    status = RoundTrip(&channel, client,
                       static_cast<std::uint16_t>(net::Opcode::kUploadBegin),
                       17, std::string(), &header, &response, &error);
    test_support::Check(status == net::FrameReadStatus::kOk &&
                            header.status == static_cast<std::uint32_t>(
                                                net::Status::kUnauthorized),
                        "SRV T4 没登录的 UPLOAD_BEGIN = UNAUTHORIZED",
                        net::StatusName(header.status));

    error.clear();
    status = RoundTrip(&channel, client, 9999, 13, std::string(), &header, &response,
                       &error);
    test_support::Check(status == net::FrameReadStatus::kOk &&
                            header.opcode == static_cast<std::uint16_t>(
                                                net::Opcode::kError) &&
                            header.status == static_cast<std::uint32_t>(
                                                net::Status::kInvalidRequest),
                        "SRV T4 未知操作码回 INVALID_REQUEST", error);

    error.clear();
    // 请求帧里的 status 必须是 0。
    remote_test_support::SendTestFrame(
        &channel, client, static_cast<std::uint16_t>(net::Opcode::kPing), 5, 14,
        std::string(), &error);
    status = remote_test_support::ReceiveTestFrame(&channel, client, &header,
                                                  &response, &error);
    test_support::Check(status == net::FrameReadStatus::kOk &&
                            header.status == static_cast<std::uint32_t>(
                                                net::Status::kInvalidRequest),
                        "SRV T4 请求帧带非 0 status 被拒绝");

    error.clear();
    status = RoundTrip(&channel, client, static_cast<std::uint16_t>(net::Opcode::kPing),
                       15, "unexpected", &header, &response, &error);
    test_support::Check(status == net::FrameReadStatus::kOk &&
                            header.status == static_cast<std::uint32_t>(
                                                net::Status::kInvalidRequest),
                        "SRV T4 PING 带 payload 被拒绝");

    // 连接仍然可用：上面的错误都没有把连接打掉。
    error.clear();
    status = RoundTrip(&channel, client, static_cast<std::uint16_t>(net::Opcode::kPing),
                       16, std::string(), &header, &response, &error);
    test_support::Check(status == net::FrameReadStatus::kOk &&
                            header.status ==
                                static_cast<std::uint32_t>(net::Status::kOk),
                        "SRV T4 判别：错误响应之后连接仍然可恢复");
    ::close(client);
    worker.join();
    server.Stop();
  }

  test_support::Section("SRV 5. 非法帧头：可恢复 vs 必须断开");
  {
    Fixture fixture;
    SetupFixture(&fixture, "srv-frames");
    net::RemoteServer server;
    std::string error;
    server.Configure(fixture.config, &error);
    server.Start(&error);
    std::thread worker = ServeOneConnection(&server);
    const int client = ConnectToLoopback(server.bound_port());
    net::SecureChannel channel;
    test_support::Check(
        HandshakeClientOrShutdown(client, fixture.pin, &channel, &error),
        "SRV T5 BPSEC1 握手完成", error);

    // 这一节测的是"帧头本身非法时服务端怎么处理"。BPSEC1 把整个帧（含帧头）
    // 当成记录的明文，所以畸形帧不能再明文写进 socket：那样写出来的是一条 tag
    // 不对的记录，服务端在记录层就断连，帧头的判别根本走不到。改用
    // channel.SendRecord()——它送出的是**合法记录的合法明文**，服务端解出来的
    // 字节与 PR #20 里完全一样，测的仍然是原来那条产品路径。

    // 1) 错版本：回 UNSUPPORTED_VERSION，连接继续。
    net::FrameHeader header;
    header.version = 2;
    header.opcode = static_cast<std::uint16_t>(net::Opcode::kPing);
    header.request_id = 21;
    header.payload_length = 0;
    std::string encoded = net::EncodeFrameHeader(header);
    error.clear();
    test_support::Check(channel.SendRecord(client, encoded, &error),
                        "SRV T5 夹具：错版本帧作为合法记录发出", error);
    error.clear();
    net::FrameHeader response_header;
    std::string response;
    net::FrameReadStatus status = remote_test_support::ReceiveTestFrame(
        &channel, client, &response_header, &response, &error);
    test_support::Check(status == net::FrameReadStatus::kOk &&
                            response_header.opcode ==
                                static_cast<std::uint16_t>(net::Opcode::kError) &&
                            response_header.request_id == 21 &&
                            response_header.status ==
                                static_cast<std::uint32_t>(
                                    net::Status::kUnsupportedVersion),
                        "SRV T5 错版本回 UNSUPPORTED_VERSION 并保持连接", error);

    // 2) reserved 非 0：回 MALFORMED_FRAME。
    header.version = net::kProtocolVersion;
    header.reserved = 3;
    header.request_id = 22;
    encoded = net::EncodeFrameHeader(header);
    error.clear();
    test_support::Check(channel.SendRecord(client, encoded, &error),
                        "SRV T5 夹具：reserved 非 0 的帧作为合法记录发出",
                        error);
    error.clear();
    status = remote_test_support::ReceiveTestFrame(&channel, client,
                                                   &response_header, &response,
                                                   &error);
    test_support::Check(status == net::FrameReadStatus::kOk &&
                            response_header.status ==
                                static_cast<std::uint32_t>(
                                    net::Status::kMalformedFrame),
                        "SRV T5 reserved 非 0 回 MALFORMED_FRAME", error);

    // 3) 假 magic：必须断开。假 magic 与"记录被改坏"是两件事：这里记录本身
    //    完好（tag 正确），坏的是记录里面的帧头，所以服务端必须按帧协议
    //    kCorruptStream 处理并断连。
    std::string bad_magic = net::EncodeFrameHeader(header);
    bad_magic[0] = 'X';
    error.clear();
    test_support::Check(channel.SendRecord(client, bad_magic, &error),
                        "SRV T5 夹具：假 magic 的帧作为合法记录发出", error);
    test_support::Check(PeerClosed(client),
                        "SRV T5 假 magic 之后服务端断开连接");
    ::close(client);
    worker.join();

    // 4) 超大长度：必须断开（新连接）。
    std::thread second_worker = ServeOneConnection(&server);
    const int client2 = ConnectToLoopback(server.bound_port());
    net::SecureChannel second_channel;
    test_support::Check(
        HandshakeClientOrShutdown(client2, fixture.pin, &second_channel, &error),
        "SRV T5 第二条连接 BPSEC1 握手完成", error);
    std::string huge = net::EncodeFrameHeader(header);
    for (int index = 24; index < 32; ++index) {
      huge[index] = static_cast<char>(0xFF);
    }
    error.clear();
    test_support::Check(second_channel.SendRecord(client2, huge, &error),
                        "SRV T5 夹具：超大长度的帧作为合法记录发出", error);
    test_support::Check(PeerClosed(client2),
                        "SRV T5 超大 payload 长度之后服务端断开连接");
    ::close(client2);
    second_worker.join();
    server.Stop();
  }

  test_support::Section("SRV 6. 同一个地址端口不允许第二个实例");
  {
    Fixture fixture;
    SetupFixture(&fixture, "srv-bind");
    net::RemoteServer first;
    std::string error;
    first.Configure(fixture.config, &error);
    test_support::Check(first.Start(&error), "SRV T6 第一个实例启动", error);

    net::RemoteServerConfig second_config = fixture.config;
    second_config.port = first.bound_port();
    net::RemoteServer second;
    std::string second_error;
    const bool configured = second.Configure(second_config, &second_error);
    test_support::Check(configured && !second.Start(&second_error) &&
                            !second.running(),
                        "SRV T6 同端口的第二个实例被拒绝", second_error);
    test_support::Check(second_error.find("bind") != std::string::npos ||
                            !second_error.empty(),
                        "SRV T6 拒绝时给出可定位的原因");
    first.Stop();
  }

  test_support::Section("SRV 7. Run() + 固定 worker 池：并发 PING");
  {
    Fixture fixture;
    SetupFixture(&fixture, "srv-pool");
    fixture.config.worker_count = 2;
    net::RemoteServer server;
    std::string error;
    server.Configure(fixture.config, &error);
    test_support::Check(server.Start(&error), "SRV T7 服务端启动", error);

    std::string run_error;
    bool run_result = true;
    std::thread runner([&server, &run_error, &run_result] {
      run_result = server.Run(&run_error);
    });

    bool all_ok = true;
    for (int round = 0; round < 2; ++round) {
      const int client = ConnectToLoopback(server.bound_port());
      if (client < 0) {
        all_ok = false;
        continue;
      }
      net::SecureChannel channel;
      std::string round_error;
      if (!HandshakeClientOrShutdown(client, fixture.pin, &channel,
                                     &round_error)) {
        all_ok = false;
        ::close(client);
        continue;
      }
      net::FrameHeader header;
      std::string response;
      const net::FrameReadStatus status = RoundTrip(
          &channel, client, static_cast<std::uint16_t>(net::Opcode::kPing), 31,
          std::string(), &header, &response, &round_error);
      if (status != net::FrameReadStatus::kOk ||
          header.status != static_cast<std::uint32_t>(net::Status::kOk)) {
        all_ok = false;
      }
      ::close(client);
    }
    test_support::Check(all_ok, "SRV T7 判别：worker 池下两次 PING 都成功");

    server.RequestStop();
    runner.join();
    test_support::Check(run_result, "SRV T7 Run() 在 RequestStop 之后正常返回",
                        run_error);
    server.Stop();
  }

  test_support::Section("SRV 8. 停止之后端口释放，可以重新启动");
  {
    Fixture fixture;
    SetupFixture(&fixture, "srv-restart");
    net::RemoteServer first;
    std::string error;
    first.Configure(fixture.config, &error);
    first.Start(&error);
    const std::uint16_t port = first.bound_port();
    first.Stop();

    net::RemoteServerConfig again = fixture.config;
    again.port = port;
    net::RemoteServer second;
    std::string second_error;
    test_support::Check(second.Configure(again, &second_error) &&
                            second.Start(&second_error),
                        "SRV T8 同一端口停止后可以重新绑定", second_error);
    second.Stop();
  }

  test_support::Section("SRV 9. 注册 / 登录 / 会话状态机");
  {
    Fixture fixture;
    SetupFixture(&fixture, "srv-auth");
    net::RemoteServer server;
    std::string error;
    server.Configure(fixture.config, &error);
    test_support::Check(server.Start(&error), "SRV T9 服务端启动", error);
    std::thread worker = ServeOneConnection(&server);
    const int client = ConnectToLoopback(server.bound_port());
    net::SecureChannel channel;
    test_support::Check(
        HandshakeClientOrShutdown(client, fixture.pin, &channel, &error),
        "SRV T9 BPSEC1 握手完成", error);

    // 用户名与口令全部运行时随机生成；口令不进任何断言文本。
    const std::string username = "night-" + RandomSecretHex().substr(0, 8);
    const std::string password = RandomSecretHex().substr(0, 24);
    const std::string wrong_password = RandomSecretHex().substr(0, 24);

    net::FrameHeader header;
    std::string response;

    error.clear();
    net::FrameReadStatus status = RoundTrip(
        &channel, client, static_cast<std::uint16_t>(net::Opcode::kList), 40,
        std::string(), &header, &response, &error);
    test_support::Check(status == net::FrameReadStatus::kOk &&
                            header.status == static_cast<std::uint32_t>(
                                                net::Status::kUnauthorized),
                        "SRV T9 没登录就 LIST = UNAUTHORIZED", error);

    error.clear();
    status = RoundTrip(&channel, client, static_cast<std::uint16_t>(net::Opcode::kRegister),
                       41, CredentialsPayload(username, password), &header,
                       &response, &error);
    test_support::Check(status == net::FrameReadStatus::kOk &&
                            header.status ==
                                static_cast<std::uint32_t>(net::Status::kOk),
                        "SRV T9 注册成功", net::StatusName(header.status));

    error.clear();
    status = RoundTrip(&channel, client, static_cast<std::uint16_t>(net::Opcode::kRegister),
                       42, CredentialsPayload(username, password), &header,
                       &response, &error);
    test_support::Check(status == net::FrameReadStatus::kOk &&
                            header.status == static_cast<std::uint32_t>(
                                                net::Status::kAlreadyExists),
                        "SRV T9 同名重复注册 = ALREADY_EXISTS",
                        net::StatusName(header.status));

    error.clear();
    status = RoundTrip(&channel, client, static_cast<std::uint16_t>(net::Opcode::kRegister),
                       43, CredentialsPayload("a/b", password), &header,
                       &response, &error);
    test_support::Check(status == net::FrameReadStatus::kOk &&
                            header.status == static_cast<std::uint32_t>(
                                                net::Status::kInvalidRequest),
                        "SRV T9 非法用户名注册 = INVALID_REQUEST");

    error.clear();
    status = RoundTrip(&channel, client, static_cast<std::uint16_t>(net::Opcode::kRegister),
                       44, CredentialsPayload("okname", "short"), &header,
                       &response, &error);
    test_support::Check(status == net::FrameReadStatus::kOk &&
                            header.status == static_cast<std::uint32_t>(
                                                net::Status::kInvalidRequest),
                        "SRV T9 过短口令注册 = INVALID_REQUEST");

    error.clear();
    std::string trailing = CredentialsPayload(username, password) + "junk";
    status = RoundTrip(&channel, client, static_cast<std::uint16_t>(net::Opcode::kRegister),
                       45, trailing, &header, &response, &error);
    test_support::Check(status == net::FrameReadStatus::kOk &&
                            header.status == static_cast<std::uint32_t>(
                                                net::Status::kInvalidRequest),
                        "SRV T9 带尾部垃圾的凭证帧 = INVALID_REQUEST");

    error.clear();
    status = RoundTrip(&channel, client, static_cast<std::uint16_t>(net::Opcode::kLogin),
                       46, CredentialsPayload(username, wrong_password), &header,
                       &response, &error);
    test_support::Check(status == net::FrameReadStatus::kOk &&
                            header.status == static_cast<std::uint32_t>(
                                                net::Status::kUnauthorized),
                        "SRV T9 口令错误 = UNAUTHORIZED",
                        net::StatusName(header.status));

    error.clear();
    status = RoundTrip(&channel, client, static_cast<std::uint16_t>(net::Opcode::kLogin),
                       47, CredentialsPayload("no-such-user", password), &header,
                       &response, &error);
    test_support::Check(status == net::FrameReadStatus::kOk &&
                            header.status == static_cast<std::uint32_t>(
                                                net::Status::kUnauthorized),
                        "SRV T9 不存在的用户与口令错误回答一致（不泄漏存在性）");

    error.clear();
    status = RoundTrip(&channel, client, static_cast<std::uint16_t>(net::Opcode::kLogin),
                       48, CredentialsPayload(username, password), &header,
                       &response, &error);
    test_support::Check(status == net::FrameReadStatus::kOk &&
                            header.status ==
                                static_cast<std::uint32_t>(net::Status::kOk),
                        "SRV T9 登录成功", net::StatusName(header.status));
    net::PayloadReader token_reader(response);
    std::string token;
    const bool token_ok = token_reader.ReadString(net::kMaxTokenBytes, &token) &&
                          token_reader.AtEnd() &&
                          token.size() == net::kTokenHexBytes;
    test_support::Check(token_ok, "SRV T9 响应里是一个定长十六进制 token");
    test_support::Check(!token.empty() &&
                            token.find(username) == std::string::npos &&
                            token.find(password) == std::string::npos,
                        "SRV T9 判别：token 里既没有用户名也没有口令");

    error.clear();
    status = RoundTrip(&channel, client, static_cast<std::uint16_t>(net::Opcode::kRegister),
                       49, CredentialsPayload("another-user", password), &header,
                       &response, &error);
    test_support::Check(status == net::FrameReadStatus::kOk &&
                            header.status == static_cast<std::uint32_t>(
                                                net::Status::kInvalidState),
                        "SRV T9 登录之后再 REGISTER = INVALID_STATE");

    error.clear();
    status = RoundTrip(&channel, client, static_cast<std::uint16_t>(net::Opcode::kList), 50,
                       std::string(), &header, &response, &error);
    net::PayloadReader list_reader(response);
    std::uint32_t entries = 1;
    const bool list_ok = status == net::FrameReadStatus::kOk &&
                         header.status ==
                             static_cast<std::uint32_t>(net::Status::kOk) &&
                         list_reader.ReadU32(&entries) && entries == 0 &&
                         list_reader.AtEnd();
    test_support::Check(list_ok, "SRV T9 登录后的 LIST 返回 0 条（还没有上传）",
                        net::StatusName(header.status));

    error.clear();
    status = RoundTrip(&channel, client, static_cast<std::uint16_t>(net::Opcode::kLogout),
                       51, std::string(), &header, &response, &error);
    test_support::Check(status == net::FrameReadStatus::kOk &&
                            header.status ==
                                static_cast<std::uint32_t>(net::Status::kOk),
                        "SRV T9 LOGOUT 成功");

    error.clear();
    status = RoundTrip(&channel, client, static_cast<std::uint16_t>(net::Opcode::kList), 52,
                       std::string(), &header, &response, &error);
    test_support::Check(status == net::FrameReadStatus::kOk &&
                            header.status == static_cast<std::uint32_t>(
                                                net::Status::kUnauthorized),
                        "SRV T9 判别：LOGOUT 之后 LIST 回到 UNAUTHORIZED");

    error.clear();
    status = RoundTrip(&channel, client, static_cast<std::uint16_t>(net::Opcode::kLogout),
                       53, std::string(), &header, &response, &error);
    test_support::Check(status == net::FrameReadStatus::kOk &&
                            header.status == static_cast<std::uint32_t>(
                                                net::Status::kUnauthorized),
                        "SRV T9 没登录时 LOGOUT = UNAUTHORIZED");

    ::close(client);
    worker.join();

    // 换一条连接重新登录：证明账号真的落在了 SQLite 里，而不是只在会话内存里。
    std::thread second_worker = ServeOneConnection(&server);
    const int again = ConnectToLoopback(server.bound_port());
    net::SecureChannel again_channel;
    test_support::Check(
        HandshakeClientOrShutdown(again, fixture.pin, &again_channel, &error),
        "SRV T9 第二条连接 BPSEC1 握手完成", error);
    error.clear();
    status = RoundTrip(&again_channel, again,
                       static_cast<std::uint16_t>(net::Opcode::kLogin), 54,
                       CredentialsPayload(username, password), &header,
                       &response, &error);
    test_support::Check(status == net::FrameReadStatus::kOk &&
                            header.status ==
                                static_cast<std::uint32_t>(net::Status::kOk),
                        "SRV T9 判别：新连接上用同一账号可以再次登录");
    ::close(again);
    second_worker.join();
    server.Stop();
  }

  return test_support::Finish("remote_server_test");
}
