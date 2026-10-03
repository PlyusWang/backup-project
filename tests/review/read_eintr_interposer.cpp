// tests/review/read_eintr_interposer.cpp
//
// 只做一件事：在自己的目标文件里定义 read() 这个符号，供
// tests/review/identity_tail_eintr.cpp 确定性地在"尾部那一次 read"上注入一次
// EINTR。它随探针一起被链接进同一个二进制，不需要 LD_PRELOAD。
//
// 为什么单独一个转换单元：_FORTIFY_SOURCE=3（本机 -O1 默认打开）时 <unistd.h>
// 会给 read() 一个 always_inline 定义，在同一个转换单元里再定义一次是重定义
// 错误。这个文件**故意不包含任何声明 read() 的头**，只用
// syscall(SYS_read, ...) 转调，所以符号覆盖只发生在链接期：glibc 的 read 依然
// 是真正干活的那一个，唯一的差别是注入的那一次。
//
// 覆盖是确定性的：不是"发一个信号试试看"，也不是 sleep 撞运气。

#include <cerrno>
#include <cstddef>
#include <sys/syscall.h>
#include <sys/types.h>

// syscall() 在这台 glibc 上由 <unistd.h> 声明，而这个文件故意不包含它（见上），
// 所以自己声明一次。签名与 glibc 的一致。
extern "C" long syscall(long number, ...);

namespace {

bool g_armed = false;
std::size_t g_material_bytes = 0;
int g_injected = 0;

}  // namespace

// 开始/结束观察。Disarm 返回这一次观察里注入了几次 EINTR（用例要断言它非 0，
// 否则"没注入也通过"就是一个空用例）。
extern "C" void ReadEintrArm() {
  g_armed = true;
  g_material_bytes = 0;
  g_injected = 0;
}

extern "C" int ReadEintrDisarm() {
  g_armed = false;
  return g_injected;
}

// 链接期覆盖：只在"已经收满 32 字节之后的那一次 1 字节读取"上注入一次 EINTR
// （正好是 LoadTransportIdentity 判断 exact-32 的那一次），其余一律原样转调。
extern "C" ssize_t read(int fd, void* buffer, std::size_t count) {
  if (g_armed) {
    if (count == 1 && g_material_bytes >= 32 && g_injected == 0) {
      g_injected += 1;
      errno = EINTR;
      return -1;
    }
    if (count != 1) {
      const ssize_t got =
          static_cast<ssize_t>(::syscall(SYS_read, fd, buffer, count));
      if (got > 0) {
        g_material_bytes += static_cast<std::size_t>(got);
      }
      return got;
    }
  }
  return static_cast<ssize_t>(::syscall(SYS_read, fd, buffer, count));
}
