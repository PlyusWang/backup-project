// terminal_secret_test.cpp
//
// 终端秘密读取器的专项测试。
//
// 两层：
//   1. 进程内单元测试：用管道与 PTY 直接喂 fd，覆盖"不是 TTY 就明确失败"、
//      "空密码失败"、"超长失败"、"EOF 失败"、"回显必须被恢复"；
//   2. 真实 PTY 集成测试：fork 一个子进程，用 setsid + TIOCSCTTY 把 PTY 变成
//      它的控制终端，然后调用**真正的产品入口** ReadSecretFromTerminal /
//      ReadSecretFromTerminalTwice —— 走的就是 backupctl 走的 /dev/tty 那条路。
//
// 父进程靠"在 PTY master 上等到提示文本出现"来同步，而不是 sleep：
// 提示语是产品自己写到终端上的，等它出现再喂输入，既没有竞态也不依赖时序。

#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <string>

#include "terminal_secret.h"
#include "test_support.h"

namespace bp = backupproject;

namespace {

// ---- 进程内：管道 ----

void TestWithoutTerminal() {
  test_support::Section("A. terminal_secret without a terminal");

  int fds[2];
  if (pipe(fds) != 0) {
    test_support::Check(false, "SEC-00 pipe creation");
    return;
  }
  const std::string payload = "piped-secret\n";
  ssize_t ignored = write(fds[1], payload.data(), payload.size());
  (void)ignored;
  close(fds[1]);

  std::string secret;
  std::string error;
  test_support::Check(!bp::ReadSecretLine(fds[0], /*require_tty=*/true, "pw: ",
                                          &secret, &error) &&
                          error.find("not an interactive terminal") !=
                              std::string::npos,
                      "SEC-01 a non-TTY is refused instead of silently read",
                      error);
  test_support::Check(secret.empty(),
                      "SEC-02 a refused read leaves no secret behind");

  // require_tty=false 只给测试用：同一根管道这次必须能读到。
  test_support::Check(bp::ReadSecretLine(fds[0], /*require_tty=*/false, "",
                                         &secret, &error),
                      "SEC-03 the injectable path reads from a pipe", error);
  test_support::Check(secret == "piped-secret",
                      "SEC-04 the piped value is read verbatim", secret);
  close(fds[0]);

  // 空密码。
  int empty_fds[2];
  if (pipe(empty_fds) == 0) {
    ignored = write(empty_fds[1], "\n", 1);
    (void)ignored;
    close(empty_fds[1]);
    test_support::Check(!bp::ReadSecretLine(empty_fds[0], false, "", &secret,
                                            &error) &&
                            error.find("must not be empty") != std::string::npos,
                        "SEC-05 an empty secret is refused", error);
    close(empty_fds[0]);
  }

  // EOF：写入端关闭且没有任何数据。
  int eof_fds[2];
  if (pipe(eof_fds) == 0) {
    close(eof_fds[1]);
    test_support::Check(!bp::ReadSecretLine(eof_fds[0], false, "", &secret,
                                            &error) &&
                            error.find("input ended") != std::string::npos,
                        "SEC-06 EOF is refused, not treated as an empty secret",
                        error);
    close(eof_fds[0]);
  }

  // 超长。
  int long_fds[2];
  if (pipe(long_fds) == 0) {
    const std::string huge(bp::kMaxSecretBytes + 64, 'x');
    ignored = write(long_fds[1], huge.data(), huge.size());
    (void)ignored;
    ignored = write(long_fds[1], "\n", 1);
    (void)ignored;
    close(long_fds[1]);
    test_support::Check(!bp::ReadSecretLine(long_fds[0], false, "", &secret,
                                            &error) &&
                            error.find("exceeds") != std::string::npos,
                        "SEC-07 an over-long secret is refused", error);
    close(long_fds[0]);
  }

  test_support::Check(!bp::ReadSecretLine(-1, false, "", &secret, &error),
                      "SEC-08 a closed descriptor is refused", error);
}

// ---- 进程内：PTY ----

struct Pty {
  int master = -1;
  int slave = -1;
  std::string name;
};

bool OpenPty(Pty* pty, std::string* error) {
  pty->master = posix_openpt(O_RDWR | O_NOCTTY);
  if (pty->master < 0) {
    *error = "posix_openpt failed";
    return false;
  }
  if (grantpt(pty->master) != 0 || unlockpt(pty->master) != 0) {
    *error = "grantpt/unlockpt failed";
    return false;
  }
  const char* name = ptsname(pty->master);
  if (name == nullptr) {
    *error = "ptsname failed";
    return false;
  }
  pty->name = name;
  pty->slave = open(name, O_RDWR | O_NOCTTY);
  if (pty->slave < 0) {
    *error = "open(slave) failed";
    return false;
  }
  return true;
}

void ClosePty(Pty* pty) {
  if (pty->slave >= 0) close(pty->slave);
  if (pty->master >= 0) close(pty->master);
  pty->slave = -1;
  pty->master = -1;
}

void TestWithPty() {
  test_support::Section("B. terminal_secret with a PTY");
  Pty pty;
  std::string error;
  if (!OpenPty(&pty, &error)) {
    test_support::Note("PTY is unavailable in this environment: " + error);
    return;
  }

  test_support::Check(isatty(pty.slave) == 1, "SEC-10 the PTY slave is a tty");

  struct termios before;
  test_support::Check(tcgetattr(pty.slave, &before) == 0,
                      "SEC-11 the PTY termios is readable");
  test_support::Check((before.c_lflag & ECHO) != 0,
                      "SEC-12 echo starts enabled");

  const std::string payload = "pty-secret\n";
  ssize_t ignored = write(pty.master, payload.data(), payload.size());
  (void)ignored;

  std::string secret;
  test_support::Check(bp::ReadSecretLine(pty.slave, /*require_tty=*/true,
                                         "password: ", &secret, &error),
                      "SEC-13 a secret is read from a real tty", error);
  test_support::Check(secret == "pty-secret",
                      "SEC-14 the tty value is read verbatim", secret);

  struct termios after;
  test_support::Check(tcgetattr(pty.slave, &after) == 0 &&
                          (after.c_lflag & ECHO) != 0,
                      "SEC-15 echo is restored after the read");

  // 提示语确实被写到了终端上（父进程在上面的 PTY 集成测试里就是靠它同步的）。
  char buffer[128];
  const ssize_t count = read(pty.master, buffer, sizeof(buffer) - 1);
  if (count > 0) {
    buffer[count] = '\0';
    test_support::Check(std::string(buffer).find("password: ") != std::string::npos,
                        "SEC-16 the prompt is written to the terminal", buffer);
  } else {
    test_support::Check(false, "SEC-16 the prompt is written to the terminal",
                        "nothing readable on the master");
  }

  ClosePty(&pty);
}

// ---- 真实 /dev/tty 集成测试 ----

struct ChildResult {
  int first_ok = 0;
  int mismatch_refused = 0;
  int match_ok = 0;
  int echo_restored = 0;
  char first_secret[64];
  char match_secret[64];
  char mismatch_error[192];
};

// 在 PTY master 上等待一段文本出现。返回是否等到。
class PromptReader {
 public:
  explicit PromptReader(int fd) : fd_(fd) {}

  bool WaitFor(const std::string& expected, int timeout_ms) {
    while (true) {
      const std::size_t found = buffer_.find(expected);
      if (found != std::string::npos) {
        buffer_.erase(0, found + expected.size());
        return true;
      }
      fd_set set;
      FD_ZERO(&set);
      FD_SET(fd_, &set);
      struct timeval timeout;
      timeout.tv_sec = timeout_ms / 1000;
      timeout.tv_usec = (timeout_ms % 1000) * 1000;
      const int ready = select(fd_ + 1, &set, nullptr, nullptr, &timeout);
      if (ready <= 0) return false;
      char chunk[256];
      const ssize_t count = read(fd_, chunk, sizeof(chunk));
      if (count <= 0) return false;
      buffer_.append(chunk, static_cast<std::size_t>(count));
      if (buffer_.size() > 8192) buffer_.erase(0, buffer_.size() - 4096);
    }
  }

 private:
  int fd_;
  std::string buffer_;
};

void RunChild(int slave, int result_fd) {
  // 把 PTY 变成自己的控制终端，/dev/tty 才会指向它。setsid() 要求调用者不是
  // 进程组组长，所以自己再 fork 一层是必须的——这里由父进程保证。
  setsid();
  ioctl(slave, TIOCSCTTY, 0);

  ChildResult result = ChildResult();

  std::string secret;
  std::string error;
  if (bp::ReadSecretFromTerminal("password: ", &secret, &error)) {
    result.first_ok = 1;
    std::snprintf(result.first_secret, sizeof(result.first_secret), "%s",
                  secret.c_str());
  }

  secret.clear();
  error.clear();
  if (!bp::ReadSecretFromTerminalTwice("pass: ", "again: ", &secret, &error)) {
    result.mismatch_refused =
        error.find("do not match") != std::string::npos ? 1 : 0;
    std::snprintf(result.mismatch_error, sizeof(result.mismatch_error), "%s",
                  error.c_str());
  }

  secret.clear();
  error.clear();
  if (bp::ReadSecretFromTerminalTwice("pass: ", "again: ", &secret, &error)) {
    result.match_ok = 1;
    std::snprintf(result.match_secret, sizeof(result.match_secret), "%s",
                  secret.c_str());
  }

  struct termios current;
  if (tcgetattr(slave, &current) == 0 && (current.c_lflag & ECHO) != 0) {
    result.echo_restored = 1;
  }

  ssize_t ignored = write(result_fd, &result, sizeof(result));
  (void)ignored;
  _exit(0);
}

void TestControllingTerminal() {
  test_support::Section("C. terminal_secret through the real /dev/tty");
  Pty pty;
  std::string error;
  if (!OpenPty(&pty, &error)) {
    test_support::Note("PTY is unavailable in this environment: " + error);
    return;
  }

  int pipes[2];
  if (pipe(pipes) != 0) {
    test_support::Check(false, "SEC-20 result pipe");
    ClosePty(&pty);
    return;
  }

  const pid_t child = fork();
  if (child == 0) {
    close(pipes[0]);
    close(pty.master);
    RunChild(pty.slave, pipes[1]);
    _exit(0);
  }
  close(pipes[1]);

  PromptReader reader(pty.master);
  bool fed = true;
  fed = fed && reader.WaitFor("password: ", 5000);
  if (fed) { ssize_t n = write(pty.master, "s3cret\n", 7); (void)n; }
  fed = fed && reader.WaitFor("pass: ", 5000);
  if (fed) { ssize_t n = write(pty.master, "one\n", 4); (void)n; }
  fed = fed && reader.WaitFor("again: ", 5000);
  if (fed) { ssize_t n = write(pty.master, "two\n", 4); (void)n; }
  fed = fed && reader.WaitFor("pass: ", 5000);
  if (fed) { ssize_t n = write(pty.master, "same\n", 5); (void)n; }
  fed = fed && reader.WaitFor("again: ", 5000);
  if (fed) { ssize_t n = write(pty.master, "same\n", 5); (void)n; }

  ChildResult result;
  const ssize_t got = read(pipes[0], &result, sizeof(result));
  close(pipes[0]);
  int status = 0;
  waitpid(child, &status, 0);
  ClosePty(&pty);

  test_support::Check(fed, "SEC-20 every prompt reached the parent in time");
  test_support::Check(got == static_cast<ssize_t>(sizeof(result)),
                      "SEC-21 the child reported its results",
                      std::to_string(got));
  if (got != static_cast<ssize_t>(sizeof(result))) return;

  test_support::Check(result.first_ok == 1,
                      "SEC-22 ReadSecretFromTerminal reads /dev/tty",
                      result.first_secret);
  test_support::Check(std::string(result.first_secret) == "s3cret",
                      "SEC-23 the /dev/tty secret is exact", result.first_secret);
  test_support::Check(result.mismatch_refused == 1,
                      "SEC-24 two different passwords are refused",
                      result.mismatch_error);
  test_support::Check(result.match_ok == 1 && std::string(result.match_secret) == "same",
                      "SEC-25 two matching passwords are accepted",
                      result.match_secret);
  test_support::Check(result.echo_restored == 1,
                      "SEC-26 echo survives the whole session");
}

}  // namespace

int main() {
  std::printf("terminal secret test\n");
  TestWithoutTerminal();
  TestWithPty();
  TestControllingTerminal();
  return test_support::Finish("terminal secret");
}
