// file_system.h
//
// Low-level file system operations used by the backup engine.
//
// v0.1 scope:
//   * regular files
//   * regular directories
//
// Every other file type (symbolic links, FIFOs, devices, sockets) is
// rejected with an explicit "unsupported" error message.  No metadata
// (mode, owner, timestamps) is preserved in this version.

#ifndef BACKUP_PROJECT_INCLUDE_FILE_SYSTEM_H_
#define BACKUP_PROJECT_INCLUDE_FILE_SYSTEM_H_

#include <string>

namespace backupproject {

// Thin wrapper around the POSIX file system primitives needed by the
// backup engine.  It is kept separate from BackupEngine so that the
// low-level copy logic can be reviewed and tested on its own and reused
// by later features.
class FileSystem {
 public:
  // Result of a lstat-based path inspection.
  enum class PathStatus {
    kMissing,      // Path does not exist (or a parent component is missing).
    kDirectory,    // Path exists and is a directory.
    kRegularFile,  // Path exists and is a regular file.
    kOther,        // Path exists but has an unsupported type.
    kError,        // Inspection failed; see error_message.
  };

  FileSystem() = default;

  // Joins two path fragments with exactly one '/' separator.
  static std::string JoinPath(const std::string& parent,
                              const std::string& child);

  // Inspects |path| with lstat and reports its status.  On kError,
  // |error_message| (when non-null) explains the failure.
  PathStatus InspectPath(const std::string& path, std::string* error_message);

  // Creates |path| together with every missing parent directory.  Fails
  // when any component of the path exists as a non-directory.
  bool MakeDirectories(const std::string& path, std::string* error_message);

  // Returns true when |path| is missing or an existing empty directory.
  // Returns false otherwise; when |error_message| is non-null it explains
  // failures, while an existing non-empty directory leaves it empty.
  bool IsMissingOrEmptyDirectory(const std::string& path,
                                 std::string* error_message);

  // Recursively copies the directory tree rooted at |source| so that the
  // same tree appears under |destination|.
  //
  // Regular files are copied byte for byte.  Regular directories are
  // created (together with missing parents) and recursed into.  Any other
  // file type fails the whole operation with an "unsupported" message.
  //
  // Returns true on success.  On failure returns false and, when
  // |error_message| is non-null, fills it with a human-readable reason
  // that includes the offending path and the system error, if any.
  bool CopyTree(const std::string& source, const std::string& destination,
                std::string* error_message);

 private:
  // Copies one regular file, handling short reads and partial writes.
  bool CopyRegularFile(const std::string& source,
                       const std::string& destination,
                       std::string* error_message);
};

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_FILE_SYSTEM_H_
