// app_paths.cpp

// 职责：把 XDG 规则翻译成本产品的配置目录与配置文件名；不创建目录、
// 不检查可写性、不读配置文件内容 —— 那些是调用方与 ConfigManager 的事。
// 数据流：环境变量 -> GenericConfigDirectory -> AppConfigDirectory
// （<config>/<组织>/<应用>）-> AppConfigFilePath -> Default*FilePath。
// 失败契约：不抛异常；Default*FilePath 没有 error 出口，用空串表示失败，
// 调用方必须把空串当失败处理。无缓存，每次调用重新读环境变量：测试里
// setenv 之后同进程内立刻生效。
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

// 空串与“未设置”同等对待：XDG_CONFIG_HOME= 这种“清空而不是删除”的写法
// 在 shell 与 CI 里很常见，它不能被当成一个有效目录。
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

// 只算目录名，不创建。HOME 不是绝对路径时返回空串，而不是退化成
// “/.config”：宁可让上层明确失败，也不把配置写到根目录去。
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

// 路径边界：file_name 必须是单一分量，空串或含 '/' 一律拒绝。当前所有
// 调用点传的都是编译期常量，外部输入到不了这里；将来若有动态名字传入，
// 必须在这里补上对 “..” 之类的拦截。
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

// 三个 Default* 入口只差文件名：目录布局改动时只需要改上面的函数，
// 文件名各自保持稳定（老用户的配置不会因为改名而“消失”）。
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
