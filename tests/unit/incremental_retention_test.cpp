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
#include "backup_catalog.h"
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

// 一份 manifest 条目表：根目录 + a.txt（内容由参数给出）。
//
// retention 的依赖链现在走**已验证身份**：链上的 delta 必须带产品形状的两个
// 副文件，而且它的 envelope.current_manifest_digest 必须与 manifest 副文件
// 的摘要一致，否则计划会（正确地）进入 fail-closed。
std::vector<bp::ManifestEntry> ManifestFor(const std::string& content) {
  std::vector<bp::ManifestEntry> entries;
  bp::ManifestEntry root;
  root.archive_path = ".";
  root.type = bp::EntryType::kDirectory;
  root.mode = 0755;
  root.uid = 1000;
  root.gid = 1000;
  entries.push_back(root);
  bp::ManifestEntry file;
  file.archive_path = "a.txt";
  file.type = bp::EntryType::kRegularFile;
  file.size = content.size();
  file.mode = 0644;
  file.uid = 1000;
  file.gid = 1000;
  file.content_digest = bp::ContentDigestOfBytes(content);
  entries.push_back(file);
  return entries;
}

// 给一份快照写产品形状的两个副文件：manifest v3 + BPIDENT2。
bool WriteSidecars(const std::string& repository, const std::string& name,
                   const std::vector<bp::ManifestEntry>& entries,
                   std::string* error) {
  bp::ManifestBinding binding;
  binding.snapshot_file_name = name;
  binding.repository_identity = bp::RepositoryIdentity(repository);
  binding.source_path = "/test-source";
  const std::string manifest = bp::SerializeManifestV3(entries, binding);
  if (manifest.empty()) {
    if (error != nullptr) *error = "cannot serialize the manifest sidecar";
    return false;
  }
  if (!test_support::WriteFile(repository + "/" + name + ".manifest", manifest,
                               0640)) {
    if (error != nullptr) *error = "cannot write the manifest sidecar";
    return false;
  }
  std::string snapshot_id;
  if (!bp::SnapshotIdOfFile(repository + "/" + name, &snapshot_id, error)) {
    return false;
  }
  // BPIDENT2 的行式格式见 include/incremental_backup.h 的说明。
  std::string identity = "BPIDENT2\n";
  identity += "version=2\n";
  identity += "snapshot_file_name=" + name + "\n";
  identity += "snapshot_id=" + snapshot_id + "\n";
  identity += "manifest_digest=" + bp::ManifestDigest(entries) + "\n";
  identity += "source=" +
              bp::SourceIdentityDigest("/test-source",
                                       binding.repository_identity) +
              "\n";
  identity += "filter=" + bp::FilterIdentityDigest({}, {}) + "\n";
  identity += "strategy=" +
              bp::StrategyIdentityDigest(bp::PackMethod::kMyPack,
                                         bp::CompressionMethod::kNone,
                                         bp::EncryptionMethod::kNone) +
              "\n";
  if (!test_support::WriteFile(repository + "/" + name + ".identity", identity,
                               0640)) {
    if (error != nullptr) *error = "cannot write the identity sidecar";
    return false;
  }
  return true;
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
  if (!bp::RunBackupPipelineFromEntries(entries, repository + "/" + name,
                                        bp::BackupOptions{}, error)) {
    return false;
  }
  return WriteSidecars(repository, name, ManifestFor(payload), error);
}

// 一份 delta：parent 由参数给出，内容不重要（这里只考依赖图）。
bool WriteDelta(const std::string& repository, const std::string& name,
                const std::string& parent_name,
                const std::string& parent_identity,
                const std::string& generation, std::string* error) {
  const std::string marker = repository + "/" + name + ".marker";
  const std::string content = "delta-payload";
  test_support::WriteFile(marker, content, 0644);
  std::vector<bp::ArchiveEntry> entries;
  entries.push_back(RootEntry(marker, "."));
  bp::ArchiveEntry file;
  file.archive_path = "a.txt";
  file.source_path = marker;
  file.type = bp::EntryType::kRegularFile;
  file.size = content.size();
  file.mode = 0644;
  file.uid = 1000;
  file.gid = 1000;
  struct stat info;
  if (test_support::StatOf(marker, &info)) {
    file.mtime_sec = static_cast<std::int64_t>(info.st_mtim.tv_sec);
    file.mtime_nsec = static_cast<std::uint32_t>(info.st_mtim.tv_nsec);
  }
  entries.push_back(file);

  const std::vector<bp::ManifestEntry> manifest_entries = ManifestFor(content);
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
  // 必须与 manifest 副文件的摘要一致，否则这份 delta 的绑定通不过验证。
  envelope.current_manifest_digest = bp::ManifestDigest(manifest_entries);
  envelope.added = 1;
  envelope.removed = 0;
  if (!bp::WriteDeltaFile(repository + "/" + name, envelope, entries,
                          bp::BackupOptions{}, error)) {
    return false;
  }
  return WriteSidecars(repository, name, manifest_entries, error);
}

// ---- 删除顺序的故障注入接缝 ----
//
// 回调没有上下文参数，所以用一个测试内的全局计数：第 g_unlink_fail_at 次
// unlink 返回"失败"。0 表示从不失败。
int g_unlink_calls = 0;
int g_unlink_fail_at = 0;

bool FailUnlinkHook(const char* /*archive_path*/) {
  ++g_unlink_calls;
  return g_unlink_fail_at != 0 && g_unlink_calls == g_unlink_fail_at;
}

// 删除集合里的每一份（还存在的）快照，其父文件名要么为空、要么对应的文件还在。
bool RemainingChainIntact(const std::string& repository,
                          const std::vector<std::string>& names,
                          std::string* detail) {
  for (const std::string& name : names) {
    if (!test_support::Exists(repository + "/" + name)) continue;
    std::string parent;
    std::string error;
    if (!bp::SnapshotParentOf(repository, name, &parent, &error)) {
      if (detail != nullptr) *detail = name + ": " + error;
      return false;
    }
    if (parent.empty()) continue;
    if (!test_support::Exists(repository + "/" + parent)) {
      if (detail != nullptr) {
        *detail = name + " still points at the deleted parent " + parent;
      }
      return false;
    }
  }
  return true;
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
    // 契约是"读不出依赖的**不删**，并如实记下来"，而不是"整份计划失败"：
    // 只因为一份读不出来的快照就拒绝淘汰其它无关的旧快照，会让仓库无上限
    // 增长——那不是安全，只是把问题推给下一轮。
    test_support::Check(planned && Contains(plan.unreadable, "d1.bak"),
                        "INC-RT T4 读不出依赖的快照被记进 unreadable", error);
    test_support::Check(!Contains(plan.remove, "d1.bak"),
                        "INC-RT T4 判别：证明不了安全的快照绝不进入删除集合");
  }


  // ---- R5：删除顺序必须 descendants-first ----
  test_support::Section("INC-RT 5. 删除顺序：叶子先删（中断也不留断链）");
  {
    const std::string work = test_support::FreshDir("inc-retention-order");
    const std::string repo = work + "/repo";
    test_support::Mkdir(repo, 0755);
    std::string error;
    test_support::Check(WriteFull(repo, "f0.bak", "base", &error),
                        "INC-RT ORDER 写出 F0", error);
    std::string f0_id;
    test_support::Check(bp::SnapshotIdOfFile(repo + "/f0.bak", &f0_id, &error),
                        "INC-RT ORDER F0 身份", error);
    test_support::Check(WriteDelta(repo, "d1.bak", "f0.bak", f0_id, f0_id,
                                   &error),
                        "INC-RT ORDER 写出 D1", error);
    bp::DeltaEnvelope d1;
    test_support::Check(bp::ReadDeltaEnvelope(repo + "/d1.bak", &d1, &error),
                        "INC-RT ORDER 读 D1", error);
    test_support::Check(
        WriteDelta(repo, "d2.bak", "d1.bak", d1.snapshot_id, f0_id, &error),
        "INC-RT ORDER 写出 D2", error);

    std::vector<std::string> candidates = {"f0.bak", "d1.bak", "d2.bak"};
    bp::RetentionPlan plan;
    test_support::Check(
        bp::PlanDependencyAwareRetention(repo, candidates, 0, &plan, &error),
        "INC-RT ORDER 计划成功（retain=0 -> 整个 generation 可删）", error);
    test_support::Check(Names(plan.remove) == candidates,
                        "INC-RT ORDER 删除集合 = 整条链",
                        std::to_string(plan.remove.size()));

    bp::BackupCatalog catalog;
    // (a) RET-ORDER-01：第一次 unlink 就失败。
    g_unlink_calls = 0;
    g_unlink_fail_at = 1;
    bp::SetBackupCatalogUnlinkFailureHookForTesting(FailUnlinkHook);
    std::vector<std::string> removed;
    std::vector<std::string> diagnostics;
    error.clear();
    const bool first_failed =
        !catalog.DeleteSnapshots(repo, plan.remove, &removed, &diagnostics,
                                 &error);
    bp::SetBackupCatalogUnlinkFailureHookForTesting(nullptr);
    test_support::Check(first_failed && removed.empty(),
                        "INC-RT ORDER-01 第一次删除就失败：什么都没删掉", error);
    test_support::Check(test_support::Exists(repo + "/f0.bak") &&
                            test_support::Exists(repo + "/d1.bak") &&
                            test_support::Exists(repo + "/d2.bak"),
                        "INC-RT ORDER-01 三份都还在");
    std::string detail;
    test_support::Check(
        RemainingChainIntact(repo, candidates, &detail),
        "INC-RT ORDER-01 判别：剩下的链依然自洽（没有指向已删父节点的后代）",
        detail);

    // (b) RET-ORDER-02：第二次 unlink 失败 -> 只删掉了叶子 D2。
    g_unlink_calls = 0;
    g_unlink_fail_at = 2;
    bp::SetBackupCatalogUnlinkFailureHookForTesting(FailUnlinkHook);
    removed.clear();
    diagnostics.clear();
    error.clear();
    const bool second_failed =
        !catalog.DeleteSnapshots(repo, plan.remove, &removed, &diagnostics,
                                 &error);
    bp::SetBackupCatalogUnlinkFailureHookForTesting(nullptr);
    test_support::Check(
        second_failed &&
            Names(removed) == std::vector<std::string>({"d2.bak"}),
        "INC-RT ORDER-02 第二次删除失败：先被删掉的是叶子 D2",
        std::to_string(removed.size()));
    test_support::Check(!test_support::Exists(repo + "/d2.bak") &&
                            test_support::Exists(repo + "/d1.bak") &&
                            test_support::Exists(repo + "/f0.bak"),
                        "INC-RT ORDER-02 剩下 F0 与 D1");
    detail.clear();
    test_support::Check(
        RemainingChainIntact(repo, {"f0.bak", "d1.bak"}, &detail),
        "INC-RT ORDER-02 判别：D1 的父亲 F0 仍然存在（可恢复）", detail);

    // (c) RET-ORDER-03：没有故障时，返回的删除顺序就是 descendants-first。
    removed.clear();
    diagnostics.clear();
    error.clear();
    // D2 在上一步已经被删掉，这里只删剩下的两份。
    removed.clear();
    diagnostics.clear();
    error.clear();
    test_support::Check(
        catalog.DeleteSnapshots(repo, {"f0.bak", "d1.bak"}, &removed,
                                &diagnostics, &error),
        "INC-RT ORDER-03 整批删除成功", error);
    test_support::Check(
        Names(removed) == std::vector<std::string>({"d1.bak", "f0.bak"}),
        "INC-RT ORDER-03 判别：执行顺序是 D1 → F0（叶子在前，不是 oldest-first）",
        removed.empty() ? std::string("empty") : removed.front());
    test_support::Check(!test_support::Exists(repo + "/f0.bak") &&
                            !test_support::Exists(repo + "/d1.bak") &&
                            !test_support::Exists(repo + "/d2.bak"),
                        "INC-RT ORDER-03 三份都被删掉");
  }

  // ---- R6：依赖不确定 -> 什么都不删 ----
  test_support::Section("INC-RT 6. 依赖不确定时 fail closed");
  {
    const std::string work = test_support::FreshDir("inc-retention-uncertain");
    const std::string repo = work + "/repo";
    test_support::Mkdir(repo, 0755);
    std::string error;
    test_support::Check(WriteFull(repo, "old.bak", "unrelated", &error),
                        "INC-RT UNCERTAIN 写出一份无关的旧快照", error);
    test_support::Check(WriteFull(repo, "f0.bak", "base", &error),
                        "INC-RT UNCERTAIN 写出 F0", error);
    std::string f0_id;
    test_support::Check(bp::SnapshotIdOfFile(repo + "/f0.bak", &f0_id, &error),
                        "INC-RT UNCERTAIN F0 身份", error);
    test_support::Check(WriteDelta(repo, "d1.bak", "f0.bak", f0_id, f0_id,
                                   &error),
                        "INC-RT UNCERTAIN 写出 D1", error);
    bp::DeltaEnvelope d1;
    test_support::Check(bp::ReadDeltaEnvelope(repo + "/d1.bak", &d1, &error),
                        "INC-RT UNCERTAIN 读 D1", error);
    test_support::Check(
        WriteDelta(repo, "d2.bak", "d1.bak", d1.snapshot_id, f0_id, &error),
        "INC-RT UNCERTAIN 写出 D2", error);

    std::vector<std::string> candidates = {"old.bak", "f0.bak", "d1.bak",
                                           "d2.bak"};
    // 正向对照：一切完好时，与链无关的旧快照照常淘汰。
    bp::RetentionPlan healthy;
    test_support::Check(
        bp::PlanDependencyAwareRetention(repo, candidates, 2, &healthy, &error),
        "INC-RT UNCERTAIN 正向对照的计划成功", error);
    test_support::Check(
        !healthy.dependency_uncertain &&
            Names(healthy.remove) == std::vector<std::string>({"old.bak"}),
        "INC-RT UNCERTAIN 正向对照：只删与链无关的那一份",
        std::to_string(healthy.remove.size()));

    // RET-UNCERTAIN-02：把保留点 D2 的 payload 改坏（信封与副文件都不动）。
    std::string bytes;
    test_support::Check(test_support::ReadFile(repo + "/d2.bak", &bytes) &&
                            bytes.size() > 8,
                        "INC-RT UNCERTAIN 读入 D2");
    bytes[bytes.size() - 1] = static_cast<char>(bytes[bytes.size() - 1] ^ 0x01);
    test_support::Check(test_support::WriteFile(repo + "/d2.bak", bytes, 0640),
                        "INC-RT UNCERTAIN 改坏 D2 的 payload");
    bp::RetentionPlan uncertain;
    test_support::Check(
        bp::PlanDependencyAwareRetention(repo, candidates, 2, &uncertain,
                                         &error),
        "INC-RT UNCERTAIN 坏链下仍然给得出计划（不抛错）", error);
    test_support::Check(uncertain.dependency_uncertain &&
                            uncertain.remove.empty(),
                        "INC-RT UNCERTAIN-02 判别：依赖不确定 -> 什么都不删",
                        uncertain.uncertainty_reason);
    test_support::Check(!uncertain.uncertainty_reason.empty(),
                        "INC-RT UNCERTAIN-02 给出了不确定的原因");
    test_support::Check(Contains(uncertain.unreadable, "d2.bak"),
                        "INC-RT UNCERTAIN-02 把读不透的那一份记进 unreadable");

    // RET-UNCERTAIN-01：F0 -> D1，D1（最新的可见点）坏掉，F0 绝不能被删。
    std::string d1_bytes;
    test_support::Check(test_support::ReadFile(repo + "/d1.bak", &d1_bytes) &&
                            d1_bytes.size() > 8,
                        "INC-RT UNCERTAIN-01 读入 D1");
    d1_bytes[d1_bytes.size() - 1] =
        static_cast<char>(d1_bytes[d1_bytes.size() - 1] ^ 0x01);
    test_support::Check(
        test_support::WriteFile(repo + "/d1.bak", d1_bytes, 0640),
        "INC-RT UNCERTAIN-01 改坏 D1 的 payload");
    std::vector<std::string> pair = {"f0.bak", "d1.bak"};
    bp::RetentionPlan tight;
    test_support::Check(
        bp::PlanDependencyAwareRetention(repo, pair, 1, &tight, &error),
        "INC-RT UNCERTAIN-01 计划成功", error);
    test_support::Check(tight.dependency_uncertain && tight.remove.empty() &&
                            !Contains(tight.remove, "f0.bak"),
                        "INC-RT UNCERTAIN-01 判别：F0 不会因为 D1 读不透而被删",
                        std::to_string(tight.remove.size()));
  }

  return test_support::Finish("incremental_retention_test");
}
