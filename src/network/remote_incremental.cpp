// src/network/remote_incremental.cpp
//
// 远端增量闭环的实现。设计约束见 include/remote_incremental.h。

#include "remote_incremental.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <map>

#include "app_paths.h"
#include "incremental_backup.h"
#include "incremental_delta.h"
#include "incremental_restore.h"
#include "snapshot_bundle.h"

namespace backupproject {
namespace net {
namespace {

constexpr const char* kRemoteArchivePrefix = "remote-";

std::string StrerrorText() { return std::string(std::strerror(errno)); }

void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) {
    *error_message = text;
  }
}

bool EnsureDirectoryTree(const std::string& path, std::string* error_message) {
  if (path.empty()) {
    SetError(error_message, "目录路径为空");
    return false;
  }
  std::string current;
  std::size_t index = 0;
  if (path[0] == '/') {
    current = "/";
    index = 1;
  }
  while (index < path.size()) {
    std::size_t slash = path.find('/', index);
    if (slash == std::string::npos) {
      slash = path.size();
    }
    const std::string part = path.substr(index, slash - index);
    if (!part.empty()) {
      if (current.empty()) {
        current = part;
      } else if (current == "/") {
        current += part;
      } else {
        current += "/" + part;
      }
      if (::mkdir(current.c_str(), 0700) != 0 && errno != EEXIST) {
        SetError(error_message,
                 "无法创建目录 " + current + "：" + StrerrorText());
        return false;
      }
    }
    index = slash + 1;
  }
  // 处理结尾不是 '/' 的路径（上面的循环已经覆盖），再确认一次它真的存在。
  struct stat info;
  if (::stat(path.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) {
    SetError(error_message, path + " 不是可用的目录");
    return false;
  }
  return true;
}

std::string FirstSixteen(const std::string& text) {
  return text.size() <= 16 ? text : text.substr(0, 16);
}

bool HasBackupSuffix(const std::string& name) {
  return name.size() > 4 && name.compare(name.size() - 4, 4, ".bak") == 0;
}

// ---- 本地索引：服务端快照 id <-> 缓存里的归档名 ----
//
// 为什么必须有它：服务端的 snapshot id（16 字节随机数的十六进制）与归档
// **自己**的身份（完整快照是 payload 摘要派生，delta 是信封自摘要）是两个
// 不同的东西，谁也不能从另一个算出来。而"续链"要回答的第一个问题就是
// "服务端说的那个父快照，本地缓存里有没有、叫什么名字"。
//
// 这个索引是**缓存层，不是信任来源**：它指向的每一份材料在使用前都要过
// LoadVerifiedSnapshotIdentity（实际字节 + 两个副文件），链的父子关系还要
// 过引擎自己的校验。索引丢了最多让下一次备份重新下载一遍；索引写错了只会
// 让校验失败，不会让它信一份错的材料。
std::string RemoteIndexPath(const std::string& cache_directory) {
  return cache_directory + "/.remote-index.tsv";
}

void LoadRemoteIndex(const std::string& cache_directory,
                     std::map<std::string, std::string>* index) {
  index->clear();
  const std::string path = RemoteIndexPath(cache_directory);
  const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW);
  if (fd < 0) {
    return;
  }
  std::string content;
  char buffer[4096];
  for (;;) {
    const ssize_t got = ::read(fd, buffer, sizeof(buffer));
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (got == 0) {
      break;
    }
    content.append(buffer, static_cast<std::size_t>(got));
  }
  ::close(fd);
  std::size_t start = 0;
  while (start < content.size()) {
    std::size_t end = content.find('\n', start);
    if (end == std::string::npos) {
      end = content.size();
    }
    const std::string line = content.substr(start, end - start);
    start = end + 1;
    const std::size_t tab = line.find('\t');
    if (tab == std::string::npos || tab == 0 || tab + 1 >= line.size()) {
      continue;
    }
    (*index)[line.substr(0, tab)] = line.substr(tab + 1);
  }
}

bool SaveRemoteIndex(const std::string& cache_directory,
                     const std::map<std::string, std::string>& index,
                     std::string* error_message) {
  const std::string path = RemoteIndexPath(cache_directory);
  const std::string part =
      path + ".part-" + std::to_string(static_cast<long>(::getpid()));
  ::unlink(part.c_str());
  const int fd = ::open(part.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd < 0) {
    SetError(error_message, "无法写缓存索引 " + part + "：" + StrerrorText());
    return false;
  }
  std::string content;
  for (const auto& entry : index) {
    if (entry.first.empty() || entry.second.empty()) {
      continue;
    }
    content += entry.first;
    content += '\t';
    content += entry.second;
    content += '\n';
  }
  std::size_t written = 0;
  while (written < content.size()) {
    const ssize_t got =
        ::write(fd, content.data() + written, content.size() - written);
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      const std::string reason = StrerrorText();
      ::close(fd);
      ::unlink(part.c_str());
      SetError(error_message, "写缓存索引失败：" + reason);
      return false;
    }
    written += static_cast<std::size_t>(got);
  }
  if (::fsync(fd) != 0 || ::close(fd) != 0) {
    ::unlink(part.c_str());
    SetError(error_message, "缓存索引落盘失败");
    return false;
  }
  // 索引是缓存元数据，允许被替换（它指向的材料仍然要逐个验证）。
  if (::rename(part.c_str(), path.c_str()) != 0) {
    const std::string reason = StrerrorText();
    ::unlink(part.c_str());
    SetError(error_message, "无法发布缓存索引：" + reason);
    return false;
  }
  return true;
}

void RememberSnapshotInIndex(const std::string& cache_directory,
                             const std::string& snapshot_id,
                             const std::string& archive_name) {
  std::map<std::string, std::string> index;
  LoadRemoteIndex(cache_directory, &index);
  if (index[snapshot_id] == archive_name) {
    return;
  }
  index[snapshot_id] = archive_name;
  std::string ignore;
  SaveRemoteIndex(cache_directory, index, &ignore);
}

// 缓存里这个名字的归档是不是一份**验证通过**的材料。
bool CacheHasVerifiedArchive(const std::string& cache_directory,
                             const std::string& archive_name) {
  if (!HasBackupSuffix(archive_name)) {
    return false;
  }
  struct stat info;
  if (::stat((cache_directory + "/" + archive_name).c_str(), &info) != 0 ||
      !S_ISREG(info.st_mode)) {
    return false;
  }
  SnapshotIdentity identity;
  std::string load_error;
  return LoadVerifiedSnapshotIdentity(cache_directory, archive_name, &identity,
                                      nullptr, &load_error) &&
         identity.sidecars_verified;
}

// 在缓存目录里找"这份远端快照的本地材料"。先查索引（它给出名字），再让
// LoadVerifiedSnapshotIdentity 用**实际字节与副文件**确认这份材料可信。
bool ScanCacheForSnapshot(const std::string& cache_directory,
                          const std::string& snapshot_id,
                          std::string* archive_name, bool* found,
                          std::string* error_message) {
  (void)error_message;
  *found = false;
  archive_name->clear();
  std::map<std::string, std::string> index;
  LoadRemoteIndex(cache_directory, &index);
  const auto entry = index.find(snapshot_id);
  if (entry == index.end()) {
    return true;
  }
  if (!CacheHasVerifiedArchive(cache_directory, entry->second)) {
    // 索引指向的材料已经不在了，或者验证不过：当作不在缓存里，
    // 让调用方重新下载一遍（而不是相信一个陈旧的索引条目）。
    return true;
  }
  *archive_name = entry->second;
  *found = true;
  return true;
}

// 下载 + 解包 + 验证一份远端快照的材料，把它放进缓存目录。
bool FetchSnapshotMaterial(RemoteArchiveClient* client,
                           const RemoteCacheLayout& cache,
                           const std::string& snapshot_id,
                           std::string* archive_name,
                           std::uint64_t* downloaded_bytes,
                           const RemoteProgressCallback& progress,
                           std::string* error_message) {
  bool found = false;
  if (!ScanCacheForSnapshot(cache.cache_directory, snapshot_id, archive_name,
                            &found, error_message)) {
    return false;
  }
  if (found) {
    return true;
  }

  const std::string bundle_path =
      cache.cache_directory + "/incoming-" + snapshot_id + "-" +
      std::to_string(static_cast<long>(::getpid())) + ".bundle";
  ::unlink(bundle_path.c_str());

  RemoteSnapshotInfo downloaded;
  if (!client->DownloadArchiveFile(snapshot_id, bundle_path,
                                   /*allow_overwrite=*/false, progress,
                                   &downloaded, error_message)) {
    ::unlink(bundle_path.c_str());
    return false;
  }
  // DownloadArchiveFile 内部已经验过"实际字节的 SHA-256 == 服务端声明值"
  // 并且只在通过之后才发布目标文件；这里拿到的是一份长度与摘要都对得上的包。

  SnapshotBundleInfo bundle;
  std::string extract_error;
  if (!ExtractSnapshotBundle(bundle_path, cache.cache_directory, &bundle,
                             &extract_error)) {
    // 缓存里已经有同名的三件套时发布会被拒绝（NoReplace）。这通常意味着
    // 索引丢了而材料还在：只要那份材料**验证得过去**，就当它命中，而不是
    // 覆盖它或者失败。验证不过就如实失败——绝不覆盖一份来路不明的材料。
    SnapshotBundleInfo existing;
    std::string inspect_error;
    if (InspectSnapshotBundle(bundle_path, &existing, &inspect_error) &&
        CacheHasVerifiedArchive(cache.cache_directory, existing.archive_name)) {
      ::unlink(bundle_path.c_str());
      RememberSnapshotInIndex(cache.cache_directory, snapshot_id,
                              existing.archive_name);
      if (downloaded_bytes != nullptr) {
        *downloaded_bytes += existing.bundle_size;
      }
      *archive_name = existing.archive_name;
      return true;
    }
    ::unlink(bundle_path.c_str());
    SetError(error_message, "材料包解包失败：" + extract_error);
    return false;
  }
  ::unlink(bundle_path.c_str());
  RememberSnapshotInIndex(cache.cache_directory, snapshot_id,
                          bundle.archive_name);
  if (downloaded_bytes != nullptr) {
    *downloaded_bytes += bundle.bundle_size;
  }

  // 关键一步：真正算数的是归档自己的字节与两个副文件，而不是服务端说它是谁。
  //
  // 注意两个 id 不是一回事：服务端的 snapshot_id 是 16 字节随机数，而归档
  // 自己的身份（SnapshotIdentity::snapshot_id）来自 payload 摘要 / 信封自摘要。
  // 谁也推不出另一个，所以这里检查的是"这份材料自身完整且可信"，把
  // "服务端 id -> 本地归档名"的对应关系留给缓存索引；而"服务端给的父子边"
  // 与"归档信封里的父子边"是否一致，由 EnsureChainMaterial 在整条链上交叉校验。
  SnapshotIdentity identity;
  std::string load_error;
  const bool verified =
      LoadVerifiedSnapshotIdentity(cache.cache_directory, bundle.archive_name,
                                   &identity, nullptr, &load_error);
  if (!verified || !identity.sidecars_verified) {
    for (const SnapshotBundleMember& member : bundle.members) {
      ::unlink((cache.cache_directory + "/" + member.name).c_str());
    }
    SetError(error_message,
             "下载到的材料自身验证不通过（" +
                 (verified ? identity.sidecar_diagnostic : load_error) +
                 "）：已从缓存中撤掉");
    return false;
  }
  *archive_name = bundle.archive_name;
  return true;
}

// 把整条链（base -> head）的材料准备到本地缓存，并返回 head 的本地名字。
bool EnsureChainMaterial(RemoteArchiveClient* client,
                         const RemoteCacheLayout& cache,
                         const std::string& target_snapshot_id,
                         std::vector<RemoteSnapshotInfo>* chain,
                         std::string* target_archive_name,
                         std::uint64_t* downloaded_bytes,
                         const RemoteProgressCallback& progress,
                         std::string* error_message) {
  std::vector<RemoteSnapshotInfo> snapshots;
  if (!client->List(&snapshots, error_message)) {
    return false;
  }
  if (!ResolveRemoteChain(snapshots, target_snapshot_id, chain,
                          error_message)) {
    return false;
  }
  std::vector<std::string> names;
  names.reserve(chain->size());
  for (const RemoteSnapshotInfo& snapshot : *chain) {
    std::string name;
    if (!FetchSnapshotMaterial(client, cache, snapshot.snapshot_id, &name,
                               downloaded_bytes, progress, error_message)) {
      return false;
    }
    names.push_back(name);
    if (snapshot.snapshot_id == target_snapshot_id) {
      *target_archive_name = name;
    }
  }
  if (target_archive_name->empty()) {
    SetError(error_message, "内部错误：目标快照的材料没有落进缓存");
    return false;
  }

  // 交叉校验：服务端给的父子边必须与**归档自己的信封**一致。
  //
  // 元数据负责"去哪里找"，归档负责"这是什么"：这里把两者对上，任何一方
  // 被改动（服务端元数据被篡改、缓存里混进了别的同名文件）都会在恢复之前
  // 就失败。这一条也是"服务端元数据不替代归档自验证"的可执行版本。
  std::vector<SnapshotIdentity> identities(chain->size());
  for (std::size_t index = 0; index < chain->size(); ++index) {
    std::string load_error;
    if (!LoadVerifiedSnapshotIdentity(cache.cache_directory, names[index],
                                      &identities[index], nullptr,
                                      &load_error)) {
      SetError(error_message, "链上的材料无法验证：" + load_error);
      return false;
    }
  }
  for (std::size_t index = 1; index < chain->size(); ++index) {
    const SnapshotIdentity& child = identities[index];
    const SnapshotIdentity& parent = identities[index - 1];
    if (child.parent_file_name != names[index - 1]) {
      SetError(error_message, "远端元数据与归档不一致：第 " +
                                  std::to_string(index) + " 跳声明的父是 " +
                                  names[index - 1] + "，归档信封里写的却是 " +
                                  child.parent_file_name);
      return false;
    }
    if (!child.parent_snapshot_id.empty() &&
        child.parent_snapshot_id != parent.snapshot_id) {
      SetError(error_message, "远端元数据与归档不一致：父子身份不匹配（第 " +
                                  std::to_string(index) + " 跳）");
      return false;
    }
    if (!parent.sidecars_verified) {
      SetError(error_message, "链上的父快照副文件验证未通过（第 " +
                                  std::to_string(index - 1) + " 跳）");
      return false;
    }
  }
  return true;
}

}  // namespace

bool PrepareRemoteCache(const std::string& root_directory,
                        const std::string& server_fingerprint,
                        const std::string& username, RemoteCacheLayout* layout,
                        std::string* error_message) {
  if (layout == nullptr) {
    SetError(error_message, "缓存布局的输出指针为空");
    return false;
  }
  *layout = RemoteCacheLayout();
  if (server_fingerprint.size() != 64 || username.empty()) {
    SetError(error_message, "无法确定远端缓存目录（指纹或用户名为空）");
    return false;
  }
  std::string root = root_directory;
  if (root.empty()) {
    std::string config_directory;
    if (!AppConfigDirectory(&config_directory, error_message)) {
      return false;
    }
    root = config_directory + "/remote-cache";
  }
  layout->root_directory = root;
  layout->server_fingerprint = server_fingerprint;
  layout->username = username;
  layout->cache_directory =
      root + "/" + FirstSixteen(server_fingerprint) + "/" + username;
  // 逻辑身份：与"本机路径"无关，所以另一台机器上传的同一条链能被认出来。
  layout->repository_identity = "remote:" + server_fingerprint + ":" + username;
  return EnsureDirectoryTree(layout->cache_directory, error_message);
}

std::string RemoteLineageId(const RemoteCacheLayout& layout,
                            const std::string& source_directory) {
  return SourceIdentityDigest(source_directory, layout.repository_identity);
}

bool ResolveRemoteChain(const std::vector<RemoteSnapshotInfo>& snapshots,
                        const std::string& target_snapshot_id,
                        std::vector<RemoteSnapshotInfo>* chain,
                        std::string* error_message) {
  if (chain == nullptr) {
    SetError(error_message, "链的输出指针为空");
    return false;
  }
  chain->clear();
  std::map<std::string, RemoteSnapshotInfo> index;
  for (const RemoteSnapshotInfo& snapshot : snapshots) {
    index[snapshot.snapshot_id] = snapshot;
  }
  std::string current = target_snapshot_id;
  while (true) {
    const auto found = index.find(current);
    if (found == index.end()) {
      SetError(error_message,
               "服务端没有这个快照，或者它不属于当前账户：" + current);
      return false;
    }
    chain->push_back(found->second);
    if (chain->size() > kMaxDeltaChainDepth + 1) {
      SetError(error_message, "远端链超过恢复侧允许的最大深度（" +
                                  std::to_string(kMaxDeltaChainDepth) +
                                  " 个 delta）");
      return false;
    }
    if (found->second.parent_snapshot_id.empty()) {
      break;
    }
    current = found->second.parent_snapshot_id;
  }
  std::reverse(chain->begin(), chain->end());

  // 自洽性检查：根必须是 full 且 generation 0，之后每一跳恰好 +1。
  // 这些字段来自服务端元数据，所以这里检查的不是"信任"，而是"自相矛盾"——
  // 一个自相矛盾的元数据不值得拿去做后续的下载与恢复。
  for (std::size_t i = 0; i < chain->size(); ++i) {
    const RemoteSnapshotInfo& snapshot = (*chain)[i];
    if (i == 0) {
      if (!snapshot.parent_snapshot_id.empty() || snapshot.generation != 0) {
        SetError(
            error_message,
            "远端链的根不是一份 generation 0 的完整快照（元数据自相矛盾）");
        return false;
      }
      continue;
    }
    if (snapshot.parent_snapshot_id != (*chain)[i - 1].snapshot_id ||
        snapshot.generation != (*chain)[i - 1].generation + 1) {
      SetError(error_message, "远端链的父子关系或代数不连续（元数据自相矛盾）");
      return false;
    }
  }
  return true;
}

bool FindRemoteLineageHead(const std::vector<RemoteSnapshotInfo>& snapshots,
                           const std::string& lineage, RemoteSnapshotInfo* head,
                           std::string* reason) {
  if (head == nullptr) {
    SetError(reason, "head 的输出指针为空");
    return false;
  }
  *head = RemoteSnapshotInfo();
  // 被别的快照当作父引用过的 id 都不是 head。
  std::map<std::string, bool> referenced;
  for (const RemoteSnapshotInfo& snapshot : snapshots) {
    if (!snapshot.parent_snapshot_id.empty()) {
      referenced[snapshot.parent_snapshot_id] = true;
    }
  }
  bool found = false;
  for (const RemoteSnapshotInfo& snapshot : snapshots) {
    if (snapshot.lineage != lineage) {
      continue;
    }
    if (referenced.find(snapshot.snapshot_id) != referenced.end()) {
      continue;
    }
    if (!found || snapshot.created_at > head->created_at ||
        (snapshot.created_at == head->created_at &&
         snapshot.generation > head->generation) ||
        (snapshot.created_at == head->created_at &&
         snapshot.generation == head->generation &&
         snapshot.snapshot_id < head->snapshot_id)) {
      *head = snapshot;
      found = true;
    }
  }
  if (!found) {
    SetError(reason, "这条链在服务端还没有任何快照");
    return false;
  }
  return true;
}

bool RunRemoteBackup(const RemoteBackupRequest& request,
                     RemoteBackupOutcome* outcome, std::string* error_message) {
  if (outcome == nullptr || request.client == nullptr) {
    SetError(error_message, "远端备份的参数为空");
    return false;
  }
  *outcome = RemoteBackupOutcome();
  if (request.source_directory.empty()) {
    SetError(error_message, "远端备份需要源目录");
    return false;
  }

  const std::string lineage =
      RemoteLineageId(request.cache, request.source_directory);

  // 1) 远端链的 head。没有 head（或者调用方要求完整基线）时强制新建一份
  //    完整基线：给引擎一个**不受管**的基线名，按 include/incremental_backup.h
  //    的公开契约（baseline_snapshot_name 非空且不是受管名字 -> 清空基线），
  //    引擎就会产出一份完整 baseline，而不是去找缓存里可能已经过期的旧基线。
  std::vector<RemoteSnapshotInfo> snapshots;
  if (!request.client->List(&snapshots, error_message)) {
    return false;
  }
  RemoteSnapshotInfo head;
  std::string head_reason;
  const bool have_head =
      request.allow_incremental &&
      FindRemoteLineageHead(snapshots, lineage, &head, &head_reason);

  std::string baseline_name;
  std::string target_archive_name;
  if (have_head) {
    std::vector<RemoteSnapshotInfo> chain;
    if (!EnsureChainMaterial(request.client, request.cache, head.snapshot_id,
                             &chain, &target_archive_name, nullptr,
                             request.progress, error_message)) {
      return false;
    }
    baseline_name = target_archive_name;
    for (const RemoteSnapshotInfo& snapshot : chain) {
      if (snapshot.snapshot_id == chain.front().snapshot_id) {
        outcome->chain_root_bytes = snapshot.size_bytes;
      }
    }
  } else {
    baseline_name = "remote-force-full-baseline";
    outcome->baseline_reason = head_reason;
  }

  // 2) 让**既有引擎**决定这次是 delta 还是完整基线。
  const std::uint64_t generation = have_head ? head.generation + 1 : 0;
  const std::string new_name =
      std::string(kRemoteArchivePrefix) + lineage.substr(0, 12) + "-" +
      std::to_string(static_cast<long long>(::time(nullptr))) + "-g" +
      std::to_string(generation) + ".bak";

  IncrementalOutcome incremental;
  std::string engine_error;
  if (!RunIncrementalBackup(
          request.source_directory, request.cache.cache_directory, new_name,
          request.cache.repository_identity, request.filter, request.options,
          request.include_rules, request.exclude_rules, baseline_name,
          &incremental, &engine_error)) {
    SetError(error_message, "增量引擎失败：" + engine_error);
    return false;
  }
  outcome->baseline_reason = incremental.baseline_reason.empty()
                                 ? outcome->baseline_reason
                                 : incremental.baseline_reason;
  if (incremental.kind == IncrementalOutcome::Kind::kNoChanges) {
    outcome->no_changes = true;
    return true;
  }

  const bool produced_delta =
      incremental.kind == IncrementalOutcome::Kind::kDelta;
  outcome->produced_delta = produced_delta;
  outcome->rebuilt_full_baseline = !produced_delta && have_head;
  outcome->archive_name = incremental.snapshot_file_name;

  // 3) 打包 + 上传。链关系由服务端校验（父必须存在、同用户、同 lineage），
  //    代数由服务端按父推导。
  const std::string bundle_path =
      request.cache.cache_directory + "/outgoing-" + new_name + "-" +
      std::to_string(static_cast<long>(::getpid())) + ".bundle";
  ::unlink(bundle_path.c_str());
  SnapshotBundleInfo bundle;
  if (!BuildSnapshotBundle(request.cache.cache_directory, new_name, bundle_path,
                           &bundle, error_message)) {
    ::unlink(bundle_path.c_str());
    return false;
  }

  RemoteUploadOptions upload_options;
  upload_options.snapshot_kind = static_cast<std::uint16_t>(
      produced_delta ? SnapshotKind::kIncremental : SnapshotKind::kFull);
  upload_options.lineage = lineage;
  if (produced_delta) {
    if (!have_head) {
      ::unlink(bundle_path.c_str());
      SetError(error_message,
               "内部错误：引擎产出了 delta，但本地没有远端 head 可当父");
      return false;
    }
    upload_options.parent_snapshot_id = head.snapshot_id;
  }

  const std::string display_name =
      request.display_name.empty() ? new_name : request.display_name;
  RemoteSnapshotInfo uploaded;
  const bool uploaded_ok = request.client->UploadSnapshotFile(
      bundle_path, display_name, upload_options, request.progress, &uploaded,
      error_message);
  ::unlink(bundle_path.c_str());
  if (!uploaded_ok) {
    return false;
  }
  outcome->snapshot_id = uploaded.snapshot_id;
  outcome->parent_snapshot_id = uploaded.parent_snapshot_id;
  outcome->generation = uploaded.generation;
  outcome->uploaded_bytes = bundle.bundle_size;
  // 记下"服务端 id -> 本地归档名"，下一次续链就能直接找到父材料。
  RememberSnapshotInIndex(request.cache.cache_directory, uploaded.snapshot_id,
                          new_name);

  // 服务端必须按父推导代数；对不上说明元数据出了问题，如实报告而不是掩盖。
  if (produced_delta) {
    if (uploaded.parent_snapshot_id != head.snapshot_id ||
        uploaded.generation != head.generation + 1) {
      SetError(error_message,
               "服务端登记的链关系与客户端请求不一致（父或代数对不上）");
      return false;
    }
  } else if (uploaded.generation != 0 || !uploaded.parent_snapshot_id.empty()) {
    SetError(error_message, "服务端把一份完整基线登记成了增量快照");
    return false;
  }
  return true;
}

bool RunRemoteRestore(RemoteArchiveClient* client,
                      const RemoteCacheLayout& cache,
                      const std::string& snapshot_id,
                      const std::string& destination_directory,
                      const RestoreOptions& restore_options,
                      RemoteRestoreOutcome* outcome,
                      std::string* error_message) {
  if (outcome == nullptr || client == nullptr) {
    SetError(error_message, "远端恢复的参数为空");
    return false;
  }
  *outcome = RemoteRestoreOutcome();
  if (destination_directory.empty()) {
    SetError(error_message, "远端恢复需要目标目录");
    return false;
  }

  std::vector<RemoteSnapshotInfo> chain;
  std::string archive_name;
  std::uint64_t downloaded = 0;
  if (!EnsureChainMaterial(client, cache, snapshot_id, &chain, &archive_name,
                           &downloaded, nullptr, error_message)) {
    return false;
  }
  outcome->downloaded_bytes = downloaded;
  outcome->chain_length = chain.size();
  outcome->delta_count = chain.empty() ? 0 : chain.size() - 1;
  outcome->archive_name = archive_name;

  // 真正应用的那一步仍然是既有实现：它自己会再验一遍链上每一跳的字节、
  // 副文件绑定与父子关系，然后把结果原子发布到目标目录。
  RestoreReport report;
  std::string restore_error;
  if (!RestoreSnapshotChain(cache.cache_directory, archive_name,
                            destination_directory, restore_options, &report,
                            &restore_error)) {
    SetError(error_message, "恢复失败：" + restore_error);
    return false;
  }
  outcome->restored_entries = report.restored_entries;
  return true;
}

}  // namespace net
}  // namespace backupproject
