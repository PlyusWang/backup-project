// filter_rule_builder.cpp
//
// 见 include/filter_rule_builder.h 的设计说明。这里只做字符串拼装和结构校验，
// 不读磁盘、不碰 Qt、不复制任何匹配逻辑。

// 职责：把界面上的规则草稿（FilterRuleDraft / FilterClauseDraft）序列化成
// 核心认识的 DSL 文本，并生成给人看的中文摘要。它是 GUI 与核心之间唯一的
// 翻译层：预览、保存、"复制为 CLI 参数"都走这里，所以同一份草稿在任何出口
// 上都会得到同一串文本。
//
// 职责边界：本文不定义语法，也不复制任何匹配逻辑。判定一条规则合不合法
// 的唯一权威是 Filter::AddRule——ValidateRule 只是把生成的 DSL 交给真实的
// Filter 试一遍。本文不读磁盘、不碰 Qt、不持有状态，全部函数都是纯函数。
//
// 数据流：
//   QML 表单 -> FilterRuleModel::DraftFromForm -> FilterRuleDraft
//            -> 本文 ToDsl -> DSL 文本 -> Filter::AddRule / --include 参数
//            -> 本文 Summarize* -> 界面上的人话摘要
//
// 不变量：
//   * 同一份草稿在 ToDsl 与 Summarize* 里必须被解释成同一件事：两处都按
//     field 分派，新增 RuleField 时两处必须同时补齐；
//   * 本文产出的 DSL 一律能被 Filter::AddRule 接受——ValidateClause 只做
//     界面层拦得住的粗筛，最终裁决在 ValidateRule。
//
// 失败语义：ToDsl / ValidateRule 返回 false 并写 error_message，中文文案
// 直接面向用户；Summarize* 没有失败路径——输入总是已经过校验的草稿，遇到
// 不认识的枚举值只退化成"未知条件"，绝不抛异常。
#include "filter_rule_builder.h"

#include <cstdio>
#include <utility>

namespace backupproject {
namespace {

// 单位 -> DSL 后缀。字节刻意不写后缀（size:1024），其余写 KB / MB / GB；
// 核心的 ParseSizeLiteral 认的就是这四个写法，进制固定 1024。
// 返回值是静态字符串，调用方不需要释放，也不会被改动。
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

// 比较方式 -> DSL 运算符。kEqual 在这里没有对应符号：size 的等于由 ToDsl
// 展开成 a..a，uid / gid 的等于展开成裸数字，两条路径都不经过这个函数。
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

// 这张表在界面侧还有一份副本（filter_rule_model.cpp 的 TypeText）。两处的
// 字符串必须与 Filter::AddRule 里的取值逐个对齐，加新类型时三处一起改。
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

// 展示用中文名，不参与任何解析。界面上出现的"普通文件 / 目录 / 命名管道"
// 全部出自这里，避免同一个概念在不同页面被叫成不同的词。
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

// 纯 ASCII 数字判定，空串算不通过。std::stoi 会接受前导空白和正负号，
// 而 "2024- 1- 1" 这种输入必须由格式检查挡掉，不能靠转换函数兜底。
bool IsDigits(const std::string& text) {
  if (text.empty()) return false;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

// 产出年月日三个整数交给核心的 mtime 规则使用：格式固定十字符，且只做粗筛
// （1..12、1..31），日期是否真实存在由核心的 mktime 归一化比对决定——这里
// 再写一套闰年判断只会多出一处可能与核心不一致的实现。
// 失败时三个出参都不写，调用方只能依赖返回值。
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

// 去掉首尾空白，只认 ASCII 空白（空格 / 制表 / CR / LF）。用户从网页或
// 表格里粘过来的扩展名常带尾随空格，而 QML 的输入框不做 trim。
std::string Trim(const std::string& text) {
  const std::string::size_type first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return std::string();
  const std::string::size_type last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1);
}

// 扩展名的规范化形式：去首尾空白 + 去掉全部前导 '.'，例如 ".txt" -> "txt"。
// 大小写原样保留：核心的 ext 比较是大小写敏感的，在这里折叠一次会让界面
// 显示的规则与实际匹配结果不一致。
std::string NormalizeExtension(const std::string& raw) {
  std::string text = Trim(raw);
  while (!text.empty() && text[0] == '.') text.erase(0, 1);
  return text;
}

// 简单拼接，不做转义。调用点已经保证 items 里不含分隔符：ext 的取值本身
// 以 ';' 分隔，单个取值不可能再含 ';'。
std::string JoinWith(const std::vector<std::string>& items, const char* glue) {
  std::string out;
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i != 0) out += glue;
    out += items[i];
  }
  return out;
}

// 十进制无符号整数，不带千分位、不带正负号：生成的文本会直接进 DSL，必须
// 是核心 ParseUnsigned 认得的写法。32 字节缓冲对 20 位十进制数绰绰有余。
std::string FormatBytes(std::uint64_t value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%llu",
                static_cast<unsigned long long>(value));
  return std::string(buffer);
}

// compare 与 low / high 的搭配由调用方保证：kRange 时 low / high 是闭区间
// 两端，kEqual 时只用 low，其余运算符把 low 当单侧边界。
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

// 摘要与 DSL 用的是同一组判定分支，只是措辞不同：区间写成"在 a 到 b 之间"，
// 等于写成 "= a"，其余带运算符。两处必须一起改，否则界面说的和实际筛的
// 就未必是一回事。
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

// 字段名 -> DSL 关键字。这些字符串就是 Filter::AddRule 的 IsKnownField
// 白名单，改动任何一个都会让已经保存下来的规则再也解析不了。
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

// 字段的中文标签，只用于展示。filter_rule_model.cpp 的 editorOptions()
// 直接把这张表喂给 QML，所以这里的措辞就是用户看到的措辞。
const char* RuleFieldLabel(RuleField field) {
  switch (field) {
    case RuleField::kName:
      return "文件名";
    case RuleField::kPath:
      return "路径";
    case RuleField::kStem:
      return "主文件名";
    case RuleField::kExt:
      return "文件扩展名";
    case RuleField::kType:
      return "文件类型";
    case RuleField::kSize:
      return "文件大小";
    case RuleField::kMtime:
      return "修改时间";
    case RuleField::kUid:
      return "用户 ID";
    case RuleField::kGid:
      return "用户组 ID";
    case RuleField::kUser:
      return "用户名";
    case RuleField::kGroup:
      return "用户组名";
  }
  return "未知条件";
}

// 每个字段的输入提示，逐条说明该字段的语法边界（通配符能不能跨目录、uid
// 是数字而不是用户名、哪些字段不支持通配符）。提示必须与实际解析能力一致：
// 对用户来说，这几句话就是唯一的语法文档。
const char* RuleFieldHint(RuleField field) {
  switch (field) {
    case RuleField::kName:
      return "支持通配符：* 匹配任意多个字符，? 匹配一个字符。";
    case RuleField::kPath:
      return "相对于备份目录的路径。* 只匹配当前目录内的字符，** "
             "可以跨目录匹配。";
    case RuleField::kStem:
      return "不含扩展名的文件名部分，例如 report 之于 report.pdf。";
    case RuleField::kExt:
      return "多个扩展名用分号分隔，例如 txt;md;pdf。不需要输入点号。";
    case RuleField::kType:
      return "按文件在文件系统里的类型筛选，从下拉里选，不需要写语法。";
    case RuleField::kSize:
      return "先选比较方式，再填数字和单位，例如 小于 1 MB。";
    case RuleField::kMtime:
      return "按文件最后一次修改的时间筛选。";
    case RuleField::kUid:
      return "属主的数字 ID（不是用户名），0 是合法值（root）。";
    case RuleField::kGid:
      return "属组的数字 ID（不是用户组名），0 是合法值（root）。";
    case RuleField::kUser:
      return "属主用户名，精确匹配、区分大小写，不支持通配符。";
    case RuleField::kGroup:
      return "属组名，精确匹配、区分大小写，不支持通配符。";
  }
  return "";
}

// 比较方式的中文名。区间特意标注"含两端"，因为核心的 kRange 是闭区间，
// 而"区间"在别的工具里常常是半开的。
const char* SizeCompareLabel(RuleSizeCompare compare) {
  switch (compare) {
    case RuleSizeCompare::kLess:
      return "小于";
    case RuleSizeCompare::kLessEqual:
      return "小于等于";
    case RuleSizeCompare::kGreater:
      return "大于";
    case RuleSizeCompare::kGreaterEqual:
      return "大于等于";
    case RuleSizeCompare::kRange:
      return "区间（含两端）";
    case RuleSizeCompare::kEqual:
      return "等于";
  }
  return "";
}

// 单位标签，同时也是表单键（见 filter_rule_model.cpp 的 kUnits）：B / KB /
// MB / GB 四个字符串既显示给用户，又被反查回枚举。
const char* SizeUnitLabel(RuleSizeUnit unit) {
  switch (unit) {
    case RuleSizeUnit::kByte:
      return "B";
    case RuleSizeUnit::kKilo:
      return "KB";
    case RuleSizeUnit::kMega:
      return "MB";
    case RuleSizeUnit::kGiga:
      return "GB";
  }
  return "";
}

// type 的中文名。单独开一个函数是为了让调用点不必知道内部复用了
// TypeDisplayName，将来要换措辞时也只需要改一处。
const char* TypeValueLabel(RuleTypeValue type) { return TypeDisplayName(type); }

// mtime 五种形态的中文名。kLastDays 的标签里带 N 占位符，实际天数由
// SummarizeClauseShort 拼出来。
const char* MtimeKindLabel(RuleMtimeKind kind) {
  switch (kind) {
    case RuleMtimeKind::kToday:
      return "今天";
    case RuleMtimeKind::kYesterday:
      return "昨天";
    case RuleMtimeKind::kLastDays:
      return "最近 N 天";
    case RuleMtimeKind::kDay:
      return "指定日期";
    case RuleMtimeKind::kDayRange:
      return "日期区间";
  }
  return "";
}

// 界面层的粗筛：只拦一看就没填完的草稿（模式为空、一个扩展名都没有、区间
// 上下界颠倒、日期格式不对），不重复核心的语义判定。
//
// 之所以还要在这里拦一次：ToDsl 会把这些草稿拼成 DSL 文本，如果拖到
// ValidateRule 才失败，用户拿到的就是 DSL 解析器的口吻（例如
// "Invalid filter rule: invalid size value in size:..."），而不是"哪个
// 输入框填错了"。这里的中文文案直接对应表单上的字段名。
//
// 前置条件：clause 已填好；本函数不修改 clause。
// 失败时写 error_message 并返回 false，成功时返回 true 且不碰 error_message。
bool ValidateClause(const FilterClauseDraft& clause,
                    std::string* error_message) {
  // 统一出口：写错误信息并返回 false。error_message 允许是空指针：只关心
  // 成败的调用点（例如只取摘要、不需要报错的序列化路径）传的就是 nullptr。
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
      // 只数规范化之后非空的扩展名：用户在输入框里留下标点（";" 或 "."）
      // 时这条规则实际上一个扩展名都没有，必须报错，而不是生成 ext: 空值。
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
      // uid 与 gid 共用一段：两者只有取哪一组字段不同，复制成两份更容易
      // 出现改了一边忘了另一边。
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

// 单条子句 -> DSL 片段（形如 字段:值，不含动作）。
//
// 前置条件：clause 来自表单或已保存的草稿；本函数自己会先调用
// ValidateClause，因此非法草稿在这里就被拒绝，不会产出半截文本。
// 失败时返回 false、写 error_message，并且不写 *dsl。
//
// 序列化规则（与核心的解析表一一对应，改动必须成对）：
//   name/path/stem  原样拼 pattern，pattern 里的空格是合法内容；
//   ext             多个取值用 ';' 连接，前导 '.' 已在规范化时去掉；
//   type            写 DSL 取值（file / folder / symlink / ...）；
//   size            kEqual 展开成 a..a——核心的 size 没有单独的 "="；
//   mtime           today / yesterday / Ndays / YYYY-MM-DD / a..b；
//   uid/gid         kEqual 是裸数字，区间是 a..b，其余带运算符；
//   user/group      原样拼，精确匹配、大小写敏感，不做任何清洗。
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

// 整条规则 -> DSL 文本：子句之间用一个空格连接，就是核心 SplitClauses 认的
// AND 写法。子句内部的值不允许出现"空白 + 已知字段名 + 冒号"的形状，
// 那正是切分规则本身。
// 空子句列表按失败处理：空规则会匹配一切，不能让它悄悄生成一段空文本。
bool ToDsl(const FilterRuleDraft& rule, std::string* dsl,
           std::string* error_message) {
  // 高级规则的正文由用户手写，这里不解析也不重排：任何"规范化"都会让用户
  // 看到的文本与真正生效的规则不再逐字相同。
  // 高级入口优先：raw_dsl 就是这条规则的完整定义。
  if (!rule.raw_dsl.empty()) {
    if (dsl != nullptr) *dsl = rule.raw_dsl;
    return true;
  }
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

// 草稿是否合法。唯一权威是真实的 Filter::AddRule：这里构造一个临时 Filter，
// 把生成的 DSL 喂进去试一遍，界面层不复制任何语法规则。
// 只在内存里过一遍、不落盘、不注册任何东西，所以可以随手调用（表单每次
// 改动都会调一次）。
bool ValidateRule(const FilterRuleDraft& rule, std::string* error_message) {
  std::string dsl;
  if (!ToDsl(rule, &dsl, error_message)) return false;
  // 最终裁决交给真实核心：GUI 不定义语法。
  Filter probe;
  return probe.AddRule(rule.action, dsl, error_message);
}

// 完整中文摘要，一条子句 = 一句人话（"名称匹配 *.log 的文件"）。
// 与 ToDsl 共用同一套 field 分派：摘要里的每个取值都来自同一个草稿字段，
// 所以"界面说在筛什么"和"实际筛什么"不会分叉。
// 只用于展示，不参与匹配；不认识的枚举值退化成"未知条件"，不抛异常。
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

// 卡片主行用的短摘要，例如 "扩展名：cpp、h"。与 SummarizeClause 的差别
// 只在措辞：主行回答"在筛什么"，完整摘要回答"这条规则是什么意思"。
std::string SummarizeClauseShort(const FilterClauseDraft& clause) {
  switch (clause.field) {
    case RuleField::kName:
      return std::string(RuleFieldLabel(clause.field)) + "：" + clause.pattern;
    case RuleField::kPath:
      return std::string(RuleFieldLabel(clause.field)) + "：" + clause.pattern;
    case RuleField::kStem:
      return std::string(RuleFieldLabel(clause.field)) + "：" + clause.pattern;
    case RuleField::kExt: {
      std::vector<std::string> cleaned;
      for (const std::string& raw : clause.extensions) {
        const std::string value = NormalizeExtension(raw);
        if (!value.empty()) cleaned.push_back(value);
      }
      // 用顿号而不是 " 或 "：主行是"在筛什么"，不是一句话。
      return std::string(RuleFieldLabel(clause.field)) + "：" +
             JoinWith(cleaned, "、");
    }
    case RuleField::kType:
      return std::string(RuleFieldLabel(clause.field)) + "：" +
             TypeValueLabel(clause.type);
    case RuleField::kSize: {
      const std::string unit = SizeUnitLabel(clause.unit);
      const std::string low = FormatBytes(clause.size_low) + " " + unit;
      if (clause.compare == RuleSizeCompare::kRange) {
        return std::string(RuleFieldLabel(clause.field)) + " " + low + " 到 " +
               FormatBytes(clause.size_high) + " " + unit + " 之间";
      }
      return std::string(RuleFieldLabel(clause.field)) + " " +
             SizeCompareLabel(clause.compare) + " " + low;
    }
    case RuleField::kMtime:
      if (clause.mtime_kind == RuleMtimeKind::kLastDays) {
        return std::string(RuleFieldLabel(clause.field)) + "：最近 " +
               std::to_string(clause.days_back) + " 天";
      }
      if (clause.mtime_kind == RuleMtimeKind::kDay) {
        return std::string(RuleFieldLabel(clause.field)) + "：" +
               clause.date_low;
      }
      if (clause.mtime_kind == RuleMtimeKind::kDayRange) {
        return std::string(RuleFieldLabel(clause.field)) + "：" +
               clause.date_low + " 至 " + clause.date_high;
      }
      return std::string(RuleFieldLabel(clause.field)) + "：" +
             MtimeKindLabel(clause.mtime_kind);
    case RuleField::kUid: {
      const std::string value = FormatBytes(clause.uid);
      if (clause.uid_compare == RuleSizeCompare::kRange) {
        return std::string(RuleFieldLabel(clause.field)) + " " + value +
               " 到 " + FormatBytes(clause.uid_high) + " 之间";
      }
      return std::string(RuleFieldLabel(clause.field)) + " " +
             SizeCompareLabel(clause.uid_compare) + " " + value;
    }
    case RuleField::kGid: {
      const std::string value = FormatBytes(clause.gid);
      if (clause.gid_compare == RuleSizeCompare::kRange) {
        return std::string(RuleFieldLabel(clause.field)) + " " + value +
               " 到 " + FormatBytes(clause.gid_high) + " 之间";
      }
      return std::string(RuleFieldLabel(clause.field)) + " " +
             SizeCompareLabel(clause.gid_compare) + " " + value;
    }
    case RuleField::kUser:
      return std::string(RuleFieldLabel(clause.field)) + "：" + clause.user;
    case RuleField::kGroup:
      return std::string(RuleFieldLabel(clause.field)) + "：" + clause.group;
  }
  return "未知条件";
}

// 整条规则的短摘要：多个子句用"，且 "连接，与核心的 AND 语义对应。
// 空草稿返回"未设置条件"而不是空串——界面需要一句能显示出来的话。
std::string SummarizeShort(const FilterRuleDraft& rule) {
  if (!rule.raw_dsl.empty()) {
    // 高级规则的正文由用户自己写：界面不假装读懂了它，主行只说明这是一条
    // 手写规则，原文照常显示在卡片里。
    return "高级规则";
  }
  std::string text;
  for (std::size_t i = 0; i < rule.clauses.size(); ++i) {
    if (i != 0) text += "，且 ";
    text += SummarizeClauseShort(rule.clauses[i]);
  }
  return text.empty() ? std::string("未设置条件") : text;
}

// 带动作前缀（包含 / 排除）的完整一句话，用于汇总面板和规则卡片。
// 高级规则原样回显、不做翻译，理由与 ToDsl 里的那条相同。
std::string Summarize(const FilterRuleDraft& rule) {
  if (!rule.raw_dsl.empty()) {
    // 高级规则原样展示：界面不该假装自己读懂了用户写的 DSL。
    if (rule.action == FilterAction::kInclude)
      return "包含（高级）：" + rule.raw_dsl;
    return "排除（高级）：" + rule.raw_dsl;
  }
  std::string text;
  for (std::size_t i = 0; i < rule.clauses.size(); ++i) {
    if (i != 0) text += "，且";
    text += SummarizeClause(rule.clauses[i]);
  }
  if (rule.action == FilterAction::kInclude) return "包含：" + text;
  return "排除：" + text;
}

// 草稿列表 -> 可以用的 Filter。先在局部变量上构建，全部成功后才整体赋值给
// *filter：失败时调用方手里那份 Filter 保持原样，拿不到成功了一半的规则集。
// filter 允许为空指针（只想校验的场景）。
bool BuildFilterFromDrafts(const std::vector<FilterRuleDraft>& rules,
                           Filter* filter, std::string* error_message) {
  // 与 CliArguments 走同一套序列化：GUI 预览与 "复制为 CLI 参数" 不可能给出
  // 不同的规则文本。
  Filter built;
  for (const FilterRuleDraft& rule : rules) {
    std::string dsl;
    if (!ToDsl(rule, &dsl, error_message)) return false;
    if (!built.AddRule(rule.action, dsl, error_message)) return false;
  }
  if (filter != nullptr) *filter = std::move(built);
  return true;
}

// 草稿列表 -> 命令行参数（"--include" "path:**/build/**" ...），按规则
// 顺序成对产出。
// 序列化失败的草稿被跳过而不是报错：这个函数是给"复制一条能粘贴的命令"
// 用的，为一条界面已经标红的草稿拦住整串输出没有意义。
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
