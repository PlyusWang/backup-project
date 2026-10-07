// server/remote_maintenance.cpp
//
// 见 include/remote_maintenance.h。

// 模块职责：把"删除一个 snapshot / 注销一个账户"落到数据目录与元数据库上。
// 服务端（backup-server）与管理工具（backup-server-admin）调用的是同一批
// 函数，所以删除顺序只有一份实现，不存在"管理工具绕过服务端语义"的旁路。
//
// 磁盘布局：<root>/users/<id>/<snapshot_id>.bak 是快照 blob；
// <root>/users/<id>/trash/ 放删除中途的文件，<root>/trash/ 放账户隔离目录。
//
// 顺序不变量：任何破坏性动作都先让数据"不可见"（rename），再动元数据
// （一个事务），最后物理删除；元数据删除失败就把名字改回去。这条顺序保证
// 失败时不会出现"元数据还在、字节已经没了"的状态。
//
// 并发前提：调用方必须已经持有数据目录锁与元数据互斥锁，本层自己不加锁。
// 失败语义：kError 不表示"磁盘一定没动过"——回滚本身也可能失败，那种情况
// 下数据会停在 trash 里，元数据行仍然存在，并会写一条诊断日志。
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

// 立刻把 errno 转成文本：errno 会被后续任何一次库调用覆盖，
// 因此只能在失败点紧邻处读取。
std::string StrerrorText() { return std::string(std::strerror(errno)); }

// 只在 error_message 为空时写入：一次操作里可能有多个失败点，第一个
// （最靠近根因的）错误信息应当保留下来，后面的补充信息不应该覆盖它。
void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr && error_message->empty()) {
    *error_message = text;
  }
}

// 失败不向上传播：这是 best-effort 的持久化加固，不是操作成功的前提。
// 例如只读挂载下 open(O_DIRECTORY) 会失败，那种情况不应该让一次已经完成的
// 删除变成失败。
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

// log_ 默认是空的：日志只是诊断手段，不参与成功/失败判定，
// 因此这里不检查它的返回值，也不在日志失败时改变任何行为。
void RemoteMaintenance::Log(const std::string& message) {
  if (log_) {
    log_(message);
  }
}

// 只建一层目录（没有 mkdir -p 语义）：调用方传进来的都是自己拼好的路径，
// 需要多级时逐级调用。EEXIST 只有在目标确实是目录时才被接受，
// 否则一个同名的普通文件会让后续写入以更难懂的方式失败。
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

// 路径布局的唯一生成点：<root>/users/<user_id>。user_id 是服务端的数字主键，
// 客户端提供的用户名与显示名从不参与路径拼接，因此这里不存在"用名字里的
// ../ 逃出 users 目录"的输入面。
std::string RemoteMaintenance::UserDirectory(std::int64_t user_id) const {
  return root_directory_ + "/users/" + std::to_string(user_id);
}

std::string RemoteMaintenance::LockFilePath(const std::string& root_directory) {
  return root_directory + "/.backup-server.lock";
}

// 只读前 256 字节并去掉行尾换行：锁文件里写的是 "pid=… started_at=…" 这类
// 纯诊断文本，读不到就返回空串。真相永远在 flock 上，这里的内容不参与任何
// 判断，也不能被当成"锁是否被持有"的依据。
std::string RemoteMaintenance::ReadLockHint(const std::string& root_directory) {
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

// 递归删除。入口用 lstat 而不是 stat：符号链接会被当作"非目录"直接 unlink，
// 绝不跟进去——跟随链接等于让一个链接把删除动作引到目录树之外。
// 目标不存在视为成功（幂等），因此重复调用是安全的。
bool RemoteMaintenance::RemoveDirectoryTree(const std::string& path,
                                            std::string* error_message) {
  struct stat info;
  if (::lstat(path.c_str(), &info) != 0) {
    if (errno == ENOENT) {
      return true;
    }
    SetError(error_message, "cannot inspect " + path + ": " + StrerrorText());
    return false;
  }
  if (!S_ISDIR(info.st_mode)) {
    // 普通文件或符号链接：只删这一个名字，绝不跟进去（跟进去就等于让一个
    // 链接把删除动作引到目录树之外）。
    if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
      SetError(error_message, "cannot remove " + path + ": " + StrerrorText());
      return false;
    }
    return true;
  }
  DIR* directory = ::opendir(path.c_str());
  if (directory == nullptr) {
    SetError(error_message, "cannot open " + path + ": " + StrerrorText());
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
  // 依赖感知删除。被别的快照当作 parent 引用的快照**不能删**：
  // 删掉它会让所有后代快照永远无法恢复（链断在中间）。检查放在**动磁盘之前**，
  // 而不是先把 blob 挪进 trash 再回滚。
  std::uint64_t children = 0;
  const StoreResult counted = store_->CountSnapshotChildren(
      user_id, snapshot_id, &children, &store_error);
  if (counted != StoreResult::kOk) {
    SetError(error_message,
             "cannot count the dependent snapshots: " + store_error);
    return StoreResult::kError;
  }
  if (children > 0) {
    SetError(error_message, "snapshot " + snapshot_id + " still has " +
                                std::to_string(children) +
                                " dependent incremental snapshot(s); delete the"
                                " descendants first");
    return StoreResult::kHasDependents;
  }
  // 路径包含性不变量：最终路径只能由 UserDirectory(user_id) 与一个纯文件名
  // 拼成。即使 storage_name 来自数据库（服务端自己写的），也重新验一遍。
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
  // 同一目录树内的 rename 是原子的，不涉及数据拷贝；trash 与 blob 同在
  // <root>/users/<id> 之下，因此不会跨文件系统（跨设备 rename 会失败）。
  // ENOENT 单独区分成 kNotFound：文件本来就不在，与"挪不动"是两回事。
  if (::rename(final_path.c_str(), pending_path.c_str()) != 0) {
    if (errno == ENOENT) {
      SetError(error_message, "the blob is missing on disk");
      return StoreResult::kNotFound;
    }
    SetError(error_message, "cannot move the blob to trash: " + StrerrorText());
    return StoreResult::kError;
  }
  // deleted 由 store 回填，只有返回 kOk 时才有效；removed 是出参，失败路径
  // 一律不写，调用方不能拿它当"删掉了什么"的凭据。
  RemoteSnapshotRecord deleted;
  const StoreResult remove_result =
      store_->DeleteSnapshot(user_id, snapshot_id, &deleted, &store_error);
  if (remove_result != StoreResult::kOk) {
    // 回滚：把文件改回正式名字，一切照旧。
    Log("the metadata delete failed; restoring the blob: " + store_error);
    if (::rename(pending_path.c_str(), final_path.c_str()) != 0) {
      Log("warning: the rollback rename failed; the blob is left in trash: " +
          StrerrorText());
    }
    SetError(error_message, remove_result == StoreResult::kNotFound
                                ? "the snapshot row disappeared during the"
                                  " delete"
                                : store_error);
    if (remove_result == StoreResult::kHasDependents) {
      // 只读检查与这里之间不可能出现新的子节点（同一把元数据互斥锁 + 数据
      // 目录锁），所以这条分支只是纵深防御。无论如何：blob 已经改回正式名字。
      return StoreResult::kHasDependents;
    }
    return remove_result == StoreResult::kNotFound ? StoreResult::kNotFound
                                                   : StoreResult::kError;
  }
  if (::unlink(pending_path.c_str()) != 0) {
    // 元数据已经不存在了，这个文件是"不可见的孤儿"：记录警告，留给将来的
    // startup reconciliation 清理，不因此把删除判为失败。
    Log("warning: could not unlink the pending blob; an invisible orphan"
        " remains in trash");
  }
  // rename / unlink 只改目录项，fsync 目录才能让这些改动掉电后可恢复。
  // 这一步排在元数据删除成功之后，因此持久化顺序与逻辑顺序一致：
  // 先是元数据里看不到这一行，再是磁盘上看不到这个文件。
  // 范围要说清：这里 fsync 的是 users/<id>（rename 的源目录项在此），
  // 被 unlink 的 pending blob 在 trash 目录下，其改动不在本次 fsync 内。
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
    quarantine = root_directory_ + "/trash/account-" + std::to_string(user_id) +
                 "." + suffix + ".deleted";
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

  // 元数据删除是整次注销唯一的提交点：它成功即视为账户已注销，之后的目录
  // 物理删除只是回收磁盘，失败只记告警。反过来，它一旦失败就必须把隔离目录
  // 改回原名，让账户回到可用状态。
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
            " the root trash: " +
            StrerrorText());
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
  // 两个目录都要 fsync：trash 下少了一个隔离目录的目录项，root 下少了
  // users/<id> 这一项。只 fsync 其中一个，掉电后可能剩下一个指不到内容的
  // 目录项，让"注销是否真的完成"无法从磁盘上判断。
  FsyncDirectory(root_directory_ + "/trash");
  FsyncDirectory(root_directory_);
  // removed_* 只在成功路径写入，并且允许传 nullptr：调用方可能只关心成败，
  // 强制要求出参指针会逼它声明无用的局部变量。
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
