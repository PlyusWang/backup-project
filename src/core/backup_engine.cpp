// backup_engine.cpp
//
// v0.1 workflow: backup copies a source directory tree into
// <repository>/data; restore copies <repository>/data back out to a
// destination directory.  All real file system work is delegated to
// FileSystem.

#include "backup_engine.h"

#include <string>

namespace backupproject {

namespace {

// Name of the directory that holds the actual backup payload inside a
// repository.  Later versions will add metadata next to it.
constexpr const char* kDataDirectoryName = "data";

// Replaces |error_message| (when non-null) with |text|.
void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) {
    *error_message = text;
  }
}

}  // namespace

BackupEngine::BackupEngine() = default;

bool BackupEngine::Backup(const std::string& source_directory,
                          const std::string& repository,
                          std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }

  // The source must be an existing directory.
  const FileSystem::PathStatus source_status =
      file_system_.InspectPath(source_directory, error_message);
  if (source_status == FileSystem::PathStatus::kError) {
    return false;
  }
  if (source_status == FileSystem::PathStatus::kMissing) {
    SetError(error_message,
             "Source directory does not exist: " + source_directory);
    return false;
  }
  if (source_status != FileSystem::PathStatus::kDirectory) {
    SetError(error_message, "Source is not a directory: " + source_directory);
    return false;
  }

  // The repository may be created by the backup itself, but nothing inside
  // it may be overwritten: refuse when repository/data already exists and
  // is not empty.  (An existing empty data directory is reused; it cannot
  // hold anything a user could lose.)
  if (!file_system_.MakeDirectories(repository, error_message)) {
    return false;
  }

  const std::string data_directory =
      FileSystem::JoinPath(repository, kDataDirectoryName);
  if (!file_system_.IsMissingOrEmptyDirectory(data_directory, error_message)) {
    if (error_message == nullptr || error_message->empty()) {
      SetError(error_message,
               "Repository data directory already exists and is not empty: " +
                   data_directory);
    }
    return false;
  }

  return file_system_.CopyTree(source_directory, data_directory, error_message);
}

bool BackupEngine::Restore(const std::string& repository,
                           const std::string& destination,
                           std::string* error_message) {
  if (error_message != nullptr) {
    error_message->clear();
  }

  // The repository and its data directory must already exist.
  const FileSystem::PathStatus repository_status =
      file_system_.InspectPath(repository, error_message);
  if (repository_status == FileSystem::PathStatus::kError) {
    return false;
  }
  if (repository_status == FileSystem::PathStatus::kMissing) {
    SetError(error_message, "Repository does not exist: " + repository);
    return false;
  }
  if (repository_status != FileSystem::PathStatus::kDirectory) {
    SetError(error_message, "Repository is not a directory: " + repository);
    return false;
  }

  const std::string data_directory =
      FileSystem::JoinPath(repository, kDataDirectoryName);
  const FileSystem::PathStatus data_status =
      file_system_.InspectPath(data_directory, error_message);
  if (data_status == FileSystem::PathStatus::kError) {
    return false;
  }
  if (data_status == FileSystem::PathStatus::kMissing) {
    SetError(error_message,
             "Repository data directory does not exist: " + data_directory);
    return false;
  }
  if (data_status != FileSystem::PathStatus::kDirectory) {
    SetError(error_message,
             "Repository data directory is not a directory: " + data_directory);
    return false;
  }

  // Never overwrite an existing non-empty destination.
  if (!file_system_.IsMissingOrEmptyDirectory(destination, error_message)) {
    if (error_message == nullptr || error_message->empty()) {
      SetError(error_message,
               "Destination directory already exists and is not empty: " +
                   destination);
    }
    return false;
  }

  return file_system_.CopyTree(data_directory, destination, error_message);
}

}  // namespace backupproject
