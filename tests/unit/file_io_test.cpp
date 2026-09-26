// file_io_test.cpp
//
// file_io.h 的状态机与权限回归：FileSink 的"路径所有权 vs fd"、私有工作目录、
// 不覆盖发布、可用空间 sanity check。
//
// 为什么每条都要真的落盘再 lstat：0600 / 0700 / O_EXCL / 失败之后必须 unlink
// 全是内核侧行为，读代码看不出 umask、mkstemp 的默认模式，也看不出注入的 hook
// 有没有真的生效。
//
// fsync / close 失败没法天然制造，所以走 file_io.h 里的注入点。每次注入都用
// RAII 守卫还原：任何一个用例提前 return，也不会把"总是失败"留给后面的用例。

#include "file_io.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "test_support.h"

namespace {

using backupproject::CheckFreeSpace;
using backupproject::FileSink;
using backupproject::PublishNoReplace;
using backupproject::RemoveTreeNoFollow;
using backupproject::TempDirectoryGuard;
namespace syscalls = backupproject::file_io_syscalls;

// 权限断言基于真实 stat：失败信息里带上实际模式，一眼能看出是 0644 还是 0000。
std::uint32_t ModeOf(const std::string& path) {
  struct stat info;
  if (::lstat(path.c_str(), &info) != 0) {
    return 0;
  }
  return static_cast<std::uint32_t>(info.st_mode & 07777);
}

void CheckMode(const std::string& path, std::uint32_t expected,
               const std::string& label) {
  const std::uint32_t actual = ModeOf(path);
  const std::string detail = actual == 0
                                 ? std::string("file is missing")
                                 : "expected " + test_support::Octal(expected) +
                                       ", got " + test_support::Octal(actual);
  test_support::Check(actual == expected, label, detail);
}

bool IsDirectory(const std::string& path) {
  struct stat info;
  return ::lstat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

// 注入"总是失败的 fsync"。errno 必须是 EIO：否则错误信息里会出现
// "Success" 这种自相矛盾的描述，反而掩盖注入有没有生效。
int FailingFsync(int fd) {
  (void)fd;
  errno = EIO;
  return -1;
}

// 真实的 close 失败（例如回写错误）在 Linux 上仍然会释放 fd，所以这里真的关
// 掉再返回 -1：既模拟了失败语义，也不会让测试进程漏掉文件描述符。
int FailingClose(int fd) {
  (void)::close(fd);
  errno = EIO;
  return -1;
}

class ScopedFsyncHook {
 public:
  explicit ScopedFsyncHook(syscalls::FsyncFn replacement)
      : previous_(syscalls::FsyncHook()) {
    syscalls::FsyncHook() = replacement;
  }
  ~ScopedFsyncHook() { syscalls::FsyncHook() = previous_; }
  ScopedFsyncHook(const ScopedFsyncHook&) = delete;
  ScopedFsyncHook& operator=(const ScopedFsyncHook&) = delete;

 private:
  syscalls::FsyncFn previous_;
};

class ScopedCloseHook {
 public:
  explicit ScopedCloseHook(syscalls::CloseFn replacement)
      : previous_(syscalls::CloseHook()) {
    syscalls::CloseHook() = replacement;
  }
  ~ScopedCloseHook() { syscalls::CloseHook() = previous_; }
  ScopedCloseHook(const ScopedCloseHook&) = delete;
  ScopedCloseHook& operator=(const ScopedCloseHook&) = delete;

 private:
  syscalls::CloseFn previous_;
};

// hook 还原之后 Close 必须恢复正常：这是"注入没有泄漏到下一条用例"的证据。
void CheckCloseStillWorks(const std::string& root, const std::string& name) {
  const std::string path = root + "/" + name;
  FileSink sink;
  std::string error;
  const bool ok = sink.Open(path, &error) &&
                  sink.Write("restored", 8, &error) && sink.Close(&error);
  test_support::Check(ok, "hook restored: Close() works again", error);
  test_support::Check(sink.committed(),
                      "hook restored: commit state is correct");
}

void TestOpenThenAbandon(const std::string& root) {
  test_support::Section("FileSink: Open -> Abandon");
  const std::string path = root + "/open-abandon.bin";
  FileSink sink;
  std::string error;
  test_support::Check(sink.Open(path, &error), "Open succeeds", error);
  test_support::Check(test_support::Exists(path),
                      "Open creates the file immediately (O_CREAT)");
  test_support::Check(sink.owns_path(), "Open takes ownership of the path");
  test_support::Check(sink.fd() >= 0, "Open leaves a usable descriptor");
  test_support::Check(sink.path() == path, "path() reports the opened path");
  sink.Abandon();
  test_support::Check(!test_support::Exists(path),
                      "Abandon removes an untouched output file");
  test_support::Check(!sink.owns_path(), "Abandon releases the path");
  test_support::Check(sink.fd() == -1, "Abandon closes the descriptor");
}

void TestWriteThenAbandon(const std::string& root) {
  test_support::Section("FileSink: write then Abandon");
  const std::string small = root + "/write-abandon.bin";
  const std::string payload = "half written payload";
  FileSink sink;
  std::string error;
  test_support::Check(sink.Open(small, &error), "Open succeeds", error);
  test_support::Check(sink.Write(payload.data(), payload.size(), &error),
                      "Write succeeds", error);
  test_support::Check(sink.bytes_written() == payload.size(),
                      "bytes_written() counts bytes still in the buffer");
  test_support::Check(test_support::Exists(small),
                      "the partial file is on disk");
  sink.Abandon();
  test_support::Check(!test_support::Exists(small),
                      "Abandon removes the partial file");
  test_support::Check(!sink.owns_path(), "Abandon releases the path");

  // 超过内部缓冲（256 KiB）的写入会走"直接写 fd"这条分支，清理语义必须一致。
  const std::string large = root + "/write-abandon-large.bin";
  const std::string big(300 * 1024, 'L');
  FileSink big_sink;
  error.clear();
  test_support::Check(big_sink.Open(large, &error), "Open (large) succeeds",
                      error);
  test_support::Check(big_sink.Write(big.data(), big.size(), &error),
                      "Write (large) succeeds", error);
  test_support::Check(big_sink.bytes_written() == big.size(),
                      "bytes_written() counts flushed and buffered bytes");
  big_sink.Abandon();
  test_support::Check(!test_support::Exists(large),
                      "Abandon removes a file written past the buffer size");
}

void TestCommitThenAbandon(const std::string& root) {
  test_support::Section("FileSink: successful Close is committed");
  const std::string path = root + "/committed.bin";
  const std::string payload(1000, 'c');
  FileSink sink;
  std::string error;
  test_support::Check(sink.Open(path, &error), "Open succeeds", error);
  test_support::Check(sink.Write(payload.data(), payload.size(), &error),
                      "Write succeeds", error);
  test_support::Check(sink.Close(&error), "Close succeeds", error);
  test_support::Check(sink.committed(), "committed() == true after Close");
  test_support::Check(test_support::Exists(path), "the committed file exists");
  test_support::Check(sink.fd() == -1, "fd() == -1 after Close");
  std::string content;
  test_support::Check(test_support::ReadFile(path, &content),
                      "the committed file can be read back");
  test_support::Check(content == payload,
                      "the committed file has the exact bytes");
  CheckMode(path, 0600, "the committed file is 0600");
  sink.Abandon();
  test_support::Check(test_support::Exists(path),
                      "Abandon after commit does not delete the file");
  test_support::Check(sink.committed(), "committed() stays true after Abandon");
}

// fsync 失败：fd 已经关掉，但路径还归本对象所有，所以 Abandon 仍然必须 unlink。
void TestFsyncFailure(const std::string& root) {
  test_support::Section("FileSink: fsync failure keeps path ownership");
  const std::string path = root + "/fsync-fail.bin";
  {
    ScopedFsyncHook hook(&FailingFsync);
    FileSink sink;
    std::string error;
    test_support::Check(sink.Open(path, &error), "Open succeeds", error);
    test_support::Check(sink.Write("payload", 7, &error), "Write succeeds",
                        error);
    const bool closed = sink.Close(&error);
    test_support::Check(!closed, "Close() returns false when fsync fails");
    test_support::Check(!error.empty(), "Close() reports the fsync failure",
                        error);
    test_support::Check(!sink.committed(),
                        "committed() == false after a failed fsync");
    test_support::Check(sink.owns_path(),
                        "owns_path() == true: the path is still ours");
    test_support::Check(sink.fd() == -1,
                        "fd() == -1: the descriptor is already closed");
    test_support::Check(test_support::Exists(path),
                        "the partial file is still on disk before cleanup");
    sink.Abandon();
    test_support::Check(!test_support::Exists(path),
                        "Abandon unlinks the file even though fd is gone");
    test_support::Check(!sink.owns_path(), "Abandon releases the path");
  }
  CheckCloseStillWorks(root, "fsync-hook-restored.bin");
}

// close 失败：与 fsync 失败同样是"fd 关了但路径还在"，清理必须照做。
void TestCloseFailure(const std::string& root) {
  test_support::Section("FileSink: close failure keeps path ownership");
  const std::string path = root + "/close-fail.bin";
  {
    ScopedCloseHook hook(&FailingClose);
    FileSink sink;
    std::string error;
    test_support::Check(sink.Open(path, &error), "Open succeeds", error);
    test_support::Check(sink.Write("payload", 7, &error), "Write succeeds",
                        error);
    const bool closed = sink.Close(&error);
    test_support::Check(!closed, "Close() returns false when close fails");
    test_support::Check(!error.empty(), "Close() reports the close failure",
                        error);
    test_support::Check(!sink.committed(),
                        "committed() == false after a failed close");
    test_support::Check(sink.owns_path(),
                        "owns_path() == true after a failed close");
    test_support::Check(sink.fd() == -1, "fd() == -1 after a failed close");
    sink.Abandon();
    test_support::Check(!test_support::Exists(path),
                        "Abandon still unlinks after a failed close");
  }
  CheckCloseStillWorks(root, "close-hook-restored.bin");
}

void TestRepeatedAbandon(const std::string& root) {
  test_support::Section("FileSink: repeated Abandon");
  const std::string path = root + "/abandon-twice.bin";
  {
    FileSink sink;
    std::string error;
    test_support::Check(sink.Open(path, &error), "Open succeeds", error);
    test_support::Check(sink.Write("x", 1, &error), "Write succeeds", error);
    sink.Abandon();
    sink.Abandon();
    test_support::Check(!sink.owns_path(), "a second Abandon is a no-op");
    test_support::Check(sink.fd() == -1, "a second Abandon keeps fd() == -1");
  }
  test_support::Check(!test_support::Exists(path), "the file stayed deleted");

  const std::string committed = root + "/abandon-twice-committed.bin";
  {
    FileSink sink;
    std::string error;
    test_support::Check(sink.Open(committed, &error),
                        "Open (committed) succeeds", error);
    test_support::Check(sink.Close(&error), "Close (committed) succeeds",
                        error);
    sink.Abandon();
    sink.Abandon();
  }
  test_support::Check(test_support::Exists(committed),
                      "Abandon after commit never deletes the final file");
}

void TestOpenExisting(const std::string& root) {
  test_support::Section("FileSink: O_EXCL never overwrites");
  const std::string path = root + "/existing.bin";
  const std::string original = "original content must survive";
  test_support::Check(test_support::WriteFile(path, original, 0640),
                      "fixture file created");
  FileSink sink;
  std::string error;
  test_support::Check(!sink.Open(path, &error),
                      "Open on an existing path must fail");
  test_support::Check(!error.empty(), "the failure is explained", error);
  test_support::Check(!sink.owns_path(),
                      "a failed Open takes no ownership (no foreign unlink)");
  test_support::Check(sink.fd() == -1, "a failed Open leaves no descriptor");
  std::string content;
  test_support::Check(
      test_support::ReadFile(path, &content) && content == original,
      "the existing file content is untouched");
  CheckMode(path, 0640, "the existing file mode is untouched");

  // 失败的 Open 不能把对象弄成"半开"状态：换一个路径必须还能用。
  const std::string other = root + "/after-failed-open.bin";
  error.clear();
  test_support::Check(sink.Open(other, &error),
                      "the sink is still usable after a failed Open", error);
  sink.Abandon();
  test_support::Check(!test_support::Exists(other),
                      "the recovered sink cleans up normally");
}

void TestOpenTemp(const std::string& root) {
  test_support::Section("FileSink: OpenTemp");
  const std::string directory = root + "/temp-sink";
  test_support::Check(test_support::Mkdir(directory, 0700),
                      "temp sink directory created");
  FileSink first;
  FileSink second;
  std::string error;
  test_support::Check(first.OpenTemp(directory, "chunk-", &error),
                      "OpenTemp succeeds", error);
  test_support::Check(second.OpenTemp(directory, "chunk-", &error),
                      "a second OpenTemp succeeds", error);
  test_support::Check(first.path() != second.path(),
                      "OpenTemp names are unique");
  CheckMode(first.path(), 0600, "the OpenTemp file is 0600");
  test_support::Check(
      first.path().compare(0, directory.size() + 1, directory + "/") == 0,
      "the OpenTemp file lives in the requested directory");
  test_support::Check(first.Write("temp", 4, &error) && first.Close(&error),
                      "the OpenTemp file can be written and committed", error);
  CheckMode(first.path(), 0600, "the committed OpenTemp file is still 0600");
  const std::string committed_temp = first.path();
  // first 已经提交：提交过的产物即使名字像临时文件也不能被 Abandon 删掉。
  first.Abandon();
  test_support::Check(test_support::Exists(committed_temp),
                      "Abandon never deletes a committed OpenTemp file");
  test_support::Check(::unlink(committed_temp.c_str()) == 0,
                      "the test removes the committed temp file itself");
  // second 从未提交：Abandon 必须把它删掉。
  const std::string abandoned_temp = second.path();
  second.Abandon();
  test_support::Check(!test_support::Exists(abandoned_temp),
                      "Abandon removes an uncommitted OpenTemp file");
}

void TestPublishNoReplace(const std::string& root) {
  test_support::Section("PublishNoReplace");
  const std::string base = root + "/publish";
  test_support::Check(test_support::Mkdir(base, 0755), "publish root created");
  const std::string work = base + "/work";
  const std::string out = base + "/out";
  const std::string other = base + "/other";
  test_support::Check(test_support::Mkdir(work, 0755), "work dir created");
  test_support::Check(test_support::Mkdir(out, 0755), "out dir created");
  test_support::Check(test_support::Mkdir(other, 0755), "other dir created");

  // 目标不存在：发布成功，temp 名字消失，最终文件是 0600。
  const std::string temp = work + "/.bptmp-first";
  const std::string final_path = out + "/final.bak";
  test_support::Check(test_support::WriteFile(temp, "fresh payload", 0600),
                      "temp fixture written");
  std::string error;
  test_support::Check(PublishNoReplace(temp, final_path, &error),
                      "publishing onto a free path succeeds", error);
  test_support::Check(test_support::Exists(final_path),
                      "the published file exists");
  test_support::Check(!test_support::Exists(temp),
                      "the temp name is gone after a successful publish");
  CheckMode(final_path, 0600, "the published file is 0600");
  std::string content;
  test_support::Check(test_support::ReadFile(final_path, &content) &&
                          content == "fresh payload",
                      "the published file has the exact bytes");

  // 目标已存在：必须失败，目标内容不变，temp 留给调用方清理。
  const std::string taken = out + "/taken.bak";
  test_support::Check(test_support::WriteFile(taken, "original", 0600),
                      "existing target written");
  const std::string temp_two = work + "/.bptmp-second";
  test_support::Check(test_support::WriteFile(temp_two, "replacement", 0600),
                      "second temp fixture written");
  error.clear();
  test_support::Check(!PublishNoReplace(temp_two, taken, &error),
                      "publishing onto an existing path must fail");
  test_support::Check(!error.empty(), "the refusal is explained", error);
  content.clear();
  test_support::Check(
      test_support::ReadFile(taken, &content) && content == "original",
      "the existing target content is untouched");
  CheckMode(taken, 0600, "the existing target mode is untouched");
  test_support::Check(
      test_support::Exists(temp_two),
      "the temp file survives a refused publish (caller cleans)");
  test_support::Check(::unlink(temp_two.c_str()) == 0,
                      "the caller can remove the leftover temp file");
  test_support::Check(!test_support::Exists(temp_two),
                      "the leftover temp file is gone");

  // 跨目录发布：temp 与 final 不在同一个目录（同一文件系统），link 依然成立。
  const std::string temp_three = work + "/.bp-work-cross";
  const std::string cross_final = other + "/cross.bak";
  test_support::Check(test_support::WriteFile(temp_three, "cross dir", 0600),
                      "cross-directory temp fixture written");
  error.clear();
  test_support::Check(PublishNoReplace(temp_three, cross_final, &error),
                      "publishing across directories succeeds", error);
  test_support::Check(test_support::Exists(cross_final),
                      "the cross-directory target exists");
  test_support::Check(!test_support::Exists(temp_three),
                      "the cross-directory temp name is gone");
  content.clear();
  test_support::Check(
      test_support::ReadFile(cross_final, &content) && content == "cross dir",
      "the cross-directory target has the exact bytes");
}

void TestTempDirectoryGuard(const std::string& root) {
  test_support::Section("TempDirectoryGuard");
  TempDirectoryGuard guard;
  std::string error;
  test_support::Check(guard.Create(root, ".bp-work-", &error),
                      "Create succeeds", error);
  test_support::Check(guard.active(), "the guard is active after Create");
  const std::string path = guard.path();
  test_support::Check(
      !path.empty() && path.find("/.bp-work-") != std::string::npos,
      "the workspace name carries the requested prefix");
  test_support::Check(IsDirectory(path), "the workspace is a directory");
  CheckMode(path, 0700, "the private workspace is 0700");

  const std::string child = guard.Child("payload.bin");
  test_support::Check(child == path + "/payload.bin",
                      "Child() resolves inside the workspace");
  FileSink child_sink;
  test_support::Check(child_sink.Open(child, &error),
                      "a file can be created through Child()", error);
  test_support::Check(
      child_sink.Write("secret", 6, &error) && child_sink.Close(&error),
      "the Child() file can be committed", error);
  CheckMode(child, 0600, "a file created through Child() is 0600");

  FileSink temp_sink;
  error.clear();
  test_support::Check(temp_sink.OpenTemp(path, "chunk-", &error),
                      "OpenTemp works inside the workspace", error);
  CheckMode(temp_sink.path(), 0600, "an OpenTemp file inside it is 0600");
  test_support::Check(
      temp_sink.Write("x", 1, &error) && temp_sink.Close(&error),
      "the workspace OpenTemp file is committable", error);

  guard.Remove();
  test_support::Check(!test_support::Exists(path),
                      "Remove deletes the directory and its contents");
  test_support::Check(!guard.active(), "the guard is inactive after Remove");
  guard.Remove();
  test_support::Check(!test_support::Exists(path),
                      "a second Remove is a no-op");

  // 析构路径：忘记显式 Remove 也必须清理（RAII 的意义就在这里）。
  std::string destroyed;
  {
    TempDirectoryGuard scoped;
    test_support::Check(scoped.Create(root, ".bp-work-scoped-", &error),
                        "scoped Create succeeds", error);
    destroyed = scoped.path();
    test_support::Check(
        test_support::WriteFile(scoped.Child("inside.bin"), "x", 0600),
        "file inside the scoped workspace created");
    test_support::Check(test_support::Exists(scoped.Child("inside.bin")),
                        "the scoped file exists before destruction");
  }
  test_support::Check(!test_support::Exists(destroyed),
                      "the destructor removes the workspace and its contents");

  // Release：目录已经被 rename 走时，守卫必须放手而不是把它删掉。
  {
    TempDirectoryGuard released;
    test_support::Check(released.Create(root, ".bp-work-released-", &error),
                        "release Create succeeds", error);
    const std::string kept = released.path();
    released.Release();
    test_support::Check(!released.active(), "Release clears the path");
    test_support::Check(test_support::Exists(kept),
                        "Release does not delete the directory");
    RemoveTreeNoFollow(kept);
    test_support::Check(!test_support::Exists(kept),
                        "the test cleans up the released directory itself");
  }
}

void TestCheckFreeSpace(const std::string& root) {
  test_support::Section("CheckFreeSpace");
  std::string error;
  test_support::Check(CheckFreeSpace(root, 4096, &error),
                      "a tiny requirement is accepted", error);
  test_support::Check(CheckFreeSpace(root, 0, &error),
                      "a zero requirement is accepted", error);
  error.clear();
  test_support::Check(!CheckFreeSpace(root, 1ull << 62, &error),
                      "an absurd requirement is rejected");
  test_support::Check(!error.empty(), "the rejection explains the shortfall",
                      error);
}

// 把 link / renameat2 换成脚本化的返回值，析构时自动还原。
// 三个真实分支（link 成功 / link 不可用而 renameat2 成功 / 两个都不可用）
// 在正常文件系统上没法同时制造出来，所以必须能注入。
class ScopedPublishHooks {
 public:
  ScopedPublishHooks(syscalls::LinkFn link_hook,
                     syscalls::RenameNoReplaceFn rename_hook)
      : saved_link_(syscalls::LinkHook()),
        saved_rename_(syscalls::RenameNoReplaceHook()) {
    syscalls::LinkHook() = link_hook;
    syscalls::RenameNoReplaceHook() = rename_hook;
  }
  ~ScopedPublishHooks() {
    syscalls::LinkHook() = saved_link_;
    syscalls::RenameNoReplaceHook() = saved_rename_;
  }
  ScopedPublishHooks(const ScopedPublishHooks&) = delete;
  ScopedPublishHooks& operator=(const ScopedPublishHooks&) = delete;

 private:
  syscalls::LinkFn saved_link_;
  syscalls::RenameNoReplaceFn saved_rename_;
};

int LinkEexist(const char*, const char*) {
  errno = EEXIST;
  return -1;
}

int LinkUnsupported(const char*, const char*) {
  errno = EOPNOTSUPP;
  return -1;
}

int LinkEacces(const char*, const char*) {
  errno = EACCES;
  return -1;
}

// "这个文件系统没有 renameat2" 的正常替身：真的挪过去，语义与
// RENAME_NOREPLACE 在"目标不存在"时一致。
int RenameNoReplaceReal(const char* old_path, const char* new_path) {
  return ::rename(old_path, new_path);
}

int RenameEnosys(const char*, const char*) {
  errno = ENOSYS;
  return -1;
}

int RenameEio(const char*, const char*) {
  errno = EIO;
  return -1;
}

// §1：两种原子 no-replace 都不可用时必须 fail closed，绝不能退回普通 rename。
void TestPublishAtomicFallbacks(const std::string& root) {
  test_support::Section("PublishNoReplace: atomic-only fallbacks");
  const std::string base = root + "/publish-atomic";
  test_support::Check(test_support::Mkdir(base, 0755), "atomic root created");
  const std::string work = base + "/work";
  test_support::Check(test_support::Mkdir(work, 0755),
                      "atomic work dir created");

  // 1) link 成功（走真实实现）：发布成功。
  {
    const std::string temp = work + "/link-ok.tmp";
    const std::string final_path = base + "/link-ok.bak";
    test_support::Check(test_support::WriteFile(temp, "via link", 0600),
                        "link fixture written");
    std::string error;
    test_support::Check(PublishNoReplace(temp, final_path, &error),
                        "link success publishes", error);
    std::string content;
    test_support::Check(
        test_support::ReadFile(final_path, &content) && content == "via link",
        "link success keeps the bytes");
    test_support::Check(!test_support::Exists(temp),
                        "link success removes the temp name");
  }

  // 2) link 返回 EEXIST：立即失败，已有目标的字节一字不改。
  {
    const std::string final_path = base + "/taken.bak";
    test_support::Check(
        test_support::WriteFile(final_path, "original bytes", 0600),
        "existing target written");
    const std::string temp = work + "/link-eexist.tmp";
    test_support::Check(test_support::WriteFile(temp, "replacement", 0600),
                        "eexist fixture written");
    ScopedPublishHooks hooks(&LinkEexist, &RenameNoReplaceReal);
    std::string error;
    test_support::Check(!PublishNoReplace(temp, final_path, &error),
                        "link EEXIST fails closed", error);
    std::string content;
    test_support::Check(test_support::ReadFile(final_path, &content) &&
                            content == "original bytes",
                        "link EEXIST leaves the target unchanged");
    test_support::Check(test_support::Exists(temp),
                        "link EEXIST leaves the temp for the caller");
  }

  // 3) link 不可用 + renameat2 成功：发布成功（这是唯一允许的备选路径）。
  {
    const std::string temp = work + "/rename-ok.tmp";
    const std::string final_path = base + "/rename-ok.bak";
    test_support::Check(test_support::WriteFile(temp, "via renameat2", 0600),
                        "rename fixture written");
    ScopedPublishHooks hooks(&LinkUnsupported, &RenameNoReplaceReal);
    std::string error;
    test_support::Check(PublishNoReplace(temp, final_path, &error),
                        "renameat2 fallback publishes", error);
    std::string content;
    test_support::Check(test_support::ReadFile(final_path, &content) &&
                            content == "via renameat2",
                        "renameat2 fallback keeps the bytes");
    test_support::Check(!test_support::Exists(temp),
                        "renameat2 fallback consumes the temp name");
  }

  // 4) 两个原子方法都不可用：fail closed，final 一定不存在。
  {
    const std::string temp = work + "/both-unsupported.tmp";
    const std::string final_path = base + "/both-unsupported.bak";
    test_support::Check(test_support::WriteFile(temp, "payload", 0600),
                        "unsupported fixture written");
    ScopedPublishHooks hooks(&LinkUnsupported, &RenameEnosys);
    std::string error;
    test_support::Check(!PublishNoReplace(temp, final_path, &error),
                        "both atomic methods unsupported -> fail", error);
    test_support::Check(!test_support::Exists(final_path),
                        "fail closed leaves no final file");
    test_support::Check(test_support::Exists(temp),
                        "fail closed leaves the temp for the caller");
    test_support::Check(error.find("(unsupported here)") != std::string::npos,
                        "the refusal names the unsupported methods", error);
  }

  // 5) link 不可用 + renameat2 返回真实错误（EIO）：必须失败，而且**不许**
  //    把真实错误说成 "unsupported"。
  {
    const std::string temp = work + "/real-error.tmp";
    const std::string final_path = base + "/real-error.bak";
    test_support::Check(test_support::WriteFile(temp, "payload", 0600),
                        "real error fixture written");
    ScopedPublishHooks hooks(&LinkUnsupported, &RenameEio);
    std::string error;
    test_support::Check(!PublishNoReplace(temp, final_path, &error),
                        "real renameat2 error -> fail", error);
    test_support::Check(!test_support::Exists(final_path),
                        "real error leaves no final file");
    test_support::Check(error.find("Input/output error") != std::string::npos,
                        "the real errno is surfaced", error);
    // 只有 link 那一半可以标 unsupported；renameat2 的 EIO 是真错误。
    std::size_t occurrences = 0;
    for (std::size_t at = error.find("(unsupported here)");
         at != std::string::npos;
         at = error.find("(unsupported here)", at + 1)) {
      ++occurrences;
    }
    test_support::Check(occurrences == 1,
                        "a real I/O error is not dressed up as unsupported",
                        error);
  }

  // 6) link 返回 EACCES（真实权限错误，不是"不支持"）：同样 fail closed，
  //    并且不标 unsupported。
  {
    const std::string temp = work + "/eacces.tmp";
    const std::string final_path = base + "/eacces.bak";
    test_support::Check(test_support::WriteFile(temp, "payload", 0600),
                        "eacces fixture written");
    ScopedPublishHooks hooks(&LinkEacces, &RenameEnosys);
    std::string error;
    test_support::Check(!PublishNoReplace(temp, final_path, &error),
                        "link EACCES -> fail", error);
    test_support::Check(!test_support::Exists(final_path),
                        "link EACCES leaves no final file");
    test_support::Check(error.find("Permission denied") != std::string::npos,
                        "the real EACCES is surfaced", error);
    test_support::Check(error.find("(unsupported here)") != std::string::npos,
                        "the truly unsupported renameat2 is still labelled",
                        error);
  }
}

}  // namespace

int main() {
  std::printf("file_io state machine and permission test\n");
  const std::string root = test_support::FreshDir("file-io");
  TestOpenThenAbandon(root);
  TestWriteThenAbandon(root);
  TestCommitThenAbandon(root);
  TestFsyncFailure(root);
  TestCloseFailure(root);
  TestRepeatedAbandon(root);
  TestOpenExisting(root);
  TestOpenTemp(root);
  TestPublishNoReplace(root);
  TestPublishAtomicFallbacks(root);
  TestTempDirectoryGuard(root);
  TestCheckFreeSpace(root);
  return test_support::Finish("file_io_test");
}
