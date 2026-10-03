// tests/review/record_fuzz.cpp
//
// PR #21 独立审查轮：BPSEC1
// 记录层的**变异模糊测试**（review-only，不参与产品构建）。
//
// 做法：每一轮都重新握手（真 X25519 + 真 HKDF + 真 MAC），让客户端用产品自己的
// SendRecord 产生一条真实记录，把它从 socket 里原样读出来，随机变异之后再喂回
// 服务端的 ReceiveRecord。断言：
//
//   I1 变异过的记录**绝不能被接受**（status == kOk 就等价于 MAC /
//   解析被绕过）； I2 失败之后通道必须不可用：established() ==
//   false，且再收发都是 kStateError； I3 不能崩、不能越界（ASan/UBSan
//   下跑）、单轮不能病态变慢、RSS 有界。
//
// 用法：record_fuzz [iterations] [seed]
// 退出码：0 = 全部不变式保持。

#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "crypto.h"
#include "network_protocol.h"
#include "secure_transport.h"

namespace {

using backupproject::net::FrameReadStatus;
using backupproject::net::SecureChannel;
using backupproject::net::SecureTransportError;
using backupproject::net::ServerKeyPin;
using backupproject::net::TransportIdentity;

std::uint64_t g_rng = 0x13198A2E03707344ull;

std::uint64_t NextRandom() {
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 7;
  g_rng ^= g_rng << 17;
  return g_rng;
}

std::size_t RandomBelow(std::size_t bound) {
  return bound == 0 ? 0 : static_cast<std::size_t>(NextRandom() % bound);
}

int g_failures = 0;

void Fail(const std::string& what, const std::string& detail) {
  ++g_failures;
  std::printf("FUZZ FAIL: %s [%s]\n", what.c_str(), detail.c_str());
}

// 把 fd 上"此刻可读"的字节全部读出来（记录是自描述的，长度不必预先知道）。
bool DrainAvailable(int fd, std::string* out) {
  out->clear();
  char buffer[4096];
  for (;;) {
    const ssize_t got = ::recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
    if (got > 0) {
      out->append(buffer, static_cast<std::size_t>(got));
      continue;
    }
    if (got == 0) {
      return !out->empty();
    }
    return errno == EAGAIN || errno == EWOULDBLOCK;
  }
}

bool WriteAll(int fd, const std::string& data) {
  std::size_t written = 0;
  while (written < data.size()) {
    const ssize_t got =
        ::send(fd, data.data() + written, data.size() - written, MSG_NOSIGNAL);
    if (got <= 0) {
      return false;
    }
    written += static_cast<std::size_t>(got);
  }
  return true;
}

bool HandshakePair(int fds[2], const TransportIdentity& identity,
                   const ServerKeyPin& pin, SecureChannel* client,
                   SecureChannel* server, std::string* detail) {
  std::string client_error;
  std::string server_error;
  bool client_ok = false;
  bool server_ok = false;
  std::thread server_thread([&]() {
    server_ok = server->HandshakeServer(fds[0], identity, &server_error);
  });
  client_ok = client->HandshakeClient(fds[1], pin, &client_error);
  server_thread.join();
  if (!client_ok || !server_ok) {
    *detail = "client=" + client_error + " server=" + server_error;
    return false;
  }
  return true;
}

std::uint64_t RssHighWaterKb() {
  std::FILE* file = std::fopen("/proc/self/status", "r");
  if (file == nullptr) {
    return 0;
  }
  char line[256];
  std::uint64_t value = 0;
  while (std::fgets(line, sizeof(line), file) != nullptr) {
    if (std::strncmp(line, "VmHWM:", 6) == 0) {
      std::sscanf(line + 6, "%llu",
                  reinterpret_cast<unsigned long long*>(&value));
      break;
    }
  }
  std::fclose(file);
  return value;
}

std::string MutateRecord(const std::string& record) {
  std::string mutated = record;
  const int mutations = 1 + static_cast<int>(RandomBelow(4));
  for (int m = 0; m < mutations; ++m) {
    switch (static_cast<int>(RandomBelow(5))) {
      case 0: {  // 单字节异或（保证真的改了）
        if (!mutated.empty()) {
          const std::size_t at = RandomBelow(mutated.size());
          const int bit = 1 << static_cast<int>(RandomBelow(8));
          mutated[at] = static_cast<char>(mutated[at] ^ bit);
        }
        break;
      }
      case 1: {  // 覆盖一个字节为极端值
        if (!mutated.empty()) {
          static const unsigned char kExtremes[] = {0x00, 0xFF, 0x7F, 0x80,
                                                    0x01};
          mutated[RandomBelow(mutated.size())] =
              static_cast<char>(kExtremes[RandomBelow(5)]);
        }
        break;
      }
      case 2: {  // 截断（可能砍掉 tag 或正文）
        if (mutated.size() > 1) {
          mutated.resize(RandomBelow(mutated.size()));
        }
        break;
      }
      case 3: {  // 追加垃圾
        mutated.append(1 + RandomBelow(8), static_cast<char>(NextRandom()));
        break;
      }
      case 4: {  // 复制一段（让长度字段与内容不一致）
        if (mutated.size() > 4) {
          const std::size_t from = RandomBelow(mutated.size());
          const std::size_t length =
              1 + RandomBelow(std::min<std::size_t>(32, mutated.size() - from));
          mutated.insert(RandomBelow(mutated.size()),
                         mutated.substr(from, length));
        }
        break;
      }
    }
  }
  // 保证**真的**改了字节：case 1 可能刚好把同一个值写回去（第一版就是用它
  // 记了个 changed=true，于是出现一次"变异没有改变任何字节"的假失败）。
  // 这里改成与原始字节逐字节比较，比布尔标记可靠。
  if (!mutated.empty() && mutated == record) {
    mutated[0] = static_cast<char>(mutated[0] ^ 0x5A);
  }
  return mutated;
}

}  // namespace

int main(int argc, char** argv) {
  const long iterations = argc > 1 ? std::atol(argv[1]) : 3000;
  if (argc > 2) {
    g_rng = std::strtoull(argv[2], nullptr, 10) | 1ull;
  }
  TransportIdentity identity;
  std::string error;
  if (!backupproject::net::GenerateTransportIdentity(&identity, &error)) {
    std::printf("生成身份密钥失败: %s\n", error.c_str());
    return 2;
  }
  ServerKeyPin pin;
  if (!backupproject::net::ParseServerKeyPin(
          "hex:" +
              backupproject::crypto::X25519FormatKeyHex(identity.public_key),
          &pin, &error)) {
    std::printf("构造 pin 失败: %s\n", error.c_str());
    return 2;
  }

  std::printf("RECORD_FUZZ_START iterations=%ld seed=%llu\n", iterations,
              static_cast<unsigned long long>(g_rng));
  long accepted = 0;
  long handshake_failures = 0;
  double worst_ms = 0.0;
  std::string worst_detail;

  for (long i = 0; i < iterations; ++i) {
    const auto started = std::chrono::steady_clock::now();
    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
      Fail("socketpair 失败", std::to_string(i));
      break;
    }
    // 与产品一致：真实连接的两侧都会设 SO_RCVTIMEO/SO_SNDTIMEO
    // （server/remote_server.cpp 的 ServeConnection、
    // remote_backup_client.cpp 建立连接时）。这里必须照做——变异可能把声明的
    // 密文长度改成天文数字，没有这个超时接收侧会一直等那些永远不会来的字节。
    // 第一版 harness 就是这么挂死的（harness 自己的问题，不是产品行为；产品的
    // 服务端与客户端本来就有超时）。
    timeval recv_timeout;
    recv_timeout.tv_sec = 0;
    recv_timeout.tv_usec = 300 * 1000;
    ::setsockopt(fds[0], SOL_SOCKET, SO_RCVTIMEO, &recv_timeout,
                 sizeof(recv_timeout));
    ::setsockopt(fds[1], SOL_SOCKET, SO_RCVTIMEO, &recv_timeout,
                 sizeof(recv_timeout));
    SecureChannel client;
    SecureChannel server;
    std::string detail;
    if (!HandshakePair(fds, identity, pin, &client, &server, &detail)) {
      ++handshake_failures;
      ::close(fds[0]);
      ::close(fds[1]);
      if (handshake_failures > 3) {
        Fail("握手在模糊测试里失败", detail);
        break;
      }
      continue;
    }

    // 用产品自己的 SendRecord 产生一条真实记录，然后原样读出来。
    const std::string plaintext = "fuzz-payload-" + std::to_string(i);
    std::string send_error;
    if (!client.SendRecord(fds[1], plaintext, &send_error)) {
      Fail("SendRecord 失败", send_error);
      ::close(fds[0]);
      ::close(fds[1]);
      break;
    }
    std::string record;
    if (!DrainAvailable(fds[0], &record) || record.empty()) {
      Fail("读不回刚发出的记录", std::to_string(record.size()));
      ::close(fds[0]);
      ::close(fds[1]);
      break;
    }

    // 对照组：未变异的记录必须被接受，明文必须一致（否则测试本身没意义）。
    if (i % 500 == 0) {
      if (!WriteAll(fds[1], record)) {
        Fail("回写原始记录失败", std::to_string(i));
        ::close(fds[0]);
        ::close(fds[1]);
        break;
      }
      std::string got;
      std::string recv_error;
      const FrameReadStatus status =
          server.ReceiveRecord(fds[0], &got, &recv_error);
      if (status != FrameReadStatus::kOk || got != plaintext) {
        Fail("对照组：未变异记录竟然不被接受",
             std::to_string(static_cast<int>(status)) + " " + recv_error);
      } else {
        ++accepted;
      }
      ::close(fds[0]);
      ::close(fds[1]);
      continue;
    }

    const std::string mutated = MutateRecord(record);
    if (mutated == record) {
      Fail("变异没有改变任何字节（测试无效）", std::to_string(i));
      ::close(fds[0]);
      ::close(fds[1]);
      break;
    }
    if (!WriteAll(fds[1], mutated)) {
      Fail("回写变异记录失败", std::to_string(i));
      ::close(fds[0]);
      ::close(fds[1]);
      break;
    }
    std::string got;
    std::string recv_error;
    const FrameReadStatus status =
        server.ReceiveRecord(fds[0], &got, &recv_error);

    // I1：记录**自身**被改过就必须被拒。
    //
    // 例外（第一版 harness 在这里误报了 21 次，全部是"变异长度 > 原始长度"）：
    // 如果变异只是在记录**之后**追加了字节，那第一条记录本身一个字节都没变——
    // 字节流里后面那截属于下一条记录的起点，不是"记录被篡改"。这种情况换一组
    // 断言：第一条记录必须以原明文被接受，追加的那截垃圾必须作为下一条记录被拒。
    const bool appended_only = mutated.size() > record.size() &&
                               mutated.compare(0, record.size(), record) == 0;
    if (status == FrameReadStatus::kOk && appended_only) {
      ++accepted;
      if (got != plaintext) {
        Fail("追加字节后第一条记录的明文变了",
             "轮 " + std::to_string(i) + " got=" + got);
      }
      std::string trailing;
      std::string trailing_error;
      const FrameReadStatus trailing_status =
          server.ReceiveRecord(fds[0], &trailing, &trailing_error);
      if (trailing_status == FrameReadStatus::kOk) {
        Fail("追加的垃圾被当成下一条记录接受了",
             "轮 " + std::to_string(i) + " 明文=" + trailing);
      }
    } else if (status == FrameReadStatus::kOk) {
      ++accepted;
      Fail("记录自身被改过却仍然通过了 MAC/解析",
           "轮 " + std::to_string(i) + " 明文=" + got +
               " 原始长度=" + std::to_string(record.size()) +
               " 变异长度=" + std::to_string(mutated.size()));
    } else if (server.established()) {
      // I2：失败之后通道必须不可用
      Fail("记录被拒但通道仍是已建立状态",
           "轮 " + std::to_string(i) + " last_error=" +
               backupproject::net::SecureTransportErrorName(
                   server.last_error()));
    } else {
      // I2（续）：失败之后任何收发都必须以 kStateError 拒绝
      std::string again;
      std::string again_error;
      const FrameReadStatus second =
          server.ReceiveRecord(fds[0], &again, &again_error);
      if (second == FrameReadStatus::kOk) {
        Fail("失败之后还能收记录", "轮 " + std::to_string(i));
      }
      std::string send_again;
      if (server.SendRecord(fds[0], "after-failure", &send_again)) {
        Fail("失败之后还能发记录", "轮 " + std::to_string(i));
      } else if (server.last_error() != SecureTransportError::kStateError) {
        Fail("失败之后的错误分类不是 kStateError",
             backupproject::net::SecureTransportErrorName(server.last_error()));
      }
    }

    const double elapsed_ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - started)
                                  .count();
    if (elapsed_ms > worst_ms) {
      worst_ms = elapsed_ms;
      worst_detail = "轮 " + std::to_string(i);
    }
    if (elapsed_ms > 2000.0) {
      Fail("单轮耗时病态（可能卡在解析/分配上）",
           std::to_string(static_cast<long>(elapsed_ms)) + " ms");
    }

    ::close(fds[0]);
    ::close(fds[1]);
    if (g_failures > 20) {
      break;
    }
    if ((i + 1) % 500 == 0) {
      std::printf("... %ld 轮，失败 %d\n", i + 1, g_failures);
    }
  }

  const std::uint64_t rss = RssHighWaterKb();
  std::printf("worst case: %.1f ms (%s)\n", worst_ms, worst_detail.c_str());
  std::printf("对照组接受次数=%ld 握手失败=%ld 峰值RSS=%llu KB\n", accepted,
              handshake_failures, static_cast<unsigned long long>(rss));
  if (rss > 256 * 1024) {
    Fail("峰值 RSS 超过 256 MB", std::to_string(rss) + " KB");
  }
  std::printf("RECORD_FUZZ_DONE iterations=%ld failures=%d\n", iterations,
              g_failures);
  return g_failures == 0 ? 0 : 1;
}
