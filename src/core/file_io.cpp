// file_io.cpp
//
// 见 file_io.h。
// 本文件是 include/file_io.h 的实现，只放"磁盘 I/O 原语"，不含归档/业务语义：
// 它不知道什么是 entry、什么是 manifest，只认字节、路径和 fd。
//
// 数据流：调用方把内容写进 FileSink，Close() 成功后得到已 fsync 的临时文件，
// 再用 PublishNoReplace / PublishReplacing 发布成正式文件；FileSource 是反方向
// 的只读端（打开时记 size、按绝对偏移读，不共享文件游标）。
//
// 核心不变量：输出永远 0600 且 O_CREAT|O_EXCL（或 mkstemp）——不截断、不跟随
// 别人预放的符号链接，也不比 legacy v0.1 的 0600 更宽松；"fd 是否打开"
// （fd_ >= 0）与"路径是否归本对象所有"（owns_path_）分开记，因为 Close() 失败
// 时 fd 已经关闭而路径仍归自己，半成品照样要 unlink；失败一律返回 false +
// error_message，已写出的字节不回滚，由调用方整份丢弃。
// 线程约束：单个 FileSink/FileSource 不是线程安全的（含 256 KiB 缓冲与偏移
// 状态），唯一的进程级全局是 file_io_syscalls 注入点，只能在单线程测试里替换。

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

// 路径里最后一个 '/' 之前的部分。"a" -> "."、"/a" -> "/"、"/" -> "/"。
//
// 刻意**不**导出：项目里另一个模块的同名函数在"没有 '/' 时返回空串"，
// 两种语义同时可见会让调用点悄悄选错一个。对外只提供按用途命名的入口
// （EnsurePrivateDirectoryFor / WriteFileAtomicallyReplacing）。
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

void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) {
    *error_message = text;
  }
}

std::string Describe(int error_number, const std::string& action,
                     const std::string& path) {
  return action + ": " + path + ": " + std::strerror(error_number);
}

std::string BaseNameOf(const std::string& path) {
  const std::size_t slash = path.rfind('/');
  if (slash == std::string::npos) return path;
  return path.substr(slash + 1);
}

// 目录项落盘不靠 fsync 文件本身，而靠 fsync 父目录。失败只当诊断：
// 内容已经写完了，因为"目录项可能还没落盘"去报失败反而会误导调用方。
// 返回值刻意是 void：目录 fsync 失败只影响"目录项何时落盘"，调用方若把它当成
// 发布成功的判据，就会在内容已经完整的情况下报出一个并不存在的失败。
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

// 注入点存在的唯一理由是"可测"：fsync/close 失败、link 与 renameat2 的三种组合
// 在真实文件系统上没法按需制造。它们是进程级可变全局，测试用完必须还原，生产
// 路径永远走 Default* 实现；正因为是全局，这些 hook 只能在单线程测试里替换。
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
// 状态机：Idle → (Open|OpenTemp) 打开 → Write/Flush/Patch → Close →
// Committed；任何一步失败都可以 Abandon()（析构也会兜底）回到 Idle 并 unlink。
// fd_ >= 0 表示可写，owns_path_ 表示"路径还归本对象负责清理"，committed_ 一旦
// 为真就不再 unlink。用三个独立标志而不是一个 enum，是因为 Close() 失败恰好
// 落在"fd 已关、路径仍归我"这个中间态上。

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

// 前置条件：对象处于 Idle（既没开 fd 也没持有路径），重复打开报内部错误而不是
// 悄悄丢掉旧 fd。父目录不会被创建（调用方负责）；O_EXCL 让"目标已存在"直接
// 失败——这是"绝不覆盖"在文件系统层的落点。
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

// mkstemp 生成唯一名字（0600 + O_CREAT|O_EXCL），明文中间产物既不会撞名也不会
// 跟随别人预放的符号链接。随机化的文件名不是安全边界，0700 的父目录才是；
// path() 事后返回真实临时名，调用方发布或清理时都用它。
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

// 绕过用户态缓冲直写 fd，bytes_written_ 只在真正写成功后累加；失败时给底层错误
// 补上路径，让"哪个文件写坏了"不必靠调用栈去猜。
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

// 语义：把 size 个字节全部接受下来（可能只是进缓冲），返回 true 就代表调用方
// 不必重试。大块直写、小块攒进缓冲，是为了不让 512 字节的 header 和对齐填充
// 各占一次 syscall；fd_ < 0 时报内部错误，因为那是调用方的编排 bug。
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

// 回填"写完才知道"的字段（entry_count、auth_tag）：先把自己的缓冲 Flush 落盘，
// 再用 pwrite 定点覆盖，绝不改动文件偏移。同一区间可以重复 Patch，但调用方要
// 保证该区间已经写满——pwrite 不会补洞，空洞在稀疏文件里读出来是 0。
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

// 顺序不可交换：Flush（缓冲落盘）→ fsync（内容落盘）→ close（释放 fd）。
// close 失败仍然算失败，但**不重试**：Linux 上 close 返回 EINTR 时 fd 已经
// 释放，重试可能关掉别的线程刚拿到的 fd。失败时刻意保留 owns_path_，让
// Abandon() 与析构仍然能把这份半成品删掉。
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

// 失败清理：关掉还开着的句柄，并在 owns_path_ && !committed_ 时 unlink。清理
// 用真正的 close 而不是注入点，否则测试把 close 换成"总是失败"就会漏 fd。
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
// 只读端：Open 时 fstat 记下 size，之后所有读取都用 pread 的绝对偏移，不共享
// 文件游标，因此同一个 fd 可以被多个线程安全地并发读。

FileSource::~FileSource() { Close(); }

// 先 Close() 是"重新打开"语义，允许同一个对象换文件。只接受普通文件：调用点
// 都是"按偏移读归档"，目录/设备/FIFO 在这里没有意义，早点拒绝比让后面的 pread
// 返回 EISDIR 更容易排障。
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

// 必须精确读满 size 字节：短读由循环补齐，EOF 直接失败——解析 header 的调用方
// 不能容忍"只解了一半"；offset 是绝对偏移，越界读由 EOF 检查兜住。
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

// 流式复制 [offset, offset+size) 到 sink，内存占用固定为 kCopyBufferSize，与
// 区间大小无关。任何一段失败就整段失败，但已经写进 sink 的字节不会撤回——
// 调用方拿到 false 必须丢弃整个 sink，而不是以为"前一半是好的"。
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

// 写满或失败，没有第三种结果：短写由循环补齐，EINTR 重试。它只保证"交给了
// 内核"，不保证落盘——fsync 是调用方的事，而这两件事的失败语义完全不同
// （一个是写不进去，一个是掉电后可能丢）。
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

// 递归删除但不 follow 软链接：lstat + unlink 删链接本身，不进入它指向的目录。
// 失败一律静默（调用点都是失败清理路径，报错也没人能处理）；递归深度等于目录
// 深度，软链接环不会让它无限递归。
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

// 析构即清理：无论成功还是失败，明文中间产物都不会留在用户目录里。这是
// TempDirectoryGuard 存在的全部理由——把"记得删"变成编译器保证的事。
TempDirectoryGuard::~TempDirectoryGuard() { Remove(); }

// 先 Remove() 让同一个对象可以重复使用。mkdtemp 已经给 0700，这里仍然显式
// chmod 一次：这条安全属性不该依赖 umask 或平台细节；chmod 失败要把刚建的
// 目录删掉，绝不能留下一个权限不明的目录让调用方继续用。
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

// 契约：temp_file 必须已经 fsync；成功时 final_path 指向完整文件、temp_file 被
// 消费掉；失败时 final_path **一定不存在**（绝不部分发布），temp_file 由调用方
// 清理。决策顺序是有意的：link 是最老的"创建即原子"原语，先试它；renameat2
// 需要较新的内核/文件系统，作为备选；两个都不可用时 fail closed，绝不退回
// 非原子的 "lstat 确认不存在 + rename"（那中间有 TOCTOU 覆盖窗口）。
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
  // 检查与 rename 之间，另一个进程完全可以创建 final，
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

// 只给"用户已经明确同意覆盖"的调用点（例如下载 --force）。这条路径允许覆盖，
// 所以不需要 no-replace 原语：rename 一步原子，读者要么看到旧的完整内容、要么
// 看到新的完整内容；失败时 final_path 保持原样。
bool PublishReplacing(const std::string& temp_file,
                      const std::string& final_path,
                      std::string* error_message) {
  if (temp_file.empty() || final_path.empty()) {
    SetError(error_message, "Internal error: publish with an empty path");
    return false;
  }
  // 这一条路径**允许**覆盖，所以不需要"不覆盖"的原语：目标已存在时 rename
  // 直接替换它，不存在时就是创建——两种情况都是一步原子的，与"先删再改"
  // 或"先截断再写"有本质区别：任何时刻读者看到的都是完整的一份。
  if (::rename(temp_file.c_str(), final_path.c_str()) != 0) {
    SetError(error_message, Describe(errno, "Failed to replace", final_path));
    return false;
  }
  SyncDirectoryQuietly(ParentDirectoryOf(final_path));
  return true;
}

// 可用空间 sanity check，不是配额系统：statvfs 拿不到就放行，绝不因为它失败而
// 让用户连正常备份都做不了。用 f_bavail（非特权用户可用）而不是 f_bfree，并留
// 1% 余量给目录项、文件系统元数据和同时发生的其他写入。
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

// ---- 目录与"整份文件原子替换" ----

// mkdir -p，权限固定 0700。路径已存在且确实是目录时直接成功，**不改**已有目录
// 的权限（改别人的目录权限不是本函数的职责）；存在但不是目录时明确失败，绝不
// 删掉它重建——那不是"创建目录"，那是破坏用户的数据。
bool EnsurePrivateDirectory(const std::string& path,
                            std::string* error_message) {
  if (path.empty() || path == "." || path == "/") return true;

  struct stat status;
  if (::lstat(path.c_str(), &status) == 0) {
    if (S_ISDIR(status.st_mode)) return true;
    // 存在的同名东西不是目录（普通文件、符号链接……）。绝不试图删掉它再建：
    // 那不是"创建目录"，那是破坏用户的数据。
    SetError(error_message,
             "Cannot create the directory: not a directory: " + path);
    return false;
  }
  if (errno != ENOENT) {
    SetError(error_message, Describe(errno, "Failed to inspect", path));
    return false;
  }

  const std::string parent = ParentDirectoryOf(path);
  // parent == path 只会在 path 不含 '/' 且不是 "." 时出现，那时 parent 是 "."
  // 已经被上面拦掉了；这个判断是防御性的，避免任何输入造成无限递归。
  if (parent != path && !EnsurePrivateDirectory(parent, error_message)) {
    return false;
  }
  if (::mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) {
    SetError(error_message,
             Describe(errno, "Failed to create the directory", path));
    return false;
  }
  return true;
}

// 按用途命名的包装："这个文件要落在哪个目录里"。比让调用方自己算父目录再调
// EnsurePrivateDirectory 少一次选错语义的机会（两者的边界语义并不相同）。
bool EnsurePrivateDirectoryFor(const std::string& file_path,
                               std::string* error_message) {
  if (file_path.empty()) {
    SetError(error_message, "Output file path is empty.");
    return false;
  }
  return EnsurePrivateDirectory(ParentDirectoryOf(file_path), error_message);
}

// 整份文件原子替换，给"配置/状态"这类小文件用：mkstemp → write → fsync →
// close → rename → fsync(目录)，任何一步失败都清理临时文件，绝不留下半份 .tmp。
// rename 之前目标文件一个字节都没动，所以崩溃只会留下完整的旧内容或完整的新
// 内容；目录 fsync 失败不算错误（少数文件系统不支持对目录 fsync）。
bool WriteFileAtomicallyReplacing(const std::string& path,
                                  const std::string& data,
                                  std::string* error_message) {
  if (path.empty()) {
    SetError(error_message, "Output file path is empty.");
    return false;
  }

  const std::string directory = ParentDirectoryOf(path);
  const std::string base = BaseNameOf(path);
  // mkstemp 的模板必须以 XXXXXX 结尾。前缀截到 40 字节：NAME_MAX 是 255，
  // 一个很长的目标名加上后缀会把模板撑到超长，而临时文件叫什么并不重要。
  std::string prefix = base.size() > 40 ? base.substr(0, 40) : base;
  if (prefix.empty()) prefix = "atomic";
  const std::string pattern =
      (directory.empty() ? std::string(".") : directory) + "/" + prefix +
      ".tmp-XXXXXX";
  std::vector<char> buffer(pattern.begin(), pattern.end());
  buffer.push_back('\0');

  // O_CREAT|O_EXCL 由 mkstemp 内部保证：名字已存在（哪怕是个符号链接）就直接
  // 换一个候选名重试，所以这里既不会跟随链接，也不会截断别人预放的文件。
  const int fd = ::mkstemp(buffer.data());
  if (fd < 0) {
    SetError(error_message,
             Describe(errno, "Failed to create temporary file", pattern));
    return false;
  }
  const std::string temporary(buffer.data());
  // mkstemp 不带 O_CLOEXEC，显式补上：临时 fd 没有任何理由泄漏进子进程。
  (void)::fcntl(fd, F_SETFD, FD_CLOEXEC);

  if (!WriteFully(fd, data.data(), data.size(), error_message)) {
    const std::string reason =
        (error_message != nullptr ? *error_message
                                  : std::string("write failed"));
    ::close(fd);
    ::unlink(temporary.c_str());
    SetError(error_message, reason);
    return false;
  }
  if (::fsync(fd) != 0) {
    const int saved_errno = errno;
    ::close(fd);
    ::unlink(temporary.c_str());
    SetError(error_message,
             Describe(saved_errno, "Failed to fsync", temporary));
    return false;
  }
  if (::close(fd) != 0) {
    const int saved_errno = errno;
    ::unlink(temporary.c_str());
    SetError(error_message,
             Describe(saved_errno, "Failed to close", temporary));
    return false;
  }
  // 到这里为止目标文件还没被碰过：任何一步失败，磁盘上留下的都是旧的完整内容。
  if (::rename(temporary.c_str(), path.c_str()) != 0) {
    const int saved_errno = errno;
    ::unlink(temporary.c_str());
    SetError(error_message, Describe(saved_errno, "Failed to replace", path));
    return false;
  }

  // 目录 fsync 让 rename 本身落盘。少数文件系统不支持对目录 fsync
  // （EINVAL / ENOTSUP），那不是错误；其它 errno 是真实 I/O 问题，如实报告。
  const int directory_fd =
      ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (directory_fd >= 0) {
    if (::fsync(directory_fd) != 0 && errno != EINVAL && errno != ENOTSUP) {
      const int saved_errno = errno;
      ::close(directory_fd);
      SetError(error_message,
               Describe(saved_errno, "Failed to fsync directory", directory));
      return false;
    }
    ::close(directory_fd);
  }
  return true;
}

}  // namespace backupproject
