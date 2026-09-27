// backup_preview.cpp
//
// 见 include/backup_preview.h。这里的判定逻辑与 GUI 预览完全同源：
// 它就是从 ui/modern/filter_rule_model.cpp 里搬出来的那一段，只是去掉了 Qt。
//
// 与 TreeScanner 的关系：两者都用 lstat、都把 FilterEntry 的字段填成同一口径，
// 但失败语义不同，所以不能直接复用同一个入口——TreeScanner 遇到"没有被排除的
// socket"会让整次备份失败（那是正确的备份行为），而预览要做的恰恰是把这类条目
// 列出来并说明后果。预览因此自己走一遍目录，但匹配一律问同一个 Filter。

#include "backup_preview.h"

#include <sys/stat.h>
#include <sys/types.h>

#include <filesystem>
#include <system_error>
#include <utility>

#include "user_directory.h"

namespace backupproject {
namespace {

namespace fs = std::filesystem;

// lstat 的 st_mode -> EntryType。与 src/core/tree_scanner.cpp 的 FactsOf
// 同一套： 预览看到的类型必须和真实扫描一致，否则 type:
// 规则的预览命中率会与备份不同。
bool TypeFromStat(const struct stat& info, EntryType* type) {
  if (S_ISDIR(info.st_mode)) {
    *type = EntryType::kDirectory;
  } else if (S_ISREG(info.st_mode)) {
    *type = EntryType::kRegularFile;
  } else if (S_ISLNK(info.st_mode)) {
    *type = EntryType::kSymlink;
  } else if (S_ISFIFO(info.st_mode)) {
    *type = EntryType::kFifo;
  } else if (S_ISCHR(info.st_mode)) {
    *type = EntryType::kCharDevice;
  } else if (S_ISBLK(info.st_mode)) {
    *type = EntryType::kBlockDevice;
  } else if (S_ISSOCK(info.st_mode)) {
    *type = EntryType::kSocket;
  } else {
    return false;
  }
  return true;
}

}  // namespace

PreviewResult PreviewBackupSelection(const std::string& source_directory,
                                     const std::vector<FilterRuleDraft>& rules,
                                     std::size_t limit) {
  PreviewResult result;
  if (limit == 0) limit = kPreviewEntryLimit;

  const fs::path root(source_directory);
  std::error_code ec;
  if (!fs::is_directory(root, ec)) {
    // 只报事实，措辞留给调用方：GUI 要给中文提示，CLI 要保持产品 CLI 的
    // 错误语义，两端都不该被这里的字符串绑住。
    result.error = "source directory does not exist or is not a directory";
    result.error_kind = PreviewErrorKind::kSourceUnusable;
    return result;
  }

  // 规则编译在扫描之前完成，而且失败就是整体失败：宁可明确报"这次预览没跑成"，
  // 也不要拿着一份少了一条 exclude 的 Filter 去列一份看起来正常的清单。
  Filter filter;
  if (!BuildFilterFromDrafts(rules, &filter, &result.error)) {
    result.error_kind = PreviewErrorKind::kRuleRejected;
    return result;
  }

  UserDirectoryCache names;

  fs::recursive_directory_iterator it(
      root, fs::directory_options::skip_permission_denied, ec);
  const fs::recursive_directory_iterator end;
  for (; it != end; it.increment(ec)) {
    if (ec) break;
    if (result.items.size() >= limit) {
      result.truncated = true;
      break;
    }
    const fs::path& path = it->path();
    // lstat：软链接不会被跟随，预览看到的就是条目自己；mtime / uid / gid 也都
    // 取自链接本身，口径与真实扫描（tree_scanner）一致。
    struct stat info;
    if (::lstat(path.c_str(), &info) != 0) continue;
    EntryType type = EntryType::kRegularFile;
    if (!TypeFromStat(info, &type)) continue;

    FilterEntry entry;
    entry.archive_path = path.lexically_relative(root).generic_string();
    entry.name = path.filename().string();
    entry.is_directory = type == EntryType::kDirectory;
    entry.type = type;
    entry.mtime_sec = static_cast<std::int64_t>(info.st_mtim.tv_sec);
    entry.uid = static_cast<std::uint32_t>(info.st_uid);
    entry.gid = static_cast<std::uint32_t>(info.st_gid);
    if (type == EntryType::kRegularFile) {
      entry.size = static_cast<std::uint64_t>(info.st_size);
    }
    // 与 tree_scanner 一致：所有类型（含软链接）都解析属主 / 属组名字。
    // uid / gid 来自 lstat，属于链接自己，解析名字不 follow。
    entry.user_name = names.UserName(entry.uid);
    entry.group_name = names.GroupName(entry.gid);

    PreviewItem item;
    item.archive_path = entry.archive_path;
    item.type = type;
    item.is_directory = entry.is_directory;
    item.size = entry.size;
    item.mtime_sec = entry.mtime_sec;
    item.uid = entry.uid;
    item.gid = entry.gid;

    // 归属判定全部问真实 Filter：预览里没有第二套匹配逻辑。
    if (type == EntryType::kDirectory) {
      if (filter.ShouldPruneDirectory(entry)) {
        // 命中 exclude 的目录整棵剪掉：子树里的 socket 也不再是问题。
        item.disposition = PreviewDisposition::kDirectoryPruned;
        it.disable_recursion_pending();
      } else {
        item.included = true;
        item.disposition = PreviewDisposition::kIncluded;
      }
    } else if (type == EntryType::kSocket) {
      // socket 不作为可恢复备份：只有明确写了 exclude 才会被跳过，
      // 否则真实备份会整次失败（见 tree_scanner.h 的失败语义）。
      item.disposition = filter.ShouldSkipSpecialEntry(entry)
                             ? PreviewDisposition::kExcludedByRule
                             : PreviewDisposition::kUnsupportedSocket;
    } else {
      // 普通文件与软链接 / FIFO / 设备走同一条 include/exclude 判定。
      item.included = filter.ShouldIncludeFile(entry);
      item.disposition = item.included ? PreviewDisposition::kIncluded
                                       : PreviewDisposition::kExcludedByRule;
    }

    if (item.included) ++result.included_count;
    result.items.push_back(std::move(item));
  }
  return result;
}

}  // namespace backupproject
