// src/core/backup_credentials.cpp

#include "backup_credentials.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>

namespace backupproject {
namespace {

void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) *error_message = text;
}

bool ContainsNul(const std::string& text) {
  return text.find('\0') != std::string::npos;
}

void WipeSecret(std::string* secret) {
  volatile char* bytes = secret->empty() ? nullptr : &(*secret)[0];
  for (std::size_t i = 0; i < secret->size(); ++i) bytes[i] = '\0';
  secret->clear();
}

struct SessionCredentials {
  std::mutex mutex;
  std::map<std::string, std::string> secrets;
  ~SessionCredentials() {
    for (auto& item : secrets) WipeSecret(&item.second);
  }
};

SessionCredentials& Credentials() {
  static SessionCredentials credentials;
  return credentials;
}

bool ReadSessionCredential(const std::string& reference, std::string* secret,
                           std::string* error_message) {
  auto& credentials = Credentials();
  std::lock_guard<std::mutex> lock(credentials.mutex);
  const auto found = credentials.secrets.find(reference);
  if (found == credentials.secrets.end()) {
    SetError(error_message, "归档密码尚未解锁，请在本次程序会话中重新输入密码");
    return false;
  }
  if (secret != nullptr) *secret = found->second;
  return true;
}

// lstat 而不是 stat：符号链接必须被明确拒绝，而不是顺着链接去检查目标。
// 否则用户可以把一个 0600 的普通文件链接到任意位置，让程序去读一个它
// 本来无权读的对象——那一步是链接目标的所有者决定的，不是凭据文件决定的。
bool InspectCredentialFile(const std::string& path, struct stat* info,
                           std::string* error_message) {
  if (path.empty()) {
    SetError(error_message, "Credential file path is empty");
    return false;
  }
  if (::lstat(path.c_str(), info) != 0) {
    SetError(error_message, "Cannot read the credential file '" + path +
                                "': " + std::strerror(errno));
    return false;
  }
  if (S_ISLNK(info->st_mode)) {
    SetError(error_message,
             "The credential file '" + path +
                 "' is a symbolic link; use a regular file owned by you");
    return false;
  }
  if (!S_ISREG(info->st_mode)) {
    SetError(error_message,
             "The credential file '" + path + "' is not a regular file");
    return false;
  }
  if (info->st_uid != ::geteuid()) {
    SetError(error_message, "The credential file '" + path +
                                "' is not owned by the current user");
    return false;
  }
  if ((info->st_mode & 0077) != 0) {
    SetError(error_message,
             "The credential file '" + path +
                 "' is accessible by group or others; run chmod 600 on it");
    return false;
  }
  if ((info->st_mode & 0400) == 0) {
    SetError(error_message,
             "The credential file '" + path + "' is not readable by its owner");
    return false;
  }
  if (info->st_size <= 0 ||
      info->st_size > static_cast<off_t>(kMaxCredentialFileBytes)) {
    SetError(error_message,
             "The credential file '" + path + "' must contain between 1 and " +
                 std::to_string(kMaxCredentialFileBytes) + " bytes");
    return false;
  }
  return true;
}

}  // namespace

bool IsSessionCredentialReference(const std::string& path) {
  return path.rfind(kSessionCredentialPrefix, 0) == 0;
}

bool SetSessionCredential(const std::string& reference,
                          const std::string& secret,
                          std::string* error_message) {
  if (error_message != nullptr) error_message->clear();
  if (!IsSessionCredentialReference(reference) ||
      !IsUsableCredentialPath(reference, error_message)) {
    SetError(error_message, "无效的会话凭据引用");
    return false;
  }
  if (secret.empty() || secret.size() > kMaxCredentialFileBytes ||
      ContainsNul(secret)) {
    SetError(error_message, "密码不能为空、过长或包含空字符");
    return false;
  }
  auto& credentials = Credentials();
  std::lock_guard<std::mutex> lock(credentials.mutex);
  auto& stored = credentials.secrets[reference];
  WipeSecret(&stored);
  stored = secret;
  return true;
}

void ClearSessionCredential(const std::string& reference) {
  auto& credentials = Credentials();
  std::lock_guard<std::mutex> lock(credentials.mutex);
  const auto found = credentials.secrets.find(reference);
  if (found != credentials.secrets.end()) {
    WipeSecret(&found->second);
    credentials.secrets.erase(found);
  }
}

bool SessionCredentialAvailable(const std::string& reference) {
  return ReadSessionCredential(reference, nullptr, nullptr);
}

bool IsUsableCredentialPath(const std::string& path,
                            std::string* error_message) {
  if (path.empty()) {
    SetError(error_message, "Credential file path is empty");
    return false;
  }
  if (path.size() > 4096) {
    SetError(error_message, "Credential file path is too long");
    return false;
  }
  if (ContainsNul(path)) {
    SetError(error_message, "Credential file path contains a NUL byte");
    return false;
  }
  if (path.front() != '/') {
    SetError(error_message,
             "Credential file path must be absolute: '" + path + "'");
    return false;
  }
  return true;
}

bool ValidateCredentialFile(const std::string& path,
                            std::string* error_message) {
  if (error_message != nullptr) error_message->clear();
  if (IsSessionCredentialReference(path)) {
    return ReadSessionCredential(path, nullptr, error_message);
  }
  struct stat info;
  if (!InspectCredentialFile(path, &info, error_message)) return false;
  return true;
}

bool ReadCredentialFile(const std::string& path, std::string* secret,
                        std::string* error_message) {
  if (secret == nullptr) {
    SetError(error_message, "Credential output must not be null");
    return false;
  }
  secret->clear();
  if (error_message != nullptr) error_message->clear();
  // 会话引用永不退回文件系统，即使存在同名文件也不能作为秘密来源。
  if (IsSessionCredentialReference(path)) {
    return ReadSessionCredential(path, secret, error_message);
  }
  struct stat info;
  if (!InspectCredentialFile(path, &info, error_message)) return false;

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    SetError(error_message, "Cannot open the credential file '" + path + "'");
    return false;
  }
  std::string content((std::istreambuf_iterator<char>(input)),
                      std::istreambuf_iterator<char>());
  if (input.bad()) {
    SetError(error_message, "Cannot read the credential file '" + path + "'");
    return false;
  }
  if (content.size() > kMaxCredentialFileBytes) {
    SetError(error_message,
             "The credential file '" + path + "' grew beyond the allowed size");
    return false;
  }
  if (content.find('\0') != std::string::npos) {
    SetError(error_message,
             "The credential file '" + path + "' contains a NUL byte");
    return false;
  }
  // 只去掉**一个**行尾换行（含 CRLF）：多行文件不是口令，不能被悄悄截断成
  // 第一行；"口令末尾恰好是换行"这种情况只能通过文件里多写一个换行来表达。
  if (!content.empty() && content.back() == '\n') {
    content.pop_back();
    if (!content.empty() && content.back() == '\r') content.pop_back();
  }
  if (content.empty()) {
    SetError(error_message, "The credential file '" + path + "' is empty");
    return false;
  }
  *secret = std::move(content);
  return true;
}

}  // namespace backupproject
