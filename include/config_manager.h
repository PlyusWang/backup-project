// config_manager.h
//
// Persistent application configuration.  This module only reads and writes
// the configuration file; it does not validate or create the repository path.
// 因此 Load 成功只代表“文件被读出来了”，不代表配置里的路径可用：调用方必须
// 自己 InspectPath 一次。这条边界是刻意的 —— “合法仓库路径”只有一处定义，
// 放在这里就会出现第二份、迟早分叉。

#ifndef BACKUP_PROJECT_INCLUDE_CONFIG_MANAGER_H_
#define BACKUP_PROJECT_INCLUDE_CONFIG_MANAGER_H_

#include <string>

namespace backupproject {

struct AppConfig {
  std::string backup_repository_path;
};

enum class ConfigLoadStatus {
  kLoaded,
  kMissing,
  kError,
};

// 构造函数接收显式路径（不自己去查 XDG），以便测试注入临时文件。
class ConfigManager {
 public:
  explicit ConfigManager(std::string config_file_path);

  ConfigLoadStatus Load(AppConfig* config, std::string* error_message) const;

  bool Save(const AppConfig& config, std::string* error_message) const;

  const std::string& config_file_path() const;

 private:
  std::string config_file_path_;
};

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_CONFIG_MANAGER_H_
