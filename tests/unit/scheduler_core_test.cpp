// scheduler_core_test.cpp
//
// 定时备份共享核心的专项测试：simple_json / source_manifest / schedule_store。
//
// 刻意不引测试框架，与仓库里其它专项测试一致：失败计数 + 非零退出。
//
// 覆盖重点（对应 PR #17 的 §35 / §37 / §38 / §40 / §54 / §55）：
//   * JSON 解析的病态输入：重复 key、非法转义、小数/指数、溢出、超深嵌套、
//     尾部多余字节；
//   * schema 绑定：缺字段、多字段、类型不对、范围越界；
//   * manifest 的序列化往返与严格的坏输入拒绝；
//   * 变化检测的分类规则（added / removed / modified / metadata_changed
//     互斥，不重复计数）；
//   * schedule store 的原子保存、0600 权限、版本与字段严格性。

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <vector>

#include "backup_option_keys.h"
#include "schedule_store.h"
#include "simple_json.h"
#include "source_manifest.h"
#include "test_support.h"

namespace bp = backupproject;

namespace {

const char* kValidJson = R"JSON({
  "version": 1,
  "name": "hello\tworld",
  "flag": true,
  "off": false,
  "nothing": null,
  "list": [1, 2, 3],
  "nested": {"inner": "x"}
})JSON";

// test_support::WriteFile 要求显式 mode；这里统一成 0644。
bool Write(const std::string& path, const std::string& content) {
  return test_support::WriteFile(path, content, 0644);
}

bp::ManifestEntry MakeEntry(const std::string& path, bp::EntryType type) {
  bp::ManifestEntry entry;
  entry.archive_path = path;
  entry.type = type;
  entry.mode = 0644;
  entry.uid = 1000;
  entry.gid = 1000;
  entry.mtime_sec = 1700000000;
  entry.mtime_nsec = 123456789;
  return entry;
}

void CheckSummary(const std::string& label, const bp::ChangeSummary& actual,
                  std::uint64_t added, std::uint64_t removed,
                  std::uint64_t modified, std::uint64_t metadata_changed) {
  const std::string detail =
      "added=" + std::to_string(actual.added) +
      " removed=" + std::to_string(actual.removed) +
      " modified=" + std::to_string(actual.modified) +
      " metadata_changed=" + std::to_string(actual.metadata_changed);
  test_support::Check(actual.added == added && actual.removed == removed &&
                          actual.modified == modified &&
                          actual.metadata_changed == metadata_changed,
                      label, detail);
}

void DiffOf(const std::vector<bp::ManifestEntry>& previous,
            const std::vector<bp::ManifestEntry>& current,
            bp::ChangeSummary* summary) {
  std::string error;
  if (!bp::DiffManifests(previous, current, summary, nullptr, &error)) {
    test_support::Check(false, "DiffManifests must succeed", error);
  }
}

// ---- A. simple_json ----

void TestSimpleJson() {
  test_support::Section("A. simple_json");
  std::string error;

  {
    bp::JsonValue value;
    test_support::Check(bp::ParseJson(kValidJson, &value, &error),
                        "JSON-01 valid document parses", error);
    const bp::JsonValue* version = value.Find("version");
    test_support::Check(version != nullptr && version->is_number() &&
                            version->number == 1,
                        "JSON-02 integer field");
    const bp::JsonValue* name = value.Find("name");
    test_support::Check(name != nullptr && name->is_string() &&
                            name->text == std::string("hello\tworld"),
                        "JSON-03 string escape is decoded");
    const bp::JsonValue* list = value.Find("list");
    test_support::Check(list != nullptr && list->is_array() &&
                            list->array.size() == 3 &&
                            list->array[2].number == 3,
                        "JSON-04 array elements");
    const bp::JsonValue* nested = value.Find("nested");
    test_support::Check(nested != nullptr && nested->is_object() &&
                            nested->Find("inner") != nullptr,
                        "JSON-05 nested object");
    const bp::JsonValue* nothing = value.Find("nothing");
    test_support::Check(nothing != nullptr && nothing->kind == bp::JsonValue::Kind::kNull,
                        "JSON-06 null literal");
    test_support::Check(value.Find("missing") == nullptr,
                        "JSON-07 missing key returns nullptr");
  }

  {
    bp::JsonValue value;
    test_support::Check(!bp::ParseJson(R"JSON({"a": 1, "a": 2})JSON", &value, &error) &&
                            error.find("duplicate") != std::string::npos,
                        "JSON-08 duplicate key is rejected", error);
  }
  {
    bp::JsonValue value;
    test_support::Check(!bp::ParseJson(R"JSON({"a": "\u0041"})JSON", &value, &error) &&
                            error.find("unsupported JSON escape") != std::string::npos,
                        "JSON-09 unicode escape is rejected", error);
  }
  {
    bp::JsonValue value;
    test_support::Check(!bp::ParseJson(R"JSON({"a": 1} trailing)JSON", &value, &error) &&
                            error.find("unexpected data") != std::string::npos,
                        "JSON-10 trailing data is rejected", error);
  }
  {
    bp::JsonValue value;
    test_support::Check(!bp::ParseJson("", &value, &error),
                        "JSON-11 empty input is rejected", error);
  }
  {
    bp::JsonValue value;
    test_support::Check(!bp::ParseJson(R"JSON({"a": 1.5})JSON", &value, &error) &&
                            error.find("only integers") != std::string::npos,
                        "JSON-12 fraction is rejected", error);
  }
  {
    bp::JsonValue value;
    test_support::Check(!bp::ParseJson(R"JSON({"a": 1e3})JSON", &value, &error),
                        "JSON-13 exponent is rejected", error);
  }
  {
    bp::JsonValue value;
    test_support::Check(!bp::ParseJson(R"JSON({"a": 9223372036854775808})JSON",
                                       &value, &error) &&
                            error.find("out of range") != std::string::npos,
                        "JSON-14 integer overflow is rejected", error);
  }
  {
    bp::JsonValue value;
    test_support::Check(!bp::ParseJson(R"JSON({"a": 007})JSON", &value, &error) &&
                            error.find("leading zeros") != std::string::npos,
                        "JSON-15 leading zeros are rejected", error);
  }
  {
    // 32 层嵌套超过 kMaxJsonDepth。
    std::string deep;
    for (int index = 0; index < 32; ++index) deep += "[";
    for (int index = 0; index < 32; ++index) deep += "]";
    bp::JsonValue value;
    test_support::Check(!bp::ParseJson(deep, &value, &error) &&
                            error.find("nesting is too deep") != std::string::npos,
                        "JSON-16 deep nesting is rejected", error);
  }
  {
    bp::JsonValue value;
    test_support::Check(!bp::ParseJson(R"JSON({"a": "unterminated})JSON", &value,
                                       &error) &&
                            error.find("unterminated") != std::string::npos,
                        "JSON-17 unterminated string is rejected", error);
  }
  {
    bp::JsonValue value;
    test_support::Check(!bp::ParseJson(R"JSON({"a": 1 "b": 2})JSON", &value, &error),
                        "JSON-18 missing comma is rejected", error);
  }
  {
    const std::string big(bp::kMaxJsonBytes + 1, 'x');
    bp::JsonValue value;
    test_support::Check(!bp::ParseJson(big, &value, &error) &&
                            error.find("too large") != std::string::npos,
                        "JSON-19 oversized document is rejected", error);
  }

  // ---- schema 绑定 ----
  {
    bp::JsonValue value;
    bp::ParseJson(R"JSON({"a": 1, "b": "x"})JSON", &value, &error);
    test_support::Check(bp::RequireExactFields(value, {"a", "b"}, "demo", &error),
                        "JSON-20 exact fields accepted", error);
    test_support::Check(!bp::RequireExactFields(value, {"a"}, "demo", &error) &&
                            error.find("unknown field") != std::string::npos,
                        "JSON-21 unknown field is rejected", error);
    test_support::Check(!bp::RequireExactFields(value, {"a", "b", "c"}, "demo",
                                               &error) &&
                            error.find("missing required field") != std::string::npos,
                        "JSON-22 missing field is rejected", error);
    std::uint32_t number = 0;
    test_support::Check(bp::RequireUint32(value, "a", "demo", 0, 10, &number,
                                          &error) &&
                            number == 1,
                        "JSON-23 uint32 in range", error);
    test_support::Check(!bp::RequireUint32(value, "a", "demo", 5, 10, &number,
                                           &error) &&
                            error.find("out of range") != std::string::npos,
                        "JSON-24 uint32 out of range is rejected", error);
    std::string text;
    test_support::Check(!bp::RequireString(value, "a", "demo", &text, &error) &&
                            error.find("must be a string") != std::string::npos,
                        "JSON-25 wrong type is rejected", error);
    const bp::JsonValue* array = nullptr;
    test_support::Check(!bp::RequireArray(value, "b", "demo", &array, &error) &&
                            error.find("must be an array") != std::string::npos,
                        "JSON-26 array type is enforced", error);
  }
  {
    std::string out;
    bp::WriteJsonString(&out, std::string("a\tb\x01"));
    const std::string quote(1, '"');
    const std::string expected =
        quote + "a" + "\\" + "t" + "b" + "\\" + "u0001" + quote;
    test_support::Check(out == expected,
                        "JSON-27 writer escapes control characters", out);
  }
}

// ---- B. source_manifest：真实目录树 ----

void TestManifestFromTree() {
  test_support::Section("B. source_manifest from a real tree");
  const std::string root = test_support::FreshDir("manifest");
  const std::string source = root + "/src";
  test_support::Mkdir(source, 0755);
  test_support::Mkdir(source + "/dir", 0755);
  Write(source + "/a.txt", "hello");
  Write(source + "/dir/b.txt", "world");
  test_support::CreateSymlink("a.txt", source + "/link");
  test_support::CreateHardlink(source + "/a.txt", source + "/hard2");
  Write(source + "/.hidden", "h");
  Write(source + "/with space.txt", "s");

  std::vector<bp::ManifestEntry> entries;
  std::string error;
  test_support::Check(bp::BuildSourceManifest(source, nullptr, &entries, &error),
                      "MAN-01 manifest builds", error);
  test_support::Check(entries.size() == 8,
                      "MAN-02 entry count matches the archive set",
                      std::to_string(entries.size()));
  test_support::Check(!entries.empty() && entries[0].archive_path == "." &&
                          entries[0].type == bp::EntryType::kDirectory,
                      "MAN-03 the first entry is the source root");

  bool found_hidden = false;
  bool found_space = false;
  bool found_symlink = false;
  bool found_hardlink = false;
  std::string hardlink_target;
  std::uint32_t leader_degree = 0;
  for (const bp::ManifestEntry& entry : entries) {
    if (entry.archive_path == ".hidden") found_hidden = true;
    if (entry.archive_path == "with space.txt") found_space = true;
    if (entry.type == bp::EntryType::kSymlink) {
      found_symlink = entry.link_target == "a.txt";
    }
    if (entry.type == bp::EntryType::kHardLink) {
      found_hardlink = true;
      hardlink_target = entry.link_target;
    }
    if (entry.archive_path == "a.txt") leader_degree = entry.hardlink_degree;
  }
  test_support::Check(found_hidden, "MAN-04 hidden path is recorded");
  test_support::Check(found_space, "MAN-05 path with a space is recorded");
  test_support::Check(found_symlink, "MAN-06 symlink target is recorded");
  test_support::Check(found_hardlink && hardlink_target == "a.txt",
                      "MAN-07 hardlink relation is recorded", hardlink_target);
  test_support::Check(leader_degree == 1,
                      "MAN-08 hardlink degree counts the followers",
                      std::to_string(leader_degree));

  // 序列化往返。
  const std::string text = bp::SerializeManifest(entries);
  std::vector<bp::ManifestEntry> parsed;
  test_support::Check(bp::ParseManifest(text, &parsed, &error),
                      "MAN-09 manifest round-trips", error);
  test_support::Check(parsed.size() == entries.size(),
                      "MAN-10 round-trip keeps the count");
  bool identical = parsed.size() == entries.size();
  for (std::size_t index = 0; identical && index < entries.size(); ++index) {
    identical = parsed[index].archive_path == entries[index].archive_path &&
                parsed[index].type == entries[index].type &&
                parsed[index].size == entries[index].size &&
                parsed[index].mtime_sec == entries[index].mtime_sec &&
                parsed[index].mtime_nsec == entries[index].mtime_nsec &&
                parsed[index].mode == entries[index].mode &&
                parsed[index].uid == entries[index].uid &&
                parsed[index].gid == entries[index].gid &&
                parsed[index].link_target == entries[index].link_target &&
                parsed[index].dev_major == entries[index].dev_major &&
                parsed[index].dev_minor == entries[index].dev_minor &&
                parsed[index].hardlink_degree == entries[index].hardlink_degree;
  }
  test_support::Check(identical, "MAN-11 round-trip is field-by-field exact");

  // 转义：文件名的 TAB / 换行 / 反斜杠必须能活着回来。
  std::vector<bp::ManifestEntry> weird;
  bp::ManifestEntry tab = MakeEntry("a\tb", bp::EntryType::kRegularFile);
  bp::ManifestEntry newline = MakeEntry("c\nd", bp::EntryType::kRegularFile);
  bp::ManifestEntry backslash = MakeEntry("e\\f", bp::EntryType::kRegularFile);
  weird.push_back(tab);
  weird.push_back(newline);
  weird.push_back(backslash);
  std::vector<bp::ManifestEntry> weird_parsed;
  test_support::Check(bp::ParseManifest(bp::SerializeManifest(weird),
                                        &weird_parsed, &error) &&
                          weird_parsed.size() == 3 &&
                          weird_parsed[0].archive_path == "a\tb" &&
                          weird_parsed[1].archive_path == "c\nd" &&
                          weird_parsed[2].archive_path == "e\\f",
                      "MAN-12 field escaping survives a round trip", error);
}

void TestManifestRejectsBadInput() {
  test_support::Section("C. source_manifest rejects bad input");
  std::string error;
  std::vector<bp::ManifestEntry> entries;

  test_support::Check(!bp::ParseManifest("", &entries, &error),
                      "MAN-20 empty manifest is rejected", error);
  test_support::Check(!bp::ParseManifest("NOTAMANIFEST\n", &entries, &error),
                      "MAN-21 wrong header is rejected", error);
  test_support::Check(!bp::ParseManifest("BPMANIFEST1 2\n", &entries, &error),
                      "MAN-22 missing entries are rejected", error);
  test_support::Check(
      !bp::ParseManifest("BPMANIFEST1 1\n2\t1\t1\t1\t644\t0\t0\t0\t0\t0\t.\n",
                         &entries, &error),
      "MAN-23 a wrong field count is rejected", error);
  test_support::Check(
      !bp::ParseManifest("BPMANIFEST1 1\n8\t0\t0\t0\t644\t0\t0\t0\t0\t0\t.\t\n",
                         &entries, &error),
      "MAN-24 the never-written socket type is rejected", error);
  test_support::Check(
      !bp::ParseManifest("BPMANIFEST1 0\n2\t0\t0\t0\t644\t0\t0\t0\t0\t0\ta\t\n",
                         &entries, &error),
      "MAN-25 trailing data after the declared count is rejected", error);
  test_support::Check(
      !bp::ParseManifest(
          "BPMANIFEST1 1\n2\t5\t0\t0\t644\t0\t0\t0\t0\t0\t/x\t\n", &entries,
          &error),
      "MAN-26 an absolute archive path is rejected", error);
  test_support::Check(
      !bp::ParseManifest(
          "BPMANIFEST1 2\n2\t0\t0\t0\t644\t0\t0\t0\t0\t0\ta\t\n"
          "2\t0\t0\t0\t644\t0\t0\t0\t0\t0\ta\t\n",
          &entries, &error),
      "MAN-27 a duplicate path is rejected", error);
}

// ---- D. 变化检测 ----

void TestChangeDetection() {
  test_support::Section("D. change detection");
  bp::ChangeSummary summary;

  const std::vector<bp::ManifestEntry> base = {
      MakeEntry(".", bp::EntryType::kDirectory),
      MakeEntry("a.txt", bp::EntryType::kRegularFile),
      MakeEntry("link", bp::EntryType::kSymlink),
  };

  DiffOf(base, base, &summary);
  CheckSummary("DIFF-01 no change is completely empty", summary, 0, 0, 0, 0);

  {
    std::vector<bp::ManifestEntry> current = base;
    bp::ManifestEntry added = MakeEntry("b.txt", bp::EntryType::kRegularFile);
    current.push_back(added);
    DiffOf(base, current, &summary);
    CheckSummary("DIFF-02 added regular file", summary, 1, 0, 0, 0);
  }
  {
    std::vector<bp::ManifestEntry> current;
    for (const bp::ManifestEntry& entry : base) {
      if (entry.archive_path == "a.txt") continue;
      current.push_back(entry);
    }
    DiffOf(base, current, &summary);
    CheckSummary("DIFF-03 removed regular file", summary, 0, 1, 0, 0);
  }
  {
    std::vector<bp::ManifestEntry> current = base;
    current[1].size = 42;
    DiffOf(base, current, &summary);
    CheckSummary("DIFF-04 size change is modified", summary, 0, 0, 1, 0);
  }
  {
    std::vector<bp::ManifestEntry> current = base;
    current[1].mtime_sec += 1;
    DiffOf(base, current, &summary);
    CheckSummary("DIFF-05 mtime change is modified", summary, 0, 0, 1, 0);
  }
  {
    std::vector<bp::ManifestEntry> current = base;
    current[1].mtime_nsec += 1;
    DiffOf(base, current, &summary);
    CheckSummary("DIFF-06 mtime nanosecond change is modified", summary, 0, 0, 1,
                 0);
  }
  {
    std::vector<bp::ManifestEntry> current = base;
    current[1].mode = 0600;
    DiffOf(base, current, &summary);
    CheckSummary("DIFF-07 mode-only change is metadata_changed", summary, 0, 0, 0,
                 1);
  }
  {
    std::vector<bp::ManifestEntry> current = base;
    current[1].uid = 4242;
    DiffOf(base, current, &summary);
    CheckSummary("DIFF-08 uid-only change is metadata_changed", summary, 0, 0, 0,
                 1);
  }
  {
    std::vector<bp::ManifestEntry> current = base;
    current[1].gid = 4243;
    DiffOf(base, current, &summary);
    CheckSummary("DIFF-09 gid-only change is metadata_changed", summary, 0, 0, 0,
                 1);
  }
  {
    std::vector<bp::ManifestEntry> current = base;
    current[2].link_target = "other.txt";
    DiffOf(base, current, &summary);
    CheckSummary("DIFF-10 symlink target change is modified", summary, 0, 0, 1,
                 0);
  }
  {
    std::vector<bp::ManifestEntry> current = base;
    current[1].type = bp::EntryType::kFifo;
    DiffOf(base, current, &summary);
    CheckSummary("DIFF-11 type change is modified", summary, 0, 0, 1, 0);
  }
  {
    // 普通文件的 mtime 变了、mode 也变了：只算 modified，不重复计数。
    std::vector<bp::ManifestEntry> current = base;
    current[1].mtime_sec += 5;
    current[1].mode = 0600;
    DiffOf(base, current, &summary);
    CheckSummary("DIFF-12 classification never double counts", summary, 0, 0, 1,
                 0);
  }
  {
    // 目录 mtime **不参与**变化检测：这是统一定义，也是一个已知盲区。
    //
    // 理由：任何子项的新增/删除都会顺带改掉父目录的 mtime。如果把它算成变化，
    // "新增一个被 filter 排除的文件"也会触发一次完整快照——而实际备份集合
    // 一个字都没变。子项自身的变化已经被逐条比较过，不需要父目录再报一次。
    // 代价是"单独 touch 一个目录、内容不变"看不出来。这个取舍写在这里，
    // 免得将来被当成 bug 顺手改回去。
    std::vector<bp::ManifestEntry> current = base;
    current[0].mtime_sec += 1;
    DiffOf(base, current, &summary);
    CheckSummary("DIFF-13 directory mtime is a documented blind spot", summary, 0,
                 0, 0, 0);
  }
  {
    std::vector<bp::ManifestEntry> previous = {
        MakeEntry("dev", bp::EntryType::kCharDevice)};
    std::vector<bp::ManifestEntry> current = previous;
    current[0].dev_major = 1;
    current[0].dev_minor = 3;
    DiffOf(previous, current, &summary);
    CheckSummary("DIFF-14 char device major/minor change is modified", summary, 0,
                 0, 1, 0);

    std::vector<bp::ManifestEntry> block = {
        MakeEntry("disk", bp::EntryType::kBlockDevice)};
    std::vector<bp::ManifestEntry> block_changed = block;
    block_changed[0].dev_minor = 17;
    DiffOf(block, block_changed, &summary);
    CheckSummary("DIFF-15 block device minor change is modified", summary, 0, 0,
                 1, 0);
  }
  {
    // hardlink：leader 的 degree 变了（多了一个指向它的硬链接）。
    std::vector<bp::ManifestEntry> previous = base;
    previous[1].hardlink_degree = 1;
    std::vector<bp::ManifestEntry> current = previous;
    current[1].hardlink_degree = 2;
    DiffOf(previous, current, &summary);
    CheckSummary("DIFF-16 hardlink degree change is metadata_changed", summary, 0,
                 0, 0, 1);
  }
  {
    // hardlink 条目只比较 link_target：同一 inode 的 mtime 由 leader 负责。
    std::vector<bp::ManifestEntry> previous = {
        MakeEntry("h", bp::EntryType::kHardLink)};
    previous[0].link_target = "a";
    std::vector<bp::ManifestEntry> current = previous;
    current[0].mtime_sec += 10;
    current[0].mode = 0600;
    DiffOf(previous, current, &summary);
    CheckSummary("DIFF-17 hardlink entry ignores inode metadata", summary, 0, 0,
                 0, 0);
  }
  {
    std::vector<bp::ManifestEntry> current = base;
    current[2].type = bp::EntryType::kHardLink;
    current[2].link_target = "a.txt";
    DiffOf(base, current, &summary);
    CheckSummary("DIFF-18 symlink replaced by hardlink is modified", summary, 0,
                 0, 1, 0);
  }
}

void TestChangeDetectionFromTrees() {
  test_support::Section("E. change detection against the real scanner");
  const std::string root = test_support::FreshDir("diff-tree");
  const std::string source = root + "/src";
  test_support::Mkdir(source, 0755);
  Write(source + "/keep.txt", "keep");
  Write(source + "/skip.log", "log");

  bp::Filter filter;
  std::string error;
  test_support::Check(filter.AddRule(bp::FilterAction::kExclude, "ext:log", &error),
                      "EXCL-01 exclude rule is accepted", error);

  std::vector<bp::ManifestEntry> before;
  test_support::Check(bp::BuildSourceManifest(source, &filter, &before, &error),
                      "EXCL-02 filtered manifest builds", error);

  // 被排除的文件发生变化：实际备份集合没变，因此不许触发新快照。
  Write(source + "/skip.log", "log changed completely");
  std::vector<bp::ManifestEntry> after_excluded;
  test_support::Check(
      bp::BuildSourceManifest(source, &filter, &after_excluded, &error),
      "EXCL-03 filtered manifest rebuilds", error);
  bp::ChangeSummary summary;
  DiffOf(before, after_excluded, &summary);
  CheckSummary("EXCL-04 an excluded file change does not change the set", summary,
               0, 0, 0, 0);

  // 没有被排除的文件发生变化则必须被看见。
  Write(source + "/keep.txt", "keep changed");
  std::vector<bp::ManifestEntry> after_included;
  test_support::Check(
      bp::BuildSourceManifest(source, &filter, &after_included, &error),
      "EXCL-05 filtered manifest rebuilds again", error);
  DiffOf(before, after_included, &summary);
  test_support::Check(summary.modified >= 1,
                      "EXCL-06 an included file change is detected",
                      std::to_string(summary.modified));

  // 新增一个被排除的文件：也不该改变集合。
  Write(source + "/another.log", "x");
  std::vector<bp::ManifestEntry> after_new_excluded;
  test_support::Check(
      bp::BuildSourceManifest(source, &filter, &after_new_excluded, &error),
      "EXCL-07 filtered manifest rebuilds with a new excluded file", error);
  DiffOf(after_included, after_new_excluded, &summary);
  CheckSummary("EXCL-08 a new excluded file does not change the set", summary, 0,
               0, 0, 0);
}

}  // namespace

int main() {
  std::printf("scheduler core test\n");
  TestSimpleJson();
  TestManifestFromTree();
  TestManifestRejectsBadInput();
  TestChangeDetection();
  TestChangeDetectionFromTrees();
  test_support::RemoveTree(test_support::TempRoot());
  return test_support::Finish("scheduler core");
}
