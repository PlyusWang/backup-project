// tests/review/sequence_exhaustion_harness.cpp
//
// PR #21 独立审查轮（review-only 工具，不参与产品构建）。
//
// 记录层序号是 SecureChannel 的**私有**成员，产品头文件只暴露只读的
// send_sequence() / receive_sequence()。这个 harness 用
// "#define private public"（按约定只允许出现在 tests/review/ 下的
// review-only 工具里）把序号直接放到 UINT64_MAX 附近，验证：
//
//   1. 发送侧在 send_sequence_ == UINT64_MAX 时拒绝发送，错误分类是
//      kStateError，并且**不回绕到 0**、不往 fd 写任何字节；
//   2. UINT64_MAX - 1 是最后一条能发出去的记录，之后必须重新握手；
//   3. 接收侧同样有"序号耗尽"守卫：收到 seq == UINT64_MAX 的记录必须被拒绝
//      （kReplayDetected），receive_sequence_ 停在 UINT64_MAX 不回绕，于是同
//      一会话里 seq == 0 的记录不可能被再接受一次。
//
//      这一条是本轮审查的**缺陷 A**：修复前接收侧没有守卫，计数器会回绕到 0，
//      回绕之后 seq == 0 的记录（字节完全相同）会被再次接受（可重放）。发送侧
//      的守卫让合规对端永远不会发出 UINT64_MAX，所以当时的影响是"两侧不对称 +
//      缺一条防御性一致性检查"，而不是能直接利用的漏洞；现在两侧对称了。
//
// 编译时**不要**再单独链接 src/network/secure_transport.cpp（会重复定义）。
// 退出码：0 = 全部通过。

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include "crypto.h"
#include "hkdf.h"
#include "network_protocol.h"
#include "x25519.h"

// 只有 review-only 工具才允许这样拿到私有成员（产品代码、产品测试都不许）。
#define private public
#include "secure_transport.h"
#undef private

#include "../../src/network/secure_transport.cpp"

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

using namespace backupproject::net;

const char* StatusName(FrameReadStatus status) {
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

// 对端缓冲区里此刻一个字节都没有（poll(0) == 0 且 recv 是 EAGAIN）。
bool PeerBufferEmpty(int fd) {
  pollfd entry;
  entry.fd = fd;
  entry.events = POLLIN;
  entry.revents = 0;
  const int ready = ::poll(&entry, 1, 0);
  unsigned char byte = 0;
  const ssize_t got = ::recv(fd, &byte, 1, MSG_DONTWAIT);
  return ready == 0 && got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
}

void DrainPeer(int fd) {
  unsigned char buffer[1024];
  for (;;) {
    const ssize_t got = ::recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
    if (got <= 0) {
      return;
    }
  }
}

// 用接收方向的会话密钥伪造一条记录：tag 覆盖 direction || seq || 长度 ||
// 密文，所以只有持有会话密钥的对端才造得出来（这里就是"合规对端自己
// 发了一条序号不合规的记录"这一情形）。
std::string ForgeRecord(const SecureChannel& channel, std::uint64_t sequence,
                        const std::string& plaintext) {
  const std::string counter =
      CounterBlock(channel.receive_nonce_prefix_, sequence);
  backupproject::crypto::Aes256Ctr cipher(channel.receive_key_, counter);
  std::string ciphertext;
  cipher.Process(plaintext.data(), plaintext.size(), &ciphertext);
  const std::string tag =
      RecordTag(channel.receive_mac_key_, channel.receive_direction_, sequence,
                ciphertext);
  const std::uint32_t magic = kBssec1Magic;
  std::string record;
  record.push_back(static_cast<char>((magic >> 24) & 0xFFu));
  record.push_back(static_cast<char>((magic >> 16) & 0xFFu));
  record.push_back(static_cast<char>((magic >> 8) & 0xFFu));
  record.push_back(static_cast<char>(magic & 0xFFu));
  record.push_back(static_cast<char>(kBssec1MessageRecord));
  record.push_back(static_cast<char>(kBssec1Version));
  record.push_back(0);
  record.push_back(0);
  for (int shift = 56; shift >= 0; shift -= 8) {
    record.push_back(static_cast<char>((sequence >> shift) & 0xFFu));
  }
  const std::uint32_t length = static_cast<std::uint32_t>(ciphertext.size());
  for (int shift = 24; shift >= 0; shift -= 8) {
    record.push_back(static_cast<char>((length >> shift) & 0xFFu));
  }
  record.append(ciphertext);
  record.append(tag);
  return record;
}

struct Pair {
  int client_fd = -1;
  int server_fd = -1;
  SecureChannel client;
  SecureChannel server;
  bool ok = false;
};

// SecureChannel 不可拷贝也不可移动（它持有密钥材料），所以用 unique_ptr。
std::unique_ptr<Pair> MakePair() {
  auto pair = std::make_unique<Pair>();
  TransportIdentity identity;
  std::string error;
  if (!GenerateTransportIdentity(&identity, &error)) {
    return pair;
  }
  ServerKeyPin pin;
  if (!ParseServerKeyPin("hex:" + backupproject::crypto::X25519FormatKeyHex(
                                      identity.public_key),
                         &pin, &error)) {
    return pair;
  }
  int fds[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
    return pair;
  }
  timeval timeout;
  timeout.tv_sec = 10;
  timeout.tv_usec = 0;
  ::setsockopt(fds[0], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  ::setsockopt(fds[1], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  pair->client_fd = fds[0];
  pair->server_fd = fds[1];
  Pair* raw = pair.get();
  std::atomic<bool> server_ok{false};
  std::string server_error;
  std::thread server_thread([raw, &identity, &server_ok, &server_error]() {
    server_ok.store(
        raw->server.HandshakeServer(raw->server_fd, identity, &server_error));
  });
  std::string client_error;
  const bool client_ok =
      raw->client.HandshakeClient(raw->client_fd, pin, &client_error);
  if (!client_ok) {
    ::shutdown(raw->client_fd, SHUT_RDWR);
  }
  server_thread.join();
  pair->ok = client_ok && server_ok.load();
  if (!pair->ok) {
    std::printf("  (MakePair 失败：%s / %s)\n", client_error.c_str(),
                server_error.c_str());
  }
  return pair;
}

void TestSendSideExhaustion() {
  // (1) send_sequence_ == UINT64_MAX：必须拒绝，且不回绕。
  {
    std::unique_ptr<Pair> pair = MakePair();
    Check(pair->ok, "发送侧：完成一次真实握手");
    if (!pair->ok) {
      return;
    }
    DrainPeer(pair->server_fd);
    pair->client.send_sequence_ = 0xFFFFFFFFFFFFFFFFull;
    std::string error;
    const bool sent =
        pair->client.SendRecord(pair->client_fd, "too-late", &error);
    Check(!sent, "发送侧：序号 = UINT64_MAX 时 SendRecord 拒绝", error);
    Check(pair->client.last_error() == SecureTransportError::kStateError,
          "发送侧：错误分类是 kStateError",
          SecureTransportErrorName(pair->client.last_error()));
    Check(pair->client.send_sequence_ == 0xFFFFFFFFFFFFFFFFull,
          "发送侧：被拒绝之后序号仍然是 UINT64_MAX（没有回绕到 0）");
    Check(PeerBufferEmpty(pair->server_fd),
          "发送侧：被拒绝的 SendRecord 一个字节都没写出去");
    Check(!pair->client.established(),
          "发送侧：序号耗尽之后通道不再被视为已建立");
    std::printf("  发送侧：last_error=%s，序号=%llu\n",
                SecureTransportErrorName(pair->client.last_error()),
                static_cast<unsigned long long>(pair->client.send_sequence_));
    ::close(pair->client_fd);
    ::close(pair->server_fd);
  }

  // (2) send_sequence_ == UINT64_MAX - 1：这是最后一条能发出去的记录。
  {
    std::unique_ptr<Pair> pair = MakePair();
    Check(pair->ok, "发送侧边界：完成一次真实握手");
    if (!pair->ok) {
      return;
    }
    DrainPeer(pair->server_fd);
    pair->client.send_sequence_ = 0xFFFFFFFFFFFFFFFEull;
    std::string error;
    const bool sent =
        pair->client.SendRecord(pair->client_fd, "last-record", &error);
    Check(sent, "发送侧边界：UINT64_MAX - 1 仍然可以发送", error);
    Check(pair->client.send_sequence_ == 0xFFFFFFFFFFFFFFFFull,
          "发送侧边界：发送之后序号变成 UINT64_MAX");
    const std::size_t record_size = kBssec1RecordHeaderSize +
                                    std::string("last-record").size() +
                                    kBssec1TagSize;
    std::string raw;
    Check(ReadExact(pair->server_fd, record_size, &raw) &&
              raw.size() == record_size,
          "发送侧边界：线上记录长度正确");
    std::uint64_t wire_sequence = 0;
    for (int index = 0; index < 8; ++index) {
      wire_sequence =
          (wire_sequence << 8) |
          static_cast<unsigned char>(raw[8 + static_cast<std::size_t>(index)]);
    }
    Check(wire_sequence == 0xFFFFFFFFFFFFFFFEull,
          "发送侧边界：线上记录的序号字段是 UINT64_MAX - 1",
          std::to_string(wire_sequence));
    const bool refused =
        !pair->client.SendRecord(pair->client_fd, "one-too-many", &error);
    Check(refused &&
              pair->client.last_error() == SecureTransportError::kStateError,
          "发送侧边界：耗尽之后下一条立即被拒绝（kStateError）", error);
    Check(pair->client.send_sequence_ == 0xFFFFFFFFFFFFFFFFull,
          "发送侧边界：拒绝之后序号没有回绕");
    ::close(pair->client_fd);
    ::close(pair->server_fd);
  }
}

void TestReceiveSideWrap() {
  // (1) 对照：期望 UINT64_MAX 时收到 seq 0，必须按重放拒绝。
  {
    std::unique_ptr<Pair> pair = MakePair();
    Check(pair->ok, "接收侧对照：完成一次真实握手");
    if (!pair->ok) {
      return;
    }
    pair->server.receive_sequence_ = 0xFFFFFFFFFFFFFFFFull;
    const std::string record = ForgeRecord(pair->server, 0, "replayed-zero");
    Check(WriteAll(pair->client_fd, record),
          "接收侧对照：把 seq 0 的记录写进去");
    std::string plaintext;
    std::string error;
    const FrameReadStatus status =
        pair->server.ReceiveRecord(pair->server_fd, &plaintext, &error);
    Check(status == FrameReadStatus::kCorruptStream,
          "接收侧对照：期望 UINT64_MAX 时收到 seq 0 → kCorruptStream",
          StatusName(status));
    Check(pair->server.last_error() == SecureTransportError::kReplayDetected,
          "接收侧对照：分类是 kReplayDetected",
          SecureTransportErrorName(pair->server.last_error()));
    Check(plaintext.empty(), "接收侧对照：不交出任何明文");
    ::close(pair->client_fd);
    ::close(pair->server_fd);
  }

  // (2) 期望序号 = UINT64_MAX，收到 seq = UINT64_MAX：被接受，然后回绕。
  {
    std::unique_ptr<Pair> pair = MakePair();
    Check(pair->ok, "接收侧：完成一次真实握手");
    if (!pair->ok) {
      return;
    }
    pair->server.receive_sequence_ = 0xFFFFFFFFFFFFFFFFull;
    const std::string last_record =
        ForgeRecord(pair->server, 0xFFFFFFFFFFFFFFFFull, "final-record");
    Check(WriteAll(pair->client_fd, last_record),
          "接收侧：写一条 seq = UINT64_MAX 的记录");
    std::string plaintext;
    std::string error;
    const FrameReadStatus status =
        pair->server.ReceiveRecord(pair->server_fd, &plaintext, &error);
    Check(status == FrameReadStatus::kCorruptStream,
          "接收侧：seq = UINT64_MAX 被拒绝（接收侧也有耗尽守卫）",
          SecureTransportErrorName(pair->server.last_error()));
    Check(pair->server.last_error() == SecureTransportError::kReplayDetected,
          "接收侧：分类是 kReplayDetected（计数器不允许回绕）",
          SecureTransportErrorName(pair->server.last_error()));
    Check(plaintext.empty(), "接收侧：不交出任何明文");
    std::printf(
        "  接收侧：收到 seq=UINT64_MAX 之后 receive_sequence_=%llu\n",
        static_cast<unsigned long long>(pair->server.receive_sequence_));
    Check(pair->server.receive_sequence_ == 0xFFFFFFFFFFFFFFFFull,
          "接收侧：计数器停在 UINT64_MAX，没有回绕到 0");

    // 修复前：计数器回绕到 0，下面这条 seq = 0
    // 的记录会被**再次接受**（可重放）。
    // 修复后连接已经进入失败状态，这条记录无论怎样都不能再交出来。
    const std::string replay = ForgeRecord(pair->server, 0, "zero-record");
    Check(WriteAll(pair->client_fd, replay),
          "接收侧：耗尽之后再写一条 seq 0 的记录");
    std::string replayed;
    const FrameReadStatus replay_status =
        pair->server.ReceiveRecord(pair->server_fd, &replayed, &error);
    Check(replay_status != FrameReadStatus::kOk && replayed.empty(),
          "接收侧：回绕后的 seq=0 重放被拒绝（修复前会被接受）",
          SecureTransportErrorName(pair->server.last_error()));
    Check(pair->server.receive_sequence_ == 0xFFFFFFFFFFFFFFFFull,
          "接收侧：重放尝试之后计数器仍未回绕");
    ::close(pair->client_fd);
    ::close(pair->server_fd);
  }
}

}  // namespace

int main() {
  std::printf("BPSEC1 记录层序号耗尽（review-only）\n");
  TestSendSideExhaustion();
  TestReceiveSideWrap();
  const int passed = g_checks - g_failures;
  std::printf("review-sequence-exhaustion: %d/%d checks passed\n", passed,
              g_checks);
  return g_failures == 0 ? 0 : 1;
}