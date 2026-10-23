// ui/modern/copy_staging.cpp
#include "copy_staging.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace backup_modern {
namespace {

constexpr const char* kLeaseName = ".lease";
constexpr const char* kLeaseMagic = "BPSCOPY1 ";
constexpr const char* kUpPrefix = ".copy-up-";
constexpr const char* kDownPrefix = ".copy-down-";

bool HexToken(std::string* token) {
  unsigned char bytes[16];
  std::size_t done = 0;
  while (done < sizeof(bytes)) {
    const ssize_t got = ::getrandom(bytes + done, sizeof(bytes) - done, 0);
    if (got < 0 && errno == EINTR) continue;
    if (got <= 0) {
      // 退化到 /dev/urandom；仍然失败就不创建租约。
      const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
      if (fd < 0) return false;
      const ssize_t fallback = ::read(fd, bytes + done, sizeof(bytes) - done);
      ::close(fd);
      if (fallback <= 0) return false;
      done += static_cast<std::size_t>(fallback);
      continue;
    }
    done += static_cast<std::size_t>(got);
  }
  static const char* kHex = "0123456789abcdef";
  token->clear();
  for (unsigned char byte : bytes) {
    token->push_back(kHex[byte >> 4]);
    token->push_back(kHex[byte & 0x0f]);
  }
  return true;
}

bool ValidLeaseHeader(const std::string& header) {
  const std::string prefix = kLeaseMagic;
  if (header.size() != prefix.size() + 32 + 1) return false;
  if (header.compare(0, prefix.size(), prefix) != 0) return false;
  if (header.back() != '\n') return false;
  for (std::size_t i = prefix.size(); i < header.size() - 1; ++i) {
    const char c = header[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

bool RemoveTreeAt(int parent_fd) {
  const int scan_fd = ::dup(parent_fd);
  if (scan_fd < 0) return false;
  DIR* directory = ::fdopendir(scan_fd);
  if (directory == nullptr) {
    ::close(scan_fd);
    return false;
  }
  bool ok = true;
  while (const dirent* entry = ::readdir(directory)) {
    const char* name = entry->d_name;
    if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0) continue;
    struct stat info;
    if (::fstatat(parent_fd, name, &info, AT_SYMLINK_NOFOLLOW) != 0) {
      ok = false;
      continue;
    }
    if (S_ISDIR(info.st_mode)) {
      const int child = ::openat(
          parent_fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (child < 0) {
        ok = false;
        continue;
      }
      if (!RemoveTreeAt(child)) ok = false;
      ::close(child);
      if (::unlinkat(parent_fd, name, AT_REMOVEDIR) != 0) ok = false;
    } else {
      if (::unlinkat(parent_fd, name, 0) != 0) ok = false;
    }
  }
  ::closedir(directory);
  return ok;
}

bool IsCandidateName(const char* name) {
  return std::strncmp(name, kUpPrefix, std::strlen(kUpPrefix)) == 0 ||
         std::strncmp(name, kDownPrefix, std::strlen(kDownPrefix)) == 0;
}

}  // namespace

CopyStagingLease::~CopyStagingLease() {
  if (!path_.empty() && dir_fd_ >= 0) {
    RemoveTreeAt(dir_fd_);
    struct stat opened;
    struct stat linked;
    if (::fstat(dir_fd_, &opened) == 0 &&
        ::lstat(path_.c_str(), &linked) == 0 &&
        opened.st_dev == linked.st_dev && opened.st_ino == linked.st_ino) {
      // path_.c_str() 是绝对路径；这里的目标只是本对象创建的目录，删除失败
      // 也不影响正式备份，下一次清理会再处理。
      char parent[4096];
      std::snprintf(parent, sizeof(parent), "%s", path_.c_str());
      char* slash = std::strrchr(parent, '/');
      if (slash != nullptr) {
        *slash = '\0';
        const char* name = path_.c_str() + (slash - parent) + 1;
        const int parent_fd =
            ::open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (parent_fd >= 0) {
          ::unlinkat(parent_fd, name, AT_REMOVEDIR);
          ::close(parent_fd);
        }
      }
    }
  }
  if (lease_fd_ >= 0) ::close(lease_fd_);
  if (dir_fd_ >= 0) ::close(dir_fd_);
}

bool CopyStagingLease::Create(const std::string& repository,
                              CopyStagingKind kind,
                              std::string* error_message) {
  path_.clear();
  if (dir_fd_ >= 0) ::close(dir_fd_);
  if (lease_fd_ >= 0) ::close(lease_fd_);
  dir_fd_ = -1;
  lease_fd_ = -1;

  const std::string prefix =
      kind == CopyStagingKind::kUpload ? kUpPrefix : kDownPrefix;
  std::string pattern = repository + "/" + prefix + "XXXXXX";
  std::vector<char> buffer(pattern.begin(), pattern.end());
  buffer.push_back('\0');
  if (::mkdtemp(buffer.data()) == nullptr) {
    if (error_message != nullptr) *error_message = "无法创建复制暂存目录";
    return false;
  }
  path_.assign(buffer.data());
  if (::chmod(path_.c_str(), 0700) != 0) {
    if (error_message != nullptr) *error_message = "无法设置暂存目录权限";
    ::rmdir(path_.c_str());
    path_.clear();
    return false;
  }
  dir_fd_ =
      ::open(path_.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (dir_fd_ < 0) {
    if (error_message != nullptr) *error_message = "无法打开暂存目录";
    ::rmdir(path_.c_str());
    path_.clear();
    return false;
  }
  lease_fd_ =
      ::openat(dir_fd_, kLeaseName,
               O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (lease_fd_ < 0) {
    if (error_message != nullptr) *error_message = "无法创建暂存租约";
    ::close(dir_fd_);
    dir_fd_ = -1;
    RemoveTreeAt(::open(path_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    path_.clear();
    return false;
  }
  // 先持锁再写头：清理方看到合法头时，锁已经在这个 fd 上。
  if (::flock(lease_fd_, LOCK_EX | LOCK_NB) != 0) {
    if (error_message != nullptr) *error_message = "无法锁定暂存租约";
    ::close(lease_fd_);
    ::close(dir_fd_);
    lease_fd_ = -1;
    dir_fd_ = -1;
    path_.clear();
    return false;
  }
  std::string token;
  std::string header = kLeaseMagic;
  if (!HexToken(&token)) {
    if (error_message != nullptr) *error_message = "无法生成暂存任务令牌";
    return false;
  }
  header += token;
  header += '\n';
  if (::write(lease_fd_, header.data(), header.size()) !=
      static_cast<ssize_t>(header.size())) {
    if (error_message != nullptr) *error_message = "无法写入暂存租约";
    return false;
  }
  if (::fsync(lease_fd_) != 0 || ::fsync(dir_fd_) != 0) {
    if (error_message != nullptr) *error_message = "无法持久化暂存租约";
    return false;
  }
  return true;
}

CopyStagingCleanupReport CleanupStaleCopyStaging(
    const std::string& repository) {
  CopyStagingCleanupReport report;
  const int repo_fd = ::open(repository.c_str(),
                             O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (repo_fd < 0) {
    ++report.failed;
    return report;
  }
  const int scan_fd = ::dup(repo_fd);
  DIR* directory = scan_fd >= 0 ? ::fdopendir(scan_fd) : nullptr;
  if (directory == nullptr) {
    if (scan_fd >= 0) ::close(scan_fd);
    ::close(repo_fd);
    ++report.failed;
    return report;
  }
  std::vector<std::string> candidates;
  while (const dirent* entry = ::readdir(directory)) {
    if (IsCandidateName(entry->d_name)) candidates.push_back(entry->d_name);
  }
  ::closedir(directory);

  for (const std::string& name : candidates) {
    struct stat path_info;
    if (::fstatat(repo_fd, name.c_str(), &path_info, AT_SYMLINK_NOFOLLOW) !=
            0 ||
        !S_ISDIR(path_info.st_mode)) {
      ++report.skipped_invalid;
      continue;
    }
    const int dir_fd = ::openat(
        repo_fd, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir_fd < 0) {
      ++report.skipped_invalid;
      continue;
    }
    struct stat dir_info;
    if (::fstat(dir_fd, &dir_info) != 0 || dir_info.st_uid != ::geteuid() ||
        (dir_info.st_mode & 0777) != 0700) {
      ::close(dir_fd);
      ++report.skipped_invalid;
      continue;
    }
    const int lease_fd =
        ::openat(dir_fd, kLeaseName, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
    if (lease_fd < 0) {
      ::close(dir_fd);
      ++report.skipped_invalid;
      continue;
    }
    struct stat lease_info;
    if (::fstat(lease_fd, &lease_info) != 0 || !S_ISREG(lease_info.st_mode) ||
        lease_info.st_uid != ::geteuid()) {
      ::close(lease_fd);
      ::close(dir_fd);
      ++report.skipped_invalid;
      continue;
    }
    if (::flock(lease_fd, LOCK_EX | LOCK_NB) != 0) {
      ::close(lease_fd);
      ::close(dir_fd);
      ++report.skipped_live;
      continue;
    }
    char raw[64] = {0};
    const ssize_t got = ::pread(lease_fd, raw, sizeof(raw) - 1, 0);
    const std::string header(raw, got > 0 ? static_cast<std::size_t>(got) : 0);
    if (!ValidLeaseHeader(header)) {
      ::close(lease_fd);
      ::close(dir_fd);
      ++report.skipped_invalid;
      continue;
    }
    // 删除期间一直持有 lease_fd 的锁和 dir_fd。先清内容，再用 (dev, ino)
    // 确认父目录里这个名字仍指向本对象，避免删除被替换后的目录。
    if (!RemoveTreeAt(dir_fd)) {
      ::close(lease_fd);
      ::close(dir_fd);
      ++report.failed;
      continue;
    }
    struct stat linked;
    if (::fstatat(repo_fd, name.c_str(), &linked, AT_SYMLINK_NOFOLLOW) == 0 &&
        linked.st_dev == dir_info.st_dev && linked.st_ino == dir_info.st_ino &&
        ::unlinkat(repo_fd, name.c_str(), AT_REMOVEDIR) == 0) {
      ++report.removed;
    } else {
      ++report.failed;
    }
    ::close(lease_fd);
    ::close(dir_fd);
  }
  ::close(repo_fd);
  return report;
}

}  // namespace backup_modern
