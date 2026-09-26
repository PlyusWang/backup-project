// filter_rule_builder.cpp
//
// 见 include/filter_rule_builder.h 的设计说明。这里只做字符串拼装和结构校验，
// 不读磁盘、不碰 Qt、不复制任何匹配逻辑。

#include "filter_rule_builder.h"

#include <cstdio>

namespace backupproject {
namespace {

const char* UnitSuffix(RuleSizeUnit unit) {
  switch (unit) {
    case RuleSizeUnit::kByte:
      return "";  // 字节直接写数字
    case RuleSizeUnit::kKilo:
      return "KB";
    case RuleSizeUnit::kMega:
      return "MB";
    case RuleSizeUnit::kGiga:
      return "GB";
  }
  return "";
}

const char* CompareOperator(RuleSizeCompare compare) {
  switch (compare) {
    case RuleSizeCompare::kLess:
      return "<";
    case RuleSizeCompare::kLessEqual:
      return "<=";
    case RuleSizeCompare::kGreater:
      return ">";
    case RuleSizeCompare::kGreaterEqual:
      return ">=";
    case RuleSizeCompare::kRange:
      return "..";
    case RuleSizeCompare::kEqual:
      return "=";
  }
  return "";
}

// type 下拉 -> DSL 取值，必须与核心的 type: 解析表逐字一致。
const char* TypeDslName(RuleTypeValue type) {
  switch (type) {
    case RuleTypeValue::kFile:
      return "file";
    case RuleTypeValue::kFolder:
      return "folder";
    case RuleTypeValue::kSymlink:
      return "symlink";
    case RuleTypeValue::kFifo:
      return "fifo";
    case RuleTypeValue::kCharDevice:
      return "char";
    case RuleTypeValue::kBlockDevice:
      return "block";
    case RuleTypeValue::kSocket:
      return "socket";
  }
  return "file";
}

// type 下拉 -> 摘要里的中文名。
const char* TypeDisplayName(RuleTypeValue type) {
  switch (type) {
    case RuleTypeValue::kFile:
      return "普通文件";
    case RuleTypeValue::kFolder:
      return "目录";
    case RuleTypeValue::kSymlink:
      return "符号链接";
    case RuleTypeValue::kFifo:
      return "命名管道";
    case RuleTypeValue::kCharDevice:
      return "字符设备";
    case RuleTypeValue::kBlockDevice:
      return "块设备";
    case RuleTypeValue::kSocket:
      return "套接字";
  }
  return "未知类型";
}

bool IsDigits(const std::string& text) {
  if (text.empty()) return false;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

// 手写日期解析，避免为一个 "YYYY-MM-DD" 引入正则或 <chrono> 解析的机器差异。
bool ParseDate(const std::string& text, int* year, int* month, int* day) {
  if (text.size() != 10) return false;
  if (text[4] != '-' || text[7] != '-') return false;
  const std::string ys = text.substr(0, 4);
  const std::string ms = text.substr(5, 2);
  const std::string ds = text.substr(8, 2);
  if (!IsDigits(ys) || !IsDigits(ms) || !IsDigits(ds)) return false;
  const int y = std::stoi(ys);
  const int m = std::stoi(ms);
  const int d = std::stoi(ds);
  if (y < 1970 || y > 9999) return false;
  if (m < 1 || m > 12) return false;
  if (d < 1 || d > 31) return false;
  *year = y;
  *month = m;
  *day = d;
  return true;
}

std::string Trim(const std::string& text) {
  const std::string::size_type first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return std::string();
  const std::string::size_type last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1);
}

std::string NormalizeExtension(const std::string& raw) {
  std::string text = Trim(raw);
  while (!text.empty() && text[0] == '.') text.erase(0, 1);
  return text;
}

std::string JoinWith(const std::vector<std::string>& items, const char* glue) {
  std::string out;
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i != 0) out += glue;
    out += items[i];
  }
  return out;
}

std::string FormatBytes(std::uint64_t value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%llu",
                static_cast<unsigned long long>(value));
  return std::string(buffer);
}

// uid:/gid: 的 DSL 片段。kEqual 写成裸数字（uid:1000），这点与 size 的
// "必须带运算符"不同；range 与其余运算符和 size 同形。
std::string IdClauseText(const char* field, std::uint32_t low,
                         std::uint32_t high, RuleSizeCompare compare) {
  const std::string prefix = std::string(field) + ":";
  if (compare == RuleSizeCompare::kRange) {
    return prefix + FormatBytes(low) + ".." + FormatBytes(high);
  }
  if (compare == RuleSizeCompare::kEqual) {
    return prefix + FormatBytes(low);
  }
  return prefix + CompareOperator(compare) + FormatBytes(low);
}

// uid / gid 的中文摘要，例如 "属主 uid = 1000"。
std::string IdSummary(const std::string& label, std::uint32_t low,
                      std::uint32_t high, RuleSizeCompare compare) {
  if (compare == RuleSizeCompare::kRange) {
    return label + " 在 " + FormatBytes(low) + " 到 " + FormatBytes(high) +
           " 之间";
  }
  if (compare == RuleSizeCompare::kEqual) {
    return label + " = " + FormatBytes(low);
  }
  return label + " " + CompareOperator(compare) + " " + FormatBytes(low);
}

}  // namespace

const char* RuleFieldName(RuleField field) {
  switch (field) {
    case RuleField::kName:
      return "name";
    case RuleField::kPath:
      return "path";
    case RuleField::kStem:
      return "stem";
    case RuleField::kExt:
      return "ext";
    case RuleField::kType:
      return "type";
    case RuleField::kSize:
      return "size";
    case RuleField::kMtime:
      return "mtime";
    case RuleField::kUid:
      return "uid";
    case RuleField::kGid:
      return "gid";
    case RuleField::kUser:
      return "user";
    case RuleField::kGroup:
      return "group";
  }
  return "?";
}

bool ValidateClause(const FilterClauseDraft& clause,
                    std::string* error_message) {
  const auto fail = [error_message](const std::string& text) {
    if (error_message != nullptr) *error_message = text;
    return false;
  };
  switch (clause.field) {
    case RuleField::kName:
    case RuleField::kPath:
    case RuleField::kStem:
      if (clause.pattern.empty()) {
        return fail(std::string(RuleFieldName(clause.field)) +
                    " 的匹配内容不能为空");
      }
      return true;
    case RuleField::kExt: {
      std::size_t usable = 0;
      for (const std::string& raw : clause.extensions) {
        if (!NormalizeExtension(raw).empty()) ++usable;
      }
      if (usable == 0) return fail("ext 至少需要一个扩展名");
      return true;
    }
    case RuleField::kType:
      return true;
    case RuleField::kSize:
      if (clause.compare == RuleSizeCompare::kRange) {
        if (clause.size_high < clause.size_low) {
          return fail("size 区间的上界不能小于下界");
        }
      }
      return true;
    case RuleField::kMtime:
      if (clause.mtime_kind == RuleMtimeKind::kLastDays) {
        if (clause.days_back <= 0) return fail("mtime 的天数必须大于 0");
        return true;
      }
      if (clause.mtime_kind == RuleMtimeKind::kDay) {
        int y = 0, m = 0, d = 0;
        if (!ParseDate(clause.date_low, &y, &m, &d)) {
          return fail("mtime 日期格式必须是 YYYY-MM-DD");
        }
        return true;
      }
      if (clause.mtime_kind == RuleMtimeKind::kDayRange) {
        int y = 0, m = 0, d = 0;
        if (!ParseDate(clause.date_low, &y, &m, &d) ||
            !ParseDate(clause.date_high, &y, &m, &d)) {
          return fail("mtime 日期区间必须是 YYYY-MM-DD..YYYY-MM-DD");
        }
        if (clause.date_high < clause.date_low) {
          return fail("mtime 区间的结束日期不能早于开始日期");
        }
        return true;
      }
      return true;
    case RuleField::kUid:
    case RuleField::kGid: {
      const bool is_uid = clause.field == RuleField::kUid;
      const RuleSizeCompare compare =
          is_uid ? clause.uid_compare : clause.gid_compare;
      const std::uint32_t low = is_uid ? clause.uid : clause.gid;
      const std::uint32_t high = is_uid ? clause.uid_high : clause.gid_high;
      // 0 是合法 uid / gid（root），这里不把 0 当作"没填"。
      if (compare == RuleSizeCompare::kRange && high < low) {
        return fail(std::string(RuleFieldName(clause.field)) +
                    " 区间的上界不能小于下界");
      }
      return true;
    }
    case RuleField::kUser:
      if (clause.user.empty()) return fail("user 不能为空");
      return true;
    case RuleField::kGroup:
      if (clause.group.empty()) return fail("group 不能为空");
      return true;
  }
  return fail("未知字段");
}

bool ToDsl(const FilterClauseDraft& clause, std::string* dsl,
           std::string* error_message) {
  if (!ValidateClause(clause, error_message)) return false;
  std::string text;
  switch (clause.field) {
    case RuleField::kName:
    case RuleField::kPath:
    case RuleField::kStem:
      text = std::string(RuleFieldName(clause.field)) + ":" + clause.pattern;
      break;
    case RuleField::kExt: {
      std::vector<std::string> cleaned;
      for (const std::string& raw : clause.extensions) {
        const std::string value = NormalizeExtension(raw);
        if (!value.empty()) cleaned.push_back(value);
      }
      text = "ext:" + JoinWith(cleaned, ";");
      break;
    }
    case RuleField::kType:
      text = std::string("type:") + TypeDslName(clause.type);
      break;
    case RuleField::kSize: {
      const char* suffix = UnitSuffix(clause.unit);
      const std::string low = FormatBytes(clause.size_low) + suffix;
      if (clause.compare == RuleSizeCompare::kRange) {
        text = "size:" + low + ".." + FormatBytes(clause.size_high) + suffix;
      } else if (clause.compare == RuleSizeCompare::kEqual) {
        // size 没有"等于"运算符：用 a..a 表达同一语义，生成的 DSL
        // 仍被核心接受。
        text = "size:" + low + ".." + low;
      } else {
        text = std::string("size:") + CompareOperator(clause.compare) + low;
      }
      break;
    }
    case RuleField::kMtime:
      switch (clause.mtime_kind) {
        case RuleMtimeKind::kToday:
          text = "mtime:today";
          break;
        case RuleMtimeKind::kYesterday:
          text = "mtime:yesterday";
          break;
        case RuleMtimeKind::kLastDays:
          text = "mtime:" + std::to_string(clause.days_back) + "days";
          break;
        case RuleMtimeKind::kDay:
          text = "mtime:" + clause.date_low;
          break;
        case RuleMtimeKind::kDayRange:
          text = "mtime:" + clause.date_low + ".." + clause.date_high;
          break;
      }
      break;
    case RuleField::kUid:
      text =
          IdClauseText("uid", clause.uid, clause.uid_high, clause.uid_compare);
      break;
    case RuleField::kGid:
      text =
          IdClauseText("gid", clause.gid, clause.gid_high, clause.gid_compare);
      break;
    case RuleField::kUser:
      text = "user:" + clause.user;
      break;
    case RuleField::kGroup:
      text = "group:" + clause.group;
      break;
  }
  if (dsl != nullptr) *dsl = text;
  return true;
}

bool ToDsl(const FilterRuleDraft& rule, std::string* dsl,
           std::string* error_message) {
  if (rule.clauses.empty()) {
    if (error_message != nullptr) *error_message = "规则至少需要一个子条件";
    return false;
  }
  std::string text;
  for (std::size_t i = 0; i < rule.clauses.size(); ++i) {
    std::string piece;
    if (!ToDsl(rule.clauses[i], &piece, error_message)) return false;
    if (i != 0) text += " ";  // 子条件之间 AND
    text += piece;
  }
  if (dsl != nullptr) *dsl = text;
  return true;
}

bool ValidateRule(const FilterRuleDraft& rule, std::string* error_message) {
  std::string dsl;
  if (!ToDsl(rule, &dsl, error_message)) return false;
  // 最终裁决交给真实核心：GUI 不定义语法。
  Filter probe;
  return probe.AddRule(rule.action, dsl, error_message);
}

std::string SummarizeClause(const FilterClauseDraft& clause) {
  switch (clause.field) {
    case RuleField::kName:
      return "名称匹配 " + clause.pattern + " 的文件";
    case RuleField::kPath:
      return "相对路径匹配 " + clause.pattern + " 的条目";
    case RuleField::kStem:
      return "主文件名匹配 " + clause.pattern + " 的文件";
    case RuleField::kExt: {
      std::vector<std::string> cleaned;
      for (const std::string& raw : clause.extensions) {
        const std::string value = NormalizeExtension(raw);
        if (!value.empty()) cleaned.push_back(value);
      }
      return "扩展名为 " + JoinWith(cleaned, " 或 ") + " 的文件";
    }
    case RuleField::kType:
      return "类型 = " + std::string(TypeDisplayName(clause.type));
    case RuleField::kSize: {
      const std::string low =
          FormatBytes(clause.size_low) + " " + UnitSuffix(clause.unit);
      if (clause.compare == RuleSizeCompare::kRange) {
        return "大小在 " + low + " 到 " + FormatBytes(clause.size_high) + " " +
               UnitSuffix(clause.unit) + " 之间";
      }
      return std::string("大小 ") + CompareOperator(clause.compare) + " " + low;
    }
    case RuleField::kMtime:
      switch (clause.mtime_kind) {
        case RuleMtimeKind::kToday:
          return "修改时间是今天";
        case RuleMtimeKind::kYesterday:
          return "修改时间是昨天";
        case RuleMtimeKind::kLastDays:
          return "最近 " + std::to_string(clause.days_back) + " 天内修改过";
        case RuleMtimeKind::kDay:
          return "修改日期是 " + clause.date_low;
        case RuleMtimeKind::kDayRange:
          return "修改日期在 " + clause.date_low + " 至 " + clause.date_high;
      }
      break;
    case RuleField::kUid:
      return IdSummary("属主 uid", clause.uid, clause.uid_high,
                       clause.uid_compare);
    case RuleField::kGid:
      return IdSummary("属组 gid", clause.gid, clause.gid_high,
                       clause.gid_compare);
    case RuleField::kUser:
      return "用户 user = " + clause.user;
    case RuleField::kGroup:
      return "用户组 group = " + clause.group;
  }
  return "未知条件";
}

std::string Summarize(const FilterRuleDraft& rule) {
  std::string text;
  for (std::size_t i = 0; i < rule.clauses.size(); ++i) {
    if (i != 0) text += "，且";
    text += SummarizeClause(rule.clauses[i]);
  }
  if (rule.action == FilterAction::kInclude) return "包含：" + text;
  return "排除：" + text;
}

std::vector<std::string> CliArguments(
    const std::vector<FilterRuleDraft>& rules) {
  std::vector<std::string> args;
  for (const FilterRuleDraft& rule : rules) {
    std::string dsl;
    if (!ToDsl(rule, &dsl, nullptr)) continue;
    args.push_back(rule.action == FilterAction::kInclude ? "--include"
                                                         : "--exclude");
    args.push_back(dsl);
  }
  return args;
}

}  // namespace backupproject
