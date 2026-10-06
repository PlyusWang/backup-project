// config_manager.cpp
//
// The reader below is deliberately not a general JSON parser.  It accepts only
// this module's version-1 schema: an object with `version` and
// `backup_repository_path` fields.

// 本文件负责应用配置 config.json 的读写。读出来的 backup_repository_path 由
// 上层（BackupController / CLI）决定怎么用；本模块不认识仓库、不认识备份，
// 只认识"一个字符串字段 + 一个版本号"。
//
// 磁盘格式 v1（UTF-8 文本，只有两个字段）：
//     {"version": 1, "backup_repository_path": "<路径>"}
// version 不是 1 一律拒绝：宁可让用户看到"版本不支持"，也不要按旧规则去
// 解释一份可能是新版本写出来的配置。
//
// 线程：ConfigManager 只保存一个路径、没有可变状态，Load / Save 都是 const，
// 可被多个线程并发调用；并发写的原子性由 WriteFileAtomicallyReplacing 保证。
#include "config_manager.h"

#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

#include "file_io.h"

namespace backupproject {
namespace {

namespace fs = std::filesystem;

void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) *error_message = text;
}

// 统一的文件系统诊断格式：动作 + 路径 + errno 文本。std::error_code 版本的
// 文件操作不会抛异常，错误只能靠这条字符串带出去。
std::string Describe(const std::string& action, const fs::path& path,
                     const std::error_code& error) {
  return action + ": " + path.string() + ": " + error.message();
}

// 仓库路径最终要写进 JSON 字符串。除 JSON 自己允许的那几个转义之外，任何
// 小于 0x20 的字节都会让产物变成别人解析不了的 JSON —— 在写盘之前就挡住，
// 而不是写出去之后再让下一个读取者抱怨。
bool HasUnsupportedControlCharacter(const std::string& value) {
  for (const char character : value) {
    if (static_cast<unsigned char>(character) >= 0x20) continue;
    if (character != '\b' && character != '\f' && character != '\n' &&
        character != '\r' && character != '\t') {
      return true;
    }
  }
  return false;
}

// 手写的极简 JSON 读取器，只服务上面这一个 schema。它**不是**通用 JSON 解析器：
// 不支持数组、数字只支持非负整数、\uXXXX 转义一律拒绝、不做嵌套。
// 之所以手写而不拉一个库：这里的输入是本地文件，攻击面小，而"接受哪些形状、
// 拒绝哪些形状"必须完全可审计 —— 少一个依赖就少一份解析器 CVE 面。
// 失败契约：任何一步失败都返回 false 并写 error_message，前缀统一是
// "Invalid config JSON: "，便于上层原样展示给用户。
class ConfigJsonReader {
 public:
  explicit ConfigJsonReader(const std::string& input) : input_(input) {}

  // 语法：必须是单个对象、字段顺序任意、不允许尾逗号、对象之后不允许多余
  // 数据。未知字段报错，重复字段也报错（后写覆盖先写只会掩盖编辑事故），
  // 缺 version 或 backup_repository_path 同样报错。
  bool Parse(AppConfig* config, std::string* error_message) {
    SkipWhitespace();
    if (!Consume('{')) return Fail(error_message, "expected JSON object");

    bool saw_version = false;
    bool saw_repository_path = false;
    SkipWhitespace();
    if (Consume('}')) return Fail(error_message, "missing required fields");
    while (true) {
      std::string key;
      if (!ParseString(&key, error_message)) return false;
      SkipWhitespace();
      if (!Consume(':')) return Fail(error_message, "expected ':' after key");
      SkipWhitespace();
      if (key == "version") {
        // 重复字段必须显式报出来。写成 "saw_version || !ParseVersion(...)"
        // 会因为短路直接 return false，error_message 一个字都不写——
        // kError 就成了没有诊断的错误。
        if (saw_version) {
          return Fail(error_message, "duplicate config field: version");
        }
        int version = 0;
        if (!ParseVersion(&version, error_message)) return false;
        if (version != 1) {
          return Fail(error_message,
                      "unsupported config version: " + std::to_string(version));
        }
        saw_version = true;
      } else if (key == "backup_repository_path") {
        if (saw_repository_path) {
          return Fail(error_message,
                      "duplicate config field: backup_repository_path");
        }
        if (!ParseString(&config->backup_repository_path, error_message)) {
          return false;
        }
        saw_repository_path = true;
      } else {
        return Fail(error_message, "unknown config field: " + key);
      }
      SkipWhitespace();
      if (Consume('}')) break;
      if (!Consume(',')) return Fail(error_message, "expected ',' or '}'");
      SkipWhitespace();
    }
    SkipWhitespace();
    if (position_ != input_.size()) {
      return Fail(error_message, "unexpected data after JSON object");
    }
    if (!saw_version || !saw_repository_path) {
      return Fail(error_message, "missing required config field");
    }
    return true;
  }

 private:
  // 所有诊断的统一出口：前缀固定，detail 说明具体原因。
  bool Fail(std::string* error_message, const std::string& detail) const {
    SetError(error_message, "Invalid config JSON: " + detail);
    return false;
  }

  void SkipWhitespace() {
    while (position_ < input_.size() &&
           (input_[position_] == ' ' || input_[position_] == '\n' ||
            input_[position_] == '\r' || input_[position_] == '\t')) {
      ++position_;
    }
  }

  bool Consume(char expected) {
    if (position_ < input_.size() && input_[position_] == expected) {
      ++position_;
      return true;
    }
    return false;
  }

  // 只接受不带前导零的十进制（"01" 被拒绝：那是八进制书写习惯，容易误读），
  // 超长数字交给 stoi 的异常兜底，不让它触发未定义行为。
  bool ParseVersion(int* version, std::string* error_message) {
    const std::size_t start = position_;
    while (position_ < input_.size() && input_[position_] >= '0' &&
           input_[position_] <= '9') {
      ++position_;
    }
    if (start == position_)
      return Fail(error_message, "version must be an integer");
    const std::string value = input_.substr(start, position_ - start);
    if (value.size() > 1 && value[0] == '0') {
      return Fail(error_message, "version must be an integer");
    }
    try {
      *version = std::stoi(value);
    } catch (const std::exception&) {
      return Fail(error_message, "version is out of range");
    }
    return true;
  }

  // 严格 JSON 字符串：值里的控制字符必须先转义，\uXXXX 不支持。读到结尾
  // 还没闭合就报 unterminated，不把已经拼出来的一半当成结果返回。
  bool ParseString(std::string* value, std::string* error_message) {
    if (!Consume('\"')) return Fail(error_message, "expected JSON string");
    value->clear();
    while (position_ < input_.size()) {
      const char character = input_[position_++];
      if (character == '\"') return true;
      if (static_cast<unsigned char>(character) < 0x20) {
        return Fail(error_message, "control character in JSON string");
      }
      if (character != '\\') {
        value->push_back(character);
        continue;
      }
      if (position_ == input_.size())
        return Fail(error_message, "truncated escape");
      switch (input_[position_++]) {
        case '\"':
          value->push_back('\"');
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
          return Fail(error_message, "unsupported JSON string escape");
      }
    }
    return Fail(error_message, "unterminated JSON string");
  }

  // 输入按引用保存：Parse 是一次同步调用，调用方保证字符串在调用期间存活。
  const std::string& input_;
  // 唯一游标。所有 Consume / SkipWhitespace 只推进它、不回退，
  // 因此这是严格解析：读到哪里就是哪里，没有试探性回溯。
  std::size_t position_ = 0;
};

// 与 ParseString 的转义集合对称：不生成 \uXXXX（这类输入已经由
// HasUnsupportedControlCharacter 在 Save 入口挡掉），其余字节原样写出。
std::string EscapeJsonString(const std::string& value) {
  std::string escaped;
  for (const char character : value) {
    switch (character) {
      case '\"':
        escaped += "\\\"";
        break;
      case '\\':
        escaped += "\\\\";
        break;
      case '\b':
        escaped += "\\b";
        break;
      case '\f':
        escaped += "\\f";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        escaped.push_back(character);
        break;
    }
  }
  return escaped;
}

}  // namespace

// 只保存路径，不做任何 I/O：构造不会失败，配置在不在要等 Load 才知道。
ConfigManager::ConfigManager(std::string config_file_path)
    : config_file_path_(std::move(config_file_path)) {}

// 三态返回是刻意的：kMissing（首次运行，正常）与 kError（配置坏了或读不了）
// 对上层是完全不同的处理，合成一个 bool 会把"没有配置"变成"配置错误"。
// 进入时先把 *config 复位成默认值，失败路径上调用方拿到的不含上一次的内容；
// 非普通文件（目录、设备、FIFO）在打开之前就拦掉。
ConfigLoadStatus ConfigManager::Load(AppConfig* config,
                                     std::string* error_message) const {
  if (error_message != nullptr) error_message->clear();
  if (config == nullptr) {
    SetError(error_message, "Config output must not be null");
    return ConfigLoadStatus::kError;
  }
  *config = AppConfig{};

  // 空路径必须在碰文件系统之前拦掉：fs::exists("") 返回 false，会被当成
  // "配置还不存在"而报 kMissing。同一个对象上 Save("") 是明确报错的，
  // 读和写对同一个输入给出不同的结论，接口语义就不一致了。
  if (config_file_path_.empty()) {
    SetError(error_message, "Cannot load config: config file path is empty");
    return ConfigLoadStatus::kError;
  }

  std::error_code error;
  const bool exists = fs::exists(config_file_path_, error);
  if (error) {
    SetError(error_message, Describe("Failed to inspect config file",
                                     config_file_path_, error));
    return ConfigLoadStatus::kError;
  }
  if (!exists) return ConfigLoadStatus::kMissing;
  if (!fs::is_regular_file(config_file_path_, error) || error) {
    SetError(error_message,
             error ? Describe("Failed to inspect config file",
                              config_file_path_, error)
                   : "Config path is not a regular file: " + config_file_path_);
    return ConfigLoadStatus::kError;
  }

  std::ifstream input(config_file_path_, std::ios::binary);
  if (!input) {
    SetError(error_message,
             "Failed to open config file for reading: " + config_file_path_);
    return ConfigLoadStatus::kError;
  }
  std::ostringstream contents;
  contents << input.rdbuf();
  if (input.bad()) {
    SetError(error_message, "Failed to read config file: " + config_file_path_);
    return ConfigLoadStatus::kError;
  }

  AppConfig loaded;
  if (!ConfigJsonReader(contents.str()).Parse(&loaded, error_message)) {
    return ConfigLoadStatus::kError;
  }
  *config = std::move(loaded);
  return ConfigLoadStatus::kLoaded;
}

// 写入是"整份替换"：先拼出完整文本再原子替换，不存在"改了一半"的中间态。
// 失败语义：返回 false 时磁盘上的旧配置原封不动（临时文件由
// WriteFileAtomicallyReplacing 自己清理），调用方可以安全地重试。
bool ConfigManager::Save(const AppConfig& config,
                         std::string* error_message) const {
  if (error_message != nullptr) error_message->clear();
  if (config_file_path_.empty()) {
    SetError(error_message, "Cannot save config: config file path is empty");
    return false;
  }
  if (HasUnsupportedControlCharacter(config.backup_repository_path)) {
    SetError(error_message,
             "Cannot save config: repository path has an unsupported control "
             "character");
    return false;
  }

  // 配置文件也是持久化的业务配置，边界与 ScheduleStore 完全一致：
  //   * 目录 0700（新建时生效）；
  //   * 同目录唯一临时文件 + O_CREAT|O_EXCL（mkstemp）：不跟随符号链接、
  //     不截断别人预放的文件、同进程并行保存也不会撞名；
  //   * 0600、fsync、rename、fsync 目录。
  // 固定名字的 ".tmp" 会跟随符号链接并 truncate 目标——那正是这次要收掉的东西。
  if (!EnsurePrivateDirectoryFor(config_file_path_, error_message)) {
    return false;
  }

  std::ostringstream text;
  text << "{\n  \"version\": 1,\n  \"backup_repository_path\": \""
       << EscapeJsonString(config.backup_repository_path) << "\"\n}\n";
  return WriteFileAtomicallyReplacing(config_file_path_, text.str(),
                                      error_message);
}

// 只读访问器：返回的是成员引用，调用方不得跨线程长期持有（对象可能先析构）。
const std::string& ConfigManager::config_file_path() const {
  return config_file_path_;
}

}  // namespace backupproject
