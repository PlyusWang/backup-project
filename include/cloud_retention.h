#ifndef BACKUP_PROJECT_INCLUDE_CLOUD_RETENTION_H_
#define BACKUP_PROJECT_INCLUDE_CLOUD_RETENTION_H_

#include <cstddef>
#include <string>
#include <vector>

#include "cloud_task.h"

namespace backupproject {

struct CloudRetentionRequest {
  CloudCredentials credentials;
  std::string expected_repository_id;
  std::size_t retain_count = 0;
  // 只删除这些稳定 snapshot_id 对应的快照；sha256 必须与远端对象一致。
  // 显示名称不参与归属判定。旧版只有名称的记录不进入自动删除。
  std::vector<std::string> owned_snapshot_ids;
  std::vector<std::string> owned_sha256;
  std::string cache_root;
  bool dry_run = false;
};

struct CloudRetentionResult {
  std::vector<std::string> kept;
  std::vector<std::string> deleted;
  std::vector<std::string> protected_ancestors;
  std::vector<std::string> dry_run_delete;
};

// 列出远端快照 -> 仅取 owned_archive_names -> 依赖感知保留 -> 叶到根删除。
// 缺少父快照信息、重名显示名、仓库标识不一致时直接失败，绝不猜测。
bool RunCloudRetention(const CloudRetentionRequest& request,
                       CloudRetentionResult* result,
                       std::string* error_message);

}  // namespace backupproject

#endif
