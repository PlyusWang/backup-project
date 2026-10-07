// src/crypto/random.cpp
//
// OS 随机数：优先 getrandom(2)，不可用时回退 /dev/urandom。
//
// 为什么不用 rand()/std::mt19937：这些是确定性伪随机数发生器，种子可预测，
// 拿来做密钥或 IV 等于把密钥空间直接交给攻击者。这里唯一的熵来源是内核 CSPRNG。
//
// 回退路径是真实存在的需求：老内核（< 3.17）没有 getrandom，容器里也可能被
// seccomp 拦成 EPERM。两条路径都失败时必须显式失败，绝不能返回未初始化内存。
// 职责与边界：把内核 CSPRNG 的字节原样交给调用方。不做编码、派生或清零：
// 不 base64/hex、不做 HKDF；“这些字节当密钥还是 IV”由调用方决定。
//
// 数据流：长度 -> RandomBytes -> getrandom(2) -> 失败回退 /dev/urandom ->
// std::string（长度恰好等于请求值，可能含 '\0'，必须按 size() 使用）。
//
// 失败语义：返回 false 且不抛异常，*out 已被清空；error_message 里同时带
// 上两条路径的失败原因，绝不返回未初始化内存。
// 线程：无全局状态、无锁，每次调用自开自关 fd，可从任意线程并发调用。

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "crypto.h"

#if defined(__linux__)
#include <sys/random.h>  // getrandom
#endif

namespace backupproject {
namespace crypto {
namespace {

// 必须在失败点之后立刻调用：errno 只在下一次库调用之前有效。
std::string ErrnoMessage(const char* what) {
  return std::string(what) + " 失败: " + std::strerror(errno);
}

// 每次 getrandom 调用最多要 1 MiB：内核单次调用的上限是 32 MiB，分片调用既
// 不会触发上限，也方便在 EINTR 之后从断点继续。
// 分片还有一个好处：EINTR 重试时只重取被打断的那一片，已填充的前缀不作废。
// flags 传 0 表示阻塞到熵池就绪为止，不会返回质量未达标的字节。
constexpr std::size_t kGetrandomChunk = 1024 * 1024;

// 前置条件：buffer 至少有 size 字节。读到 EOF 也按失败处理 —— /dev/urandom
// 不该有“文件末尾”，出现只能说明这个设备被换掉了，继续用会交出差的数据。
bool ReadFromUrandom(std::size_t size, unsigned char* buffer,
                     std::string* error_message) {
  // O_CLOEXEC：避免随机数 fd 泄漏给 fork/exec 出来的子进程。
  const int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    *error_message = ErrnoMessage("open /dev/urandom");
    return false;
  }
  std::size_t done = 0;
  bool ok = true;
  while (done < size) {
    const ssize_t got = read(fd, buffer + done, size - done);
    if (got < 0) {
      if (errno == EINTR) continue;  // 被信号打断，重试
      *error_message = ErrnoMessage("read /dev/urandom");
      ok = false;
      break;
    }
    if (got == 0) {
      *error_message = "read /dev/urandom 提前到达文件末尾";
      ok = false;
      break;
    }
    done += static_cast<std::size_t>(got);
  }
  close(fd);
  return ok;
}

#if defined(__linux__)
bool ReadFromGetrandom(std::size_t size, unsigned char* buffer,
                       std::string* error_message) {
  std::size_t done = 0;
  while (done < size) {
    std::size_t chunk = size - done;
    if (chunk > kGetrandomChunk) chunk = kGetrandomChunk;
    const ssize_t got = getrandom(buffer + done, chunk, 0);
    if (got < 0) {
      if (errno == EINTR) continue;
      *error_message = ErrnoMessage("getrandom");
      return false;
    }
    if (got == 0) {
      *error_message = "getrandom 返回 0 字节";
      return false;
    }
    done += static_cast<std::size_t>(got);
  }
  return true;
}
#endif

}  // namespace

bool RandomBytesFromUrandom(std::size_t size, std::string* out,
                            std::string* error_message) {
  if (error_message != nullptr) error_message->clear();
  if (out == nullptr) {
    if (error_message != nullptr) *error_message = "随机数: 输出指针为空";
    return false;
  }
  out->clear();
  if (size == 0) return true;
  std::string buffer;
  buffer.resize(size);
  if (!ReadFromUrandom(size, reinterpret_cast<unsigned char*>(&buffer[0]),
                       error_message)) {
    return false;
  }
  *out = std::move(buffer);
  return true;
}

// 两条路径都失败时把两条原因一起报出来：只说“getrandom 失败”会把用户引向
// “内核太老”，而真实原因常常是回退路径也不可用（容器里没挂 /dev）。
bool RandomBytes(std::size_t size, std::string* out,
                 std::string* error_message) {
  if (error_message != nullptr) error_message->clear();
  if (out == nullptr) {
    if (error_message != nullptr) *error_message = "随机数: 输出指针为空";
    return false;
  }
  out->clear();
  if (size == 0) return true;

  std::string buffer;
  buffer.resize(size);
  unsigned char* data = reinterpret_cast<unsigned char*>(&buffer[0]);

#if defined(__linux__)
  std::string getrandom_error;
  if (ReadFromGetrandom(size, data, &getrandom_error)) {
    *out = std::move(buffer);
    return true;
  }
  // 走到这里说明 getrandom 不可用（ENOSYS/EPERM 等），回退到设备文件。
  std::string fallback_error;
  if (ReadFromUrandom(size, data, &fallback_error)) {
    *out = std::move(buffer);
    return true;
  }
  if (error_message != nullptr) {
    *error_message =
        "随机数不可用: " + getrandom_error + "；回退 " + fallback_error;
  }
  return false;
#else
  // 非 Linux 平台没有 getrandom，直接走设备文件。
  if (ReadFromUrandom(size, data, error_message)) {
    *out = std::move(buffer);
    return true;
  }
  return false;
#endif
}

}  // namespace crypto
}  // namespace backupproject
