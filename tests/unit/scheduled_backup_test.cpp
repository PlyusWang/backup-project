// scheduled_backup_test.cpp
//
// Scheduled + Full 服务的专项测试：时刻语义、变化检测驱动的快照、retention、
// history、single-runner 锁、以及稳定性。
//
// 全部用**假时钟** + 临时目录，所以"1 分钟计划"不需要真的等一分钟，
// 时钟回拨 / 前跳 / 溢出也能直接构造出来。

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include "archive_pipeline.h"
#include "backup_catalog.h"
#include "backup_engine.h"
#include "scheduled_backup_service.h"
#include "scheduler_lock.h"
#include "test_support.h"

namespace bp = backupproject;

namespace {

bool Write(const std::string& path, const std::string& content) {
  return test_support::WriteFile(path, content, 0644);
}

struct Env {
  std::string root;
  std::string source;
  std::string repository;
  std::string schedule_file;
};

// 默认一分钟一轮：测试用的是假时钟，间隔越短越容易把刻度排开。
Env MakeEnv(const std::string& name, std::uint32_t retain,
            std::uint32_t interval = 1) {
  Env env;
  env.root = test_support::FreshDir(name);
  env.source = env.root + "/source";
  env.repository = env.root + "/repository";
  env.schedule_file = env.root + "/schedule.json";
  test_support::Mkdir(env.source, 0755);
  test_support::Mkdir(env.repository, 0755);

  bp::ScheduleDocument document;
  document.config.enabled = true;
  document.config.source_path = env.source;
  document.config.retain_count = retain;
  document.config.interval_minutes = interval;
  bp::ScheduleStore store(env.schedule_file);
  std::string error;
  if (!store.Save(document, &error)) {
    test_support::Check(false, "ENV config saves", error);
  }
  return env;
}

void SaveConfig(const Env& env, const bp::ScheduleConfig& config) {
  bp::ScheduleDocument document;
  std::string error;
  bp::ScheduleStore store(env.schedule_file);
  if (store.Load(&document, &error) == bp::ScheduleLoadStatus::kError) {
    test_support::Check(false, "ENV config loads", error);
    return;
  }
  document.config = config;
  if (!store.Save(document, &error)) {
    test_support::Check(false, "ENV config saves", error);
  }
}

bp::ScheduleConfig LoadConfig(const Env& env) {
  bp::ScheduleDocument document;
  std::string error;
  bp::ScheduleStore store(env.schedule_file);
  store.Load(&document, &error);
  return document.config;
}

bp::ScheduleDocument LoadDocument(const Env& env) {
  bp::ScheduleDocument document;
  std::string error;
  bp::ScheduleStore store(env.schedule_file);
  if (store.Load(&document, &error) == bp::ScheduleLoadStatus::kError) {
    test_support::Check(false, "ENV document loads", error);
  }
  return document;
}

bool Evaluate(const Env& env, std::int64_t now,
              bp::ScheduleEvaluationResult* result, std::string* error) {
  bp::ScheduleStore store(env.schedule_file);
  bp::ScheduledBackupService service(env.repository, &store);
  return service.Evaluate(now, result, error);
}

std::vector<std::string> RepoArchives(const std::string& repository) {
  bp::BackupCatalog catalog;
  std::vector<bp::BackupRecord> records;
  std::string error;
  std::vector<std::string> names;
  if (!catalog.List(repository, &records, &error)) return names;
  for (const bp::BackupRecord& record : records) names.push_back(record.file_name);
  std::sort(names.begin(), names.end());
  return names;
}

std::string JoinNames(const std::vector<std::string>& names) {
  std::string joined;
  for (const std::string& name : names) {
    if (!joined.empty()) joined += " ";
    joined += name;
  }
  return joined;
}

// 跑一轮并断言状态。失败时把诊断一起打出来，免得只能看到"失败"两个字。
void ExpectStatus(const std::string& label, const Env& env, std::int64_t now,
                  bp::ScheduleEvaluationStatus expected,
                  bp::ScheduleEvaluationResult* result) {
  std::string error;
  const bool ok = Evaluate(env, now, result, &error);
  if (!ok) {
    test_support::Check(false, label, error);
    return;
  }
  test_support::Check(
      result->status == expected, label,
      std::string("status=") + bp::ScheduleEvaluationStatusKey(result->status) +
          " diagnostic=" + result->diagnostic);
}

// ---- A. 时刻语义 ----

void TestTimeSemantics() {
  test_support::Section("A. schedule time semantics");

  test_support::Check(bp::ScheduleNextRunTime(1000, 1) == 1060,
                      "TIME-01 next run is now + interval",
                      std::to_string(bp::ScheduleNextRunTime(1000, 1)));
  test_support::Check(bp::ScheduleNextRunTime(0, 60) == 3600,
                      "TIME-02 an hour is 3600 seconds",
                      std::to_string(bp::ScheduleNextRunTime(0, 60)));
  test_support::Check(
      bp::ScheduleNextRunTime(INT64_MAX - 10, bp::kMaxIntervalMinutes) ==
          INT64_MAX,
      "TIME-03 interval overflow saturates instead of wrapping");

  test_support::Check(bp::IsScheduleDue(100, 0, 60),
                      "TIME-04 a never-computed next run is due");
  test_support::Check(bp::IsScheduleDue(200, 100, 1),
                      "TIME-05 an overdue run is due");
  test_support::Check(!bp::IsScheduleDue(100, 130, 1),
                      "TIME-06 a run inside the interval is not due");
  test_support::Check(bp::IsScheduleDue(100, 100000, 1),
                      "TIME-07 a large clock backward jump makes it due");
  // 极端值只要求"不溢出不崩"，不要求给出某个特定答案：这里两个时间相差
  // 远超一个周期，按"next run 不可信"处理才是正确行为。
  test_support::Check(bp::IsScheduleDue(INT64_MIN + 1, INT64_MAX - 1, 1),
                      "TIME-08 extreme values are handled without overflow");
  test_support::Check(!bp::IsScheduleDue(INT64_MAX - 30, INT64_MAX - 1, 1),
                      "TIME-08b a huge next run close to now is still awaited");
  test_support::Check(bp::ScheduleNextRunTime(0, 1) == 60,
                      "TIME-09 the minimum interval is one minute");

  bp::ScheduleConfig config;
  config.source_path = "/tmp";
  config.interval_minutes = 0;
  std::string error;
  test_support::Check(!bp::ValidateScheduleConfig(config, &error) &&
                          error.find("interval") != std::string::npos,
                      "TIME-10 a zero interval is rejected", error);
  config.interval_minutes = bp::kMaxIntervalMinutes + 1;
  test_support::Check(!bp::ValidateScheduleConfig(config, &error),
                      "TIME-11 an interval above the bound is rejected", error);
  config.interval_minutes = 60;
  config.retain_count = 0;
  test_support::Check(!bp::ValidateScheduleConfig(config, &error),
                      "TIME-12 retain 0 is rejected", error);
  config.retain_count = bp::kMaxRetainCount + 1;
  test_support::Check(!bp::ValidateScheduleConfig(config, &error),
                      "TIME-13 retain above the bound is rejected", error);
  config.retain_count = 12;
  config.strategy = bp::BackupStrategy::kIncremental;
  test_support::Check(!bp::ValidateScheduleConfig(config, &error) &&
                          error.find("Unsupported backup mode") != std::string::npos,
                      "TIME-14 an unimplemented strategy is refused, not faked",
                      error);
  config.strategy = bp::BackupStrategy::kFull;
  config.trigger = bp::BackupTrigger::kRealtime;
  test_support::Check(!bp::ValidateScheduleConfig(config, &error),
                      "TIME-15 an unimplemented trigger is refused", error);
  config.trigger = bp::BackupTrigger::kScheduled;
  config.encryption_method = bp::EncryptionMethod::kAes256CtrHmacSha256;
  test_support::Check(
      !bp::ValidateScheduleConfig(config, &error) &&
          error.find("定时无人值守加密需要安全的密钥来源") != std::string::npos,
      "TIME-16 unattended encryption is refused with the documented reason",
      error);
}

// ---- B. 首次运行与 skip ----

void TestFirstRunAndSkip() {
  test_support::Section("B. first run, then skip when nothing changed");
  const Env env = MakeEnv("first-run", 12);
  Write(env.source + "/a.txt", "alpha");
  Write(env.source + "/b.txt", "beta");

  bp::ScheduleEvaluationResult result;
  ExpectStatus("RUN-01 the first run creates a snapshot", env, 1000,
               bp::ScheduleEvaluationStatus::kCreatedSnapshot, &result);
  test_support::Check(result.first_snapshot,
                      "RUN-02 the first run is marked as a first snapshot");
  test_support::Check(result.changes.added == 3 && result.changes.removed == 0 &&
                          result.changes.modified == 0 &&
                          result.changes.metadata_changed == 0,
                      "RUN-03 the first snapshot reports the whole set as added",
                      std::to_string(result.changes.added));
  test_support::Check(!result.archive_file_name.empty(),
                      "RUN-04 the snapshot has a file name");

  const std::vector<std::string> after_first = RepoArchives(env.repository);
  test_support::Check(after_first.size() == 1,
                      "RUN-05 exactly one archive exists",
                      JoinNames(after_first));
  bp::ScheduleDocument document = LoadDocument(env);
  test_support::Check(document.state.managed_snapshots.size() == 1,
                      "RUN-06 one managed snapshot is recorded");
  test_support::Check(document.state.history.size() == 1 &&
                          document.state.history[0].result ==
                              bp::ScheduleRunResult::kSuccessCreated,
                      "RUN-07 the history records success_created");
  test_support::Check(document.state.next_run_time_sec == 1060,
                      "RUN-08 next run is now + interval",
                      std::to_string(document.state.next_run_time_sec));

  // 没到点：什么都不做。
  bp::ScheduleEvaluationResult not_due;
  ExpectStatus("RUN-09 a run before the due time does nothing", env, 1030,
               bp::ScheduleEvaluationStatus::kNotDue, &not_due);
  test_support::Check(RepoArchives(env.repository).size() == 1,
                      "RUN-10 no archive was added before the due time");

  // 到点但没变化：skip。这是本 PR 的核心语义。
  bp::ScheduleEvaluationResult skipped;
  ExpectStatus("RUN-11 a due run without changes is skipped", env, 1060,
               bp::ScheduleEvaluationStatus::kSkippedNoChanges, &skipped);
  const std::vector<std::string> after_skip = RepoArchives(env.repository);
  test_support::Check(after_skip == after_first,
                      "RUN-12 skipping creates no new archive and no pseudo-rotation",
                      JoinNames(after_skip));
  document = LoadDocument(env);
  test_support::Check(document.state.history.size() == 2 &&
                          document.state.history[1].result ==
                              bp::ScheduleRunResult::kSkippedNoChanges,
                      "RUN-13 the skip is recorded in history");
  test_support::Check(document.state.managed_snapshots.size() == 1,
                      "RUN-14 the managed list did not change on a skip");
  test_support::Check(document.state.next_run_time_sec == 1120,
                      "RUN-15 next run advanced by one interval",
                      std::to_string(document.state.next_run_time_sec));
  test_support::Check(document.state.last_success_time_sec == 1000,
                      "RUN-16 last success time still points at the snapshot");
}

// ---- B2. "立即检查并运行" ----

void TestRunNowSemantics() {
  test_support::Section("B2. run now ignores the due time but keeps change detection");
  const Env env = MakeEnv("run-now", 12);
  Write(env.source + "/a.txt", "alpha");

  bp::ScheduleStore store(env.schedule_file);
  bp::ScheduledBackupService service(env.repository, &store);
  bp::ScheduleEvaluationResult result;
  std::string error;

  test_support::Check(service.EvaluateNow(1000, &result, &error),
                      "NOW-01 run now evaluates", error);
  test_support::Check(result.status ==
                          bp::ScheduleEvaluationStatus::kCreatedSnapshot,
                      "NOW-02 run now creates the first snapshot",
                      bp::ScheduleEvaluationStatusKey(result.status));

  // 还没到点，而且没有变化：run now 也必须是 skip，绝不能变成"强制备份"。
  test_support::Check(service.EvaluateNow(1010, &result, &error),
                      "NOW-03 run now evaluates again", error);
  test_support::Check(result.status ==
                          bp::ScheduleEvaluationStatus::kSkippedNoChanges,
                      "NOW-04 run now without changes is still skipped",
                      bp::ScheduleEvaluationStatusKey(result.status));
  test_support::Check(RepoArchives(env.repository).size() == 1,
                      "NOW-05 the skip produced no archive");

  // 同一个时刻的到期检查仍然会说"没到点"：force 只作用于 run now 这一条入口。
  test_support::Check(service.Evaluate(1010, &result, &error) &&
                          result.status ==
                              bp::ScheduleEvaluationStatus::kNotDue,
                      "NOW-06 the automatic path still respects the due time");

  Write(env.source + "/b.txt", "beta");
  test_support::Check(service.EvaluateNow(1020, &result, &error),
                      "NOW-07 run now evaluates after a change", error);
  test_support::Check(result.status ==
                              bp::ScheduleEvaluationStatus::kCreatedSnapshot &&
                          result.changes.added == 1,
                      "NOW-08 run now creates a snapshot when something changed",
                      bp::ScheduleEvaluationStatusKey(result.status));

  const bp::ScheduleDocument document = LoadDocument(env);
  test_support::Check(document.state.managed_snapshots.size() == 2,
                      "NOW-09 both snapshots are managed");
  test_support::Check(document.state.next_run_time_sec == 1080,
                      "NOW-10 next run is recomputed from the run-now time",
                      std::to_string(document.state.next_run_time_sec));
}

// ---- C. 各类变化 ----

void TestChangeKinds() {
  test_support::Section("C. every change kind creates a full snapshot");
  const Env env = MakeEnv("change-kinds", 12);
  Write(env.source + "/a.txt", "alpha");

  bp::ScheduleEvaluationResult result;
  ExpectStatus("CHG-01 baseline snapshot", env, 1000,
               bp::ScheduleEvaluationStatus::kCreatedSnapshot, &result);

  Write(env.source + "/b.txt", "beta");
  ExpectStatus("CHG-02 an added file creates a snapshot", env, 1060,
               bp::ScheduleEvaluationStatus::kCreatedSnapshot, &result);
  test_support::Check(result.changes.added == 1 && result.changes.modified == 0 &&
                          result.changes.removed == 0 &&
                          result.changes.metadata_changed == 0,
                      "CHG-03 added is exactly one",
                      std::to_string(result.changes.added) + "/" +
                          std::to_string(result.changes.metadata_changed));

  unlink((env.source + "/b.txt").c_str());
  ExpectStatus("CHG-04 a removed file creates a snapshot", env, 1120,
               bp::ScheduleEvaluationStatus::kCreatedSnapshot, &result);
  test_support::Check(result.changes.removed == 1 && result.changes.added == 0,
                      "CHG-05 removed is exactly one",
                      std::to_string(result.changes.removed));

  Write(env.source + "/a.txt", "alpha changed");
  ExpectStatus("CHG-06 a content change creates a snapshot", env, 1180,
               bp::ScheduleEvaluationStatus::kCreatedSnapshot, &result);
  test_support::Check(result.changes.modified == 1 &&
                          result.changes.metadata_changed == 0,
                      "CHG-07 a size change is modified",
                      std::to_string(result.changes.modified));

  test_support::Check(chmod((env.source + "/a.txt").c_str(), 0600) == 0,
                      "CHG-08 chmod succeeds");
  ExpectStatus("CHG-09 a metadata-only change creates a snapshot", env, 1240,
               bp::ScheduleEvaluationStatus::kCreatedSnapshot, &result);
  test_support::Check(result.changes.modified == 0 &&
                          result.changes.metadata_changed >= 1,
                      "CHG-10 a mode change is metadata_changed",
                      std::to_string(result.changes.metadata_changed));

  // 不存在的源目录：明确失败，并且仍然推进 next run（不会每 tick 重试）。
  const bp::ScheduleConfig saved = LoadConfig(env);
  bp::ScheduleConfig broken = saved;
  broken.source_path = env.root + "/does-not-exist";
  SaveConfig(env, broken);
  ExpectStatus("CHG-11 a missing source fails the run", env, 1300,
               bp::ScheduleEvaluationStatus::kFailed, &result);
  test_support::Check(result.diagnostic.find("does-not-exist") != std::string::npos,
                      "CHG-12 the failure names the missing path",
                      result.diagnostic);
  const bp::ScheduleDocument failed_document = LoadDocument(env);
  test_support::Check(failed_document.state.next_run_time_sec == 1360,
                      "CHG-13 a failed run still advances next run",
                      std::to_string(failed_document.state.next_run_time_sec));
  test_support::Check(!failed_document.state.history.empty() &&
                          failed_document.state.history.back().result ==
                              bp::ScheduleRunResult::kFailed,
                      "CHG-14 the failure is recorded in history");
  SaveConfig(env, saved);
}

// ---- D. 每份快照都是完整独立的 ----

void TestSnapshotsAreIndependent() {
  test_support::Section("D. every scheduled snapshot restores on its own");
  const Env env = MakeEnv("independent", 12);
  Write(env.source + "/one.txt", "first");
  bp::ScheduleEvaluationResult result;
  ExpectStatus("IND-01 snapshot one", env, 1000,
               bp::ScheduleEvaluationStatus::kCreatedSnapshot, &result);
  const std::string oldest = result.archive_file_name;

  Write(env.source + "/two.txt", "second");
  ExpectStatus("IND-02 snapshot two", env, 1060,
               bp::ScheduleEvaluationStatus::kCreatedSnapshot, &result);

  Write(env.source + "/three.txt", "third");
  ExpectStatus("IND-03 snapshot three", env, 1120,
               bp::ScheduleEvaluationStatus::kCreatedSnapshot, &result);

  // 把最旧的那一份**单独**搬到另一个目录再恢复："
  // 它不依赖仓库里的其它任何文件"这件事只有这样才证明得了。
  const std::string elsewhere = env.root + "/elsewhere";
  test_support::Mkdir(elsewhere, 0755);
  std::string bytes;
  test_support::Check(test_support::ReadFile(env.repository + "/" + oldest, &bytes),
                      "IND-04 the oldest archive is readable");
  const std::string lone = elsewhere + "/lone_copy.bak";
  test_support::Check(Write(lone, bytes), "IND-05 the oldest archive is copied out");

  const std::string restored = env.root + "/restored-oldest";
  bp::BackupEngine engine;
  std::string error;
  test_support::Check(engine.Restore(lone, restored, &error),
                      "IND-06 the oldest snapshot restores from a lone copy",
                      error);
  test_support::Check(test_support::Exists(restored + "/one.txt") &&
                          !test_support::Exists(restored + "/two.txt") &&
                          !test_support::Exists(restored + "/three.txt"),
                      "IND-07 the oldest snapshot holds exactly the state of its "
                      "own time (not a delta)");
  std::string content;
  test_support::ReadFile(restored + "/one.txt", &content);
  test_support::Check(content == "first",
                      "IND-08 the restored content matches that moment", content);

  // 最新的一份也必须能独立恢复。
  const std::vector<std::string> names = RepoArchives(env.repository);
  test_support::Check(names.size() == 3, "IND-09 three snapshots exist",
                      JoinNames(names));
  const std::string newest_dir = env.root + "/restored-newest";
  test_support::Check(engine.Restore(env.repository + "/" + names.back(),
                                     newest_dir, &error),
                      "IND-10 the newest snapshot restores", error);
  test_support::Check(test_support::Exists(newest_dir + "/three.txt"),
                      "IND-11 the newest snapshot holds the latest state");
}

// ---- E. retention ----

void TestRetention() {
  test_support::Section("E. retention only touches scheduled snapshots");

  {
    const Env env = MakeEnv("retain-12", 12);
    // 手工放一份"用户自己的"备份，并且把它的 mtime 设得比谁都老。
    const std::string manual = env.repository + "/manual_old.bak";
    Write(manual, "this is not a real archive, only a user file");
    test_support::SetTimes(manual, 100, 0);

    bp::ScheduleEvaluationResult result;
    std::string oldest_name;
    bool every_run_created = true;
    for (int index = 0; index < 13; ++index) {
      Write(env.source + "/file.txt", "version " + std::to_string(index));
      std::string error;
      if (!Evaluate(env, 1000 + index * 60, &result, &error) ||
          result.status != bp::ScheduleEvaluationStatus::kCreatedSnapshot) {
        every_run_created = false;
        test_support::Check(false, "RET-00 evaluation succeeds", error);
        break;
      }
      if (index == 0) oldest_name = result.archive_file_name;
    }
    test_support::Check(every_run_created,
                        "RET-01 thirteen changing runs each create a snapshot");
    const bp::ScheduleDocument document = LoadDocument(env);
    test_support::Check(document.state.managed_snapshots.size() == 12,
                        "RET-02 retain=12 keeps exactly twelve managed snapshots",
                        std::to_string(document.state.managed_snapshots.size()));
    const std::vector<std::string> names = RepoArchives(env.repository);
    test_support::Check(names.size() == 13,
                        "RET-03 the repository holds 12 scheduled + 1 manual",
                        JoinNames(names));
    test_support::Check(std::find(names.begin(), names.end(), "manual_old.bak") !=
                            names.end(),
                        "RET-04 an old manual backup is never removed");
    test_support::Check(!oldest_name.empty() &&
                            std::find(names.begin(), names.end(), oldest_name) ==
                                names.end(),
                        "RET-05 the oldest scheduled snapshot was deleted",
                        oldest_name);
    test_support::Check(document.state.history.size() == 13,
                        "RET-06 the history keeps every run",
                        std::to_string(document.state.history.size()));
  }

  {
    const Env env = MakeEnv("retain-1", 1);
    bp::ScheduleEvaluationResult result;
    for (int index = 0; index < 4; ++index) {
      Write(env.source + "/file.txt", "v" + std::to_string(index));
      std::string error;
      if (!Evaluate(env, 1000 + index * 60, &result, &error)) {
        test_support::Check(false, "RET-10 evaluation succeeds", error);
        break;
      }
      const bp::ScheduleDocument document = LoadDocument(env);
      if (document.state.managed_snapshots.size() != 1) {
        test_support::Check(false, "RET-11 retain=1 keeps exactly one snapshot",
                            std::to_string(
                                document.state.managed_snapshots.size()));
        break;
      }
    }
    test_support::Check(RepoArchives(env.repository).size() == 1,
                        "RET-11 retain=1 keeps exactly one snapshot",
                        JoinNames(RepoArchives(env.repository)));
  }

  {
    // 用户手动删掉一个 managed snapshot：下一次 reconcile 自愈。
    const Env env = MakeEnv("reconcile", 12);
    Write(env.source + "/a.txt", "a");
    bp::ScheduleEvaluationResult result;
    ExpectStatus("RET-20 snapshot one", env, 1000,
                 bp::ScheduleEvaluationStatus::kCreatedSnapshot, &result);
    const std::string first = result.archive_file_name;
    Write(env.source + "/b.txt", "b");
    ExpectStatus("RET-21 snapshot two", env, 1060,
                 bp::ScheduleEvaluationStatus::kCreatedSnapshot, &result);
    test_support::Check(LoadDocument(env).state.managed_snapshots.size() == 2,
                        "RET-22 two managed snapshots");
    test_support::Check(unlink((env.repository + "/" + first).c_str()) == 0,
                        "RET-23 the user deletes one archive by hand");
    ExpectStatus("RET-24 the next run reconciles without a catastrophic error",
                 env, 1120, bp::ScheduleEvaluationStatus::kSkippedNoChanges,
                 &result);
    test_support::Check(LoadDocument(env).state.managed_snapshots.size() == 1,
                        "RET-25 the stale managed record was removed",
                        std::to_string(
                            LoadDocument(env).state.managed_snapshots.size()));
  }

  {
    // 删除失败：新快照保留、旧 record 保留、状态是 retention warning。
    const Env env = MakeEnv("retention-failure", 1);
    bp::ScheduleStore store(env.schedule_file);
    bp::ScheduleDocument document;
    std::string error;
    store.Load(&document, &error);

    // 旧的那条 record 指向一个符号链接：BackupCatalog::Delete 会拒绝删除它。
    // 这是非 root 用户在可写仓库里唯一能构造出来的"删不掉"，比伪造返回值诚实。
    const std::string link_name = "ghost_20200101_000000.bak";
    test_support::CreateSymlink("/nonexistent-target",
                                env.repository + "/" + link_name);
    bp::ScheduledSnapshotRecord record;
    record.file_name = link_name;
    record.created_time_sec = 1;
    document.state.managed_snapshots.push_back(record);

    bp::ScheduledSnapshotRecord newer;
    newer.file_name = "newer_20200102_000000.bak";
    newer.created_time_sec = 2;
    Write(env.repository + "/" + newer.file_name, "placeholder");
    document.state.managed_snapshots.push_back(newer);
    test_support::Check(store.Save(document, &error),
                        "RET-30 the crafted document saves", error);

    bp::ScheduledBackupService service(env.repository, &store);
    bp::ScheduleDocument loaded;
    store.Load(&loaded, &error);
    std::uint64_t deleted = 0;
    std::uint64_t failed = 0;
    std::string retention_error;
    test_support::Check(
        !service.RunRetention(&loaded, &deleted, &failed, &retention_error),
        "RET-31 a deletion failure is reported");
    test_support::Check(deleted == 0 && failed == 1,
                        "RET-32 exactly one deletion failed",
                        std::to_string(deleted) + "/" + std::to_string(failed));
    test_support::Check(loaded.state.managed_snapshots.size() == 2,
                        "RET-33 the un-deletable record is kept for the next retry",
                        std::to_string(loaded.state.managed_snapshots.size()));
    test_support::Check(retention_error.find(link_name) != std::string::npos,
                        "RET-34 the retention error names the archive",
                        retention_error);
    test_support::Check(bp::StatusForRetention(true) ==
                            bp::ScheduleEvaluationStatus::kCreatedSnapshot,
                        "RET-35 a successful retention maps to created_snapshot");
    test_support::Check(
        bp::StatusForRetention(false) ==
            bp::ScheduleEvaluationStatus::kCreatedWithRetentionWarning,
        "RET-36 a failed retention maps to a warning, never to a failure");
    test_support::Check(
        std::string(bp::ScheduleRunResultKey(
            bp::ScheduleRunResult::kSuccessWithRetentionWarning)) ==
            "success_with_retention_warning",
        "RET-37 the warning has its own persisted key");
  }
}

// ---- F. 流水线矩阵 ----

void TestPipelineMatrix() {
  test_support::Section("F. scheduled snapshots across the pipeline matrix");
  struct Case {
    const char* label;
    bp::PackMethod pack;
    bp::CompressionMethod compression;
  };
  const Case cases[] = {
      {"mypack + none", bp::PackMethod::kMyPack, bp::CompressionMethod::kNone},
      {"ustar + huffman", bp::PackMethod::kUstar,
       bp::CompressionMethod::kHuffman},
      {"fast-ustar + lzss-huffman", bp::PackMethod::kFastUstar,
       bp::CompressionMethod::kLzssHuffman},
  };

  for (const Case& item : cases) {
    const std::string name =
        std::string("matrix-") + std::to_string(static_cast<int>(item.pack));
    const Env env = MakeEnv(name, 3);
    bp::ScheduleConfig config = LoadConfig(env);
    config.pack_method = item.pack;
    config.compression_method = item.compression;
    SaveConfig(env, config);

    Write(env.source + "/payload.txt", "payload for the matrix");
    bp::ScheduleEvaluationResult result;
    ExpectStatus(std::string("MAT-01 ") + item.label + " creates a snapshot", env,
                 1000, bp::ScheduleEvaluationStatus::kCreatedSnapshot, &result);

    const std::string archive = env.repository + "/" + result.archive_file_name;
    bp::ArchiveFileInfo info;
    std::string error;
    test_support::Check(bp::IdentifyArchiveFile(archive, &info, &error) &&
                            info.kind == bp::ArchiveFileInfo::Kind::kContainerV2 &&
                            info.pack_method == item.pack &&
                            info.compression_method == item.compression &&
                            info.encryption_method == bp::EncryptionMethod::kNone,
                        std::string("MAT-02 ") + item.label +
                            " is a v2 container with those methods",
                        error);

    const std::string restored = env.root + "/restored";
    bp::BackupEngine engine;
    test_support::Check(engine.Restore(archive, restored, &error),
                        std::string("MAT-03 ") + item.label + " restores", error);
    std::string content;
    test_support::ReadFile(restored + "/payload.txt", &content);
    test_support::Check(content == "payload for the matrix",
                        std::string("MAT-04 ") + item.label +
                            " restores the payload byte for byte",
                        content);
  }
}

// ---- G. 加密边界与锁 ----

void TestEncryptionBoundaryAndLock() {
  test_support::Section("G. encryption boundary and the single-runner lock");
  const Env env = MakeEnv("boundary", 12);
  bp::ScheduleConfig config = LoadConfig(env);
  config.encryption_method = bp::EncryptionMethod::kDesCbcHmacSha256;
  bp::ScheduleStore store(env.schedule_file);
  bp::ScheduleDocument document;
  document.config = config;
  std::string error;
  test_support::Check(!store.Save(document, &error) &&
                          error.find("定时无人值守加密需要安全的密钥来源") !=
                              std::string::npos,
                      "ENC-01 a schedule store refuses to persist encryption",
                      error);

  // 手写一份带 password 字段的 schedule.json 也不会被接受：解析层只认 schema，
  // 未知字段直接报错。这样"密码写进 schedule.json"这条路连门都没有。
  Write(env.root + "/handwritten.json",
        "{\"version\": 1, \"password\": \"x\"}");
  bp::ScheduleStore handwritten(env.root + "/handwritten.json");
  bp::ScheduleDocument loaded;
  test_support::Check(
      handwritten.Load(&loaded, &error) == bp::ScheduleLoadStatus::kError &&
          !error.empty(),
      "ENC-02 a hand-written store with an unknown field is rejected", error);

  const std::string lock_path = env.root + "/schedule.json.lock";
  bp::SchedulerLock first;
  test_support::Check(first.Acquire(lock_path, &error),
                      "LOCK-01 the first runner takes the lock", error);
  test_support::Check(first.held(), "LOCK-02 the lock reports itself as held");
  test_support::Check(first.ReadOwnerHint().find("pid=") != std::string::npos,
                      "LOCK-03 the owner hint names a pid",
                      first.ReadOwnerHint());
  bp::SchedulerLock second;
  test_support::Check(!second.Acquire(lock_path, &error) &&
                          error.find("already held") != std::string::npos,
                      "LOCK-04 a second runner is refused, not double-backed-up",
                      error);
  test_support::Check(!second.held(), "LOCK-05 the refused lock holds nothing");
  first.Release();
  test_support::Check(!first.held(), "LOCK-06 release clears the ownership");
  test_support::Check(second.Acquire(lock_path, &error),
                      "LOCK-07 the lock can be taken after a release", error);
  second.Release();
}

// ---- H. 稳定性 ----

void TestStability() {
  test_support::Section("H. stability");

  {
    const Env env = MakeEnv("stress-100", 3, 1);
    bp::ScheduleEvaluationResult result;
    std::vector<std::string> seen;
    bool ok = true;
    std::string failure;
    for (int index = 0; index < 100 && ok; ++index) {
      // 变化 / 无变化交替：偶数轮改内容，奇数轮什么都不做。
      if (index % 2 == 0) {
        Write(env.source + "/counter.txt", "round " + std::to_string(index));
      }
      std::string error;
      if (!Evaluate(env, 1000 + index * 60, &result, &error)) {
        ok = false;
        failure = error;
        break;
      }
      if (!result.archive_file_name.empty()) {
        seen.push_back(result.archive_file_name);
      }
      bp::ScheduleDocument document = LoadDocument(env);
      if (document.state.managed_snapshots.size() > 3) {
        ok = false;
        failure = "managed list exceeded retain_count: " +
                  std::to_string(document.state.managed_snapshots.size());
        break;
      }
      if (document.state.history.size() > bp::kMaxHistoryEntries) {
        ok = false;
        failure = "history exceeded the bound";
        break;
      }
    }
    test_support::Check(ok, "STRESS-01 100 evaluations stay bounded", failure);
    std::sort(seen.begin(), seen.end());
    test_support::Check(std::adjacent_find(seen.begin(), seen.end()) == seen.end(),
                        "STRESS-02 no duplicate archive file name was produced",
                        std::to_string(seen.size()));
    test_support::Check(RepoArchives(env.repository).size() <= 3,
                        "STRESS-03 the repository never exceeds retain_count",
                        JoinNames(RepoArchives(env.repository)));
    const bp::ScheduleDocument document = LoadDocument(env);
    test_support::Check(document.state.managed_snapshots.size() == 3,
                        "STRESS-04 the managed list settled at retain_count",
                        std::to_string(document.state.managed_snapshots.size()));
  }

  {
    // 1000+ 条目的 manifest：diff 必须快，而且相同输入必须零变化。
    const Env env = MakeEnv("manifest-large", 3);
    for (int index = 0; index < 1200; ++index) {
      Write(env.source + "/f" + std::to_string(index) + ".txt", "x");
    }
    bp::Filter filter;
    std::vector<bp::ManifestEntry> first;
    std::vector<bp::ManifestEntry> second;
    std::string error;
    test_support::Check(bp::BuildSourceManifest(env.source, &filter, &first, &error),
                        "STRESS-10 a 1200-entry manifest builds", error);
    test_support::Check(first.size() >= 1200,
                        "STRESS-11 every entry is present",
                        std::to_string(first.size()));
    test_support::Check(bp::BuildSourceManifest(env.source, &filter, &second, &error),
                        "STRESS-12 the manifest rebuilds", error);
    bp::ChangeSummary summary;
    const clock_t started = std::clock();
    test_support::Check(bp::DiffManifests(first, second, &summary, nullptr, &error),
                        "STRESS-13 diffing 1200 entries succeeds", error);
    const double seconds =
        static_cast<double>(std::clock() - started) / CLOCKS_PER_SEC;
    test_support::Check(summary.empty(),
                        "STRESS-14 an identical large manifest has no changes");
    test_support::Check(seconds < 5.0, "STRESS-15 diffing stays fast",
                        std::to_string(seconds) + "s");
  }

  {
    // 100 次 state 装载 / 保存：每一次都还能原样读回来。
    const Env env = MakeEnv("cycles", 4);
    bp::ScheduleStore store(env.schedule_file);
    bp::ScheduleDocument document;
    std::string error;
    store.Load(&document, &error);
    bool ok = true;
    std::string failure;
    for (int index = 0; index < 100; ++index) {
      bp::ScheduledSnapshotRecord record;
      record.file_name = "cycle_" + std::to_string(index) + ".bak";
      record.created_time_sec = index;
      record.entry_count = static_cast<std::uint64_t>(index);
      record.archive_size = static_cast<std::uint64_t>(index) * 7;
      document.state.managed_snapshots.clear();
      document.state.managed_snapshots.push_back(record);
      bp::ScheduleHistoryEntry entry;
      entry.scheduled_at_sec = index;
      entry.result = bp::ScheduleRunResult::kSuccessCreated;
      entry.archive_file_name = record.file_name;
      document.state.history.push_back(entry);
      if (document.state.history.size() > bp::kMaxHistoryEntries) {
        document.state.history.erase(document.state.history.begin());
      }
      if (!store.Save(document, &error)) {
        ok = false;
        failure = error;
        break;
      }
      bp::ScheduleDocument reloaded;
      if (store.Load(&reloaded, &error) != bp::ScheduleLoadStatus::kLoaded) {
        ok = false;
        failure = error;
        break;
      }
      if (reloaded.state.managed_snapshots.size() != 1 ||
          reloaded.state.managed_snapshots[0].file_name != record.file_name ||
          reloaded.state.history.size() != document.state.history.size()) {
        ok = false;
        failure = "round " + std::to_string(index) + " did not round-trip";
        break;
      }
      document = reloaded;
    }
    test_support::Check(ok, "STRESS-20 100 save/load cycles all reload", failure);
    test_support::Check(document.state.history.size() ==
                            bp::kMaxHistoryEntries,
                        "STRESS-21 the history stops at its bound",
                        std::to_string(document.state.history.size()));
    test_support::Check(document.state.history.size() <= bp::kMaxHistoryEntries,
                        "STRESS-22 the history respects its bound");
  }
}

}  // namespace

int main() {
  std::printf("scheduled backup test\n");
  TestTimeSemantics();
  TestFirstRunAndSkip();
  TestRunNowSemantics();
  TestChangeKinds();
  TestSnapshotsAreIndependent();
  TestRetention();
  TestPipelineMatrix();
  TestEncryptionBoundaryAndLock();
  TestStability();
  test_support::RemoveTree(test_support::TempRoot());
  return test_support::Finish("scheduled backup");
}
