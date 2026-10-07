// filter.cpp
//
// 基础筛选规则的解析与匹配。设计上只有三件事：
//
//   1. 解析：把 "exclude path:**/build/**" 这样的文本变成若干子句；
//   2. 匹配：glob / 扩展名列表 / 数值范围 / 属主 / 日期范围；
//   3. 决策：exclude 优先，存在 include 时普通文件必须命中至少一条。
//
// 刻意不做的事：布尔表达式、regex、内容搜索、后代统计——它们都记在
// docs/backlog/filter_future.md 里，本文既不实现也不为它们留隐式钩子。

// 数据流：DSL 文本（命令行 --include/--exclude，或界面草稿经
// FilterRuleBuilder 序列化后的产物）在这里被解析成 Rule + Clause；扫描层
// 逐条目构造 FilterEntry 后调用 ShouldPruneDirectory / ShouldIncludeFile /
// ShouldSkipSpecialEntry，把结果交给归档写入器。
// Filter 自己不产生任何 I/O，也不持有扫描状态：一份构造完成的 Filter
// 可以给任意多条条目复用，而且只需要只读访问。
//
// 不变量：
//   * rules_ 里每条 Rule 的 clauses 都非空且全部解析成功——AddRule 只在
//     整条文本解析完之后才 push_back，半成品规则永远不会入库；
//   * 规则之间是 OR，一条规则内部的子句之间是 AND；
//   * exclude 永远优先：include 只能收窄，不能把已被 exclude 的东西放回来。
//
// 失败语义：唯一的失败入口是 AddRule，返回 false 并写 error_message，且
// 不改变已有规则（调用方可以放心逐条添加）。匹配类接口都是纯查询，没有
// 失败路径——非法输入在 AddRule 阶段就被拒绝，运行期不会遇到半个规则。
//
// 线程与生命周期：全部成员都是值语义，没有共享可变状态；AddRule 之后只读
// 复用，不需要加锁。对象由调用方持有，本文不 new、不 delete、不缓存指针。
//
// 安全边界：DSL 来自命令行与界面输入，属于不受信输入。解析只做字段白名单
// 与取值范围校验，绝不把输入当作路径、命令或格式串；不认识的东西一律明确
// 报错，不做"尽力猜一个"的降级。
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

// mtime:Ndays 里的"N 天"是 N x 24 小时（一段时长），不是 N 个日历日；
// 日历日的边界一律交给 mktime 归一化，见 LocalDayStart / LocalDayEnd。
constexpr std::int64_t kSecondsPerDay = 24 * 60 * 60;
constexpr std::uint64_t kMaxDaysBack = 36500;  // 约 100 年，防止离谱输入

void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) {
    *error_message = text;
  }
}

bool IsAsciiDigit(char c) { return c >= '0' && c <= '9'; }

// ---- glob ---------------------------------------------------------------

// token 只有 kind 与 literal 两个字段，连续的普通字符会被合并成一个
// kLiteral，所以 token 数远小于模式长度，DP 表也跟着小一圈。
// 这里刻意不用正则：std::regex 的行为与性能随实现而变，而筛选规则必须在
// 所有平台上逐字一致。
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

// 单遍扫描，不回溯：'*' / '?' 各自成 token，其余字符累积成字面量。
// 刻意不支持转义、字符类 [a-z]、花括号展开和大小写折叠——它们都是 glob
// 的方言，一旦支持就必须同时定义反斜杠怎么转义，而反斜杠在 Linux 文件名
// 里是合法普通字符，把它定成转义符会让一部分真实文件名再也匹配不上。
// 唯一的多字符特例是 "**/"：必须整体识别，否则它里面的 '/' 会被当成
// 字面量字符，规则只在字面量意义上成立。
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

// glob 匹配。时间与空间都是 O(|tokens| x |text|)，全部在 vector 上分配：
// 模式来自用户、路径来自文件系统，长度都不可信，递归写法等于把栈交给输入。
//
// 语义：
//   * 整串匹配，不是子串查找；空模式只匹配空串；
//   * 大小写敏感、逐字节比较（Linux 文件名就是字节串）；
//   * '*' 与 '?' 不跨越 '/'，只有 '**' 系列才跨目录。
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

// 扩展名 = 最后一个 '.' 之后的部分，同样排除隐藏文件名；"a.tar.gz" 的
// 扩展名是 "gz"（只取最后一段），"a." 的扩展名是空串。
// 比较是逐字节精确比较、不做大小写折叠：ext:JPG 匹配不到 a.jpg。
std::string ExtensionOf(const std::string& name) {
  const std::size_t dot = name.rfind('.');
  if (dot == std::string::npos || dot == 0) {
    return std::string();
  }
  return name.substr(dot + 1);
}

// ---- size ---------------------------------------------------------------

// 只接受非空的 ASCII 十进制数字串：前后空白、正负号、十六进制一律拒绝
// （strtoull 会接受 " 12"、"+7"、"0x10"，那些都不是 DSL 的语法）。
// 溢出在这里就判定，调用方拿到的一定是完整解析后的值，不必再查 errno。
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

// size: 的数值字面量 = <十进制数字><单位?>，单位取 B / KB / MB / GB
// （大写不敏感，1024 进制，与界面上 "1 MB" 的含义一致）；缺省单位是字节。
//
// 三条失败路径都写 error_message 并返回 false，且都不写 *out：单位不认识、
// 数字部分非法、以及乘完之后会溢出 uint64。溢出检查放在乘法之前，
// size:18446744073709551615GB 这类输入必须被拒绝而不是回绕成一个小数。
//
// 固定 1024 进制是刻意的：同一条规则在 CLI 与 GUI 上必须给出同一个边界，
// 引入 1000 进制（MB 的另一种常见定义）会让两边悄悄分叉。
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

// ---- uid / gid ----------------------------------------------------------

// 取值范围跟着 POSIX 的 uid_t / gid_t 走：0 合法（root），上界是 uint32
// 满值。两条边界都必须显式——接受 0 才不让 root 的规则永远匹配不到，
// 卡住上界才不会把超范围输入悄悄截断成一条完全不同的规则。
// uid:/gid: 的数值：先按 uint64 解析（顺带查溢出），再卡到 uint32 上界。
// 99999999999 这类超范围输入必须明确报错，而不是截断成一个"看起来能跑"的数。
bool ParseIdNumber(const std::string& field, const std::string& text,
                   std::uint32_t* out, std::string* error_message) {
  std::uint64_t value = 0;
  if (!ParseUnsigned(text, &value) || value > UINT32_MAX) {
    SetError(error_message,
             "Invalid filter rule: " + field +
                 " value out of range (0..4294967295): " + field + ":" + text);
    return false;
  }
  *out = static_cast<std::uint32_t>(value);
  return true;
}

// ---- mtime --------------------------------------------------------------

// mktime 返回 -1 既可能是这个时刻表示不出来，也可能是真的出错，这里统一
// 按失败处理：筛选规则里的日期都在 1970 年之后，区分这两种情况没有收益。
// 本地时区某一天的 00:00:00。day_offset 是相对 (year, month, day) 的自然日
// 偏移（可以为负），跨月 / 跨年 / 跨 DST 全部交给 mktime 归一化，不自己算日期。
//
// 只有 day_offset == 0 时才校验"日期本身合法"：带偏移的日期本来就允许落到
// 相邻的月份或年份，归一化是预期行为而不是错误。
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

// 某一天的最后一秒 = 下一天 00:00:00 - 1。不能写成 start + 86400 - 1：
// DST 切换那天只有 23 或 25 小时，写死 86400 会让窗口端点落进相邻的日历日。
bool LocalDayEnd(int year, int month, int day, std::int64_t* out) {
  std::int64_t next_start = 0;
  if (!LocalDayStart(year, month, day, 1, &next_start)) {
    return false;
  }
  *out = next_start - 1;
  return true;
}

// 格式是死的四位年 '-' 两位月 '-' 两位日，不接受 "2024-1-1" 这类宽松
// 写法：宽松解析会让 "2024-13-01" 这种手误有机会被归一化悄悄变成另一个
// 日期。月 / 日只做粗筛（1..12、1..31），真正的合法性判断由 LocalDayStart
// 的归一化比对给出，年份范围交给 mktime 自己兜底。
// YYYY-MM-DD -> 本地日历日。年月日一并回给调用方：窗口上界要用它们算
// "这一天的最后一秒"（见 LocalDayEnd），只回一个时间戳是算不出来的。
bool ParseDate(const std::string& text, int* year, int* month, int* day,
               std::int64_t* out) {
  if (text.size() != 10 || text[4] != '-' || text[7] != '-') {
    return false;
  }
  const std::size_t digits[] = {0, 1, 2, 3, 5, 6, 8, 9};
  for (std::size_t position : digits) {
    if (!IsAsciiDigit(text[position])) {
      return false;
    }
  }
  const int parsed_year = std::atoi(text.substr(0, 4).c_str());
  const int parsed_month = std::atoi(text.substr(5, 2).c_str());
  const int parsed_day = std::atoi(text.substr(8, 2).c_str());
  if (parsed_month < 1 || parsed_month > 12 || parsed_day < 1 ||
      parsed_day > 31) {
    return false;
  }
  if (!LocalDayStart(parsed_year, parsed_month, parsed_day, 0, out)) {
    return false;
  }
  *year = parsed_year;
  *month = parsed_month;
  *day = parsed_day;
  return true;
}

// mtime: 的取值语法（全部小写）：
//   today | yesterday            归一化成 [当天 00:00, 当天 23:59:59]
//   <N>days                      N 是十进制正整数，上限 kMaxDaysBack
//   YYYY-MM-DD                   单日，等价于 [当天 00:00, 次日 00:00 - 1]
//   YYYY-MM-DD..YYYY-MM-DD       闭区间，两端都含
//
// 返回的 *kind 就是 Clause::TimeKind 的枚举序号（0=kDay、1=kDayRange、
// 2=kLastDays），调用方直接 static_cast 装回枚举：这个整数映射是既有约定，
// 新增 TimeKind 时只能追加，不能插在中间。
//
// kLastDays 故意不在这里换算成时间戳：规则可能在长驻进程里活很久，"最近
// 7 天"的锚点是匹配那一刻的 now，而不是解析那一刻。
//
// 失败时写 error_message 并返回 false，*kind / *low / *high 一律不写。
// mtime 取值换算成闭区间 [low, high]。kLastDays 只记天数，匹配时再拿
// "现在"去算，避免长驻进程把"今天"固定在启动那一刻。
bool ParseMtimeValue(const std::string& value, int* kind, std::int64_t* low,
                     std::int64_t* high, std::int64_t* days_back,
                     std::string* error_message) {
  if (value == "today" || value == "yesterday") {
    const bool is_today = value == "today";
    const std::time_t now = std::time(nullptr);
    struct tm local;
    localtime_r(&now, &local);
    const int year = local.tm_year + 1900;
    const int month = local.tm_mon + 1;
    const int day = local.tm_mday;
    // today     = [今天 00:00, 明天 00:00 - 1]
    // yesterday = [昨天 00:00, 今天 00:00 - 1]
    // 上界一律写成"下一天的开始减一秒"，DST 那天才会自动变成 23 / 25 小时。
    std::int64_t start = 0;
    std::int64_t next_start = 0;
    if (!LocalDayStart(year, month, day, is_today ? 0 : -1, &start) ||
        !LocalDayStart(year, month, day, is_today ? 1 : 0, &next_start)) {
      SetError(error_message, "Invalid filter rule: bad mtime value " + value);
      return false;
    }
    *kind = 0;  // kDay
    *low = start;
    *high = next_start - 1;
    return true;
  }
  if (value.size() > 4 && value.compare(value.size() - 4, 4, "days") == 0) {
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
    int start_year = 0;
    int start_month = 0;
    int start_day = 0;
    int end_year = 0;
    int end_month = 0;
    int end_day = 0;
    std::int64_t start = 0;
    std::int64_t end = 0;
    if (!ParseDate(value.substr(0, range), &start_year, &start_month,
                   &start_day, &start) ||
        !ParseDate(value.substr(range + 2), &end_year, &end_month, &end_day,
                   &end) ||
        !LocalDayEnd(end_year, end_month, end_day, &end)) {
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
    *high = end;  // 结束日的最后一秒 = 次日 00:00 - 1
    return true;
  }
  int year = 0;
  int month = 0;
  int day = 0;
  std::int64_t start = 0;
  std::int64_t end = 0;
  if (!ParseDate(value, &year, &month, &day, &start) ||
      !LocalDayEnd(year, month, day, &end)) {
    SetError(error_message,
             "Invalid filter rule: invalid mtime value " + value);
    return false;
  }
  *kind = 0;  // kDay
  *low = start;
  *high = end;
  return true;
}

// ---- 规则文本 -----------------------------------------------------------

// 字段白名单，也是整个 DSL 的词汇表。SplitClauses 用它判断空白后面是不是
// 一个新子句的开始，ParseClause 用它判断字段名是否合法——两处必须共用这
// 一张表，否则会出现切得开却解析不了、或者反过来漏切的分叉。
bool IsKnownField(const std::string& field) {
  return field == "name" || field == "path" || field == "stem" ||
         field == "ext" || field == "type" || field == "size" ||
         field == "mtime" || field == "uid" || field == "gid" ||
         field == "user" || field == "group";
}

// 把一条规则文本切成子句。分隔符不是空白本身，而是空白加已知字段名加冒号：
//   * name:my report.txt 里的空格属于文件名，不是分隔符；
//   * type:folder path:**/build 之间的空白才是真的子句边界。
// 切不出任何子句时写 "empty rule" 并返回空 vector，调用方据此直接失败，
// 不会拿到零个子句的合法规则。
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

// 单个子句 -> (字段, 原值)。只按第一个 ':' 切开，因为值本身可以含 ':'。
//
// 后置条件：成功时 field_out 与 value_out 都被写入，且字段在白名单内、值
// 非空；失败时两者都不写，只写 error_message。范围的合法性不在这里判定，
// 留到 AddRule 的对应分支——那里才知道这个值该按哪种类型解释。
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

// 单个子句的匹配。纯函数：只读 clause 与 entry，不碰磁盘、不改任何状态，
// 因此同一份 Filter 可以被多个线程并行查询。
// 没有匹配失败这种结果：每个分支都必然给出 true 或 false。
bool Filter::ClauseMatches(const Clause& clause,
                           const FilterEntry& entry) const {
  // low 就是用户写的那一侧边界，high 只在 kRange 时有意义；uid / gid 的
  // kEqual 来自裸数字，size 的等于被 builder 展开成 a..a，两条路径最终都
  // 落到这张表上。比较本身是无符号的：uid / gid / size 都不可能为负。
  // size / uid / gid 共用同一套数值比较；闭区间两端都算命中。
  const auto numeric_match = [](std::uint64_t value, Clause::Compare compare,
                                std::uint64_t low, std::uint64_t high) {
    switch (compare) {
      case Clause::Compare::kLess:
        return value < low;
      case Clause::Compare::kLessEqual:
        return value <= low;
      case Clause::Compare::kGreater:
        return value > low;
      case Clause::Compare::kGreaterEqual:
        return value >= low;
      case Clause::Compare::kEqual:
        return value == low;
      case Clause::Compare::kRange:
        return value >= low && value <= high;
    }
    return false;
  };
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
    case Clause::Field::kType: {
      // type:file 只表示普通文件。它曾经等价于 !is_directory，于是 symlink /
      // FIFO / 字符设备 / 块设备 / socket 统统被"普通文件"命中——而界面上那个
      // 下拉框写的正是"普通文件"，DSL 里也早有 type:symlink 等独立取值。
      // 旧调用方不受影响：EntryType 的默认值就是 kRegularFile，只填
      // is_directory=false 的条目照旧被当成普通文件。
      // 边界：扫描层为硬链接去重生成的 kHardLink 条目不是源目录里的类型，
      // 因此不命中 type:file（它的 size 也被清零，见下面的 size 分支）。
      if (clause.type_kind == Clause::TypeKind::kFile) {
        return !entry.is_directory && entry.type == EntryType::kRegularFile;
      }
      // type:folder 仍然只看 is_directory：旧调用方没有 type 可填。
      if (clause.type_kind == Clause::TypeKind::kFolder) {
        return entry.is_directory;
      }
      if (entry.is_directory) {
        return false;
      }
      switch (clause.type_kind) {
        case Clause::TypeKind::kSymlink:
          return entry.type == EntryType::kSymlink;
        case Clause::TypeKind::kFifo:
          return entry.type == EntryType::kFifo;
        case Clause::TypeKind::kCharDevice:
          return entry.type == EntryType::kCharDevice;
        case Clause::TypeKind::kBlockDevice:
          return entry.type == EntryType::kBlockDevice;
        case Clause::TypeKind::kSocket:
          return entry.type == EntryType::kSocket;
        case Clause::TypeKind::kFile:
        case Clause::TypeKind::kFolder:
          return false;  // 上面已经处理，这里只为穷尽枚举
      }
      return false;
    }
    case Clause::Field::kSize:
      // size 只对普通文件有意义：扫描层把目录与 symlink / FIFO / 设备 /
      // socket 的 size 一律置 0，若照旧参与比较，用户写 exclude size:<=1KB
      // 会把它们全部误伤——而写 size: 时想的显然是文件内容的长度。
      if (!entry.is_directory && entry.type == EntryType::kRegularFile) {
        return numeric_match(entry.size, clause.compare, clause.size_low,
                             clause.size_high);
      }
      return false;
    case Clause::Field::kUid:
      return numeric_match(entry.uid, clause.compare, clause.uid_low,
                           clause.uid_high);
    case Clause::Field::kGid:
      return numeric_match(entry.gid, clause.compare, clause.gid_low,
                           clause.gid_high);
    case Clause::Field::kUser:
      // 精确匹配、大小写敏感。user_name 为空（解析失败）时不匹配：既不报错
      // 也不崩，更不会把"读不出名字"当成"匹配所有用户"。
      return !entry.user_name.empty() && entry.user_name == clause.user_name;
    case Clause::Field::kGroup:
      return !entry.group_name.empty() && entry.group_name == clause.group_name;
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

// 一条规则 = 它所有子句的 AND。空子句列表返回 false 而不是 true：空规则
// 会匹配一切，那正是用户写错却看不出后果的情况；AddRule 与这里的双重保护
// 让空规则不可能被理解成全匹配。
bool Filter::RuleMatches(const Rule& rule, const FilterEntry& entry) const {
  for (const Clause& clause : rule.clauses) {
    if (!ClauseMatches(clause, entry)) {
      return false;  // 同一规则内的多个子句是 AND
    }
  }
  return !rule.clauses.empty();
}

// 动作维度上的 OR：只要有一条同向规则命中就算命中。
// 线性扫描是刻意的——规则数量是人手写出来的个位数，排序或建索引带来的
// 复杂度与顺序即优先级这样的隐式约定都不划算。
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

// 目录剪枝。这是语义而不只是优化：剪掉之后子树里的 symlink / FIFO /
// socket 不再被遍历，也就不会再触发备份遇到不支持类型的失败。
//
// 下面第二段只对整条规则都是 path 子句的 exclude 生效：规则里只要混进
// type: / size: 等条件，就不能只凭路径决定整棵子树，否则那些条件会被绕过。
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

// 归档是否收录这个条目。三种来源：被任意 exclude 命中 -> 否；一条
// include 都没有 -> 是（行为与未引入筛选时逐字一致）；否则必须命中至少
// 一条 include。目录也走这个函数（保留结构）：要不要连子树一起剪掉是
// ShouldPruneDirectory 的问题，两个问题分开回答。
bool Filter::ShouldIncludeFile(const FilterEntry& entry) const {
  if (MatchesAny(FilterAction::kExclude, entry)) {
    return false;  // exclude 优先
  }
  if (!has_include()) {
    return true;  // 没有 include 时默认全收
  }
  return MatchesAny(FilterAction::kInclude, entry);
}

// 特殊文件（symlink / FIFO / socket / 设备）默认让整次备份失败，这是
// fail-closed：静默跳过会让用户以为备份成功且完整。
// 只有用户明确写了 exclude 才算授权跳过；include 规则不构成这种授权，
// 所以这里只查 exclude。
bool Filter::ShouldSkipSpecialEntry(const FilterEntry& entry) const {
  // 只有明确的 exclude 才能让特殊文件被跳过；否则调用方仍然报错。
  return MatchesAny(FilterAction::kExclude, entry);
}

// 解析并追加一条规则。事务语义：整条文本全部解析成功才 push_back，中途
// 任何一步失败都直接返回 false，rules_ 保持调用前的样子。
// 进入时先清空 error_message，避免调用方在一次成功调用后读到上次的残留。
//
// 失败时写 error_message（仅当指针非空），文案以 "Invalid filter rule: "
// 开头——CLI 与 GUI 共用这句报错，前缀属于对外契约，改动前要确认没有测试
// 或文档按前缀匹配。
//
// rule.text 原样保留用户写的那一行，只用于展示与报错，不参与匹配：匹配全部
// 走解析后的 Clause，避免展示文本和判定依据各说各话。
bool Filter::AddRule(FilterAction action, const std::string& text,
                     std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  if (text.empty()) {
    SetError(error_message, "Invalid filter rule: empty rule");
    return false;
  }
  // uid:/gid: 共用的解析：裸数字表示"等于"，另支持 < <= > >= 与 a..b 闭区间。
  // 写成 AddRule 内的 lambda，是为了能直接写出私有的 Clause::Compare 类型。
  const auto parse_id = [](const std::string& field, const std::string& value,
                           Clause::Compare* compare, std::uint32_t* low,
                           std::uint32_t* high,
                           std::string* error_message) -> bool {
    const std::size_t range = value.find("..");
    if (range != std::string::npos) {
      std::uint32_t start = 0;
      std::uint32_t end = 0;
      if (!ParseIdNumber(field, value.substr(0, range), &start,
                         error_message) ||
          !ParseIdNumber(field, value.substr(range + 2), &end, error_message)) {
        return false;
      }
      if (end < start) {
        SetError(error_message, "Invalid filter rule: " + field +
                                    " range is reversed: " + field + ":" +
                                    value);
        return false;
      }
      *compare = Clause::Compare::kRange;
      *low = start;
      *high = end;
      return true;
    }
    Clause::Compare kind = Clause::Compare::kEqual;
    std::string rest = value;
    if (value.compare(0, 2, "<=") == 0) {
      kind = Clause::Compare::kLessEqual;
      rest = value.substr(2);
    } else if (value.compare(0, 2, ">=") == 0) {
      kind = Clause::Compare::kGreaterEqual;
      rest = value.substr(2);
    } else if (value[0] == '<') {
      kind = Clause::Compare::kLess;
      rest = value.substr(1);
    } else if (value[0] == '>') {
      kind = Clause::Compare::kGreater;
      rest = value.substr(1);
    }
    std::uint32_t bound = 0;
    if (!ParseIdNumber(field, rest, &bound, error_message)) {
      return false;
    }
    *compare = kind;
    *low = bound;
    *high = bound;
    return true;
  };
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
        clause.type_kind = Clause::TypeKind::kFile;
      } else if (value == "folder") {
        clause.type_kind = Clause::TypeKind::kFolder;
      } else if (value == "symlink") {
        clause.type_kind = Clause::TypeKind::kSymlink;
      } else if (value == "fifo") {
        clause.type_kind = Clause::TypeKind::kFifo;
      } else if (value == "char") {
        clause.type_kind = Clause::TypeKind::kCharDevice;
      } else if (value == "block") {
        clause.type_kind = Clause::TypeKind::kBlockDevice;
      } else if (value == "socket") {
        clause.type_kind = Clause::TypeKind::kSocket;
      } else {
        SetError(error_message,
                 "Invalid filter rule: unknown type '" + value +
                     "' (expected file, folder, symlink, fifo, char, block "
                     "or socket)");
        ok = false;
      }
      // size
      // 没有裸数字形式：省略运算符没有唯一合理的默认解释（小于还是等于？），
      // 因此明确要求写 < <= > >= 或 a..b 区间，而不是替用户猜一个。
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
      // uid / gid 的裸数字表示等于（uid:1000），也支持 < <= > >= 与 a..b。
      // 与 size 的差别正在这里：size 拒绝裸数字，uid 接受，因为只看某个用户
      // 是最常见的用法。
    } else if (field == "uid" || field == "gid") {
      clause.field =
          (field == "uid") ? Clause::Field::kUid : Clause::Field::kGid;
      Clause::Compare compare = Clause::Compare::kEqual;
      std::uint32_t low = 0;
      std::uint32_t high = 0;
      if (!parse_id(field, value, &compare, &low, &high, error_message)) {
        ok = false;
      } else if (field == "uid") {
        clause.compare = compare;
        clause.uid_low = low;
        clause.uid_high = high;
      } else {
        clause.compare = compare;
        clause.gid_low = low;
        clause.gid_high = high;
      }
    } else if (field == "user" || field == "group") {
      clause.field =
          (field == "user") ? Clause::Field::kUser : Clause::Field::kGroup;
      if (field == "user") {
        clause.user_name = value;
      } else {
        clause.group_name = value;
      }
    } else if (field == "mtime") {
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
    } else {
      // IsKnownField 已经过滤过一次，这里只是穷尽分支，正常不可达。
      SetError(error_message,
               "Invalid filter rule: unknown field '" + field + "'");
      ok = false;
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
