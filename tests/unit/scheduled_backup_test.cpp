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
#include "incremental_backup.h"
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
  for (const bp::BackupRecord& record : records)
    names.push_back(record.file_name);
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

// 指定仓库、并且明确跳过"到没到点"的一轮评估。baseline 的测试关心的是
// "这一轮该不该建快照"，不是时间表，所以默认走 EvaluateNow。
bool EvaluateAt(const Env& env, const std::string& repository, bool force,
                std::int64_t now, bp::ScheduleEvaluationResult* result,
                std::string* error) {
  bp::ScheduleStore store(env.schedule_file);
  bp::ScheduledBackupService service(repository, &store);
  return force ? service.EvaluateNow(now, result, error)
               : service.Evaluate(now, result, error);
}

void ExpectNow(const std::string& label, const Env& env,
               const std::string& repository, std::int64_t now,
               bp::ScheduleEvaluationStatus expected,
               bp::ScheduleEvaluationResult* result) {
  std::string error;
  if (!EvaluateAt(env, repository, /*force=*/true, now, result, &error)) {
    test_support::Check(false, label, error);
    return;
  }
  test_support::Check(result->status == expected, label,
                      std::string("status=") +
                          bp::ScheduleEvaluationStatusKey(result->status) +
                          " diagnostic=" + result->diagnostic);
}

// 把文本里的第一处 from 换成 to。只用于"手工改坏 schedule.json"这种用例。
void ReplaceOnce(std::string* text, const std::string& from,
                 const std::string& to) {
  const std::size_t at = text->find(from);
  if (at != std::string::npos) text->replace(at, from.size(), to);
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
  test_support::Check(result->status == expected, label,
                      std::string("status=") +
                          bp::ScheduleEvaluationStatusKey(result->status) +
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
  // PR #19：Realtime × {Full, Incremental} 两格现在都是真实支持的组合，六格
  // 全开。这条用例要钉的性质没有变：**没人实现的组合必须被明确拒绝，而不是
  // 被当成 full 偷偷跑掉**——只是例子换成了仍然不受支持的那一个
  // （Incremental + USTAR：USTAR 表达不了 tombstone 与 parent 依赖）。
  config.trigger = bp::BackupTrigger::kScheduled;
  config.strategy = bp::BackupStrategy::kIncremental;
  config.pack_method = bp::PackMethod::kUstar;
  test_support::Check(
      !bp::ValidateScheduleConfig(config, &error) &&
          error.find("MyPack") != std::string::npos,
      "TIME-14 an unimplemented combination is refused, not faked", error);
  config.pack_method = bp::PackMethod::kMyPack;
  // 这一份 store 只属于 scheduled 触发：手工塞一个 realtime 进来不是
  // "换了个触发方式"，而是这份文件根本不该被 schedule 路径执行。
  config.trigger = bp::BackupTrigger::kRealtime;
  test_support::Check(!bp::ValidateScheduleConfig(config, &error) &&
                          error.find("realtime.json") != std::string::npos,
                      "TIME-15 a trigger outside this store's scope is refused",
                      error);
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
  test_support::Check(
      result.changes.added == 3 && result.changes.removed == 0 &&
          result.changes.modified == 0 && result.changes.metadata_changed == 0,
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
  test_support::Check(
      after_skip == after_first,
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
  test_support::Section(
      "B2. run now ignores the due time but keeps change detection");
  const Env env = MakeEnv("run-now", 12);
  Write(env.source + "/a.txt", "alpha");

  bp::ScheduleStore store(env.schedule_file);
  bp::ScheduledBackupService service(env.repository, &store);
  bp::ScheduleEvaluationResult result;
  std::string error;

  test_support::Check(service.EvaluateNow(1000, &result, &error),
                      "NOW-01 run now evaluates", error);
  test_support::Check(
      result.status == bp::ScheduleEvaluationStatus::kCreatedSnapshot,
      "NOW-02 run now creates the first snapshot",
      bp::ScheduleEvaluationStatusKey(result.status));

  // 还没到点，而且没有变化：run now 也必须是 skip，绝不能变成"强制备份"。
  test_support::Check(service.EvaluateNow(1010, &result, &error),
                      "NOW-03 run now evaluates again", error);
  test_support::Check(
      result.status == bp::ScheduleEvaluationStatus::kSkippedNoChanges,
      "NOW-04 run now without changes is still skipped",
      bp::ScheduleEvaluationStatusKey(result.status));
  test_support::Check(RepoArchives(env.repository).size() == 1,
                      "NOW-05 the skip produced no archive");

  // 同一个时刻的到期检查仍然会说"没到点"：force 只作用于 run now 这一条入口。
  test_support::Check(
      service.Evaluate(1010, &result, &error) &&
          result.status == bp::ScheduleEvaluationStatus::kNotDue,
      "NOW-06 the automatic path still respects the due time");

  Write(env.source + "/b.txt", "beta");
  test_support::Check(service.EvaluateNow(1020, &result, &error),
                      "NOW-07 run now evaluates after a change", error);
  test_support::Check(
      result.status == bp::ScheduleEvaluationStatus::kCreatedSnapshot &&
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
  test_support::Check(
      result.changes.added == 1 && result.changes.modified == 0 &&
          result.changes.removed == 0 && result.changes.metadata_changed == 0,
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
  test_support::Check(
      result.changes.modified == 1 && result.changes.metadata_changed == 0,
      "CHG-07 a size change is modified",
      std::to_string(result.changes.modified));

  test_support::Check(chmod((env.source + "/a.txt").c_str(), 0600) == 0,
                      "CHG-08 chmod succeeds");
  ExpectStatus("CHG-09 a metadata-only change creates a snapshot", env, 1240,
               bp::ScheduleEvaluationStatus::kCreatedSnapshot, &result);
  test_support::Check(
      result.changes.modified == 0 && result.changes.metadata_changed >= 1,
      "CHG-10 a mode change is metadata_changed",
      std::to_string(result.changes.metadata_changed));

  // 不存在的源目录：明确失败，并且仍然推进 next run（不会每 tick 重试）。
  const bp::ScheduleConfig saved = LoadConfig(env);
  bp::ScheduleConfig broken = saved;
  broken.source_path = env.root + "/does-not-exist";
  SaveConfig(env, broken);
  ExpectStatus("CHG-11 a missing source fails the run", env, 1300,
               bp::ScheduleEvaluationStatus::kFailed, &result);
  test_support::Check(
      result.diagnostic.find("does-not-exist") != std::string::npos,
      "CHG-12 the failure names the missing path", result.diagnostic);
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
  test_support::Check(
      test_support::ReadFile(env.repository + "/" + oldest, &bytes),
      "IND-04 the oldest archive is readable");
  const std::string lone = elsewhere + "/lone_copy.bak";
  test_support::Check(Write(lone, bytes),
                      "IND-05 the oldest archive is copied out");

  const std::string restored = env.root + "/restored-oldest";
  bp::BackupEngine engine;
  std::string error;
  test_support::Check(engine.Restore(lone, restored, &error),
                      "IND-06 the oldest snapshot restores from a lone copy",
                      error);
  test_support::Check(
      test_support::Exists(restored + "/one.txt") &&
          !test_support::Exists(restored + "/two.txt") &&
          !test_support::Exists(restored + "/three.txt"),
      "IND-07 the oldest snapshot holds exactly the state of its "
      "own time (not a delta)");
  std::string content;
  test_support::ReadFile(restored + "/one.txt", &content);
  test_support::Check(content == "first",
                      "IND-08 the restored content matches that moment",
                      content);

  // 最新的一份也必须能独立恢复。
  const std::vector<std::string> names = RepoArchives(env.repository);
  test_support::Check(names.size() == 3, "IND-09 three snapshots exist",
                      JoinNames(names));
  const std::string newest_dir = env.root + "/restored-newest";
  test_support::Check(
      engine.Restore(env.repository + "/" + names.back(), newest_dir, &error),
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
    test_support::Check(
        document.state.managed_snapshots.size() == 12,
        "RET-02 retain=12 keeps exactly twelve managed snapshots",
        std::to_string(document.state.managed_snapshots.size()));
    const std::vector<std::string> names = RepoArchives(env.repository);
    test_support::Check(names.size() == 13,
                        "RET-03 the repository holds 12 scheduled + 1 manual",
                        JoinNames(names));
    test_support::Check(
        std::find(names.begin(), names.end(), "manual_old.bak") != names.end(),
        "RET-04 an old manual backup is never removed");
    test_support::Check(
        !oldest_name.empty() &&
            std::find(names.begin(), names.end(), oldest_name) == names.end(),
        "RET-05 the oldest scheduled snapshot was deleted", oldest_name);
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
        test_support::Check(
            false, "RET-11 retain=1 keeps exactly one snapshot",
            std::to_string(document.state.managed_snapshots.size()));
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
    test_support::Check(
        LoadDocument(env).state.managed_snapshots.size() == 1,
        "RET-25 the stale managed record was removed",
        std::to_string(LoadDocument(env).state.managed_snapshots.size()));
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
    // 保留点必须是一份**验证得过**的快照：retention 现在只信实际字节与副文件，
    // 保留点读不透时会整轮不删（fail closed）。这里直接走产品路径写一份真的。
    {
      bp::Filter filter;
      bp::BackupOptions options;
      bp::IncrementalOutcome outcome;
      test_support::Check(bp::RunIncrementalBackup(
                              env.source, env.repository, newer.file_name,
                              bp::RepositoryIdentity(env.repository), filter,
                              options, std::vector<std::string>(),
                              std::vector<std::string>(), "", &outcome, &error),
                          "RET-29 the kept snapshot is a real, verifiable one",
                          error);
    }
    document.state.managed_snapshots.push_back(newer);
    test_support::Check(store.Save(document, &error),
                        "RET-30 the crafted document saves", error);

    bp::ScheduledBackupService service(env.repository, &store);
    bp::ScheduleDocument loaded;
    store.Load(&loaded, &error);
    std::uint64_t deleted = 0;
    std::uint64_t failed = 0;
    // PR #18：多一个出参——因为被依赖而保留的祖先数量。
    std::uint64_t dependency_retained = 0;
    std::uint64_t unreadable = 0;
    std::string retention_error;
    test_support::Check(
        !service.RunRetention(&loaded, &deleted, &failed, &dependency_retained,
                              &unreadable, &retention_error),
        "RET-31 a deletion failure is reported");
    test_support::Check(deleted == 0 && failed == 1,
                        "RET-32 exactly one deletion failed",
                        std::to_string(deleted) + "/" + std::to_string(failed));
    test_support::Check(
        loaded.state.managed_snapshots.size() == 2,
        "RET-33 the un-deletable record is kept for the next retry",
        std::to_string(loaded.state.managed_snapshots.size()));
    test_support::Check(retention_error.find(link_name) != std::string::npos,
                        "RET-34 the retention error names the archive",
                        retention_error);
    test_support::Check(
        bp::StatusForRetention(true) ==
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
    ExpectStatus(std::string("MAT-01 ") + item.label + " creates a snapshot",
                 env, 1000, bp::ScheduleEvaluationStatus::kCreatedSnapshot,
                 &result);

    const std::string archive = env.repository + "/" + result.archive_file_name;
    bp::ArchiveFileInfo info;
    std::string error;
    test_support::Check(
        bp::IdentifyArchiveFile(archive, &info, &error) &&
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
                        std::string("MAT-03 ") + item.label + " restores",
                        error);
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
  test_support::Check(
      !store.Save(document, &error) &&
          error.find("定时无人值守加密需要安全的密钥来源") != std::string::npos,
      "ENC-01 a schedule store refuses to persist encryption", error);

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
  test_support::Check(
      !second.Acquire(lock_path, &error) &&
          error.find("already held") != std::string::npos,
      "LOCK-04 a second runner is refused, not double-backed-up", error);
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
    test_support::Check(
        std::adjacent_find(seen.begin(), seen.end()) == seen.end(),
        "STRESS-02 no duplicate archive file name was produced",
        std::to_string(seen.size()));
    test_support::Check(RepoArchives(env.repository).size() <= 3,
                        "STRESS-03 the repository never exceeds retain_count",
                        JoinNames(RepoArchives(env.repository)));
    const bp::ScheduleDocument document = LoadDocument(env);
    test_support::Check(
        document.state.managed_snapshots.size() == 3,
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
    test_support::Check(
        bp::BuildSourceManifest(env.source, &filter, &first, &error),
        "STRESS-10 a 1200-entry manifest builds", error);
    test_support::Check(first.size() >= 1200,
                        "STRESS-11 every entry is present",
                        std::to_string(first.size()));
    test_support::Check(
        bp::BuildSourceManifest(env.source, &filter, &second, &error),
        "STRESS-12 the manifest rebuilds", error);
    bp::ChangeSummary summary;
    const clock_t started = std::clock();
    test_support::Check(
        bp::DiffManifests(first, second, &summary, nullptr, &error),
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
    test_support::Check(ok, "STRESS-20 100 save/load cycles all reload",
                        failure);
    test_support::Check(document.state.history.size() == bp::kMaxHistoryEntries,
                        "STRESS-21 the history stops at its bound",
                        std::to_string(document.state.history.size()));
    test_support::Check(document.state.history.size() <= bp::kMaxHistoryEntries,
                        "STRESS-22 the history respects its bound");
  }
}

// ---- I. 支持矩阵与启用时刻 ----

void TestModeMatrix() {
  test_support::Section("I. the backup mode support matrix");

  struct ModeCase {
    bp::BackupTrigger trigger;
    bp::BackupStrategy strategy;
    bool supported;
    const char* label;
  };
  const ModeCase cases[] = {
      {bp::BackupTrigger::kManual, bp::BackupStrategy::kFull, true,
       "manual-full"},
      // PR #18：两条增量组合都变成了真实支持的组合 —— 手动走共享增量引擎，
      // 计划把决策委托给同一个引擎，且 retention 已经是 dependency-aware。
      // 这张表仍然是唯一答案来源：把某一条改回 false 而不改实现，这里立刻红。
      {bp::BackupTrigger::kManual, bp::BackupStrategy::kIncremental, true,
       "manual-incremental"},
      {bp::BackupTrigger::kScheduled, bp::BackupStrategy::kFull, true,
       "scheduled-full"},
      {bp::BackupTrigger::kScheduled, bp::BackupStrategy::kIncremental, true,
       "scheduled-incremental"},
      // PR #19：实时触发的两格打开（watch + debounce + 共享引擎 + marker），
      // 六格全开。把某一条改回 false 而不改实现，这里立刻红。
      {bp::BackupTrigger::kRealtime, bp::BackupStrategy::kFull, true,
       "realtime-full"},
      {bp::BackupTrigger::kRealtime, bp::BackupStrategy::kIncremental, true,
       "realtime-incremental"},
  };

  // 3 x 2 真值表逐格钉死。只判断 trigger 的写法会在这里被抓住 ——
  // 把"没实现的策略"误判成 supported，正是"选了增量却按全量跑"这类静默降级的
  // 入口；反过来把已实现的组合判成不支持，则会让功能存在却没人能用到。
  for (const ModeCase& item : cases) {
    const std::string label = std::string("MODE-") + item.label;
    test_support::Check(
        bp::IsSupportedBackupMode(item.trigger, item.strategy) ==
            item.supported,
        label + " IsSupportedBackupMode(" + bp::BackupTriggerKey(item.trigger) +
            " + " + bp::BackupStrategyKey(item.strategy) + ")",
        item.supported
            ? ""
            : bp::UnsupportedBackupModeReason(item.trigger, item.strategy));
  }

  // 配置层必须真的用那张表，而不是自己再判断一遍。这里其实要同时问两个不同的
  // 问题，两个都要回答对：
  //   * 矩阵：这个 trigger × strategy 产品实现了吗？
  //   * store 作用域：这份 schedule.json 允许装它吗？（只装 scheduled 触发，
  //     实时触发有它自己的 realtime.json）
  // 只把矩阵答案抄一遍，会让一份手改成 realtime 的计划被 schedule 路径执行。
  for (const ModeCase& item : cases) {
    bp::ScheduleConfig config;
    config.source_path = "/tmp";
    config.trigger = item.trigger;
    config.strategy = item.strategy;
    std::string error;
    const bool in_scope = item.trigger == bp::BackupTrigger::kScheduled;
    test_support::Check(
        bp::ValidateScheduleConfig(config, &error) ==
            (item.supported && in_scope),
        std::string("MODE-") + item.label + " in ValidateScheduleConfig",
        error);
    if (!in_scope) {
      test_support::Check(
          !error.empty() && error.find("realtime.json") != std::string::npos,
          std::string("MODE-") + item.label + " 说明的是 store 作用域", error);
    }
  }

  // 不支持时必须有能直接显示的原文，GUI / CLI 不各自拼句子。
  test_support::Check(
      bp::UnsupportedBackupModeReason(bp::BackupTrigger::kManual,
                                      bp::BackupStrategy::kIncremental)
              .find("Manual + Incremental") != std::string::npos,
      "MODE-07 the refusal names the exact combination");
}

void TestEnableTransition() {
  test_support::Section("I2. enabling moves the next run one interval ahead");

  bp::ScheduleDocument document;
  document.config.enabled = false;
  document.config.interval_minutes = 60;
  document.state.next_run_time_sec = 0;

  bp::ApplyScheduleEnableTransition(&document, false, 1000);
  test_support::Check(document.state.next_run_time_sec == 0,
                      "ENABLE-01 a disabled schedule keeps its next run",
                      std::to_string(document.state.next_run_time_sec));

  // disabled -> enabled，t=1000，interval=60 分钟 -> 4600。
  document.config.enabled = true;
  bp::ApplyScheduleEnableTransition(&document, false, 1000);
  test_support::Check(document.state.next_run_time_sec == 4600,
                      "ENABLE-02 the first enable schedules one interval ahead",
                      std::to_string(document.state.next_run_time_sec));
  test_support::Check(
      !bp::IsScheduleDue(1001, document.state.next_run_time_sec, 60),
      "ENABLE-03 the very next tick is not due");
  test_support::Check(
      bp::IsScheduleDue(4600, document.state.next_run_time_sec, 60),
      "ENABLE-04 the run becomes due exactly one interval later");

  // 已经启用：show / load / set 走的都是这条路，绝不能把时间表往后推。
  bp::ApplyScheduleEnableTransition(&document, true, 9000);
  test_support::Check(
      document.state.next_run_time_sec == 4600,
      "ENABLE-05 an already-enabled schedule keeps its next run",
      std::to_string(document.state.next_run_time_sec));

  // 停用不动时间表，再启用才重算。
  document.config.enabled = false;
  bp::ApplyScheduleEnableTransition(&document, true, 9000);
  test_support::Check(document.state.next_run_time_sec == 4600,
                      "ENABLE-06 disabling leaves the next run alone",
                      std::to_string(document.state.next_run_time_sec));
  document.config.enabled = true;
  bp::ApplyScheduleEnableTransition(&document, false, 9000);
  test_support::Check(document.state.next_run_time_sec == 12600,
                      "ENABLE-07 re-enabling recomputes the next run",
                      std::to_string(document.state.next_run_time_sec));
}

void TestEnabledScheduleDoesNotRunImmediately() {
  test_support::Section("I3. an enabled schedule waits for its first interval");

  const Env env = MakeEnv("enable-timing", 3);
  Write(env.source + "/a.txt", "alpha");

  // 模拟 enable 那一刻：把 next_run 推成一个周期之后。
  bp::ScheduleStore store(env.schedule_file);
  bp::ScheduleDocument document;
  std::string error;
  test_support::Check(
      store.Load(&document, &error) == bp::ScheduleLoadStatus::kLoaded,
      "ENABLE-10 the store loads", error);
  bp::ApplyScheduleEnableTransition(&document, /*was_enabled=*/false, 1000);
  test_support::Check(store.Save(document, &error), "ENABLE-11 the store saves",
                      error);

  bp::ScheduleEvaluationResult result;
  ExpectStatus("ENABLE-12 the automatic path is not due yet", env, 1001,
               bp::ScheduleEvaluationStatus::kNotDue, &result);
  test_support::Check(
      RepoArchives(env.repository).empty(),
      "ENABLE-13 nothing was written before the interval elapsed",
      JoinNames(RepoArchives(env.repository)));

  // "立即检查并运行"仍然立刻做真实的变化检测。
  ExpectNow("ENABLE-14 run-now still evaluates immediately", env,
            env.repository, 1002,
            bp::ScheduleEvaluationStatus::kCreatedSnapshot, &result);

  // 到点之后自动路径才动；源没变，所以它照样是 skip 而不是"补一份备份"。
  ExpectStatus("ENABLE-15 the automatic path fires once due", env, 4600,
               bp::ScheduleEvaluationStatus::kSkippedNoChanges, &result);
  test_support::Check(RepoArchives(env.repository).size() == 1,
                      "ENABLE-16 the due run did not add a second snapshot",
                      JoinNames(RepoArchives(env.repository)));
}

// ---- J. manifest 与真实 baseline 快照的绑定 ----

void TestBaselineBinding() {
  test_support::Section("J. the manifest is bound to a live baseline snapshot");

  // T1：S1 -> 变化 -> S2 -> 手工删掉 S2 -> 源不变 -> 必须建 S3。
  {
    const Env env = MakeEnv("baseline-delete-newest", 12);
    Write(env.source + "/a.txt", "one");
    bp::ScheduleEvaluationResult result;
    ExpectStatus("BASE-01 S1 is created", env, 1000,
                 bp::ScheduleEvaluationStatus::kCreatedSnapshot, &result);
    const std::string s1 = result.archive_file_name;

    Write(env.source + "/b.txt", "two");
    ExpectStatus("BASE-02 S2 is created after a change", env, 2000,
                 bp::ScheduleEvaluationStatus::kCreatedSnapshot, &result);
    const std::string s2 = result.archive_file_name;
    test_support::Check(!s1.empty() && !s2.empty() && s1 != s2,
                        "BASE-03 the two snapshots have different names");

    // state 必须明确记下"这份 manifest 属于 S2"。
    bp::ScheduleDocument document = LoadDocument(env);
    test_support::Check(document.state.baseline.snapshot_file_name == s2,
                        "BASE-04 the baseline names S2",
                        document.state.baseline.snapshot_file_name);
    test_support::Check(document.state.baseline.source_path == env.source,
                        "BASE-05 the baseline records its source directory",
                        document.state.baseline.source_path);
    test_support::Check(!document.state.baseline.repository_identity.empty(),
                        "BASE-06 the baseline records a repository identity",
                        document.state.baseline.repository_identity);

    // 用户手工删掉最新的 S2，源从此不再变化。
    bp::BackupCatalog catalog;
    std::string error;
    test_support::Check(catalog.Delete(env.repository, s2, &error),
                        "BASE-07 the newest snapshot is deleted by hand",
                        error);

    ExpectNow("BASE-08 deleting the baseline forces a new full snapshot", env,
              env.repository, 3000,
              bp::ScheduleEvaluationStatus::kCreatedSnapshot, &result);
    test_support::Check(result.baseline_reset,
                        "BASE-09 the run is reported as a baseline reset",
                        result.diagnostic);
    test_support::Check(!result.first_snapshot,
                        "BASE-10 it is not reported as a first snapshot");
    const std::string s3 = result.archive_file_name;
    test_support::Check(s3 != s1 && s3 != s2 && !s3.empty(),
                        "BASE-11 a third snapshot was created", s3);

    // S3 必须等于当前源：a.txt 与 b.txt 都在。
    bp::BackupEngine engine;
    const std::string restored = env.root + "/restored-s3";
    test_support::Check(
        engine.Restore(env.repository + "/" + s3, restored, &error),
        "BASE-12 S3 restores", error);
    test_support::Check(test_support::Exists(restored + "/a.txt") &&
                            test_support::Exists(restored + "/b.txt"),
                        "BASE-13 S3 holds the current source, not the old one");
    test_support::Check(RepoArchives(env.repository).size() == 2,
                        "BASE-14 the repository holds S1 and S3",
                        JoinNames(RepoArchives(env.repository)));

    // 再跑一轮：S3 是 baseline，源没变 -> skip。
    ExpectNow("BASE-15 the next run skips again", env, env.repository, 4000,
              bp::ScheduleEvaluationStatus::kSkippedNoChanges, &result);
  }

  // T2：换仓库。源没变，但新仓库里一个 baseline 都没有。
  {
    const Env env = MakeEnv("baseline-repo-switch", 12);
    Write(env.source + "/a.txt", "alpha");
    bp::ScheduleEvaluationResult result;
    ExpectStatus("BASE-20 repository A gets a baseline", env, 1000,
                 bp::ScheduleEvaluationStatus::kCreatedSnapshot, &result);
    test_support::Check(RepoArchives(env.repository).size() == 1,
                        "BASE-21 repository A holds one snapshot");

    const std::string repository_b = env.root + "/repository-b";
    test_support::Check(test_support::Mkdir(repository_b, 0755),
                        "BASE-22 repository B exists and is empty");

    ExpectNow("BASE-23 switching repositories creates a baseline in B", env,
              repository_b, 2000,
              bp::ScheduleEvaluationStatus::kCreatedSnapshot, &result);
    test_support::Check(result.baseline_reset,
                        "BASE-24 the repository switch is a baseline reset",
                        result.diagnostic);
    const std::vector<std::string> in_b = RepoArchives(repository_b);
    test_support::Check(in_b.size() == 1,
                        "BASE-25 repository B now holds exactly "
                        "one snapshot",
                        JoinNames(in_b));
    test_support::Check(result.archive_file_name == in_b[0],
                        "BASE-26 the new snapshot lives in repository B");
    test_support::Check(RepoArchives(env.repository).size() == 1,
                        "BASE-27 repository A was not touched",
                        JoinNames(RepoArchives(env.repository)));

    // 记下来的 identity 也必须跟着换成 B。
    const bp::ScheduleDocument document = LoadDocument(env);
    test_support::Check(document.state.baseline.repository_identity ==
                            bp::RepositoryIdentity(repository_b),
                        "BASE-28 the baseline now names repository B",
                        document.state.baseline.repository_identity);
  }

  // T3：baseline 文件被外部删掉（managed 名单里还留着它）。
  {
    const Env env = MakeEnv("baseline-external-removal", 12);
    Write(env.source + "/a.txt", "alpha");
    bp::ScheduleEvaluationResult result;
    ExpectStatus("BASE-30 a baseline is created", env, 1000,
                 bp::ScheduleEvaluationStatus::kCreatedSnapshot, &result);
    const std::string baseline = result.archive_file_name;

    // 绕过 BackupCatalog 直接 unlink：文件名安全边界管的是"能不能删"，
    // 管不了"别人绕过它去删"。这一条测的是被绕过之后能不能自愈。
    test_support::Check(
        ::unlink((env.repository + "/" + baseline).c_str()) == 0,
        "BASE-31 the baseline file disappears behind our back");

    ExpectNow("BASE-32 the next evaluation replaces the missing baseline", env,
              env.repository, 2000,
              bp::ScheduleEvaluationStatus::kCreatedSnapshot, &result);
    test_support::Check(result.baseline_reset,
                        "BASE-33 it is reported as a baseline reset",
                        result.diagnostic);
    test_support::Check(RepoArchives(env.repository).size() == 1,
                        "BASE-34 exactly one snapshot exists again",
                        JoinNames(RepoArchives(env.repository)));
  }

  // T4：删掉的是**旧的、非 baseline** 的那一份 -> baseline 仍然有效 -> 照样
  // skip。
  {
    const Env env = MakeEnv("baseline-keep-old", 12);
    Write(env.source + "/a.txt", "one");
    bp::ScheduleEvaluationResult result;
    ExpectStatus("BASE-40 S1 is created", env, 1000,
                 bp::ScheduleEvaluationStatus::kCreatedSnapshot, &result);
    const std::string s1 = result.archive_file_name;
    Write(env.source + "/b.txt", "two");
    ExpectStatus("BASE-41 S2 is created", env, 2000,
                 bp::ScheduleEvaluationStatus::kCreatedSnapshot, &result);
    const std::string s2 = result.archive_file_name;

    bp::BackupCatalog catalog;
    std::string error;
    test_support::Check(catalog.Delete(env.repository, s1, &error),
                        "BASE-42 the old snapshot is deleted", error);

    ExpectNow("BASE-43 deleting a non-baseline snapshot still skips", env,
              env.repository, 3000,
              bp::ScheduleEvaluationStatus::kSkippedNoChanges, &result);
    test_support::Check(!result.baseline_reset && !result.first_snapshot,
                        "BASE-44 the baseline was not reset");
    const std::vector<std::string> left = RepoArchives(env.repository);
    test_support::Check(left.size() == 1 && left[0] == s2,
                        "BASE-45 only the baseline is left", JoinNames(left));
  }

  // 换源目录：两棵树的 manifest 一模一样，"比较 manifest"分辨不出来，
  // 只有 baseline 里记的 source_path 能。
  {
    const Env env = MakeEnv("baseline-source-switch", 12);
    Write(env.source + "/a.txt", "alpha");
    bp::ScheduleEvaluationResult result;
    ExpectStatus("BASE-50 a baseline is created", env, 1000,
                 bp::ScheduleEvaluationStatus::kCreatedSnapshot, &result);

    const std::string other = env.root + "/other-source";
    test_support::Check(test_support::Mkdir(other, 0755),
                        "BASE-51 the other source exists");
    Write(other + "/a.txt", "alpha");
    bp::ScheduleConfig config = LoadConfig(env);
    config.source_path = other;
    SaveConfig(env, config);

    ExpectNow("BASE-52 switching the source rebuilds the baseline", env,
              env.repository, 2000,
              bp::ScheduleEvaluationStatus::kCreatedSnapshot, &result);
    test_support::Check(result.baseline_reset,
                        "BASE-53 the source switch is a baseline reset",
                        result.diagnostic);
    test_support::Check(RepoArchives(env.repository).size() == 2,
                        "BASE-54 a second snapshot exists",
                        JoinNames(RepoArchives(env.repository)));
  }
}

void TestBaselineAgainstAnUnreadableRepository() {
  test_support::Section(
      "J2. an unreadable repository never resets the baseline");

  const Env env = MakeEnv("baseline-repo-gone", 12);
  Write(env.source + "/a.txt", "alpha");
  bp::ScheduleEvaluationResult result;
  ExpectStatus("BASE-60 a baseline is created", env, 1000,
               bp::ScheduleEvaluationStatus::kCreatedSnapshot, &result);

  // 仓库整个不见了（没挂载 / 被搬走）。这正是 ReconcileManagedSnapshots
  // 刻意"什么都不做"的那种情况，现在多了一层：有 baseline 记录时直接失败，
  // 绝不在一个空的挂载点里新建一份备份。
  const std::string moved = env.root + "/repository-moved-away";
  test_support::Check(::rename(env.repository.c_str(), moved.c_str()) == 0,
                      "BASE-61 the repository is moved away");

  std::string error;
  bp::ScheduleEvaluationResult failing;
  const bool ok =
      EvaluateAt(env, env.repository, /*force=*/true, 2000, &failing, &error);
  test_support::Check(
      ok && failing.status == bp::ScheduleEvaluationStatus::kFailed,
      "BASE-62 an unreadable repository fails the run",
      ok ? failing.diagnostic : error);
  test_support::Check(!test_support::Exists(env.repository),
                      "BASE-63 nothing was created at the missing path");
  const bp::ScheduleDocument document = LoadDocument(env);
  test_support::Check(!document.state.baseline.snapshot_file_name.empty(),
                      "BASE-64 the baseline record was kept",
                      document.state.baseline.snapshot_file_name);
  test_support::Check(document.state.managed_snapshots.size() == 1,
                      "BASE-65 the managed list was not emptied",
                      std::to_string(document.state.managed_snapshots.size()));

  // 仓库搬回来之后一切照旧：源没变 -> 照样 skip。
  test_support::Check(::rename(moved.c_str(), env.repository.c_str()) == 0,
                      "BASE-66 the repository is restored");
  ExpectNow("BASE-67 the restored repository skips again", env, env.repository,
            3000, bp::ScheduleEvaluationStatus::kSkippedNoChanges, &result);
}

void TestRetentionKeepsTheBaselineInvariant() {
  test_support::Section("J3. retention never leaves a dangling baseline");

  const Env env = MakeEnv("retention-baseline", 12);
  Write(env.source + "/a.txt", "alpha");
  bp::ScheduleEvaluationResult result;
  ExpectStatus("BASE-70 a baseline is created", env, 1000,
               bp::ScheduleEvaluationStatus::kCreatedSnapshot, &result);
  const std::string baseline = result.archive_file_name;

  // 手工构造"baseline 恰好是最旧的那一份、而且超额"的 state。
  // 正常路径碰不到（淘汰的是最旧的，baseline 是最新的），但 retain_count 被
  // 调小、或者 state 被手工改过时是可能的，invariant 必须在那儿也成立。
  std::string bytes;
  test_support::Check(
      test_support::ReadFile(env.repository + "/" + baseline, &bytes),
      "BASE-71 the baseline archive is readable");
  const std::string newer = "zzz_newer_copy.bak";
  test_support::Check(Write(env.repository + "/" + newer, bytes),
                      "BASE-72 a second archive is planted");

  bp::ScheduleDocument document = LoadDocument(env);
  document.state.managed_snapshots.clear();
  bp::ScheduledSnapshotRecord oldest;
  oldest.file_name = baseline;
  oldest.created_time_sec = 1000;
  document.state.managed_snapshots.push_back(oldest);
  bp::ScheduledSnapshotRecord latest = oldest;
  latest.file_name = newer;
  latest.created_time_sec = 2000;
  document.state.managed_snapshots.push_back(latest);
  document.config.retain_count = 1;

  bp::ScheduleStore store(env.schedule_file);
  std::string error;
  test_support::Check(store.Save(document, &error),
                      "BASE-73 the abnormal state saves", error);

  bp::ScheduledBackupService service(env.repository, &store);
  std::uint64_t deleted = 0;
  std::uint64_t failed = 0;
  // PR #18：多一个出参——因为被依赖而保留的祖先数量。
  std::uint64_t dependency_retained = 0;
  std::uint64_t unreadable = 0;
  test_support::Check(
      service.RunRetention(&document, &deleted, &failed, &dependency_retained,
                           &unreadable, &error),
      "BASE-74 retention runs", error);
  test_support::Check(deleted == 1 && failed == 0,
                      "BASE-75 exactly one snapshot was removed",
                      std::to_string(deleted) + "/" + std::to_string(failed));
  test_support::Check(document.state.baseline.snapshot_file_name.empty(),
                      "BASE-76 retention clears a baseline it removed",
                      document.state.baseline.snapshot_file_name);
  test_support::Check(store.Save(document, &error),
                      "BASE-77 the cleared state saves", error);

  // 下一轮必须重建，而不是拿着一个指向空气的 baseline 继续 skip。
  ExpectNow("BASE-78 the next run rebuilds a snapshot", env, env.repository,
            3000, bp::ScheduleEvaluationStatus::kCreatedSnapshot, &result);
  test_support::Check(
      !result.changes.empty() || result.first_snapshot || result.baseline_reset,
      "BASE-79 the rebuild is accounted for", result.diagnostic);
}

void TestUnsupportedModeIsNeverRunAsFull() {
  test_support::Section(
      "J4. an unimplemented combination is refused, never run as full");

  const Env env = MakeEnv("unsupported-mode", 12);
  Write(env.source + "/a.txt", "alpha");

  // 手工把 store 改成 trigger=realtime / strategy=incremental —— 一个**仍然
  // 没有人实现**的组合。它必须明确挂起，绝不能被当成全量悄悄跑掉。
  //
  // PR #18 之前这里用的是 manual + incremental；那一条现在是真实支持的组合
  // （手动增量由共享引擎实现），所以这条用例换成真值表里仍然为 false 的那一格。
  // 要钉的性质没有变：支持矩阵是唯一答案来源，手改 JSON 也绕不过去。
  std::string text;
  test_support::Check(test_support::ReadFile(env.schedule_file, &text),
                      "MODE-20 the store is readable");
  ReplaceOnce(&text, "\"trigger\": \"scheduled\"", "\"trigger\": \"realtime\"");
  ReplaceOnce(&text, "\"strategy\": \"full\"", "\"strategy\": \"incremental\"");
  test_support::Check(text.find("\"incremental\"") != std::string::npos,
                      "MODE-21 the strategy really says incremental");
  test_support::Check(test_support::WriteFile(env.schedule_file, text, 0600),
                      "MODE-22 the store is rewritten by hand");

  // 评估之前先把 state 抄下来：挂起必须是"一个字节都不写"，包括 next_run
  // 与 history —— 否则 GUI 会每一轮都重新判定"到点了"，退化成每秒重试。
  bp::ScheduleDocument before;
  bp::ScheduleStore store(env.schedule_file);
  std::string error;
  test_support::Check(
      store.Load(&before, &error) == bp::ScheduleLoadStatus::kLoaded,
      "MODE-23a the hand-edited store still parses", error);

  bp::ScheduleEvaluationResult result;
  test_support::Check(
      EvaluateAt(env, env.repository, /*force=*/true, 1000, &result, &error),
      "MODE-23 the evaluation itself completes", error);
  test_support::Check(
      result.status == bp::ScheduleEvaluationStatus::kConfigInvalid,
      "MODE-24 realtime + incremental suspends the schedule (it is not just a "
      "failed attempt)",
      std::string(bp::ScheduleEvaluationStatusKey(result.status)) + " " +
          result.diagnostic);
  test_support::Check(
      result.diagnostic.find("realtime.json") != std::string::npos,
      "MODE-25 the refusal names why this store cannot run it",
      result.diagnostic);
  test_support::Check(RepoArchives(env.repository).empty(),
                      "MODE-26 no full backup was silently created",
                      JoinNames(RepoArchives(env.repository)));

  std::string after_text;
  test_support::ReadFile(env.schedule_file, &after_text);
  test_support::Check(after_text == text,
                      "MODE-27 a suspended run leaves the store byte-for-byte "
                      "untouched (no silent repair)");
  bp::ScheduleDocument after;
  test_support::Check(
      store.Load(&after, &error) == bp::ScheduleLoadStatus::kLoaded,
      "MODE-28 the store still reloads", error);
  test_support::Check(
      after.state.next_run_time_sec == before.state.next_run_time_sec,
      "MODE-29 a suspended run does not advance next_run (otherwise the GUI "
      "would retry every tick)",
      std::to_string(before.state.next_run_time_sec) + " -> " +
          std::to_string(after.state.next_run_time_sec));
  test_support::Check(after.state.history.size() == before.state.history.size(),
                      "MODE-30 a suspended run records no history entry",
                      std::to_string(before.state.history.size()) + " -> " +
                          std::to_string(after.state.history.size()));
  test_support::Check(
      after.state.managed_snapshots.size() ==
          before.state.managed_snapshots.size(),
      "MODE-31 a suspended run does not touch the managed list");
}

// ---- K. store 的父目录与旧文件兼容 ----

void TestStoreParentDirectoryAndLegacyFiles() {
  test_support::Section("K. store parent directories and legacy documents");

  // 全新的深层路径：父目录一个都不存在。
  {
    const std::string root = test_support::FreshDir("store-parent");
    const std::string deep = root + "/a/b/c/schedule.json";
    bp::ScheduleStore store(deep);
    bp::ScheduleDocument document;
    document.config.enabled = false;
    document.config.source_path = root;
    std::string error;
    test_support::Check(store.Save(document, &error),
                        "STORE-01 save creates the missing parent directories",
                        error);
    test_support::Check(test_support::Exists(deep), "STORE-02 the file exists");

    struct stat info;
    const bool stat_ok = test_support::StatOf(deep, &info);
    test_support::Check(stat_ok && (info.st_mode & 07777) == 0600,
                        "STORE-03 the schedule file is still 0600",
                        stat_ok ? test_support::Octal(info.st_mode & 07777)
                                : std::string("stat failed"));

    bp::ScheduleDocument reloaded;
    test_support::Check(
        store.Load(&reloaded, &error) == bp::ScheduleLoadStatus::kLoaded,
        "STORE-04 the file loads back", error);
    test_support::Check(reloaded.config.source_path == root,
                        "STORE-05 the round trip keeps the source path");
  }

  // 父目录存在但不是目录 -> 明确报错，不静默成功。
  {
    const std::string root = test_support::FreshDir("store-parent-file");
    const std::string blocker = root + "/blocker";
    Write(blocker, "not a directory");
    bp::ScheduleStore store(blocker + "/schedule.json");
    bp::ScheduleDocument document;
    std::string error;
    test_support::Check(!store.Save(document, &error),
                        "STORE-06 a non-directory parent is refused");
    test_support::Check(!error.empty(), "STORE-07 the refusal explains itself",
                        error);
  }

  // baseline 的序列化往返。
  {
    const std::string root = test_support::FreshDir("store-baseline");
    const std::string path = root + "/schedule.json";
    bp::ScheduleStore store(path);
    bp::ScheduleDocument document;
    document.config.source_path = root;
    document.state.baseline.snapshot_file_name = "source_20260926_120000.bak";
    document.state.baseline.repository_identity = "/tmp/repo";
    document.state.baseline.source_path = root;
    std::string error;
    test_support::Check(store.Save(document, &error),
                        "STORE-10 the baseline saves", error);
    bp::ScheduleDocument reloaded;
    test_support::Check(
        store.Load(&reloaded, &error) == bp::ScheduleLoadStatus::kLoaded,
        "STORE-11 the baseline reloads", error);
    test_support::Check(
        reloaded.state.baseline.snapshot_file_name ==
                "source_20260926_120000.bak" &&
            reloaded.state.baseline.repository_identity == "/tmp/repo" &&
            reloaded.state.baseline.source_path == root,
        "STORE-12 every baseline field round-trips");

    // 非法 baseline 文件名必须在保存时就被拦住。
    bp::ScheduleDocument bad = reloaded;
    bad.state.baseline.snapshot_file_name = "../escape.bak";
    test_support::Check(!store.Save(bad, &error),
                        "STORE-13 a traversing baseline name is refused");
  }

  // 旧版本写出的 schedule.json：没有那三个 baseline 字段，必须仍然读得进来。
  {
    const std::string root = test_support::FreshDir("store-legacy");
    const std::string path = root + "/schedule.json";
    const std::string legacy =
        "{\n"
        "  \"version\": 1,\n"
        "  \"config\": {\n"
        "    \"enabled\": false,\n"
        "    \"trigger\": \"scheduled\",\n"
        "    \"strategy\": \"full\",\n"
        "    \"source_path\": \"" +
        root +
        "\",\n"
        "    \"interval_minutes\": 60,\n"
        "    \"retain_count\": 12,\n"
        "    \"pack\": \"mypack\",\n"
        "    \"compression\": \"none\",\n"
        "    \"encryption\": \"none\",\n"
        "    \"include_rules\": [],\n"
        "    \"exclude_rules\": []\n"
        "  },\n"
        "  \"state\": {\n"
        "    \"next_run_time_sec\": 0,\n"
        "    \"last_success_time_sec\": 0,\n"
        "    \"last_manifest_entry_count\": 0,\n"
        "    \"managed_snapshots\": [],\n"
        "    \"history\": []\n"
        "  }\n"
        "}\n";
    test_support::Check(test_support::WriteFile(path, legacy, 0600),
                        "STORE-20 the legacy document is written");
    bp::ScheduleStore store(path);
    bp::ScheduleDocument document;
    std::string error;
    test_support::Check(
        store.Load(&document, &error) == bp::ScheduleLoadStatus::kLoaded,
        "STORE-21 a document without baseline fields still loads", error);
    test_support::Check(document.state.baseline.snapshot_file_name.empty() &&
                            document.state.baseline.repository_identity.empty(),
                        "STORE-22 the missing baseline degrades to 'unknown'");

    // 但"不认识的字段"照样拒绝：可选列表不是放松未知字段。
    std::string tampered = legacy;
    ReplaceOnce(&tampered, "\"history\": []",
                "\"history\": [], \"baseline_bogus\": \"x\"");
    test_support::Check(test_support::WriteFile(path, tampered, 0600),
                        "STORE-23 the tampered document is written");
    test_support::Check(
        store.Load(&document, &error) == bp::ScheduleLoadStatus::kError &&
            error.find("unknown field") != std::string::npos,
        "STORE-24 an unknown state field is still rejected", error);
  }
}

}  // namespace

// ---- L. 崩溃一致性：三个文件的中间状态 ----
//
// archive / manifest / schedule.json 是三个独立文件，各自原子替换，
// **没有任何时刻能让三个一起提交**。所以"崩在两次写盘之间"留下的中间状态
// 是必然会出现的真实磁盘状态，不是假想的。这一节把每一种中间状态都摆出来，
// 钉住同一条不变式：
//
//   *** 只有 manifest 自己声明的归属与 state 记录的 baseline 完全一致，
//       才允许由 "manifest == current" 推出"可以跳过"。 ***
//
// 任何一环对不上都必须重建一份完整基线快照。多建一份是安全的代价；
// 错误跳过是一个再也补不回来的数据缺口。
//
// 素材全部由**真实评估**跑出来，场景只做搬运与就地篡改——不手写字节，
// 免得测试自己构造出一个产品永远写不出来的形状。

std::string ManifestPath(const Env& env) {
  return bp::ScheduleStore(env.schedule_file).manifest_file_path();
}

bool CopyFileTo(const std::string& from, const std::string& to) {
  std::string bytes;
  if (!test_support::ReadFile(from, &bytes)) return false;
  return Write(to, bytes);
}

void RemoveFile(const std::string& path) { ::unlink(path.c_str()); }

struct CrashCut {
  Env env;
  std::string archive1;
  std::string archive2;
};

// 跑两轮真实评估，留下四份素材：S1/M1 与 S2/M2。
CrashCut PrepareCrashCut(const std::string& name) {
  CrashCut cut;
  cut.env = MakeEnv(name, 12);
  Write(cut.env.source + "/file.txt", "v1\n");
  Write(cut.env.source + "/keep.log", "keep\n");

  bp::ScheduleEvaluationResult result;
  ExpectStatus("CUT-01 S1 is created", cut.env, 1000,
               bp::ScheduleEvaluationStatus::kCreatedSnapshot, &result);
  cut.archive1 = result.archive_file_name;
  test_support::Check(
      CopyFileTo(cut.env.schedule_file, cut.env.root + "/state-S1.json"),
      "CUT-02 the state written together with S1 is kept");
  test_support::Check(
      CopyFileTo(ManifestPath(cut.env), cut.env.root + "/manifest-S1.dat"),
      "CUT-03 the manifest written together with S1 is kept");

  Write(cut.env.source + "/file.txt", "v2\n");
  ExpectStatus("CUT-04 S2 is created after a change", cut.env, 2000,
               bp::ScheduleEvaluationStatus::kCreatedSnapshot, &result);
  cut.archive2 = result.archive_file_name;
  test_support::Check(!cut.archive2.empty() && cut.archive2 != cut.archive1,
                      "CUT-05 the second snapshot has its own name",
                      cut.archive2);
  test_support::Check(
      CopyFileTo(cut.env.schedule_file, cut.env.root + "/state-S2.json"),
      "CUT-06 the state written together with S2 is kept");
  test_support::Check(
      CopyFileTo(ManifestPath(cut.env), cut.env.root + "/manifest-S2.dat"),
      "CUT-07 the manifest written together with S2 is kept");
  return cut;
}

std::string StateMaterial(const CrashCut& cut, const std::string& which) {
  return cut.env.root + "/state-" + which + ".json";
}

std::string ManifestMaterial(const CrashCut& cut, const std::string& which) {
  return cut.env.root + "/manifest-" + which + ".dat";
}

// 把磁盘摆成某个中间状态。"S1" / "S2" / 空串表示"这个文件不存在"。
void PlaceState(const CrashCut& cut, const std::string& which) {
  if (which.empty()) {
    RemoveFile(cut.env.schedule_file);
    return;
  }
  CopyFileTo(StateMaterial(cut, which), cut.env.schedule_file);
}

void PlaceManifest(const CrashCut& cut, const std::string& which) {
  if (which.empty()) {
    RemoveFile(ManifestPath(cut.env));
    return;
  }
  CopyFileTo(ManifestMaterial(cut, which), ManifestPath(cut.env));
}

// 读回一份素材的原文。
std::string MaterialBytes(const CrashCut& cut, const std::string& which) {
  std::string bytes;
  test_support::ReadFile(ManifestMaterial(cut, which), &bytes);
  return bytes;
}

// 跑一轮，核对结果，并且——只要这一轮应该新建快照——把**新建的那一份**
// 单独恢复出来与当前源逐节点比较。"新快照等于当前源"必须是被证明的，
// 不能只是被假设。
bp::ScheduleEvaluationResult ExpectCut(const std::string& label, CrashCut* cut,
                                       bp::ScheduleEvaluationStatus expected) {
  bp::ScheduleEvaluationResult result;
  const std::vector<std::string> before = RepoArchives(cut->env.repository);

  std::string error;
  if (!Evaluate(cut->env, 3000, &result, &error)) {
    test_support::Check(false, label, error);
    return result;
  }
  test_support::Check(result.status == expected, label,
                      std::string("status=") +
                          bp::ScheduleEvaluationStatusKey(result.status) +
                          " diagnostic=" + result.diagnostic);

  const std::vector<std::string> after = RepoArchives(cut->env.repository);
  std::vector<std::string> created;
  for (const std::string& name : after) {
    if (std::find(before.begin(), before.end(), name) == before.end()) {
      created.push_back(name);
    }
  }

  if (expected == bp::ScheduleEvaluationStatus::kSkippedNoChanges) {
    test_support::Check(created.empty(),
                        label + ": a skip must not create a snapshot",
                        JoinNames(created));
    return result;
  }
  if (created.size() != 1) {
    test_support::Check(false, label + ": exactly one new snapshot",
                        JoinNames(created));
    return result;
  }

  // 单组件拷贝出去再恢复：只有这样才能证明它不依赖仓库里的别的文件。
  const std::string lone = cut->env.root + "/lone-" + created[0];
  test_support::Check(CopyFileTo(cut->env.repository + "/" + created[0], lone),
                      label + ": the new snapshot can be copied out");
  const std::string restored = cut->env.root + "/restored-" + created[0];
  bp::BackupEngine engine;
  std::string restore_error;
  test_support::Check(engine.Restore(lone, restored, &restore_error),
                      label + ": the new snapshot restores from a lone copy",
                      restore_error);
  std::string detail;
  test_support::Check(
      test_support::CompareTrees(cut->env.source, restored, &detail),
      label + ": the new snapshot equals the current source", detail);
  return result;
}

void TestCrashConsistency() {
  test_support::Section(
      "L. crash consistency: the manifest must name its baseline");

  // C0 正常配对：state=S2、manifest 声明的也是 S2、两份归档都在、源没变。
  // 这是唯一允许 skip 的形状。
  {
    CrashCut cut = PrepareCrashCut("cut-c0");
    PlaceState(cut, "S2");
    PlaceManifest(cut, "S2");
    ExpectCut("CUT-C0 a matching pair still skips", &cut,
              bp::ScheduleEvaluationStatus::kSkippedNoChanges);
  }

  // C1 **真实 bug**：崩在 SaveManifest 与 Save(state) 之间。
  // state 还停在 S1，manifest 却已经是 S2 的（内容 = M2），两份归档都在。
  // S1 依然存在、依然 managed，manifest 也依然等于当前源——只比这两条就会
  // 错误地跳过一轮，而仓库里根本没有任何一份快照装得下 M2。
  {
    CrashCut cut = PrepareCrashCut("cut-c1");
    PlaceState(cut, "S1");
    PlaceManifest(cut, "S2");
    const bp::ScheduleEvaluationResult result =
        ExpectCut("CUT-C1 an old state with a new manifest must not skip", &cut,
                  bp::ScheduleEvaluationStatus::kCreatedSnapshot);
    test_support::Check(result.baseline_reset,
                        "CUT-C1b the run is reported as a baseline reset",
                        result.diagnostic);
    test_support::Check(!result.first_snapshot,
                        "CUT-C1c it is not reported as a first snapshot");
    test_support::Check(
        result.diagnostic.find("belongs to a different snapshot") !=
            std::string::npos,
        "CUT-C1d the diagnostic names the mismatch", result.diagnostic);

    // 重建之后必须收敛：源没再变，下一轮就是一次正常的 skip。
    PlaceState(cut, "S2");
    PlaceManifest(cut, "S2");
    ExpectCut("CUT-C1e the rebuilt baseline makes the next run skip again",
              &cut, bp::ScheduleEvaluationStatus::kSkippedNoChanges);
  }

  // C2 崩在另一次写盘之间：state 前进到了 S2，manifest 还停在 M1。
  {
    CrashCut cut = PrepareCrashCut("cut-c2");
    PlaceState(cut, "S2");
    PlaceManifest(cut, "S1");
    const bp::ScheduleEvaluationResult result =
        ExpectCut("CUT-C2 a new state with an old manifest must not skip", &cut,
                  bp::ScheduleEvaluationStatus::kCreatedSnapshot);
    test_support::Check(result.baseline_reset,
                        "CUT-C2b it is reported as a baseline reset",
                        result.diagnostic);
  }

  // C3 manifest 整个不见了。
  {
    CrashCut cut = PrepareCrashCut("cut-c3");
    PlaceState(cut, "S2");
    PlaceManifest(cut, "");
    const bp::ScheduleEvaluationResult result =
        ExpectCut("CUT-C3 a missing manifest forces a full baseline", &cut,
                  bp::ScheduleEvaluationStatus::kCreatedSnapshot);
    test_support::Check(result.baseline_reset,
                        "CUT-C3b baseline reset reported", result.diagnostic);
  }

  // C4 manifest 被截断：这是真实的"写到一半掉电"形状。
  {
    CrashCut cut = PrepareCrashCut("cut-c4");
    const std::string whole = MaterialBytes(cut, "S2");
    test_support::Check(whole.size() > 64,
                        "CUT-C4a the material is big enough");
    test_support::Check(Write(ManifestPath(cut.env), whole.substr(0, 64)),
                        "CUT-C4b the manifest is truncated");
    PlaceState(cut, "S2");
    const bp::ScheduleEvaluationResult result =
        ExpectCut("CUT-C4 a truncated manifest forces a full baseline", &cut,
                  bp::ScheduleEvaluationStatus::kCreatedSnapshot);
    test_support::Check(result.baseline_reset,
                        "CUT-C4c baseline reset reported", result.diagnostic);
  }

  // C5 旧版本留下的 v1 manifest：读得出来，但它没有归属信息，不可信。
  {
    CrashCut cut = PrepareCrashCut("cut-c5");
    const std::string whole = MaterialBytes(cut, "S2");
    const std::size_t first_tab = whole.find('\t');
    const std::size_t first_newline = whole.find('\n');
    test_support::Check(first_tab != std::string::npos &&
                            first_newline != std::string::npos &&
                            first_tab < first_newline,
                        "CUT-C5a the v2 header is shaped as expected");
    // "BPMANIFEST2 <count>\t..." -> "BPMANIFEST1 <count>\n<same entries>"
    const std::string version2 = "BPMANIFEST2";
    const std::string v1 =
        "BPMANIFEST1" +
        whole.substr(version2.size(), first_tab - version2.size()) +
        whole.substr(first_newline);
    test_support::Check(Write(ManifestPath(cut.env), v1),
                        "CUT-C5b a version 1 manifest is written");
    PlaceState(cut, "S2");
    const bp::ScheduleEvaluationResult result =
        ExpectCut("CUT-C5 a legacy manifest is never a trusted baseline", &cut,
                  bp::ScheduleEvaluationStatus::kCreatedSnapshot);
    test_support::Check(result.baseline_reset,
                        "CUT-C5c baseline reset reported", result.diagnostic);
    test_support::Check(
        result.diagnostic.find("older version") != std::string::npos,
        "CUT-C5d the diagnostic says the manifest is from an older version",
        result.diagnostic);

    // 重建出来的必须是 v2，否则升一次级就会永远重建下去。
    std::string rebuilt;
    test_support::ReadFile(ManifestPath(cut.env), &rebuilt);
    test_support::Check(rebuilt.compare(0, 11, "BPMANIFEST2") == 0,
                        "CUT-C5e the rebuilt manifest is version 2",
                        rebuilt.substr(0, 32));
  }

  // C6 state 与 manifest 都指向 S2，但 S2 的归档已经不在了。
  {
    CrashCut cut = PrepareCrashCut("cut-c6");
    RemoveFile(cut.env.repository + "/" + cut.archive2);
    PlaceState(cut, "S2");
    PlaceManifest(cut, "S2");
    ExpectCut("CUT-C6 a missing baseline archive forces a full baseline", &cut,
              bp::ScheduleEvaluationStatus::kCreatedSnapshot);
  }

  // C7 manifest 自己声明的仓库不是当前仓库。
  {
    CrashCut cut = PrepareCrashCut("cut-c7");
    std::string bytes = MaterialBytes(cut, "S2");
    const std::string from = "\t" + cut.env.repository + "\t";
    const std::string to = "\t/tmp/other-repository\t";
    test_support::Check(bytes.find(from) != std::string::npos,
                        "CUT-C7a the header really carries the repository");
    ReplaceOnce(&bytes, from, to);
    test_support::Check(Write(ManifestPath(cut.env), bytes),
                        "CUT-C7b the repository field is tampered with");
    PlaceState(cut, "S2");
    const bp::ScheduleEvaluationResult result =
        ExpectCut("CUT-C7 a manifest bound to another repository must not skip",
                  &cut, bp::ScheduleEvaluationStatus::kCreatedSnapshot);
    test_support::Check(result.baseline_reset,
                        "CUT-C7c baseline reset reported", result.diagnostic);
  }

  // C8 manifest 自己声明的源不是当前源。源路径是头行最后一个字段，
  // 后面直接跟换行。
  {
    CrashCut cut = PrepareCrashCut("cut-c8");
    std::string bytes = MaterialBytes(cut, "S2");
    const std::string from = "\t" + cut.env.source + "\n";
    const std::string to = "\t/tmp/other-source\n";
    test_support::Check(bytes.find(from) != std::string::npos,
                        "CUT-C8a the header really carries the source path");
    ReplaceOnce(&bytes, from, to);
    test_support::Check(Write(ManifestPath(cut.env), bytes),
                        "CUT-C8b the source field is tampered with");
    PlaceState(cut, "S2");
    const bp::ScheduleEvaluationResult result =
        ExpectCut("CUT-C8 a manifest bound to another source must not skip",
                  &cut, bp::ScheduleEvaluationStatus::kCreatedSnapshot);
    test_support::Check(result.baseline_reset,
                        "CUT-C8c baseline reset reported", result.diagnostic);
  }
}

// ---- M. 满额名单：写出去的 state 必须读得回来 ----
//
// retain_count 允许取到 kMaxRetainCount，而创建路径会先把新快照 push 进名单、
// 再淘汰。所以名单长度在淘汰之前是 retain + 1 条，而 ScheduleStore 只接受
// kMaxRetainCount 条。若先落盘后淘汰，满额那一轮就会要求写出一份自己都读不回来
// 的 state：Save 拒绝 -> 在写盘处提前返回 -> retention 永远轮不到 -> 下一轮再建
// 一份。结果是仓库无上限增长，而且每一轮都报成功、退出码 0。

void TestManagedListStaysWritableAtTheBound() {
  test_support::Section("M. the managed list stays writable at the bound");

  const Env env = MakeEnv("managed-bound", bp::kMaxRetainCount);
  Write(env.source + "/a.txt", "one");

  bp::ScheduleEvaluationResult result;
  ExpectStatus("BOUND-01 the seed snapshot is created", env, 1000,
               bp::ScheduleEvaluationStatus::kCreatedSnapshot, &result);
  const std::string seed = result.archive_file_name;
  test_support::Check(!seed.empty(), "BOUND-02 the seed snapshot has a name",
                      seed);

  std::string bytes;
  test_support::Check(
      test_support::ReadFile(env.repository + "/" + seed, &bytes),
      "BOUND-03 the seed archive is readable");

  // 把仓库填到文件上界：一共 kMaxRetainCount 份**真实**归档。产品连续跑满
  // 这么多轮之后，磁盘上就是这个样子。
  std::vector<std::string> names;
  names.push_back(seed);
  std::size_t fillers = 0;
  for (std::size_t index = 1; index < bp::kMaxRetainCount; ++index) {
    char name[64];
    std::snprintf(name, sizeof(name), "filler_%04zu.bak", index);
    if (Write(env.repository + "/" + name, bytes)) ++fillers;
    names.push_back(name);
  }
  test_support::Check(
      fillers + 1 == bp::kMaxRetainCount,
      "BOUND-04 the repository holds a full managed list",
      std::to_string(fillers + 1) + "/" + std::to_string(bp::kMaxRetainCount));

  // 用产品自己的 writer 写出一份满额 state：这不是伪造字节，而是产品跑满之后
  // 的真实内容。它必须写得出去。
  bp::ScheduleStore store(env.schedule_file);
  bp::ScheduleDocument document = LoadDocument(env);
  document.config.retain_count = bp::kMaxRetainCount;
  document.state.managed_snapshots.clear();
  for (const std::string& name : names) {
    bp::ScheduledSnapshotRecord record;
    record.file_name = name;
    record.created_time_sec = 1000;
    record.entry_count = 1;
    record.archive_size = 1;
    document.state.managed_snapshots.push_back(record);
  }
  document.state.baseline.snapshot_file_name = seed;
  document.state.baseline.repository_identity =
      bp::RepositoryIdentity(env.repository);
  document.state.baseline.source_path = env.source;

  std::string error;
  test_support::Check(
      document.state.managed_snapshots.size() == bp::kMaxRetainCount,
      "BOUND-05 the crafted list sits exactly at the bound");
  test_support::Check(store.Save(document, &error),
                      "BOUND-06 a full managed list still saves", error);

  // 源变了：这一轮必须建快照、必须真的淘汰，而且 state 必须仍然读得回来。
  Write(env.source + "/b.txt", "two");
  const std::size_t before = RepoArchives(env.repository).size();
  ExpectStatus("BOUND-10 a change at the bound still creates a snapshot", env,
               2000, bp::ScheduleEvaluationStatus::kCreatedSnapshot, &result);
  const std::size_t after = RepoArchives(env.repository).size();
  test_support::Check(after == bp::kMaxRetainCount,
                      "BOUND-11 retention really ran at the bound",
                      std::to_string(before) + " -> " + std::to_string(after));

  bp::ScheduleDocument reloaded;
  const bp::ScheduleLoadStatus status = store.Load(&reloaded, &error);
  test_support::Check(status == bp::ScheduleLoadStatus::kLoaded,
                      "BOUND-12 the state written at the bound loads back",
                      error);
  test_support::Check(
      reloaded.state.managed_snapshots.size() <= bp::kMaxRetainCount,
      "BOUND-13 the persisted list never exceeds the bound",
      std::to_string(reloaded.state.managed_snapshots.size()));
  bool recorded = false;
  for (const bp::ScheduledSnapshotRecord& record :
       reloaded.state.managed_snapshots) {
    if (record.file_name == result.archive_file_name) recorded = true;
  }
  test_support::Check(recorded, "BOUND-14 the new snapshot was recorded",
                      result.archive_file_name);

  // 源没再变：下一轮必须 skip。卡死的话这里会再建一份，仓库继续长。
  ExpectStatus("BOUND-15 the next run skips instead of wedging", env, 3000,
               bp::ScheduleEvaluationStatus::kSkippedNoChanges, &result);
  const std::size_t settled = RepoArchives(env.repository).size();
  test_support::Check(settled == bp::kMaxRetainCount,
                      "BOUND-16 the repository does not grow without bound",
                      std::to_string(settled));
}

// ---- N. 只让 manifest 写失败：state 前进、manifest 停在旧的 ----
//
// 非 root 也能把这一对真实地拆开：在状态目录里放一个**目录**占住
// schedule-manifest.dat 的位置。rename(tmp, target) 必然失败（EISDIR），
// 而同一个目录里的 schedule.json 照常写得进去。
//
// 这正是"崩在 SaveManifest 与 Save(state) 之间"留下的形状，只不过是被
// 真故障而不是被杀进程造出来的：archive 与 state 都已经换成新的，manifest
// 还停在旧的那一份。

void TestManifestOnlySaveFailure() {
  test_support::Section("N. a manifest-only save failure still converges");

  const Env env = MakeEnv("manifest-only", 12);
  Write(env.source + "/a.txt", "one");
  bp::ScheduleEvaluationResult result;
  ExpectStatus("MAN2-01 the first snapshot is created", env, 1000,
               bp::ScheduleEvaluationStatus::kCreatedSnapshot, &result);

  bp::ScheduleStore store(env.schedule_file);
  const std::string manifest = store.manifest_file_path();
  std::string whole;
  test_support::Check(test_support::ReadFile(manifest, &whole),
                      "MAN2-02 the manifest is readable");
  test_support::Check(whole.compare(0, 11, "BPMANIFEST2") == 0,
                      "MAN2-03 the production manifest is version 2",
                      whole.substr(0, 16));

  // 占住 manifest 的写入位置。旧内容先留一份，稍后原样放回去 —— 一次失败的
  // 原子替换本来就不会破坏旧文件。
  test_support::Check(::unlink(manifest.c_str()) == 0,
                      "MAN2-04 the manifest is moved aside");
  test_support::Check(test_support::Mkdir(manifest, 0755),
                      "MAN2-05 the manifest path is now a directory");

  Write(env.source + "/b.txt", "two");
  ExpectStatus("MAN2-06 the snapshot is still created", env, 2000,
               bp::ScheduleEvaluationStatus::kCreatedSnapshot, &result);
  test_support::Check(
      result.diagnostic.find("source manifest could not be saved") !=
          std::string::npos,
      "MAN2-07 the diagnostic names the manifest", result.diagnostic);
  test_support::Check(RepoArchives(env.repository).size() == 2,
                      "MAN2-08 the archive itself was published",
                      std::to_string(RepoArchives(env.repository).size()));

  test_support::Check(::rmdir(manifest.c_str()) == 0,
                      "MAN2-09 the placeholder is removed");
  test_support::Check(Write(manifest, whole),
                      "MAN2-10 the previous manifest content is back");

  // state 已经前进到 S2，manifest 还属于 S1：这一对不配套，绝不能 skip。
  const std::size_t before = RepoArchives(env.repository).size();
  ExpectStatus("MAN2-11 an old manifest with a new state must not skip", env,
               3000, bp::ScheduleEvaluationStatus::kCreatedSnapshot, &result);
  test_support::Check(result.baseline_reset,
                      "MAN2-12 it is reported as a baseline reset",
                      result.diagnostic);
  test_support::Check(RepoArchives(env.repository).size() == before + 1,
                      "MAN2-13 exactly one snapshot was added",
                      std::to_string(before) + " -> " +
                          std::to_string(RepoArchives(env.repository).size()));

  // 收敛：manifest 现在是新的，源没再变，下一轮回到 skip。
  ExpectStatus("MAN2-14 the next run skips again", env, 4000,
               bp::ScheduleEvaluationStatus::kSkippedNoChanges, &result);
  test_support::Check(RepoArchives(env.repository).size() == before + 1,
                      "MAN2-15 the repository stopped growing",
                      std::to_string(RepoArchives(env.repository).size()));
}

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
  TestModeMatrix();
  TestEnableTransition();
  TestEnabledScheduleDoesNotRunImmediately();
  TestBaselineBinding();
  TestBaselineAgainstAnUnreadableRepository();
  TestRetentionKeepsTheBaselineInvariant();
  TestUnsupportedModeIsNeverRunAsFull();
  TestStoreParentDirectoryAndLegacyFiles();
  TestCrashConsistency();
  TestManagedListStaysWritableAtTheBound();
  TestManifestOnlySaveFailure();
  TestStability();
  test_support::RemoveTree(test_support::TempRoot());
  return test_support::Finish("scheduled backup");
}
