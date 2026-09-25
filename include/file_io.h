// file_io.h
//
// 流水线里的 I/O 原语：带缓冲的"必须新建"输出、精确读满的输入、私有工作目录、
// 不覆盖的发布、以及磁盘空间 sanity check。
//
// 单独成模块的理由是几条边界必须处处一致：
//   * 输出永远是 0600 + O_CREAT|O_EXCL：不覆盖、不截断用户已有的文件，也不会
//     比 legacy v0.1（0600）更宽松；
//   * 明文中间产物（packed / token / compressed / decrypted）只能出现在 0700 的
//     私有工作目录里，文件名不可预测也不必要——目录本身就是隔离边界；
//   * "对象是否还拥有这个路径"和"文件描述符是否还开着"是两件事。finalize 可能
//     在 fd 已经关掉之后失败，那时半成品仍然必须被删掉；
//   * 正式 .bak 不是边生成边发布的：先在私有目录里写完整、fsync、close，
//     再用不覆盖的方式发布，杜绝"发布了半个文件"。

#ifndef BACKUP_PROJECT_INCLUDE_FILE_IO_H_
#define BACKUP_PROJECT_INCLUDE_FILE_IO_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace backupproject {

// ---- 低层 syscall 注入点（只给测试用）--------------------------------------
//
// fsync / close 失败很难天然制造，而"finalize 失败之后半成品必须仍然被删掉"
// 恰恰是最需要覆盖的一条状态机。这里把两个 syscall 收成可替换的函数指针，
// 测试注入失败之后必须还原。生产路径永远走默认实现。
namespace file_io_syscalls {

using FsyncFn = int (*)(int);
using CloseFn = int (*)(int);

FsyncFn& FsyncHook();
CloseFn& CloseHook();

}  // namespace file_io_syscalls

// 只写、必须新建的输出文件，内部 256 KiB 缓冲，创建模式固定 0600。
//
// Patch() 走 pwrite，用来回填"写完才知道"的字段（entry_count、auth_tag）。
// Flush() 之后才能安全 Patch 已经写过的区间。
class FileSink {
 public:
  FileSink() = default;
  ~FileSink();

  FileSink(const FileSink&) = delete;
  FileSink& operator=(const FileSink&) = delete;

  // 打开一个必须不存在的文件（0600）。父目录不会被创建（调用方负责）。
  bool Open(const std::string& path, std::string* error_message);
  // 在 directory 下创建一个唯一命名的临时文件（mkstemp => 0600）。
  bool OpenTemp(const std::string& directory, const std::string& prefix,
                std::string* error_message);
  bool Write(const void* data, std::size_t size, std::string* error_message);
  bool Patch(std::uint64_t offset, const void* data, std::size_t size,
             std::string* error_message);
  bool Flush(std::string* error_message);

  // 冲刷 → fsync → close。三步全部成功才算 committed。
  //
  // 任何一步失败都返回 false，并且**不**放弃路径所有权：对象停在
  // "owns_path && !committed"，所以随后（或由析构函数）调用 Abandon()
  // 仍然会 unlink 这个半成品。这正是"fd 已经关掉但路径仍归我管"那个状态。
  bool Close(std::string* error_message);

  // 失败路径：关掉还开着的句柄，并在 owns_path && !committed 时 unlink。
  // 可以重复调用；成功提交之后再调用是空操作。
  void Abandon();

  // 已经接受过的逻辑字节数（含还在缓冲区里、尚未落到 fd 的那部分）。
  std::uint64_t bytes_written() const { return bytes_written_ + buffered_; }
  bool owns_path() const { return owns_path_; }
  bool committed() const { return committed_; }
  int fd() const { return fd_; }
  const std::string& path() const { return path_; }

 private:
  bool WriteRaw(const void* data, std::size_t size, std::string* error_message);

  int fd_ = -1;
  std::string path_;
  std::uint64_t bytes_written_ = 0;
  std::size_t buffered_ = 0;
  std::vector<unsigned char> buffer_;
  // 路径所有权与提交状态分开记：fd_
  // 只说明句柄，说明不了"这个文件还是不是我写的"。
  bool owns_path_ = false;
  bool committed_ = false;
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

// 递归删除一棵树，不 follow 软链接（软链接本身被 unlink，不进去）。
// 尽力而为：失败不报错，因为它的调用点都是失败清理路径。
void RemoveTreeNoFollow(const std::string& path);

// 私有工作目录：mkdtemp 出来的唯一目录，模式固定 0700。
//
// 一次 v2 Backup / Restore 的所有明文中间产物都住在里面，所以"中间文件是
// 0600、目录是 0700"这条要求有一个能被编译器保证的落点：析构即递归清理，
// 成功或失败都不会把明文留在用户目录里。
class TempDirectoryGuard {
 public:
  TempDirectoryGuard() = default;
  ~TempDirectoryGuard();
  TempDirectoryGuard(const TempDirectoryGuard&) = delete;
  TempDirectoryGuard& operator=(const TempDirectoryGuard&) = delete;

  // 在 parent 下建 <prefix>XXXXXX。parent 必须已经存在。
  bool Create(const std::string& parent, const std::string& prefix,
              std::string* error_message);
  // 目录内的固定名字路径：靠 0700 目录隔离，不需要再做随机化。
  std::string Child(const std::string& name) const;
  // 放弃所有权（例如目录已经被 rename 走了）。
  void Release() { path_.clear(); }
  // 立刻递归删除。可以重复调用。
  void Remove();
  const std::string& path() const { return path_; }
  bool active() const { return !path_.empty(); }

 private:
  std::string path_;
};

// 把已经 fsync 好的临时文件"不覆盖地"发布成 final_path。
//
//   1. link(temp, final)：同文件系统、原子、已存在即 EEXIST —— 首选；
//   2. link 不被支持时（EPERM / EOPNOTSUPP / ENOSYS / EACCES）退到
//      renameat2(RENAME_NOREPLACE)；
//   3. 再退到"先 lstat 确认不存在再 rename"，这一步有理论上的 TOCTOU 窗口，
//      但它只在既没有 link 也没有 renameat2 的文件系统上才会走到。
//
// 成功之后会 fsync 父目录（失败只当诊断，不影响结果）。
// 失败时 final_path 一定不存在；temp_file 由调用方负责清理。
bool PublishNoReplace(const std::string& temp_file,
                      const std::string& final_path,
                      std::string* error_message);

// 可用空间 sanity check：目录所在文件系统的可用字节数是否够 need_bytes。
// 明确不够就提前失败，而不是写到 ENOSPC 才知道。
bool CheckFreeSpace(const std::string& directory, std::uint64_t need_bytes,
                    std::string* error_message);

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_FILE_IO_H_
