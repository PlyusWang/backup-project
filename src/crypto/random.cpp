// src/crypto/random.cpp
//
// OS 随机数：优先 getrandom(2)，不可用时回退 /dev/urandom。
//
// 为什么不用 rand()/std::mt19937：这些是确定性伪随机数发生器，种子可预测，
// 拿来做密钥或 IV 等于把密钥空间直接交给攻击者。这里唯一的熵来源是内核 CSPRNG。
//
// 回退路径是真实存在的需求：老内核（< 3.17）没有 getrandom，容器里也可能被
// seccomp 拦成 EPERM。两条路径都失败时必须显式失败，绝不能返回未初始化内存。

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

std::string ErrnoMessage(const char* what) {
  return std::string(what) + " 失败: " + std::strerror(errno);
}

// 每次 getrandom 调用最多要 1 MiB：内核单次调用的上限是 32 MiB，分片调用既
// 不会触发上限，也方便在 EINTR 之后从断点继续。
constexpr std::size_t kGetrandomChunk = 1024 * 1024;

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
