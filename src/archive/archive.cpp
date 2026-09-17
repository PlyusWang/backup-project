// archive.cpp
//
// Archive Format v0.1 的读写实现。格式定义见 docs/format/archive_v0.1.md。
//
// 两个刻意的设计，先说清楚：
//
//   1. 所有整数字段都是手工按 little-endian 逐字节写进缓冲区的，
//      绝不 write(fd, &some_struct, sizeof(some_struct))。结构体布局会
//      被 padding、对齐和主机字节序影响，同一份代码换个编译器读出来就变了。
//
//   2. 读侧分两阶段：preflight 先把整个归档校验完（magic、每个 header、
//      路径合法性、payload 边界、entry_count、EOF），确认无误之后才真的
//      往 destination 写。坏归档不会留下半个恢复目录。

// 这个模块对外只有两件事：把目录树写成一个文件，把那个文件还原成目录树。
// 为了保证这两件事在异常输入下也不会伤到磁盘，代码里反复出现三类约束：
//
//   1. 写侧顺序：先验源、再验拓扑、再看目标是否存在、最后才 open(O_EXCL)。
//      任何一步失败都不留下残留文件；
//   2. 读侧两阶段：preflight 只看不写，结构合法之后才创建 destination；
//   3. 数值边界：宁可用减法比较，也不写 position + size 这种可能溢出的表达式。

#include "archive.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "file_system.h"
#include "filter.h"

namespace backupproject {

namespace {

// 格式常量定义在 archive.h 的 archive_v01 命名空间里：外部工具、测试和
// 将来的压缩层都需要认得这些数字，不能把它们锁死在这个 .cpp 内部。
// 这里只做 using 声明，正文里仍用短名字书写。
using archive_v01::kEntryCountOffset;
using archive_v01::kEntryHeaderSize;
using archive_v01::kFormatFlags;
using archive_v01::kFormatVersion;
using archive_v01::kGlobalHeaderSize;
using archive_v01::kMagic;
using archive_v01::kMaxMtimeNsec;
using archive_v01::kMaxPathLength;
using archive_v01::kPermissionMask;
using archive_v01::kTypeDirectory;
using archive_v01::kTypeRegularFile;

// 下面两个是纯实现细节，不属于格式契约，留在本文件里。
// payload 全程流式处理，固定 64 KiB 缓冲；不把文件读进内存。
constexpr std::size_t kIoBufferSize = 64 * 1024;

// 目录先按可写权限建出来，所有子项完成后再恢复真正的 mode：
// 归档里可能是 0555 的目录，先设成 0555 后面的孩子就建不进去了。
constexpr mode_t kStagingDirectoryMode = 0700;

void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) {
    *error_message = text;
  }
}

// 统一错误文案：动作 + 路径 + errno 说明，三个要素都带上。
std::string Describe(int error_number, const std::string& action,
                     const std::string& path) {
  return action + ": " + path + ": " + std::strerror(error_number);
}

// fd / DIR* 的 RAII：任何提前 return 都会自动关闭。
class ScopedFd {
 public:
  ScopedFd() = default;
  explicit ScopedFd(int fd) : fd_(fd) {}
  ~ScopedFd() { Reset(); }

  ScopedFd(const ScopedFd&) = delete;
  ScopedFd& operator=(const ScopedFd&) = delete;

  int get() const { return fd_; }
  bool valid() const { return fd_ >= 0; }
  int Release() {
    const int fd = fd_;
    fd_ = -1;
    return fd;
  }
  void Reset(int fd = -1) {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = fd;
  }

 private:
  int fd_ = -1;
};

class ScopedDir {
 public:
  explicit ScopedDir(DIR* dir) : dir_(dir) {}
  ~ScopedDir() {
    if (dir_ != nullptr) {
      ::closedir(dir_);
    }
  }

  ScopedDir(const ScopedDir&) = delete;
  ScopedDir& operator=(const ScopedDir&) = delete;

  DIR* get() const { return dir_; }

 private:
  DIR* dir_ = nullptr;
};

// ---- little-endian 编解码 ----
// 逐字节拼，既不依赖主机字节序，也不依赖结构体布局。
//
// 每个字段单独写一次，看起来啰嗦，但这正是格式稳定的原因：结构体一旦被
// 编译器插入 padding，或者被换个字节序的机器读，字段就会整片错位。
// 读侧同样逐字段还原，并且每一步都要求缓冲区里确实还有这么多字节。

void AppendU16LE(std::string* out, std::uint16_t value) {
  out->push_back(static_cast<char>(value & 0xffu));
  out->push_back(static_cast<char>((value >> 8) & 0xffu));
}

void AppendU32LE(std::string* out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out->push_back(static_cast<char>((value >> shift) & 0xffu));
  }
}

void AppendU64LE(std::string* out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out->push_back(static_cast<char>((value >> shift) & 0xffu));
  }
}

// 读侧：字段不完整就直接失败，绝不读到一半继续用。
bool ReadU8(const unsigned char* data, std::size_t size, std::size_t* offset,
            std::uint8_t* out) {
  if (*offset + 1 > size) {
    return false;
  }
  *out = data[*offset];
  *offset += 1;
  return true;
}

bool ReadU16LE(const unsigned char* data, std::size_t size, std::size_t* offset,
               std::uint16_t* out) {
  if (*offset + 2 > size) {
    return false;
  }
  std::uint16_t value = 0;
  for (int i = 0; i < 2; ++i) {
    value |= static_cast<std::uint16_t>(data[*offset + i]) << (8 * i);
  }
  *offset += 2;
  *out = value;
  return true;
}

bool ReadU32LE(const unsigned char* data, std::size_t size, std::size_t* offset,
               std::uint32_t* out) {
  if (*offset + 4 > size) {
    return false;
  }
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(data[*offset + i]) << (8 * i);
  }
  *offset += 4;
  *out = value;
  return true;
}

bool ReadU64LE(const unsigned char* data, std::size_t size, std::size_t* offset,
               std::uint64_t* out) {
  if (*offset + 8 > size) {
    return false;
  }
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(data[*offset + i]) << (8 * i);
  }
  *offset += 8;
  *out = value;
  return true;
}

// pread 的薄封装：EINTR 重试，返回实际读到的字节数，出错返回 -1。
ssize_t ReadAt(int fd, void* buffer, std::size_t size, std::uint64_t offset) {
  unsigned char* cursor = static_cast<unsigned char*>(buffer);
  std::size_t remaining = size;
  std::uint64_t position = offset;
  while (remaining > 0) {
    const ssize_t got =
        ::pread(fd, cursor, remaining, static_cast<off_t>(position));
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    if (got == 0) {
      break;
    }
    cursor += got;
    remaining -= static_cast<std::size_t>(got);
    position += static_cast<std::uint64_t>(got);
  }
  return static_cast<ssize_t>(size - remaining);
}

// 归档输出文件：自己维护写入偏移，最后回填 entry_count。
class ArchiveOutput {
 public:
  ~ArchiveOutput() { fd_.Reset(); }

  // O_EXCL：目标已存在就失败，不覆盖、不截断用户已有的文件。
  bool Open(const std::string& path, std::string* error_message) {
    path_ = path;
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
      SetError(error_message,
               Describe(errno, "Failed to create archive file", path));
      return false;
    }
    fd_.Reset(fd);
    return true;
  }

  // 用 pwrite + 自己维护的偏移，而不是 write + lseek：
  // 回填 entry_count 的时候要跳回 offset 16 写 8 个字节，
  // 显式偏移省掉了"改完再挪回来"这种容易写错的状态切换。
  bool Write(const void* data, std::size_t size, std::string* error_message) {
    const unsigned char* cursor = static_cast<const unsigned char*>(data);
    std::size_t remaining = size;
    while (remaining > 0) {
      const ssize_t written =
          ::pwrite(fd_.get(), cursor, remaining, static_cast<off_t>(offset_));
      if (written < 0) {
        if (errno == EINTR) {
          continue;
        }
        SetError(error_message,
                 Describe(errno, "Failed to write archive file", path_));
        return false;
      }
      if (written == 0) {
        SetError(error_message, "Short write to archive file: " + path_);
        return false;
      }
      cursor += written;
      remaining -= static_cast<std::size_t>(written);
      offset_ += static_cast<std::uint64_t>(written);
    }
    return true;
  }

  // 回填 entry_count：写入时先填 0，扫完源目录再 seek 回 offset 16 写真实值。
  bool PatchU64(std::uint64_t offset, std::uint64_t value,
                std::string* error_message) {
    std::string encoded;
    AppendU64LE(&encoded, value);
    const unsigned char* cursor =
        reinterpret_cast<const unsigned char*>(encoded.data());
    std::size_t remaining = encoded.size();
    std::uint64_t position = offset;
    while (remaining > 0) {
      const ssize_t written =
          ::pwrite(fd_.get(), cursor, remaining, static_cast<off_t>(position));
      if (written < 0) {
        if (errno == EINTR) {
          continue;
        }
        SetError(error_message,
                 Describe(errno, "Failed to patch archive header", path_));
        return false;
      }
      if (written == 0) {
        // pwrite 返回 0 时 remaining 永远不会减少，循环就成了死循环。
        // 和 Write() 一样，把这种情况当成明确的失败。
        SetError(error_message, "Short write to archive header: " + path_);
        return false;
      }
      cursor += written;
      remaining -= static_cast<std::size_t>(written);
      position += static_cast<std::uint64_t>(written);
    }
    return true;
  }

  // close 的返回值也要看：出错说明数据可能没落全。
  // 只 close 不检查，会让一个写失败的归档看起来是成功的。
  bool Close(std::string* error_message) {
    if (!fd_.valid()) {
      return true;
    }
    const int fd = fd_.Release();
    if (::close(fd) != 0) {
      SetError(error_message,
               Describe(errno, "Failed to close archive file", path_));
      return false;
    }
    return true;
  }

 private:
  ScopedFd fd_;
  std::string path_;
  std::uint64_t offset_ = 0;
};

// ---- 写侧：把源目录树按顺序写成 entry ----

// 归档要保存的元数据，只包含 v0.1 承诺的那几项。
//
// 不保存 uid / gid：恢复出来的东西属于执行恢复的用户，这是 v0.1 的明确边界，
// 也让归档不携带"谁能读"的额外信息。sticky / setuid / setgid 位同样不进归档：
// 一个从别处拿来的归档，不应该能凭里面的元数据给文件加上提权位。
struct EntryMetadata {
  std::uint32_t mode = 0;
  std::int64_t mtime_sec = 0;
  std::uint32_t mtime_nsec = 0;
};

EntryMetadata MetadataOf(const struct stat& info) {
  EntryMetadata metadata;
  // 只留 0777：setuid / setgid / sticky 和文件类型位都不写进归档。
  metadata.mode = static_cast<std::uint32_t>(info.st_mode) & kPermissionMask;
  metadata.mtime_sec = static_cast<std::int64_t>(info.st_mtim.tv_sec);
  metadata.mtime_nsec = static_cast<std::uint32_t>(info.st_mtim.tv_nsec);
  return metadata;
}

// entry header 的字段顺序与宽度见 docs/format/archive_v0.1.md；
// 顺序写错或漏掉一个字段，读侧就会整片错位。
void BuildEntryHeader(std::uint8_t type, std::uint32_t path_length,
                      const EntryMetadata& metadata, std::uint64_t payload_size,
                      std::string* out) {
  out->clear();
  out->push_back(static_cast<char>(type));
  out->push_back('\0');  // reserved0
  AppendU16LE(out, 0);   // reserved1
  AppendU32LE(out, path_length);
  AppendU32LE(out, metadata.mode);
  AppendU32LE(out, metadata.mtime_nsec);
  // mtime_sec 是有符号的，先按补码转无符号再按 LE 写。
  AppendU64LE(out, static_cast<std::uint64_t>(metadata.mtime_sec));
  AppendU64LE(out, payload_size);

  // 自检：header 必须正好 32 字节。少写或多写一个字段会让读侧整片错位，
  // 而这种错位在写入时是看不出来的，所以在这里当场拦住。
  if (out->size() != kEntryHeaderSize) {
    out->clear();
  }
}

// 全局 header：magic + version + flags + header_size + entry_count。
// entry_count 先写 0 占位，扫完源目录再回填，避免为了数文件先扫两遍
// 或者把整棵树的信息都攒在内存里。
bool WriteGlobalHeader(ArchiveOutput* output, std::string* error_message) {
  std::string header;
  header.append(reinterpret_cast<const char*>(kMagic), sizeof(kMagic));
  AppendU16LE(&header, kFormatVersion);
  AppendU16LE(&header, kFormatFlags);
  AppendU32LE(&header, static_cast<std::uint32_t>(kGlobalHeaderSize));
  AppendU64LE(&header, 0);
  if (header.size() != kGlobalHeaderSize) {
    SetError(error_message, "Internal error: bad global header size");
    return false;
  }
  return output->Write(header.data(), header.size(), error_message);
}

// 写一条 entry 的 header + path，并让 entry_count 加一。
// 路径语法校验写侧也要用，而它的定义在读侧那一段，所以先声明一次。
bool ValidateArchivePath(const std::string& path, bool is_first_entry,
                         std::uint8_t type, std::string* error_message);

bool WriteEntryPrefix(ArchiveOutput* output, std::uint8_t type,
                      const std::string& archive_path,
                      const EntryMetadata& metadata, std::uint64_t payload_size,
                      std::uint64_t* entry_count, std::string* error_message) {
  // 写之前先用读侧那一套规则校验一遍路径，保证"写侧能产出"蕴含"读侧能接受"。
  // 这里的 is_first_entry 用 entry_count 判断：第一条永远是 source root。
  if (!ValidateArchivePath(archive_path, *entry_count == 0, type,
                           error_message)) {
    return false;
  }

  std::string header;
  BuildEntryHeader(type, static_cast<std::uint32_t>(archive_path.size()),
                   metadata, payload_size, &header);
  if (header.size() != kEntryHeaderSize) {
    SetError(error_message, "Internal error: bad entry header size");
    return false;
  }
  if (!output->Write(header.data(), header.size(), error_message)) {
    return false;
  }
  if (!output->Write(archive_path.data(), archive_path.size(), error_message)) {
    return false;
  }
  *entry_count += 1;
  return true;
}

// 普通文件的 payload 必须流式照抄：固定 64 KiB 缓冲，绝不把文件读进内存。
bool WriteFilePayload(ArchiveOutput* output, const std::string& disk_path,
                      const struct stat& initial_info,
                      std::string* error_message) {
  // 以打包开始时那次 stat 为准：读完再和它逐项比（见函数末尾）。
  const std::uint64_t expected_size =
      static_cast<std::uint64_t>(initial_info.st_size);
  const int raw_fd = ::open(disk_path.c_str(), O_RDONLY);
  if (raw_fd < 0) {
    SetError(error_message,
             Describe(errno, "Failed to open source file", disk_path));
    return false;
  }
  ScopedFd source(raw_fd);

  std::vector<unsigned char> buffer(kIoBufferSize);
  std::uint64_t copied = 0;
  while (copied < expected_size) {
    const std::size_t want = static_cast<std::size_t>(
        std::min<std::uint64_t>(kIoBufferSize, expected_size - copied));
    const ssize_t got = ::read(source.get(), buffer.data(), want);
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      SetError(error_message,
               Describe(errno, "Failed to read source file", disk_path));
      return false;
    }
    if (got == 0) {
      // 按初始 stat 的 size 读不够字节：源文件在打包过程中被截短了。
      // 这种归档不能算成功——它里面的 payload_size 和实际内容不一致。
      SetError(error_message, "Source file shrank while packing: " + disk_path);
      return false;
    }
    if (!output->Write(buffer.data(), static_cast<std::size_t>(got),
                       error_message)) {
      return false;
    }
    copied += static_cast<std::uint64_t>(got);
  }

  // 这里有两层保护，缺一层都会产出"看起来成功、其实内容对不上"的归档：
  //   * 读取循环以初始 stat 的 size 为准，读不满就是源文件被截短了；
  //   * 读满之后还要再 fstat 一次，确认文件没有在读取过程中被改写。
  //
  // 再 fstat 一次，和开始时的 stat 逐项比较：size、mtime 秒、mtime 纳秒。
  // 只比 size 是不够的——等长覆盖（原地改几个字节、长度不变）不会改变
  // st_size，但内容和 mtime 都变了，那种归档不能被报告成成功。
  // v0.1 不做快照，只是不假装成功。
  struct stat after;
  if (::fstat(source.get(), &after) != 0) {
    SetError(error_message,
             Describe(errno, "Failed to re-inspect source file", disk_path));
    return false;
  }
  const bool size_changed =
      static_cast<std::uint64_t>(after.st_size) != expected_size;
  const bool mtime_changed =
      after.st_mtim.tv_sec != initial_info.st_mtim.tv_sec ||
      after.st_mtim.tv_nsec != initial_info.st_mtim.tv_nsec;
  if (size_changed || mtime_changed) {
    SetError(error_message, "Source file changed while packing: " + disk_path);
    return false;
  }
  return true;
}

bool WriteDirectoryTree(ArchiveOutput* output,
                        const std::string& disk_directory,
                        const std::string& archive_path, const Filter* filter,
                        std::uint64_t* entry_count, std::string* error_message);

// 写一个普通文件 entry：header + path + 原始 payload。
bool WriteRegularFileEntry(ArchiveOutput* output, const std::string& disk_path,
                           const std::string& archive_path,
                           const struct stat& info, std::uint64_t* entry_count,
                           std::string* error_message) {
  const EntryMetadata metadata = MetadataOf(info);
  const std::uint64_t payload_size = static_cast<std::uint64_t>(info.st_size);
  if (!WriteEntryPrefix(output, kTypeRegularFile, archive_path, metadata,
                        payload_size, entry_count, error_message)) {
    return false;
  }
  return WriteFilePayload(output, disk_path, info, error_message);
}

// 递归写目录：先写目录自己的 entry（这样读侧能拿到目录的 mode / mtime），
// 再按文件名排序写 children。排序让同样的源每次产出同样的归档，便于比对。
bool WriteDirectoryTree(ArchiveOutput* output,
                        const std::string& disk_directory,
                        const std::string& archive_path, const Filter* filter,
                        std::uint64_t* entry_count,
                        std::string* error_message) {
  struct stat info;
  if (lstat(disk_directory.c_str(), &info) != 0) {
    SetError(error_message,
             Describe(errno, "Failed to inspect directory", disk_directory));
    return false;
  }
  if (!S_ISDIR(info.st_mode)) {
    SetError(error_message, "Not a directory: " + disk_directory);
    return false;
  }
  if (!WriteEntryPrefix(output, kTypeDirectory, archive_path, MetadataOf(info),
                        0, entry_count, error_message)) {
    return false;
  }

  DIR* raw_dir = opendir(disk_directory.c_str());
  if (raw_dir == nullptr) {
    SetError(error_message,
             Describe(errno, "Failed to open directory", disk_directory));
    return false;
  }
  ScopedDir dir(raw_dir);

  // readdir 的顺序由文件系统决定，先收名字再排序，输出才稳定。
  std::vector<std::string> names;
  errno = 0;
  while (struct dirent* item = ::readdir(dir.get())) {
    const std::string name = item->d_name;
    if (name == "." || name == "..") {
      continue;
    }
    names.push_back(name);
    errno = 0;
  }
  if (errno != 0) {
    SetError(error_message,
             Describe(errno, "Failed to read directory", disk_directory));
    return false;
  }
  std::sort(names.begin(), names.end());

  for (const std::string& name : names) {
    const std::string child_disk = FileSystem::JoinPath(disk_directory, name);
    // 归档内的路径统一用 '/'，root 的孩子直接就是名字本身。
    const std::string child_archive =
        (archive_path == ".") ? name : archive_path + "/" + name;
    if (child_archive.size() > kMaxPathLength) {
      SetError(error_message, "Archive path too long: " + child_archive);
      return false;
    }

    struct stat child_info;
    // lstat：软链接不会被跟随，会原样暴露成"不支持的条目类型"。
    if (lstat(child_disk.c_str(), &child_info) != 0) {
      SetError(error_message,
               Describe(errno, "Failed to inspect path", child_disk));
      return false;
    }
    // Filter 只看这些元数据，不读文件内容。
    FilterEntry filter_entry;
    filter_entry.archive_path = child_archive;
    filter_entry.name = name;
    filter_entry.is_directory = S_ISDIR(child_info.st_mode) != 0;
    filter_entry.size = S_ISREG(child_info.st_mode)
                            ? static_cast<std::uint64_t>(child_info.st_size)
                            : 0;
    filter_entry.mtime_sec =
        static_cast<std::int64_t>(child_info.st_mtim.tv_sec);

    if (S_ISDIR(child_info.st_mode)) {
      // 命中 exclude 的目录整棵剪掉：不再递归，子树里的特殊文件也不再检查。
      if (filter != nullptr && filter->ShouldPruneDirectory(filter_entry)) {
        continue;
      }
      if (!WriteDirectoryTree(output, child_disk, child_archive, filter,
                              entry_count, error_message)) {
        return false;
      }
    } else if (S_ISREG(child_info.st_mode)) {
      // 普通文件：exclude 优先；有 include 时必须命中至少一条。
      if (filter != nullptr && !filter->ShouldIncludeFile(filter_entry)) {
        continue;
      }
      if (!WriteRegularFileEntry(output, child_disk, child_archive, child_info,
                                 entry_count, error_message)) {
        return false;
      }
    } else {
      // 软链接、FIFO、设备、socket：默认整次失败，不跳过、不跟随、
      // 也不当普通文件复制——那三种做法都会让"备份成功"变成假话。
      // 只有用户明确写了 exclude 才跳过它。
      if (filter != nullptr && filter->ShouldSkipSpecialEntry(filter_entry)) {
        continue;
      }
      SetError(error_message, "Unsupported source entry type: " + child_disk);
      return false;
    }
  }
  return true;
}

// ---- 读侧：先 preflight 校验，再动磁盘 ----

struct ParsedEntry {
  std::string path;
  std::uint8_t type = 0;
  std::uint32_t mode = 0;
  std::int64_t mtime_sec = 0;
  std::uint32_t mtime_nsec = 0;
  std::uint64_t payload_size = 0;
  std::uint64_t payload_offset = 0;
};

// 归档内路径的语法校验：写侧和读侧共用这一份实现。
// v0.1 只接受相对路径，分隔符固定 '/'。
//
// 共用不是"顺手抽象"：Linux 允许文件名里出现反斜杠，也允许 C:note.txt
// 这种形状，而归档格式不接受它们。如果只有读侧检查，写侧就会产出
// "自己刚写的包自己读不回来"的归档——备份工具的失败方式里最不该有这一种。
//
// 这是防 path traversal 的第一道闸，也是整个读侧最要紧的一段判断：
// 恢复时的目标路径是 destination + 归档内路径拼接出来的，只要归档里能出现
// ".."、绝对路径或者空 component，拼接结果就可能跑到 destination 之外。
// 与其在拼接的时候做归一化（那要处理软链接、大小写、符号等价等一堆情况），
// 不如在这里就把这类路径判死：合法的归档根本不需要它们。
bool ValidateArchivePath(const std::string& path, bool is_first_entry,
                         std::uint8_t type, std::string* error_message) {
  if (path.empty()) {
    SetError(error_message, "Invalid archive path: empty path");
    return false;
  }
  if (path.size() > kMaxPathLength) {
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
  // C:\... 这类 Windows 盘符路径不属于 v0.1 的语义，直接拒绝。
  if (path.size() >= 2 && path[1] == ':' &&
      std::isalpha(static_cast<unsigned char>(path[0])) != 0) {
    SetError(error_message, "Invalid archive path (drive letter): " + path);
    return false;
  }
  if (path == ".") {
    // "." 代表 source root 本身，只允许作为第一条 entry，而且必须是目录。
    if (!is_first_entry || type != kTypeDirectory) {
      SetError(error_message, "Invalid root entry in archive");
      return false;
    }
    return true;
  }

  // 逐段检查：空段（foo//bar）、"."（foo/./bar）、".."（../escape）都不允许。
  // 这是防 path traversal 的第一道闸，必须在动磁盘之前跑完。
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

// 阶段一：把整个归档从头到尾校验一遍。任何一条不满足都返回 false，
// 此时调用方还没有创建过 destination 里的任何东西。
//
// 整个函数只做"读 + 判断"，不产生任何写操作，也不按 entry_count 预分配内存：
// 一个声称有 2^64 条 entry 的头，如果拿去 reserve，进程会当场被 OOM 掉。
// 顺序扫描 + 逐条校验，遇到不一致自然会在读到 EOF 时失败。
// 校验策略：只认这一版格式，不认识的取值一律拒绝。
//
// 一个备份文件最危险的失败方式不是"读不出来"，而是"读出来一半、
// 剩下那半靠猜"。所以这里对每个字段都是白名单式的判断：
//   * type 只认 1 和 2；
//   * reserved 字段必须真的为 0，而不是"反正没人用"；
//   * version / flags / header_size 不认识就停；
//   * 读满 entry_count 之后必须正好 EOF，多一个字节都算失败。
// 宽松解析能让更多"坏样本"通过，但代价是用户拿到一个不完整的恢复结果
// 却以为成功了——对备份工具来说这个代价太大。
bool PreflightArchive(int fd, const std::string& archive_path,
                      std::uint64_t file_size,
                      std::vector<ParsedEntry>* entries,
                      std::string* error_message) {
  if (file_size < kGlobalHeaderSize) {
    SetError(error_message, "Truncated archive header: " + archive_path);
    return false;
  }

  unsigned char header[kGlobalHeaderSize];
  if (ReadAt(fd, header, sizeof(header), 0) !=
      static_cast<ssize_t>(sizeof(header))) {
    SetError(error_message, "Truncated archive header: " + archive_path);
    return false;
  }
  if (std::memcmp(header, kMagic, sizeof(kMagic)) != 0) {
    SetError(error_message, "Invalid archive magic: " + archive_path);
    return false;
  }

  std::size_t cursor = sizeof(kMagic);
  std::uint16_t version = 0;
  std::uint16_t flags = 0;
  std::uint32_t header_size = 0;
  std::uint64_t entry_count = 0;
  if (!ReadU16LE(header, sizeof(header), &cursor, &version) ||
      !ReadU16LE(header, sizeof(header), &cursor, &flags) ||
      !ReadU32LE(header, sizeof(header), &cursor, &header_size) ||
      !ReadU64LE(header, sizeof(header), &cursor, &entry_count)) {
    SetError(error_message, "Truncated archive header: " + archive_path);
    return false;
  }
  if (version != kFormatVersion) {
    SetError(error_message, "Unsupported archive version: " +
                                std::to_string(static_cast<int>(version)));
    return false;
  }
  if (flags != kFormatFlags) {
    SetError(error_message, "Unsupported archive flags: " +
                                std::to_string(static_cast<int>(flags)));
    return false;
  }
  if (header_size != kGlobalHeaderSize) {
    SetError(error_message,
             "Invalid archive header size: " + std::to_string(header_size));
    return false;
  }

  // path -> type。既用来查重，也用来确认每条路径的父目录确实是个目录。
  // 有了它，"重复路径"和"文件被当成父目录"这两种结构性错误都能在读 header
  // 的同一趟里查出来，不需要事后再扫一遍。
  std::map<std::string, std::uint8_t> entry_types;
  // 已经是普通文件的路径集合：后面不允许再出现以它为父目录的 entry。
  std::set<std::string> file_paths;

  std::uint64_t position = kGlobalHeaderSize;
  std::vector<unsigned char> entry_header(kEntryHeaderSize);
  for (std::uint64_t index = 0; index < entry_count; ++index) {
    if (position + kEntryHeaderSize > file_size) {
      SetError(error_message,
               "Truncated entry header at entry " + std::to_string(index));
      return false;
    }
    if (ReadAt(fd, entry_header.data(), kEntryHeaderSize, position) !=
        static_cast<ssize_t>(kEntryHeaderSize)) {
      SetError(error_message,
               "Truncated entry header at entry " + std::to_string(index));
      return false;
    }
    position += kEntryHeaderSize;

    std::size_t offset = 0;
    std::uint8_t type = 0;
    std::uint8_t reserved0 = 0;
    std::uint16_t reserved1 = 0;
    std::uint32_t path_length = 0;
    std::uint32_t mode = 0;
    std::uint32_t mtime_nsec = 0;
    std::uint64_t mtime_sec_raw = 0;
    std::uint64_t payload_size = 0;
    if (!ReadU8(entry_header.data(), entry_header.size(), &offset, &type) ||
        !ReadU8(entry_header.data(), entry_header.size(), &offset,
                &reserved0) ||
        !ReadU16LE(entry_header.data(), entry_header.size(), &offset,
                   &reserved1) ||
        !ReadU32LE(entry_header.data(), entry_header.size(), &offset,
                   &path_length) ||
        !ReadU32LE(entry_header.data(), entry_header.size(), &offset, &mode) ||
        !ReadU32LE(entry_header.data(), entry_header.size(), &offset,
                   &mtime_nsec) ||
        !ReadU64LE(entry_header.data(), entry_header.size(), &offset,
                   &mtime_sec_raw) ||
        !ReadU64LE(entry_header.data(), entry_header.size(), &offset,
                   &payload_size)) {
      SetError(error_message,
               "Truncated entry header at entry " + std::to_string(index));
      return false;
    }

    if (type != kTypeDirectory && type != kTypeRegularFile) {
      SetError(error_message,
               "Invalid entry type: " + std::to_string(static_cast<int>(type)));
      return false;
    }
    if (reserved0 != 0 || reserved1 != 0) {
      SetError(error_message,
               "Non-zero reserved field at entry " + std::to_string(index));
      return false;
    }
    if (path_length == 0 || path_length > kMaxPathLength) {
      SetError(error_message,
               "Invalid path length: " + std::to_string(path_length));
      return false;
    }
    if ((mode & ~kPermissionMask) != 0) {
      SetError(error_message, "Invalid entry mode: " + std::to_string(mode));
      return false;
    }
    if (mtime_nsec > kMaxMtimeNsec) {
      SetError(error_message,
               "Invalid mtime nanoseconds: " + std::to_string(mtime_nsec));
      return false;
    }
    if (type == kTypeDirectory && payload_size != 0) {
      SetError(error_message, "Directory entry has a payload at entry " +
                                  std::to_string(index));
      return false;
    }
    if (position + path_length > file_size) {
      SetError(error_message,
               "Truncated entry path at entry " + std::to_string(index));
      return false;
    }

    std::string entry_path(path_length, '\0');
    if (ReadAt(fd, &entry_path[0], path_length, position) !=
        static_cast<ssize_t>(path_length)) {
      SetError(error_message,
               "Truncated entry path at entry " + std::to_string(index));
      return false;
    }
    position += path_length;

    if (!ValidateArchivePath(entry_path, index == 0, type, error_message)) {
      return false;
    }
    if (entry_types.find(entry_path) != entry_types.end()) {
      SetError(error_message, "Duplicate archive path: " + entry_path);
      return false;
    }

    if (entry_path != ".") {
      // 父目录必须已经作为目录出现过：这同时挡住了"先出现文件 a，
      // 再出现 a/b"这种情况，因为 a 的类型会是 regular file。
      const std::size_t slash = entry_path.rfind('/');
      const std::string parent =
          (slash == std::string::npos) ? "." : entry_path.substr(0, slash);
      const auto parent_it = entry_types.find(parent);
      if (parent_it == entry_types.end()) {
        SetError(error_message,
                 "Archive entry has no parent directory: " + entry_path);
        return false;
      }
      if (parent_it->second != kTypeDirectory) {
        // 形如 a 是普通文件、后面又出现 a/b：这种归档结构自相矛盾。
        SetError(error_message,
                 "Archive path is both a file and a directory: " + entry_path);
        return false;
      }
    }
    if (type == kTypeRegularFile) {
      // 反方向再查一次：这条路径已经作为文件存在时，不能有 a/b 这样的
      // 子路径先出现过。lower_bound 找的是第一个 >= "path/" 的路径。
      const auto next = entry_types.lower_bound(entry_path + "/");
      if (next != entry_types.end() &&
          next->first.compare(0, entry_path.size() + 1, entry_path + "/") ==
              0) {
        SetError(error_message,
                 "Archive path is both a file and a directory: " + entry_path);
        return false;
      }
      file_paths.insert(entry_path);
    }
    entry_types[entry_path] = type;

    // payload 必须完整落在文件里；用减法比较，避免 position + size 溢出。
    if (payload_size > file_size - position) {
      SetError(error_message,
               "Payload exceeds archive size for: " + entry_path);
      return false;
    }

    ParsedEntry parsed;
    parsed.path = entry_path;
    parsed.type = type;
    parsed.mode = mode;
    parsed.mtime_sec = static_cast<std::int64_t>(mtime_sec_raw);
    parsed.mtime_nsec = mtime_nsec;
    parsed.payload_size = payload_size;
    parsed.payload_offset = position;
    entries->push_back(parsed);

    position += payload_size;
  }

  // 最后必须刚好落在文件末尾：v0.1 不接受"忽略尾巴"。
  if (position != file_size) {
    SetError(error_message,
             "Trailing bytes after the last archive entry: " + archive_path);
    return false;
  }
  if (entries->empty() || entries->front().path != "." ||
      entries->front().type != kTypeDirectory) {
    SetError(
        error_message,
        "Archive does not start with the source root entry: " + archive_path);
    return false;
  }
  return true;
}

// 恢复一个普通文件：边读归档边写目标，写完再设权限和时间。
//
// O_EXCL 是刻意的：preflight 已经确认过 destination 是空的，
// 万一这里还是撞上同名文件（并发、或者我们自己的判断有漏洞），
// 宁可失败也不要覆盖别人的文件。
// 解包阶段的三条顺序约束，任何一条反过来都会出问题：
//   * 目录先用 0700 建，最后才 chmod 成归档里的 mode；
//   * 文件先写完并 close，再 chmod / utimensat；
//   * 目录的元数据按深度从深到浅恢复，root 最后。
// 反过来做就会遇到"0555 的目录里写不进文件""父目录 mtime 被子项覆盖"这类
// 只在特定输入下才暴露的问题。
bool ExtractRegularFile(int archive_fd, const ParsedEntry& entry,
                        const std::string& disk_path,
                        std::string* error_message) {
  const int raw_fd =
      ::open(disk_path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (raw_fd < 0) {
    SetError(error_message,
             Describe(errno, "Failed to create file", disk_path));
    return false;
  }
  ScopedFd target(raw_fd);

  std::vector<unsigned char> buffer(kIoBufferSize);
  std::uint64_t copied = 0;
  while (copied < entry.payload_size) {
    const std::size_t want = static_cast<std::size_t>(
        std::min<std::uint64_t>(kIoBufferSize, entry.payload_size - copied));
    const ssize_t got =
        ReadAt(archive_fd, buffer.data(), want, entry.payload_offset + copied);
    if (got <= 0) {
      SetError(error_message, "Truncated payload for: " + entry.path);
      return false;
    }
    const unsigned char* cursor = buffer.data();
    std::size_t remaining = static_cast<std::size_t>(got);
    while (remaining > 0) {
      const ssize_t written = ::write(target.get(), cursor, remaining);
      if (written < 0) {
        if (errno == EINTR) {
          continue;
        }
        SetError(error_message,
                 Describe(errno, "Failed to write file", disk_path));
        return false;
      }
      if (written == 0) {
        SetError(error_message, "Short write to file: " + disk_path);
        return false;
      }
      cursor += written;
      remaining -= static_cast<std::size_t>(written);
    }
    copied += static_cast<std::uint64_t>(got);
  }

  const int fd = target.Release();
  if (::close(fd) != 0) {
    SetError(error_message, Describe(errno, "Failed to close file", disk_path));
    return false;
  }
  return true;
}

// 恢复 mode 与 mtime。文件在内容写完、close 之后再调用；
// 目录要等所有子项都建完，否则子项创建会把 mtime 又改掉。
bool ApplyMetadata(const std::string& disk_path, const ParsedEntry& entry,
                   std::string* error_message) {
  if (::chmod(disk_path.c_str(), static_cast<mode_t>(entry.mode)) != 0) {
    SetError(error_message, Describe(errno, "Failed to set mode", disk_path));
    return false;
  }
  struct timespec times[2];
  // v0.1 不保存 atime，所以不去动它：UTIME_OMIT 让内核保持原值。
  times[0].tv_sec = 0;
  times[0].tv_nsec = UTIME_OMIT;
  times[1].tv_sec = static_cast<time_t>(entry.mtime_sec);
  times[1].tv_nsec = static_cast<long>(entry.mtime_nsec);
  if (::utimensat(AT_FDCWD, disk_path.c_str(), times, 0) != 0) {
    SetError(error_message, Describe(errno, "Failed to set mtime", disk_path));
    return false;
  }
  return true;
}

// 路径深度，只用于决定目录 metadata 的恢复顺序："." 是 0 层。
//
// 深的先恢复、浅的后恢复：写了子文件会更新父目录的 mtime，
// 所以父目录必须等所有后代都安顿好之后再盖上自己的时间戳。
// root "." 深度最小，自然排在最后。
std::size_t ArchivePathDepth(const std::string& path) {
  if (path == ".") {
    return 0;
  }
  return static_cast<std::size_t>(std::count(path.begin(), path.end(), '/')) +
         1;
}

}  // namespace

// ---- 公开接口 ----

bool ArchiveWriter::Write(const std::string& source_directory,
                          const std::string& archive_file,
                          std::string* error_message) const {
  // 没有筛选规则：等价于一个空 Filter，行为与 PR #8 完全一致。
  return Write(source_directory, archive_file, nullptr, error_message);
}

bool ArchiveWriter::Write(const std::string& source_directory,
                          const std::string& archive_file, const Filter* filter,
                          std::string* error_message) const {
  if (error_message != nullptr) {
    error_message->clear();
  }
  if (source_directory.empty() || archive_file.empty()) {
    SetError(error_message,
             "Source directory and archive file must both be provided.");
    return false;
  }

  // 源必须是已存在的目录。lstat 不跟随软链接：给一个指向目录的软链接
  // 也会被当成"不是目录"，不会悄悄跟进去。
  struct stat source_info;
  if (lstat(source_directory.c_str(), &source_info) != 0) {
    SetError(error_message,
             Describe(errno, "Failed to inspect source directory",
                      source_directory));
    return false;
  }
  if (!S_ISDIR(source_info.st_mode)) {
    SetError(error_message, "Source is not a directory: " + source_directory);
    return false;
  }

  // 拓扑检查放在存在性检查之前：归档文件等于源目录时，"它已经存在"
  // 和"它就是源目录"同时成立，而后者才是用户真正需要知道的原因。
  // 这一步同样在创建任何文件之前完成，非法拓扑保证 0 文件系统改动。
  FileSystem file_system;
  if (!file_system.IsDestinationOutsideSource(source_directory, archive_file,
                                              error_message)) {
    return false;
  }

  // 归档文件必须还不存在：v0.1 不覆盖、不截断用户已有的文件。
  struct stat target_info;
  if (lstat(archive_file.c_str(), &target_info) == 0) {
    SetError(error_message, "Archive file already exists: " + archive_file);
    return false;
  }
  if (errno != ENOENT) {
    SetError(error_message,
             Describe(errno, "Failed to inspect archive file", archive_file));
    return false;
  }

  // 归档文件所在目录不存在就补建（mkdir -p 语义）：用户写
  // /backups/2024/x.bak 时，不该因为中间目录没建而失败。这一步排在
  // 所有检查之后，所以被拒绝的拓扑仍然不会留下任何目录。
  const std::filesystem::path archive_parent =
      std::filesystem::path(archive_file).parent_path();
  if (!archive_parent.empty() &&
      !file_system.MakeDirectories(archive_parent.string(), error_message)) {
    return false;
  }

  ArchiveOutput output;
  if (!output.Open(archive_file, error_message)) {
    return false;
  }

  bool ok = WriteGlobalHeader(&output, error_message);
  std::uint64_t entry_count = 0;
  if (ok) {
    // 第一条永远是 "."：源目录本身的 mode / mtime 也进归档。
    // 源目录根条目 "." 永远保留：即使规则把内容全过滤掉，归档仍然是一个
    // 合法归档（只有根目录），恢复出来就是空目录。
    ok = WriteDirectoryTree(&output, source_directory, ".", filter,
                            &entry_count, error_message);
  }
  if (ok) {
    ok = output.PatchU64(kEntryCountOffset, entry_count, error_message);
  }
  if (ok) {
    ok = output.Close(error_message);
  }
  if (!ok) {
    output.Close(nullptr);
    // 任何一步失败都要把已写出的半个文件删掉：用户看到目录里有个 .bak，
    // 会默认它是可用的备份。留一个坏文件比什么都不留更危险。
    // 失败就删掉半成品：宁可没有归档文件，也不要留下一个"看起来像备份"
    // 的残文件让用户误以为成功。
    ::unlink(archive_file.c_str());
    return false;
  }
  return true;
}

bool ArchiveReader::Extract(const std::string& archive_file,
                            const std::string& destination_directory,
                            std::string* error_message) const {
  if (error_message != nullptr) {
    error_message->clear();
  }
  if (archive_file.empty() || destination_directory.empty()) {
    SetError(error_message,
             "Archive file and destination directory must both be provided.");
    return false;
  }

  // 归档必须是普通文件：目录、软链接、设备都不接受。
  struct stat archive_info;
  if (lstat(archive_file.c_str(), &archive_info) != 0) {
    SetError(error_message,
             Describe(errno, "Failed to inspect archive file", archive_file));
    return false;
  }
  if (!S_ISREG(archive_info.st_mode)) {
    SetError(error_message, "Archive is not a regular file: " + archive_file);
    return false;
  }

  const int raw_fd = ::open(archive_file.c_str(), O_RDONLY);
  if (raw_fd < 0) {
    SetError(error_message,
             Describe(errno, "Failed to open archive file", archive_file));
    return false;
  }
  ScopedFd archive_fd(raw_fd);

  // ---- 阶段一：preflight ----
  // 这一步只看不写。坏归档在这里就被拦下，destination 不会留下任何东西。
  std::vector<ParsedEntry> entries;
  if (!PreflightArchive(archive_fd.get(), archive_file,
                        static_cast<std::uint64_t>(archive_info.st_size),
                        &entries, error_message)) {
    return false;
  }

  // 目标只允许"不存在"或者"存在但是空目录"。
  FileSystem file_system;
  if (!file_system.IsMissingOrEmptyDirectory(destination_directory,
                                             error_message)) {
    if (error_message == nullptr || error_message->empty()) {
      SetError(error_message,
               "Destination directory already exists and is not empty: " +
                   destination_directory);
    }
    return false;
  }

  // ---- 阶段二：按 entry 顺序落盘 ----
  // 走到这里说明归档结构已经全部验证通过，剩下的失败只可能来自磁盘：
  // 空间不足、权限不够、路径被外部改动。这类失败会留下已完成的部分，
  // 不会回滚——v0.1 不做事务，但错误信息里会写清是哪一步、哪个路径。
  if (!file_system.MakeDirectories(destination_directory, error_message)) {
    return false;
  }
  if (::chmod(destination_directory.c_str(), kStagingDirectoryMode) != 0) {
    SetError(error_message,
             Describe(errno, "Failed to prepare destination directory",
                      destination_directory));
    return false;
  }

  std::vector<std::size_t> directory_indexes;
  // 跳过 index 0：那就是 destination 自己，已经建好了。
  for (std::size_t index = 1; index < entries.size(); ++index) {
    const ParsedEntry& entry = entries[index];
    const std::string disk_path =
        FileSystem::JoinPath(destination_directory, entry.path);
    if (entry.type == kTypeDirectory) {
      // 先建成可写目录：归档里可能是 0555，先设成 0555 后面的孩子就建不进去。
      if (::mkdir(disk_path.c_str(), kStagingDirectoryMode) != 0) {
        SetError(error_message,
                 Describe(errno, "Failed to create directory", disk_path));
        return false;
      }
      directory_indexes.push_back(index);
    } else {
      if (!ExtractRegularFile(archive_fd.get(), entry, disk_path,
                              error_message)) {
        return false;
      }
      // 文件的内容写完、fd 关掉之后再恢复 mode / mtime：
      // 之后再没有人会写这个文件，元数据不会被覆盖掉。
      if (!ApplyMetadata(disk_path, entry, error_message)) {
        return false;
      }
    }
  }

  // 目录的 mode / mtime 必须最后恢复，且从深到浅：创建子项会改父目录 mtime。
  // root "." 深度最小，自然排在最后。
  directory_indexes.push_back(0);
  std::sort(directory_indexes.begin(), directory_indexes.end(),
            [&entries](std::size_t left, std::size_t right) {
              return ArchivePathDepth(entries[left].path) >
                     ArchivePathDepth(entries[right].path);
            });
  for (std::size_t index : directory_indexes) {
    const ParsedEntry& entry = entries[index];
    const std::string disk_path =
        (entry.path == ".")
            ? destination_directory
            : FileSystem::JoinPath(destination_directory, entry.path);
    if (!ApplyMetadata(disk_path, entry, error_message)) {
      return false;
    }
  }
  return true;
}

}  // namespace backupproject
