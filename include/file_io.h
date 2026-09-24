// file_io.h
//
// 流水线里的 I/O 原语：带缓冲的"必须新建"输出、精确读满的输入、区间复制。
//
// 单独成模块的理由是三条边界必须处处一致：
//   * 输出永远用 O_CREAT|O_EXCL：不覆盖、不截断用户已有的文件；
//   * 失败路径必须能删掉自己创建的半成品——用户看到目录里有个 .bak 就会
//     把它当可用备份，留一个坏文件比什么都不留更危险；
//   * 整数偏移全部用 uint64，越界判断用减法，避免 offset + size 溢出。

#ifndef BACKUP_PROJECT_INCLUDE_FILE_IO_H_
#define BACKUP_PROJECT_INCLUDE_FILE_IO_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace backupproject {

// 只写、必须新建的输出文件，内部 256 KiB 缓冲。
//
// Patch() 走 pwrite，用来回填"写完才知道"的字段（entry_count、auth_tag）。
// Flush() 之后才能安全 Patch 已经写过的区间。
class FileSink {
 public:
  FileSink() = default;
  ~FileSink();

  FileSink(const FileSink&) = delete;
  FileSink& operator=(const FileSink&) = delete;

  // 打开一个必须不存在的文件。父目录不会被创建（调用方负责）。
  bool Open(const std::string& path, std::string* error_message);
  // 在 directory 下创建一个唯一命名的临时文件（mkstemp，0600，O_EXCL 语义）。
  // 名字带 prefix 前缀，方便出问题时一眼看出是谁留下的。
  bool OpenTemp(const std::string& directory, const std::string& prefix,
                std::string* error_message);
  bool Write(const void* data, std::size_t size, std::string* error_message);
  bool Patch(std::uint64_t offset, const void* data, std::size_t size,
             std::string* error_message);
  bool Flush(std::string* error_message);
  // 冲刷 + fsync + close。成功后对象进入"已关闭"状态。
  bool Close(std::string* error_message);
  // 失败路径：关掉句柄并 unlink 半成品。可以重复调用。
  void Abandon();

  // 已经接受过的逻辑字节数（含还在缓冲区里、尚未落到 fd 的那部分）。
  std::uint64_t bytes_written() const { return bytes_written_ + buffered_; }
  int fd() const { return fd_; }
  const std::string& path() const { return path_; }

 private:
  bool WriteRaw(const void* data, std::size_t size, std::string* error_message);

  int fd_ = -1;
  std::string path_;
  std::uint64_t bytes_written_ = 0;
  std::size_t buffered_ = 0;
  std::vector<unsigned char> buffer_;
};

// 只读的普通文件，打开时记录大小。
class FileSource {
 public:
  FileSource() = default;
  ~FileSource();

  FileSource(const FileSource&) = delete;
  FileSource& operator=(const FileSource&) = delete;

  bool Open(const std::string& path, std::string* error_message);
  // 精确定位读满 size 个字节；短读或 EOF 都返回 false。
  bool ReadAt(std::uint64_t offset, void* buffer, std::size_t size,
              std::string* error_message) const;
  // 把 [offset, offset + size) 流式复制到 sink。
  bool CopyRangeTo(std::uint64_t offset, std::uint64_t size, FileSink* sink,
                   std::string* error_message) const;
  void Close();

  bool valid() const { return fd_ >= 0; }
  int fd() const { return fd_; }
  std::uint64_t size() const { return size_; }
  const std::string& path() const { return path_; }

 private:
  int fd_ = -1;
  std::string path_;
  std::uint64_t size_ = 0;
};

// 把整块数据写到 fd 直到写完，处理短写与 EINTR。
bool WriteFully(int fd, const void* data, std::size_t size,
                std::string* error_message);

// 占住一个唯一的临时文件名，然后立刻把它释放掉。
//
// 为什么不直接用 OpenTemp：mkstemp 会真的创建文件，而 pack 层（MyPack v2 /
// USTAR）必须用 O_CREAT|O_EXCL 自己创建输出——留着那个空文件会让创建立刻
// 失败并报 "File exists"。所以这里只借名字：名字里带着 pid 与 mkstemp 的随机
// 后缀，被别的进程抢注的窗口可以忽略；就算真被抢注，后续写入会明确失败，
// 不会写错文件。
bool ReserveTempPath(const std::string& directory, const std::string& prefix,
                     std::string* path, std::string* error_message);

// 临时文件的 RAII 守卫：析构时 unlink。Release() 之后不再管它。
//
// 存在的意义是"成功或失败都清理"这条要求有一个能被编译器保证的落点：
// 中途 return、抛错、提前退出都不会把中间产物留在用户目录里。
class TempFileGuard {
 public:
  explicit TempFileGuard(std::string path) : path_(std::move(path)) {}
  ~TempFileGuard();
  TempFileGuard(const TempFileGuard&) = delete;
  TempFileGuard& operator=(const TempFileGuard&) = delete;

  void Release() { path_.clear(); }
  const std::string& path() const { return path_; }
  bool active() const { return !path_.empty(); }

 private:
  std::string path_;
};

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_FILE_IO_H_
