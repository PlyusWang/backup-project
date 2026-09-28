// realtime_backup_test.cpp
//
// PR #19：一次已经稳定的 realtime 触发**做了什么**，以及 realtime 快照的归属
// 是不是真的绑在实际 archive bytes 上。
//
// 要钉住的（对应设计文档第 27 节的判别用例）：
//   * Realtime 只是 Trigger：Full 走 BackupEngine、Incremental 走
//     RunIncrementalBackup，两份策略语义各自由原实现负责；
//   * Full 是保守的：每个 settled generation 都建一份完整快照，excluded-only
//     也会建（内容里没有 excluded 文件）；
//   * Incremental 三选一如实报告：full-baseline / delta / no-changes；
//   * 同大小同 mtime 的改写靠强哈希识别成 delta；
//   * marker 只信实际字节：换掉 payload 之后 marker 立刻不再 verified；
//   * marker 发布失败 -> 归档保留 + warning + **不执行**破坏性 retention；
//   * 归属只认 job_identity：换源之后的旧快照不参与本轮淘汰；
//   * overflow / resync 只被如实记录，不假装成"改了 N 个文件"。

#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <vector>

#include "archive_pipeline.h"
#include "backup_catalog.h"
#include "backup_engine.h"
#include "backup_mode.h"
#include "filter.h"
#include "incremental_backup.h"
#include "incremental_restore.h"
#include "realtime_backup_service.h"
#include "test_support.h"

namespace bp = backupproject;

namespace {

constexpr std::int64_t kTimeBase = 1800000000;

// 每次调用都给一个更大的整秒：归档名与 marker.created_time_sec 都是秒粒度，
// 固定下来的时间让断言（排序、保留哪几份）可复现。
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
                                    bool resync = false,
                                    bool overflow = false) {
  bp::RealtimeEventSummary events;
  events.generation = generation;
  events.event_count = event_count;
  events.resync = resync;
  events.overflow = overflow;
  return events;
}

std::string JobIdentityOf(const bp::RealtimeConfig& config,
                          const std::string& repository) {
  return bp::RealtimeJobIdentityDigest(
      config, bp::RepositoryIdentity(repository), config.source_path);
}

// 一次触发。失败时自己记一条 FAIL，调用方可以直接 return。
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
    return records;
  }
  return records;
}

bool RestoreTo(const std::string& repository, const std::string& file_name,
               const std::string& destination, std::string* error_message) {
  bp::RestoreOptions options;
  bp::RestoreReport report;
  return bp::RestoreSnapshotChain(repository, file_name, destination, options,
                                  &report, error_message);
}

// 仓库里直接子项的个数（用来证明"没有留下 temp / 孤儿 marker"）。
std::size_t RepoEntryCount(const std::string& repository) {
  return test_support::DirEntries(repository).size();
}

// 手工（非 realtime）建一份完整快照，用来验证"别人的东西不动"。
bool MakeManualFullSnapshot(const std::string& source,
                            const std::string& repository,
                            std::string* file_name, std::string* error) {
  bp::BackupCatalog catalog;
  if (!catalog.EnsureRepository(repository, error)) return false;
  std::string archive_path;
  if (!catalog.BuildArchivePath(repository, source, NextTime(), &archive_path,
                                error)) {
    return false;
  }
  bp::Filter filter;
  bp::BackupOptions options;
  options.pack_method = bp::PackMethod::kMyPack;
  options.compression_method = bp::CompressionMethod::kNone;
  options.encryption_method = bp::EncryptionMethod::kNone;
  bp::BackupEngine engine;
  if (!engine.Backup(source, archive_path, filter, options, error))
    return false;
  *file_name = archive_path.substr(archive_path.rfind('/') + 1);
  return true;
}

}  // namespace

int main() {
  // ============================================================
  test_support::Section(
      "INC-RTB 1. Realtime + Full：每个 settled generation 一份完整快照");
  // ============================================================
  {
    const std::string work = test_support::FreshDir("realtime-backup-full");
    const std::string source = work + "/src";
    const std::string repository = work + "/repo";
    test_support::Mkdir(source, 0755);
    test_support::Mkdir(source + "/sub", 0755);
    test_support::Mkdir(repository, 0755);
    test_support::WriteFile(source + "/a.txt", "alpha\n", 0644);
    test_support::WriteFile(source + "/sub/b.txt", "bee\n", 0644);

    bp::RealtimeConfig config =
        MakeConfig(source, bp::BackupStrategy::kFull, 12);
    config.include_rules = {"ext:txt"};

    // ---- T1：resync 之后的第一份 ----
    bp::RealtimeOutcome first;
    if (RunOnce(config, repository, MakeEvents(1, 0, /*resync=*/true),
                NextTime(), &first, "INC-RTB T1")) {
      test_support::Check(
          first.kind == bp::RealtimeOutcome::Kind::kFullSnapshot,
          "INC-RTB T1 判别：Realtime+Full 建完整快照（不复制策略实现）");
      test_support::Check(first.summary_text == "创建完整实时快照",
                          "INC-RTB T1 面向用户的那句话 = 创建完整实时快照",
                          first.summary_text);
      test_support::Check(first.marker_written && !first.marker_warning,
                          "INC-RTB T1 marker 写成");
      test_support::Check(!first.snapshot_file_name.empty(),
                          "INC-RTB T1 快照有名字");

      bp::RealtimeMarker marker;
      std::string error;
      const bool loaded = bp::LoadRealtimeMarker(
          repository, first.snapshot_file_name, &marker, &error);
      test_support::Check(loaded, "INC-RTB T1 marker 读得回来", error);
      if (loaded) {
        bp::SnapshotIdentity identity;
        std::string identity_error;
        const bool verified = bp::LoadVerifiedSnapshotIdentity(
            repository, first.snapshot_file_name, &identity, nullptr,
            &identity_error);
        test_support::Check(verified, "INC-RTB T1 快照字节可验证",
                            identity_error);
        test_support::Check(
            verified && marker.snapshot_id == identity.snapshot_id,
            "INC-RTB T1 判别：marker 绑定的是**实际 archive bytes** 的 "
            "verified id");
        test_support::Check(
            marker.job_identity == JobIdentityOf(config, repository),
            "INC-RTB T1 marker 的归属 = 当前 job identity");
        test_support::Check(
            !marker.source_identity.empty() && !marker.filter_identity.empty(),
            "INC-RTB T1 marker 记下源/规则身份");
        test_support::Check(marker.outcome_kind == "full",
                            "INC-RTB T1 marker outcome_kind=full");
        test_support::Check(marker.resync_trigger && marker.event_count == 0,
                            "INC-RTB T1 resync 被如实记录（event_count=0）");
        test_support::Check(
            marker.strategy == bp::BackupStrategy::kFull &&
                marker.pack_method == bp::PackMethod::kMyPack &&
                marker.compression_method == bp::CompressionMethod::kNone,
            "INC-RTB T1 marker 记下策略与 pipeline 选项");
      }
    }

    // ---- T2：第二次触发（真实事件） ----
    test_support::WriteFile(source + "/a.txt", "alpha-2\n", 0644);
    bp::RealtimeOutcome second;
    if (RunOnce(config, repository, MakeEvents(2, 3), NextTime(), &second,
                "INC-RTB T2")) {
      test_support::Check(
          second.kind == bp::RealtimeOutcome::Kind::kFullSnapshot,
          "INC-RTB T2 每个 settled generation 都建 Full");
      test_support::Check(second.snapshot_file_name != first.snapshot_file_name,
                          "INC-RTB T2 两份快照名字不同");
      bp::RealtimeMarker marker;
      std::string error;
      if (bp::LoadRealtimeMarker(repository, second.snapshot_file_name, &marker,
                                 &error)) {
        test_support::Check(marker.event_count == 3 && !marker.resync_trigger,
                            "INC-RTB T2 marker 记下真实事件数且不是 resync");
      } else {
        test_support::Check(false, "INC-RTB T2 marker 读得回来", error);
      }
    }

    // 内容正确性：恢复最新快照，a.txt 必须是新内容。
    {
      const std::string destination = work + "/restore-2";
      std::string error;
      test_support::Check(
          RestoreTo(repository, second.snapshot_file_name, destination, &error),
          "INC-RTB T2 最新快照可恢复", error);
      std::string content;
      test_support::Check(
          test_support::ReadFile(destination + "/a.txt", &content) &&
              content == "alpha-2\n",
          "INC-RTB T2 判别：恢复出来的内容 = 触发时的源树内容");
    }

    // ---- T3：excluded-only 也建 Full（保守），但内容里没有 excluded 文件 ----
    bp::RealtimeConfig excluded = config;
    excluded.exclude_rules = {"name:secret.log"};
    test_support::WriteFile(source + "/secret.log", "not backed up\n", 0644);
    bp::RealtimeOutcome third;
    if (RunOnce(excluded, repository, MakeEvents(3, 1), NextTime(), &third,
                "INC-RTB T3")) {
      test_support::Check(
          third.kind == bp::RealtimeOutcome::Kind::kFullSnapshot,
          "INC-RTB T3 判别：Full 是保守的，excluded-only 也建快照"
          "（不假装成 no-changes）");
      const std::string destination = work + "/restore-3";
      std::string error;
      if (RestoreTo(repository, third.snapshot_file_name, destination,
                    &error)) {
        test_support::Check(!test_support::Exists(destination + "/secret.log"),
                            "INC-RTB T3 判别：excluded 文件不在归档内容里");
        test_support::Check(
            test_support::Exists(destination + "/a.txt") &&
                test_support::Exists(destination + "/sub/b.txt"),
            "INC-RTB T3 included 文件仍在");
      } else {
        test_support::Check(false, "INC-RTB T3 快照可恢复", error);
      }
    }

    // ---- T4：history 与残留 ----
    {
      const std::vector<bp::RealtimeSnapshotRecord> records =
          ListRealtime(repository);
      test_support::Check(records.size() == 3,
                          "INC-RTB T4 history 里有 3 份 realtime 快照",
                          std::to_string(records.size()));
      bool all_verified = !records.empty();
      bool ordered = true;
      for (std::size_t index = 0; index < records.size(); ++index) {
        all_verified = all_verified && records[index].verified;
        if (index > 0 && records[index - 1].created_time_sec >
                             records[index].created_time_sec) {
          ordered = false;
        }
      }
      test_support::Check(all_verified, "INC-RTB T4 全部 marker 都 verified");
      test_support::Check(ordered, "INC-RTB T4 history 按 created_time 升序");
      const std::vector<std::string> entries =
          test_support::DirEntries(repository);
      bool no_temp = true;
      for (const std::string& name : entries) {
        if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".tmp") == 0) {
          no_temp = false;
        }
      }
      test_support::Check(no_temp, "INC-RTB T4 不留 .tmp 半成品");
      test_support::Check(RepoEntryCount(repository) == 6,
                          "INC-RTB T4 仓库里只有 3 份归档 + 3 个 marker",
                          std::to_string(RepoEntryCount(repository)));
    }

    // ---- T5：overflow / resync 只被如实记录 ----
    bp::RealtimeOutcome overflow;
    if (RunOnce(config, repository,
                MakeEvents(4, 42, /*resync=*/true,
                           /*overflow=*/true),
                NextTime(), &overflow, "INC-RTB T5")) {
      test_support::Check(
          overflow.kind == bp::RealtimeOutcome::Kind::kFullSnapshot,
          "INC-RTB T5 overflow 之后照样建快照");
      bp::RealtimeMarker marker;
      std::string error;
      if (bp::LoadRealtimeMarker(repository, overflow.snapshot_file_name,
                                 &marker, &error)) {
        test_support::Check(
            marker.overflow_recovery && marker.resync_trigger,
            "INC-RTB T5 marker 记下 overflow_recovery + resync_trigger");
        test_support::Check(marker.added == 0 && marker.removed == 0 &&
                                marker.modified == 0 &&
                                marker.metadata_changed == 0,
                            "INC-RTB T5 判别：Full 不假装知道「改了几个文件」");
      } else {
        test_support::Check(false, "INC-RTB T5 marker 读得回来", error);
      }
    }
  }

  // ============================================================
  test_support::Section(
      "INC-RTB 2. Realtime + Incremental：baseline / delta / no-changes");
  // ============================================================
  {
    const std::string work = test_support::FreshDir("realtime-backup-incr");
    const std::string source = work + "/src";
    const std::string repository = work + "/repo";
    test_support::Mkdir(source, 0755);
    test_support::Mkdir(repository, 0755);
    test_support::WriteFile(source + "/a.txt", "AAAA\n", 0644);
    test_support::WriteFile(source + "/b.txt", "BBBB\n", 0644);

    bp::RealtimeConfig config =
        MakeConfig(source, bp::BackupStrategy::kIncremental, 12);
    config.include_rules = {"ext:txt"};

    // ---- T1：没有基线 -> 完整基线 ----
    bp::RealtimeOutcome baseline;
    if (RunOnce(config, repository, MakeEvents(1, 0, true), NextTime(),
                &baseline, "INC-RTB I1")) {
      test_support::Check(
          baseline.kind == bp::RealtimeOutcome::Kind::kFullBaseline,
          "INC-RTB I1 判别：无基线时如实报告 full-baseline（不是 delta）");
      test_support::Check(
          baseline.summary_text == "实时增量策略建立了新的完整基线",
          "INC-RTB I1 判别：基线不会被说成 delta", baseline.summary_text);
      bp::RealtimeMarker marker;
      std::string error;
      if (bp::LoadRealtimeMarker(repository, baseline.snapshot_file_name,
                                 &marker, &error)) {
        test_support::Check(
            marker.outcome_kind == "full-baseline" &&
                marker.strategy == bp::BackupStrategy::kIncremental,
            "INC-RTB I1 marker 的 outcome_kind=full-baseline");
      } else {
        test_support::Check(false, "INC-RTB I1 marker 读得回来", error);
      }
      test_support::Check(
          test_support::Exists(
              repository + "/" +
              bp::SnapshotManifestFileName(baseline.snapshot_file_name)),
          "INC-RTB I1 基线写了 manifest 副文件");
    }

    // ---- T2：有变化 -> delta，父是基线 ----
    test_support::WriteFile(source + "/b.txt", "BBBB-2\n", 0644);
    bp::RealtimeOutcome delta;
    if (RunOnce(config, repository, MakeEvents(2, 2), NextTime(), &delta,
                "INC-RTB I2")) {
      test_support::Check(delta.kind == bp::RealtimeOutcome::Kind::kDelta,
                          "INC-RTB I2 判别：有变化时写 delta");
      test_support::Check(delta.summary_text == "实时增量快照已创建",
                          "INC-RTB I2 面向用户的那句话 = 实时增量快照已创建",
                          delta.summary_text);
      test_support::Check(delta.changes.modified + delta.changes.added +
                                  delta.changes.removed >=
                              1,
                          "INC-RTB I2 delta 如实报告变化计数");
      std::string parent;
      std::string error;
      if (bp::SnapshotParentOf(repository, delta.snapshot_file_name, &parent,
                               &error)) {
        test_support::Check(parent == baseline.snapshot_file_name,
                            "INC-RTB I2 delta 的父是上一份基线");
      } else {
        test_support::Check(false, "INC-RTB I2 读 delta 的父", error);
      }
      const std::string destination = work + "/restore-delta";
      test_support::Check(
          RestoreTo(repository, delta.snapshot_file_name, destination, &error),
          "INC-RTB I2 delta 可沿链恢复", error);
      std::string content;
      test_support::Check(
          test_support::ReadFile(destination + "/b.txt", &content) &&
              content == "BBBB-2\n",
          "INC-RTB I2 判别：恢复出来的 delta 内容 = 当前源树");
    }

    // ---- T3：没有变化 -> 什么都不写 ----
    {
      const std::size_t before = RepoEntryCount(repository);
      bp::RealtimeOutcome nothing;
      if (RunOnce(config, repository, MakeEvents(3, 5), NextTime(), &nothing,
                  "INC-RTB I3")) {
        test_support::Check(
            nothing.kind == bp::RealtimeOutcome::Kind::kNoChanges,
            "INC-RTB I3 判别：有效备份集合没变 -> no-changes");
        test_support::Check(
            nothing.summary_text ==
                "检测到文件系统事件，但有效备份集合没有变化；未创建快照",
            "INC-RTB I3 判别：no-changes 的文案不冒充「已创建快照」",
            nothing.summary_text);
        test_support::Check(
            !nothing.marker_written && nothing.snapshot_file_name.empty(),
            "INC-RTB I3 no-changes 不写 marker、不写快照");
        test_support::Check(RepoEntryCount(repository) == before,
                            "INC-RTB I3 仓库一个文件都没多",
                            std::to_string(RepoEntryCount(repository)) +
                                " vs " + std::to_string(before));
      }
    }

    // ---- T4：同大小同 mtime 的改写仍然被识别 ----
    {
      struct stat info;
      test_support::Check(test_support::StatOf(source + "/a.txt", &info),
                          "INC-RTB I4 取 a.txt 的 mtime");
      test_support::WriteFile(source + "/a.txt", "ZZZZ\n",
                              0644);  // 4 字节，同大小
      test_support::SetTimes(source + "/a.txt", info.st_mtim.tv_sec,
                             static_cast<std::uint32_t>(info.st_mtim.tv_nsec));
      bp::RealtimeOutcome same_size;
      if (RunOnce(config, repository, MakeEvents(4, 1), NextTime(), &same_size,
                  "INC-RTB I4")) {
        test_support::Check(
            same_size.kind == bp::RealtimeOutcome::Kind::kDelta,
            "INC-RTB I4 判别：大小与 mtime 都没变，强哈希仍然识别成 delta");
        test_support::Check(same_size.changes.modified >= 1,
                            "INC-RTB I4 changes.modified >= 1",
                            std::to_string(same_size.changes.modified));
      }
    }

    // ---- T5：metadata-only 变化 ----
    {
      test_support::Check(::chmod((source + "/b.txt").c_str(), 0600) == 0,
                          "INC-RTB I5 造 metadata 变化");
      bp::RealtimeOutcome metadata;
      if (RunOnce(config, repository, MakeEvents(5, 1), NextTime(), &metadata,
                  "INC-RTB I5")) {
        test_support::Check(metadata.kind == bp::RealtimeOutcome::Kind::kDelta,
                            "INC-RTB I5 metadata-only 变化也是 delta");
        test_support::Check(
            metadata.changes.metadata_changed + metadata.changes.modified >= 1,
            "INC-RTB I5 变化计数里体现了 metadata");
      }
    }

    // ---- T6：删除文件 ----
    {
      test_support::Check(::unlink((source + "/b.txt").c_str()) == 0,
                          "INC-RTB I6 删掉 b.txt");
      bp::RealtimeOutcome removed;
      if (RunOnce(config, repository, MakeEvents(6, 1), NextTime(), &removed,
                  "INC-RTB I6")) {
        test_support::Check(removed.kind == bp::RealtimeOutcome::Kind::kDelta &&
                                removed.changes.removed >= 1,
                            "INC-RTB I6 删除也是 delta（tombstone）");
        const std::string destination = work + "/restore-removed";
        std::string error;
        if (RestoreTo(repository, removed.snapshot_file_name, destination,
                      &error)) {
          test_support::Check(!test_support::Exists(destination + "/b.txt") &&
                                  test_support::Exists(destination + "/a.txt"),
                              "INC-RTB I6 判别：删除在恢复结果里生效");
        } else {
          test_support::Check(false, "INC-RTB I6 快照可恢复", error);
        }
      }
    }

    // ---- T7：任意 restore point 都能恢复 ----
    {
      const std::vector<bp::RealtimeSnapshotRecord> records =
          ListRealtime(repository);
      bool all_restore = !records.empty();
      std::string detail;
      for (const bp::RealtimeSnapshotRecord& record : records) {
        const std::string destination =
            work + "/restore-all-" + record.file_name;
        std::string error;
        if (!RestoreTo(repository, record.file_name, destination, &error)) {
          all_restore = false;
          detail = record.file_name + ": " + error;
        }
      }
      test_support::Check(all_restore,
                          "INC-RTB I7 每个 realtime restore point 都能恢复",
                          detail);
    }
  }

  // ============================================================
  test_support::Section("INC-RTB 3. marker 信任与发布失败");
  // ============================================================
  {
    const std::string work = test_support::FreshDir("realtime-marker-trust");
    const std::string source = work + "/src";
    const std::string repository = work + "/repo";
    test_support::Mkdir(source, 0755);
    test_support::Mkdir(repository, 0755);
    test_support::WriteFile(source + "/a.txt", "one\n", 0644);

    bp::RealtimeConfig config =
        MakeConfig(source, bp::BackupStrategy::kFull, 12);

    bp::RealtimeOutcome first;
    RunOnce(config, repository, MakeEvents(1, 0, true), NextTime(), &first,
            "INC-RTB M1");

    // ---- T1：换掉 payload -> marker 立刻不再 verified ----
    {
      const std::string archive = repository + "/" + first.snapshot_file_name;
      std::string original;
      test_support::Check(test_support::ReadFile(archive, &original),
                          "INC-RTB M1 读原始归档");
      test_support::WriteFile(archive, original + "tampered", 0644);
      const std::vector<bp::RealtimeSnapshotRecord> records =
          ListRealtime(repository);
      bool found_unverified = false;
      for (const bp::RealtimeSnapshotRecord& record : records) {
        if (record.file_name == first.snapshot_file_name && !record.verified &&
            !record.diagnostic.empty()) {
          found_unverified = true;
        }
      }
      test_support::Check(found_unverified,
                          "INC-RTB M1 判别：archive bytes 被换掉之后 marker "
                          "不再 verified（带原因）");
      bp::RealtimeRetentionResult retention;
      std::string error;
      test_support::Check(bp::RunRealtimeRetention(
                              repository, JobIdentityOf(config, repository), 1,
                              &retention, &error),
                          "INC-RTB M1 retention 可调用", error);
      test_support::Check(
          retention.uncertain && retention.deleted == 0,
          "INC-RTB M1 判别：有不可验证的 marker -> 本轮一份都不删");
      test_support::WriteFile(archive, original, 0644);
    }

    // ---- T2：marker 缺失 = 不是 realtime 快照 ----
    {
      const std::string marker_path =
          repository + "/" +
          bp::RealtimeMarkerFileName(first.snapshot_file_name);
      test_support::Check(::unlink(marker_path.c_str()) == 0,
                          "INC-RTB M2 删掉 marker");
      test_support::Check(
          ListRealtime(repository).empty(),
          "INC-RTB M2 判别：没有 marker 的 .bak 不算 realtime 快照");
      test_support::Check(
          test_support::Exists(repository + "/" + first.snapshot_file_name),
          "INC-RTB M2 归档本身还在（marker 只是归属）");
    }

    // ---- T3：marker 损坏 -> 记录 unverified，绝不当作可信记录 ----
    {
      bp::RealtimeOutcome second;
      RunOnce(config, repository, MakeEvents(2, 1), NextTime(), &second,
              "INC-RTB M3");
      const std::string marker_path =
          repository + "/" +
          bp::RealtimeMarkerFileName(second.snapshot_file_name);
      test_support::WriteFile(marker_path, "BPREALTIME1\nsnapshot=???\n", 0600);
      const std::vector<bp::RealtimeSnapshotRecord> records =
          ListRealtime(repository);
      bool found = false;
      for (const bp::RealtimeSnapshotRecord& record : records) {
        if (record.file_name == second.snapshot_file_name) {
          found = true;
          test_support::Check(
              !record.verified && !record.diagnostic.empty(),
              "INC-RTB M3 判别：坏 marker 只是 unverified + 诊断，"
              "不是被当成好记录");
        }
      }
      test_support::Check(found,
                          "INC-RTB M3 坏 marker 仍然出现在 history 里"
                          "（用户看得见，但不参与淘汰）");
    }

    // ---- T4：marker 发布失败 -> 归档保留 + 不执行破坏性 retention ----
    {
      const std::string work2 = test_support::FreshDir("realtime-marker-fail");
      const std::string repo2 = work2 + "/repo";
      const std::string src2 = work2 + "/src";
      test_support::Mkdir(repo2, 0755);
      test_support::Mkdir(src2, 0755);
      test_support::WriteFile(src2 + "/a.txt", "v1\n", 0644);
      bp::RealtimeConfig config2 =
          MakeConfig(src2, bp::BackupStrategy::kFull, 12);
      for (int index = 0; index < 3; ++index) {
        test_support::WriteFile(src2 + "/a.txt",
                                "v" + std::to_string(index + 2) + "\n", 0644);
        bp::RealtimeOutcome outcome;
        RunOnce(config2, repo2,
                MakeEvents(static_cast<std::uint64_t>(index + 1), 1),
                NextTime(), &outcome, "INC-RTB M4 准备");
      }
      const std::size_t before = RepoEntryCount(repo2);
      test_support::Check(before == 6,
                          "INC-RTB M4 准备：3 份快照 + 3 个 marker",
                          std::to_string(before));

      config2.retain_count = 1;
      bp::SetRealtimeMarkerWriteFailureForTesting(true);
      test_support::WriteFile(src2 + "/a.txt", "v9\n", 0644);
      bp::RealtimeOutcome failed;
      const bool ran = RunOnce(config2, repo2, MakeEvents(4, 1), NextTime(),
                               &failed, "INC-RTB M4");
      bp::SetRealtimeMarkerWriteFailureForTesting(false);
      if (ran) {
        test_support::Check(
            failed.kind == bp::RealtimeOutcome::Kind::kFullSnapshot &&
                !failed.marker_written && failed.marker_warning,
            "INC-RTB M4 判别：marker 写失败 = 成功但带 ownership warning");
        test_support::Check(!failed.diagnostic.empty(),
                            "INC-RTB M4 失败原因说清楚了", failed.diagnostic);
        test_support::Check(
            failed.retention_deleted == 0,
            "INC-RTB M4 判别：没有可信 marker 就不执行破坏性 retention");
        test_support::Check(RepoEntryCount(repo2) == before + 1,
                            "INC-RTB M4 归档保留（4 份 .bak + 3 个 marker）",
                            std::to_string(RepoEntryCount(repo2)));
        test_support::Check(
            test_support::Exists(repo2 + "/" + failed.snapshot_file_name),
            "INC-RTB M4 新归档还在仓库里");
      }

      // marker 恢复可写之后，retention 才真的开始淘汰。
      test_support::WriteFile(src2 + "/a.txt", "v10\n", 0644);
      bp::RealtimeOutcome recovered;
      if (RunOnce(config2, repo2, MakeEvents(5, 1), NextTime(), &recovered,
                  "INC-RTB M4b")) {
        test_support::Check(recovered.marker_written,
                            "INC-RTB M4b marker 恢复可写");
        test_support::Check(recovered.retention_deleted >= 1,
                            "INC-RTB M4b 判别：归属可信之后 retention 才删",
                            std::to_string(recovered.retention_deleted));
      }
    }

    // ---- T5：归属只认 job_identity ----
    {
      const std::string work3 = test_support::FreshDir("realtime-job-identity");
      const std::string repo3 = work3 + "/repo";
      const std::string src_a = work3 + "/src-a";
      const std::string src_b = work3 + "/src-b";
      test_support::Mkdir(repo3, 0755);
      test_support::Mkdir(src_a, 0755);
      test_support::Mkdir(src_b, 0755);
      test_support::WriteFile(src_a + "/a.txt", "a\n", 0644);
      test_support::WriteFile(src_b + "/b.txt", "b\n", 0644);

      bp::RealtimeConfig config_a =
          MakeConfig(src_a, bp::BackupStrategy::kFull, 1);
      bp::RealtimeConfig config_b =
          MakeConfig(src_b, bp::BackupStrategy::kFull, 1);
      bp::RealtimeOutcome first_a;
      bp::RealtimeOutcome first_b;
      RunOnce(config_a, repo3, MakeEvents(1, 0, true), NextTime(), &first_a,
              "INC-RTB M5");
      RunOnce(config_b, repo3, MakeEvents(1, 0, true), NextTime(), &first_b,
              "INC-RTB M5");
      test_support::Check(
          JobIdentityOf(config_a, repo3) != JobIdentityOf(config_b, repo3),
          "INC-RTB M5 两个源 = 两个 job identity");
      bp::RealtimeRetentionResult retention;
      std::string error;
      bp::RunRealtimeRetention(repo3, JobIdentityOf(config_b, repo3), 1,
                               &retention, &error);
      test_support::Check(
          retention.deleted == 0,
          "INC-RTB M5 判别：另一个 job 的 realtime 快照不参与本轮淘汰");
      test_support::Check(
          test_support::Exists(repo3 + "/" + first_a.snapshot_file_name) &&
              test_support::Exists(repo3 + "/" + first_b.snapshot_file_name),
          "INC-RTB M5 两份快照都还在");
    }
  }

  // ============================================================
  test_support::Section("INC-RTB 4. 重启 catch-up（resync 捕获当前状态）");
  // ============================================================
  {
    const std::string work = test_support::FreshDir("realtime-restart");
    const std::string source = work + "/src";
    const std::string repository = work + "/repo";
    test_support::Mkdir(source, 0755);
    test_support::Mkdir(repository, 0755);
    test_support::WriteFile(source + "/a.txt", "before\n", 0644);

    bp::RealtimeConfig full = MakeConfig(source, bp::BackupStrategy::kFull, 12);
    bp::RealtimeOutcome first;
    RunOnce(full, repository, MakeEvents(1, 0, true), NextTime(), &first,
            "INC-RTB R1");

    // 进程"停着"的这段时间里改了很多次，没有任何事件被记录。
    test_support::WriteFile(source + "/a.txt", "after-1\n", 0644);
    test_support::WriteFile(source + "/c.txt", "after-2\n", 0644);
    test_support::Mkdir(source + "/newdir", 0755);
    test_support::WriteFile(source + "/newdir/d.txt", "after-3\n", 0644);

    bp::RealtimeOutcome catchup;
    if (RunOnce(full, repository, MakeEvents(1, 0, /*resync=*/true), NextTime(),
                &catchup, "INC-RTB R1")) {
      test_support::Check(
          catchup.kind == bp::RealtimeOutcome::Kind::kFullSnapshot,
          "INC-RTB R1 判别：重启后一次 resync 捕获当前状态");
      const std::string destination = work + "/restore-full";
      std::string error;
      if (RestoreTo(repository, catchup.snapshot_file_name, destination,
                    &error)) {
        std::string content;
        test_support::Check(
            test_support::ReadFile(destination + "/a.txt", &content) &&
                content == "after-1\n" &&
                test_support::Exists(destination + "/c.txt") &&
                test_support::Exists(destination + "/newdir/d.txt"),
            "INC-RTB R1 停机期间的三次改写都被捕获");
      } else {
        test_support::Check(false, "INC-RTB R1 快照可恢复", error);
      }
    }

    // Incremental 侧同样：resync 之后按当前状态产出 delta / 基线。
    {
      const std::string work2 = test_support::FreshDir("realtime-restart-incr");
      const std::string src2 = work2 + "/src";
      const std::string repo2 = work2 + "/repo";
      test_support::Mkdir(src2, 0755);
      test_support::Mkdir(repo2, 0755);
      test_support::WriteFile(src2 + "/a.txt", "one\n", 0644);
      bp::RealtimeConfig incr =
          MakeConfig(src2, bp::BackupStrategy::kIncremental, 12);
      bp::RealtimeOutcome baseline;
      RunOnce(incr, repo2, MakeEvents(1, 0, true), NextTime(), &baseline,
              "INC-RTB R2");
      test_support::WriteFile(src2 + "/a.txt", "two\n", 0644);
      test_support::WriteFile(src2 + "/b.txt", "new\n", 0644);
      bp::RealtimeOutcome catchup2;
      if (RunOnce(incr, repo2, MakeEvents(1, 0, true), NextTime(), &catchup2,
                  "INC-RTB R2")) {
        test_support::Check(
            catchup2.kind == bp::RealtimeOutcome::Kind::kDelta ||
                catchup2.kind == bp::RealtimeOutcome::Kind::kFullBaseline,
            "INC-RTB R2 resync 之后由增量引擎自己决定基线/delta");
        const std::string destination = work2 + "/restore";
        std::string error;
        if (RestoreTo(repo2, catchup2.snapshot_file_name, destination,
                      &error)) {
          std::string content;
          test_support::Check(
              test_support::ReadFile(destination + "/a.txt", &content) &&
                  content == "two\n" &&
                  test_support::Exists(destination + "/b.txt"),
              "INC-RTB R2 判别：重启 resync 之后的 restore point = 当前源树");
        } else {
          test_support::Check(false, "INC-RTB R2 快照可恢复", error);
        }
      }
    }
  }

  // ============================================================
  test_support::Section("INC-RTB 5. 别人建的快照不被 realtime retention 牵扯");
  // ============================================================
  {
    const std::string work = test_support::FreshDir("realtime-foreign");
    const std::string source = work + "/src";
    const std::string repository = work + "/repo";
    test_support::Mkdir(source, 0755);
    test_support::Mkdir(repository, 0755);
    test_support::WriteFile(source + "/a.txt", "one\n", 0644);

    bp::RealtimeConfig config =
        MakeConfig(source, bp::BackupStrategy::kFull, 1);
    bp::RealtimeOutcome first;
    RunOnce(config, repository, MakeEvents(1, 0, true), NextTime(), &first,
            "INC-RTB F1");
    std::string manual_name;
    std::string error;
    test_support::Check(
        MakeManualFullSnapshot(source, repository, &manual_name, &error),
        "INC-RTB F1 手工建一份非 realtime 快照", error);
    test_support::Check(!manual_name.empty(), "INC-RTB F1 手工快照有名字");

    test_support::WriteFile(source + "/a.txt", "two\n", 0644);
    bp::RealtimeOutcome second;
    if (RunOnce(config, repository, MakeEvents(2, 1), NextTime(), &second,
                "INC-RTB F2")) {
      test_support::Check(second.retention_deleted >= 1,
                          "INC-RTB F2 realtime 快照按 retain=1 淘汰",
                          std::to_string(second.retention_deleted));
      test_support::Check(
          test_support::Exists(repository + "/" + manual_name),
          "INC-RTB F2 判别：非 realtime 快照不在候选里，一份都没动");
      test_support::Check(
          test_support::Exists(repository + "/" +
                               bp::RealtimeMarkerFileName(manual_name)) ==
              false,
          "INC-RTB F2 手工快照本来就没有 marker（也没有被伪造一个出来）");
    }
  }

  return test_support::Finish("realtime_backup_test");
}
