// file_io.cpp
//
// 见 file_io.h。

#include "file_io.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

namespace backupproject {

namespace {

void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) {
    *error_message = text;
  }
}

std::string Describe(int error_number, const std::string& action,
                     const std::string& path) {
  return action + ": " + path + ": " + std::strerror(error_number);
}

std::string ParentDirectoryOf(const std::string& path) {
  const std::size_t slash = path.rfind('/');
  if (slash == std::string::npos) {
    return std::string(".");
  }
  if (slash == 0) {
    return std::string("/");
  }
  return path.substr(0, slash);
}

// 目录项落盘不靠 fsync 文件本身，而靠 fsync 父目录。失败只当诊断：
// 内容已经写完了，因为"目录项可能还没落盘"去报失败反而会误导调用方。
void SyncDirectoryQuietly(const std::string& directory) {
  const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    return;
  }
  (void)::fsync(fd);
  ::close(fd);
}

// 缓冲大小取 256 KiB：足以把 512 字节 header、对齐填充和小文件正文聚合成
// 大块写，同时又远小于内存关心量级。
constexpr std::size_t kSinkBufferSize = 256 * 1024;

// 区间复制的读缓冲：64 KiB～1 MiB 之间的保守取值。
constexpr std::size_t kCopyBufferSize = 256 * 1024;

int DefaultFsync(int fd) { return ::fsync(fd); }
int DefaultClose(int fd) { return ::close(fd); }

int DefaultLink(const char* existing_path, const char* new_path) {
  return ::link(existing_path, new_path);
}

// RENAME_NOREPLACE：内核保证"目标已存在"时返回 EEXIST，不做任何覆盖。
int DefaultRenameNoReplace(const char* old_path, const char* new_path) {
  return ::renameat2(AT_FDCWD, old_path, AT_FDCWD, new_path, RENAME_NOREPLACE);
}

}  // namespace

namespace file_io_syscalls {

FsyncFn& FsyncHook() {
  static FsyncFn hook = &DefaultFsync;
  return hook;
}

CloseFn& CloseHook() {
  static CloseFn hook = &DefaultClose;
  return hook;
}

LinkFn& LinkHook() {
  static LinkFn hook = &DefaultLink;
  return hook;
}

RenameNoReplaceFn& RenameNoReplaceHook() {
  static RenameNoReplaceFn hook = &DefaultRenameNoReplace;
  return hook;
}

}  // namespace file_io_syscalls

// ---- FileSink --------------------------------------------------------------

FileSink::~FileSink() {
  // 析构也是 fail-safe 清理点：只要路径还归本对象所有而且没提交，就删掉。
  if (owns_path_ && !committed_) {
    Abandon();
    return;
  }
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

bool FileSink::Open(const std::string& path, std::string* error_message) {
  if (fd_ >= 0 || owns_path_) {
    SetError(error_message, "Internal error: sink already open: " + path);
    return false;
  }
  if (path.empty()) {
    SetError(error_message, "Output file path is empty.");
    return false;
  }
  // 0600：备份文件可能包含任何东西，没有理由让同机器上的其他用户读到它。
  // legacy v0.1 一直就是 0600，新路径不能更宽松。
  const int raw_fd =
      ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (raw_fd < 0) {
    SetError(error_message, Describe(errno, "Failed to create file", path));
    return false;
  }
  fd_ = raw_fd;
  path_ = path;
  bytes_written_ = 0;
  buffered_ = 0;
  buffer_.assign(kSinkBufferSize, 0);
  owns_path_ = true;
  committed_ = false;
  return true;
}

bool FileSink::OpenTemp(const std::string& directory, const std::string& prefix,
                        std::string* error_message) {
  if (fd_ >= 0 || owns_path_) {
    SetError(error_message, "Internal error: sink already open");
    return false;
  }
  std::string base = directory.empty() ? std::string(".") : directory;
  if (base.back() == '/') {
    base.pop_back();
  }
  std::string pattern = base + "/" + prefix + "XXXXXX";
  std::vector<char> buffer(pattern.begin(), pattern.end());
  buffer.push_back('\0');
  const int raw_fd = ::mkstemp(buffer.data());
  if (raw_fd < 0) {
    SetError(error_message,
             Describe(errno, "Failed to create temporary file in", base));
    return false;
  }
  fd_ = raw_fd;
  path_.assign(buffer.data());
  bytes_written_ = 0;
  buffered_ = 0;
  buffer_.assign(kSinkBufferSize, 0);
  owns_path_ = true;
  committed_ = false;
  return true;
}

bool FileSink::WriteRaw(const void* data, std::size_t size,
                        std::string* error_message) {
  if (size == 0) {
    return true;
  }
  if (!WriteFully(fd_, data, size, error_message)) {
    if (error_message != nullptr) {
      *error_message = *error_message + ": " + path_;
    }
    return false;
  }
  bytes_written_ += static_cast<std::uint64_t>(size);
  return true;
}

bool FileSink::Write(const void* data, std::size_t size,
                     std::string* error_message) {
  if (fd_ < 0) {
    SetError(error_message, "Internal error: sink is not open");
    return false;
  }
  const unsigned char* bytes = static_cast<const unsigned char*>(data);
  std::size_t remaining = size;
  while (remaining > 0) {
    // 单块比缓冲还大时直接写出去，不先拆成缓冲碎片。
    if (buffered_ == 0 && remaining >= buffer_.size()) {
      if (!WriteRaw(bytes, remaining, error_message)) {
        return false;
      }
      return true;
    }
    const std::size_t room = buffer_.size() - buffered_;
    const std::size_t take = (remaining < room) ? remaining : room;
    std::memcpy(buffer_.data() + buffered_, bytes, take);
    buffered_ += take;
    bytes += take;
    remaining -= take;
    if (buffered_ == buffer_.size()) {
      if (!WriteRaw(buffer_.data(), buffered_, error_message)) {
        return false;
      }
      buffered_ = 0;
    }
  }
  return true;
}

bool FileSink::Flush(std::string* error_message) {
  if (buffered_ > 0) {
    if (!WriteRaw(buffer_.data(), buffered_, error_message)) {
      return false;
    }
    buffered_ = 0;
  }
  return true;
}

bool FileSink::Patch(std::uint64_t offset, const void* data, std::size_t size,
                     std::string* error_message) {
  if (fd_ < 0) {
    SetError(error_message, "Internal error: sink is not open");
    return false;
  }
  if (!Flush(error_message)) {
    return false;
  }
  std::size_t done = 0;
  const unsigned char* bytes = static_cast<const unsigned char*>(data);
  while (done < size) {
    const ssize_t written = ::pwrite(fd_, bytes + done, size - done,
                                     static_cast<off_t>(offset + done));
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      SetError(error_message,
               Describe(errno, "Failed to patch file header", path_));
      return false;
    }
    done += static_cast<std::size_t>(written);
  }
  return true;
}

bool FileSink::Close(std::string* error_message) {
  if (fd_ < 0) {
    if (committed_) {
      return true;
    }
    SetError(error_message, "Internal error: sink is not open: " + path_);
    return false;
  }
  bool ok = Flush(error_message);
  if (ok && file_io_syscalls::FsyncHook()(fd_) != 0) {
    SetError(error_message, Describe(errno, "Failed to sync file", path_));
    ok = false;
  }
  const int fd = fd_;
  fd_ = -1;
  buffered_ = 0;
  if (file_io_syscalls::CloseHook()(fd) != 0 && ok) {
    SetError(error_message, Describe(errno, "Failed to close file", path_));
    ok = false;
  }
  // 关键：失败时**不**清 owns_path_。此刻 fd 已经关了，但路径还归本对象所有，
  // 所以 Abandon()（以及析构）仍然必须把它 unlink 掉。
  if (ok) {
    committed_ = true;
  }
  return ok;
}

void FileSink::Abandon() {
  if (fd_ >= 0) {
    // 清理路径用真正的 close，不走注入点：测试把 close 换成"总是失败"之后，
    // 句柄还是应该被真正关掉，否则测试进程会漏 fd。
    ::close(fd_);
    fd_ = -1;
  }
  buffered_ = 0;
  if (owns_path_ && !committed_ && !path_.empty()) {
    ::unlink(path_.c_str());
  }
  owns_path_ = false;
}

// ---- FileSource ------------------------------------------------------------

FileSource::~FileSource() { Close(); }

bool FileSource::Open(const std::string& path, std::string* error_message) {
  Close();
  const int raw_fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (raw_fd < 0) {
    SetError(error_message, Describe(errno, "Failed to open file", path));
    return false;
  }
  struct stat info;
  if (::fstat(raw_fd, &info) != 0) {
    SetError(error_message, Describe(errno, "Failed to inspect file", path));
    ::close(raw_fd);
    return false;
  }
  if (!S_ISREG(info.st_mode)) {
    SetError(error_message, "Not a regular file: " + path);
    ::close(raw_fd);
    return false;
  }
  fd_ = raw_fd;
  path_ = path;
  size_ = static_cast<std::uint64_t>(info.st_size);
  return true;
}

void FileSource::Close() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  size_ = 0;
}

bool FileSource::ReadAt(std::uint64_t offset, void* buffer, std::size_t size,
                        std::string* error_message) const {
  if (fd_ < 0) {
    SetError(error_message, "Internal error: source is not open");
    return false;
  }
  std::size_t done = 0;
  unsigned char* bytes = static_cast<unsigned char*>(buffer);
  while (done < size) {
    const ssize_t got = ::pread(fd_, bytes + done, size - done,
                                static_cast<off_t>(offset + done));
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      SetError(error_message, Describe(errno, "Failed to read file", path_));
      return false;
    }
    if (got == 0) {
      SetError(error_message, "Unexpected end of file: " + path_);
      return false;
    }
    done += static_cast<std::size_t>(got);
  }
  return true;
}

bool FileSource::CopyRangeTo(std::uint64_t offset, std::uint64_t size,
                             FileSink* sink, std::string* error_message) const {
  if (fd_ < 0 || sink == nullptr) {
    SetError(error_message, "Internal error: copy with closed handle");
    return false;
  }
  std::vector<unsigned char> buffer(kCopyBufferSize);
  std::uint64_t copied = 0;
  while (copied < size) {
    const std::uint64_t remaining = size - copied;
    const std::size_t want = static_cast<std::size_t>(
        remaining < kCopyBufferSize ? remaining : kCopyBufferSize);
    const ssize_t got =
        ::pread(fd_, buffer.data(), want, static_cast<off_t>(offset + copied));
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      SetError(error_message, Describe(errno, "Failed to read file", path_));
      return false;
    }
    if (got == 0) {
      SetError(error_message, "Unexpected end of file: " + path_);
      return false;
    }
    if (!sink->Write(buffer.data(), static_cast<std::size_t>(got),
                     error_message)) {
      return false;
    }
    copied += static_cast<std::uint64_t>(got);
  }
  return true;
}

bool WriteFully(int fd, const void* data, std::size_t size,
                std::string* error_message) {
  const unsigned char* bytes = static_cast<const unsigned char*>(data);
  std::size_t done = 0;
  while (done < size) {
    const ssize_t written = ::write(fd, bytes + done, size - done);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      SetError(error_message, std::strerror(errno));
      return false;
    }
    done += static_cast<std::size_t>(written);
  }
  return true;
}

// ---- 清理与私有工作目录 -----------------------------------------------------

void RemoveTreeNoFollow(const std::string& path) {
  struct stat info;
  if (::lstat(path.c_str(), &info) != 0) {
    return;
  }
  if (!S_ISDIR(info.st_mode)) {
    ::unlink(path.c_str());
    return;
  }
  DIR* raw_dir = ::opendir(path.c_str());
  if (raw_dir != nullptr) {
    while (struct dirent* item = ::readdir(raw_dir)) {
      const std::string name = item->d_name;
      if (name == "." || name == "..") {
        continue;
      }
      RemoveTreeNoFollow(path + "/" + name);
    }
    ::closedir(raw_dir);
  }
  ::rmdir(path.c_str());
}

TempDirectoryGuard::~TempDirectoryGuard() { Remove(); }

bool TempDirectoryGuard::Create(const std::string& parent,
                                const std::string& prefix,
                                std::string* error_message) {
  Remove();
  std::string base = parent.empty() ? std::string(".") : parent;
  if (base.size() > 1 && base.back() == '/') {
    base.pop_back();
  }
  std::string pattern = base + "/" + prefix + "XXXXXX";
  std::vector<char> buffer(pattern.begin(), pattern.end());
  buffer.push_back('\0');
  if (::mkdtemp(buffer.data()) == nullptr) {
    SetError(error_message,
             Describe(errno, "Failed to create private workspace in", base));
    return false;
  }
  path_.assign(buffer.data());
  // mkdtemp 已经给 0700，但这条安全属性不能依赖 umask
  // 或平台细节，显式再设一次。
  if (::chmod(path_.c_str(), 0700) != 0) {
    SetError(error_message,
             Describe(errno, "Failed to secure private workspace", path_));
    Remove();
    return false;
  }
  return true;
}

std::string TempDirectoryGuard::Child(const std::string& name) const {
  if (path_.empty()) {
    return name;
  }
  return path_ + "/" + name;
}

void TempDirectoryGuard::Remove() {
  if (path_.empty()) {
    return;
  }
  RemoveTreeNoFollow(path_);
  path_.clear();
}

// ---- 发布 ------------------------------------------------------------------

bool PublishNoReplace(const std::string& temp_file,
                      const std::string& final_path,
                      std::string* error_message) {
  if (temp_file.empty() || final_path.empty()) {
    SetError(error_message, "Internal error: publish with an empty path");
    return false;
  }
  const std::string parent = ParentDirectoryOf(final_path);

  // 首选：硬链接。同文件系统内原子，"目标已存在"由内核保证返回 EEXIST。
  errno = 0;
  if (file_io_syscalls::LinkHook()(temp_file.c_str(), final_path.c_str()) ==
      0) {
    // final 已经是完整文件了；temp 只是同一 inode 的第二个名字，删掉即可。
    // 万一删不掉也不该让调用方以为发布失败——目录会在 workspace 清理时消失。
    (void)::unlink(temp_file.c_str());
    SyncDirectoryQuietly(parent);
    return true;
  }
  const int link_error = errno;
  if (link_error == EEXIST) {
    SetError(error_message, "Archive file already exists: " + final_path);
    return false;
  }

  // 备选一：renameat2(RENAME_NOREPLACE)。同样是原子的不覆盖语义。
  errno = 0;
  if (file_io_syscalls::RenameNoReplaceHook()(temp_file.c_str(),
                                              final_path.c_str()) == 0) {
    SyncDirectoryQuietly(parent);
    return true;
  }
  const int rename_error = errno;
  if (rename_error == EEXIST) {
    SetError(error_message, "Archive file already exists: " + final_path);
    return false;
  }

  // 两个原子 no-replace 原语都不可用 / 都失败：**fail closed**。
  //
  // 这里绝不能再退回 "lstat(final) 确认不存在，然后普通 rename(temp, final)"：
  // 检查与 rename 之间，另一个进程（或另一个 dsh 任务）完全可以创建 final，
  // 而普通 rename() 会**直接覆盖**它。"绝不覆盖已有备份"是备份工具最不能
  // 让步的一条，所以宁可用一个没有人读得懂的失败，也不要一次静默覆盖。
  //
  // 也不把真实错误掩盖成 "unsupported"：两个 errno 都原样报出来，
  // 让调用方分得清"这个文件系统不支持"和"磁盘真的坏了"。
  // 只有"这个文件系统/内核根本不支持"才算 unsupported。EACCES / EIO / EMLINK
  // 都是真实错误，它们的 errno 必须原样透出去，不能被说成"不支持"——
  // 那会让排障的人以为换个文件系统就好了。
  const bool link_unsupported = link_error == EPERM ||
                                link_error == EOPNOTSUPP ||
                                link_error == ENOSYS || link_error == EXDEV;
  const bool rename_unsupported = rename_error == ENOSYS ||
                                  rename_error == EINVAL ||
                                  rename_error == EOPNOTSUPP;
  std::string reason =
      "Failed to publish archive file atomically: " + final_path +
      ": link(): " + std::strerror(link_error) +
      (link_unsupported ? " (unsupported here)" : "") +
      "; renameat2(RENAME_NOREPLACE): " + std::strerror(rename_error) +
      (rename_unsupported ? " (unsupported here)" : "") +
      "; refusing to fall back to a non-atomic rename";
  SetError(error_message, reason);
  return false;
}

bool CheckFreeSpace(const std::string& directory, std::uint64_t need_bytes,
                    std::string* error_message) {
  std::string base = directory.empty() ? std::string(".") : directory;
  if (base.size() > 1 && base.back() == '/') {
    base.pop_back();
  }
  struct statvfs info;
  if (::statvfs(base.c_str(), &info) != 0) {
    // 拿不到就不拦：这是 sanity check，不是配额系统，不该因为它失败而让用户
    // 连正常的备份都做不了。
    return true;
  }
  const std::uint64_t available = static_cast<std::uint64_t>(info.f_bavail) *
                                  static_cast<std::uint64_t>(info.f_frsize);
  // 留 1% 余量：目录项、文件系统元数据、以及同时发生的其他写入都要算进去。
  const std::uint64_t reserve = available / 100;
  if (need_bytes > available - reserve) {
    SetError(error_message, "Not enough free space in " + base + ": need " +
                                std::to_string(need_bytes) +
                                " bytes, available " +
                                std::to_string(available));
    return false;
  }
  return true;
}

}  // namespace backupproject
