// file_system.cpp
//
// Sprint 1 文件系统层的实现。整棵目录树的递归复制都在这个文件里，
// 只处理普通目录和普通文件。
//
// 复制循环没有用 std::ifstream 之类的现成封装，而是直接 read / write：
// 这样 short read、partial write、EINTR 这些 POSIX 层面的坑都能
// 当面处理清楚，代码也可以直接讲给老师听。

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

// 一次 read 多少字节。64 KiB 是个折中：太小了系统调用次数多，
// 太大了又占内存。这个量级对本地盘够用，v0.1 不做更细的调优。
constexpr std::size_t kCopyBufferSize = 64 * 1024;

// fd 的 RAII 包装。核心价值是兜住各种提前 return 的路径：
// 函数无论怎么退出，fd 都会被 close，不用在每个错误分支手动清理。
// 所以下面两个复制函数里才敢到处提前 return。
class ScopedFileDescriptor {
 public:
  explicit ScopedFileDescriptor(int fd) : fd_(fd) {}

  ScopedFileDescriptor(const ScopedFileDescriptor&) = delete;
  ScopedFileDescriptor& operator=(const ScopedFileDescriptor&) = delete;

  ~ScopedFileDescriptor() {
    if (fd_ >= 0) {
      // 析构里的 close 就算失败也补救不了什么，安静关掉即可。
      close(fd_);
    }
  }

  // 只负责把 fd 交给 read / write 用，生命周期仍由本类管理。
  int get() const { return fd_; }

  // 把 fd 交出来让调用方自己 close 并检查错误。
  // 只有“关闭结果会影响正确性”的地方才需要这样做，
  // 见 CopyRegularFile 结尾。
  int release() {
    const int fd = fd_;
    fd_ = -1;
    return fd;
  }

 private:
  int fd_;
};

// DIR* 的 RAII 包装，作用同上：任何路径退出都保证 closedir。
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

// 所有错误信息统一成“原因: 路径: 系统错误”的格式。
// 带上路径很重要：出错时用户一眼能看出卡在哪个文件上。
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

// 拼接路径。父路径末尾有没有 '/' 都行，结果保证只有一个分隔符。
std::string FileSystem::JoinPath(const std::string& parent,
                                 const std::string& child) {
  if (parent.empty() || parent.back() == '/') {
    return parent + child;
  }
  return parent + "/" + child;
}

// 看一个路径当前是什么状态，供引擎做前置检查。
// 这里故意用 lstat 而不是 stat：stat 会跟着软链接走，lstat 不会。
// v0.1 要的是“识别出软链接并拒绝它”，而不是偷偷复制链接目标，
// 所以必须用 lstat。
FileSystem::PathStatus FileSystem::InspectPath(const std::string& path,
                                               std::string* error_message) {
  struct stat info;
  if (lstat(path.c_str(), &info) != 0) {
    // 路径不存在对调用方是正常预期（比如第一次备份时仓库还没建），
    // 归为 kMissing 而不是报错。ENOTDIR 也一并算“缺失”：
    // 它说明中间某层根本不是目录，对上层来说就是找不到这个路径。
    if (errno == ENOENT || errno == ENOTDIR) {
      return PathStatus::kMissing;
    }
    // 其余失败（典型的如 EACCES）才是真错误，交给调用方。
    if (error_message != nullptr) {
      *error_message = Describe(errno, "Failed to inspect path", path);
    }
    return PathStatus::kError;
  }
  // st_mode 的类型位和 lstat 的结果一一对应，按它分支。
  if (S_ISDIR(info.st_mode)) {
    return PathStatus::kDirectory;
  }
  if (S_ISREG(info.st_mode)) {
    return PathStatus::kRegularFile;
  }
  return PathStatus::kOther;
}

// 自底向上把 path 的每一层都建出来，相当于 mkdir -p。
// 一层一层建而不是直接 mkdir(path)，是因为 mkdir 不会自动建父目录，
// 而 restore 的目标经常连父目录都还不存在。
bool FileSystem::MakeDirectories(const std::string& path,
                                 std::string* error_message) {
  // 空路径没有意义，直接拒绝，免得后面逻辑踩坑。
  if (path.empty()) {
    if (error_message != nullptr) {
      *error_message = "Failed to create destination directory: path is empty";
    }
    return false;
  }

  std::string current;
  // 绝对路径（以 '/' 开头）要先把 current 置成根，否则第一段会拼错。
  if (path.front() == '/') {
    current = "/";
  }

  // 按 '/' 把 path 切成一段一段，从前往后逐段 mkdir。
  // 连续斜杠和尾斜杠在这里自然被跳过，不影响结果。
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

    // 拼接当前累积路径：第一段直接放进去，之后每段前面补一个 '/'。
    // 相对路径和绝对路径（current 初始为 "/"）都适用。
    if (current.empty()) {
      current = component;
    } else if (current.back() == '/') {
      current += component;
    } else {
      current += '/';
      current += component;
    }

    // 0777 会被进程 umask 收口，最终权限由 umask 决定——v0.1 不保存
    // 元数据，所以不追求复刻源目录的权限。EEXIST 说明这一层本来就有，
    // 不算错误；它到底能不能当目录用，交给最后的检查。
    if (mkdir(current.c_str(), 0777) != 0 && errno != EEXIST) {
      if (error_message != nullptr) {
        *error_message =
            Describe(errno, "Failed to create destination directory", current);
      }
      return false;
    }
  }

  // 全部建完后再确认一次 path 本身是目录。这一步能兜住
  // “中间某层是普通文件”的情况：那时 mkdir 只会得到 EEXIST 或 ENOTDIR，
  // 一路放过的话，到复制阶段才会莫名失败，这里提前说清楚。
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

// 回答“这里能不能直接用”：路径不存在，或者是个空目录，就返回 true。
// 返回 false 有两种可能，引擎靠 error_message 是否为空来区分：
// 有内容 = 检查出错（按错误处理）；为空 = 已存在且非空（拒绝覆盖）。
bool FileSystem::IsMissingOrEmptyDirectory(const std::string& path,
                                           std::string* error_message) {
  // 先看路径状态：不存在直接算“可用”。
  const PathStatus status = InspectPath(path, error_message);
  if (status == PathStatus::kMissing) {
    return true;
  }
  if (status == PathStatus::kError) {
    // 原因已经由 InspectPath 写进 error_message 了。
    return false;
  }
  // 存在但不是目录（比如普通文件），按错误处理。
  if (status != PathStatus::kDirectory) {
    if (error_message != nullptr) {
      *error_message = "Path exists and is not a directory: " + path;
    }
    return false;
  }

  // 走到这里说明是个已存在的目录，再确认它空不空。
  DIR* dir = opendir(path.c_str());
  if (dir == nullptr) {
    if (error_message != nullptr) {
      *error_message = Describe(errno, "Failed to open directory", path);
    }
    return false;
  }
  ScopedDirectory scoped_dir(dir);

  // readdir 返回 nullptr 有两个意思：读完了，或者出错了。
  // 所以循环前先把 errno 清 0，拿到 nullptr 再看 errno 才能分辨。
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
    // 看到 "." 和 ".." 以外的条目就说明目录非空，可以直接返回。
    return false;
  }
}

// 复制单个普通文件，整个函数就是“读一块、写一块”的循环。
// 两个容易踩的坑都显式处理：read 不一定给满整个缓冲区；
// write 也不保证一次写完，所以写侧再套一层循环，直到全部落盘。
bool FileSystem::CopyRegularFile(const std::string& source,
                                 const std::string& destination,
                                 std::string* error_message) {
  // 源文件只读打开。打不开的原因（不存在、EACCES 等）原样报给用户。
  ScopedFileDescriptor source_fd(open(source.c_str(), O_RDONLY));
  if (source_fd.get() < 0) {
    if (error_message != nullptr) {
      *error_message = Describe(errno, "Failed to open source file", source);
    }
    return false;
  }

  // O_TRUNC 保证“已存在的同名目标”从头写起；能走到这里说明上层
  // 已经确认过目标目录允许复用。0666 同样交给 umask 收口。
  ScopedFileDescriptor destination_fd(
      open(destination.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666));
  if (destination_fd.get() < 0) {
    if (error_message != nullptr) {
      *error_message =
          Describe(errno, "Failed to open destination file", destination);
    }
    return false;
  }

  // 缓冲区直接放栈上，64 KiB 的量级不需要手动堆管理。
  std::array<char, kCopyBufferSize> buffer;
  // 循环读源文件直到 EOF，每次 read 的结果单独判断。
  for (;;) {
    const ssize_t read_bytes =
        read(source_fd.get(), buffer.data(), buffer.size());
    if (read_bytes < 0) {
      // EINTR 表示被信号打断、一个字节都没读，不是真失败，重试即可。
      if (errno == EINTR) {
        continue;
      }
      if (error_message != nullptr) {
        *error_message = Describe(errno, "Failed to read source file", source);
      }
      return false;
    }
    if (read_bytes == 0) {
      // 读到文件尾，这一轮复制结束。
      break;
    }

    // write 可能只写了一部分（磁盘压力、信号等都可能导致），
    // 所以用 cursor / remaining 一直写到这一块全部写完为止。
    ssize_t remaining = read_bytes;
    const char* cursor = buffer.data();
    while (remaining > 0) {
      const ssize_t written = write(destination_fd.get(), cursor,
                                    static_cast<std::size_t>(remaining));
      if (written < 0) {
        // 和 read 一样：EINTR 时直接重试。
        if (errno == EINTR) {
          continue;
        }
        if (error_message != nullptr) {
          *error_message =
              Describe(errno, "Failed to write destination file", destination);
        }
        return false;
      }
      if (written == 0) {
        // write 返回 0 正常不该出现，当作写失败处理，
        // 否则这个循环可能永远转下去。
        if (error_message != nullptr) {
          *error_message =
              Describe(EIO, "Failed to write destination file", destination);
        }
        return false;
      }
      // 写成功 written 字节。循环不变量：remaining 等于这一块
      // 还没写完的字节数，清 0 才算这一块落盘完成。
      cursor += written;
      remaining -= written;
    }
  }

  // 目标文件显式 close 并检查错误：有些文件系统的延迟写错误要等到
  // close 才报出来。源文件只读，关失败不影响数据，交给 RAII 即可。
  if (close(destination_fd.release()) < 0) {
    if (error_message != nullptr) {
      *error_message =
          Describe(errno, "Failed to close destination file", destination);
    }
    return false;
  }
  return true;
}

// 递归复制一个节点，是本文件的核心。
// 目录：先建目标目录，再逐个孩子递归；普通文件：复制内容；
// 其他类型（软链接、FIFO、设备、socket）：明确失败。
// 任何一个孩子失败，整次调用都失败——宁可备份不完整也要显式报错，
// 不能生成“看起来成功、其实缺东西”的备份。
bool FileSystem::CopyTree(const std::string& source,
                          const std::string& destination,
                          std::string* error_message) {
  // 先 lstat 定类型：目录走递归，文件走复制，其余类型报错。
  struct stat source_info;
  if (lstat(source.c_str(), &source_info) != 0) {
    if (error_message != nullptr) {
      *error_message = Describe(errno, "Failed to inspect source path", source);
    }
    return false;
  }

  if (S_ISDIR(source_info.st_mode)) {
    // 空目录也要先建出来：restore 之后目录结构必须和源一致，
    // 只复制文件的话空目录会凭空消失。
    if (!MakeDirectories(destination, error_message)) {
      return false;
    }

    // 逐个读孩子。readdir 不保证顺序，这里只关心“有什么”，
    // 不关心先后，所以顺序无所谓。
    DIR* dir = opendir(source.c_str());
    if (dir == nullptr) {
      if (error_message != nullptr) {
        *error_message =
            Describe(errno, "Failed to open source directory", source);
      }
      return false;
    }
    ScopedDirectory scoped_dir(dir);

    // 同样用“循环前清 errno”的办法区分“读完了”和“读出错”。
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
      // "." 和 ".." 不是真正的孩子，跳过，否则递归会转圈。
      if (name == "." || name == "..") {
        continue;
      }
      // 子路径直接用 d_name 拼接：文件名里的空格、UTF-8 字符都
      // 原样保留，不在这里做任何清洗。
      if (!CopyTree(JoinPath(source, name), JoinPath(destination, name),
                    error_message)) {
        return false;
      }
    }
  }

  // 普通文件不再递归，直接复制内容。
  if (S_ISREG(source_info.st_mode)) {
    return CopyRegularFile(source, destination, error_message);
  }

  // v0.1 只支持普通目录和普通文件，走到这里说明遇到了软链接、FIFO
  // 之类。直接失败，绝不跟着链接走，也不默默跳过。
  if (error_message != nullptr) {
    *error_message = "Unsupported file type: " + source;
  }
  return false;
}

}  // namespace backupproject
