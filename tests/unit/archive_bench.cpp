// archive_bench.cpp
//
// baseline USTAR 与 Fast USTAR 的打包性能对比。
//
//   corpus 1：1 个 128 MiB 普通文件
//   corpus 2：10000 个 1~8 KiB 小文件
//   corpus 3：混合目录（若干大文件 + 若干子目录小文件），约 100 MiB
//
// 两个写入器的 wire format 都是标准 USTAR，差别只在 I/O 策略：
// baseline 64 KiB 缓冲、header/padding 各自 write；Fast 1 MiB 统一缓冲、
// 把 header 与对齐填充聚合进同一个缓冲、对输入 fd 调 POSIX_FADV_SEQUENTIAL。
//
// 参数：archive_bench [大文件 MiB] [小文件个数] [混合目录 MiB]
// 默认 128 / 10000 / 100。

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "archive_entry.h"
#include "test_support.h"
#include "tree_scanner.h"
#include "ustar.h"

namespace {

using backupproject::ArchiveEntry;
using backupproject::Filter;

struct Result {
  double baseline_ms = 0.0;
  double fast_ms = 0.0;
  std::uint64_t baseline_bytes = 0;
  std::uint64_t fast_bytes = 0;
  std::size_t entries = 0;
  bool identical = false;
  bool baseline_ok = false;
  bool fast_ok = false;
  std::string error;
};

double NowMs() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration<double, std::milli>(
             clock::now().time_since_epoch())
      .count();
}

std::uint64_t FileSize(const std::string& path) {
  struct stat info;
  if (::stat(path.c_str(), &info) != 0) {
    return 0;
  }
  return static_cast<std::uint64_t>(info.st_size);
}

// 写一个确定内容的普通文件：用固定 seed 的 PRNG，保证两个写入器读到同一份数据。
void WritePseudoRandom(const std::string& path, std::uint64_t size,
                       std::uint64_t seed) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    return;
  }
  std::vector<unsigned char> buffer(256 * 1024);
  std::uint64_t state = seed | 1;
  std::uint64_t written = 0;
  while (written < size) {
    const std::size_t want = static_cast<std::size_t>(
        (size - written) < buffer.size() ? (size - written) : buffer.size());
    for (std::size_t index = 0; index < want; ++index) {
      state = state * 6364136223846793005ull + 1442695040888963407ull;
      buffer[index] = static_cast<unsigned char>(state >> 33);
    }
    std::size_t done = 0;
    while (done < want) {
      const ssize_t got = ::write(fd, buffer.data() + done, want - done);
      if (got <= 0) {
        ::close(fd);
        return;
      }
      done += static_cast<std::size_t>(got);
    }
    written += want;
  }
  ::close(fd);
}

// 两个 archive 是不是逐字节相同。FastUSTAR 的目标是"同样的 wire format，
// 更少的 syscall"，所以逐字节相同是最强的等价证据。
bool FilesIdentical(const std::string& left, const std::string& right) {
  std::string a;
  std::string b;
  if (!test_support::ReadFile(left, &a) || !test_support::ReadFile(right, &b)) {
    return false;
  }
  return a == b;
}

Result RunCorpus(const std::string& name, const std::string& source,
                 const std::string& workdir) {
  Result result;
  const std::string baseline = workdir + "/" + name + "-baseline.tar";
  const std::string fast = workdir + "/" + name + "-fast.tar";
  test_support::RemoveTree(baseline);
  test_support::RemoveTree(fast);

  std::string error;
  std::vector<ArchiveEntry> entries;
  if (!backupproject::ScanSourceTree(source, nullptr, &entries, &error)) {
    result.error = "scan failed: " + error;
    return result;
  }
  result.entries = entries.size();

  const double start_baseline = NowMs();
  result.baseline_ok =
      backupproject::ustar::WriteBaseline(entries, baseline, &error);
  const double end_baseline = NowMs();
  result.baseline_ms = end_baseline - start_baseline;
  if (!result.baseline_ok) {
    result.error = "baseline failed: " + error;
    return result;
  }

  const double start_fast = NowMs();
  result.fast_ok = backupproject::ustar::WriteFast(entries, fast, &error);
  const double end_fast = NowMs();
  result.fast_ms = end_fast - start_fast;
  if (!result.fast_ok) {
    result.error = "fast failed: " + error;
    return result;
  }

  result.baseline_bytes = FileSize(baseline);
  result.fast_bytes = FileSize(fast);
  result.identical = FilesIdentical(baseline, fast);

  // 两个 archive 都必须被我们自己的 reader 认出来，而且条目数一致。
  std::vector<backupproject::ustar::Member> members;
  if (!backupproject::ustar::Scan(baseline, &members, &error) ||
      members.size() != entries.size()) {
    result.error = "baseline archive does not scan back: " + error;
    result.baseline_ok = false;
  }
  members.clear();
  if (!backupproject::ustar::Scan(fast, &members, &error) ||
      members.size() != entries.size()) {
    result.error = "fast archive does not scan back: " + error;
    result.fast_ok = false;
  }
  return result;
}

void BuildBigFileCorpus(const std::string& dir, std::uint64_t mebibytes) {
  test_support::Mkdir(dir, 0755);
  WritePseudoRandom(dir + "/big.bin", mebibytes * 1024ull * 1024ull, 12345);
}

void BuildSmallFileCorpus(const std::string& dir, std::size_t count) {
  test_support::Mkdir(dir, 0755);
  std::uint64_t state = 987654321ull;
  for (std::size_t index = 0; index < count; ++index) {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    const std::size_t size = 1024 + static_cast<std::size_t>((state >> 40) % 7168);
    char name[64];
    std::snprintf(name, sizeof(name), "/f%05zu.bin", index);
    std::string content(size, '\0');
    for (std::size_t offset = 0; offset < size; ++offset) {
      content[offset] = static_cast<char>((offset * 31 + index) & 0xFF);
    }
    test_support::WriteFile(dir + name, content, 0644);
    if ((index + 1) % 2000 == 0) {
      std::printf("  ... created %zu/%zu small files\n", index + 1, count);
      std::fflush(stdout);
    }
  }
}

void BuildMixedCorpus(const std::string& dir, std::uint64_t mebibytes) {
  test_support::Mkdir(dir, 0755);
  const std::uint64_t chunk = mebibytes / 4;
  for (int index = 0; index < 3; ++index) {
    char name[64];
    std::snprintf(name, sizeof(name), "/large%d.bin", index);
    WritePseudoRandom(dir + name, chunk * 1024ull * 1024ull,
                      1000 + static_cast<std::uint64_t>(index));
  }
  // 剩下的体积用 8 层目录 × 每个 8 个 64 KiB 文件凑出来。
  for (int outer = 0; outer < 8; ++outer) {
    char outer_name[64];
    std::snprintf(outer_name, sizeof(outer_name), "/d%d", outer);
    test_support::Mkdir(dir + outer_name, 0755);
    for (int inner = 0; inner < 8; ++inner) {
      char inner_name[64];
      std::snprintf(inner_name, sizeof(inner_name), "%s/s%d.bin", outer_name,
                    inner);
      WritePseudoRandom(dir + inner_name, 64 * 1024,
                        7000 + static_cast<std::uint64_t>(outer * 8 + inner));
    }
  }
}

void PrintRow(const std::string& corpus, const Result& result,
              std::uint64_t bytes) {
  const double mib = static_cast<double>(bytes) / (1024.0 * 1024.0);
  const double baseline_ms = result.baseline_ms > 0 ? result.baseline_ms : 1e-9;
  const double fast_ms = result.fast_ms > 0 ? result.fast_ms : 1e-9;
  const double speedup = baseline_ms / fast_ms;
  std::printf(
      "TARBENCH\t%s\t%.1f\t%.1f\t%.2fx\t%.1f\t%.1f\t%llu\t%llu\t%s\t%s\n",
      corpus.c_str(), result.baseline_ms, result.fast_ms, speedup,
      mib / (result.baseline_ms / 1000.0), mib / (result.fast_ms / 1000.0),
      static_cast<unsigned long long>(result.baseline_bytes),
      static_cast<unsigned long long>(result.fast_bytes),
      result.identical ? "identical" : "differ",
      (result.baseline_ok && result.fast_ok && result.error.empty()) ? "PASS"
                                                                   : "FAIL");
  if (!result.error.empty()) {
    std::printf("  error: %s\n", result.error.c_str());
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::uint64_t big_mib = 128;
  std::size_t small_count = 10000;
  std::uint64_t mixed_mib = 100;
  if (argc > 1) {
    big_mib = static_cast<std::uint64_t>(std::strtoull(argv[1], nullptr, 10));
  }
  if (argc > 2) {
    small_count = static_cast<std::size_t>(std::strtoul(argv[2], nullptr, 10));
  }
  if (argc > 3) {
    mixed_mib = static_cast<std::uint64_t>(std::strtoull(argv[3], nullptr, 10));
  }

  const std::string workdir = test_support::FreshDir("bench");
  std::printf("archive pack benchmark (baseline USTAR vs Fast USTAR)\n");
  std::printf("corpus sizes: big=%llu MiB small=%zu files mixed=%llu MiB\n",
              static_cast<unsigned long long>(big_mib), small_count,
              static_cast<unsigned long long>(mixed_mib));
  std::printf(
      "TARBENCH\tcorpus\tbaseline_ms\tfast_ms\tspeedup\tbaseline_MiBps\t"
      "fast_MiBps\tbaseline_bytes\tfast_bytes\tsize\tresult\n");

  // corpus 1：§76 要求 1 MiB / 10 MiB / 128 MiB 三档都测一遍
  {
    const std::uint64_t sizes[] = {1, 10, big_mib};
    for (const std::uint64_t mebibytes : sizes) {
      const std::string tag = std::to_string(mebibytes);
      const std::string source = workdir + "/corpus1-" + tag;
      std::printf("building single-file corpus (%llu MiB)...\n",
                  static_cast<unsigned long long>(mebibytes));
      std::fflush(stdout);
      BuildBigFileCorpus(source, mebibytes);
      PrintRow("single-" + tag + "MiB-file",
               RunCorpus("corpus1-" + tag, source, workdir),
               FileSize(source + "/big.bin"));
      test_support::RemoveTree(source);
    }
  }
  // corpus 2
  {
    const std::string source = workdir + "/corpus2";
    std::printf("building corpus 2 (%zu small files)...\n", small_count);
    std::fflush(stdout);
    BuildSmallFileCorpus(source, small_count);
    std::uint64_t total = 0;
    for (const std::string& name : test_support::DirEntries(source)) {
      total += FileSize(source + "/" + name);
    }
    PrintRow("10000-small-files", RunCorpus("corpus2", source, workdir), total);
    test_support::RemoveTree(source);
  }
  // corpus 3
  {
    const std::string source = workdir + "/corpus3";
    std::printf("building corpus 3 (mixed %llu MiB)...\n",
                static_cast<unsigned long long>(mixed_mib));
    std::fflush(stdout);
    BuildMixedCorpus(source, mixed_mib);
    std::uint64_t total = 0;
    std::vector<std::string> pending{source};
    while (!pending.empty()) {
      const std::string current = pending.back();
      pending.pop_back();
      struct stat info;
      if (::lstat(current.c_str(), &info) != 0) {
        continue;
      }
      if (S_ISDIR(info.st_mode)) {
        for (const std::string& name : test_support::DirEntries(current)) {
          pending.push_back(current + "/" + name);
        }
      } else {
        total += static_cast<std::uint64_t>(info.st_size);
      }
    }
    PrintRow("mixed-tree", RunCorpus("corpus3", source, workdir), total);
    test_support::RemoveTree(source);
  }

  test_support::RemoveTree(workdir);
  std::printf("archive bench done\n");
  return 0;
}
