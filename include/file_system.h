// file_system.h
//
// Sprint 1 的本地文件系统层：路径检查、建目录、递归复制目录树、
// 逐字节复制普通文件都在这里。只认普通目录和普通文件，软链接、FIFO、
// 设备、socket 一律明确拒绝；权限与时间戳的保存由归档层负责
// （见 docs/format/archive_v0.1.md），这一层只提供文件系统原语。

#ifndef BACKUP_PROJECT_INCLUDE_FILE_SYSTEM_H_
#define BACKUP_PROJECT_INCLUDE_FILE_SYSTEM_H_

#include <string>

namespace backupproject {

// 对 POSIX 文件系统原语（lstat / opendir / open / read / write / mkdir）
// 做一层薄封装：错误信息统一格式，并用 RAII 保证出错时 fd 和 DIR*
// 也能自动关闭。独立成类是为了复制逻辑能单独测试、后续 Sprint 好复用。
class FileSystem {
 public:
  // lstat 之后对路径的分类，各函数靠它决定走哪条分支。
  enum class PathStatus {
    // 路径不存在，或中间某层目录不存在（ENOTDIR 也归到这里）。
    kMissing,
    kDirectory,
    kRegularFile,
    // 存在，但是软链接、FIFO 等 v0.1 不支持的家伙。
    kOther,
    // lstat 本身失败（比如权限不足），原因看 error_message。
    kError,
  };

  FileSystem() = default;

  // 拼接两段路径，保证中间恰好一个 '/'（父路径带不带尾斜杠都行）。
  static std::string JoinPath(const std::string& parent,
                              const std::string& child);

  // 用 lstat 看路径是什么。lstat 不跟随软链接，“路径本身就是软链接”
  // 才能如实暴露出来；kError 时把原因写进 error_message。
  PathStatus InspectPath(const std::string& path, std::string* error_message);

  // 类似 mkdir -p：把 path 连同缺的父目录一起建出来。
  // 中间某层已存在但不是目录（比如是普通文件）时失败。
  bool MakeDirectories(const std::string& path, std::string* error_message);

  // 判断“还没建”或者“建了但是空的”。返回 false 时靠 error_message
  // 是否为空区分“检查出错”和“已存在且非空，该拒绝覆盖”。
  bool IsMissingOrEmptyDirectory(const std::string& path,
                                 std::string* error_message);

  // 递归复制一个文件系统节点：目录递归、普通文件复制内容、其他类型失败。
  // 动手之前先做一次路径拓扑检查：destination 不能等于 source，也不能落在
  // source 里面，否则复制出来的东西会被当成新的输入一层层套下去
  // （ROB-02 / ROB-03 的根因）。失败返回 false，并写明出错路径和原因。
  bool CopyTree(const std::string& source, const std::string& destination,
                std::string* error_message);

  // destination 是否在 source 外面（既不相等也不在其下）。发布成公开接口
  // 是为了让归档写入复用同一套拓扑判断：归档文件同样不能落在源目录里面。
  // 逐段比较规范化后的路径组件，不用字符串前缀，避免 /tmp/a 与 /tmp/abc
  // 被误判成父子关系。
  bool IsDestinationOutsideSource(const std::string& source,
                                  const std::string& destination,
                                  std::string* error_message);

 private:
  // 复制单个普通文件：短读、半写和 EINTR 这些 POSIX 坑在实现里兜住。
  bool CopyRegularFile(const std::string& source,
                       const std::string& destination,
                       std::string* error_message);

  // 真正干活的递归复制：目录递归、文件复制、其他类型报错。
  // 只由 CopyTree 调用一次，递归时直接调自己，不再重复做路径规范化。
  bool CopyTreeInternal(const std::string& source,
                        const std::string& destination,
                        std::string* error_message);
};

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_FILE_SYSTEM_H_
