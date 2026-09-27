// source_tree_walker.h
//
// **唯一一份**源目录树遍历。
//
// 为什么它必须存在：Backup 与 Preview 都在回答同一个问题——"这棵源目录树里
// 有什么、哪些会进归档"。这两件事曾经各写一套遍历（Backup 用
// opendir/readdir + lexical sort，Preview 用
// std::filesystem::recursive_directory_iterator + skip_permission_denied），
// 于是同一棵树在两边会得到不同的事实：
//
//   * source root 是 symlink-to-directory 时，一边拒绝、一边跟进去；
//   * 子目录不可读时，一边整次失败、一边静默跳过并报成功；
//   * 顺序一边是 lexical DFS、一边由实现定义；
//   * 没有被排除的 socket 一边让备份必然失败、一边被预览悄悄漏掉。
//
// 结果就是"预览说这份筛选可以备份"与"备份真的能成功"可以同时不成立——而
// 预览存在的唯一理由就是回答前者。所以遍历收敛到这一份实现：
//
//                WalkSourceTree(source, filter, visitor)
//                     |                        |
//        ArchiveEntry（Backup 消费者）   PreviewItem（Preview 消费者）
//
// 这一份实现负责（Backup 与 Preview 逐条共享）：
//   * source root 校验（lstat + 必须是真目录，symlink 不接受）；
//   * 每个条目的 lstat 语义（不 follow 软链接）；
//   * opendir / readdir，以及 readdir 的错误处理；
//   * 每层子项 lexical 排序后的 DFS 先序（顺序是合同，不是实现细节）；
//   * archive 相对路径构造（'/' 分隔，root 是 "."）与路径长度校验；
//   * EntryType 判定、mode / uid / gid / size / mtime（秒 + 纳秒）；
//   * user / group 名字解析（失败留空，不报错）；
//   * FilterEntry 构造，以及 ShouldPruneDirectory / ShouldIncludeFile /
//     ShouldSkipSpecialEntry 的判定顺序；
//   * **归档路径语法**（IsValidArchivePath：反斜杠 / 盘符 / 绝对路径 / 结尾
//   '/'、
//     空 component / "." / ".." / NUL）——但只在**这个条目真的会进入归档**时
//     才校验。只查长度会让预览把 Linux 上合法、归档里非法的名字（a\b.txt）
//     报成"可以备份"；而在 Filter 之前就校验全套 grammar，又会让"本来会被
//     规则排除、根本不会进归档"的名字提前阻塞整次备份——两种都是错的；
//   * 全部 fail-closed 失败语义（见下）。
//
// 各消费者自己保留（刻意不共享）：hardlink 编码、软链接目标读取、设备号、
// (st_dev, st_ino) 快照、payload 读取、打包 / 压缩 / 加密，以及预览的窗口与
// 展示方式。
//
// ---- 每个 child 的决策顺序（这就是合同）----
//
//   build archive-relative path
//   -> 长度硬边界（kMaxArchivePathLength；历史语义：在 lstat 与 Filter 之前）
//   -> lstat
//   -> 构造 FilterEntry
//   -> 目录：ShouldPruneDirectory 为真 -> 剪掉整棵子树，不做语法校验
//                                    为假 -> IsValidArchivePath -> 递归
//      特殊文件：ShouldSkipSpecialEntry 为真 -> 跳过，不做语法校验
//                                          为假 -> "Unsupported special type:
//                                                  socket"（**优先于**任何
//                                                  路径语法错误）
//      其余：ShouldIncludeFile 为真 -> IsValidArchivePath -> visitor
//                              为假 -> 跳过，不做语法校验
//
// 一句话：**只有真正进入归档的条目才必须满足完整的 archive grammar**。
// source root（"."）始终属于归档，所以它在 Walk() 里用 (true, true) 直接校验。
//
// ---- 失败语义：整次遍历失败，不产出半个结果 ----
//
// 下列情况一律返回 false，并给出与真实 Backup 报出的**同一句**原文：
//   * 源目录不存在 / 不是真目录 / lstat 失败（kSourceRoot）；
//   * 某个条目 lstat 失败（kInspect）；
//   * opendir / readdir 失败（kDirectoryRead）；
//   * 某条**会进入归档**的条目的路径没过 IsValidArchivePath
//     （kInvalidArchivePath，含遍历阶段的长度硬边界）；
//   * stat 给出的类型无法表示（kUnsupportedType）；
//   * 出现**没有被明确排除**的 socket（kSocket）——socket 不是可恢复备份，
//     静默跳过、跟随它、把它当普通文件复制，这三种做法都会让"备份成功"变成假话。
//
// 这条规则对 Preview 一视同仁：预览是"告诉用户 Backup 会发生什么"，不是
// "尽最大努力列一点文件"。
//
// 本文件是纯 C++17：不依赖 Qt，也不依赖任何第三方库。

#ifndef BACKUP_PROJECT_INCLUDE_SOURCE_TREE_WALKER_H_
#define BACKUP_PROJECT_INCLUDE_SOURCE_TREE_WALKER_H_

#include <cstdint>
#include <string>

#include "archive_entry.h"
#include "filter.h"

namespace backupproject {

// 一次 lstat 得到的全部事实。
//
// user_name / group_name 也在这里：它们同样是"同一棵树必须给出同一份元数据"
// 的一部分（user: / group: 规则在预览里命中、真实备份却漏掉，正是两边各解析
// 一次名字的后果）。解析失败留空，Filter 对空名字一律视为不匹配。
struct SourceEntryFacts {
  EntryType type = EntryType::kRegularFile;
  std::uint32_t mode = 0;
  std::uint32_t uid = 0;
  std::uint32_t gid = 0;
  std::int64_t mtime_sec = 0;
  std::uint32_t mtime_nsec = 0;
  // 普通文件的正文长度；其余类型为 0（软链接是例外：lstat 的 size 就是链接
  // 目标的字节数，消费者读链接目标时可以拿它当缓冲长度提示）。
  std::uint64_t size = 0;
  std::uint64_t device_id = 0;
  std::uint64_t inode = 0;
  std::uint32_t dev_major = 0;
  std::uint32_t dev_minor = 0;
  std::uint64_t link_count = 0;
  std::string user_name;
  std::string group_name;
};

// 这个条目被判定成了什么。判定顺序与 Filter 的语义一致：
// exclude 优先；存在 include 规则时必须命中至少一条；目录命中 exclude 时
// 整棵子树被剪掉（子树里的条目不会出现在遍历结果里）。
enum class SourceEntryDecision {
  // 进入归档。源目录自己永远是 kIncluded（archive_path == "."）。
  kIncluded,
  // 被规则排除。
  kExcludedByRule,
  // 目录命中 exclude：整棵剪掉。
  kDirectoryPruned,
};

enum class SourceWalkFailureKind {
  kNone,
  // 源目录本身不可用（不存在 / 不是真目录 / lstat 失败）。
  kSourceRoot,
  // 某个条目的 lstat 失败（例如枚举之后、stat 之前被删掉）。
  kInspect,
  // opendir / readdir 失败（例如权限不足、目录被删掉、I/O 错误）。
  kDirectoryRead,
  // 归档路径没过 IsValidArchivePath 的完整语法：太长、含反斜杠、Windows 盘符、
  // 绝对路径、结尾 '/'、空 component、"." / ".." component、含 NUL。
  // 这是**同一个** grammar：任何真实 Backup 最终无法接受的 archive path，
  // 预览都在这里被判死。
  kInvalidArchivePath,
  // stat 给出的类型无法表示。
  kUnsupportedType,
  // 没有被明确排除的 socket：真实 Backup 必然失败。
  kSocket,
  // 消费者自己失败（例如写侧路径校验、读软链接目标失败）。
  kConsumerFailed,
};

struct SourceWalkFailure {
  SourceWalkFailureKind kind = SourceWalkFailureKind::kNone;
  // 与真实 Backup 报出的同一句原文。GUI / CLI 都直接转述它，不各自翻译。
  std::string message;
  // 出问题的条目：磁盘路径与归档相对路径（源目录自己是 "."）。
  std::string disk_path;
  std::string archive_path;
};

// 遍历访问者。判断"这个条目会不会进归档"的只有 walker 一份实现，消费者只
// 决定拿这些事实做什么。
class SourceTreeVisitor {
 public:
  virtual ~SourceTreeVisitor() = default;

  // 每个被遍历到的条目调用一次，顺序 = 遍历顺序（DFS 先序，同级 lexical
  // 升序）。源目录自己会出现一次（archive_path == "."，kIncluded）。
  //
  // 返回 false 表示消费者失败：遍历立即结束，WalkSourceTree 返回 false，
  // failure->kind == kConsumerFailed，failure->message 取自 error_message。
  virtual bool OnEntry(const std::string& disk_path,
                       const std::string& archive_path,
                       const SourceEntryFacts& facts,
                       SourceEntryDecision decision,
                       std::string* error_message) = 0;
};

// 测试注入点。**生产代码永远传 nullptr**（默认），此时每个 syscall 都按真
// 实语义执行。
//
// 为什么需要它：权限类失败（EACCES on opendir）在 root / CAP_DAC_OVERRIDE
// 下根本造不出来，靠 chmod 000 写出来的用例会在那种环境里静默变成"通过"。
// 所以错误路径用一个很窄的 seam 覆盖：
//
//   * 它只回答"这一次 syscall 该不该失败、失败时 errno 是多少"；
//   * 它不提供目录内容、不提供 stat 结果、不替换任何真实语义；
//   * 它不能把一次成功变成"别的事实"，只能把一次调用变成失败。
//
// 也就是说它注入的是失败，而不是一套假的文件系统。
enum class SourceWalkSyscall { kLstat, kOpenDirectory, kReadDirectory };

struct SourceWalkFaults {
  // 返回 0 表示"照常调用真实 syscall"；返回非 0 表示"用这个 errno 让这次
  // 调用失败"。
  int (*fail_syscall)(SourceWalkSyscall call, const std::string& disk_path,
                      void* context) = nullptr;
  void* context = nullptr;
};

// 遍历 source_directory。filter 为 nullptr 等价于"没有任何规则"。
//
// 失败时返回 false 并填好 failure（kind / message / disk_path /
// archive_path）。failure 可以为 nullptr；error_message 不属于本接口——
// 消费者要的是结构化失败原因，调用方要人话就取 failure.message。
bool WalkSourceTree(const std::string& source_directory, const Filter* filter,
                    SourceTreeVisitor* visitor, SourceWalkFailure* failure,
                    const SourceWalkFaults* faults = nullptr);

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_SOURCE_TREE_WALKER_H_
