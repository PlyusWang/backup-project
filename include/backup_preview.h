// backup_preview.h
//
// **Manual Backup 的筛选预览核心**：源目录 + 规则 -> 一份"哪些条目会进入
// 归档"的只读快照。
//
// 为什么它必须是一个独立的、Qt 无关的核心：
//
//   Modern GUI 的 Manual Backup 是"填规则 -> 预览 -> 备份"，product CLI 也必须
//   是同一件事。如果预览由 Qt 控制器自己实现，CLI 就只剩"直接备份"这一半能力；
//   如果 CLI 再写一份扫描器，两边就会各自漂移。所以那份实现被搬到这里，
//   三个消费者共用同一段代码：
//
//       GUI 预览  --//       CLI 预览  ----> PreviewBackupSelection() -->
//       Filter（唯一语义来源） 真实备份  --/                                ^
//                                                    |
//                            BackupEngine / TreeScanner 用的也是同一个 Filter
//
// 本文件是纯 C++17：不依赖 Qt，也不依赖任何第三方库，因此它可以被单元测试
// 直接覆盖，也可以被 ASan/UBSan 直接跑。
//
// ---- 与真实备份的关系（不变量，不是"尽量一致"）----
//
// 预览的 included 集合 == 用同一个 source + 同一组规则真实备份出来的条目集合。
// 匹配判定全部问 backupproject::Filter，本文件里没有一条 glob、后缀或大小判断。
//
// ---- 预览窗口 ----
//
// 扫描在收集到 limit 条**被检查过的条目**之后停止，并把 truncated 置位。
// 这与 GUI 一直以来的行为一致（目录太大时只显示开头部分），也是 GUI 与 CLI
// 共用的截断契约：同一个 limit、同一面窗口、同一个 truncated 标志。
// 所以 truncated 的含义是"源目录里还有没被检查的条目"，而不是"匹配项超过
// limit 条"——被检查过的条目里既有 included 也有 excluded，两者都占窗口。

#ifndef BACKUP_PROJECT_INCLUDE_BACKUP_PREVIEW_H_
#define BACKUP_PROJECT_INCLUDE_BACKUP_PREVIEW_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "archive_entry.h"
#include "filter_rule_builder.h"
#include "source_tree_walker.h"

namespace backupproject {

// 预览窗口的默认大小：最多**列出** 300 个条目。
//
// 注意窗口限制的是展示，不是检查：整棵源目录树都会被遍历与验证
// （见 PreviewBackupSelection 的说明）。把一个 30 万文件的目录检查完是预览
// 该付的代价——如果为了快就在第 300 项停下，第 301 项上的 socket 或权限错误
// 就会被漏掉，"预览说可以备份"这句话也就不再成立。
//
// 这是 GUI 与 CLI 共用的常量，不是某一端的显示偏好：两边报出来的
// truncated 必须来自同一个数。
inline constexpr std::size_t kPreviewEntryLimit = 300;

// 一个条目为什么进 / 不进归档。界面据此给出人话，CLI 只需要 included 这一位。
//
// 只有三种取值：遍历的判定就是三选一（见 SourceEntryDecision）。"没有被排除的
// socket"不在这里——它不让遍历继续走下去，而是让整次遍历失败（真实 Backup 也
// 是这么做的），所以它出现在 PreviewResult 的失败字段里，而不是某一行上。
enum class PreviewDisposition {
  // 进入归档（普通文件 / 软链接 / FIFO / 设备命中 include；目录保留结构）。
  kIncluded,
  // 命中了 exclude 规则。
  kExcludedByRule,
  // 目录命中 exclude：整棵子树被剪掉，子树里的条目不会再出现在结果里。
  kDirectoryPruned,
};

struct PreviewItem {
  // 归档内部路径：相对源目录、'/' 分隔，与 ArchiveEntry::archive_path
  // 同一语义。 预览不泄漏磁盘上的绝对路径。
  std::string archive_path;
  EntryType type = EntryType::kRegularFile;
  bool is_directory = false;
  // 普通文件的正文长度；其余类型为 0。
  std::uint64_t size = 0;
  std::int64_t mtime_sec = 0;
  std::uint32_t uid = 0;
  std::uint32_t gid = 0;
  bool included = false;
  PreviewDisposition disposition = PreviewDisposition::kExcludedByRule;
};

// 预览为什么没有结果。三类失败对应三种完全不同的处置：
//
//   kSourceUnusable    用户把源目录写错了（界面要给中文提示）；
//   kRuleRejected      规则没能编译：核心 Filter::AddRule 的原文，原样转述；
//   kSelectionBlocked  源目录与规则都没问题，但这份选择**无法被成功备份**
//                      （目前只有"没有被明确排除的 socket"）。这不是语法错误，
//                      所以 CLI 用 exit 1 而不是 2；
//   kScanFailed        遍历本身失败（lstat / opendir / readdir / 路径过长 /
//                      无法表示的类型）——与真实 Backup 完全同一套失败语义。
enum class PreviewErrorKind {
  kNone,
  kSourceUnusable,
  kRuleRejected,
  kSelectionBlocked,
  kScanFailed,
};

struct PreviewResult {
  // **显示窗口**：遍历顺序里的前 limit 条（含被排除的条目，界面要逐条标注）。
  // included 为真的条目就是"会进入归档"的那些。
  std::vector<PreviewItem> items;
  // 整棵树里被检查过的条目总数（不受窗口限制）。
  std::size_t total_entries = 0;
  // 整棵树里会进入归档的条目数。这是**全量**数字，不是窗口里的数字：
  // 第 301 个条目也是 socket 时，预览必须报失败而不是"前 300 个看起来没问题"。
  std::size_t included_count = 0;
  // 还有条目没有进窗口（items 不是全部）。
  bool truncated = false;
  // 非空表示这次预览没有给出结果。此时 items 为空，调用方应当把 error 报给
  // 用户，而不是显示"0 项"——"一项都没匹配"和"这次预览没跑成"是两件事。
  std::string error;
  PreviewErrorKind error_kind = PreviewErrorKind::kNone;
  // 让这次预览失败的那个条目（归档相对路径 / 磁盘路径）。kSelectionBlocked
  // 时它就是"必须先排除掉的条目"；其它失败时用来定位。
  std::string blocking_archive_path;
  std::string blocking_disk_path;
};

// 扫描 source_directory，按 rules 判定每个条目会不会进入归档。
//
// **遍历与真实 Backup 完全共用一份实现**（src/core/source_tree_walker.cpp）：
// 同样的 source root 校验、同样的 lstat、同样的 opendir/readdir 与失败语义、
// 同样的 lexical DFS 顺序、同样的路径长度校验、同样的剪枝与 socket 规则。
// 所以"预览会选中这些"与"备份会写入这些"不可能因为遍历差异而分叉。
//
// 两件事分开，别混：
//   * 整棵树都会被检查（否则第 301 个条目是 socket 时预览会撒谎）；
//   * 只有前 limit 条会进 items（展示窗口）。
//
// 只读：不创建归档、不碰 repository / config / schedule / history，也不写任何
// 临时文件。失败时返回 error，不抛异常。
//
// limit 为 0 时按 kPreviewEntryLimit 处理（"没有窗口"不是一个有意义的请求）。
PreviewResult PreviewBackupSelection(const std::string& source_directory,
                                     const std::vector<FilterRuleDraft>& rules,
                                     std::size_t limit = kPreviewEntryLimit);

// 同一件事，但允许注入 filesystem 失败。
//
// 只给测试用：生产调用方一律用上面那个重载。存在的理由与
// ScanSourceTree 的注入重载完全一样——"预览遇到 opendir/readdir/lstat 失败
// 也必须 fail closed"这条语义要和真实 Backup **对着同一次注入**一起验证，
// 而权限类失败在 root / CAP_DAC_OVERRIDE 下根本造不出来。注入点只能把一次
// syscall 变成失败，不能伪造文件系统内容（见 SourceWalkFaults）。
PreviewResult PreviewBackupSelection(const std::string& source_directory,
                                     const std::vector<FilterRuleDraft>& rules,
                                     std::size_t limit,
                                     const SourceWalkFaults* faults);

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_BACKUP_PREVIEW_H_
