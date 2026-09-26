// simple_json.cpp

#include "simple_json.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace backupproject {
namespace {

void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) *error_message = text;
}

class JsonParser {
 public:
  JsonParser(const std::string& input, std::string* error_message)
      : input_(input), error_message_(error_message) {}

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
  bool Fail(const std::string& detail) {
    SetError(error_message_, "Invalid JSON: " + detail);
    return false;
  }

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

  bool Consume(char expected) {
    if (position_ < input_.size() && input_[position_] == expected) {
      ++position_;
      return true;
    }
    return false;
  }

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

  bool CountNode() {
    if (++nodes_ > kMaxJsonNodes) {
      return Fail("JSON document has too many values");
    }
    return true;
  }

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
    (void)start;
    return true;
  }

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

  const std::string& input_;
  std::string* error_message_;
  std::size_t position_ = 0;
  std::size_t nodes_ = 0;
};

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

bool Reject(const std::string& what, const std::string& detail,
            std::string* error_message) {
  SetError(error_message, "Invalid schedule store: " + what + ": " + detail);
  return false;
}

}  // namespace

const JsonValue* JsonValue::Find(const std::string& key) const {
  for (const std::pair<std::string, JsonValue>& member : object) {
    if (member.first == key) return &member.second;
  }
  return nullptr;
}

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

bool RequireExactFields(const JsonValue& object,
                        const std::vector<const char*>& fields,
                        const std::string& what, std::string* error_message) {
  return RequireExactFields(object, fields, std::vector<const char*>(), what,
                            error_message);
}

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

void WriteJsonString(std::string* out, const std::string& value) {
  static const char kHex[] = "0123456789abcdef";
  out->push_back('"');
  for (const char character : value) {
    const unsigned char byte = static_cast<unsigned char>(character);
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
