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
#include <sys/wait.h>
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

// ---- R-01 回归用的确定性失败注入 ----
//
// 产品把 staging / overlay / inner_container 三个临时路径放在 destination
// 旁边、名字带 getpid() 后缀。同进程的测试因此能精确算出它们：把"失败点"表达
// 成**文件系统事实**（挡路的同名目录、清理不掉的只读残留、0500 的合并目标），
// 既不需要产品侧测试后门，也不依赖时序或随机性。
std::string TempPath(const std::string& destination, const char* suffix) {
  return destination + "." + std::to_string(static_cast<long>(::getpid())) +
         suffix;
}

// 造一个"清理不掉的残留"：0500 的目录 + 里面一个文件。产品进入时会先
// RemoveTree 同名残留，但只读目录里的 unlink 会 EACCES、unlink 本身又删不掉
// 目录，所以它原样留着——正好把下游那一步钉成失败。
bool MakeStubbornResidue(const std::string& path) {
  if (::mkdir(path.c_str(), 0755) != 0) return false;
  if (!test_support::WriteFile(path + "/residue", "residue", 0644)) {
    return false;
  }
  return ::chmod(path.c_str(), 0500) == 0;
}

void DropStubbornResidue(const std::string& path) {
  ::chmod(path.c_str(), 0755);
  test_support::RemoveTree(path);
}

// 把 BKPINC1 delta 的 **payload 区**首字节改成垃圾，长度保持不变。
//
// 磁盘布局（src/core/incremental_delta.cpp 的 ReadDeltaLayout）：
//   offset 0..23  定长头：magic(8) / version(2) / header_size(2) /
//                 envelope_len(u32 @12) / payload_len(u64 @16)
//   offset 24..   envelope 文本
//   之后          payload = 内层 v2 container
// 只改 payload，所以"文件长度 == 24 + envelope_len + payload_len"这条自洽性
// 检查照样通过：ReadDeltaEnvelope 与 ExtractDeltaPayload 都成功，失败点精确
// 落在"把 container 恢复到 overlay"那一步。
bool CorruptDeltaPayload(const std::string& delta_file) {
  const int fd = ::open(delta_file.c_str(), O_RDWR | O_CLOEXEC);
  if (fd < 0) return false;
  unsigned char header[24] = {0};
  if (::pread(fd, header, sizeof(header), 0) !=
      static_cast<ssize_t>(sizeof(header))) {
    ::close(fd);
    return false;
  }
  const std::uint32_t envelope_len =
      static_cast<std::uint32_t>(header[12]) |
      (static_cast<std::uint32_t>(header[13]) << 8) |
      (static_cast<std::uint32_t>(header[14]) << 16) |
      (static_cast<std::uint32_t>(header[15]) << 24);
  const off_t payload_offset = static_cast<off_t>(sizeof(header) + envelope_len);
  const unsigned char garbage[8] = {0xFF, 0xFF, 0xFF, 0xFF,
                                    0xFF, 0xFF, 0xFF, 0xFF};
  const ssize_t written = ::pwrite(fd, garbage, sizeof(garbage), payload_offset);
  ::close(fd);
  return written == static_cast<ssize_t>(sizeof(garbage));
}

// 只读子树让"尽力而为"的清理删不干净：产品的 RemoveTree 全程 lstat + unlink，
// 不会为了删除先去 chmod。契约允许残留（只占磁盘，下次进入再清一次），所以
// 测试自己按"先放开权限再删"的方式把这棵注入用的树收掉。
void DropTreeForcingModes(const std::string& path) {
  struct stat info;
  if (::lstat(path.c_str(), &info) != 0) return;
  if (!S_ISDIR(info.st_mode)) {
    test_support::RemoveTree(path);
    return;
  }
  ::chmod(path.c_str(), 0700);
  for (const std::string& name : test_support::DirEntries(path)) {
    DropTreeForcingModes(path + "/" + name);
  }
  test_support::RemoveTree(path);
}

// 与产品一致的临时条目**所有权标记**：目录名 + 标记内容同时匹配才会被回收。
// 没有标记的同名目录（= 用户自建）一律不碰，这是本轮 P0 的安全契约。
std::string TempOwnerMarkerPath(const std::string& destination, long owner_pid) {
  return destination + "." + std::to_string(owner_pid) + ".owner";
}

std::string TempOwnerMarkerText(long owner_pid) {
  return std::string("BPRESTORE-TMP-OWNER/1\n") + std::to_string(owner_pid) +
         "\n";
}

// 一次受支持的增量备份（MyPack + 不压缩 + 不加密）。
bool MakeSnapshot(const std::string& source, const std::string& repository,
                  const std::string& name, bp::IncrementalOutcome* outcome,
                  std::string* error) {
  bp::Filter filter;
  bp::BackupOptions options;
  return bp::RunIncrementalBackup(
      source, repository, name, "repo-identity", filter, options,
      std::vector<std::string>(), std::vector<std::string>(), "", outcome,
      error);
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


  // ---- R-01：链上任何一个 delta 失败都必须让整次恢复失败 ----
  //
  // 旧控制流里，ReadDeltaEnvelope / ExtractDeltaPayload / delta 的
  // RunRestorePipeline / MergeTree 这四个失败分支只 break 了内层 for，没有把
  // 失败写进 delta_failed。for 结束后 delta_failed 仍是 false，代码继续走到
  // 发布：返回 true，destination 里出现一棵"没有应用完增量"的树。
  //
  // 每个用例都先把**指定的那一步**钉成确定性失败，再要求：返回 false、
  // destination 一个字节都没发布、临时路径按原契约清理。失败的具体位置由
  // error_message 里的路径断言兜住，证明打的确实是目标分支。
  test_support::Section("INC-R 10. R-01：delta 失败不得发布半成品");
  {
    // 10.1 成功路径的对照：多 delta 链上每一个 delta 的结果都要验证。
    const std::string work = test_support::FreshDir("inc-r01-multi");
    const std::string source = work + "/src";
    const std::string repository = work + "/repo";
    const std::string oracle_repo = work + "/oracle";
    test_support::Mkdir(source, 0755);
    test_support::Mkdir(repository, 0755);
    test_support::Mkdir(oracle_repo, 0755);
    bp::IncrementalOutcome outcome;
    std::string error;

    test_support::WriteFile(source + "/keep.txt", "keep-v1", 0644);
    test_support::WriteFile(source + "/first.txt", "first-v1", 0644);
    test_support::Check(
        MakeSnapshot(source, repository, "s1.bak", &outcome, &error) &&
            outcome.kind == bp::IncrementalOutcome::Kind::kFullBaseline,
        "INC-R T10 基线建立成功", error);
    test_support::WriteFile(source + "/first.txt", "first-v2", 0644);
    test_support::Check(
        MakeSnapshot(source, repository, "s2.bak", &outcome, &error) &&
            outcome.kind == bp::IncrementalOutcome::Kind::kDelta,
        "INC-R T10 第一个 delta 建立成功", error);
    test_support::WriteFile(source + "/second.txt", "second-v1", 0644);
    test_support::Check(
        MakeSnapshot(source, repository, "s3.bak", &outcome, &error) &&
            outcome.kind == bp::IncrementalOutcome::Kind::kDelta,
        "INC-R T10 第二个 delta 建立成功", error);

    const std::string restored = work + "/restored";
    bp::RestoreReport report;
    error.clear();
    test_support::Check(
        bp::RestoreSnapshotChain(repository, "s3.bak", restored,
                                 bp::RestoreOptions{}, &report, &error),
        "INC-R T10 三份快照的链恢复成功", error);
    std::string content;
    test_support::Check(
        test_support::ReadFile(restored + "/first.txt", &content) &&
            content == "first-v2",
        "INC-R T10 第一个 delta 的结果也在（不是只看最后一个）", content);
    test_support::Check(
        test_support::ReadFile(restored + "/second.txt", &content) &&
            content == "second-v1",
        "INC-R T10 第二个 delta 的结果也在", content);
    test_support::Check(
        test_support::ReadFile(restored + "/keep.txt", &content) &&
            content == "keep-v1",
        "INC-R T10 没被任何 delta 触碰的文件原样保留", content);
    bp::Filter filter;
    std::string oracle;
    test_support::Check(
        MakeOracle(source, oracle_repo, "multi", filter, &oracle, &error),
        "INC-R T10 oracle 完整备份成功", error);
    std::string detail;
    test_support::Check(test_support::CompareTrees(oracle, restored, &detail),
                        "INC-R T10 链恢复 == 完整恢复（逐节点）", detail);
  }
  {
    // 10.2 目标分支：**最后一个 delta** 的 MergeTree 失败（合并目标不可写）。
    const std::string work = test_support::FreshDir("inc-r01-last");
    const std::string source = work + "/src";
    const std::string repository = work + "/repo";
    test_support::Mkdir(source, 0755);
    test_support::Mkdir(repository, 0755);
    bp::IncrementalOutcome outcome;
    std::string error;

    test_support::WriteFile(source + "/a.txt", "a-v1", 0644);
    test_support::Mkdir(source + "/ro", 0755);
    test_support::WriteFile(source + "/ro/keep.txt", "keep", 0644);
    test_support::Check(::chmod((source + "/ro").c_str(), 0500) == 0,
                        "INC-R T11 源树里的 ro 置成 0500");
    test_support::Check(
        MakeSnapshot(source, repository, "s1.bak", &outcome, &error) &&
            outcome.kind == bp::IncrementalOutcome::Kind::kFullBaseline,
        "INC-R T11 基线建立成功（带上只读目录的 mode）", error);
    // 第一个 delta 只改 a.txt：ro 的 mode 在 s1 / s2 两处都是 0500，
    // staging 里的 ro 因此一直是只读的——最后一个 delta 往它里面写新文件时
    // 才必然失败（这正是要注入的那一步）。
    test_support::WriteFile(source + "/a.txt", "a-v2", 0644);
    test_support::Check(
        MakeSnapshot(source, repository, "s2.bak", &outcome, &error) &&
            outcome.kind == bp::IncrementalOutcome::Kind::kDelta,
        "INC-R T11 第一个 delta 建立成功", error);
    test_support::Check(::chmod((source + "/ro").c_str(), 0755) == 0 &&
                            test_support::WriteFile(source + "/ro/new.txt",
                                                    "new", 0644) &&
                            ::chmod((source + "/ro").c_str(), 0500) == 0,
                        "INC-R T11 只读目录里加上 ro/new.txt 后收回 0500");
    test_support::Check(
        MakeSnapshot(source, repository, "s3.bak", &outcome, &error) &&
            outcome.kind == bp::IncrementalOutcome::Kind::kDelta,
        "INC-R T11 最后一个 delta 建立成功（新增 ro/new.txt）", error);

    // 前提校验：恢复出来的 ro 确实带着 0500，合并才必然失败。
    const std::string probe = work + "/probe";
    bp::RestoreReport probe_report;
    error.clear();
    test_support::Check(
        bp::RestoreSnapshotChain(repository, "s2.bak", probe,
                                 bp::RestoreOptions{}, &probe_report, &error),
        "INC-R T11 探针：s2 的链恢复成功", error);
    struct stat ro_info;
    test_support::Check(
        test_support::StatOf(probe + "/ro", &ro_info) &&
            (ro_info.st_mode & 07777) == 0500,
        "INC-R T11 探针：恢复出来的 ro 是 0500（注入前提成立）");

    const std::string destination = work + "/restored";
    bp::RestoreReport report;
    error.clear();
    const bool ok = bp::RestoreSnapshotChain(repository, "s3.bak", destination,
                                             bp::RestoreOptions{}, &report,
                                             &error);
    test_support::Check(!ok, "INC-R T11 判别：最后一个 delta 合并失败即整次失败",
                        error);
    test_support::Check(!test_support::Exists(destination),
                        "INC-R T11 判别：失败时 destination 一个字节都没发布");
    test_support::Check(error.find("ro/new.txt") != std::string::npos,
                        "INC-R T11 失败出在合并 ro/new.txt 这一步", error);
    test_support::Check(!test_support::Exists(TempPath(destination, ".container")),
                        "INC-R T11 失败后中间容器文件清理掉了");
    // R-01 残留修复（本轮）：staging 里那棵只读子树以前会让"尽力而为"的清理
    // 留下一整棵删不掉的中间目录，而且跨 pid 累积。修复后失败路径也必须把
    // 自己的 staging / overlay 收干净。
    test_support::Check(!test_support::Exists(TempPath(destination, ".staging")),
                        "INC-R T11 失败后 staging 不留残留");
    test_support::Check(!test_support::Exists(TempPath(destination, ".overlay")),
                        "INC-R T11 失败后 overlay 不留残留");
    // 无论上面是红是绿，都把注入树收掉，避免它污染后续用例。
    DropTreeForcingModes(TempPath(destination, ".staging"));
    DropTreeForcingModes(TempPath(destination, ".overlay"));
  }
  {
    // 10.3 目标分支：**中间 delta** 的 payload 提取失败（同名残留挡路，
    // ExtractDeltaPayload 用 O_CREAT|O_EXCL 打开容器文件）。
    const std::string work = test_support::FreshDir("inc-r01-extract");
    const std::string source = work + "/src";
    const std::string repository = work + "/repo";
    test_support::Mkdir(source, 0755);
    test_support::Mkdir(repository, 0755);
    bp::IncrementalOutcome outcome;
    std::string error;

    test_support::WriteFile(source + "/a.txt", "a-v1", 0644);
    test_support::Check(
        MakeSnapshot(source, repository, "s1.bak", &outcome, &error),
        "INC-R T12 基线建立成功", error);
    test_support::WriteFile(source + "/a.txt", "a-v2", 0644);
    test_support::Check(
        MakeSnapshot(source, repository, "s2.bak", &outcome, &error),
        "INC-R T12 中间 delta 建立成功", error);
    test_support::WriteFile(source + "/b.txt", "b-v1", 0644);
    test_support::Check(
        MakeSnapshot(source, repository, "s3.bak", &outcome, &error),
        "INC-R T12 最后一个 delta 建立成功", error);

    const std::string destination = work + "/restored";
    test_support::Check(
        ::mkdir(TempPath(destination, ".container").c_str(), 0755) == 0,
        "INC-R T12 预置挡路的 container 残留");
    bp::RestoreReport report;
    error.clear();
    const bool ok = bp::RestoreSnapshotChain(repository, "s3.bak", destination,
                                             bp::RestoreOptions{}, &report,
                                             &error);
    test_support::Check(!ok, "INC-R T12 判别：中间 delta 提取失败即整次失败",
                        error);
    test_support::Check(!test_support::Exists(destination),
                        "INC-R T12 判别：失败时不发布（不建目录、不写半棵树）");
    test_support::Check(error.find(".container") != std::string::npos,
                        "INC-R T12 失败出在 container 那一步", error);
    test_support::Check(
        !test_support::Exists(TempPath(destination, ".staging")) &&
            !test_support::Exists(TempPath(destination, ".overlay")),
        "INC-R T12 失败后 staging / overlay 都清理掉了");
    test_support::RemoveTree(TempPath(destination, ".container"));
  }
  {
    // 10.4 目标分支：delta 容器恢复到 overlay 时失败。
    //
    // 注入方式（本轮修正）：以前这里是"预置一个 0500 的 overlay 残留让清理
    // 失败"——那是**利用清理缺陷**做故障注入。本轮把该缺陷修好之后这个注入
    // 不再成立（残留会被正确回收），于是改成直接损坏 delta 的 payload：
    // 信封与长度仍然自洽，只有内层 container 变成垃圾，失败点依旧精确落在
    // "container -> overlay"这一步，覆盖的分支没有变。
    const std::string work = test_support::FreshDir("inc-r01-container");
    const std::string source = work + "/src";
    const std::string repository = work + "/repo";
    test_support::Mkdir(source, 0755);
    test_support::Mkdir(repository, 0755);
    bp::IncrementalOutcome outcome;
    std::string error;

    test_support::WriteFile(source + "/a.txt", "a-v1", 0644);
    test_support::Check(
        MakeSnapshot(source, repository, "s1.bak", &outcome, &error),
        "INC-R T13 基线建立成功", error);
    test_support::WriteFile(source + "/a.txt", "a-v2", 0644);
    test_support::Check(
        MakeSnapshot(source, repository, "s2.bak", &outcome, &error) &&
            outcome.kind == bp::IncrementalOutcome::Kind::kDelta,
        "INC-R T13 delta 建立成功", error);

    const std::string destination = work + "/restored";
    test_support::Check(CorruptDeltaPayload(repository + "/s2.bak"),
                        "INC-R T13 把 delta 的 payload 改成垃圾（长度不变）");
    bp::RestoreReport report;
    error.clear();
    const bool ok = bp::RestoreSnapshotChain(repository, "s2.bak", destination,
                                             bp::RestoreOptions{}, &report,
                                             &error);
    test_support::Check(!ok, "INC-R T13 判别：容器恢复失败即整次失败", error);
    test_support::Check(!test_support::Exists(destination),
                        "INC-R T13 判别：失败时不发布");
    test_support::Check(!error.empty(), "INC-R T13 失败带诊断信息", error);
    test_support::Check(!test_support::Exists(TempPath(destination, ".staging")),
                        "INC-R T13 失败后 staging 清理掉了");
    test_support::Check(!test_support::Exists(TempPath(destination, ".overlay")),
                        "INC-R T13 失败后 overlay 清理掉了");
    test_support::Check(
        !test_support::Exists(TempPath(destination, ".container")),
        "INC-R T13 失败后 container 清理掉了");
  }
  {
    // 10.5 既有分支回归：tombstone 操作失败必须整次失败（这条以前就是对的，
    // 修复不能把它弄坏）。
    const std::string work = test_support::FreshDir("inc-r01-tombstone");
    const std::string source = work + "/src";
    const std::string repository = work + "/repo";
    test_support::Mkdir(source, 0755);
    test_support::Mkdir(repository, 0755);
    bp::IncrementalOutcome outcome;
    std::string error;

    test_support::WriteFile(source + "/a.txt", "a-v1", 0644);
    test_support::Mkdir(source + "/ro", 0755);
    test_support::WriteFile(source + "/ro/keep.txt", "keep", 0644);
    test_support::Check(::chmod((source + "/ro").c_str(), 0500) == 0,
                        "INC-R T14 源树里的 ro 置成 0500");
    test_support::Check(
        MakeSnapshot(source, repository, "s1.bak", &outcome, &error),
        "INC-R T14 基线建立成功", error);
    test_support::Check(::chmod((source + "/ro").c_str(), 0755) == 0 &&
                            ::unlink((source + "/ro/keep.txt").c_str()) == 0 &&
                            ::chmod((source + "/ro").c_str(), 0500) == 0,
                        "INC-R T14 删掉 ro/keep.txt（delta 会带上 tombstone）");
    test_support::Check(
        MakeSnapshot(source, repository, "s2.bak", &outcome, &error) &&
            outcome.kind == bp::IncrementalOutcome::Kind::kDelta,
        "INC-R T14 带 tombstone 的 delta 建立成功", error);

    const std::string destination = work + "/restored";
    bp::RestoreReport report;
    error.clear();
    const bool ok = bp::RestoreSnapshotChain(repository, "s2.bak", destination,
                                             bp::RestoreOptions{}, &report,
                                             &error);
    test_support::Check(!ok, "INC-R T14 判别：tombstone 删不掉即整次失败", error);
    test_support::Check(!test_support::Exists(destination),
                        "INC-R T14 判别：失败时不发布");
    test_support::Check(error.find("tombstone") != std::string::npos,
                        "INC-R T14 失败出在 tombstone 那一步", error);
  }
  {
    // 10.6 error_message 是可选诊断出参：传 nullptr 时失败控制流必须照样正确；
    // 已存在的**空** destination 不能被当成"发布成功"；失败不污染后续恢复。
    const std::string work = test_support::FreshDir("inc-r01-null");
    const std::string source = work + "/src";
    const std::string repository = work + "/repo";
    test_support::Mkdir(source, 0755);
    test_support::Mkdir(repository, 0755);
    bp::IncrementalOutcome outcome;
    std::string error;

    test_support::WriteFile(source + "/a.txt", "a-v1", 0644);
    test_support::Check(
        MakeSnapshot(source, repository, "s1.bak", &outcome, &error),
        "INC-R T15 基线建立成功", error);
    test_support::WriteFile(source + "/a.txt", "a-v2", 0644);
    test_support::Check(
        MakeSnapshot(source, repository, "s2.bak", &outcome, &error),
        "INC-R T15 delta 建立成功", error);

    // (a) nullptr：同一个失败注入，诊断通道关掉。
    const std::string dest_null = work + "/restored-null";
    test_support::Check(
        ::mkdir(TempPath(dest_null, ".container").c_str(), 0755) == 0,
        "INC-R T15 预置挡路的 container 残留");
    bp::RestoreReport null_report;
    const bool null_ok = bp::RestoreSnapshotChain(
        repository, "s2.bak", dest_null, bp::RestoreOptions{}, &null_report,
        nullptr);
    test_support::Check(!null_ok,
                        "INC-R T15 判别：error_message=nullptr 时失败仍然返回 "
                        "false");
    test_support::Check(!test_support::Exists(dest_null),
                        "INC-R T15 判别：error_message=nullptr 时也不发布");
    test_support::RemoveTree(TempPath(dest_null, ".container"));

    // (b) destination 预先存在但是空的：失败之后必须还是那个空目录。
    const std::string dest_empty = work + "/restored-empty";
    test_support::Check(test_support::Mkdir(dest_empty, 0755),
                        "INC-R T15 预置空 destination");
    test_support::Check(
        ::mkdir(TempPath(dest_empty, ".container").c_str(), 0755) == 0,
        "INC-R T15 预置挡路的 container 残留（空 destination 版）");
    bp::RestoreReport empty_report;
    error.clear();
    const bool empty_ok = bp::RestoreSnapshotChain(
        repository, "s2.bak", dest_empty, bp::RestoreOptions{}, &empty_report,
        &error);
    test_support::Check(!empty_ok, "INC-R T15 判别：空 destination 版也失败",
                        error);
    test_support::Check(test_support::Exists(dest_empty) &&
                            test_support::DirEntries(dest_empty).empty(),
                        "INC-R T15 判别：原本存在的空 destination 没有被发布成半成品");
    test_support::RemoveTree(TempPath(dest_empty, ".container"));

    // (c) 失败之后同一个仓库再做一次有效恢复：必须完全正常。
    const std::string dest_ok = work + "/restored-after-failure";
    bp::RestoreReport ok_report;
    error.clear();
    test_support::Check(
        bp::RestoreSnapshotChain(repository, "s2.bak", dest_ok,
                                 bp::RestoreOptions{}, &ok_report, &error),
        "INC-R T15 失败之后再次执行有效恢复成功", error);
    std::string content;
    test_support::Check(
        test_support::ReadFile(dest_ok + "/a.txt", &content) &&
            content == "a-v2",
        "INC-R T15 再次恢复的结果正确", content);
  }
  {
    // 10.7 对照：**基座**恢复失败走的本来就是 do-while 的 break（这条一直
    // 是对的，用它说明"不是所有失败都被 for 吞掉"）。
    const std::string work = test_support::FreshDir("inc-r01-base");
    const std::string source = work + "/src";
    const std::string repository = work + "/repo";
    test_support::Mkdir(source, 0755);
    test_support::Mkdir(repository, 0755);
    bp::IncrementalOutcome outcome;
    std::string error;

    test_support::WriteFile(source + "/a.txt", "a-v1", 0644);
    test_support::Check(
        MakeSnapshot(source, repository, "s1.bak", &outcome, &error),
        "INC-R T16 基线建立成功", error);
    test_support::WriteFile(source + "/a.txt", "a-v2", 0644);
    test_support::Check(
        MakeSnapshot(source, repository, "s2.bak", &outcome, &error),
        "INC-R T16 delta 建立成功", error);

    const std::string destination = work + "/restored";
    // 注入方式（本轮修正）：以前是"预置一个清理不掉的 staging 残留"——同样是
    // 利用清理缺陷。改成把**工作目录**设成 0500：base 那一步要在它下面建
    // staging，mkdir 必然 EACCES，失败点仍然精确落在基座恢复这一步。
    test_support::Check(::chmod(work.c_str(), 0500) == 0,
                        "INC-R T16 把工作目录设成 0500");
    bp::RestoreReport report;
    error.clear();
    const bool ok = bp::RestoreSnapshotChain(repository, "s2.bak", destination,
                                             bp::RestoreOptions{}, &report,
                                             &error);
    ::chmod(work.c_str(), 0755);
    test_support::Check(!ok, "INC-R T16 判别：基座恢复失败即整次失败", error);
    test_support::Check(!test_support::Exists(destination),
                        "INC-R T16 判别：失败时不发布");
    test_support::Check(!error.empty(), "INC-R T16 失败带诊断信息", error);

    // 反向对照（残留修复的直接回归）：同 pid 的 0500 残留是"上一次同 pid 的
    // 进程崩在中途"留下的，属于本进程自己的临时数据，**必须被强制清理**，
    // 恢复要照常成功；修复之前这里会以"not empty"失败。
    test_support::Check(
        MakeStubbornResidue(TempPath(destination, ".staging")),
        "INC-R T16 预置同 pid 的 0500 staging 残留");
    error.clear();
    const bool ok_after = bp::RestoreSnapshotChain(
        repository, "s2.bak", destination, bp::RestoreOptions{}, &report, &error);
    test_support::Check(ok_after,
                        "INC-R T16 同 pid 残留被强制清理后恢复成功", error);
    test_support::Check(test_support::Exists(destination + "/a.txt"),
                        "INC-R T16 恢复结果正确（a.txt 内容为 a-v2）");
    test_support::Check(!test_support::Exists(TempPath(destination, ".staging")),
                        "INC-R T16 成功后不留 staging 残留");
    DropStubbornResidue(TempPath(destination, ".staging"));
  }

  {
    // 10.8 R-01 临时文件生命周期：跨进程（跨 pid）回收 + 严格边界。
    //
    // 产品只回收"名字精确等于 <destination 基名>.<十进制 pid>.<kind>、与
    // destination 同父目录、属主是当前 euid、且该 pid 已经不存在"的条目；
    // 其余（活 pid、名字不匹配、别人的 destination、别人的文件）一律不动。
    const std::string work = test_support::FreshDir("inc-r01-residue");
    const std::string source = work + "/src";
    const std::string repository = work + "/repo";
    test_support::Mkdir(source, 0755);
    test_support::Mkdir(repository, 0755);
    bp::IncrementalOutcome outcome;
    std::string error;

    test_support::WriteFile(source + "/a.txt", "a-v1", 0644);
    test_support::Check(
        MakeSnapshot(source, repository, "s1.bak", &outcome, &error),
        "INC-R T17 基线建立成功", error);

    const std::string destination = work + "/restored";

    // 一个**真的已经死掉**的 pid：fork 后立刻退出并回收。
    const pid_t dead_pid = ::fork();
    if (dead_pid == 0) {
      ::_exit(0);
    }
    int child_status = 0;
    if (dead_pid > 0) {
      ::waitpid(dead_pid, &child_status, 0);
    }
    // 一个**活着**的 pid：子进程睡着，代表"另一个进程正在用的中间数据"。
    const pid_t live_pid = ::fork();
    if (live_pid == 0) {
      ::sleep(60);
      ::_exit(0);
    }
    // 第二个死 pid：专门用来构造"同名前缀但是软链接"的残留。
    const pid_t dead_pid2 = ::fork();
    if (dead_pid2 == 0) {
      ::_exit(0);
    }
    if (dead_pid2 > 0) {
      ::waitpid(dead_pid2, &child_status, 0);
    }
    // 第三个死 pid：构造"名字合规但**完全没有标记**"的用户自建目录。
    // 注意所有权标记是 **per-pid** 的（见 TempOwnerMarkerPath），所以不能与
    // 已经有标记的 pid 复用，否则测的就不是"没有标记"这件事了。
    const pid_t dead_pid3 = ::fork();
    if (dead_pid3 == 0) {
      ::_exit(0);
    }
    if (dead_pid3 > 0) {
      ::waitpid(dead_pid3, &child_status, 0);
    }
    test_support::Check(dead_pid > 0 && live_pid > 0 && dead_pid2 > 0 &&
                            dead_pid3 > 0,
                        "INC-R T17 fork 出构造用的死/活 pid");

    if (dead_pid > 0 && live_pid > 0) {
      const std::string dead_suffix =
          "." + std::to_string(static_cast<long>(dead_pid));
      const std::string live_suffix =
          "." + std::to_string(static_cast<long>(live_pid));

      // 1) 死 pid 的 staging 目录 + container 文件：应当被回收。
      const std::string stale_staging = destination + dead_suffix + ".staging";
      const std::string stale_overlay = destination + dead_suffix + ".overlay";
      const std::string stale_container = destination + dead_suffix + ".container";
      test_support::Mkdir(stale_staging, 0755);
      test_support::WriteFile(stale_staging + "/left.txt", "left", 0644);
      test_support::Mkdir(stale_overlay, 0755);
      test_support::WriteFile(stale_overlay + "/left.txt", "left", 0644);
      test_support::WriteFile(stale_container, "container", 0644);
      // 只读子树：以前正是它让"尽力而为"的清理整棵失败。
      test_support::Mkdir(stale_staging + "/ro", 0500);
      // 来源证明：这些条目要能被回收，必须有内容正确的所有权标记。
      test_support::Check(
          test_support::WriteFile(
              TempOwnerMarkerPath(destination, static_cast<long>(dead_pid)),
              TempOwnerMarkerText(static_cast<long>(dead_pid)), 0600),
          "INC-R T17 为死 pid 的条目写下所有权标记");

      // 2) 活 pid 的 staging：必须原样保留（那是别人正在用的）。
      const std::string live_staging = destination + live_suffix + ".staging";
      test_support::Mkdir(live_staging, 0755);
      test_support::WriteFile(live_staging + "/inuse.txt", "inuse", 0644);

      // 3) 名字不匹配 / 属于别的 destination 的同前缀兄弟：一个都不能动。
      const std::string not_a_number = destination + ".notanumber.staging";
      const std::string wrong_kind = destination + dead_suffix + ".staging.bak";
      const std::string other_destination = work + "/restored-other";
      const std::string other_stale = other_destination + dead_suffix + ".staging";
      test_support::Mkdir(not_a_number, 0755);
      test_support::Mkdir(wrong_kind, 0755);
      test_support::Mkdir(other_stale, 0755);
      test_support::WriteFile(other_stale + "/keep.txt", "keep", 0644);

      // 4) 软链接形态的"同名前缀"：绝不 follow，只允许摘掉链接本身。
      const std::string precious = work + "/precious";
      test_support::Mkdir(precious, 0755);
      test_support::WriteFile(precious + "/treasure.txt", "treasure", 0644);
      // 名字**完全符合**回收候选规则（合法 kind 后缀 + 死 pid），但它是软链接：
      // 只允许摘掉链接本身，绝不允许 follow 进去删目标。
      const std::string dead_suffix2 =
          "." + std::to_string(static_cast<long>(dead_pid2));
      const std::string symlink_path = destination + dead_suffix2 + ".overlay";
      test_support::Check(::symlink(precious.c_str(), symlink_path.c_str()) == 0,
                          "INC-R T17 造出指回 precious 的软链接残留");
      test_support::Check(
          test_support::WriteFile(
              TempOwnerMarkerPath(destination, static_cast<long>(dead_pid2)),
              TempOwnerMarkerText(static_cast<long>(dead_pid2)), 0600),
          "INC-R T17 为软链接条目写下所有权标记");
      // 安全对照：名字完全合规、但**没有标记**（用户自建）的目录必须原样保留。
      const std::string dead_suffix3 =
          "." + std::to_string(static_cast<long>(dead_pid3));
      const std::string unmarked = destination + dead_suffix3 + ".staging";
      test_support::Mkdir(unmarked, 0755);
      test_support::WriteFile(unmarked + "/user-file.txt", "user", 0644);

      bp::RestoreReport report;
      error.clear();
      const bool ok = bp::RestoreSnapshotChain(
          repository, "s1.bak", destination, bp::RestoreOptions{}, &report,
          &error);
      test_support::Check(ok, "INC-R T17 正常恢复成功（回收不影响发布）", error);
      test_support::Check(test_support::Exists(destination + "/a.txt"),
                          "INC-R T17 恢复结果正确");

      test_support::Check(!test_support::Exists(stale_staging),
                          "INC-R T17 死 pid 的 staging 残留被回收（含只读子树）");
      test_support::Check(!test_support::Exists(stale_overlay),
                          "INC-R T17 死 pid 的 overlay 残留被回收");
      test_support::Check(!test_support::Exists(stale_container),
                          "INC-R T17 死 pid 的 container 残留被回收");
      test_support::Check(test_support::Exists(live_staging + "/inuse.txt"),
                          "INC-R T17 活 pid 的中间数据未被删除");
      test_support::Check(test_support::Exists(not_a_number),
                          "INC-R T17 名字不是 pid 的同前缀目录未被删除");
      test_support::Check(test_support::Exists(wrong_kind),
                          "INC-R T17 kind 后缀不匹配的目录未被删除");
      test_support::Check(test_support::Exists(other_stale + "/keep.txt"),
                          "INC-R T17 别的 destination 的残留未被删除");
      test_support::Check(test_support::Exists(precious + "/treasure.txt"),
                          "INC-R T17 软链接残留没有被 follow（目标完好）");
      struct stat link_info;
      test_support::Check(::lstat(symlink_path.c_str(), &link_info) != 0,
                          "INC-R T17 软链接残留本身被摘掉（unlink 不 follow）");
      test_support::Check(
          test_support::Exists(unmarked + "/user-file.txt"),
          "INC-R T17 无标记的同名用户目录原样保留（来源证明生效）");
      test_support::Check(
          !test_support::Exists(
              TempOwnerMarkerPath(destination, static_cast<long>(dead_pid))),
          "INC-R T17 回收后死 pid 的所有权标记被清掉");

      // 收尾：把活着的子进程和注入树收掉。
      ::kill(live_pid, SIGTERM);
      ::waitpid(live_pid, &child_status, 0);
      DropTreeForcingModes(live_staging);
      DropTreeForcingModes(not_a_number);
      DropTreeForcingModes(wrong_kind);
      DropTreeForcingModes(other_stale);
      DropTreeForcingModes(symlink_path);
      DropTreeForcingModes(unmarked);
    }
  }

  {
    // 10.9 临时条目回收的**来源证明**与路径安全（本轮 P0 修复的回归）。
    //
    // 契约：只有同时满足"名字规则 + 所有权标记内容匹配 + 属主是自己 + pid 已死"
    // 的条目才会被回收；**没有标记的一律不碰**。这直接对应"同一 UID 的用户自建
    // 同名目录不能被删"这条安全要求。
    const std::string work = test_support::FreshDir("inc-tmp-provenance");
    const std::string source = work + "/src";
    const std::string repository = work + "/repo";
    test_support::Mkdir(source, 0755);
    test_support::Mkdir(repository, 0755);
    bp::IncrementalOutcome outcome;
    std::string error;

    test_support::WriteFile(source + "/a.txt", "a-v1", 0644);
    test_support::Check(
        MakeSnapshot(source, repository, "s1.bak", &outcome, &error),
        "INC-R T18 基线建立成功", error);

    const std::string destination = work + "/restored";

    // 三个**真的已经死掉**的 pid（fork 后立刻退出并回收）。
    pid_t dead[3] = {0, 0, 0};
    int child_status = 0;
    for (int index = 0; index < 3; ++index) {
      dead[index] = ::fork();
      if (dead[index] == 0) {
        ::_exit(0);
      }
      if (dead[index] > 0) {
        ::waitpid(dead[index], &child_status, 0);
      }
    }
    test_support::Check(dead[0] > 0 && dead[1] > 0 && dead[2] > 0,
                        "INC-R T18 fork 出三个死 pid");

    const long pid0 = static_cast<long>(dead[0]);
    const long pid1 = static_cast<long>(dead[1]);
    const long pid2 = static_cast<long>(dead[2]);

    // (1) 同 UID 用户自建、名字**完全符合**回收规则的普通目录，里面放重要文件。
    //     它的"标记"是伪造的（内容不对），因此必须原样保留。
    const std::string forged = destination + "." + std::to_string(pid0) + ".staging";
    test_support::Mkdir(forged, 0755);
    test_support::WriteFile(forged + "/precious.txt", "user-data", 0644);
    test_support::WriteFile(TempOwnerMarkerPath(destination, pid0),
                            "THIS-IS-NOT-OUR-MAGIC\n" + std::to_string(pid0) +
                                "\n",
                            0600);

    // (2) 真正由本软件留下的旧残留：目录 + 内容正确的标记 → 应当被回收。
    const std::string real = destination + "." + std::to_string(pid1) + ".staging";
    test_support::Mkdir(real, 0755);
    test_support::WriteFile(real + "/left.txt", "left", 0644);
    test_support::Mkdir(real + "/ro", 0500);  // 只读子树：清理仍须成功
    test_support::WriteFile(TempOwnerMarkerPath(destination, pid1),
                            TempOwnerMarkerText(pid1), 0600);

    // (3) 名字与标记都合规，但条目本身是**指向受保护目录的软链接**：
    //     只允许摘掉链接，绝不允许 follow 进目标。
    const std::string precious_dir = work + "/precious-dir";
    test_support::Mkdir(precious_dir, 0755);
    test_support::WriteFile(precious_dir + "/treasure.txt", "treasure", 0644);
    const std::string link_entry =
        destination + "." + std::to_string(pid2) + ".overlay";
    test_support::Check(::symlink(precious_dir.c_str(), link_entry.c_str()) == 0,
                        "INC-R T18 造出合规名字的软链接条目");
    test_support::WriteFile(TempOwnerMarkerPath(destination, pid2),
                            TempOwnerMarkerText(pid2), 0600);

    bp::RestoreReport report;
    error.clear();
    const bool ok = bp::RestoreSnapshotChain(repository, "s1.bak", destination,
                                             bp::RestoreOptions{}, &report,
                                             &error);
    test_support::Check(ok, "INC-R T18 正常恢复成功（回收不影响发布）", error);
    std::string restored;
    test_support::Check(
        test_support::ReadFile(destination + "/a.txt", &restored) &&
            restored == "a-v1",
        "INC-R T18 恢复内容正确", restored);

    // 核心安全断言：没有来源证明的用户目录**一个字节都不能少**。
    test_support::Check(test_support::Exists(forged),
                        "INC-R T18 无标记的同名用户目录未被删除");
    test_support::Check(test_support::Exists(forged + "/precious.txt"),
                        "INC-R T18 无标记目录里的受保护文件未被删除");
    test_support::Check(test_support::Exists(TempOwnerMarkerPath(destination, pid0)),
                        "INC-R T18 伪造的标记文件未被删除");

    // 有来源证明的旧残留应当被回收（含 0500 只读子树）。
    test_support::Check(!test_support::Exists(real),
                        "INC-R T18 有标记的旧 staging 残留被回收（含只读子树）");
    test_support::Check(!test_support::Exists(TempOwnerMarkerPath(destination, pid1)),
                        "INC-R T18 有标记的旧残留其标记也一并清掉");

    // 软链接：目标必须完好，链接条目本身被摘掉。
    test_support::Check(test_support::Exists(precious_dir + "/treasure.txt"),
                        "INC-R T18 合规名字的软链接未被 follow（目标完好）");
    struct stat link_info;
    test_support::Check(::lstat(link_entry.c_str(), &link_info) != 0,
                        "INC-R T18 合规名字的软链接条目本身被摘掉");
    test_support::Check(!test_support::Exists(TempOwnerMarkerPath(destination, pid2)),
                        "INC-R T18 软链接条目的标记也已清掉");

    // 本轮自己的标记：正常结束后不应残留。
    const std::string self_pid_text =
        std::to_string(static_cast<long>(::getpid()));
    test_support::Check(
        !test_support::Exists(destination + "." + self_pid_text + ".owner"),
        "INC-R T18 本轮自己的所有权标记已撤掉");

    DropTreeForcingModes(forged);
    DropTreeForcingModes(TempOwnerMarkerPath(destination, pid0));
    DropTreeForcingModes(real);
  }
  {
    // 10.10 destination 词法规范化：尾斜杠不得再破坏恢复（本轮 P1 修复的回归）。
    const std::string work = test_support::FreshDir("inc-trailing-slash");
    const std::string source = work + "/src";
    const std::string repository = work + "/repo";
    test_support::Mkdir(source, 0755);
    test_support::Mkdir(repository, 0755);
    bp::IncrementalOutcome outcome;
    std::string error;
    test_support::WriteFile(source + "/a.txt", "slash-v1", 0644);
    test_support::Check(
        MakeSnapshot(source, repository, "s1.bak", &outcome, &error),
        "INC-R T19 基线建立成功", error);
    bp::RestoreReport report;

    const std::string with_slash = work + "/dest-one";
    test_support::Mkdir(with_slash, 0755);
    error.clear();
    test_support::Check(
        bp::RestoreSnapshotChain(repository, "s1.bak", with_slash + "/",
                                 bp::RestoreOptions{}, &report, &error),
        "INC-R T19 单尾斜杠（已存在空目录）恢复成功", error);
    std::string got;
    test_support::Check(
        test_support::ReadFile(with_slash + "/a.txt", &got) && got == "slash-v1",
        "INC-R T19 单尾斜杠恢复内容正确", got);

    const std::string many_slash = work + "/dest-many";
    error.clear();
    test_support::Check(
        bp::RestoreSnapshotChain(repository, "s1.bak", many_slash + "///",
                                 bp::RestoreOptions{}, &report, &error),
        "INC-R T19 多尾斜杠（目标不存在）恢复成功", error);
    test_support::Check(
        test_support::ReadFile(many_slash + "/a.txt", &got) && got == "slash-v1",
        "INC-R T19 多尾斜杠恢复内容正确", got);

    const std::string plain = work + "/dest-plain";
    error.clear();
    test_support::Check(
        bp::RestoreSnapshotChain(repository, "s1.bak", plain,
                                 bp::RestoreOptions{}, &report, &error),
        "INC-R T19 无尾斜杠恢复正常", error);
    test_support::Check(
        test_support::ReadFile(plain + "/a.txt", &got) && got == "slash-v1",
        "INC-R T19 无尾斜杠内容正确", got);

    error.clear();
    test_support::Check(
        !bp::RestoreSnapshotChain(repository, "s1.bak", with_slash,
                                  bp::RestoreOptions{}, &report, &error),
        "INC-R T19 非空目标被拒绝", error);
    test_support::Check(
        test_support::ReadFile(with_slash + "/a.txt", &got) && got == "slash-v1",
        "INC-R T19 非空目标内容未被覆盖", got);

    error.clear();
    test_support::Check(
        !bp::RestoreSnapshotChain(repository, "s1.bak", "/",
                                  bp::RestoreOptions{}, &report, &error),
        "INC-R T19 根目录被安全拒绝", error);
    test_support::Check(test_support::Exists("/tmp"),
                        "INC-R T19 根目录尝试后 /tmp 仍然存在");

    const std::string self_pid_text =
        std::to_string(static_cast<long>(::getpid()));
    test_support::Check(
        !test_support::Exists(work + "/dest-one." + self_pid_text + ".staging"),
        "INC-R T19 尾斜杠恢复后不留 staging 残留");
    test_support::Check(
        !test_support::Exists(work + "/dest-one." + self_pid_text + ".owner"),
        "INC-R T19 尾斜杠恢复后不留所有权标记");
  }

  return test_support::Finish("incremental_restore_test");
}
