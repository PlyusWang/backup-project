// simple_json.cpp

// 模块职责：本项目唯一的通用 JSON 语法层——解析器（JsonParser）、值树
// （JsonValue）与严格 schema 绑定辅助（Require*），只做"文本 <-> 值"的转换，
// 不认识任何业务字段。
//
// 边界：schema 绑定不在这里。"哪个对象有哪几个字段、什么类型、什么范围"是各
// 调用方自己的知识（schedule_store.cpp / realtime_store.cpp），它们用
// RequireExactFields + RequireString 等逐字段读走，错误文本里带 what 名。
//
// 数据流（读）：文本 -> ParseJson -> JsonValue 树 -> Require* 校验取值 ->
// 调用方结构体。写方向只有 WriteJsonString：调用方自己拼文档骨架，本模块只
// 保证每个字符串字面量都转义成合法 JSON。
//
// 严格性（每条都有测试）：整数只认十进制、无小数与指数；重复 key 报错；解析
// 结束后还有多余字节报错；深度 16 / 节点 20 万 / 单串 1 MiB / 文档 4 MiB 四
// 道硬上界把病态输入变成可预期的失败，而不是内存爆炸。
//
// 失败语义：所有函数都不抛异常，返回 bool 并把原因写进 error_message（允许为
// nullptr，此时只丢弃文本）。失败是整体的：只有返回 true 才能读取结果；false
// 时目标 JsonValue 里可能是半截内容，调用方不得使用。
//
// 线程与生命周期：状态全在局部对象里，没有全局量，可被多个线程同时调用；
// Require* 返回的指针指向调用方持有的那棵树，树被改动或销毁后即失效。
// 文件结构：匿名 namespace 里是错误写入、解析器与 schema 报错出口；其后的公开
// 入口依次是 JsonValue::Find、ParseJson、Require* 家族与 WriteJsonString。
#include "simple_json.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace backupproject {
// 匿名 namespace：以下内容全部 internal linkage，头文件不导出其中任何一个名字——
// 解析器状态机、错误前缀、类型描述都只是实现细节，调用方只能看到
// JsonValue / ParseJson / Require* / WriteJsonString 这些公开入口。
namespace {

// error_message 允许为 nullptr（有些调用方只关心成败），所以每次写前都要判空。
// 写入的是"第一个失败原因"：调用方失败即返回，后续失败不会再覆盖它。
void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) *error_message = text;
}

// 单遍递归下降解析器：position_ 只前进、不回退，所以整体是 O(n) 且无需回溯。
// input_ 是引用，必须比本对象活得久；解析失败时目标 JsonValue 可能只写了一半
// （例如对象只填了部分成员），调用方只能按返回值判断结果是否可用。
// 不可拷贝、不可复用：position_ 只前进不回退，一次 Parse 用完即弃。中间结果直接
// 写进调用方给的 JsonValue 树（就地增长），本类不持有也不释放任何资源。
// 递归深度与调用栈同阶，由 kMaxJsonDepth 兜底，病态嵌套不会爆栈。
class JsonParser {
 public:
  JsonParser(const std::string& input, std::string* error_message)
      : input_(input), error_message_(error_message) {}

  // 入口顺序：先拒绝超大文档（在做任何分配之前），再解析，最后要求正好停在
  // 输入末尾——"后面还跟着别的东西"也算错，否则被截断的配置文件会被当成完整
  // 文档读进来，缺字段的问题要到更深的地方才暴露。
  // 四道上界各自拦一类病态输入：字节数（原文规模）、节点数（宽——大量短值）、
  // 深度（深——递归爆栈）、单串长度（一个值吃光配额）。数值见 simple_json.h；
  // 超限走正常失败路径：不抛异常，也不回滚已建好的那部分树，调用方整棵丢弃。
  bool Parse(JsonValue* value) {
    if (input_.size() > kMaxJsonBytes) {
      return Fail("JSON document is too large: " +
                  std::to_string(input_.size()) + " bytes");
    }
    SkipWhitespace();
    if (!ParseValue(value, 0)) return false;
    SkipWhitespace();
    if (position_ != input_.size()) {
      return Fail("unexpected data after the JSON value");
    }
    return true;
  }

 private:
  // 统一前缀，让用户一眼看出是"文件内容不合法"，而不是"文件读不出来"。
  bool Fail(const std::string& detail) {
    SetError(error_message_, "Invalid JSON: " + detail);
    return false;
  }

  // 只接受 JSON 规定的四种空白（空格 / \t / \n / \r）；别的空白字符不认。
  void SkipWhitespace() {
    while (position_ < input_.size()) {
      const char character = input_[position_];
      if (character == ' ' || character == '\t' || character == '\n' ||
          character == '\r') {
        ++position_;
        continue;
      }
      break;
    }
  }

  // 匹配则前进一格；不匹配时位置不动，由调用方决定报什么错。
  bool Consume(char expected) {
    if (position_ < input_.size() && input_[position_] == expected) {
      ++position_;
      return true;
    }
    return false;
  }

  // 只在首字符已确认匹配时调用（'t' / 'f' / 'n'）；失败时不消费任何字符，
  // 因此不需要回退位置。
  bool ConsumeLiteral(const char* literal) {
    std::size_t index = 0;
    while (literal[index] != '\0') {
      if (position_ + index >= input_.size() ||
          input_[position_ + index] != literal[index]) {
        return false;
      }
      ++index;
    }
    position_ += index;
    return true;
  }

  // 每个值算一个节点。计数与深度检查都在 ParseValue 入口，两者一起把"病态
  // 嵌套"和"病态宽度"的输入挡在分配之前。
  bool CountNode() {
    if (++nodes_ > kMaxJsonNodes) {
      return Fail("JSON document has too many values");
    }
    return true;
  }

  // 值分派。kind 在解析内容之前就写好，所以失败时目标值是"半成品"——这正是
  // 调用方必须检查返回值的原因。深度用 > 判定：kMaxJsonDepth 层容器合法，
  // 第 17 层容器才被拒绝。
  // 接受的文法只是 JSON 的一个真子集，拒绝项都是刻意的：
  //   * 小数与指数 —— 项目里所有数值字段都是整数，多一种表示就多一处不一致；
  //   * \uXXXX 除 \u00XX 外的形式 —— 写侧只产出这一种，见 ParseString；
  //   * 重复 key / 裸控制字符 / 结尾多余字节 —— 会让“文件里写了什么”与“程序实际
  //     用了什么”悄悄分叉，宁可 fail-closed；表外字符（含 UTF-8 BOM）直接报错。
  bool ParseValue(JsonValue* value, std::size_t depth) {
    if (depth > kMaxJsonDepth) return Fail("JSON nesting is too deep");
    if (!CountNode()) return false;
    if (position_ >= input_.size()) return Fail("unexpected end of input");

    const char character = input_[position_];
    if (character == '{') return ParseObject(value, depth);
    if (character == '[') return ParseArray(value, depth);
    if (character == '"') {
      value->kind = JsonValue::Kind::kString;
      return ParseString(&value->text);
    }
    if (character == 't') {
      if (!ConsumeLiteral("true")) return Fail("invalid literal");
      value->kind = JsonValue::Kind::kBool;
      value->boolean = true;
      return true;
    }
    if (character == 'f') {
      if (!ConsumeLiteral("false")) return Fail("invalid literal");
      value->kind = JsonValue::Kind::kBool;
      value->boolean = false;
      return true;
    }
    if (character == 'n') {
      if (!ConsumeLiteral("null")) return Fail("invalid literal");
      value->kind = JsonValue::Kind::kNull;
      return true;
    }
    if (character == '-' || (character >= '0' && character <= '9')) {
      value->kind = JsonValue::Kind::kNumber;
      return ParseNumber(&value->number);
    }
    return Fail(std::string("unexpected character '") + character + "'");
  }

  // 成员按出现顺序追加（保序，见 JsonValue::object）。重复 key 直接报错而不是
  // 后者覆盖前者：否则"配置里写了什么"与"程序实际用了什么"会悄悄分叉，而用户
  // 看到的却是保存成功。
  // 重复 key 的检查是对已收成员的线性扫描（整体 O(n^2)）：文档字段数都在几十，
  // 建哈希表反而更重；等真出现上千字段的对象再换结构也不迟。
  bool ParseObject(JsonValue* value, std::size_t depth) {
    if (!Consume('{')) return Fail("expected '{'");
    value->kind = JsonValue::Kind::kObject;
    SkipWhitespace();
    if (Consume('}')) return true;
    while (true) {
      SkipWhitespace();
      std::string key;
      if (!ParseString(&key)) return false;
      SkipWhitespace();
      if (!Consume(':')) return Fail("expected ':' after an object key");
      SkipWhitespace();
      JsonValue member;
      if (!ParseValue(&member, depth + 1)) return false;
      // 重复 key 一律报错，不做"后者覆盖前者"。
      for (const std::pair<std::string, JsonValue>& existing : value->object) {
        if (existing.first == key) {
          return Fail("duplicate object key '" + key + "'");
        }
      }
      value->object.emplace_back(std::move(key), std::move(member));
      SkipWhitespace();
      if (Consume('}')) return true;
      if (!Consume(',')) return Fail("expected ',' or '}' in an object");
    }
  }

  // 元素顺序即文件顺序；解析不排序、不去重，每个元素都走一次 ParseValue。
  bool ParseArray(JsonValue* value, std::size_t depth) {
    if (!Consume('[')) return Fail("expected '['");
    value->kind = JsonValue::Kind::kArray;
    SkipWhitespace();
    if (Consume(']')) return true;
    while (true) {
      SkipWhitespace();
      JsonValue element;
      if (!ParseValue(&element, depth + 1)) return false;
      value->array.push_back(std::move(element));
      SkipWhitespace();
      if (Consume(']')) return true;
      if (!Consume(',')) return Fail("expected ',' or ']' in an array");
    }
  }

  // 只接受十进制整数文法：可选 '-' + 至少一位数字，禁止前导零；出现 '.'、'e'、
  // 'E' 时给出"只支持整数"的明确原因，而不是让上层报"多余字符"。
  // 溢出在乘法之前判断（magnitude > (UINT64_MAX - digit) / 10），所以不会回绕；
  // 范围与 int64 的两个极值比较，-2^63 单独处理，避免对 2^63 取负造成 UB。
  bool ParseNumber(std::int64_t* value) {
    const std::size_t start = position_;
    bool negative = false;
    if (Consume('-')) negative = true;
    if (position_ >= input_.size() || input_[position_] < '0' ||
        input_[position_] > '9') {
      return Fail("a number must have at least one digit");
    }
    if (input_[position_] == '0' && position_ + 1 < input_.size() &&
        input_[position_ + 1] >= '0' && input_[position_ + 1] <= '9') {
      return Fail("a number must not have leading zeros");
    }
    std::uint64_t magnitude = 0;
    while (position_ < input_.size() && input_[position_] >= '0' &&
           input_[position_] <= '9') {
      const std::uint64_t digit =
          static_cast<std::uint64_t>(input_[position_] - '0');
      if (magnitude > (UINT64_MAX - digit) / 10u) {
        return Fail("a number is out of range");
      }
      magnitude = magnitude * 10u + digit;
      ++position_;
    }
    if (position_ < input_.size()) {
      const char trailing = input_[position_];
      if (trailing == '.' || trailing == 'e' || trailing == 'E') {
        return Fail("only integers are supported (no fractions or exponents)");
      }
    }
    constexpr std::uint64_t kPositiveLimit = 9223372036854775807ull;
    constexpr std::uint64_t kNegativeLimit = 9223372036854775808ull;
    if (negative) {
      if (magnitude > kNegativeLimit) return Fail("a number is out of range");
      *value = (magnitude == kNegativeLimit)
                   ? INT64_MIN
                   : -static_cast<std::int64_t>(magnitude);
      return true;
    }
    if (magnitude > kPositiveLimit) return Fail("a number is out of range");
    *value = static_cast<std::int64_t>(magnitude);
    // start 曾用于把出错位置写进错误文本，现在错误信息里没有位置，这一行只剩
    // 显式压制"未使用变量"告警的作用。
    (void)start;
    return true;
  }

  // 转义白名单就是写侧会产出的那些（见 WriteJsonString），外加 \u00XX（XX <
  // 0x20）这一格，原因见下面的 case 'u'。裸控制字符（< 0x20）一律拒绝：它们会
  // 让"字符串"在别的工具里被当成结构，也会让日志与终端行为不可预期。
  // 对象 key 与字符串值共用这一个函数：key 因此同样受 1 MiB 上界约束、同样拒绝
  // 裸控制字符，也同样是“先 clear 再填”——失败时目标串已被清空。
  bool ParseString(std::string* value) {
    if (!Consume('"')) return Fail("expected a JSON string");
    value->clear();
    while (position_ < input_.size()) {
      const char character = input_[position_++];
      if (character == '"') return true;
      if (static_cast<unsigned char>(character) < 0x20) {
        return Fail("a raw control character appears in a JSON string");
      }
      if (character != '\\') {
        value->push_back(character);
        if (value->size() > kMaxJsonStringBytes) {
          return Fail("a JSON string is too long");
        }
        continue;
      }
      if (position_ >= input_.size()) return Fail("truncated escape sequence");
      const char escaped = input_[position_++];
      switch (escaped) {
        case '"':
          value->push_back('"');
          break;
        case '\\':
          value->push_back('\\');
          break;
        case '/':
          value->push_back('/');
          break;
        case 'b':
          value->push_back('\b');
          break;
        case 'f':
          value->push_back('\f');
          break;
        case 'n':
          value->push_back('\n');
          break;
        case 'r':
          value->push_back('\r');
          break;
        case 't':
          value->push_back('\t');
          break;
        case 'u':
          // 写侧对 C0 控制区里没有专用短转义的那些字节（0x00-0x07、0x0B、
          // 0x0E-0x1F）只能写成 \u00XX —— JSON 不允许裸控制字节出现在字符串里，
          // 而 \b \f \n \r \t 之外的控制字节没有短转义可用。
          // 所以读侧必须认得**正是这一种**形式，否则就是"写得出去、读不回来"：
          // ScheduleStore 会写出一份自己再也读不进来的 schedule.json，而且连
          // "再 set 一次"都修不好（set 也要先读）。两端各自的理由互相以为对方
          // 是另一种样子，这里把写侧真正会产出的那一格补上。
          //
          // 只认 \u00XX 且 XX < 0x20。其余 \uXXXX 一律继续拒绝：写侧只有这一条
          // \u 路径，从不产生别的形式，UTF-16 代理对就更没有理由在这里猜。
          if (position_ + 4 > input_.size()) {
            return Fail("truncated \\u escape sequence");
          }
          {
            const bool zeros =
                input_[position_] == '0' && input_[position_ + 1] == '0';
            int code = 0;
            bool hex_ok = true;
            for (int index = 0; index < 2; ++index) {
              const char digit = input_[position_ + 2 + index];
              int nibble = -1;
              if (digit >= '0' && digit <= '9') {
                nibble = digit - '0';
              } else if (digit >= 'a' && digit <= 'f') {
                nibble = digit - 'a' + 10;
              } else if (digit >= 'A' && digit <= 'F') {
                nibble = digit - 'A' + 10;
              }
              if (nibble < 0) {
                hex_ok = false;
                break;
              }
              code = code * 16 + nibble;
            }
            if (!zeros || !hex_ok || code >= 0x20) {
              return Fail("unsupported JSON escape '\\u'");
            }
            position_ += 4;
            // 只有确认是 \u00XX 才前进 4 个字符：半认半不认会让位置漂移。
            value->push_back(static_cast<char>(code));
            // code 可以是 0x00：解析结果因此可能内嵌 NUL，调用方必须按长度
            // （而不是 C 字符串）使用这个 std::string。
          }
          break;
        default:
          return Fail(std::string("unsupported JSON escape '\\") + escaped +
                      "'");
      }
      if (value->size() > kMaxJsonStringBytes) {
        return Fail("a JSON string is too long");
      }
    }
    return Fail("unterminated JSON string");
  }

  // 解析器状态：输入引用 + 当前偏移 + 已消费的节点数。三者都不共享，所以一个
  // 实例只在一次 Parse 调用期间有效：position_ 不回卷，复用必然读到错位置。
  const std::string& input_;
  std::string* error_message_;
  std::size_t position_ = 0;
  std::size_t nodes_ = 0;
};

// 只服务于错误信息：把"类型不对"写成用户能读懂的说法（"got a string"），
// 由调用方拼进自己的 what 前缀。
std::string DescribeType(const JsonValue& value) {
  switch (value.kind) {
    case JsonValue::Kind::kNull:
      return "null";
    case JsonValue::Kind::kBool:
      return "a boolean";
    case JsonValue::Kind::kNumber:
      return "a number";
    case JsonValue::Kind::kString:
      return "a string";
    case JsonValue::Kind::kArray:
      return "an array";
    case JsonValue::Kind::kObject:
      return "an object";
  }
  return "an unknown value";
}

// schema 校验的统一出口：文本形如 "Invalid schedule store: <对象名>: <原因>"。
// 前缀是历史沿用的（最早的调用方是 ScheduleStore），现在 realtime_store 也在用
// 同一批 helper，所以它不总是字面准确——调用方只显示，不要解析。
bool Reject(const std::string& what, const std::string& detail,
            std::string* error_message) {
  SetError(error_message, "Invalid schedule store: " + what + ": " + detail);
  return false;
}

}  // namespace

// 保序线性查找：字段个数很少，比建哈希表更省也更简单。返回的指针指向 object
// 内部元素，树被改动或销毁后失效；找不到时返回 nullptr。
const JsonValue* JsonValue::Find(const std::string& key) const {
  for (const std::pair<std::string, JsonValue>& member : object) {
    if (member.first == key) return &member.second;
  }
  return nullptr;
}

// 入口：先清空目标值再解析，保证失败时不会残留上一次的内容；成功时不触碰
// error_message，调用方只按返回值判断。
// 成功路径不会清空 error_message：上一次失败留下的文本仍然在里面。调用方必须
// “先看返回值、再看错误文本”，不能只凭错误文本非空就判定这次失败。
bool ParseJson(const std::string& text, JsonValue* value,
               std::string* error_message) {
  if (value == nullptr) {
    SetError(error_message, "JSON output must not be null");
    return false;
  }
  *value = JsonValue{};
  JsonParser parser(text, error_message);
  return parser.Parse(value);
}

// nullptr 与"类型不对"分开报：前者是字段根本没写，后者是写错了类型，用户要
// 做的修复动作完全不同。
// ---- Require* 家族的统一契约 ----
// 前置：object 必须来自一次成功的解析；解析失败时这些函数没有意义。
// 出参：只有返回 true 时才被写入。false 时不保证出参内容（有的函数会先 clear
// 再逐项填，例如 RequireStringArray），调用方在 false 时必须整体放弃这次读取。
// 错误文本：统一 "<what>: <原因>"，what 由调用方给（"config"、
// "managed_snapshots[3]"），原因区分“缺失 / 类型不对 / 越界”三类修复动作。
// 本家族只读，从不修改树；返回的指针指向调用方持有的那棵树，随树一起失效。
// 线程：纯函数、无共享状态，不同线程读同一棵树也是安全的只读操作。
bool RequireObject(const JsonValue* value, const std::string& what,
                   const JsonValue** out, std::string* error_message) {
  if (value == nullptr || !value->is_object()) {
    return Reject(what,
                  value == nullptr
                      ? "is missing"
                      : "must be an object, got " + DescribeType(*value),
                  error_message);
  }
  *out = value;
  return true;
}

// 三参数版本 = "没有可选字段"，直接委托给四参数版本，保证两条路径的报错逐字
// 一致（错误文案是用户可见契约的一部分）。
bool RequireExactFields(const JsonValue& object,
                        const std::vector<const char*>& fields,
                        const std::string& what, std::string* error_message) {
  return RequireExactFields(object, fields, std::vector<const char*>(), what,
                            error_message);
}

// 封闭世界 schema：fields 少一个报 missing，多一个报 unknown。多一个也报错是
// 刻意的——手改配置最常见的错误就是拼错字段名，静默忽略等于"写错了但程序照
// 跑"。optional 只用于一种情况：新版本追加字段、旧文件仍要能读。
// 两个集合都用线性扫描比较：schema 只有几个字段，不值得为它建哈希表。
bool RequireExactFields(const JsonValue& object,
                        const std::vector<const char*>& fields,
                        const std::vector<const char*>& optional,
                        const std::string& what, std::string* error_message) {
  if (!object.is_object()) {
    return Reject(what, "must be an object", error_message);
  }
  for (const char* field : fields) {
    if (object.Find(field) == nullptr) {
      return Reject(what, std::string("missing required field '") + field + "'",
                    error_message);
    }
  }
  for (const std::pair<std::string, JsonValue>& member : object.object) {
    bool known = false;
    for (const char* field : fields) {
      if (member.first == field) {
        known = true;
        break;
      }
    }
    if (!known) {
      for (const char* field : optional) {
        if (member.first == field) {
          known = true;
          break;
        }
      }
    }
    if (!known) {
      return Reject(what, "unknown field '" + member.first + "'",
                    error_message);
    }
  }
  return true;
}

// 返回的是树内部的指针（不拷贝），调用方必须保证那棵树在读取期间存活；想直接
// 拿元素内容用 RequireStringArray 之类的包装。
bool RequireArray(const JsonValue& object, const char* key,
                  const std::string& what, const JsonValue** out,
                  std::string* error_message) {
  const JsonValue* value = object.Find(key);
  if (value == nullptr) {
    return Reject(what, std::string("missing required field '") + key + "'",
                  error_message);
  }
  if (!value->is_array()) {
    return Reject(what,
                  std::string("field '") + key + "' must be an array, got " +
                      DescribeType(*value),
                  error_message);
  }
  *out = value;
  return true;
}

// 取的是副本。空串是合法值，不与"缺失"混淆：缺失在 Find 那一步就报错了，要
// 不要把空串当成未配置由调用方决定。
bool RequireString(const JsonValue& object, const char* key,
                   const std::string& what, std::string* out,
                   std::string* error_message) {
  const JsonValue* value = object.Find(key);
  if (value == nullptr) {
    return Reject(what, std::string("missing required field '") + key + "'",
                  error_message);
  }
  if (!value->is_string()) {
    return Reject(what,
                  std::string("field '") + key + "' must be a string, got " +
                      DescribeType(*value),
                  error_message);
  }
  *out = value->text;
  return true;
}

// 不做数字 / 字符串强转：JSON 里 "true" 与 true 是两回事，静默转换会让手写
// 配置里的引号错误一直藏到运行时。
bool RequireBool(const JsonValue& object, const char* key,
                 const std::string& what, bool* out,
                 std::string* error_message) {
  const JsonValue* value = object.Find(key);
  if (value == nullptr) {
    return Reject(what, std::string("missing required field '") + key + "'",
                  error_message);
  }
  if (!value->is_bool()) {
    return Reject(what,
                  std::string("field '") + key + "' must be a boolean, got " +
                      DescribeType(*value),
                  error_message);
  }
  *out = value->boolean;
  return true;
}

// 数值字段统一先按 int64 读出，再由更窄的包装函数做范围裁剪与命名。
bool RequireInt64(const JsonValue& object, const char* key,
                  const std::string& what, std::int64_t* out,
                  std::string* error_message) {
  const JsonValue* value = object.Find(key);
  if (value == nullptr) {
    return Reject(what, std::string("missing required field '") + key + "'",
                  error_message);
  }
  if (!value->is_number()) {
    return Reject(what,
                  std::string("field '") + key + "' must be a number, got " +
                      DescribeType(*value),
                  error_message);
  }
  *out = value->number;
  return true;
}

// 闭区间校验，min/max 由调用方给出。先按 int64 读、再比较、最后窄化，所以不
// 存在"负数转成巨大 uint32"的路径；越界文本带实际值与期望区间，用户不用翻源码。
bool RequireUint32(const JsonValue& object, const char* key,
                   const std::string& what, std::uint32_t minimum,
                   std::uint32_t maximum, std::uint32_t* out,
                   std::string* error_message) {
  std::int64_t number = 0;
  if (!RequireInt64(object, key, what, &number, error_message)) return false;
  if (number < static_cast<std::int64_t>(minimum) ||
      number > static_cast<std::int64_t>(maximum)) {
    return Reject(what,
                  std::string("field '") + key +
                      "' is out of range: " + std::to_string(number) +
                      " (expected " + std::to_string(minimum) + ".." +
                      std::to_string(maximum) + ")",
                  error_message);
  }
  *out = static_cast<std::uint32_t>(number);
  return true;
}

// 下界固定 0，上界由调用方给。JSON 数字在本模块里只能表达 int64 范围，超过
// 2^63-1 的值无法表示——刻意的取舍，不值得为此引入大整数。
// 负数显式拒绝：直接 static_cast 会把 -1 变成 18446744073709551615。
bool RequireUint64(const JsonValue& object, const char* key,
                   const std::string& what, std::uint64_t maximum,
                   std::uint64_t* out, std::string* error_message) {
  std::int64_t number = 0;
  if (!RequireInt64(object, key, what, &number, error_message)) return false;
  if (number < 0 || static_cast<std::uint64_t>(number) > maximum) {
    return Reject(what,
                  std::string("field '") + key +
                      "' is out of range: " + std::to_string(number),
                  error_message);
  }
  *out = static_cast<std::uint64_t>(number);
  return true;
}

// 逐元素严格校验：只要有一个不是字符串就整体失败，不做"跳过坏的、留下好的"，
// 那等于悄悄放宽配置。顺序保持文件顺序，不清洗空白、不去重。
bool RequireStringArray(const JsonValue& object, const char* key,
                        const std::string& what, std::vector<std::string>* out,
                        std::string* error_message) {
  const JsonValue* value = nullptr;
  if (!RequireArray(object, key, what, &value, error_message)) return false;
  out->clear();
  out->reserve(value->array.size());
  for (const JsonValue& element : value->array) {
    if (!element.is_string()) {
      return Reject(what,
                    std::string("field '") + key +
                        "' must contain only strings, got " +
                        DescribeType(element),
                    error_message);
    }
    out->push_back(element.text);
  }
  return true;
}

// 写侧契约：输出一定是合法的 JSON 字符串字面量。除五个短转义（\b \f \n \r
// \t）与 " \ 之外，所有 C0 控制字节写成 \u00XX——这正是读侧必须接受
// \u00XX 的原因（读侧只认这一种形式，见 ParseString）。
// 0x80 以上的字节原样透传：本模块假定文本是 UTF-8 但不做校验——控制字节已经
// 全部转义，JSON 结构不可能被内容破坏，而校验 UTF-8 需要额外的依赖与错误语义。
// 追加语义：本函数往 out 末尾写，不 clear、不补换行、不加分隔符——调用方负责先
// 写好字段名前缀（见 schedule_store.cpp 的 Append*Field 系列）。
void WriteJsonString(std::string* out, const std::string& value) {
  static const char kHex[] = "0123456789abcdef";
  out->push_back('"');
  for (const char character : value) {
    const unsigned char byte = static_cast<unsigned char>(character);
    // 先转成 unsigned 再比较：char 是否带符号由实现决定，直接拿 char 与 0x20
    // 比较时，0x80 以上的字节在 signed char 平台上会被误判成控制字符而多转义。
    switch (character) {
      case '"':
        *out += "\\\"";
        continue;
      case '\\':
        *out += "\\\\";
        continue;
      case '\b':
        *out += "\\b";
        continue;
      case '\f':
        *out += "\\f";
        continue;
      case '\n':
        *out += "\\n";
        continue;
      case '\r':
        *out += "\\r";
        continue;
      case '\t':
        *out += "\\t";
        continue;
      default:
        break;
    }
    if (byte < 0x20) {
      *out += "\\u00";
      out->push_back(kHex[(byte >> 4) & 0x0F]);
      out->push_back(kHex[byte & 0x0F]);
      continue;
    }
    out->push_back(character);
  }
  out->push_back('"');
}

}  // namespace backupproject
