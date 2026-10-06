// archive_path.cpp
//
// 见 archive_path.h：这里是路径语法与拓扑校验的唯一实现。

// 本文件是"归档内路径"这一概念的唯一实现处：语法合法性、条目之间的拓扑
// 合法性，以及"把归档里的相对路径落到 staging 根目录下"的安全解析。
//
// 调用方三类：写入器（生成条目名并登记）、恢复侧（写盘前必须走
// ResolveUnderRootNoSymlinkAncestors）、展示层（只读结论，不自己判断）。
//
// 安全边界：归档内容一律是不可信输入，这里的校验全部 fail-closed —— 任何
// 不是预期形状的路径直接拒绝，绝不"清洗一下再用"，也不接受"调用方已经
// 校验过"作为前提。
//
// 线程：本文件只有纯函数与操作自身成员的小对象，没有全局状态；同一个
// Registry 对象不保证并发安全，由调用方串行使用。
#include "archive_path.h"

#include <sys/stat.h>
#include <sys/types.h>

#include <cctype>
#include <cerrno>
#include <cstring>

namespace backupproject {

namespace {

void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) {
    *error_message = text;
  }
}

// strerror 对未知 errno 可能返回 nullptr，兜底成 "errno N"，
// 保证诊断信息永远不为空。
std::string ErrnoText(int error_number) {
  const char* text = ::strerror(error_number);
  return text == nullptr ? std::string("errno ") + std::to_string(error_number)
                         : std::string(text);
}

// 只负责补一个斜杠：不去重、不规范化，也不判断 relative 是否越界 ——
// 越界判定是逐段 lstat 的职责，不是拼接的职责。
std::string JoinRoot(const std::string& root, const std::string& relative) {
  if (root.empty()) return relative;
  if (root.back() == '/') return root + relative;
  return root + "/" + relative;
}

}  // namespace

// 归档路径的语法与"第一条 entry"语义。规则（任一不满足即拒绝）：
//   * 非空、长度 <= max_path_length、不含 NUL 字节；
//   * 必须是相对路径：不以 '/' 开头、不以 '/' 结尾、不含 '\\'、
//     不是 "C:" 这类盘符写法（那不是本项目的语义）；
//   * 每一段都非空且不是 "." / ".."，堵死 foo//bar 与 ../escape；
//   * "." 只允许作为第一条 entry，而且必须是目录：它代表 source root 本身。
// 返回 false 时 error_message 是给人看的英文诊断，带原始路径。
bool IsValidArchivePath(const std::string& path, bool is_first_entry,
                        bool is_directory, std::size_t max_path_length,
                        std::string* error_message) {
  if (path.empty()) {
    SetError(error_message, "Invalid archive path: empty path");
    return false;
  }
  if (path.size() > max_path_length) {
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
  // C:\... 这类 Windows 盘符路径不属于本项目的语义，直接拒绝。
  if (path.size() >= 2 && path[1] == ':' &&
      std::isalpha(static_cast<unsigned char>(path[0])) != 0) {
    SetError(error_message, "Invalid archive path (drive letter): " + path);
    return false;
  }
  if (path == ".") {
    // "." 代表 source root 本身，只允许作为第一条 entry，而且必须是目录。
    if (!is_first_entry || !is_directory) {
      SetError(error_message, "Invalid root entry in archive");
      return false;
    }
    return true;
  }

  // 逐段检查：空段（foo//bar）、"."（foo/./bar）、".."（../escape）都不允许。
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

// 深度 = 斜杠数 + 1；空串与 "." 记为 0 层（root 自身不算一层）。
// 只对已经通过 IsValidArchivePath 的路径有意义。
std::size_t ArchivePathDepth(const std::string& path) {
  if (path.empty() || path == ".") {
    return 0;
  }
  std::size_t depth = 1;
  for (const char character : path) {
    if (character == '/') {
      ++depth;
    }
  }
  return depth;
}

// 单层父目录：没有斜杠时父就是 "."（root 自身）。这里不做规范化，
// 能走到这里的路径都已经过语法校验。
std::string ArchivePathRegistry::ParentOf(const std::string& path) {
  const std::size_t slash = path.rfind('/');
  if (slash == std::string::npos) {
    return ".";
  }
  return path.substr(0, slash);
}

// 登记一个条目。两条共同前提：路径不能重复（重复直接报错，不静默去重，
// 否则归档里会出现两条同名 entry）。
// require_parent_first_ 打开时依赖"写入器是 DFS 先序"这个事实，要求父目录
// 已经作为目录登记过；关闭时只禁止"把普通文件当父目录"，父目录可以后到，
// 由 Finalize() 补一次全量复核。
bool ArchivePathRegistry::Add(const std::string& path, bool is_directory,
                              std::string* error_message) {
  if (all_.find(path) != all_.end()) {
    SetError(error_message, "Duplicate archive path: " + path);
    return false;
  }
  if (path != ".") {
    const std::string parent = ParentOf(path);
    const bool parent_listed = all_.find(parent) != all_.end();
    const bool parent_is_directory =
        directories_.find(parent) != directories_.end();
    if (require_parent_first_) {
      // 我们的写入器是 DFS 先序：父目录一定先出现过。
      if (!parent_is_directory) {
        SetError(error_message,
                 "Archive path has no parent directory entry: " + path);
        return false;
      }
    } else if (parent_listed && !parent_is_directory) {
      SetError(error_message,
               "Archive path uses a non-directory as parent: " + path);
      return false;
    }
  }
  all_.insert(path);
  if (is_directory) {
    directories_.insert(path);
  }
  return true;
}

// 关档前的最后一道检查。严格模式下 Add 已经把不变量守住了，直接返回；
// 宽松模式必须沿父链一路上溯到 "."，因为父条目可能比子条目晚出现。
// const：只读已登记的集合，可重复调用且结果一致。
bool ArchivePathRegistry::Finalize(std::string* error_message) const {
  if (require_parent_first_) {
    return true;
  }
  // 非严格模式下父目录可能后出现，这里再走一遍全量复核。
  for (const std::string& path : all_) {
    if (path == ".") {
      continue;
    }
    std::string parent = ParentOf(path);
    while (true) {
      const bool parent_listed = all_.find(parent) != all_.end();
      if (parent_listed && directories_.find(parent) == directories_.end()) {
        SetError(error_message,
                 "Archive path uses a non-directory as parent: " + path);
        return false;
      }
      if (parent == ".") {
        break;
      }
      parent = ParentOf(parent);
    }
  }
  return true;
}

// 把归档里的相对路径解析成 staging 根下的真实路径，且**任何一级中间组件都
// 不允许是符号链接**。这是恢复写盘的最后一道闸，也是本文件的安全核心：
//
//   * 输入不可信：relative_path 来自归档，root 来自调用方配置；
//   * 语义是 tombstone 用的：目标必须在 root 之下，不允许是 root 自己，
//     也不允许为空、以 '/' 开头、带 '\\' —— 语法在这里独立复查一遍，
//     不把"调用方已经校验过"当前提；
//   * 逐段 lstat 而不是 stat：中间某一段是符号链接就立刻拒绝，因为链接能把
//     后续写入引到 root 之外，哪怕前面的名字看起来完全正常；
//   * 失败一律 fail-closed，绝不返回"尽力而为"的候选路径。
//
// 成功时 *resolved_path 是 root 与 relative_path 的拼接；*exists 说明末级
// 组件当前是否真实存在（false 时调用方可以按"新建"处理）。二者在进入函数
// 时先被清空 / 置 false，失败时不会留下半成品。
bool ResolveUnderRootNoSymlinkAncestors(const std::string& root,
                                        const std::string& relative_path,
                                        std::string* resolved_path,
                                        bool* exists,
                                        std::string* error_message) {
  if (resolved_path == nullptr || exists == nullptr) {
    SetError(error_message,
             "ResolveUnderRootNoSymlinkAncestors: outputs must not be null");
    return false;
  }
  resolved_path->clear();
  *exists = false;
  if (root.empty()) {
    SetError(error_message, "The resolution root must not be empty");
    return false;
  }
  if (root.find('\0') != std::string::npos) {
    SetError(error_message, "The resolution root contains a NUL byte");
    return false;
  }
  // 语法先自己过一遍，不把"调用方已经校验过"当成前提：这个函数是最后一道闸，
  // 它自己必须能独立成立。
  if (relative_path.empty() || relative_path == ".") {
    SetError(error_message,
             "A tombstone must name a path below the staging root, not the "
             "root itself");
    return false;
  }
  if (relative_path.find('\0') != std::string::npos) {
    SetError(error_message, "Invalid path (contains NUL byte)");
    return false;
  }
  if (relative_path.front() == '/') {
    SetError(error_message, "Invalid path (absolute): " + relative_path);
    return false;
  }
  if (relative_path.back() == '/') {
    SetError(error_message, "Invalid path (trailing slash): " + relative_path);
    return false;
  }
  if (relative_path.find('\\') != std::string::npos) {
    SetError(error_message, "Invalid path (backslash): " + relative_path);
    return false;
  }

  std::string current = root;
  std::size_t start = 0;
  while (true) {
    const std::size_t slash = relative_path.find('/', start);
    const bool is_last = slash == std::string::npos;
    const std::string component =
        is_last ? relative_path.substr(start)
                : relative_path.substr(start, slash - start);
    if (component.empty() || component == "." || component == "..") {
      SetError(error_message, "Invalid path component: " + relative_path);
      return false;
    }
    current = current + "/" + component;

    struct stat info;
    if (::lstat(current.c_str(), &info) != 0) {
      if (errno != ENOENT) {
        SetError(error_message,
                 "Cannot inspect " + current + ": " + ErrnoText(errno));
        return false;
      }
      // 不存在就是"没有东西要处理"：调用方按 exists = false 处理。
      *resolved_path = JoinRoot(root, relative_path);
      *exists = false;
      return true;
    }
    if (is_last) {
      *resolved_path = JoinRoot(root, relative_path);
      *exists = true;
      return true;
    }
    // 中间组件必须是真实目录。软链接单独报一句：那正是这条检查存在的理由。
    if (!S_ISDIR(info.st_mode)) {
      if (S_ISLNK(info.st_mode)) {
        SetError(
            error_message,
            "Refusing to follow a symlink inside the staging tree: " + current);
      } else {
        SetError(error_message,
                 "A path component is not a directory: " + current);
      }
      return false;
    }
    start = slash + 1;
  }
}

// 展示与落盘用的拼接：任一侧为空或 "." 时返回另一侧，避免出现 "./x" 或
// "dest/" 这类噪音。它不做任何校验，不能替代上面的解析函数。
std::string JoinArchivePath(const std::string& destination,
                            const std::string& archive_path) {
  if (archive_path.empty() || archive_path == ".") {
    return destination;
  }
  if (destination.empty() || destination == ".") {
    return archive_path;
  }
  if (destination.back() == '/') {
    return destination + archive_path;
  }
  return destination + "/" + archive_path;
}

}  // namespace backupproject
