// archive_path.h
//
// 归档内路径的语法与拓扑校验。
//
// 这一份实现被三处共用：v0.1 写入器、v0.1 读取器 preflight、v2 流水线的
// MyPack v2 / USTAR 读写两侧。把它们收成一个模块不是因为"顺手抽象"，而是
// 因为写侧必须保证"自己能产出"蕴含"读侧能接受"：如果只有读侧检查，写侧就会
// 产出自己刚写的包自己读不回来的归档。
//
// 它同时是防 path traversal 的第一道闸：恢复时的目标路径是 destination 与
// 归档内路径拼出来的，只要归档里能出现 ".."、绝对路径或空 component，拼接结果
// 就可能跑到 destination 之外。与其在拼接时做归一化（要处理软链接、大小写、
// 符号等价等一堆情况），不如在这里就把这类路径判死：合法的归档不需要它们。

#ifndef BACKUP_PROJECT_INCLUDE_ARCHIVE_PATH_H_
#define BACKUP_PROJECT_INCLUDE_ARCHIVE_PATH_H_

#include <cstddef>
#include <string>
#include <unordered_set>

namespace backupproject {

// 归档内路径的长度上限，与 Linux PATH_MAX 同量级。
inline constexpr std::size_t kMaxArchivePathLength = 4096;

// 单条路径的语法校验。is_first_entry 为真时只接受 "."（且必须是目录）。
//
// 接受："."、"a"、"a/b"、"中文 名字.txt"、UTF-8 原样字节。
// 拒绝：空路径、绝对路径、结尾 '/'、空 component、"." / ".." component、
//       含 NUL、含反斜杠、Windows 盘符、超过 max_path_length。
bool IsValidArchivePath(const std::string& path, bool is_first_entry,
                        bool is_directory, std::size_t max_path_length,
                        std::string* error_message);

// 归档路径的深度："." 是 0，"a" 是 1，"a/b" 是 2。用于"从深到浅"恢复 metadata。
std::size_t ArchivePathDepth(const std::string& path);

// 路径注册表：读侧用来同时守住"重复路径""父目录必须是目录""父先于子"。
//
// require_parent_first 为真时（MyPack v2 用），每条路径的父目录必须已经以目录
// 身份出现过——这是我们自己的写入器保证的顺序，检查它等于免费加固。
// 为假时（USTAR 用，GNU tar 的输出不保证这个顺序），只检查"某条路径被当成
// 父目录时它必须真的是目录"，并在 Finalize 里补一次全量复核。
class ArchivePathRegistry {
 public:
  explicit ArchivePathRegistry(bool require_parent_first)
      : require_parent_first_(require_parent_first) {}

  // 登记一条路径。失败时写 error_message 并保持内部状态不变。
  bool Add(const std::string& path, bool is_directory,
           std::string* error_message);

  // 非严格模式下补齐"父目录后出现"的复核。严格模式下是空操作。
  bool Finalize(std::string* error_message) const;

  std::size_t size() const { return all_.size(); }

 private:
  // 返回 path 的父路径；没有 '/' 时返回 "."。
  static std::string ParentOf(const std::string& path);

  bool require_parent_first_ = true;
  std::unordered_set<std::string> all_;
  std::unordered_set<std::string> directories_;
};

// 把归档内的相对路径拼到 destination 下。输入的 path 必须已经过
// IsValidArchivePath 校验；"." 直接返回 destination 本身。
std::string JoinArchivePath(const std::string& destination,
                            const std::string& archive_path);

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_ARCHIVE_PATH_H_
