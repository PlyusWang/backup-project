// app_paths.cpp

#include "app_paths.h"

#include <cstdlib>

namespace backupproject {
namespace {

void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) *error_message = text;
}

// XDG 规范：XDG_CONFIG_HOME 必须是绝对路径才被采用；相对路径一律忽略，
// 回退到 $HOME/.config。Qt 的 QStandardPaths 用的就是这条规则。
bool IsAbsolutePath(const std::string& path) {
  return !path.empty() && path[0] == '/';
}

const char* NonEmptyEnvironment(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') return nullptr;
  return value;
}

// 去掉尾部多余的 '/'，让 "/a/b/" 与 "/a/b" 得到同一个结果。
// 例外：全是 '/' 时保留一个，避免把 "/" 变成空串。
std::string StripTrailingSlashes(const std::string& path) {
  std::size_t end = path.size();
  while (end > 1 && path[end - 1] == '/') --end;
  return path.substr(0, end);
}

}  // namespace

std::string GenericConfigDirectory() {
  const char* xdg_config_home = NonEmptyEnvironment("XDG_CONFIG_HOME");
  if (xdg_config_home != nullptr &&
      IsAbsolutePath(std::string(xdg_config_home))) {
    return StripTrailingSlashes(xdg_config_home);
  }
  const char* home = NonEmptyEnvironment("HOME");
  if (home == nullptr || !IsAbsolutePath(std::string(home)))
    return std::string();
  return StripTrailingSlashes(home) + "/.config";
}

bool AppConfigDirectory(std::string* directory, std::string* error_message) {
  if (directory == nullptr) {
    SetError(error_message, "App config directory output must not be null");
    return false;
  }
  directory->clear();
  const std::string base = GenericConfigDirectory();
  if (base.empty()) {
    SetError(error_message,
             "Cannot resolve the application config directory: neither "
             "XDG_CONFIG_HOME nor HOME is an absolute path");
    return false;
  }
  *directory = base + "/" + kAppOrganizationName + "/" + kAppApplicationName;
  return true;
}

bool AppConfigFilePath(const std::string& file_name, std::string* path,
                       std::string* error_message) {
  if (path == nullptr) {
    SetError(error_message, "App config file path output must not be null");
    return false;
  }
  path->clear();
  if (file_name.empty() || file_name.find('/') != std::string::npos) {
    SetError(
        error_message,
        "App config file name must be a single path component: " + file_name);
    return false;
  }
  std::string directory;
  if (!AppConfigDirectory(&directory, error_message)) return false;
  *path = directory + "/" + file_name;
  return true;
}

std::string DefaultConfigFilePath() {
  std::string path;
  std::string error;
  if (!AppConfigFilePath(kAppConfigFileName, &path, &error))
    return std::string();
  return path;
}

std::string DefaultRealtimeFilePath() {
  std::string path;
  std::string error;
  if (!AppConfigFilePath(kAppRealtimeFileName, &path, &error)) {
    return std::string();
  }
  return path;
}

std::string DefaultScheduleFilePath() {
  std::string path;
  std::string error;
  if (!AppConfigFilePath(kAppScheduleFileName, &path, &error)) {
    return std::string();
  }
  return path;
}

}  // namespace backupproject
