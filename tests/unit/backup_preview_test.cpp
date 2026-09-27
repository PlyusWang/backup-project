// backup_preview_test.cpp
//
// Manual Backup
// 的筛选预览核心（backupproject::PreviewBackupSelection）专项测试。
//
// 它要钉住的不变量只有一条，但要钉死：
//
//     预览给出的 filesystem 事实与结论
//         == 真实 Backup 给出的 filesystem 事实与结论
//
// 这条不变量是 GUI 与 CLI 共用的地基：三个前端都走同一份遍历
// （src/core/source_tree_walker.cpp），只要"预览 == 真实备份"成立，
// "GUI 预览 == CLI 预览 == 实际归档条目"就是推论，而不是各自维护的约定。
//
// 所以下面的用例分四组：
//   * 集合与顺序：预览的 included 序列 == 真实备份扫描 / 归档的序列；
//   * 失败语义：源目录不可用、opendir / readdir / lstat 失败、路径过长、
//     未被排除的 socket —— 两边必须给出同一句原文；
//   * 窗口：只展示前 300 条，但整棵树都被检查过（第 301 条的 socket 与
//     第 301 条的 lstat 失败都不能被窗口掩盖）；
//   * 不能有第二套实现：预览层里没有自己的遍历（由 modern_gui_check.sh 的
//     静态断言与这里的"顺序逐项相等"共同保证）。
//
// 权限类失败用 SourceWalkFaults 注入：在 root / CAP_DAC_OVERRIDE 下 chmod 000
// 根本不会 EACCES，靠它写出来的用例会在那种环境里静默变成"通过"。注入点只能
// 把一次 syscall 变成失败，不能伪造文件系统内容。
//
// 比较对象刻意取**恢复出来的目录树**而不是中间结构：中间结构只能证明"两次
// 调用同一个函数得到同一个结果"，恢复出来的树才能证明"归档里真的有这些条目"。
//
// 这里不碰 Qt、不碰仓库，也不依赖任何测试框架。

#include "backup_preview.h"

#include <errno.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "archive_path.h"
#include "backup_engine.h"
#include "filter.h"
#include "source_tree_walker.h"
#include "test_support.h"
#include "tree_scanner.h"

namespace bp = backupproject;

namespace {

// ---- 小工具 ---------------------------------------------------------------

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

// 预览结果里 included 为真的路径，升序（集合比较用）。
std::vector<std::string> IncludedPaths(const bp::PreviewResult& preview) {
  std::vector<std::string> paths;
  for (const bp::PreviewItem& item : preview.items) {
    if (item.included) paths.push_back(item.archive_path);
  }
  std::sort(paths.begin(), paths.end());
  return paths;
}

// 预览结果里 included 为真的路径，**保持遍历顺序**（顺序比较用）。
// 顺序本身就是被测试的对象，所以这里绝不排序。
std::vector<std::string> IncludedInOrder(const bp::PreviewResult& preview) {
  std::vector<std::string> paths;
  for (const bp::PreviewItem& item : preview.items) {
    if (item.included) paths.push_back(item.archive_path);
  }
  return paths;
}

// 备份扫描的顺序，去掉 source root 自己（预览不列根）。
std::vector<std::string> ScanOrder(
    const std::vector<bp::ArchiveEntry>& entries) {
  std::vector<std::string> paths;
  for (const bp::ArchiveEntry& entry : entries) {
    if (entry.archive_path != ".") paths.push_back(entry.archive_path);
  }
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

// 记录共享遍历自己给出的顺序（第三个比较对象）。
class OrderRecorder : public bp::SourceTreeVisitor {
 public:
  bool OnEntry(const std::string& disk_path, const std::string& archive_path,
               const bp::SourceEntryFacts& facts,
               bp::SourceEntryDecision decision,
               std::string* error_message) override {
    (void)disk_path;
    (void)facts;
    (void)error_message;
    if (archive_path == ".") return true;
    all.push_back(archive_path);
    if (decision == bp::SourceEntryDecision::kIncluded) {
      included.push_back(archive_path);
    }
    return true;
  }

  std::vector<std::string> all;
  std::vector<std::string> included;
};

// ---- 注入点 ---------------------------------------------------------------

struct FaultPlan {
  bp::SourceWalkSyscall call = bp::SourceWalkSyscall::kLstat;
  std::string path;
  int error_number = EACCES;
};

int FaultHook(bp::SourceWalkSyscall call, const std::string& disk_path,
              void* context) {
  const FaultPlan* plan = static_cast<const FaultPlan*>(context);
  if (call == plan->call && disk_path == plan->path) return plan->error_number;
  return 0;
}

bp::SourceWalkFaults FaultsFor(FaultPlan* plan) {
  bp::SourceWalkFaults faults;
  faults.fail_syscall = &FaultHook;
  faults.context = plan;
  return faults;
}

// 同一次注入下，预览与真实备份必须同时失败，而且说出同一句话。
void CheckInjectedFailure(const std::string& label, const std::string& source,
                          const FaultPlan& plan) {
  FaultPlan mutable_plan = plan;
  const bp::SourceWalkFaults faults = FaultsFor(&mutable_plan);

  const bp::PreviewResult preview =
      bp::PreviewBackupSelection(source, {}, bp::kPreviewEntryLimit, &faults);
  std::vector<bp::ArchiveEntry> entries;
  std::string scan_error;
  const bool backup_ok =
      bp::ScanSourceTree(source, nullptr, &entries, &scan_error, &faults);

  test_support::Check(!preview.error.empty(),
                      "PREV " + label + " 预览 fail closed", preview.error);
  test_support::Check(!backup_ok, "PREV " + label + " 真实备份同样失败",
                      scan_error);
  test_support::Check(
      !preview.error.empty() && preview.error == scan_error,
      "PREV " + label + " 两边报出同一句原文",
      "preview=[" + preview.error + "] backup=[" + scan_error + "]");
  test_support::Check(preview.items.empty() && preview.included_count == 0 &&
                          preview.total_entries == 0 && !preview.truncated,
                      "PREV " + label + " 失败时不返回半份结果");
}

// 一次完整的三方对账：预览 -> 真实备份 -> 恢复 -> 比较（集合）。
void CheckPreviewMatchesBackup(const std::string& label,
                               const std::string& source,
                               const std::string& work,
                               const std::vector<bp::FilterRuleDraft>& rules,
                               int index) {
  const std::string tag = label + " (" + std::to_string(index) + ")";
  const bp::PreviewResult preview = bp::PreviewBackupSelection(source, rules);
  test_support::Check(preview.error.empty(), "PREV " + tag + " 预览成功",
                      preview.error + " kind=" +
                          std::to_string(static_cast<int>(preview.error_kind)));
  if (!preview.error.empty()) return;

  std::string error;
  const bp::Filter filter = CompileOrDie(rules, &error);
  const std::string archive =
      work + "/archive-" + std::to_string(index) + ".bak";
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
  test_support::Check(
      expected == actual,
      "PREV " + tag + " 预览 included 集合 == 恢复出来的条目集合",
      "preview=[" + Join(expected) + "] restored=[" + Join(actual) + "]");

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
    test_support::WriteFile(source + "/sub/big.txt", std::string(20000, 'z'),
                            0644);
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
    CheckPreviewMatchesBackup("P4 include+exclude", source, work,
                              {Rule(bp::FilterAction::kInclude, "name:*.txt"),
                               Rule(bp::FilterAction::kExclude, "name:b*")},
                              4);
    // P5：被排除的目录整棵剪掉
    CheckPreviewMatchesBackup("P5 excluded directory", source, work,
                              {Rule(bp::FilterAction::kExclude, "name:build")},
                              5);
    // 多条 include 之间是 OR（与 compound AND 的区别就在这里）
    CheckPreviewMatchesBackup("P2b 多条 include 是 OR", source, work,
                              {Rule(bp::FilterAction::kInclude, "name:*.md"),
                               Rule(bp::FilterAction::kInclude, "path:sub/*")},
                              6);

    // P5b：被排除的目录连同子树都不出现在预览里。上面的对账只能证明"预览与
    // 备份一致"，这里把"一致在正确的方向上"钉死。
    const bp::PreviewResult pruned = bp::PreviewBackupSelection(
        source, {Rule(bp::FilterAction::kExclude, "name:build")});
    const std::vector<std::string> pruned_paths = IncludedPaths(pruned);
    bool has_build = false;
    for (const std::string& path : pruned_paths) {
      if (path == "build" || path.rfind("build/", 0) == 0) has_build = true;
    }
    test_support::Check(!has_build, "PREV P5b 被排除目录及其子树不出现在预览里",
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
    const bp::PreviewResult read_only = bp::PreviewBackupSelection(
        source, {Rule(bp::FilterAction::kInclude, "ext:txt")});
    test_support::Check(test_support::DirEntries(work) == before,
                        "PREV P1b 预览不在工作目录里创建任何东西");
    test_support::Check(TreeNodes(source) == source_before,
                        "PREV P1b 预览不改变源目录");
    test_support::Check(!read_only.items.empty(),
                        "PREV P1b 只读检查本身不是空跑");
  }

  test_support::Section("PREV 2. Source root 语义（T1 / T2）");
  {
    const std::string work = test_support::FreshDir("backup-preview-root");
    const std::string source = work + "/src";
    test_support::Mkdir(source, 0755);
    test_support::WriteFile(source + "/a.txt", "aaa\n", 0644);

    // T1：真目录 -> 预览与备份都成功
    const bp::PreviewResult ok = bp::PreviewBackupSelection(source, {});
    std::vector<bp::ArchiveEntry> ok_entries;
    std::string ok_error;
    test_support::Check(
        ok.error.empty() && ok.included_count == 1,
        "PREV T1 真目录：预览成功且看到 1 条",
        ok.error + " count=" + std::to_string(ok.included_count));
    test_support::Check(
        bp::ScanSourceTree(source, nullptr, &ok_entries, &ok_error),
        "PREV T1 真目录：真实备份扫描成功", ok_error);

    // T2：source root 是 symlink-to-directory -> 两边都必须拒绝，理由一致。
    const std::string link = work + "/link-src";
    test_support::Check(test_support::CreateSymlink(source, link),
                        "PREV T2 建好 symlink-to-directory");
    const bp::PreviewResult linked = bp::PreviewBackupSelection(link, {});
    std::vector<bp::ArchiveEntry> link_entries;
    std::string link_error;
    const bool link_backup_ok =
        bp::ScanSourceTree(link, nullptr, &link_entries, &link_error);
    test_support::Check(
        !linked.error.empty() &&
            linked.error_kind == bp::PreviewErrorKind::kSourceUnusable,
        "PREV T2 symlink 源目录：预览拒绝", linked.error);
    test_support::Check(!link_backup_ok, "PREV T2 symlink 源目录：真实备份拒绝",
                        link_error);
    test_support::Check(
        linked.error == link_error, "PREV T2 两边报出同一句原文",
        "preview=[" + linked.error + "] backup=[" + link_error + "]");
    test_support::Check(linked.items.empty() && linked.included_count == 0,
                        "PREV T2 拒绝时不返回任何条目");

    // T2b：source root 是普通文件 -> 同样两边都拒绝。
    const std::string plain = work + "/plain.txt";
    test_support::WriteFile(plain, "x\n", 0644);
    const bp::PreviewResult file_preview =
        bp::PreviewBackupSelection(plain, {});
    std::vector<bp::ArchiveEntry> file_entries;
    std::string file_error;
    const bool file_backup_ok =
        bp::ScanSourceTree(plain, nullptr, &file_entries, &file_error);
    test_support::Check(
        !file_preview.error.empty() && !file_backup_ok &&
            file_preview.error == file_error,
        "PREV T2b 普通文件作源：两边同一句拒绝",
        "preview=[" + file_preview.error + "] backup=[" + file_error + "]");
  }

  test_support::Section("PREV 3. 遍历顺序（T7）：逐项比较，不排序");
  {
    const std::string work = test_support::FreshDir("backup-preview-order");
    const std::string source = work + "/src";
    test_support::Mkdir(source, 0755);
    // 创建顺序故意与 lexical 顺序不一致：如果谁还在用"文件系统给的顺序"
    // （readdir / recursive_directory_iterator），这里立刻会露出来。
    const char* names[] = {"zulu.txt", "alpha.txt", "mike.txt", "bravo.txt",
                           "yankee.txt"};
    for (const char* name : names) {
      test_support::WriteFile(source + "/" + name, "x\n", 0644);
    }
    test_support::Mkdir(source + "/nested", 0755);
    test_support::WriteFile(source + "/nested/inner.txt", "x\n", 0644);
    test_support::Mkdir(source + "/alpha-dir", 0755);
    test_support::WriteFile(source + "/alpha-dir/deep.txt", "x\n", 0644);

    const std::vector<std::string> expected = {
        "alpha-dir",        "alpha-dir/deep.txt", "alpha.txt",
        "bravo.txt",        "mike.txt",           "nested",
        "nested/inner.txt", "yankee.txt",         "zulu.txt"};

    OrderRecorder recorder;
    bp::SourceWalkFailure failure;
    const bool walked =
        bp::WalkSourceTree(source, nullptr, &recorder, &failure);
    test_support::Check(walked, "PREV T7 共享遍历成功", failure.message);
    test_support::Check(recorder.all == expected,
                        "PREV T7 共享遍历顺序 == 每层 lexical 的 DFS 先序",
                        Join(recorder.all));

    const bp::PreviewResult preview = bp::PreviewBackupSelection(source, {});
    test_support::Check(IncludedInOrder(preview) == expected,
                        "PREV T7 预览顺序 == 共享遍历顺序（逐项，未排序）",
                        Join(IncludedInOrder(preview)));

    std::vector<bp::ArchiveEntry> entries;
    std::string scan_error;
    test_support::Check(
        bp::ScanSourceTree(source, nullptr, &entries, &scan_error),
        "PREV T7 真实备份扫描成功", scan_error);
    test_support::Check(ScanOrder(entries) == expected,
                        "PREV T7 真实备份扫描顺序 == 共享遍历顺序（逐项）",
                        Join(ScanOrder(entries)));
    // 归档就是按这个序列写的（pack 层按 entries 顺序消费），所以
    // "预览前 N 项 == 归档前 N 项"由上面三条相等直接成立。

    // 排序不能是"碰巧对"：把同一批名字用相反的顺序再建一次，结果必须不变。
    const std::string second = work + "/src2";
    test_support::Mkdir(second, 0755);
    for (int index = 4; index >= 0; --index) {
      test_support::WriteFile(second + "/" + names[index], "x\n", 0644);
    }
    test_support::Mkdir(second + "/nested", 0755);
    test_support::WriteFile(second + "/nested/inner.txt", "x\n", 0644);
    test_support::Mkdir(second + "/alpha-dir", 0755);
    test_support::WriteFile(second + "/alpha-dir/deep.txt", "x\n", 0644);
    const bp::PreviewResult second_preview =
        bp::PreviewBackupSelection(second, {});
    test_support::Check(IncludedInOrder(second_preview) == expected,
                        "PREV T7 创建顺序反过来，遍历顺序仍然一样",
                        Join(IncludedInOrder(second_preview)));
  }

  test_support::Section("PREV 4. Socket（T5 / T6 / T9）");
  {
    const std::string work = test_support::FreshDir("backup-preview-socket");
    const std::string source = work + "/src";
    test_support::Mkdir(source, 0755);
    test_support::Mkdir(source + "/sub", 0755);
    test_support::WriteFile(source + "/a.txt", "aaa\n", 0644);
    test_support::WriteFile(source + "/sub/b.txt", "bbb\n", 0644);
    const int socket_fd = test_support::CreateUnixSocket(source + "/sock");
    if (socket_fd < 0) {
      test_support::Note("PREV T5 无法创建 unix socket，跳过这一组");
    } else {
      // T5：没有被明确排除的 socket -> 预览必须 blocked，备份必须失败，
      // 而且两边说的是同一句话。
      const bp::PreviewResult blocked = bp::PreviewBackupSelection(source, {});
      std::vector<bp::ArchiveEntry> entries;
      std::string scan_error;
      const bool backup_ok =
          bp::ScanSourceTree(source, nullptr, &entries, &scan_error);
      test_support::Check(
          !blocked.error.empty() &&
              blocked.error_kind == bp::PreviewErrorKind::kSelectionBlocked,
          "PREV T5 未排除的 socket：预览报 blocked", blocked.error);
      test_support::Check(!backup_ok && !scan_error.empty(),
                          "PREV T5 未排除的 socket：真实备份失败", scan_error);
      test_support::Check(
          blocked.error == scan_error, "PREV T5 两边报出同一句原文",
          "preview=[" + blocked.error + "] backup=[" + scan_error + "]");
      test_support::Check(blocked.items.empty() && blocked.included_count == 0,
                          "PREV T5 blocked 时不假装列出一半结果");
      test_support::Check(blocked.blocking_archive_path == "sock" ||
                              blocked.blocking_archive_path == "sub",
                          "PREV T5 指出是哪个条目挡住了备份",
                          blocked.blocking_archive_path);

      // T5b：socket 在子目录里也一样（不是只看根那一层）。
      test_support::Check(::rename((source + "/sock").c_str(),
                                   (source + "/sub/sock").c_str()) == 0,
                          "PREV T5b 把 socket 移进子目录");
      const bp::PreviewResult nested = bp::PreviewBackupSelection(source, {});
      test_support::Check(
          nested.error_kind == bp::PreviewErrorKind::kSelectionBlocked &&
              nested.blocking_archive_path == "sub/sock",
          "PREV T5b 子目录里的 socket 同样 blocked 且定位准确",
          nested.blocking_archive_path + " / " + nested.error);

      // T6：明确 exclude 掉 socket -> 预览与备份都必须成功，且它不在结果里。
      const std::vector<bp::FilterRuleDraft> excluding = {
          Rule(bp::FilterAction::kExclude, "name:sock")};
      const bp::PreviewResult allowed =
          bp::PreviewBackupSelection(source, excluding);
      test_support::Check(allowed.error.empty(),
                          "PREV T6 明确排除 socket 之后预览成功",
                          allowed.error);
      const std::vector<std::string> paths = IncludedPaths(allowed);
      bool has_socket = false;
      for (const std::string& path : paths) {
        if (path == "sub/sock") has_socket = true;
      }
      test_support::Check(
          !has_socket, "PREV T6 被排除的 socket 不在 included 里", Join(paths));
      // 备份侧：同一组规则真的能跑完并恢复出来，而且里面没有 socket。
      std::string error;
      const bp::Filter filter = CompileOrDie(excluding, &error);
      const std::string archive = work + "/allowed.bak";
      const std::string restored = work + "/allowed";
      bp::BackupEngine engine;
      bp::BackupOptions options;
      test_support::Check(
          engine.Backup(source, archive, filter, options, &error) &&
              engine.Restore(archive, restored, &error),
          "PREV T6 同一组规则下真实备份成功", error);
      struct stat socket_info;
      test_support::Check(
          ::lstat((restored + "/sub/sock").c_str(), &socket_info) != 0,
          "PREV T6 恢复出来的树里没有 socket");
      test_support::Check(TreeNodes(restored) == paths,
                          "PREV T6 排除 socket 后预览集合 == 恢复出来的集合",
                          "preview=[" + Join(paths) + "] restored=[" +
                              Join(TreeNodes(restored)) + "]");
      ::unlink((source + "/sub/sock").c_str());
    }
    if (socket_fd >= 0) ::close(socket_fd);
  }

  test_support::Section("PREV 5. 遍历失败必须 fail closed（T3 / T4）");
  {
    const std::string work = test_support::FreshDir("backup-preview-faults");
    const std::string source = work + "/src";
    test_support::Mkdir(source, 0755);
    test_support::Mkdir(source + "/child", 0755);
    test_support::WriteFile(source + "/a.txt", "aaa\n", 0644);
    test_support::WriteFile(source + "/child/b.txt", "bbb\n", 0644);

    // T3：子目录 opendir 失败（权限不足最典型的形态）
    FaultPlan open_plan;
    open_plan.call = bp::SourceWalkSyscall::kOpenDirectory;
    open_plan.path = source + "/child";
    open_plan.error_number = EACCES;
    CheckInjectedFailure("T3 子目录 opendir 失败", source, open_plan);

    // T3b：readdir 失败（读到一半 I/O 错误）
    FaultPlan read_plan;
    read_plan.call = bp::SourceWalkSyscall::kReadDirectory;
    read_plan.path = source + "/child";
    read_plan.error_number = EIO;
    CheckInjectedFailure("T3b 子目录 readdir 失败", source, read_plan);

    // T3c：根目录 opendir 失败
    FaultPlan root_open_plan;
    root_open_plan.call = bp::SourceWalkSyscall::kOpenDirectory;
    root_open_plan.path = source;
    root_open_plan.error_number = EACCES;
    CheckInjectedFailure("T3c 根目录 opendir 失败", source, root_open_plan);

    // T4：枚举完成之后、lstat 之前条目消失
    FaultPlan stat_plan;
    stat_plan.call = bp::SourceWalkSyscall::kLstat;
    stat_plan.path = source + "/child/b.txt";
    stat_plan.error_number = ENOENT;
    CheckInjectedFailure("T4 条目在 lstat 前消失", source, stat_plan);

    // T4b：根目录自己的 lstat 失败
    FaultPlan root_stat_plan;
    root_stat_plan.call = bp::SourceWalkSyscall::kLstat;
    root_stat_plan.path = source;
    root_stat_plan.error_number = EACCES;
    CheckInjectedFailure("T4b 源目录 lstat 失败", source, root_stat_plan);

    // 没有注入时同一棵树当然要成功——否则上面的失败可能来自别的原因。
    // 三个条目：a.txt / child / child/b.txt。
    const bp::PreviewResult clean = bp::PreviewBackupSelection(source, {});
    test_support::Check(
        clean.error.empty() && clean.included_count == 3,
        "PREV T4c 同一棵树在没有注入时成功（对照组）",
        clean.error + " count=" + std::to_string(clean.included_count));
  }

  test_support::Section("PREV 6. 300 项窗口：只展示前 300，整棵树都被检查");
  {
    const std::string work = test_support::FreshDir("backup-preview-window");
    const std::string source = work + "/src";
    test_support::Mkdir(source, 0755);
    const int kFiles = 350;
    for (int index = 0; index < kFiles; ++index) {
      char name[64];
      std::snprintf(name, sizeof(name), "f%03d.dat", index);
      test_support::WriteFile(source + "/" + name, "x", 0644);
    }

    // T8：350 个普通条目
    const bp::PreviewResult preview = bp::PreviewBackupSelection(source, {});
    test_support::Check(preview.error.empty(), "PREV T8 预览成功",
                        preview.error);
    test_support::Check(
        preview.total_entries == static_cast<std::size_t>(kFiles),
        "PREV T8 total_entries 是整棵树的数字",
        std::to_string(preview.total_entries));
    test_support::Check(
        preview.included_count == static_cast<std::size_t>(kFiles),
        "PREV T8 included_count 是整棵树的数字（不是窗口里的）",
        std::to_string(preview.included_count));
    test_support::Check(preview.items.size() == bp::kPreviewEntryLimit,
                        "PREV T8 展示窗口是 300 条",
                        std::to_string(preview.items.size()));
    test_support::Check(preview.truncated, "PREV T8 truncated 为真");

    std::vector<bp::ArchiveEntry> entries;
    std::string scan_error;
    test_support::Check(
        bp::ScanSourceTree(source, nullptr, &entries, &scan_error),
        "PREV T8 真实备份扫描成功", scan_error);
    const std::vector<std::string> scan_order = ScanOrder(entries);
    const std::vector<std::string> preview_order = IncludedInOrder(preview);
    test_support::Check(
        preview_order.size() == bp::kPreviewEntryLimit &&
            std::equal(preview_order.begin(), preview_order.end(),
                       scan_order.begin()),
        "PREV T8 预览前 300 项 == 真实备份扫描的前 300 项（逐项，未排序）",
        Join(preview_order).substr(0, 160));

    // T8b：显式 limit 同理。
    const bp::PreviewResult small = bp::PreviewBackupSelection(source, {}, 5);
    test_support::Check(
        small.items.size() == 5 && small.truncated &&
            small.total_entries == static_cast<std::size_t>(kFiles),
        "PREV T8b 显式 limit 只影响窗口，不影响总数",
        std::to_string(small.items.size()) + "/" +
            std::to_string(small.total_entries));

    // T8c：条目数正好等于窗口时不算 truncated。
    const bp::PreviewResult exact =
        bp::PreviewBackupSelection(source, {}, kFiles);
    test_support::Check(
        exact.items.size() == static_cast<std::size_t>(kFiles) &&
            !exact.truncated,
        "PREV T8c 条目数正好等于窗口时不算 truncated",
        std::to_string(exact.items.size()) +
            " truncated=" + (exact.truncated ? "true" : "false"));
  }

  test_support::Section("PREV 7. 窗口之外的问题不能被掩盖（T9 / T10）");
  {
    const std::string work = test_support::FreshDir("backup-preview-blocked");
    const std::string source = work + "/src";
    test_support::Mkdir(source, 0755);
    for (int index = 0; index < 300; ++index) {
      char name[64];
      std::snprintf(name, sizeof(name), "f%03d.dat", index);
      test_support::WriteFile(source + "/" + name, "x", 0644);
    }

    // T9：第 301 个条目是 socket（名字排在所有 f***.dat 之后）
    const int socket_fd =
        test_support::CreateUnixSocket(source + "/zzz-socket");
    if (socket_fd < 0) {
      test_support::Note("PREV T9 无法创建 unix socket，跳过这一条");
    } else {
      const bp::PreviewResult blocked = bp::PreviewBackupSelection(source, {});
      std::vector<bp::ArchiveEntry> entries;
      std::string scan_error;
      const bool backup_ok =
          bp::ScanSourceTree(source, nullptr, &entries, &scan_error);
      test_support::Check(
          blocked.error_kind == bp::PreviewErrorKind::kSelectionBlocked,
          "PREV T9 第 301 条的 socket 不会被 300 项窗口掩盖", blocked.error);
      test_support::Check(!backup_ok, "PREV T9 真实备份确实会失败", scan_error);
      test_support::Check(
          blocked.error == scan_error, "PREV T9 两边报出同一句原文",
          "preview=[" + blocked.error + "] backup=[" + scan_error + "]");
      ::unlink((source + "/zzz-socket").c_str());
      ::close(socket_fd);
    }

    // T10：第 301 个条目 lstat 失败
    FaultPlan late_plan;
    late_plan.call = bp::SourceWalkSyscall::kLstat;
    late_plan.path = source + "/f300.dat";
    late_plan.error_number = ENOENT;
    test_support::WriteFile(source + "/f300.dat", "x", 0644);
    FaultPlan mutable_plan = late_plan;
    const bp::SourceWalkFaults faults = FaultsFor(&mutable_plan);
    const bp::PreviewResult late =
        bp::PreviewBackupSelection(source, {}, bp::kPreviewEntryLimit, &faults);
    std::vector<bp::ArchiveEntry> late_entries;
    std::string late_error;
    const bool late_backup_ok = bp::ScanSourceTree(
        source, nullptr, &late_entries, &late_error, &faults);
    test_support::Check(
        !late.error.empty() && !late_backup_ok && late.error == late_error,
        "PREV T10 第 301 条的 lstat 失败同样让两边一起失败",
        "preview=[" + late.error + "] backup=[" + late_error + "]");
    test_support::Check(late.items.empty() && late.included_count == 0,
                        "PREV T10 失败时不返回窗口里的那 300 条");
  }

  test_support::Section("PREV 8. 归档路径 grammar（PTH-01..PTH-05）");
  {
    // Linux 允许文件名里出现反斜杠，也允许 "C:note.txt" 这种形状；归档格式
    // 两者都不接受（见 include/archive_path.h）。共享 walker 现在在生成
    // archive-relative path 之后调用的是**完整**的 IsValidArchivePath，所以这
    // 类名字必须在同一层就让预览失败——否则预览会把它列出来，而真实备份随后
    // 必然失败，正好违反"预览 == 备份"。
    const std::string work = test_support::FreshDir("backup-preview-grammar");
    const std::string backslash = std::string(1, '\\');

    // 每个非法形状：先直接问 IsValidArchivePath（确认当前 grammar 的真实
    // 结论，不猜），再要求预览与备份给出同一个结论、同一句原文。
    struct GrammarCase {
      std::string label;
      std::string relative;  // 归档相对路径
      bool is_directory;
    };
    const GrammarCase cases[] = {
        {"PTH-01 文件名含反斜杠", std::string("a") + backslash + "b.txt",
         false},
        {"PTH-02 目录名含反斜杠", std::string("dir") + backslash + "name",
         true},
        {"PTH-03 盘符风格文件名", "C:note.txt", false},
    };

    int index = 0;
    for (const GrammarCase& item : cases) {
      const std::string label = item.label;
      const std::string relative =
          item.is_directory ? item.relative + "/file.txt" : item.relative;
      ++index;
      const std::string source = work + "/case-" + std::to_string(index);

      // grammar 自己怎么说？用与 walker **相同**的参数（非首条、真实类型）。
      //
      // 目录这一条要特别说清楚：walker 在遍历到目录自己时就失败了（还没进入
      // 它），所以它报的是"目录那一条"的原文。下面的 grammar 期望值因此取
      // item.relative；同时另外确认更深的子路径本身也是非法的——否则
      // "目录恰好合法、子文件非法"这种情况会被漏掉。
      std::string grammar_error;
      const bool grammar_ok =
          bp::IsValidArchivePath(item.relative, false, item.is_directory,
                                 bp::kMaxArchivePathLength, &grammar_error);
      test_support::Check(!grammar_ok,
                          "PREV " + label + " IsValidArchivePath 拒绝该路径",
                          grammar_error);
      if (item.is_directory) {
        std::string deep_error;
        const bool deep_ok = bp::IsValidArchivePath(
            relative, false, false, bp::kMaxArchivePathLength, &deep_error);
        test_support::Check(!deep_ok, "PREV " + label + " 子路径本身也是非法的",
                            deep_error);
      }

      test_support::Check(test_support::Mkdir(source, 0755),
                          "PREV " + label + " 建好源目录");
      bool materialized = false;
      if (item.is_directory) {
        materialized =
            test_support::Mkdir(source + "/" + item.relative, 0755) &&
            test_support::WriteFile(source + "/" + item.relative + "/file.txt",
                                    "x\n", 0644);
      } else {
        materialized =
            test_support::WriteFile(source + "/" + item.relative, "x\n", 0644);
      }
      test_support::Check(materialized,
                          "PREV " + label + " 该名字在 Linux 上真的建得出来");

      const bp::PreviewResult preview = bp::PreviewBackupSelection(source, {});
      std::vector<bp::ArchiveEntry> entries;
      std::string scan_error;
      const bool backup_ok =
          bp::ScanSourceTree(source, nullptr, &entries, &scan_error);
      test_support::Check(!preview.error.empty(),
                          "PREV " + label + " 预览 fail closed", preview.error);
      test_support::Check(!backup_ok, "PREV " + label + " 真实备份同样失败",
                          scan_error);
      test_support::Check(
          !preview.error.empty() && preview.error == scan_error,
          "PREV " + label + " 两边报出同一句原文",
          "preview=[" + preview.error + "] backup=[" + scan_error + "]");
      test_support::Check(
          preview.error == grammar_error,
          "PREV " + label + " 用的就是同一个 grammar 的原文",
          "preview=[" + preview.error + "] grammar=[" + grammar_error + "]");
      test_support::Check(preview.items.empty() && preview.included_count == 0,
                          "PREV " + label + " 失败时不返回任何条目");
    }

    // PTH-04：合法名字不能被误伤。Linux 上完全正常的名字（含中文、空格、点、
    // 连字符、下划线）必须全部通过，而且预览与真实备份看到的是同一批。
    const std::string legal = work + "/legal";
    test_support::Check(test_support::Mkdir(legal, 0755),
                        "PREV PTH-04 建好合法名字的源目录");
    const char* legal_names[] = {"a_b.txt",  "a-b.txt",        "a.b.txt",
                                 "中文.txt", "space name.txt", "UPPER.TXT"};
    const std::size_t legal_count =
        sizeof(legal_names) / sizeof(legal_names[0]);
    for (const char* name : legal_names) {
      std::string grammar_error;
      test_support::Check(
          bp::IsValidArchivePath(name, false, false, bp::kMaxArchivePathLength,
                                 &grammar_error),
          std::string("PREV PTH-04 IsValidArchivePath 接受 ") + name,
          grammar_error);
      test_support::Check(
          test_support::WriteFile(legal + "/" + name, "x\n", 0644),
          std::string("PREV PTH-04 建好 ") + name);
    }
    const bp::PreviewResult legal_preview =
        bp::PreviewBackupSelection(legal, {});
    test_support::Check(legal_preview.error.empty() &&
                            legal_preview.included_count == legal_count,
                        "PREV PTH-04 合法名字全部通过预览",
                        legal_preview.error + " count=" +
                            std::to_string(legal_preview.included_count));
    std::vector<bp::ArchiveEntry> legal_entries;
    std::string legal_error;
    test_support::Check(
        bp::ScanSourceTree(legal, nullptr, &legal_entries, &legal_error),
        "PREV PTH-04 合法名字全部通过真实备份扫描", legal_error);
    test_support::Check(
        IncludedInOrder(legal_preview) == ScanOrder(legal_entries),
        "PREV PTH-04 两边给出的条目序列一致",
        Join(IncludedInOrder(legal_preview)));

    // PTH-05：source root 自己（"."）不能被新增的 grammar 检查搞坏：它是
    // is_first_entry = true + is_directory = true 的那一条特例。
    const std::string root_source = work + "/root-case";
    test_support::Check(test_support::Mkdir(root_source, 0755),
                        "PREV PTH-05 建好源目录");
    test_support::Check(
        test_support::WriteFile(root_source + "/only.txt", "x\n", 0644),
        "PREV PTH-05 建好一个普通文件");
    const bp::PreviewResult root_preview =
        bp::PreviewBackupSelection(root_source, {});
    std::vector<bp::ArchiveEntry> root_entries;
    std::string root_error;
    test_support::Check(
        root_preview.error.empty() && root_preview.included_count == 1,
        "PREV PTH-05 root 仍然是合法的第一条（预览）", root_preview.error);
    test_support::Check(
        bp::ScanSourceTree(root_source, nullptr, &root_entries, &root_error) &&
            !root_entries.empty() && root_entries.front().archive_path == ".",
        "PREV PTH-05 root 仍然是第一条 entry（备份）", root_error);
    const std::string empty_root = work + "/empty-root";
    test_support::Check(test_support::Mkdir(empty_root, 0755),
                        "PREV PTH-05 建好空源目录");
    const bp::PreviewResult empty_preview =
        bp::PreviewBackupSelection(empty_root, {});
    std::vector<bp::ArchiveEntry> empty_entries;
    std::string empty_error;
    test_support::Check(
        empty_preview.error.empty() && empty_preview.included_count == 0 &&
            bp::ScanSourceTree(empty_root, nullptr, &empty_entries,
                               &empty_error) &&
            empty_entries.size() == 1,
        "PREV PTH-05 空目录：预览 0 项，备份只有 root 一条", empty_error);
  }

  test_support::Section("PREV 9. 无法给出结果时必须明确失败");
  {
    const std::string work = test_support::FreshDir("backup-preview-error");
    const std::string source = work + "/src";
    test_support::Mkdir(source, 0755);
    test_support::WriteFile(source + "/a.txt", "aaa\n", 0644);

    // P6：非法 DSL 由核心拒绝，不会静默退化成"没有规则"。
    const bp::PreviewResult bad = bp::PreviewBackupSelection(
        source, {Rule(bp::FilterAction::kInclude, "nonsense:xx")});
    test_support::Check(
        !bad.error.empty() &&
            bad.error_kind == bp::PreviewErrorKind::kRuleRejected,
        "PREV P6 非法规则 -> kRuleRejected", bad.error);
    test_support::Check(bad.items.empty(),
                        "PREV P6 规则被拒绝时一个条目都不返回");

    bp::Filter probe;
    std::string direct;
    probe.AddRule(bp::FilterAction::kInclude, "nonsense:xx", &direct);
    test_support::Check(!direct.empty() && bad.error == direct,
                        "PREV P6 报错就是 Filter::AddRule 的原文",
                        "preview=[" + bad.error + "] filter=[" + direct + "]");

    const bp::PreviewResult mixed = bp::PreviewBackupSelection(
        source, {Rule(bp::FilterAction::kInclude, "ext:txt"),
                 Rule(bp::FilterAction::kExclude, "broken:")});
    test_support::Check(
        !mixed.error.empty() &&
            mixed.error_kind == bp::PreviewErrorKind::kRuleRejected,
        "PREV P6 好坏规则混在一起时整体失败", mixed.error);

    const bp::PreviewResult missing =
        bp::PreviewBackupSelection(work + "/no-such-dir", {});
    test_support::Check(
        !missing.error.empty() &&
            missing.error_kind == bp::PreviewErrorKind::kSourceUnusable,
        "PREV P6 源目录不存在 -> kSourceUnusable", missing.error);
    test_support::Check(missing.items.empty() && missing.included_count == 0,
                        "PREV P6 源目录不可用时不给任何条目");

    test_support::WriteFile(work + "/plain.txt", "x", 0644);
    const bp::PreviewResult file_source =
        bp::PreviewBackupSelection(work + "/plain.txt", {});
    test_support::Check(
        file_source.error_kind == bp::PreviewErrorKind::kSourceUnusable,
        "PREV P6 源路径是普通文件 -> kSourceUnusable");

    const std::string empty = work + "/empty";
    test_support::Mkdir(empty, 0755);
    const bp::PreviewResult nothing = bp::PreviewBackupSelection(empty, {});
    test_support::Check(nothing.error.empty() && nothing.items.empty() &&
                            nothing.included_count == 0 &&
                            nothing.total_entries == 0 && !nothing.truncated,
                        "PREV P6 空目录 -> 0 项且不是错误");
  }

  test_support::Section("PREV 10. 路径语义");
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
