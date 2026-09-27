// incremental_retention_test.cpp
//
// PR #18：依赖感知 retention 的专项测试。
//
// 要钉住的核心事实只有一条：
//
//     被"要保留的 restore point"依赖的祖先，永远不会进入删除集合。
//
// 对 Full 来说"删最旧的"是安全的，所以这条性质在只有完整快照时退回原行为；
// 有链之后它是数据安全问题：删掉 F0/Δ1 会让 Δ2、Δ3 全部不可恢复，而列表上
// 看起来只是"少了两份旧快照"。

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "archive_pipeline.h"
#include "incremental_backup.h"
#include "incremental_delta.h"
#include "source_digest.h"
#include "test_support.h"

namespace bp = backupproject;

namespace {

bp::ArchiveEntry RootEntry(const std::string& source_path,
                           const std::string& archive_path) {
  bp::ArchiveEntry entry;
  entry.archive_path = archive_path;
  entry.source_path = source_path;
  entry.type = bp::EntryType::kDirectory;
  entry.mode = 0755;
  entry.uid = 1000;
  entry.gid = 1000;
  return entry;
}

// 一份完整快照：内容不同 => 身份不同，链不会误认为是同一个 generation。
bool WriteFull(const std::string& repository, const std::string& name,
               const std::string& payload, std::string* error) {
  const std::string marker = repository + "/" + name + ".marker";
  test_support::WriteFile(marker, payload, 0644);
  bp::ArchiveEntry root = RootEntry(marker, ".");
  std::vector<bp::ArchiveEntry> entries;
  entries.push_back(root);
  bp::ArchiveEntry file;
  file.archive_path = "a.txt";
  file.source_path = marker;
  file.type = bp::EntryType::kRegularFile;
  file.size = payload.size();
  file.mode = 0644;
  file.uid = 1000;
  file.gid = 1000;
  struct stat info;
  if (test_support::StatOf(marker, &info)) {
    file.mtime_sec = static_cast<std::int64_t>(info.st_mtim.tv_sec);
    file.mtime_nsec = static_cast<std::uint32_t>(info.st_mtim.tv_nsec);
  }
  entries.push_back(file);
  return bp::RunBackupPipelineFromEntries(entries, repository + "/" + name,
                                          bp::BackupOptions{}, error);
}

// 一份 delta：parent 由参数给出，内容不重要（这里只考依赖图）。
bool WriteDelta(const std::string& repository, const std::string& name,
                const std::string& parent_name,
                const std::string& parent_identity,
                const std::string& generation, std::string* error) {
  const std::string marker = repository + "/" + name + ".marker";
  test_support::WriteFile(marker, "delta-payload", 0644);
  std::vector<bp::ArchiveEntry> entries;
  entries.push_back(RootEntry(marker, "."));
  bp::ArchiveEntry file;
  file.archive_path = "a.txt";
  file.source_path = marker;
  file.type = bp::EntryType::kRegularFile;
  file.size = 13;
  file.mode = 0644;
  file.uid = 1000;
  file.gid = 1000;
  struct stat info;
  if (test_support::StatOf(marker, &info)) {
    file.mtime_sec = static_cast<std::int64_t>(info.st_mtim.tv_sec);
    file.mtime_nsec = static_cast<std::uint32_t>(info.st_mtim.tv_nsec);
  }
  entries.push_back(file);

  bp::DeltaEnvelope envelope;
  envelope.parent_file_name = parent_name;
  envelope.parent_snapshot_id = parent_identity;
  envelope.parent_manifest_digest = bp::ContentDigestOfBytes(name + "-parent");
  envelope.base_generation_id = generation;
  envelope.source_identity = bp::SourceIdentityDigest("/src", "repo");
  envelope.filter_identity = bp::FilterIdentityDigest({}, {});
  envelope.strategy_identity = bp::StrategyIdentityDigest(
      bp::PackMethod::kMyPack, bp::CompressionMethod::kNone,
      bp::EncryptionMethod::kNone);
  envelope.current_manifest_digest = bp::ContentDigestOfBytes(name);
  envelope.added = 1;
  envelope.removed = 0;
  return bp::WriteDeltaFile(repository + "/" + name, envelope, entries,
                            bp::BackupOptions{}, error);
}

std::vector<std::string> Names(const std::vector<std::string>& values) {
  return values;
}

bool Contains(const std::vector<std::string>& values, const std::string& item) {
  return std::find(values.begin(), values.end(), item) != values.end();
}

}  // namespace

int main() {
  // ---- R1：没有链时退回原来的行为 ----
  test_support::Section("INC-RT 1. 全是完整快照：仍然是'删最旧的'");
  {
    const std::string work = test_support::FreshDir("inc-retention-full");
    const std::string repo = work + "/repo";
    test_support::Mkdir(repo, 0755);
    std::string error;
    for (int index = 1; index <= 4; ++index) {
      const std::string name = "f" + std::to_string(index) + ".bak";
      test_support::Check(
          WriteFull(repo, name, "payload-" + std::to_string(index), &error),
          "INC-RT T1 写出 " + name, error);
    }
    std::vector<std::string> candidates = {"f1.bak", "f2.bak", "f3.bak",
                                           "f4.bak"};
    bp::RetentionPlan plan;
    test_support::Check(
        bp::PlanDependencyAwareRetention(repo, candidates, 2, &plan, &error),
        "INC-RT T1 计划成功", error);
    test_support::Check(Names(plan.keep_visible) ==
                            std::vector<std::string>({"f3.bak", "f4.bak"}),
                        "INC-RT T1 可见集合是最近两个");
    test_support::Check(plan.keep_ancestors.empty(),
                        "INC-RT T1 完整快照没有祖先");
    test_support::Check(
        Names(plan.remove) == std::vector<std::string>({"f1.bak", "f2.bak"}),
        "INC-RT T1 可以删的是最旧的两个（最旧在前）");
  }

  // ---- R2：链上的祖先永远不进删除集合 ----
  test_support::Section("INC-RT 2. 依赖链：祖先被保留，绝不删断链");
  {
    const std::string work = test_support::FreshDir("inc-retention-chain");
    const std::string repo = work + "/repo";
    test_support::Mkdir(repo, 0755);
    std::string error;
    test_support::Check(WriteFull(repo, "f0.bak", "base", &error),
                        "INC-RT T2 写出完整基线", error);
    std::string f0_id;
    test_support::Check(bp::SnapshotIdOfFile(repo + "/f0.bak", &f0_id, &error),
                        "INC-RT T2 基线身份", error);
    test_support::Check(
        WriteDelta(repo, "d1.bak", "f0.bak", f0_id, f0_id, &error),
        "INC-RT T2 写出 d1", error);
    std::string d1_id;
    bp::DeltaEnvelope d1;
    test_support::Check(bp::ReadDeltaEnvelope(repo + "/d1.bak", &d1, &error),
                        "INC-RT T2 读 d1", error);
    d1_id = d1.snapshot_id;
    test_support::Check(
        WriteDelta(repo, "d2.bak", "d1.bak", d1_id, f0_id, &error),
        "INC-RT T2 写出 d2", error);
    bp::DeltaEnvelope d2;
    test_support::Check(bp::ReadDeltaEnvelope(repo + "/d2.bak", &d2, &error),
                        "INC-RT T2 读 d2", error);
    test_support::Check(
        WriteDelta(repo, "d3.bak", "d2.bak", d2.snapshot_id, f0_id, &error),
        "INC-RT T2 写出 d3", error);

    std::vector<std::string> candidates = {"f0.bak", "d1.bak", "d2.bak",
                                           "d3.bak"};
    bp::RetentionPlan plan;
    test_support::Check(
        bp::PlanDependencyAwareRetention(repo, candidates, 2, &plan, &error),
        "INC-RT T2 计划成功", error);
    test_support::Check(Names(plan.keep_visible) ==
                            std::vector<std::string>({"d2.bak", "d3.bak"}),
                        "INC-RT T2 可见集合是最近两个 restore point");
    test_support::Check(Contains(plan.keep_ancestors, "d1.bak") &&
                            Contains(plan.keep_ancestors, "f0.bak"),
                        "INC-RT T2 d1 与 f0 因为被依赖而保留",
                        std::to_string(plan.keep_ancestors.size()));
    test_support::Check(plan.remove.empty(),
                        "INC-RT T2 判别：没有任何一份可以被删（删谁都会断链）",
                        std::to_string(plan.remove.size()));
    // 反过来说：如果按"删最旧"来做，f0 与 d1 都会消失，剩下 d2/d3 不可恢复。
    test_support::Check(
        !Contains(plan.remove, "f0.bak") && !Contains(plan.remove, "d1.bak"),
        "INC-RT T2 判别：朴素实现会删掉的这两份被保住了");

    // retain=1：只剩下 d3 可见，它的整条祖先链都要保留。
    bp::RetentionPlan tight;
    test_support::Check(
        bp::PlanDependencyAwareRetention(repo, candidates, 1, &tight, &error),
        "INC-RT T2 retain=1 计划成功", error);
    test_support::Check(
        Names(tight.keep_visible) == std::vector<std::string>({"d3.bak"}) &&
            tight.keep_ancestors.size() == 3 && tight.remove.empty(),
        "INC-RT T2 retain=1：只有最新一个可见，其余三个作为祖先保留",
        std::to_string(tight.keep_ancestors.size()));
  }

  // ---- R3：链外的老快照仍然可以删 ----
  test_support::Section("INC-RT 3. 与链无关的旧快照照样可以淘汰");
  {
    const std::string work = test_support::FreshDir("inc-retention-mixed");
    const std::string repo = work + "/repo";
    test_support::Mkdir(repo, 0755);
    std::string error;
    test_support::Check(WriteFull(repo, "old.bak", "old-independent", &error),
                        "INC-RT T3 写出一份无关的旧完整快照", error);
    test_support::Check(WriteFull(repo, "f0.bak", "base", &error),
                        "INC-RT T3 写出基线", error);
    std::string f0_id;
    test_support::Check(bp::SnapshotIdOfFile(repo + "/f0.bak", &f0_id, &error),
                        "INC-RT T3 基线身份", error);
    test_support::Check(
        WriteDelta(repo, "d1.bak", "f0.bak", f0_id, f0_id, &error),
        "INC-RT T3 写出 delta", error);

    std::vector<std::string> candidates = {"old.bak", "f0.bak", "d1.bak"};
    bp::RetentionPlan plan;
    test_support::Check(
        bp::PlanDependencyAwareRetention(repo, candidates, 2, &plan, &error),
        "INC-RT T3 计划成功", error);
    test_support::Check(
        Names(plan.remove) == std::vector<std::string>({"old.bak"}),
        "INC-RT T3 只删那份与链无关的旧快照",
        std::to_string(plan.remove.size()));
    // f0 本身就在可见集合里（它是最近两个之一），所以它不会被记进
    // keep_ancestors —— 那一列只记"不可见但必须留"的祖先。这里要断言的是
    // 它没有被删掉，而不是它出现在哪一列。
    test_support::Check(!Contains(plan.remove, "f0.bak") &&
                            Contains(plan.keep_visible, "f0.bak"),
                        "INC-RT T3 f0 仍然被保留（它在可见集合里）");
  }

  // ---- R4：坏链 -> 拒绝出计划，而不是猜着删 ----
  test_support::Section("INC-RT 4. 读不出依赖关系时拒绝出计划（fail closed）");
  {
    const std::string work = test_support::FreshDir("inc-retention-broken");
    const std::string repo = work + "/repo";
    test_support::Mkdir(repo, 0755);
    std::string error;
    test_support::Check(WriteFull(repo, "f0.bak", "base", &error),
                        "INC-RT T4 写出基线", error);
    // 一份被截断的 delta：magic 对，内容是坏的。
    std::string bytes;
    test_support::Check(
        WriteDelta(repo, "d1.bak", "f0.bak", std::string(64, 'a'),
                   std::string(64, 'b'), &error),
        "INC-RT T4 写出 d1", error);
    test_support::Check(test_support::ReadFile(repo + "/d1.bak", &bytes),
                        "INC-RT T4 读入 d1");
    test_support::Check(
        test_support::WriteFile(repo + "/d1.bak",
                                bytes.substr(0, bytes.size() / 2), 0644),
        "INC-RT T4 截断 d1");
    std::vector<std::string> candidates = {"f0.bak", "d1.bak"};
    bp::RetentionPlan plan;
    const bool planned =
        bp::PlanDependencyAwareRetention(repo, candidates, 1, &plan, &error);
    test_support::Check(!planned,
                        "INC-RT T4 判别：读不出依赖就拒绝出计划，绝不猜着删",
                        planned ? "(竟然成功了)" : error);
  }

  return test_support::Finish("incremental_retention_test");
}
