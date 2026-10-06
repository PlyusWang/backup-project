// tests/review/cleanup_fault_interposer.cpp
//
// 链接期注入器：让 fsync() / close() 在指定的第几次调用上确定性地失败，
// 用来证明"清理步骤短路"这类缺陷不会再回来。
//
// 为什么单独一个转换单元：它必须**定义** fsync / close 这两个符号（而不是
// 调用它们），才能覆盖产品代码里的同名调用。ld 解析未定义符号时只看目标文件
// 的先后顺序，所以把这个 .o 放在 libc 前面（见
// scripts/cleanup_contract_test.sh），不需要 LD_PRELOAD，也不 sleep 撞运气。
//
// 真正干活的仍是 libc：所有"不注入"的调用都原样转调。转调用 syscall(SYS_*)
// 而不是调用同名函数——后者会递归到自己。

#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>

// syscall() 由 <unistd.h> 声明；这里只是显式再声明一次便于阅读。
extern "C" long syscall(long number, ...);

namespace {

// bit0 = 第 1 次调用失败，bit1 = 第 2 次失败，依此类推。
std::uint32_t g_fsync_fail_mask = 0;
std::uint32_t g_fsync_calls = 0;
int g_fsync_last_errno = 0;

std::uint32_t g_close_fail_mask = 0;
std::uint32_t g_close_calls = 0;
int g_close_last_errno = 0;

int RealFsync(int fd) { return static_cast<int>(::syscall(SYS_fsync, fd)); }
int RealClose(int fd) { return static_cast<int>(::syscall(SYS_close, fd)); }

}  // namespace

// 装弹：从现在起，第 1..2 次 fsync / close 各自按掩码注入失败。
// 计数每次装弹都归零，所以用例之间互不影响。
extern "C" void CleanupFaultArm(std::uint32_t fsync_fail_mask,
                                std::uint32_t close_fail_mask) {
  g_fsync_fail_mask = fsync_fail_mask;
  g_fsync_calls = 0;
  g_fsync_last_errno = 0;
  g_close_fail_mask = close_fail_mask;
  g_close_calls = 0;
  g_close_last_errno = 0;
}

extern "C" std::uint32_t CleanupFaultFsyncCalls() { return g_fsync_calls; }
extern "C" std::uint32_t CleanupFaultCloseCalls() { return g_close_calls; }
extern "C" int CleanupFaultFsyncErrno() { return g_fsync_last_errno; }
extern "C" int CleanupFaultCloseErrno() { return g_close_last_errno; }

// 探针专用：绕开注入直接调用真实现（用例用它检查某个 fd 到底关没关）。
extern "C" int CleanupFaultCallRealFsync(int fd) { return RealFsync(fd); }
extern "C" int CleanupFaultCallRealClose(int fd) { return RealClose(fd); }

extern "C" int fsync(int fd) {
  g_fsync_calls += 1;
  const std::uint32_t bit = 1u << (g_fsync_calls - 1);
  if (g_fsync_calls <= 32 && (g_fsync_fail_mask & bit) != 0) {
    errno = ENOSPC;
    g_fsync_last_errno = errno;
    return -1;
  }
  return RealFsync(fd);
}

extern "C" int close(int fd) {
  g_close_calls += 1;
  const std::uint32_t bit = 1u << (g_close_calls - 1);
  if (g_close_calls <= 32 && (g_close_fail_mask & bit) != 0) {
    // 第一次注入 EIO、第二次注入 ENOSPC：两种 errno 不同，用例才能分辨
    // "诊断里报的是第一次失败的原因"还是"被后面的清理改写成了别的"。
    errno = (g_close_calls == 1) ? EIO : ENOSPC;
    g_close_last_errno = errno;
    // 关键：注入的失败**不真的关闭** fd（模拟"close 报错但语义未定"），
    // 用例因此能用 fcntl(fd, F_GETFD) 判断"调用方有没有尝试关它"。
    return -1;
  }
  return RealClose(fd);
}