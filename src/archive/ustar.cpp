// ustar.cpp
//
// 标准 USTAR（POSIX tar）的编解码与读写实现。格式布局见 include/ustar.h。
//
// 代码按三层组织，按这个顺序读最省力：
//
//   1. 字段层：八进制数字字段、定长字符字段、checksum；
//   2. 条目层：ArchiveEntry <-> Header 的映射、路径拆分与校验；
//   3. I/O 层：两个写入器、Scan、ExtractData。
//
// EncodeHeader 与 DecodeHeader 共用同一组字段规则（ParseOctal、ComputeChecksum
// 都是同一份代码），所以"我们自己写出去的东西一定读得回来"是结构上成立的，
// 而不是靠读写两边小心翼翼地互相对齐。
//
// 几个刻意的选择，值得单独说：
//
//   * 数字字段必须有终止符。字段被八进制数字填满而没有 NUL / 空格时判失败，
//     因为那通常说明写入方对字段宽度的理解和我们的不一致，继续猜只会把
//     size 读歪；
//   * devmajor / devminor 允许整字段全 NUL：GNU tar 对非设备条目就是这么写的，
//     拒绝它等于拒绝所有 GNU 归档；
//   * 负 mtime 明确失败。ustar 的 mtime 是 11 位八进制，没有符号位，base-256
//     是 GNU 扩展——与其写一个自己也解释不了的值，不如让调用方知道存不下；
//   * 读侧容忍 GNU 的 "./" 前缀和目录名结尾的 '/'（那是同一个路径的两种写法），
//     其余路径规则一条不让。

#include "ustar.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace backupproject {
namespace ustar {

namespace {

// ---- header 内的字段偏移 ----
// 直接对应 POSIX ustar 布局：name[100] mode[8] uid[8] gid[8] size[12]
// mtime[12] chksum[8] typeflag[1] linkname[100] magic[6] version[2]
// uname[32] gname[32] devmajor[8] devminor[8] prefix[155] pad[12]，合计 512。
constexpr std::size_t kModeOffset = 100;
constexpr std::size_t kUidOffset = 108;
constexpr std::size_t kGidOffset = 116;
constexpr std::size_t kSizeOffset = 124;
constexpr std::size_t kMtimeOffset = 136;
constexpr std::size_t kChecksumOffset = 148;
constexpr std::size_t kTypeFlagOffset = 156;
constexpr std::size_t kLinkNameOffset = 157;
constexpr std::size_t kMagicOffset = 257;
constexpr std::size_t kVersionOffset = 263;
constexpr std::size_t kUnameOffset = 265;
constexpr std::size_t kGnameOffset = 297;
constexpr std::size_t kDevMajorOffset = 329;
constexpr std::size_t kDevMinorOffset = 337;
constexpr std::size_t kPrefixOffset = 345;

constexpr char kMagic[6] = {'u', 's', 't', 'a', 'r', '\0'};
constexpr char kVersion[2] = {'0', '0'};

// uname / gname 字段 32 字节，但要留一个 NUL 终止符，所以最多写 31 字节。
constexpr std::size_t kOwnerNameLength = 31;

// 权限位上限：mode 字段只放 07777（rwx + setuid + setgid + sticky）。
constexpr std::uint32_t kMaxMode = 07777u;

// 两个写入器的缓冲参数。baseline 是"小而直白"的那一个：64 KiB 缓冲 + 逐
// entry flush；Fast 用 1 MiB 聚合缓冲，并保证每次 read 至少 256 KiB。
constexpr std::size_t kBaselineBufferSize = 64 * 1024;
constexpr std::size_t kBaselineReadChunk = 64 * 1024;
constexpr std::size_t kFastBufferSize = 1024 * 1024;
constexpr std::size_t kFastReadChunk = 256 * 1024;
constexpr std::size_t kExtractChunkSize = 64 * 1024;
constexpr std::size_t kTrailingCheckChunk = 64 * 1024;

void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) {
    *error_message = text;
  }
}

// 统一错误文案：动作 + 路径 + errno 说明，三个要素都带上。
std::string DescribeErrno(int error_number, const std::string& action,
                          const std::string& path) {
  return action + ": " + path + ": " + std::strerror(error_number);
}

// 给已有的错误补上下文（block 偏移、条目路径），失败原因本身留在最前面。
void AddContext(std::string* error_message, const std::string& context) {
  if (error_message != nullptr) {
    *error_message += " [" + context + "]";
  }
}

// 删除写坏的半成品。删不掉也改变不了"这次失败"的结论，所以这里只尽力而为：
// 调用方手上已经有一个更重要的错误要返回。
void RemoveFile(const std::string& path) {
  if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
    // 故意不覆盖调用方的错误信息。
  }
}

// fd 的 RAII：任何提前 return 都会关闭。
class ScopedFd {
 public:
  ScopedFd() = default;
  explicit ScopedFd(int fd) : fd_(fd) {}
  ~ScopedFd() { Reset(); }

  ScopedFd(const ScopedFd&) = delete;
  ScopedFd& operator=(const ScopedFd&) = delete;

  int get() const { return fd_; }
  bool valid() const { return fd_ >= 0; }

  void Reset(int fd = -1) {
    if (fd_ >= 0) {
      (void)::close(fd_);
    }
    fd_ = fd;
  }

  // 显式 close 并检查返回值：写侧最后一步的错误（EIO / ENOSPC）只在这里出现。
  bool Close(const std::string& path, std::string* error_message) {
    if (fd_ < 0) {
      return true;
    }
    const int fd = fd_;
    fd_ = -1;
    if (::close(fd) != 0) {
      SetError(error_message, DescribeErrno(errno, "关闭文件失败", path));
      return false;
    }
    return true;
  }

 private:
  int fd_ = -1;
};

// ---- 八进制数字字段 ----

// 8 的 digits 次方减一：digits 位八进制能表示的最大值。
std::uint64_t OctalLimit(std::size_t digits) {
  std::uint64_t limit = 1;
  for (std::size_t i = 0; i < digits; ++i) {
    limit *= 8;
  }
  return limit - 1;
}

// 写数字字段：field_size - 1 位八进制 + 1 个 NUL。
// 放不下就失败，不截断——把 size 写小只会让归档说谎。
bool AppendOctal(char* block, std::size_t offset, std::size_t field_size,
                 std::uint64_t value, const char* field_name,
                 std::string* error_message) {
  const std::size_t digits = field_size - 1;
  const std::uint64_t limit = OctalLimit(digits);
  if (value > limit) {
    SetError(error_message,
             std::string("ustar header: ") + field_name + " 字段放不下 " +
                 std::to_string(value) + "（上限 " + std::to_string(limit) +
                 "，格式是 " + std::to_string(digits) + " 位八进制 + NUL）");
    return false;
  }
  for (std::size_t i = 0; i < digits; ++i) {
    const std::size_t shift = (digits - 1 - i) * 3;
    block[offset + i] = static_cast<char>('0' + ((value >> shift) & 7u));
  }
  block[offset + digits] = '\0';
  return true;
}

// 解析数字字段。容忍前导空格与末尾的 NUL / 空格（GNU tar 写 "0000644\0"，
// 也有实现写 "0000644 "），但**必须**有终止符：字段被数字填满而没有 NUL /
// 空格时判失败。allow_empty 只给 devmajor / devminor 用——GNU tar 对非设备
// 条目写的就是 8 个 NUL，那不是"空字段"，是"没有这个字段"。
bool ParseOctal(const char* field, std::size_t field_size,
                const char* field_name, bool allow_empty, std::uint64_t* out,
                std::string* error_message) {
  std::size_t index = 0;
  while (index < field_size && field[index] == ' ') {
    ++index;
  }
  std::uint64_t value = 0;
  std::size_t digits = 0;
  while (index < field_size && field[index] >= '0' && field[index] <= '7') {
    value = value * 8 + static_cast<std::uint64_t>(field[index] - '0');
    ++index;
    ++digits;
  }
  if (digits == 0) {
    bool blank = true;
    for (std::size_t i = index; i < field_size; ++i) {
      if (field[i] != '\0' && field[i] != ' ') {
        blank = false;
        break;
      }
    }
    if (allow_empty && blank) {
      *out = 0;
      return true;
    }
    SetError(error_message,
             std::string("ustar header: ") + field_name +
                 " 字段是空的（既没有八进制数字，也没有 NUL 终止符）");
    return false;
  }
  if (index == field_size) {
    SetError(error_message, std::string("ustar header: ") + field_name +
                                " 字段被数字填满而没有终止符（必须是 " +
                                std::to_string(field_size - 1) +
                                " 位数字 + NUL）");
    return false;
  }
  for (std::size_t i = index; i < field_size; ++i) {
    if (field[i] == '\0' || field[i] == ' ') {
      continue;
    }
    SetError(error_message,
             std::string("ustar header: ") + field_name +
                 " 字段含非法八进制字符（只允许 0-7、前导/末尾空格与 NUL）");
    return false;
  }
  *out = value;
  return true;
}

// ---- 定长字符字段 ----

// 写字符字段：不超长、不含 NUL、剩余部分补 0。超长时失败而不是截断：
// 名字被截断就是另一个文件了。
bool AppendFixedField(char* block, std::size_t offset, std::size_t field_size,
                      const std::string& value, const char* field_name,
                      std::string* error_message) {
  if (value.find('\0') != std::string::npos) {
    SetError(error_message, std::string("ustar header: ") + field_name +
                                " 含 NUL 字节，ustar 字段里没法表示");
    return false;
  }
  if (value.size() > field_size) {
    SetError(error_message, std::string("ustar header: ") + field_name +
                                " 长 " + std::to_string(value.size()) +
                                " 字节，超过字段容量 " +
                                std::to_string(field_size));
    return false;
  }
  if (!value.empty()) {
    std::memcpy(block + offset, value.data(), value.size());
  }
  std::memset(block + offset + value.size(), 0, field_size - value.size());
  return true;
}

// 读字符字段。字段可以正好被填满（POSIX 允许 100 字节的 name 不带 NUL），
// 但不能在第一个 NUL 之后还有非 NUL 字节：那说明有人想把第二个名字塞进
// 同一个字段里（tar 世界里的经典 NUL 注入）。
bool DecodeFixedField(const char* field, std::size_t field_size,
                      const char* field_name, std::string* out,
                      std::string* error_message) {
  std::size_t length = 0;
  while (length < field_size && field[length] != '\0') {
    ++length;
  }
  for (std::size_t i = length; i < field_size; ++i) {
    if (field[i] != '\0') {
      SetError(error_message,
               std::string("ustar header: ") + field_name +
                   " 字段在终止 NUL 之后还有非 NUL 字节（NUL 注入）");
      return false;
    }
  }
  out->assign(field, length);
  return true;
}

// ---- checksum ----

// 计算时把 chksum 字段当成 8 个空格，写和读都用这一份。
std::uint64_t ComputeChecksum(const char* block) {
  std::uint64_t sum = 0;
  for (std::size_t i = 0; i < kBlockSize; ++i) {
    const bool in_checksum_field =
        i >= kChecksumOffset && i < kChecksumOffset + 8;
    const unsigned char byte = in_checksum_field
                                   ? static_cast<unsigned char>(' ')
                                   : static_cast<unsigned char>(block[i]);
    sum += byte;
  }
  return sum;
}

// ---- 路径 ----

// 512 对齐的 payload 长度。调用方保证 size <= 077777777777，不会溢出。
std::uint64_t RoundUpToBlock(std::uint64_t size) {
  return ((size + kBlockSize - 1) / kBlockSize) * kBlockSize;
}

std::uint64_t PaddingFor(std::uint64_t size) {
  return RoundUpToBlock(size) - size;
}

// 归档内部路径的语义校验，读写两侧共用：
// 必须是相对路径、'/' 分隔、没有空 component、没有 '.' / '..' component、
// 不以 '/' 结尾、不含 NUL。source root 自身写作 "."。
//
// 这是路径穿越的第一道防线：只要进来的路径过了这一关，后面所有拼接都只会
// 发生在 destination 里面。
bool ValidateRelativePath(const std::string& path, std::string* error_message) {
  if (path.empty()) {
    SetError(error_message, "路径为空");
    return false;
  }
  if (path.find('\0') != std::string::npos) {
    SetError(error_message, "路径含 NUL 字节");
    return false;
  }
  if (path[0] == '/') {
    SetError(error_message, "不接受绝对路径: " + path);
    return false;
  }
  if (path.back() == '/') {
    SetError(error_message, "路径以 '/' 结尾（目录由 typeflag 表达）: " + path);
    return false;
  }
  if (path == ".") {
    // 归档根自身。只允许整体等于 "."，不允许 "a/./b" 这种中间形式。
    return true;
  }
  std::size_t position = 0;
  while (true) {
    const std::size_t slash = path.find('/', position);
    const std::string component = slash == std::string::npos
                                      ? path.substr(position)
                                      : path.substr(position, slash - position);
    if (component.empty()) {
      SetError(error_message, "路径含空 component: " + path);
      return false;
    }
    if (component == ".") {
      SetError(error_message, "路径含 '.' component: " + path);
      return false;
    }
    if (component == "..") {
      SetError(error_message, "路径含 '..' component（路径穿越）: " + path);
      return false;
    }
    if (slash == std::string::npos) {
      break;
    }
    position = slash + 1;
  }
  return true;
}

// 读侧的路径归一化：GNU tar 用 "tar -C dir ." 打包时写出的名字是 "./a/b"，
// 目录条目还带结尾 '/'；这两种写法与 "a/b" 是同一个路径，读的时候归一化掉。
// 归一化之后仍然走 ValidateRelativePath，"a/./b"、"a//b"、"../x" 一条都不放。
bool NormalizeMemberPath(const std::string& raw, std::string* normalized,
                         std::string* error_message) {
  std::string path = raw;
  while (path.size() > 1 && path.back() == '/') {
    path.pop_back();
  }
  // "./" 只允许出现在最前面；"./" 自身归一化成 "."。
  while (path.size() > 2 && path.compare(0, 2, "./") == 0) {
    path.erase(0, 2);
  }
  if (!ValidateRelativePath(path, error_message)) {
    return false;
  }
  *normalized = path;
  return true;
}

// ---- 定位读写 ----

// 从固定偏移读满 length 字节。pread 不改文件偏移，Scan 里反复按偏移定位比
// lseek + read 少一层状态；EINTR 与短读都在这里处理干净，调用方只需要关心
// "读到"还是"读不到"。
bool ReadFullAt(int fd, char* buffer, std::size_t length, std::uint64_t offset,
                const std::string& path, std::string* error_message) {
  std::size_t done = 0;
  while (done < length) {
    const ssize_t got = ::pread(fd, buffer + done, length - done,
                                static_cast<off_t>(offset + done));
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      SetError(error_message, DescribeErrno(errno, "读取失败", path));
      return false;
    }
    if (got == 0) {
      SetError(error_message, "读取失败: " + path + ": 文件在预期的 " +
                                  std::to_string(length) + " 字节之前就结束了");
      return false;
    }
    done += static_cast<std::size_t>(got);
  }
  return true;
}

// 写满 length 字节：partial write 与 EINTR 都要处理，否则大归档在慢盘上会
// 悄悄少写一截。
bool WriteAllFd(int fd, const char* data, std::size_t length,
                const std::string& path, std::string* error_message) {
  std::size_t done = 0;
  while (done < length) {
    const ssize_t written = ::write(fd, data + done, length - done);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      SetError(error_message, DescribeErrno(errno, "写入失败", path));
      return false;
    }
    done += static_cast<std::size_t>(written);
  }
  return true;
}

bool IsZeroBlock(const char* block) {
  for (std::size_t i = 0; i < kBlockSize; ++i) {
    if (block[i] != '\0') {
      return false;
    }
  }
  return true;
}

// 两个全零 block 之后只允许出现 0 字节。GNU tar 会把归档补到 10240 字节的
// 整数倍，那是记录对齐的填充，必须容忍；但只要出现一个非零字节就判失败——
// 写入方在结尾标记之后还塞了东西，我们不会假装没看见。
bool CheckTrailingZeros(int fd, std::uint64_t offset, std::uint64_t file_size,
                        const std::string& path, std::string* error_message) {
  std::vector<char> buffer(kTrailingCheckChunk);
  std::uint64_t position = offset;
  while (position < file_size) {
    const std::size_t want = static_cast<std::size_t>(
        std::min<std::uint64_t>(file_size - position, buffer.size()));
    if (!ReadFullAt(fd, buffer.data(), want, position, path, error_message)) {
      return false;
    }
    for (std::size_t i = 0; i < want; ++i) {
      if (buffer[i] != '\0') {
        SetError(error_message,
                 "归档在结尾标记（两个全零 block）之后还有非零字节，偏移 " +
                     std::to_string(position + i));
        return false;
      }
    }
    position += want;
  }
  return true;
}

// 打开归档并确认它是普通文件：目录 / FIFO / 设备读起来会阻塞或者给出无穷
// 无尽的"数据"，preflight 必须在这之前就把它们挡掉。
bool OpenArchiveForRead(const std::string& archive_file, ScopedFd* fd,
                        std::uint64_t* file_size, std::string* error_message) {
  fd->Reset(::open(archive_file.c_str(), O_RDONLY | O_CLOEXEC));
  if (!fd->valid()) {
    SetError(error_message, DescribeErrno(errno, "打开归档失败", archive_file));
    return false;
  }
  struct stat status;
  if (::fstat(fd->get(), &status) != 0) {
    SetError(error_message,
             DescribeErrno(errno, "读取归档属性失败", archive_file));
    return false;
  }
  if (!S_ISREG(status.st_mode)) {
    SetError(error_message, "归档不是普通文件: " + archive_file);
    return false;
  }
  *file_size = static_cast<std::uint64_t>(status.st_size);
  return true;
}

// Scan 之外的第二次边界检查。Member 可能来自调用方手工拼装，也可能在 Scan
// 之后归档文件被换过，所以 ExtractData 不省这一步。
bool CheckMemberBounds(const Member& member, std::uint64_t file_size,
                       std::string* error_message) {
  if (member.data_offset % kBlockSize != 0) {
    SetError(error_message,
             "payload 偏移 " + std::to_string(member.data_offset) +
                 " 不是 512 的倍数: " + member.entry.archive_path);
    return false;
  }
  if (member.data_offset > file_size ||
      member.data_size > file_size - member.data_offset) {
    SetError(error_message, "payload 越界（偏移 " +
                                std::to_string(member.data_offset) + "，长度 " +
                                std::to_string(member.data_size) + "，归档 " +
                                std::to_string(file_size) +
                                " 字节）: " + member.entry.archive_path);
    return false;
  }
  return true;
}

// ---- 写入器共用的部件 ----
//
// 两个写入器的格式逻辑必须只有一份：header 编码、路径拆分、checksum、typeflag
// 映射都在下面的 BuildHeader / PrepareEntry 里，WriteBaseline 与 WriteFast
// 只决定"字节怎么送出去"。

// 输出归档文件：O_EXCL 创建（绝不覆盖已有文件），失败或提前返回时把自己
// 创建的那个半成品删掉——备份工具不能在磁盘上留下"看起来成功了"的残file。
class OutputArchive {
 public:
  OutputArchive() = default;
  ~OutputArchive() {
    if (fd_ >= 0) {
      (void)::close(fd_);
    }
    if (owns_file_ && !finished_) {
      RemoveFile(path_);
    }
  }

  OutputArchive(const OutputArchive&) = delete;
  OutputArchive& operator=(const OutputArchive&) = delete;

  bool Create(const std::string& path, std::string* error_message) {
    // O_EXCL：已存在的文件一律不覆盖。备份文件被静默覆盖是最不能接受的失败模式。
    fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd_ < 0) {
      SetError(error_message,
               DescribeErrno(errno, "创建归档失败（不覆盖已有文件）", path));
      return false;
    }
    path_ = path;
    owns_file_ = true;
    return true;
  }

  bool WriteAll(const char* data, std::size_t length,
                std::string* error_message) {
    return WriteAllFd(fd_, data, length, path_, error_message);
  }

  // 收尾：close 的返回值必须检查，写侧的错误可能只在这里出现；close 失败时
  // 归档不算写成，析构会把半成品删掉。
  bool Finish(std::string* error_message) {
    if (fd_ < 0) {
      return true;
    }
    const int fd = fd_;
    fd_ = -1;
    if (::close(fd) != 0) {
      SetError(error_message, DescribeErrno(errno, "关闭归档失败", path_));
      return false;
    }
    finished_ = true;
    return true;
  }

 private:
  int fd_ = -1;
  bool owns_file_ = false;
  bool finished_ = false;
  std::string path_;
};

// 定长输出缓冲。两个写入器的真实差别不在"有没有缓冲"，而在缓冲多大、
// 什么时候 flush、payload 是读进临时数组再拷，还是直接在缓冲的空闲区里读。
class OutputBuffer {
 public:
  OutputBuffer(OutputArchive* output, std::size_t capacity)
      : output_(output), data_(capacity) {}

  OutputBuffer(const OutputBuffer&) = delete;
  OutputBuffer& operator=(const OutputBuffer&) = delete;

  bool Append(const char* bytes, std::size_t length,
              std::string* error_message) {
    std::size_t copied = 0;
    while (copied < length) {
      if (used_ == data_.size() && !Flush(error_message)) {
        return false;
      }
      const std::size_t chunk = std::min(length - copied, data_.size() - used_);
      std::memcpy(data_.data() + used_, bytes + copied, chunk);
      used_ += chunk;
      copied += chunk;
    }
    return true;
  }

  // padding 与结尾的两个全零 block。
  bool AppendZeros(std::size_t length, std::string* error_message) {
    static const char kZeros[kBlockSize] = {};
    while (length > 0) {
      const std::size_t chunk = std::min(length, sizeof(kZeros));
      if (!Append(kZeros, chunk, error_message)) {
        return false;
      }
      length -= chunk;
    }
    return true;
  }

  std::size_t FreeSpace() const { return data_.size() - used_; }
  char* FreeData() { return data_.data() + used_; }
  void Commit(std::size_t length) { used_ += length; }

  bool Flush(std::string* error_message) {
    if (used_ == 0) {
      return true;
    }
    if (!output_->WriteAll(data_.data(), used_, error_message)) {
      return false;
    }
    used_ = 0;
    return true;
  }

 private:
  OutputArchive* output_;
  std::vector<char> data_;
  std::size_t used_ = 0;
};

// 只读打开源文件。O_NOFOLLOW 保证不会 follow 软链接：扫描层已经把软链接标成
// kSymlink，这里再跟着链接走就等于备份了链接指向的东西，而不是条目本身。
bool OpenSourceFile(const ArchiveEntry& entry, ScopedFd* source,
                    std::string* error_message) {
  const int fd =
      ::open(entry.source_path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    SetError(error_message,
             DescribeErrno(errno, "打开源文件失败", entry.source_path) +
                 "（条目 " + entry.archive_path + "）");
    return false;
  }
  source->Reset(fd);
  return true;
}

// ArchiveEntry -> Header。两个写入器共用这一份映射，格式不可能各自漂移。
bool BuildHeader(const ArchiveEntry& entry, Header* header,
                 std::string* error_message) {
  if (entry.type == EntryType::kSocket) {
    // socket 只存在于扫描层：归档格式里没有它，写进去就是撒谎。
    SetError(error_message, std::string("unsupported special type: socket: ") +
                                entry.archive_path);
    return false;
  }
  std::string name;
  std::string prefix;
  if (!SplitPath(entry.archive_path, &name, &prefix, error_message)) {
    return false;
  }
  header->typeflag = TypeFlagFor(entry.type);
  header->name = name;
  header->prefix = prefix;
  header->mode = entry.mode;
  header->uid = entry.uid;
  header->gid = entry.gid;
  header->mtime = entry.mtime_sec;
  // 目录 / FIFO / 设备 / 软链接 / 硬链接没有 payload，size 字段一律写 0。
  header->size = entry.type == EntryType::kRegularFile ? entry.size : 0;
  header->linkname.clear();
  if (entry.type == EntryType::kSymlink || entry.type == EntryType::kHardLink) {
    if (entry.link_target.empty()) {
      SetError(error_message,
               std::string("条目缺少链接目标（link_target 为空）: ") +
                   entry.archive_path);
      return false;
    }
    header->linkname = entry.link_target;
  }
  header->devmajor = entry.dev_major;
  header->devminor = entry.dev_minor;
  header->uname = entry.user_name;
  header->gname = entry.group_name;
  return true;
}

// 把一个条目编码成 512 字节 header。错误信息带上条目路径，否则"uid 放不下"
// 这类报错不知道说的是哪一条。
bool PrepareEntry(const ArchiveEntry& entry, std::string* block,
                  std::string* error_message) {
  Header header;
  if (!BuildHeader(entry, &header, error_message)) {
    return false;
  }
  if (!EncodeHeader(header, block, error_message)) {
    AddContext(error_message, entry.archive_path);
    return false;
  }
  return true;
}

// 不碰磁盘的预校验：任何一条 metadata 在 ustar 里放不下（路径太长、uid 超范围、
// 负 mtime、socket、linkname 超长）都在创建归档文件之前失败，磁盘上不留痕迹。
// 两个写入器都先跑这一遍，代价是每条 512 字节的编码，换来的是"格式错误永远
// 发生在 open(O_EXCL) 之前"。
bool ValidateEntries(const std::vector<ArchiveEntry>& entries,
                     std::string* error_message) {
  std::string block;
  for (const ArchiveEntry& entry : entries) {
    if (!PrepareEntry(entry, &block, error_message)) {
      return false;
    }
  }
  return true;
}

}  // namespace

// ---- 公开 API ----

bool SplitPath(const std::string& archive_path, std::string* name,
               std::string* prefix, std::string* error_message) {
  if (name == nullptr || prefix == nullptr) {
    SetError(error_message, "SplitPath: 输出参数是空指针");
    return false;
  }
  name->clear();
  prefix->clear();
  if (archive_path.size() > kMaxPathSize) {
    SetError(
        error_message,
        "路径长 " + std::to_string(archive_path.size()) +
            " 字节，超过 ustar 的 255 字节上限（name 100 + prefix 155）: " +
            archive_path);
    return false;
  }
  if (!ValidateRelativePath(archive_path, error_message)) {
    return false;
  }
  if (archive_path.size() <= kNameSize) {
    // 常见情况：整条路径进 name，prefix 留空。
    *name = archive_path;
    return true;
  }
  // 超过 100 字节就必须切。切点 i 要同时满足：
  //   prefix = path[0, i)  <= 155，name = path[i+1, end) <= 100
  // 即 i <= 155 且 i >= len - 101。从大到小找第一个 '/'，prefix 里放尽可能多
  // 的目录、name 只留最后一段——这也是 GNU tar 的取向，同一个路径在不同实现
  // 之间更容易得到同样的拆分。
  const std::size_t length = archive_path.size();
  const std::size_t lowest =
      length > kNameSize + 1 ? length - kNameSize - 1 : 0;
  const std::size_t highest = std::min<std::size_t>(kPrefixSize, length - 1);
  std::size_t index = highest;
  while (true) {
    if (archive_path[index] == '/') {
      *prefix = archive_path.substr(0, index);
      *name = archive_path.substr(index + 1);
      return true;
    }
    if (index == lowest) {
      break;
    }
    --index;
  }
  SetError(
      error_message,
      "路径长 " + std::to_string(length) +
          " 字节，但没有任何 '/' 能把它切成 prefix(<=155) + name(<=100): " +
          archive_path);
  return false;
}

char TypeFlagFor(EntryType type) {
  switch (type) {
    case EntryType::kRegularFile:
      return '0';
    case EntryType::kHardLink:
      return '1';
    case EntryType::kSymlink:
      return '2';
    case EntryType::kCharDevice:
      return '3';
    case EntryType::kBlockDevice:
      return '4';
    case EntryType::kDirectory:
      return '5';
    case EntryType::kFifo:
      return '6';
    case EntryType::kSocket:
      // ustar 里没有 socket 条目。GNU tar 用 's' 表示，我们在读侧认出它并明确
      // 拒绝；写侧（BuildHeader）在本函数被调用之前就已经报错了，这里的返回值
      // 只是让映射保持完整、可测试。
      return 's';
  }
  return '\0';
}

bool TypeFromFlag(char typeflag, EntryType* type, std::string* error_message) {
  if (type == nullptr) {
    SetError(error_message, "TypeFromFlag: type 是空指针");
    return false;
  }
  switch (typeflag) {
    case '0':
    case '\0':
      // 早期的 tar 用全 NUL 的 typeflag 表示普通文件，POSIX 要求读侧接受。
      *type = EntryType::kRegularFile;
      return true;
    case '1':
      *type = EntryType::kHardLink;
      return true;
    case '2':
      *type = EntryType::kSymlink;
      return true;
    case '3':
      *type = EntryType::kCharDevice;
      return true;
    case '4':
      *type = EntryType::kBlockDevice;
      return true;
    case '5':
      *type = EntryType::kDirectory;
      return true;
    case '6':
      *type = EntryType::kFifo;
      return true;
    case 's':
      // GNU 的 socket flag。归档格式里没有 socket，认了它就得凭空造一个条目。
      SetError(error_message, "unsupported special type: socket");
      return false;
    case '7':
      SetError(error_message,
               "unsupported typeflag '7'（contiguous file：POSIX "
               "有定义，但我们不写也不认）");
      return false;
    case 'x':
    case 'g':
    case 'L':
    case 'K':
    case 'A':
    case 'D':
    case 'M':
    case 'N':
    case 'S':
    case 'V':
      // pax 扩展头 / GNU 长名字 / sparse / 多卷 / 卷标：都带额外语义，静默跳过
      // 会让我们读到错误的路径和大小。
      SetError(error_message,
               std::string("unsupported GNU/pax extension header (typeflag '") +
                   typeflag + "')");
      return false;
    default: {
      const char* kHexDigits = "0123456789abcdef";
      const unsigned char byte = static_cast<unsigned char>(typeflag);
      std::string text = "unknown typeflag 0x";
      text.push_back(kHexDigits[(byte >> 4) & 0x0fu]);
      text.push_back(kHexDigits[byte & 0x0fu]);
      SetError(error_message, text);
      return false;
    }
  }
}

bool EncodeHeader(const Header& header, std::string* block,
                  std::string* error_message) {
  if (block == nullptr) {
    SetError(error_message, "EncodeHeader: block 是空指针");
    return false;
  }
  // 先过 typeflag 映射：socket 与未知 flag 在这里就被挡住，绝不会写出一个
  // 我们自己都读不回来的 header。
  EntryType type = EntryType::kRegularFile;
  if (!TypeFromFlag(header.typeflag, &type, error_message)) {
    return false;
  }
  if (header.name.empty()) {
    SetError(error_message, "ustar header: name 不能为空");
    return false;
  }
  if (header.name.size() > kNameSize) {
    SetError(error_message,
             "ustar header: name 长 " + std::to_string(header.name.size()) +
                 " 字节，超过 100 字节字段（应该先过 SplitPath）");
    return false;
  }
  if (header.prefix.size() > kPrefixSize) {
    SetError(error_message,
             "ustar header: prefix 长 " + std::to_string(header.prefix.size()) +
                 " 字节，超过 155 字节字段（应该先过 SplitPath）");
    return false;
  }
  if (header.linkname.size() > kLinkNameSize) {
    SetError(error_message, "ustar header: linkname 长 " +
                                std::to_string(header.linkname.size()) +
                                " 字节，超过 100 字节字段");
    return false;
  }
  if (header.mode > kMaxMode) {
    SetError(error_message, "ustar header: mode " +
                                std::to_string(header.mode) +
                                " 超过 07777（权限位 + setuid/setgid/sticky）");
    return false;
  }
  if (type != EntryType::kRegularFile && header.size != 0) {
    SetError(error_message, std::string("ustar header: ") +
                                EntryTypeName(type) +
                                " 条目不能声明 payload（size 必须为 0，当前 " +
                                std::to_string(header.size) + "）");
    return false;
  }
  if (header.mtime < 0) {
    SetError(
        error_message,
        "ustar header: mtime " + std::to_string(header.mtime) +
            " 是 1970 之前的负值，ustar 的 11 位八进制字段没有符号位，存不下");
    return false;
  }
  if ((type == EntryType::kSymlink || type == EntryType::kHardLink) &&
      header.linkname.empty()) {
    SetError(error_message, std::string("ustar header: ") +
                                EntryTypeName(type) +
                                " 条目的 linkname 不能为空");
    return false;
  }

  block->assign(kBlockSize, '\0');
  char* out = block->data();
  if (!AppendFixedField(out, 0, kNameSize, header.name, "name",
                        error_message)) {
    return false;
  }
  if (!AppendOctal(out, kModeOffset, 8, header.mode, "mode", error_message)) {
    return false;
  }
  if (!AppendOctal(out, kUidOffset, 8, header.uid, "uid", error_message)) {
    return false;
  }
  if (!AppendOctal(out, kGidOffset, 8, header.gid, "gid", error_message)) {
    return false;
  }
  if (!AppendOctal(out, kSizeOffset, 12, header.size, "size", error_message)) {
    return false;
  }
  if (!AppendOctal(out, kMtimeOffset, 12,
                   static_cast<std::uint64_t>(header.mtime), "mtime",
                   error_message)) {
    return false;
  }
  out[kTypeFlagOffset] = header.typeflag;
  if (!AppendFixedField(out, kLinkNameOffset, kLinkNameSize, header.linkname,
                        "linkname", error_message)) {
    return false;
  }
  std::memcpy(out + kMagicOffset, kMagic, sizeof(kMagic));
  std::memcpy(out + kVersionOffset, kVersion, sizeof(kVersion));
  // uname / gname 超过 31 字节时按 tar 惯例截断：这两个字段是给人看的提示，
  // 截断不会改变文件身份（uid / gid 才是权威）。注意是按字节截断，不保证
  // UTF-8 边界——ustar 里这两个字段本来就没有编码约定。
  std::string uname = header.uname;
  if (uname.size() > kOwnerNameLength) {
    uname.resize(kOwnerNameLength);
  }
  std::string gname = header.gname;
  if (gname.size() > kOwnerNameLength) {
    gname.resize(kOwnerNameLength);
  }
  if (!AppendFixedField(out, kUnameOffset, 32, uname, "uname", error_message)) {
    return false;
  }
  if (!AppendFixedField(out, kGnameOffset, 32, gname, "gname", error_message)) {
    return false;
  }
  if (!AppendOctal(out, kDevMajorOffset, 8, header.devmajor, "devmajor",
                   error_message)) {
    return false;
  }
  if (!AppendOctal(out, kDevMinorOffset, 8, header.devminor, "devminor",
                   error_message)) {
    return false;
  }
  if (!AppendFixedField(out, kPrefixOffset, kPrefixSize, header.prefix,
                        "prefix", error_message)) {
    return false;
  }
  // checksum 最后算：它覆盖前面所有字段（chksum 自己按 8 个空格参与）。
  // 写法是 6 位八进制 + NUL + 空格，这是 tar 世界事实上的标准形式；
  // 512 字节的字节和最大 130560，6 位八进制足够。
  const std::uint64_t checksum = ComputeChecksum(out);
  for (std::size_t i = 0; i < 6; ++i) {
    out[kChecksumOffset + i] =
        static_cast<char>('0' + ((checksum >> ((5 - i) * 3)) & 7u));
  }
  out[kChecksumOffset + 6] = '\0';
  out[kChecksumOffset + 7] = ' ';
  return true;
}

bool DecodeHeader(const char* block, Header* header,
                  std::string* error_message) {
  if (block == nullptr || header == nullptr) {
    SetError(error_message, "DecodeHeader: 参数是空指针");
    return false;
  }
  if (std::memcmp(block + kMagicOffset, kMagic, sizeof(kMagic)) != 0) {
    // GNU 的老变体是 "ustar  \0"（magic 里带空格、version 是 " \0"）。认了它
    // 就等于认了 GNU 扩展世界（base-256 数值、sparse），这里直接拒绝。
    SetError(error_message, "ustar header: magic 不是 \"ustar\" + NUL");
    return false;
  }
  if (std::memcmp(block + kVersionOffset, kVersion, sizeof(kVersion)) != 0) {
    SetError(error_message, "ustar header: version 不是 \"00\"");
    return false;
  }
  // checksum：字段本身必须是合法八进制且带终止符，重算时把该字段当 8 个空格。
  std::uint64_t stored_checksum = 0;
  if (!ParseOctal(block + kChecksumOffset, 8, "chksum", false, &stored_checksum,
                  error_message)) {
    return false;
  }
  const std::uint64_t computed_checksum = ComputeChecksum(block);
  if (stored_checksum != computed_checksum) {
    SetError(error_message, "ustar header: checksum 不匹配（字段 " +
                                std::to_string(stored_checksum) + "，实际 " +
                                std::to_string(computed_checksum) + "）");
    return false;
  }

  std::uint64_t mode = 0;
  std::uint64_t uid = 0;
  std::uint64_t gid = 0;
  std::uint64_t size = 0;
  std::uint64_t mtime = 0;
  std::uint64_t devmajor = 0;
  std::uint64_t devminor = 0;
  if (!ParseOctal(block + kModeOffset, 8, "mode", false, &mode,
                  error_message)) {
    return false;
  }
  if (mode > kMaxMode) {
    SetError(error_message,
             "ustar header: mode " + std::to_string(mode) + " 超过 07777");
    return false;
  }
  if (!ParseOctal(block + kUidOffset, 8, "uid", false, &uid, error_message)) {
    return false;
  }
  if (!ParseOctal(block + kGidOffset, 8, "gid", false, &gid, error_message)) {
    return false;
  }
  if (!ParseOctal(block + kSizeOffset, 12, "size", false, &size,
                  error_message)) {
    return false;
  }
  if (!ParseOctal(block + kMtimeOffset, 12, "mtime", false, &mtime,
                  error_message)) {
    return false;
  }
  // devmajor / devminor 允许整字段为空：GNU tar 对非设备条目写的就是 8 个 NUL。
  if (!ParseOctal(block + kDevMajorOffset, 8, "devmajor", true, &devmajor,
                  error_message)) {
    return false;
  }
  if (!ParseOctal(block + kDevMinorOffset, 8, "devminor", true, &devminor,
                  error_message)) {
    return false;
  }

  EntryType type = EntryType::kRegularFile;
  if (!TypeFromFlag(block[kTypeFlagOffset], &type, error_message)) {
    return false;
  }
  if (type != EntryType::kRegularFile && size != 0) {
    SetError(error_message, std::string("ustar header: ") +
                                EntryTypeName(type) + " 条目声明了 size " +
                                std::to_string(size) +
                                "，但非普通条目没有 payload");
    return false;
  }

  Header decoded;
  decoded.typeflag = block[kTypeFlagOffset];
  if (!DecodeFixedField(block, kNameSize, "name", &decoded.name,
                        error_message)) {
    return false;
  }
  if (!DecodeFixedField(block + kPrefixOffset, kPrefixSize, "prefix",
                        &decoded.prefix, error_message)) {
    return false;
  }
  if (!DecodeFixedField(block + kLinkNameOffset, kLinkNameSize, "linkname",
                        &decoded.linkname, error_message)) {
    return false;
  }
  if (!DecodeFixedField(block + kUnameOffset, 32, "uname", &decoded.uname,
                        error_message)) {
    return false;
  }
  if (!DecodeFixedField(block + kGnameOffset, 32, "gname", &decoded.gname,
                        error_message)) {
    return false;
  }
  if (decoded.name.empty()) {
    SetError(error_message, "ustar header: name 是空的");
    return false;
  }
  if (!decoded.prefix.empty() && decoded.name.empty()) {
    SetError(error_message, "ustar header: 有 prefix 却没有 name");
    return false;
  }
  if ((type == EntryType::kSymlink || type == EntryType::kHardLink) &&
      decoded.linkname.empty()) {
    SetError(error_message, std::string("ustar header: ") +
                                EntryTypeName(type) +
                                " 条目的 linkname 是空的");
    return false;
  }
  decoded.mode = static_cast<std::uint32_t>(mode);
  decoded.uid = static_cast<std::uint32_t>(uid);
  decoded.gid = static_cast<std::uint32_t>(gid);
  decoded.size = size;
  decoded.mtime = static_cast<std::int64_t>(mtime);
  decoded.devmajor = static_cast<std::uint32_t>(devmajor);
  decoded.devminor = static_cast<std::uint32_t>(devminor);
  *header = decoded;
  return true;
}

// ---- 两个写入器 ----

bool WriteBaseline(const std::vector<ArchiveEntry>& entries,
                   const std::string& archive_file,
                   std::string* error_message) {
  // 预校验先跑：任何格式层面的错误都在创建文件之前失败，磁盘上不留痕迹。
  if (!ValidateEntries(entries, error_message)) {
    return false;
  }
  OutputArchive output;
  if (!output.Create(archive_file, error_message)) {
    return false;
  }
  // 64 KiB 输出缓冲 + 逐 entry flush：header / padding / payload 各自 append
  // 一次，payload 先读进固定大小的临时数组再拷进缓冲。写法直白，作为"格式对
  // 不对"的参照实现；它的 syscall 次数（每条 entry 至少一次 write）也正好
  // 反衬 Fast 的差别。
  OutputBuffer buffer(&output, kBaselineBufferSize);
  std::vector<char> chunk(kBaselineReadChunk);
  std::string header_block;
  for (const ArchiveEntry& entry : entries) {
    if (!PrepareEntry(entry, &header_block, error_message)) {
      return false;
    }
    if (!buffer.Append(header_block.data(), header_block.size(),
                       error_message)) {
      return false;
    }
    if (entry.type == EntryType::kRegularFile) {
      // 即使 size 为 0 也打开一次：源文件不存在和"空文件"是两件事，
      // 写入端不能把前者写成后者。
      ScopedFd source;
      if (!OpenSourceFile(entry, &source, error_message)) {
        return false;
      }
      std::uint64_t remaining = entry.size;
      while (remaining > 0) {
        const std::size_t want = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, chunk.size()));
        const ssize_t got = ::read(source.get(), chunk.data(), want);
        if (got < 0) {
          if (errno == EINTR) {
            continue;
          }
          SetError(error_message,
                   DescribeErrno(errno, "读取源文件失败", entry.source_path));
          return false;
        }
        if (got == 0) {
          // 扫描之后源文件被改小或清空：读到的字节数不等于声明的 size，
          // 整次写入失败，不补齐、不假装成功。
          SetError(error_message,
                   "源文件比声明的 size 短: " + entry.source_path + "（条目 " +
                       entry.archive_path + "，还差 " +
                       std::to_string(remaining) + " 字节）");
          return false;
        }
        if (!buffer.Append(chunk.data(), static_cast<std::size_t>(got),
                           error_message)) {
          return false;
        }
        remaining -= static_cast<std::uint64_t>(got);
      }
      if (!buffer.AppendZeros(PaddingFor(entry.size), error_message)) {
        return false;
      }
    }
    // baseline 的"简单"就体现在这一行：每条 entry 结束就 flush。
    if (!buffer.Flush(error_message)) {
      return false;
    }
  }
  // 结尾：两个全零 block，之后不再有任何字节。
  if (!buffer.AppendZeros(2 * kBlockSize, error_message)) {
    return false;
  }
  if (!buffer.Flush(error_message)) {
    return false;
  }
  return output.Finish(error_message);
}

bool WriteFast(const std::vector<ArchiveEntry>& entries,
               const std::string& archive_file, std::string* error_message) {
  if (!ValidateEntries(entries, error_message)) {
    return false;
  }
  OutputArchive output;
  if (!output.Create(archive_file, error_message)) {
    return false;
  }
  // 1 MiB 统一缓冲：header、payload、padding 全聚合进同一个缓冲，只有缓冲满
  // 或收尾时才 write。payload 直接 read 进缓冲的空闲区，省掉一次 memcpy。
  OutputBuffer buffer(&output, kFastBufferSize);
  std::string header_block;
  for (const ArchiveEntry& entry : entries) {
    if (!PrepareEntry(entry, &header_block, error_message)) {
      return false;
    }
    if (!buffer.Append(header_block.data(), header_block.size(),
                       error_message)) {
      return false;
    }
    if (entry.type == EntryType::kRegularFile) {
      ScopedFd source;
      if (!OpenSourceFile(entry, &source, error_message)) {
        return false;
      }
      // 顺序读提示：大文件交给内核多做预读。失败直接忽略——这是纯优化，
      // 不影响任何语义，某些文件系统也不支持这个 advice。
      (void)::posix_fadvise(source.get(), 0, 0, POSIX_FADV_SEQUENTIAL);
      std::uint64_t remaining = entry.size;
      while (remaining > 0) {
        // 先保证缓冲里至少有 kFastReadChunk 的空闲（除非剩余 payload 更少），
        // 这样每次 read 都是大块，不会退化成几十字节的小读。缓冲容量
        // （1 MiB）大于这个下限，所以 flush 之后一定放得下。
        const std::uint64_t wanted =
            std::min<std::uint64_t>(remaining, kFastReadChunk);
        if (buffer.FreeSpace() < wanted && !buffer.Flush(error_message)) {
          return false;
        }
        const std::size_t want = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, buffer.FreeSpace()));
        if (want == 0) {
          SetError(error_message,
                   "WriteFast: 输出缓冲没有空闲空间（内部不变量被破坏）");
          return false;
        }
        const ssize_t got = ::read(source.get(), buffer.FreeData(), want);
        if (got < 0) {
          if (errno == EINTR) {
            continue;
          }
          SetError(error_message,
                   DescribeErrno(errno, "读取源文件失败", entry.source_path));
          return false;
        }
        if (got == 0) {
          SetError(error_message,
                   "源文件比声明的 size 短: " + entry.source_path + "（条目 " +
                       entry.archive_path + "，还差 " +
                       std::to_string(remaining) + " 字节）");
          return false;
        }
        buffer.Commit(static_cast<std::size_t>(got));
        remaining -= static_cast<std::uint64_t>(got);
      }
      if (!buffer.AppendZeros(PaddingFor(entry.size), error_message)) {
        return false;
      }
    }
  }
  if (!buffer.AppendZeros(2 * kBlockSize, error_message)) {
    return false;
  }
  if (!buffer.Flush(error_message)) {
    return false;
  }
  return output.Finish(error_message);
}

// ---- 读取器 ----

bool Scan(const std::string& archive_file, std::vector<Member>* members,
          std::string* error_message) {
  if (members == nullptr) {
    SetError(error_message, "Scan: members 是空指针");
    return false;
  }
  members->clear();

  ScopedFd archive;
  std::uint64_t file_size = 0;
  if (!OpenArchiveForRead(archive_file, &archive, &file_size, error_message)) {
    return false;
  }

  // 路径 -> 成员下标。三个用途：重复路径检测、"父路径必须是目录"、以及
  // 反向的前缀冲突检测（std::map 的 lower_bound 能做前缀查询）。
  std::map<std::string, std::size_t> index_by_path;
  std::uint64_t offset = 0;
  char block[kBlockSize];
  char second_block[kBlockSize];
  while (true) {
    // 上限检查放在读 header 之前：超过上限就报错，而不是继续 push_back
    // 直到把内存吃光。
    if (members->size() >= kMaxMembers) {
      SetError(error_message, "归档条目数超过上限 " +
                                  std::to_string(kMaxMembers) +
                                  " 条，拒绝继续解析: " + archive_file);
      return false;
    }
    if (offset >= file_size) {
      SetError(error_message,
               "归档在结尾标记（两个全零 block）之前就结束了: " + archive_file);
      return false;
    }
    if (file_size - offset < kBlockSize) {
      SetError(error_message, "归档最后一个 block 不足 512 字节（偏移 " +
                                  std::to_string(offset) +
                                  "，文件被截断）: " + archive_file);
      return false;
    }
    if (!ReadFullAt(archive.get(), block, kBlockSize, offset, archive_file,
                    error_message)) {
      return false;
    }
    if (IsZeroBlock(block)) {
      // 结尾标记：必须连续两个全零 block。之后只允许 0（GNU tar 会按 10240
      // 字节的记录对齐补零），出现任何非零字节都判失败。
      if (file_size - offset < 2 * kBlockSize) {
        SetError(error_message,
                 "归档结尾只有 1 个全零 block（末尾必须是连续两个），偏移 " +
                     std::to_string(offset));
        return false;
      }
      if (!ReadFullAt(archive.get(), second_block, kBlockSize,
                      offset + kBlockSize, archive_file, error_message)) {
        return false;
      }
      if (!IsZeroBlock(second_block)) {
        SetError(error_message, "归档结尾的两个 block 不全为零（偏移 " +
                                    std::to_string(offset + kBlockSize) + "）");
        return false;
      }
      if (!CheckTrailingZeros(archive.get(), offset + 2 * kBlockSize, file_size,
                              archive_file, error_message)) {
        return false;
      }
      break;
    }

    Header header;
    if (!DecodeHeader(block, &header, error_message)) {
      AddContext(error_message, "block 偏移 " + std::to_string(offset));
      return false;
    }
    EntryType type = EntryType::kRegularFile;
    if (!TypeFromFlag(header.typeflag, &type, error_message)) {
      AddContext(error_message, "block 偏移 " + std::to_string(offset));
      return false;
    }
    const std::string raw_path =
        header.prefix.empty() ? header.name : header.prefix + "/" + header.name;
    const bool trailing_slash = raw_path.size() > 1 && raw_path.back() == '/';
    std::string path;
    if (!NormalizeMemberPath(raw_path, &path, error_message)) {
      AddContext(error_message, "block 偏移 " + std::to_string(offset));
      return false;
    }
    if (trailing_slash && type != EntryType::kDirectory) {
      SetError(error_message, std::string("只有目录条目可以带结尾 '/'，但 ") +
                                  path + " 是 " + EntryTypeName(type) +
                                  "（偏移 " + std::to_string(offset) + "）");
      return false;
    }

    Member member;
    member.entry.archive_path = path;
    member.entry.source_path.clear();  // Scan 不碰文件系统，也就没有源路径
    member.entry.type = type;
    member.entry.mode = header.mode;
    member.entry.uid = header.uid;
    member.entry.gid = header.gid;
    member.entry.mtime_sec = header.mtime;
    member.entry.mtime_nsec = 0;  // ustar 只保存秒，纳秒在格式里没有位置
    member.entry.size = type == EntryType::kRegularFile ? header.size : 0;
    member.entry.dev_major = header.devmajor;
    member.entry.dev_minor = header.devminor;
    member.entry.user_name = header.uname;
    member.entry.group_name = header.gname;
    if (type == EntryType::kHardLink) {
      // 硬链接目标指向归档里的另一个路径，必须和我们归一化后的成员路径
      // 对得上（GNU tar 写的是 "./file.txt"）。
      std::string target;
      if (!NormalizeMemberPath(header.linkname, &target, error_message)) {
        AddContext(error_message, "硬链接 " + path);
        return false;
      }
      member.entry.link_target = target;
    } else if (type == EntryType::kSymlink) {
      // 软链接的目标可以是任意字符串（绝对路径、'..' 都合法）：那是链接自己的
      // 语义，不是本归档的成员路径，因此不归一化、不校验，原样保存。
      member.entry.link_target = header.linkname;
    }
    member.data_offset = offset + kBlockSize;
    member.data_size = member.entry.size;

    // payload 与 padding 必须完整落在文件里。先做 payload 检查，再单独检查
    // 补齐到 512 之后的部分，两次都用减法，避免 offset + size 溢出。
    if (!CheckMemberBounds(member, file_size, error_message)) {
      AddContext(error_message, "block 偏移 " + std::to_string(offset));
      return false;
    }
    const std::uint64_t padded = RoundUpToBlock(member.data_size);
    if (padded > file_size - member.data_offset) {
      SetError(error_message,
               "payload 之后的 padding 越界：size " +
                   std::to_string(member.data_size) + " 需要 " +
                   std::to_string(padded) + " 字节（含补齐），文件只剩 " +
                   std::to_string(file_size - member.data_offset) +
                   " 字节: " + path);
      return false;
    }

    if (index_by_path.find(path) != index_by_path.end()) {
      SetError(error_message, "归档里有重复路径: " + path);
      return false;
    }
    // 正向父子冲突：某个祖先路径已经是"非目录"，却又出现以它为前提的路径。
    for (std::size_t i = 0; i < path.size(); ++i) {
      if (path[i] != '/') {
        continue;
      }
      const std::string parent = path.substr(0, i);
      const auto found = index_by_path.find(parent);
      if (found == index_by_path.end()) {
        continue;
      }
      const EntryType parent_type = (*members)[found->second].entry.type;
      if (parent_type != EntryType::kDirectory) {
        SetError(error_message, std::string("父子冲突：") + parent +
                                    " 已经是 " + EntryTypeName(parent_type) +
                                    "，却又出现子路径 " + path);
        return false;
      }
    }
    // 反向父子冲突：新条目是"非目录"，而归档里已经有以它为前提的路径。
    if (type != EntryType::kDirectory) {
      const std::string subtree = path + "/";
      const auto found = index_by_path.lower_bound(subtree);
      if (found != index_by_path.end() &&
          found->first.compare(0, subtree.size(), subtree) == 0) {
        SetError(error_message, std::string("父子冲突：") + path + " 是 " +
                                    EntryTypeName(type) + "，但它下面已经有 " +
                                    found->first);
        return false;
      }
    }

    members->push_back(member);
    index_by_path.emplace(path, members->size() - 1);
    offset = member.data_offset + padded;
  }

  // 第二遍：硬链接目标必须在归档里，而且必须出现在硬链接之前——解包是顺序
  // 执行的，指向后面某个条目的硬链接在真正恢复时是建不出来的。这一遍放在
  // 收集完所有路径之后做，为的是给出确定的错误，而不是"解到一半才发现"。
  for (std::size_t i = 0; i < members->size(); ++i) {
    const Member& member = (*members)[i];
    if (member.entry.type != EntryType::kHardLink) {
      continue;
    }
    const auto found = index_by_path.find(member.entry.link_target);
    if (found == index_by_path.end()) {
      SetError(error_message,
               "硬链接目标不在归档中: " + member.entry.link_target + "（来自 " +
                   member.entry.archive_path + "）");
      return false;
    }
    if (found->second >= i) {
      SetError(error_message,
               "硬链接目标出现在硬链接之后: " + member.entry.link_target +
                   "（来自 " + member.entry.archive_path + "）");
      return false;
    }
    const EntryType target_type = (*members)[found->second].entry.type;
    if (target_type != EntryType::kRegularFile &&
        target_type != EntryType::kHardLink) {
      SetError(error_message,
               "硬链接目标不是普通文件: " + member.entry.link_target + " 是 " +
                   EntryTypeName(target_type));
      return false;
    }
  }
  return true;
}

bool ExtractData(const std::string& archive_file, const Member& member,
                 const std::string& destination_file,
                 std::string* error_message) {
  if (member.entry.type != EntryType::kRegularFile) {
    SetError(error_message, "ExtractData: 只有普通文件条目有 payload，当前是 " +
                                std::string(EntryTypeName(member.entry.type)) +
                                ": " + member.entry.archive_path);
    return false;
  }
  ScopedFd archive;
  std::uint64_t file_size = 0;
  if (!OpenArchiveForRead(archive_file, &archive, &file_size, error_message)) {
    return false;
  }
  if (!CheckMemberBounds(member, file_size, error_message)) {
    return false;
  }
  // 目标文件：O_CREAT|O_TRUNC。权限先给 0600，归档里的 mode 由恢复层在
  // 内容写完之后统一设置——先设权限会让后续写入失败（例如 0444）。
  ScopedFd destination(::open(destination_file.c_str(),
                              O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600));
  if (!destination.valid()) {
    SetError(error_message,
             DescribeErrno(errno, "创建目标文件失败", destination_file));
    return false;
  }
  std::vector<char> buffer(kExtractChunkSize);
  std::uint64_t offset = member.data_offset;
  std::uint64_t remaining = member.data_size;
  while (remaining > 0) {
    const std::size_t want = static_cast<std::size_t>(
        std::min<std::uint64_t>(remaining, buffer.size()));
    if (!ReadFullAt(archive.get(), buffer.data(), want, offset, archive_file,
                    error_message)) {
      RemoveFile(destination_file);
      return false;
    }
    if (!WriteAllFd(destination.get(), buffer.data(), want, destination_file,
                    error_message)) {
      RemoveFile(destination_file);
      return false;
    }
    offset += want;
    remaining -= want;
  }
  if (!destination.Close(destination_file, error_message)) {
    RemoveFile(destination_file);
    return false;
  }
  return true;
}

bool ExtractDataToString(const std::string& archive_file, const Member& member,
                         std::string* output, std::string* error_message) {
  if (output == nullptr) {
    SetError(error_message, "ExtractDataToString: output 是空指针");
    return false;
  }
  if (member.entry.type != EntryType::kRegularFile) {
    SetError(error_message,
             "ExtractDataToString: 只有普通文件条目有 payload，当前是 " +
                 std::string(EntryTypeName(member.entry.type)) + ": " +
                 member.entry.archive_path);
    return false;
  }
  output->clear();
  ScopedFd archive;
  std::uint64_t file_size = 0;
  if (!OpenArchiveForRead(archive_file, &archive, &file_size, error_message)) {
    return false;
  }
  if (!CheckMemberBounds(member, file_size, error_message)) {
    return false;
  }
  if (member.data_size == 0) {
    return true;  // 空文件：已经是空串
  }
  // data_size 已经确认不超过归档文件本身的大小，所以这里不会因为归档里的
  // 一个数字就申请到天文数字的内存。
  output->resize(static_cast<std::size_t>(member.data_size));
  if (!ReadFullAt(archive.get(), output->data(), output->size(),
                  member.data_offset, archive_file, error_message)) {
    output->clear();
    return false;
  }
  return true;
}

}  // namespace ustar
}  // namespace backupproject
