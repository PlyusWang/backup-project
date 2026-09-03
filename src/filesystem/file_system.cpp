// file_system.cpp
//
// Implementation of the v0.1 file system helpers.
//
// Every file copy is done with plain read()/write() loops so that short
// reads, partial writes and EINTR interrupts are handled explicitly.

#include "file_system.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <string>

namespace backupproject {

namespace {

// Buffer size for one read() call during a file copy.  Small enough to
// stay cheap, large enough to keep the system call overhead low.
constexpr std::size_t kCopyBufferSize = 64 * 1024;

// RAII wrapper for a file descriptor: the descriptor is always closed
// when the wrapper goes out of scope, including on early returns.
class ScopedFileDescriptor {
 public:
  explicit ScopedFileDescriptor(int fd) : fd_(fd) {}

  ScopedFileDescriptor(const ScopedFileDescriptor&) = delete;
  ScopedFileDescriptor& operator=(const ScopedFileDescriptor&) = delete;

  ~ScopedFileDescriptor() {
    if (fd_ >= 0) {
      close(fd_);  // Best effort: nothing useful can be done on failure.
    }
  }

  int get() const { return fd_; }

  // Releases ownership and returns the descriptor so that an explicit
  // close() failure can still be reported by the caller.
  int release() {
    const int fd = fd_;
    fd_ = -1;
    return fd;
  }

 private:
  int fd_;
};

// RAII wrapper for a DIR* stream, mirroring ScopedFileDescriptor.
class ScopedDirectory {
 public:
  explicit ScopedDirectory(DIR* dir) : dir_(dir) {}

  ScopedDirectory(const ScopedDirectory&) = delete;
  ScopedDirectory& operator=(const ScopedDirectory&) = delete;

  ~ScopedDirectory() {
    if (dir_ != nullptr) {
      closedir(dir_);
    }
  }

  DIR* get() const { return dir_; }

 private:
  DIR* dir_;
};

// Formats "<reason>: <path>: <strerror>" so that every failure message
// names the failed operation, the offending path and the system error.
std::string Describe(int error, const std::string& reason,
                     const std::string& path) {
  std::string message = reason + ": " + path;
  if (error != 0) {
    message += ": ";
    message += std::strerror(error);
  }
  return message;
}

}  // namespace

std::string FileSystem::JoinPath(const std::string& parent,
                                 const std::string& child) {
  if (parent.empty() || parent.back() == '/') {
    return parent + child;
  }
  return parent + "/" + child;
}

FileSystem::PathStatus FileSystem::InspectPath(const std::string& path,
                                               std::string* error_message) {
  struct stat info;
  if (lstat(path.c_str(), &info) != 0) {
    // A missing path (including a missing intermediate component, which
    // surfaces as ENOTDIR) is a normal, expected situation for callers.
    if (errno == ENOENT || errno == ENOTDIR) {
      return PathStatus::kMissing;
    }
    if (error_message != nullptr) {
      *error_message = Describe(errno, "Failed to inspect path", path);
    }
    return PathStatus::kError;
  }
  if (S_ISDIR(info.st_mode)) {
    return PathStatus::kDirectory;
  }
  if (S_ISREG(info.st_mode)) {
    return PathStatus::kRegularFile;
  }
  return PathStatus::kOther;
}

bool FileSystem::MakeDirectories(const std::string& path,
                                 std::string* error_message) {
  if (path.empty()) {
    if (error_message != nullptr) {
      *error_message = "Failed to create destination directory: path is empty";
    }
    return false;
  }

  std::string current;
  if (path.front() == '/') {
    current = "/";
  }

  // Create every missing component of the path in order.
  std::size_t index = 0;
  while (index < path.size()) {
    while (index < path.size() && path[index] == '/') {
      ++index;
    }
    if (index == path.size()) {
      break;
    }
    const std::size_t component_begin = index;
    while (index < path.size() && path[index] != '/') {
      ++index;
    }
    const std::string component =
        path.substr(component_begin, index - component_begin);

    if (current.empty()) {
      current = component;
    } else if (current.back() == '/') {
      current += component;
    } else {
      current += '/';
      current += component;
    }

    // 0777 lets the process umask decide the final permission bits; v0.1
    // does not preserve metadata.  EEXIST is fine: the component exists
    // already and is verified again below.
    if (mkdir(current.c_str(), 0777) != 0 && errno != EEXIST) {
      if (error_message != nullptr) {
        *error_message =
            Describe(errno, "Failed to create destination directory", current);
      }
      return false;
    }
  }

  // Verify the final path is a directory; this catches a path whose
  // intermediate component exists as a regular file.
  struct stat info;
  if (lstat(path.c_str(), &info) != 0) {
    if (error_message != nullptr) {
      *error_message = Describe(errno, "Failed to inspect path", path);
    }
    return false;
  }
  if (!S_ISDIR(info.st_mode)) {
    if (error_message != nullptr) {
      *error_message = "Failed to create destination directory: " + path +
                       ": path component is not a directory";
    }
    return false;
  }
  return true;
}

bool FileSystem::IsMissingOrEmptyDirectory(const std::string& path,
                                           std::string* error_message) {
  const PathStatus status = InspectPath(path, error_message);
  if (status == PathStatus::kMissing) {
    return true;
  }
  if (status == PathStatus::kError) {
    return false;  // error_message already filled by InspectPath.
  }
  if (status != PathStatus::kDirectory) {
    if (error_message != nullptr) {
      *error_message = "Path exists and is not a directory: " + path;
    }
    return false;
  }

  DIR* dir = opendir(path.c_str());
  if (dir == nullptr) {
    if (error_message != nullptr) {
      *error_message = Describe(errno, "Failed to open directory", path);
    }
    return false;
  }
  ScopedDirectory scoped_dir(dir);

  // The directory is empty when it contains no entry besides "." and "..".
  for (;;) {
    errno = 0;
    struct dirent* entry = readdir(scoped_dir.get());
    if (entry == nullptr) {
      if (errno != 0) {
        if (error_message != nullptr) {
          *error_message = Describe(errno, "Failed to read directory", path);
        }
        return false;
      }
      return true;
    }
    const std::string name(entry->d_name);
    if (name == "." || name == "..") {
      continue;
    }
    return false;
  }
}

bool FileSystem::CopyRegularFile(const std::string& source,
                                 const std::string& destination,
                                 std::string* error_message) {
  ScopedFileDescriptor source_fd(open(source.c_str(), O_RDONLY));
  if (source_fd.get() < 0) {
    if (error_message != nullptr) {
      *error_message = Describe(errno, "Failed to open source file", source);
    }
    return false;
  }

  // 0666 lets the process umask decide the final permission bits; v0.1
  // does not preserve metadata.
  ScopedFileDescriptor destination_fd(
      open(destination.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666));
  if (destination_fd.get() < 0) {
    if (error_message != nullptr) {
      *error_message =
          Describe(errno, "Failed to open destination file", destination);
    }
    return false;
  }

  std::array<char, kCopyBufferSize> buffer;
  for (;;) {
    const ssize_t read_bytes =
        read(source_fd.get(), buffer.data(), buffer.size());
    if (read_bytes < 0) {
      if (errno == EINTR) {
        continue;  // Interrupted before anything was read: retry.
      }
      if (error_message != nullptr) {
        *error_message = Describe(errno, "Failed to read source file", source);
      }
      return false;
    }
    if (read_bytes == 0) {
      break;  // End of file.
    }

    // write() may accept fewer bytes than requested, so keep writing the
    // remaining part of the chunk until it is complete.
    ssize_t remaining = read_bytes;
    const char* cursor = buffer.data();
    while (remaining > 0) {
      const ssize_t written = write(destination_fd.get(), cursor,
                                    static_cast<std::size_t>(remaining));
      if (written < 0) {
        if (errno == EINTR) {
          continue;  // Interrupted before anything was written: retry.
        }
        if (error_message != nullptr) {
          *error_message =
              Describe(errno, "Failed to write destination file", destination);
        }
        return false;
      }
      if (written == 0) {
        // A zero-length write is treated as a failure to avoid an endless
        // loop on a full or broken file system.
        if (error_message != nullptr) {
          *error_message =
              Describe(EIO, "Failed to write destination file", destination);
        }
        return false;
      }
      cursor += written;
      remaining -= written;
    }
  }

  // Close the destination explicitly: on some file systems close() reports
  // late write errors.  The source descriptor needs no such check because
  // it was opened read-only.
  if (close(destination_fd.release()) < 0) {
    if (error_message != nullptr) {
      *error_message =
          Describe(errno, "Failed to close destination file", destination);
    }
    return false;
  }
  return true;
}

bool FileSystem::CopyTree(const std::string& source,
                          const std::string& destination,
                          std::string* error_message) {
  struct stat source_info;
  if (lstat(source.c_str(), &source_info) != 0) {
    if (error_message != nullptr) {
      *error_message = Describe(errno, "Failed to inspect source path", source);
    }
    return false;
  }

  if (S_ISDIR(source_info.st_mode)) {
    // Create the destination directory (and any missing parents).  An
    // existing directory is fine; an existing file of any kind is not.
    if (!MakeDirectories(destination, error_message)) {
      return false;
    }

    DIR* dir = opendir(source.c_str());
    if (dir == nullptr) {
      if (error_message != nullptr) {
        *error_message =
            Describe(errno, "Failed to open source directory", source);
      }
      return false;
    }
    ScopedDirectory scoped_dir(dir);

    for (;;) {
      errno = 0;
      struct dirent* entry = readdir(scoped_dir.get());
      if (entry == nullptr) {
        if (errno != 0) {
          if (error_message != nullptr) {
            *error_message =
                Describe(errno, "Failed to read source directory", source);
          }
          return false;
        }
        return true;
      }
      const std::string name(entry->d_name);
      if (name == "." || name == "..") {
        continue;
      }
      if (!CopyTree(JoinPath(source, name), JoinPath(destination, name),
                    error_message)) {
        return false;
      }
    }
  }

  if (S_ISREG(source_info.st_mode)) {
    return CopyRegularFile(source, destination, error_message);
  }

  // v0.1 deliberately supports only regular files and directories.
  if (error_message != nullptr) {
    *error_message = "Unsupported file type: " + source;
  }
  return false;
}

}  // namespace backupproject
