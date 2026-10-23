// src/cloud/cloud_task.cpp
#include "cloud_task.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <vector>

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

// 从系统密钥库/密码管理器读取密码。命令由 0600 凭据文件所有者配置；
// 输出只进内存，不写日志，也不出现在我们自己的 argv 里。
bool RunPasswordCommand(const std::string& command, std::string* secret,
                        std::string* error_message) {
  if (command.empty()) {
    if (error_message != nullptr) *error_message = "password_command 为空";
    return false;
  }
  int pipe_fds[2];
  if (::pipe(pipe_fds) != 0) {
    if (error_message != nullptr) *error_message = "无法创建密码命令管道";
    return false;
  }
  const pid_t child = ::fork();
  if (child == 0) {
    ::setsid();
    ::close(pipe_fds[0]);
    if (::dup2(pipe_fds[1], STDOUT_FILENO) < 0) _exit(127);
    ::close(pipe_fds[1]);
    ::execl("/bin/sh", "sh", "-c", command.c_str(), nullptr);
    _exit(127);
  }
  ::close(pipe_fds[1]);
  if (child < 0) {
    ::close(pipe_fds[0]);
    if (error_message != nullptr) *error_message = "无法启动密码命令";
    return false;
  }
  std::string output;
  char buffer[1024];
  bool too_long = false;
  bool timed_out = false;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (true) {
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0) {
      timed_out = true;
      break;
    }
    struct pollfd poll_fd;
    poll_fd.fd = pipe_fds[0];
    poll_fd.events = POLLIN;
    poll_fd.revents = 0;
    const int polled = ::poll(&poll_fd, 1, static_cast<int>(remaining.count()));
    if (polled < 0 && errno == EINTR) continue;
    if (polled <= 0) {
      timed_out = true;
      break;
    }
    const ssize_t got = ::read(pipe_fds[0], buffer, sizeof(buffer));
    if (got < 0 && errno == EINTR) continue;
    if (got <= 0) break;
    if (output.size() + static_cast<std::size_t>(got) > 4096) {
      too_long = true;
      break;
    }
    output.append(buffer, static_cast<std::size_t>(got));
  }
  ::close(pipe_fds[0]);
  if (too_long || timed_out) {
    ::kill(-child, SIGKILL);
    int status = 0;
    ::waitpid(child, &status, 0);
    if (error_message != nullptr) {
      *error_message = too_long ? "密码命令输出过长" : "密码命令执行超时";
    }
    return false;
  }
  int status = 0;
  while (true) {
    const pid_t waited = ::waitpid(child, &status, WNOHANG);
    if (waited == child) break;
    if (waited < 0 && errno != EINTR) {
      if (error_message != nullptr) *error_message = "等待密码命令失败";
      return false;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      ::kill(-child, SIGKILL);
      while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
      }
      if (error_message != nullptr) *error_message = "密码命令执行超时";
      return false;
    }
    ::poll(nullptr, 0, 20);
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    if (error_message != nullptr) *error_message = "密码命令执行失败";
    return false;
  }
  while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
    output.pop_back();
  }
  if (output.empty() || output.find('\0') != std::string::npos) {
    if (error_message != nullptr) *error_message = "密码命令没有输出有效密码";
    return false;
  }
  *secret = output;
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
    } else if (key == "password_command") {
      parsed.password_command = value;
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
  if (parsed.host.empty() || parsed.port == 0 || parsed.username.empty()) {
    if (error_message != nullptr) {
      *error_message = "云端凭据文件缺少 host/port/username";
    }
    return false;
  }
  if (!parsed.password.empty() && !parsed.password_command.empty()) {
    if (error_message != nullptr) {
      *error_message = "password 与 password_command 只能二选一";
    }
    return false;
  }
  if (parsed.password.empty() && parsed.password_command.empty()) {
    if (error_message != nullptr) {
      *error_message = "云端凭据文件缺少 password 或 password_command";
    }
    return false;
  }
  if (!parsed.password_command.empty() &&
      !RunPasswordCommand(parsed.password_command, &parsed.password,
                          error_message)) {
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

bool IsRetryableCloudError(const std::string& error_message) {
  static const char* kPermanent[] = {
      "UNAUTHORIZED", "ALREADY_EXISTS", "认证", "凭据",     "密码",
      "pin",          "certificate",    "证书", "identity", "身份",
      "permission",   "拒绝访问",       "配置"};
  for (const char* token : kPermanent) {
    if (error_message.find(token) != std::string::npos) return false;
  }
  return true;
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
  outcome->sha256 = result.sha256;
  outcome->parent_snapshot_id = result.parent_snapshot_id;
  outcome->generation = result.generation;
  outcome->archive_name = result.archive_name;
  outcome->uploaded_bytes = result.uploaded_bytes;
  return true;
}

}  // namespace backupproject
