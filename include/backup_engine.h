// backup_engine.h
//
// Sprint 1 的高层备份 / 恢复流程。
//
// Backup(source_directory, repository) 把源目录整棵树复制到
// <repository>/data 下；Restore(repository, destination) 做相反的事，
// 把 <repository>/data 里的树原样恢复到 destination。
//
// v0.1 的边界：
//   * 只支持普通目录和普通文件，其余类型明确失败；
//   * 不保存任何元数据（权限、属主、时间戳都不带）；
//   * 已有内容的 data 目录和恢复目标一律不覆盖。

#ifndef BACKUP_PROJECT_INCLUDE_BACKUP_ENGINE_H_
#define BACKUP_PROJECT_INCLUDE_BACKUP_ENGINE_H_

#include <string>

#include "file_system.h"

namespace backupproject {

// 只编排“先检查什么、再复制什么”的高层流程，真正的文件操作全部
// 交给 FileSystem。这样 CLI 和引擎都不用关心 open/read/write 的细节。
class BackupEngine {
 public:
  BackupEngine();

  // 把 source_directory 递归复制到 repository/data。
  // 源必须是已存在的目录；repository 可以不存在（会自动创建），
  // 但 repository/data 已有内容时直接拒绝，防止误覆盖旧备份。
  bool Backup(const std::string& source_directory,
              const std::string& repository, std::string* error_message);

  // 从 repository/data 恢复目录树到 destination。
  // repository 和它的 data 目录都必须已经存在；
  // destination 已存在且非空时拒绝执行，避免覆盖用户文件。
  bool Restore(const std::string& repository, const std::string& destination,
               std::string* error_message);

 private:
  FileSystem file_system_;
};

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_BACKUP_ENGINE_H_
