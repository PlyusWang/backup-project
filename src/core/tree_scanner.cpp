// tree_scanner.cpp
//
// 见 tree_scanner.h。
//
// 遍历本身不在这里：它是 src/core/source_tree_walker.cpp 里那一份共享实现，
// Backup 与 Preview 都用它（见 include/source_tree_walker.h 开头的说明）。
// 这一层只做"把遍历给出的事实变成 ArchiveEntry"——也就是真正属于归档格式的
// 那部分：
//
//   * hardlink 编码（同一 (st_dev, st_ino) 第二次出现时写成指向第一条的链接）
//   * 软链接目标原文（readlink）
//   * (st_dev, st_ino) 快照，供打包阶段复核"读的还是扫描时那一个 inode"
//   * 写侧归档路径校验（IsValidArchivePath）
//
// v0.1 写入器的关系不变：v0.1 的 WriteDirectoryTree 只认目录和普通文件，
// 遇到软链接/FIFO/设备/socket 一律让整次备份失败。v2 的扫描器把前三类变成
// 一等公民，只保留 socket 的"要么被明确排除、要么整次失败"语义。

#include "tree_scanner.h"

#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "archive_path.h"
#include "source_tree_walker.h"

namespace backupproject {
namespace {

void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) {
    *error_message = text;
  }
}

// 同一次扫描里出现过的 inode。hardlink 检测只在"这一棵树内部"成立：
// 跨备份的 inode 复用没有意义，也不该被当成同一次复制。
struct InodeKey {
  std::uint64_t device = 0;
  std::uint64_t inode = 0;

  bool operator<(const InodeKey& other) const {
    if (device != other.device) {
      return device < other.device;
    }
    return inode < other.inode;
  }
};

// 软链接目标原文。readlink 不 follow，读到的是链接自己存的那串字节。
// 用 lstat 的 size 作为起始缓冲长度：对软链接来说它就是目标的字节数。
// 用 4 次尝试 × 每次翻倍来对付“读到一半目标又被改长”，上限 64 KiB。
bool ReadLinkTarget(const std::string& disk_path, std::uint64_t hint,
                    std::string* target, std::string* error_message) {
  std::size_t size = static_cast<std::size_t>(hint);
  if (size == 0 || size > kMaxArchivePathLength) {
    size = kMaxArchivePathLength;
  }
  for (int attempt = 0; attempt < 4; ++attempt) {
    std::vector<char> buffer(size + 1);
    const ssize_t got = ::readlink(disk_path.c_str(), buffer.data(), size);
    if (got < 0) {
      SetError(error_message, "Failed to read symbolic link: " + disk_path +
                                  ": " + std::strerror(errno));
      return false;
    }
    if (static_cast<std::size_t>(got) < size) {
      target->assign(buffer.data(), static_cast<std::size_t>(got));
      return true;
    }
    // 缓冲被填满说明目标可能更长（竞争修改），加倍再来一次。
    size *= 2;
    if (size > 64 * 1024) {
      SetError(error_message, "Symbolic link target too long: " + disk_path);
      return false;
    }
  }
  SetError(error_message, "Symbolic link target too long: " + disk_path);
  return false;
}

// Backup 消费者：把 walker 给出的事实落成 ArchiveEntry。
// 生命周期：栈上构造、只在一次 WalkSourceTree 调用内存活，所以 seen_inodes_
// 天然就是“单次扫描”的作用域，不会被下一次扫描复用（跨备份的 inode 复用
// 没有意义，见上面的 InodeKey 说明）。
class ArchiveEntryBuilder : public SourceTreeVisitor {
 public:
  explicit ArchiveEntryBuilder(std::vector<ArchiveEntry>* entries)
      : entries_(entries) {}

  // 消费者契约：返回 false 表示“这次扫描整体失败”，walker 会立刻停止并把它
  // 转成 kConsumerFailed —— 所以这里的每个 return false 之前都必须先写好
  // error_message，没有别人会替你补。
  bool OnEntry(const std::string& disk_path, const std::string& archive_path,
               const SourceEntryFacts& facts, SourceEntryDecision decision,
               std::string* error_message) override {
    // 被排除 / 被剪枝的条目与归档无关，Backup 侧不需要它们。
    if (decision != SourceEntryDecision::kIncluded) return true;

    ArchiveEntry entry;
    // 条目顺序即归档顺序：walker 是确定的 lexical DFS，同一份输入必然产生
    // 逐字节相同的条目序列，hardlink 的“第一条”因此可复现，不随 readdir
    // 顺序漂移。
    entry.archive_path = archive_path;
    entry.source_path = disk_path;
    entry.type = facts.type;
    entry.mode = facts.mode;
    entry.uid = facts.uid;
    entry.gid = facts.gid;
    entry.mtime_sec = facts.mtime_sec;
    entry.mtime_nsec = facts.mtime_nsec;
    entry.size = facts.size;
    entry.dev_major = facts.dev_major;
    entry.dev_minor = facts.dev_minor;
    // 扫描那一刻的 (st_dev, st_ino)。这是**内部快照字段**，不写进任何归档
    // 格式，存在的意义只有一个：打包时确认"我现在读的还是扫描时那一个
    // inode"。USTAR 的两个写入器在读完 payload 之后会拿它复核。
    entry.source_dev = facts.device_id;
    entry.source_ino = facts.inode;

    // 写侧也走读侧那一套路径规则：保证"自己能产出"蕴含"读侧能接受"。
    // is_directory 只在 path == "." 时起作用，其余路径不看它。
    if (!IsValidArchivePath(archive_path, archive_path == ".",
                            facts.type == EntryType::kDirectory,
                            kMaxArchivePathLength, error_message)) {
      return false;
    }

    switch (facts.type) {
      case EntryType::kDirectory:
        break;
      case EntryType::kRegularFile: {
        // hardlink：同一 (st_dev, st_ino) 第二次出现时只写一条"指向第一次
        // archive_path"的 hardlink 条目，绝不重复存一份 payload。
        // link_count 来自 lstat：1 表示只有这一条目录项，>1 才是硬链接。
        // 扫描按路径走，同一 inode 只能靠这张表认出“刚才已经存过它”。
        if (facts.link_count > 1) {
          const InodeKey key{facts.device_id, facts.inode};
          const auto found = seen_inodes_.find(key);
          if (found != seen_inodes_.end() && found->second != archive_path) {
            entry.type = EntryType::kHardLink;
            entry.link_target = found->second;
            entry.size = 0;
            entry.source_path.clear();
            break;
          }
          seen_inodes_.emplace(key, archive_path);
        }
        break;
      }
      case EntryType::kSymlink: {
        std::string target;
        if (!ReadLinkTarget(disk_path, facts.size, &target, error_message)) {
          return false;
        }
        if (target.empty()) {
          SetError(error_message, "Empty symbolic link target: " + disk_path);
          return false;
        }
        entry.link_target = target;
        break;
      }
      case EntryType::kFifo:
      case EntryType::kCharDevice:
      case EntryType::kBlockDevice:
        break;
      case EntryType::kHardLink:
      case EntryType::kSocket:
        // walker 不会把这两种交过来；放在这里是为了让 switch 完整。
        SetError(error_message, "Unsupported source entry type: " + disk_path);
        return false;
    }

    // 名字由 walker 统一解析（Backup 与 Preview 同一份），这里直接用。
    entry.user_name = facts.user_name;
    entry.group_name = facts.group_name;
    entries_->push_back(std::move(entry));
    return true;
  }

 private:
  std::vector<ArchiveEntry>* entries_ = nullptr;
  std::map<InodeKey, std::string> seen_inodes_;
};

}  // namespace

bool ScanSourceTree(const std::string& source_directory, const Filter* filter,
                    std::vector<ArchiveEntry>* entries,
                    std::string* error_message) {
  return ScanSourceTree(source_directory, filter, entries, error_message,
                        nullptr);
}

bool ScanSourceTree(const std::string& source_directory, const Filter* filter,
                    std::vector<ArchiveEntry>* entries,
                    std::string* error_message,
                    const SourceWalkFaults* faults) {
  if (error_message != nullptr) {
    error_message->clear();
  }
  if (entries == nullptr) {
    SetError(error_message, "Internal error: null entry list");
    return false;
  }
  if (source_directory.empty()) {
    SetError(error_message, "Source directory is empty.");
    return false;
  }

  std::vector<ArchiveEntry> scanned;
  ArchiveEntryBuilder builder(&scanned);
  SourceWalkFailure failure;
  if (!WalkSourceTree(source_directory, filter, &builder, &failure, faults)) {
    SetError(error_message, failure.message);
    return false;
  }
  *entries = std::move(scanned);
  return true;
}

}  // namespace backupproject
