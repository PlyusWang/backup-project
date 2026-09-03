// file_system.h
//
// Sprint 1 的本地文件系统层。备份引擎所有“碰磁盘”的操作都走这里：
// 路径检查、建目录、递归复制目录树、逐字节复制普通文件。
//
// 当前版本只认普通目录和普通文件。软链接、FIFO、设备、socket 这类
// 类型会明确报 Unsupported，而不是跟着链接走或假装复制成功。
// 元数据（权限、属主、时间戳）v0.1 一律不保存。

#ifndef BACKUP_PROJECT_INCLUDE_FILE_SYSTEM_H_
#define BACKUP_PROJECT_INCLUDE_FILE_SYSTEM_H_

#include <string>

namespace backupproject {

// 对 POSIX 文件系统原语（lstat / opendir / open / read / write / mkdir）
// 做一层很薄的封装，主要干两件事：把错误信息收拾成统一格式，
// 以及用 RAII 保证出错时 fd 和 DIR* 也能自动关闭。独立成类是为了
// 让复制逻辑可以单独测试，也方便后续 Sprint 复用。
class FileSystem {
 public:
  // lstat 之后对路径的分类，各函数靠它决定走哪条分支。
  enum class PathStatus {
    // 路径不存在，或中间某层目录不存在（ENOTDIR 也归到这里）。
    kMissing,
    // 普通目录。
    kDirectory,
    // 普通文件。
    kRegularFile,
    // 存在，但是软链接、FIFO 等 v0.1 不支持的家伙。
    kOther,
    // lstat 本身失败（比如权限不足），原因看 error_message。
    kError,
  };

  FileSystem() = default;

  // 拼接两段路径，保证中间恰好一个 '/'。调用方不用关心父路径
  // 带不带尾斜杠。
  static std::string JoinPath(const std::string& parent,
                              const std::string& child);

  // 用 lstat 看路径是什么。lstat 不会跟随软链接，这样“路径本身是
  // 软链接”能如实暴露出来，而不是漏过去看到链接目标。kError 时
  // 会把原因写进 error_message。
  PathStatus InspectPath(const std::string& path, std::string* error_message);

  // 类似 mkdir -p：把 path 连同缺的父目录一起建出来。
  // 中间某层已存在但不是目录（比如是普通文件）时失败。
  bool MakeDirectories(const std::string& path, std::string* error_message);

  // 判断“还没建”或者“建了但是空的”。返回 false 分两种情况：
  // 出错（error_message 有内容）和已存在且非空（error_message 为空），
  // 引擎靠这个区分“报错”和“该拒绝覆盖”。
  bool IsMissingOrEmptyDirectory(const std::string& path,
                                 std::string* error_message);

  // 递归复制一个文件系统节点：普通目录先建出来再逐个孩子递归，
  // 普通文件交给 CopyRegularFile，其他类型直接失败。
  // 失败返回 false，并把出错路径和系统错误写进 error_message。
  bool CopyTree(const std::string& source, const std::string& destination,
                std::string* error_message);

 private:
  // 复制单个普通文件。read 可能比要的少、write 可能只写一半、
  // 还可能被 EINTR 打断，这些坑都在实现里兜住。
  bool CopyRegularFile(const std::string& source,
                       const std::string& destination,
                       std::string* error_message);
};

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_FILE_SYSTEM_H_
