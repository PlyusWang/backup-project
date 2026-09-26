// test_support.h
//
// 归档流水线专项测试用的小工具：断言、临时目录、文件构造、目录树比较。
//
// 刻意不引入任何测试框架：这些测试要和产品代码一起在最小的环境里编译运行，
// 多一个依赖就多一个"跑不起来"的理由。失败计数 + 非零退出足够表达结果。

#ifndef BACKUP_PROJECT_TESTS_UNIT_TEST_SUPPORT_H_
#define BACKUP_PROJECT_TESTS_UNIT_TEST_SUPPORT_H_

#include <dirent.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace test_support {

inline int& FailureCount() {
  static int value = 0;
  return value;
}

inline int& CheckCount() {
  static int value = 0;
  return value;
}

inline void Check(bool ok, const std::string& label,
                  const std::string& detail = std::string()) {
  CheckCount() += 1;
  if (ok) {
    std::printf("  PASS  %s\n", label.c_str());
    return;
  }
  FailureCount() += 1;
  if (detail.empty()) {
    std::printf("  FAIL  %s\n", label.c_str());
  } else {
    std::printf("  FAIL  %s -- %s\n", label.c_str(), detail.c_str());
  }
  std::fflush(stdout);
}

// 只打印信息，不计入断言。
inline void Note(const std::string& text) {
  std::printf("  NOTE  %s\n", text.c_str());
  std::fflush(stdout);
}

inline void Section(const std::string& title) {
  std::printf("\n== %s ==\n", title.c_str());
  std::fflush(stdout);
}

inline std::string Octal(std::uint32_t value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "0%o", value);
  return std::string(buffer);
}

// ---- 临时目录 --------------------------------------------------------------

inline std::string TempRoot() {
  static std::string root;
  if (root.empty()) {
    root = "/tmp/bp-sprint-" + std::to_string(static_cast<long>(::getpid()));
  }
  return root;
}

inline void RemoveTree(const std::string& path) {
  struct stat info;
  if (::lstat(path.c_str(), &info) != 0) {
    return;
  }
  if (!S_ISDIR(info.st_mode)) {
    ::unlink(path.c_str());
    return;
  }
  DIR* dir = ::opendir(path.c_str());
  if (dir != nullptr) {
    while (struct dirent* item = ::readdir(dir)) {
      const std::string name = item->d_name;
      if (name == "." || name == "..") {
        continue;
      }
      RemoveTree(path + "/" + name);
    }
    ::closedir(dir);
  }
  ::rmdir(path.c_str());
}

// 每次都从空目录开始，测试之间不会互相污染。
inline std::string FreshDir(const std::string& name) {
  const std::string path = TempRoot() + "/" + name;
  RemoveTree(path);
  ::mkdir(TempRoot().c_str(), 0755);
  ::mkdir(path.c_str(), 0755);
  return path;
}

// ---- 文件构造 --------------------------------------------------------------

inline bool Mkdir(const std::string& path, std::uint32_t mode) {
  return ::mkdir(path.c_str(), static_cast<mode_t>(mode)) == 0;
}

inline bool WriteFile(const std::string& path, const std::string& content,
                      std::uint32_t mode) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    return false;
  }
  std::size_t done = 0;
  while (done < content.size()) {
    const ssize_t written =
        ::write(fd, content.data() + done, content.size() - done);
    if (written <= 0) {
      ::close(fd);
      return false;
    }
    done += static_cast<std::size_t>(written);
  }
  ::close(fd);
  return ::chmod(path.c_str(), static_cast<mode_t>(mode)) == 0;
}

inline bool ReadFile(const std::string& path, std::string* content) {
  content->clear();
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return false;
  }
  char buffer[8192];
  while (true) {
    const ssize_t got = ::read(fd, buffer, sizeof(buffer));
    if (got < 0) {
      ::close(fd);
      return false;
    }
    if (got == 0) {
      break;
    }
    content->append(buffer, static_cast<std::size_t>(got));
  }
  ::close(fd);
  return true;
}

inline bool Exists(const std::string& path) {
  struct stat info;
  return ::lstat(path.c_str(), &info) == 0;
}

inline bool StatOf(const std::string& path, struct stat* info) {
  return ::lstat(path.c_str(), info) == 0;
}

inline bool SetTimes(const std::string& path, std::int64_t seconds,
                     std::uint32_t nanoseconds, bool no_follow = false) {
  struct timespec times[2];
  times[0].tv_sec = 0;
  times[0].tv_nsec = UTIME_OMIT;
  times[1].tv_sec = static_cast<time_t>(seconds);
  times[1].tv_nsec = static_cast<long>(nanoseconds);
  return ::utimensat(AT_FDCWD, path.c_str(), times,
                     no_follow ? AT_SYMLINK_NOFOLLOW : 0) == 0;
}

inline bool CreateSymlink(const std::string& target, const std::string& path) {
  return ::symlink(target.c_str(), path.c_str()) == 0;
}

inline bool CreateHardlink(const std::string& existing, const std::string& path) {
  return ::link(existing.c_str(), path.c_str()) == 0;
}

inline bool CreateFifo(const std::string& path, std::uint32_t mode) {
  return ::mkfifo(path.c_str(), static_cast<mode_t>(mode)) == 0;
}

// 建一个 unix socket 并保持监听：socket 文件必须真的存在，扫描器才会遇到它。
inline int CreateUnixSocket(const std::string& path) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  struct sockaddr_un address;
  std::memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  if (path.size() >= sizeof(address.sun_path)) {
    ::close(fd);
    return -1;
  }
  std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
  if (::bind(fd, reinterpret_cast<struct sockaddr*>(&address),
             sizeof(address)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

// ---- 目录树比较 ------------------------------------------------------------

inline std::vector<std::string> DirEntries(const std::string& path) {
  std::vector<std::string> names;
  DIR* dir = ::opendir(path.c_str());
  if (dir == nullptr) {
    return names;
  }
  while (struct dirent* item = ::readdir(dir)) {
    const std::string name = item->d_name;
    if (name != "." && name != "..") {
      names.push_back(name);
    }
  }
  ::closedir(dir);
  std::sort(names.begin(), names.end());
  return names;
}

// 把一棵树的 mtime 全部改成"整秒、且与路径相关"的确定值。
//
// 为什么矩阵测试需要它：USTAR 的 mtime 字段是 12 字节八进制秒，格式本身没有
// 纳秒位。要比较"两种 pack 后端都完整往返"，fixture 就必须落在 USTAR 能精确
// 表达的范围里；纳秒精度单由 MyPack v2 的专项测试验证。
//
// 顺序必须是"先子后父"：创建/修改子项会改父目录的 mtime。
inline void NormalizeTimes(const std::string& root, std::int64_t base) {
  std::int64_t offset = 0;
  for (const std::string& name : DirEntries(root)) {
    const std::string child = root + "/" + name;
    struct stat info;
    if (::lstat(child.c_str(), &info) != 0) {
      continue;
    }
    ++offset;
    if (S_ISDIR(info.st_mode)) {
      NormalizeTimes(child, base + offset);
    } else {
      SetTimes(child, base + offset, 0, /*no_follow=*/true);
    }
  }
  SetTimes(root, base, 0);
}

inline bool CompareFiles(const std::string& left, const std::string& right,
                         std::string* detail) {
  std::string a;
  std::string b;
  if (!ReadFile(left, &a) || !ReadFile(right, &b)) {
    *detail = "cannot read file for comparison: " + left;
    return false;
  }
  if (a.size() != b.size()) {
    *detail = "file size differs: " + left + " (" + std::to_string(a.size()) +
              " vs " + std::to_string(b.size()) + ")";
    return false;
  }
  if (a != b) {
    *detail = "file content differs: " + left;
    return false;
  }
  return true;
}

inline bool CompareNodes(const std::string& left, const std::string& right,
                         std::string* detail) {
  struct stat a;
  struct stat b;
  if (::lstat(left.c_str(), &a) != 0) {
    *detail = "missing on the left: " + left;
    return false;
  }
  if (::lstat(right.c_str(), &b) != 0) {
    *detail = "missing on the right: " + right;
    return false;
  }
  if ((a.st_mode & S_IFMT) != (b.st_mode & S_IFMT)) {
    *detail = "entry type differs: " + left;
    return false;
  }
  if ((a.st_mode & 07777) != (b.st_mode & 07777)) {
    *detail = "mode differs: " + left + " " + Octal(a.st_mode & 07777) + " vs " +
              Octal(b.st_mode & 07777);
    return false;
  }
  if (a.st_mtim.tv_sec != b.st_mtim.tv_sec ||
      a.st_mtim.tv_nsec != b.st_mtim.tv_nsec) {
    *detail = "mtime differs: " + left;
    return false;
  }
  if (a.st_uid != b.st_uid || a.st_gid != b.st_gid) {
    *detail = "owner differs: " + left + " " + std::to_string(a.st_uid) + ":" +
              std::to_string(a.st_gid) + " vs " + std::to_string(b.st_uid) +
              ":" + std::to_string(b.st_gid);
    return false;
  }
  if (S_ISDIR(a.st_mode)) {
    if (DirEntries(left) != DirEntries(right)) {
      *detail = "directory entries differ: " + left;
      return false;
    }
    for (const std::string& name : DirEntries(left)) {
      if (!CompareNodes(left + "/" + name, right + "/" + name, detail)) {
        return false;
      }
    }
    return true;
  }
  if (S_ISREG(a.st_mode)) {
    if (a.st_size != b.st_size) {
      *detail = "size differs: " + left;
      return false;
    }
    return CompareFiles(left, right, detail);
  }
  if (S_ISLNK(a.st_mode)) {
    char target_a[4096];
    char target_b[4096];
    const ssize_t len_a = ::readlink(left.c_str(), target_a, sizeof(target_a));
    const ssize_t len_b = ::readlink(right.c_str(), target_b, sizeof(target_b));
    if (len_a != len_b ||
        std::string(target_a, static_cast<std::size_t>(len_a)) !=
            std::string(target_b, static_cast<std::size_t>(len_b))) {
      *detail = "symbolic link target differs: " + left;
      return false;
    }
    return true;
  }
  if (S_ISCHR(a.st_mode) || S_ISBLK(a.st_mode)) {
    if (a.st_rdev != b.st_rdev) {
      *detail = "device number differs: " + left;
      return false;
    }
    return true;
  }
  return true;
}

// 硬链接关系必须一起比较：两条路径共享 inode 这件事本身是归档要保住的信息，
// 只比较内容会漏掉"被复制成两份独立文件"这种错误。
inline std::vector<std::string> InodeGroups(const std::string& root) {
  std::vector<std::pair<std::string, std::pair<std::uint64_t, std::uint64_t>>>
      flat;
  std::vector<std::string> pending{root};
  while (!pending.empty()) {
    const std::string current = pending.back();
    pending.pop_back();
    struct stat info;
    if (::lstat(current.c_str(), &info) != 0) {
      continue;
    }
    const std::string relative =
        current == root ? std::string(".") : current.substr(root.size() + 1);
    if (S_ISDIR(info.st_mode)) {
      for (const std::string& name : DirEntries(current)) {
        pending.push_back(current + "/" + name);
      }
    } else if (S_ISREG(info.st_mode) && info.st_nlink > 1) {
      flat.push_back({relative,
                      {static_cast<std::uint64_t>(info.st_dev),
                       static_cast<std::uint64_t>(info.st_ino)}});
    }
  }
  std::sort(flat.begin(), flat.end());
  std::vector<std::string> groups;
  std::vector<std::string> current_group;
  std::pair<std::uint64_t, std::uint64_t> current_key{0, 0};
  for (const auto& item : flat) {
    if (current_group.empty() || item.second != current_key) {
      if (current_group.size() > 1) {
        std::string joined;
        for (const std::string& name : current_group) {
          joined += name + ",";
        }
        groups.push_back(joined);
      }
      current_group.clear();
      current_key = item.second;
    }
    current_group.push_back(item.first);
  }
  if (current_group.size() > 1) {
    std::string joined;
    for (const std::string& name : current_group) {
      joined += name + ",";
    }
    groups.push_back(joined);
  }
  std::sort(groups.begin(), groups.end());
  return groups;
}

// 完整的目录树比较：类型、mode、mtime（秒 + 纳秒）、属主、内容、软链接目标、
// 设备号、硬链接分组。
inline bool CompareTrees(const std::string& left, const std::string& right,
                         std::string* detail) {
  if (!CompareNodes(left, right, detail)) {
    return false;
  }
  const std::vector<std::string> groups_left = InodeGroups(left);
  const std::vector<std::string> groups_right = InodeGroups(right);
  if (groups_left != groups_right) {
    *detail = "hard link grouping differs between source and restore";
    return false;
  }
  return true;
}

inline int Finish(const std::string& name) {
  const int checks = CheckCount();
  const int failures = FailureCount();
  std::printf("\n%s: %d/%d checks passed\n", name.c_str(), checks - failures,
              checks);
  return failures == 0 ? 0 : 1;
}

}  // namespace test_support

#endif  // BACKUP_PROJECT_TESTS_UNIT_TEST_SUPPORT_H_
