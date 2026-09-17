// tests/unit/filter_rule_builder_test.cpp
//
// 可视化规则编辑器中间层的单元测试。
//
// 两种跑法共用一份测试代码：装了 GoogleTest 就用它（自己提供 main，为了支持
// 下面的 --emit 模式，因此只链 -lgtest -pthread）；没装就用内置无依赖 harness。
//
// 除普通断言外还有一条关键约束：builder 生成的每一条 DSL 都必须能被真实的
// backupproject::Filter::AddRule 接受。这是"GUI 是规则构造器、不是第二个匹配
// 引擎"的可执行证据。

#include "filter_rule_builder.h"

#include <cstdio>
#include <string>
#include <vector>

namespace bp = backupproject;

#if defined(__has_include)
#  if __has_include(<gtest/gtest.h>)
#    include <gtest/gtest.h>
#    define L3_HAVE_GTEST 1
#  endif
#endif

#ifndef L3_HAVE_GTEST
namespace l3 {
struct Case { const char* name; void (*fn)(); };
inline std::vector<Case>& Registry() { static std::vector<Case> r; return r; }
inline int& Failures() { static int f = 0; return f; }
inline int& Checks() { static int c = 0; return c; }
struct Reg { Reg(const char* n, void (*f)()) { Registry().push_back(Case{n, f}); } };
}  // namespace l3
#define L3_TEST(suite, name)                                                  \
  static void suite##_##name();                                               \
  static ::l3::Reg l3_reg_##suite##_##name(#suite "." #name, suite##_##name); \
  static void suite##_##name()
#define L3_FAIL(expr)                                                       \
  do {                                                                      \
    ++::l3::Failures();                                                     \
    std::printf("    CHECK FAILED: %s (%s:%d)\n", expr, __FILE__, __LINE__); \
  } while (0)
#define EXPECT_TRUE(x) do { ++::l3::Checks(); if (!(x)) L3_FAIL(#x); } while (0)
#define EXPECT_FALSE(x) do { ++::l3::Checks(); if (x) L3_FAIL("!(" #x ")"); } while (0)
#define EXPECT_EQ(a, b) do { ++::l3::Checks(); if (!((a) == (b))) L3_FAIL(#a " == " #b); } while (0)
#define TEST(suite, name) L3_TEST(suite, name)
#endif

namespace {

bp::FilterClauseDraft ExtClause(std::vector<std::string> exts) {
  bp::FilterClauseDraft c;
  c.field = bp::RuleField::kExt;
  c.extensions = std::move(exts);
  return c;
}

bp::FilterClauseDraft PatternClause(bp::RuleField field, std::string pattern) {
  bp::FilterClauseDraft c;
  c.field = field;
  c.pattern = std::move(pattern);
  return c;
}

bp::FilterClauseDraft SizeClause(bp::RuleSizeCompare compare, std::uint64_t low,
                                 std::uint64_t high, bp::RuleSizeUnit unit) {
  bp::FilterClauseDraft c;
  c.field = bp::RuleField::kSize;
  c.compare = compare;
  c.size_low = low;
  c.size_high = high;
  c.unit = unit;
  return c;
}

bp::FilterRuleDraft Rule(bp::FilterAction action,
                         std::vector<bp::FilterClauseDraft> clauses) {
  bp::FilterRuleDraft r;
  r.action = action;
  r.clauses = std::move(clauses);
  return r;
}

std::string Dsl(const bp::FilterRuleDraft& rule) {
  std::string text;
  bp::ToDsl(rule, &text, nullptr);
  return text;
}

// 集成测试用的固定场景：GUI 上点几下就应该产出这些规则。
std::vector<bp::FilterRuleDraft> Scenario(const std::string& name, bool* valid) {
  *valid = true;
  if (name == "ext_txt_md") {
    return {Rule(bp::FilterAction::kInclude, {ExtClause({"txt", "md"})})};
  }
  if (name == "exclude_build") {
    return {Rule(bp::FilterAction::kExclude,
                 {PatternClause(bp::RuleField::kPath, "**/build/**")})};
  }
  if (name == "include_txt_exclude_secret") {
    return {Rule(bp::FilterAction::kInclude, {ExtClause({"txt"})}),
            Rule(bp::FilterAction::kExclude,
                 {PatternClause(bp::RuleField::kName, "secret.txt")})};
  }
  if (name == "multi_clause_folder_cache") {
    bp::FilterClauseDraft type;
    type.field = bp::RuleField::kType;
    type.type = bp::RuleTypeValue::kFolder;
    return {Rule(bp::FilterAction::kExclude,
                 {type, PatternClause(bp::RuleField::kPath, "**/cache")})};
  }
  if (name == "invalid_empty_ext") {
    *valid = false;
    return {Rule(bp::FilterAction::kInclude, {ExtClause({})})};
  }
  *valid = false;
  return {};
}

}  // namespace

TEST(Field, NamesMatchCoreSyntax) {
  EXPECT_EQ(std::string(bp::RuleFieldName(bp::RuleField::kName)), std::string("name"));
  EXPECT_EQ(std::string(bp::RuleFieldName(bp::RuleField::kPath)), std::string("path"));
  EXPECT_EQ(std::string(bp::RuleFieldName(bp::RuleField::kStem)), std::string("stem"));
  EXPECT_EQ(std::string(bp::RuleFieldName(bp::RuleField::kExt)), std::string("ext"));
  EXPECT_EQ(std::string(bp::RuleFieldName(bp::RuleField::kType)), std::string("type"));
  EXPECT_EQ(std::string(bp::RuleFieldName(bp::RuleField::kSize)), std::string("size"));
  EXPECT_EQ(std::string(bp::RuleFieldName(bp::RuleField::kMtime)), std::string("mtime"));
}

TEST(Ext, SerializesSemicolonList) {
  EXPECT_EQ(Dsl(Rule(bp::FilterAction::kInclude, {ExtClause({"txt", "md"})})),
            std::string("ext:txt;md"));
  EXPECT_EQ(Dsl(Rule(bp::FilterAction::kInclude, {ExtClause({"cpp", "h", "hpp"})})),
            std::string("ext:cpp;h;hpp"));
}

TEST(Ext, NormalizesDotsSpacesAndEmptyEntries) {
  EXPECT_EQ(Dsl(Rule(bp::FilterAction::kInclude,
                     {ExtClause({".txt", " md ", "", "  "})})),
            std::string("ext:txt;md"));
}

TEST(Ext, EmptyListIsRejected) {
  bp::FilterRuleDraft rule = Rule(bp::FilterAction::kInclude, {ExtClause({})});
  std::string error;
  EXPECT_FALSE(bp::ValidateClause(rule.clauses[0], &error));
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(bp::ValidateRule(rule, &error));
}

TEST(Ext, SingleExtension) {
  EXPECT_EQ(Dsl(Rule(bp::FilterAction::kExclude, {ExtClause({"log"})})),
            std::string("ext:log"));
}

TEST(Pattern, NamePathStem) {
  EXPECT_EQ(Dsl(Rule(bp::FilterAction::kInclude,
                     {PatternClause(bp::RuleField::kName, "*.txt")})),
            std::string("name:*.txt"));
  EXPECT_EQ(Dsl(Rule(bp::FilterAction::kExclude,
                     {PatternClause(bp::RuleField::kPath, "**/build/**")})),
            std::string("path:**/build/**"));
  EXPECT_EQ(Dsl(Rule(bp::FilterAction::kInclude,
                     {PatternClause(bp::RuleField::kStem, "report*")})),
            std::string("stem:report*"));
}

TEST(Pattern, EmptyPatternIsRejected) {
  std::string error;
  EXPECT_FALSE(bp::ValidateClause(PatternClause(bp::RuleField::kName, ""), &error));
  EXPECT_FALSE(error.empty());
}

TEST(Type, FileAndFolder) {
  bp::FilterClauseDraft folder;
  folder.field = bp::RuleField::kType;
  folder.type = bp::RuleTypeValue::kFolder;
  EXPECT_EQ(Dsl(Rule(bp::FilterAction::kExclude, {folder})), std::string("type:folder"));
  bp::FilterClauseDraft file;
  file.field = bp::RuleField::kType;
  file.type = bp::RuleTypeValue::kFile;
  EXPECT_EQ(Dsl(Rule(bp::FilterAction::kInclude, {file})), std::string("type:file"));
}

TEST(Size, BytesHaveNoSuffix) {
  EXPECT_EQ(Dsl(Rule(bp::FilterAction::kInclude,
                     {SizeClause(bp::RuleSizeCompare::kGreaterEqual, 512, 0,
                                 bp::RuleSizeUnit::kByte)})),
            std::string("size:>=512"));
}

TEST(Size, KiloMegaGigaUnits) {
  EXPECT_EQ(Dsl(Rule(bp::FilterAction::kInclude,
                     {SizeClause(bp::RuleSizeCompare::kGreaterEqual, 1, 0,
                                 bp::RuleSizeUnit::kKilo)})),
            std::string("size:>=1KB"));
  EXPECT_EQ(Dsl(Rule(bp::FilterAction::kInclude,
                     {SizeClause(bp::RuleSizeCompare::kLess, 1, 0,
                                 bp::RuleSizeUnit::kMega)})),
            std::string("size:<1MB"));
  EXPECT_EQ(Dsl(Rule(bp::FilterAction::kInclude,
                     {SizeClause(bp::RuleSizeCompare::kGreaterEqual, 2, 0,
                                 bp::RuleSizeUnit::kGiga)})),
            std::string("size:>=2GB"));
}

TEST(Size, RangeAndOperators) {
  EXPECT_EQ(Dsl(Rule(bp::FilterAction::kInclude,
                     {SizeClause(bp::RuleSizeCompare::kRange, 1, 10,
                                 bp::RuleSizeUnit::kMega)})),
            std::string("size:1MB..10MB"));
  EXPECT_EQ(Dsl(Rule(bp::FilterAction::kInclude,
                     {SizeClause(bp::RuleSizeCompare::kLessEqual, 100, 0,
                                 bp::RuleSizeUnit::kMega)})),
            std::string("size:<=100MB"));
  EXPECT_EQ(Dsl(Rule(bp::FilterAction::kInclude,
                     {SizeClause(bp::RuleSizeCompare::kGreater, 4, 0,
                                 bp::RuleSizeUnit::kKilo)})),
            std::string("size:>4KB"));
}

TEST(Size, ReversedRangeIsRejected) {
  std::string error;
  EXPECT_FALSE(bp::ValidateClause(
      SizeClause(bp::RuleSizeCompare::kRange, 10, 1, bp::RuleSizeUnit::kMega),
      &error));
  EXPECT_FALSE(error.empty());
}

TEST(Mtime, AllKinds) {
  bp::FilterClauseDraft c;
  c.field = bp::RuleField::kMtime;
  c.mtime_kind = bp::RuleMtimeKind::kToday;
  EXPECT_EQ(Dsl(Rule(bp::FilterAction::kInclude, {c})), std::string("mtime:today"));
  c.mtime_kind = bp::RuleMtimeKind::kYesterday;
  EXPECT_EQ(Dsl(Rule(bp::FilterAction::kInclude, {c})), std::string("mtime:yesterday"));
  c.mtime_kind = bp::RuleMtimeKind::kLastDays;
  c.days_back = 7;
  EXPECT_EQ(Dsl(Rule(bp::FilterAction::kInclude, {c})), std::string("mtime:7days"));
  c.mtime_kind = bp::RuleMtimeKind::kDay;
  c.date_low = "2026-09-01";
  EXPECT_EQ(Dsl(Rule(bp::FilterAction::kInclude, {c})), std::string("mtime:2026-09-01"));
  c.mtime_kind = bp::RuleMtimeKind::kDayRange;
  c.date_high = "2026-09-12";
  EXPECT_EQ(Dsl(Rule(bp::FilterAction::kInclude, {c})),
            std::string("mtime:2026-09-01..2026-09-12"));
}

TEST(Mtime, InvalidDatesAreRejected) {
  std::string error;
  bp::FilterClauseDraft c;
  c.field = bp::RuleField::kMtime;
  c.mtime_kind = bp::RuleMtimeKind::kDay;
  c.date_low = "2026-13-01";
  EXPECT_FALSE(bp::ValidateClause(c, &error));
  c.date_low = "2026-9-1";
  EXPECT_FALSE(bp::ValidateClause(c, &error));
  c.date_low = "not-a-date";
  EXPECT_FALSE(bp::ValidateClause(c, &error));
  c.mtime_kind = bp::RuleMtimeKind::kLastDays;
  c.days_back = 0;
  EXPECT_FALSE(bp::ValidateClause(c, &error));
}

TEST(Mtime, ReversedRangeIsRejected) {
  std::string error;
  bp::FilterClauseDraft c;
  c.field = bp::RuleField::kMtime;
  c.mtime_kind = bp::RuleMtimeKind::kDayRange;
  c.date_low = "2026-09-12";
  c.date_high = "2026-09-01";
  EXPECT_FALSE(bp::ValidateClause(c, &error));
}

TEST(Clauses, MultipleClausesJoinWithSpaceAsAnd) {
  bp::FilterClauseDraft type;
  type.field = bp::RuleField::kType;
  type.type = bp::RuleTypeValue::kFolder;
  const bp::FilterRuleDraft rule = Rule(
      bp::FilterAction::kExclude,
      {type, PatternClause(bp::RuleField::kPath, "**/cache")});
  EXPECT_EQ(Dsl(rule), std::string("type:folder path:**/cache"));
}

TEST(Rule, EmptyRuleIsRejected) {
  std::string error;
  EXPECT_FALSE(bp::ToDsl(Rule(bp::FilterAction::kInclude, {}), nullptr, &error));
  EXPECT_FALSE(error.empty());
}

TEST(Equivalence, EveryGeneratedDslIsAcceptedByTheRealCore) {
  const std::vector<std::string> names = {"ext_txt_md", "exclude_build",
                                          "include_txt_exclude_secret",
                                          "multi_clause_folder_cache"};
  for (const std::string& name : names) {
    bool valid = false;
    const std::vector<bp::FilterRuleDraft> rules = Scenario(name, &valid);
    EXPECT_TRUE(valid);
    for (const bp::FilterRuleDraft& rule : rules) {
      std::string error;
      EXPECT_TRUE(bp::ValidateRule(rule, &error));
      // 再直接问一次核心：builder 说是合法，核心也必须说合法。
      bp::Filter filter;
      EXPECT_TRUE(filter.AddRule(rule.action, Dsl(rule), &error));
    }
  }
}

TEST(Equivalence, InvalidDraftIsRejectedByBuilderAndCoreNeverSeesIt) {
  bool valid = false;
  const std::vector<bp::FilterRuleDraft> rules = Scenario("invalid_empty_ext", &valid);
  EXPECT_FALSE(valid);
  std::string error;
  EXPECT_FALSE(bp::ValidateRule(rules[0], &error));
  EXPECT_FALSE(error.empty());
  // 对照：核心对同样非法的文本也拒绝（GUI 只是把这句话提前到前端）。
  bp::Filter filter;
  EXPECT_FALSE(filter.AddRule(bp::FilterAction::kInclude, "bogus:x", &error));
}

TEST(Equivalence, AllSizeAndMtimeFormsAreAcceptedByTheCore) {
  // size 的每种序列化都交给真实核心裁决：字节不带单位、KB/MB/GB、五个运算符、区间。
  // 任何一条核心不认，这里就会红，而不是等到界面里才发现。
  struct SizeCase {
    bp::RuleSizeCompare compare;
    std::uint64_t low;
    std::uint64_t high;
    bp::RuleSizeUnit unit;
  };
  const std::vector<SizeCase> sizes = {
      {bp::RuleSizeCompare::kLess, 1, 0, bp::RuleSizeUnit::kMega},
      {bp::RuleSizeCompare::kLessEqual, 100, 0, bp::RuleSizeUnit::kMega},
      {bp::RuleSizeCompare::kGreater, 4, 0, bp::RuleSizeUnit::kKilo},
      {bp::RuleSizeCompare::kGreaterEqual, 1, 0, bp::RuleSizeUnit::kGiga},
      {bp::RuleSizeCompare::kRange, 1, 10, bp::RuleSizeUnit::kMega},
      {bp::RuleSizeCompare::kGreaterEqual, 512, 0, bp::RuleSizeUnit::kByte},
      {bp::RuleSizeCompare::kLess, 1024, 0, bp::RuleSizeUnit::kByte},
      {bp::RuleSizeCompare::kRange, 512, 2048, bp::RuleSizeUnit::kByte}};
  for (const SizeCase& c : sizes) {
    const bp::FilterRuleDraft rule = Rule(
        bp::FilterAction::kInclude, {SizeClause(c.compare, c.low, c.high, c.unit)});
    std::string error;
    EXPECT_TRUE(bp::ValidateRule(rule, &error));
  }
  const std::vector<std::pair<bp::RuleMtimeKind, std::string>> mtimes = {
      {bp::RuleMtimeKind::kToday, ""},
      {bp::RuleMtimeKind::kYesterday, ""},
      {bp::RuleMtimeKind::kLastDays, ""},
      {bp::RuleMtimeKind::kDay, "2026-09-01"},
      {bp::RuleMtimeKind::kDayRange, "2026-09-01"}};
  for (const std::pair<bp::RuleMtimeKind, std::string>& item : mtimes) {
    bp::FilterClauseDraft c;
    c.field = bp::RuleField::kMtime;
    c.mtime_kind = item.first;
    c.days_back = 7;
    c.date_low = item.second;
    c.date_high = "2026-09-12";
    const bp::FilterRuleDraft rule = Rule(bp::FilterAction::kInclude, {c});
    std::string error;
    EXPECT_TRUE(bp::ValidateRule(rule, &error));
  }
}

TEST(Summary, ClauseAndRuleText) {
  EXPECT_EQ(bp::Summarize(Rule(bp::FilterAction::kInclude, {ExtClause({"txt", "md"})})),
            std::string("包含：扩展名为 txt 或 md 的文件"));
  EXPECT_EQ(bp::Summarize(Rule(bp::FilterAction::kExclude,
                              {PatternClause(bp::RuleField::kPath, "**/build/**")})),
            std::string("排除：相对路径匹配 **/build/** 的条目"));
  EXPECT_EQ(bp::Summarize(Rule(bp::FilterAction::kInclude,
                              {SizeClause(bp::RuleSizeCompare::kRange, 1, 10,
                                          bp::RuleSizeUnit::kMega)})),
            std::string("包含：大小在 1 MB 到 10 MB 之间"));
}

TEST(Cli, ArgumentsAlternateFlagAndRuleText) {
  bool valid = false;
  const std::vector<bp::FilterRuleDraft> rules =
      Scenario("include_txt_exclude_secret", &valid);
  const std::vector<std::string> args = bp::CliArguments(rules);
  EXPECT_EQ(args.size(), static_cast<std::size_t>(4));
  EXPECT_EQ(args[0], std::string("--include"));
  EXPECT_EQ(args[1], std::string("ext:txt"));
  EXPECT_EQ(args[2], std::string("--exclude"));
  EXPECT_EQ(args[3], std::string("name:secret.txt"));
}

int main(int argc, char** argv) {
  // 集成测试用：把场景翻译成规则文本 / CLI 参数，供真实 backupctl 使用。
  if (argc >= 3) {
    const std::string mode = argv[1];
    if (mode == "--emit" || mode == "--emit-args") {
      bool valid = false;
      const std::vector<bp::FilterRuleDraft> rules = Scenario(argv[2], &valid);
      if (!valid || rules.empty()) {
        std::fprintf(stderr, "scenario '%s' is invalid\n", argv[2]);
        return 3;
      }
      for (const bp::FilterRuleDraft& rule : rules) {
        std::string error;
        if (!bp::ValidateRule(rule, &error)) {
          std::fprintf(stderr, "%s\n", error.c_str());
          return 3;
        }
        std::string dsl;
        bp::ToDsl(rule, &dsl, nullptr);
        if (mode == "--emit-args") {
          std::printf("%s\n%s\n",
                      rule.action == bp::FilterAction::kInclude ? "--include"
                                                                : "--exclude",
                      dsl.c_str());
        } else {
          std::printf("%s|%s\n",
                      rule.action == bp::FilterAction::kInclude ? "include"
                                                                : "exclude",
                      dsl.c_str());
        }
      }
      return 0;
    }
  }
#ifdef L3_HAVE_GTEST
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
#else
  int passed = 0;
  int failed = 0;
  for (const l3::Case& c : l3::Registry()) {
    const int before = l3::Failures();
    c.fn();
    if (l3::Failures() == before) { ++passed; std::printf("[  PASSED  ] %s\n", c.name); }
    else { ++failed; std::printf("[  FAILED  ] %s\n", c.name); }
  }
  std::printf("[harness] tests=%d passed=%d failed=%d checks=%d\n",
              static_cast<int>(l3::Registry().size()), passed, failed, l3::Checks());
  return failed == 0 ? 0 : 1;
#endif
}
