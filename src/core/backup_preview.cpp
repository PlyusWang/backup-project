// backup_preview.cpp
//
// 见 include/backup_preview.h。
//
// 遍历不在这里：它和真实 Backup 共用 src/core/source_tree_walker.cpp 那一份
// 实现（同样的 lstat、同样的 opendir/readdir 与错误语义、同样的 lexical DFS、
// 同样的路径长度校验、同样的剪枝与 socket 规则）。这一层只做两件事：
//
//   1. 把 walker 的事实与判定记成 PreviewItem（给界面标注用）；
//   2. 把 walker 的失败原样转述成 PreviewResult 的失败。
//
// 这里没有一条自己的 glob / 后缀 / 大小判断，也没有一次自己的目录遍历。

#include "backup_preview.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "source_tree_walker.h"

namespace backupproject {
namespace {

// Preview 消费者：把遍历结果记成"会显示的那一份"。
//
// 两件事必须分开：
//   * **整棵树**都要被检查——第 301 个条目是 socket 时，前 300 个正常条目
//     不能让它看起来"可以备份"；
//   * **只保存前 limit 条**用于展示——预览是给人看的，不是把整棵树搬进内存。
// 所以计数走全量，items 走窗口，truncated 表示"还有条目没有进窗口"。
class PreviewCollector : public SourceTreeVisitor {
 public:
  PreviewCollector(std::size_t limit, PreviewResult* result)
      : limit_(limit), result_(result) {}

  bool OnEntry(const std::string& disk_path, const std::string& archive_path,
               const SourceEntryFacts& facts, SourceEntryDecision decision,
               std::string* error_message) override {
    (void)disk_path;
    (void)error_message;
    // 源目录自己不作为预览项：预览列的是"树里有什么"，不是"根自己"。
    if (archive_path == ".") return true;

    ++result_->total_entries;
    if (decision == SourceEntryDecision::kIncluded) {
      ++result_->included_count;
    }
    if (result_->items.size() >= limit_) {
      result_->truncated = true;
      return true;
    }

    PreviewItem item;
    item.archive_path = archive_path;
    item.type = facts.type;
    item.is_directory = facts.type == EntryType::kDirectory;
    item.size = facts.size;
    item.mtime_sec = facts.mtime_sec;
    item.uid = facts.uid;
    item.gid = facts.gid;
    switch (decision) {
      case SourceEntryDecision::kIncluded:
        item.included = true;
        item.disposition = PreviewDisposition::kIncluded;
        break;
      case SourceEntryDecision::kDirectoryPruned:
        item.disposition = PreviewDisposition::kDirectoryPruned;
        break;
      case SourceEntryDecision::kExcludedByRule:
        item.disposition = PreviewDisposition::kExcludedByRule;
        break;
    }
    result_->items.push_back(std::move(item));
    return true;
  }

 private:
  std::size_t limit_ = kPreviewEntryLimit;
  PreviewResult* result_ = nullptr;
};

PreviewErrorKind ErrorKindOf(SourceWalkFailureKind kind) {
  switch (kind) {
    case SourceWalkFailureKind::kSourceRoot:
      return PreviewErrorKind::kSourceUnusable;
    case SourceWalkFailureKind::kSocket:
      // 源目录本身没问题、规则也能编译：是这份选择无法被成功备份。
      return PreviewErrorKind::kSelectionBlocked;
    case SourceWalkFailureKind::kNone:
    case SourceWalkFailureKind::kInspect:
    case SourceWalkFailureKind::kDirectoryRead:
    case SourceWalkFailureKind::kPathTooLong:
    case SourceWalkFailureKind::kUnsupportedType:
    case SourceWalkFailureKind::kConsumerFailed:
      break;
  }
  return PreviewErrorKind::kScanFailed;
}

}  // namespace

PreviewResult PreviewBackupSelection(const std::string& source_directory,
                                     const std::vector<FilterRuleDraft>& rules,
                                     std::size_t limit) {
  return PreviewBackupSelection(source_directory, rules, limit, nullptr);
}

PreviewResult PreviewBackupSelection(const std::string& source_directory,
                                     const std::vector<FilterRuleDraft>& rules,
                                     std::size_t limit,
                                     const SourceWalkFaults* faults) {
  PreviewResult result;
  if (limit == 0) limit = kPreviewEntryLimit;

  // 规则编译在遍历之前完成，而且失败就是整体失败：宁可明确报"这次预览没跑
  // 起来"，也不要拿着一份少了一条 exclude 的 Filter 去列一份看起来正常的清单。
  Filter filter;
  if (!BuildFilterFromDrafts(rules, &filter, &result.error)) {
    result.error_kind = PreviewErrorKind::kRuleRejected;
    return result;
  }

  PreviewCollector collector(limit, &result);
  SourceWalkFailure failure;
  if (!WalkSourceTree(source_directory, &filter, &collector, &failure,
                      faults)) {
    // 遍历失败时不给半份结果：调用方必须把失败原样报出去，而不是显示
    // "前 12 项看起来没问题"。这正是"预览 == 备份会发生什么"的含义。
    result.items.clear();
    result.included_count = 0;
    result.total_entries = 0;
    result.truncated = false;
    result.error = failure.message;
    result.error_kind = ErrorKindOf(failure.kind);
    result.blocking_archive_path = failure.archive_path;
    result.blocking_disk_path = failure.disk_path;
    return result;
  }
  return result;
}

}  // namespace backupproject
