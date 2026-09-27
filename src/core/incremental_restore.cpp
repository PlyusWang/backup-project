// incremental_restore.cpp
//
// 见 include/incremental_restore.h。

#include "incremental_restore.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utime.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "container_format.h"

namespace backupproject {

namespace {

void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) *error_message = text;
}

std::string ErrnoText(int error_number) {
  const char* text = ::strerror(error_number);
  return text == nullptr ? std::string("errno ") + std::to_string(error_number)
                         : std::string(text);
}

void AddNote(RestoreReport* report, const std::string& note) {
  if (report != nullptr) report->notes.push_back(note);
}

bool IsPlainSingleComponentName(const std::string& name) {
  if (name.empty() || name == "." || name == "..") return false;
  if (name.find('/') != std::string::npos) return false;
  if (name.find('\\') != std::string::npos) return false;
  if (name.find('\0') != std::string::npos) return false;
  return true;
}

std::string JoinPath(const std::string& directory, const std::string& name) {
  if (directory.empty()) return name;
  if (directory.back() == '/') return directory + name;
  return directory + "/" + name;
}

// 递归删除（staging / overlay 的清理用）。不存在视为成功。
bool RemoveTree(const std::string& path) {
  struct stat info;
  if (::lstat(path.c_str(), &info) != 0) {
    return errno == ENOENT;
  }
  if (!S_ISDIR(info.st_mode)) {
    return ::unlink(path.c_str()) == 0;
  }
  DIR* directory = ::opendir(path.c_str());
  if (directory == nullptr) return false;
  bool ok = true;
  while (struct dirent* item = ::readdir(directory)) {
    const std::string name = item->d_name;
    if (name == "." || name == "..") continue;
    if (!RemoveTree(JoinPath(path, name))) ok = false;
  }
  ::closedir(directory);
  if (::rmdir(path.c_str()) != 0) ok = false;
  return ok;
}

bool IsDirectoryEmpty(const std::string& path, bool* empty,
                      std::string* error_message) {
  DIR* directory = ::opendir(path.c_str());
  if (directory == nullptr) {
    SetError(error_message, "Cannot open " + path + ": " + ErrnoText(errno));
    return false;
  }
  *empty = true;
  while (struct dirent* item = ::readdir(directory)) {
    const std::string name = item->d_name;
    if (name == "." || name == "..") continue;
    *empty = false;
    break;
  }
  ::closedir(directory);
  return true;
}

// 把一段路径按深度排序（深的在前）。tombstone 必须深的先删，否则父目录先没了
// 之后子路径的删除会变成"路径不存在"，看起来像成功，实际留下不一致。
void SortDeepestFirst(std::vector<std::string>* paths) {
  std::stable_sort(paths->begin(), paths->end(),
                   [](const std::string& left, const std::string& right) {
                     const std::size_t left_depth = static_cast<std::size_t>(
                         std::count(left.begin(), left.end(), '/'));
                     const std::size_t right_depth = static_cast<std::size_t>(
                         std::count(right.begin(), right.end(), '/'));
                     if (left_depth != right_depth)
                       return left_depth > right_depth;
                     return left.size() > right.size();
                   });
}

// ---- staging 合并 ----
//
// delta 的 payload 先被恢复成一个独立的 overlay 目录（复用现有 restore，得到
// 正确的路径、正文、软链接、hardlink 拓扑与 metadata），再合并进 staging。
//
// 这一步与"从归档恢复"是不同的操作：这里合并的是两棵**已经落地**的目录树，
// 所以它不需要、也不应该重新实现归档格式里的任何东西。

struct MergeContext {
  RestoreReport* report = nullptr;
  // (st_dev, st_ino) -> 已经合并过去的目标路径。用来保持 hardlink 拓扑：
  // overlay 里两个共享 inode 的条目，合并之后必须仍然共享一个 inode。
  std::map<std::pair<std::uint64_t, std::uint64_t>, std::string> inodes;
  std::uint64_t merged_entries = 0;
};

bool CopyFileContents(const std::string& from, const std::string& to,
                      std::string* error_message) {
  const int in_fd = ::open(from.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (in_fd < 0) {
    SetError(error_message, "Cannot open " + from + ": " + ErrnoText(errno));
    return false;
  }
  const int out_fd =
      ::open(to.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (out_fd < 0) {
    const std::string text = ErrnoText(errno);
    ::close(in_fd);
    SetError(error_message, "Cannot create " + to + ": " + text);
    return false;
  }
  std::vector<char> buffer(64u * 1024u);
  bool ok = true;
  for (;;) {
    const ssize_t got = ::read(in_fd, buffer.data(), buffer.size());
    if (got < 0) {
      if (errno == EINTR) continue;
      SetError(error_message, "Cannot read " + from + ": " + ErrnoText(errno));
      ok = false;
      break;
    }
    if (got == 0) break;
    std::size_t remaining = static_cast<std::size_t>(got);
    const char* cursor = buffer.data();
    while (remaining > 0) {
      const ssize_t written = ::write(out_fd, cursor, remaining);
      if (written < 0) {
        if (errno == EINTR) continue;
        SetError(error_message, "Cannot write " + to + ": " + ErrnoText(errno));
        ok = false;
        break;
      }
      cursor += written;
      remaining -= static_cast<std::size_t>(written);
    }
    if (!ok) break;
  }
  if (::close(in_fd) != 0 && ok) {
    SetError(error_message, "Cannot close " + from + ": " + ErrnoText(errno));
    ok = false;
  }
  if (::close(out_fd) != 0 && ok) {
    SetError(error_message, "Cannot close " + to + ": " + ErrnoText(errno));
    ok = false;
  }
  return ok;
}

// 尽力而为地把 metadata 搬过去。ownership 搬不动不是失败：非 root 进程本来
// 就改不了任意属主，既有的 recover 路径也是这么处理的（如实记录，不假装）。
void ApplyMetadataBestEffort(const std::string& path, const struct stat& info,
                             bool is_symlink, MergeContext* context) {
  if (is_symlink) {
    if (::lchown(path.c_str(), info.st_uid, info.st_gid) != 0 &&
        errno != EPERM) {
      AddNote(context->report,
              "lchown failed for " + path + ": " + ErrnoText(errno));
    }
    // 软链接自己的时间戳也只能用 AT_SYMLINK_NOFOLLOW 贴：跟随会把时间写到
    // 目标上，那是另一个条目的事。
    struct timespec link_times[2];
    link_times[0].tv_sec = info.st_atim.tv_sec;
    link_times[0].tv_nsec = info.st_atim.tv_nsec;
    link_times[1].tv_sec = info.st_mtim.tv_sec;
    link_times[1].tv_nsec = info.st_mtim.tv_nsec;
    if (::utimensat(AT_FDCWD, path.c_str(), link_times, AT_SYMLINK_NOFOLLOW) !=
        0) {
      AddNote(context->report,
              "utimensat failed for " + path + ": " + ErrnoText(errno));
    }
    return;
  }
  if (::chmod(path.c_str(), info.st_mode & 07777) != 0) {
    AddNote(context->report,
            "chmod failed for " + path + ": " + ErrnoText(errno));
  }
  if (::chown(path.c_str(), info.st_uid, info.st_gid) != 0) {
    if (errno == EPERM) {
      if (context->report != nullptr) ++context->report->skipped_ownership;
    } else {
      AddNote(context->report,
              "chown failed for " + path + ": " + ErrnoText(errno));
    }
  }
  struct timespec times[2];
  times[0].tv_sec = info.st_atim.tv_sec;
  times[0].tv_nsec = info.st_atim.tv_nsec;
  times[1].tv_sec = info.st_mtim.tv_sec;
  times[1].tv_nsec = info.st_mtim.tv_nsec;
  if (::utimensat(AT_FDCWD, path.c_str(), times, 0) != 0) {
    AddNote(context->report,
            "utimensat failed for " + path + ": " + ErrnoText(errno));
  }
}

bool EnsureRemoved(const std::string& path, std::string* error_message) {
  struct stat info;
  if (::lstat(path.c_str(), &info) != 0) {
    return errno == ENOENT;
  }
  if (S_ISDIR(info.st_mode)) {
    if (!RemoveTree(path)) {
      SetError(error_message, "Cannot remove directory " + path);
      return false;
    }
    return true;
  }
  if (::unlink(path.c_str()) != 0) {
    SetError(error_message, "Cannot remove " + path + ": " + ErrnoText(errno));
    return false;
  }
  return true;
}

bool MergeNode(const std::string& source_path, const std::string& target_path,
               MergeContext* context, std::string* error_message);

bool MergeDirectory(const std::string& source_path,
                    const std::string& target_path, const struct stat& info,
                    MergeContext* context, std::string* error_message) {
  struct stat target_info;
  if (::lstat(target_path.c_str(), &target_info) == 0) {
    if (!S_ISDIR(target_info.st_mode)) {
      // 类型变化：文件/软链接 → 目录。必须先删掉旧表示再建目录，
      // 直接覆盖是做不到的。
      if (!EnsureRemoved(target_path, error_message)) return false;
      if (::mkdir(target_path.c_str(), 0700) != 0) {
        SetError(error_message, "Cannot create directory " + target_path +
                                    ": " + ErrnoText(errno));
        return false;
      }
    }
  } else if (errno == ENOENT) {
    if (::mkdir(target_path.c_str(), 0700) != 0) {
      SetError(error_message, "Cannot create directory " + target_path + ": " +
                                  ErrnoText(errno));
      return false;
    }
  } else {
    SetError(error_message,
             "Cannot stat " + target_path + ": " + ErrnoText(errno));
    return false;
  }

  DIR* directory = ::opendir(source_path.c_str());
  if (directory == nullptr) {
    SetError(error_message,
             "Cannot open " + source_path + ": " + ErrnoText(errno));
    return false;
  }
  bool ok = true;
  while (struct dirent* item = ::readdir(directory)) {
    const std::string name = item->d_name;
    if (name == "." || name == "..") continue;
    if (!MergeNode(JoinPath(source_path, name), JoinPath(target_path, name),
                   context, error_message)) {
      ok = false;
      break;
    }
  }
  ::closedir(directory);
  if (!ok) return false;
  // 目录自己的 metadata 最后写：写早了会被子项的创建改掉 mtime。
  ApplyMetadataBestEffort(target_path, info, false, context);
  return true;
}

bool MergeNode(const std::string& source_path, const std::string& target_path,
               MergeContext* context, std::string* error_message) {
  struct stat info;
  if (::lstat(source_path.c_str(), &info) != 0) {
    SetError(error_message,
             "Cannot stat " + source_path + ": " + ErrnoText(errno));
    return false;
  }

  if (S_ISDIR(info.st_mode)) {
    return MergeDirectory(source_path, target_path, info, context,
                          error_message);
  }

  if (S_ISLNK(info.st_mode)) {
    char buffer[4096];
    const ssize_t size =
        ::readlink(source_path.c_str(), buffer, sizeof(buffer));
    if (size < 0) {
      SetError(error_message,
               "Cannot read link " + source_path + ": " + ErrnoText(errno));
      return false;
    }
    if (!EnsureRemoved(target_path, error_message)) return false;
    const std::string target(buffer, static_cast<std::size_t>(size));
    if (::symlink(target.c_str(), target_path.c_str()) != 0) {
      SetError(error_message, "Cannot create symlink " + target_path + ": " +
                                  ErrnoText(errno));
      return false;
    }
    ApplyMetadataBestEffort(target_path, info, true, context);
    ++context->merged_entries;
    return true;
  }

  if (S_ISREG(info.st_mode)) {
    // hardlink 拓扑：同一个 inode 的第二个条目直接 link 到第一个已合并的路径。
    if (info.st_nlink > 1) {
      const std::pair<std::uint64_t, std::uint64_t> key(
          static_cast<std::uint64_t>(info.st_dev),
          static_cast<std::uint64_t>(info.st_ino));
      const auto found = context->inodes.find(key);
      if (found != context->inodes.end()) {
        if (!EnsureRemoved(target_path, error_message)) return false;
        if (::link(found->second.c_str(), target_path.c_str()) != 0) {
          SetError(error_message,
                   "Cannot link " + target_path + ": " + ErrnoText(errno));
          return false;
        }
        ++context->merged_entries;
        return true;
      }
      context->inodes[key] = target_path;
    }
    if (!EnsureRemoved(target_path, error_message)) return false;
    if (!CopyFileContents(source_path, target_path, error_message))
      return false;
    ApplyMetadataBestEffort(target_path, info, false, context);
    ++context->merged_entries;
    return true;
  }

  if (S_ISFIFO(info.st_mode)) {
    if (!EnsureRemoved(target_path, error_message)) return false;
    if (::mkfifo(target_path.c_str(), info.st_mode & 07777) != 0) {
      SetError(error_message,
               "Cannot create fifo " + target_path + ": " + ErrnoText(errno));
      return false;
    }
    ApplyMetadataBestEffort(target_path, info, false, context);
    ++context->merged_entries;
    return true;
  }

  if (S_ISCHR(info.st_mode) || S_ISBLK(info.st_mode)) {
    if (!EnsureRemoved(target_path, error_message)) return false;
    if (::mknod(target_path.c_str(), info.st_mode, info.st_rdev) != 0) {
      // 非 root 造不出设备节点：如实记录并跳过，不让整次恢复失败。
      AddNote(context->report, "Cannot create device node " + target_path +
                                   ": " + ErrnoText(errno));
      return true;
    }
    ApplyMetadataBestEffort(target_path, info, false, context);
    ++context->merged_entries;
    return true;
  }

  AddNote(context->report, "Skipped unsupported node type: " + source_path);
  return true;
}

bool MergeTree(const std::string& source_directory,
               const std::string& target_directory, MergeContext* context,
               std::string* error_message) {
  DIR* directory = ::opendir(source_directory.c_str());
  if (directory == nullptr) {
    SetError(error_message,
             "Cannot open " + source_directory + ": " + ErrnoText(errno));
    return false;
  }
  bool ok = true;
  while (struct dirent* item = ::readdir(directory)) {
    const std::string name = item->d_name;
    if (name == "." || name == "..") continue;
    if (!MergeNode(JoinPath(source_directory, name),
                   JoinPath(target_directory, name), context, error_message)) {
      ok = false;
      break;
    }
  }
  ::closedir(directory);
  return ok;
}

}  // namespace

bool ResolveSnapshotChain(const std::string& repository_directory,
                          const std::string& target_file_name,
                          SnapshotChain* chain, std::string* error_message) {
  if (chain == nullptr) {
    SetError(error_message, "Snapshot chain output must not be null");
    return false;
  }
  *chain = SnapshotChain{};
  if (!IsPlainSingleComponentName(target_file_name)) {
    SetError(error_message,
             "A snapshot file name must be a single path component");
    return false;
  }

  std::vector<std::string> reversed_files;
  std::vector<std::string> reversed_names;
  std::vector<std::string> visited;
  std::string current = target_file_name;
  std::string expected_parent_id;

  for (std::size_t depth = 0;; ++depth) {
    if (depth > kMaxDeltaChainDepth) {
      SetError(error_message, "The snapshot chain is deeper than " +
                                  std::to_string(kMaxDeltaChainDepth) +
                                  " levels");
      return false;
    }
    if (std::find(visited.begin(), visited.end(), current) != visited.end()) {
      SetError(error_message,
               "Snapshot chain contains a cycle at '" + current + "'");
      return false;
    }
    visited.push_back(current);

    const std::string path = JoinPath(repository_directory, current);
    const SnapshotFileKind kind = ClassifySnapshotFile(path, error_message);
    if (kind == SnapshotFileKind::kUnknown) {
      if (error_message != nullptr && error_message->empty()) {
        // 说清是"哪一个"以及"它是被当作谁的父亲在找"：链断掉时用户需要知道
        // 缺的是哪一份，而不是一句笼统的失败。
        SetError(error_message, expected_parent_id.empty()
                                    ? "Unknown snapshot file: " + current
                                    : "The parent snapshot is missing or "
                                      "unreadable: " +
                                          current);
      }
      return false;
    }

    // 父身份的核对：子节点记录的 parent_snapshot_id 必须等于父文件真实的身份。
    if (!expected_parent_id.empty()) {
      std::string actual_id;
      if (!SnapshotIdOfFile(path, &actual_id, error_message)) return false;
      if (actual_id != expected_parent_id) {
        SetError(error_message, "Snapshot '" + current +
                                    "' does not match the parent identity "
                                    "recorded by its child (the file was "
                                    "replaced or the chain is broken)");
        return false;
      }
    }

    reversed_files.push_back(path);
    reversed_names.push_back(current);

    if (kind == SnapshotFileKind::kContainer) {
      // 链的底必须真的是这份 delta 声明的 generation：否则这条链会被应用在
      // 一个不是它祖先的完整快照上，结果是一个从未存在过的目录树。
      std::string base_id;
      if (!FullSnapshotId(path, &base_id, error_message)) return false;
      if (!chain->base_generation_id.empty() &&
          chain->base_generation_id != base_id) {
        SetError(error_message,
                 "The full snapshot at the bottom of this chain is not the "
                 "generation the deltas were built against");
        return false;
      }
      chain->base_generation_id = base_id;
      break;
    }

    DeltaEnvelope envelope;
    if (!ReadDeltaEnvelope(path, &envelope, error_message)) return false;
    if (reversed_files.size() == 1) {
      chain->target_manifest_digest = envelope.current_manifest_digest;
      chain->base_generation_id = envelope.base_generation_id;
    } else if (envelope.base_generation_id != chain->base_generation_id) {
      SetError(error_message, "Snapshot chain mixes two generations; '" +
                                  current +
                                  "' belongs to a different full baseline");
      return false;
    }
    if (envelope.parent_file_name.empty()) {
      SetError(error_message, "Delta '" + current + "' has no parent");
      return false;
    }
    expected_parent_id = envelope.parent_snapshot_id;
    current = envelope.parent_file_name;
  }

  std::reverse(reversed_files.begin(), reversed_files.end());
  std::reverse(reversed_names.begin(), reversed_names.end());
  chain->files = std::move(reversed_files);
  chain->file_names = std::move(reversed_names);
  chain->delta_count = chain->files.size() - 1;
  return true;
}

bool RestoreSnapshotChain(const std::string& repository_directory,
                          const std::string& target_file_name,
                          const std::string& destination_directory,
                          const RestoreOptions& options, RestoreReport* report,
                          std::string* error_message) {
  SnapshotChain chain;
  if (!ResolveSnapshotChain(repository_directory, target_file_name, &chain,
                            error_message)) {
    return false;
  }
  if (destination_directory.empty()) {
    SetError(error_message, "The destination directory must not be empty");
    return false;
  }

  // destination 必须不存在或为空——与既有 restore 的对外承诺一致。
  struct stat info;
  if (::lstat(destination_directory.c_str(), &info) == 0) {
    if (!S_ISDIR(info.st_mode)) {
      SetError(error_message,
               "The destination exists and is not a directory: " +
                   destination_directory);
      return false;
    }
    bool empty = false;
    if (!IsDirectoryEmpty(destination_directory, &empty, error_message)) {
      return false;
    }
    if (!empty) {
      SetError(error_message, "The destination directory is not empty: " +
                                  destination_directory);
      return false;
    }
  } else if (errno != ENOENT) {
    SetError(error_message, "Cannot stat the destination: " + ErrnoText(errno));
    return false;
  }

  const std::string suffix =
      "." + std::to_string(static_cast<unsigned long>(::getpid()));
  const std::string staging = destination_directory + suffix + ".staging";
  const std::string overlay = destination_directory + suffix + ".overlay";
  const std::string inner_container =
      destination_directory + suffix + ".container";
  RemoveTree(staging);
  RemoveTree(overlay);
  ::unlink(inner_container.c_str());

  bool ok = false;
  do {
    // 1) base：走既有的完整恢复路径（它自己也是 staging + 原子发布的写法）。
    if (!RunRestorePipeline(chain.files.front(), staging, options, report,
                            error_message)) {
      break;
    }
    // 2) 逐个 delta 应用。
    for (std::size_t index = 1; index < chain.files.size(); ++index) {
      const std::string& delta = chain.files[index];
      DeltaEnvelope envelope;
      if (!ReadDeltaEnvelope(delta, &envelope, error_message)) break;
      if (!ExtractDeltaPayload(delta, inner_container, error_message)) break;
      RemoveTree(overlay);
      if (!RunRestorePipeline(inner_container, overlay, options, report,
                              error_message)) {
        break;
      }
      ::unlink(inner_container.c_str());

      // 2a) tombstone：深的先删。
      std::vector<std::string> tombstones = envelope.tombstones;
      SortDeepestFirst(&tombstones);
      for (const std::string& relative : tombstones) {
        if (relative.empty() || relative == ".") {
          SetError(error_message, "A tombstone may not remove the source root");
          ok = false;
          break;
        }
        const std::string path = JoinPath(staging, relative);
        if (!RemoveTree(path)) {
          SetError(error_message, "Cannot apply a tombstone: " + path);
          break;
        }
      }
      if (error_message != nullptr && !error_message->empty()) break;

      // 2b) 覆盖新增/修改/类型变化。
      MergeContext context;
      context.report = report;
      if (!MergeTree(overlay, staging, &context, error_message)) break;
      // 2c) 源根自己的 metadata：overlay 的根就是源根，它的 mode/uid/gid/mtime
      //     由 delta 的源根条目负责。合并只处理了子项，根要单独贴一次，
      //     否则"往根里写了东西"之后的根 mtime 就没人负责了。
      struct stat overlay_root;
      if (::lstat(overlay.c_str(), &overlay_root) == 0) {
        ApplyMetadataBestEffort(staging, overlay_root, false, &context);
      }
      if (report != nullptr) {
        report->restored_entries += context.merged_entries;
      }
      RemoveTree(overlay);
    }
    if (error_message != nullptr && !error_message->empty()) break;

    // 3) 发布。destination 已存在（空目录）时先删掉，保证 rename 是原子的。
    struct stat target_info;
    if (::lstat(destination_directory.c_str(), &target_info) == 0) {
      if (::rmdir(destination_directory.c_str()) != 0) {
        SetError(error_message,
                 "Cannot replace the destination: " + ErrnoText(errno));
        break;
      }
    }
    if (::rename(staging.c_str(), destination_directory.c_str()) != 0) {
      SetError(error_message,
               "Cannot publish the restored tree: " + ErrnoText(errno));
      break;
    }
    ok = true;
  } while (false);

  RemoveTree(staging);
  RemoveTree(overlay);
  ::unlink(inner_container.c_str());
  return ok;
}

}  // namespace backupproject
