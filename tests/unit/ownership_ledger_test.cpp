#include "ownership_ledger.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using backupproject::OwnershipRecord;

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
  if (!ok) {
    ++failures;
    std::fprintf(stderr, "FAIL %s\n", what);
  } else {
    std::printf("ok   %s\n", what);
  }
}
std::string TempDir() {
  char pattern[] = "/tmp/ownership-ledger-XXXXXX";
  char* made = ::mkdtemp(pattern);
  return made == nullptr ? std::string() : std::string(made);
}
std::string MakeId(int value) {
  char text[33];
  std::snprintf(text, sizeof(text), "%032x", value);
  return text;
}
std::string MakeSha(int value) {
  char text[65];
  std::snprintf(text, sizeof(text), "%064x", value);
  return text;
}
}  // namespace

int main() {
  const std::string dir = TempDir();
  if (dir.empty()) return 2;
  const std::string ledger = dir + "/ledger";
  for (int i = 0; i < 40; ++i) {
    std::string error;
    if (!backupproject::AppendOwnershipRecord(
            ledger, OwnershipRecord{MakeId(i + 1), MakeSha(i + 1)}, &error)) {
      std::fprintf(stderr, "append failed: %s\n", error.c_str());
      return 2;
    }
  }
  std::vector<OwnershipRecord> records;
  std::string error;
  Check(backupproject::ReadOwnershipRecords(ledger, &records, &error),
        "A 读取 40 条长期归属记录");
  Check(records.size() == 40, "A 记录数超过 32 仍完整");
  struct stat info;
  Check(::stat(ledger.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600,
        "C 账本权限 0600");

  const std::string link = dir + "/link";
  Check(::symlink(ledger.c_str(), link.c_str()) == 0, "C 创建符号链接");
  Check(!backupproject::AppendOwnershipRecord(
            link, OwnershipRecord{MakeId(99), MakeSha(99)}, &error),
        "C 写入拒绝符号链接");
  Check(!backupproject::ReadOwnershipRecords(link, &records, &error),
        "C 读取拒绝符号链接");

  const std::string truncated = dir + "/truncated";
  {
    int fd = ::open(truncated.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    const char* text = "BPSOWN1 00000000000000000000000000000001 ";
    ::write(fd, text, std::strlen(text));
    ::close(fd);
  }
  Check(!backupproject::ReadOwnershipRecords(truncated, &records, &error),
        "C 截断记录被拒绝");

  const std::string conflict = dir + "/conflict";
  {
    int fd = ::open(conflict.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    std::string line = "BPSOWN1 " + MakeId(7) + " " + MakeSha(7) + "\n" +
                       "BPSOWN1 " + MakeId(7) + " " + MakeSha(8) + "\n";
    ::write(fd, line.data(), line.size());
    ::close(fd);
  }
  Check(!backupproject::ReadOwnershipRecords(conflict, &records, &error),
        "C 同 id 不同摘要被拒绝");

  // 双进程：管道屏障后同时追加各自的 25 条记录。
  const std::string shared = dir + "/shared";
  int barrier[2];
  if (::pipe(barrier) != 0) return 2;
  for (int child = 0; child < 2; ++child) {
    const pid_t pid = ::fork();
    if (pid == 0) {
      char token = 0;
      ::read(barrier[0], &token, 1);
      for (int i = 0; i < 25; ++i) {
        const int base = child * 100 + i + 1;
        std::string child_error;
        if (!backupproject::AppendOwnershipRecord(
                shared, OwnershipRecord{MakeId(base), MakeSha(base)},
                &child_error)) {
          _exit(3);
        }
      }
      _exit(0);
    }
  }
  ::close(barrier[0]);
  const char go[2] = {1, 1};
  ::write(barrier[1], go, sizeof(go));
  ::close(barrier[1]);
  int status = 0;
  for (int child = 0; child < 2; ++child) {
    ::wait(&status);
    Check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "C 双进程写入退出码 0");
  }
  Check(backupproject::ReadOwnershipRecords(shared, &records, &error),
        "C 双进程后读取成功");
  Check(records.size() == 50, "C 双进程 50 条记录无丢失");

  const std::string legacy = dir + "/legacy";
  {
    int fd = ::open(legacy.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    const char* text = "old-snapshot-name.bak\n";
    if (::write(fd, text, std::strlen(text)) < 0) return 2;
    ::close(fd);
  }
  Check(!backupproject::ReadOwnershipRecords(legacy, &records, &error),
        "C 旧名称格式被拒绝");

  const std::string bad_mode = dir + "/bad-mode";
  {
    int fd = ::open(bad_mode.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    std::string line = "BPSOWN1 " + MakeId(500) + " " + MakeSha(500) + "\n";
    if (::write(fd, line.data(), line.size()) < 0) return 2;
    ::close(fd);
  }
  if (::chmod(bad_mode.c_str(), 0644) != 0) return 2;
  Check(!backupproject::ReadOwnershipRecords(bad_mode, &records, &error),
        "C 非 0600 账本被拒绝");

  const std::string killed = dir + "/killed";
  const pid_t killer = ::fork();
  if (killer == 0) {
    int fd = ::open(killed.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) _exit(3);
    const char* partial = "BPSOWN1 0000000000000000000000000000000";
    if (::write(fd, partial, std::strlen(partial)) < 0) _exit(3);
    ::fsync(fd);
    ::kill(::getpid(), SIGKILL);
    _exit(4);
  }
  ::waitpid(killer, &status, 0);
  Check(!backupproject::ReadOwnershipRecords(killed, &records, &error),
        "C SIGKILL 造成的半行记录被拒绝");

  std::printf("ownership-ledger-test failures=%d\n", failures);
  return failures == 0 ? 0 : 1;
}
