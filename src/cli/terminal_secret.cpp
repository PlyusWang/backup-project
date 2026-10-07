// terminal_secret.cpp
// 模块职责：把“向用户索要一个口令”收敛成唯一一条受控路径——只从控制终端
// /dev/tty 逐字符读入、读入期间关闭 ECHO、永不回显、永不落盘。
//
// 边界（不负责什么）：本模块不做口令强度校验、不做哈希、不做重试策略，
// 也不接受任何非交互来源（argv、环境变量、管道、重定向文件）——那是产品
// 级的安全约定：口令不进 argv、不进日志、不进任何请求对象。
//
// 失败语义：入口返回 bool，失败时写 *error_message，并保证 *secret 为空。
// 调用方看到 false 必须放弃本次操作，不能把“空口令”当成“用户没输”。
//
// 线程与生命周期：进程内同一时刻只允许一条线程读口令。终端状态与信号处置
// 都放在文件级全局量里（见 g_saved_termios），并发进入会互相踩踏。

#include "terminal_secret.h"

#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <string>

namespace backupproject {
namespace {

// 错误信息是可选输出：nullptr 表示调用方不关心原因；语义是覆盖而不是追加，
// 因此每个入口开头都会清空它，避免上一次的残留被当成这一次的原因。
void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) *error_message = text;
}

// ---- 信号安全地恢复终端模式 ----
//
// 用户在敲密码时按 Ctrl+C 是最常见的中断方式。如果此时不恢复 termios，
// 终端会停在"不回显"的状态上，用户回到 shell 之后会以为键盘坏了。
//
// 处理函数里只做两件 async-signal-safe 的事：tcsetattr 和 raise。
// 状态用文件级变量保存——信号处理函数没有别的办法拿到它们。

// 这三个全局量是信号处理函数与析构守卫之间唯一的通信通道：handler 里能安全
// 访问的只有 sig_atomic_t 和这份保存下来的 termios。
struct termios g_saved_termios;
volatile sig_atomic_t g_termios_active = 0;
volatile sig_atomic_t g_termios_fd = -1;

// 信号处理器里只做 async-signal-safe 的事：tcsetattr 恢复回显，再把处置改回
// SIG_DFL 并 raise 自杀——Ctrl+C 对用户仍然是“按了就结束”，只是结束前终端
// 一定被还原。这里不分配内存、不打日志、不加锁。
void RestoreAndReraise(int signal_number) {
  if (g_termios_active != 0) {
    ::tcsetattr(static_cast<int>(g_termios_fd), TCSANOW, &g_saved_termios);
    g_termios_active = 0;
  }
  ::signal(signal_number, SIG_DFL);
  ::raise(signal_number);
}

// 读取期间临时接管 SIGINT / SIGTERM。析构时无论如何都把终端还原回去。
// RAII 守卫：构造即接管 SIGINT / SIGTERM，析构即还原终端属性。之所以要接管
// 信号：默认处置会直接终止进程，析构函数根本不会跑，终端就停在无回显状态。
// 守卫只保证“终端属性被还原”，不保证进程活着。
//
// 不可拷贝：它代表一份独占的终端状态所有权，复制会让两个析构函数争着还原
// 同一个 fd。
class TerminalModeGuard {
 public:
  TerminalModeGuard(int fd, const struct termios& saved)
      : fd_(fd), saved_(saved) {
    g_saved_termios = saved;
    g_termios_fd = fd;
    g_termios_active = 1;
    previous_int_ = ::signal(SIGINT, RestoreAndReraise);
    previous_term_ = ::signal(SIGTERM, RestoreAndReraise);
  }

  ~TerminalModeGuard() {
    // 顺序很重要：先把 active 清掉，再还原，最后才把 handler 换回去。
    g_termios_active = 0;
    ::tcsetattr(fd_, TCSANOW, &saved_);
    ::signal(SIGINT, previous_int_);
    ::signal(SIGTERM, previous_term_);
  }

  TerminalModeGuard(const TerminalModeGuard&) = delete;
  TerminalModeGuard& operator=(const TerminalModeGuard&) = delete;

 private:
  int fd_;
  struct termios saved_;
  void (*previous_int_)(int) = SIG_DFL;
  void (*previous_term_)(int) = SIG_DFL;
};

// 短写循环：write(2) 允许只写出一部分；返回 false 表示“写失败了”，不代表
// “没写出去”：提示串可能只留在终端上一半。EINTR 必须重试，否则一个无害的
// 信号就能让提示串消失。
bool WriteAll(int fd, const std::string& text) {
  std::size_t written = 0;
  while (written < text.size()) {
    const ssize_t result =
        ::write(fd, text.data() + written, text.size() - written);
    if (result < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (result == 0) return false;
    written += static_cast<std::size_t>(result);
  }
  return true;
}

}  // namespace

// 打开控制终端而不是 stdin：stdin 可能是重定向文件、管道，甚至由别的进程
// 提供——从它读“口令”等于允许脚本注入。
// O_CLOEXEC 保证这个 fd 不会泄漏给之后 fork 出去的子进程；失败即失败，
// 绝不回落到 stdin（那正是本模块存在的理由）。
int OpenControllingTerminal(std::string* error_message) {
  const int fd = ::open("/dev/tty", O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    SetError(error_message,
             "Cannot open the controlling terminal /dev/tty: " +
                 std::string(std::strerror(errno)) +
                 ". An interactive terminal is required to enter a password; "
                 "passwords are never accepted from arguments, environment "
                 "variables or piped standard input.");
    return -1;
  }
  return fd;
}

// 读一行口令。前置条件：fd 是已打开的可读终端（require_tty 为 true 时必须
// 是 tty）。后置条件：成功时 *secret 是去掉换行的原始字节；失败时 *secret
// 保持为空。
//
// kMaxSecretBytes 是 fail-closed 的防线：超长输入作废整次读取，而不是截断后
// 当口令用——截断之后，“口令错误”和“输入太长”就再也分不开了。
bool ReadSecretLine(int fd, bool require_tty, const std::string& prompt,
                    std::string* secret, std::string* error_message) {
  if (error_message != nullptr) error_message->clear();
  if (secret == nullptr) {
    SetError(error_message, "Secret output must not be null");
    return false;
  }
  secret->clear();
  if (fd < 0) {
    SetError(error_message, "Cannot read a secret: no terminal is open");
    return false;
  }

  const bool is_terminal = ::isatty(fd) == 1;
  if (!is_terminal && require_tty) {
    SetError(error_message,
             "Cannot read a password: standard input is not an interactive "
             "terminal. Run this command from a terminal; passwords are never "
             "read from a pipe, a file, an argument or an environment "
             "variable.");
    return false;
  }

  struct termios saved;
  if (is_terminal) {
    if (::tcgetattr(fd, &saved) != 0) {
      SetError(error_message,
               "Cannot read a password: failed to inspect the terminal: " +
                   std::string(std::strerror(errno)));
      return false;
    }
  }

  if (!prompt.empty() && !WriteAll(fd, prompt)) {
    SetError(error_message,
             "Cannot read a password: failed to write the prompt");
    return false;
  }

  std::string buffer;
  {
    TerminalModeGuard* guard = nullptr;
    struct termios quiet;
    if (is_terminal) {
      quiet = saved;
      quiet.c_lflag &= static_cast<tcflag_t>(~ECHO);
      // TCSAFLUSH：把用户在提示出现之前就已经敲进去的字符丢掉，
      // 免得它们被当成密码的一部分。
      if (::tcsetattr(fd, TCSAFLUSH, &quiet) != 0) {
        SetError(error_message,
                 "Cannot read a password: failed to disable terminal echo: " +
                     std::string(std::strerror(errno)));
        return false;
      }
      guard = new TerminalModeGuard(fd, saved);
    }

    // 逐字节读：这里读的是行规程（canonical mode）下的终端，一次 read 最多
    // 返回一行。按字节读让 Ctrl+D（EOF）与 '\r' 的处理和长度上限都能就地
    // 判断，也免去为一行口令分配缓冲区。
    bool finished = false;
    bool failed = false;
    std::string failure;
    while (!finished) {
      char character = 0;
      const ssize_t count = ::read(fd, &character, 1);
      if (count < 0) {
        if (errno == EINTR) continue;
        failed = true;
        failure = "Cannot read a password: failed to read the terminal: " +
                  std::string(std::strerror(errno));
        break;
      }
      if (count == 0) {
        // EOF：终端被挂断，或者用户在空行上按了 Ctrl+D。
        finished = true;
        if (buffer.empty()) {
          failed = true;
          failure =
              "Cannot read a password: the input ended before a password "
              "was entered";
        }
        break;
      }
      if (character == '\n') {
        finished = true;
        break;
      }
      if (character == '\r') continue;
      buffer.push_back(character);
      if (buffer.size() > kMaxSecretBytes) {
        failed = true;
        failure = "Cannot read a password: the input exceeds " +
                  std::to_string(kMaxSecretBytes) + " bytes";
        break;
      }
    }
    // 显式 delete 守卫：先让析构恢复回显，再处理失败分支——失败路径也不会
    // 把终端留在无回显状态。非终端时 guard 为 nullptr，delete 是空操作。
    delete guard;
    if (failed) {
      SetError(error_message, failure);
      return false;
    }
  }

  if (is_terminal) {
    // ECHO 关着，用户敲的那个回车没有被回显；补一个换行，界面才正常。
    WriteAll(fd, "\n");
  }

  if (buffer.empty()) {
    SetError(error_message, "The password must not be empty");
    return false;
  }
  *secret = buffer;
  return true;
}

// 便捷包装：自己开 /dev/tty、读完关闭，供不需要复用 fd 的调用点使用。
// fd 只活在本函数内，不会跨调用泄漏，也不会被继承进子进程。
bool ReadSecretFromTerminal(const std::string& prompt, std::string* secret,
                            std::string* error_message) {
  std::string open_error;
  const int fd = OpenControllingTerminal(&open_error);
  if (fd < 0) {
    SetError(error_message, open_error);
    return false;
  }
  const bool ok =
      ReadSecretLine(fd, /*require_tty=*/true, prompt, secret, error_message);
  ::close(fd);
  return ok;
}

// 注册 / 改口令用的二次输入：两次读共用同一个 fd，用户在同一个终端里连输
// 两遍；只有逐字节完全相同才算成功（不做大小写或空白归一化）。
// 任一环节失败都不会写出 *secret，调用方按失败处理即可。
bool ReadSecretFromTerminalTwice(const std::string& prompt,
                                 const std::string& confirm_prompt,
                                 std::string* secret,
                                 std::string* error_message) {
  std::string open_error;
  const int fd = OpenControllingTerminal(&open_error);
  if (fd < 0) {
    SetError(error_message, open_error);
    return false;
  }

  std::string first;
  std::string second;
  bool ok =
      ReadSecretLine(fd, /*require_tty=*/true, prompt, &first, error_message);
  if (ok) {
    ok = ReadSecretLine(fd, /*require_tty=*/true, confirm_prompt, &second,
                        error_message);
  }
  ::close(fd);
  if (!ok) return false;

  if (first != second) {
    SetError(error_message, "The two passwords do not match");
    return false;
  }
  *secret = first;
  return true;
}

}  // namespace backupproject
