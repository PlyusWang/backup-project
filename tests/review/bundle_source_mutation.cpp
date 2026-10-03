// tests/review/bundle_source_mutation.cpp
//
// PR #21 最后一个 merge blocker 的判别性回归。
//
// 缺陷（HEAD 2889116 及以前）：BuildSnapshotBundle() 分两遍走——
//   第一遍 HashFile() 算出每个成员的 size 与 SHA-256，写进包头；
//   第二遍重新打开源文件、比对长度、把字节复制进 bundle。
// 两遍之间（以及第二遍复制期间）一次**同长度**的改写不会触发任何检查：
// 包头声明的摘要来自第一遍，实际写进去的字节来自第二遍。于是
// BuildSnapshotBundle() 返回 true，却发布了一个**自相矛盾**的 BPSNAP1：
// 服务端只校验整个 blob 的 SHA-256（它不解析成员），所以这个坏材料会被
// 正常收下、写进远端元数据，直到将来 restore 才失败。
//
// 正确的不变式只有一个：
//   SHA-256(实际复制进 bundle 的字节) == 包头里声明的 SHA-256
// mtime / inode / size 比较只能提高发现概率，不能保证任何东西。
//
// 本文件是 OLD/NEW 判别用的**同一个**复现程序，两种期望值：
//   --expect old   跑在 git archive 2889116 出来的独立旧树上：缺陷必须存在
//                  （Build 成功、Extract 失败；增长被静默丢弃）。
//   --expect new   跑在修复后的树上：必须当场拒绝，且不留任何半成品。
// 断言是自证的：期望结果只有在"改写真的落进了第二遍"时才可能出现；触发
// 条件没命中时两种模式都会以可读的失败结束（见每个用例的守卫结论）。
//
// 触发不使用 sleep 撞运气，而是用**显式可观察事件 + 可证明的前置条件**：
//   * .part 文件出现 == 第一遍已经结束（三个成员的摘要都已定稿）；
//   * .part 的字节数 == 第二遍已经复制到哪儿（读一块、写一块、同一循环）；
//   * 于是"改写点在复制游标之后"、"目标成员还没被第二遍打开"都是可证的。
// 每个用例都会把守卫结论打出来。
//
// 退出码：0 = 全部通过。

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "backup_catalog.h"
#include "incremental_backup.h"
#include "snapshot_bundle.h"

namespace {

int g_checks = 0;
int g_failures = 0;
bool g_keep = false;

void Check(bool ok, const std::string& name, const std::string& detail = "") {
  g_checks += 1;
  if (!ok) {
    g_failures += 1;
    std::printf("  FAIL %s%s\n", name.c_str(),
                detail.empty() ? "" : (" -- " + detail).c_str());
  }
}

void Note(const std::string& text) {
  std::printf("[bundle-mutation] %s\n", text.c_str());
}

// 大 .bak 的内容是按偏移确定的伪随机模式：既避免把 32 MiB 放进内存，
// 又让"某个偏移上的字节应该是什么"在测试里可复算。
char PatternByte(std::uint64_t offset) {
  std::uint64_t value = offset * 0x9E3779B97F4A7C15ull + 0x1234567ull;
  value ^= value >> 29;
  return static_cast<char>((value >> 13) & 0xFFu);
}

std::string WorkRoot() {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "/tmp/bundle-source-mutation-%ld",
                static_cast<long>(::getpid()));
  return std::string(buffer);
}

bool MakeDirectoryTree(const std::string& path) {
  if (path.empty()) {
    return false;
  }
  std::string current;
  for (std::size_t index = 0; index <= path.size(); ++index) {
    if (index == path.size() || path[index] == '/') {
      if (!current.empty() && current != "/") {
        if (::mkdir(current.c_str(), 0700) != 0 && errno != EEXIST) {
          return false;
        }
      }
    }
    if (index < path.size()) {
      current.push_back(path[index]);
    }
  }
  return true;
}

bool WriteAllFd(int fd, const char* data, std::size_t size) {
  std::size_t done = 0;
  while (done < size) {
    const ssize_t written = ::write(fd, data + done, size - done);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    done += static_cast<std::size_t>(written);
  }
  return true;
}

bool WriteFile(const std::string& path, const std::string& content) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    return false;
  }
  const bool ok = WriteAllFd(fd, content.data(), content.size());
  return ::close(fd) == 0 && ok;
}

bool WritePatternFile(const std::string& path, std::uint64_t size) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    return false;
  }
  std::vector<char> buffer(1u << 20);
  std::uint64_t offset = 0;
  while (offset < size) {
    std::size_t want = buffer.size();
    if (size - offset < want) {
      want = static_cast<std::size_t>(size - offset);
    }
    for (std::size_t index = 0; index < want; ++index) {
      buffer[index] = PatternByte(offset + index);
    }
    if (!WriteAllFd(fd, buffer.data(), want)) {
      ::close(fd);
      return false;
    }
    offset += want;
  }
  return ::close(fd) == 0;
}

bool ReadFile(const std::string& path, std::string* out) {
  out->clear();
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return false;
  }
  char buffer[64 * 1024];
  for (;;) {
    const ssize_t got = ::read(fd, buffer, sizeof(buffer));
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      ::close(fd);
      return false;
    }
    if (got == 0) {
      break;
    }
    out->append(buffer, static_cast<std::size_t>(got));
  }
  return ::close(fd) == 0;
}

bool ReadByteAt(const std::string& path, std::uint64_t offset, char* out) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return false;
  }
  const ssize_t got = ::pread(fd, out, 1, static_cast<off_t>(offset));
  ::close(fd);
  return got == 1;
}

bool FileExists(const std::string& path) {
  struct stat info;
  return ::stat(path.c_str(), &info) == 0;
}

std::uint64_t FileSize(const std::string& path) {
  struct stat info;
  if (::stat(path.c_str(), &info) != 0) {
    return 0;
  }
  return static_cast<std::uint64_t>(info.st_size);
}

std::vector<std::string> DirectoryEntries(const std::string& path) {
  std::vector<std::string> entries;
  DIR* directory = ::opendir(path.c_str());
  if (directory == nullptr) {
    return entries;
  }
  while (struct dirent* item = ::readdir(directory)) {
    const std::string name = item->d_name;
    if (name == "." || name == "..") {
      continue;
    }
    entries.push_back(name);
  }
  ::closedir(directory);
  return entries;
}

// 第二遍中途留下的临时文件：<bundle 文件名>.part-*（名字里带 pid）。
std::vector<std::string> PartFiles(const std::string& directory,
                                   const std::string& bundle_path) {
  const std::string base = bundle_path.substr(bundle_path.rfind('/') + 1);
  std::vector<std::string> found;
  for (const std::string& name : DirectoryEntries(directory)) {
    if (name.size() > base.size() + 6 &&
        name.compare(0, base.size(), base) == 0 &&
        name.compare(base.size(), 6, ".part-") == 0) {
      found.push_back(directory + "/" + name);
    }
  }
  return found;
}

std::uint64_t PartBytes(const std::string& directory,
                        const std::string& bundle_path) {
  std::uint64_t total = 0;
  for (const std::string& path : PartFiles(directory, bundle_path)) {
    total += FileSize(path);
  }
  return total;
}

bool FilesIdentical(const std::string& left, const std::string& right) {
  if (FileSize(left) != FileSize(right)) {
    return false;
  }
  const int a = ::open(left.c_str(), O_RDONLY);
  const int b = ::open(right.c_str(), O_RDONLY);
  if (a < 0 || b < 0) {
    if (a >= 0) {
      ::close(a);
    }
    if (b >= 0) {
      ::close(b);
    }
    return false;
  }
  char buffer_a[64 * 1024];
  char buffer_b[64 * 1024];
  bool same = true;
  for (;;) {
    const ssize_t got_a = ::read(a, buffer_a, sizeof(buffer_a));
    const ssize_t got_b = ::read(b, buffer_b, sizeof(buffer_b));
    if (got_a <= 0 || got_a != got_b) {
      same = (got_a == 0) && (got_b == 0);
      break;
    }
    if (std::memcmp(buffer_a, buffer_b, static_cast<std::size_t>(got_a)) != 0) {
      same = false;
      break;
    }
  }
  ::close(a);
  ::close(b);
  return same;
}

bool Contains(const std::string& text, const char* needle) {
  return text.find(needle) != std::string::npos;
}

// ---- 复现程序 ----

enum class Action {
  kInPlaceAhead,   // 改写 .bak 里**复制游标之后**的一个字节（同 inode、同长度）
  kAtomicReplace,  // 原子 rename 覆盖 .manifest / .identity（同长度、不同内容）
  kAppendTail,     // 在 .bak 复制期间往文件尾部追加字节（文件变长）
};

enum class Expect { kOld, kNew };

struct CaseResult {
  std::string directory;
  std::string bundle_path;
  std::string archive;
  std::uint64_t source_bak_size = 0;
  bool acted = false;
  bool guard_ok = false;
  std::string guard_note;
  bool build_ok = false;
  std::string build_error;
  bool final_exists = false;
  std::size_t part_files = 0;
  std::uint64_t trigger_part_bytes = 0;
};

const std::uint64_t kBakBytes = 32ull * 1024ull * 1024ull;
const char kArchiveName[] = "remote-cccccccccccc-300-g0.bak";

// 观察线程：等 .part 出现（= 第一遍结束），再按可证前置条件动手。
void WatchAndMutate(const std::string& directory, const std::string& bundle_path,
                    const std::string& target_path, Action action,
                    std::uint64_t bak_size, std::uint64_t trigger_bytes,
                    std::uint64_t replace_offset, CaseResult* result) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(180);
  // 1) 等 .part 出现。它由第二遍创建，所以"出现"== 三个成员的摘要都已定稿。
  while (PartFiles(directory, bundle_path).empty()) {
    if (std::chrono::steady_clock::now() > deadline) {
      result->guard_note = "等 .part 超时（构建可能没跑到第二遍）";
      return;
    }
    std::this_thread::yield();
  }
  // 2) 等复制进度越过触发线（.part 的字节数就是已经复制出去的字节数）。
  for (;;) {
    const std::uint64_t part_bytes = PartBytes(directory, bundle_path);
    if (part_bytes > trigger_bytes) {
      result->trigger_part_bytes = part_bytes;
      break;
    }
    if (PartFiles(directory, bundle_path).empty() ||
        std::chrono::steady_clock::now() > deadline) {
      result->guard_note = "还没开始复制 .part 就消失了（或等待超时）";
      return;
    }
    std::this_thread::yield();
  }

  const std::uint64_t part_bytes = result->trigger_part_bytes;
  if (action == Action::kInPlaceAhead) {
    // 复制游标落后 .part 的字节数最多一个块（256 KiB）。改写点必须在
    // 游标 + 1 MiB 之后，才谈得上"第二遍还没读到它"。
    if (!(part_bytes < bak_size) ||
        !(replace_offset > part_bytes + (1u << 20))) {
      result->guard_note =
          "守卫不成立：part_bytes=" + std::to_string(part_bytes) +
          " replace_offset=" + std::to_string(replace_offset);
      return;
    }
    const int fd = ::open(target_path.c_str(), O_WRONLY);
    if (fd < 0) {
      result->guard_note = "打开 .bak 失败";
      return;
    }
    const char replacement =
        static_cast<char>(PatternByte(replace_offset) ^ 0xFF);
    const ssize_t written =
        ::pwrite(fd, &replacement, 1, static_cast<off_t>(replace_offset));
    const bool closed = ::close(fd) == 0;
    result->acted = (written == 1) && closed;
    result->guard_ok = result->acted;
    result->guard_note =
        "part_bytes=" + std::to_string(part_bytes) + " 时改写偏移 " +
        std::to_string(replace_offset) + "（在游标之后 " +
        std::to_string(replace_offset - part_bytes) + " 字节，长度不变）";
    return;
  }

  if (action == Action::kAtomicReplace) {
    // 目标成员在 .bak 之后才被第二遍打开。只要 .bak 的复制**还没结束**，
    // 目标成员的 open() 就一定还没发生：part_bytes < bak_size 可证这一点。
    if (!(part_bytes < bak_size)) {
      result->guard_note = "守卫不成立：.bak 已经复制完（part_bytes=" +
                           std::to_string(part_bytes) + "）";
      return;
    }
    const std::string replacement_path = target_path + ".replacement";
    if (!FileExists(replacement_path)) {
      result->guard_note = "找不到替换文件";
      return;
    }
    result->acted = ::rename(replacement_path.c_str(), target_path.c_str()) == 0;
    result->guard_ok = result->acted;
    result->guard_note =
        "part_bytes=" + std::to_string(part_bytes) + "（< .bak 的 " +
        std::to_string(bak_size) + "）时原子替换 " +
        target_path.substr(target_path.rfind('/') + 1);
    return;
  }

  // kAppendTail：fstat 一定发生在第一次 read 之前，而 part_bytes > 0 说明
  // 已经在读；此刻追加的尾巴既不会被复制，也没进第一遍的摘要。
  if (!(part_bytes > 0) || !(part_bytes < bak_size)) {
    result->guard_note = "守卫不成立：part_bytes=" + std::to_string(part_bytes);
    return;
  }
  const int fd = ::open(target_path.c_str(), O_WRONLY | O_APPEND);
  if (fd < 0) {
    result->guard_note = "打开 .bak 失败";
    return;
  }
  std::vector<char> tail(1u << 20, 'Z');
  const bool wrote = WriteAllFd(fd, tail.data(), tail.size());
  const bool closed = ::close(fd) == 0;
  result->acted = wrote && closed;
  result->guard_ok = result->acted;
  result->guard_note = "part_bytes=" + std::to_string(part_bytes) +
                       " 时追加 1 MiB（fstat 已经用旧长度做完）";
}

CaseResult RunCase(const std::string& root, const std::string& case_name,
                   Action action, std::size_t target_index) {
  CaseResult result;
  result.directory = root + "/" + case_name;
  result.archive = kArchiveName;
  const std::string directory = result.directory;
  if (!MakeDirectoryTree(directory)) {
    result.guard_note = "建目录失败";
    return result;
  }

  const std::string manifest =
      backupproject::SnapshotManifestFileName(result.archive);
  const std::string identity =
      backupproject::SnapshotIdentityFileName(result.archive);
  const std::string bak_path = directory + "/" + result.archive;
  const std::string manifest_path = directory + "/" + manifest;
  const std::string identity_path = directory + "/" + identity;
  result.bundle_path = directory + "/snapshot.bundle";

  // 大 .bak：让第二遍的复制有足够长的窗口，观察线程来得及在"可证"的位置动手。
  result.source_bak_size = kBakBytes;
  if (!WritePatternFile(bak_path, kBakBytes)) {
    result.guard_note = "写 .bak 失败";
    return result;
  }
  const std::string manifest_body = "BPMANIFEST3 1\nentry-count=3\npayload=" +
                                    std::string(64, 'm') + "\n";
  const std::string identity_body =
      "BPIDENT2\nsnapshot_id=cccccccccccccccccccccccccccccccc\n";
  if (!WriteFile(manifest_path, manifest_body) ||
      !WriteFile(identity_path, identity_body)) {
    result.guard_note = "写副文件失败";
    return result;
  }

  std::string target_path = bak_path;
  if (target_index == 1) {
    target_path = manifest_path;
  } else if (target_index == 2) {
    target_path = identity_path;
  }

  // 触发线：一小段，保证"观察 -> 动作"发生在复制的极早期。
  const std::uint64_t trigger_bytes = 4ull * 1024ull * 1024ull;
  const std::uint64_t replace_offset = kBakBytes - 4ull * 1024ull * 1024ull;

  if (action == Action::kAtomicReplace) {
    std::string original;
    if (!ReadFile(target_path, &original) || original.empty()) {
      result.guard_note = "读目标成员失败";
      return result;
    }
    std::string replacement = original;
    replacement[replacement.size() - 1] =
        static_cast<char>(replacement[replacement.size() - 1] ^ 0x5A);
    if (replacement == original ||
        !WriteFile(target_path + ".replacement", replacement)) {
      result.guard_note = "写替换文件失败";
      return result;
    }
  }

  std::thread watcher(WatchAndMutate, directory, result.bundle_path, target_path,
                      action, kBakBytes, trigger_bytes, replace_offset, &result);

  backupproject::net::SnapshotBundleInfo info;
  std::string error;
  result.build_ok = backupproject::net::BuildSnapshotBundle(
      directory, result.archive, result.bundle_path, &info, &error);
  result.build_error = error;
  watcher.join();
  result.final_exists = FileExists(result.bundle_path);
  result.part_files = PartFiles(directory, result.bundle_path).size();
  return result;
}

void ReportTrigger(const std::string& case_name, const CaseResult& result) {
  Note(case_name + " 触发：" +
       (result.acted ? result.guard_note
                     : ("没有动手 -- " + result.guard_note)));
}

// NEW 期望：当场拒绝，不留半成品。
void ExpectRejected(const std::string& case_name, const CaseResult& result,
                    const char* reason) {
  ReportTrigger(case_name, result);
  Check(result.acted && result.guard_ok, case_name + " 触发条件命中（自证）",
        result.guard_note);
  Check(!result.build_ok, case_name + " NEW 打包必须失败",
        "BuildSnapshotBundle 返回 true");
  Check(Contains(result.build_error, reason),
        case_name + std::string(" NEW 失败原因是「") + reason + "」",
        result.build_error);
  Check(!result.final_exists, case_name + " NEW 没有发布最终材料包");
  Check(result.part_files == 0, case_name + " NEW 没有留下 .part 半成品",
        std::to_string(result.part_files) + " 个");
}

// OLD 期望（同长度改写）：打包照旧成功，但这个包自己解不开。
void ExpectOldUnreadable(const std::string& case_name,
                         const CaseResult& result) {
  ReportTrigger(case_name, result);
  Check(result.acted && result.guard_ok, case_name + " 触发条件命中（自证）",
        result.guard_note);
  Check(result.build_ok, case_name + " OLD 会接受这个改写（缺陷存在）",
        result.build_error);
  if (!result.build_ok) {
    return;
  }
  Check(result.final_exists, case_name + " OLD 发布了材料包");
  Check(result.part_files == 0, case_name + " OLD 发布后没有 .part 残留");
  backupproject::net::SnapshotBundleInfo inspected;
  std::string error;
  Check(backupproject::net::InspectSnapshotBundle(result.bundle_path, &inspected,
                                                  &error),
        case_name + " OLD 的材料包结构上仍然合法（Inspect 通过）", error);
  const std::string out = result.directory + "/out";
  Check(MakeDirectoryTree(out), case_name + " 建解包目录");
  backupproject::net::SnapshotBundleInfo extracted;
  const bool extract_ok = backupproject::net::ExtractSnapshotBundle(
      result.bundle_path, out, &extracted, &error);
  Check(!extract_ok, case_name + " OLD 自己解不开这个材料包（Extract 失败）",
        "竟然解开了");
  Check(Contains(error, "SHA-256") || Contains(error, "校验失败"),
        case_name + " OLD 的失败原因是成员摘要不符", error);
  Check(DirectoryEntries(out).empty(),
        case_name + " OLD 解包失败后目标目录为空");
  if (!extract_ok) {
    Note(case_name +
         " 观测：OLD 的 BuildSnapshotBundle 返回 true 并发布了材料包，但同一个 "
         "OLD 的 ExtractSnapshotBundle 拒绝它（" +
         error + "）。");
  }
}

// OLD 期望（复制期间变长）：打包照旧成功，多出来的尾巴被静默丢弃。
void ExpectOldGrowthDropped(const std::string& case_name,
                            const CaseResult& result) {
  ReportTrigger(case_name, result);
  Check(result.acted && result.guard_ok, case_name + " 触发条件命中（自证）",
        result.guard_note);
  Check(result.build_ok, case_name + " OLD 会接受复制期间的变长（缺陷存在）",
        result.build_error);
  if (!result.build_ok) {
    return;
  }
  const std::string out = result.directory + "/out";
  Check(MakeDirectoryTree(out), case_name + " 建解包目录");
  backupproject::net::SnapshotBundleInfo extracted;
  std::string error;
  Check(backupproject::net::ExtractSnapshotBundle(result.bundle_path, out,
                                                  &extracted, &error),
        case_name + " OLD 解包成功", error);
  const std::string extracted_bak = out + "/" + result.archive;
  Check(FileSize(extracted_bak) == result.source_bak_size,
        case_name + " OLD 只复制了声明长度（尾巴被丢掉）",
        std::to_string(FileSize(extracted_bak)) + " vs " +
            std::to_string(result.source_bak_size));
  char last = 0;
  Check(ReadByteAt(extracted_bak, result.source_bak_size - 1, &last) &&
            last == PatternByte(result.source_bak_size - 1),
        case_name + " OLD 复制出来的前缀与原文件一致");
  Note(case_name + " 观测：源文件在复制期间长了 1 MiB，OLD 仍然返回 true，解出来的 "
                   ".bak 只有声明的 " +
       std::to_string(result.source_bak_size) + " 字节（新尾巴被静默丢弃）。");
}

void TestNormal() {
  Note("正常路径回归（无并发改写）");
  const std::string root = WorkRoot();
  const std::string directory = root + "/normal";
  Check(MakeDirectoryTree(directory), "正常路径：建目录");
  const std::string archive = "remote-dddddddddddd-301-g0.bak";
  const std::string manifest = backupproject::SnapshotManifestFileName(archive);
  const std::string identity = backupproject::SnapshotIdentityFileName(archive);
  Check(WritePatternFile(directory + "/" + archive, 8ull * 1024ull * 1024ull),
        "正常路径：写 8 MiB .bak");
  Check(WriteFile(directory + "/" + manifest, "BPMANIFEST3 1\nentry-count=2\n"),
        "正常路径：写 .manifest");
  Check(WriteFile(directory + "/" + identity, "BPIDENT2\nsnapshot_id=dddd\n"),
        "正常路径：写 .identity");

  const std::string bundle = directory + "/snapshot.bundle";
  backupproject::net::SnapshotBundleInfo info;
  std::string error;
  Check(backupproject::net::BuildSnapshotBundle(directory, archive, bundle, &info,
                                                &error),
        "正常路径：打包成功", error);
  Check(info.members.size() == 3, "正常路径：包头给出 3 个成员");
  Check(PartFiles(directory, bundle).empty(), "正常路径：发布后没有 .part 残留");

  backupproject::net::SnapshotBundleInfo inspected;
  Check(backupproject::net::InspectSnapshotBundle(bundle, &inspected, &error),
        "正常路径：Inspect 通过", error);

  const std::string out = directory + "/out";
  Check(MakeDirectoryTree(out), "正常路径：建解包目录");
  backupproject::net::SnapshotBundleInfo extracted;
  Check(backupproject::net::ExtractSnapshotBundle(bundle, out, &extracted, &error),
        "正常路径：Extract 通过", error);
  Check(FilesIdentical(directory + "/" + archive, out + "/" + archive),
        "正常路径：解出来的 .bak 与源逐字节一致");
  Check(FilesIdentical(directory + "/" + manifest, out + "/" + manifest),
        "正常路径：解出来的 .manifest 与源逐字节一致");
  Check(FilesIdentical(directory + "/" + identity, out + "/" + identity),
        "正常路径：解出来的 .identity 与源逐字节一致");
}

}  // namespace

int main(int argc, char** argv) {
  Expect expect = Expect::kNew;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--expect" && index + 1 < argc) {
      const std::string value = argv[++index];
      if (value == "old") {
        expect = Expect::kOld;
      } else if (value == "new") {
        expect = Expect::kNew;
      } else {
        std::printf("未知的 --expect 取值：%s\n", value.c_str());
        return 2;
      }
    } else if (argument == "--keep") {
      g_keep = true;
    } else {
      std::printf("未知参数：%s\n", argument.c_str());
      return 2;
    }
  }
  const std::string root = WorkRoot();
  MakeDirectoryTree(root);

  std::printf("材料包一致性判别（%s）\n",
              expect == Expect::kOld ? "--expect old" : "--expect new");
  TestNormal();

  // 1) .identity 原子替换（三件套的最后一个成员：窗口最宽，必测）
  const CaseResult identity_result =
      RunCase(root, "identity-replace", Action::kAtomicReplace, 2);
  // 2) .manifest 原子替换
  const CaseResult manifest_result =
      RunCase(root, "manifest-replace", Action::kAtomicReplace, 1);
  // 3) .bak 同长度改写（复制游标之后）
  const CaseResult bak_result =
      RunCase(root, "bak-inplace", Action::kInPlaceAhead, 0);
  // 4) .bak 复制期间变长
  const CaseResult growth_result =
      RunCase(root, "bak-growth", Action::kAppendTail, 0);

  if (expect == Expect::kNew) {
    ExpectRejected("identity-replace", identity_result, "内容发生变化");
    ExpectRejected("manifest-replace", manifest_result, "内容发生变化");
    ExpectRejected("bak-inplace", bak_result, "内容发生变化");
    ExpectRejected("bak-growth", growth_result, "变长");
  } else {
    ExpectOldUnreadable("identity-replace", identity_result);
    ExpectOldUnreadable("manifest-replace", manifest_result);
    ExpectOldUnreadable("bak-inplace", bak_result);
    ExpectOldGrowthDropped("bak-growth", growth_result);
  }

  if (!g_keep) {
    const std::string cleanup = "rm -rf '" + root + "'";
    if (std::system(cleanup.c_str()) != 0) {
      Note("清理 " + root + " 失败（不影响判定）");
    }
  }

  const int passed = g_checks - g_failures;
  std::printf("bundle-source-mutation: %d/%d checks passed\n", passed, g_checks);
  return g_failures == 0 ? 0 : 1;
}
