// filter_rule_builder.h
//
// 可视化规则编辑器的中间层：GUI 表单草稿 <-> 现有 Filter 规则语法。
//
// 它只做四件事：结构校验、DSL 序列化、人类可读摘要、CLI 参数拼装。
// 真正的语法裁决仍然交给 backupproject::Filter::AddRule —— GUI 层不实现
// 第二套匹配器，也不重复定义规则语法。
//
// 位置的取舍：放在 src/filter/ 而不是 src/ui/，因为它产出和消费的都是
// filter DSL，是纯 C++、零 Qt 依赖，CLI、两套 GUI 和单元测试可以直接复用。

#ifndef BACKUP_PROJECT_INCLUDE_FILTER_RULE_BUILDER_H_
#define BACKUP_PROJECT_INCLUDE_FILTER_RULE_BUILDER_H_

#include <cstdint>
#include <string>
#include <vector>

#include "filter.h"

namespace backupproject {

// 编辑器支持的字段，与 Filter 的 Clause::Field 一一对应。
enum class RuleField {
  kName,
  kPath,
  kStem,
  kExt,
  kType,
  kSize,
  kMtime,
  kUid,
  kGid,
  kUser,
  kGroup
};

// size / uid / gid 的比较运算符（range 对应核心的 kRange）。
// kEqual 追加在最后：已有取值的位置属于已经定型的序列化行为，不能插到中间。
enum class RuleSizeCompare {
  kLess,
  kLessEqual,
  kGreater,
  kGreaterEqual,
  kRange,
  kEqual
};

// 1024 进制，与文档固定下来的语义一致。
enum class RuleSizeUnit { kByte, kKilo, kMega, kGiga };

// type 下拉的取值，与 DSL 的 type: 一一对应。
enum class RuleTypeValue {
  kFile,
  kFolder,
  kSymlink,
  kFifo,
  kCharDevice,
  kBlockDevice,
  kSocket
};

enum class RuleMtimeKind { kToday, kYesterday, kLastDays, kDay, kDayRange };

// 一个子条件。字段之间互斥，用到哪个字段就填哪几个成员。
struct FilterClauseDraft {
  RuleField field = RuleField::kName;
  std::string pattern;                  // name / path / stem
  std::vector<std::string> extensions;  // ext
  RuleTypeValue type = RuleTypeValue::kFile;
  RuleSizeCompare compare = RuleSizeCompare::kGreaterEqual;
  std::uint64_t size_low = 0;   // 用户输入的数值（单位见 unit）
  std::uint64_t size_high = 0;  // 仅 kRange 使用
  RuleSizeUnit unit = RuleSizeUnit::kKilo;
  RuleMtimeKind mtime_kind = RuleMtimeKind::kToday;
  int days_back = 7;      // 仅 kLastDays
  std::string date_low;   // "YYYY-MM-DD"，kDay / kDayRange
  std::string date_high;  // 仅 kDayRange

  // uid / gid：uid / gid 是下界（kEqual 时就是唯一值），uid_high / gid_high
  // 只在 kRange 用。默认 kEqual，因为表单里最常见的是"属主就是 1000"。
  // 0 是合法值（root），不能拿它当"没填"。
  std::uint32_t uid = 0;
  RuleSizeCompare uid_compare = RuleSizeCompare::kEqual;
  std::uint32_t uid_high = 0;
  std::uint32_t gid = 0;
  RuleSizeCompare gid_compare = RuleSizeCompare::kEqual;
  std::uint32_t gid_high = 0;

  // user / group：精确匹配的名字，大小写敏感；留空表示用户没填。
  std::string user;
  std::string group;
};

// 一条规则 = 一个动作 + 若干子条件（子条件之间是 AND，与核心语义一致）。
struct FilterRuleDraft {
  FilterAction action = FilterAction::kInclude;
  std::vector<FilterClauseDraft> clauses;

  // 高级入口：整条规则的 DSL 原文。
  //
  // 非空时它就是这条规则的**唯一定义**：ToDsl / ValidateRule / Summarize /
  // CliArguments 全部直接用它，不再看 clauses。
  //
  // 为什么需要它：可视化表单每次只构造一个 clause，而 DSL 允许一条 rule 里写
  // 多个 condition（AND 语义，例如 "name:*.txt size:<1MB"）。多个 --include
  // 之间是 OR，所以"表单只能填单条件"并不是等价的表达。产品要求 GUI 与 CLI
  // 能力一致，因此这里必须留一个完整的 DSL 入口——校验依旧走同一个
  // Filter::AddRule，GUI 不定义语法。
  std::string raw_dsl;
};

const char* RuleFieldName(RuleField field);

// ---- 展示用的中文表 ----
//
// 普通用户不该看到 ext: / name: / size:<1MB 这些语法，所以"有哪些条件可选、
// 每个条件叫什么、怎么填"必须有地方定义，而且只能有一个地方。
//
// 这几张表就是那个地方：GUI 的条件类型下拉、单位下拉、比较方式下拉，以及规则
// 卡片主行那句人话摘要，全部读这里。于是"界面上能选的条件"与"builder 真的能
// 生成的条件"在结构上是同一份东西——不会出现"下拉里有一个核心执行不了的项"，
// 也不会出现"某个条件能生成、界面上却叫不出名字"。
const char* RuleFieldLabel(RuleField field);
// 一句面向用户的填写说明（占位符 / 例句之外的那句"为什么"）。
const char* RuleFieldHint(RuleField field);
const char* SizeCompareLabel(RuleSizeCompare compare);
const char* SizeUnitLabel(RuleSizeUnit unit);
const char* TypeValueLabel(RuleTypeValue type);
const char* MtimeKindLabel(RuleMtimeKind kind);

// 结构校验：只查"表单填得对不对"（空值、非法日期、range 反向）。
// 不做语法裁决——那是 ValidateRule 的事。
bool ValidateClause(const FilterClauseDraft& clause,
                    std::string* error_message);

// 单个子条件 -> DSL 片段，例如 "size:>=1KB"、"path:**/build/**"。
bool ToDsl(const FilterClauseDraft& clause, std::string* dsl,
           std::string* error_message);

// 整条规则 -> DSL，多子条件用空格连接（AND），例如
// "type:folder path:**/cache"。动作不进 DSL，由调用方决定放进 --include
// 还是 --exclude。
bool ToDsl(const FilterRuleDraft& rule, std::string* dsl,
           std::string* error_message);

// 最终裁决：把生成的 DSL 交给真实 Filter::AddRule。
// 这一条保证"前端校验 = 后端语义"，也是 GUI 不可能偏离核心的原因。
bool ValidateRule(const FilterRuleDraft& rule, std::string* error_message);

// 把一整份草稿列表编译成真正参与匹配的 Filter。
//
// 这是**唯一**的"草稿 -> 匹配器"通道：每一条规则都先序列化成 DSL，再走
// Filter::AddRule，所以 GUI 预览、CLI 预览与真实备份拿到的是同一个 Filter。
// 任何一端自己解释草稿，都会立刻变成第二套语义。
//
// 失败是**整体失败**，不是"跳过这一条"：静默丢掉一条规则会让筛选结果与用户
// 写的规则不一致（少一条 exclude 就等于多显示一批不该进归档的条目，而且看不
// 出来）。失败时 filter 保持在"还没有任何规则"的状态，调用方必须放弃这次操作。
//
// 正常路径上不会失败：草稿在进入列表之前已经过 ValidateRule（见 addRule /
// addAdvancedRule / RunPreviewCommand）。这一层是防御，不是常用分支。
bool BuildFilterFromDrafts(const std::vector<FilterRuleDraft>& rules,
                           Filter* filter, std::string* error_message);

// 人类可读摘要（中文，给界面显示用）。
std::string SummarizeClause(const FilterClauseDraft& clause);
std::string Summarize(const FilterRuleDraft& rule);

// 短摘要：规则卡片**主行**用的那一句，例如 "文件扩展名：cpp、h"、
// "文件大小 小于 1 MB"。
//
// 与 Summarize 的分工：Summarize 是一整句话（"包含：扩展名为 cpp 或 h
// 的文件"），
// 适合放在卡片正文；短摘要是"条件：取值"的名词短语，适合放在标题行，让人一眼
// 看出这条规则在筛什么。两者都从同一份草稿生成，不解析 DSL 文本。
std::string SummarizeClauseShort(const FilterClauseDraft& clause);
std::string SummarizeShort(const FilterRuleDraft& rule);

// 拼 CLI 参数：--include / --exclude 与规则文本交替出现，供"复制为 CLI 参数"。
std::vector<std::string> CliArguments(
    const std::vector<FilterRuleDraft>& rules);

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_FILTER_RULE_BUILDER_H_
