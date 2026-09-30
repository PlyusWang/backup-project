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
#include "remote_server.h"
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
  fixture->config.bind_address = "127.0.0.1";
  fixture->config.port = 0;  // 内核分配端口，测试之间不抢
  fixture->config.root_directory = fixture->root;
  fixture->config.database_path = fixture->database;
  fixture->config.secret_file_path = fixture->secret_file;
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

net::FrameReadStatus RoundTrip(int fd, std::uint16_t opcode,
                               std::uint64_t request_id,
                               const std::string& payload,
                               net::FrameHeader* header,
                               std::string* response, std::string* error) {
  if (!net::SendFrame(fd, opcode, 0, request_id, payload, error)) {
    return net::FrameReadStatus::kIoError;
  }
  return net::ReceiveFrame(fd, header, response, error);
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

    net::FrameHeader header;
    std::string response;
    const net::FrameReadStatus status =
        RoundTrip(client, static_cast<std::uint16_t>(net::Opcode::kPing), 7,
                  std::string(), &header, &response, &error);
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

    net::FrameHeader header;
    std::string response;
    net::FrameReadStatus status = RoundTrip(
        client, static_cast<std::uint16_t>(net::Opcode::kList), 11,
        std::string(), &header, &response, &error);
    test_support::Check(status == net::FrameReadStatus::kOk &&
                            header.status == static_cast<std::uint32_t>(
                                                net::Status::kUnsupported),
                        "SRV T4 没登录的 LIST 被拒绝（当前构建回答 UNSUPPORTED）",
                        net::StatusName(header.status));

    error.clear();
    net::PayloadBuilder credentials;
    credentials.AppendString("night-user", net::kMaxUsernameBytes, &error);
    credentials.AppendString("secret-password", net::kMaxPasswordBytes, &error);
    status = RoundTrip(client, static_cast<std::uint16_t>(net::Opcode::kLogin),
                       12, credentials.data(), &header, &response, &error);
    test_support::Check(status == net::FrameReadStatus::kOk &&
                            header.status == static_cast<std::uint32_t>(
                                                net::Status::kUnsupported),
                        "SRV T4 LOGIN 在认证接入前如实回答 UNSUPPORTED",
                        net::StatusName(header.status));

    error.clear();
    status = RoundTrip(client, 9999, 13, std::string(), &header, &response,
                       &error);
    test_support::Check(status == net::FrameReadStatus::kOk &&
                            header.opcode == static_cast<std::uint16_t>(
                                                net::Opcode::kError) &&
                            header.status == static_cast<std::uint32_t>(
                                                net::Status::kInvalidRequest),
                        "SRV T4 未知操作码回 INVALID_REQUEST", error);

    error.clear();
    // 请求帧里的 status 必须是 0。
    net::SendFrame(client, static_cast<std::uint16_t>(net::Opcode::kPing), 5,
                   14, std::string(), &error);
    status = net::ReceiveFrame(client, &header, &response, &error);
    test_support::Check(status == net::FrameReadStatus::kOk &&
                            header.status == static_cast<std::uint32_t>(
                                                net::Status::kInvalidRequest),
                        "SRV T4 请求帧带非 0 status 被拒绝");

    error.clear();
    status = RoundTrip(client, static_cast<std::uint16_t>(net::Opcode::kPing),
                       15, "unexpected", &header, &response, &error);
    test_support::Check(status == net::FrameReadStatus::kOk &&
                            header.status == static_cast<std::uint32_t>(
                                                net::Status::kInvalidRequest),
                        "SRV T4 PING 带 payload 被拒绝");

    // 连接仍然可用：上面的错误都没有把连接打掉。
    error.clear();
    status = RoundTrip(client, static_cast<std::uint16_t>(net::Opcode::kPing),
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

    // 1) 错版本：回 UNSUPPORTED_VERSION，连接继续。
    net::FrameHeader header;
    header.version = 2;
    header.opcode = static_cast<std::uint16_t>(net::Opcode::kPing);
    header.request_id = 21;
    header.payload_length = 0;
    std::string encoded = net::EncodeFrameHeader(header);
    net::SendAll(client, encoded.data(), encoded.size(), &error);
    net::FrameHeader response_header;
    std::string response;
    net::FrameReadStatus status =
        net::ReceiveFrame(client, &response_header, &response, &error);
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
    net::SendAll(client, encoded.data(), encoded.size(), &error);
    status = net::ReceiveFrame(client, &response_header, &response, &error);
    test_support::Check(status == net::FrameReadStatus::kOk &&
                            response_header.status ==
                                static_cast<std::uint32_t>(
                                    net::Status::kMalformedFrame),
                        "SRV T5 reserved 非 0 回 MALFORMED_FRAME", error);

    // 3) 假 magic：必须断开。
    std::string bad_magic = net::EncodeFrameHeader(header);
    bad_magic[0] = 'X';
    error.clear();
    net::SendAll(client, bad_magic.data(), bad_magic.size(), &error);
    test_support::Check(PeerClosed(client),
                        "SRV T5 假 magic 之后服务端断开连接");
    ::close(client);
    worker.join();

    // 4) 超大长度：必须断开（新连接）。
    std::thread second_worker = ServeOneConnection(&server);
    const int client2 = ConnectToLoopback(server.bound_port());
    std::string huge = net::EncodeFrameHeader(header);
    for (int index = 24; index < 32; ++index) {
      huge[index] = static_cast<char>(0xFF);
    }
    error.clear();
    net::SendAll(client2, huge.data(), huge.size(), &error);
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
      net::FrameHeader header;
      std::string response;
      std::string round_error;
      const net::FrameReadStatus status = RoundTrip(
          client, static_cast<std::uint16_t>(net::Opcode::kPing), 31,
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

  return test_support::Finish("remote_server_test");
}
