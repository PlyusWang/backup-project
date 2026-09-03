// backup_engine.cpp
//
// v0.1 的流程很简单：backup = 验源目录 + 建仓库 + 整树复制到
// <repository>/data；restore = 验仓库 + 把 data 整树复制到目标。
// 真正动文件系统的是 FileSystem，这里只决定“什么时候允许复制”。

#include "backup_engine.h"

#include <string>

namespace backupproject {

namespace {

// 仓库里真正放备份内容的目录名。后续版本会在旁边加清单、元数据，
// 所以从 v0.1 就把名字固定下来，而不是到处写 "data"。
constexpr const char* kDataDirectoryName = "data";

// 统一写错误信息的小工具，省得每个失败分支都判一次空指针。
void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) {
    *error_message = text;
  }
}

}  // namespace

// 引擎本身没有状态，默认构造即可；实际工作全靠成员 FileSystem 完成。
BackupEngine::BackupEngine() = default;

// 流程详见 backup_engine.h，这里拆成三步：验源、备仓库、复制。
bool BackupEngine::Backup(const std::string& source_directory,
                          const std::string& repository,
                          std::string* error_message) {
  // 先清掉上一次遗留的错误信息，避免调用方误读。
  if (error_message != nullptr) {
    error_message->clear();
  }

  // 源必须是已存在的目录。不存在、以及“存在但只是普通文件”分开报错，
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

  // 仓库可以顺手建出来，但里面已有内容时绝不覆盖。
  if (!file_system_.MakeDirectories(repository, error_message)) {
    return false;
  }

  // 仓库和数据目录的路径统一由 JoinPath 拼，不手写 '/'。
  const std::string data_directory =
      FileSystem::JoinPath(repository, kDataDirectoryName);
  // repository/data 已经有内容就直接拒绝。v0.1 不做覆盖或合并，
  // 避免一次误操作把旧备份冲掉。空目录例外：里面没有可丢的东西，
  // 允许直接复用。
  if (!file_system_.IsMissingOrEmptyDirectory(data_directory, error_message)) {
    if (error_message == nullptr || error_message->empty()) {
      SetError(error_message,
               "Repository data directory already exists and is not empty: " +
                   data_directory);
    }
    return false;
  }

  // 前置检查全部通过，剩下就是把整棵树复制过去。
  return file_system_.CopyTree(source_directory, data_directory, error_message);
}

// restore 是 backup 的镜像：先验仓库，再验目标，最后整树复制。
bool BackupEngine::Restore(const std::string& repository,
                           const std::string& destination,
                           std::string* error_message) {
  // 同样先清空错误信息，保持和 Backup 一致的约定。
  if (error_message != nullptr) {
    error_message->clear();
  }

  // 仓库必须已经存在，restore 不会替用户补建仓库。
  const FileSystem::PathStatus repository_status =
      file_system_.InspectPath(repository, error_message);
  if (repository_status == FileSystem::PathStatus::kError) {
    // 原因已经由 InspectPath 写好，直接失败。
    return false;
  }
  if (repository_status == FileSystem::PathStatus::kMissing) {
    SetError(error_message, "Repository does not exist: " + repository);
    return false;
  }
  if (repository_status != FileSystem::PathStatus::kDirectory) {
    SetError(error_message, "Repository is not a directory: " + repository);
    return false;
  }

  const std::string data_directory =
      FileSystem::JoinPath(repository, kDataDirectoryName);
  // data 目录也必须已经存在，而且必须是目录：不存在、或者被换成
  // 普通文件，都要明确报错。
  const FileSystem::PathStatus data_status =
      file_system_.InspectPath(data_directory, error_message);
  if (data_status == FileSystem::PathStatus::kError) {
    return false;
  }
  if (data_status == FileSystem::PathStatus::kMissing) {
    SetError(error_message,
             "Repository data directory does not exist: " + data_directory);
    return false;
  }
  if (data_status != FileSystem::PathStatus::kDirectory) {
    SetError(error_message,
             "Repository data directory is not a directory: " + data_directory);
    return false;
  }

  // destination 已存在且非空时拒绝，避免覆盖用户文件。
  // 和备份侧一样：空目录可以复用。
  if (!file_system_.IsMissingOrEmptyDirectory(destination, error_message)) {
    if (error_message == nullptr || error_message->empty()) {
      SetError(error_message,
               "Destination directory already exists and is not empty: " +
                   destination);
    }
    return false;
  }

  // 前置检查全部通过，整树复制到目标。
  return file_system_.CopyTree(data_directory, destination, error_message);
}

}  // namespace backupproject
