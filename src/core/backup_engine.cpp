// backup_engine.cpp
//
// 引擎只做两件事：把参数检查干净，然后把活儿交给归档读写器。
// 这里没有一行二进制格式处理的代码：header 怎么排、payload 怎么流式复制、
// 恶意路径怎么拦，全部在 ArchiveWriter / ArchiveReader 里，
// 这样将来如果把归档层换成"归档 + 压缩"的组合，这个文件不用跟着改。

#include "backup_engine.h"

#include <string>

#include "archive.h"

namespace backupproject {

namespace {

// 统一写错误信息的小工具，省得每个失败分支都判一次空指针。
void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) {
    *error_message = text;
  }
}

}  // namespace

// 引擎本身没有状态，默认构造即可。
BackupEngine::BackupEngine() = default;

// 打包流程：验源目录 → 交给 ArchiveWriter。
// 归档文件自身的存在性、是否落在源目录内部这些检查由写入器负责，
// 它会把它们放在创建输出文件之前，保证拒绝时不留下任何文件系统改动。
bool BackupEngine::Backup(const std::string& source_directory,
                          const std::string& archive_file,
                          std::string* error_message) {
  // 不带筛选：空 Filter 与"没有规则"完全等价。
  const Filter no_filter;
  return Backup(source_directory, archive_file, no_filter, error_message);
}

bool BackupEngine::Backup(const std::string& source_directory,
                          const std::string& archive_file, const Filter& filter,
                          std::string* error_message) {
  // 先清掉上一次遗留的错误信息，避免调用方误读。
  if (error_message != nullptr) {
    error_message->clear();
  }

  if (archive_file.empty()) {
    SetError(error_message, "Backup file path is empty.");
    return false;
  }

  // 源必须是已存在的目录。不存在、以及"存在但只是普通文件"分开报错，
  // 消息里带上具体路径，方便用户定位。
  const FileSystem::PathStatus source_status =
      file_system_.InspectPath(source_directory, error_message);
  if (source_status == FileSystem::PathStatus::kError) {
    // 检查本身失败（比如权限不足），原因已经写在 error_message 里。
    return false;
  }
  if (source_status == FileSystem::PathStatus::kMissing) {
    SetError(error_message,
             "Source directory does not exist: " + source_directory);
    return false;
  }
  if (source_status != FileSystem::PathStatus::kDirectory) {
    SetError(error_message, "Source is not a directory: " + source_directory);
    return false;
  }

  const ArchiveWriter writer;
  return writer.Write(source_directory, archive_file, &filter, error_message);
}

// 这里刻意不做"先建目标目录再解包"的优化：目标目录一旦建出来，
// 后面任何一步失败都会在用户磁盘上留下痕迹。解包的两阶段校验
// （preflight + 写入）全部在 ArchiveReader 里，引擎只负责参数层。
// 归档内容的合法性（magic、每条 header、路径、payload 边界）由读取器在
// preflight 阶段整体校验，校验不过就不会碰目标目录。
bool BackupEngine::Restore(const std::string& archive_file,
                           const std::string& destination_directory,
                           std::string* error_message) {
  // 同样先清空错误信息，保持和 Backup 一致的约定。
  if (error_message != nullptr) {
    error_message->clear();
  }

  if (destination_directory.empty()) {
    SetError(error_message, "Destination directory is empty.");
    return false;
  }

  const FileSystem::PathStatus archive_status =
      file_system_.InspectPath(archive_file, error_message);
  if (archive_status == FileSystem::PathStatus::kError) {
    return false;
  }
  if (archive_status == FileSystem::PathStatus::kMissing) {
    SetError(error_message, "Backup file does not exist: " + archive_file);
    return false;
  }
  if (archive_status != FileSystem::PathStatus::kRegularFile) {
    SetError(error_message,
             "Backup file is not a regular file: " + archive_file);
    return false;
  }

  const ArchiveReader reader;
  return reader.Extract(archive_file, destination_directory, error_message);
}

}  // namespace backupproject
