// simple_json.h
//
// 本项目的第二个手写 JSON 读取器，也是最后一个。
//
// 为什么不引第三方库：ConfigManager 已经立下"配置解析不引依赖"的先例，
// 而且这里要的并不是"通用 JSON"，而是"严格、可报错、有上界"的固定 schema
// 读取——一个通用库反而要额外写一层 schema 校验。
//
// 与 ConfigManager 里那个读取器的关系：那个只认两个字段的平铺对象，直接内嵌
// 在 config_manager.cpp 里；schedule.json 有嵌套对象与数组，再复制一份递归
// 下降没有意义。所以这里抽出一个**通用语法层**（本模块），schema 绑定仍然留在
// ScheduleStore 里——"哪几个字段、什么类型、什么范围"是各模块自己的知识。
//
// 严格性（每一条都有测试）：
//   * 只接受对象 / 数组 / 字符串 / 整数 / true / false / null；
//   * 整数只允许十进制，不接受小数与指数，超出 int64 直接报错；
//   * 字符串转义只接受 ConfigManager 支持的那 7 种；
//     \uXXXX 明确拒绝（写侧从不产生它，读侧就没有理由猜）；
//   * 同一个对象里出现重复 key 一律报错——"后一个覆盖前一个"会让
//     "配置里写了什么"和"程序用了什么"悄悄分叉；
//   * 深度、输入字节数、节点总数都有硬上界，病态输入不会吃光内存；
//   * 解析结束后还有多余字节 = 报错，不接受"后面跟了别的东西"。
//
// 本文件是纯 C++17：不依赖 Qt，也不依赖任何第三方库。

#ifndef BACKUP_PROJECT_INCLUDE_SIMPLE_JSON_H_
#define BACKUP_PROJECT_INCLUDE_SIMPLE_JSON_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace backupproject {

// 单份 JSON 文档的字节上界。
inline constexpr std::size_t kMaxJsonBytes = 4u * 1024u * 1024u;
inline constexpr std::size_t kMaxJsonDepth = 16;
inline constexpr std::size_t kMaxJsonNodes = 200000u;
inline constexpr std::size_t kMaxJsonStringBytes = 1024u * 1024u;

struct JsonValue {
  enum class Kind { kNull, kBool, kNumber, kString, kArray, kObject };

  Kind kind = Kind::kNull;
  bool boolean = false;
  std::int64_t number = 0;
  std::string text;
  std::vector<JsonValue> array;
  // 保序的 key/value 列表。字段很少，线性查找比哈希表更简单也更快。
  std::vector<std::pair<std::string, JsonValue>> object;

  bool is_object() const { return kind == Kind::kObject; }
  bool is_array() const { return kind == Kind::kArray; }
  bool is_string() const { return kind == Kind::kString; }
  bool is_number() const { return kind == Kind::kNumber; }
  bool is_bool() const { return kind == Kind::kBool; }

  // 不存在时返回 nullptr。
  const JsonValue* Find(const std::string& key) const;
};

bool ParseJson(const std::string& text, JsonValue* value,
               std::string* error_message);

// ---- 严格 schema 绑定辅助 ----
//
// what 是给用户看的对象名（"config" / "state" / "managed_snapshots[3]"），
// 报错信息里必须带上它，否则一个嵌套数组里的类型错误根本无法定位。

// fields 必须与对象的 key 集合**完全相等**：少一个报"missing"，多一个报
// "unknown"。这样"配置文件被手改出一个我们看不懂的字段"不会被静默忽略。
bool RequireExactFields(const JsonValue& object,
                        const std::vector<const char*>& fields,
                        const std::string& what, std::string* error_message);

bool RequireObject(const JsonValue* value, const std::string& what,
                   const JsonValue** out, std::string* error_message);
bool RequireArray(const JsonValue& object, const char* key,
                  const std::string& what, const JsonValue** out,
                  std::string* error_message);
bool RequireString(const JsonValue& object, const char* key,
                   const std::string& what, std::string* out,
                   std::string* error_message);
bool RequireBool(const JsonValue& object, const char* key,
                 const std::string& what, bool* out,
                 std::string* error_message);
// 数值字段一律给闭区间：范围检查放在读取处，越界就是明确的配置错误。
bool RequireUint32(const JsonValue& object, const char* key,
                   const std::string& what, std::uint32_t minimum,
                   std::uint32_t maximum, std::uint32_t* out,
                   std::string* error_message);
bool RequireInt64(const JsonValue& object, const char* key,
                  const std::string& what, std::int64_t* out,
                  std::string* error_message);
bool RequireStringArray(const JsonValue& object, const char* key,
                        const std::string& what, std::vector<std::string>* out,
                        std::string* error_message);
bool RequireUint64(const JsonValue& object, const char* key,
                   const std::string& what, std::uint64_t maximum,
                   std::uint64_t* out, std::string* error_message);

// ---- 写出 ----

// 控制字符（除 \b \f \n \r \t 之外，以及这五个本身）全部写成 \uXXXX，
// 保证输出永远是合法 JSON。写侧因此不需要"拒绝控制字符"这条额外规则。
void WriteJsonString(std::string* out, const std::string& value);

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_SIMPLE_JSON_H_
