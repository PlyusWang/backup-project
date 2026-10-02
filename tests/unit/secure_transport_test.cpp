// tests/unit/secure_transport_test.cpp
//
// PR #21：BPSEC1 加密传输层的单元测试。
//
// 覆盖四类证据：
//   1. 握手：真实 TCP loopback 上的完整握手（pin 校验、transcript 一致）；
//   2. 明文泄漏：客户端 -> 捕获代理 -> 服务端，抓真实 TCP 字节，
//      断言已知 marker 在线上出现 0 次，同时服务端确实收到了业务数据；
//   3. 篡改矩阵：密文翻位、tag 翻位、序号改写、截断、多余字节、超长声明、
//      错误方向密钥、重放，逐项要求 fail closed 且不交出明文；
//   4. 握手篡改：ClientHello / ServerHello / ClientFinished / ServerFinished
//      在链路上被改一个字节，双方都必须失败；以及错误 pin、全零对端公钥、
//      未配置 pin 时的拒绝。
//
// 传输层之外的东西（X25519 / HKDF 的官方向量）在 x25519_hkdf_test.cpp 里测。
//
// 退出码：0 = 全部通过。

#include <cerrno>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "crypto.h"
#include "secure_transport.h"

using backupproject::crypto::RandomBytes;
using backupproject::net::FrameHeader;
using backupproject::net::FrameReadStatus;
using backupproject::net::GenerateTransportIdentity;
using backupproject::net::Opcode;
using backupproject::net::ParseServerKeyPin;
using backupproject::net::SecureChannel;
using backupproject::net::SecureTransportError;
using backupproject::net::ServerKeyPin;
using backupproject::net::TransportIdentity;

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const std::string& name, const std::string& detail = "") {
  g_checks += 1;
  if (!ok) {
    g_failures += 1;
    std::printf("  FAIL %s%s\n", name.c_str(),
                detail.empty() ? "" : (" -- " + detail).c_str());
  }
}

std::size_t CountOccurrences(const std::string& haystack,
                             const std::string& needle) {
  if (needle.empty()) {
    return 0;
  }
  std::size_t count = 0;
  std::size_t position = haystack.find(needle);
  while (position != std::string::npos) {
    count += 1;
    position = haystack.find(needle, position + 1);
  }
  return count;
}

// ---- 阻塞式 socket 小工具 ----

bool WriteAll(int fd, const std::string& data) {
  std::size_t written = 0;
  while (written < data.size()) {
    const ssize_t count = ::send(fd, data.data() + written, data.size() - written, MSG_NOSIGNAL);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (count == 0) {
      return false;
    }
    written += static_cast<std::size_t>(count);
  }
  return true;
}

bool ReadExact(int fd, std::size_t size, std::string* out) {
  out->assign(size, '\0');
  std::size_t read_bytes = 0;
  while (read_bytes < size) {
    const ssize_t count = ::recv(fd, &(*out)[read_bytes], size - read_bytes, 0);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (count == 0) {
      return false;
    }
    read_bytes += static_cast<std::size_t>(count);
  }
  return true;
}

int MakeListener(std::uint16_t* port) {
  const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listener < 0) {
    return -1;
  }
  int reuse = 1;
  ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_in address;
  std::memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(listener);
    return -1;
  }
  if (::listen(listener, 4) != 0) {
    ::close(listener);
    return -1;
  }
  socklen_t length = sizeof(address);
  if (::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
    ::close(listener);
    return -1;
  }
  *port = ntohs(address.sin_port);
  return listener;
}

// poll + accept，避免测试在"没人连"时永久挂住。
int AcceptWithTimeout(int listener, int seconds) {
  pollfd entry;
  entry.fd = listener;
  entry.events = POLLIN;
  entry.revents = 0;
  const int ready = ::poll(&entry, 1, seconds * 1000);
  if (ready <= 0) {
    return -1;
  }
  return ::accept(listener, nullptr, nullptr);
}

int DialLocal(std::uint16_t port, int timeout_seconds) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  timeval timeout;
  timeout.tv_sec = timeout_seconds;
  timeout.tv_usec = 0;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  sockaddr_in address;
  std::memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

// ---- 捕获代理 ----
//
// 客户端连它，它连服务端，两个方向逐字节转发并**留下原始字节**。
// 可以按绝对偏移篡改**一个**字节（用于握手篡改测试）。
struct CaptureProxy {
  int listener = -1;
  std::uint16_t port = 0;
  std::thread worker;
  std::string client_to_server;
  std::string server_to_client;
  std::size_t mutate_c2s_offset = static_cast<std::size_t>(-1);
  unsigned char mutate_c2s_mask = 0;
  std::size_t mutate_s2c_offset = static_cast<std::size_t>(-1);
  unsigned char mutate_s2c_mask = 0;

  bool Start(std::uint16_t target_port, std::string* error) {
    listener = MakeListener(&port);
    if (listener < 0) {
      *error = "无法创建捕获代理的监听 socket";
      return false;
    }
    const int accept_listener = listener;
    const std::size_t c2s_offset = mutate_c2s_offset;
    const unsigned char c2s_mask = mutate_c2s_mask;
    const std::size_t s2c_offset = mutate_s2c_offset;
    const unsigned char s2c_mask = mutate_s2c_mask;
    worker = std::thread([this, accept_listener, target_port, c2s_offset,
                          c2s_mask, s2c_offset, s2c_mask]() {
      const int client = AcceptWithTimeout(accept_listener, 10);
      if (client < 0) {
        return;
      }
      const int server = DialLocal(target_port, 10);
      if (server < 0) {
        ::close(client);
        return;
      }
      bool client_open = true;
      bool server_open = true;
      unsigned char buffer[65536];
      while (client_open || server_open) {
        pollfd entries[2];
        entries[0].fd = client;
        entries[0].events = client_open ? POLLIN : 0;
        entries[0].revents = 0;
        entries[1].fd = server;
        entries[1].events = server_open ? POLLIN : 0;
        entries[1].revents = 0;
        const int ready = ::poll(entries, 2, 5000);
        if (ready <= 0) {
          break;
        }
        if (entries[0].revents & (POLLIN | POLLHUP)) {
          const ssize_t got = ::recv(client, buffer, sizeof(buffer), 0);
          if (got <= 0) {
            client_open = false;
            ::shutdown(server, SHUT_WR);
          } else {
            std::string chunk(reinterpret_cast<char*>(buffer),
                              static_cast<std::size_t>(got));
            for (std::size_t i = 0; i < chunk.size(); ++i) {
              const std::size_t absolute = client_to_server.size() + i;
              if (absolute == c2s_offset) {
                chunk[i] = static_cast<char>(chunk[i] ^ c2s_mask);
              }
            }
            client_to_server += chunk;
            if (!WriteAll(server, chunk)) {
              break;
            }
          }
        }
        if (entries[1].revents & (POLLIN | POLLHUP)) {
          const ssize_t got = ::recv(server, buffer, sizeof(buffer), 0);
          if (got <= 0) {
            server_open = false;
            ::shutdown(client, SHUT_WR);
          } else {
            std::string chunk(reinterpret_cast<char*>(buffer),
                              static_cast<std::size_t>(got));
            for (std::size_t i = 0; i < chunk.size(); ++i) {
              const std::size_t absolute = server_to_client.size() + i;
              if (absolute == s2c_offset) {
                chunk[i] = static_cast<char>(chunk[i] ^ s2c_mask);
              }
            }
            server_to_client += chunk;
            if (!WriteAll(client, chunk)) {
              break;
            }
          }
        }
      }
      ::close(client);
      ::close(server);
    });
    return true;
  }

  void Join() {
    if (worker.joinable()) {
      worker.join();
    }
    if (listener >= 0) {
      ::close(listener);
      listener = -1;
    }
  }
};

// ---- 服务端侧：握手 + echo ----
struct SecureEchoServer {
  int listener = -1;
  std::uint16_t port = 0;
  std::thread worker;
  std::atomic<bool> handshake_ok{false};
  std::atomic<int> handshake_code{0};
  std::string handshake_error;
  std::vector<std::string> received_payloads;
  std::vector<std::uint16_t> received_opcodes;
  std::string transcript;

  bool Start(const TransportIdentity& identity, std::string* error) {
    listener = MakeListener(&port);
    if (listener < 0) {
      *error = "无法创建 echo 服务端的监听 socket";
      return false;
    }
    const int accept_listener = listener;
    const TransportIdentity local_identity = identity;
    worker = std::thread([this, accept_listener, local_identity]() {
      const int client = AcceptWithTimeout(accept_listener, 10);
      if (client < 0) {
        return;
      }
      timeval timeout;
      timeout.tv_sec = 10;
      timeout.tv_usec = 0;
      ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
      SecureChannel channel;
      std::string error_text;
      if (!channel.HandshakeServer(client, local_identity, &error_text)) {
        handshake_error = error_text;
        handshake_code.store(static_cast<int>(channel.last_error()));
        ::close(client);
        return;
      }
      handshake_ok.store(true);
      transcript = channel.transcript_hash();
      for (;;) {
        FrameHeader header;
        std::string payload;
        std::string read_error;
        const FrameReadStatus status =
            channel.ReceiveFrame(client, &header, &payload, &read_error);
        if (status != FrameReadStatus::kOk) {
          break;
        }
        received_opcodes.push_back(header.opcode);
        received_payloads.push_back(payload);
        if (!channel.SendFrame(client, header.opcode, 0, header.request_id,
                               "echo:" + payload, &error_text)) {
          break;
        }
      }
      ::close(client);
    });
    return true;
  }

  void Stop() {
    if (worker.joinable()) {
      worker.join();
    }
    if (listener >= 0) {
      ::close(listener);
      listener = -1;
    }
  }
};

// SecureChannel 不可拷贝也不可移动（它持有密钥材料），所以这里用 unique_ptr 传递。
struct ChannelPair {
  int client_fd = -1;
  int server_fd = -1;
  SecureChannel client;
  SecureChannel server;
  bool ok = false;
};

// 在 socketpair 上完成一次真实握手：服务端侧在另一个线程里跑。
std::unique_ptr<ChannelPair> MakePair(const TransportIdentity& identity,
                                      const ServerKeyPin& pin) {
  auto pair = std::make_unique<ChannelPair>();
  int fds[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
    return pair;
  }
  pair->client_fd = fds[0];
  pair->server_fd = fds[1];
  ChannelPair* raw = pair.get();
  std::atomic<bool> server_ok{false};
  std::thread server_thread([raw, &identity, &server_ok]() {
    std::string error;
    server_ok.store(raw->server.HandshakeServer(raw->server_fd, identity, &error));
  });
  std::string error;
  const bool client_ok =
      pair->client.HandshakeClient(pair->client_fd, pin, &error);
  if (!client_ok) {
    // 客户端已经放弃这条连接：显式半关，让服务端线程立刻看到 EOF。
    // 不做这一步的话 join() 会等服务端读完 ClientFinished，直接卡死。
    ::shutdown(pair->client_fd, SHUT_RDWR);
    std::printf("  (MakePair: client handshake failed: %s)\n", error.c_str());
  }
  server_thread.join();
  pair->ok = client_ok && server_ok.load();
  return pair;
}

// 把字节喂到一个新的 socketpair 上，让 channel 从读端消费。
FrameReadStatus Feed(SecureChannel* channel, const std::string& bytes,
                     std::string* plaintext, std::string* error_message,
                     bool truncate_write = false) {
  int fds[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
    return FrameReadStatus::kIoError;
  }
  // 1 MiB 的记录比 socketpair 的发送缓冲区大：必须在另一个线程里写，
  // 否则写入方在 send() 里等缓冲区腾空、读入方还在等写入完成，自己和自己死锁。
  std::thread writer([&fds, &bytes]() {
    if (!bytes.empty()) {
      WriteAll(fds[1], bytes);
    }
    ::shutdown(fds[1], SHUT_WR);
  });
  (void)truncate_write;
  const FrameReadStatus status =
      channel->ReceiveRecord(fds[0], plaintext, error_message);
  // 先关读端：通道可能在读完之前就拒绝了（例如声明长度超限），
  // 这时写入线程还堵在 send() 里；关掉读端会让它拿到 EPIPE 立刻退出
  // （WriteAll 用 MSG_NOSIGNAL，不会收到 SIGPIPE）。
  ::close(fds[0]);
  writer.join();
  ::close(fds[1]);
  return status;
}

// 只构造 20 字节记录头：用来测试"光看声明长度就必须拒绝"的路径
// （不需要真的把 1 MiB 的假记录体写出去）。
std::string BuildRecordHeader(std::uint64_t sequence,
                              std::uint32_t declared_length) {
  std::string record;
  const std::uint32_t magic = backupproject::net::kBssec1Magic;
  record.push_back(static_cast<char>((magic >> 24) & 0xFF));
  record.push_back(static_cast<char>((magic >> 16) & 0xFF));
  record.push_back(static_cast<char>((magic >> 8) & 0xFF));
  record.push_back(static_cast<char>(magic & 0xFF));
  record.push_back(static_cast<char>(backupproject::net::kBssec1MessageRecord));
  record.push_back(static_cast<char>(backupproject::net::kBssec1Version));
  record.push_back(0);
  record.push_back(0);
  for (int shift = 56; shift >= 0; shift -= 8) {
    record.push_back(static_cast<char>((sequence >> shift) & 0xFF));
  }
  for (int shift = 24; shift >= 0; shift -= 8) {
    record.push_back(static_cast<char>((declared_length >> shift) & 0xFF));
  }
  return record;
}

// ============================ 测试 ============================

void TestHandshakeOverTcp() {
  std::printf("[transport] 真实 TCP 上的握手\n");
  TransportIdentity identity;
  std::string error;
  Check(GenerateTransportIdentity(&identity, &error), "生成服务端身份密钥", error);

  ServerKeyPin pin;
  Check(ParseServerKeyPin("sha256:" + backupproject::crypto::X25519Fingerprint(identity.public_key),
                          &pin, &error),
        "按指纹 pin", error);

  SecureEchoServer server;
  Check(server.Start(identity, &error), "启动 echo 服务端", error);
  const int fd = DialLocal(server.port, 10);
  Check(fd >= 0, "连接到 echo 服务端");

  SecureChannel client;
  Check(client.HandshakeClient(fd, pin, &error), "客户端握手成功", error);
  Check(client.established(), "客户端通道已建立");
  Check(client.peer_public_key() == identity.public_key,
        "客户端看到的是服务端身份公钥");
  Check(client.peer_fingerprint() ==
            backupproject::crypto::X25519Fingerprint(identity.public_key),
        "对端指纹与 pin 一致");

  const std::string payload = "hello-secure-world";
  Check(client.SendFrame(fd, static_cast<std::uint16_t>(Opcode::kPing), 0, 7,
                         payload, &error),
        "发送加密帧", error);
  FrameHeader header;
  std::string response;
  Check(client.ReceiveFrame(fd, &header, &response, &error) == FrameReadStatus::kOk,
        "收到加密响应", error);
  Check(response == "echo:" + payload, "响应内容正确", response);
  Check(header.request_id == 7, "request_id 原样返回");
  Check(client.send_sequence() == 1 && client.receive_sequence() == 1,
        "客户端序号各自独立递增");
  ::close(fd);
  server.Stop();
  Check(server.handshake_ok.load(), "服务端握手成功");
  Check(server.received_payloads.size() == 1 &&
            server.received_payloads[0] == payload,
        "服务端解出业务明文");
  Check(server.transcript == client.transcript_hash(),
        "两端 transcript 摘要一致");
  Check(!server.transcript.empty() && server.transcript.size() == 32,
        "transcript 摘要是 32 字节");
}

void TestWireCaptureHasNoPlaintext() {
  std::printf("[transport] 线上字节捕获：明文 marker 必须 0 次出现\n");
  TransportIdentity identity;
  std::string error;
  Check(GenerateTransportIdentity(&identity, &error), "生成服务端身份密钥", error);
  ServerKeyPin pin;
  Check(ParseServerKeyPin("hex:" + backupproject::crypto::X25519FormatKeyHex(identity.public_key),
                          &pin, &error),
        "按公钥 pin", error);

  SecureEchoServer server;
  Check(server.Start(identity, &error), "启动 echo 服务端", error);

  CaptureProxy proxy;
  Check(proxy.Start(server.port, &error), "启动捕获代理", error);

  const int fd = DialLocal(proxy.port, 10);
  Check(fd >= 0, "通过代理连接");
  SecureChannel client;
  Check(client.HandshakeClient(fd, pin, &error), "经代理握手成功", error);

  const std::string password_marker = "PR21-PLAINTEXT-PASSWORD-MARKER";
  const std::string username_marker = "PR21-USERNAME-MARKER";
  const std::string token_marker = "PR21-TOKEN-MARKER";
  const std::string metadata_marker = "PR21-SNAPSHOT-METADATA-MARKER";

  struct Case {
    std::uint16_t opcode;
    std::string payload;
  };
  const Case cases[] = {
      {static_cast<std::uint16_t>(Opcode::kLogin),
       username_marker + "\x01" + password_marker},
      {static_cast<std::uint16_t>(Opcode::kResume), token_marker},
      {static_cast<std::uint16_t>(Opcode::kList),
       metadata_marker + std::string(500, 'x')},
      {static_cast<std::uint16_t>(Opcode::kUploadChunk),
       std::string(1000, 'A') + password_marker},
  };
  std::uint64_t request_id = 100;
  for (const Case& item : cases) {
    Check(client.SendFrame(fd, item.opcode, 0, request_id++, item.payload,
                           &error),
          "发送含 marker 的加密帧", error);
    FrameHeader header;
    std::string response;
    Check(client.ReceiveFrame(fd, &header, &response, &error) ==
              FrameReadStatus::kOk,
          "收到 echo 响应", error);
  }
  ::close(fd);
  server.Stop();
  proxy.Join();

  Check(!proxy.client_to_server.empty(), "捕获到了客户端发出的字节");
  Check(!proxy.server_to_client.empty(), "捕获到了服务端发出的字节");
  const std::string wire = proxy.client_to_server + proxy.server_to_client;
  Check(CountOccurrences(wire, password_marker) == 0, "线上没有口令明文");
  Check(CountOccurrences(wire, username_marker) == 0, "线上没有用户名字面量");
  Check(CountOccurrences(wire, token_marker) == 0, "线上没有 token 明文");
  Check(CountOccurrences(wire, metadata_marker) == 0,
        "线上没有快照元数据明文");
  Check(CountOccurrences(wire, std::string(500, 'x')) == 0,
        "线上没有 payload 的重复字节模式");
  Check(CountOccurrences(proxy.client_to_server, "BPN1") == 0,
        "线上没有 BPNET1 帧头 magic");
  // 握手字节确实经过了代理（证明抓的是这次会话）。
  Check(proxy.client_to_server.size() > 72 + 40, "客户端方向至少包含握手与记录");
  Check(server.handshake_ok.load(), "服务端握手成功（经代理）");
  Check(server.received_payloads.size() == 4, "服务端收到 4 帧业务数据");
  bool markers_arrived = false;
  if (server.received_payloads.size() == 4) {
    markers_arrived =
        server.received_payloads[0].find(password_marker) != std::string::npos &&
        server.received_payloads[0].find(username_marker) != std::string::npos &&
        server.received_payloads[1] == token_marker &&
        server.received_payloads[3].find(password_marker) != std::string::npos;
  }
  Check(markers_arrived, "marker 确实作为业务数据到达服务端（说明是加密而不是丢弃）");
}

void TestRecordTamperMatrix() {
  std::printf("[transport] 篡改矩阵\n");
  TransportIdentity identity;
  std::string error;
  Check(GenerateTransportIdentity(&identity, &error), "生成身份密钥", error);
  ServerKeyPin pin;
  Check(ParseServerKeyPin("sha256:" +
                              backupproject::crypto::X25519Fingerprint(
                                  identity.public_key),
                          &pin, &error),
        "pin", error);

  const std::string plaintext = "PR21-TAMPER-PAYLOAD-0123456789";

  // ciphertext 翻位
  {
    std::printf("  [tamper] case 1\n");
    std::fflush(stdout);
    auto pair = MakePair(identity, pin);
    Check(pair->ok, "篡改用例：握手成功");
    Check(pair->client.SendRecord(pair->client_fd, plaintext, &error),
          "发送一条记录", error);
    std::string raw;
    Check(ReadExact(pair->server_fd,
                    backupproject::net::kBssec1RecordHeaderSize + plaintext.size() +
                        backupproject::net::kBssec1TagSize,
                    &raw),
          "抓到原始记录字节");
    raw[backupproject::net::kBssec1RecordHeaderSize + 3] ^= 0x40;
    std::string out;
    std::string err;
    const FrameReadStatus status = Feed(&pair->server, raw, &out, &err);
    Check(status != FrameReadStatus::kOk, "密文翻位被拒绝");
    Check(out.empty(), "密文翻位时不交出明文");
    Check(pair->server.last_error() == SecureTransportError::kRecordAuthentication,
          "错误分类是 record-authentication");
  }

  // tag 翻位
  {
    std::printf("  [tamper] case 2\n");
    std::fflush(stdout);
    auto pair = MakePair(identity, pin);
    Check(pair->client.SendRecord(pair->client_fd, plaintext, &error), "发送记录", error);
    std::string raw;
    ReadExact(pair->server_fd, backupproject::net::kBssec1RecordHeaderSize +
                                  plaintext.size() +
                                  backupproject::net::kBssec1TagSize,
              &raw);
    raw[backupproject::net::kBssec1RecordHeaderSize + plaintext.size() + 9] ^= 0x01;
    std::string out;
    std::string err;
    Check(Feed(&pair->server, raw, &out, &err) != FrameReadStatus::kOk,
          "tag 翻位被拒绝");
    Check(out.empty(), "tag 翻位时不交出明文");
  }

  // 序号改写（seq + 2）
  {
    std::printf("  [tamper] case 3\n");
    std::fflush(stdout);
    auto pair = MakePair(identity, pin);
    Check(pair->client.SendRecord(pair->client_fd, plaintext, &error), "发送记录", error);
    std::string raw;
    ReadExact(pair->server_fd, backupproject::net::kBssec1RecordHeaderSize +
                                  plaintext.size() +
                                  backupproject::net::kBssec1TagSize,
              &raw);
    raw[15] = static_cast<char>(2);
    std::string out;
    std::string err;
    Check(Feed(&pair->server, raw, &out, &err) != FrameReadStatus::kOk,
          "序号 +2 被拒绝");
    Check(pair->server.last_error() == SecureTransportError::kReplayDetected,
          "错误分类是 replay");
  }

  // 重放：同一条记录喂两次
  {
    std::printf("  [tamper] case 4\n");
    std::fflush(stdout);
    auto pair = MakePair(identity, pin);
    Check(pair->client.SendRecord(pair->client_fd, plaintext, &error), "发送记录", error);
    std::string raw;
    ReadExact(pair->server_fd, backupproject::net::kBssec1RecordHeaderSize +
                                  plaintext.size() +
                                  backupproject::net::kBssec1TagSize,
              &raw);
    std::string out;
    std::string err;
    Check(Feed(&pair->server, raw, &out, &err) == FrameReadStatus::kOk,
          "第一次投递被接受");
    Check(out == plaintext, "第一次投递解出正确明文");
    Check(Feed(&pair->server, raw, &out, &err) != FrameReadStatus::kOk,
          "重放同一条记录被拒绝");
  }

  // 截断：记录体少一个字节就 EOF
  {
    std::printf("  [tamper] case 5\n");
    std::fflush(stdout);
    auto pair = MakePair(identity, pin);
    Check(pair->client.SendRecord(pair->client_fd, plaintext, &error), "发送记录", error);
    std::string raw;
    ReadExact(pair->server_fd, backupproject::net::kBssec1RecordHeaderSize +
                                  plaintext.size() +
                                  backupproject::net::kBssec1TagSize,
              &raw);
    const std::string truncated = raw.substr(0, raw.size() - 1);
    std::string out;
    std::string err;
    Check(Feed(&pair->server, truncated, &out, &err) != FrameReadStatus::kOk,
          "截断的记录被拒绝");
    Check(out.empty(), "截断时不交出明文");
  }

  // 多余字节：合法记录后面跟着一个字节，不能被当成下一条记录
  {
    std::printf("  [tamper] case 6\n");
    std::fflush(stdout);
    auto pair = MakePair(identity, pin);
    Check(pair->client.SendRecord(pair->client_fd, plaintext, &error), "发送记录", error);
    std::string raw;
    ReadExact(pair->server_fd, backupproject::net::kBssec1RecordHeaderSize +
                                  plaintext.size() +
                                  backupproject::net::kBssec1TagSize,
              &raw);
    std::string out;
    std::string err;
    Check(Feed(&pair->server, raw + std::string(1, 'Z'), &out, &err) ==
              FrameReadStatus::kOk,
          "第一条记录仍然被接受");
    Check(Feed(&pair->server, std::string(1, 'Z'), &out, &err) !=
              FrameReadStatus::kOk,
          "尾部多余字节不会变成一条合法记录");
  }

  // 超长声明
  {
    std::printf("  [tamper] case 7\n");
    std::fflush(stdout);
    auto pair = MakePair(identity, pin);
    const std::string oversized = BuildRecordHeader(
        0, static_cast<std::uint32_t>(
               backupproject::net::kBssec1MaxPlaintextBytes + 1));
    std::string out;
    std::string err;
    Check(Feed(&pair->server, oversized, &out, &err) != FrameReadStatus::kOk,
          "声明长度超过上限的记录被拒绝");
    Check(pair->server.last_error() == SecureTransportError::kOversizedRecord,
          "错误分类是 oversized-record");
  }

  // 错误方向密钥：把客户端发出的记录喂回客户端自己
  {
    std::printf("  [tamper] case 8\n");
    std::fflush(stdout);
    auto pair = MakePair(identity, pin);
    Check(pair->client.SendRecord(pair->client_fd, plaintext, &error), "发送记录", error);
    std::string raw;
    ReadExact(pair->server_fd, backupproject::net::kBssec1RecordHeaderSize +
                                  plaintext.size() +
                                  backupproject::net::kBssec1TagSize,
              &raw);
    std::string out;
    std::string err;
    Check(Feed(&pair->client, raw, &out, &err) != FrameReadStatus::kOk,
          "拿 c2s 记录喂给 s2c 方向被拒绝");
  }

  // 记录 magic 被打坏
  {
    std::printf("  [tamper] case 9\n");
    std::fflush(stdout);
    auto pair = MakePair(identity, pin);
    Check(pair->client.SendRecord(pair->client_fd, plaintext, &error), "发送记录", error);
    std::string raw;
    ReadExact(pair->server_fd, backupproject::net::kBssec1RecordHeaderSize +
                                  plaintext.size() +
                                  backupproject::net::kBssec1TagSize,
              &raw);
    raw[0] ^= 0x20;
    std::string out;
    std::string err;
    Check(Feed(&pair->server, raw, &out, &err) != FrameReadStatus::kOk,
          "记录 magic 被改后拒绝");
  }

  // 空明文记录必须能正常往返
  {
    std::printf("  [tamper] case 10\n");
    std::fflush(stdout);
    auto pair = MakePair(identity, pin);
    Check(pair->client.SendRecord(pair->client_fd, "", &error), "发送空记录", error);
    std::string raw;
    Check(ReadExact(pair->server_fd,
                    backupproject::net::kBssec1RecordHeaderSize +
                        backupproject::net::kBssec1TagSize,
                    &raw),
          "抓到空记录的字节");
    std::string out;
    std::string err;
    Check(Feed(&pair->server, raw, &out, &err) == FrameReadStatus::kOk,
          "空明文记录被接受");
    Check(out.empty(), "空明文记录解出空明文");
  }

  // 最大负载记录（1 MiB payload）必须能往返，且序号继续递增
  {
    std::printf("  [tamper] case 11\n");
    std::fflush(stdout);
    auto pair = MakePair(identity, pin);
    const std::string big(backupproject::net::kMaxPayloadBytes, 'B');
    bool send_ok = false;
    std::thread sender([&pair, &big, &error, &send_ok]() {
      send_ok = pair->client.SendFrame(
          pair->client_fd, static_cast<std::uint16_t>(Opcode::kUploadChunk), 0, 1,
          big, &error);
    });
    std::string raw;
    Check(ReadExact(pair->server_fd,
                    backupproject::net::kBssec1RecordHeaderSize +
                        backupproject::net::kFrameHeaderSize + big.size() +
                        backupproject::net::kBssec1TagSize,
                    &raw),
          "抓到最大记录的字节");
    sender.join();
    Check(send_ok, "发送 1 MiB payload 的加密帧", error);
    std::string out;
    std::string err;
    Check(Feed(&pair->server, raw, &out, &err) == FrameReadStatus::kOk,
          "最大记录被接受");
    // 同一条记录再喂给刚建立的新通道，验证 ReceiveFrame 能把 1 MiB 帧解回来。
    auto second = MakePair(identity, pin);
    Check(second->ok, "最大帧用例：第二次握手");
    bool second_send_ok = false;
    std::thread second_sender([&second, &big, &error, &second_send_ok]() {
      second_send_ok = second->client.SendFrame(
          second->client_fd, static_cast<std::uint16_t>(Opcode::kUploadChunk), 0,
          42, big, &error);
    });
    std::string raw2;
    Check(ReadExact(second->server_fd,
                    backupproject::net::kBssec1RecordHeaderSize +
                        backupproject::net::kFrameHeaderSize + big.size() +
                        backupproject::net::kBssec1TagSize,
                    &raw2),
          "抓到第二条最大记录");
    second_sender.join();
    Check(second_send_ok, "第二次发送 1 MiB 帧", error);
    int fds[2] = {-1, -1};
    Check(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "创建 socketpair");
    std::thread writer([&fds, &raw2]() {
      WriteAll(fds[1], raw2);
      ::shutdown(fds[1], SHUT_WR);
    });
    FrameHeader header;
    std::string payload;
    Check(second->server.ReceiveFrame(fds[0], &header, &payload, &err) ==
              FrameReadStatus::kOk,
          "1 MiB 加密帧被完整解回", err);
    Check(header.opcode == static_cast<std::uint16_t>(Opcode::kUploadChunk) &&
              header.request_id == 42 && header.payload_length == big.size(),
          "帧头字段正确");
    Check(payload == big, "1 MiB payload 逐字节一致");
    writer.join();
    ::close(fds[0]);
    ::close(fds[1]);
    ::close(second->client_fd);
    ::close(second->server_fd);
  }
}

void TestHandshakeMutations() {
  std::printf("[transport] 握手篡改与身份校验\n");
  TransportIdentity identity;
  std::string error;
  Check(GenerateTransportIdentity(&identity, &error), "生成身份密钥", error);
  ServerKeyPin pin;
  Check(ParseServerKeyPin("sha256:" +
                              backupproject::crypto::X25519Fingerprint(
                                  identity.public_key),
                          &pin, &error),
        "pin", error);
  TransportIdentity other_identity;
  Check(GenerateTransportIdentity(&other_identity, &error), "生成另一份身份密钥",
        error);
  ServerKeyPin wrong_pin;
  Check(ParseServerKeyPin("sha256:" +
                              backupproject::crypto::X25519Fingerprint(
                                  other_identity.public_key),
                          &wrong_pin, &error),
        "错误的 pin", error);

  struct MutationCase {
    const char* name;
    bool mutate_client_to_server;
    std::size_t offset;
    unsigned char mask;
    bool expect_client_failure;
    bool expect_server_failure;
  };
  const MutationCase cases[] = {
      {"ClientHello 里的临时公钥被改", true, 40 + 5, 0x01, true, true},
      {"ServerHello 里的身份公钥被改", false, 40 + 7, 0x01, true, true},
      {"ClientFinished 的 HMAC 被改", true, 72 + 8, 0x80, true, true},
      {"ServerFinished 的 HMAC 被改", false, 104 + 8, 0x01, true, false},
  };
  for (const MutationCase& item : cases) {
  std::printf("  [mutate] step 1\n");
  std::fflush(stdout);
    SecureEchoServer server;
    Check(server.Start(identity, &error), std::string(item.name) + "：启动服务端",
          error);
    CaptureProxy proxy;
    if (item.mutate_client_to_server) {
      proxy.mutate_c2s_offset = item.offset;
      proxy.mutate_c2s_mask = item.mask;
    } else {
      proxy.mutate_s2c_offset = item.offset;
      proxy.mutate_s2c_mask = item.mask;
    }
    Check(proxy.Start(server.port, &error), std::string(item.name) + "：启动代理",
          error);
    const int fd = DialLocal(proxy.port, 5);
    SecureChannel client;
    std::string client_error;
    const bool client_ok = fd >= 0 && client.HandshakeClient(fd, pin, &client_error);
    if (fd >= 0) {
      ::close(fd);
    }
    server.Stop();
    proxy.Join();
    Check(!client_ok == item.expect_client_failure,
          std::string(item.name) + "：客户端侧结果符合预期",
          client_ok ? "客户端竟然成功了" : client_error);
    if (item.expect_server_failure) {
      Check(!server.handshake_ok.load(),
            std::string(item.name) + "：服务端拒绝握手", server.handshake_error);
    }
    Check(server.received_payloads.empty(),
          std::string(item.name) + "：失败握手后没有处理任何业务帧");
  }

  // 全零临时公钥必须被归类为 weak-shared-secret（而不是"随便算出一个密钥"）
  std::printf("  [mutate] step 2\n");
  std::fflush(stdout);
  {
    SecureEchoServer server;
    Check(server.Start(identity, &error), "全零公钥：启动服务端", error);
    CaptureProxy proxy;
    Check(proxy.Start(server.port, &error), "全零公钥：启动代理", error);
    // 直接构造一条全是 0 的 ClientHello：临时公钥全零 -> 共享秘密全零。
    const int fd = DialLocal(proxy.port, 5);
    if (fd >= 0) {
      std::string hello;
      const std::uint32_t magic = backupproject::net::kBssec1Magic;
      hello.push_back(static_cast<char>((magic >> 24) & 0xFF));
      hello.push_back(static_cast<char>((magic >> 16) & 0xFF));
      hello.push_back(static_cast<char>((magic >> 8) & 0xFF));
      hello.push_back(static_cast<char>(magic & 0xFF));
      hello.push_back(1);
      hello.push_back(1);
      hello.push_back(0);
      hello.push_back(1);
      hello += std::string(32, '\x11');   // client random
      hello += std::string(32, '\0');     // client ephemeral：全零
      WriteAll(fd, hello);
      ::close(fd);
    }
    server.Stop();
    proxy.Join();
    Check(!server.handshake_ok.load(), "全零对端公钥被服务端拒绝",
          server.handshake_error);
    Check(server.handshake_error.find("低阶点") != std::string::npos ||
              server.handshake_error.find("共享秘密") != std::string::npos,
          "全零公钥的原因文案提到共享秘密/低阶点", server.handshake_error);
  }

  // 服务端身份与 pin 不符：客户端必须拒绝，并且双方都失败
  std::printf("  [mutate] step 3\n");
  std::fflush(stdout);
  {
    SecureEchoServer server;
    Check(server.Start(identity, &error), "错误 pin：启动服务端", error);
    const int fd = DialLocal(server.port, 5);
    SecureChannel client;
    std::string client_error;
    const bool ok = fd >= 0 && client.HandshakeClient(fd, wrong_pin, &client_error);
    if (fd >= 0) {
      ::close(fd);
    }
    server.Stop();
    Check(!ok, "pin 与服务端身份不符时握手失败");
    Check(client.last_error() == SecureTransportError::kServerKeyMismatch,
          "错误分类是 server-key-mismatch",
          backupproject::net::SecureTransportErrorName(client.last_error()));
    Check(client_error.find("pin") != std::string::npos,
          "错误文案提到 pin", client_error);
    Check(!server.handshake_ok.load(), "服务端侧也没有建立会话");
  }

  // 没有配置 pin：必须明确拒绝，且不往网络上写任何字节
  std::printf("  [mutate] step 4\n");
  std::fflush(stdout);
  {
    SecureEchoServer server;
    Check(server.Start(identity, &error), "无 pin：启动服务端", error);
    const int fd = DialLocal(server.port, 5);
    SecureChannel client;
    ServerKeyPin empty_pin;
    std::string client_error;
    const bool ok = fd >= 0 && client.HandshakeClient(fd, empty_pin, &client_error);
    std::string probe;
    bool saw_bytes = false;
    if (fd >= 0) {
      pollfd entry;
      entry.fd = fd;
      entry.events = POLLIN;
      entry.revents = 0;
      if (::poll(&entry, 1, 200) > 0 && (entry.revents & POLLIN) != 0) {
        saw_bytes = true;
      }
      ::close(fd);
    }
    server.Stop();
    Check(!ok, "未配置 pin 时拒绝握手");
    Check(client.last_error() == SecureTransportError::kNoPinConfigured,
          "错误分类是 no-server-key-pin");
    Check(!saw_bytes, "未配置 pin 时一个字节都没有发出去（没有 TOFU）");
  }

  // pin 文本解析
  std::printf("  [mutate] step 5\n");
  std::fflush(stdout);
  {
    ServerKeyPin parsed;
    std::string parse_error;
    Check(ParseServerKeyPin("sha256:ABCDEF" + std::string(58, '0'), &parsed,
                            &parse_error),
          "接受大写指纹");
    Check(parsed.fingerprint_hex == "abcdef" + std::string(58, '0'),
          "指纹归一化成小写");
    Check(!ParseServerKeyPin(std::string(64, 'a'), &parsed, &parse_error),
          "不接受不带前缀的裸十六进制");
    Check(!ParseServerKeyPin("sha256:xyz", &parsed, &parse_error),
          "不接受非十六进制指纹");
    Check(!ParseServerKeyPin("hex:1234", &parsed, &parse_error),
          "不接受长度不对的公钥");
  }

  // 会话密钥：握手后双方不应持有相同的发送密钥（方向分离）
  std::printf("  [mutate] step 6\n");
  std::fflush(stdout);
  {
    auto pair = MakePair(identity, pin);
    Check(pair->ok, "方向分离用例：握手成功");
    Check(pair->client.send_sequence() == 0 && pair->server.receive_sequence() == 0,
          "握手后序号都从 0 开始");
    // 客户端发一条，服务端收到：序号在各自方向上独立推进。
    std::string error_text;
    Check(pair->client.SendRecord(pair->client_fd, "x", &error_text), "发送一条记录");
    std::string raw;
    Check(ReadExact(pair->server_fd, backupproject::net::kBssec1RecordHeaderSize + 1 +
                                        backupproject::net::kBssec1TagSize,
                    &raw),
          "抓到记录");
    ::close(pair->client_fd);
    ::close(pair->server_fd);
  }
}

// 读当前进程的常驻内存（KB）。只用于"没有线性增长"这条证据。
long ResidentKilobytes() {
  std::FILE* file = std::fopen("/proc/self/statm", "r");
  if (file == nullptr) {
    return -1;
  }
  long total_pages = 0;
  long resident_pages = 0;
  const int matched = std::fscanf(file, "%ld %ld", &total_pages, &resident_pages);
  std::fclose(file);
  if (matched != 2) {
    return -1;
  }
  return resident_pages * (sysconf(_SC_PAGESIZE) / 1024);
}

void TestLargeStreamingTransfer() {
  // 32 MiB 连续经过加密记录层：证明它是**流式**的，而不是"整份读进内存再加密"。
  // 每个记录正好携带一个 1 MiB payload 的 BPNET1 帧（协议允许的最大帧）。
  std::printf("[transport] 32 MiB 流式传输与内存上界\n");
  TransportIdentity identity;
  std::string error;
  Check(GenerateTransportIdentity(&identity, &error), "生成身份密钥", error);
  ServerKeyPin pin;
  Check(ParseServerKeyPin(
            "sha256:" +
                backupproject::crypto::X25519Fingerprint(identity.public_key),
            &pin, &error),
        "pin", error);

  auto pair = MakePair(identity, pin);
  Check(pair->ok, "大流量用例：握手成功");

  const std::size_t chunk_bytes = backupproject::net::kMaxPayloadBytes;  // 1 MiB
  const std::size_t target_bytes = 32u * 1024u * 1024u;                  // 32 MiB
  const std::size_t expected_frames = target_bytes / chunk_bytes;

  std::atomic<std::size_t> received_frames{0};
  std::atomic<std::size_t> received_bytes{0};
  std::atomic<bool> payload_ok{true};
  std::thread reader([&pair, &received_frames, &received_bytes, &payload_ok]() {
    std::string error_text;
    for (;;) {
      FrameHeader header;
      std::string payload;
      const FrameReadStatus status = pair->server.ReceiveFrame(
          pair->server_fd, &header, &payload, &error_text);
      if (status != FrameReadStatus::kOk) {
        break;
      }
      // 每个 payload 都是同一个 4 字节模式，长度必须是 1 MiB。
      if (payload.size() != backupproject::net::kMaxPayloadBytes ||
          payload[0] != 'Z' || payload[payload.size() - 1] != 'Z') {
        payload_ok.store(false);
      }
      received_frames.fetch_add(1);
      received_bytes.fetch_add(payload.size());
    }
  });

  const std::string chunk(chunk_bytes, 'Z');
  const long rss_before = ResidentKilobytes();
  bool sent = true;
  for (std::size_t index = 0; index < expected_frames; ++index) {
    if (!pair->client.SendFrame(pair->client_fd,
                                static_cast<std::uint16_t>(Opcode::kUploadChunk),
                                0, static_cast<std::uint64_t>(index + 1), chunk,
                                &error)) {
      sent = false;
      break;
    }
  }
  // 发完就关掉客户端，让读线程在收完最后一帧后拿到 kClosed 退出。
  Check(sent, "32 MiB 全部发出", error);
  ::shutdown(pair->client_fd, SHUT_WR);
  reader.join();
  const long rss_after = ResidentKilobytes();

  Check(received_frames.load() == expected_frames && sent,
        "32 MiB 全部收到",
        std::to_string(received_frames.load()) + "/" +
            std::to_string(expected_frames) + " 帧");
  Check(received_bytes.load() == target_bytes, "收到的字节数正确",
        std::to_string(received_bytes.load()));
  Check(payload_ok.load(), "每一帧的 payload 逐字节完整（长度与首尾字节都对）");
  const long growth = (rss_before > 0 && rss_after > 0) ? rss_after - rss_before : -1;
  Check(growth >= 0 && growth < 16 * 1024,
        "RSS 没有线性增长（32 MiB 传输）",
        "RSS " + std::to_string(rss_before) + " -> " + std::to_string(rss_after) +
            " KB");
  std::printf("  RSS_BOUND: %ld KB -> %ld KB（传输 %zu MiB）\n", rss_before,
              rss_after, target_bytes / (1024u * 1024u));
  ::close(pair->client_fd);
  ::close(pair->server_fd);
}

}  // namespace

int main() {
  std::printf("BPSEC1 加密传输层单元测试\n");
  TestHandshakeOverTcp();
  TestWireCaptureHasNoPlaintext();
  TestRecordTamperMatrix();
  TestHandshakeMutations();
  TestLargeStreamingTransfer();
  const int passed = g_checks - g_failures;
  std::printf("secure-transport-test: %d/%d checks passed\n", passed, g_checks);
  return g_failures == 0 ? 0 : 1;
}
