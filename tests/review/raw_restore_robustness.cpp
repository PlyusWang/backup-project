// tests/review/raw_restore_robustness.cpp
//
// 原始归档"单独恢复"（RunRemoteRawRestore）的 Qt 无关健壮性 / 消毒剂回归。
//
// 被测的是**产品实现**本身：include/remote_incremental.h 里的
// RunRemoteRawRestore，以及它下面那两层既有核心——
//   * RemoteArchiveClient：下载 + 长度 / SHA-256 校验 + 原子发布；
//   * BackupEngine / RunRestorePipeline / ArchiveReader：按内容识别格式之后
//     的本地恢复（与 backupctl restore、GUI 本地恢复是同一份实现）。
//
// 本文件**不重新实现任何一条判断**：它只按 include/remote_incremental.h 里
// 写下的硬性行为逐条断言，每一条断言都用"可观察的事实"（返回值、错误文本、
// 目标目录状态、逐字节的树对比）说话：
//
//   * 按**内容**判断格式：随机字节即使显示名以 .bak 结尾也必须被拒绝；
//   * 截断的归档、声明长度离谱的容器：必须失败，而且不能照着不可信的长度
//     去分配 / 读取（ASan / UBSan 打开时这一点是硬证据）；
//   * 单独的 delta 必须失败，并说清它属于某条链、脱离不了依赖链；
//   * 加密归档没有密码时不绕过：明确要求密码；有正确密码则恢复成功；
//   * 任何失败都不在目标目录留下半成品（"no half restore"）。
//
// 跑的是**真的**服务端：本程序用 RemoteArchiveClient 连一个已经在运行的
// backup-server（BPSEC1 加密传输 + 服务端身份 pin 必须显式配置），
// 走注册 / 登录 / 上传的真实路径；服务端由 /tmp/raw_robustness.sh 启动，
// host / port / pin / 工作目录从 argv 取。
//
// 报告格式（一行一条，最后一行是汇总）：
//   ok   <tag> <text>
//   FAIL <tag> <text> -- <detail>
//   raw-restore-robustness: <passed>/<total> checks passed
// 退出码：0 = 全部检查通过。

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "archive_pipeline.h"
#include "container_format.h"
#include "crypto.h"
#include "incremental_backup.h"
#include "incremental_delta.h"
#include "remote_backup_client.h"
#include "remote_incremental.h"

namespace backupproject {
namespace {

namespace net = backupproject::net;

// ---- 报告器：一行一条，最后一条是汇总 ----

int g_passed = 0;
int g_total = 0;

std::string Printable(const std::string& value) {
  return value.empty() ? std::string("<空>") : value;
}

void Report(bool ok, const std::string& tag, const std::string& text,
            const std::string& detail = std::string()) {
  ++g_total;
  if (ok) {
    ++g_passed;
    std::printf("  ok   %s %s\n", tag.c_str(), text.c_str());
  } else {
    std::printf("  FAIL %s %s -- %s\n", tag.c_str(), text.c_str(),
                Printable(detail).c_str());
  }
  std::fflush(stdout);
}

int Finish() {
  std::printf("raw-restore-robustness: %d/%d checks passed\n", g_passed,
              g_total);
  std::fflush(stdout);
  return g_passed == g_total ? 0 : 1;
}

// ---- 文件系统小工具（只用 POSIX，不引入任何测试框架） ----

bool IsDirectory(const std::string& path) {
  struct stat info;
  return ::lstat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool ReadWholeFile(const std::string& path, std::string* out) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return false;
  }
  out->clear();
  char buffer[65536];
  std::size_t got = 0;
  while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    out->append(buffer, got);
  }
  const bool ok = std::ferror(file) == 0;
  std::fclose(file);
  return ok;
}

bool WriteAll(int fd, const std::string& bytes) {
  std::size_t written = 0;
  while (written < bytes.size()) {
    const ssize_t step =
        ::write(fd, bytes.data() + written, bytes.size() - written);
    if (step <= 0) {
      return false;
    }
    written += static_cast<std::size_t>(step);
  }
  return true;
}

// 0600 + O_EXCL：新建一个测试夹具文件（已经存在就是用例自己的错，直接失败）。
// 权限不是被测对象，0600 只是别把测试数据放到别人能改的地方。
bool WriteWholeFile(const std::string& path, const std::string& bytes) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd < 0) {
    return false;
  }
  const bool ok = WriteAll(fd, bytes);
  return ::close(fd) == 0 && ok;
}

// 覆盖已经存在的夹具文件：只有"模拟源树发生变化"这一步用它。
bool RewriteWholeFile(const std::string& path, const std::string& bytes) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    return false;
  }
  const bool ok = WriteAll(fd, bytes);
  return ::close(fd) == 0 && ok;
}

bool MakeDirectory(const std::string& path) {
  return ::mkdir(path.c_str(), 0700) == 0 || errno == EEXIST;
}

bool HasEntryWithPrefix(const std::string& directory,
                        const std::string& prefix) {
  DIR* dir = ::opendir(directory.c_str());
  if (dir == nullptr) {
    return false;
  }
  bool found = false;
  while (struct dirent* entry = ::readdir(dir)) {
    const std::string name = entry->d_name;
    if (name.size() >= prefix.size() &&
        name.compare(0, prefix.size(), prefix) == 0) {
      found = true;
      break;
    }
  }
  ::closedir(dir);
  return found;
}

// ---- 树的逐字节对比：相对路径 + 每个文件的字节 ----

struct TreeEntry {
  std::string relative_path;
  bool is_directory = false;
  std::string contents;
};

void CollectTree(const std::string& root, const std::string& prefix,
                 std::vector<TreeEntry>* out, bool* ok) {
  DIR* dir = ::opendir(root.c_str());
  if (dir == nullptr) {
    *ok = false;
    return;
  }
  std::vector<std::string> names;
  while (struct dirent* entry = ::readdir(dir)) {
    const std::string name = entry->d_name;
    if (name == "." || name == "..") {
      continue;
    }
    names.push_back(name);
  }
  ::closedir(dir);
  std::sort(names.begin(), names.end());
  for (const std::string& name : names) {
    const std::string path = root + "/" + name;
    const std::string relative = prefix.empty() ? name : prefix + "/" + name;
    struct stat info;
    if (::lstat(path.c_str(), &info) != 0) {
      *ok = false;
      return;
    }
    TreeEntry entry;
    entry.relative_path = relative;
    if (S_ISDIR(info.st_mode)) {
      entry.is_directory = true;
      out->push_back(entry);
      CollectTree(path, relative, out, ok);
      if (!*ok) {
        return;
      }
    } else if (S_ISREG(info.st_mode)) {
      if (!ReadWholeFile(path, &entry.contents)) {
        *ok = false;
        return;
      }
      out->push_back(entry);
    } else {
      // 用例只造目录与普通文件；出现别的类型就记成"特殊"，让对比必然不等。
      entry.contents = "<special>";
      out->push_back(entry);
    }
  }
}

bool TreesMatch(const std::string& left, const std::string& right,
                std::string* mismatch) {
  std::vector<TreeEntry> left_entries;
  std::vector<TreeEntry> right_entries;
  bool left_ok = true;
  bool right_ok = true;
  CollectTree(left, std::string(), &left_entries, &left_ok);
  CollectTree(right, std::string(), &right_entries, &right_ok);
  if (!left_ok || !right_ok) {
    *mismatch = "树无法完整读取（" + left + " / " + right + "）";
    return false;
  }
  if (left_entries.size() != right_entries.size()) {
    *mismatch = "条目数不同：" + std::to_string(left_entries.size()) + " vs " +
                std::to_string(right_entries.size());
    return false;
  }
  for (std::size_t index = 0; index < left_entries.size(); ++index) {
    const TreeEntry& a = left_entries[index];
    const TreeEntry& b = right_entries[index];
    if (a.relative_path != b.relative_path) {
      *mismatch = "路径不同：" + a.relative_path + " vs " + b.relative_path;
      return false;
    }
    if (a.is_directory != b.is_directory) {
      *mismatch = "类型不同：" + a.relative_path;
      return false;
    }
    if (!a.is_directory && a.contents != b.contents) {
      *mismatch = "内容不同：" + a.relative_path + "（" +
                  std::to_string(a.contents.size()) + " vs " +
                  std::to_string(b.contents.size()) + " 字节）";
      return false;
    }
  }
  return true;
}

// ---- 字节工具 ----

std::string DeterministicBytes(std::size_t size, std::uint64_t seed) {
  std::string out;
  out.reserve(size);
  std::uint64_t state = seed | 1ull;
  while (out.size() < size) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    for (int index = 0; index < 8 && out.size() < size; ++index) {
      out.push_back(
          static_cast<char>((state >> (index * 8)) & static_cast<std::uint64_t>(0xff)));
    }
  }
  return out;
}

std::uint64_t ReadLeU64(const std::string& bytes, std::size_t offset) {
  std::uint64_t value = 0;
  for (int index = 7; index >= 0; --index) {
    value = (value << 8) |
            static_cast<unsigned char>(bytes[offset + static_cast<std::size_t>(index)]);
  }
  return value;
}

void WriteLeU64(std::string* bytes, std::size_t offset, std::uint64_t value) {
  for (int index = 0; index < 8; ++index) {
    (*bytes)[offset + static_cast<std::size_t>(index)] =
        static_cast<char>((value >> (index * 8)) & static_cast<std::uint64_t>(0xff));
  }
}

bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

bool ContainsAny(const std::string& haystack,
                 const std::vector<std::string>& needles) {
  for (const std::string& needle : needles) {
    if (Contains(haystack, needle)) {
      return true;
    }
  }
  return false;
}

// ---- 客户端装配：与 src/cli/remote_commands.cpp 走同一条路 ----
//
// pin 必须显式给出（endpoint.server_key_pin）。本客户端不做 TOFU：空 pin 时
// Connect() 会直接失败，所以这里也不需要另写一套检查。

void NoopProgress(const net::RemoteTransferProgress& progress) {
  (void)progress;
}

struct HarnessContext {
  net::RemoteArchiveClient* client = nullptr;
  std::string work;
  std::string cache;
  std::string source;
  std::string control_path;
  std::string control_snapshot_id;
};

bool UploadRawArchive(net::RemoteArchiveClient* client,
                      const std::string& local_path,
                      const std::string& display_name,
                      std::string* snapshot_id, std::string* error) {
  net::RemoteSnapshotInfo uploaded;
  if (!client->UploadArchiveFile(local_path, display_name, NoopProgress,
                                 &uploaded, error)) {
    return false;
  }
  if (uploaded.snapshot_id.empty()) {
    *error = "服务端接受了上传，但没有返回快照 id";
    return false;
  }
  *snapshot_id = uploaded.snapshot_id;
  return true;
}

struct RawRestoreRun {
  bool ok = false;
  std::string error;
  net::RemoteRawRestoreOutcome outcome;
};

RawRestoreRun RunRawRestore(const HarnessContext& context,
                            const std::string& snapshot_id,
                            const std::string& display_name,
                            const std::string& destination,
                            const RestoreOptions& options) {
  net::RemoteRawRestoreRequest request;
  request.client = context.client;
  request.cache.root_directory = context.work;
  request.cache.cache_directory = context.cache;
  request.snapshot_id = snapshot_id;
  request.display_name = display_name;
  request.destination_directory = destination;
  request.restore_options = options;
  RawRestoreRun run;
  run.ok = net::RunRemoteRawRestore(request, &run.outcome, &run.error);
  return run;
}

// "no half restore" 的唯一可观察形式：目标目录要么根本不存在，要么存在但
// 一个条目都没有。恢复流水线的暂存目录是**兄弟**目录（.bp-work-*），
// 只有全部成功之后才 rename 成目标目录，所以这条断言是产品承诺的直接检查。
void CheckNoDestination(const std::string& tag,
                        const std::string& destination) {
  struct stat info;
  if (::lstat(destination.c_str(), &info) != 0) {
    Report(true, tag, "失败后目标目录没有被创建（no half restore）");
    return;
  }
  if (!S_ISDIR(info.st_mode)) {
    Report(false, tag, "失败后目标路径存在，而且不是空目录", destination);
    return;
  }
  DIR* dir = ::opendir(destination.c_str());
  if (dir == nullptr) {
    Report(false, tag, "失败后目标目录无法读取", destination);
    return;
  }
  bool empty = true;
  std::string first;
  while (struct dirent* entry = ::readdir(dir)) {
    const std::string name = entry->d_name;
    if (name == "." || name == "..") {
      continue;
    }
    empty = false;
    if (first.empty()) {
      first = name;
    }
  }
  ::closedir(dir);
  if (empty) {
    Report(true, tag, "失败后目标目录存在但为空（no half restore）");
    return;
  }
  Report(false, tag, "失败后目标目录里留下了条目", first);
}

// 一条"必须失败"的用例：上传 -> raw restore -> 失败 + 目标目录干净。
// needles 非空时还要求错误信息里出现其中任意一个片段（可读原因，不是崩溃）。
void ExpectFailure(const HarnessContext& context, const std::string& tag,
                   const std::string& label, const std::string& local_path,
                   const std::string& display_name,
                   const std::string& destination, const RestoreOptions& options,
                   const std::vector<std::string>& needles) {
  std::string snapshot_id;
  std::string error;
  if (!UploadRawArchive(context.client, local_path, display_name, &snapshot_id,
                        &error)) {
    Report(false, tag, label + "：上传原始归档失败（用例无法构造）", error);
    return;
  }
  const RawRestoreRun run =
      RunRawRestore(context, snapshot_id, display_name, destination, options);
  Report(!run.ok, tag, label,
         run.ok ? std::string("竟然成功了（应当失败）") : run.error);
  if (!run.ok && !needles.empty()) {
    Report(ContainsAny(run.error, needles), tag, label + "：原因可读",
           run.error);
  }
  if (!run.ok) {
    CheckNoDestination(tag, destination);
  }
}

// ---- 用例 1：有效的 standalone 控制组 ----

void CaseValidControl(const HarnessContext& context) {
  std::string error;
  if (!RunBackupPipeline(context.source, context.control_path, Filter(),
                         BackupOptions(), &error)) {
    Report(false, "control", "构造前提：产品流水线产出 standalone 完整 .bak",
           error);
    return;
  }
  std::string control_bytes;
  const bool read_ok = ReadWholeFile(context.control_path, &control_bytes);
  Report(read_ok &&
             LooksLikeContainer(
                 reinterpret_cast<const unsigned char*>(control_bytes.data()),
                 control_bytes.size()),
         "control", "构造前提：控制归档按内容是 BKPCNT2 容器", error);

  std::string snapshot_id;
  if (!UploadRawArchive(context.client, context.control_path,
                        "standalone-control.bak", &snapshot_id, &error)) {
    Report(false, "control", "上传 standalone 控制归档（lineage 为空）", error);
    return;
  }
  Report(true, "control", "上传 standalone 控制归档（lineage 为空）");

  const std::string destination = context.work + "/dest-control";
  const RawRestoreRun run = RunRawRestore(
      context, snapshot_id, "standalone-control.bak", destination,
      RestoreOptions());
  Report(run.ok, "control", "RunRemoteRawRestore 成功", run.error);
  if (!run.ok) {
    CheckNoDestination("control", destination);
    return;
  }

  Report(static_cast<std::size_t>(run.outcome.downloaded_bytes) ==
             control_bytes.size(),
         "control", "downloaded_bytes 等于归档实际长度",
         std::to_string(run.outcome.downloaded_bytes) + " vs " +
             std::to_string(control_bytes.size()));
  Report(run.outcome.verified_sha256 == crypto::Sha256Hex(control_bytes),
         "control", "verified_sha256 是实际字节的 SHA-256",
         run.outcome.verified_sha256);
  Report(run.outcome.archive_name == "standalone-control.bak", "control",
         "下载下来的临时文件用的是显示名（可信化之后的单组件名）",
         run.outcome.archive_name);
  Report(run.outcome.archive_format == "v2-container", "control",
         "按内容识别出来的格式是 v2-container", run.outcome.archive_format);
  Report(!run.outcome.password_required, "control",
         "未加密归档不要求密码");
  Report(run.outcome.restored_entries > 0, "control", "restored_entries > 0",
         std::to_string(run.outcome.restored_entries));

  std::string mismatch;
  Report(TreesMatch(context.source, destination, &mismatch), "control",
         "恢复出来的树与源树逐字节一致（相对路径 + 每个文件的字节）",
         mismatch);
}

// ---- 用例 2：被截断的归档 ----

void CaseTruncatedArchive(const HarnessContext& context) {
  std::string control_bytes;
  if (!ReadWholeFile(context.control_path, &control_bytes)) {
    Report(false, "truncated", "读取控制归档", context.control_path);
    return;
  }
  const std::string truncated = control_bytes.substr(0, control_bytes.size() / 2);
  const std::string path = context.work + "/truncated.bak";
  if (!WriteWholeFile(path, truncated)) {
    Report(false, "truncated", "写出截断归档", path);
    return;
  }
  // 构造前提：文件确实只有一半，而且**头部还是完整的容器头**——这样坏掉的
  // 是"长度 / 内容"，不是"格式识别"，用例才落在被测的那条判断上。
  Report(truncated.size() == control_bytes.size() / 2 &&
             truncated.size() > container_v2::kHeaderSize &&
             LooksLikeContainer(
                 reinterpret_cast<const unsigned char*>(truncated.data()),
                 truncated.size()),
         "truncated", "构造前提：一半长度、容器头仍然完整");

  // 这一条现在必须落在"**认得出是我们的容器、但内容与自己的声明不符**"上，
  // 而不是"随便一个不受支持的文件"：截断的归档仍然是归档，界面要如实说"已损
  // 坏"（GUI 的 RAW-U08 就是这一条）。
  ExpectFailure(context, "truncated", "截断到一半的归档必须失败", path,
                "truncated-control.bak", context.work + "/dest-truncated",
                RestoreOptions(),
                {"corrupted archive", "已损坏", "完整性"});
}

// ---- 用例 3：任意字节（显示名仍然以 .bak 结尾） ----

void CaseArbitraryBytes(const HarnessContext& context) {
  std::string garbage = DeterministicBytes(4096, 0x5eed1234u);
  // 极端巧合（前 8 字节撞上 magic）会让"按内容识别"变成别的情形；这里是
  // 确定性字节流，撞上就直接改一个字节并如实报告构造前提。
  const bool collides =
      LooksLikeContainer(reinterpret_cast<const unsigned char*>(garbage.data()),
                         garbage.size()) ||
      LooksLikeDelta(reinterpret_cast<const unsigned char*>(garbage.data()),
                     garbage.size());
  if (collides) {
    garbage[0] = static_cast<char>(garbage[0] ^ 0x7f);
  }
  const std::string path = context.work + "/arbitrary.bak";
  if (!WriteWholeFile(path, garbage)) {
    Report(false, "garbage", "写出 4096 字节随机内容", path);
    return;
  }
  Report(!LooksLikeContainer(
             reinterpret_cast<const unsigned char*>(garbage.data()),
             garbage.size()) &&
             !LooksLikeDelta(
                 reinterpret_cast<const unsigned char*>(garbage.data()),
                 garbage.size()),
         "garbage", "构造前提：4096 字节按内容既不是容器也不是 delta");

  ExpectFailure(context, "garbage",
                "任意 4096 字节（显示名 .bak）必须按内容被拒绝", path,
                "arbitrary-bytes.bak", context.work + "/dest-garbage",
                RestoreOptions(),
                {"不是受支持的", "not a supported archive"});
}

// ---- 用例 4：声明长度离谱的容器 ----

void CaseOversizedDeclaration(const HarnessContext& context) {
  // 布局来自 EncodeContainerHeader 的写入顺序：
  //   0..8 magic、8 version(u16)、10 header_size(u16)、12..16 三个 method + flags、
  //   16 entry_count(u64)、24 packed_size、32 compressed_size、40 payload_size、
  //   48 kdf_iterations(u32)、52..56 四个 u8、56 salt。
  // 用 kSaltOffset 交叉验证偏移：算错了这个用例就没有意义。
  constexpr std::size_t kPackedSizeOffset = 24;
  constexpr std::size_t kCompressedSizeOffset = 32;
  constexpr std::size_t kPayloadSizeOffset = 40;
  Report(container_v2::kHeaderSize == 160 &&
             container_v2::kSaltOffset == kPayloadSizeOffset + 16,
         "oversized", "构造前提：header 偏移与 container_format.h 一致");

  std::string control_bytes;
  if (!ReadWholeFile(context.control_path, &control_bytes)) {
    Report(false, "oversized", "读取控制归档", context.control_path);
    return;
  }
  if (control_bytes.size() <= container_v2::kHeaderSize) {
    Report(false, "oversized", "构造前提：控制归档比 header 长",
           std::to_string(control_bytes.size()));
    return;
  }

  // 两个变体，都保持"三个 size 字段互相自洽"（否则先失败的是别的规则，
  // 就测不到"不可信的声明长度"这一条本身）：
  //   huge     = 2^62 字节，超过格式自己的上界 kMaxStreamSize（4 TiB）；
  //   plausible= 2 TiB，仍在格式允许范围内，只有磁盘上的真实长度对不上。
  struct Variant {
    const char* name;
    std::uint64_t declared;
  };
  const Variant variants[2] = {{"huge-2^62", 1ull << 62},
                               {"plausible-2TiB", 1ull << 41}};
  for (const Variant& variant : variants) {
    std::string crafted = control_bytes;
    WriteLeU64(&crafted, kPackedSizeOffset, variant.declared);
    WriteLeU64(&crafted, kCompressedSizeOffset, variant.declared);
    WriteLeU64(&crafted, kPayloadSizeOffset, variant.declared);
    const std::string path =
        context.work + "/oversized-" + variant.name + ".bak";
    if (!WriteWholeFile(path, crafted)) {
      Report(false, "oversized", std::string("写出变体 ") + variant.name, path);
      continue;
    }
    const bool header_ok =
        ReadLeU64(crafted, kPayloadSizeOffset) == variant.declared &&
        ReadLeU64(crafted, kPackedSizeOffset) == variant.declared &&
        ReadLeU64(crafted, kCompressedSizeOffset) == variant.declared &&
        crafted.size() == control_bytes.size() &&
        LooksLikeContainer(
            reinterpret_cast<const unsigned char*>(crafted.data()),
            crafted.size());
    Report(header_ok, "oversized",
           std::string("构造前提：") + variant.name +
               " 的声明长度确实写进了 header，magic 与长度不变");

    // 目标目录：失败时不能建出来，也不能出现半个文件。
    ExpectFailure(context, "oversized",
                  std::string("声明 payload = ") + variant.name +
                      " 的容器必须失败，而不是照着这个长度去读",
                  path, std::string("oversized-") + variant.name + ".bak",
                  context.work + "/dest-oversized-" + variant.name,
                  RestoreOptions(),
                  {"恢复失败", "已损坏", "corrupted archive",
                   "not a supported archive"});
  }
}

// ---- 用例 5：加密归档的三条路径 ----

void CaseEncryptedArchive(const HarnessContext& context) {
  const std::string password = "raw-restore-robustness-password";
  const std::string encrypted_path = context.work + "/encrypted.bak";
  BackupOptions options;
  options.encryption_method = EncryptionMethod::kAes256CtrHmacSha256;
  options.password = password;
  std::string error;
  if (!RunBackupPipeline(context.source, encrypted_path, Filter(), options,
                         &error)) {
    Report(false, "encrypted", "构造前提：AES-256-CTR 加密的完整 .bak", error);
    return;
  }
  std::string encrypted_bytes;
  ContainerHeader header;
  ArchiveFileInfo info;
  const bool inspected =
      ReadWholeFile(encrypted_path, &encrypted_bytes) &&
      InspectContainerFile(encrypted_path, &header, &error) &&
      IdentifyArchiveFile(encrypted_path, &info, &error);
  Report(inspected &&
             header.encryption_method ==
                 static_cast<std::uint8_t>(
                     EncryptionMethod::kAes256CtrHmacSha256) &&
             !info.password_hint.empty(),
         "encrypted", "构造前提：归档是 AES-256-CTR 加密的（识别结果带密码提示）",
         inspected ? std::string() : error);

  std::string snapshot_id;
  if (!UploadRawArchive(context.client, encrypted_path,
                        "encrypted-control.bak", &snapshot_id, &error)) {
    Report(false, "encrypted", "上传加密控制归档", error);
    return;
  }

  // (a) 没有密码：明确要求密码，不尝试绕过，目标目录一个字节都不碰。
  const std::string dest_without =
      context.work + "/dest-encrypted-no-password";
  const RawRestoreRun without_password =
      RunRawRestore(context, snapshot_id, "encrypted-control.bak", dest_without,
                    RestoreOptions());
  Report(!without_password.ok && without_password.outcome.password_required,
         "encrypted", "没有密码时失败，并且明确标记 password_required",
         without_password.error);
  if (!without_password.ok) {
    CheckNoDestination("encrypted", dest_without);
  }

  // (b) 正确密码 + 未改动的加密控制：必须成功（密码路径的正向对照）。
  const std::string dest_ok = context.work + "/dest-encrypted-ok";
  RestoreOptions with_password;
  with_password.password = password;
  const RawRestoreRun positive = RunRawRestore(
      context, snapshot_id, "encrypted-control.bak", dest_ok, with_password);
  Report(positive.ok, "encrypted", "正确密码恢复未改动的加密归档成功",
         positive.error);
  if (positive.ok) {
    std::string mismatch;
    Report(TreesMatch(context.source, dest_ok, &mismatch), "encrypted",
           "正确密码恢复出来的树与源树逐字节一致", mismatch);
  }

  // (c) 改掉 payload 里的一个字节（其余一切不变）：正确密码也必须失败。
  std::string corrupted = encrypted_bytes;
  const std::size_t payload_offset =
      container_v2::kHeaderSize +
      static_cast<std::size_t>(header.payload_size / 2);
  if (payload_offset >= corrupted.size()) {
    Report(false, "encrypted", "构造前提：payload 中间字节落在文件内",
           std::to_string(payload_offset));
    return;
  }
  corrupted[payload_offset] =
      static_cast<char>(corrupted[payload_offset] ^ 0x5a);
  const std::string corrupted_path = context.work + "/encrypted-corrupted.bak";
  if (!WriteWholeFile(corrupted_path, corrupted)) {
    Report(false, "encrypted", "写出被改动的加密归档", corrupted_path);
    return;
  }
  Report(corrupted.size() == encrypted_bytes.size() &&
             corrupted[payload_offset] != encrypted_bytes[payload_offset],
         "encrypted", "构造前提：只改了 payload 中间的一个字节，长度不变");

  std::string corrupted_id;
  if (!UploadRawArchive(context.client, corrupted_path,
                        "encrypted-corrupted.bak", &corrupted_id, &error)) {
    Report(false, "encrypted", "上传被改动的加密归档", error);
    return;
  }
  const std::string dest_corrupted = context.work + "/dest-encrypted-corrupted";
  const RawRestoreRun negative = RunRawRestore(
      context, corrupted_id, "encrypted-corrupted.bak", dest_corrupted,
      with_password);
  // 下载这一层必须是通过的（服务端元数据与实际字节一致），失败必须发生在
  // 加密层的认证上——否则这条用例测到的只是传输层。
  Report(!negative.ok && negative.outcome.verified_sha256 ==
                             crypto::Sha256Hex(corrupted),
         "encrypted",
         "改动一个 payload 字节后（正确密码）恢复失败，且失败发生在认证层",
         negative.ok ? std::string("竟然成功了")
                     : (negative.outcome.verified_sha256 + " / " +
                        negative.error));
  // 可区分性的正面证据（GUI 文案就靠它）：payload 字节被改动时 payload_sha256
  // 先在 HMAC 之前比对失败，所以这一条必须说"已损坏"，**不能**甩给密码。
  Report(!negative.ok &&
             Contains(negative.error, "corrupted archive") &&
             !Contains(negative.error, "authentication failed"),
         "encrypted",
         "payload 被改动 -> 明确是“已损坏 / 完整性校验失败”，不甩给密码",
         negative.error);
  if (!negative.ok) {
    CheckNoDestination("encrypted", dest_corrupted);
  }

  // (d) 错密码：必须说"认证没过"（密码错**或**容器头被改动），而且不能说成
  // "文件已损坏"——这两件事在密码学上不可区分，文案必须如实。
  const std::string dest_wrong = context.work + "/dest-encrypted-wrong";
  RestoreOptions wrong_options;
  wrong_options.password = "definitely-not-the-password";
  const RawRestoreRun wrong = RunRawRestore(
      context, snapshot_id, "encrypted-control.bak", dest_wrong, wrong_options);
  Report(!wrong.ok && Contains(wrong.error, "authentication failed") &&
             !Contains(wrong.error, "corrupted archive"),
         "encrypted",
         "错密码 -> 明确是“认证没过”（密码错或容器头被改动），不说成已损坏",
         wrong.ok ? std::string("竟然成功了") : wrong.error);
  if (!wrong.ok) {
    CheckNoDestination("encrypted", dest_wrong);
  }
}

// ---- 用例 8：密码重试的会话（Prepare 一次 -> 错 -> 错 -> 对）----

// GUI 的"错密码可以直接重输、不重新下载"走的就是这条路径。这里在消毒剂下把它
// 完整走一遍：Prepare **一次**、Run 多次；每次失败都不碰目标目录，那份归档一直
// 是同一个 inode（没有被重新下载），最后成功并清掉工作目录。
void CasePasswordRetry(const HarnessContext& context) {
  const std::string password = "raw-restore-robustness-retry";
  const std::string encrypted_path = context.work + "/encrypted-retry.bak";
  BackupOptions options;
  options.encryption_method = EncryptionMethod::kAes256CtrHmacSha256;
  options.password = password;
  std::string error;
  if (!RunBackupPipeline(context.source, encrypted_path, Filter(), options,
                         &error)) {
    Report(false, "password-retry", "构造前提：加密的 standalone 归档", error);
    return;
  }
  std::string snapshot_id;
  if (!UploadRawArchive(context.client, encrypted_path, "encrypted-retry.bak",
                        &snapshot_id, &error)) {
    Report(false, "password-retry", "上传加密归档", error);
    return;
  }
  net::RemoteRawRestoreRequest request;
  request.client = context.client;
  request.cache.root_directory = context.work;
  request.cache.cache_directory = context.cache;
  request.snapshot_id = snapshot_id;
  request.display_name = "encrypted-retry.bak";
  request.destination_directory = context.work + "/dest-password-retry";
  net::RemoteRawRestoreSession session;
  net::RemoteRawRestoreOutcome outcome;
  const bool prepared = session.Prepare(request, &outcome, &error);
  Report(prepared, "password-retry", "Prepare：下载 + SHA-256 校验 + 识别",
         error);
  if (!prepared) {
    return;
  }
  const std::string archive_path = session.archive_path_for_test();
  struct stat before;
  const bool stat_ok = ::stat(archive_path.c_str(), &before) == 0;
  Report(session.download_count() == 1 && stat_ok, "password-retry",
         "Prepare 之后：只下载了 1 次，那份归档在工作目录里", archive_path);

  net::RemoteRawRestoreOutcome none;
  std::string none_error;
  const bool none_ok = session.Run(std::string(), &none, &none_error);
  Report(!none_ok && none.password_required && session.prepared(),
         "password-retry",
         "没有密码：明确 password_required，而且会话保持有效", none_error);
  CheckNoDestination("password-retry", request.destination_directory);

  for (int attempt = 0; attempt < 2; ++attempt) {
    net::RemoteRawRestoreOutcome wrong;
    std::string wrong_error;
    const bool wrong_ok = session.Run(
        "wrong-password-" + std::to_string(attempt), &wrong, &wrong_error);
    Report(!wrong_ok && Contains(wrong_error, "authentication failed") &&
               !Contains(wrong_error, "corrupted archive"),
           "password-retry",
           "错密码报“认证没过”，不说成归档损坏（第 " +
               std::to_string(attempt + 1) + " 次）",
           wrong_error);
    CheckNoDestination("password-retry", request.destination_directory);
  }
  struct stat after_wrong;
  const bool still_there = ::stat(archive_path.c_str(), &after_wrong) == 0;
  Report(still_there && session.download_count() == 1 &&
             after_wrong.st_ino == before.st_ino &&
             after_wrong.st_size == before.st_size,
         "password-retry",
         "两次错密码之后仍是**同一份**字节（同 inode 同长度），下载次数还是 1");

  net::RemoteRawRestoreOutcome right;
  std::string right_error;
  const bool right_ok = session.Run(password, &right, &right_error);
  Report(right_ok, "password-retry", "正确密码：恢复成功", right_error);
  if (right_ok) {
    std::string mismatch;
    Report(TreesMatch(context.source, request.destination_directory, &mismatch),
           "password-retry", "密码重试之后恢复出来的树与源树逐字节一致",
           mismatch);
    Report(session.download_count() == 1, "password-retry",
           "整条交互只下载过一次（错密码重试用的是同一份字节）");
  }
  session.Abandon();
  struct stat gone;
  Report(::stat(archive_path.c_str(), &gone) != 0, "password-retry",
         "Abandon 之后那份临时归档被删掉（收尾是 fail-closed）");
}

// ---- 用例 6：单独一份 delta ----

void CaseLoneDelta(const HarnessContext& context) {
  // 本地仓库里造一条两步链：F0（完整基线）-> D1（增量）。
  const std::string source = context.work + "/delta-source";
  const std::string repository = context.work + "/repo";
  if (!MakeDirectory(source) || !MakeDirectory(repository)) {
    Report(false, "lone-delta", "构造前提：建出源目录与本地仓库",
           source + " / " + repository);
    return;
  }
  if (!WriteWholeFile(source + "/A.txt", "A-v1\n") ||
      !WriteWholeFile(source + "/B.txt", DeterministicBytes(4096, 0xabcdu))) {
    Report(false, "lone-delta", "构造前提：写出源树");
    return;
  }

  const std::string identity = "raw-restore-robustness-repository";
  IncrementalOutcome outcome;
  std::string error;
  Filter filter;
  const std::vector<std::string> no_rules;
  if (!RunIncrementalBackup(source, repository, "F0.bak", identity, filter,
                            BackupOptions(), no_rules, no_rules, std::string(),
                            &outcome, &error)) {
    Report(false, "lone-delta", "构造前提：第一次增量备份产出完整基线", error);
    return;
  }
  Report(outcome.kind == IncrementalOutcome::Kind::kFullBaseline,
         "lone-delta", "构造前提：第一次备份是完整基线");

  if (!RewriteWholeFile(source + "/A.txt", "A-v2-with-more-content\n") ||
      !WriteWholeFile(source + "/C.txt", "C-new\n")) {
    Report(false, "lone-delta", "构造前提：改动源树");
    return;
  }
  IncrementalOutcome second;
  if (!RunIncrementalBackup(source, repository, "D1.bak", identity, filter,
                            BackupOptions(), no_rules, no_rules, "F0.bak",
                            &second, &error)) {
    Report(false, "lone-delta", "构造前提：第二次增量备份产出 delta", error);
    return;
  }
  Report(second.kind == IncrementalOutcome::Kind::kDelta, "lone-delta",
         "构造前提：第二次备份真的产出了 delta");

  const std::string delta_path = repository + "/D1.bak";
  std::string delta_bytes;
  Report(ReadWholeFile(delta_path, &delta_bytes) &&
             ClassifySnapshotFile(delta_path, nullptr) ==
                 SnapshotFileKind::kDelta,
         "lone-delta",
         "构造前提：D1.bak 按内容被识别成 BKPINC1 delta（不是完整归档）");

  ExpectFailure(context, "lone-delta",
                "单独一份 delta 不能脱离依赖链恢复", delta_path,
                "lone-delta.bak", context.work + "/dest-lone-delta",
                RestoreOptions(),
                {"依赖链", "父快照", "chain", "parent"});
}

// ---- 用例 7：不存在的快照 id ----

void CaseMissingSnapshot(const HarnessContext& context) {
  // 32 个十六进制字符、格式合法但服务端一定没有的一个 id。
  const std::string missing_id = "0123456789abcdef0123456789abcdef";
  const std::string destination = context.work + "/dest-missing";
  std::vector<net::RemoteSnapshotInfo> snapshots;
  std::string error;
  if (context.client->List(&snapshots, &error)) {
    for (const net::RemoteSnapshotInfo& info : snapshots) {
      if (info.snapshot_id == missing_id) {
        Report(false, "missing-id",
               "构造前提：这个 id 在服务端确实不存在", missing_id);
        return;
      }
    }
  }
  const RawRestoreRun run =
      RunRawRestore(context, missing_id, "missing.bak", destination,
                    RestoreOptions());
  Report(!run.ok, "missing-id", "不存在的快照 id 干净地失败（没有崩溃）",
         run.ok ? std::string("竟然成功了") : run.error);
  Report(!run.ok && !run.error.empty(), "missing-id",
         "失败时给出了可读的原因", run.error);
  if (!run.ok) {
    CheckNoDestination("missing-id", destination);
  }
}

// ---- 入口 ----

int RunAll(int argc, char** argv) {
  if (argc != 5) {
    std::fprintf(stderr,
                 "用法: %s <host> <port> <sha256:指纹> <工作目录>\n",
                 argv[0]);
    return 2;
  }
  const std::string host = argv[1];
  const unsigned long port_value = std::strtoul(argv[2], nullptr, 10);
  if (port_value == 0 || port_value > 65535) {
    std::fprintf(stderr, "端口不合法: %s\n", argv[2]);
    return 2;
  }
  HarnessContext context;
  context.work = argv[4];
  context.cache = context.work + "/cache";
  context.source = context.work + "/source";
  context.control_path = context.work + "/control.bak";
  if (!IsDirectory(context.work) || !MakeDirectory(context.cache) ||
      !MakeDirectory(context.source)) {
    std::fprintf(stderr, "工作目录不可用: %s\n", context.work.c_str());
    return 2;
  }
  std::printf("[raw-restore-robustness] 连接 %s:%lu（BPSEC1，pin 显式配置）\n",
              host.c_str(), port_value);

  // 源树：大文件 + 小文件 + 一层子目录，足够让"逐字节一致"这句话有意义。
  const bool source_ok =
      WriteWholeFile(context.source + "/big.bin",
                     DeterministicBytes(200000, 0x1234abcdu)) &&
      WriteWholeFile(context.source + "/A.txt", "A-v1\n") &&
      WriteWholeFile(context.source + "/C.txt", "C-v1\n") &&
      MakeDirectory(context.source + "/sub") &&
      WriteWholeFile(context.source + "/sub/nested.bin",
                     DeterministicBytes(7777, 0xfeedfaceu));
  Report(source_ok, "setup", "构造源树（大小文件 + 子目录）", context.source);
  if (!source_ok) {
    return Finish();
  }

  // 与 src/cli/remote_commands.cpp 完全同一条装配：endpoint 带 pin -> Connect
  // -> Register -> Login。pin 来自 backup-server-keygen 打印的 sha256: 指纹。
  net::RemoteEndpoint endpoint;
  endpoint.host = host;
  endpoint.port = static_cast<std::uint16_t>(port_value);
  endpoint.timeout_seconds = 120;
  endpoint.server_key_pin = argv[3];

  net::RemoteArchiveClient client;
  std::string error;
  const std::string username =
      "raw-robust-" + std::to_string(static_cast<long>(::getpid()));
  const std::string password =
      "raw-robustness-password-" + std::to_string(static_cast<long>(::getpid()));
  bool connected = client.Connect(endpoint, &error);
  if (connected) {
    connected = client.Register(username, password, &error);
  }
  if (connected) {
    connected = client.Login(username, password, &error);
  }
  Report(connected, "setup", "BPSEC1 握手 + 注册 + 登录（真实 backup-server）",
         error);
  if (!connected) {
    client.Disconnect();
    return Finish();
  }
  context.client = &client;

  CaseValidControl(context);
  CaseTruncatedArchive(context);
  CaseArbitraryBytes(context);
  CaseOversizedDeclaration(context);
  CaseEncryptedArchive(context);
  CasePasswordRetry(context);
  CaseLoneDelta(context);
  CaseMissingSnapshot(context);

  // 收尾：失败与成功路径都不该在工作目录里留下私有的临时目录。
  const bool leftover = HasEntryWithPrefix(context.work, ".bp-work-") ||
                        HasEntryWithPrefix(context.cache, "raw-restore-");
  Report(!leftover, "hygiene",
         "工作目录与缓存目录里没有留下临时目录（.bp-work-* / raw-restore-*）");

  client.Logout(&error);
  client.Disconnect();
  return Finish();
}

}  // namespace
}  // namespace backupproject

int main(int argc, char** argv) { return backupproject::RunAll(argc, argv); }
