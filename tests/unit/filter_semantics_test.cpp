// tests/unit/filter_semantics_test.cpp
//
// Filter 语义一致性单元测试：把"界面上写的"和"核心实际匹配的"钉在一起。
//
// 覆盖四组修复：
//   1. type:file 只表示普通文件，不再等价于"非目录"；
//   2. size: 只对普通文件参与匹配（special entry 的 size 被扫描层置 0）；
//   3. 软链接也解析 user: / group:（lstat 给的本来就是链接自己的属主）；
//   4. mtime 的日历日窗口按"次日 00:00 - 1"算，DST 那天是 23 / 25 小时。
//
// 独立 main()：不依赖任何第三方测试框架，失败时返回非零，最后一行打印
// "filter-semantics: N/M checks passed"。

#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#include "archive_entry.h"
#include "filter.h"
#include "tree_scanner.h"
#include "user_directory.h"

namespace bp = backupproject;

namespace {

int g_checks = 0;
int g_failures = 0;
const char* g_section = "";

void Section(const char* name) {
  g_section = name;
  std::printf("[filter-semantics] -- %s\n", name);
}

void Check(bool ok, const char* expression, int line) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::printf("[filter-semantics] FAIL [%s] (%s:%d): %s\n", g_section,
                __FILE__, line, expression);
  }
}

#define CHECK(expr) Check((expr), #expr, __LINE__)

std::string BaseName(const std::string& path) {
  const std::size_t slash = path.rfind('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

// 按扫描层给出的形状造条目：special entry 的 size 就是 0。
bp::FilterEntry TypedEntry(const std::string& path, bp::EntryType type,
                           bool is_directory) {
  bp::FilterEntry entry;
  entry.archive_path = path;
  entry.name = BaseName(path);
  entry.is_directory = is_directory;
  entry.type = type;
  return entry;
}

bp::FilterEntry RegularEntry(const std::string& path, std::uint64_t size) {
  bp::FilterEntry entry = TypedEntry(path, bp::EntryType::kRegularFile, false);
  entry.size = size;
  return entry;
}

// 只填初版字段的旧调用方形状：除 is_directory 外什么都不填，type 走默认值。
bp::FilterEntry LegacyEntry(const std::string& path, bool is_directory,
                            std::uint64_t size) {
  bp::FilterEntry entry;
  entry.archive_path = path;
  entry.name = BaseName(path);
  entry.is_directory = is_directory;
  entry.size = size;
  return entry;
}

bool Add(bp::Filter* filter, bp::FilterAction action, const std::string& text) {
  std::string error;
  const bool ok = filter->AddRule(action, text, &error);
  if (!ok) {
    std::printf("[filter-semantics]   AddRule 拒绝 [%s]: %s\n", text.c_str(),
                error.c_str());
  }
  CHECK(ok);
  return ok;
}

// ---- 现算当前 uid / gid 的名字 ------------------------------------------
//
// 不写死 alice / root 这类名字，也不拿被测的 LookupUserName 当标准答案：
// 直接用 libc 查一遍再对照，换台机器跑同样成立。
bool DirectUserName(std::uint32_t uid, std::string* out) {
  std::vector<char> buffer(1 << 16);
  struct passwd entry;
  struct passwd* result = nullptr;
  const int status = ::getpwuid_r(static_cast<uid_t>(uid), &entry,
                                  buffer.data(), buffer.size(), &result);
  if (status != 0 || result == nullptr || entry.pw_name == nullptr) {
    return false;
  }
  out->assign(entry.pw_name);
  return true;
}

bool DirectGroupName(std::uint32_t gid, std::string* out) {
  std::vector<char> buffer(1 << 16);
  struct group entry;
  struct group* result = nullptr;
  const int status = ::getgrgid_r(static_cast<gid_t>(gid), &entry,
                                  buffer.data(), buffer.size(), &result);
  if (status != 0 || result == nullptr || entry.gr_name == nullptr) {
    return false;
  }
  out->assign(entry.gr_name);
  return true;
}

// ---- 临时目录 -----------------------------------------------------------

// 只写 /tmp，析构时整棵删掉；断言失败也不会留下垃圾。
class TempTree {
 public:
  TempTree() {
    char pattern[] = "/tmp/filter-semantics-XXXXXX";
    char* created = ::mkdtemp(pattern);
    if (created != nullptr) {
      path_ = created;
    }
  }

  ~TempTree() {
    if (!path_.empty()) {
      std::error_code error;
      std::filesystem::remove_all(path_, error);
    }
  }

  TempTree(const TempTree&) = delete;
  TempTree& operator=(const TempTree&) = delete;

  bool ok() const { return !path_.empty(); }
  const std::string& path() const { return path_; }
  std::string Child(const std::string& name) const {
    return path_ + "/" + name;
  }

  bool WriteFile(const std::string& name, const char* text) {
    FILE* file = std::fopen(Child(name).c_str(), "wb");
    if (file == nullptr) {
      return false;
    }
    const std::size_t length = std::strlen(text);
    const bool ok = std::fwrite(text, 1, length, file) == length;
    std::fclose(file);
    return ok;
  }

  bool MakeDir(const std::string& name) {
    return ::mkdir(Child(name).c_str(), 0755) == 0;
  }

  bool MakeFifo(const std::string& name) {
    return ::mkfifo(Child(name).c_str(), 0644) == 0;
  }

  bool MakeSymlink(const std::string& target, const std::string& name) {
    return ::symlink(target.c_str(), Child(name).c_str()) == 0;
  }

 private:
  std::string path_;
};

// ---- 扫描辅助 -----------------------------------------------------------

bool ScanWithInclude(const std::string& root, const std::string& rule,
                     std::vector<bp::ArchiveEntry>* entries,
                     std::string* error) {
  bp::Filter filter;
  std::string add_error;
  if (!filter.AddRule(bp::FilterAction::kInclude, rule, &add_error)) {
    *error = add_error;
    return false;
  }
  return bp::ScanSourceTree(root, &filter, entries, error);
}

const bp::ArchiveEntry* FindEntry(const std::vector<bp::ArchiveEntry>& entries,
                                  const std::string& path) {
  for (const bp::ArchiveEntry& entry : entries) {
    if (entry.archive_path == path) {
      return &entry;
    }
  }
  return nullptr;
}

// ---- 1. type:file 只表示普通文件 ------------------------------------------

void TypeFileMeansRegularFile() {
  Section("type:file 只匹配普通文件");
  const bp::FilterEntry directory =
      TypedEntry("d", bp::EntryType::kDirectory, true);
  const bp::FilterEntry regular =
      TypedEntry("f.txt", bp::EntryType::kRegularFile, false);
  const bp::FilterEntry symlink =
      TypedEntry("link", bp::EntryType::kSymlink, false);
  const bp::FilterEntry fifo = TypedEntry("pipe", bp::EntryType::kFifo, false);
  const bp::FilterEntry chardev =
      TypedEntry("null", bp::EntryType::kCharDevice, false);
  const bp::FilterEntry blockdev =
      TypedEntry("sda", bp::EntryType::kBlockDevice, false);
  const bp::FilterEntry socket =
      TypedEntry("sock", bp::EntryType::kSocket, false);

  bp::Filter exclude;
  if (Add(&exclude, bp::FilterAction::kExclude, "type:file")) {
    CHECK(!exclude.ShouldIncludeFile(regular));
    CHECK(exclude.ShouldIncludeFile(directory));
    CHECK(exclude.ShouldIncludeFile(symlink));
    CHECK(exclude.ShouldIncludeFile(fifo));
    CHECK(exclude.ShouldIncludeFile(chardev));
    CHECK(exclude.ShouldIncludeFile(blockdev));
    CHECK(exclude.ShouldIncludeFile(socket));
    CHECK(!exclude.ShouldPruneDirectory(directory));
  }

  bp::Filter include;
  if (Add(&include, bp::FilterAction::kInclude, "type:file")) {
    CHECK(include.ShouldIncludeFile(regular));
    CHECK(!include.ShouldIncludeFile(directory));
    CHECK(!include.ShouldIncludeFile(symlink));
    CHECK(!include.ShouldIncludeFile(fifo));
    CHECK(!include.ShouldIncludeFile(chardev));
    CHECK(!include.ShouldIncludeFile(blockdev));
    CHECK(!include.ShouldIncludeFile(socket));
  }

  // 旧调用方兼容：只填 is_directory=false、不填 type 的条目仍然是普通文件
  // （EntryType 的默认值就是 kRegularFile），匹配结果与改动前一致。
  const bp::FilterEntry legacy_file = LegacyEntry("legacy.txt", false, 0);
  const bp::FilterEntry legacy_dir = LegacyEntry("legacy", true, 0);
  CHECK(legacy_file.type == bp::EntryType::kRegularFile);
  CHECK(!exclude.ShouldIncludeFile(legacy_file));
  CHECK(exclude.ShouldIncludeFile(legacy_dir));
  CHECK(include.ShouldIncludeFile(legacy_file));

  // type:folder 仍然按 is_directory 判断，不看 type。
  bp::Filter folder;
  if (Add(&folder, bp::FilterAction::kExclude, "type:folder")) {
    CHECK(folder.ShouldPruneDirectory(directory));
    CHECK(folder.ShouldPruneDirectory(legacy_dir));
    CHECK(!folder.ShouldPruneDirectory(regular));
    CHECK(!folder.ShouldPruneDirectory(symlink));
  }
}

// ---- 2. size: 只对普通文件 -----------------------------------------------

void SizeOnlyAppliesToRegularFiles() {
  Section("size: 只对普通文件参与匹配");
  const bp::FilterEntry small = RegularEntry("small.bin", 512);
  const bp::FilterEntry big = RegularEntry("big.bin", 4096);
  const bp::FilterEntry symlink =
      TypedEntry("link", bp::EntryType::kSymlink, false);
  const bp::FilterEntry fifo = TypedEntry("pipe", bp::EntryType::kFifo, false);
  const bp::FilterEntry chardev =
      TypedEntry("null", bp::EntryType::kCharDevice, false);
  const bp::FilterEntry blockdev =
      TypedEntry("sda", bp::EntryType::kBlockDevice, false);
  const bp::FilterEntry socket =
      TypedEntry("sock", bp::EntryType::kSocket, false);
  const bp::FilterEntry directory =
      TypedEntry("d", bp::EntryType::kDirectory, true);

  // 扫描层把 special entry 的 size 置 0；这里刻意保持 0，重现真实形状。
  bp::Filter exclude;
  if (Add(&exclude, bp::FilterAction::kExclude, "size:<=1KB")) {
    CHECK(!exclude.ShouldIncludeFile(small));
    CHECK(exclude.ShouldIncludeFile(big));
    CHECK(exclude.ShouldIncludeFile(symlink));
    CHECK(exclude.ShouldIncludeFile(fifo));
    CHECK(exclude.ShouldIncludeFile(chardev));
    CHECK(exclude.ShouldIncludeFile(blockdev));
    CHECK(exclude.ShouldIncludeFile(socket));
    CHECK(exclude.ShouldIncludeFile(directory));
    CHECK(!exclude.ShouldPruneDirectory(directory));
  }

  bp::Filter include;
  if (Add(&include, bp::FilterAction::kInclude, "size:<=1KB")) {
    CHECK(include.ShouldIncludeFile(small));
    CHECK(!include.ShouldIncludeFile(big));
    CHECK(!include.ShouldIncludeFile(symlink));
    CHECK(!include.ShouldIncludeFile(fifo));
    CHECK(!include.ShouldIncludeFile(chardev));
    CHECK(!include.ShouldIncludeFile(blockdev));
    CHECK(!include.ShouldIncludeFile(socket));
    CHECK(!include.ShouldIncludeFile(directory));
  }

  // 其它字段不受影响：uid / gid / user / group / type / name / path 对
  // special entry 照旧生效。
  bp::FilterEntry owned = symlink;
  owned.uid = 1000;
  owned.gid = 100;
  owned.user_name = "alice";
  owned.group_name = "staff";
  bp::Filter fields;
  if (Add(&fields, bp::FilterAction::kInclude,
          "uid:1000 gid:100 user:alice group:staff type:symlink name:link "
          "path:link")) {
    CHECK(fields.ShouldIncludeFile(owned));
  }

  // 旧调用方：只填 size、不填 type 的条目照旧参与 size 匹配。
  const bp::FilterEntry legacy = LegacyEntry("legacy.bin", false, 512);
  CHECK(!exclude.ShouldIncludeFile(legacy));
  CHECK(include.ShouldIncludeFile(legacy));
}

// ---- 3. 软链接的 user / group --------------------------------------------

void ScanResolvesOwnerNamesForEveryType() {
  Section("ScanSourceTree：软链接与 FIFO 也解析 user 与 group");
  TempTree tree;
  CHECK(tree.ok());
  if (!tree.ok()) {
    return;
  }
  CHECK(tree.MakeDir("sub"));
  CHECK(tree.WriteFile("regular.txt", "hello\n"));
  CHECK(tree.WriteFile("sub/inner.txt", "inner\n"));
  CHECK(tree.MakeSymlink("regular.txt", "link"));
  CHECK(tree.MakeFifo("pipe"));

  const std::uint32_t uid = static_cast<std::uint32_t>(::geteuid());
  const std::uint32_t gid = static_cast<std::uint32_t>(::getegid());
  std::string user;
  std::string group;
  CHECK(DirectUserName(uid, &user));
  CHECK(DirectGroupName(gid, &group));
  if (user.empty() || group.empty()) {
    std::printf("[filter-semantics]   当前 uid/gid 查不到名字，跳过扫描断言\n");
    return;
  }

  // 前置条件：新条目属于当前进程（/tmp 不是 setgid 目录）。
  struct stat info;
  CHECK(::lstat(tree.Child("link").c_str(), &info) == 0);
  CHECK(static_cast<std::uint32_t>(info.st_uid) == uid);
  CHECK(static_cast<std::uint32_t>(info.st_gid) == gid);

  std::vector<bp::ArchiveEntry> entries;
  std::string error;
  CHECK(ScanWithInclude(tree.path(), "user:" + user, &entries, &error));
  const bp::ArchiveEntry* link = FindEntry(entries, "link");
  CHECK(link != nullptr);
  if (link != nullptr) {
    CHECK(link->type == bp::EntryType::kSymlink);
    CHECK(link->user_name == user);
    CHECK(link->group_name == group);
    // 名字取自 lstat：链接自己没有被动过。
    CHECK(link->link_target == "regular.txt");
  }
  const bp::ArchiveEntry* regular = FindEntry(entries, "regular.txt");
  CHECK(regular != nullptr);
  if (regular != nullptr) {
    CHECK(regular->type == bp::EntryType::kRegularFile);
    CHECK(regular->user_name == user);
  }
  const bp::ArchiveEntry* fifo = FindEntry(entries, "pipe");
  CHECK(fifo != nullptr);
  if (fifo != nullptr) {
    CHECK(fifo->type == bp::EntryType::kFifo);
    CHECK(fifo->user_name == user);
  }

  std::vector<bp::ArchiveEntry> by_group;
  CHECK(ScanWithInclude(tree.path(), "group:" + group, &by_group, &error));
  const bp::ArchiveEntry* group_link = FindEntry(by_group, "link");
  CHECK(group_link != nullptr);
  if (group_link != nullptr) {
    CHECK(group_link->group_name == group);
  }

  // 反面对照：不存在的用户名一条都不命中（目录结构除外）。
  std::vector<bp::ArchiveEntry> none;
  CHECK(ScanWithInclude(tree.path(), "user:" + user + "-no-such-user", &none,
                        &error));
  CHECK(FindEntry(none, "link") == nullptr);
  CHECK(FindEntry(none, "regular.txt") == nullptr);
  CHECK(FindEntry(none, "pipe") == nullptr);
  CHECK(FindEntry(none, ".") != nullptr);
}

void ScanHonoursTypeFileAndSize() {
  Section("ScanSourceTree：type:file 与 size: 不误伤 special entry");
  TempTree tree;
  CHECK(tree.ok());
  if (!tree.ok()) {
    return;
  }
  CHECK(tree.WriteFile("regular.txt", "hello\n"));
  CHECK(tree.MakeSymlink("regular.txt", "link"));
  CHECK(tree.MakeFifo("pipe"));

  std::vector<bp::ArchiveEntry> typed;
  std::string error;
  CHECK(ScanWithInclude(tree.path(), "type:file", &typed, &error));
  CHECK(FindEntry(typed, "regular.txt") != nullptr);
  CHECK(FindEntry(typed, "link") == nullptr);
  CHECK(FindEntry(typed, "pipe") == nullptr);

  // size:<=1KB：软链接 / FIFO 的 size 是 0，但 size 规则对它们一律为 false，
  // 不会被"小文件"这条规则捞进来。
  std::vector<bp::ArchiveEntry> sized;
  CHECK(ScanWithInclude(tree.path(), "size:<=1KB", &sized, &error));
  CHECK(FindEntry(sized, "regular.txt") != nullptr);
  CHECK(FindEntry(sized, "link") == nullptr);
  CHECK(FindEntry(sized, "pipe") == nullptr);

  // 反面对照：type:symlink 仍然命中链接，说明上面排除掉的确实是 type:file /
  // size: 的越界，而不是链接与 FIFO 根本没被扫到。
  std::vector<bp::ArchiveEntry> links;
  CHECK(ScanWithInclude(tree.path(), "type:symlink", &links, &error));
  CHECK(FindEntry(links, "link") != nullptr);
  CHECK(FindEntry(links, "regular.txt") == nullptr);
}

// ---- 4. 共享解析入口 ------------------------------------------------------

void SharedUserDirectoryHelper() {
  Section("LookupUserName / LookupGroupName / UserDirectoryCache");
  const std::uint32_t uid = static_cast<std::uint32_t>(::geteuid());
  const std::uint32_t gid = static_cast<std::uint32_t>(::getegid());
  std::string expected_user;
  std::string expected_group;
  CHECK(DirectUserName(uid, &expected_user));
  CHECK(DirectGroupName(gid, &expected_group));

  std::string name = "sentinel";
  CHECK(bp::LookupUserName(uid, &name));
  CHECK(name == expected_user);

  name = "sentinel";
  CHECK(bp::LookupGroupName(gid, &name));
  CHECK(name == expected_group);

  // 几乎不可能存在的 id：明确返回 false，并把 out 清空——留下旧值会让调用方
  // 把上一个人的名字安到这个人头上。
  name = "sentinel";
  CHECK(!bp::LookupUserName(0xFFFFFFFEu, &name));
  CHECK(name.empty());

  name = "sentinel";
  CHECK(!bp::LookupGroupName(0xFFFFFFFDu, &name));
  CHECK(name.empty());

  // out 为 nullptr 不是崩溃点。
  CHECK(!bp::LookupUserName(uid, nullptr));
  CHECK(!bp::LookupGroupName(gid, nullptr));

  bp::UserDirectoryCache cache;
  const std::string& cached_user = cache.UserName(uid);
  CHECK(cached_user == expected_user);
  CHECK(cache.GroupName(gid) == expected_group);
  // 失败结果（空串）同样进缓存，但不会污染已经缓存过的 uid。
  CHECK(cache.UserName(0xFFFFFFFEu).empty());
  CHECK(cache.UserName(uid) == expected_user);
  CHECK(&cache.UserName(uid) == &cached_user);
}

// ---- 5. mtime 的日历日 ----------------------------------------------------

// TZ 的保存 / 恢复：DST 用例固定在 America/Los_Angeles 上跑，跑完恢复原值，
// 免得影响同一进程里后面的时区相关断言。
class ScopedTimeZone {
 public:
  explicit ScopedTimeZone(const char* zone) {
    const char* current = ::getenv("TZ");
    if (current != nullptr) {
      saved_ = current;
      had_tz_ = true;
    }
    ::setenv("TZ", zone, 1);
    ::tzset();
  }

  ~ScopedTimeZone() {
    if (had_tz_) {
      ::setenv("TZ", saved_.c_str(), 1);
    } else {
      ::unsetenv("TZ");
    }
    ::tzset();
  }

  ScopedTimeZone(const ScopedTimeZone&) = delete;
  ScopedTimeZone& operator=(const ScopedTimeZone&) = delete;

 private:
  bool had_tz_ = false;
  std::string saved_;
};

std::int64_t LocalTime(int year, int month, int day, int hour, int minute) {
  struct tm parts;
  std::memset(&parts, 0, sizeof(parts));
  parts.tm_year = year - 1900;
  parts.tm_mon = month - 1;
  parts.tm_mday = day;
  parts.tm_hour = hour;
  parts.tm_min = minute;
  parts.tm_isdst = -1;  // 让 mktime 自己判断夏令时
  return static_cast<std::int64_t>(std::mktime(&parts));
}

bool MatchesMtime(const bp::Filter& filter, std::int64_t when) {
  bp::FilterEntry entry = RegularEntry("probe.txt", 0);
  entry.mtime_sec = when;
  return filter.ShouldIncludeFile(entry);
}

// 二分出规则真正命中的那段闭区间：lo 在窗口之前、inside 在窗口之内、hi 在
// 窗口之后。端点完全由被测实现决定，测试不预判窗口宽度。
bool ProbeWindow(const bp::Filter& filter, std::int64_t lo, std::int64_t inside,
                 std::int64_t hi, std::int64_t* first, std::int64_t* last) {
  if (MatchesMtime(filter, lo) || !MatchesMtime(filter, inside) ||
      MatchesMtime(filter, hi)) {
    return false;
  }
  std::int64_t left = lo;
  std::int64_t right = inside;
  while (right - left > 1) {
    const std::int64_t mid = left + (right - left) / 2;
    if (MatchesMtime(filter, mid)) {
      right = mid;
    } else {
      left = mid;
    }
  }
  *first = right;
  left = inside;
  right = hi;
  while (right - left > 1) {
    const std::int64_t mid = left + (right - left) / 2;
    if (MatchesMtime(filter, mid)) {
      left = mid;
    } else {
      right = mid;
    }
  }
  *last = left;
  return true;
}

void DayWindow(const char* dsl, int year, int month, int day,
               std::int64_t expected_width) {
  bp::Filter filter;
  std::string error;
  if (!filter.AddRule(bp::FilterAction::kInclude, dsl, &error)) {
    std::printf("[filter-semantics]   AddRule 拒绝 [%s]: %s\n", dsl,
                error.c_str());
    CHECK(false);
    return;
  }
  std::int64_t first = 0;
  std::int64_t last = 0;
  const bool found =
      ProbeWindow(filter, LocalTime(year, month, day - 1, 12, 0),
                  LocalTime(year, month, day, 12, 0),
                  LocalTime(year, month, day + 1, 12, 0), &first, &last);
  CHECK(found);
  if (!found) {
    return;
  }
  CHECK(last - first + 1 == expected_width);
  // 端点：start-1 与 end+1 都不命中。
  CHECK(!MatchesMtime(filter, first - 1));
  CHECK(MatchesMtime(filter, first));
  CHECK(MatchesMtime(filter, last));
  CHECK(!MatchesMtime(filter, last + 1));
}

void MtimeCalendarDaysFollowDst() {
  Section("mtime 日历日跨 DST：窗口 = 次日 00:00 - 1");
  ScopedTimeZone zone("America/Los_Angeles");

  // 2026-03-08 春季前移：这一天有 82800 秒，不是 86400。
  DayWindow("mtime:2026-03-08", 2026, 3, 8, 82800);
  // 反向对照：普通一天仍然是 86400，变窄的是 DST 那天而不是整体。
  DayWindow("mtime:2026-06-15", 2026, 6, 15, 86400);
  // 2026-11-01 秋季回拨：这一天有 90000 秒。
  DayWindow("mtime:2026-11-01", 2026, 11, 1, 90000);

  bp::Filter filter;
  std::string error;
  CHECK(filter.AddRule(bp::FilterAction::kInclude, "mtime:2026-03-08", &error));
  std::int64_t first = 0;
  std::int64_t last = 0;
  CHECK(ProbeWindow(filter, LocalTime(2026, 3, 7, 12, 0),
                    LocalTime(2026, 3, 8, 12, 0), LocalTime(2026, 3, 9, 12, 0),
                    &first, &last));
  const std::int64_t day_start = LocalTime(2026, 3, 8, 0, 0);
  CHECK(first == day_start);
  CHECK(last == day_start + 82800 - 1);
  // 当天 23:30 必须命中。
  CHECK(MatchesMtime(filter, LocalTime(2026, 3, 8, 23, 30)));
  // 次日 00:30 不命中：旧写法 start + 86400 - 1 会把它算成 3 月 8 日，
  // 因为这一天实际上只过了 82800 秒就跨到了 3 月 9 日。
  CHECK(!MatchesMtime(filter, LocalTime(2026, 3, 9, 0, 30)));
  CHECK(!MatchesMtime(filter, LocalTime(2026, 3, 9, 1, 30)));

  // today / yesterday 必须首尾相接：[昨天 00:00, 今天 00:00 - 1]。
  bp::Filter today;
  bp::Filter yesterday;
  CHECK(today.AddRule(bp::FilterAction::kInclude, "mtime:today", &error));
  CHECK(
      yesterday.AddRule(bp::FilterAction::kInclude, "mtime:yesterday", &error));
  const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
  std::int64_t today_first = 0;
  std::int64_t today_last = 0;
  CHECK(ProbeWindow(today, now - 2 * 86400, now, now + 2 * 86400, &today_first,
                    &today_last));
  CHECK(today_first <= now && now <= today_last);
  std::int64_t y_first = 0;
  std::int64_t y_last = 0;
  CHECK(ProbeWindow(yesterday, today_first - 3 * 86400, today_first - 1,
                    today_first, &y_first, &y_last));
  CHECK(y_last + 1 == today_first);
  CHECK(MatchesMtime(yesterday, today_first - 1));
  CHECK(!MatchesMtime(today, today_first - 1));
}

}  // namespace

int main() {
  TypeFileMeansRegularFile();
  SizeOnlyAppliesToRegularFiles();
  ScanResolvesOwnerNamesForEveryType();
  ScanHonoursTypeFileAndSize();
  SharedUserDirectoryHelper();
  MtimeCalendarDaysFollowDst();

  const int passed = g_checks - g_failures;
  std::printf("filter-semantics: %d/%d checks passed\n", passed, g_checks);
  return g_failures == 0 ? 0 : 1;
}
