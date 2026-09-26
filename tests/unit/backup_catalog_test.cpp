// tests/unit/backup_catalog_test.cpp
//
// ArchiveReader::InspectHeader 与 BackupCatalog 的单元测试。
//
// 两种跑法共用一份测试代码：装了 GoogleTest 就用它（自己提供 main），
// 没装就用内置无依赖 harness——与 tests/unit/filter_rule_builder_test.cpp
// 是同一套写法。
//
// 全文件有一条核心契约：InspectHeader 只认全局 header，不等于完整校验。
// "全局 header 合法、后面被截断"的归档会被 InspectHeader 接受，而真正的
// 恢复必须拒绝它——这是本轮新增 API 最重要的边界，所以专门用契约测试钉住。
//
// 所有临时数据都建在 mkdtemp 出来的目录里（默认 /tmp），进程退出时整个删掉，
// 不碰真实 HOME。

#include "archive.h"
#include "archive_pipeline.h"
#include "backup_catalog.h"
#include "backup_engine.h"
#include "container_format.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

namespace bp = backupproject;
namespace fs = std::filesystem;

// L3_FORCE_NO_GTEST 由脚本在没有可用 GoogleTest 时定义。
// 只看 __has_include 是不够的：头文件装了但链接不了的时候，探测会失败，
// 这时必须真的退回内置 harness，而不是照样去 include 一个用不了的 gtest。
#if defined(__has_include)
#if __has_include(<gtest/gtest.h>) && !defined(L3_FORCE_NO_GTEST)
#include <gtest/gtest.h>
#define L3_HAVE_GTEST 1
#endif
#endif

#ifndef L3_HAVE_GTEST
namespace l3 {
struct Case {
  const char* name;
  void (*fn)();
};
inline std::vector<Case>& Registry() {
  static std::vector<Case> r;
  return r;
}
inline int& Failures() {
  static int f = 0;
  return f;
}
inline int& Checks() {
  static int c = 0;
  return c;
}
struct Reg {
  Reg(const char* n, void (*f)()) { Registry().push_back(Case{n, f}); }
};
}  // namespace l3
#define L3_TEST(suite, name)                                                  \
  static void suite##_##name();                                               \
  static ::l3::Reg l3_reg_##suite##_##name(#suite "." #name, suite##_##name); \
  static void suite##_##name()
#define L3_FAIL(expr)                                                        \
  do {                                                                       \
    ++::l3::Failures();                                                      \
    std::printf("    CHECK FAILED: %s (%s:%d)\n", expr, __FILE__, __LINE__); \
  } while (0)
#define EXPECT_TRUE(x)     \
  do {                     \
    ++::l3::Checks();      \
    if (!(x)) L3_FAIL(#x); \
  } while (0)
#define EXPECT_FALSE(x)          \
  do {                           \
    ++::l3::Checks();            \
    if (x) L3_FAIL("!(" #x ")"); \
  } while (0)
#define EXPECT_EQ(a, b)                       \
  do {                                        \
    ++::l3::Checks();                         \
    if (!((a) == (b))) L3_FAIL(#a " == " #b); \
  } while (0)
#define TEST(suite, name) L3_TEST(suite, name)
#endif
namespace {

// ---- 临时目录 ----

// 每次运行一个独立的临时根目录，进程退出时整个删掉。
// 默认落在 $TMPDIR 或 /tmp，不碰真实 HOME。
class WorkRoot {
 public:
  static const std::string& Path() {
    static const WorkRoot instance;
    return instance.path_;
  }

 private:
  WorkRoot() {
    const char* tmp = std::getenv("TMPDIR");
    std::string base =
        (tmp != nullptr && tmp[0] != '\0') ? std::string(tmp) : std::string("/tmp");
    while (!base.empty() && base.back() == '/') {
      base.pop_back();
    }
    if (base.empty()) {
      base = "/tmp";
    }
    std::string tmpl = base + "/backup_catalog_test_XXXXXX";
    std::vector<char> buffer(tmpl.begin(), tmpl.end());
    buffer.push_back('\0');
    const char* created = ::mkdtemp(buffer.data());
    path_ = (created != nullptr) ? std::string(created) : std::string();
  }

  ~WorkRoot() {
    if (!path_.empty()) {
      std::error_code ec;
      fs::remove_all(path_, ec);
    }
  }

  WorkRoot(const WorkRoot&) = delete;
  WorkRoot& operator=(const WorkRoot&) = delete;

  std::string path_;
};

// 每个用例一个独立子目录，用例之间互不影响。
std::string CaseDir(const std::string& name) {
  if (WorkRoot::Path().empty()) {
    std::fprintf(stderr, "FATAL: cannot create temporary work directory\n");
    std::exit(1);
  }
  const std::string dir = WorkRoot::Path() + "/" + name;
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  return dir;
}

// ---- 文件系统小工具 ----

bool MakeDir(const std::string& path) {
  std::error_code ec;
  fs::create_directories(path, ec);
  return fs::is_directory(path);
}

bool WriteFile(const std::string& path, const std::string& content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return false;
  }
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
  return out.good();
}

bool ReadFile(const std::string& path, std::string* content) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  *content = std::string((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
  return true;
}

bool FileExists(const std::string& path) {
  struct stat info;
  return ::lstat(path.c_str(), &info) == 0;
}

bool IsDirectory(const std::string& path) {
  struct stat info;
  return ::lstat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool IsSymlink(const std::string& path) {
  struct stat info;
  return ::lstat(path.c_str(), &info) == 0 && S_ISLNK(info.st_mode);
}

bool SetMtime(const std::string& path, std::int64_t sec) {
  struct timespec times[2];
  times[0].tv_sec = static_cast<time_t>(sec);
  times[0].tv_nsec = 0;
  times[1].tv_sec = static_cast<time_t>(sec);
  times[1].tv_nsec = 0;
  return ::utimensat(AT_FDCWD, path.c_str(), times, 0) == 0;
}

// 目录直接子项的名字，排序后返回。用来断言"这次调用没有创建任何文件"。
std::vector<std::string> DirectoryEntries(const std::string& directory) {
  std::vector<std::string> names;
  DIR* dir = ::opendir(directory.c_str());
  if (dir == nullptr) {
    return names;
  }
  while (struct dirent* entry = ::readdir(dir)) {
    const std::string name(entry->d_name);
    if (name != "." && name != "..") {
      names.push_back(name);
    }
  }
  ::closedir(dir);
  std::sort(names.begin(), names.end());
  return names;
}

bool Contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

// ---- 归档构造与小工具 ----

// 用真实 BackupEngine 造一个合法归档：测试面对的是产品代码真正写出的字节，
// 而不是测试自己拼出来的"看起来像归档"的文件。
bool MakeArchive(const std::string& source_dir, const std::string& archive_path,
                 std::string* error_message) {
  bp::BackupEngine engine;
  std::string message;
  if (engine.Backup(source_dir, archive_path, &message)) {
    return true;
  }
  if (error_message != nullptr) {
    *error_message = message;
  }
  return false;
}

// ---- v2 流水线字段用的小工具 ----

// 用真实 BackupEngine + BackupOptions 造一个 v2 container：测试面对的是产品
// 代码自己写出的 160 字节外层 header，而不是测试拼的字节。
bool MakePipelineArchive(const std::string& source_dir,
                         const std::string& archive_path,
                         const bp::BackupOptions& options,
                         std::string* error_message) {
  bp::BackupEngine engine;
  std::string message;
  if (engine.Backup(source_dir, archive_path, bp::Filter(), options,
                    &message)) {
    return true;
  }
  if (error_message != nullptr) {
    *error_message = message;
  }
  return false;
}

// v2 用例的源目录：比 MakeSampleSource 多一个几 KB 的文件，让压缩层真的有
// 内容可压（空流与极小流的边界另有 archive_pipeline 测试覆盖）。
std::string MakePipelineSource(const std::string& dir) {
  const std::string source = dir + "/source";
  if (!MakeDir(source)) {
    return std::string();
  }
  WriteFile(source + "/a.txt", "alpha\n");
  WriteFile(source + "/repeat.txt", std::string(4096, 'x') + "\n");
  return source;
}

// 四个字段一起断言：认得、有流水线、三种算法、恢复要不要密码。
void ExpectPipelineMethods(const bp::BackupRecord& record,
                           bp::PackMethod pack_method,
                           bp::CompressionMethod compression_method,
                           bp::EncryptionMethod encryption_method,
                           bool password_required) {
  EXPECT_TRUE(record.recognized_archive);
  EXPECT_TRUE(record.has_pipeline_methods);
  EXPECT_EQ(record.pack_method, pack_method);
  EXPECT_EQ(record.compression_method, compression_method);
  EXPECT_EQ(record.encryption_method, encryption_method);
  EXPECT_EQ(record.password_required, password_required);
}

// 全局 header 的字段偏移，与 docs/format/archive_v0.1.md 的偏移表一致。
// entry_count 直接用 archive.h 导出的 archive_v01::kEntryCountOffset。
constexpr std::size_t kMagicOffset = 0;
constexpr std::size_t kVersionOffset = 8;
constexpr std::size_t kFlagsOffset = 10;
constexpr std::size_t kHeaderSizeOffset = 12;

std::string U16LE(std::uint16_t value) {
  std::string out;
  out.push_back(static_cast<char>(value & 0xffu));
  out.push_back(static_cast<char>((value >> 8) & 0xffu));
  return out;
}

std::string U32LE(std::uint32_t value) {
  std::string out;
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<char>((value >> shift) & 0xffu));
  }
  return out;
}

std::string U64LE(std::uint64_t value) {
  std::string out;
  for (int shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<char>((value >> shift) & 0xffu));
  }
  return out;
}

// 在归档副本上按偏移改字节。
bool PatchBytes(const std::string& source, const std::string& target,
                std::size_t offset, const std::string& bytes) {
  std::string data;
  if (!ReadFile(source, &data)) {
    return false;
  }
  if (offset > data.size() || offset + bytes.size() > data.size()) {
    return false;
  }
  data.replace(offset, bytes.size(), bytes);
  return WriteFile(target, data);
}

bool TruncateTo(const std::string& source, const std::string& target,
                std::size_t length) {
  std::string data;
  if (!ReadFile(source, &data)) {
    return false;
  }
  if (length > data.size()) {
    return false;
  }
  data.resize(length);
  return WriteFile(target, data);
}

// 固定时间戳：2026-09-24 15:30:12 UTC。
// 脚本把 TZ 固定成 UTC，BuildArchivePath 又是 localtime_r，
// 所以这个 epoch 值对应的文件名是确定的。
constexpr std::int64_t kFixedTime = 1790263812;
constexpr const char* kFixedStamp = "20260924_153012";

// 造一个"源目录 + 两个文件"的样本，返回源目录路径。
std::string MakeSampleSource(const std::string& dir) {
  const std::string source = dir + "/source";
  if (!MakeDir(source)) {
    return std::string();
  }
  WriteFile(source + "/a.txt", "alpha\n");
  WriteFile(source + "/b.txt", "beta\n");
  return source;
}

}  // namespace

// ============================================================
// ArchiveReader::InspectHeader
// ============================================================

TEST(ArchiveInspect, AcceptsArchiveWrittenByBackupEngine) {
  const std::string dir = CaseDir("inspect_ok");
  const std::string source = MakeSampleSource(dir);
  EXPECT_FALSE(source.empty());
  const std::string archive = dir + "/sample.bak";
  std::string error;
  EXPECT_TRUE(MakeArchive(source, archive, &error));

  bp::ArchiveReader reader;
  bp::ArchiveSummary summary;
  error.clear();
  EXPECT_TRUE(reader.InspectHeader(archive, &summary, &error));
  EXPECT_TRUE(error.empty());
  EXPECT_EQ(summary.format_version, bp::archive_v01::kFormatVersion);
  EXPECT_EQ(summary.flags, bp::archive_v01::kFormatFlags);
  // "." + a.txt + b.txt
  EXPECT_EQ(summary.entry_count, static_cast<std::uint64_t>(3));
}

TEST(ArchiveInspect, ReportsEntryCountForEmptyDirectory) {
  const std::string dir = CaseDir("inspect_empty_dir");
  const std::string source = dir + "/empty";
  EXPECT_TRUE(MakeDir(source));
  const std::string archive = dir + "/empty.bak";
  std::string error;
  EXPECT_TRUE(MakeArchive(source, archive, &error));

  bp::ArchiveReader reader;
  bp::ArchiveSummary summary;
  error.clear();
  EXPECT_TRUE(reader.InspectHeader(archive, &summary, &error));
  // 空目录也要有第一条 "." 记录源目录自己的 mode / mtime。
  EXPECT_EQ(summary.entry_count, static_cast<std::uint64_t>(1));
}

TEST(ArchiveInspect, RejectsMissingFile) {
  const std::string dir = CaseDir("inspect_missing");
  bp::ArchiveReader reader;
  bp::ArchiveSummary summary;
  std::string error;
  EXPECT_FALSE(reader.InspectHeader(dir + "/nope.bak", &summary, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_TRUE(Contains(error, "nope.bak"));
}

TEST(ArchiveInspect, RejectsDirectory) {
  const std::string dir = CaseDir("inspect_directory");
  const std::string source = MakeSampleSource(dir);
  bp::ArchiveReader reader;
  bp::ArchiveSummary summary;
  std::string error;
  EXPECT_FALSE(reader.InspectHeader(source, &summary, &error));
  EXPECT_TRUE(Contains(error, "not a regular file"));
}

TEST(ArchiveInspect, RejectsSymlink) {
  const std::string dir = CaseDir("inspect_symlink");
  const std::string source = MakeSampleSource(dir);
  const std::string archive = dir + "/real.bak";
  std::string error;
  EXPECT_TRUE(MakeArchive(source, archive, &error));
  const std::string link = dir + "/link.bak";
  EXPECT_EQ(::symlink(archive.c_str(), link.c_str()), 0);

  bp::ArchiveReader reader;
  bp::ArchiveSummary summary;
  error.clear();
  // lstat 语义：软链接就是软链接，不顺着它去读目标。
  EXPECT_FALSE(reader.InspectHeader(link, &summary, &error));
  EXPECT_TRUE(Contains(error, "not a regular file"));
}

TEST(ArchiveInspect, RejectsEmptyFile) {
  const std::string dir = CaseDir("inspect_empty_file");
  const std::string archive = dir + "/empty.bak";
  EXPECT_TRUE(WriteFile(archive, ""));
  bp::ArchiveReader reader;
  bp::ArchiveSummary summary;
  std::string error;
  EXPECT_FALSE(reader.InspectHeader(archive, &summary, &error));
  EXPECT_TRUE(Contains(error, "Truncated archive header"));
}

TEST(ArchiveInspect, RejectsShortHeader) {
  const std::string dir = CaseDir("inspect_short_header");
  const std::string archive = dir + "/short.bak";
  // 比 24 字节的全局 header 少，连 magic 都读不满。
  // 11 个字节：比 24 字节的全局 header 短，字段读不满。
  const std::string partial("BKPARCH\0\1\0\0", 11);
  EXPECT_TRUE(WriteFile(archive, partial));
  bp::ArchiveReader reader;
  bp::ArchiveSummary summary;
  std::string error;
  EXPECT_FALSE(reader.InspectHeader(archive, &summary, &error));
  EXPECT_TRUE(Contains(error, "Truncated archive header"));
}

TEST(ArchiveInspect, RejectsWrongMagic) {
  const std::string dir = CaseDir("inspect_wrong_magic");
  const std::string source = MakeSampleSource(dir);
  const std::string archive = dir + "/good.bak";
  std::string error;
  EXPECT_TRUE(MakeArchive(source, archive, &error));
  const std::string patched = dir + "/bad_magic.bak";
  EXPECT_TRUE(PatchBytes(archive, patched, kMagicOffset, std::string("NOTARCH\0")));

  bp::ArchiveReader reader;
  bp::ArchiveSummary summary;
  error.clear();
  EXPECT_FALSE(reader.InspectHeader(patched, &summary, &error));
  EXPECT_TRUE(Contains(error, "Invalid archive magic"));
}

TEST(ArchiveInspect, RejectsWrongVersion) {
  const std::string dir = CaseDir("inspect_wrong_version");
  const std::string source = MakeSampleSource(dir);
  const std::string archive = dir + "/good.bak";
  std::string error;
  EXPECT_TRUE(MakeArchive(source, archive, &error));
  const std::string patched = dir + "/bad_version.bak";
  EXPECT_TRUE(PatchBytes(archive, patched, kVersionOffset, U16LE(2)));

  bp::ArchiveReader reader;
  bp::ArchiveSummary summary;
  error.clear();
  EXPECT_FALSE(reader.InspectHeader(patched, &summary, &error));
  EXPECT_TRUE(Contains(error, "Unsupported archive version"));
}

TEST(ArchiveInspect, RejectsWrongFlags) {
  const std::string dir = CaseDir("inspect_wrong_flags");
  const std::string source = MakeSampleSource(dir);
  const std::string archive = dir + "/good.bak";
  std::string error;
  EXPECT_TRUE(MakeArchive(source, archive, &error));
  const std::string patched = dir + "/bad_flags.bak";
  EXPECT_TRUE(PatchBytes(archive, patched, kFlagsOffset, U16LE(1)));

  bp::ArchiveReader reader;
  bp::ArchiveSummary summary;
  error.clear();
  EXPECT_FALSE(reader.InspectHeader(patched, &summary, &error));
  EXPECT_TRUE(Contains(error, "Unsupported archive flags"));
}

TEST(ArchiveInspect, RejectsWrongHeaderSize) {
  const std::string dir = CaseDir("inspect_wrong_header_size");
  const std::string source = MakeSampleSource(dir);
  const std::string archive = dir + "/good.bak";
  std::string error;
  EXPECT_TRUE(MakeArchive(source, archive, &error));

  bp::ArchiveReader reader;
  bp::ArchiveSummary summary;
  // 比真实值大和比真实值小都要拒绝：header_size 是白名单式的判断。
  const std::string bigger = dir + "/header_bigger.bak";
  EXPECT_TRUE(PatchBytes(archive, bigger, kHeaderSizeOffset, U32LE(32)));
  error.clear();
  EXPECT_FALSE(reader.InspectHeader(bigger, &summary, &error));
  EXPECT_TRUE(Contains(error, "Invalid archive header size"));

  const std::string smaller = dir + "/header_smaller.bak";
  EXPECT_TRUE(PatchBytes(archive, smaller, kHeaderSizeOffset, U32LE(16)));
  error.clear();
  EXPECT_FALSE(reader.InspectHeader(smaller, &summary, &error));
  EXPECT_TRUE(Contains(error, "Invalid archive header size"));
}

TEST(ArchiveInspect, RejectsNullSummaryAndEmptyPath) {
  const std::string dir = CaseDir("inspect_bad_arguments");
  const std::string source = MakeSampleSource(dir);
  const std::string archive = dir + "/good.bak";
  std::string error;
  EXPECT_TRUE(MakeArchive(source, archive, &error));

  bp::ArchiveReader reader;
  bp::ArchiveSummary summary;
  error.clear();
  EXPECT_FALSE(reader.InspectHeader(archive, nullptr, &error));
  EXPECT_FALSE(error.empty());

  error.clear();
  EXPECT_FALSE(reader.InspectHeader("", &summary, &error));
  EXPECT_FALSE(error.empty());
}

// 本轮最重要的契约测试：InspectHeader 不是完整校验。
TEST(ArchiveInspect, SucceedsOnTruncatedPayloadWhileExtractFails) {
  const std::string dir = CaseDir("inspect_truncated_payload");
  const std::string source = MakeSampleSource(dir);
  const std::string archive = dir + "/good.bak";
  std::string error;
  EXPECT_TRUE(MakeArchive(source, archive, &error));

  bp::ArchiveReader reader;
  bp::ArchiveSummary good;
  error.clear();
  EXPECT_TRUE(reader.InspectHeader(archive, &good, &error));
  EXPECT_EQ(good.entry_count, static_cast<std::uint64_t>(3));

  // 只留全局 header(24B) + 第一条 entry header(32B)：header 完好，正文全没。
  const std::string truncated = dir + "/truncated.bak";
  EXPECT_TRUE(TruncateTo(archive, truncated,
                         bp::archive_v01::kGlobalHeaderSize +
                             bp::archive_v01::kEntryHeaderSize));

  bp::ArchiveSummary damaged;
  error.clear();
  // InspectHeader 必须接受它——这正是"只看 header"的定义。
  EXPECT_TRUE(reader.InspectHeader(truncated, &damaged, &error));
  EXPECT_TRUE(error.empty());
  EXPECT_EQ(damaged.format_version, good.format_version);
  EXPECT_EQ(damaged.entry_count, good.entry_count);

  // 而真正的恢复必须拒绝它，并且不在磁盘上留下任何东西。
  const std::string destination = dir + "/restored";
  bp::BackupEngine engine;
  std::string restore_error;
  EXPECT_FALSE(engine.Restore(truncated, destination, &restore_error));
  EXPECT_FALSE(restore_error.empty());
  EXPECT_FALSE(FileExists(destination));
}

// 同一个边界的另一种形态：header 里的 entry_count 是"声明"，不是"事实"。
TEST(ArchiveInspect, TrustsHeaderEntryCountWithoutReadingEntries) {
  const std::string dir = CaseDir("inspect_claimed_entry_count");
  const std::string source = MakeSampleSource(dir);
  const std::string archive = dir + "/good.bak";
  std::string error;
  EXPECT_TRUE(MakeArchive(source, archive, &error));

  const std::string patched = dir + "/claimed.bak";
  EXPECT_TRUE(PatchBytes(archive, patched, bp::archive_v01::kEntryCountOffset,
                         U64LE(999999)));

  bp::ArchiveReader reader;
  bp::ArchiveSummary summary;
  error.clear();
  EXPECT_TRUE(reader.InspectHeader(patched, &summary, &error));
  EXPECT_EQ(summary.entry_count, static_cast<std::uint64_t>(999999));

  const std::string destination = dir + "/restored";
  bp::BackupEngine engine;
  std::string restore_error;
  EXPECT_FALSE(engine.Restore(patched, destination, &restore_error));
  EXPECT_FALSE(FileExists(destination));
}

// ============================================================
// BackupCatalog::EnsureRepository
// ============================================================

TEST(CatalogEnsureRepository, CreatesMissingDirectoryTree) {
  const std::string dir = CaseDir("catalog_ensure_create");
  const std::string repository = dir + "/a/b/c/repo";
  bp::BackupCatalog catalog;
  std::string error;
  EXPECT_TRUE(catalog.EnsureRepository(repository, &error));
  EXPECT_TRUE(error.empty());
  EXPECT_TRUE(IsDirectory(repository));
}

TEST(CatalogEnsureRepository, AcceptsExistingDirectory) {
  const std::string dir = CaseDir("catalog_ensure_existing");
  const std::string repository = dir + "/repo";
  EXPECT_TRUE(MakeDir(repository));
  bp::BackupCatalog catalog;
  std::string error;
  EXPECT_TRUE(catalog.EnsureRepository(repository, &error));
  EXPECT_TRUE(error.empty());
}

TEST(CatalogEnsureRepository, RejectsExistingRegularFile) {
  const std::string dir = CaseDir("catalog_ensure_file");
  const std::string repository = dir + "/repo";
  EXPECT_TRUE(WriteFile(repository, "not a directory"));
  bp::BackupCatalog catalog;
  std::string error;
  EXPECT_FALSE(catalog.EnsureRepository(repository, &error));
  EXPECT_TRUE(Contains(error, "not a directory"));
  // 已有的东西一个字节都不能动。
  std::string content;
  EXPECT_TRUE(ReadFile(repository, &content));
  EXPECT_EQ(content, std::string("not a directory"));
}

TEST(CatalogEnsureRepository, RejectsEmptyPath) {
  const std::string dir = CaseDir("catalog_ensure_empty");
  bp::BackupCatalog catalog;
  std::string error;
  EXPECT_FALSE(catalog.EnsureRepository("", &error));
  EXPECT_FALSE(error.empty());
}

TEST(CatalogEnsureRepository, RejectsSymlinkToDirectory) {
  const std::string dir = CaseDir("catalog_ensure_symlink");
  const std::string real = dir + "/real";
  EXPECT_TRUE(MakeDir(real));
  const std::string link = dir + "/link";
  EXPECT_EQ(::symlink(real.c_str(), link.c_str()), 0);
  bp::BackupCatalog catalog;
  std::string error;
  // 本类不跟随软链接，仓库根自己也不例外。
  EXPECT_FALSE(catalog.EnsureRepository(link, &error));
  EXPECT_FALSE(error.empty());
}

// ============================================================
// repository 根本身的安全性
// ============================================================

// 只查文件名是不够的：POSIX 会跟随路径里的中间组件。仓库根是软链接时
// unlink("repo/a.bak") 真正作用在链接指向的那个目录上，所以四个入口都必须
// 拒绝，而不是只靠"最后一段用 lstat"来兜。
TEST(CatalogRepositoryRoot, RejectsSymlinkRootEverywhere) {
  const std::string dir = CaseDir("catalog_symlink_root");
  const std::string real = dir + "/real_repo";
  const std::string source = MakeSampleSource(dir);
  bp::BackupCatalog catalog;
  std::string error;
  EXPECT_TRUE(catalog.EnsureRepository(real, &error));

  // 真实仓库里放两份文件：一份带已知字节的普通文件，一份真归档。
  const std::string payload = "known payload bytes that must survive";
  EXPECT_TRUE(WriteFile(real + "/keep.bak", payload));
  EXPECT_TRUE(MakeArchive(source, real + "/good.bak", &error));
  std::string good_before;
  EXPECT_TRUE(ReadFile(real + "/good.bak", &good_before));

  // 对照组：走真实路径时一切正常，说明 fixture 本身是个合法仓库。
  std::vector<bp::BackupRecord> records;
  error.clear();
  EXPECT_TRUE(catalog.List(real, &records, &error));
  EXPECT_EQ(records.size(), static_cast<std::size_t>(2));
  std::string resolved;
  error.clear();
  EXPECT_TRUE(catalog.Resolve(real, "keep.bak", &resolved, &error));
  EXPECT_EQ(resolved, real + "/keep.bak");

  const std::string link = dir + "/repo_link";
  EXPECT_EQ(::symlink(real.c_str(), link.c_str()), 0);

  // 四个入口一律拒绝，并且要指名 symbolic link。
  records.clear();
  error.clear();
  EXPECT_FALSE(catalog.List(link, &records, &error));
  EXPECT_TRUE(Contains(error, "symbolic link"));

  std::string path;
  error.clear();
  EXPECT_FALSE(catalog.Resolve(link, "keep.bak", &path, &error));
  EXPECT_TRUE(Contains(error, "symbolic link"));
  EXPECT_TRUE(path.empty());

  error.clear();
  EXPECT_FALSE(catalog.Delete(link, "keep.bak", &error));
  EXPECT_TRUE(Contains(error, "symbolic link"));

  error.clear();
  EXPECT_FALSE(
      catalog.BuildArchivePath(link, source, kFixedTime, &path, &error));
  EXPECT_TRUE(Contains(error, "symbolic link"));

  // 攻击回归：真文件必须逐字节原封不动。
  std::string content;
  EXPECT_TRUE(ReadFile(real + "/keep.bak", &content));
  EXPECT_EQ(content, payload);
  EXPECT_TRUE(FileExists(real + "/good.bak"));
  EXPECT_TRUE(ReadFile(real + "/good.bak", &content));
  EXPECT_EQ(content, good_before);

  // 真实路径依旧可用：被拒的只是软链接这一条入口。
  records.clear();
  error.clear();
  EXPECT_TRUE(catalog.List(real, &records, &error));
  EXPECT_EQ(records.size(), static_cast<std::size_t>(2));
}


// ============================================================
// BackupCatalog::List
// ============================================================

TEST(CatalogList, RejectsMissingRepository) {
  const std::string dir = CaseDir("catalog_list_missing");
  bp::BackupCatalog catalog;
  std::vector<bp::BackupRecord> records;
  std::string error;
  EXPECT_FALSE(catalog.List(dir + "/nope", &records, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_TRUE(records.empty());
}

TEST(CatalogList, RejectsRepositoryThatIsAFile) {
  const std::string dir = CaseDir("catalog_list_file_repo");
  const std::string repository = dir + "/repo";
  EXPECT_TRUE(WriteFile(repository, "not a directory"));
  bp::BackupCatalog catalog;
  std::vector<bp::BackupRecord> records;
  std::string error;
  EXPECT_FALSE(catalog.List(repository, &records, &error));
  EXPECT_TRUE(Contains(error, "not a directory"));
}

TEST(CatalogList, ReturnsEmptyForEmptyRepository) {
  const std::string dir = CaseDir("catalog_list_empty");
  const std::string repository = dir + "/repo";
  bp::BackupCatalog catalog;
  std::string error;
  EXPECT_TRUE(catalog.EnsureRepository(repository, &error));
  std::vector<bp::BackupRecord> records;
  error.clear();
  EXPECT_TRUE(catalog.List(repository, &records, &error));
  EXPECT_TRUE(records.empty());
}

TEST(CatalogList, RejectsNullRecords) {
  const std::string dir = CaseDir("catalog_list_null");
  const std::string repository = dir + "/repo";
  bp::BackupCatalog catalog;
  std::string error;
  EXPECT_TRUE(catalog.EnsureRepository(repository, &error));
  error.clear();
  EXPECT_FALSE(catalog.List(repository, nullptr, &error));
  EXPECT_FALSE(error.empty());
}

TEST(CatalogList, DescribesOneValidArchive) {
  const std::string dir = CaseDir("catalog_list_one");
  const std::string repository = dir + "/repo";
  const std::string source = MakeSampleSource(dir);
  bp::BackupCatalog catalog;
  std::string error;
  EXPECT_TRUE(catalog.EnsureRepository(repository, &error));
  const std::string archive = repository + "/one.bak";
  EXPECT_TRUE(MakeArchive(source, archive, &error));
  EXPECT_TRUE(SetMtime(archive, 1700000000));

  std::vector<bp::BackupRecord> records;
  error.clear();
  EXPECT_TRUE(catalog.List(repository, &records, &error));
  EXPECT_TRUE(error.empty());
  EXPECT_EQ(records.size(), static_cast<std::size_t>(1));
  if (records.size() == 1) {
    EXPECT_EQ(records[0].file_name, std::string("one.bak"));
    EXPECT_EQ(records[0].archive_path, repository + "/one.bak");
    EXPECT_TRUE(records[0].recognized_archive);
    EXPECT_EQ(records[0].format_version, bp::archive_v01::kFormatVersion);
    EXPECT_EQ(records[0].entry_count, static_cast<std::uint64_t>(3));
    EXPECT_TRUE(records[0].diagnostic.empty());
    EXPECT_EQ(records[0].modified_time_sec, static_cast<std::int64_t>(1700000000));
    EXPECT_EQ(records[0].archive_size,
              static_cast<std::uint64_t>(fs::file_size(archive)));
  }
}

TEST(CatalogList, SortsNewestFirstThenByName) {
  const std::string dir = CaseDir("catalog_list_order");
  const std::string repository = dir + "/repo";
  const std::string source = MakeSampleSource(dir);
  bp::BackupCatalog catalog;
  std::string error;
  EXPECT_TRUE(catalog.EnsureRepository(repository, &error));
  EXPECT_TRUE(MakeArchive(source, repository + "/a.bak", &error));
  EXPECT_TRUE(MakeArchive(source, repository + "/b.bak", &error));
  EXPECT_TRUE(MakeArchive(source, repository + "/c.bak", &error));
  EXPECT_TRUE(SetMtime(repository + "/a.bak", 1000));
  EXPECT_TRUE(SetMtime(repository + "/b.bak", 3000));
  EXPECT_TRUE(SetMtime(repository + "/c.bak", 2000));

  std::vector<bp::BackupRecord> records;
  error.clear();
  EXPECT_TRUE(catalog.List(repository, &records, &error));
  EXPECT_EQ(records.size(), static_cast<std::size_t>(3));
  if (records.size() == 3) {
    EXPECT_EQ(records[0].file_name, std::string("b.bak"));
    EXPECT_EQ(records[1].file_name, std::string("c.bak"));
    EXPECT_EQ(records[2].file_name, std::string("a.bak"));
  }

  // 时间完全相同的时候按文件名升序，保证顺序是确定的。
  EXPECT_TRUE(SetMtime(repository + "/a.bak", 5000));
  EXPECT_TRUE(SetMtime(repository + "/b.bak", 5000));
  EXPECT_TRUE(SetMtime(repository + "/c.bak", 5000));
  records.clear();
  error.clear();
  EXPECT_TRUE(catalog.List(repository, &records, &error));
  EXPECT_EQ(records.size(), static_cast<std::size_t>(3));
  if (records.size() == 3) {
    EXPECT_EQ(records[0].file_name, std::string("a.bak"));
    EXPECT_EQ(records[1].file_name, std::string("b.bak"));
    EXPECT_EQ(records[2].file_name, std::string("c.bak"));
  }
}

TEST(CatalogList, KeepsCorruptedArchivesWithDiagnostic) {
  const std::string dir = CaseDir("catalog_list_corrupted");
  const std::string repository = dir + "/repo";
  const std::string source = MakeSampleSource(dir);
  bp::BackupCatalog catalog;
  std::string error;
  EXPECT_TRUE(catalog.EnsureRepository(repository, &error));
  const std::string good = dir + "/good.bak";
  EXPECT_TRUE(MakeArchive(source, good, &error));

  // ① 完全不是归档
  EXPECT_TRUE(WriteFile(repository + "/garbage.bak",
                        "this file is definitely not a backup archive."));
  // ② 只有全局 header 的截断件：header 合法，所以 InspectHeader 认得它
  EXPECT_TRUE(TruncateTo(good, repository + "/headonly.bak",
                         bp::archive_v01::kGlobalHeaderSize));
  // ③ header 合法但版本号不认识
  EXPECT_TRUE(PatchBytes(good, repository + "/oldver.bak", kVersionOffset,
                         U16LE(7)));

  std::vector<bp::BackupRecord> records;
  error.clear();
  // 坏文件不能让整张列表失败。
  EXPECT_TRUE(catalog.List(repository, &records, &error));
  EXPECT_TRUE(error.empty());
  EXPECT_EQ(records.size(), static_cast<std::size_t>(3));

  for (const bp::BackupRecord& record : records) {
    EXPECT_TRUE(record.archive_size > 0);
    if (record.file_name == "garbage.bak") {
      EXPECT_FALSE(record.recognized_archive);
      EXPECT_TRUE(Contains(record.diagnostic, "Invalid archive magic"));
    } else if (record.file_name == "headonly.bak") {
      // 只有 24 字节也照样"认得"——这正是 InspectHeader 的边界，
      // 它不代表归档可以用。
      EXPECT_TRUE(record.recognized_archive);
      EXPECT_EQ(record.entry_count, static_cast<std::uint64_t>(3));
      EXPECT_TRUE(record.diagnostic.empty());
    } else if (record.file_name == "oldver.bak") {
      EXPECT_FALSE(record.recognized_archive);
      EXPECT_TRUE(Contains(record.diagnostic, "Unsupported archive version"));
    } else {
      EXPECT_EQ(record.file_name, std::string("<unexpected>"));
    }
  }
}

TEST(CatalogList, IgnoresNonBackupEntries) {
  const std::string dir = CaseDir("catalog_list_filters");
  const std::string repository = dir + "/repo";
  const std::string source = MakeSampleSource(dir);
  bp::BackupCatalog catalog;
  std::string error;
  EXPECT_TRUE(catalog.EnsureRepository(repository, &error));
  EXPECT_TRUE(MakeArchive(source, repository + "/keep.bak", &error));

  EXPECT_TRUE(WriteFile(repository + "/notes.txt", "not a backup"));
  EXPECT_TRUE(WriteFile(repository + "/UPPER.BAK", "not a backup"));
  EXPECT_TRUE(WriteFile(repository + "/noext", "not a backup"));
  // 目录
  EXPECT_TRUE(MakeDir(repository + "/folder.bak"));
  // 不递归：子目录里的 .bak 不算
  EXPECT_TRUE(MakeDir(repository + "/sub"));
  EXPECT_TRUE(MakeArchive(source, repository + "/sub/inner.bak", &error));
  // 软链接：即使指向合法归档也不算
  EXPECT_EQ(::symlink((repository + "/keep.bak").c_str(),
                      (repository + "/link.bak").c_str()),
            0);

  std::vector<bp::BackupRecord> records;
  error.clear();
  EXPECT_TRUE(catalog.List(repository, &records, &error));
  EXPECT_EQ(records.size(), static_cast<std::size_t>(1));
  if (records.size() == 1) {
    EXPECT_EQ(records[0].file_name, std::string("keep.bak"));
    EXPECT_TRUE(records[0].recognized_archive);
  }
}

TEST(CatalogList, ClearsRecordsWhenItFails) {
  const std::string dir = CaseDir("catalog_list_clears");
  bp::BackupCatalog catalog;
  std::vector<bp::BackupRecord> records;
  records.resize(3);
  std::string error;
  EXPECT_FALSE(catalog.List(dir + "/nope", &records, &error));
  EXPECT_TRUE(records.empty());
}

// ============================================================
// BackupCatalog::List：v2 流水线字段
// ============================================================
//
// 界面要能在不知道密码的前提下说清每个备份用了哪三种算法、恢复要不要密码。
// 这四个字段读的是 v2 container 的 160 字节外层 header，属于"声明"：归档是否
// 完整、能不能恢复仍然由恢复路径判断，列表不做这个承诺。

TEST(CatalogListPipelineMethods, ReportsLegacyV01WithoutPipeline) {
  const std::string dir = CaseDir("catalog_methods_legacy");
  const std::string repository = dir + "/repo";
  const std::string source = MakeSampleSource(dir);
  bp::BackupCatalog catalog;
  std::string error;
  EXPECT_TRUE(catalog.EnsureRepository(repository, &error));
  // 不传 BackupOptions：走的就是 legacy v0.1 写入路径。
  EXPECT_TRUE(MakeArchive(source, repository + "/legacy.bak", &error));

  std::vector<bp::BackupRecord> records;
  error.clear();
  EXPECT_TRUE(catalog.List(repository, &records, &error));
  EXPECT_TRUE(error.empty());
  EXPECT_EQ(records.size(), static_cast<std::size_t>(1));
  if (records.size() == 1) {
    EXPECT_TRUE(records[0].recognized_archive);
    EXPECT_EQ(records[0].format_version, bp::archive_v01::kFormatVersion);
    EXPECT_FALSE(records[0].has_pipeline_methods);
    EXPECT_FALSE(records[0].password_required);
    // v0.1 没有流水线：三个算法字段保持默认值。
    EXPECT_EQ(records[0].pack_method, bp::PackMethod::kMyPack);
    EXPECT_EQ(records[0].compression_method, bp::CompressionMethod::kNone);
    EXPECT_EQ(records[0].encryption_method, bp::EncryptionMethod::kNone);
  }
}

TEST(CatalogListPipelineMethods, ReportsV2MyPackWithoutCompression) {
  const std::string dir = CaseDir("catalog_methods_mypack");
  const std::string repository = dir + "/repo";
  const std::string source = MakePipelineSource(dir);
  bp::BackupCatalog catalog;
  std::string error;
  EXPECT_TRUE(catalog.EnsureRepository(repository, &error));

  bp::BackupOptions options;
  options.pack_method = bp::PackMethod::kMyPack;
  options.compression_method = bp::CompressionMethod::kNone;
  options.encryption_method = bp::EncryptionMethod::kNone;
  EXPECT_TRUE(
      MakePipelineArchive(source, repository + "/plain.bak", options, &error));

  std::vector<bp::BackupRecord> records;
  error.clear();
  EXPECT_TRUE(catalog.List(repository, &records, &error));
  EXPECT_TRUE(error.empty());
  EXPECT_EQ(records.size(), static_cast<std::size_t>(1));
  if (records.size() == 1) {
    // format_version 来自外层 container header，与 v0.1 的 1 区分开。
    EXPECT_EQ(records[0].format_version,
              static_cast<std::uint16_t>(bp::container_v2::kVersion));
    EXPECT_TRUE(records[0].entry_count > 0);
    EXPECT_TRUE(records[0].diagnostic.empty());
    ExpectPipelineMethods(records[0], bp::PackMethod::kMyPack,
                          bp::CompressionMethod::kNone,
                          bp::EncryptionMethod::kNone,
                          /*password_required=*/false);
  }
}

TEST(CatalogListPipelineMethods, ReportsV2UstarWithHuffman) {
  const std::string dir = CaseDir("catalog_methods_ustar");
  const std::string repository = dir + "/repo";
  const std::string source = MakePipelineSource(dir);
  bp::BackupCatalog catalog;
  std::string error;
  EXPECT_TRUE(catalog.EnsureRepository(repository, &error));

  bp::BackupOptions options;
  options.pack_method = bp::PackMethod::kUstar;
  options.compression_method = bp::CompressionMethod::kHuffman;
  options.encryption_method = bp::EncryptionMethod::kNone;
  EXPECT_TRUE(MakePipelineArchive(source, repository + "/huffman.bak", options,
                                  &error));

  std::vector<bp::BackupRecord> records;
  error.clear();
  EXPECT_TRUE(catalog.List(repository, &records, &error));
  EXPECT_TRUE(error.empty());
  EXPECT_EQ(records.size(), static_cast<std::size_t>(1));
  if (records.size() == 1) {
    ExpectPipelineMethods(records[0], bp::PackMethod::kUstar,
                          bp::CompressionMethod::kHuffman,
                          bp::EncryptionMethod::kNone,
                          /*password_required=*/false);
  }
}

TEST(CatalogListPipelineMethods, ReportsV2FastUstarWithLzssAndAes) {
  const std::string dir = CaseDir("catalog_methods_aes");
  const std::string repository = dir + "/repo";
  const std::string source = MakePipelineSource(dir);
  bp::BackupCatalog catalog;
  std::string error;
  EXPECT_TRUE(catalog.EnsureRepository(repository, &error));

  bp::BackupOptions options;
  options.pack_method = bp::PackMethod::kFastUstar;
  options.compression_method = bp::CompressionMethod::kLzssHuffman;
  options.encryption_method = bp::EncryptionMethod::kAes256CtrHmacSha256;
  options.password = "catalog pipeline password";
  EXPECT_TRUE(
      MakePipelineArchive(source, repository + "/secret.bak", options, &error));

  // 列目录全程没有给过密码：需要密码这件事本身就是从外层 header 读出来的。
  std::vector<bp::BackupRecord> records;
  error.clear();
  EXPECT_TRUE(catalog.List(repository, &records, &error));
  EXPECT_TRUE(error.empty());
  EXPECT_EQ(records.size(), static_cast<std::size_t>(1));
  if (records.size() == 1) {
    ExpectPipelineMethods(records[0], bp::PackMethod::kFastUstar,
                          bp::CompressionMethod::kLzssHuffman,
                          bp::EncryptionMethod::kAes256CtrHmacSha256,
                          /*password_required=*/true);
  }
}

// 认不出来的 .bak 依旧：留在列表里、recognized=false、diagnostic 非空、删得掉。
TEST(CatalogListPipelineMethods, KeepsBadArchivesListedAndDeletable) {
  const std::string dir = CaseDir("catalog_methods_bad");
  const std::string repository = dir + "/repo";
  const std::string source = MakeSampleSource(dir);
  bp::BackupCatalog catalog;
  std::string error;
  EXPECT_TRUE(catalog.EnsureRepository(repository, &error));

  // ① 完全不是归档。
  EXPECT_TRUE(WriteFile(repository + "/garbage.bak",
                        "this file is definitely not a backup archive."));
  // ② magic 是 BKPARCH 但全局 header 被截断：DetectKind 只看前 8 字节，
  //    所以 IdentifyArchiveFile 会说"这是 legacy"，而 InspectHeader 读不出来。
  //    这种文件必须仍然是 recognized=false + 非空 diagnostic。
  const std::string good = dir + "/good.bak";
  EXPECT_TRUE(MakeArchive(source, good, &error));
  EXPECT_TRUE(TruncateTo(good, repository + "/headless.bak", 16));

  std::vector<bp::BackupRecord> records;
  error.clear();
  EXPECT_TRUE(catalog.List(repository, &records, &error));
  EXPECT_TRUE(error.empty());
  EXPECT_EQ(records.size(), static_cast<std::size_t>(2));
  for (const bp::BackupRecord& record : records) {
    EXPECT_FALSE(record.recognized_archive);
    EXPECT_FALSE(record.diagnostic.empty());
    // 认不出来就没有流水线可报，也不该顺手说"需要密码"。
    EXPECT_FALSE(record.has_pipeline_methods);
    EXPECT_FALSE(record.password_required);
  }

  // 坏归档照样能删：列表里看得见，也清得掉。
  error.clear();
  EXPECT_TRUE(catalog.Delete(repository, "garbage.bak", &error));
  EXPECT_TRUE(error.empty());
  EXPECT_FALSE(FileExists(repository + "/garbage.bak"));

  error.clear();
  EXPECT_TRUE(catalog.Delete(repository, "headless.bak", &error));
  EXPECT_TRUE(error.empty());
  EXPECT_FALSE(FileExists(repository + "/headless.bak"));

  records.clear();
  error.clear();
  EXPECT_TRUE(catalog.List(repository, &records, &error));
  EXPECT_TRUE(records.empty());
}

// ============================================================
// BackupCatalog::BuildArchivePath
// ============================================================

TEST(CatalogBuildArchivePath, BuildsReadableNameFromSourceDirectory) {
  const std::string dir = CaseDir("catalog_build_plain");
  const std::string repository = dir + "/repo";
  bp::BackupCatalog catalog;
  std::string error;
  EXPECT_TRUE(catalog.EnsureRepository(repository, &error));

  std::string path;
  error.clear();
  EXPECT_TRUE(catalog.BuildArchivePath(repository, "/home/a/Documents",
                                       kFixedTime, &path, &error));
  EXPECT_TRUE(error.empty());
  EXPECT_EQ(path, repository + "/Documents_" + kFixedStamp + ".bak");

  // 相对路径同样只看最后一段。
  error.clear();
  EXPECT_TRUE(catalog.BuildArchivePath(repository, "Documents", kFixedTime,
                                       &path, &error));
  EXPECT_EQ(path, repository + "/Documents_" + kFixedStamp + ".bak");
}

TEST(CatalogBuildArchivePath, NormalizesTrailingSlashes) {
  const std::string dir = CaseDir("catalog_build_slashes");
  const std::string repository = dir + "/repo";
  bp::BackupCatalog catalog;
  std::string error;
  EXPECT_TRUE(catalog.EnsureRepository(repository, &error));

  const std::string expected = repository + "/Documents_" + kFixedStamp + ".bak";
  std::string path;
  error.clear();
  EXPECT_TRUE(catalog.BuildArchivePath(repository, "/home/a/Documents/",
                                       kFixedTime, &path, &error));
  EXPECT_EQ(path, expected);

  error.clear();
  EXPECT_TRUE(catalog.BuildArchivePath(repository, "/home/a/Documents///",
                                       kFixedTime, &path, &error));
  EXPECT_EQ(path, expected);

  // repository 自己的尾斜杠也要规整掉，不能出现 "repo//x.bak"。
  error.clear();
  EXPECT_TRUE(catalog.BuildArchivePath(repository + "/", "/home/a/Documents",
                                       kFixedTime, &path, &error));
  EXPECT_EQ(path, expected);
}

TEST(CatalogBuildArchivePath, HandlesUtf8AndSpaces) {
  const std::string dir = CaseDir("catalog_build_utf8");
  const std::string repository = dir + "/repo";
  bp::BackupCatalog catalog;
  std::string error;
  EXPECT_TRUE(catalog.EnsureRepository(repository, &error));

  std::string path;
  error.clear();
  EXPECT_TRUE(catalog.BuildArchivePath(repository, "/home/a/项目", kFixedTime,
                                       &path, &error));
  EXPECT_EQ(path, repository + "/项目_" + kFixedStamp + ".bak");

  error.clear();
  EXPECT_TRUE(catalog.BuildArchivePath(repository, "/home/a/My Documents",
                                       kFixedTime, &path, &error));
  EXPECT_EQ(path, repository + "/My Documents_" + kFixedStamp + ".bak");
}

TEST(CatalogBuildArchivePath, FallsBackForRootAndDotPaths) {
  const std::string dir = CaseDir("catalog_build_fallback");
  const std::string repository = dir + "/repo";
  bp::BackupCatalog catalog;
  std::string error;
  EXPECT_TRUE(catalog.EnsureRepository(repository, &error));

  const std::string expected = repository + "/backup_" + kFixedStamp + ".bak";
  std::string path;
  error.clear();
  EXPECT_TRUE(
      catalog.BuildArchivePath(repository, "/", kFixedTime, &path, &error));
  EXPECT_EQ(path, expected);

  error.clear();
  EXPECT_TRUE(catalog.BuildArchivePath(repository, "/home/a/..", kFixedTime,
                                       &path, &error));
  EXPECT_EQ(path, expected);

  error.clear();
  EXPECT_TRUE(catalog.BuildArchivePath(repository, "/home/a/.", kFixedTime,
                                       &path, &error));
  EXPECT_EQ(path, expected);
}

TEST(CatalogBuildArchivePath, IsDeterministicAndCreatesNothing) {
  const std::string dir = CaseDir("catalog_build_pure");
  const std::string repository = dir + "/repo";
  const std::string source = dir + "/Documents";
  EXPECT_TRUE(MakeDir(source));
  bp::BackupCatalog catalog;
  std::string error;
  EXPECT_TRUE(catalog.EnsureRepository(repository, &error));
  const std::vector<std::string> before = DirectoryEntries(repository);

  std::string first;
  std::string second;
  error.clear();
  EXPECT_TRUE(catalog.BuildArchivePath(repository, source, kFixedTime, &first,
                                       &error));
  error.clear();
  EXPECT_TRUE(catalog.BuildArchivePath(repository, source, kFixedTime, &second,
                                       &error));
  EXPECT_EQ(first, second);
  EXPECT_EQ(first, repository + "/Documents_" + kFixedStamp + ".bak");

  // 只是候选：不创建文件，仓库里一个条目都不许多。
  EXPECT_FALSE(FileExists(first));
  EXPECT_EQ(DirectoryEntries(repository).size(), before.size());
}

TEST(CatalogBuildArchivePath, AddsThreeDigitCollisionSuffix) {
  const std::string dir = CaseDir("catalog_build_collision");
  const std::string repository = dir + "/repo";
  bp::BackupCatalog catalog;
  std::string error;
  EXPECT_TRUE(catalog.EnsureRepository(repository, &error));
  const std::string base = repository + "/Documents_" + kFixedStamp;
  EXPECT_TRUE(WriteFile(base + ".bak", "occupied"));

  std::string path;
  error.clear();
  EXPECT_TRUE(catalog.BuildArchivePath(repository, "/home/a/Documents",
                                       kFixedTime, &path, &error));
  EXPECT_EQ(path, base + "_001.bak");

  EXPECT_TRUE(WriteFile(base + "_001.bak", "occupied"));
  error.clear();
  EXPECT_TRUE(catalog.BuildArchivePath(repository, "/home/a/Documents",
                                       kFixedTime, &path, &error));
  EXPECT_EQ(path, base + "_002.bak");

  EXPECT_TRUE(WriteFile(base + "_002.bak", "occupied"));
  error.clear();
  EXPECT_TRUE(catalog.BuildArchivePath(repository, "/home/a/Documents",
                                       kFixedTime, &path, &error));
  EXPECT_EQ(path, base + "_003.bak");
}

TEST(CatalogBuildArchivePath, FailsWhenCollisionSpaceIsExhausted) {
  const std::string dir = CaseDir("catalog_build_exhausted");
  const std::string repository = dir + "/repo";
  bp::BackupCatalog catalog;
  std::string error;
  EXPECT_TRUE(catalog.EnsureRepository(repository, &error));
  const std::string prefix = repository + "/Documents_" + kFixedStamp;

  EXPECT_TRUE(WriteFile(prefix + ".bak", "x"));
  for (int suffix = 1; suffix <= 999; ++suffix) {
    char name[32] = {};
    std::snprintf(name, sizeof(name), "_%03d.bak", suffix);
    EXPECT_TRUE(WriteFile(prefix + name, "x"));
  }

  std::string path;
  error.clear();
  // 001 … 999 全占满之后必须明确失败，而不是悄悄换一套命名。
  EXPECT_FALSE(catalog.BuildArchivePath(repository, "/home/a/Documents",
                                        kFixedTime, &path, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_TRUE(path.empty());
  EXPECT_TRUE(Contains(error, "free archive name"));
}

TEST(CatalogBuildArchivePath, RejectsEmptyAndNullInputs) {
  const std::string dir = CaseDir("catalog_build_bad_args");
  const std::string repository = dir + "/repo";
  bp::BackupCatalog catalog;
  std::string error;
  EXPECT_TRUE(catalog.EnsureRepository(repository, &error));

  std::string path;
  error.clear();
  EXPECT_FALSE(catalog.BuildArchivePath("", "/home/a/Documents", kFixedTime,
                                        &path, &error));
  EXPECT_FALSE(error.empty());

  error.clear();
  EXPECT_FALSE(
      catalog.BuildArchivePath(repository, "", kFixedTime, &path, &error));
  EXPECT_FALSE(error.empty());

  error.clear();
  EXPECT_FALSE(catalog.BuildArchivePath(repository, "/home/a/Documents",
                                        kFixedTime, nullptr, &error));
  EXPECT_FALSE(error.empty());
}

// 契约：BuildArchivePath 成功生成的名字，一定落在 Catalog 自己允许管理的名字
// 空间里。否则系统会产出"自己生成、自己却 Resolve / Delete 不了"的文件名。
TEST(CatalogBuildArchivePath, GeneratedNameIsResolvable) {
  const std::string dir = CaseDir("catalog_build_contract");
  const std::string repository = dir + "/repo";
  bp::BackupCatalog catalog;
  std::string error;
  EXPECT_TRUE(catalog.EnsureRepository(repository, &error));

  const std::string backslash_source = std::string("/tmp/foo") + '\\' + "bar";
  const std::string sources[] = {"/home/a/Documents",     // 纯 ASCII
                                 backslash_source,       // 含 '\\'
                                 "/home/a/项目",          // UTF-8
                                 "/home/a/My Documents",  // 空格
                                 "/home/a/a#b%c"};        // '#' 与 '%'

  for (const std::string& source : sources) {
    std::string candidate;
    error.clear();
    EXPECT_TRUE(catalog.BuildArchivePath(repository, source, kFixedTime,
                                         &candidate, &error));
    EXPECT_TRUE(error.empty());

    const std::size_t slash = candidate.rfind('/');
    EXPECT_TRUE(slash != std::string::npos);
    const std::string base = candidate.substr(slash + 1);
    // 生成的名字里不能带 '\\'：ValidateFileName 一律禁止它。
    EXPECT_FALSE(Contains(base, std::string(1, '\\')));
    if (source == backslash_source) {
      EXPECT_EQ(base, std::string("foo_bar_") + kFixedStamp + ".bak");
    }

    // 真的建出文件，然后必须能被 Resolve 接受、被 Delete 删掉。
    EXPECT_TRUE(WriteFile(candidate, "placeholder"));
    std::string resolved;
    error.clear();
    EXPECT_TRUE(catalog.Resolve(repository, base, &resolved, &error));
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(resolved, candidate);

    error.clear();
    EXPECT_TRUE(catalog.Delete(repository, base, &error));
    EXPECT_FALSE(FileExists(candidate));
  }
}

TEST(CatalogBuildArchivePath, RejectsNulBytes) {
  const std::string dir = CaseDir("catalog_build_nul");
  const std::string repository = dir + "/repo";
  bp::BackupCatalog catalog;
  std::string error;
  EXPECT_TRUE(catalog.EnsureRepository(repository, &error));

  const std::string nul_source =
      std::string("/home/a/Doc") + '\0' + "uments";
  std::string path;
  error.clear();
  EXPECT_FALSE(catalog.BuildArchivePath(repository, nul_source, kFixedTime,
                                        &path, &error));
  EXPECT_TRUE(Contains(error, "NUL"));

  // repository 里的内嵌 NUL 同样拒绝：POSIX 拿的是 c_str()，不拦的话
  // "校验看到的路径"和"内核看到的路径"就不是同一个东西。
  const std::string nul_repository =
      repository + std::string(1, '\0') + "x";
  error.clear();
  EXPECT_FALSE(catalog.BuildArchivePath(nul_repository, "/home/a/Documents",
                                        kFixedTime, &path, &error));
  EXPECT_TRUE(Contains(error, "NUL"));

  std::vector<bp::BackupRecord> records;
  error.clear();
  EXPECT_FALSE(catalog.List(nul_repository, &records, &error));
  EXPECT_TRUE(Contains(error, "NUL"));

  std::string resolved;
  error.clear();
  EXPECT_FALSE(catalog.Resolve(nul_repository, "a.bak", &resolved, &error));
  EXPECT_TRUE(Contains(error, "NUL"));

  error.clear();
  EXPECT_FALSE(catalog.Delete(nul_repository, "a.bak", &error));
  EXPECT_TRUE(Contains(error, "NUL"));
}

// 命名函数不创建任何东西：仓库只由 EnsureRepository 创建。
TEST(CatalogBuildArchivePath, RequiresExistingRepository) {
  const std::string dir = CaseDir("catalog_build_missing_repo");
  bp::BackupCatalog catalog;
  const std::string missing = dir + "/nope";
  std::string error;
  std::string path;
  EXPECT_FALSE(catalog.BuildArchivePath(missing, "/home/a/Documents",
                                        kFixedTime, &path, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_TRUE(path.empty());
  EXPECT_FALSE(FileExists(missing));
}


// ============================================================
// BackupCatalog::Resolve
// ============================================================

TEST(CatalogResolve, ResolvesDirectChildArchive) {
  const std::string dir = CaseDir("catalog_resolve_ok");
  const std::string repository = dir + "/repo";
  const std::string source = MakeSampleSource(dir);
  bp::BackupCatalog catalog;
  std::string error;
  EXPECT_TRUE(catalog.EnsureRepository(repository, &error));
  EXPECT_TRUE(MakeArchive(source, repository + "/one.bak", &error));

  std::string path;
  error.clear();
  EXPECT_TRUE(catalog.Resolve(repository, "one.bak", &path, &error));
  EXPECT_TRUE(error.empty());
  EXPECT_EQ(path, repository + "/one.bak");

  // repository 带尾斜杠时不能拼出 "repo//one.bak"。
  error.clear();
  EXPECT_TRUE(catalog.Resolve(repository + "/", "one.bak", &path, &error));
  EXPECT_EQ(path, repository + "/one.bak");
}

TEST(CatalogResolve, RejectsMissingFile) {
  const std::string dir = CaseDir("catalog_resolve_missing");
  const std::string repository = dir + "/repo";
  bp::BackupCatalog catalog;
  std::string error;
  EXPECT_TRUE(catalog.EnsureRepository(repository, &error));
  std::string path;
  error.clear();
  EXPECT_FALSE(catalog.Resolve(repository, "nope.bak", &path, &error));
  EXPECT_FALSE(error.empty());
}

TEST(CatalogResolve, RejectsTraversalAndSeparators) {
  const std::string dir = CaseDir("catalog_resolve_traversal");
  const std::string repository = dir + "/repo";
  bp::BackupCatalog catalog;
  std::string error;
  EXPECT_TRUE(catalog.EnsureRepository(repository, &error));
  // 仓库外面确实有一个同名文件：只要越界一步就能删掉它，所以必须拦住。
  EXPECT_TRUE(WriteFile(dir + "/outside.bak", "outside"));

  const std::string backslash_name = std::string("a") + '\\' + "b.bak";
  const std::string absolute = dir + "/outside.bak";
  const char* names[] = {"../outside.bak",
                         "foo/../outside.bak",
                         absolute.c_str(),
                         "sub/one.bak",
                         backslash_name.c_str()};
  for (const char* name : names) {
    std::string path;
    std::string message;
    EXPECT_FALSE(catalog.Resolve(repository, name, &path, &message));
    EXPECT_FALSE(message.empty());
    EXPECT_TRUE(path.empty());
  }
}

TEST(CatalogResolve, RejectsNonBackupNames) {
  const std::string dir = CaseDir("catalog_resolve_names");
  const std::string repository = dir + "/repo";
  bp::BackupCatalog catalog;
  std::string error;
  EXPECT_TRUE(catalog.EnsureRepository(repository, &error));
  EXPECT_TRUE(WriteFile(repository + "/x.txt", "text"));
  EXPECT_TRUE(WriteFile(repository + "/x.BAK", "upper case"));

  const char* names[] = {"", ".", "..", "x.txt", "x.BAK", "noext", ".bak"};
  for (const char* name : names) {
    std::string path;
    std::string message;
    const bool ok = catalog.Resolve(repository, name, &path, &message);
    if (std::string(name) == ".bak") {
      // ".bak" 本身就是个合法的 .bak 文件名，只是这里不存在。
      EXPECT_FALSE(ok);
      EXPECT_TRUE(Contains(message, ".bak"));
    } else {
      EXPECT_FALSE(ok);
      EXPECT_FALSE(message.empty());
    }
  }
}

TEST(CatalogResolve, RejectsDirectoryAndSymlink) {
  const std::string dir = CaseDir("catalog_resolve_types");
  const std::string repository = dir + "/repo";
  const std::string source = MakeSampleSource(dir);
  bp::BackupCatalog catalog;
  std::string error;
  EXPECT_TRUE(catalog.EnsureRepository(repository, &error));
  EXPECT_TRUE(MakeDir(repository + "/folder.bak"));
  EXPECT_TRUE(MakeArchive(source, repository + "/real.bak", &error));
  EXPECT_EQ(::symlink((repository + "/real.bak").c_str(),
                      (repository + "/link.bak").c_str()),
            0);

  std::string path;
  std::string message;
  EXPECT_FALSE(catalog.Resolve(repository, "folder.bak", &path, &message));
  EXPECT_TRUE(Contains(message, "not a regular file"));

  message.clear();
  EXPECT_FALSE(catalog.Resolve(repository, "link.bak", &path, &message));
  EXPECT_TRUE(Contains(message, "not a regular file"));

  // 但软链接指向的目标本身是好的。
  message.clear();
  EXPECT_TRUE(catalog.Resolve(repository, "real.bak", &path, &message));
  EXPECT_EQ(path, repository + "/real.bak");
}

TEST(CatalogResolve, RejectsNullOutput) {
  const std::string dir = CaseDir("catalog_resolve_null");
  const std::string repository = dir + "/repo";
  bp::BackupCatalog catalog;
  std::string error;
  EXPECT_TRUE(catalog.EnsureRepository(repository, &error));
  error.clear();
  EXPECT_FALSE(catalog.Resolve(repository, "one.bak", nullptr, &error));
  EXPECT_FALSE(error.empty());
}

// ============================================================
// BackupCatalog::Delete
// ============================================================

TEST(CatalogDelete, DeletesValidArchive) {
  const std::string dir = CaseDir("catalog_delete_ok");
  const std::string repository = dir + "/repo";
  const std::string source = MakeSampleSource(dir);
  bp::BackupCatalog catalog;
  std::string error;
  EXPECT_TRUE(catalog.EnsureRepository(repository, &error));
  EXPECT_TRUE(MakeArchive(source, repository + "/one.bak", &error));

  error.clear();
  EXPECT_TRUE(catalog.Delete(repository, "one.bak", &error));
  EXPECT_TRUE(error.empty());
  EXPECT_FALSE(FileExists(repository + "/one.bak"));

  std::vector<bp::BackupRecord> records;
  error.clear();
  EXPECT_TRUE(catalog.List(repository, &records, &error));
  EXPECT_TRUE(records.empty());
}

TEST(CatalogDelete, DeletesCorruptedArchive) {
  const std::string dir = CaseDir("catalog_delete_corrupted");
  const std::string repository = dir + "/repo";
  bp::BackupCatalog catalog;
  std::string error;
  EXPECT_TRUE(catalog.EnsureRepository(repository, &error));
  EXPECT_TRUE(WriteFile(repository + "/broken.bak",
                        "this file is definitely not a backup archive."));

  // 前提：它确实不是一个能被认出来的归档。
  bp::ArchiveReader reader;
  bp::ArchiveSummary summary;
  std::string diagnostic;
  EXPECT_FALSE(reader.InspectHeader(repository + "/broken.bak", &summary,
                                    &diagnostic));
  EXPECT_FALSE(diagnostic.empty());

  // 坏归档仍然是普通 .bak 文件，必须删得掉。
  error.clear();
  EXPECT_TRUE(catalog.Delete(repository, "broken.bak", &error));
  EXPECT_TRUE(error.empty());
  EXPECT_FALSE(FileExists(repository + "/broken.bak"));
}

TEST(CatalogDelete, RejectsMissingFile) {
  const std::string dir = CaseDir("catalog_delete_missing");
  const std::string repository = dir + "/repo";
  bp::BackupCatalog catalog;
  std::string error;
  EXPECT_TRUE(catalog.EnsureRepository(repository, &error));
  error.clear();
  EXPECT_FALSE(catalog.Delete(repository, "nope.bak", &error));
  EXPECT_FALSE(error.empty());
}

TEST(CatalogDelete, RefusesToTouchOutsideTheRepository) {
  const std::string dir = CaseDir("catalog_delete_outside");
  const std::string repository = dir + "/repo";
  bp::BackupCatalog catalog;
  std::string error;
  EXPECT_TRUE(catalog.EnsureRepository(repository, &error));

  const std::string outside = dir + "/outside.bak";
  EXPECT_TRUE(WriteFile(outside, "precious bytes"));
  const std::string inside = repository + "/inside.bak";
  EXPECT_TRUE(WriteFile(inside, "inside bytes"));

  const std::string absolute = outside;
  const std::string backslash_name = std::string("..") + '\\' + "outside.bak";
  const char* names[] = {"../outside.bak", "foo/../outside.bak",
                         backslash_name.c_str(), absolute.c_str()};
  for (const char* name : names) {
    std::string message;
    EXPECT_FALSE(catalog.Delete(repository, name, &message));
    EXPECT_FALSE(message.empty());
  }

  // 仓库外的文件必须一个字节都没变。
  std::string content;
  EXPECT_TRUE(ReadFile(outside, &content));
  EXPECT_EQ(content, std::string("precious bytes"));
  EXPECT_TRUE(ReadFile(inside, &content));
  EXPECT_EQ(content, std::string("inside bytes"));
}

TEST(CatalogDelete, RefusesDirectoryAndSymlink) {
  const std::string dir = CaseDir("catalog_delete_types");
  const std::string repository = dir + "/repo";
  const std::string source = MakeSampleSource(dir);
  bp::BackupCatalog catalog;
  std::string error;
  EXPECT_TRUE(catalog.EnsureRepository(repository, &error));
  EXPECT_TRUE(MakeDir(repository + "/folder.bak"));
  EXPECT_TRUE(MakeArchive(source, repository + "/real.bak", &error));
  EXPECT_EQ(::symlink((repository + "/real.bak").c_str(),
                      (repository + "/link.bak").c_str()),
            0);

  std::string message;
  EXPECT_FALSE(catalog.Delete(repository, "folder.bak", &message));
  EXPECT_TRUE(Contains(message, "not a regular file"));
  EXPECT_TRUE(IsDirectory(repository + "/folder.bak"));

  message.clear();
  // 软链接本身不能删，它指向的目标更不能被删。
  EXPECT_FALSE(catalog.Delete(repository, "link.bak", &message));
  EXPECT_TRUE(Contains(message, "not a regular file"));
  EXPECT_TRUE(IsSymlink(repository + "/link.bak"));
  EXPECT_TRUE(FileExists(repository + "/real.bak"));
}

TEST(CatalogDelete, RejectsNonBackupNames) {
  const std::string dir = CaseDir("catalog_delete_names");
  const std::string repository = dir + "/repo";
  bp::BackupCatalog catalog;
  std::string error;
  EXPECT_TRUE(catalog.EnsureRepository(repository, &error));
  EXPECT_TRUE(WriteFile(repository + "/x.txt", "text"));
  EXPECT_TRUE(WriteFile(repository + "/x.BAK", "upper case"));

  const char* names[] = {"", ".", "..", "x.txt", "x.BAK", "noext"};
  for (const char* name : names) {
    std::string message;
    EXPECT_FALSE(catalog.Delete(repository, name, &message));
    EXPECT_FALSE(message.empty());
  }

  EXPECT_TRUE(FileExists(repository + "/x.txt"));
  EXPECT_TRUE(FileExists(repository + "/x.BAK"));
}

int main(int argc, char** argv) {
#ifdef L3_HAVE_GTEST
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
#else
  (void)argc;
  (void)argv;
  int passed = 0;
  int failed = 0;
  for (const l3::Case& c : l3::Registry()) {
    const int before = l3::Failures();
    c.fn();
    if (l3::Failures() == before) {
      ++passed;
      std::printf("[  PASSED  ] %s\n", c.name);
    } else {
      ++failed;
      std::printf("[  FAILED  ] %s\n", c.name);
    }
  }
  std::printf("[harness] tests=%d passed=%d failed=%d checks=%d\n",
              static_cast<int>(l3::Registry().size()), passed, failed,
              l3::Checks());
  return failed == 0 ? 0 : 1;
#endif
}
