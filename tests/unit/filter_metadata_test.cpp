// tests/unit/filter_metadata_test.cpp
//
// 元数据字段单元测试：uid / gid / user / group 与 type 的 7 个取值。
//
// 覆盖三层：
//   1. 核心 Filter 的解析与匹配（AddRule / ShouldIncludeFile /
//   ShouldPruneDirectory /
//      ShouldSkipSpecialEntry）；
//   2. 目录剪枝契约：命中 exclude 的目录，其子路径根本不会再被询问；
//   3. FilterRuleBuilder 的序列化 / 校验 / 摘要必须落在同一份 DSL 语义上。
//
// 独立 main()：不依赖任何第三方测试框架，失败时返回非零，最后一行打印
// "filter-metadata: N/M checks passed"。

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include "filter.h"
#include "filter_rule_builder.h"

namespace bp = backupproject;

namespace {

int g_checks = 0;
int g_failures = 0;
const char* g_section = "";

void Section(const char* name) {
  g_section = name;
  std::printf("[filter-metadata] -- %s\n", name);
}

void Check(bool ok, const char* expression, int line) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::printf("[filter-metadata] FAIL [%s] (%s:%d): %s\n", g_section,
                __FILE__, line, expression);
  }
}

#define CHECK(expr) Check((expr), #expr, __LINE__)

std::string BaseName(const std::string& path) {
  const std::size_t slash = path.rfind('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

bp::FilterEntry FileEntry(const std::string& path) {
  bp::FilterEntry entry;
  entry.archive_path = path;
  entry.name = BaseName(path);
  entry.is_directory = false;
  entry.type = bp::EntryType::kRegularFile;
  return entry;
}

bp::FilterEntry TypedEntry(const std::string& path, bp::EntryType type) {
  bp::FilterEntry entry = FileEntry(path);
  entry.type = type;
  return entry;
}

bp::FilterEntry DirEntry(const std::string& path) {
  bp::FilterEntry entry;
  entry.archive_path = path;
  entry.name = BaseName(path);
  entry.is_directory = true;
  entry.type = bp::EntryType::kDirectory;
  return entry;
}

// 只填初版字段（is_directory / size / mtime）的旧调用方形状：
// type 保持默认值，uid / gid / user_name / group_name 一律不填。
bp::FilterEntry LegacyEntry(const std::string& path, bool is_directory,
                            std::uint64_t size, std::int64_t mtime_sec) {
  bp::FilterEntry entry;
  entry.archive_path = path;
  entry.name = BaseName(path);
  entry.is_directory = is_directory;
  entry.size = size;
  entry.mtime_sec = mtime_sec;
  return entry;
}

bool Add(bp::Filter* filter, bp::FilterAction action, const std::string& text) {
  std::string error;
  const bool ok = filter->AddRule(action, text, &error);
  if (!ok) {
    std::printf("[filter-metadata]   AddRule 拒绝 [%s]: %s\n", text.c_str(),
                error.c_str());
  }
  CHECK(ok);
  return ok;
}

// 必须被拒绝：返回 false 且给出非空错误信息。
void Rejected(bp::Filter* filter, bp::FilterAction action,
              const std::string& text) {
  std::string error;
  const bool ok = filter->AddRule(action, text, &error);
  if (ok) {
    std::printf("[filter-metadata]   期望被拒绝，但通过了: %s\n", text.c_str());
  }
  CHECK(!ok);
  CHECK(!error.empty());
}

// 本地时区当天 12:00 的时间戳：与核心的 YYYY-MM-DD / today 用同一套本地时间，
// 因此换个时区跑也不会 flaky（不像写死的 epoch 秒）。
std::int64_t LocalNoon(int year, int month, int day) {
  std::tm parts{};
  parts.tm_year = year - 1900;
  parts.tm_mon = month - 1;
  parts.tm_mday = day;
  parts.tm_hour = 12;
  parts.tm_isdst = -1;
  return static_cast<std::int64_t>(std::mktime(&parts));
}

bool Contains(const std::vector<std::string>& items, const char* value) {
  for (const std::string& item : items) {
    if (item == value) {
      return true;
    }
  }
  return false;
}

// ---- 核心：uid / gid ----------------------------------------------------

void UidAndGidComparisons() {
  Section("uid / gid 的 = > >= < <= 与闭区间（include 与 exclude）");
  bp::FilterEntry entry = FileEntry("data.txt");
  entry.uid = 1000;
  entry.gid = 100;

  struct Case {
    bp::FilterAction action;
    const char* rule;
    bool expected;
  };
  const Case cases[] = {
      {bp::FilterAction::kInclude, "uid:1000", true},
      {bp::FilterAction::kInclude, "uid:1001", false},
      {bp::FilterAction::kInclude, "uid:>999", true},
      {bp::FilterAction::kInclude, "uid:>1000", false},
      {bp::FilterAction::kInclude, "uid:>=1000", true},
      {bp::FilterAction::kInclude, "uid:>=1001", false},
      {bp::FilterAction::kInclude, "uid:<1001", true},
      {bp::FilterAction::kInclude, "uid:<1000", false},
      {bp::FilterAction::kInclude, "uid:<=1000", true},
      {bp::FilterAction::kInclude, "uid:<=999", false},
      {bp::FilterAction::kInclude, "uid:999..1001", true},
      {bp::FilterAction::kInclude, "uid:1000..1000", true},
      {bp::FilterAction::kInclude, "uid:0..999", false},
      {bp::FilterAction::kInclude, "uid:1001..2000", false},
      {bp::FilterAction::kInclude, "gid:100", true},
      {bp::FilterAction::kInclude, "gid:101", false},
      {bp::FilterAction::kInclude, "gid:>99", true},
      {bp::FilterAction::kInclude, "gid:>=100", true},
      {bp::FilterAction::kInclude, "gid:<101", true},
      {bp::FilterAction::kInclude, "gid:<=100", true},
      {bp::FilterAction::kInclude, "gid:<=99", false},
      {bp::FilterAction::kInclude, "gid:50..150", true},
      {bp::FilterAction::kInclude, "gid:101..200", false},
      {bp::FilterAction::kExclude, "uid:1000", false},
      {bp::FilterAction::kExclude, "uid:1001", true},
      {bp::FilterAction::kExclude, "uid:999..1001", false},
      {bp::FilterAction::kExclude, "gid:>=100", false},
      {bp::FilterAction::kExclude, "gid:<100", true},
      {bp::FilterAction::kExclude, "gid:100..100", false},
  };
  for (const Case& item : cases) {
    bp::Filter filter;
    if (!Add(&filter, item.action, item.rule)) {
      continue;
    }
    CHECK(filter.ShouldIncludeFile(entry) == item.expected);
  }

  // uid / gid 对目录同样生效：目录上的 exclude 一样能触发整棵剪枝。
  bp::FilterEntry directory = DirEntry("owned");
  directory.uid = 1000;
  directory.gid = 100;
  bp::Filter prune;
  if (Add(&prune, bp::FilterAction::kExclude, "uid:1000 gid:100")) {
    CHECK(prune.ShouldPruneDirectory(directory));
  }
}

void UidBoundaries() {
  Section("uid / gid 数字边界：0、4294967295、超 uint32");
  bp::FilterEntry root = FileEntry("root.txt");
  root.uid = 0;
  root.gid = 0;

  bp::Filter root_uid;
  if (Add(&root_uid, bp::FilterAction::kInclude, "uid:0")) {
    CHECK(root_uid.ShouldIncludeFile(root));
  }
  bp::Filter root_range;
  if (Add(&root_range, bp::FilterAction::kInclude, "uid:0..0")) {
    CHECK(root_range.ShouldIncludeFile(root));
  }
  bp::Filter root_gid;
  if (Add(&root_gid, bp::FilterAction::kExclude, "gid:0")) {
    CHECK(!root_gid.ShouldIncludeFile(root));
  }

  bp::FilterEntry top = FileEntry("top.txt");
  top.uid = 4294967295u;
  top.gid = 4294967295u;
  bp::Filter max_equal;
  if (Add(&max_equal, bp::FilterAction::kInclude, "uid:4294967295")) {
    CHECK(max_equal.ShouldIncludeFile(top));
  }
  bp::Filter max_greater;
  if (Add(&max_greater, bp::FilterAction::kInclude, "uid:>=4294967295")) {
    CHECK(max_greater.ShouldIncludeFile(top));
  }
  bp::Filter max_range;
  if (Add(&max_range, bp::FilterAction::kInclude,
          "gid:4294967295..4294967295")) {
    CHECK(max_range.ShouldIncludeFile(top));
  }
  bp::Filter max_less_equal;
  if (Add(&max_less_equal, bp::FilterAction::kInclude, "uid:<=4294967295")) {
    CHECK(max_less_equal.ShouldIncludeFile(top));
  }
  // 带运算符的界同样必须落在 uint32 内：4294967296 也要被拒绝，不能悄悄截断。
  bp::Filter max_below;
  Rejected(&max_below, bp::FilterAction::kInclude, "uid:<4294967296");

  // 超 uint32 / 反向区间 / 空值：必须明确拒绝，不能截断成"看起来能跑"的数。
  bp::Filter bad;
  Rejected(&bad, bp::FilterAction::kInclude, "uid:99999999999");
  Rejected(&bad, bp::FilterAction::kInclude, "gid:4294967296");
  Rejected(&bad, bp::FilterAction::kInclude, "uid:>=4294967296");
  Rejected(&bad, bp::FilterAction::kInclude, "uid:0..99999999999");
  Rejected(&bad, bp::FilterAction::kInclude, "gid:2000..1000");
  Rejected(&bad, bp::FilterAction::kInclude, "uid:");
  Rejected(&bad, bp::FilterAction::kInclude, "uid:abc");
  CHECK(bad.rule_count() == 0);

  // 拒绝不能污染已有规则集：旧规则仍然生效，条数也不变。
  bp::Filter keep;
  if (Add(&keep, bp::FilterAction::kExclude, "name:skip.txt")) {
    Rejected(&keep, bp::FilterAction::kExclude, "uid:99999999999");
    CHECK(keep.rule_count() == 1);
    CHECK(!keep.ShouldIncludeFile(FileEntry("skip.txt")));
    CHECK(keep.ShouldIncludeFile(FileEntry("keep.txt")));
  }
}

// ---- 核心：user / group -------------------------------------------------

void UserAndGroupNames() {
  Section("user / group：精确匹配、大小写敏感、空名字不匹配");
  bp::FilterEntry entry = FileEntry("note.txt");
  entry.user_name = "alice";
  entry.group_name = "staff";

  struct Case {
    bp::FilterAction action;
    const char* rule;
    bool expected;
  };
  const Case cases[] = {
      {bp::FilterAction::kInclude, "user:alice", true},
      {bp::FilterAction::kInclude, "user:Alice", false},
      {bp::FilterAction::kInclude, "user:ali", false},
      {bp::FilterAction::kInclude, "user:alice2", false},
      {bp::FilterAction::kInclude, "group:staff", true},
      {bp::FilterAction::kInclude, "group:Staff", false},
      {bp::FilterAction::kInclude, "group:staf", false},
      {bp::FilterAction::kInclude, "user:alice group:staff", true},
      {bp::FilterAction::kInclude, "user:alice group:other", false},
      {bp::FilterAction::kExclude, "user:alice", false},
      {bp::FilterAction::kExclude, "user:bob", true},
      {bp::FilterAction::kExclude, "group:staff", false},
      {bp::FilterAction::kExclude, "group:other", true},
  };
  for (const Case& item : cases) {
    bp::Filter filter;
    if (!Add(&filter, item.action, item.rule)) {
      continue;
    }
    CHECK(filter.ShouldIncludeFile(entry) == item.expected);
  }

  // 名字解析失败（空字符串）：不匹配、不报错、也不崩。
  const bp::FilterEntry unknown = FileEntry("unknown.txt");
  CHECK(unknown.user_name.empty());
  CHECK(unknown.group_name.empty());
  bp::Filter user_rule;
  if (Add(&user_rule, bp::FilterAction::kInclude, "user:alice")) {
    CHECK(!user_rule.ShouldIncludeFile(unknown));
  }
  bp::Filter group_rule;
  if (Add(&group_rule, bp::FilterAction::kInclude, "group:staff")) {
    CHECK(!group_rule.ShouldIncludeFile(unknown));
  }
  bp::Filter exclude_user;
  if (Add(&exclude_user, bp::FilterAction::kExclude, "user:alice")) {
    CHECK(exclude_user.ShouldIncludeFile(unknown));
    CHECK(!exclude_user.ShouldSkipSpecialEntry(unknown));
  }

  // 名字里带空格：子句切分只在"空白 + 已知字段名 + 冒号"处发生，不会被切开。
  bp::FilterEntry spaced = FileEntry("spaced.txt");
  spaced.user_name = "my user";
  bp::Filter spaced_rule;
  if (Add(&spaced_rule, bp::FilterAction::kInclude, "user:my user")) {
    CHECK(spaced_rule.ShouldIncludeFile(spaced));
  }
}

// ---- 核心：type ---------------------------------------------------------

void TypeValues() {
  Section("type 的 7 个取值：file folder symlink fifo char block socket");
  const bp::FilterEntry directory = DirEntry("d");
  const bp::FilterEntry file = TypedEntry("f", bp::EntryType::kRegularFile);
  const bp::FilterEntry symlink = TypedEntry("s", bp::EntryType::kSymlink);
  const bp::FilterEntry fifo = TypedEntry("p", bp::EntryType::kFifo);
  const bp::FilterEntry chardev = TypedEntry("c", bp::EntryType::kCharDevice);
  const bp::FilterEntry blockdev = TypedEntry("b", bp::EntryType::kBlockDevice);
  const bp::FilterEntry socket = TypedEntry("k", bp::EntryType::kSocket);

  // file / folder 沿用初版语义：只看 is_directory，所以 file = "非目录"。
  bp::Filter file_rule;
  if (Add(&file_rule, bp::FilterAction::kExclude, "type:file")) {
    CHECK(!file_rule.ShouldIncludeFile(file));
    CHECK(!file_rule.ShouldIncludeFile(symlink));
    CHECK(!file_rule.ShouldIncludeFile(fifo));
    CHECK(!file_rule.ShouldIncludeFile(chardev));
    CHECK(!file_rule.ShouldIncludeFile(blockdev));
    CHECK(!file_rule.ShouldIncludeFile(socket));
    CHECK(!file_rule.ShouldPruneDirectory(directory));
    CHECK(file_rule.ShouldIncludeFile(directory));
  }
  bp::Filter folder_rule;
  if (Add(&folder_rule, bp::FilterAction::kExclude, "type:folder")) {
    CHECK(folder_rule.ShouldPruneDirectory(directory));
    CHECK(!folder_rule.ShouldPruneDirectory(file));
    CHECK(!folder_rule.ShouldIncludeFile(directory));
  }

  struct Special {
    const char* dsl;
    const bp::FilterEntry* hit;
  };
  const Special specials[] = {
      {"type:symlink", &symlink}, {"type:fifo", &fifo},
      {"type:char", &chardev},    {"type:block", &blockdev},
      {"type:socket", &socket},
  };
  const bp::FilterEntry* const others[] = {
      &directory, &file, &symlink, &fifo, &chardev, &blockdev, &socket};
  for (const Special& special : specials) {
    bp::Filter filter;
    if (!Add(&filter, bp::FilterAction::kExclude, special.dsl)) {
      continue;
    }
    // 只命中自己那一类：目录、普通文件、其他 4 种特殊文件都不受影响。
    CHECK(!filter.ShouldIncludeFile(*special.hit));
    for (const bp::FilterEntry* other : others) {
      if (other == special.hit) {
        continue;
      }
      CHECK(filter.ShouldIncludeFile(*other));
    }
    // 特殊类型的 exclude 也能让 ShouldSkipSpecialEntry 放行（跳过而不是失败）。
    CHECK(filter.ShouldSkipSpecialEntry(*special.hit));
  }

  // 未知取值必须被拒绝，不能悄悄退化成 file。
  bp::Filter bad;
  Rejected(&bad, bp::FilterAction::kInclude, "type:hardlink");
  Rejected(&bad, bp::FilterAction::kInclude, "type:regular");
  Rejected(&bad, bp::FilterAction::kInclude, "type:");
}

// ---- 核心：优先级、剪枝、组合 -------------------------------------------

void ExcludeBeatsInclude() {
  Section("exclude 优先于 include（同一条路径同时命中两侧）");
  bp::Filter filter;
  if (Add(&filter, bp::FilterAction::kInclude, "type:file uid:1000") &&
      Add(&filter, bp::FilterAction::kExclude, "user:alice")) {
    bp::FilterEntry entry = FileEntry("a.txt");
    entry.uid = 1000;
    entry.user_name = "alice";
    CHECK(!filter.ShouldIncludeFile(entry));
    entry.user_name = "bob";
    CHECK(filter.ShouldIncludeFile(entry));
    entry.uid = 1001;
    entry.user_name = "alice";
    CHECK(!filter.ShouldIncludeFile(entry));
    entry.user_name = "bob";
    CHECK(!filter.ShouldIncludeFile(entry));
  }
}

void DirectoryPruning() {
  Section("目录剪枝：exclude type:folder name:excluded");
  bp::Filter filter;
  if (!Add(&filter, bp::FilterAction::kExclude, "type:folder name:excluded")) {
    return;
  }
  CHECK(filter.ShouldPruneDirectory(DirEntry("excluded")));
  CHECK(filter.ShouldPruneDirectory(DirEntry("sub/excluded")));
  CHECK(!filter.ShouldPruneDirectory(DirEntry("kept")));
  // name 命中但不是目录：type:folder 不成立，子句 AND 不命中。
  CHECK(!filter.ShouldPruneDirectory(FileEntry("excluded")));
  CHECK(filter.ShouldIncludeFile(FileEntry("excluded")));

  // 模拟归档扫描（父一定排在子前面）：目录被剪掉之后，子树里的路径根本不会被询问。
  struct Node {
    const char* path;
    bool is_directory;
  };
  const Node tree[] = {
      {"excluded", true}, {"excluded/inner.txt", false},
      {"kept", true},     {"kept/note.txt", false},
      {"a.txt", false},
  };
  std::vector<std::string> queried;
  std::vector<std::string> included;
  std::vector<std::string> pruned;
  for (const Node& node : tree) {
    bool covered = false;
    for (const std::string& prefix : pruned) {
      const std::string path = node.path;
      if (path.size() > prefix.size() &&
          path.compare(0, prefix.size(), prefix) == 0 &&
          path[prefix.size()] == '/') {
        covered = true;
        break;
      }
    }
    if (covered) {
      continue;  // 调用方根本不会问被剪掉目录的子路径
    }
    queried.push_back(node.path);
    const bp::FilterEntry entry =
        node.is_directory ? DirEntry(node.path) : FileEntry(node.path);
    if (node.is_directory) {
      if (filter.ShouldPruneDirectory(entry)) {
        pruned.push_back(node.path);
      }
    } else if (filter.ShouldIncludeFile(entry)) {
      included.push_back(node.path);
    }
  }
  CHECK(Contains(queried, "excluded"));
  CHECK(!Contains(queried, "excluded/inner.txt"));
  CHECK(Contains(queried, "kept"));
  CHECK(Contains(queried, "kept/note.txt"));
  CHECK(Contains(queried, "a.txt"));
  CHECK(!Contains(included, "excluded/inner.txt"));
  CHECK(Contains(included, "kept/note.txt"));
  CHECK(Contains(included, "a.txt"));

  // 回归：path: 规则仍然按"目录前缀"剪枝 build 目录本身。
  bp::Filter path_filter;
  if (Add(&path_filter, bp::FilterAction::kExclude, "path:**/build/**")) {
    CHECK(path_filter.ShouldPruneDirectory(DirEntry("a/build")));
    CHECK(!path_filter.ShouldPruneDirectory(DirEntry("a/building")));
  }
}

void CombinedClausesAreAnd() {
  Section("组合子句 AND：type:file uid:1000 size:>=1KB");
  bp::Filter filter;
  if (!Add(&filter, bp::FilterAction::kInclude,
           "type:file uid:1000 size:>=1KB")) {
    return;
  }
  bp::FilterEntry entry = FileEntry("big.bin");
  entry.uid = 1000;
  entry.size = 2048;
  CHECK(filter.ShouldIncludeFile(entry));
  entry.uid = 1001;
  CHECK(!filter.ShouldIncludeFile(entry));
  entry.uid = 1000;
  entry.size = 1023;
  CHECK(!filter.ShouldIncludeFile(entry));
  entry.size = 1024;  // 闭区间：等于下界算命中
  CHECK(filter.ShouldIncludeFile(entry));
  bp::FilterEntry directory = DirEntry("big");
  directory.uid = 1000;
  directory.size = 4096;
  CHECK(!filter.ShouldIncludeFile(directory));

  // user + group + gid 三个新字段同一条规则内 AND。
  bp::Filter names;
  if (Add(&names, bp::FilterAction::kInclude,
          "user:alice group:staff gid:100")) {
    bp::FilterEntry person = FileEntry("person.txt");
    person.user_name = "alice";
    person.group_name = "staff";
    person.gid = 100;
    CHECK(names.ShouldIncludeFile(person));
    person.gid = 101;
    CHECK(!names.ShouldIncludeFile(person));
  }
}

void MetadataCombinesWithMtime() {
  Section("新字段与 mtime 组合，且旧 mtime 形式全部保留");
  const std::int64_t noon = LocalNoon(2026, 9, 1);
  const std::int64_t next_noon = LocalNoon(2026, 9, 2);

  bp::Filter filter;
  if (Add(&filter, bp::FilterAction::kInclude, "uid:1000 mtime:2026-09-01")) {
    bp::FilterEntry entry = FileEntry("a.txt");
    entry.uid = 1000;
    entry.mtime_sec = noon;
    CHECK(filter.ShouldIncludeFile(entry));
    entry.mtime_sec = next_noon;
    CHECK(!filter.ShouldIncludeFile(entry));
    entry.mtime_sec = noon;
    entry.uid = 1001;
    CHECK(!filter.ShouldIncludeFile(entry));
  }
  bp::Filter range;
  if (Add(&range, bp::FilterAction::kInclude,
          "gid:100 mtime:2026-09-01..2026-09-02")) {
    bp::FilterEntry entry = FileEntry("a.txt");
    entry.gid = 100;
    entry.mtime_sec = next_noon;
    CHECK(range.ShouldIncludeFile(entry));
  }
  bp::Filter recent;
  if (Add(&recent, bp::FilterAction::kInclude, "user:alice mtime:7days")) {
    bp::FilterEntry entry = FileEntry("a.txt");
    entry.user_name = "alice";
    entry.mtime_sec = static_cast<std::int64_t>(std::time(nullptr));
    CHECK(recent.ShouldIncludeFile(entry));
  }
  bp::Filter type_time;
  if (Add(&type_time, bp::FilterAction::kExclude, "type:symlink mtime:today")) {
    bp::FilterEntry link = TypedEntry("link", bp::EntryType::kSymlink);
    link.mtime_sec = static_cast<std::int64_t>(std::time(nullptr));
    CHECK(!type_time.ShouldIncludeFile(link));
    link.mtime_sec = LocalNoon(2020, 1, 1);
    CHECK(type_time.ShouldIncludeFile(link));
  }

  const char* const mtime_forms[] = {"mtime:today", "mtime:yesterday",
                                     "mtime:7days", "mtime:2026-09-01",
                                     "mtime:2026-09-01..2026-09-12"};
  for (const char* form : mtime_forms) {
    bp::Filter probe;
    CHECK(probe.AddRule(bp::FilterAction::kInclude, form, nullptr));
  }
}

void LegacyCallersKeepTheirBehaviour() {
  Section("只填 is_directory / size / mtime 的旧调用方行为不变");
  const bp::FilterEntry file =
      LegacyEntry("dir/a.txt", false, 2048, LocalNoon(2026, 9, 1));
  const bp::FilterEntry directory =
      LegacyEntry("dir", true, 0, LocalNoon(2026, 9, 1));
  CHECK(file.type == bp::EntryType::kRegularFile);
  CHECK(file.uid == 0);
  CHECK(file.user_name.empty());

  bp::Filter type_file;
  if (Add(&type_file, bp::FilterAction::kExclude, "type:file")) {
    CHECK(!type_file.ShouldIncludeFile(file));
    CHECK(type_file.ShouldIncludeFile(directory));
  }
  bp::Filter type_folder;
  if (Add(&type_folder, bp::FilterAction::kExclude, "type:folder")) {
    CHECK(type_folder.ShouldPruneDirectory(directory));
    CHECK(!type_folder.ShouldPruneDirectory(file));
  }
  // 旧调用方不填 type：细分类型的规则不会误伤它们。
  bp::Filter type_symlink;
  if (Add(&type_symlink, bp::FilterAction::kExclude, "type:symlink")) {
    CHECK(type_symlink.ShouldIncludeFile(file));
    CHECK(!type_symlink.ShouldPruneDirectory(directory));
    CHECK(!type_symlink.ShouldSkipSpecialEntry(file));
  }
  bp::Filter old_size;
  if (Add(&old_size, bp::FilterAction::kInclude, "size:>=1KB name:*.txt")) {
    CHECK(old_size.ShouldIncludeFile(file));
  }
  bp::Filter old_mtime;
  if (Add(&old_mtime, bp::FilterAction::kInclude, "mtime:2026-09-01")) {
    CHECK(old_mtime.ShouldIncludeFile(file));
  }
  // 旧调用方没有 uid/gid 可填，默认 0：uid:0 会命中它——文档里写明了这个边界。
  bp::Filter default_uid;
  if (Add(&default_uid, bp::FilterAction::kInclude, "uid:0")) {
    CHECK(default_uid.ShouldIncludeFile(file));
  }
}

// ---- FilterRuleBuilder --------------------------------------------------

bp::FilterClauseDraft UidClause(bp::RuleSizeCompare compare, std::uint32_t low,
                                std::uint32_t high) {
  bp::FilterClauseDraft clause;
  clause.field = bp::RuleField::kUid;
  clause.uid_compare = compare;
  clause.uid = low;
  clause.uid_high = high;
  return clause;
}

bp::FilterClauseDraft GidClause(bp::RuleSizeCompare compare, std::uint32_t low,
                                std::uint32_t high) {
  bp::FilterClauseDraft clause;
  clause.field = bp::RuleField::kGid;
  clause.gid_compare = compare;
  clause.gid = low;
  clause.gid_high = high;
  return clause;
}

bp::FilterClauseDraft UserClause(const std::string& user) {
  bp::FilterClauseDraft clause;
  clause.field = bp::RuleField::kUser;
  clause.user = user;
  return clause;
}

bp::FilterClauseDraft GroupClause(const std::string& group) {
  bp::FilterClauseDraft clause;
  clause.field = bp::RuleField::kGroup;
  clause.group = group;
  return clause;
}

bp::FilterClauseDraft TypeClause(bp::RuleTypeValue type) {
  bp::FilterClauseDraft clause;
  clause.field = bp::RuleField::kType;
  clause.type = type;
  return clause;
}

bp::FilterClauseDraft SizeClause(bp::RuleSizeCompare compare, std::uint64_t low,
                                 std::uint64_t high, bp::RuleSizeUnit unit) {
  bp::FilterClauseDraft clause;
  clause.field = bp::RuleField::kSize;
  clause.compare = compare;
  clause.size_low = low;
  clause.size_high = high;
  clause.unit = unit;
  return clause;
}

bp::FilterRuleDraft Rule(bp::FilterAction action,
                         std::vector<bp::FilterClauseDraft> clauses) {
  bp::FilterRuleDraft rule;
  rule.action = action;
  rule.clauses = std::move(clauses);
  return rule;
}

std::string Dsl(const bp::FilterRuleDraft& rule) {
  std::string text;
  bp::ToDsl(rule, &text, nullptr);
  return text;
}

bool RuleIsValid(const bp::FilterRuleDraft& rule) {
  std::string error;
  const bool ok = bp::ValidateRule(rule, &error);
  if (!ok) {
    std::printf("[filter-metadata]   ValidateRule 拒绝: %s\n", error.c_str());
  }
  CHECK(ok);
  return ok;
}

void BuilderSerializesNewFields() {
  Section("builder：新字段的 ToDsl 逐字断言 + ValidateRule 通过");

  struct IdCase {
    bp::RuleSizeCompare compare;
    std::uint32_t low;
    std::uint32_t high;
    const char* dsl;
  };
  const IdCase uid_cases[] = {
      {bp::RuleSizeCompare::kEqual, 1000, 0, "uid:1000"},
      {bp::RuleSizeCompare::kLess, 1000, 0, "uid:<1000"},
      {bp::RuleSizeCompare::kLessEqual, 1000, 0, "uid:<=1000"},
      {bp::RuleSizeCompare::kGreater, 1000, 0, "uid:>1000"},
      {bp::RuleSizeCompare::kGreaterEqual, 1000, 0, "uid:>=1000"},
      {bp::RuleSizeCompare::kRange, 1000, 2000, "uid:1000..2000"},
      {bp::RuleSizeCompare::kEqual, 0, 0, "uid:0"},
  };
  for (const IdCase& item : uid_cases) {
    const bp::FilterRuleDraft rule =
        Rule(bp::FilterAction::kInclude,
             {UidClause(item.compare, item.low, item.high)});
    CHECK(Dsl(rule) == std::string(item.dsl));
    RuleIsValid(rule);
  }
  const IdCase gid_cases[] = {
      {bp::RuleSizeCompare::kEqual, 100, 0, "gid:100"},
      {bp::RuleSizeCompare::kLess, 100, 0, "gid:<100"},
      {bp::RuleSizeCompare::kLessEqual, 100, 0, "gid:<=100"},
      {bp::RuleSizeCompare::kGreater, 100, 0, "gid:>100"},
      {bp::RuleSizeCompare::kGreaterEqual, 100, 0, "gid:>=100"},
      {bp::RuleSizeCompare::kRange, 100, 200, "gid:100..200"},
      {bp::RuleSizeCompare::kEqual, 0, 0, "gid:0"},
  };
  for (const IdCase& item : gid_cases) {
    const bp::FilterRuleDraft rule =
        Rule(bp::FilterAction::kExclude,
             {GidClause(item.compare, item.low, item.high)});
    CHECK(Dsl(rule) == std::string(item.dsl));
    RuleIsValid(rule);
  }

  const bp::FilterRuleDraft user_rule =
      Rule(bp::FilterAction::kInclude, {UserClause("alice")});
  CHECK(Dsl(user_rule) == std::string("user:alice"));
  RuleIsValid(user_rule);
  const bp::FilterRuleDraft group_rule =
      Rule(bp::FilterAction::kExclude, {GroupClause("staff")});
  CHECK(Dsl(group_rule) == std::string("group:staff"));
  RuleIsValid(group_rule);

  struct TypeCase {
    bp::RuleTypeValue type;
    const char* dsl;
  };
  const TypeCase type_cases[] = {
      {bp::RuleTypeValue::kFile, "type:file"},
      {bp::RuleTypeValue::kFolder, "type:folder"},
      {bp::RuleTypeValue::kSymlink, "type:symlink"},
      {bp::RuleTypeValue::kFifo, "type:fifo"},
      {bp::RuleTypeValue::kCharDevice, "type:char"},
      {bp::RuleTypeValue::kBlockDevice, "type:block"},
      {bp::RuleTypeValue::kSocket, "type:socket"},
  };
  for (const TypeCase& item : type_cases) {
    const bp::FilterRuleDraft rule =
        Rule(bp::FilterAction::kExclude, {TypeClause(item.type)});
    CHECK(Dsl(rule) == std::string(item.dsl));
    RuleIsValid(rule);
  }

  // size 没有"等于"；builder 把它写成 a..a，生成的 DSL 仍然被核心接受。
  const bp::FilterRuleDraft size_equal = Rule(
      bp::FilterAction::kInclude, {SizeClause(bp::RuleSizeCompare::kEqual, 1024,
                                              0, bp::RuleSizeUnit::kByte)});
  CHECK(Dsl(size_equal) == std::string("size:1024..1024"));
  RuleIsValid(size_equal);

  // 多子句 AND：type:file uid:1000 size:>=1KB。
  const bp::FilterRuleDraft combined =
      Rule(bp::FilterAction::kInclude,
           {TypeClause(bp::RuleTypeValue::kFile),
            UidClause(bp::RuleSizeCompare::kEqual, 1000, 0),
            SizeClause(bp::RuleSizeCompare::kGreaterEqual, 1, 0,
                       bp::RuleSizeUnit::kKilo)});
  CHECK(Dsl(combined) == std::string("type:file uid:1000 size:>=1KB"));
  if (RuleIsValid(combined)) {
    bp::Filter core;
    CHECK(core.AddRule(bp::FilterAction::kInclude, Dsl(combined), nullptr));
    bp::FilterEntry entry = FileEntry("big.bin");
    entry.uid = 1000;
    entry.size = 1024;
    CHECK(core.ShouldIncludeFile(entry));
    entry.uid = 1001;
    CHECK(!core.ShouldIncludeFile(entry));
  }
}

void BuilderRejectsBadDrafts() {
  Section("builder：uid range 反向 / 空 user / 空 group 必须报错");
  std::string error;

  const bp::FilterClauseDraft uid_reversed =
      UidClause(bp::RuleSizeCompare::kRange, 2000, 1000);
  CHECK(!bp::ValidateClause(uid_reversed, &error));
  CHECK(!error.empty());
  const bp::FilterRuleDraft uid_rule =
      Rule(bp::FilterAction::kExclude, {uid_reversed});
  error.clear();
  CHECK(!bp::ValidateRule(uid_rule, &error));
  CHECK(!error.empty());

  const bp::FilterClauseDraft gid_reversed =
      GidClause(bp::RuleSizeCompare::kRange, 300, 100);
  CHECK(!bp::ValidateClause(gid_reversed, &error));
  CHECK(!error.empty());
  const bp::FilterRuleDraft gid_rule =
      Rule(bp::FilterAction::kInclude, {gid_reversed});
  error.clear();
  CHECK(!bp::ValidateRule(gid_rule, &error));
  CHECK(!error.empty());

  CHECK(!bp::ValidateClause(UserClause(""), &error));
  CHECK(!error.empty());
  CHECK(!bp::ValidateRule(Rule(bp::FilterAction::kInclude, {UserClause("")}),
                          &error));
  CHECK(!bp::ValidateClause(GroupClause(""), &error));
  CHECK(!error.empty());
  CHECK(!bp::ValidateRule(Rule(bp::FilterAction::kExclude, {GroupClause("")}),
                          &error));

  // uid / gid 允许 0（root），不能被当成"没填"。
  CHECK(
      bp::ValidateClause(UidClause(bp::RuleSizeCompare::kEqual, 0, 0), &error));
  CHECK(
      bp::ValidateClause(GidClause(bp::RuleSizeCompare::kEqual, 0, 0), &error));
  CHECK(
      bp::ValidateClause(UidClause(bp::RuleSizeCompare::kRange, 0, 0), &error));
  CHECK(RuleIsValid(
      Rule(bp::FilterAction::kInclude,
           {UidClause(bp::RuleSizeCompare::kRange, 0, 4294967295u)})));
}

void BuilderSummariesAndNames() {
  Section("builder：摘要与字段名");
  CHECK(
      bp::Summarize(Rule(bp::FilterAction::kInclude,
                         {UidClause(bp::RuleSizeCompare::kEqual, 1000, 0)})) ==
      std::string("包含：属主 uid = 1000"));
  CHECK(bp::Summarize(
            Rule(bp::FilterAction::kExclude,
                 {UidClause(bp::RuleSizeCompare::kRange, 1000, 2000)})) ==
        std::string("排除：属主 uid 在 1000 到 2000 之间"));
  CHECK(bp::Summarize(Rule(bp::FilterAction::kInclude,
                           {GidClause(bp::RuleSizeCompare::kEqual, 100, 0)})) ==
        std::string("包含：属组 gid = 100"));
  CHECK(
      bp::Summarize(Rule(bp::FilterAction::kInclude, {UserClause("alice")})) ==
      std::string("包含：用户 user = alice"));
  CHECK(
      bp::Summarize(Rule(bp::FilterAction::kExclude, {GroupClause("staff")})) ==
      std::string("排除：用户组 group = staff"));
  CHECK(bp::Summarize(Rule(bp::FilterAction::kExclude,
                           {TypeClause(bp::RuleTypeValue::kSymlink)})) ==
        std::string("排除：类型 = 符号链接"));
  CHECK(bp::Summarize(Rule(bp::FilterAction::kExclude,
                           {TypeClause(bp::RuleTypeValue::kFifo)})) ==
        std::string("排除：类型 = 命名管道"));
  CHECK(bp::Summarize(Rule(bp::FilterAction::kExclude,
                           {TypeClause(bp::RuleTypeValue::kSocket)})) ==
        std::string("排除：类型 = 套接字"));
  CHECK(bp::Summarize(
            Rule(bp::FilterAction::kInclude,
                 {UidClause(bp::RuleSizeCompare::kGreaterEqual, 1000, 0),
                  UserClause("alice")})) ==
        std::string("包含：属主 uid >= 1000，且用户 user = alice"));

  // 每个新字段都要有非空摘要（界面直接展示，空字符串会变成空白卡片）。
  const bp::FilterClauseDraft new_fields[] = {
      UidClause(bp::RuleSizeCompare::kEqual, 0, 0),
      GidClause(bp::RuleSizeCompare::kRange, 0, 10),
      UserClause("alice"),
      GroupClause("staff"),
      TypeClause(bp::RuleTypeValue::kCharDevice),
      TypeClause(bp::RuleTypeValue::kBlockDevice),
      TypeClause(bp::RuleTypeValue::kFile),
      TypeClause(bp::RuleTypeValue::kFolder),
  };
  for (const bp::FilterClauseDraft& clause : new_fields) {
    CHECK(!bp::SummarizeClause(clause).empty());
  }

  CHECK(std::string(bp::RuleFieldName(bp::RuleField::kUid)) == "uid");
  CHECK(std::string(bp::RuleFieldName(bp::RuleField::kGid)) == "gid");
  CHECK(std::string(bp::RuleFieldName(bp::RuleField::kUser)) == "user");
  CHECK(std::string(bp::RuleFieldName(bp::RuleField::kGroup)) == "group");
}

void BuilderCliArguments() {
  Section("builder：CLI 参数（新字段）");
  const std::vector<bp::FilterRuleDraft> rules = {
      Rule(bp::FilterAction::kInclude,
           {UidClause(bp::RuleSizeCompare::kGreaterEqual, 1000, 0)}),
      Rule(bp::FilterAction::kExclude, {UserClause("alice")})};
  const std::vector<std::string> args = bp::CliArguments(rules);
  CHECK(args.size() == static_cast<std::size_t>(4));
  if (args.size() == 4) {
    CHECK(args[0] == "--include");
    CHECK(args[1] == "uid:>=1000");
    CHECK(args[2] == "--exclude");
    CHECK(args[3] == "user:alice");
  }
}

}  // namespace

int main() {
  UidAndGidComparisons();
  UidBoundaries();
  UserAndGroupNames();
  TypeValues();
  ExcludeBeatsInclude();
  DirectoryPruning();
  CombinedClausesAreAnd();
  MetadataCombinesWithMtime();
  LegacyCallersKeepTheirBehaviour();
  BuilderSerializesNewFields();
  BuilderRejectsBadDrafts();
  BuilderSummariesAndNames();
  BuilderCliArguments();

  const int passed = g_checks - g_failures;
  std::printf("filter-metadata: %d/%d checks passed\n", passed, g_checks);
  return g_failures == 0 ? 0 : 1;
}
