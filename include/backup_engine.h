// backup_engine.h
//
// 备份 / 恢复的高层流程。
//
// Backup(source_directory, archive_file) 把源目录整棵树打包成一个归档文件；
// Restore(archive_file, destination_directory) 把归档解包回目录树。
//
// 归档文件里放的是我们自己的格式（见 docs/format/archive_v0.1.md）：
// 全局 header + 逐条 entry header + 原样照抄的文件正文。这是"打包"，
// 不是"压缩"——payload 与源文件 byte-for-byte 相同。
//
// v0.1 的边界：
//   * 只支持普通目录和普通文件，其余类型整次操作明确失败；
//   * 只保存 mode（0777 位）与 mtime（秒 + 纳秒），不含属主、ACL、xattr；
//   * 归档文件已存在时不覆盖；恢复目标已存在且非空时不覆盖。

#ifndef BACKUP_PROJECT_INCLUDE_BACKUP_ENGINE_H_
#define BACKUP_PROJECT_INCLUDE_BACKUP_ENGINE_H_

#include <string>

#include "file_system.h"
#include "filter.h"

namespace backupproject {

// 只负责校验与编排：路径是否合理、什么时候可以动手。真正的归档格式读写
// 在 ArchiveWriter / ArchiveReader 里，二进制格式的细节不落到这个文件。
class BackupEngine {
 public:
  BackupEngine();

  // 把 source_directory 打包成归档文件 archive_file。
  // 源必须是已存在的目录；archive_file 必须不存在，也不能落在源目录内部
  // （否则扫描过程中归档文件自己会成为输入树的一部分）。
  bool Backup(const std::string& source_directory,
              const std::string& archive_file, std::string* error_message);

  // 带筛选的版本：Filter 决定哪些条目进入归档；不传则等价于 PR #8 行为。
  bool Backup(const std::string& source_directory,
              const std::string& archive_file, const Filter& filter,
              std::string* error_message);

  // 把归档文件 archive_file 恢复到 destination_directory。
  // 归档必须是普通文件；目标目录不存在或存在但为空时都可以。
  bool Restore(const std::string& archive_file,
               const std::string& destination_directory,
               std::string* error_message);

 private:
  FileSystem file_system_;
};

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_BACKUP_ENGINE_H_
