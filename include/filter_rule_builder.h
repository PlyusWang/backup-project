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
enum class RuleField { kName, kPath, kStem, kExt, kType, kSize, kMtime };

// size 的比较运算符（range 对应核心的 kRange）。
enum class RuleSizeCompare { kLess, kLessEqual, kGreater, kGreaterEqual, kRange };

// 1024 进制，与文档固定下来的语义一致。
enum class RuleSizeUnit { kByte, kKilo, kMega, kGiga };

enum class RuleTypeValue { kFile, kFolder };

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
  int days_back = 7;            // 仅 kLastDays
  std::string date_low;         // "YYYY-MM-DD"，kDay / kDayRange
  std::string date_high;        // 仅 kDayRange
};

// 一条规则 = 一个动作 + 若干子条件（子条件之间是 AND，与核心语义一致）。
struct FilterRuleDraft {
  FilterAction action = FilterAction::kInclude;
  std::vector<FilterClauseDraft> clauses;
};

const char* RuleFieldName(RuleField field);

// 结构校验：只查"表单填得对不对"（空值、非法日期、range 反向）。
// 不做语法裁决——那是 ValidateRule 的事。
bool ValidateClause(const FilterClauseDraft& clause, std::string* error_message);

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

// 人类可读摘要（中文，给界面显示用）。
std::string SummarizeClause(const FilterClauseDraft& clause);
std::string Summarize(const FilterRuleDraft& rule);

// 拼 CLI 参数：--include / --exclude 与规则文本交替出现，供"复制为 CLI 参数"。
std::vector<std::string> CliArguments(const std::vector<FilterRuleDraft>& rules);

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_FILTER_RULE_BUILDER_H_
