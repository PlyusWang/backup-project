// backup_engine.h
//
// High-level backup / restore workflow (v0.1).
//
// Backup(source_directory, repository) stores a byte-for-byte copy of the
// source directory tree under <repository>/data.
//
// Restore(repository, destination) rebuilds the tree stored under
// <repository>/data inside <destination>.
//
// v0.1 rules:
//   * only regular files and regular directories are supported;
//   * no metadata is preserved;
//   * an existing non-empty backup data directory or restore destination
//     is never overwritten.

#ifndef BACKUP_PROJECT_INCLUDE_BACKUP_ENGINE_H_
#define BACKUP_PROJECT_INCLUDE_BACKUP_ENGINE_H_

#include <string>

#include "file_system.h"

namespace backupproject {

// Orchestrates the high-level backup and restore workflows.  All real
// file system work is delegated to FileSystem.
class BackupEngine {
 public:
  BackupEngine();

  // Backs up |source_directory| into |repository| (creating
  // <repository>/data).  Returns true on success; on failure fills
  // |error_message| (when non-null) with the reason.
  bool Backup(const std::string& source_directory,
              const std::string& repository, std::string* error_message);

  // Restores the tree stored under <repository>/data into |destination|.
  // Returns true on success; on failure fills |error_message| (when
  // non-null) with the reason.
  bool Restore(const std::string& repository, const std::string& destination,
               std::string* error_message);

 private:
  FileSystem file_system_;
};

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_BACKUP_ENGINE_H_
