// filter.h
//
// 备份筛选：决定哪些条目进入归档。
//
// 位置在归档写入之前：
//   源目录 → Filter → ArchiveWriter → .bak
//
// Filter 只回答"这条路径要不要进备份"，不负责压缩、加密、增量或校验；
// 它只看 lstat 拿到的元数据（名字、路径、类型、大小、mtime），不读文件内容。
//
// 规则语法、include / exclude 语义、glob 规则见 docs/filter_usage.md；
// 明确不做与后续计划见 docs/backlog/filter_future.md。

#ifndef BACKUP_PROJECT_INCLUDE_FILTER_H_
#define BACKUP_PROJECT_INCLUDE_FILTER_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace backupproject {

// 规则动作。exclude 优先于 include。
enum class FilterAction { kInclude, kExclude };

// 被判断对象的最小信息集合。archive_path 使用归档内部形式：
// 相对 source root、'/' 分隔；source root 自身是 "."。
struct FilterEntry {
  std::string archive_path;
  std::string name;
  bool is_directory = false;
  std::uint64_t size = 0;
  std::int64_t mtime_sec = 0;
};

class Filter {
 public:
  Filter() = default;

  // 解析并追加一条规则。失败时返回 false 并写 error_message，
  // 且**不改变**已有规则——调用方可以放心逐条添加。
  bool AddRule(FilterAction action, const std::string& text,
               std::string* error_message);

  // 没有任何规则时，备份行为必须与 PR #8 完全一致。
  bool empty() const { return rules_.empty(); }
  bool has_include() const;
  std::size_t rule_count() const { return rules_.size(); }

  // 目录是否整体剪枝：命中任意 exclude 就不再向下递归（子树里的特殊文件
  // 也因此不再被检查）。
  bool ShouldPruneDirectory(const FilterEntry& entry) const;

  // 普通文件是否进入归档：exclude 优先；存在 include 时必须命中至少一条。
  bool ShouldIncludeFile(const FilterEntry& entry) const;

  // 特殊文件（symlink / FIFO / socket / 设备）是否被显式排除。
  // 只有用户明确写了 exclude 才跳过；否则仍然让整次备份失败——不能因为
  // 存在 include 规则就把未匹配的特殊文件静默跳过。
  bool ShouldSkipSpecialEntry(const FilterEntry& entry) const;

 private:
  // 一条规则内部可以写多个子句（用空白分隔，且空白后面紧跟已知字段名），
  // 子句之间是 AND；规则之间是 OR。
  struct Clause {
    enum class Field { kName, kPath, kStem, kExt, kType, kSize, kMtime };
    enum class Compare { kLess, kLessEqual, kGreater, kGreaterEqual, kRange };
    enum class TimeKind { kDay, kDayRange, kLastDays };

    Field field = Field::kName;
    std::string pattern;                  // name / path / stem 的 glob
    std::vector<std::string> extensions;  // ext 的多个取值
    bool wants_directory = false;         // type:file / type:folder
    Compare compare = Compare::kLess;     // size
    std::uint64_t size_low = 0;
    std::uint64_t size_high = 0;
    TimeKind time_kind = TimeKind::kDay;  // mtime 的闭区间 [low, high]
    std::int64_t time_low = 0;
    std::int64_t time_high = 0;
    std::int64_t days_back = 0;  // kLastDays：过去 N * 24h
  };

  struct Rule {
    FilterAction action = FilterAction::kInclude;
    std::vector<Clause> clauses;
    std::string text;  // 原文，报错和展示用
  };

  bool ClauseMatches(const Clause& clause, const FilterEntry& entry) const;
  bool RuleMatches(const Rule& rule, const FilterEntry& entry) const;
  bool MatchesAny(FilterAction action, const FilterEntry& entry) const;

  std::vector<Rule> rules_;
};

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_FILTER_H_
