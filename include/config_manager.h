// config_manager.h
//
// Persistent application configuration.  This module only reads and writes
// the configuration file; it does not validate or create the repository path.

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
