// include/cloud_task.h
//
// 云端目标仓库的执行层：读取 0600 凭据文件、建立与远程备份相同的 BPSEC 连接、
// 登录、准备远端缓存、调用共享的 RunRemoteBackup。GUI/定时/实时都调用这里，
// 不各自复制网络逻辑。
#ifndef BACKUP_PROJECT_INCLUDE_CLOUD_TASK_H_
#define BACKUP_PROJECT_INCLUDE_CLOUD_TASK_H_

#include <cstdint>
#include <string>
#include <vector>

#include "archive_pipeline.h"
#include "filter.h"
#include "remote_incremental.h"

namespace backupproject {

struct CloudCredentials {
  std::string host;
  std::uint16_t port = 0;
  std::string username;
  std::string password;
  // "pin" 或 "certificate"；与 RemoteEndpoint 的语义一致。
  std::string identity_mode = "pin";
  std::string server_key_pin;
  std::string trusted_roots_file;
  std::string expected_server_id;
};

// 读取 0600 凭据文件。格式是每行 key=value：
//   host=, port=, username=, password=
//   identity_mode=pin|certificate
//   pin= / roots= / server_id=
// 失败原因只包含路径与原因，绝不包含文件内容。
bool LoadCloudCredentialsFile(const std::string& path,
                              CloudCredentials* credentials,
                              std::string* error_message);

// 稳定仓库标识：cloud://host:port/username#pin:<前16位> 或
// #cert:<server_id>。不包含密码。
std::string CloudRepositoryId(const CloudCredentials& credentials);

struct CloudTaskRequest {
  CloudCredentials credentials;
  std::string expected_repository_id;
  std::string source_directory;
  Filter filter;
  BackupOptions options;
  bool allow_incremental = false;
  // 空 = 应用默认缓存目录。
  std::string cache_root;
};

struct CloudTaskOutcome {
  bool produced_delta = false;
  bool rebuilt_full_baseline = false;
  bool no_changes = false;
  std::string snapshot_id;
  std::string parent_snapshot_id;
  std::uint64_t generation = 0;
  std::string archive_name;
  std::uint64_t uploaded_bytes = 0;
};

bool RunCloudTask(const CloudTaskRequest& request, CloudTaskOutcome* outcome,
                  std::string* error_message);

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_CLOUD_TASK_H_
