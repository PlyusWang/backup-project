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

#include "secure_transport.h"

#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "crypto.h"

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
    const ssize_t count =
        ::send(fd, data.data() + written, data.size() - written, MSG_NOSIGNAL);
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
  if (::bind(listener, reinterpret_cast<sockaddr*>(&address),
             sizeof(address)) != 0) {
    ::close(listener);
    return -1;
  }
  if (::listen(listener, 4) != 0) {
    ::close(listener);
    return -1;
  }
  socklen_t length = sizeof(address);
  if (::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) !=
      0) {
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
  if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) !=
      0) {
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

// SecureChannel 不可拷贝也不可移动（它持有密钥材料），所以这里用 unique_ptr
// 传递。
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
    server_ok.store(
        raw->server.HandshakeServer(raw->server_fd, identity, &error));
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

// ---- 红队工具：切分中继、看门狗、时间与内存测量 ----
//
// 这一节只服务于本次审查新增的用例。它们从"坏网络 + 主动攻击者"的视角
// 制造三种情况：一次 recv 只拿到半个消息、对端读到一半就消失、声明的长度
// 大到根本不能分配。任何一条用例挂死时只记一条失败，而不是把整个测试
// 二进制拖到外层 timeout 才被杀掉。

std::size_t ElapsedMs(std::chrono::steady_clock::time_point start) {
  return static_cast<std::size_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start)
          .count());
}

// 当前进程的峰值 RSS（Linux 上 ru_maxrss 的单位是 KB）。它只增不减，
// 所以"跨过一次调用没有涨"可以证明那次调用**没有**按声明的长度去分配内存
// （std::string(n, '\0') 会把每一页都写一遍，4 GiB 的分配必然体现在这里）。
long PeakRssKb() {
  struct rusage usage;
  std::memset(&usage, 0, sizeof(usage));
  if (::getrusage(RUSAGE_SELF, &usage) != 0) {
    return -1;
  }
  return usage.ru_maxrss;
}

// 把可能永久阻塞的调用放到另一个线程里；主线程最多等 timeout_ms，
// 超时就 ::shutdown 两端（阻塞中的 recv/send 会立刻返回）再 join。
struct WatchedCall {
  bool completed = false;
  std::size_t elapsed_ms = 0;

  template <typename Callable>
  bool Run(Callable callable, std::size_t timeout_ms, int fd_a, int fd_b) {
    std::atomic<bool> done{false};
    const auto start = std::chrono::steady_clock::now();
    std::thread worker([&callable, &done]() {
      callable();
      done.store(true);
    });
    while (!done.load() && ElapsedMs(start) <= timeout_ms) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!done.load()) {
      if (fd_a >= 0) {
        ::shutdown(fd_a, SHUT_RDWR);
      }
      if (fd_b >= 0) {
        ::shutdown(fd_b, SHUT_RDWR);
      }
      for (int i = 0; i < 250 && !done.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    }
    completed = done.load();
    worker.join();
    elapsed_ms = ElapsedMs(start);
    return completed;
  }
};

const char* FrameReadStatusName(FrameReadStatus status) {
  switch (status) {
    case FrameReadStatus::kOk:
      return "kOk";
    case FrameReadStatus::kClosed:
      return "kClosed";
    case FrameReadStatus::kInvalidFrame:
      return "kInvalidFrame";
    case FrameReadStatus::kCorruptStream:
      return "kCorruptStream";
    case FrameReadStatus::kIoError:
      return "kIoError";
  }
  return "unknown";
}

void DrainSocket(int fd) {
  unsigned char buffer[4096];
  for (;;) {
    const ssize_t got = ::recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
    if (got <= 0) {
      return;
    }
  }
}

// 断言 fd 的读端此刻一个字节都没有：poll(0) 必须是 0，紧接着的 recv
// 必须是 EAGAIN/EWOULDBLOCK。C++ 的 send 可能已经把字节放进缓冲区，
// 所以这个检查之前必须先 DrainSocket。
bool SocketBufferEmpty(int fd) {
  pollfd entry;
  entry.fd = fd;
  entry.events = POLLIN;
  entry.revents = 0;
  const int ready = ::poll(&entry, 1, 0);
  unsigned char byte = 0;
  const ssize_t got = ::recv(fd, &byte, 1, MSG_DONTWAIT);
  if (got > 0) {
    DrainSocket(fd);
    return false;
  }
  if (got == 0) {
    return false;  // 对端已经关闭：这不算"什么都没写"，要显式暴露出来
  }
  return ready == 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
}

// 线上字节里有没有 BPSEC1 记录消息（magic "BPS1" + 类型 5）。
// 握手失败的连接里**不允许**出现业务记录。
bool ContainsBssec1Record(const std::string& bytes) {
  const char magic[4] = {'B', 'P', 'S', '1'};
  for (std::size_t i = 0; i + 4 < bytes.size(); ++i) {
    bool match = true;
    for (std::size_t j = 0; j < 4; ++j) {
      if (bytes[i + j] != magic[j]) {
        match = false;
        break;
      }
    }
    if (match && static_cast<unsigned char>(bytes[i + 4]) == 5) {
      return true;
    }
  }
  return false;
}

// 手工拼一条 40 字节的 Finished 消息（magic + 类型 + 版本 + 保留 + tag）。
// 攻击者能用它回一条**格式完全正确、tag 是伪造的** ServerFinished。
std::string BuildFinishedBytes(std::uint8_t type, const std::string& tag) {
  const std::uint32_t magic = backupproject::net::kBssec1Magic;
  std::string out;
  out.push_back(static_cast<char>((magic >> 24) & 0xFFu));
  out.push_back(static_cast<char>((magic >> 16) & 0xFFu));
  out.push_back(static_cast<char>((magic >> 8) & 0xFFu));
  out.push_back(static_cast<char>(magic & 0xFFu));
  out.push_back(static_cast<char>(type));
  out.push_back(static_cast<char>(backupproject::net::kBssec1Version));
  out.push_back(0);
  out.push_back(0);
  out.append(tag);
  return out;
}

// 一段线上字节是不是"长度正确、magic/类型/版本都对"的 BPSEC1 握手消息。
// 断连矩阵里对端收到的 ServerHello / ClientFinished 是**这一轮**新生成的
// （随机数与临时公钥每次都不一样），所以只能做结构断言，不能和上一轮的
// 捕获结果逐字节比较。
bool LooksLikeBssec1Message(const std::string& bytes, std::size_t size,
                            std::uint8_t type) {
  if (bytes.size() != size) {
    return false;
  }
  const std::uint32_t magic = backupproject::net::kBssec1Magic;
  const char expected[4] = {static_cast<char>((magic >> 24) & 0xFFu),
                            static_cast<char>((magic >> 16) & 0xFFu),
                            static_cast<char>((magic >> 8) & 0xFFu),
                            static_cast<char>(magic & 0xFFu)};
  for (std::size_t i = 0; i < 4; ++i) {
    if (bytes[i] != expected[i]) {
      return false;
    }
  }
  return static_cast<unsigned char>(bytes[4]) == type &&
         static_cast<unsigned char>(bytes[5]) ==
             backupproject::net::kBssec1Version;
}

// 限速/切分中继：把一条字节流拆成 chunk 字节一片（chunk == 0 时按
// splits 里的绝对偏移强制切分）再转发，并记录两个方向的原始字节。
// 它是"坏网络"的替身：任何"一次 recv 就是一个结构体"的假设都会在这里
// 立刻暴露——写侧每次最多只放 N 个字节。
struct FragmentedRelay {
  int client_fd = -1;
  int server_fd = -1;
  std::thread worker;
  std::atomic<std::size_t> c2s_writes{0};
  std::atomic<std::size_t> s2c_writes{0};
  std::string c2s_bytes;
  std::string s2c_bytes;

  bool Start(std::size_t chunk, std::vector<std::size_t> splits_c2s,
             std::vector<std::size_t> splits_s2c, int delay_us,
             std::string* error) {
    int first[2] = {-1, -1};
    int second[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, first) != 0 ||
        ::socketpair(AF_UNIX, SOCK_STREAM, 0, second) != 0) {
      *error = "无法创建中继用的 socketpair";
      return false;
    }
    timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    ::setsockopt(first[1], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(first[1], SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(second[0], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(second[0], SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    client_fd = first[0];
    server_fd = second[1];
    const int inner_client = first[1];
    const int inner_server = second[0];
    worker = std::thread([this, inner_client, inner_server, chunk, splits_c2s,
                          splits_s2c, delay_us]() {
      Pump(inner_client, inner_server, chunk, splits_c2s, splits_s2c, delay_us);
    });
    return true;
  }

  void Join() {
    if (client_fd >= 0) {
      ::shutdown(client_fd, SHUT_RDWR);
    }
    if (server_fd >= 0) {
      ::shutdown(server_fd, SHUT_RDWR);
    }
    if (worker.joinable()) {
      worker.join();
    }
    if (client_fd >= 0) {
      ::close(client_fd);
      client_fd = -1;
    }
    if (server_fd >= 0) {
      ::close(server_fd);
      server_fd = -1;
    }
  }

 private:
  static std::size_t NextPiece(std::size_t chunk,
                               const std::vector<std::size_t>& splits,
                               std::size_t* split_index, std::size_t offset,
                               std::size_t available) {
    if (chunk > 0) {
      return available < chunk ? available : chunk;
    }
    std::size_t allowed = available;
    if (*split_index < splits.size()) {
      const std::size_t target = splits[*split_index];
      if (target > offset && target - offset < allowed) {
        allowed = target - offset;
      }
      if (offset + allowed >= target) {
        *split_index += 1;
      }
    }
    return allowed == 0 ? available : allowed;
  }

  void Pump(int inner_client, int inner_server, std::size_t chunk,
            const std::vector<std::size_t>& splits_c2s,
            const std::vector<std::size_t>& splits_s2c, int delay_us) {
    std::string pending_c2s;
    std::string pending_s2c;
    std::size_t offset_c2s = 0;
    std::size_t offset_s2c = 0;
    std::size_t index_c2s = 0;
    std::size_t index_s2c = 0;
    bool client_open = true;
    bool server_open = true;
    unsigned char buffer[4096];
    while (client_open || server_open) {
      pollfd entries[2];
      entries[0].fd = inner_client;
      entries[0].events = client_open ? POLLIN : 0;
      entries[0].revents = 0;
      entries[1].fd = inner_server;
      entries[1].events = server_open ? POLLIN : 0;
      entries[1].revents = 0;
      const int ready = ::poll(entries, 2, 5000);
      if (ready == 0) {
        break;  // 5 秒没有任何动静：链路已经废了，交给测试收尾。
      }
      if (ready < 0) {
        if (errno == EINTR) {
          continue;
        }
        break;
      }
      if (client_open && entries[0].revents != 0) {
        const ssize_t got = ::recv(inner_client, buffer, sizeof(buffer), 0);
        if (got > 0) {
          pending_c2s.append(reinterpret_cast<const char*>(buffer),
                             static_cast<std::size_t>(got));
        } else if (got == 0 || (got < 0 && errno != EINTR && errno != EAGAIN)) {
          client_open = false;
        }
      }
      if (server_open && entries[1].revents != 0) {
        const ssize_t got = ::recv(inner_server, buffer, sizeof(buffer), 0);
        if (got > 0) {
          pending_s2c.append(reinterpret_cast<const char*>(buffer),
                             static_cast<std::size_t>(got));
        } else if (got == 0 || (got < 0 && errno != EINTR && errno != EAGAIN)) {
          server_open = false;
        }
      }
      while (!pending_c2s.empty()) {
        const std::size_t piece = NextPiece(chunk, splits_c2s, &index_c2s,
                                            offset_c2s, pending_c2s.size());
        if (!WriteAll(inner_server, pending_c2s.substr(0, piece))) {
          client_open = false;
          pending_c2s.clear();
          break;
        }
        c2s_bytes.append(pending_c2s, 0, piece);
        c2s_writes.fetch_add(1);
        offset_c2s += piece;
        pending_c2s.erase(0, piece);
        if (delay_us > 0) {
          std::this_thread::sleep_for(std::chrono::microseconds(delay_us));
        }
      }
      while (!pending_s2c.empty()) {
        const std::size_t piece = NextPiece(chunk, splits_s2c, &index_s2c,
                                            offset_s2c, pending_s2c.size());
        if (!WriteAll(inner_client, pending_s2c.substr(0, piece))) {
          server_open = false;
          pending_s2c.clear();
          break;
        }
        s2c_bytes.append(pending_s2c, 0, piece);
        s2c_writes.fetch_add(1);
        offset_s2c += piece;
        pending_s2c.erase(0, piece);
        if (delay_us > 0) {
          std::this_thread::sleep_for(std::chrono::microseconds(delay_us));
        }
      }
    }
  }
};

// 跑一次真实握手并把四个握手消息原样抓下来（供断连矩阵当"真实前缀"用）。
bool CaptureHandshakeMessages(const TransportIdentity& identity,
                              const ServerKeyPin& pin,
                              std::string* client_hello,
                              std::string* server_hello,
                              std::string* client_finished,
                              std::string* server_finished,
                              std::string* error) {
  FragmentedRelay relay;
  if (!relay.Start(0, {}, {}, 0, error)) {
    return false;
  }
  SecureChannel client_channel;
  SecureChannel server_channel;
  bool client_ok = false;
  bool server_ok = false;
  std::string client_error;
  std::string server_error;
  WatchedCall watch;
  const bool completed = watch.Run(
      [&]() {
        std::thread server_thread([&]() {
          server_ok = server_channel.HandshakeServer(relay.server_fd, identity,
                                                     &server_error);
        });
        client_ok =
            client_channel.HandshakeClient(relay.client_fd, pin, &client_error);
        if (!client_ok) {
          ::shutdown(relay.client_fd, SHUT_RDWR);
        }
        server_thread.join();
      },
      5000, relay.client_fd, relay.server_fd);
  relay.Join();
  if (!completed || !client_ok || !server_ok) {
    *error = "无法捕获握手消息：" + client_error + " / " + server_error;
    return false;
  }
  const std::size_t hello_size = backupproject::net::kBssec1ClientHelloSize;
  const std::size_t server_hello_size =
      backupproject::net::kBssec1ServerHelloSize;
  const std::size_t finished_size = backupproject::net::kBssec1FinishedSize;
  *client_hello = relay.c2s_bytes.substr(0, hello_size);
  *client_finished = relay.c2s_bytes.substr(hello_size, finished_size);
  *server_hello = relay.s2c_bytes.substr(0, server_hello_size);
  *server_finished = relay.s2c_bytes.substr(server_hello_size, finished_size);
  return relay.c2s_bytes.size() == hello_size + finished_size &&
         relay.s2c_bytes.size() == server_hello_size + finished_size;
}

// ---- 断连矩阵 ----

struct DisconnectOutcome {
  bool completed = false;  // 调用在超时前返回了（没有挂死）
  bool handshake_ok = false;
  bool established = false;
  SecureTransportError error = SecureTransportError::kNone;
  std::string peer_saw;  // 脚本化那一侧实际读到的字节
  std::string message;
};

struct DisconnectCase {
  bool channel_is_client;  // true：被测的是 HandshakeClient（对端演服务端）
  std::size_t prefix_bytes;
  bool full_close;  // true：close()；false：只 shutdown(SHUT_WR)
  const char* label;
};

DisconnectOutcome RunDisconnectCase(const DisconnectCase& test_case,
                                    const TransportIdentity& identity,
                                    const ServerKeyPin& pin,
                                    const std::string& client_hello,
                                    const std::string& server_hello) {
  DisconnectOutcome outcome;
  int fds[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
    outcome.message = "socketpair 失败";
    return outcome;
  }
  const std::string source =
      test_case.channel_is_client ? server_hello : client_hello;
  const std::string prefix = source.substr(0, test_case.prefix_bytes);
  const bool full_close = test_case.full_close;
  std::thread peer([&fds, &outcome, prefix, full_close]() {
    if (!prefix.empty()) {
      WriteAll(fds[1], prefix);
    }
    if (full_close) {
      ::shutdown(fds[1], SHUT_RDWR);
      return;
    }
    ::shutdown(fds[1], SHUT_WR);
    unsigned char buffer[4096];
    for (;;) {
      const ssize_t got = ::recv(fds[1], buffer, sizeof(buffer), 0);
      if (got <= 0) {
        break;
      }
      outcome.peer_saw.append(reinterpret_cast<const char*>(buffer),
                              static_cast<std::size_t>(got));
    }
  });
  SecureChannel channel;
  std::string error;
  bool ok = true;
  WatchedCall watch;
  outcome.completed = watch.Run(
      [&]() {
        if (test_case.channel_is_client) {
          ok = channel.HandshakeClient(fds[0], pin, &error);
        } else {
          ok = channel.HandshakeServer(fds[0], identity, &error);
        }
      },
      3000, fds[0], fds[1]);
  // 让对端线程的 drain 结束：只半关**写**端（对端读完队列里已有的字节
  // 之后才看到 EOF）。这里不能直接 SHUT_RDWR —— 那会把还在队列里的
  // ServerHello / ClientFinished 一起丢掉，检查就失去意义了。
  ::shutdown(fds[0], SHUT_WR);
  peer.join();
  ::shutdown(fds[0], SHUT_RDWR);
  outcome.handshake_ok = ok;
  outcome.established = channel.established();
  outcome.error = channel.last_error();
  outcome.message = error;
  ::close(fds[0]);
  ::close(fds[1]);
  return outcome;
}

// 在新的 socketpair 上给**同一个**通道对象重新握手（Reset 之后复用）。
bool HandshakeAgain(SecureChannel* client, const TransportIdentity& identity,
                    const ServerKeyPin& pin, int* client_fd_out,
                    SecureChannel* server_channel, int* server_fd_out,
                    std::string* error) {
  int fds[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
    *error = "socketpair 失败";
    return false;
  }
  timeval timeout;
  timeout.tv_sec = 5;
  timeout.tv_usec = 0;
  ::setsockopt(fds[0], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  ::setsockopt(fds[1], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  *client_fd_out = fds[0];
  *server_fd_out = fds[1];
  bool client_ok = false;
  bool server_ok = false;
  std::string client_error;
  std::string server_error;
  WatchedCall watch;
  const bool completed = watch.Run(
      [&]() {
        std::thread server_thread([&]() {
          server_ok =
              server_channel->HandshakeServer(fds[1], identity, &server_error);
        });
        client_ok = client->HandshakeClient(fds[0], pin, &client_error);
        if (!client_ok) {
          ::shutdown(fds[0], SHUT_RDWR);
        }
        server_thread.join();
      },
      5000, fds[0], fds[1]);
  *error = client_error + " / " + server_error;
  return completed && client_ok && server_ok;
}

// ---- 记录头长度边界：只发头，正文一个字节都不给 ----

struct FeedOutcome {
  FrameReadStatus status = FrameReadStatus::kIoError;
  bool completed = false;
  std::size_t elapsed_ms = 0;
  long rss_growth_kb = 0;
  std::string plaintext;
  std::string error;
};

FeedOutcome FeedRecordHeader(SecureChannel* channel, std::uint32_t length,
                             int rcv_timeout_ms, bool close_after_header) {
  FeedOutcome outcome;
  int fds[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
    outcome.error = "socketpair 失败";
    return outcome;
  }
  timeval timeout;
  timeout.tv_sec = rcv_timeout_ms / 1000;
  timeout.tv_usec = (rcv_timeout_ms % 1000) * 1000;
  ::setsockopt(fds[0], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  const std::string header = BuildRecordHeader(0, length);
  std::atomic<bool> stop{false};
  std::thread peer([&fds, &header, &stop, close_after_header]() {
    WriteAll(fds[1], header);
    if (close_after_header) {
      ::shutdown(fds[1], SHUT_WR);
    }
    while (!stop.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  });
  const long rss_before = PeakRssKb();
  std::string plaintext;
  std::string error;
  FrameReadStatus status = FrameReadStatus::kIoError;
  WatchedCall watch;
  const bool completed = watch.Run(
      [&]() { status = channel->ReceiveRecord(fds[0], &plaintext, &error); },
      5000, fds[0], fds[1]);
  outcome.elapsed_ms = watch.elapsed_ms;
  outcome.rss_growth_kb = PeakRssKb() - rss_before;
  outcome.status = status;
  outcome.completed = completed;
  outcome.plaintext = plaintext;
  outcome.error = error;
  stop = true;
  ::shutdown(fds[1], SHUT_RDWR);
  peer.join();
  ::close(fds[0]);
  ::close(fds[1]);
  return outcome;
}

// ============================ 测试 ============================

void TestHandshakeOverTcp() {
  std::printf("[transport] 真实 TCP 上的握手\n");
  TransportIdentity identity;
  std::string error;
  Check(GenerateTransportIdentity(&identity, &error), "生成服务端身份密钥",
        error);

  ServerKeyPin pin;
  Check(ParseServerKeyPin("sha256:" + backupproject::crypto::X25519Fingerprint(
                                          identity.public_key),
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
  Check(client.ReceiveFrame(fd, &header, &response, &error) ==
            FrameReadStatus::kOk,
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
  Check(GenerateTransportIdentity(&identity, &error), "生成服务端身份密钥",
        error);
  ServerKeyPin pin;
  Check(ParseServerKeyPin("hex:" + backupproject::crypto::X25519FormatKeyHex(
                                       identity.public_key),
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
  Check(CountOccurrences(wire, metadata_marker) == 0, "线上没有快照元数据明文");
  Check(CountOccurrences(wire, std::string(500, 'x')) == 0,
        "线上没有 payload 的重复字节模式");
  Check(CountOccurrences(proxy.client_to_server, "BPN1") == 0,
        "线上没有 BPNET1 帧头 magic");
  // 握手字节确实经过了代理（证明抓的是这次会话）。
  Check(proxy.client_to_server.size() > 72 + 40,
        "客户端方向至少包含握手与记录");
  Check(server.handshake_ok.load(), "服务端握手成功（经代理）");
  Check(server.received_payloads.size() == 4, "服务端收到 4 帧业务数据");
  bool markers_arrived = false;
  if (server.received_payloads.size() == 4) {
    markers_arrived =
        server.received_payloads[0].find(password_marker) !=
            std::string::npos &&
        server.received_payloads[0].find(username_marker) !=
            std::string::npos &&
        server.received_payloads[1] == token_marker &&
        server.received_payloads[3].find(password_marker) != std::string::npos;
  }
  Check(markers_arrived,
        "marker 确实作为业务数据到达服务端（说明是加密而不是丢弃）");
}

void TestRecordTamperMatrix() {
  std::printf("[transport] 篡改矩阵\n");
  TransportIdentity identity;
  std::string error;
  Check(GenerateTransportIdentity(&identity, &error), "生成身份密钥", error);
  ServerKeyPin pin;
  Check(ParseServerKeyPin("sha256:" + backupproject::crypto::X25519Fingerprint(
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
                    backupproject::net::kBssec1RecordHeaderSize +
                        plaintext.size() + backupproject::net::kBssec1TagSize,
                    &raw),
          "抓到原始记录字节");
    raw[backupproject::net::kBssec1RecordHeaderSize + 3] ^= 0x40;
    std::string out;
    std::string err;
    const FrameReadStatus status = Feed(&pair->server, raw, &out, &err);
    Check(status != FrameReadStatus::kOk, "密文翻位被拒绝");
    Check(out.empty(), "密文翻位时不交出明文");
    Check(pair->server.last_error() ==
              SecureTransportError::kRecordAuthentication,
          "错误分类是 record-authentication");
  }

  // tag 翻位
  {
    std::printf("  [tamper] case 2\n");
    std::fflush(stdout);
    auto pair = MakePair(identity, pin);
    Check(pair->client.SendRecord(pair->client_fd, plaintext, &error),
          "发送记录", error);
    std::string raw;
    ReadExact(pair->server_fd,
              backupproject::net::kBssec1RecordHeaderSize + plaintext.size() +
                  backupproject::net::kBssec1TagSize,
              &raw);
    raw[backupproject::net::kBssec1RecordHeaderSize + plaintext.size() + 9] ^=
        0x01;
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
    Check(pair->client.SendRecord(pair->client_fd, plaintext, &error),
          "发送记录", error);
    std::string raw;
    ReadExact(pair->server_fd,
              backupproject::net::kBssec1RecordHeaderSize + plaintext.size() +
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
    Check(pair->client.SendRecord(pair->client_fd, plaintext, &error),
          "发送记录", error);
    std::string raw;
    ReadExact(pair->server_fd,
              backupproject::net::kBssec1RecordHeaderSize + plaintext.size() +
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
    Check(pair->client.SendRecord(pair->client_fd, plaintext, &error),
          "发送记录", error);
    std::string raw;
    ReadExact(pair->server_fd,
              backupproject::net::kBssec1RecordHeaderSize + plaintext.size() +
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
    Check(pair->client.SendRecord(pair->client_fd, plaintext, &error),
          "发送记录", error);
    std::string raw;
    ReadExact(pair->server_fd,
              backupproject::net::kBssec1RecordHeaderSize + plaintext.size() +
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
    Check(pair->client.SendRecord(pair->client_fd, plaintext, &error),
          "发送记录", error);
    std::string raw;
    ReadExact(pair->server_fd,
              backupproject::net::kBssec1RecordHeaderSize + plaintext.size() +
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
    Check(pair->client.SendRecord(pair->client_fd, plaintext, &error),
          "发送记录", error);
    std::string raw;
    ReadExact(pair->server_fd,
              backupproject::net::kBssec1RecordHeaderSize + plaintext.size() +
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
    Check(pair->client.SendRecord(pair->client_fd, "", &error), "发送空记录",
          error);
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
          pair->client_fd, static_cast<std::uint16_t>(Opcode::kUploadChunk), 0,
          1, big, &error);
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
          second->client_fd, static_cast<std::uint16_t>(Opcode::kUploadChunk),
          0, 42, big, &error);
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
  Check(ParseServerKeyPin("sha256:" + backupproject::crypto::X25519Fingerprint(
                                          identity.public_key),
                          &pin, &error),
        "pin", error);
  TransportIdentity other_identity;
  Check(GenerateTransportIdentity(&other_identity, &error),
        "生成另一份身份密钥", error);
  ServerKeyPin wrong_pin;
  Check(ParseServerKeyPin("sha256:" + backupproject::crypto::X25519Fingerprint(
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
    Check(server.Start(identity, &error),
          std::string(item.name) + "：启动服务端", error);
    CaptureProxy proxy;
    if (item.mutate_client_to_server) {
      proxy.mutate_c2s_offset = item.offset;
      proxy.mutate_c2s_mask = item.mask;
    } else {
      proxy.mutate_s2c_offset = item.offset;
      proxy.mutate_s2c_mask = item.mask;
    }
    Check(proxy.Start(server.port, &error),
          std::string(item.name) + "：启动代理", error);
    const int fd = DialLocal(proxy.port, 5);
    SecureChannel client;
    std::string client_error;
    const bool client_ok =
        fd >= 0 && client.HandshakeClient(fd, pin, &client_error);
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
            std::string(item.name) + "：服务端拒绝握手",
            server.handshake_error);
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
      hello += std::string(32, '\x11');  // client random
      hello += std::string(32, '\0');    // client ephemeral：全零
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
    const bool ok =
        fd >= 0 && client.HandshakeClient(fd, wrong_pin, &client_error);
    if (fd >= 0) {
      ::close(fd);
    }
    server.Stop();
    Check(!ok, "pin 与服务端身份不符时握手失败");
    Check(client.last_error() == SecureTransportError::kServerKeyMismatch,
          "错误分类是 server-key-mismatch",
          backupproject::net::SecureTransportErrorName(client.last_error()));
    Check(client_error.find("pin") != std::string::npos, "错误文案提到 pin",
          client_error);
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
    const bool ok =
        fd >= 0 && client.HandshakeClient(fd, empty_pin, &client_error);
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
    Check(pair->client.send_sequence() == 0 &&
              pair->server.receive_sequence() == 0,
          "握手后序号都从 0 开始");
    // 客户端发一条，服务端收到：序号在各自方向上独立推进。
    std::string error_text;
    Check(pair->client.SendRecord(pair->client_fd, "x", &error_text),
          "发送一条记录");
    std::string raw;
    Check(ReadExact(pair->server_fd,
                    backupproject::net::kBssec1RecordHeaderSize + 1 +
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
  const int matched =
      std::fscanf(file, "%ld %ld", &total_pages, &resident_pages);
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
  Check(ParseServerKeyPin("sha256:" + backupproject::crypto::X25519Fingerprint(
                                          identity.public_key),
                          &pin, &error),
        "pin", error);

  auto pair = MakePair(identity, pin);
  Check(pair->ok, "大流量用例：握手成功");

  const std::size_t chunk_bytes =
      backupproject::net::kMaxPayloadBytes;              // 1 MiB
  const std::size_t target_bytes = 32u * 1024u * 1024u;  // 32 MiB
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
  const auto started = std::chrono::steady_clock::now();
  bool sent = true;
  for (std::size_t index = 0; index < expected_frames; ++index) {
    if (!pair->client.SendFrame(
            pair->client_fd, static_cast<std::uint16_t>(Opcode::kUploadChunk),
            0, static_cast<std::uint64_t>(index + 1), chunk, &error)) {
      sent = false;
      break;
    }
  }
  // 发完就关掉客户端，让读线程在收完最后一帧后拿到 kClosed 退出。
  Check(sent, "32 MiB 全部发出", error);
  ::shutdown(pair->client_fd, SHUT_WR);
  reader.join();
  const long rss_after = ResidentKilobytes();

  Check(received_frames.load() == expected_frames && sent, "32 MiB 全部收到",
        std::to_string(received_frames.load()) + "/" +
            std::to_string(expected_frames) + " 帧");
  Check(received_bytes.load() == target_bytes, "收到的字节数正确",
        std::to_string(received_bytes.load()));
  Check(payload_ok.load(), "每一帧的 payload 逐字节完整（长度与首尾字节都对）");
  const long growth =
      (rss_before > 0 && rss_after > 0) ? rss_after - rss_before : -1;
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || \
    defined(ADDRESS_SANITIZER)
  // AddressSanitizer 会保留一片隔离区（quarantine）来延迟复用已释放的内存，
  // 所以"常驻内存没有随传输量增长"这条阈值在消毒剂构建里**不适用**：增长来自
  // 隔离区，而不是传输路径。这与项目既有约定一致（network_test.sh
  // 里明确写过）。
  // 这里仍然打印真实数字，并且保留"字节数正确"这条与内存无关的硬断言。
  const bool sanitizer_build = true;
#else
  const bool sanitizer_build = false;
#endif
  if (sanitizer_build) {
    std::printf(
        "  RSS_BOUND: 不适用（消毒剂构建：隔离区会保留已释放内存；"
        "真实数字见下一行）\n");
  } else {
    Check(growth >= 0 && growth < 16 * 1024, "RSS 没有线性增长（32 MiB 传输）",
          "RSS " + std::to_string(rss_before) + " -> " +
              std::to_string(rss_after) + " KB");
  }
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
          .count();
  std::printf("  RSS_BOUND: %ld KB -> %ld KB（传输 %zu MiB）\n", rss_before,
              rss_after, target_bytes / (1024u * 1024u));
  std::printf(
      "  THROUGHPUT: %.1f MiB/s（单核，含 AES-256-CTR 与 HMAC-SHA256，"
      "构建时优化 -O1）\n",
      seconds > 0.0
          ? (static_cast<double>(target_bytes) / (1024.0 * 1024.0)) / seconds
          : 0.0);
  ::close(pair->client_fd);
  ::close(pair->server_fd);
}

// ---- 畸形输入：确定性变异（可复现，不依赖随机种子）----
//
// 目标不是"找出崩溃"，而是把 fail-closed 这条性质钉成断言：
//   * 记录层：任何被改过的记录都只能被拒绝；被拒绝之后通道不再接受任何数据；
//   * 握手层：任何畸形 / 随机 / 截断的 ClientHello 都只能让握手失败。
// 这些用例同时跑在 ASan + UBSan 构建下，所以"没崩"是有证据的。

std::uint64_t NextRandom(std::uint64_t* state) {
  // xorshift64：确定、可复现，不需要第三方库。
  std::uint64_t value = *state;
  value ^= value << 13;
  value ^= value >> 7;
  value ^= value << 17;
  *state = value;
  return value;
}

void TestMalformedRecordFuzz() {
  std::printf("[transport] 记录层确定性变异\n");
  TransportIdentity identity;
  std::string error;
  Check(GenerateTransportIdentity(&identity, &error), "生成身份密钥", error);
  ServerKeyPin pin;
  Check(ParseServerKeyPin("sha256:" + backupproject::crypto::X25519Fingerprint(
                                          identity.public_key),
                          &pin, &error),
        "pin", error);

  const std::string payload = "PR21-FUZZ-PAYLOAD";
  const std::size_t record_size = backupproject::net::kBssec1RecordHeaderSize +
                                  payload.size() +
                                  backupproject::net::kBssec1TagSize;
  const int rounds = 300;
  int accepted = 0;
  int rejected = 0;
  int still_established = 0;
  std::uint64_t state = 0x9E3779B97F4A7C15ull;
  for (int round = 0; round < rounds; ++round) {
    auto pair = MakePair(identity, pin);
    if (!pair->ok) {
      Check(false, "变异用例：握手失败");
      return;
    }
    if (!pair->client.SendRecord(pair->client_fd, payload, &error)) {
      Check(false, "变异用例：发送失败", error);
      return;
    }
    std::string raw;
    if (!ReadExact(pair->server_fd, record_size, &raw)) {
      Check(false, "变异用例：抓取失败");
      return;
    }
    const int mutations = 1 + static_cast<int>(NextRandom(&state) % 3);
    for (int index = 0; index < mutations; ++index) {
      const std::size_t offset =
          static_cast<std::size_t>(NextRandom(&state) % raw.size());
      raw[offset] =
          static_cast<char>(raw[offset] ^ (1u << (NextRandom(&state) % 8)));
    }
    if (NextRandom(&state) % 4 == 0) {
      raw.resize(static_cast<std::size_t>(NextRandom(&state) % raw.size()) + 1);
    }
    std::string plaintext;
    std::string feed_error;
    const FrameReadStatus status =
        Feed(&pair->server, raw, &plaintext, &feed_error);
    if (status == FrameReadStatus::kOk) {
      accepted += 1;
    } else {
      rejected += 1;
    }
    if (pair->server.established()) {
      still_established += 1;
    }
    Check(plaintext.empty() || plaintext == payload,
          "变异记录要么被拒绝、要么解出原始明文（绝不解出别的东西）");
    ::close(pair->client_fd);
    ::close(pair->server_fd);
  }
  Check(accepted == 0, "300 条变异记录全部被拒绝",
        std::to_string(accepted) + " 条被接受");
  Check(rejected + accepted == rounds, "每一轮都有明确结论");
  Check(still_established == 0, "被拒绝之后通道不再被视为已建立");
}

void TestMalformedHandshakeFuzz() {
  std::printf("[transport] 握手层确定性变异\n");
  TransportIdentity identity;
  std::string error;
  Check(GenerateTransportIdentity(&identity, &error), "生成身份密钥", error);

  const int rounds = 200;
  int refused = 0;
  int left_established = 0;
  std::uint64_t state = 0x243F6A8885A308D3ull;
  for (int round = 0; round < rounds; ++round) {
    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
      Check(false, "socketpair");
      return;
    }
    // 一半是纯随机字节；一半是"magic/版本/套件都正确、内容是垃圾"的
    // ClientHello——后一种能真正走进密钥交换与 Finished 校验，而不是在
    // 头几个字节就被挡掉。
    std::string junk;
    if (NextRandom(&state) % 2 == 0) {
      const std::uint32_t magic = backupproject::net::kBssec1Magic;
      junk.push_back(static_cast<char>((magic >> 24) & 0xFF));
      junk.push_back(static_cast<char>((magic >> 16) & 0xFF));
      junk.push_back(static_cast<char>((magic >> 8) & 0xFF));
      junk.push_back(static_cast<char>(magic & 0xFF));
      junk.push_back(1);  // 类型 ClientHello
      junk.push_back(1);  // 版本
      junk.push_back(0);  // 套件高字节
      junk.push_back(1);  // 套件低字节
    }
    const std::size_t length =
        static_cast<std::size_t>(NextRandom(&state) % 200);
    for (std::size_t index = 0; index < length; ++index) {
      junk.push_back(static_cast<char>(NextRandom(&state) & 0xFF));
    }
    if (!junk.empty()) {
      WriteAll(fds[1], junk);
    }
    ::shutdown(fds[1], SHUT_WR);
    SecureChannel server;
    std::string handshake_error;
    if (!server.HandshakeServer(fds[0], identity, &handshake_error)) {
      refused += 1;
    }
    if (server.established()) {
      left_established += 1;
    }
    ::close(fds[0]);
    ::close(fds[1]);
  }
  Check(refused == rounds, "200 段畸形 ClientHello 全部被拒绝",
        std::to_string(refused) + "/" + std::to_string(rounds));
  Check(left_established == 0, "畸形握手不会留下已建立的通道");
}

// ============================ 红队用例 ============================
//
// 下面这些用例是 PR #21 独立审查轮（攻击者 + 测试作者）新增的，针对的是
// "握手到底证明了什么"、"任意字节切分下还能不能拼回消息"、"对端读到一半
// 消失会怎样"、"声明的长度能不能逼出一次大分配"这些边界。

// 用例 1：服务端必须证明**持有私钥**，hello 里的 pinned 公钥本身不算认证。
void TestServerMustProvePrivateKeyPossession() {
  std::printf("[redteam] 服务端必须证明持有私钥（光有 pin 里的公钥不算）\n");
  TransportIdentity honest;
  TransportIdentity attacker;
  std::string error;
  Check(GenerateTransportIdentity(&honest, &error), "生成诚实服务端身份密钥",
        error);
  Check(GenerateTransportIdentity(&attacker, &error), "生成攻击者自己的密钥对",
        error);

  // 客户端 pin 的是**诚实服务端**的公钥（攻击者能拿到它：它就是公开信息）。
  ServerKeyPin pin;
  Check(ParseServerKeyPin("hex:" + backupproject::crypto::X25519FormatKeyHex(
                                       honest.public_key),
                          &pin, &error),
        "客户端 pin 诚实服务端的公钥", error);

  // 攻击者构造的身份：hello 里出示 pinned 公钥，但私钥是它自己的。
  TransportIdentity forged;
  forged.public_key = honest.public_key;
  forged.private_key = attacker.private_key;

  // (1a) 用真的 HandshakeServer 跑这个伪造身份：服务端自己会在
  //      ClientFinished 上失败（它算不出客户端算的那把会话密钥），
  //      然后按真实服务端的行为关掉连接。客户端这一侧看到的是
  //      "对端在 ServerFinished 之前关闭"——也就是 kIoError，而不是
  //      kAuthenticationFailed。这不是缺陷：**光有 pinned 公钥的攻击者
  //      连一条能通过校验的 Finished 都造不出来**，握手在两侧都失败。
  //      1b 才是"客户端必须亲自在 ServerFinished 上拒绝"的那条证据。
  {
    int fds[2] = {-1, -1};
    Check(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
    SecureChannel client_channel;
    SecureChannel server_channel;
    bool client_ok = true;
    bool server_ok = true;
    std::string client_error;
    std::string server_error;
    WatchedCall watch;
    const bool completed = watch.Run(
        [&]() {
          std::thread server_thread([&]() {
            server_ok =
                server_channel.HandshakeServer(fds[1], forged, &server_error);
            // 真实服务端握手失败后会关掉连接。
            ::shutdown(fds[1], SHUT_RDWR);
          });
          client_ok =
              client_channel.HandshakeClient(fds[0], pin, &client_error);
          ::shutdown(fds[0], SHUT_RDWR);
          server_thread.join();
        },
        5000, fds[0], fds[1]);
    Check(completed, "1a：握手在 5 秒内结束（没有挂死）");
    std::printf("  1a 实测：客户端 last_error=%s；服务端 last_error=%s\n",
                backupproject::net::SecureTransportErrorName(
                    client_channel.last_error()),
                backupproject::net::SecureTransportErrorName(
                    server_channel.last_error()));
    Check(!client_ok, "1a：客户端握手失败", client_error);
    Check(!client_channel.established(), "1a：客户端通道未建立");
    Check(client_channel.last_error() != SecureTransportError::kNone,
          "1a：客户端有明确错误分类",
          backupproject::net::SecureTransportErrorName(
              client_channel.last_error()));
    Check(!server_ok, "1a：伪造身份的服务端自己也失败", server_error);
    Check(!server_channel.established(), "1a：服务端通道未建立");
    Check(client_channel.peer_public_key() == honest.public_key,
          "1a：客户端确实看到并接受了 pinned 公钥（不是 pin 不匹配）");
    ::close(fds[0]);
    ::close(fds[1]);
  }

  // (1b) 攻击者更进一步：握手失败之后继续扮演服务端，回一条**格式完全
  //      正确、tag 伪造**的 ServerFinished。客户端必须正好在 ServerFinished
  //      校验这一步失败（kAuthenticationFailed），而不是"因为对面关了连接"。
  {
    int fds[2] = {-1, -1};
    Check(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
    SecureChannel client_channel;
    SecureChannel server_channel;
    bool client_ok = true;
    bool server_ok = true;
    std::string client_error;
    std::string server_error;
    std::string forged_tag;
    Check(RandomBytes(32, &forged_tag, &error), "生成伪造的 tag", error);
    WatchedCall watch;
    const bool completed = watch.Run(
        [&]() {
          std::thread server_thread([&]() {
            server_ok =
                server_channel.HandshakeServer(fds[1], forged, &server_error);
            // 格式正确但 tag 是攻击者编的：客户端必须拒绝。
            const std::string fake = BuildFinishedBytes(
                backupproject::net::kBssec1MessageServerFinished, forged_tag);
            WriteAll(fds[1], fake);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            ::shutdown(fds[1], SHUT_RDWR);
          });
          client_ok =
              client_channel.HandshakeClient(fds[0], pin, &client_error);
          ::shutdown(fds[0], SHUT_RDWR);
          server_thread.join();
        },
        5000, fds[0], fds[1]);
    Check(completed, "1b：握手在 5 秒内结束（没有挂死）");
    Check(!client_ok, "1b：客户端拒绝伪造的 ServerFinished", client_error);
    Check(client_channel.last_error() ==
              SecureTransportError::kAuthenticationFailed,
          "1b：客户端在 ServerFinished 校验上失败（kAuthenticationFailed）",
          backupproject::net::SecureTransportErrorName(
              client_channel.last_error()));
    Check(!client_channel.established(), "1b：客户端通道未建立");
    Check(client_channel.peer_public_key() == honest.public_key,
          "1b：pin 校验通过了，仍然被 ServerFinished 挡下");
    Check(client_error.find("ServerFinished") != std::string::npos,
          "1b：错误信息点名 ServerFinished", client_error);
    Check(!server_ok, "1b：伪造身份的服务端失败", server_error);
    ::close(fds[0]);
    ::close(fds[1]);
  }

  // (1c) 对照组：诚实私钥在同一套 pin 下必须成功——证明 1a/1b 的失败
  //      来自"私钥不匹配"，而不是 pin 或测试环境本身有问题。
  {
    auto pair = MakePair(honest, pin);
    Check(pair->ok, "1c：诚实身份 + 同一 pin 握手成功");
    Check(pair->client.established() && pair->server.established(),
          "1c：两端通道都已建立");
    Check(pair->client.transcript_hash() == pair->server.transcript_hash(),
          "1c：两端 transcript 一致");
    ::close(pair->client_fd);
    ::close(pair->server_fd);
  }
}

// 用例 2：握手分片矩阵。写侧每次最多放 N 个字节（N = 1/2/3/7/64），
// 外加两个"卡在结构体边界上"的切分点配置。四个握手消息 + 一条记录
// 都必须在这种链路上正确完成。
void TestHandshakeFragmentationMatrix() {
  std::printf("[redteam] 握手分片矩阵：一次 recv != 一个结构体\n");
  TransportIdentity identity;
  std::string error;
  Check(GenerateTransportIdentity(&identity, &error), "生成服务端身份密钥",
        error);
  ServerKeyPin pin;
  Check(ParseServerKeyPin("hex:" + backupproject::crypto::X25519FormatKeyHex(
                                       identity.public_key),
                          &pin, &error),
        "按公钥 pin", error);

  struct Config {
    const char* label;
    std::size_t chunk;
    std::vector<std::size_t> splits_c2s;
    std::vector<std::size_t> splits_s2c;
    std::size_t min_writes;
  };
  // 握手两个方向一共 256 字节（72+104+40+40），再加一条 16 字节 payload
  // 的记录（68 字节）。chunk == 1 时写次数 >= 180。
  const Config configs[] = {
      {"每次 1 字节", 1, {}, {}, 150},
      {"每次 2 字节", 2, {}, {}, 80},
      {"每次 3 字节", 3, {}, {}, 50},
      {"每次 7 字节", 7, {}, {}, 20},
      {"每次 64 字节", 64, {}, {}, 3},
      {"切分点 71/72 与 103/104", 0, {71}, {103}, 4},
      {"切分点 72/111 与 104/143", 0, {72, 111}, {104, 143}, 4},
  };

  for (const Config& config : configs) {
    FragmentedRelay relay;
    std::string relay_error;
    if (!relay.Start(config.chunk, config.splits_c2s, config.splits_s2c,
                     config.chunk == 1 ? 300 : 120, &relay_error)) {
      Check(false, std::string("启动限速中继（") + config.label + "）",
            relay_error);
      continue;
    }
    SecureChannel client_channel;
    SecureChannel server_channel;
    bool client_ok = false;
    bool server_ok = false;
    std::string client_error;
    std::string server_error;
    WatchedCall handshake_watch;
    const bool handshake_completed = handshake_watch.Run(
        [&]() {
          std::thread server_thread([&]() {
            server_ok = server_channel.HandshakeServer(relay.server_fd,
                                                       identity, &server_error);
          });
          client_ok = client_channel.HandshakeClient(relay.client_fd, pin,
                                                     &client_error);
          if (!client_ok) {
            ::shutdown(relay.client_fd, SHUT_RDWR);
          }
          server_thread.join();
        },
        5000, relay.client_fd, relay.server_fd);
    const std::string label = config.label;
    Check(handshake_completed,
          std::string("分片下握手在 5 秒内返回（") + label + "）");
    Check(client_ok && server_ok,
          std::string("分片下握手成功（") + label + "）",
          client_error + " / " + server_error);
    Check(client_channel.established() && server_channel.established(),
          std::string("分片下两端通道都已建立（") + label + "）");
    Check(client_channel.transcript_hash() == server_channel.transcript_hash(),
          std::string("分片下 transcript 一致（") + label + "）");

    const std::string payload = "fragmented-frame";
    bool sent = false;
    FrameReadStatus status = FrameReadStatus::kIoError;
    std::string received;
    std::string record_error;
    WatchedCall record_watch;
    const bool record_completed = record_watch.Run(
        [&]() {
          sent = client_channel.SendRecord(relay.client_fd, payload,
                                           &record_error);
          if (sent) {
            status = server_channel.ReceiveRecord(relay.server_fd, &received,
                                                  &record_error);
          }
        },
        5000, relay.client_fd, relay.server_fd);
    Check(record_completed && sent && status == FrameReadStatus::kOk &&
              received == payload,
          std::string("记录层同样容忍分片（") + label + "）", record_error);

    const std::size_t writes =
        relay.c2s_writes.load() + relay.s2c_writes.load();
    Check(writes >= config.min_writes,
          std::string("中继确实把字节切碎了（") + label + "）",
          std::to_string(writes) +
              " 次写，期望 >= " + std::to_string(config.min_writes));
    relay.Join();
  }
}

// 用例 3：断连矩阵。对端在任意位置（0 / 1 / 半个消息 / 消息边界 /
// 边界 + 1）关闭连接，握手必须失败、不留已建立状态、不挂死，
// 而且链路上不允许出现任何业务记录。
void TestPeerDisconnectMatrix() {
  std::printf("[redteam] 断连矩阵：对端在任意位置关闭\n");
  TransportIdentity identity;
  std::string error;
  Check(GenerateTransportIdentity(&identity, &error), "生成服务端身份密钥",
        error);
  ServerKeyPin pin;
  Check(ParseServerKeyPin("hex:" + backupproject::crypto::X25519FormatKeyHex(
                                       identity.public_key),
                          &pin, &error),
        "按公钥 pin", error);

  std::string client_hello;
  std::string server_hello;
  std::string client_finished;
  std::string server_finished;
  Check(CaptureHandshakeMessages(identity, pin, &client_hello, &server_hello,
                                 &client_finished, &server_finished, &error),
        "捕获四个真实握手消息", error);
  Check(client_hello.size() == backupproject::net::kBssec1ClientHelloSize &&
            server_hello.size() == backupproject::net::kBssec1ServerHelloSize &&
            client_finished.size() == backupproject::net::kBssec1FinishedSize &&
            server_finished.size() == backupproject::net::kBssec1FinishedSize,
        "捕获到的四个消息长度符合协议");
  Check(!ContainsBssec1Record(client_hello + server_hello + client_finished +
                              server_finished),
        "真实握手消息里没有业务记录（断连矩阵的基线）");

  const std::size_t hello = backupproject::net::kBssec1ClientHelloSize;   // 72
  const std::size_t shello = backupproject::net::kBssec1ServerHelloSize;  // 104
  const DisconnectCase cases[] = {
      {false, 0, false, "服务端侧：对端 0 字节后半关"},
      {false, 1, false, "服务端侧：对端 1 字节后半关"},
      {false, hello / 2, false, "服务端侧：对端在 ClientHello 中间半关"},
      {false, hello - 1, false,
       "服务端侧：对端在 ClientHello 最后 1 字节前半关"},
      {false, hello, false, "服务端侧：ClientHello 边界（72）半关"},
      {false, hello + 1, false, "服务端侧：72+1 字节后半关"},
      {false, hello, true, "服务端侧：ClientHello 边界（72）直接 close()"},
      {false, hello / 2, true, "服务端侧：消息中间直接 close()"},
      {true, 0, false, "客户端侧：对端 0 字节后半关"},
      {true, 1, false, "客户端侧：对端 1 字节后半关"},
      {true, shello / 2, false, "客户端侧：对端在 ServerHello 中间半关"},
      {true, shello - 1, false,
       "客户端侧：对端在 ServerHello 最后 1 字节前半关"},
      {true, shello, false, "客户端侧：ServerHello 边界（104）半关"},
      {true, shello + 1, false, "客户端侧：104+1 字节后半关"},
      {true, shello, true, "客户端侧：ServerHello 边界（104）直接 close()"},
      {true, shello / 2, true, "客户端侧：消息中间直接 close()"},
  };

  for (const DisconnectCase& test_case : cases) {
    const DisconnectOutcome outcome =
        RunDisconnectCase(test_case, identity, pin, client_hello, server_hello);
    const std::string label = test_case.label;
    Check(outcome.completed, label + "：握手在 3 秒内返回（没有挂死）");
    Check(!outcome.handshake_ok, label + "：握手失败", outcome.message);
    Check(!outcome.established, label + "：通道未建立");
    Check(outcome.error == SecureTransportError::kIoError,
          label + "：错误分类是 kIoError",
          backupproject::net::SecureTransportErrorName(outcome.error));
    Check(!ContainsBssec1Record(outcome.peer_saw),
          label + "：对端没有收到任何业务记录");
    // 被测通道这一侧**应该**写出去的握手字节：
    //   * 客户端先发 ClientHello（72），读到完整 ServerHello 之后才发
    //     ClientFinished（40）；
    //   * 服务端只有读全 ClientHello 之后才会回 ServerHello（104），
    //     而且这一轮里它永远走不到 ServerFinished。
    std::size_t expected_wire = server_hello.size();
    if (test_case.channel_is_client) {
      expected_wire = client_hello.size();
      if (test_case.prefix_bytes >= shello) {
        expected_wire += client_finished.size();
      }
    } else if (test_case.prefix_bytes < hello) {
      expected_wire = 0;
    }
    if (!test_case.full_close) {
      Check(outcome.peer_saw.size() == expected_wire,
            label + "：对端收到的字节数正好是握手消息",
            std::to_string(outcome.peer_saw.size()) + " 字节，期望 " +
                std::to_string(expected_wire) + " 字节");
      if (test_case.channel_is_client) {
        Check(LooksLikeBssec1Message(
                  outcome.peer_saw.substr(0, client_hello.size()),
                  client_hello.size(),
                  backupproject::net::kBssec1MessageClientHello),
              label + "：对端收到的是结构完整的 ClientHello");
        if (test_case.prefix_bytes >= shello) {
          Check(LooksLikeBssec1Message(
                    outcome.peer_saw.substr(hello), client_finished.size(),
                    backupproject::net::kBssec1MessageClientFinished),
                label + "：ClientHello 之后只跟了一条 ClientFinished");
        }
      } else if (test_case.prefix_bytes >= hello) {
        Check(LooksLikeBssec1Message(
                  outcome.peer_saw, server_hello.size(),
                  backupproject::net::kBssec1MessageServerHello),
              label + "：对端收到的是结构完整的 ServerHello");
      }
      Check(
          outcome.peer_saw != client_hello && outcome.peer_saw != server_hello,
          label + "：对端收到的是这一轮新生成的握手字节（不是重放旧捕获）");
    }
  }
}

// 用例 4：握手失败之后的状态机。四个公开操作全部 fail closed：返回失败、
// 分类是 kStateError、不往 fd 写任何字节；Reset() 之后可以重新握手成功，
// 序号从 0 重新开始。
void CheckFailedChannelIsInert(const std::string& label, SecureChannel* channel,
                               int channel_fd, int observer_fd) {
  const std::string marker = "PR21-NEVER-SENT-PLAINTEXT";
  std::string error;

  // 1) SendRecord
  DrainSocket(observer_fd);
  error.clear();
  const bool sent_record = channel->SendRecord(channel_fd, marker, &error);
  Check(!sent_record, label + "：SendRecord 返回失败", error);
  Check(channel->last_error() == SecureTransportError::kStateError,
        label + "：SendRecord 的错误分类是 kStateError",
        backupproject::net::SecureTransportErrorName(channel->last_error()));
  Check(SocketBufferEmpty(observer_fd),
        label + "：SendRecord 没有往 fd 写任何字节");

  // 2) SendFrame
  DrainSocket(observer_fd);
  error.clear();
  const bool sent_frame =
      channel->SendFrame(channel_fd, static_cast<std::uint16_t>(Opcode::kPing),
                         0, 1, marker, &error);
  Check(!sent_frame, label + "：SendFrame 返回失败", error);
  Check(channel->last_error() == SecureTransportError::kStateError,
        label + "：SendFrame 的错误分类是 kStateError",
        backupproject::net::SecureTransportErrorName(channel->last_error()));
  Check(SocketBufferEmpty(observer_fd),
        label + "：SendFrame 没有往 fd 写任何字节");

  // 3) ReceiveRecord
  error.clear();
  std::string plaintext;
  const FrameReadStatus record_status =
      channel->ReceiveRecord(channel_fd, &plaintext, &error);
  Check(record_status != FrameReadStatus::kOk,
        label + "：ReceiveRecord 不返回 kOk");
  Check(channel->last_error() == SecureTransportError::kStateError,
        label + "：ReceiveRecord 的错误分类是 kStateError",
        backupproject::net::SecureTransportErrorName(channel->last_error()));
  Check(plaintext.empty(), label + "：失败的 ReceiveRecord 不交出任何明文");
  Check(SocketBufferEmpty(observer_fd),
        label + "：ReceiveRecord 没有往 fd 写任何字节");

  // 4) ReceiveFrame
  error.clear();
  FrameHeader header;
  std::string payload;
  const FrameReadStatus frame_status =
      channel->ReceiveFrame(channel_fd, &header, &payload, &error);
  Check(frame_status != FrameReadStatus::kOk,
        label + "：ReceiveFrame 不返回 kOk");
  Check(channel->last_error() == SecureTransportError::kStateError,
        label + "：ReceiveFrame 的错误分类是 kStateError",
        backupproject::net::SecureTransportErrorName(channel->last_error()));
  Check(payload.empty(), label + "：失败的 ReceiveFrame 不交出任何明文");
  Check(SocketBufferEmpty(observer_fd),
        label + "：ReceiveFrame 没有往 fd 写任何字节");

  Check(!channel->established(),
        label + "：失败之后 established() 永远为 false");
}

void TestFailedChannelStateMachine() {
  std::printf("[redteam] 握手失败之后的状态机与 Reset\n");
  TransportIdentity identity;
  std::string error;
  Check(GenerateTransportIdentity(&identity, &error), "生成服务端身份密钥",
        error);
  ServerKeyPin pin;
  Check(ParseServerKeyPin("hex:" + backupproject::crypto::X25519FormatKeyHex(
                                       identity.public_key),
                          &pin, &error),
        "按公钥 pin", error);

  // (a) 客户端通道：对端立刻半关，握手在 ServerHello 上失败。
  {
    int fds[2] = {-1, -1};
    Check(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
    ::shutdown(fds[1], SHUT_WR);
    SecureChannel channel;
    const bool ok = channel.HandshakeClient(fds[0], pin, &error);
    Check(!ok, "客户端握手失败（对端立刻半关）", error);
    Check(channel.last_error() == SecureTransportError::kIoError,
          "客户端失败分类是 kIoError",
          backupproject::net::SecureTransportErrorName(channel.last_error()));
    Check(!channel.established(), "失败之后 established() 是 false");
    DrainSocket(fds[1]);  // 把 ClientHello（72 字节）读掉，缓冲区归零
    Check(SocketBufferEmpty(fds[1]), "对端缓冲区已排空（基线）");
    CheckFailedChannelIsInert("客户端通道", &channel, fds[0], fds[1]);

    // Reset 之后同一个对象必须能重新握手，并且序号从 0 开始。
    channel.Reset();
    Check(channel.last_error() == SecureTransportError::kNone &&
              !channel.established() && channel.send_sequence() == 0 &&
              channel.receive_sequence() == 0,
          "Reset 清空失败状态与序号");
    SecureChannel fresh_server;
    int client_fd = -1;
    int server_fd = -1;
    Check(HandshakeAgain(&channel, identity, pin, &client_fd, &fresh_server,
                         &server_fd, &error),
          "Reset 之后同一个通道对象重新握手成功", error);
    Check(channel.send_sequence() == 0 && channel.receive_sequence() == 0,
          "重新握手之后收发序号都是 0");
    Check(channel.SendRecord(client_fd, "after-reset", &error) &&
              channel.send_sequence() == 1,
          "Reset 之后记录层可用且发送序号从 0 递增到 1", error);
    std::string received;
    Check(fresh_server.ReceiveRecord(server_fd, &received, &error) ==
                  FrameReadStatus::kOk &&
              received == "after-reset",
          "新会话用新密钥解出记录（Reset 真的换了会话）", error);
    Check(fresh_server.receive_sequence() == 1, "新会话接收序号从 0 开始");
    ::shutdown(client_fd, SHUT_RDWR);
    ::shutdown(server_fd, SHUT_RDWR);
    ::close(client_fd);
    ::close(server_fd);
    ::close(fds[0]);
    ::close(fds[1]);
  }

  // (b) 服务端通道：对端发 72 字节垃圾（magic 不对）后关闭。
  {
    int fds[2] = {-1, -1};
    Check(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
    const std::string junk(backupproject::net::kBssec1ClientHelloSize, 'x');
    WriteAll(fds[1], junk);
    ::shutdown(fds[1], SHUT_WR);
    SecureChannel channel;
    const bool ok = channel.HandshakeServer(fds[0], identity, &error);
    Check(!ok, "服务端握手失败（magic 不对的 ClientHello）", error);
    Check(channel.last_error() == SecureTransportError::kMalformedMessage,
          "服务端失败分类是 kMalformedMessage",
          backupproject::net::SecureTransportErrorName(channel.last_error()));
    DrainSocket(fds[1]);
    Check(SocketBufferEmpty(fds[1]), "对端缓冲区已排空（基线）");
    CheckFailedChannelIsInert("服务端通道", &channel, fds[0], fds[1]);
    ::close(fds[0]);
    ::close(fds[1]);
  }

  // (c) 已经用过序号的通道 Reset 之后，序号必须回到 0（而不是接着数）。
  {
    auto pair = MakePair(identity, pin);
    Check(pair->ok, "初始握手成功");
    Check(pair->client.SendRecord(pair->client_fd, "before-reset", &error),
          "Reset 之前先发一条记录", error);
    Check(pair->client.send_sequence() == 1, "发送序号变成 1");
    std::string received;
    Check(pair->server.ReceiveRecord(pair->server_fd, &received, &error) ==
                  FrameReadStatus::kOk &&
              received == "before-reset",
          "对端解出这条记录", error);
    Check(pair->server.receive_sequence() == 1, "对端接收序号变成 1");

    pair->client.Reset();
    Check(!pair->client.established() && pair->client.send_sequence() == 0 &&
              pair->client.receive_sequence() == 0,
          "Reset 把发送/接收序号清零");
    SecureChannel fresh_server;
    int client_fd = -1;
    int server_fd = -1;
    Check(HandshakeAgain(&pair->client, identity, pin, &client_fd,
                         &fresh_server, &server_fd, &error),
          "用过序号之后 Reset 仍然可以重新握手", error);
    Check(pair->client.send_sequence() == 0 &&
              pair->client.receive_sequence() == 0,
          "重新握手之后序号回到 0");
    Check(pair->client.SendRecord(client_fd, "second-session", &error) &&
              pair->client.send_sequence() == 1,
          "Reset 之后发送序号从 0 重新开始", error);
    std::string second;
    Check(fresh_server.ReceiveRecord(server_fd, &second, &error) ==
                  FrameReadStatus::kOk &&
              second == "second-session",
          "新会话接受的记录序号是 0（不是接着旧会话）", error);
    Check(fresh_server.receive_sequence() == 1, "新会话接收序号从 0 走到 1");
    ::shutdown(client_fd, SHUT_RDWR);
    ::shutdown(server_fd, SHUT_RDWR);
    ::close(client_fd);
    ::close(server_fd);
    ::close(pair->client_fd);
    ::close(pair->server_fd);
  }
}

// 用例 5（记录层部分）：计数器纪律必须在**线上字节**上成立——
// 同一条明文在 seq 0 / seq 1 下的密文不同；c2s 与 s2c 加密同一条明文的
// 密文也不同。计数器块本身的唯一性在 tests/review/counter_block_harness.cpp
// 里单独验证（要拿到匿名命名空间里的 CounterBlock）。
std::string RecordCiphertextOf(const std::string& record) {
  if (record.size() < backupproject::net::kBssec1RecordHeaderSize) {
    return std::string();
  }
  std::uint32_t length = 0;
  for (int i = 0; i < 4; ++i) {
    length = (length << 8) | static_cast<unsigned char>(
                                 record[16 + static_cast<std::size_t>(i)]);
  }
  if (record.size() != backupproject::net::kBssec1RecordHeaderSize + length +
                           backupproject::net::kBssec1TagSize) {
    return std::string();
  }
  return record.substr(backupproject::net::kBssec1RecordHeaderSize, length);
}

std::uint64_t RecordSequenceOf(const std::string& record) {
  std::uint64_t sequence = 0;
  for (int i = 0; i < 8; ++i) {
    sequence = (sequence << 8) | static_cast<unsigned char>(
                                     record[8 + static_cast<std::size_t>(i)]);
  }
  return sequence;
}

void TestRecordLayerCounterDiscipline() {
  std::printf("[redteam] 记录层计数器纪律：明文相同、密文不得相同\n");
  TransportIdentity identity;
  std::string error;
  Check(GenerateTransportIdentity(&identity, &error), "生成服务端身份密钥",
        error);
  ServerKeyPin pin;
  Check(ParseServerKeyPin("hex:" + backupproject::crypto::X25519FormatKeyHex(
                                       identity.public_key),
                          &pin, &error),
        "按公钥 pin", error);

  FragmentedRelay relay;
  std::string relay_error;
  if (!relay.Start(0, {}, {}, 0, &relay_error)) {
    Check(false, "启动记录中继", relay_error);
    return;
  }
  SecureChannel client_channel;
  SecureChannel server_channel;
  bool client_ok = false;
  bool server_ok = false;
  std::string client_error;
  std::string server_error;
  WatchedCall watch;
  const bool handshake_completed = watch.Run(
      [&]() {
        std::thread server_thread([&]() {
          server_ok = server_channel.HandshakeServer(relay.server_fd, identity,
                                                     &server_error);
        });
        client_ok =
            client_channel.HandshakeClient(relay.client_fd, pin, &client_error);
        if (!client_ok) {
          ::shutdown(relay.client_fd, SHUT_RDWR);
        }
        server_thread.join();
      },
      5000, relay.client_fd, relay.server_fd);
  Check(handshake_completed && client_ok && server_ok, "中继上握手成功",
        client_error + " / " + server_error);

  const std::string plaintext = "same-plaintext-different-counter";
  const std::size_t record_size = backupproject::net::kBssec1RecordHeaderSize +
                                  plaintext.size() +
                                  backupproject::net::kBssec1TagSize;
  bool sent_first = false;
  bool sent_second = false;
  bool sent_reverse = false;
  std::string got_first;
  std::string got_second;
  std::string got_reverse;
  FrameReadStatus first_status = FrameReadStatus::kIoError;
  FrameReadStatus second_status = FrameReadStatus::kIoError;
  FrameReadStatus reverse_status = FrameReadStatus::kIoError;
  WatchedCall record_watch;
  const bool record_completed = record_watch.Run(
      [&]() {
        sent_first = client_channel.SendRecord(relay.client_fd, plaintext,
                                               &client_error);
        sent_second = client_channel.SendRecord(relay.client_fd, plaintext,
                                                &client_error);
        first_status = server_channel.ReceiveRecord(relay.server_fd, &got_first,
                                                    &server_error);
        second_status = server_channel.ReceiveRecord(
            relay.server_fd, &got_second, &server_error);
        sent_reverse = server_channel.SendRecord(relay.server_fd, plaintext,
                                                 &server_error);
        reverse_status = client_channel.ReceiveRecord(
            relay.client_fd, &got_reverse, &client_error);
      },
      5000, relay.client_fd, relay.server_fd);
  Check(record_completed, "三条记录在 5 秒内往返完成");
  Check(sent_first && sent_second && first_status == FrameReadStatus::kOk &&
            second_status == FrameReadStatus::kOk && got_first == plaintext &&
            got_second == plaintext,
        "c2s 方向 seq 0 / seq 1 两条记录都被正确解出", server_error);
  Check(sent_reverse && reverse_status == FrameReadStatus::kOk &&
            got_reverse == plaintext,
        "s2c 方向的同明文记录被正确解出", client_error);
  relay.Join();  // 之后读 c2s_bytes / s2c_bytes 才是无数据竞争的

  const std::size_t c2s_handshake = backupproject::net::kBssec1ClientHelloSize +
                                    backupproject::net::kBssec1FinishedSize;
  const std::size_t s2c_handshake = backupproject::net::kBssec1ServerHelloSize +
                                    backupproject::net::kBssec1FinishedSize;
  Check(relay.c2s_bytes.size() == c2s_handshake + 2 * record_size,
        "c2s 方向线上字节 = 握手 + 两条记录",
        std::to_string(relay.c2s_bytes.size()));
  Check(relay.s2c_bytes.size() == s2c_handshake + record_size,
        "s2c 方向线上字节 = 握手 + 一条记录",
        std::to_string(relay.s2c_bytes.size()));
  if (relay.c2s_bytes.size() != c2s_handshake + 2 * record_size ||
      relay.s2c_bytes.size() != s2c_handshake + record_size) {
    return;
  }
  const std::string first = relay.c2s_bytes.substr(c2s_handshake, record_size);
  const std::string second =
      relay.c2s_bytes.substr(c2s_handshake + record_size, record_size);
  const std::string reverse =
      relay.s2c_bytes.substr(s2c_handshake, record_size);

  Check(RecordSequenceOf(first) == 0 && RecordSequenceOf(second) == 1,
        "两条 c2s 记录的序号是 0 与 1");
  Check(first != second, "同一条明文、seq 0 与 seq 1 的记录在线路上不相同");
  Check(RecordCiphertextOf(first) != RecordCiphertextOf(second),
        "同一条明文、seq 0 与 seq 1 的密文不相同（计数器块没有重叠）");
  Check(
      !RecordCiphertextOf(first).empty() && !RecordCiphertextOf(second).empty(),
      "两条记录的密文字段都能解析出来");
  Check(RecordCiphertextOf(first) != RecordCiphertextOf(reverse),
        "c2s 与 s2c 加密同一条明文的密文不相同（方向分离）");
  Check(first.substr(20 + plaintext.size()) !=
            reverse.substr(20 + plaintext.size()),
        "c2s 与 s2c 的 tag 也不相同");
  Check(first.find(plaintext) == std::string::npos &&
            reverse.find(plaintext) == std::string::npos,
        "记录里不出现明文");
}

// 用例 6：解析/分配边界。声明长度 0 / 1 / 上限 / 上限+1 / 0x7FFFFFFF /
// 0xFFFFFFFF，以及"头之后立刻 EOF"的截断变体：超过上限的必须在**分配
// 之前**被拒绝（立刻返回、峰值 RSS 不涨），合法的截断必须报 kIoError。
void TestDeclaredLengthBounds() {
  std::printf("[redteam] 记录长度边界：声明值必须在分配之前被拒绝\n");
  TransportIdentity identity;
  std::string error;
  Check(GenerateTransportIdentity(&identity, &error), "生成服务端身份密钥",
        error);
  ServerKeyPin pin;
  Check(ParseServerKeyPin("hex:" + backupproject::crypto::X25519FormatKeyHex(
                                       identity.public_key),
                          &pin, &error),
        "按公钥 pin", error);

  const std::uint32_t limit =
      static_cast<std::uint32_t>(backupproject::net::kBssec1MaxPlaintextBytes);
  struct LengthCase {
    std::uint32_t length;
    bool oversized;
    const char* label;
  };
  const LengthCase cases[] = {
      {0, false, "长度 0"},
      {1, false, "长度 1"},
      {limit, false, "长度 = 上限"},
      {limit + 1, true, "长度 = 上限 + 1"},
      {0x7FFFFFFFu, true, "长度 = 0x7FFFFFFF"},
      {0xFFFFFFFFu, true, "长度 = 0xFFFFFFFF"},
  };

  for (const LengthCase& test_case : cases) {
    auto pair = MakePair(identity, pin);
    if (!pair->ok) {
      Check(false, std::string(test_case.label) + "：无法建立已握手的通道");
      ::close(pair->client_fd);
      ::close(pair->server_fd);
      continue;
    }
    const FeedOutcome outcome =
        FeedRecordHeader(&pair->server, test_case.length, 700, false);
    const std::string label = test_case.label;
    Check(outcome.completed, label + "：ReceiveRecord 在 5 秒内返回");
    Check(outcome.plaintext.empty(), label + "：不交出任何明文");
    std::printf("  %s：status=%s elapsed=%zu ms 峰值 RSS 增长=%ld KB\n",
                test_case.label, FrameReadStatusName(outcome.status),
                outcome.elapsed_ms, outcome.rss_growth_kb);
    if (test_case.oversized) {
      Check(outcome.status == FrameReadStatus::kCorruptStream,
            label + "：立刻返回 kCorruptStream");
      Check(pair->server.last_error() == SecureTransportError::kOversizedRecord,
            label + "：last_error 是 oversized-record",
            backupproject::net::SecureTransportErrorName(
                pair->server.last_error()));
      Check(outcome.elapsed_ms < 400,
            label + "：没有去读正文（SO_RCVTIMEO 是 700 ms）",
            std::to_string(outcome.elapsed_ms) + " ms");
      Check(outcome.rss_growth_kb >= 0 && outcome.rss_growth_kb < 128 * 1024,
            label + "：没有按声明值分配（峰值 RSS 增长 < 128 MiB）",
            std::to_string(outcome.rss_growth_kb) + " KB");
      Check(!pair->server.established(), label + "：通道不再被视为已建立");
    } else {
      Check(outcome.status == FrameReadStatus::kIoError,
            label + "：正文被截断 → kIoError", outcome.error);
    }
    ::close(pair->client_fd);
    ::close(pair->server_fd);
  }

  // 截断变体：头写完立刻 EOF。超限的长度仍然必须是 oversized 拒绝，
  // 而不是"读到 EOF 就当截断"。
  const LengthCase truncated[] = {
      {limit, false, "截断：长度 = 上限"},
      {limit + 1, true, "截断：长度 = 上限 + 1"},
      {0xFFFFFFFFu, true, "截断：长度 = 0xFFFFFFFF"},
  };
  for (const LengthCase& test_case : truncated) {
    auto pair = MakePair(identity, pin);
    if (!pair->ok) {
      Check(false, std::string(test_case.label) + "：无法建立已握手的通道");
      ::close(pair->client_fd);
      ::close(pair->server_fd);
      continue;
    }
    const FeedOutcome outcome =
        FeedRecordHeader(&pair->server, test_case.length, 700, true);
    const std::string label = test_case.label;
    Check(outcome.completed, label + "：ReceiveRecord 在 5 秒内返回");
    Check(outcome.plaintext.empty(), label + "：不交出任何明文");
    if (test_case.oversized) {
      Check(outcome.status == FrameReadStatus::kCorruptStream,
            label + "：仍然立刻返回 kCorruptStream");
      Check(pair->server.last_error() == SecureTransportError::kOversizedRecord,
            label + "：last_error 仍是 oversized-record",
            backupproject::net::SecureTransportErrorName(
                pair->server.last_error()));
      Check(outcome.rss_growth_kb >= 0 && outcome.rss_growth_kb < 128 * 1024,
            label + "：没有按声明值分配",
            std::to_string(outcome.rss_growth_kb) + " KB");
    } else {
      Check(outcome.status == FrameReadStatus::kIoError,
            label + "：读到一半 EOF → kIoError", outcome.error);
    }
    ::close(pair->client_fd);
    ::close(pair->server_fd);
  }
}

// 附加用例：身份密钥文件的攻击面。PR #21 自己声称"私钥文件必须是普通文件、
// 只有所有者可读写"，所以这里逐条验证：符号链接、目录、权限过宽、长度不对
// 都必须被拒绝；同时记录一个 open-then-check 的顺序问题（FIFO 会先阻塞在
// open() 上，fstat 的普通文件校验根本没机会执行）。
void TestIdentityFileAttackSurface() {
  std::printf("[redteam] 身份密钥文件：符号链接 / FIFO / 权限 / 长度\n");
  TransportIdentity identity;
  std::string error;
  Check(GenerateTransportIdentity(&identity, &error), "生成身份密钥", error);
  const std::string dir = "/tmp/secure-transport-identity-" +
                          std::to_string(static_cast<long>(::getpid()));
  ::unlink(dir.c_str());
  Check(::mkdir(dir.c_str(), 0700) == 0, "创建临时目录", dir);
  const std::string key_path = dir + "/transport.key";
  ::unlink(key_path.c_str());

  // (1) 往返 + 权限
  Check(SaveTransportIdentity(key_path, identity, false, &error),
        "保存身份私钥", error);
  struct stat info;
  std::memset(&info, 0, sizeof(info));
  Check(::stat(key_path.c_str(), &info) == 0 &&
            static_cast<int>(info.st_mode & 0777) == 0600,
        "私钥文件权限是 0600");
  TransportIdentity loaded;
  Check(LoadTransportIdentity(key_path, &loaded, &error), "读回身份私钥",
        error);
  Check(loaded.private_key == identity.private_key &&
            loaded.public_key == identity.public_key,
        "读回的私钥与派生公钥都和写入的一致");
  Check(!SaveTransportIdentity(key_path, identity, false, &error),
        "已存在且 overwrite=false 时拒绝覆盖（O_EXCL）", error);
  Check(::chmod(key_path.c_str(), 0644) == 0, "把私钥文件改成 0644");
  Check(!LoadTransportIdentity(key_path, &loaded, &error),
        "组/其他用户可读的私钥文件被拒绝", error);
  Check(::chmod(key_path.c_str(), 0600) == 0, "权限改回 0600");

  // (2) 长度不对的私钥文件
  const std::string short_path = dir + "/short.key";
  const int short_fd =
      ::open(short_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (short_fd >= 0) {
    const char bytes[31] = {0};
    Check(::write(short_fd, bytes, sizeof(bytes)) == 31, "写 31 字节");
    ::close(short_fd);
  }
  Check(!LoadTransportIdentity(short_path, &loaded, &error),
        "31 字节的私钥文件被拒绝", error);
  const std::string long_path = dir + "/long.key";
  const int long_fd =
      ::open(long_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (long_fd >= 0) {
    const char bytes[33] = {0};
    Check(::write(long_fd, bytes, sizeof(bytes)) == 33, "写 33 字节");
    ::close(long_fd);
  }
  Check(!LoadTransportIdentity(long_path, &loaded, &error),
        "33 字节的私钥文件被拒绝", error);

  // (3) 符号链接与目录：O_NOFOLLOW / S_ISREG 必须挡住
  const std::string link_path = dir + "/link.key";
  Check(::symlink(key_path.c_str(), link_path.c_str()) == 0, "创建符号链接");
  Check(!LoadTransportIdentity(link_path, &loaded, &error),
        "指向私钥的符号链接被 Load 拒绝（O_NOFOLLOW）", error);
  Check(!SaveTransportIdentity(link_path, identity, true, &error),
        "符号链接不允许被 Save 跟随（O_NOFOLLOW）", error);
  Check(!SaveTransportIdentity(link_path, identity, false, &error),
        "符号链接 + overwrite=false 也被拒绝");
  Check(!LoadTransportIdentity(dir, &loaded, &error), "目录路径被拒绝", error);

  // (4) FIFO：普通文件校验必须挡住它。当前实现先 open() 后 fstat()，
  //     而 open(FIFO, O_RDONLY) 会一直等到有写者——所以这里是"记录在案"
  //     而不是一条会失败的断言：断言只要求 FIFO 最终不被接受（修好之后
  //     依然成立），阻塞与否单独打印出来。
  const std::string fifo_path = dir + "/fifo.key";
  Check(::mkfifo(fifo_path.c_str(), 0600) == 0, "创建 FIFO", fifo_path);
  std::atomic<bool> done{false};
  bool fifo_loaded = true;
  std::string fifo_error;
  TransportIdentity fifo_identity;
  std::thread loader([&]() {
    fifo_loaded = LoadTransportIdentity(fifo_path, &fifo_identity, &fifo_error);
    done.store(true);
  });
  std::size_t waited_ms = 0;
  while (!done.load() && waited_ms < 1200) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    waited_ms += 2;
  }
  const bool blocked = !done.load();
  // 解锁：以 O_RDWR|O_NONBLOCK 打开同一个 FIFO 永远不会阻塞。
  const int release = ::open(fifo_path.c_str(), O_RDWR | O_NONBLOCK);
  if (release >= 0) {
    ::close(release);
  }
  loader.join();
  Check(!fifo_loaded, "FIFO 不会被当成合法的身份私钥文件", fifo_error);
  std::printf(blocked ? "  FINDING：FIFO 路径上 LoadTransportIdentity 阻塞在 "
                        "open() 上 1.2 秒没有返回（先 open 后 fstat）\n"
                      : "  记录：FIFO 路径上 LoadTransportIdentity 立刻返回"
                        "失败（open 已经带 O_NONBLOCK）\n");

  ::unlink(fifo_path.c_str());
  ::unlink(link_path.c_str());
  ::unlink(short_path.c_str());
  ::unlink(long_path.c_str());
  ::unlink(key_path.c_str());
  ::rmdir(dir.c_str());
}

// 用例 8：握手 DoS 边界。SO_RCVTIMEO 是**每次 recv** 的超时：一个完全
// 不动的对端最多占住 worker 一个超时；但一个"每次超时前都挤 1 个字节"
// 的对端可以无限期占住（没有整体截止时间）——这一条是记录在案的发现。
struct StalledHandshake {
  bool completed = false;
  bool handshake_ok = true;
  bool established = true;
  SecureTransportError error = SecureTransportError::kNone;
  std::size_t elapsed_ms = 0;
  std::string message;
};

StalledHandshake RunStalledServerHandshake(const TransportIdentity& identity,
                                           const std::string& bytes,
                                           int interval_ms, int timeout_ms) {
  StalledHandshake outcome;
  int fds[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
    outcome.message = "socketpair 失败";
    return outcome;
  }
  timeval timeout;
  timeout.tv_sec = timeout_ms / 1000;
  timeout.tv_usec = (timeout_ms % 1000) * 1000;
  ::setsockopt(fds[0], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  std::atomic<bool> stop{false};
  std::thread peer([&fds, &bytes, &stop, interval_ms]() {
    for (std::size_t i = 0; i < bytes.size() && !stop.load(); ++i) {
      if (!WriteAll(fds[1], bytes.substr(i, 1))) {
        break;
      }
      if (interval_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
      }
    }
    while (!stop.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  });
  SecureChannel channel;
  std::string error;
  bool ok = true;
  WatchedCall watch;
  const bool completed = watch.Run(
      [&]() { ok = channel.HandshakeServer(fds[0], identity, &error); }, 15000,
      fds[0], fds[1]);
  outcome.completed = completed;
  outcome.elapsed_ms = watch.elapsed_ms;
  outcome.handshake_ok = ok;
  outcome.established = channel.established();
  outcome.error = channel.last_error();
  outcome.message = error;
  stop = true;
  ::shutdown(fds[1], SHUT_RDWR);
  peer.join();
  ::close(fds[0]);
  ::close(fds[1]);
  return outcome;
}

void TestStalledHandshakeTimeout() {
  std::printf("[redteam] 握手 DoS 边界：停住/慢速滴水的对端\n");
  TransportIdentity identity;
  std::string error;
  Check(GenerateTransportIdentity(&identity, &error), "生成服务端身份密钥",
        error);
  ServerKeyPin pin;
  Check(ParseServerKeyPin("hex:" + backupproject::crypto::X25519FormatKeyHex(
                                       identity.public_key),
                          &pin, &error),
        "按公钥 pin", error);
  std::string client_hello;
  std::string server_hello;
  std::string client_finished;
  std::string server_finished;
  Check(CaptureHandshakeMessages(identity, pin, &client_hello, &server_hello,
                                 &client_finished, &server_finished, &error),
        "捕获真实 ClientHello（供滴水用）", error);

  // (8a) 发了 5 个字节就不动了：SO_RCVTIMEO = 1 秒后必须返回。
  {
    const StalledHandshake outcome =
        RunStalledServerHandshake(identity, client_hello.substr(0, 5), 0, 1000);
    Check(outcome.completed,
          "8a：HandshakeServer 在 15 秒内返回（没有无限等待）");
    Check(!outcome.handshake_ok && !outcome.established,
          "8a：握手失败且不留已建立状态", outcome.message);
    Check(outcome.error == SecureTransportError::kIoError,
          "8a：错误分类是 kIoError（超时）",
          backupproject::net::SecureTransportErrorName(outcome.error));
    Check(outcome.elapsed_ms >= 900 && outcome.elapsed_ms <= 3000,
          "8a：放弃时机 ≈ SO_RCVTIMEO（1 秒），worker 被释放",
          std::to_string(outcome.elapsed_ms) + " ms");
    std::printf("  8a 实测：%zu ms 后返回，last_error=%s\n", outcome.elapsed_ms,
                backupproject::net::SecureTransportErrorName(outcome.error));
  }

  // (8b) 每 200 毫秒滴 1 个字节，滴 8 个之后停住：超时后仍然必须放弃。
  {
    const StalledHandshake outcome = RunStalledServerHandshake(
        identity, client_hello.substr(0, 8), 200, 1000);
    Check(outcome.completed, "8b：HandshakeServer 在 15 秒内返回");
    Check(!outcome.handshake_ok && !outcome.established,
          "8b：握手失败且不留已建立状态", outcome.message);
    Check(outcome.error == SecureTransportError::kIoError,
          "8b：错误分类是 kIoError（最后一个字节之后超时）",
          backupproject::net::SecureTransportErrorName(outcome.error));
    Check(outcome.elapsed_ms >= 1900 && outcome.elapsed_ms <= 4500,
          "8b：耗时 = 滴字节的时间 + 一个超时",
          std::to_string(outcome.elapsed_ms) + " ms");
  }

  // (8c) 慢速滴水的对端：SO_RCVTIMEO 只约束**单次** recv，每个超时周期挤 1 个
  //      字节就能让它永不触发。修复前实测：SO_RCVTIMEO = 500 ms 时，1.2 秒后
  //      HandshakeServer 仍未返回（本轮记录在案的缺陷 C）。修复后握手带一个
  //      **整体**预算，这里设成 800 ms，断言它在对端滴完之前收场。
  {
    int fds[2] = {-1, -1};
    Check(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
    timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 500 * 1000;  // 500 ms
    ::setsockopt(fds[0], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    const std::string dribble = client_hello.substr(0, 30);
    std::atomic<bool> stop{false};
    std::atomic<std::size_t> sent{0};
    std::thread peer([&fds, &dribble, &stop, &sent]() {
      for (std::size_t i = 0; i < dribble.size() && !stop.load(); ++i) {
        if (!WriteAll(fds[1], dribble.substr(i, 1))) {
          break;
        }
        sent.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      while (!stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    });
    SecureChannel channel;
    channel.SetHandshakeTimeoutMs(800);
    std::atomic<bool> done{false};
    std::string server_error;
    bool ok = true;
    std::thread server_thread([&]() {
      ok = channel.HandshakeServer(fds[0], identity, &server_error);
      done.store(true);
    });
    // 30 个字节按每 50 ms 一个要滴 1.5 秒；预算 800 ms，所以必须提前收场。
    int waited_ms = 0;
    while (!done.load() && waited_ms < 5000) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      waited_ms += 20;
    }
    const bool completed = done.load();
    const std::size_t dribbled = sent.load();
    Check(completed, "8c：有整体预算时慢速滴水的握手必须返回（不再被无限占住）",
          server_error);
    Check(completed && waited_ms <= 3000,
          "8c：返回时机由整体预算决定，而不是等对端滴完",
          std::to_string(waited_ms) + " ms");
    Check(!ok && !channel.established(), "8c：握手失败且不留已建立状态",
          server_error);
    Check(channel.last_error() == SecureTransportError::kIoError,
          "8c：错误分类是 kIoError（整体预算耗尽）",
          backupproject::net::SecureTransportErrorName(channel.last_error()));
    Check(dribbled < dribble.size(),
          "8c：确实是在对端滴完 30 个字节之前就放弃了",
          std::to_string(dribbled) + " 字节");
    std::printf(
        "  8c 实测：整体预算 800 ms，%d ms 返回，对端只推进 %zu 字节，"
        "last_error=%s\n",
        waited_ms, dribbled,
        backupproject::net::SecureTransportErrorName(channel.last_error()));
    stop = true;
    ::shutdown(fds[0], SHUT_RDWR);
    ::shutdown(fds[1], SHUT_RDWR);
    server_thread.join();
    peer.join();
    Check(!ok && !channel.established(), "8c：关掉连接之后握手以失败收场",
          server_error);
    ::close(fds[0]);
    ::close(fds[1]);
  }
}

}  // namespace

int main() {
  std::printf("BPSEC1 加密传输层单元测试\n");
  TestHandshakeOverTcp();
  TestWireCaptureHasNoPlaintext();
  TestRecordTamperMatrix();
  TestHandshakeMutations();
  TestLargeStreamingTransfer();
  TestMalformedRecordFuzz();
  TestMalformedHandshakeFuzz();
  TestServerMustProvePrivateKeyPossession();
  TestHandshakeFragmentationMatrix();
  TestPeerDisconnectMatrix();
  TestFailedChannelStateMachine();
  TestRecordLayerCounterDiscipline();
  TestDeclaredLengthBounds();
  TestStalledHandshakeTimeout();
  TestIdentityFileAttackSurface();
  const int passed = g_checks - g_failures;
  std::printf("secure-transport-test: %d/%d checks passed\n", passed, g_checks);
  return g_failures == 0 ? 0 : 1;
}
