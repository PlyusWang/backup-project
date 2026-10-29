// 会话凭据的生命周期、落盘边界及高级文件兼容性。
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <string>

#include "backup_credentials.h"
#include "cloud_task.h"
#include "crypto.h"
#include "realtime_store.h"
#include "schedule_store.h"
#include "test_support.h"

namespace bp = backupproject;
namespace ts = test_support;

namespace {

constexpr const char* kArchiveReference =
    "/@backup-project-session/test-archive";

bool FreshProcessHasNoSecrets() {
  std::string secret;
  std::string error;
  bp::CloudCredentials cloud;
  return !bp::SessionCredentialAvailable(kArchiveReference) &&
         !bp::ReadCredentialFile(kArchiveReference, &secret, &error) &&
         secret.empty() && !bp::SessionCloudCredentialsAvailable() &&
         !bp::LoadCloudCredentialsFile(bp::kSessionCloudCredentialsReference,
                                       &cloud, &error) &&
         cloud.password.empty();
}

bool RandomPassword(std::string* password, std::string* error) {
  std::string bytes;
  if (!bp::crypto::RandomBytes(24, &bytes, error)) return false;
  static const char kHex[] = "0123456789abcdef";
  password->clear();
  for (unsigned char byte : bytes) {
    password->push_back(kHex[byte >> 4]);
    password->push_back(kHex[byte & 15]);
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string(argv[1]) == "--fresh-process") {
    return FreshProcessHasNoSecrets() ? 0 : 1;
  }
  char temporary[] = "/tmp/bp-session-credentials-XXXXXX";
  char* created = ::mkdtemp(temporary);
  if (created == nullptr) return 1;
  const std::string root(created);
  std::string error;
  std::string secret;
  std::string first_password;
  std::string second_password;
  if (!RandomPassword(&first_password, &error) ||
      !RandomPassword(&second_password, &error)) {
    ts::RemoveTree(root);
    return 1;
  }
  ts::Check(!bp::ReadCredentialFile(kArchiveReference, &secret, &error) &&
                secret.empty(),
            "locked session reference is rejected");
  const std::string locked_error = error;
  ts::Check(
      bp::SetSessionCredential(kArchiveReference, first_password, &error) &&
          bp::ValidateCredentialFile(kArchiveReference, &error) &&
          bp::ReadCredentialFile(kArchiveReference, &secret, &error) &&
          secret == first_password,
      "session unlock provides the exact archive password");
  ts::Check(
      bp::SetSessionCredential(kArchiveReference, second_password, &error) &&
          bp::ReadCredentialFile(kArchiveReference, &secret, &error) &&
          secret == second_password,
      "replacement password supersedes the previous password");
  ts::Check(
      !bp::SetSessionCredential(root + "/password", first_password, &error) &&
          !bp::SetSessionCredential(kArchiveReference, "", &error) &&
          !bp::SetSessionCredential(kArchiveReference, std::string("a\0b", 3),
                                    &error),
      "invalid references and invalid secrets are rejected");

  // 即使绕过同一目录的文件名，整个保留命名空间也只能查内存。
  const std::string file = root + "/advanced-password";
  ts::Check(ts::WriteFile(file, first_password + "\n", 0600) &&
                bp::ReadCredentialFile(file, &secret, &error) &&
                secret == first_password,
            "advanced 0600 archive password file remains supported");
  const std::string reserved_file =
      std::string(bp::kSessionCredentialPrefix) + "../.." + file;
  ts::Check(
      !bp::ReadCredentialFile(reserved_file, &secret, &error) &&
          secret.empty() && error == locked_error,
      "reserved references use the session lookup before file inspection");
  ::chmod(file.c_str(), 0644);
  ts::Check(!bp::ReadCredentialFile(file, &secret, &error) && secret.empty(),
            "advanced password files retain permission checks");

  bp::CloudCredentials cloud;
  cloud.host = "127.0.0.1";
  cloud.port = 32100;
  cloud.username = "synthetic-user";
  cloud.password = first_password;
  cloud.server_key_pin = "sha256:11111111111111111111111111111111";
  cloud.repository_host = "ssh/synthetic-server/127.0.0.1";
  cloud.repository_port = 18765;
  ts::Check(bp::SetSessionCloudCredentials(cloud, &error) &&
                bp::SessionCloudCredentialsAvailable(),
            "authenticated cloud connection can be shared in memory");
  bp::CloudCredentials loaded;
  ts::Check(bp::LoadCloudCredentialsFile(bp::kSessionCloudCredentialsReference,
                                         &loaded, &error) &&
                loaded.host == cloud.host && loaded.port == cloud.port &&
                loaded.password == first_password,
            "cloud session reads a complete value snapshot");
  loaded.password = second_password;
  ts::Check(bp::LoadCloudCredentialsFile(bp::kSessionCloudCredentialsReference,
                                         &loaded, &error) &&
                loaded.password == first_password,
            "callers cannot mutate the registered cloud credentials");
  const std::string repository_id = bp::CloudRepositoryId(cloud);
  cloud.port = 32101;
  ts::Check(bp::CloudRepositoryId(cloud) == repository_id,
            "SSH local port changes preserve logical repository identity");
  cloud.username = "another-user";
  ts::Check(bp::CloudRepositoryId(cloud) != repository_id,
            "account changes produce a different repository identity");
  cloud.username = "synthetic-user";
  cloud.server_key_pin = "sha256:22222222222222222222222222222222";
  ts::Check(bp::CloudRepositoryId(cloud) != repository_id,
            "server identity changes produce a different repository identity");

  bp::ScheduleDocument document;
  document.config.source_path = root;
  document.config.encryption_method =
      bp::EncryptionMethod::kAes256CtrHmacSha256;
  document.config.encryption_password_file = kArchiveReference;
  document.config.repository_type = "cloud";
  document.config.cloud_credentials_file =
      bp::kSessionCloudCredentialsReference;
  document.config.cloud_repository_id = repository_id;
  bp::ScheduleStore schedule(root + "/schedule.json");
  bp::RealtimeConfig realtime;
  realtime.source_path = root;
  realtime.encryption_method = document.config.encryption_method;
  realtime.encryption_password_file = kArchiveReference;
  realtime.repository_type = "cloud";
  realtime.cloud_credentials_file = bp::kSessionCloudCredentialsReference;
  realtime.cloud_repository_id = repository_id;
  bp::RealtimeStore realtime_store(root + "/realtime.json");
  std::string schedule_text;
  std::string realtime_text;
  ts::Check(schedule.Save(document, &error) &&
                realtime_store.Save(realtime, &error) &&
                ts::ReadFile(schedule.schedule_file_path(), &schedule_text) &&
                ts::ReadFile(realtime_store.file_path(), &realtime_text) &&
                schedule_text.find(kArchiveReference) != std::string::npos &&
                realtime_text.find(kArchiveReference) != std::string::npos &&
                schedule_text.find(first_password) == std::string::npos &&
                schedule_text.find(second_password) == std::string::npos &&
                realtime_text.find(first_password) == std::string::npos &&
                realtime_text.find(second_password) == std::string::npos,
            "automatic task stores persist references without passwords");
  bp::ScheduleDocument saved_document;
  bp::RealtimeConfig saved_realtime;
  ts::Check(
      schedule.Load(&saved_document, &error) ==
              bp::ScheduleLoadStatus::kLoaded &&
          realtime_store.Load(&saved_realtime, &error) ==
              bp::RealtimeLoadStatus::kLoaded &&
          saved_document.config.encryption_password_file == kArchiveReference &&
          saved_realtime.encryption_password_file == kArchiveReference,
      "session reference configuration round trips through both stores");

  const pid_t child = ::fork();
  if (child == 0) {
    ::execl("/proc/self/exe", "/proc/self/exe", "--fresh-process", nullptr);
    ::_exit(127);
  }
  int status = 0;
  ts::Check(child > 0 && ::waitpid(child, &status, 0) == child &&
                WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "a fresh process requires archive unlock and cloud login again");

  bp::ClearSessionCredential(kArchiveReference);
  ts::Check(!bp::ReadCredentialFile(kArchiveReference, &secret, &error) &&
                !bp::ValidateCredentialFile(kArchiveReference, &error) &&
                secret.empty() &&
                error.find(first_password) == std::string::npos &&
                error.find(second_password) == std::string::npos,
            "locking removes the archive secret and never echoes it");
  bp::ClearSessionCloudCredentials();
  ts::Check(!bp::SessionCloudCredentialsAvailable() &&
                bp::SessionCloudRepositoryId().empty() &&
                !bp::LoadCloudCredentialsFile(
                    bp::kSessionCloudCredentialsReference, &loaded, &error) &&
                loaded.password.empty() &&
                error.find(first_password) == std::string::npos,
            "logout clears shared credentials and output values");
  ts::Check(!bp::LoadCloudCredentialsFile(kArchiveReference, &loaded, &error),
            "archive and cloud credential namespaces remain distinct");
  ts::RemoveTree(root);
  std::printf("session-credentials-test: %d/%d checks passed\n",
              ts::CheckCount() - ts::FailureCount(), ts::CheckCount());
  return ts::FailureCount() == 0 ? 0 : 1;
}
