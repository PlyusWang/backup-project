// config_manager.cpp
//
// The reader below is deliberately not a general JSON parser.  It accepts only
// this module's version-1 schema: an object with `version` and
// `backup_repository_path` fields.

#include "config_manager.h"

#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

namespace backupproject {
namespace {

namespace fs = std::filesystem;

void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) *error_message = text;
}

std::string Describe(const std::string& action, const fs::path& path,
                     const std::error_code& error) {
  return action + ": " + path.string() + ": " + error.message();
}

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

class ConfigJsonReader {
 public:
  explicit ConfigJsonReader(const std::string& input) : input_(input) {}

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

  const std::string& input_;
  std::size_t position_ = 0;
};

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

ConfigManager::ConfigManager(std::string config_file_path)
    : config_file_path_(std::move(config_file_path)) {}

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

  const fs::path config_file(config_file_path_);
  const fs::path parent = config_file.parent_path();
  std::error_code error;
  if (!parent.empty()) fs::create_directories(parent, error);
  if (error) {
    SetError(error_message,
             Describe("Failed to create config directory", parent, error));
    return false;
  }

  const fs::path temporary = config_file.string() + ".tmp";
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
      SetError(error_message,
               "Failed to open temporary config file: " + temporary.string());
      return false;
    }
    output << "{\n  \"version\": 1,\n  \"backup_repository_path\": \""
           << EscapeJsonString(config.backup_repository_path) << "\"\n}\n";
    output.close();
    if (!output) {
      std::error_code cleanup_error;
      fs::remove(temporary, cleanup_error);
      SetError(error_message,
               "Failed to write temporary config file: " + temporary.string());
      return false;
    }
  }

  fs::rename(temporary, config_file, error);
  if (error) {
    const std::error_code rename_error = error;
    std::error_code cleanup_error;
    fs::remove(temporary, cleanup_error);
    SetError(error_message, Describe("Failed to replace config file",
                                     config_file, rename_error));
    return false;
  }
  return true;
}

const std::string& ConfigManager::config_file_path() const {
  return config_file_path_;
}

}  // namespace backupproject
