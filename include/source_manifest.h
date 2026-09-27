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

  // ---- 内容身份（version 3 才写盘）----
  //
  // 普通文件：正文的 SHA-256。软链接：目标字节的 SHA-256。
  // 其余类型为空：它们的身份由上面的字段唯一决定（类型 + 设备号 + hardlink
  // 关系），再算一遍摘要只是重复。
  //
  // 空串的含义是"这一版没有内容摘要"（version 1 / 2 写出来的 manifest）。
  // 调用方**不得**把空串当成"内容为空"，也不得把 v1/v2 当成增量基线。
  std::string content_digest;

  // 磁盘上的真实路径。**刻意不参与序列化**：它只用于生成摘要时读正文，
  // 写进 manifest 等于把源目录的绝对路径留在磁盘上。
  std::string source_path;
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
// ---- 强化版 manifest：增量备份的内容身份 ----
//
// 集合与 BuildSourceManifest 完全一致（同一个 ScanSourceTree、同一个 Filter、
// 同一套 socket 规则），额外做两件事：
//
//   * 每个 included 普通文件读一遍正文算 SHA-256；
//   * 每个软链接算目标字节的 SHA-256。
//
// 第一版刻意**全量哈希**：size+mtime 摘要缓存自己就是一个 correctness 问题
// （失效判断写错就会漏掉真实变化），性能优化留到以后。
//
// 读数期间源发生改动（size / mtime 与扫描时不一致，或条目消失）会让整次构建
// 失败：摘要必须描述一个真实存在过的状态，否则 manifest 会把"读到的内容"和
// "记录的元数据"拼成一个从未存在过的版本。
bool BuildStrongSourceManifest(const std::string& source_directory,
                               const Filter* filter,
                               std::vector<ManifestEntry>* entries,
                               std::string* error_message);

// 这份 manifest 是否带齐了内容身份：所有普通文件与软链接都有合法摘要。
// 只有它为真时，这份 manifest 才可以当作增量链的基线。
bool HasContentDigests(const std::vector<ManifestEntry>& entries);

// manifest 自身的摘要（64 个小写十六进制）。覆盖"规范化的 v3 正文"：
// 条目先按 archive_path 排序再序列化，所以同一个 source state 无论遍历细节
// 如何，摘要都可复现。它不包含 binding——binding 说的是"这份 manifest 属于
// 哪一份快照"，不是源的内容身份。
std::string ManifestDigest(const std::vector<ManifestEntry>& entries);

bool DiffManifests(const std::vector<ManifestEntry>& previous,
                   const std::vector<ManifestEntry>& current,
                   ChangeSummary* summary,
                   std::vector<std::string>* changed_paths,
                   std::string* error_message);

// ---- 这份 manifest 属于哪一份真实快照 ----
//
// 单独一份 manifest 只说明"上一次扫描到的源长这样"，它**证明不了**仓库里还有
// 一份与它对应的完整快照。三个字段各自钉住一个维度：
//
//   * snapshot_file_name  哪一份快照（单组件 .bak 名字，绝不存绝对路径）；
//   * repository_identity 哪一个仓库（稳定 identity，不是文件名猜测）；
//   * source_path         哪一个源目录（换了源、恰好 manifest
//   相似时不能误用）。
//
// 为什么必须写进文件本身，而不是只留在 schedule.json 里：archive / manifest /
// schedule.json
// 是三个独立文件，各自原子替换，**没有任何时刻能让三个一起提交**。
// 于是"manifest 与 state 说的不是同一份快照"这种中间状态一定会出现——只要进程
// 崩在两次写盘之间就会留下它。把归属写进 manifest 之后，下一轮只要发现
// manifest 自己声明的归属与 state 记录的 baseline
// 不一致，就能判定这一对不可信，
// 老老实实重建一份完整快照。多建一份，绝不错误跳过。
//
// 反过来说：只比对"源有没有变化"是不够的。崩在 SaveManifest 与 Save(state)
// 之间时，state 里还是旧的 baseline S1，manifest 却已经是新源状态 M2，而 S1
// 依然真实存在于仓库里——"baseline 存在"+"current == manifest" 两条同时成立，
// 于是错误地跳过一轮，而仓库里根本没有任何一份快照装得下 M2。
struct ManifestBinding {
  std::string snapshot_file_name;
  std::string repository_identity;
  std::string source_path;

  // 三个字段全空 = 这份 manifest 没有归属信息（version 1 格式）。
  // 调用方**不得**把它当成可信基线。
  bool empty() const {
    return snapshot_file_name.empty() && repository_identity.empty() &&
           source_path.empty();
  }
};

// ---- 序列化 ----
//
// 行式文本，不是 JSON：manifest 可能有几十万条，塞进 schedule.json 会让每次
// 读写都在解析一个巨大的对象。
//
// 当前写出格式是 version 2，头行在版本与条数之外多带三个转义字段，也就是上面
// 那层归属（TAB 分隔）：
//
//   BPMANIFEST2 <entry_count>\t<escaped baseline snapshot file name>
//     \t<escaped baseline repository identity>\t<escaped baseline source
//     path>\n
//   <type_id>\t<size>\t<mtime_sec>\t<mtime_nsec>\t<mode>\t<uid>\t<gid>
//     \t<dev_major>\t<dev_minor>\t<hardlink_degree>
//     \t<escaped archive_path>\t<escaped link_target>\n
//
// version 1（BPMANIFEST1 <entry_count>\n ...）仍然**读得出来**，但解析结果里
// binding 是空的，也就是"不可信基线"。升级语义因此是单向安全的：v1 用户升级后
// 最多多建一份完整快照，绝不会因此漏掉一次变化。
//
// 转义只作用于字符串字段：反斜杠、TAB、换行、回车。
// 解析严格：头必须完全匹配、条数必须与正文一致、不接受多余字节、每个数字都
// 做范围检查——manifest 是机器写的，出现偏差就是状态坏了，必须报错而不是尽力猜。

inline constexpr std::size_t kMaxManifestBytes = 64u * 1024u * 1024u;
inline constexpr std::size_t kMaxManifestEntries = 2000000u;
inline constexpr std::size_t kMaxManifestLineBytes = 64u * 1024u;
// binding 三个字段各自的长度上界。它与 kMaxScheduleStringBytes 取同一个量级：
// 这里存的是路径与文件名，没有理由更长。
inline constexpr std::size_t kMaxManifestBindingBytes = 4096u;

// 写出 version 2。binding 必须完整（三个字段非空、名字是合法的单组件名、
// 长度有限、不含 NUL），否则返回空串——宁可什么都不写，也不写一份归属不明的
// manifest 出去，那恰好是本次修复要消灭的状态。
std::string SerializeManifest(const std::vector<ManifestEntry>& entries,
                              const ManifestBinding& binding);

// 写出 version 3：在 v2 的 12 个字段之后追加第 13 个字段——内容摘要。
//
//   BPMANIFEST3 <entry_count>\t<binding...>\n
//   <12 个 v2 字段>\t<escaped content_digest>\n
//
// 普通文件与软链接必须带摘要，否则返回空串（宁可什么都不写，也不写一份
// 自称 v3、却没有内容身份的 manifest 出去——那正是假增量的入口）。
std::string SerializeManifestV3(const std::vector<ManifestEntry>& entries,
                                const ManifestBinding& binding);

// 写出 version 1（没有 binding）。存在的理由只有一个：兼容性与迁移测试需要
// 造出一份"旧版本留下的 manifest"。**生产路径一律用上面那个带 binding
// 的版本。**
std::string SerializeManifestV1(const std::vector<ManifestEntry>& entries);

// binding 非空且合法时填进 *binding；读到的是 version 1 时 *binding 留空。
// 调用方据此区分"可信归属"与"旧格式，必须重建基线"。
bool ParseManifest(const std::string& text, std::vector<ManifestEntry>* entries,
                   ManifestBinding* binding, std::string* error_message);

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_SOURCE_MANIFEST_H_
