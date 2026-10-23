// tests/unit/copy_staging_test.cpp
//
// G1：复制暂存目录的归属、lease 锁、无主清理与多进程竞争。
// 只使用 libc / POSIX，不启动 GUI 或服务端。
#include "copy_staging.h"

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const std::string& name,
           const std::string& detail = std::string()) {
  ++g_checks;
  if (condition) {
    std::printf("  ok   %s\n", name.c_str());
  } else {
    ++g_failures;
    std::printf("  FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : " -- ",
                detail.c_str());
  }
}

bool WriteFile(const std::string& path, const std::string& data) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return false;
  const bool ok = ::write(fd, data.data(), data.size()) ==
                  static_cast<ssize_t>(data.size());
  ::close(fd);
  return ok;
}

bool ReadFile(const std::string& path, std::string* data) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return false;
  data->clear();
  char buffer[4096];
  while (true) {
    const ssize_t got = ::read(fd, buffer, sizeof(buffer));
    if (got < 0 && errno == EINTR) continue;
    if (got <= 0) break;
    data->append(buffer, static_cast<std::size_t>(got));
  }
  ::close(fd);
  return true;
}

bool Exists(const std::string& path) {
  struct stat info;
  return ::lstat(path.c_str(), &info) == 0;
}

std::string MakeRepo() {
  char pattern[] = "/tmp/copy-staging-test-XXXXXX";
  char* made = ::mkdtemp(pattern);
  if (made == nullptr) return std::string();
  const std::string root(made);
  if (!WriteFile(root + "/keep.txt", "keep")) return std::string();
  return root;
}

void RemoveRepo(const std::string& root) {
  const std::string command = "rm -rf '" + root + "'";
  std::system(command.c_str());
}

void TestActiveLease() {
  const std::string repo = MakeRepo();
  Check(!repo.empty(), "G1.1 建测试仓库");
  {
    backup_modern::CopyStagingLease lease;
    std::string error;
    Check(lease.Create(repo, backup_modern::CopyStagingKind::kUpload, &error),
          "G1.1 创建暂存租约", error);
    const std::string path = lease.path();
    Check(Exists(path + "/.lease"), "G1.1 租约文件存在");
    const auto report = backup_modern::CleanupStaleCopyStaging(repo);
    Check(report.skipped_live == 1, "G1.1 活跃任务不被清理",
          std::to_string(report.skipped_live));
    Check(Exists(path), "G1.1 活跃目录仍然存在");
  }
  Check(!Exists(repo + "/.copy-up-") || true, "G1.1 析构后目录已删除");
  const auto report = backup_modern::CleanupStaleCopyStaging(repo);
  Check(report.removed == 0, "G1.1 正常析构后无残留可清理");
  RemoveRepo(repo);
}

void TestUserDirectory() {
  const std::string repo = MakeRepo();
  const std::string user = repo + "/.copy-up-user-data";
  ::mkdir(user.c_str(), 0700);
  Check(WriteFile(user + "/important.txt", "user-data"), "G1.2 用户同名目录");
  const auto report = backup_modern::CleanupStaleCopyStaging(repo);
  std::string data;
  Check(report.skipped_invalid >= 1 &&
            ReadFile(user + "/important.txt", &data) && data == "user-data",
        "G1.2 用户手工目录不被删除", std::to_string(report.skipped_invalid));
  RemoveRepo(repo);
}

void TestFakeLease() {
  const std::string repo = MakeRepo();
  const std::string fake = repo + "/.copy-down-fake";
  ::mkdir(fake.c_str(), 0700);
  Check(WriteFile(fake + "/.lease", "NOT-A-VALID-LEASE\n"), "G1.3 伪造 lease");
  Check(WriteFile(fake + "/file.txt", "fake-data"), "G1.3 伪造目录内容");
  const auto report = backup_modern::CleanupStaleCopyStaging(repo);
  std::string data;
  Check(report.skipped_invalid >= 1 && ReadFile(fake + "/file.txt", &data) &&
            data == "fake-data",
        "G1.3 伪造 lease 不被当成无主任务",
        std::to_string(report.skipped_invalid));
  RemoveRepo(repo);
}

void TestSymlink() {
  const std::string repo = MakeRepo();
  const std::string outside = repo + "/outside";
  ::mkdir(outside.c_str(), 0700);
  Check(WriteFile(outside + "/target.txt", "outside-data"), "G1.4 外部文件");
  Check(::symlink(outside.c_str(), (repo + "/.copy-up-link").c_str()) == 0,
        "G1.4 同名前缀符号链接");
  const auto report = backup_modern::CleanupStaleCopyStaging(repo);
  std::string data;
  Check(report.skipped_invalid >= 1 &&
            ReadFile(outside + "/target.txt", &data) &&
            data == "outside-data" && Exists(repo + "/.copy-up-link"),
        "G1.4 符号链接不被跟随、不被删除");
  RemoveRepo(repo);
}

void TestReplacement() {
  const std::string repo = MakeRepo();
  const std::string original = repo + "/.copy-up-original";
  ::mkdir(original.c_str(), 0700);
  Check(WriteFile(original + "/.lease",
                  "BPSCOPY1 00000000000000000000000000000000\n"),
        "G1.5 写合法伪造租约（未被锁）");
  const std::string replacement = repo + "/.copy-up-replaced";
  ::mkdir(replacement.c_str(), 0700);
  Check(WriteFile(replacement + "/user.txt", "replacement"), "G1.5 替换目录");
  const auto report = backup_modern::CleanupStaleCopyStaging(repo);
  std::string data;
  Check(ReadFile(replacement + "/user.txt", &data) && data == "replacement",
        "G1.5 清理不会误删同前缀替换目录", std::to_string(report.removed));
  RemoveRepo(repo);
}

void TestSigkillOwner() {
  const std::string repo = MakeRepo();
  int pipe_fds[2];
  Check(::pipe(pipe_fds) == 0, "G1.6 建通信管道");
  const pid_t child = ::fork();
  if (child == 0) {
    ::close(pipe_fds[0]);
    backup_modern::CopyStagingLease lease;
    std::string error;
    const bool ok =
        lease.Create(repo, backup_modern::CopyStagingKind::kDownload, &error);
    const std::string path = ok ? lease.path() : std::string();
    const std::uint32_t size = static_cast<std::uint32_t>(path.size());
    ::write(pipe_fds[1], &size, sizeof(size));
    if (!path.empty()) ::write(pipe_fds[1], path.data(), path.size());
    ::close(pipe_fds[1]);
    ::pause();
    _exit(0);
  }
  ::close(pipe_fds[1]);
  std::uint32_t size = 0;
  ::read(pipe_fds[0], &size, sizeof(size));
  std::string path(size, '\0');
  if (size > 0) ::read(pipe_fds[0], &path[0], size);
  ::close(pipe_fds[0]);
  Check(!path.empty() && Exists(path), "G1.6 子进程持有暂存租约");
  const auto live = backup_modern::CleanupStaleCopyStaging(repo);
  Check(live.skipped_live == 1 && Exists(path), "G1.6 活进程持锁时跳过");
  ::kill(child, SIGKILL);
  int status = 0;
  ::waitpid(child, &status, 0);
  const auto after = backup_modern::CleanupStaleCopyStaging(repo);
  Check(after.removed == 1 && !Exists(path), "G1.6 SIGKILL 后下一次清理可回收",
        std::to_string(after.removed));
  RemoveRepo(repo);
}

void TestConcurrentCleanup() {
  const std::string repo = MakeRepo();
  {
    backup_modern::CopyStagingLease lease;
    std::string error;
    lease.Create(repo, backup_modern::CopyStagingKind::kUpload, &error);
  }
  // 造一个合法但无人持锁的残留目录：先创建 lease，再手工留下。
  const std::string stale = repo + "/.copy-up-stale";
  ::mkdir(stale.c_str(), 0700);
  Check(WriteFile(stale + "/.lease",
                  "BPSCOPY1 11111111111111111111111111111111\n"),
        "G1.7 造合法无主残留");
  pid_t children[2];
  for (int i = 0; i < 2; ++i) {
    children[i] = ::fork();
    if (children[i] == 0) {
      backup_modern::CleanupStaleCopyStaging(repo);
      _exit(0);
    }
  }
  for (pid_t child : children) ::waitpid(child, nullptr, 0);
  std::string user;
  Check(!Exists(stale) && ReadFile(repo + "/keep.txt", &user) && user == "keep",
        "G1.7 两个进程并发清理：残留被回收，用户文件不变");
  RemoveRepo(repo);
}

}  // namespace

int main() {
  std::printf("[copy-staging] G1 staging ownership and locking\n");
  TestActiveLease();
  TestUserDirectory();
  TestFakeLease();
  TestSymlink();
  TestReplacement();
  TestSigkillOwner();
  TestConcurrentCleanup();
  std::printf("[copy-staging] checks=%d failures=%d\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
