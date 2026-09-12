// filter.cpp
//
// 基础筛选规则的解析与匹配。设计上只有三件事：
//
//   1. 解析：把 "exclude path:**/build/**" 这样的文本变成若干子句；
//   2. 匹配：glob / 扩展名列表 / 数值范围 / 日期范围；
//   3. 决策：exclude 优先，存在 include 时普通文件必须命中至少一条。
//
// 刻意不做的事：布尔表达式、regex、内容搜索、后代统计——它们都记在
// docs/backlog/filter_future.md 里，本文既不实现也不为它们留隐式钩子。

#include "filter.h"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace backupproject {

namespace {

constexpr std::int64_t kSecondsPerDay = 24 * 60 * 60;
constexpr std::uint64_t kMaxDaysBack = 36500;  // 约 100 年，防止离谱输入

void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) {
    *error_message = text;
  }
}

bool IsAsciiDigit(char c) { return c >= '0' && c <= '9'; }

// ---- glob ---------------------------------------------------------------

// 把 glob 模式切成 token。** 与 **/ 分开：后者按"零个或多个路径段"匹配，
// 这样 **/build/** 既能命中 build/x，也能命中 a/build/x。
struct GlobToken {
  enum class Kind { kLiteral, kStar, kQuestion, kDoubleStar, kDoubleStarSlash };
  Kind kind = Kind::kLiteral;
  std::string literal;
};

void FlushLiteral(std::string* literal, std::vector<GlobToken>* tokens) {
  if (literal->empty()) {
    return;
  }
  GlobToken token;
  token.kind = GlobToken::Kind::kLiteral;
  token.literal = *literal;
  tokens->push_back(token);
  literal->clear();
}

std::vector<GlobToken> TokenizeGlob(const std::string& pattern) {
  std::vector<GlobToken> tokens;
  std::string literal;
  std::size_t index = 0;
  while (index < pattern.size()) {
    const char c = pattern[index];
    if (c == '*') {
      FlushLiteral(&literal, &tokens);
      if (index + 1 < pattern.size() && pattern[index + 1] == '*') {
        GlobToken token;
        if (index + 2 < pattern.size() && pattern[index + 2] == '/') {
          token.kind = GlobToken::Kind::kDoubleStarSlash;
          index += 3;
        } else {
          token.kind = GlobToken::Kind::kDoubleStar;
          index += 2;
        }
        tokens.push_back(token);
      } else {
        GlobToken token;
        token.kind = GlobToken::Kind::kStar;
        tokens.push_back(token);
        ++index;
      }
      continue;
    }
    if (c == '?') {
      FlushLiteral(&literal, &tokens);
      GlobToken token;
      token.kind = GlobToken::Kind::kQuestion;
      tokens.push_back(token);
      ++index;
      continue;
    }
    literal.push_back(c);
    ++index;
  }
  FlushLiteral(&literal, &tokens);
  return tokens;
}

// 自底向上的 DP：dp[i][j] 表示 tokens[i..] 能否匹配 text[j..]。
// 不用朴素递归回溯——a*a*a*a* 这类模式在朴素写法下会指数级爆炸。
bool GlobMatch(const std::string& pattern, const std::string& text) {
  const std::vector<GlobToken> tokens = TokenizeGlob(pattern);
  const std::size_t n = tokens.size();
  const std::size_t m = text.size();
  std::vector<std::vector<char>> dp(n + 1, std::vector<char>(m + 1, 0));
  dp[n][m] = 1;
  for (std::size_t i = n; i-- > 0;) {
    for (std::size_t j = m + 1; j-- > 0;) {
      bool ok = false;
      const GlobToken& token = tokens[i];
      switch (token.kind) {
        case GlobToken::Kind::kLiteral: {
          const std::size_t length = token.literal.size();
          if (j + length <= m && text.compare(j, length, token.literal) == 0) {
            ok = dp[i + 1][j + length] != 0;
          }
          break;
        }
        case GlobToken::Kind::kQuestion:
          ok = j < m && text[j] != '/' && dp[i + 1][j + 1] != 0;
          break;
        case GlobToken::Kind::kStar:
          // 吃掉任意个非 '/' 字符（0 个也行）。
          ok = dp[i + 1][j] != 0 ||
               (j < m && text[j] != '/' && dp[i][j + 1] != 0);
          break;
        case GlobToken::Kind::kDoubleStar:
        case GlobToken::Kind::kDoubleStarSlash:
          // 零个或多个任意字符（**/ 允许从任意位置开始，因此既匹配
          // "build/x" 也匹配 "a/build/x"）。
          ok = dp[i + 1][j] != 0 || (j < m && dp[i][j + 1] != 0);
          break;
      }
      dp[i][j] = ok ? 1 : 0;
    }
  }
  return dp[0][0] != 0;
}

// ---- 名字派生字段 -------------------------------------------------------

// 去掉最后一个扩展名；以 '.' 开头的名字（如 .gitignore）没有扩展名。
std::string StemOf(const std::string& name) {
  const std::size_t dot = name.rfind('.');
  if (dot == std::string::npos || dot == 0) {
    return name;
  }
  return name.substr(0, dot);
}

std::string ExtensionOf(const std::string& name) {
  const std::size_t dot = name.rfind('.');
  if (dot == std::string::npos || dot == 0) {
    return std::string();
  }
  return name.substr(dot + 1);
}

// ---- size ---------------------------------------------------------------

bool ParseUnsigned(const std::string& text, std::uint64_t* out) {
  if (text.empty()) {
    return false;
  }
  std::uint64_t value = 0;
  for (char c : text) {
    if (!IsAsciiDigit(c)) {
      return false;
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    // 手动累加并检查溢出，比 strtoull + errno 更直接。
    if (value > (UINT64_MAX - digit) / 10) {
      return false;
    }
    value = value * 10 + digit;
  }
  *out = value;
  return true;
}

bool ParseSizeLiteral(const std::string& text, std::uint64_t* out,
                      std::string* error_message) {
  std::size_t split = 0;
  while (split < text.size() && IsAsciiDigit(text[split])) {
    ++split;
  }
  const std::string digits = text.substr(0, split);
  std::string unit = text.substr(split);
  for (char& c : unit) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  std::uint64_t factor = 0;
  if (unit.empty() || unit == "B") {
    factor = 1;
  } else if (unit == "KB") {
    factor = 1024;
  } else if (unit == "MB") {
    factor = 1024ULL * 1024ULL;
  } else if (unit == "GB") {
    factor = 1024ULL * 1024ULL * 1024ULL;
  } else {
    SetError(error_message,
             "Invalid filter rule: unknown size unit in size:" + text);
    return false;
  }

  std::uint64_t value = 0;
  if (!ParseUnsigned(digits, &value)) {
    SetError(error_message,
             "Invalid filter rule: invalid size value in size:" + text);
    return false;
  }
  if (value > UINT64_MAX / factor) {
    SetError(error_message,
             "Invalid filter rule: size value overflows in size:" + text);
    return false;
  }
  *out = value * factor;
  return true;
}

// ---- mtime --------------------------------------------------------------

// 本地时区某一天的 00:00:00。day_offset 用来取"前一天 / 后一天"，
// 交给 mktime 归一化，这样夏令时切换也不会算错。
bool LocalDayStart(int year, int month, int day, int day_offset,
                   std::int64_t* out) {
  struct tm parts;
  std::memset(&parts, 0, sizeof(parts));
  parts.tm_year = year - 1900;
  parts.tm_mon = month - 1;
  parts.tm_mday = day + day_offset;
  parts.tm_isdst = -1;  // 让 mktime 自己判断夏令时
  const std::time_t value = std::mktime(&parts);
  if (value == static_cast<std::time_t>(-1)) {
    return false;
  }
  if (day_offset == 0 && (parts.tm_year != year - 1900 ||
                          parts.tm_mon != month - 1 || parts.tm_mday != day)) {
    return false;  // mktime 归一化过，说明原始日期不合法
  }
  *out = static_cast<std::int64_t>(value);
  return true;
}

bool ParseDate(const std::string& text, std::int64_t* out) {
  if (text.size() != 10 || text[4] != '-' || text[7] != '-') {
    return false;
  }
  const std::size_t digits[] = {0, 1, 2, 3, 5, 6, 8, 9};
  for (std::size_t position : digits) {
    if (!IsAsciiDigit(text[position])) {
      return false;
    }
  }
  const int year = std::atoi(text.substr(0, 4).c_str());
  const int month = std::atoi(text.substr(5, 2).c_str());
  const int day = std::atoi(text.substr(8, 2).c_str());
  if (month < 1 || month > 12 || day < 1 || day > 31) {
    return false;
  }
  return LocalDayStart(year, month, day, 0, out);
}

// mtime 取值换算成闭区间 [low, high]。kLastDays 只记天数，匹配时再拿
// "现在"去算，避免长驻进程把"今天"固定在启动那一刻。
bool ParseMtimeValue(const std::string& value, int* kind, std::int64_t* low,
                     std::int64_t* high, std::int64_t* days_back,
                     std::string* error_message) {
  if (value == "today" || value == "yesterday") {
    const std::time_t now = std::time(nullptr);
    struct tm local;
    localtime_r(&now, &local);
    std::int64_t start = 0;
    if (!LocalDayStart(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                       value == "today" ? 0 : -1, &start)) {
      SetError(error_message, "Invalid filter rule: bad mtime value " + value);
      return false;
    }
    *kind = 0;  // kDay
    *low = start;
    *high = start + kSecondsPerDay - 1;
    return true;
  }
  if (value.size() > 5 && value.compare(value.size() - 4, 4, "days") == 0) {
    std::uint64_t days = 0;
    if (!ParseUnsigned(value.substr(0, value.size() - 4), &days) || days == 0 ||
        days > kMaxDaysBack) {
      SetError(error_message,
               "Invalid filter rule: invalid mtime value " + value);
      return false;
    }
    *kind = 2;  // kLastDays
    *days_back = static_cast<std::int64_t>(days);
    *low = 0;
    *high = 0;
    return true;
  }
  const std::size_t range = value.find("..");
  if (range != std::string::npos) {
    std::int64_t start = 0;
    std::int64_t end = 0;
    if (!ParseDate(value.substr(0, range), &start) ||
        !ParseDate(value.substr(range + 2), &end)) {
      SetError(error_message,
               "Invalid filter rule: invalid mtime range " + value);
      return false;
    }
    if (end < start) {
      SetError(error_message,
               "Invalid filter rule: mtime range is reversed: " + value);
      return false;
    }
    *kind = 1;  // kDayRange
    *low = start;
    *high = end + kSecondsPerDay - 1;
    return true;
  }
  std::int64_t start = 0;
  if (!ParseDate(value, &start)) {
    SetError(error_message,
             "Invalid filter rule: invalid mtime value " + value);
    return false;
  }
  *kind = 0;  // kDay
  *low = start;
  *high = start + kSecondsPerDay - 1;
  return true;
}

// ---- 规则文本 -----------------------------------------------------------

bool IsKnownField(const std::string& field) {
  return field == "name" || field == "path" || field == "stem" ||
         field == "ext" || field == "type" || field == "size" ||
         field == "mtime";
}

// 只在"空白后面紧跟已知字段名 + 冒号"处切分，这样 name:my file.txt 里的
// 空格不会被误当成分隔符，而 type:folder path:**/build 能切成两个子句。
std::vector<std::string> SplitClauses(const std::string& text,
                                      std::string* error_message) {
  std::vector<std::string> clauses;
  std::string current;
  std::size_t index = 0;
  while (index < text.size()) {
    const char c = text[index];
    if (std::isspace(static_cast<unsigned char>(c)) == 0) {
      current.push_back(c);
      ++index;
      continue;
    }
    std::size_t probe = index;
    while (probe < text.size() &&
           std::isspace(static_cast<unsigned char>(text[probe])) != 0) {
      ++probe;
    }
    const std::size_t colon = text.find(':', probe);
    const std::string candidate = (colon == std::string::npos)
                                      ? text.substr(probe)
                                      : text.substr(probe, colon - probe);
    if (colon != std::string::npos && !candidate.empty() &&
        IsKnownField(candidate)) {
      if (!current.empty()) {
        clauses.push_back(current);
        current.clear();
      }
      index = probe;
      continue;
    }
    current.push_back(c);
    ++index;
  }
  if (!current.empty()) {
    clauses.push_back(current);
  }
  if (clauses.empty()) {
    SetError(error_message, "Invalid filter rule: empty rule");
  }
  return clauses;
}

bool ParseClause(const std::string& text, std::string* field_out,
                 std::string* value_out, std::string* error_message) {
  const std::size_t colon = text.find(':');
  if (colon == std::string::npos) {
    SetError(error_message,
             "Invalid filter rule: missing ':' in clause: " + text);
    return false;
  }
  const std::string field = text.substr(0, colon);
  const std::string value = text.substr(colon + 1);
  if (!IsKnownField(field)) {
    SetError(error_message,
             "Invalid filter rule: unknown field '" + field + "'");
    return false;
  }
  if (value.empty()) {
    SetError(error_message,
             "Invalid filter rule: empty value for field '" + field + "'");
    return false;
  }
  *field_out = field;
  *value_out = value;
  return true;
}

}  // namespace

bool Filter::ClauseMatches(const Clause& clause,
                           const FilterEntry& entry) const {
  switch (clause.field) {
    case Clause::Field::kName:
      return GlobMatch(clause.pattern, entry.name);
    case Clause::Field::kPath:
      return GlobMatch(clause.pattern, entry.archive_path);
    case Clause::Field::kStem:
      return GlobMatch(clause.pattern, StemOf(entry.name));
    case Clause::Field::kExt: {
      const std::string extension = ExtensionOf(entry.name);
      if (extension.empty()) {
        return false;
      }
      for (const std::string& candidate : clause.extensions) {
        if (extension == candidate) {
          return true;
        }
      }
      return false;
    }
    case Clause::Field::kType:
      return entry.is_directory == clause.wants_directory;
    case Clause::Field::kSize:
      // 目录没有"文件大小"的语义，size 规则一律不命中目录。
      if (entry.is_directory) {
        return false;
      }
      switch (clause.compare) {
        case Clause::Compare::kLess:
          return entry.size < clause.size_low;
        case Clause::Compare::kLessEqual:
          return entry.size <= clause.size_low;
        case Clause::Compare::kGreater:
          return entry.size > clause.size_low;
        case Clause::Compare::kGreaterEqual:
          return entry.size >= clause.size_low;
        case Clause::Compare::kRange:
          // 闭区间：两端都算命中。
          return entry.size >= clause.size_low &&
                 entry.size <= clause.size_high;
      }
      return false;
    case Clause::Field::kMtime: {
      if (clause.time_kind == Clause::TimeKind::kLastDays) {
        const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
        return entry.mtime_sec >= now - clause.days_back * kSecondsPerDay;
      }
      return entry.mtime_sec >= clause.time_low &&
             entry.mtime_sec <= clause.time_high;
    }
  }
  return false;
}

bool Filter::RuleMatches(const Rule& rule, const FilterEntry& entry) const {
  for (const Clause& clause : rule.clauses) {
    if (!ClauseMatches(clause, entry)) {
      return false;  // 同一规则内的多个子句是 AND
    }
  }
  return !rule.clauses.empty();
}

bool Filter::MatchesAny(FilterAction action, const FilterEntry& entry) const {
  for (const Rule& rule : rules_) {
    if (rule.action == action && RuleMatches(rule, entry)) {
      return true;
    }
  }
  return false;
}

bool Filter::has_include() const {
  for (const Rule& rule : rules_) {
    if (rule.action == FilterAction::kInclude) {
      return true;
    }
  }
  return false;
}

bool Filter::ShouldPruneDirectory(const FilterEntry& entry) const {
  if (MatchesAny(FilterAction::kExclude, entry)) {
    return true;
  }
  // 目录再按"前缀形式"匹配一次：把路径末尾补上 '/' 再比一次 glob。
  // 这样 exclude path:**/build/** 会直接剪掉 build 目录本身（连同子树），
  // 而不只是过滤它里面的文件——剪掉之后，子树里的特殊文件也不再触发失败。
  // 只有"整条规则都是 path 子句"时才走这条路径，避免绕过 type:/size: 等条件。
  for (const Rule& rule : rules_) {
    if (rule.action != FilterAction::kExclude) {
      continue;
    }
    bool all_path_clauses = !rule.clauses.empty();
    bool matched = true;
    for (const Clause& clause : rule.clauses) {
      if (clause.field != Clause::Field::kPath) {
        all_path_clauses = false;
        break;
      }
      if (!GlobMatch(clause.pattern, entry.archive_path + "/")) {
        matched = false;
        break;
      }
    }
    if (all_path_clauses && matched) {
      return true;
    }
  }
  return false;
}

bool Filter::ShouldIncludeFile(const FilterEntry& entry) const {
  if (MatchesAny(FilterAction::kExclude, entry)) {
    return false;  // exclude 优先
  }
  if (!has_include()) {
    return true;  // 没有 include 时默认全收
  }
  return MatchesAny(FilterAction::kInclude, entry);
}

bool Filter::ShouldSkipSpecialEntry(const FilterEntry& entry) const {
  // 只有明确的 exclude 才能让特殊文件被跳过；否则调用方仍然报错。
  return MatchesAny(FilterAction::kExclude, entry);
}

bool Filter::AddRule(FilterAction action, const std::string& text,
                     std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  if (text.empty()) {
    SetError(error_message, "Invalid filter rule: empty rule");
    return false;
  }
  std::string clause_error;
  const std::vector<std::string> clause_texts =
      SplitClauses(text, &clause_error);
  if (clause_texts.empty()) {
    SetError(error_message, clause_error);
    return false;
  }

  Rule rule;
  rule.action = action;
  rule.text = text;
  for (const std::string& clause_text : clause_texts) {
    std::string field;
    std::string value;
    if (!ParseClause(clause_text, &field, &value, error_message)) {
      // 解析失败时不改动已有规则，调用方可以继续用旧规则集。
      return false;
    }
    Clause clause;
    bool ok = true;
    if (field == "name" || field == "path" || field == "stem") {
      clause.field = (field == "name")   ? Clause::Field::kName
                     : (field == "path") ? Clause::Field::kPath
                                         : Clause::Field::kStem;
      clause.pattern = value;
    } else if (field == "ext") {
      clause.field = Clause::Field::kExt;
      std::size_t start = 0;
      while (ok) {
        const std::size_t semi = value.find(';', start);
        const std::string item = (semi == std::string::npos)
                                     ? value.substr(start)
                                     : value.substr(start, semi - start);
        if (item.empty()) {
          SetError(error_message,
                   "Invalid filter rule: empty extension in ext:" + value);
          ok = false;
          break;
        }
        clause.extensions.push_back(item);
        if (semi == std::string::npos) {
          break;
        }
        start = semi + 1;
      }
    } else if (field == "type") {
      clause.field = Clause::Field::kType;
      if (value == "file") {
        clause.wants_directory = false;
      } else if (value == "folder") {
        clause.wants_directory = true;
      } else {
        SetError(error_message, "Invalid filter rule: unknown type '" + value +
                                    "' (expected file or folder)");
        ok = false;
      }
    } else if (field == "size") {
      clause.field = Clause::Field::kSize;
      const std::size_t range = value.find("..");
      if (range != std::string::npos) {
        std::uint64_t low = 0;
        std::uint64_t high = 0;
        if (!ParseSizeLiteral(value.substr(0, range), &low, error_message) ||
            !ParseSizeLiteral(value.substr(range + 2), &high, error_message)) {
          ok = false;
        } else if (high < low) {
          SetError(
              error_message,
              "Invalid filter rule: size range is reversed: size:" + value);
          ok = false;
        } else {
          clause.compare = Clause::Compare::kRange;
          clause.size_low = low;
          clause.size_high = high;
        }
      } else {
        Clause::Compare compare = Clause::Compare::kLess;
        std::string rest = value;
        if (value.compare(0, 2, "<=") == 0) {
          compare = Clause::Compare::kLessEqual;
          rest = value.substr(2);
        } else if (value.compare(0, 2, ">=") == 0) {
          compare = Clause::Compare::kGreaterEqual;
          rest = value.substr(2);
        } else if (value[0] == '<') {
          compare = Clause::Compare::kLess;
          rest = value.substr(1);
        } else if (value[0] == '>') {
          compare = Clause::Compare::kGreater;
          rest = value.substr(1);
        } else {
          SetError(error_message,
                   "Invalid filter rule: size needs <, <=, >, >= or a..b "
                   "range: size:" +
                       value);
          ok = false;
        }
        if (ok) {
          std::uint64_t bound = 0;
          if (!ParseSizeLiteral(rest, &bound, error_message)) {
            ok = false;
          } else {
            clause.compare = compare;
            clause.size_low = bound;
            clause.size_high = bound;
          }
        }
      }
    } else {
      clause.field = Clause::Field::kMtime;
      int kind = 0;
      std::int64_t low = 0;
      std::int64_t high = 0;
      std::int64_t days = 0;
      if (!ParseMtimeValue(value, &kind, &low, &high, &days, error_message)) {
        ok = false;
      } else {
        clause.time_kind = static_cast<Clause::TimeKind>(kind);
        clause.time_low = low;
        clause.time_high = high;
        clause.days_back = days;
      }
    }
    if (!ok) {
      return false;
    }
    rule.clauses.push_back(clause);
  }

  rules_.push_back(rule);
  return true;
}

}  // namespace backupproject
