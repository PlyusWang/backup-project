// backup_preview_test.cpp
//
// Manual Backup 的筛选预览核心（backupproject::PreviewBackupSelection）专项测试。
//
// 它要钉住的不变量只有一条，但要钉死：
//
//     预览说"会进归档"的条目集合
//         == 用同一份 source + 同一组规则真实备份、再恢复出来的条目集合
//
// 这条不变量是 GUI 与 CLI 共用的地基：两边都调用同一个函数，所以只要
// "预览 == 真实备份"成立，"GUI 预览 == CLI 预览 == 实际归档条目"就是推论，
// 而不是需要各自维护的约定。
//
// 比较对象刻意取**恢复出来的目录树**而不是中间结构：中间结构只能证明"两次
// 调用同一个函数得到同一个结果"，恢复出来的树才能证明"归档里真的有这些条目"。
//
// 这里不碰 Qt、不碰仓库，也不依赖任何测试框架。

#include <algorithm>
#include <string>
#include <vector>

#include "backup_engine.h"
#include "backup_preview.h"
#include "filter.h"
#include "test_support.h"

namespace bp = backupproject;

namespace {

// 递归列出 root 下所有节点的相对路径（文件 + 目录），升序。
// 目录也在内：预览承诺"目录保留结构"，那就必须能在恢复结果里看见它们。
void CollectNodes(const std::string& root, const std::string& prefix,
                  std::vector<std::string>* out) {
  for (const std::string& name : test_support::DirEntries(root)) {
    const std::string relative = prefix.empty() ? name : prefix + "/" + name;
    out->push_back(relative);
    struct stat info;
    if (test_support::StatOf(root + "/" + name, &info) &&
        S_ISDIR(info.st_mode)) {
      CollectNodes(root + "/" + name, relative, out);
    }
  }
  std::sort(out->begin(), out->end());
}

std::vector<std::string> TreeNodes(const std::string& root) {
  std::vector<std::string> nodes;
  CollectNodes(root, std::string(), &nodes);
  return nodes;
}

// 预览结果里 included 为真的路径（CLI 打印的就是这一份），升序。
std::vector<std::string> IncludedPaths(const bp::PreviewResult& preview) {
  std::vector<std::string> paths;
  for (const bp::PreviewItem& item : preview.items) {
    if (item.included) paths.push_back(item.archive_path);
  }
  std::sort(paths.begin(), paths.end());
  return paths;
}

std::string Join(const std::vector<std::string>& values) {
  std::string text;
  for (const std::string& value : values) {
    if (!text.empty()) text += " ";
    text += value;
  }
  return text.empty() ? std::string("(empty)") : text;
}

// 一条规则：--include / --exclude 的完整 DSL 原文（与 CLI 的 argv 同源）。
bp::FilterRuleDraft Rule(bp::FilterAction action, const std::string& dsl) {
  bp::FilterRuleDraft rule;
  rule.action = action;
  rule.raw_dsl = dsl;
  return rule;
}

bp::Filter CompileOrDie(const std::vector<bp::FilterRuleDraft>& rules,
                        std::string* error) {
  bp::Filter filter;
  if (!bp::BuildFilterFromDrafts(rules, &filter, error)) {
    error->insert(0, "BuildFilterFromDrafts failed: ");
  }
  return filter;
}

// 一次完整的三方对账：预览 -> 真实备份 -> 恢复 -> 比较。
void CheckPreviewMatchesBackup(const std::string& label,
                               const std::string& source,
                               const std::string& work,
                               const std::vector<bp::FilterRuleDraft>& rules,
                               int index) {
  const std::string tag = label + " (" + std::to_string(index) + ")";
  const bp::PreviewResult preview =
      bp::PreviewBackupSelection(source, rules);
  test_support::Check(preview.error.empty(),
                      "PREV " + tag + " 预览成功",
                      preview.error + " kind=" +
                          std::to_string(static_cast<int>(preview.error_kind)));
  if (!preview.error.empty()) return;

  std::string error;
  const bp::Filter filter = CompileOrDie(rules, &error);
  const std::string archive = work + "/archive-" + std::to_string(index) + ".bak";
  const std::string restored = work + "/restored-" + std::to_string(index);
  bp::BackupEngine engine;
  // 与产品路径一致：默认就是 MyPack + 不压缩 + 不加密的 v2 容器。
  bp::BackupOptions options;
  if (!engine.Backup(source, archive, filter, options, &error) ||
      !engine.Restore(archive, restored, &error)) {
    test_support::Check(false, "PREV " + tag + " 备份 + 恢复成功", error);
    return;
  }

  const std::vector<std::string> expected = IncludedPaths(preview);
  const std::vector<std::string> actual = TreeNodes(restored);
  test_support::Check(expected == actual,
                      "PREV " + tag + " 预览 included 集合 == 恢复出来的条目集合",
                      "preview=[" + Join(expected) + "] restored=[" +
                          Join(actual) + "]");

  // included_count 是 CLI 打印的那个数，必须与列表本身一致。
  test_support::Check(preview.included_count == expected.size(),
                      "PREV " + tag + " included_count 与列表一致",
                      std::to_string(preview.included_count) + " vs " +
                          std::to_string(expected.size()));
}

}  // namespace

int main() {
  test_support::Section("PREV 1. 预览 == 真实备份（同一个 Filter）");
  {
    const std::string work = test_support::FreshDir("backup-preview");
    const std::string source = work + "/src";
    test_support::Mkdir(source, 0755);
    test_support::Mkdir(source + "/build", 0755);
    test_support::Mkdir(source + "/build/deep", 0755);
    test_support::Mkdir(source + "/sub", 0755);
    test_support::Mkdir(source + "/cache", 0755);
    test_support::Mkdir(source + "/empty_dir", 0755);
    test_support::WriteFile(source + "/a.txt", "aaa\n", 0644);
    test_support::WriteFile(source + "/b.txt", "bbbbb\n", 0644);
    test_support::WriteFile(source + "/notes.md", "notes\n", 0644);
    test_support::WriteFile(source + "/build/obj.o", "obj\n", 0644);
    test_support::WriteFile(source + "/build/deep/x.txt", "deep\n", 0644);
    test_support::WriteFile(source + "/sub/c.txt", "ccc\n", 0644);
    test_support::WriteFile(source + "/sub/big.txt",
                            std::string(20000, 'z'), 0644);
    test_support::WriteFile(source + "/cache/tmp.dat", "tmp\n", 0644);

    // P1：没有规则 -> 整棵树
    CheckPreviewMatchesBackup("P1 无规则", source, work, {}, 1);
    // P2：单条件 include
    CheckPreviewMatchesBackup("P2 单条件 include", source, work,
                              {Rule(bp::FilterAction::kInclude, "ext:txt")}, 2);
    // P3：一条规则内的 compound AND
    CheckPreviewMatchesBackup(
        "P3 compound AND", source, work,
        {Rule(bp::FilterAction::kInclude, "name:*.txt size:<5")}, 3);
    // P4：include + exclude 冲突 -> exclude 优先
    CheckPreviewMatchesBackup(
        "P4 include+exclude", source, work,
        {Rule(bp::FilterAction::kInclude, "name:*.txt"),
         Rule(bp::FilterAction::kExclude, "name:b*")},
        4);
    // P5：被排除的目录整棵剪掉
    CheckPreviewMatchesBackup(
        "P5 excluded directory", source, work,
        {Rule(bp::FilterAction::kExclude, "name:build")}, 5);
    // 多条 include 之间是 OR（与 compound AND 的区别就在这里）
    CheckPreviewMatchesBackup(
        "P2b 多条 include 是 OR", source, work,
        {Rule(bp::FilterAction::kInclude, "name:*.md"),
         Rule(bp::FilterAction::kInclude, "path:sub/*")},
        6);

    // P5b：被排除的目录连同子树都不出现在预览里。上面的对账只能证明"预览与
    // 备份一致"，这里把"一致在正确的方向上"钉死：排除项真的不在列表里，
    // 而不是两边都错误地包含了它。
    const bp::PreviewResult pruned = bp::PreviewBackupSelection(
        source, {Rule(bp::FilterAction::kExclude, "name:build")});
    const std::vector<std::string> pruned_paths = IncludedPaths(pruned);
    bool has_build = false;
    for (const std::string& path : pruned_paths) {
      if (path == "build" || path.rfind("build/", 0) == 0) has_build = true;
    }
    test_support::Check(!has_build,
                        "PREV P5b 被排除目录及其子树不出现在预览里",
                        Join(pruned_paths));
    bool saw_pruned_disposition = false;
    for (const bp::PreviewItem& item : pruned.items) {
      if (item.disposition == bp::PreviewDisposition::kDirectoryPruned) {
        saw_pruned_disposition = true;
      }
    }
    test_support::Check(saw_pruned_disposition,
                        "PREV P5b 被剪枝的目录带 kDirectoryPruned 标记");

    // 预览必须只读：不建归档、不建临时文件、不动源目录。
    const std::vector<std::string> before = test_support::DirEntries(work);
    const std::vector<std::string> source_before = TreeNodes(source);
    const bp::PreviewResult read_only =
        bp::PreviewBackupSelection(source, {Rule(bp::FilterAction::kInclude,
                                                 "ext:txt")});
    test_support::Check(test_support::DirEntries(work) == before,
                        "PREV P1b 预览不在工作目录里创建任何东西");
    test_support::Check(TreeNodes(source) == source_before,
                        "PREV P1b 预览不改变源目录");
    test_support::Check(!read_only.items.empty(),
                        "PREV P1b 只读检查本身不是空跑");
  }

  test_support::Section("PREV 2. 预览窗口（limit / truncated）");
  {
    const std::string work = test_support::FreshDir("backup-preview-limit");
    const std::string source = work + "/src";
    test_support::Mkdir(source, 0755);
    const int kFiles = 320;
    for (int index = 0; index < kFiles; ++index) {
      test_support::WriteFile(
          source + "/f" + std::to_string(index) + ".dat", "x", 0644);
    }

    const bp::PreviewResult full = bp::PreviewBackupSelection(source, {});
    test_support::Check(full.items.size() == bp::kPreviewEntryLimit,
                        "PREV P7 默认窗口一次最多检查 300 个条目",
                        std::to_string(full.items.size()));
    test_support::Check(full.truncated,
                        "PREV P7 源目录大于窗口时 truncated 为真");
    test_support::Check(full.included_count == bp::kPreviewEntryLimit,
                        "PREV P7 窗口内的条目全部 included");

    const bp::PreviewResult small =
        bp::PreviewBackupSelection(source, {}, 5);
    test_support::Check(small.items.size() == 5 && small.truncated,
                        "PREV P7 显式 limit 生效且同样报 truncated",
                        std::to_string(small.items.size()));

    const bp::PreviewResult exact =
        bp::PreviewBackupSelection(source, {}, kFiles);
    test_support::Check(exact.items.size() == static_cast<std::size_t>(kFiles) &&
                            !exact.truncated,
                        "PREV P7 条目数正好等于窗口时不算 truncated",
                        std::to_string(exact.items.size()) + " truncated=" +
                            (exact.truncated ? "true" : "false"));

    // 排序 = 扫描顺序（GUI 与 CLI 都原样使用，不做二次排序）。
    bool ordered = true;
    for (std::size_t index = 1; index < small.items.size(); ++index) {
      if (small.items[index].archive_path ==
          small.items[index - 1].archive_path) {
        ordered = false;
      }
    }
    test_support::Check(ordered, "PREV P7 结果里没有重复条目");
  }

  test_support::Section("PREV 3. 无法给出结果时必须明确失败");
  {
    const std::string work = test_support::FreshDir("backup-preview-error");
    const std::string source = work + "/src";
    test_support::Mkdir(source, 0755);
    test_support::WriteFile(source + "/a.txt", "aaa\n", 0644);

    // P6：非法 DSL 由核心拒绝，不会静默退化成"没有规则"。
    const bp::PreviewResult bad = bp::PreviewBackupSelection(
        source, {Rule(bp::FilterAction::kInclude, "nonsense:xx")});
    test_support::Check(!bad.error.empty() &&
                            bad.error_kind == bp::PreviewErrorKind::kRuleRejected,
                        "PREV P6 非法规则 -> kRuleRejected", bad.error);
    test_support::Check(bad.items.empty(),
                        "PREV P6 规则被拒绝时一个条目都不返回");

    // 同一句话必须与 Filter::AddRule 说的一模一样：GUI / CLI 报的是核心原文。
    bp::Filter probe;
    std::string direct;
    probe.AddRule(bp::FilterAction::kInclude, "nonsense:xx", &direct);
    test_support::Check(!direct.empty() && bad.error == direct,
                        "PREV P6 报错就是 Filter::AddRule 的原文",
                        "preview=[" + bad.error + "] filter=[" + direct + "]");

    // 一条坏规则不能让整份列表变成"部分生效"。
    const bp::PreviewResult mixed = bp::PreviewBackupSelection(
        source, {Rule(bp::FilterAction::kInclude, "ext:txt"),
                 Rule(bp::FilterAction::kExclude, "broken:")});
    test_support::Check(!mixed.error.empty() &&
                            mixed.error_kind == bp::PreviewErrorKind::kRuleRejected,
                        "PREV P6 好坏规则混在一起时整体失败", mixed.error);

    // 源目录不可用是另一类失败：调用方要给完全不同的提示。
    const bp::PreviewResult missing =
        bp::PreviewBackupSelection(work + "/no-such-dir", {});
    test_support::Check(
        !missing.error.empty() &&
            missing.error_kind == bp::PreviewErrorKind::kSourceUnusable,
        "PREV P6 源目录不存在 -> kSourceUnusable", missing.error);
    test_support::Check(missing.items.empty() && missing.included_count == 0,
                        "PREV P6 源目录不可用时不给任何条目");

    // 源目录是普通文件时同样不可用（is_directory 而不是 exists）。
    test_support::WriteFile(work + "/plain.txt", "x", 0644);
    const bp::PreviewResult file_source =
        bp::PreviewBackupSelection(work + "/plain.txt", {});
    test_support::Check(
        file_source.error_kind == bp::PreviewErrorKind::kSourceUnusable,
        "PREV P6 源路径是普通文件 -> kSourceUnusable");

    // 空目录是合法输入：0 项、0 错误。
    const std::string empty = work + "/empty";
    test_support::Mkdir(empty, 0755);
    const bp::PreviewResult nothing = bp::PreviewBackupSelection(empty, {});
    test_support::Check(nothing.error.empty() && nothing.items.empty() &&
                            nothing.included_count == 0,
                        "PREV P6 空目录 -> 0 项且不是错误");
  }

  test_support::Section("PREV 4. 路径语义");
  {
    const std::string work = test_support::FreshDir("backup-preview-paths");
    const std::string source = work + "/src";
    test_support::Mkdir(source, 0755);
    test_support::Mkdir(source + "/a", 0755);
    test_support::Mkdir(source + "/a/b", 0755);
    test_support::WriteFile(source + "/a/b/c.txt", "c\n", 0644);

    const bp::PreviewResult preview = bp::PreviewBackupSelection(source, {});
    test_support::Check(preview.error.empty(),
                        "PREV P1c 路径用例的预览本身成功", preview.error);
    bool relative = !preview.items.empty();
    for (const bp::PreviewItem& item : preview.items) {
      if (item.archive_path.empty() || item.archive_path[0] == '/' ||
          item.archive_path.find(source) != std::string::npos) {
        relative = false;
      }
    }
    test_support::Check(relative,
                        "PREV P1c 预览路径是相对归档路径，不泄漏磁盘绝对路径",
                        Join(IncludedPaths(preview)));
    test_support::Check(IncludedPaths(preview) ==
                            std::vector<std::string>({"a", "a/b", "a/b/c.txt"}),
                        "PREV P1c 目录与文件都用同一种 '/' 分隔的相对路径",
                        Join(IncludedPaths(preview)));
  }

  return test_support::Finish("backup_preview_test");
}
