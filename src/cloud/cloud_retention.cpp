#include "cloud_retention.h"

#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>

#include "remote_backup_client.h"

namespace backupproject {
namespace {

void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) *error_message = text;
}

}  // namespace

bool RunCloudRetention(const CloudRetentionRequest& request,
                       CloudRetentionResult* result,
                       std::string* error_message) {
  if (result == nullptr) {
    SetError(error_message, "retention 输出指针为空");
    return false;
  }
  *result = CloudRetentionResult{};
  if (request.retain_count == 0) {
    SetError(error_message, "retention 保留数量必须大于 0");
    return false;
  }
  if (!request.expected_repository_id.empty() &&
      request.expected_repository_id !=
          CloudRepositoryId(request.credentials)) {
    SetError(error_message, "云端凭据与任务保存的仓库标识不一致，拒绝淘汰");
    return false;
  }
  net::RemoteEndpoint endpoint;
  endpoint.host = request.credentials.host;
  endpoint.port = request.credentials.port;
  endpoint.identity_mode = request.credentials.identity_mode;
  endpoint.server_key_pin = request.credentials.server_key_pin;
  endpoint.trusted_roots_file = request.credentials.trusted_roots_file;
  endpoint.expected_server_id = request.credentials.expected_server_id;
  net::RemoteArchiveClient client;
  if (!client.Connect(endpoint, error_message)) return false;
  std::string password = request.credentials.password;
  const bool logged_in =
      client.Login(request.credentials.username, password, error_message);
  password.assign(password.size(), '\0');
  if (!logged_in) return false;
  std::vector<net::RemoteSnapshotInfo> snapshots;
  if (!client.List(&snapshots, error_message)) return false;

  std::map<std::string, net::RemoteSnapshotInfo> by_id;
  std::map<std::string, std::string> id_by_name;
  for (const auto& snapshot : snapshots) {
    if (!by_id.emplace(snapshot.snapshot_id, snapshot).second) {
      SetError(error_message, "远端快照 id 重复，拒绝淘汰");
      return false;
    }
    if (snapshot.display_name.empty()) {
      SetError(error_message, "远端快照缺少显示名，拒绝淘汰");
      return false;
    }
    if (!id_by_name.emplace(snapshot.display_name, snapshot.snapshot_id)
             .second) {
      SetError(error_message, "远端存在重名快照，无法确认归属，拒绝淘汰");
      return false;
    }
  }
  for (const auto& snapshot : snapshots) {
    if (!snapshot.parent_snapshot_id.empty() &&
        by_id.find(snapshot.parent_snapshot_id) == by_id.end()) {
      SetError(error_message, "远端快照缺少父快照信息，拒绝淘汰");
      return false;
    }
  }

  std::set<std::string> owned_names(request.owned_archive_names.begin(),
                                    request.owned_archive_names.end());
  std::vector<net::RemoteSnapshotInfo> owned;
  for (const auto& snapshot : snapshots) {
    if (owned_names.count(snapshot.display_name) > 0) {
      owned.push_back(snapshot);
    }
  }
  std::sort(
      owned.begin(), owned.end(),
      [](const net::RemoteSnapshotInfo& a, const net::RemoteSnapshotInfo& b) {
        if (a.created_at != b.created_at) return a.created_at > b.created_at;
        return a.snapshot_id > b.snapshot_id;
      });
  if (owned.size() <= request.retain_count) {
    for (const auto& snapshot : owned)
      result->kept.push_back(snapshot.display_name);
    return true;
  }

  std::set<std::string> keep_ids;
  for (std::size_t i = 0; i < request.retain_count; ++i) {
    keep_ids.insert(owned[i].snapshot_id);
    result->kept.push_back(owned[i].display_name);
  }
  // 所有保留点的祖先都保留；祖先可以是别的任务/手工上传的快照。
  for (const std::string& id :
       std::vector<std::string>(keep_ids.begin(), keep_ids.end())) {
    std::string cursor = id;
    std::set<std::string> path;
    while (!cursor.empty()) {
      if (!path.insert(cursor).second) {
        SetError(error_message, "远端依赖链存在环，拒绝淘汰");
        return false;
      }
      const auto found = by_id.find(cursor);
      if (found == by_id.end()) break;
      const std::string& parent = found->second.parent_snapshot_id;
      if (parent.empty()) break;
      if (keep_ids.insert(parent).second) {
        result->protected_ancestors.push_back(by_id.at(parent).display_name);
      }
      cursor = parent;
    }
  }

  std::set<std::string> delete_ids;
  for (const auto& snapshot : owned) {
    if (keep_ids.count(snapshot.snapshot_id) == 0) {
      delete_ids.insert(snapshot.snapshot_id);
    }
  }
  // 任何仍有保留/非候选子节点的候选都不能删。
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto& snapshot : snapshots) {
      if (snapshot.parent_snapshot_id.empty()) continue;
      if (delete_ids.count(snapshot.parent_snapshot_id) > 0 &&
          delete_ids.count(snapshot.snapshot_id) == 0) {
        delete_ids.erase(snapshot.parent_snapshot_id);
        keep_ids.insert(snapshot.parent_snapshot_id);
        changed = true;
      }
    }
  }

  std::vector<std::string> order;
  for (const auto& id : delete_ids) order.push_back(id);
  std::sort(order.begin(), order.end(),
            [&](const std::string& a, const std::string& b) {
              return by_id.at(a).created_at < by_id.at(b).created_at;
            });
  for (const auto& id : order) {
    result->dry_run_delete.push_back(by_id.at(id).display_name);
  }
  if (request.dry_run) return true;

  // 叶到根：每轮只删当前没有子节点的候选。
  while (!delete_ids.empty()) {
    bool progress = false;
    for (const auto& id :
         std::vector<std::string>(delete_ids.begin(), delete_ids.end())) {
      bool has_child = false;
      for (const auto& snapshot : snapshots) {
        if (snapshot.parent_snapshot_id == id &&
            delete_ids.count(snapshot.snapshot_id) > 0) {
          has_child = true;
          break;
        }
      }
      if (has_child) continue;
      std::string delete_error;
      if (!client.Delete(id, &delete_error)) {
        SetError(error_message, "删除云端快照失败：" + delete_error);
        return false;
      }
      result->deleted.push_back(by_id.at(id).display_name);
      delete_ids.erase(id);
      progress = true;
    }
    if (!progress) {
      SetError(error_message, "依赖关系阻止了安全删除，已停止");
      return false;
    }
  }
  return true;
}

}  // namespace backupproject
