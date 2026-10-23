// src/cloud/cloud_task.cpp
#include "cloud_task.h"

#include <cstdlib>
#include <sstream>

#include "backup_credentials.h"
#include "remote_backup_client.h"

namespace backupproject {
namespace {

bool ParsePort(const std::string& text, std::uint16_t* port) {
  if (text.empty()) return false;
  std::size_t consumed = 0;
  unsigned long value = 0;
  try {
    value = std::stoul(text, &consumed, 10);
  } catch (...) {
    return false;
  }
  if (consumed != text.size() || value == 0 || value > 65535) return false;
  *port = static_cast<std::uint16_t>(value);
  return true;
}

}  // namespace

bool LoadCloudCredentialsFile(const std::string& path,
                              CloudCredentials* credentials,
                              std::string* error_message) {
  if (credentials == nullptr) {
    if (error_message != nullptr) *error_message = "凭据输出指针为空";
    return false;
  }
  std::string content;
  if (!ReadCredentialFile(path, &content, error_message)) return false;

  CloudCredentials parsed;
  std::istringstream stream(content);
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    const std::size_t equals = line.find('=');
    if (equals == std::string::npos || equals == 0) {
      if (error_message != nullptr) {
        *error_message = "云端凭据文件包含无法识别的行";
      }
      return false;
    }
    const std::string key = line.substr(0, equals);
    const std::string value = line.substr(equals + 1);
    if (key == "host") {
      parsed.host = value;
    } else if (key == "port") {
      if (!ParsePort(value, &parsed.port)) {
        if (error_message != nullptr) *error_message = "云端凭据文件端口非法";
        return false;
      }
    } else if (key == "username") {
      parsed.username = value;
    } else if (key == "password") {
      parsed.password = value;
    } else if (key == "identity_mode") {
      parsed.identity_mode = value;
    } else if (key == "pin") {
      parsed.server_key_pin = value;
    } else if (key == "roots") {
      parsed.trusted_roots_file = value;
    } else if (key == "server_id") {
      parsed.expected_server_id = value;
    } else {
      if (error_message != nullptr) {
        *error_message = "云端凭据文件包含未知字段：" + key;
      }
      return false;
    }
  }
  if (parsed.host.empty() || parsed.port == 0 || parsed.username.empty() ||
      parsed.password.empty()) {
    if (error_message != nullptr) {
      *error_message = "云端凭据文件缺少 host/port/username/password";
    }
    return false;
  }
  if (parsed.identity_mode != "pin" && parsed.identity_mode != "certificate") {
    if (error_message != nullptr)
      *error_message = "云端身份方式必须是 pin 或 certificate";
    return false;
  }
  if (parsed.identity_mode == "pin" && parsed.server_key_pin.empty()) {
    if (error_message != nullptr) *error_message = "pin 模式必须提供 pin";
    return false;
  }
  if (parsed.identity_mode == "certificate" &&
      parsed.expected_server_id.empty()) {
    if (error_message != nullptr) {
      *error_message = "certificate 模式必须提供 server_id";
    }
    return false;
  }
  // 不清空 password：调用方在登录后自行抹掉；这里只保证错误信息不含它。
  *credentials = parsed;
  return true;
}

std::string CloudRepositoryId(const CloudCredentials& credentials) {
  std::string id = "cloud://" + credentials.host + ":" +
                   std::to_string(credentials.port) + "/" +
                   credentials.username;
  if (credentials.identity_mode == "certificate") {
    id += "#cert:" + credentials.expected_server_id;
  } else {
    std::string pin = credentials.server_key_pin;
    const std::size_t colon = pin.find(':');
    if (colon != std::string::npos) pin = pin.substr(colon + 1);
    if (pin.size() > 16) pin = pin.substr(0, 16);
    id += "#pin:" + pin;
  }
  return id;
}

bool RunCloudTask(const CloudTaskRequest& request, CloudTaskOutcome* outcome,
                  std::string* error_message) {
  if (outcome == nullptr) {
    if (error_message != nullptr) *error_message = "云端任务输出指针为空";
    return false;
  }
  *outcome = CloudTaskOutcome{};
  if (!request.expected_repository_id.empty() &&
      request.expected_repository_id !=
          CloudRepositoryId(request.credentials)) {
    if (error_message != nullptr) {
      *error_message =
          "云端凭据与任务保存的仓库标识不一致，已拒绝执行（避免任务悄悄换库）";
    }
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
  net::RemoteCacheLayout cache;
  if (!net::PrepareRemoteCache(request.cache_root, client.server_fingerprint(),
                               request.credentials.username, &cache,
                               error_message)) {
    return false;
  }
  net::RemoteBackupRequest backup;
  backup.client = &client;
  backup.cache = cache;
  backup.source_directory = request.source_directory;
  backup.filter = request.filter;
  backup.options = request.options;
  backup.allow_incremental = request.allow_incremental;
  net::RemoteBackupOutcome result;
  if (!net::RunRemoteBackup(backup, &result, error_message)) return false;
  outcome->produced_delta = result.produced_delta;
  outcome->rebuilt_full_baseline = result.rebuilt_full_baseline;
  outcome->no_changes = result.no_changes;
  outcome->snapshot_id = result.snapshot_id;
  outcome->parent_snapshot_id = result.parent_snapshot_id;
  outcome->generation = result.generation;
  outcome->archive_name = result.archive_name;
  outcome->uploaded_bytes = result.uploaded_bytes;
  return true;
}

}  // namespace backupproject
