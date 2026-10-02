// server/remote_maintenance.cpp
//
// 见 include/remote_maintenance.h。

#include "remote_maintenance.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

#include "crypto.h"

namespace backupproject {
namespace net {
namespace {

std::string StrerrorText() { return std::string(std::strerror(errno)); }

void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr && error_message->empty()) {
    *error_message = text;
  }
}

// rename 的持久性要靠父目录 fsync 才算完整：只 fsync 文件本身，掉电后可能
// 留下"文件内容在、目录项没落盘"的状态。（与 remote_server.cpp 同一条规则。）
void FsyncDirectory(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd < 0) {
    return;
  }
  ::fsync(fd);
  ::close(fd);
}

}  // namespace

void RemoteMaintenance::Log(const std::string& message) {
  if (log_) {
    log_(message);
  }
}

bool RemoteMaintenance::EnsureDirectory(const std::string& path,
                                        std::string* error_message) {
  if (::mkdir(path.c_str(), 0700) == 0) {
    return true;
  }
  if (errno == EEXIST) {
    struct stat info;
    if (::stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode)) {
      return true;
    }
  }
  SetError(error_message,
           "cannot create the directory " + path + ": " + StrerrorText());
  return false;
}

std::string RemoteMaintenance::UserDirectory(std::int64_t user_id) const {
  return root_directory_ + "/users/" + std::to_string(user_id);
}

std::string RemoteMaintenance::LockFilePath(
    const std::string& root_directory) {
  return root_directory + "/.backup-server.lock";
}

std::string RemoteMaintenance::ReadLockHint(
    const std::string& root_directory) {
  const int fd =
      ::open(LockFilePath(root_directory).c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return std::string();
  }
  std::string hint;
  char buffer[256];
  const ssize_t count = ::read(fd, buffer, sizeof(buffer));
  if (count > 0) {
    hint.assign(buffer, static_cast<std::size_t>(count));
  }
  ::close(fd);
  while (!hint.empty() && (hint.back() == '\n' || hint.back() == '\r')) {
    hint.pop_back();
  }
  return hint;
}

bool RemoteMaintenance::RemoveDirectoryTree(const std::string& path,
                                            std::string* error_message) {
  struct stat info;
  if (::lstat(path.c_str(), &info) != 0) {
    if (errno == ENOENT) {
      return true;
    }
    SetError(error_message,
             "cannot inspect " + path + ": " + StrerrorText());
    return false;
  }
  if (!S_ISDIR(info.st_mode)) {
    // 普通文件或符号链接：只删这一个名字，绝不跟进去（跟进去就等于让一个
    // 链接把删除动作引到目录树之外）。
    if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
      SetError(error_message,
               "cannot remove " + path + ": " + StrerrorText());
      return false;
    }
    return true;
  }
  DIR* directory = ::opendir(path.c_str());
  if (directory == nullptr) {
    SetError(error_message,
             "cannot open " + path + ": " + StrerrorText());
    return false;
  }
  bool ok = true;
  for (;;) {
    struct dirent* entry = ::readdir(directory);
    if (entry == nullptr) {
      break;
    }
    const std::string name = entry->d_name;
    if (name == "." || name == "..") {
      continue;
    }
    if (!RemoveDirectoryTree(path + "/" + name, error_message)) {
      ok = false;
    }
  }
  ::closedir(directory);
  if (::rmdir(path.c_str()) != 0 && errno != ENOENT) {
    SetError(error_message,
             "cannot remove the directory " + path + ": " + StrerrorText());
    ok = false;
  }
  return ok;
}

StoreResult RemoteMaintenance::DeleteSnapshot(std::int64_t user_id,
                                              const std::string& snapshot_id,
                                              RemoteSnapshotRecord* removed,
                                              std::string* error_message) {
  if (!ready()) {
    SetError(error_message, "the maintenance layer is not configured");
    return StoreResult::kError;
  }
  // 查询自带 user_id 过滤：别人的快照与不存在的快照在这里是同一个答案。
  RemoteSnapshotRecord record;
  std::string store_error;
  const StoreResult found =
      store_->FindSnapshot(user_id, snapshot_id, &record, &store_error);
  if (found == StoreResult::kNotFound) {
    SetError(error_message, "no such snapshot for this user");
    return StoreResult::kNotFound;
  }
  if (found != StoreResult::kOk) {
    SetError(error_message, "cannot read the snapshot row: " + store_error);
    return StoreResult::kError;
  }
  // 纵深防御：storage_name 是服务端自己写进去的，但仍然确认它是个纯文件名。
  if (record.storage_name.empty() ||
      record.storage_name.find('/') != std::string::npos ||
      record.storage_name.find('\\') != std::string::npos ||
      record.storage_name != record.snapshot_id + ".bak") {
    SetError(error_message,
             "refusing to touch a snapshot whose storage name is not"
             " canonical");
    return StoreResult::kError;
  }
  const std::string directory = UserDirectory(user_id);
  const std::string final_path = directory + "/" + record.storage_name;
  std::string raw;
  std::string random_error;
  if (!crypto::RandomBytes(8, &raw, &random_error)) {
    SetError(error_message, "cannot generate a pending name: " + random_error);
    return StoreResult::kError;
  }
  const std::string pending_id = crypto::ToHex(
      reinterpret_cast<const unsigned char*>(raw.data()), raw.size());
  if (!EnsureDirectory(directory + "/trash", error_message)) {
    return StoreResult::kError;
  }
  const std::string pending_path = directory + "/trash/" + record.storage_name +
                                   "." + pending_id + ".deleted";

  // ---- 删除顺序：先把 blob 挪成不可见，再删元数据，最后物理删除 ----
  //
  // 直接 unlink 再删记录，会留下"文件没了、记录还在"的窗口；反过来先删记录
  // 再 unlink，又会在 unlink 失败时把文件变成谁也看不见的孤儿。
  // 先 rename 到 trash：任何一步失败都能把它改回来。
  if (::rename(final_path.c_str(), pending_path.c_str()) != 0) {
    if (errno == ENOENT) {
      SetError(error_message, "the blob is missing on disk");
      return StoreResult::kNotFound;
    }
    SetError(error_message, "cannot move the blob to trash: " + StrerrorText());
    return StoreResult::kError;
  }
  RemoteSnapshotRecord deleted;
  const StoreResult remove_result =
      store_->DeleteSnapshot(user_id, snapshot_id, &deleted, &store_error);
  if (remove_result != StoreResult::kOk) {
    // 回滚：把文件改回正式名字，一切照旧。
    Log("the metadata delete failed; restoring the blob: " + store_error);
    if (::rename(pending_path.c_str(), final_path.c_str()) != 0) {
      Log("warning: the rollback rename failed; the blob is left in trash");
    }
    SetError(error_message, remove_result == StoreResult::kNotFound
                                ? "the snapshot row disappeared during the"
                                  " delete"
                                : store_error);
    return remove_result == StoreResult::kNotFound ? StoreResult::kNotFound
                                                   : StoreResult::kError;
  }
  if (::unlink(pending_path.c_str()) != 0) {
    // 元数据已经不存在了，这个文件是"不可见的孤儿"：记录警告，留给将来的
    // startup reconciliation 清理，不因此把删除判为失败。
    Log("warning: could not unlink the pending blob; an invisible orphan"
        " remains in trash");
  }
  FsyncDirectory(directory);
  if (removed != nullptr) {
    *removed = deleted;
  }
  return StoreResult::kOk;
}

StoreResult RemoteMaintenance::DeleteAccount(std::int64_t user_id,
                                             std::uint64_t* removed_snapshots,
                                             std::uint64_t* removed_bytes,
                                             std::string* error_message) {
  if (!ready()) {
    SetError(error_message, "the maintenance layer is not configured");
    return StoreResult::kError;
  }
  const std::string directory = UserDirectory(user_id);
  std::string quarantine;
  bool quarantined = false;

  struct stat info;
  bool directory_exists = (::lstat(directory.c_str(), &info) == 0);
  if (directory_exists && !S_ISDIR(info.st_mode)) {
    SetError(error_message,
             "the account path exists but is not a directory: " + directory);
    return StoreResult::kError;
  }
  if (directory_exists) {
    if (!EnsureDirectory(root_directory_ + "/trash", error_message)) {
      return StoreResult::kError;
    }
    std::string raw;
    std::string random_error;
    if (!crypto::RandomBytes(8, &raw, &random_error)) {
      SetError(error_message,
               "cannot generate a quarantine name: " + random_error);
      return StoreResult::kError;
    }
    const std::string suffix = crypto::ToHex(
        reinterpret_cast<const unsigned char*>(raw.data()), raw.size());
    quarantine = root_directory_ + "/trash/account-" +
                 std::to_string(user_id) + "." + suffix + ".deleted";
    // 一步原子操作就让这个账户的全部字节变得不可见：目录被改名之后，
    // 没有任何路径能把 users/<id>/... 再解析出来。
    if (::rename(directory.c_str(), quarantine.c_str()) != 0) {
      if (errno != ENOENT) {
        SetError(error_message,
                 "cannot quarantine the account directory: " + StrerrorText());
        return StoreResult::kError;
      }
      // 另一个并发的注销动作刚好把它搬走了：继续走元数据删除，
      // 结果由事务决定（大概率是 kNotFound）。
      directory_exists = false;
    } else {
      quarantined = true;
    }
  }

  std::uint64_t snapshots = 0;
  std::uint64_t bytes = 0;
  std::string store_error;
  const StoreResult result =
      store_->DeleteUser(user_id, &snapshots, &bytes, &store_error);
  if (result != StoreResult::kOk) {
    if (quarantined) {
      Log("the metadata delete failed; restoring the account directory: " +
          store_error);
      if (::rename(quarantine.c_str(), directory.c_str()) != 0) {
        Log("warning: the rollback rename failed; the account data is left in"
            " the root trash");
      }
    }
    SetError(error_message, result == StoreResult::kNotFound
                                ? "no such user row"
                                : store_error);
    return result;
  }
  if (quarantined) {
    std::string cleanup_error;
    if (!RemoveDirectoryTree(quarantine, &cleanup_error)) {
      // 元数据已经不存在了：这些字节对客户端不可见，只是还占着磁盘。
      Log("warning: could not remove the quarantined account directory " +
          quarantine + ": " + cleanup_error);
    }
  }
  FsyncDirectory(root_directory_ + "/trash");
  FsyncDirectory(root_directory_);
  if (removed_snapshots != nullptr) {
    *removed_snapshots = snapshots;
  }
  if (removed_bytes != nullptr) {
    *removed_bytes = bytes;
  }
  return StoreResult::kOk;
}

}  // namespace net
}  // namespace backupproject
