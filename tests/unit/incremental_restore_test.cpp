// incremental_restore_test.cpp
//
// PR #18 的端到端测试：增量备份 + 依赖链恢复。
//
// oracle 刻意选**产品自己的完整备份路径**：每个状态下都另外写一份真实 Full
// 快照并恢复出来，再要求"增量链恢复出来的树"与它逐节点一致（类型、mode、
// mtime 秒+纳秒、属主、内容、软链接目标、设备号、hardlink 分组）。
// 这样断言的不是"两次调用同一个函数得到同一个结果"，而是"增量恢复 ==
// 完整恢复"。
//
// 同时覆盖必须拒绝的情形：父亲不存在、父亲身份不符、以及"规则或算法变了"必须
// 重新建基线而不是接着旧链。

#include "incremental_restore.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <vector>

#include "archive_pipeline.h"
#include "backup_catalog.h"
#include "incremental_backup.h"
#include "incremental_delta.h"
#include "source_manifest.h"
#include "test_support.h"
#include "tree_scanner.h"

namespace bp = backupproject;

namespace {

// 全量扫描当前源树（含源根）——与备份看到的集合完全一致。
bool ScanAll(const std::string& source, const bp::Filter& filter,
             std::vector<bp::ArchiveEntry>* entries, std::string* error) {
  return bp::ScanSourceTree(source, &filter, entries, error);
}

// 用**产品完整备份路径**给某个状态造一份 oracle：完整快照 + 它的恢复结果。
bool MakeOracle(const std::string& source, const std::string& repository,
                const std::string& name, const bp::Filter& filter,
                std::string* restored_tree, std::string* error) {
  std::vector<bp::ArchiveEntry> entries;
  if (!ScanAll(source, filter, &entries, error)) return false;
  const std::string archive = repository + "/" + name + ".bak";
  bp::BackupOptions options;
  if (!bp::RunBackupPipelineFromEntries(entries, archive, options, error)) {
    return false;
  }
  *restored_tree = repository + "/" + name + "-tree";
  bp::RestoreReport report;
  return bp::RunRestorePipeline(archive, *restored_tree, bp::RestoreOptions{},
                                &report, error);
}

std::string SnapshotName(int index) {
  // 名字里带序号就够了；真实产品用 catalog 生成的时间戳名字。
  return "snap-" + std::to_string(index) + ".bak";
}

}  // namespace

int main() {
  const std::string work = test_support::FreshDir("inc-chain");
  const std::string source = work + "/src";
  const std::string repository = work + "/repo";
  const std::string oracle_repo = work + "/oracle";
  test_support::Mkdir(source, 0755);
  test_support::Mkdir(repository, 0755);
  test_support::Mkdir(oracle_repo, 0755);

  const std::string repository_identity = "repo-identity";
  const std::string source_path = source;

  // ---- 状态 1：建立完整基线 ----
  test_support::Section("INC-R 1. 第一次增量 = 完整基线");
  {
    test_support::WriteFile(source + "/a.txt", "alpha", 0644);
    test_support::WriteFile(source + "/b.txt", "bravo", 0644);
    test_support::Mkdir(source + "/dir", 0755);
    test_support::WriteFile(source + "/dir/c.txt", "charlie", 0644);
    test_support::CreateSymlink("a.txt", source + "/link");

    bp::Filter filter;
    bp::IncrementalOutcome outcome;
    std::string error;
    bp::BackupOptions options;
    test_support::Check(
        bp::RunIncrementalBackup(source, repository, SnapshotName(1),
                                 repository_identity, filter, options, {}, {},
                                 "", &outcome, &error),
        "INC-R T1 第一次增量备份成功", error);
    test_support::Check(
        outcome.kind == bp::IncrementalOutcome::Kind::kFullBaseline,
        "INC-R T1 没有基线时建立完整基线（不是空 delta）");
    test_support::Check(!outcome.baseline_reason.empty(),
                        "INC-R T1 说明了为什么建基线", outcome.baseline_reason);
    // 基线是完整归档，不是 delta。
    test_support::Check(
        bp::ClassifySnapshotFile(repository + "/" + SnapshotName(1), &error) ==
            bp::SnapshotFileKind::kContainer,
        "INC-R T1 基线是一份完整 v2 归档");

    std::string oracle;
    test_support::Check(
        MakeOracle(source, oracle_repo, "state1", filter, &oracle, &error),
        "INC-R T1 oracle 完整备份成功", error);
    const std::string restored = work + "/restore1";
    bp::RestoreReport report;
    test_support::Check(
        bp::RestoreSnapshotChain(repository, SnapshotName(1), restored,
                                 bp::RestoreOptions{}, &report, &error),
        "INC-R T1 链恢复成功", error);
    std::string detail;
    test_support::Check(test_support::CompareTrees(oracle, restored, &detail),
                        "INC-R T1 增量恢复 == 完整恢复（逐节点）", detail);
  }

  // ---- 状态 2：真实变化 + same-size/same-mtime 改写 ----
  test_support::Section(
      "INC-R 2. 第二次 = delta；含 same-size + same-mtime 的改写");
  {
    const std::int64_t fixed_time = 1700000000;
    test_support::Check(
        test_support::SetTimes(source + "/a.txt", fixed_time, 0),
        "INC-R T2 固定 a.txt 的 mtime");
    // 就地改写 a.txt：长度与 mtime 都不变——metadata-first 看不见的那一种。
    const int fd = ::open((source + "/a.txt").c_str(), O_WRONLY | O_CLOEXEC);
    test_support::Check(fd >= 0, "INC-R T2 打开 a.txt 以便就地改写");
    if (fd >= 0) {
      const char replacement[] = "ALPHA";
      test_support::Check(::pwrite(fd, replacement, 5, 0) == 5,
                          "INC-R T2 就地改写 a.txt（长度不变）");
      ::close(fd);
    }
    test_support::Check(
        test_support::SetTimes(source + "/a.txt", fixed_time, 0),
        "INC-R T2 把 a.txt 的 mtime 压回原值");
    // 其它真实变化：新增、删除、软链接目标改变。
    test_support::WriteFile(source + "/d.txt", "delta", 0644);
    test_support::Check(::unlink((source + "/b.txt").c_str()) == 0,
                        "INC-R T2 删除 b.txt");
    test_support::Check(::unlink((source + "/link").c_str()) == 0,
                        "INC-R T2 删除旧软链接");
    test_support::CreateSymlink("dir", source + "/link");

    bp::Filter filter;
    bp::BackupOptions options;
    bp::IncrementalOutcome outcome;
    std::string error;
    test_support::Check(
        bp::RunIncrementalBackup(source, repository, SnapshotName(2),
                                 repository_identity, filter, options, {}, {},
                                 "", &outcome, &error),
        "INC-R T2 第二次增量备份成功", error);
    test_support::Check(outcome.kind == bp::IncrementalOutcome::Kind::kDelta,
                        "INC-R T2 有基线且确有变化 -> delta");
    // 恰好：a.txt 就地改写 = modified，link 换了目标 = modified，
    // d.txt 新增 = added，b.txt 删除 = removed。
    // added / removed 都按 archive_path 判定，所以"同名的软链接换目标"是
    // modified 而不是 removed + added。
    test_support::Check(
        outcome.summary.added == 1 && outcome.summary.modified == 2 &&
            outcome.summary.removed == 1 &&
            outcome.summary.metadata_changed == 0,
        "INC-R T2 变化分类正确（改写/软链接 = modified，新增 = added，删除 = "
        "removed）",
        "added=" + std::to_string(outcome.summary.added) +
            " modified=" + std::to_string(outcome.summary.modified) +
            " removed=" + std::to_string(outcome.summary.removed));
    test_support::Check(
        bp::ClassifySnapshotFile(repository + "/" + SnapshotName(2), &error) ==
            bp::SnapshotFileKind::kDelta,
        "INC-R T2 第二份快照真的是 delta（不是改名的 Full）");

    std::string oracle;
    test_support::Check(
        MakeOracle(source, oracle_repo, "state2", filter, &oracle, &error),
        "INC-R T2 oracle 完整备份成功", error);
    const std::string restored = work + "/restore2";
    bp::RestoreReport report;
    test_support::Check(
        bp::RestoreSnapshotChain(repository, SnapshotName(2), restored,
                                 bp::RestoreOptions{}, &report, &error),
        "INC-R T2 链恢复成功", error);
    std::string detail;
    test_support::Check(test_support::CompareTrees(oracle, restored, &detail),
                        "INC-R T2 判别：same-size/same-mtime 改写被正确恢复",
                        detail);
    std::string content;
    test_support::Check(test_support::ReadFile(restored + "/a.txt", &content) &&
                            content == "ALPHA",
                        "INC-R T2 改写后的内容真的生效了", content);
    test_support::Check(!test_support::Exists(restored + "/b.txt"),
                        "INC-R T2 tombstone 生效：b.txt 不在恢复结果里");
  }

  // ---- 状态 3：类型变化 + 目录删除 ----
  test_support::Section("INC-R 3. 第三次 = 类型变化与目录删除");
  {
    // dir/c.txt -> dir 变成一个普通文件（目录 -> 文件）
    test_support::Check(::unlink((source + "/dir/c.txt").c_str()) == 0,
                        "INC-R T3 删除 dir/c.txt");
    test_support::Check(::rmdir((source + "/dir").c_str()) == 0,
                        "INC-R T3 删除 dir");
    test_support::WriteFile(source + "/dir", "now a file", 0644);
    // 新增一个目录并删除 d.txt
    test_support::Mkdir(source + "/newdir", 0755);
    test_support::WriteFile(source + "/newdir/e.txt", "echo", 0644);
    test_support::Check(::unlink((source + "/d.txt").c_str()) == 0,
                        "INC-R T3 删除 d.txt");

    bp::Filter filter;
    bp::BackupOptions options;
    bp::IncrementalOutcome outcome;
    std::string error;
    test_support::Check(
        bp::RunIncrementalBackup(source, repository, SnapshotName(3),
                                 repository_identity, filter, options, {}, {},
                                 "", &outcome, &error),
        "INC-R T3 第三次增量备份成功", error);
    test_support::Check(outcome.kind == bp::IncrementalOutcome::Kind::kDelta,
                        "INC-R T3 第三次也是 delta");

    std::string oracle;
    test_support::Check(
        MakeOracle(source, oracle_repo, "state3", filter, &oracle, &error),
        "INC-R T3 oracle 完整备份成功", error);
    const std::string restored = work + "/restore3";
    bp::RestoreReport report;
    test_support::Check(
        bp::RestoreSnapshotChain(repository, SnapshotName(3), restored,
                                 bp::RestoreOptions{}, &report, &error),
        "INC-R T3 三次链恢复成功", error);
    std::string detail;
    test_support::Check(test_support::CompareTrees(oracle, restored, &detail),
                        "INC-R T3 类型变化（目录 -> 文件）恢复正确", detail);
    // 目录 -> 文件必须把旧目录整棵删掉，不能"文件覆盖目录"。
    struct stat info;
    test_support::Check(
        test_support::StatOf(restored + "/dir", &info) && S_ISREG(info.st_mode),
        "INC-R T3 dir 现在是普通文件（旧目录子树已删除）");
    test_support::Check(!test_support::Exists(restored + "/dir/c.txt"),
                        "INC-R T3 旧目录的内容没有残留");
  }

  // ---- 无变化不建快照 ----
  test_support::Section("INC-R 4. 没有有效变化就不建新快照");
  {
    bp::Filter filter;
    bp::BackupOptions options;
    bp::IncrementalOutcome outcome;
    std::string error;
    const std::string name = SnapshotName(4);
    test_support::Check(
        bp::RunIncrementalBackup(source, repository, name, repository_identity,
                                 filter, options, {}, {}, "", &outcome, &error),
        "INC-R T4 无变化时调用成功", error);
    test_support::Check(
        outcome.kind == bp::IncrementalOutcome::Kind::kNoChanges,
        "INC-R T4 无有效变化 -> 不建新快照");
    test_support::Check(!test_support::Exists(repository + "/" + name),
                        "INC-R T4 确实没有留下新文件");

    // 只 touch 一个目录：按产品决定不算变化。
    test_support::Check(
        test_support::SetTimes(source + "/newdir", 1800000000, 123456789),
        "INC-R T4 touch 目录");
    bp::IncrementalOutcome after_touch;
    test_support::Check(
        bp::RunIncrementalBackup(source, repository, SnapshotName(5),
                                 repository_identity, filter, options, {}, {},
                                 "", &after_touch, &error),
        "INC-R T4 touch 目录之后调用成功", error);
    test_support::Check(
        after_touch.kind == bp::IncrementalOutcome::Kind::kNoChanges,
        "INC-R T4 只 touch 目录仍然不建快照（产品决定不翻案）");
    test_support::Check(
        !test_support::Exists(repository + "/" + SnapshotName(5)),
        "INC-R T4 touch 目录也没有留下新文件");
  }

  // ---- 缺失 / 错误的父亲 ----
  test_support::Section("INC-R 5. missing parent / wrong parent identity");
  {
    const std::string missing_repo = work + "/missing-repo";
    test_support::Mkdir(missing_repo, 0755);
    // 复制 delta 与它的 manifest，但**不**复制父亲。
    std::string delta_bytes;
    test_support::Check(test_support::ReadFile(
                            repository + "/" + SnapshotName(2), &delta_bytes),
                        "INC-R T5 读入 delta 字节");
    test_support::Check(
        test_support::WriteFile(missing_repo + "/" + SnapshotName(2),
                                delta_bytes, 0644),
        "INC-R T5 在另一个仓库里放一份 delta");
    bp::SnapshotChain chain;
    std::string error;
    test_support::Check(!bp::ResolveSnapshotChain(missing_repo, SnapshotName(2),
                                                  &chain, &error),
                        "INC-R T5 判别：父亲不存在时拒绝解析链");
    test_support::Check(error.find("parent") != std::string::npos ||
                            error.find(SnapshotName(1)) != std::string::npos,
                        "INC-R T5 拒绝理由说清了是父亲的问题", error);
    const std::string restored = work + "/restore-missing";
    bp::RestoreReport report;
    test_support::Check(
        !bp::RestoreSnapshotChain(missing_repo, SnapshotName(2), restored,
                                  bp::RestoreOptions{}, &report, &error),
        "INC-R T5 判别：链不完整时拒绝恢复");
    test_support::Check(!test_support::Exists(restored),
                        "INC-R T5 失败时不留半个目标目录");
  }

  // ---- 规则 / 算法变化 -> 新基线 ----
  test_support::Section("INC-R 6. 规则或算法变了必须重新建基线");
  {
    bp::BackupOptions options;
    bp::IncrementalOutcome outcome;
    std::string error;
    bp::Filter include_filter;
    std::string filter_error;
    test_support::Check(include_filter.AddRule(bp::FilterAction::kInclude,
                                               "ext:txt", &filter_error),
                        "INC-R T6 编译 include 规则", filter_error);
    test_support::Check(
        bp::RunIncrementalBackup(source, repository, SnapshotName(6),
                                 repository_identity, include_filter, options,
                                 {"ext:txt"}, {}, "", &outcome, &error),
        "INC-R T6 换规则后的增量调用成功", error);
    test_support::Check(
        outcome.kind == bp::IncrementalOutcome::Kind::kFullBaseline,
        "INC-R T6 判别：规则变了 -> 重新建完整基线，不把规则变化当 tombstone",
        outcome.baseline_reason);

    // 算法变化同理。
    bp::BackupOptions compressed;
    compressed.compression_method = bp::CompressionMethod::kHuffman;
    bp::IncrementalOutcome compressed_outcome;
    test_support::Check(bp::RunIncrementalBackup(
                            source, repository, SnapshotName(7),
                            repository_identity, include_filter, compressed,
                            {"ext:txt"}, {}, "", &compressed_outcome, &error),
                        "INC-R T6 换压缩算法后的增量调用成功", error);
    test_support::Check(
        compressed_outcome.kind == bp::IncrementalOutcome::Kind::kFullBaseline,
        "INC-R T6 判别：pack/compress/encrypt 变了 -> 新基线",
        compressed_outcome.baseline_reason);
  }

  // ---- 特殊文件与硬链接走真实的链 ----
  test_support::Section(
      "INC-R 7. hardlink / FIFO / 软链接 / 权限变更在链上同样正确");
  {
    const std::string work2 = test_support::FreshDir("inc-chain-special");
    const std::string src2 = work2 + "/src";
    const std::string repo2 = work2 + "/repo";
    const std::string oracle2 = work2 + "/oracle";
    test_support::Mkdir(src2, 0755);
    test_support::Mkdir(repo2, 0755);
    test_support::Mkdir(oracle2, 0755);

    // 状态 1：一个硬链接对、一个 FIFO、一个软链接、一个子目录。
    test_support::WriteFile(src2 + "/leader.txt", "shared-body", 0644);
    test_support::Check(
        test_support::CreateHardlink(src2 + "/leader.txt", src2 + "/peer.txt"),
        "INC-R T7 建立硬链接对");
    test_support::Check(test_support::CreateFifo(src2 + "/pipe", 0644),
                        "INC-R T7 建立 FIFO");
    test_support::CreateSymlink("leader.txt", src2 + "/link");
    test_support::Mkdir(src2 + "/sub", 0755);
    test_support::WriteFile(src2 + "/sub/deep.txt", "deep", 0640);

    bp::Filter filter;
    bp::BackupOptions options;
    bp::IncrementalOutcome outcome;
    std::string error;
    test_support::Check(bp::RunIncrementalBackup(
                            src2, repo2, SnapshotName(11), repository_identity,
                            filter, options, {}, {}, "", &outcome, &error),
                        "INC-R T7 特殊文件基线建立成功", error);
    test_support::Check(
        outcome.kind == bp::IncrementalOutcome::Kind::kFullBaseline,
        "INC-R T7 第一次是完整基线");

    // 状态
    // 2：删掉一个硬链接、新增另一个；通过链接改内容（两个名字都要跟着变）；
    // 改软链接目标；删 FIFO；加一个新 FIFO；改权限与属主可见位。
    test_support::Check(::unlink((src2 + "/peer.txt").c_str()) == 0,
                        "INC-R T7 删掉一个硬链接");
    test_support::Check(
        test_support::CreateHardlink(src2 + "/leader.txt", src2 + "/peer2.txt"),
        "INC-R T7 新增一个指向同一 inode 的硬链接");
    test_support::WriteFile(src2 + "/leader.txt", "rewritten-via-link", 0644);
    test_support::Check(::unlink((src2 + "/link").c_str()) == 0,
                        "INC-R T7 删掉旧软链接");
    test_support::CreateSymlink("sub", src2 + "/link");
    test_support::Check(::unlink((src2 + "/pipe").c_str()) == 0,
                        "INC-R T7 删掉 FIFO");
    test_support::Check(test_support::CreateFifo(src2 + "/pipe2", 0600),
                        "INC-R T7 新增 FIFO");
    test_support::Check(::chmod((src2 + "/sub/deep.txt").c_str(), 0604) == 0,
                        "INC-R T7 改 deep.txt 的权限");

    bp::IncrementalOutcome second;
    test_support::Check(bp::RunIncrementalBackup(
                            src2, repo2, SnapshotName(12), repository_identity,
                            filter, options, {}, {}, "", &second, &error),
                        "INC-R T7 特殊文件 delta 写出成功", error);
    test_support::Check(second.kind == bp::IncrementalOutcome::Kind::kDelta,
                        "INC-R T7 第二次是 delta");
    // 至少要有：内容改写（两个链接名都算）、软链接目标变化、FIFO 增删里的一类。
    test_support::Check(
        second.summary.modified >= 2 && second.summary.added >= 1 &&
            second.summary.removed >= 1,
        "INC-R T7 变化分类覆盖修改 / 新增 / 删除",
        "added=" + std::to_string(second.summary.added) +
            " modified=" + std::to_string(second.summary.modified) +
            " removed=" + std::to_string(second.summary.removed));

    std::string oracle_tree;
    test_support::Check(
        MakeOracle(src2, oracle2, "special2", filter, &oracle_tree, &error),
        "INC-R T7 oracle 完整备份成功", error);
    const std::string restored2 = work2 + "/restore2";
    bp::RestoreReport report2;
    test_support::Check(
        bp::RestoreSnapshotChain(repo2, SnapshotName(12), restored2,
                                 bp::RestoreOptions{}, &report2, &error),
        "INC-R T7 特殊文件链恢复成功", error);
    std::string detail2;
    // CompareTrees 会逐项比较类型、mode、mtime（秒+纳秒）、属主、内容、
    // 软链接目标，并单独比较 hardlink 分组 —— 所以这一条断言同时覆盖了
    // "硬链接拓扑在链上保持"与"FIFO / 软链接 / 权限都被正确应用"。
    // 比的是 oracle 的**恢复结果**，不是 oracle 的仓库目录 —— 后者当然和一棵
    // 源树长得不一样。（第一版就写错了这一行，CompareTrees 立刻用
    // "mtime differs" 戳穿了它，这也说明它比的是真实文件系统事实。）
    test_support::Check(
        test_support::CompareTrees(oracle_tree, restored2, &detail2),
        "INC-R T7 判别：特殊文件与硬链接的链恢复 == 完整恢复", detail2);

    // 硬链接拓扑单独再钉一次：两个名字必须还是同一个 inode。
    struct stat first_info;
    struct stat second_info;
    const bool same_inode =
        test_support::StatOf(restored2 + "/leader.txt", &first_info) &&
        test_support::StatOf(restored2 + "/peer2.txt", &second_info) &&
        first_info.st_ino == second_info.st_ino &&
        first_info.st_dev == second_info.st_dev;
    test_support::Check(same_inode,
                        "INC-R T7 恢复出来的两个名字仍然共享一个 inode");
    std::string body;
    test_support::Check(
        test_support::ReadFile(restored2 + "/peer2.txt", &body) &&
            body == "rewritten-via-link",
        "INC-R T7 通过另一个链接改写的内容被正确恢复", body);
    test_support::Check(!test_support::Exists(restored2 + "/pipe") &&
                            test_support::Exists(restored2 + "/pipe2"),
                        "INC-R T7 FIFO 的删除与新增都生效了");
  }

  // ---- 任意 restore point：链中间那一份也要能恢复 ----
  test_support::Section("INC-R 8. 六连链：任意一个 restore point 都能恢复");
  {
    const std::string work3 = test_support::FreshDir("inc-chain-six");
    const std::string src3 = work3 + "/src";
    const std::string repo3 = work3 + "/repo";
    const std::string oracle3 = work3 + "/oracle";
    test_support::Mkdir(src3, 0755);
    test_support::Mkdir(repo3, 0755);
    test_support::Mkdir(oracle3, 0755);

    bp::Filter filter;
    bp::BackupOptions options;
    std::string error;
    // 六个状态：每一次都只动一个文件，于是链是 F0 -> d1 -> ... -> d5。
    std::vector<std::string> oracle_trees;
    for (int step = 0; step < 6; ++step) {
      test_support::WriteFile(
          src3 + "/state.txt",
          "state-" + std::to_string(step) + std::string(8, 'x'), 0644);
      test_support::WriteFile(src3 + "/other.txt", "unchanged", 0644);
      const std::string name = "snap" + std::to_string(step) + ".bak";
      bp::IncrementalOutcome outcome;
      test_support::Check(bp::RunIncrementalBackup(
                              src3, repo3, name, repository_identity, filter,
                              options, {}, {}, "", &outcome, &error),
                          "INC-R T8 第 " + std::to_string(step) + " 步增量成功",
                          error);
      if (step == 0) {
        test_support::Check(
            outcome.kind == bp::IncrementalOutcome::Kind::kFullBaseline,
            "INC-R T8 第一步是完整基线");
      } else {
        test_support::Check(
            outcome.kind == bp::IncrementalOutcome::Kind::kDelta &&
                outcome.parent_file_name ==
                    "snap" + std::to_string(step - 1) + ".bak",
            "INC-R T8 第 " + std::to_string(step) + " 步挂在上一份快照上",
            outcome.parent_file_name);
      }
      std::string tree;
      test_support::Check(
          MakeOracle(src3, oracle3, "six" + std::to_string(step), filter, &tree,
                     &error),
          "INC-R T8 第 " + std::to_string(step) + " 步 oracle 成功", error);
      oracle_trees.push_back(tree);
    }

    // 逐个 restore point 都恢复一遍：包括链中间的、以及最早的那一份。
    for (int step = 0; step < 6; ++step) {
      const std::string restored = work3 + "/restore-" + std::to_string(step);
      bp::RestoreReport report;
      const std::string name = "snap" + std::to_string(step) + ".bak";
      const bool ok = bp::RestoreSnapshotChain(
          repo3, name, restored, bp::RestoreOptions{}, &report, &error);
      if (!ok) {
        test_support::Check(false, "INC-R T8 恢复 " + name, error);
        continue;
      }
      std::string detail;
      test_support::Check(
          test_support::CompareTrees(oracle_trees[step], restored, &detail),
          "INC-R T8 restore point " + std::to_string(step) +
              " 与同状态的完整备份逐节点一致",
          detail);
      std::string body;
      test_support::Check(
          test_support::ReadFile(restored + "/state.txt", &body) &&
              body == "state-" + std::to_string(step) + std::string(8, 'x'),
          "INC-R T8 restore point " + std::to_string(step) + " 的内容正确",
          body);
    }
  }

  // ---- 压缩与增量正交；加密与增量**不相容**（PR #18 closure 收紧的合同）----
  test_support::Section(
      "INC-R 9. 压缩照旧；增量 + 加密被明确拒绝，完整备份仍然支持加密");
  {
    const std::string work4 = test_support::FreshDir("inc-chain-crypto");
    const std::string src4 = work4 + "/src";
    const std::string repo4 = work4 + "/repo";
    const std::string oracle4 = work4 + "/oracle";
    test_support::Mkdir(src4, 0755);
    test_support::Mkdir(repo4, 0755);
    test_support::Mkdir(oracle4, 0755);
    // 内容要够大，压缩与加密才真的被走到（空 payload 什么也证明不了）。
    std::string bulk;
    for (int index = 0; index < 400; ++index) bulk += "compressible-payload-";
    test_support::WriteFile(src4 + "/bulk.txt", bulk, 0644);
    test_support::WriteFile(src4 + "/small.txt", "small", 0644);

    bp::Filter filter;
    bp::BackupOptions encrypted;
    encrypted.compression_method = bp::CompressionMethod::kLzssHuffman;
    encrypted.encryption_method = bp::EncryptionMethod::kAes256CtrHmacSha256;
    encrypted.password = "correct horse battery staple";
    std::string error;
    bp::IncrementalOutcome outcome;
    // PR #18 v1 的合同：增量 + 加密 = 明确拒绝，而且是在**建任何东西之前**。
    test_support::Check(
        !bp::RunIncrementalBackup(src4, repo4, "c0.bak", repository_identity,
                                  filter, encrypted, {}, {}, "", &outcome,
                                  &error),
        "INC-R T9 判别：增量 + 加密被拒绝（旧 HEAD 会接受）", error);
    test_support::Check(
        error.find("does not support encryption") != std::string::npos,
        "INC-R T9 拒绝理由说明外层信封未被认证", error);
    test_support::Check(!test_support::Exists(repo4 + "/c0.bak"),
                        "INC-R T9 拒绝之后没有留下基线");
    test_support::Check(
        !test_support::Exists(repo4 + "/c0.bak.manifest") &&
            !test_support::Exists(repo4 + "/c0.bak.identity"),
        "INC-R T9 拒绝之后没有留下副文件");

    // 同一组算法换成**压缩**（不加密）：仍然完全支持，而且链可以恢复。
    bp::BackupOptions compressed;
    compressed.compression_method = bp::CompressionMethod::kLzssHuffman;
    error.clear();
    bp::IncrementalOutcome first;
    test_support::Check(
        bp::RunIncrementalBackup(src4, repo4, "k0.bak", repository_identity,
                                 filter, compressed, {}, {}, "", &first,
                                 &error) &&
            first.kind == bp::IncrementalOutcome::Kind::kFullBaseline,
        "INC-R T9 压缩（不加密）的增量基线成功", error);
    test_support::WriteFile(src4 + "/small.txt", "small-changed", 0644);
    error.clear();
    bp::IncrementalOutcome second;
    test_support::Check(
        bp::RunIncrementalBackup(src4, repo4, "k1.bak", repository_identity,
                                 filter, compressed, {}, {}, "", &second,
                                 &error) &&
            second.kind == bp::IncrementalOutcome::Kind::kDelta,
        "INC-R T9 压缩（不加密）的 delta 成功", error);
    const std::string restored = work4 + "/restored";
    bp::RestoreReport report;
    error.clear();
    test_support::Check(bp::RestoreSnapshotChain(repo4, "k1.bak", restored,
                                                 bp::RestoreOptions{}, &report,
                                                 &error),
                        "INC-R T9 压缩链恢复成功", error);
    std::string tree;
    test_support::Check(
        MakeOracle(src4, oracle4, "crypto", filter, &tree, &error),
        "INC-R T9 oracle 成功", error);
    std::string detail;
    test_support::Check(test_support::CompareTrees(tree, restored, &detail),
                        "INC-R T9 压缩链恢复 == 完整恢复", detail);

    // 加密本身没有被拿掉：完整备份 + 加密 + 压缩照旧可用，密码错了必须拒绝。
    const std::string full_archive = work4 + "/full-encrypted.bak";
    std::vector<bp::ArchiveEntry> entries;
    error.clear();
    test_support::Check(bp::ScanSourceTree(src4, &filter, &entries, &error) &&
                            bp::RunBackupPipelineFromEntries(
                                entries, full_archive, encrypted, &error),
                        "INC-R T9 完整备份 + 加密 + 压缩仍然可用", error);
    std::string bytes;
    test_support::Check(test_support::ReadFile(full_archive, &bytes) &&
                            bytes.find("small-changed") == std::string::npos,
                        "INC-R T9 判别：完整备份的 payload 是密文");
    const std::string full_restored = work4 + "/full-restored";
    bp::RestoreOptions restore_options;
    restore_options.password = encrypted.password;
    bp::RestoreReport full_report;
    error.clear();
    test_support::Check(bp::RunRestorePipeline(full_archive, full_restored,
                                               restore_options, &full_report,
                                               &error),
                        "INC-R T9 加密的完整备份带密码恢复成功", error);
    bp::RestoreOptions wrong;
    wrong.password = "wrong password";
    const std::string restored_wrong = work4 + "/restored-wrong";
    bp::RestoreReport wrong_report;
    test_support::Check(!bp::RunRestorePipeline(full_archive, restored_wrong,
                                                wrong, &wrong_report, &error),
                        "INC-R T9 判别：错误密码被拒绝");
    test_support::Check(!test_support::Exists(restored_wrong),
                        "INC-R T9 恢复失败时不留半个目标目录");
  }

  return test_support::Finish("incremental_restore_test");
}
