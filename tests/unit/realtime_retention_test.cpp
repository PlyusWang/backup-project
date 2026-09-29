// realtime_retention_test.cpp
//
// PR #19：realtime retention 的专项测试。
//
// 要钉住的：
//   * retention 只在**当前 job_identity** 的 realtime 快照里淘汰；
//   * 真正删除复用 PR #18 的 PlanDependencyAwareRetention +
//     BackupCatalog::DeleteSnapshots（descendants-first），marker
//     跟着归档一起清；
//   * 跨 Trigger 的祖先（Manual / Scheduled 建的）绝不被误删；有活着的非
//     realtime 后代时，删除被拒绝而不是"删了让它变成不可恢复"；
//   * 任何一步无法验证（坏 marker、坏依赖链、孤儿 marker）-> 这一轮一份都不删；
//   * 什么都不用删的时候不是错误，也不报成 uncertain。

#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <vector>

#include "archive_pipeline.h"
#include "backup_catalog.h"
#include "backup_mode.h"
#include "filter.h"
#include "incremental_backup.h"
#include "incremental_restore.h"
#include "realtime_backup_service.h"
#include "test_support.h"

namespace bp = backupproject;

namespace {

constexpr std::int64_t kTimeBase = 1810000000;

std::int64_t NextTime() {
  static std::int64_t value = kTimeBase;
  value += 7;
  return value;
}

bp::RealtimeConfig MakeConfig(const std::string& source,
                              bp::BackupStrategy strategy,
                              std::uint32_t retain_count) {
  bp::RealtimeConfig config;
  config.version = bp::kRealtimeConfigVersion;
  config.enabled = true;
  config.trigger = bp::BackupTrigger::kRealtime;
  config.source_path = source;
  config.debounce_ms = bp::kDefaultRealtimeDebounceMs;
  config.max_wait_ms = bp::kDefaultRealtimeMaxWaitMs;
  config.retain_count = retain_count;
  config.strategy = strategy;
  config.pack_method = bp::PackMethod::kMyPack;
  config.compression_method = bp::CompressionMethod::kNone;
  config.encryption_method = bp::EncryptionMethod::kNone;
  return config;
}

bp::RealtimeEventSummary MakeEvents(std::uint64_t generation,
                                    std::uint64_t event_count,
                                    bool resync = false) {
  bp::RealtimeEventSummary events;
  events.generation = generation;
  events.event_count = event_count;
  events.resync = resync;
  return events;
}

std::string JobIdentityOf(const bp::RealtimeConfig& config,
                          const std::string& repository) {
  return bp::RealtimeJobIdentityDigest(
      config, bp::RepositoryIdentity(repository), config.source_path);
}

bool RunOnce(const bp::RealtimeConfig& config, const std::string& repository,
             const bp::RealtimeEventSummary& events, std::int64_t now_sec,
             bp::RealtimeOutcome* outcome, const std::string& label) {
  std::string error;
  if (!bp::RunRealtimeBackupOnce(config, repository,
                                 bp::RepositoryIdentity(repository), events,
                                 now_sec, outcome, &error)) {
    test_support::Check(false, label + " 执行成功", error);
    return false;
  }
  return true;
}

std::vector<bp::RealtimeSnapshotRecord> ListRealtime(
    const std::string& repository) {
  std::vector<bp::RealtimeSnapshotRecord> records;
  std::string error;
  if (!bp::ListRealtimeSnapshots(repository, &records, &error)) {
    test_support::Check(false, "ListRealtimeSnapshots 成功", error);
  }
  return records;
}

bool Exists(const std::string& path) { return test_support::Exists(path); }

std::string MarkerPath(const std::string& repository,
                       const std::string& snapshot_file_name) {
  return repository + "/" + bp::RealtimeMarkerFileName(snapshot_file_name);
}

bool MakeManualBaseline(const std::string& source,
                        const std::string& repository, std::string* file_name,
                        std::string* error) {
  bp::BackupCatalog catalog;
  if (!catalog.EnsureRepository(repository, error)) return false;
  std::string archive_path;
  if (!catalog.BuildArchivePath(repository, source, NextTime(), &archive_path,
                                error)) {
    return false;
  }
  const std::string name = archive_path.substr(archive_path.rfind('/') + 1);
  bp::Filter filter;
  bp::BackupOptions options;
  options.pack_method = bp::PackMethod::kMyPack;
  options.compression_method = bp::CompressionMethod::kNone;
  options.encryption_method = bp::EncryptionMethod::kNone;
  bp::IncrementalOutcome outcome;
  if (!bp::RunIncrementalBackup(
          source, repository, name, bp::RepositoryIdentity(repository), filter,
          options, {}, {}, std::string(), &outcome, error)) {
    return false;
  }
  *file_name = name;
  return outcome.kind == bp::IncrementalOutcome::Kind::kFullBaseline;
}

// 手工 delta（没有 realtime marker）：模拟 Manual / Scheduled 触发的后代。
bool MakeManualDelta(const std::string& source, const std::string& repository,
                     const std::string& parent_file_name,
                     std::string* file_name, std::string* error) {
  bp::BackupCatalog catalog;
  std::string archive_path;
  if (!catalog.BuildArchivePath(repository, source, NextTime(), &archive_path,
                                error)) {
    return false;
  }
  const std::string name = archive_path.substr(archive_path.rfind('/') + 1);
  bp::Filter filter;
  bp::BackupOptions options;
  options.pack_method = bp::PackMethod::kMyPack;
  options.compression_method = bp::CompressionMethod::kNone;
  options.encryption_method = bp::EncryptionMethod::kNone;
  bp::IncrementalOutcome outcome;
  if (!bp::RunIncrementalBackup(
          source, repository, name, bp::RepositoryIdentity(repository), filter,
          options, {}, {}, parent_file_name, &outcome, error)) {
    return false;
  }
  *file_name = name;
  return outcome.kind == bp::IncrementalOutcome::Kind::kDelta;
}

std::size_t RepoEntryCount(const std::string& repository) {
  return test_support::DirEntries(repository).size();
}

}  // namespace

int main() {
  // ============================================================
  test_support::Section("INC-RTR 1. Full retain=3：留最近 3 份，marker 一起清");
  // ============================================================
  {
    const std::string work = test_support::FreshDir("realtime-retention-full");
    const std::string source = work + "/src";
    const std::string repository = work + "/repo";
    test_support::Mkdir(source, 0755);
    test_support::Mkdir(repository, 0755);
    test_support::WriteFile(source + "/a.txt", "v0\n", 0644);

    bp::RealtimeConfig config =
        MakeConfig(source, bp::BackupStrategy::kFull, 3);
    std::vector<std::string> names;
    std::uint64_t deleted_total = 0;
    for (int index = 1; index <= 5; ++index) {
      test_support::WriteFile(source + "/a.txt",
                              "v" + std::to_string(index) + "\n", 0644);
      bp::RealtimeOutcome outcome;
      if (!RunOnce(config, repository,
                   MakeEvents(static_cast<std::uint64_t>(index), 1), NextTime(),
                   &outcome, "INC-RTR T1")) {
        break;
      }
      names.push_back(outcome.snapshot_file_name);
      deleted_total += outcome.retention_deleted;
      if (index <= 3) {
        test_support::Check(outcome.retention_deleted == 0,
                            "INC-RTR T1 候选 <= retain 时一份都不删");
      }
    }
    test_support::Check(names.size() == 5, "INC-RTR T1 五次触发都成功");
    test_support::Check(deleted_total == 2,
                        "INC-RTR T1 判别：多出来的两份真的被淘汰",
                        std::to_string(deleted_total));

    const std::vector<bp::RealtimeSnapshotRecord> records =
        ListRealtime(repository);
    test_support::Check(records.size() == 3,
                        "INC-RTR T1 仓库里只剩 3 份 realtime 快照",
                        std::to_string(records.size()));
    bool keep_newest = records.size() == 3;
    if (names.size() == 5 && keep_newest) {
      keep_newest = records[0].file_name == names[2] &&
                    records[1].file_name == names[3] &&
                    records[2].file_name == names[4];
    }
    test_support::Check(keep_newest,
                        "INC-RTR T1 判别：留下的是最近 3 份（最旧在前）");

    bool all_verified = !records.empty();
    for (const bp::RealtimeSnapshotRecord& record : records) {
      all_verified = all_verified && record.verified;
    }
    test_support::Check(all_verified, "INC-RTR T1 留下来的都 verified");

    if (names.size() == 5) {
      test_support::Check(!Exists(repository + "/" + names[0]) &&
                              !Exists(repository + "/" + names[1]),
                          "INC-RTR T1 被淘汰的归档不在了");
      test_support::Check(!Exists(MarkerPath(repository, names[0])) &&
                              !Exists(MarkerPath(repository, names[1])),
                          "INC-RTR T1 判别：marker 跟着归档一起清（不留孤儿）");
      test_support::Check(Exists(repository + "/" + names[4]) &&
                              Exists(MarkerPath(repository, names[4])),
                          "INC-RTR T1 最新一份的归档与 marker 都在");
    }
    test_support::Check(RepoEntryCount(repository) == 6,
                        "INC-RTR T1 仓库里就是 3 份归档 + 3 个 marker",
                        std::to_string(RepoEntryCount(repository)));
    std::vector<std::string> leftovers;
    for (const std::string& name : test_support::DirEntries(repository)) {
      if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".tmp") == 0) {
        leftovers.push_back(name);
      }
    }
    test_support::Check(leftovers.empty(), "INC-RTR T1 不留 .tmp 半成品");
  }

  // ============================================================
  test_support::Section(
      "INC-RTR 2. Incremental 链：可见点需要的祖先一定要留下来");
  // ============================================================
  {
    const std::string work = test_support::FreshDir("realtime-retention-chain");
    const std::string source = work + "/src";
    const std::string repository = work + "/repo";
    test_support::Mkdir(source, 0755);
    test_support::Mkdir(repository, 0755);
    test_support::WriteFile(source + "/a.txt", "v0\n", 0644);

    bp::RealtimeConfig config =
        MakeConfig(source, bp::BackupStrategy::kIncremental, 2);
    bp::RealtimeOutcome baseline;
    RunOnce(config, repository, MakeEvents(1, 0, true), NextTime(), &baseline,
            "INC-RTR T2");
    test_support::WriteFile(source + "/a.txt", "v1\n", 0644);
    bp::RealtimeOutcome delta1;
    RunOnce(config, repository, MakeEvents(2, 1), NextTime(), &delta1,
            "INC-RTR T2");
    test_support::WriteFile(source + "/a.txt", "v2\n", 0644);
    bp::RealtimeOutcome delta2;
    RunOnce(config, repository, MakeEvents(3, 1), NextTime(), &delta2,
            "INC-RTR T2");

    test_support::Check(delta1.kind == bp::RealtimeOutcome::Kind::kDelta &&
                            delta2.kind == bp::RealtimeOutcome::Kind::kDelta,
                        "INC-RTR T2 准备：两份 delta");
    test_support::Check(delta2.retention_deleted == 0,
                        "INC-RTR T2 判别：可见点依赖的祖先不会被删掉",
                        std::to_string(delta2.retention_deleted));
    test_support::Check(delta2.retention_kept_ancestors >= 1,
                        "INC-RTR T2 判别：祖先被记为 dependency retained",
                        std::to_string(delta2.retention_kept_ancestors));
    test_support::Check(
        Exists(repository + "/" + baseline.snapshot_file_name) &&
            Exists(repository + "/" + delta1.snapshot_file_name) &&
            Exists(repository + "/" + delta2.snapshot_file_name),
        "INC-RTR T2 链上三份都还在（链优先于淘汰数量）");
    {
      const std::string destination = work + "/restore";
      bp::RestoreOptions options;
      bp::RestoreReport report;
      std::string error;
      test_support::Check(
          bp::RestoreSnapshotChain(repository, delta2.snapshot_file_name,
                                   destination, options, &report, &error),
          "INC-RTR T2 最新 delta 仍然可恢复", error);
      std::string content;
      test_support::Check(
          test_support::ReadFile(destination + "/a.txt", &content) &&
              content == "v2\n",
          "INC-RTR T2 恢复内容 = 当前源树");
    }
  }

  // ============================================================
  test_support::Section(
      "INC-RTR 3. 被完全取代的旧链会被真正删掉（而不是只涨不落）");
  // ============================================================
  {
    const std::string work = test_support::FreshDir("realtime-retention-old");
    const std::string source = work + "/src";
    const std::string repository = work + "/repo";
    test_support::Mkdir(source, 0755);
    test_support::Mkdir(repository, 0755);
    test_support::WriteFile(source + "/a.txt", "v0\n", 0644);

    bp::RealtimeConfig config =
        MakeConfig(source, bp::BackupStrategy::kIncremental, 1);
    bp::RealtimeOutcome old_baseline;
    RunOnce(config, repository, MakeEvents(1, 0, true), NextTime(),
            &old_baseline, "INC-RTR T3");

    // 让旧基线不再可用作基线（identity 副文件丢了）：下一次会建一条**独立**的
    // 新链，旧链于是没有任何活着的后代，可以被真正淘汰。
    test_support::Check(
        ::unlink((repository + "/" +
                  bp::SnapshotIdentityFileName(old_baseline.snapshot_file_name))
                     .c_str()) == 0,
        "INC-RTR T3 造出「旧基线的 identity 丢了」这一状态");

    test_support::WriteFile(source + "/a.txt", "v1\n", 0644);
    bp::RealtimeOutcome new_baseline;
    if (RunOnce(config, repository, MakeEvents(2, 1), NextTime(), &new_baseline,
                "INC-RTR T3")) {
      test_support::Check(
          new_baseline.kind == bp::RealtimeOutcome::Kind::kFullBaseline,
          "INC-RTR T3 准备：建了一条独立的新基线");
      test_support::Check(new_baseline.retention_deleted == 1,
                          "INC-RTR T3 判别：被完全取代的旧链真的被删掉",
                          std::to_string(new_baseline.retention_deleted));
      test_support::Check(
          !Exists(repository + "/" + old_baseline.snapshot_file_name) &&
              !Exists(MarkerPath(repository, old_baseline.snapshot_file_name)),
          "INC-RTR T3 旧链的归档与 marker 都清掉了");
      test_support::Check(
          Exists(repository + "/" + new_baseline.snapshot_file_name),
          "INC-RTR T3 新基线还在");
      test_support::Check(
          !new_baseline.retention_uncertain,
          "INC-RTR T3 这一次的淘汰是确定的（不是 fail-closed）");
    }

    // 再挂一个 delta：新基线成了"可见点依赖的祖先"，于是不能再动。
    test_support::WriteFile(source + "/a.txt", "v2\n", 0644);
    bp::RealtimeOutcome child;
    if (RunOnce(config, repository, MakeEvents(3, 1), NextTime(), &child,
                "INC-RTR T3b")) {
      test_support::Check(child.kind == bp::RealtimeOutcome::Kind::kDelta,
                          "INC-RTR T3b 准备：新链上有一份 delta");
      test_support::Check(
          child.retention_deleted == 0 && child.retention_kept_ancestors >= 1,
          "INC-RTR T3b 判别：可见 delta 的父被留下来当祖先",
          std::to_string(child.retention_deleted) + "/" +
              std::to_string(child.retention_kept_ancestors));
      test_support::Check(
          Exists(repository + "/" + new_baseline.snapshot_file_name) &&
              Exists(repository + "/" + child.snapshot_file_name),
          "INC-RTR T3b 基线与 delta 都还在");
    }
  }

  // ============================================================
  test_support::Section(
      "INC-RTR 4. 跨 Trigger：还有活着的非 realtime 后代时，删除被拒绝");
  // ============================================================
  {
    const std::string work = test_support::FreshDir("realtime-retention-cross");
    const std::string source = work + "/src";
    const std::string repository = work + "/repo";
    test_support::Mkdir(source, 0755);
    test_support::Mkdir(repository, 0755);
    test_support::WriteFile(source + "/a.txt", "v0\n", 0644);

    bp::RealtimeConfig config =
        MakeConfig(source, bp::BackupStrategy::kIncremental, 1);
    bp::RealtimeOutcome realtime_baseline;
    RunOnce(config, repository, MakeEvents(1, 0, true), NextTime(),
            &realtime_baseline, "INC-RTR T4");

    // Manual / Scheduled 触发的后代：没有 realtime marker，但真的依赖那份基线。
    // 先改一次源，否则手工 delta 会（正确地）报告 no-changes。
    test_support::WriteFile(source + "/a.txt", "v1\n", 0644);
    std::string manual_name;
    std::string error;
    test_support::Check(
        MakeManualDelta(source, repository,
                        realtime_baseline.snapshot_file_name, &manual_name,
                        &error),
        "INC-RTR T4 手工 delta 挂在 realtime 基线上（跨 Trigger 依赖）", error);

    test_support::WriteFile(source + "/a.txt", "v2\n", 0644);
    bp::RealtimeOutcome realtime_child;
    if (RunOnce(config, repository, MakeEvents(2, 1), NextTime(),
                &realtime_child, "INC-RTR T4")) {
      test_support::Check(
          realtime_child.retention_deleted == 0,
          "INC-RTR T4 判别：有活着的非 realtime 后代 -> 一份都不删",
          std::to_string(realtime_child.retention_deleted));
      // F 之后这里不再是 uncertain：保留点的依赖链把那份基线保了下来，属于
      // 健康的 dependency retention（"删除被拒绝"的证据在下面直接调
      // DeleteSnapshots 的那一条）。
      test_support::Check(
          !realtime_child.retention_uncertain &&
              realtime_child.retention_kept_ancestors >= 1,
          "INC-RTR T4 判别：跨 Trigger 的祖先被当成依赖保留，不再是 "
          "uncertainty",
          std::to_string(realtime_child.retention_kept_ancestors));
      test_support::Check(
          Exists(repository + "/" + realtime_baseline.snapshot_file_name) &&
              Exists(repository + "/" + manual_name),
          "INC-RTR T4 跨 Trigger 的祖先仍然在（没有被误删成断链）");
      std::vector<std::string> descendants;
      std::string descendant_error;
      if (bp::FindReachableDescendants(repository,
                                       realtime_baseline.snapshot_file_name,
                                       &descendants, &descendant_error)) {
        bool found_manual = false;
        for (const std::string& name : descendants) {
          if (name == manual_name) found_manual = true;
        }
        test_support::Check(
            found_manual,
            "INC-RTR T4 判别的依据：手工 delta 是那份基线的可达后代");
      } else {
        test_support::Check(false, "INC-RTR T4 读依赖图", descendant_error);
      }
    }
    // 直接把"删祖先"这件事单独问一次：DeleteSnapshots 也必须拒绝。
    {
      bp::BackupCatalog catalog;
      std::vector<std::string> removed;
      std::vector<std::string> diagnostics;
      std::string delete_error;
      const bool deleted = catalog.DeleteSnapshots(
          repository, {realtime_baseline.snapshot_file_name}, &removed,
          &diagnostics, &delete_error);
      test_support::Check(
          !deleted && removed.empty(),
          "INC-RTR T4 判别：有活后代时 DeleteSnapshots 拒绝整批删除",
          delete_error);
      test_support::Check(
          Exists(repository + "/" + realtime_baseline.snapshot_file_name),
          "INC-RTR T4 拒绝之后那份基线还在");
    }
  }

  // ============================================================
  test_support::Section(
      "INC-RTR 5. 依赖链读不出来 -> remove 为空（fail-closed）");
  // ============================================================
  {
    const std::string work =
        test_support::FreshDir("realtime-retention-broken");
    const std::string source = work + "/src";
    const std::string repository = work + "/repo";
    test_support::Mkdir(source, 0755);
    test_support::Mkdir(repository, 0755);
    test_support::WriteFile(source + "/a.txt", "v0\n", 0644);

    // 1) 同一 job identity 下"更旧的一份 realtime 候选"：它必须在保留点的
    //    依赖链之外，这样才会真的形成一份 remove 计划。
    bp::RealtimeConfig incr_config =
        MakeConfig(source, bp::BackupStrategy::kIncremental, 12);
    bp::RealtimeOutcome old_full;
    RunOnce(incr_config, repository, MakeEvents(1, 0, true), NextTime(),
            &old_full, "INC-RTR T5");
    // 让这份旧候选不再能当基线（副文件丢了）：下一个手工基线于是另起一条链。
    // 它的字节与 marker 都没坏，所以它仍然是一份 verified 的 realtime 快照。
    test_support::Check(
        ::unlink((repository + "/" +
                  bp::SnapshotIdentityFileName(old_full.snapshot_file_name))
                     .c_str()) == 0 &&
            ::unlink((repository + "/" +
                      bp::SnapshotManifestFileName(old_full.snapshot_file_name))
                         .c_str()) == 0,
        "INC-RTR T5 让旧候选不再可用作基线");

    // 2) 手工基线（没有 marker）+ 手工 delta：realtime 侧看不见的两跳。
    test_support::WriteFile(source + "/a.txt", "v1\n", 0644);
    std::string manual_baseline;
    std::string manual_delta;
    std::string error;
    test_support::Check(
        MakeManualBaseline(source, repository, &manual_baseline, &error),
        "INC-RTR T5 手工建基线", error);
    test_support::WriteFile(source + "/a.txt", "v2\n", 0644);
    test_support::Check(MakeManualDelta(source, repository, manual_baseline,
                                        &manual_delta, &error),
                        "INC-RTR T5 手工建 delta", error);

    // 3) realtime delta：它的依赖链要走 manual_delta -> manual_baseline。
    test_support::WriteFile(source + "/a.txt", "v3\n", 0644);
    bp::RealtimeOutcome realtime_delta;
    RunOnce(incr_config, repository, MakeEvents(2, 1), NextTime(),
            &realtime_delta, "INC-RTR T5");
    test_support::Check(
        realtime_delta.kind == bp::RealtimeOutcome::Kind::kDelta,
        "INC-RTR T5 准备：realtime 侧写了一份 delta");

    // 4) 把链底那份手工基线的字节弄坏：保留点 D 的依赖链从此走不通。
    test_support::WriteFile(repository + "/" + manual_baseline,
                            "this is not an archive anymore", 0644);

    bp::RealtimeRetentionResult result;
    std::string retention_error;
    test_support::Check(bp::RunRealtimeRetention(
                            repository, JobIdentityOf(incr_config, repository),
                            1, &result, &retention_error),
                        "INC-RTR T5 retention 可调用", retention_error);
    test_support::Check(
        result.uncertain && result.deleted == 0,
        "INC-RTR T5 判别：依赖链读不出来时 remove 为空、一份都不删",
        result.reason);
    test_support::Check(!result.reason.empty(),
                        "INC-RTR T5 不给理由就不算 fail-closed", result.reason);
    test_support::Check(
        Exists(repository + "/" + old_full.snapshot_file_name) &&
            Exists(repository + "/" + manual_baseline) &&
            Exists(repository + "/" + manual_delta) &&
            Exists(repository + "/" + realtime_delta.snapshot_file_name),
        "INC-RTR T5 四份快照一份都没少");
  }

  // ============================================================
  test_support::Section("INC-RTR 6. 孤儿 marker 与空仓库");
  // ============================================================
  {
    const std::string work =
        test_support::FreshDir("realtime-retention-orphan");
    const std::string source = work + "/src";
    const std::string repository = work + "/repo";
    test_support::Mkdir(source, 0755);
    test_support::Mkdir(repository, 0755);
    test_support::WriteFile(source + "/a.txt", "v0\n", 0644);

    bp::RealtimeConfig config =
        MakeConfig(source, bp::BackupStrategy::kFull, 1);
    bp::RealtimeRetentionResult empty_result;
    std::string error;
    test_support::Check(
        bp::RunRealtimeRetention(repository, JobIdentityOf(config, repository),
                                 1, &empty_result, &error),
        "INC-RTR T6 空仓库上 retention 不是错误", error);
    test_support::Check(empty_result.deleted == 0 && !empty_result.uncertain,
                        "INC-RTR T6 空仓库：没得删，也不是不确定");

    bp::RealtimeOutcome first;
    RunOnce(config, repository, MakeEvents(1, 0, true), NextTime(), &first,
            "INC-RTR T6");
    test_support::Check(
        ::unlink((repository + "/" + first.snapshot_file_name).c_str()) == 0,
        "INC-RTR T6 造一个孤儿 marker（归档没了，marker 还在）");

    test_support::WriteFile(source + "/a.txt", "v1\n", 0644);
    bp::RealtimeOutcome second;
    if (RunOnce(config, repository, MakeEvents(2, 1), NextTime(), &second,
                "INC-RTR T6")) {
      test_support::Check(
          second.retention_deleted == 0 && second.retention_uncertain,
          "INC-RTR T6 判别：孤儿 marker 让本轮 retention 整轮不动",
          std::to_string(second.retention_deleted));
      test_support::Check(
          Exists(MarkerPath(repository, first.snapshot_file_name)),
          "INC-RTR T6 孤儿 marker 不被顺手删掉（那是别人的诊断线索）");
      test_support::Check(
          Exists(repository + "/" + second.snapshot_file_name) &&
              Exists(MarkerPath(repository, second.snapshot_file_name)),
          "INC-RTR T6 新快照与它的 marker 都在");
      const std::vector<bp::RealtimeSnapshotRecord> records =
          ListRealtime(repository);
      int unverified = 0;
      for (const bp::RealtimeSnapshotRecord& record : records) {
        if (!record.verified) ++unverified;
      }
      test_support::Check(
          records.size() == 2 && unverified == 1,
          "INC-RTR T6 history 把孤儿 marker 如实标成 unverified");
    }
  }

  // ============================================================
  test_support::Section(
      "INC-RTR 7. 健康链的 remove-empty 不是 uncertainty（F）");
  // ============================================================
  {
    const std::string work =
        test_support::FreshDir("realtime-retention-healthy");
    const std::string source = work + "/src";
    const std::string repository = work + "/repo";
    test_support::Mkdir(source, 0755);
    test_support::Mkdir(repository, 0755);
    test_support::WriteFile(source + "/a.txt", "v0\n", 0644);

    // 先用宽 retain 建一条健康的增量链 F0 -> D1 -> D2。
    bp::RealtimeConfig config =
        MakeConfig(source, bp::BackupStrategy::kIncremental, 12);
    bp::RealtimeOutcome baseline;
    RunOnce(config, repository, MakeEvents(1, 0, true), NextTime(), &baseline,
            "INC-RTR F1");
    test_support::WriteFile(source + "/a.txt", "v1\n", 0644);
    bp::RealtimeOutcome delta1;
    RunOnce(config, repository, MakeEvents(2, 1), NextTime(), &delta1,
            "INC-RTR F1");
    test_support::WriteFile(source + "/a.txt", "v2\n", 0644);
    bp::RealtimeOutcome delta2;
    RunOnce(config, repository, MakeEvents(3, 1), NextTime(), &delta2,
            "INC-RTR F1");
    test_support::Check(delta2.kind == bp::RealtimeOutcome::Kind::kDelta,
                        "INC-RTR F1 准备：链上有两份 delta");

    // 直接问 retention：retain=1，可见点是 D2，它依赖 D1 与 F0。
    bp::RealtimeRetentionResult result;
    std::string error;
    test_support::Check(
        bp::RunRealtimeRetention(repository, JobIdentityOf(config, repository),
                                 1, &result, &error),
        "INC-RTR F1 retention 可调用", error);
    test_support::Check(
        !result.uncertain,
        "INC-RTR F1 判别：健康的 dependency retention 不是 uncertain",
        result.reason);
    test_support::Check(result.deleted == 0,
                        "INC-RTR F1 判别：这一轮确实没有可删的东西",
                        std::to_string(result.deleted));
    test_support::Check(result.kept_visible == 1 && result.kept_ancestors >= 2,
                        "INC-RTR F1 判别：可见点与祖先都按真实计数保留",
                        std::to_string(result.kept_visible) + "/" +
                            std::to_string(result.kept_ancestors));
    test_support::Check(result.reason.empty(),
                        "INC-RTR F1 判别：healthy 不给出 warning 理由",
                        result.reason);

    // 服务路径同样不能把这件事报成 warning。
    config.retain_count = 1;
    test_support::WriteFile(source + "/a.txt", "v3\n", 0644);
    bp::RealtimeOutcome third;
    if (RunOnce(config, repository, MakeEvents(4, 1), NextTime(), &third,
                "INC-RTR F2")) {
      test_support::Check(
          !third.retention_uncertain,
          "INC-RTR F2 判别：service 侧 retention_uncertain == false");
      test_support::Check(
          third.retention_deleted == 0 && third.retention_kept_ancestors >= 2,
          "INC-RTR F2 判别：祖先保留计数真实，删除数为 0",
          std::to_string(third.retention_deleted) + "/" +
              std::to_string(third.retention_kept_ancestors));
      test_support::Check(
          third.diagnostic.find("retention warning") == std::string::npos,
          "INC-RTR F2 判别：不产生 retention warning 文案", third.diagnostic);
      test_support::Check(
          Exists(repository + "/" + baseline.snapshot_file_name) &&
              Exists(repository + "/" + delta1.snapshot_file_name) &&
              Exists(repository + "/" + delta2.snapshot_file_name),
          "INC-RTR F2 链仍然完整");
    }
  }

  return test_support::Finish("realtime_retention_test");
}
