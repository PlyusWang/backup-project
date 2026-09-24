// archive_path.cpp
//
// 见 archive_path.h：这里是路径语法与拓扑校验的唯一实现。

#include "archive_path.h"

#include <cctype>

namespace backupproject {

namespace {

void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) {
    *error_message = text;
  }
}

}  // namespace

bool IsValidArchivePath(const std::string& path, bool is_first_entry,
                        bool is_directory, std::size_t max_path_length,
                        std::string* error_message) {
  if (path.empty()) {
    SetError(error_message, "Invalid archive path: empty path");
    return false;
  }
  if (path.size() > max_path_length) {
    SetError(error_message, "Archive path too long: " + path);
    return false;
  }
  if (path.find('\0') != std::string::npos) {
    SetError(error_message, "Invalid archive path: contains NUL byte");
    return false;
  }
  if (path.front() == '/') {
    SetError(error_message, "Invalid archive path (absolute): " + path);
    return false;
  }
  if (path.back() == '/') {
    SetError(error_message, "Invalid archive path (trailing slash): " + path);
    return false;
  }
  if (path.find('\\') != std::string::npos) {
    SetError(error_message, "Invalid archive path (backslash): " + path);
    return false;
  }
  // C:\... 这类 Windows 盘符路径不属于本项目的语义，直接拒绝。
  if (path.size() >= 2 && path[1] == ':' &&
      std::isalpha(static_cast<unsigned char>(path[0])) != 0) {
    SetError(error_message, "Invalid archive path (drive letter): " + path);
    return false;
  }
  if (path == ".") {
    // "." 代表 source root 本身，只允许作为第一条 entry，而且必须是目录。
    if (!is_first_entry || !is_directory) {
      SetError(error_message, "Invalid root entry in archive");
      return false;
    }
    return true;
  }

  // 逐段检查：空段（foo//bar）、"."（foo/./bar）、".."（../escape）都不允许。
  std::size_t start = 0;
  while (true) {
    const std::size_t slash = path.find('/', start);
    const std::string component = (slash == std::string::npos)
                                      ? path.substr(start)
                                      : path.substr(start, slash - start);
    if (component.empty() || component == "." || component == "..") {
      SetError(error_message, "Invalid archive path: " + path);
      return false;
    }
    if (slash == std::string::npos) {
      break;
    }
    start = slash + 1;
  }
  return true;
}

std::size_t ArchivePathDepth(const std::string& path) {
  if (path.empty() || path == ".") {
    return 0;
  }
  std::size_t depth = 1;
  for (const char character : path) {
    if (character == '/') {
      ++depth;
    }
  }
  return depth;
}

std::string ArchivePathRegistry::ParentOf(const std::string& path) {
  const std::size_t slash = path.rfind('/');
  if (slash == std::string::npos) {
    return ".";
  }
  return path.substr(0, slash);
}

bool ArchivePathRegistry::Add(const std::string& path, bool is_directory,
                              std::string* error_message) {
  if (all_.find(path) != all_.end()) {
    SetError(error_message, "Duplicate archive path: " + path);
    return false;
  }
  if (path != ".") {
    const std::string parent = ParentOf(path);
    const bool parent_listed = all_.find(parent) != all_.end();
    const bool parent_is_directory =
        directories_.find(parent) != directories_.end();
    if (require_parent_first_) {
      // 我们的写入器是 DFS 先序：父目录一定先出现过。
      if (!parent_is_directory) {
        SetError(error_message,
                 "Archive path has no parent directory entry: " + path);
        return false;
      }
    } else if (parent_listed && !parent_is_directory) {
      SetError(error_message,
               "Archive path uses a non-directory as parent: " + path);
      return false;
    }
  }
  all_.insert(path);
  if (is_directory) {
    directories_.insert(path);
  }
  return true;
}

bool ArchivePathRegistry::Finalize(std::string* error_message) const {
  if (require_parent_first_) {
    return true;
  }
  // 非严格模式下父目录可能后出现，这里再走一遍全量复核。
  for (const std::string& path : all_) {
    if (path == ".") {
      continue;
    }
    std::string parent = ParentOf(path);
    while (true) {
      const bool parent_listed = all_.find(parent) != all_.end();
      if (parent_listed && directories_.find(parent) == directories_.end()) {
        SetError(error_message,
                 "Archive path uses a non-directory as parent: " + path);
        return false;
      }
      if (parent == ".") {
        break;
      }
      parent = ParentOf(parent);
    }
  }
  return true;
}

std::string JoinArchivePath(const std::string& destination,
                            const std::string& archive_path) {
  if (archive_path.empty() || archive_path == ".") {
    return destination;
  }
  if (destination.empty() || destination == ".") {
    return archive_path;
  }
  if (destination.back() == '/') {
    return destination + archive_path;
  }
  return destination + "/" + archive_path;
}

}  // namespace backupproject
