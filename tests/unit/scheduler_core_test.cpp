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

#include <algorithm>
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

// 一份完整的 manifest 归属。三个字段都不是随便填的：repository_identity 与
// source_path 用真实路径，snapshot_file_name 用合法的单组件 .bak 名字。
bp::ManifestBinding MakeBinding(const std::string& snapshot_file_name) {
  bp::ManifestBinding binding;
  binding.snapshot_file_name = snapshot_file_name;
  binding.repository_identity = "/home/u/repo";
  binding.source_path = "/home/u/src";
  return binding;
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
    test_support::Check(
        version != nullptr && version->is_number() && version->number == 1,
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
    test_support::Check(
        nothing != nullptr && nothing->kind == bp::JsonValue::Kind::kNull,
        "JSON-06 null literal");
    test_support::Check(value.Find("missing") == nullptr,
                        "JSON-07 missing key returns nullptr");
  }

  {
    bp::JsonValue value;
    test_support::Check(
        !bp::ParseJson(R"JSON({"a": 1, "a": 2})JSON", &value, &error) &&
            error.find("duplicate") != std::string::npos,
        "JSON-08 duplicate key is rejected", error);
  }
  {
    bp::JsonValue value;
    test_support::Check(
        !bp::ParseJson(R"JSON({"a": "\u0041"})JSON", &value, &error) &&
            error.find("unsupported JSON escape") != std::string::npos,
        "JSON-09 unicode escape is rejected", error);
  }
  // \u 只认写侧真正会产出的那一格：\u00XX 且 XX 落在 C0 控制区。
  {
    bp::JsonValue value;
    test_support::Check(
        bp::ParseJson(R"JSON({"a": "\u0001"})JSON", &value, &error),
        "JSON-09b the escape the writer emits parses", error);
    test_support::Check(
        !bp::ParseJson(R"JSON({"a": "\u00ff"})JSON", &value, &error) &&
            error.find("unsupported JSON escape") != std::string::npos,
        "JSON-09c a non-control \\u escape is still rejected", error);
    test_support::Check(
        !bp::ParseJson(R"JSON({"a": "\u1234"})JSON", &value, &error),
        "JSON-09d a surrogate-range \\u escape is still rejected", error);
    test_support::Check(
        !bp::ParseJson(R"JSON({"a": "\u0"})JSON", &value, &error),
        "JSON-09e a truncated \\u escape is rejected", error);
    test_support::Check(
        !bp::ParseJson(R"JSON({"a": "\u00zz"})JSON", &value, &error),
        "JSON-09f a non-hex \\u escape is rejected", error);
  }
  // 写出去的东西必须全都读得回来。这条不变式此前是断的：写侧把
  // 0x00-0x07 / 0x0B / 0x0E-0x1F 写成 \u00XX，读侧却拒绝一切 \uXXXX，
  // 于是 ScheduleStore 能写出一份自己再也读不进来的 schedule.json，
  // 而且连"再 set 一次"都修不好（set 也要先读）。
  {
    bool all_round_trip = true;
    std::string first_failure;
    for (int byte = 0; byte < 0x20; ++byte) {
      const std::string raw(1, static_cast<char>(byte));
      std::string encoded;
      bp::WriteJsonString(&encoded, raw);
      bp::JsonValue parsed;
      std::string parse_error;
      const bp::JsonValue* field = nullptr;
      const bool ok =
          bp::ParseJson("{\"k\": " + encoded + "}", &parsed, &parse_error) &&
          (field = parsed.Find("k")) != nullptr && field->is_string() &&
          field->text == raw;
      if (!ok) {
        all_round_trip = false;
        if (first_failure.empty()) {
          char hex[8];
          std::snprintf(hex, sizeof(hex), "%02x", byte);
          first_failure = std::string("byte 0x") + hex + " encoded as " +
                          encoded + " : " + parse_error;
        }
      }
    }
    test_support::Check(
        all_round_trip,
        "JSON-09g every C0 control byte survives write then parse",
        first_failure);
  }
  {
    bp::JsonValue value;
    test_support::Check(
        !bp::ParseJson(R"JSON({"a": 1} trailing)JSON", &value, &error) &&
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
    test_support::Check(
        !bp::ParseJson(R"JSON({"a": 1.5})JSON", &value, &error) &&
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
    test_support::Check(
        !bp::ParseJson(R"JSON({"a": 007})JSON", &value, &error) &&
            error.find("leading zeros") != std::string::npos,
        "JSON-15 leading zeros are rejected", error);
  }
  {
    // 32 层嵌套超过 kMaxJsonDepth。
    std::string deep;
    for (int index = 0; index < 32; ++index) deep += "[";
    for (int index = 0; index < 32; ++index) deep += "]";
    bp::JsonValue value;
    test_support::Check(
        !bp::ParseJson(deep, &value, &error) &&
            error.find("nesting is too deep") != std::string::npos,
        "JSON-16 deep nesting is rejected", error);
  }
  {
    bp::JsonValue value;
    test_support::Check(
        !bp::ParseJson(R"JSON({"a": "unterminated})JSON", &value, &error) &&
            error.find("unterminated") != std::string::npos,
        "JSON-17 unterminated string is rejected", error);
  }
  {
    bp::JsonValue value;
    test_support::Check(
        !bp::ParseJson(R"JSON({"a": 1 "b": 2})JSON", &value, &error),
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
    test_support::Check(
        bp::RequireExactFields(value, {"a", "b"}, "demo", &error),
        "JSON-20 exact fields accepted", error);
    test_support::Check(!bp::RequireExactFields(value, {"a"}, "demo", &error) &&
                            error.find("unknown field") != std::string::npos,
                        "JSON-21 unknown field is rejected", error);
    test_support::Check(
        !bp::RequireExactFields(value, {"a", "b", "c"}, "demo", &error) &&
            error.find("missing required field") != std::string::npos,
        "JSON-22 missing field is rejected", error);
    // 可选字段：只有 optional 列表里的 key 才被额外放行。它是"新版本往已发布
    // 的 schema 里追加字段"的唯一通道，不是"放宽未知字段"。
    test_support::Check(
        bp::RequireExactFields(value, {"a"}, {"b"}, "demo", &error),
        "JSON-22b a declared optional field is accepted", error);
    test_support::Check(
        bp::RequireExactFields(value, {"a", "b"}, {"c"}, "demo", &error),
        "JSON-22c an absent optional field is accepted", error);
    test_support::Check(
        !bp::RequireExactFields(value, {"a"}, {"c"}, "demo", &error) &&
            error.find("unknown field") != std::string::npos,
        "JSON-22d a field outside fields+optional is still "
        "rejected",
        error);
    test_support::Check(
        !bp::RequireExactFields(value, {"a", "b", "c"}, {"b"}, "demo",
                                &error) &&
            error.find("missing required field") != std::string::npos,
        "JSON-22e optional does not excuse a missing required "
        "field",
        error);
    std::uint32_t number = 0;
    test_support::Check(
        bp::RequireUint32(value, "a", "demo", 0, 10, &number, &error) &&
            number == 1,
        "JSON-23 uint32 in range", error);
    test_support::Check(
        !bp::RequireUint32(value, "a", "demo", 5, 10, &number, &error) &&
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
  test_support::Check(
      bp::BuildSourceManifest(source, nullptr, &entries, &error),
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

  // 序列化往返。带上一份完整的 binding：生产路径写出的就是 version 2。
  const bp::ManifestBinding binding = MakeBinding("src-1.bak");
  const std::string text = bp::SerializeManifest(entries, binding);
  std::vector<bp::ManifestEntry> parsed;
  bp::ManifestBinding parsed_binding;
  test_support::Check(bp::ParseManifest(text, &parsed, &parsed_binding, &error),
                      "MAN-09 manifest round-trips", error);
  test_support::Check(
      parsed_binding.snapshot_file_name == "src-1.bak" &&
          parsed_binding.repository_identity == binding.repository_identity &&
          parsed_binding.source_path == binding.source_path,
      "MAN-09b the round trip keeps the baseline binding");
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
  bp::ManifestBinding weird_binding;
  test_support::Check(
      bp::ParseManifest(bp::SerializeManifest(weird, binding), &weird_parsed,
                        &weird_binding, &error) &&
          weird_parsed.size() == 3 && weird_parsed[0].archive_path == "a\tb" &&
          weird_parsed[1].archive_path == "c\nd" &&
          weird_parsed[2].archive_path == "e\\f",
      "MAN-12 field escaping survives a round trip", error);
}

void TestManifestRejectsBadInput() {
  test_support::Section("C. source_manifest rejects bad input");
  std::string error;
  std::vector<bp::ManifestEntry> entries;

  bp::ManifestBinding binding;
  test_support::Check(!bp::ParseManifest("", &entries, &binding, &error),
                      "MAN-20 empty manifest is rejected", error);
  test_support::Check(
      !bp::ParseManifest("NOTAMANIFEST\n", &entries, &binding, &error),
      "MAN-21 wrong header is rejected", error);
  test_support::Check(
      !bp::ParseManifest("BPMANIFEST1 2\n", &entries, &binding, &error),
      "MAN-22 missing entries are rejected", error);
  test_support::Check(
      !bp::ParseManifest("BPMANIFEST1 1\n2\t1\t1\t1\t644\t0\t0\t0\t0\t0\t.\n",
                         &entries, &binding, &error),
      "MAN-23 a wrong field count is rejected", error);
  test_support::Check(
      !bp::ParseManifest("BPMANIFEST1 1\n8\t0\t0\t0\t644\t0\t0\t0\t0\t0\t.\t\n",
                         &entries, &binding, &error),
      "MAN-24 the never-written socket type is rejected", error);
  test_support::Check(
      !bp::ParseManifest("BPMANIFEST1 0\n2\t0\t0\t0\t644\t0\t0\t0\t0\t0\ta\t\n",
                         &entries, &binding, &error),
      "MAN-25 trailing data after the declared count is rejected", error);
  test_support::Check(
      !bp::ParseManifest(
          "BPMANIFEST1 1\n2\t5\t0\t0\t644\t0\t0\t0\t0\t0\t/x\t\n", &entries,
          &binding, &error),
      "MAN-26 an absolute archive path is rejected", error);
  test_support::Check(
      !bp::ParseManifest("BPMANIFEST1 2\n2\t0\t0\t0\t644\t0\t0\t0\t0\t0\ta\t\n"
                         "2\t0\t0\t0\t644\t0\t0\t0\t0\t0\ta\t\n",
                         &entries, &binding, &error),
      "MAN-27 a duplicate path is rejected", error);
  // 空 manifest 是**合法**的 v1（0 条），但它没有 binding。
  test_support::Check(bp::ParseManifest(bp::SerializeManifestV1({}), &entries,
                                        &binding, &error) &&
                          entries.empty() && binding.empty(),
                      "MAN-28 a version 1 manifest still parses, without a "
                      "binding",
                      error);
}

// 手工拼一个 v2 头行。字段之间是 TAB，头行以换行结束。
std::string V2Header(const std::string& count, const std::string& snapshot,
                     const std::string& repository, const std::string& source) {
  return "BPMANIFEST2 " + count + "\t" + snapshot + "\t" + repository + "\t" +
         source + "\n";
}

void TestManifestBindingFormat() {
  test_support::Section("C2. manifest version 2 carries the baseline binding");
  std::string error;
  std::vector<bp::ManifestEntry> entries;
  bp::ManifestBinding binding;

  // ---- 写出：归属不完整就什么都不写 ----
  test_support::Check(bp::SerializeManifest({}, bp::ManifestBinding()).empty(),
                      "MAN-30 an empty binding cannot be serialized");
  bp::ManifestBinding no_repository = MakeBinding("src-1.bak");
  no_repository.repository_identity.clear();
  test_support::Check(bp::SerializeManifest({}, no_repository).empty(),
                      "MAN-31 a binding without a repository is refused");
  bp::ManifestBinding no_source = MakeBinding("src-1.bak");
  no_source.source_path.clear();
  test_support::Check(bp::SerializeManifest({}, no_source).empty(),
                      "MAN-32 a binding without a source is refused");
  // 快照名必须是单组件："带路径分隔符"与"."/".." 都不是合法的归档名。
  test_support::Check(
      bp::SerializeManifest({}, MakeBinding("sub/dir.bak")).empty(),
      "MAN-33 a snapshot name with a slash is refused");
  test_support::Check(bp::SerializeManifest({}, MakeBinding("..")).empty(),
                      "MAN-34 a dot-dot snapshot name is refused");

  // ---- 读入：头行结构必须严格 ----
  const std::string good =
      V2Header("0", "src-1.bak", "/home/u/repo", "/home/u/src");
  test_support::Check(bp::ParseManifest(good, &entries, &binding, &error) &&
                          entries.empty() &&
                          binding.snapshot_file_name == "src-1.bak",
                      "MAN-35 a well-formed version 2 header parses", error);
  test_support::Check(
      !bp::ParseManifest("BPMANIFEST2 0\tsrc-1.bak\t/home/u/repo\n", &entries,
                         &binding, &error),
      "MAN-36 a three-field version 2 header is rejected", error);
  test_support::Check(
      !bp::ParseManifest("BPMANIFEST2 0\tsrc-1.bak\t/home/u/repo\t/home/u/"
                         "src\textra\n",
                         &entries, &binding, &error),
      "MAN-37 a five-field version 2 header is rejected", error);
  test_support::Check(
      !bp::ParseManifest(V2Header("0", "", "/home/u/repo", "/home/u/src"),
                         &entries, &binding, &error),
      "MAN-38 an empty snapshot name is rejected", error);
  test_support::Check(
      !bp::ParseManifest(
          V2Header("0", "sub/dir.bak", "/home/u/repo", "/home/u/src"), &entries,
          &binding, &error),
      "MAN-39 a snapshot name with a slash is rejected", error);
  test_support::Check(
      !bp::ParseManifest(V2Header("0", "src-1.bak", "", "/home/u/src"),
                         &entries, &binding, &error),
      "MAN-40 an empty repository identity is rejected", error);
  test_support::Check(
      !bp::ParseManifest(
          V2Header("0", "src-1.bak", "/home/u/repo", "/home/u/src") + "stray\n",
          &entries, &binding, &error),
      "MAN-41 trailing data is still rejected in version 2", error);
  test_support::Check(
      !bp::ParseManifest(
          V2Header("0", std::string(5000, 'x'), "/home/u/repo", "/home/u/src"),
          &entries, &binding, &error),
      "MAN-42 an over-long binding field is rejected", error);

  // ---- 转义：binding 的字符串字段与条目字段走同一套转义 ----
  {
    bp::ManifestBinding odd = MakeBinding("src-1.bak");
    odd.repository_identity = "/home/u/back\\slash";
    odd.source_path = "/home/u/with\ttab";
    const std::string text = bp::SerializeManifest({}, odd);
    std::size_t tabs = 0;
    for (std::size_t index = 0; index < text.find('\n'); ++index) {
      if (text[index] == '\t') ++tabs;
    }
    test_support::Check(
        tabs == 3, "MAN-43 the header keeps exactly three field separators",
        std::to_string(tabs));
    bp::ManifestBinding back;
    test_support::Check(
        bp::ParseManifest(text, &entries, &back, &error) &&
            back.snapshot_file_name == odd.snapshot_file_name &&
            back.repository_identity == odd.repository_identity &&
            back.source_path == odd.source_path,
        "MAN-44 binding escaping survives a round trip", error);
  }

  // ---- v1 / v2 的条目正文完全一样，只有头行不同 ----
  {
    const std::vector<bp::ManifestEntry> sample = {
        MakeEntry(".", bp::EntryType::kDirectory),
        MakeEntry("a.txt", bp::EntryType::kRegularFile),
    };
    const std::string v1 = bp::SerializeManifestV1(sample);
    const std::string v2 =
        bp::SerializeManifest(sample, MakeBinding("src-1.bak"));
    test_support::Check(
        v1.compare(0, 11, "BPMANIFEST1") == 0 &&
            v2.compare(0, 11, "BPMANIFEST2") == 0,
        "MAN-45 the two writers emit different version headers");
    test_support::Check(v1.substr(v1.find('\n')) == v2.substr(v2.find('\n')),
                        "MAN-46 the entry bodies are byte-identical");
    // 同一份正文，一个带归属一个不带：这正是升级路径要区分的那件事。
    bp::ManifestBinding from_v1;
    bp::ManifestBinding from_v2;
    test_support::Check(
        bp::ParseManifest(v1, &entries, &from_v1, &error) && from_v1.empty(),
        "MAN-47 a version 1 manifest parses with no binding", error);
    test_support::Check(
        bp::ParseManifest(v2, &entries, &from_v2, &error) && !from_v2.empty(),
        "MAN-48 a version 2 manifest parses with its binding", error);
  }
}

void TestBaselineBindingComparison() {
  test_support::Section(
      "C3. the manifest binding must match the state baseline");
  const bp::ScheduleBaseline baseline =
      bp::BaselineOf(MakeBinding("src-1.bak"));

  test_support::Check(
      bp::SameBaselineBinding(baseline, MakeBinding("src-1.bak")),
      "MAN-50 an identical binding matches");
  // 旧格式没有归属信息，一律不可信——这里绝不猜。
  test_support::Check(!bp::SameBaselineBinding(baseline, bp::ManifestBinding()),
                      "MAN-51 a version 1 manifest never matches");
  test_support::Check(!bp::SameBaselineBinding(bp::ScheduleBaseline{},
                                               MakeBinding("src-1.bak")),
                      "MAN-52 a missing baseline never matches");

  bp::ManifestBinding other_snapshot = MakeBinding("src-1.bak");
  other_snapshot.snapshot_file_name = "src-2.bak";
  test_support::Check(!bp::SameBaselineBinding(baseline, other_snapshot),
                      "MAN-53 a different snapshot name does not match");
  bp::ManifestBinding other_repository = MakeBinding("src-1.bak");
  other_repository.repository_identity = "/home/u/other-repo";
  test_support::Check(!bp::SameBaselineBinding(baseline, other_repository),
                      "MAN-54 a different repository does not match");
  bp::ManifestBinding other_source = MakeBinding("src-1.bak");
  other_source.source_path = "/home/u/other-src";
  test_support::Check(!bp::SameBaselineBinding(baseline, other_source),
                      "MAN-55 a different source does not match");

  // 转换必须是双向无损的：两边字段一一对应，漏一个就会让上面每条判断失效。
  const bp::ScheduleBaseline round_trip =
      bp::BaselineOf(bp::BindingOf(baseline));
  test_support::Check(
      round_trip.snapshot_file_name == baseline.snapshot_file_name &&
          round_trip.repository_identity == baseline.repository_identity &&
          round_trip.source_path == baseline.source_path,
      "MAN-56 baseline and binding convert losslessly");
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
    CheckSummary("DIFF-06 mtime nanosecond change is modified", summary, 0, 0,
                 1, 0);
  }
  {
    std::vector<bp::ManifestEntry> current = base;
    current[1].mode = 0600;
    DiffOf(base, current, &summary);
    CheckSummary("DIFF-07 mode-only change is metadata_changed", summary, 0, 0,
                 0, 1);
  }
  {
    std::vector<bp::ManifestEntry> current = base;
    current[1].uid = 4242;
    DiffOf(base, current, &summary);
    CheckSummary("DIFF-08 uid-only change is metadata_changed", summary, 0, 0,
                 0, 1);
  }
  {
    std::vector<bp::ManifestEntry> current = base;
    current[1].gid = 4243;
    DiffOf(base, current, &summary);
    CheckSummary("DIFF-09 gid-only change is metadata_changed", summary, 0, 0,
                 0, 1);
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
    CheckSummary("DIFF-13 directory mtime is a documented blind spot", summary,
                 0, 0, 0, 0);
  }
  {
    std::vector<bp::ManifestEntry> previous = {
        MakeEntry("dev", bp::EntryType::kCharDevice)};
    std::vector<bp::ManifestEntry> current = previous;
    current[0].dev_major = 1;
    current[0].dev_minor = 3;
    DiffOf(previous, current, &summary);
    CheckSummary("DIFF-14 char device major/minor change is modified", summary,
                 0, 0, 1, 0);

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
    CheckSummary("DIFF-16 hardlink degree change is metadata_changed", summary,
                 0, 0, 0, 1);
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
  test_support::Check(
      filter.AddRule(bp::FilterAction::kExclude, "ext:log", &error),
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
  CheckSummary("EXCL-04 an excluded file change does not change the set",
               summary, 0, 0, 0, 0);

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
  CheckSummary("EXCL-08 a new excluded file does not change the set", summary,
               0, 0, 0, 0);
}

// ---- 目录 mtime 合同 ------------------------------------------------------
//
// 这一组把"目录 mtime 只影响 restore、不影响 schedule 触发"这条最终产品语义
// 逐项钉住：实现（source_manifest.cpp 的 DiffManifests）、测试、文档三者必须
// 说同一件事。
//
// 为什么不加所谓的 Strict Metadata 模式：那样"同一份源目录"就有了两种触发
// 定义，GUI 与 CLI、文档与代码都会各自漂移。产品只有这一套定义。
void TestDirectoryMtimeContract() {
  test_support::Section("DIFF-DIR. directory mtime contract");

  const std::vector<bp::ManifestEntry> base = {
      MakeEntry("dir", bp::EntryType::kDirectory),
      MakeEntry("dir/keep.txt", bp::EntryType::kRegularFile),
      MakeEntry("dir/sub", bp::EntryType::kDirectory),
  };
  bp::ChangeSummary summary;

  // 1) 仅仅目录自身的 mtime 改变 -> 不触发。
  {
    std::vector<bp::ManifestEntry> current = base;
    current[0].mtime_sec += 7;
    current[0].mtime_nsec = 12345;
    DiffOf(base, current, &summary);
    CheckSummary("DIR-01 directory mtime only does not trigger", summary, 0, 0,
                 0, 0);
  }

  // 2) 目录的 mode / uid / gid 是会影响 restore 结果的 metadata -> 触发。
  {
    std::vector<bp::ManifestEntry> current = base;
    current[0].mode = 0700;
    DiffOf(base, current, &summary);
    CheckSummary("DIR-02 directory mode change triggers", summary, 0, 0, 0, 1);
  }
  {
    std::vector<bp::ManifestEntry> current = base;
    current[0].uid += 1;
    DiffOf(base, current, &summary);
    CheckSummary("DIR-03 directory uid change triggers", summary, 0, 0, 0, 1);
  }
  {
    std::vector<bp::ManifestEntry> current = base;
    current[0].gid += 1;
    DiffOf(base, current, &summary);
    CheckSummary("DIR-04 directory gid change triggers", summary, 0, 0, 0, 1);
  }

  // 3) 子项的新增 / 删除 / 改名由 path 本身的变化检测，与父目录无关。
  {
    std::vector<bp::ManifestEntry> current = base;
    current.push_back(MakeEntry("dir/new.txt", bp::EntryType::kRegularFile));
    // 真实的子项新增一定会顺带改掉父目录 mtime：这里一起改掉，验证"只算一次、
    // 不算成 metadata_changed"。
    current[0].mtime_sec += 3;
    DiffOf(base, current, &summary);
    CheckSummary("DIR-05 a new child is added, the parent mtime is not counted",
                 summary, 1, 0, 0, 0);
  }
  {
    std::vector<bp::ManifestEntry> current = base;
    current.erase(current.begin() + 1);
    current[0].mtime_sec += 3;
    DiffOf(base, current, &summary);
    CheckSummary(
        "DIR-06 a removed child is removed, the parent mtime is not "
        "counted",
        summary, 0, 1, 0, 0);
  }
  {
    // 改名 = 一条 added + 一条 removed（manifest 按路径排好序，没有 inode 身份
    // 可用，也不假装有）。
    std::vector<bp::ManifestEntry> current = base;
    current[1].archive_path = "dir/renamed.txt";
    std::sort(current.begin(), current.end(),
              [](const bp::ManifestEntry& a, const bp::ManifestEntry& b) {
                return a.archive_path < b.archive_path;
              });
    current[0].mtime_sec += 3;
    DiffOf(base, current, &summary);
    CheckSummary("DIR-07 a rename is add + remove", summary, 1, 1, 0, 0);
  }
}

// 真实文件系统上的同一条合同：只 touch 目录不触发；被 filter 排除的子项增删
// 也不触发；被包含的子项增删触发。不 sleep：全部靠显式改 mtime 与重建
// manifest。
void TestDirectoryMtimeContractOnRealTree() {
  test_support::Section("DIFF-DIR-FS. the same contract on a real tree");

  const std::string root = test_support::FreshDir("dir-contract");
  const std::string source = root + "/src";
  const std::string build = source + "/build";
  test_support::Mkdir(source, 0755);
  test_support::Mkdir(build, 0755);
  Write(source + "/keep.txt", "keep");
  Write(build + "/excluded.o", "object");

  bp::Filter filter;
  std::string error;
  test_support::Check(
      filter.AddRule(bp::FilterAction::kExclude, "path:**/build/**", &error),
      "DIR-08 the exclude rule is accepted", error);

  std::vector<bp::ManifestEntry> before;
  test_support::Check(bp::BuildSourceManifest(source, &filter, &before, &error),
                      "DIR-09 the filtered manifest builds", error);

  // 只改源目录自己的 mtime：备份集合一个字都没变。
  test_support::Check(test_support::SetTimes(source, 1600000000, 0),
                      "DIR-10 the source directory is touched");
  std::vector<bp::ManifestEntry> touched;
  test_support::Check(
      bp::BuildSourceManifest(source, &filter, &touched, &error),
      "DIR-11 the manifest rebuilds after the touch", error);
  bp::ChangeSummary summary;
  DiffOf(before, touched, &summary);
  CheckSummary("DIR-12 touching a directory alone never triggers", summary, 0,
               0, 0, 0);

  // 被排除的子项增删：父目录 mtime 一定会变，但集合没变。
  Write(build + "/another.o", "object two");
  std::vector<bp::ManifestEntry> excluded_added;
  test_support::Check(
      bp::BuildSourceManifest(source, &filter, &excluded_added, &error),
      "DIR-13 the manifest rebuilds after an excluded child is added", error);
  DiffOf(touched, excluded_added, &summary);
  CheckSummary("DIR-14 an excluded child add never triggers", summary, 0, 0, 0,
               0);

  std::string ignored;
  test_support::Check(::unlink((build + "/another.o").c_str()) == 0,
                      "DIR-15 the excluded child is removed again", ignored);
  std::vector<bp::ManifestEntry> excluded_removed;
  test_support::Check(
      bp::BuildSourceManifest(source, &filter, &excluded_removed, &error),
      "DIR-16 the manifest rebuilds after an excluded child is removed", error);
  DiffOf(excluded_added, excluded_removed, &summary);
  CheckSummary("DIR-17 an excluded child removal never triggers", summary, 0, 0,
               0, 0);

  // 被包含的子项新增：必须触发。
  Write(source + "/new.txt", "new");
  std::vector<bp::ManifestEntry> included_added;
  test_support::Check(
      bp::BuildSourceManifest(source, &filter, &included_added, &error),
      "DIR-18 the manifest rebuilds after an included child is added", error);
  DiffOf(excluded_removed, included_added, &summary);
  test_support::Check(
      summary.added == 1 && summary.removed == 0 && summary.modified == 0 &&
          summary.metadata_changed == 0,
      "DIR-19 an included child add triggers exactly once",
      std::to_string(summary.added) + "/" + std::to_string(summary.removed) +
          "/" + std::to_string(summary.modified) + "/" +
          std::to_string(summary.metadata_changed));
}

// ---- 数字选项解析合同 ------------------------------------------------------
//
// CLI 的 --interval-minutes/--retain 与 GUI 文本框走的是同一个
// ParseBoundedScheduleNumber。这张 case table 是"两个前端结论必须一致"的
// 唯一判据，GUI 侧的 --schedule-test 会把同一张表再喂一遍并逐项比对。
void TestNumberParserContract() {
  test_support::Section("NUM. shared number parser contract");

  struct Case {
    const char* text;
    bool accepted;
    std::uint32_t value;
  };
  const Case cases[] = {
      {"5", true, 5},
      {"60", true, 60},
      // 前后空白由共享 parser 自己处理：CLI 送 argv 原文、GUI 送文本框原文，
      // 两边都不预处理，所以不可能一边接受一边拒绝。
      {" 5 ", true, 5},
      {"\t5\t", true, 5},
      {"\n60\r", true, 60},
      {"007", true, 7},
      // 空与纯空白
      {"", false, 0},
      {"   ", false, 0},
      {"\t", false, 0},
      // 非纯数字
      {"5x", false, 0},
      {"x5", false, 0},
      {"-1", false, 0},
      {"+5", false, 0},
      {"1.0", false, 0},
      {"5 5", false, 0},
      {"0x10", false, 0},
      // 溢出：边解析边夹，不做回绕
      {"99999999999999999999", false, 0},
      {"18446744073709551616", false, 0},
      // 范围
      {"0", false, 0},
      {"525601", false, 0},
  };
  for (const Case& item : cases) {
    std::uint32_t value = 0;
    std::string error;
    const bool ok = bp::ParseBoundedScheduleNumber(
        item.text, 1, 525600, "--interval-minutes", &value, &error);
    test_support::Check(ok == item.accepted,
                        std::string("NUM-01 [") + item.text +
                            "] accepted=" + (item.accepted ? "yes" : "no"),
                        error);
    if (item.accepted) {
      test_support::Check(
          ok && value == item.value,
          std::string("NUM-02 [") + item.text + "] parsed value",
          std::to_string(value));
    } else {
      test_support::Check(
          !ok && !error.empty(),
          std::string("NUM-03 [") + item.text + "] rejection explains itself");
    }
  }

  // 同一个函数、同一个上界，用于 retain：范围不同、规则相同。
  {
    std::uint32_t value = 0;
    std::string error;
    test_support::Check(bp::ParseBoundedScheduleNumber(
                            " 12 ", 1, 1000, "--retain", &value, &error) &&
                            value == 12,
                        "NUM-04 the retain range uses the same parser", error);
    test_support::Check(!bp::ParseBoundedScheduleNumber(
                            "1001", 1, 1000, "--retain", &value, &error),
                        "NUM-05 retain above the maximum is refused", error);
  }
}

}  // namespace

int main() {
  std::printf("scheduler core test\n");
  TestSimpleJson();
  TestManifestFromTree();
  TestManifestRejectsBadInput();
  TestManifestBindingFormat();
  TestBaselineBindingComparison();
  TestChangeDetection();
  TestChangeDetectionFromTrees();
  TestDirectoryMtimeContract();
  TestDirectoryMtimeContractOnRealTree();
  TestNumberParserContract();
  test_support::RemoveTree(test_support::TempRoot());
  return test_support::Finish("scheduler core");
}
