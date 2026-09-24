// file_io.cpp
//
// 见 file_io.h。

#include "file_io.h"

#include <fcntl.h>
#include <sys/stat.h>
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

// 缓冲大小取 256 KiB：足以把 512 字节 header、对齐填充和小文件正文聚合成
// 大块写，同时又远小于内存关心量级。
constexpr std::size_t kSinkBufferSize = 256 * 1024;

// 区间复制的读缓冲：64 KiB～1 MiB 之间的保守取值。
constexpr std::size_t kCopyBufferSize = 256 * 1024;

}  // namespace

FileSink::~FileSink() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

bool ReserveTempPath(const std::string& directory, const std::string& prefix,
                     std::string* path, std::string* error_message) {
  FileSink sink;
  if (!sink.OpenTemp(directory, prefix, error_message)) {
    return false;
  }
  const std::string reserved = sink.path();
  if (!sink.Close(error_message)) {
    sink.Abandon();
    return false;
  }
  if (::unlink(reserved.c_str()) != 0 && errno != ENOENT) {
    SetError(
        error_message,
        Describe(errno, "Failed to release temporary file name", reserved));
    return false;
  }
  *path = reserved;
  return true;
}

TempFileGuard::~TempFileGuard() {
  if (!path_.empty()) {
    ::unlink(path_.c_str());
  }
}

bool FileSink::Open(const std::string& path, std::string* error_message) {
  if (fd_ >= 0) {
    SetError(error_message, "Internal error: sink already open: " + path);
    return false;
  }
  if (path.empty()) {
    SetError(error_message, "Output file path is empty.");
    return false;
  }
  const int raw_fd =
      ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  if (raw_fd < 0) {
    SetError(error_message, Describe(errno, "Failed to create file", path));
    return false;
  }
  fd_ = raw_fd;
  path_ = path;
  bytes_written_ = 0;
  buffered_ = 0;
  buffer_.assign(kSinkBufferSize, 0);
  return true;
}

bool FileSink::OpenTemp(const std::string& directory, const std::string& prefix,
                        std::string* error_message) {
  if (fd_ >= 0) {
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
    return true;
  }
  bool ok = Flush(error_message);
  if (ok && ::fsync(fd_) != 0) {
    SetError(error_message, Describe(errno, "Failed to sync file", path_));
    ok = false;
  }
  const int fd = fd_;
  fd_ = -1;
  if (::close(fd) != 0 && ok) {
    SetError(error_message, Describe(errno, "Failed to close file", path_));
    ok = false;
  }
  return ok;
}

void FileSink::Abandon() {
  if (fd_ < 0) {
    return;
  }
  const int fd = fd_;
  fd_ = -1;
  buffered_ = 0;
  ::close(fd);
  if (!path_.empty()) {
    ::unlink(path_.c_str());
  }
}

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

}  // namespace backupproject
