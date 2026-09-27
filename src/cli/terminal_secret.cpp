// terminal_secret.cpp

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

struct termios g_saved_termios;
volatile sig_atomic_t g_termios_active = 0;
volatile sig_atomic_t g_termios_fd = -1;

void RestoreAndReraise(int signal_number) {
  if (g_termios_active != 0) {
    ::tcsetattr(static_cast<int>(g_termios_fd), TCSANOW, &g_saved_termios);
    g_termios_active = 0;
  }
  ::signal(signal_number, SIG_DFL);
  ::raise(signal_number);
}

// 读取期间临时接管 SIGINT / SIGTERM。析构时无论如何都把终端还原回去。
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
