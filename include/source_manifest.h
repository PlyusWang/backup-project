// source_manifest.h
//
// 源目录树的"内容身份"快照，以及两份快照之间的变化摘要。
//
// 用途只有一个：让定时备份回答"这一轮到底有没有变化"。它是**变化检测**，
// 不是增量存储——有变化时照旧生成一份完整独立的 .bak，绝不产生任何
// baseline 依赖链。
//
// 与备份集合的一致性：manifest 由 ScanSourceTree 产出，也就是备份自己用的
// 那个扫描器。同一个 Filter、同一套类型判定、同一套 socket 规则，因此
// "manifest 看到的集合"与"备份实际写入的集合"不可能漂移。
// 特别地：遇到不该归档的 socket 时扫描本身就失败，manifest 跟着失败，
// 而不是悄悄少一条。
//
// 性能策略是 metadata-first：只 lstat，不读任何文件内容，不给普通文件算
// SHA-256。代价必须说清楚——
//
//   *** 已知盲区：same-size + same-mtime 的人为 in-place rewrite ***
//   *** 逃得过这一版变化检测。这不是密码学意义上的完整性校验。 ***
//
// 未来的 Incremental / Realtime 可以强化这一点；本 PR 不声称能做到。
//
// 本文件是纯 C++17：不依赖 Qt，也不依赖任何第三方库。

#ifndef BACKUP_PROJECT_INCLUDE_SOURCE_MANIFEST_H_
#define BACKUP_PROJECT_INCLUDE_SOURCE_MANIFEST_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "archive_entry.h"
#include "filter.h"

namespace backupproject {

// manifest 里的一条记录。字段与 ArchiveEntry 一一对应，外加一个
// hardlink 连带计数。
struct ManifestEntry {
  // 归档内部路径：相对 source root、'/' 分隔，source root 自身是 "."。
  std::string archive_path;
  EntryType type = EntryType::kRegularFile;

  std::uint64_t size = 0;
  std::int64_t mtime_sec = 0;
  std::uint32_t mtime_nsec = 0;
  std::uint32_t mode = 0;
  std::uint32_t uid = 0;
  std::uint32_t gid = 0;

  // symlink 的目标原文；hardlink 指向的 archive_path；其余类型为空。
  std::string link_target;

  // 字符/块设备的 major/minor；其余类型为 0。
  std::uint32_t dev_major = 0;
  std::uint32_t dev_minor = 0;

  // 有多少条 hardlink 条目把本路径当作 leader（即 link_target == 本路径的
  // 条目数）。它是"canonical relation"之外额外记录的一条 hardlink 身份信息：
  // 新建/删除一个指向同一 inode 的硬链接时，leader 自己的其它字段一字未变，
  // 只有这个计数会动。
  std::uint32_t hardlink_degree = 0;
};

// 变化摘要。四个计数互斥，绝不重复计数：一条 path 只会落进其中一个桶。
struct ChangeSummary {
  std::uint64_t added = 0;
  std::uint64_t removed = 0;
  std::uint64_t modified = 0;
  std::uint64_t metadata_changed = 0;

  bool empty() const {
    return added == 0 && removed == 0 && modified == 0 && metadata_changed == 0;
  }
};

std::uint64_t ChangeSummaryTotal(const ChangeSummary& summary);

// 生成 manifest。filter 为 nullptr 表示没有规则（等价于空 Filter）。
//
// 失败语义与 ScanSourceTree 完全一致（整次失败，不产出半个结果）：
// lstat / opendir 出错、归档路径超长、出现会被归档的 socket。
bool BuildSourceManifest(const std::string& source_directory,
                         const Filter* filter,
                         std::vector<ManifestEntry>* entries,
                         std::string* error_message);

// 比较两份 manifest。previous 为空等价于"没有上一份快照"——但那种情况请直接
// 走 first-run 语义，不要调用本函数。
//
// 分类规则（互斥，按此顺序判断，命中即停）：
//   * 只出现在 current              -> added
//   * 只出现在 previous             -> removed
//   * 类型变了                       -> modified
//   * 普通文件 size / mtime 变了     -> modified
//   * symlink / hardlink target 变了 -> modified
//   * 字符/块设备 major/minor 变了   -> modified
//   * 否则 mode / uid / gid 变了     -> metadata_changed
//   * 否则 FIFO / 软链接 / 设备的 mtime 变了 -> metadata_changed
//   * 否则 hardlink_degree 变了      -> metadata_changed
//
// 统一的目录约定：**目录的 mtime 不参与比较**。这是一条刻意的、有测试钉住的
// 已知盲区。任何子项的新增/删除都会顺带改掉父目录的 mtime，把它算成变化会让
// "新增一个被 filter 排除的文件"也触发一次完整快照，而实际备份集合并没有变
// ——这正好违反"schedule 实际备份集合没变就不该建新快照"这一条产品语义。
// 代价是"单独 touch 一个目录、内容不变"看不出来。
//
// hardlink 条目只比较 link_target：它在 inode 上的 size / mtime / mode /
// uid / gid 全部由 leader 那条记录负责，比较两次只会制造重复计数。
//
// changed_paths 可以为空；非空时按 archive_path 升序填入发生变化的路径。
bool DiffManifests(const std::vector<ManifestEntry>& previous,
                   const std::vector<ManifestEntry>& current,
                   ChangeSummary* summary,
                   std::vector<std::string>* changed_paths,
                   std::string* error_message);

// ---- 序列化 ----
//
// 行式文本，不是 JSON：manifest 可能有几十万条，塞进 schedule.json 会让每次
// 读写都在解析一个巨大的对象。格式如下（字段间是 TAB，首行是版本头）：
//
//   BPMANIFEST1 <entry_count>\n
//   <type_id>\t<size>\t<mtime_sec>\t<mtime_nsec>\t<mode>\t<uid>\t<gid>
//     \t<dev_major>\t<dev_minor>\t<hardlink_degree>
//     \t<escaped archive_path>\t<escaped link_target>\n
//
// 转义只作用于两个字符串字段：反斜杠、TAB、换行、回车。
// 解析严格：头必须完全匹配、条数必须与正文一致、不接受多余字节、每个数字都
// 做范围检查——manifest 是机器写的，出现偏差就是状态坏了，必须报错而不是尽力猜。

inline constexpr std::size_t kMaxManifestBytes = 64u * 1024u * 1024u;
inline constexpr std::size_t kMaxManifestEntries = 2000000u;
inline constexpr std::size_t kMaxManifestLineBytes = 64u * 1024u;

std::string SerializeManifest(const std::vector<ManifestEntry>& entries);

bool ParseManifest(const std::string& text, std::vector<ManifestEntry>* entries,
                   std::string* error_message);

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_SOURCE_MANIFEST_H_
