// tests/unit/legacy_filter_test.cpp
//
// legacy v0.1 路径的 Filter 元数据投影测试。
//
// Modern GUI / CLI 的旧签名
//   BackupEngine::Backup(source_directory, archive_file, filter, error_message)
// 仍然走 legacy v0.1 的 ArchiveWriter。它喂给 Filter 的元数据必须和 v2
// pipeline 一样完整：type、uid、gid、user_name、group_name 少一个，
// type: / uid: / gid: / user: / group: / size: 规则就会按默认值乱判
// （type 默认 kRegularFile、uid / gid 默认 0）。
//
// 这里直接调用那个旧签名，把"投影正确"钉在三件事上：
//   1. uid / user / group 的 include 真的能让普通文件进归档；
//   2. type:symlink 的 exclude 能跳过链接，而 type:file / size: 的 exclude
//      不能把链接误判成普通文件；
//   3. v0.1 的线上格式一个字节都没变（前 24 字节 + entry_count + 归档大小
//      + ArchiveReader::Extract 往返）。
//
// 独立 main()：不依赖任何第三方测试框架，失败时返回非零，最后一行打印
// "legacy-filter: N/M checks passed"。
//
// 断言写法约定：凡是"调用 + 检查它的输出"的地方，都先把调用结果存进局部变量
// 再断言。把 reader.InspectHeader(...) 和 summary.entry_count 写在同一个实参
// 列表里，实参求值顺序未指定，失败时打印出来的可能是调用之前的旧值——那会让
// 失败信息骗人。

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "archive.h"
#include "backup_engine.h"
#include "filter.h"
#include "test_support.h"
#include "user_directory.h"

namespace bp = backupproject;

namespace {

// ---- fixture --------------------------------------------------------------

// v0.1 不覆盖已存在的归档，所以归档路径先清干净。
std::string FreshArchive(const std::string& name) {
  const std::string path = test_support::TempRoot() + "/" + name + ".bak";
  test_support::RemoveTree(path);
  return path;
}

// 两个普通文件 + 一层子目录：够验证 include 规则真的放行了文件内容。
std::string MakeBasicSource(const std::string& name) {
  const std::string source = test_support::FreshDir(name);
  test_support::WriteFile(source + "/reg.txt", "legacy v0.1 payload\n", 0644);
  test_support::Mkdir(source + "/sub", 0755);
  test_support::WriteFile(source + "/sub/nested.txt", "nested payload\n", 0640);
  return source;
}

// 扁平 fixture：只有普通文件，没有子目录。include 只筛普通文件，目录作为
// 结构项始终保留，所以只有扁平树才可能出现"归档里只剩根条目"。
std::string MakeFlatSource(const std::string& name) {
  const std::string source = test_support::FreshDir(name);
  test_support::WriteFile(source + "/reg.txt", "legacy v0.1 payload\n", 0644);
  test_support::WriteFile(source + "/other.txt", "another payload\n", 0644);
  return source;
}

// 含软链接的 fixture：链接指向同目录的普通文件，没有被规则排除时
// legacy 写入器必须整次失败（v0.1 不支持 special files）。
std::string MakeSymlinkSource(const std::string& name) {
  const std::string source = test_support::FreshDir(name);
  test_support::WriteFile(source + "/data.txt", "data payload\n", 0644);
  test_support::Check(
      test_support::CreateSymlink("data.txt", source + "/link.txt"),
      "fixture: create symlink");
  return source;
}

// ---- 调用与断言工具 --------------------------------------------------------

bp::Filter FilterWith(bp::FilterAction action, const std::string& rule) {
  bp::Filter filter;
  std::string error;
  const bool accepted = filter.AddRule(action, rule, &error);
  test_support::Check(accepted, "filter rule accepted: " + rule, error);
  return filter;
}

// 旧签名：不传 BackupOptions，走的就是 legacy v0.1 ArchiveWriter。
bool BackupLegacy(const std::string& source, const std::string& archive,
                  const bp::Filter& filter, std::string* error) {
  bp::BackupEngine engine;
  return engine.Backup(source, archive, filter, error);
}

// 备份必须失败、必须说清原因、必须不留 .bak。
void CheckBackupFails(const std::string& label, const std::string& source,
                      const std::string& archive, const bp::Filter& filter) {
  std::string error;
  const bool backed_up = BackupLegacy(source, archive, filter, &error);
  test_support::Check(!backed_up, label + ": backup fails", error);
  test_support::Check(
      error.find("Unsupported source entry type") != std::string::npos,
      label + ": error names the unsupported entry type", error);
  test_support::Check(!test_support::Exists(archive),
                      label + ": failure left no .bak behind");
}

// ---- v0.1 全局 header 的独立解码 -------------------------------------------
//
// 故意不复用生产代码的 DecodeGlobalHeader：这里要钉的是"线上字节"。
// 用自己的解码去读，才能发现"实现和测试一起改了"这种情况。
struct RawGlobalHeader {
  unsigned char magic[8] = {0};
  std::uint16_t version = 0;
  std::uint16_t flags = 0;
  std::uint32_t header_size = 0;
  std::uint64_t entry_count = 0;
};

bool ReadRawGlobalHeader(const std::string& path, RawGlobalHeader* out) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return false;
  }
  unsigned char buffer[24];
  std::size_t done = 0;
  while (done < sizeof(buffer)) {
    const ssize_t got = ::read(fd, buffer + done, sizeof(buffer) - done);
    if (got <= 0) {
      ::close(fd);
      return false;
    }
    done += static_cast<std::size_t>(got);
  }
  ::close(fd);

  std::memcpy(out->magic, buffer, sizeof(out->magic));
  out->version =
      static_cast<std::uint16_t>(static_cast<std::uint16_t>(buffer[8]) |
                                 (static_cast<std::uint16_t>(buffer[9]) << 8));
  out->flags =
      static_cast<std::uint16_t>(static_cast<std::uint16_t>(buffer[10]) |
                                 (static_cast<std::uint16_t>(buffer[11]) << 8));
  // 每个字节先转无符号再移位：整型提升成 int 之后，左移 24 位是未定义行为。
  out->header_size = static_cast<std::uint32_t>(buffer[12]) |
                     (static_cast<std::uint32_t>(buffer[13]) << 8) |
                     (static_cast<std::uint32_t>(buffer[14]) << 16) |
                     (static_cast<std::uint32_t>(buffer[15]) << 24);
  out->entry_count = 0;
  for (int index = 7; index >= 0; --index) {
    out->entry_count = (out->entry_count << 8) | buffer[16 + index];
  }
  return true;
}

// ---- 用例 ------------------------------------------------------------------

// 没有任何规则时，软链接仍然让 legacy 备份失败：这是 v0.1 的既有语义，
// 不能被"补全元数据"顺手改掉。
void UnfilteredSymlinkStillFails() {
  test_support::Section("unfiltered symlink still fails (v0.1 semantics)");
  const std::string source = MakeSymlinkSource("legacy-sym-plain");
  const std::string archive = FreshArchive("sym-plain");
  const bp::Filter no_rules;
  CheckBackupFails("no rules", source, archive, no_rules);
}

// exclude type:symlink：链接被跳过，备份成功，恢复出来的树里没有链接。
void ExplicitExcludeSkipsSymlink() {
  test_support::Section("exclude type:symlink skips the link");
  const std::string source = MakeSymlinkSource("legacy-sym-excluded");
  const std::string archive = FreshArchive("sym-excluded");
  const std::string out = test_support::FreshDir("legacy-sym-excluded-out");
  const bp::Filter filter =
      FilterWith(bp::FilterAction::kExclude, "type:symlink");

  std::string error;
  const bool backed_up = BackupLegacy(source, archive, filter, &error);
  test_support::Check(backed_up, "backup succeeds once the symlink is excluded",
                      error);
  const bp::ArchiveReader reader;
  const bool restored = reader.Extract(archive, out, &error);
  test_support::Check(restored, "restore succeeds", error);
  test_support::Check(!test_support::Exists(out + "/link.txt"),
                      "restored tree contains no symlink");
  std::string content;
  const bool kept = test_support::ReadFile(out + "/data.txt", &content);
  test_support::Check(kept && content == "data payload\n",
                      "excluding links does not drop the regular file");
}

// exclude type:file 只认普通文件：软链接不是普通文件，所以它仍然没有被排除，
// backup 必须失败。若投影里 type 退回默认的 kRegularFile，这里就会"成功"。
void ExcludeTypeFileDoesNotSkipSymlink() {
  test_support::Section("exclude type:file must not match a symlink");
  const std::string source = MakeSymlinkSource("legacy-type-file");
  const std::string archive = FreshArchive("type-file");
  const bp::Filter filter = FilterWith(bp::FilterAction::kExclude, "type:file");
  CheckBackupFails("exclude type:file", source, archive, filter);
}

// size: 只对普通文件有意义：软链接的 size 被填 0，但如果它被当成 0 字节的
// 普通文件，exclude size:<=1KB 就会把它一起排除，backup 于是"假成功"。
void ExcludeSizeDoesNotSkipSymlink() {
  test_support::Section("exclude size:<=1KB must not match a symlink");
  const std::string source = MakeSymlinkSource("legacy-size");
  const std::string archive = FreshArchive("size");
  const bp::Filter filter =
      FilterWith(bp::FilterAction::kExclude, "size:<=1KB");
  CheckBackupFails("exclude size:<=1KB", source, archive, filter);
}

// include uid:<当前 uid>：普通文件真的进归档，解出来内容逐字节一致。
void IncludeByUid() {
  test_support::Section("include uid:<current uid> keeps regular files");
  const std::string source = MakeBasicSource("legacy-uid");
  const std::string archive = FreshArchive("uid");
  const std::string out = test_support::FreshDir("legacy-uid-out");
  const std::uint32_t uid = static_cast<std::uint32_t>(::getuid());
  const bp::Filter filter =
      FilterWith(bp::FilterAction::kInclude, "uid:" + std::to_string(uid));

  std::string error;
  const bool backed_up = BackupLegacy(source, archive, filter, &error);
  test_support::Check(backed_up, "backup with include uid:<self> succeeds",
                      error);

  const bp::ArchiveReader reader;
  bp::ArchiveSummary summary;
  const bool inspected = reader.InspectHeader(archive, &summary, &error);
  test_support::Check(
      inspected && summary.entry_count == 4,
      "archive holds all 4 entries (. + 2 files + 1 dir)",
      "entry_count=" + std::to_string(summary.entry_count) + " error=" + error);
  const bool restored = reader.Extract(archive, out, &error);
  test_support::Check(restored, "restore succeeds", error);
  std::string detail;
  const bool reg_ok = test_support::CompareFiles(source + "/reg.txt",
                                                 out + "/reg.txt", &detail);
  test_support::Check(reg_ok, "reg.txt round-trips byte for byte", detail);
  detail.clear();
  const bool nested_ok = test_support::CompareFiles(
      source + "/sub/nested.txt", out + "/sub/nested.txt", &detail);
  test_support::Check(nested_ok, "sub/nested.txt round-trips byte for byte",
                      detail);
}

// include user:<当前用户名>：名字现算（getpwuid_r），不写死。
void IncludeByUserName() {
  test_support::Section("include user:<current user name>");
  std::string user_name;
  const bool resolved =
      bp::LookupUserName(static_cast<std::uint32_t>(::getuid()), &user_name);
  test_support::Check(resolved && !user_name.empty(),
                      "current uid resolves to a user name");
  if (user_name.empty()) {
    return;
  }
  const std::string source = MakeBasicSource("legacy-user");
  const std::string archive = FreshArchive("user");
  const std::string out = test_support::FreshDir("legacy-user-out");
  const bp::Filter filter =
      FilterWith(bp::FilterAction::kInclude, "user:" + user_name);

  std::string error;
  const bool backed_up = BackupLegacy(source, archive, filter, &error);
  test_support::Check(
      backed_up, "backup with include user:" + user_name + " succeeds", error);
  const bp::ArchiveReader reader;
  bp::ArchiveSummary summary;
  const bool inspected = reader.InspectHeader(archive, &summary, &error);
  test_support::Check(
      inspected && summary.entry_count == 4,
      "user: rule keeps the regular files",
      "entry_count=" + std::to_string(summary.entry_count) + " error=" + error);
  const bool restored = reader.Extract(archive, out, &error);
  test_support::Check(restored, "restore succeeds", error);
  std::string content;
  const bool kept = test_support::ReadFile(out + "/reg.txt", &content);
  test_support::Check(kept && content == "legacy v0.1 payload\n",
                      "reg.txt is present after the user: filter");
}

// include group:<当前组名>：同样现算。
void IncludeByGroupName() {
  test_support::Section("include group:<current group name>");
  std::string group_name;
  const bool resolved =
      bp::LookupGroupName(static_cast<std::uint32_t>(::getgid()), &group_name);
  test_support::Check(resolved && !group_name.empty(),
                      "current gid resolves to a group name");
  if (group_name.empty()) {
    return;
  }
  const std::string source = MakeBasicSource("legacy-group");
  const std::string archive = FreshArchive("group");
  const std::string out = test_support::FreshDir("legacy-group-out");
  const bp::Filter filter =
      FilterWith(bp::FilterAction::kInclude, "group:" + group_name);

  std::string error;
  const bool backed_up = BackupLegacy(source, archive, filter, &error);
  test_support::Check(backed_up,
                      "backup with include group:" + group_name + " succeeds",
                      error);
  const bp::ArchiveReader reader;
  bp::ArchiveSummary summary;
  const bool inspected = reader.InspectHeader(archive, &summary, &error);
  test_support::Check(
      inspected && summary.entry_count == 4,
      "group: rule keeps the regular files",
      "entry_count=" + std::to_string(summary.entry_count) + " error=" + error);
  const bool restored = reader.Extract(archive, out, &error);
  test_support::Check(restored, "restore succeeds", error);
  std::string content;
  const bool kept = test_support::ReadFile(out + "/reg.txt", &content);
  test_support::Check(kept && content == "legacy v0.1 payload\n",
                      "reg.txt is present after the group: filter");
}

// uid 的反例：换成别人的 uid，普通文件全部被过滤。用扁平 fixture，因为目录
// 作为结构项不受 include 影响，只有扁平树才可能"归档里只剩根条目"。
// 这条同时说明 uid: 真的在读 lstat 的 uid，而不是一条恒真的规则。
void IncludeWithOtherUidFiltersFilesOut() {
  test_support::Section("include uid:<other> keeps only the root entry");
  const std::string source = MakeFlatSource("legacy-uid-other");
  const std::string archive = FreshArchive("uid-other");
  const std::string out = test_support::FreshDir("legacy-uid-other-out");
  const std::uint32_t other = static_cast<std::uint32_t>(::getuid()) + 1;
  const bp::Filter filter =
      FilterWith(bp::FilterAction::kInclude, "uid:" + std::to_string(other));

  std::string error;
  const bool backed_up = BackupLegacy(source, archive, filter, &error);
  test_support::Check(
      backed_up, "backup succeeds (the root entry is never filtered)", error);
  const bp::ArchiveReader reader;
  bp::ArchiveSummary summary;
  const bool inspected = reader.InspectHeader(archive, &summary, &error);
  test_support::Check(
      inspected && summary.entry_count == 1,
      "archive keeps only the source root entry",
      "entry_count=" + std::to_string(summary.entry_count) + " error=" + error);
  const bool restored = reader.Extract(archive, out, &error);
  test_support::Check(restored, "restore of the root-only archive succeeds",
                      error);
  test_support::Check(test_support::DirEntries(out).empty(),
                      "restored tree is empty");
}

// v0.1 的线上格式一个字节都没变：magic / version / flags / header_size /
// entry_count / 归档总长度，全部按偏移手工读出来核对。
void LegacyWireFormatIsUnchanged() {
  test_support::Section("v0.1 wire format is unchanged");
  const std::string source = MakeBasicSource("legacy-format");
  const std::string archive = FreshArchive("format");
  const std::string out = test_support::FreshDir("legacy-format-out");
  const bp::Filter no_rules;

  std::string error;
  const bool backed_up = BackupLegacy(source, archive, no_rules, &error);
  test_support::Check(backed_up, "backup without rules succeeds", error);

  RawGlobalHeader raw;
  const bool read_ok = ReadRawGlobalHeader(archive, &raw);
  test_support::Check(read_ok, "the first 24 bytes are readable");
  const char expected_magic[9] = "BKPARCH\0";
  test_support::Check(std::memcmp(raw.magic, expected_magic, 8) == 0,
                      "magic is BKPARCH\\0");
  test_support::Check(std::memcmp(raw.magic, bp::archive_v01::kMagic,
                                  sizeof(bp::archive_v01::kMagic)) == 0,
                      "the kMagic constant still matches those bytes");
  test_support::Check(raw.version == 1, "format version is 1");
  test_support::Check(raw.flags == 0, "flags are 0");
  test_support::Check(raw.header_size == 24, "global header size is 24");
  test_support::Check(raw.entry_count == 4,
                      "entry_count counts . + 2 files + 1 dir");

  // 24 (header) + 33 (".", 1 字节路径) + 59 ("reg.txt", 7 + 20 payload)
  // + 35 ("sub", 3) + 61 ("sub/nested.txt", 14 + 15 payload) = 212。
  // 这是 layout 的逐字节证据：任何一处 header 宽度或字段顺序变了都对不上。
  struct stat info;
  const bool stat_ok = test_support::StatOf(archive, &info);
  test_support::Check(stat_ok && info.st_size == 212,
                      "archive size matches the v0.1 layout exactly");

  const bp::ArchiveReader reader;
  bp::ArchiveSummary summary;
  const bool inspected = reader.InspectHeader(archive, &summary, &error);
  test_support::Check(
      inspected && summary.format_version == 1,
      "ArchiveSummary.format_version is 1",
      "format_version=" + std::to_string(summary.format_version) +
          " error=" + error);
  const bool restored = reader.Extract(archive, out, &error);
  test_support::Check(restored, "ArchiveReader::Extract still restores it",
                      error);
  std::string detail;
  const bool reg_ok = test_support::CompareFiles(source + "/reg.txt",
                                                 out + "/reg.txt", &detail);
  test_support::Check(reg_ok, "restored reg.txt is byte-identical", detail);
  detail.clear();
  const bool nested_ok = test_support::CompareFiles(
      source + "/sub/nested.txt", out + "/sub/nested.txt", &detail);
  test_support::Check(nested_ok, "restored sub/nested.txt is byte-identical",
                      detail);
}

// 补全投影不能把既有的 size: / type: 语义改坏：
//   * size: 仍然按普通文件的内容长度筛；
//   * exclude type:fifo 能跳过 FIFO（投影里它是 kFifo，不是 kRegularFile）。
void SizeAndFifoRulesKeepWorking() {
  test_support::Section("size: and type:fifo rules keep working");

  const std::string small_source = test_support::FreshDir("legacy-size-small");
  test_support::WriteFile(small_source + "/small.txt", std::string(100, 'A'),
                          0644);
  test_support::WriteFile(small_source + "/big.txt", std::string(2048, 'B'),
                          0644);
  const std::string small_archive = FreshArchive("size-small");
  const std::string small_out = test_support::FreshDir("legacy-size-small-out");
  const bp::Filter size_filter =
      FilterWith(bp::FilterAction::kInclude, "size:>1KB");
  std::string error;
  const bool small_backed_up =
      BackupLegacy(small_source, small_archive, size_filter, &error);
  test_support::Check(small_backed_up,
                      "include size:>1KB succeeds on the legacy path", error);
  const bp::ArchiveReader reader;
  const bool small_restored = reader.Extract(small_archive, small_out, &error);
  test_support::Check(small_restored,
                      "restore of the size-filtered archive succeeds", error);
  std::string content;
  const bool kept = test_support::ReadFile(small_out + "/big.txt", &content);
  test_support::Check(kept && content.size() == 2048,
                      "the 2 KB file is kept by size:>1KB");
  test_support::Check(!test_support::Exists(small_out + "/small.txt"),
                      "the 100 byte file is dropped by size:>1KB");

  const std::string fifo_source = test_support::FreshDir("legacy-fifo");
  test_support::WriteFile(fifo_source + "/keep.txt", "keep me\n", 0644);
  test_support::Check(test_support::CreateFifo(fifo_source + "/pipe", 0644),
                      "fixture: create FIFO");
  const std::string fifo_archive = FreshArchive("fifo");
  const std::string fifo_out = test_support::FreshDir("legacy-fifo-out");
  const bp::Filter fifo_filter =
      FilterWith(bp::FilterAction::kExclude, "type:fifo");
  const bool fifo_backed_up =
      BackupLegacy(fifo_source, fifo_archive, fifo_filter, &error);
  test_support::Check(fifo_backed_up, "exclude type:fifo skips the FIFO",
                      error);
  const bp::ArchiveReader fifo_reader;
  const bool fifo_restored =
      fifo_reader.Extract(fifo_archive, fifo_out, &error);
  test_support::Check(fifo_restored,
                      "restore of the FIFO-filtered archive succeeds", error);
  test_support::Check(!test_support::Exists(fifo_out + "/pipe"),
                      "restored tree contains no FIFO");
  content.clear();
  const bool fifo_kept =
      test_support::ReadFile(fifo_out + "/keep.txt", &content);
  test_support::Check(fifo_kept && content == "keep me\n",
                      "excluding the FIFO keeps the regular file");
}

}  // namespace

int main() {
  UnfilteredSymlinkStillFails();
  ExplicitExcludeSkipsSymlink();
  ExcludeTypeFileDoesNotSkipSymlink();
  ExcludeSizeDoesNotSkipSymlink();
  IncludeByUid();
  IncludeByUserName();
  IncludeByGroupName();
  IncludeWithOtherUidFiltersFilesOut();
  LegacyWireFormatIsUnchanged();
  SizeAndFifoRulesKeepWorking();

  const int status = test_support::Finish("legacy-filter");
  if (status == 0) {
    // 全绿时清掉 /tmp 下的 fixture；失败时保留现场，方便对着日志复查。
    test_support::RemoveTree(test_support::TempRoot());
  }
  return status;
}
