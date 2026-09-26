// stream_rlimit_test.cpp
//
// 证明"流式压缩"不是纸面声明：在一个 RLIMIT_AS 被压到远小于语料的子进程里
// 跑完整的 v2 备份 + 恢复。
//
//   语料       256 MiB（可压缩的重复内容，避免生成太慢）
//   RLIMIT_AS  160 MiB
//
// 老的实现把整条输入读成 std::string、再把整条输出攒成另一个 std::string：
// 256 MiB 输入 + 256 MiB 输出 = 512 MiB 峰值，在这个 limit 下必然 bad_alloc。
// 流式实现全程固定缓冲，VmHWM 应该只有几 MiB。
//
// ASan 构建会把地址空间放大好几倍，不适合低 RLIMIT_AS，所以这一项只在
// 普通构建里跑（脚本里也是这么安排的）。

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "archive_pipeline.h"
#include "backup_engine.h"
#include "file_io.h"
#include "filter.h"
#include "test_support.h"

namespace {

using backupproject::BackupEngine;
using backupproject::BackupOptions;
using backupproject::CompressionMethod;
using backupproject::EncryptionMethod;
using backupproject::Filter;
using backupproject::RestoreOptions;
using backupproject::RestoreReport;

constexpr std::uint64_t kCorpusBytes = 256ull * 1024 * 1024;
constexpr std::uint64_t kAddressSpaceLimit = 160ull * 1024 * 1024;

std::uint64_t VmHwmKib() {
  std::FILE* status = std::fopen("/proc/self/status", "r");
  if (status == nullptr) {
    return 0;
  }
  char line[256];
  std::uint64_t value = 0;
  while (std::fgets(line, sizeof(line), status) != nullptr) {
    if (std::strncmp(line, "VmHWM:", 6) == 0) {
      value = std::strtoull(line + 6, nullptr, 10);
      break;
    }
  }
  std::fclose(status);
  return value;
}

// 用一个 4 符号的 4 KiB 图案铺满整个文件：可压缩、生成快、又不像全 0 那么极端。
bool WriteCorpus(const std::string& path, std::uint64_t bytes,
                 std::string* error) {
  const char pattern[4] = {'a', 'b', 'c', 'a'};
  std::vector<char> block(4096);
  for (std::size_t index = 0; index < block.size(); ++index) {
    block[index] = pattern[index % 4];
  }
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    *error = "cannot create corpus";
    return false;
  }
  std::uint64_t written = 0;
  while (written < bytes) {
    const std::size_t want = static_cast<std::size_t>(
        (bytes - written) < block.size() ? (bytes - written) : block.size());
    std::size_t done = 0;
    while (done < want) {
      const ssize_t got = ::write(fd, block.data() + done, want - done);
      if (got <= 0) {
        ::close(fd);
        *error = "short write on corpus";
        return false;
      }
      done += static_cast<std::size_t>(got);
    }
    written += want;
  }
  ::close(fd);
  return true;
}

// 逐块比较，避免为了比较把 256 MiB 读进内存。
bool FilesIdentical(const std::string& left, const std::string& right,
                    std::string* error) {
  backupproject::FileSource a;
  backupproject::FileSource b;
  if (!a.Open(left, error) || !b.Open(right, error)) {
    return false;
  }
  if (a.size() != b.size()) {
    *error = "size differs";
    return false;
  }
  std::vector<unsigned char> buffer_a(1u << 20);
  std::vector<unsigned char> buffer_b(1u << 20);
  std::uint64_t offset = 0;
  while (offset < a.size()) {
    const std::size_t want = static_cast<std::size_t>(
        std::min<std::uint64_t>(buffer_a.size(), a.size() - offset));
    if (!a.ReadAt(offset, buffer_a.data(), want, error) ||
        !b.ReadAt(offset, buffer_b.data(), want, error)) {
      return false;
    }
    if (std::memcmp(buffer_a.data(), buffer_b.data(), want) != 0) {
      *error = "content differs";
      return false;
    }
    offset += want;
  }
  return true;
}

int RunChild(const std::string& workdir) {
  struct rlimit limit;
  limit.rlim_cur = kAddressSpaceLimit;
  limit.rlim_max = kAddressSpaceLimit;
  if (::setrlimit(RLIMIT_AS, &limit) != 0) {
    std::printf("  FAIL  setrlimit(RLIMIT_AS)\n");
    return 1;
  }
  struct rlimit actual;
  ::getrlimit(RLIMIT_AS, &actual);
  std::printf("RLIMIT_AS_SOFT\t%llu\n",
              static_cast<unsigned long long>(actual.rlim_cur));

  const std::string source = workdir + "/source";
  ::mkdir(source.c_str(), 0755);
  std::string error;
  if (!WriteCorpus(source + "/big.bin", kCorpusBytes, &error)) {
    std::printf("  FAIL  write corpus -- %s\n", error.c_str());
    return 1;
  }
  std::printf("CORPUS_BYTES\t%llu\n",
              static_cast<unsigned long long>(kCorpusBytes));

  const CompressionMethod methods[2] = {CompressionMethod::kHuffman,
                                        CompressionMethod::kLzssHuffman};
  const char* labels[2] = {"huffman", "lzss-huffman"};
  int failures = 0;
  for (int index = 0; index < 2; ++index) {
    const std::string archive = workdir + "/" + labels[index] + ".bak";
    const std::string destination = workdir + "/" + labels[index] + ".out";
    BackupOptions options;
    options.pack_method = backupproject::PackMethod::kUstar;
    options.compression_method = methods[index];
    options.encryption_method = EncryptionMethod::kAes256CtrHmacSha256;
    options.password = "rlimit password";
    BackupEngine engine;
    error.clear();
    if (!engine.Backup(source, archive, Filter(), options, &error)) {
      std::printf("  FAIL  %s backup under RLIMIT_AS -- %s\n", labels[index],
                  error.c_str());
      ++failures;
      continue;
    }
    RestoreOptions restore_options;
    restore_options.password = options.password;
    RestoreReport report;
    error.clear();
    if (!engine.Restore(archive, destination, restore_options, &report,
                        &error)) {
      std::printf("  FAIL  %s restore under RLIMIT_AS -- %s\n", labels[index],
                  error.c_str());
      ++failures;
      continue;
    }
    if (!FilesIdentical(source + "/big.bin", destination + "/big.bin",
                        &error)) {
      std::printf("  FAIL  %s round trip content -- %s\n", labels[index],
                  error.c_str());
      ++failures;
      continue;
    }
    std::printf("  PASS  %s backup+restore+compare under RLIMIT_AS\n",
                labels[index]);
  }
  // 峰值 RSS 写在文件里：子进程是用 _exit 结束的，stdio 缓冲不会自动冲刷，
  // 直接打印会丢。父进程读这个文件并断言它远小于地址空间上限。
  const std::uint64_t peak = VmHwmKib();
  std::printf("VmHWM_KIB\t%llu\n", static_cast<unsigned long long>(peak));
  std::fflush(stdout);
  std::FILE* record = std::fopen((workdir + "/vmhwm.txt").c_str(), "w");
  if (record != nullptr) {
    std::fprintf(record, "%llu\n", static_cast<unsigned long long>(peak));
    std::fclose(record);
  }
  return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  std::printf("streaming resource test (RLIMIT_AS)\n");
  if (argc > 1 && std::strcmp(argv[1], "--child") == 0) {
    const std::string workdir =
        argc > 2 ? std::string(argv[2]) : test_support::FreshDir("rlimit");
    return RunChild(workdir);
  }
  const std::string workdir = test_support::FreshDir("rlimit");

  // 子进程里 setrlimit：父进程自己不受影响，而且能看清楚子进程的退出原因。
  const pid_t child = ::fork();
  if (child < 0) {
    std::printf("  FAIL  fork\n");
    return 1;
  }
  if (child == 0) {
    // 先把缓冲区冲掉：_exit 不做 stdio flush，直接退会丢掉子进程的全部输出。
    const int child_code = RunChild(workdir);
    std::fflush(stdout);
    std::fflush(stderr);
    ::_exit(child_code);
  }
  int status = 0;
  if (::waitpid(child, &status, 0) < 0) {
    std::printf("  FAIL  waitpid\n");
    return 1;
  }
  const bool exited = WIFEXITED(status);
  const int code = exited ? WEXITSTATUS(status) : -1;
  test_support::Check(exited && code == 0, "child completed under RLIMIT_AS",
                      exited ? ("exit=" + std::to_string(code))
                             : "killed by signal (很可能是 OOM / RLIMIT_AS)");
  std::printf("RLIMIT child exit=%d\n", code);

  // 峰值 RSS 必须远小于地址空间上限，否则"流式"只是说法。
  std::uint64_t peak_kib = 0;
  std::string recorded;
  if (test_support::ReadFile(workdir + "/vmhwm.txt", &recorded)) {
    peak_kib = std::strtoull(recorded.c_str(), nullptr, 10);
  }
  std::printf("RLIMIT_AS_LIMIT_KIB=%llu\n",
              static_cast<unsigned long long>(kAddressSpaceLimit / 1024));
  std::printf("PEAK_RSS_KIB=%llu\n", static_cast<unsigned long long>(peak_kib));
  std::printf("CORPUS_KIB=%llu\n",
              static_cast<unsigned long long>(kCorpusBytes / 1024));
  test_support::Check(peak_kib > 0, "child recorded its peak RSS");
  test_support::Check(peak_kib * 1024 < kAddressSpaceLimit,
                      "peak RSS stays under the address space limit",
                      std::to_string(peak_kib) + " KiB");
  // 语料本身是 256 MiB，峰值 RSS 必须远小于它——这正是"不再整条读进内存"的证据。
  test_support::Check(peak_kib * 1024 < kCorpusBytes / 8,
                      "peak RSS is far below the corpus size",
                      std::to_string(peak_kib) + " KiB");
  test_support::RemoveTree(workdir);
  return test_support::Finish("stream-rlimit");
}
