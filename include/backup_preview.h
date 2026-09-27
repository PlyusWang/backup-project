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

namespace backupproject {

// 预览窗口的默认大小：一次最多检查 300 个条目。
//
// 这是 GUI 与 CLI 共用的常量，不是某一端的显示偏好：两边报出来的
// truncated 必须来自同一个数。
inline constexpr std::size_t kPreviewEntryLimit = 300;

// 一个条目为什么进 / 不进归档。界面据此给出人话，CLI 只需要 included 这一位。
enum class PreviewDisposition {
  // 进入归档（普通文件 / 软链接 / FIFO / 设备命中 include；目录保留结构）。
  kIncluded,
  // 命中了 exclude 规则。
  kExcludedByRule,
  // 目录命中 exclude：整棵子树被剪掉，子树里的条目不会再出现在结果里。
  kDirectoryPruned,
  // socket 且没有被显式排除。归档格式装不下 socket，真实备份会整次失败，
  // 所以它既不算 included，也不是"被规则排除"——是第三种后果。
  kUnsupportedSocket,
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

// 预览为什么没有结果。区分这两件事是有意义的：源目录不可用是用户路径写错了
// （界面要给中文提示），规则被拒绝是核心的语法原文（界面原样转述，不翻译）。
enum class PreviewErrorKind {
  kNone,
  // 源目录不存在 / 不是目录。
  kSourceUnusable,
  // 规则没能编译成 Filter。error 里是核心 Filter::AddRule 的原文。
  kRuleRejected,
};

struct PreviewResult {
  // 按扫描顺序，最多 limit 条。included 为真的条目就是"会进入归档"的那些。
  std::vector<PreviewItem> items;
  // 源目录里还有没被检查的条目（达到 limit 就停）。
  bool truncated = false;
  // 非空表示这次预览根本没跑起来。此时 items 为空，调用方应当把 error 报给
  // 用户，而不是显示"0 项"——"一项都没匹配"和"这次预览没跑成"是两件事。
  std::string error;
  PreviewErrorKind error_kind = PreviewErrorKind::kNone;
  // items 里 included 为真的条数。GUI 显示全部条目并逐条标注，CLI 只列这些，
  // 两边因此不需要各自数一遍。
  std::size_t included_count = 0;
};

// 扫描 source_directory，按 rules 判定每个条目会不会进入归档。
//
// 只读：不创建归档、不碰 repository / config / schedule / history，也不写任何
// 临时文件。源目录不可读时返回 error，不抛异常。
//
// limit 为 0 时按 kPreviewEntryLimit 处理（"没有窗口"不是一个有意义的请求）。
PreviewResult PreviewBackupSelection(const std::string& source_directory,
                                     const std::vector<FilterRuleDraft>& rules,
                                     std::size_t limit = kPreviewEntryLimit);

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_BACKUP_PREVIEW_H_
