// scheduled_backup_service.h
//
// Scheduled + Full 的共享核心。
//
// 它把"到点了"这件事完整走完一遍：
//
//   到点 -> 扫描源目录 -> 与上一份成功 manifest 比较
//        -> 没变化：skip（不调用 BackupEngine、不产生 .bak、不动 manifest）
//        -> 有变化：创建一份**完整独立**的 v2 快照
//        -> 成功后落盘 manifest / state
//        -> retention（只淘汰 scheduler 自己管理的旧快照）
//        -> 记录 history
//
// 三件事它**不做**：
//
//   1. 不做事。它不取系统时间。now_sec 由调用方注入，产品层传
//      std::time(nullptr)，单元测试传假时钟——这样"1 分钟计划"不需要真的
//      等一分钟，时钟回拨/前跳也能被测到。
//   2. 不做增量存储。有变化时产出的仍然是一份可以单独拷走、单独恢复的
//      完整 .bak，没有 baseline 依赖链。
//   3. 不碰加密。无人值守的计划任务没有安全的持久密钥来源，配置层已经把
//      encryption != none 拒掉了；这里不提供任何密码参数。
//
// 依赖方向是单向的：
//
//   ScheduledBackupService -> ScheduleStore / BackupCatalog / BackupEngine
//
// 反过来不存在：BackupCatalog 不知道 scheduler 的存在。所以删掉
// schedule.json 之后，仓库里的 .bak 照样能列出、恢复、删除，
// 只是"来源"退化成未知。
//
// 本文件是纯 C++17：不依赖 Qt。

#ifndef BACKUP_PROJECT_INCLUDE_SCHEDULED_BACKUP_SERVICE_H_
#define BACKUP_PROJECT_INCLUDE_SCHEDULED_BACKUP_SERVICE_H_

#include <cstdint>
#include <string>

#include "schedule_store.h"

namespace backupproject {

enum class ScheduleEvaluationStatus {
  // 计划没启用。不写盘、不动 next_run。
  kDisabled,
  // 还没到点。
  kNotDue,
  // 到点了，但源目录与上一份成功快照没有差别 —— 这是本 PR 的核心语义：
  // 只有真的发生变化才建立新副本。
  kSkippedNoChanges,
  kCreatedSnapshot,
  // 新快照成功创建，但旧快照没删掉。两者必须分开报告：
  // "计划备份成功，但旧版本淘汰失败"不等于"备份失败"。
  kCreatedWithRetentionWarning,
  kFailed,
  // 落盘的配置本身不合法（结构读得懂，但业务规则不认）。
  //
  // 与 kFailed 刻意分开，因为后果完全不同：
  //   * kFailed 是"这一次运行失败了"，next_run 照常推进、history 照常记录，
  //     下一轮到点再试；
  //   * kConfigInvalid 是"这份计划已经被挂起"：**一个字节都不写**（改了也存
  //     不回去），next_run 原地不动，也不记 history。调用方据此进入明确的
  //     config-error 状态并停止周期性重试——否则 GUI 会每个 tick 都重做一遍
  //     完整校验，永远停不下来。
  // 恢复方式只有一条：用户显式保存一份合法的配置（GUI 的保存 / CLI 的
  // schedule set）。产品禁止多进程，所以不存在"别的进程在背后修好了文件"。
  kConfigInvalid,
};

const char* ScheduleEvaluationStatusKey(ScheduleEvaluationStatus status);
const char* ScheduleEvaluationStatusText(ScheduleEvaluationStatus status);

struct ScheduleEvaluationResult {
  ScheduleEvaluationStatus status = ScheduleEvaluationStatus::kDisabled;

  // true 表示这一轮没有可用的上一份 manifest（从来没跑过、或者 manifest
  // 被删/损坏），因此产出的是一份"首次完整快照"，而不是"相对上一版的变化"。
  bool first_snapshot = false;

  // true 表示"以前确实记过 baseline，但它已经不可信了"——对应快照被用户删掉、
  // 仓库被换成了另一个、源目录被换掉、或者该快照不再归本 scheduler 管理。
  //
  // 这一轮和 first_snapshot 的结果完全一样：产出一份**完整基线快照**并重建
  // baseline 记录，绝不因为"manifest 恰好相同"而 skip。
  bool baseline_reset = false;

  ChangeSummary changes;

  // 只有真的产出归档时非空（单组件文件名）。
  std::string archive_file_name;

  // 出错原因原文，或本轮的说明。典型例子：这一轮重建了一份完整基线快照，
  // 原因是记录的基线已经不可用（快照被删 / 换了仓库 / 换了源 / manifest
  // 读不出来） —— 那叫 baseline reset，不叫"首次快照"。绝不含密码。
  std::string diagnostic;

  std::int64_t next_run_time_sec = 0;
  std::uint64_t retention_deleted = 0;
  std::uint64_t retention_failed = 0;
};

// next_run = now + interval。整秒运算，溢出被夹到 INT64_MAX。
std::int64_t ScheduleNextRunTime(std::int64_t now_sec,
                                 std::uint32_t interval_minutes);

// disabled -> enabled 这一次转换应该怎么处理时间表。
//
// 语义（GUI 与 CLI 完全一致，两边都只调用这一个函数）：
//   * 只有"启用"这一侧才动 next_run；停用不重置，方便下次启用重新算。
//   * 已经启用时是 no-op —— show / load / set 都不许顺手把时间表推后，
//     否则用户每改一次配置，下一次运行就被推迟一整个周期。
//   * 首次启用把 next_run 推成 now + interval：用户刚勾上"启用"，下一秒就
//     看到一次备份跑起来是反直觉的。想立刻跑有明确的入口（GUI 的"立即检查
//     并运行"、CLI 的 schedule run），不需要靠 next_run = 0 的副作用。
void ApplyScheduleEnableTransition(ScheduleDocument* document, bool was_enabled,
                                   std::int64_t now_sec);

// schedule-manifest.dat 里那份源清单到底能不能用。
//
// 这是本 PR 最重要的一条不变式：**manifest 只有在能证明它属于当前仓库里一份
// 真实存在的、仍然归本 scheduler 管理的快照时才有意义**。单独一份 manifest
// 只说明"上次扫描到的源状态"，它证明不了仓库里还有与它对应的那份备份。
enum class ScheduleBaselineStatus {
  // 从来没记录过 baseline（首次运行、或者旧版本的 schedule.json）。
  kMissing,
  // 记录的仓库与当前仓库不是同一个。
  kRepositoryChanged,
  // 记录的源目录与当前计划源目录不是同一个。
  kSourceChanged,
  // 记录的那份快照已经不在 managed 名单里：用户手工删了、或者被别的进程删了。
  kNotManaged,
  // 记录的那份快照在 managed 名单里，但在仓库里已经解析不出来了
  // （文件被外部删掉、被替换成软链接、名字不再合法……）。
  kSnapshotGone,
  kValid,
};

const char* ScheduleBaselineStatusKey(ScheduleBaselineStatus status);
std::string ScheduleBaselineStatusText(ScheduleBaselineStatus status);

// 判定 baseline 是否仍然可用。document 必须是**已经 reconcile 过**的文档
// （即 managed 名单已经剔除掉仓库里不存在的记录），否则 kNotManaged 与
// kSnapshotGone 的分工就失去意义。
//
// 顺序是刻意的：先做三个纯字符串比较（免费），再做任何文件系统动作。
ScheduleBaselineStatus EvaluateScheduleBaseline(
    const ScheduleDocument& document, const std::string& repository_path,
    std::string* error_message);

// 是否到点。
//
//   * next_run <= 0 视为"从未算过" -> 到点（首次启用立即跑一次）；
//   * now >= next_run -> 到点（含 overdue：应用重启后只补跑一次，
//     因为跑完就会把 next_run 重算成 now + interval，不会按错过的轮数连补）；
//   * now < next_run 且回退幅度超过一个周期 -> 视为到点。
//     一次 clock backward 不该把计划冻住好几天；把不可信的未来 next_run
//     直接作废，比"等到那个时间点"更符合用户预期。
bool IsScheduleDue(std::int64_t now_sec, std::int64_t next_run_time_sec,
                   std::uint32_t interval_minutes);

// "retention 成功 / 失败" 到运行状态的映射。
//
// 单独抽成一个函数不是为了好看：非 root 用户在**可写**仓库里没有任何合法手段
// 让 unlink 真的失败（删除权限来自目录，不来自文件；chattr +i 需要 root），
// 所以"新快照成功、淘汰失败"这条端到端路径只能通过 RunRetention 的公开入口
// 加这个纯函数来覆盖，而不是靠造假。把它留在这里，测试就能直接钉住语义。
ScheduleEvaluationStatus StatusForRetention(bool retention_ok);

class ScheduledBackupService {
 public:
  // store 必须活得比本对象久；repository_path 是备份仓库的显式路径
  // （不从配置里猜）。
  ScheduledBackupService(std::string repository_path, ScheduleStore* store);

  // 换仓库。给长运行的调用方用（backupctl schedule watch、任何常驻 runner）：
  // 它们在每一轮真正评估之前重新从共享 ConfigManager 读一次当前仓库，
  // 然后喂进来。
  //
  // 为什么必须有这个入口：进程启动时读一次仓库、之后整个生命周期都用它，
  // 会在用户改了 config 之后继续往**旧仓库**写备份——那是静默的数据错位，
  // 比一次明确的失败糟得多。仓库换了之后，baseline 的仓库 identity 对不上，
  // 于是下一轮会老老实实重建一份完整基线快照。
  void SetRepositoryPath(std::string repository_path);
  const std::string& repository_path() const { return repository_path_; }

  // 完整走一轮评估，**先判断是否到点**。now_sec 由调用方注入。
  //
  // 这是自动路径（GUI 的 timer tick、backupctl schedule watch）用的入口。
  //
  // 返回值：
  //   true  —— 这一轮评估本身走完了（哪怕结果是 kFailed，也可以读
  //            result->diagnostic 拿到原因）；
  //   false —— 连 store 都读不出来这种基础设施问题，error_message 里有原因。
  //
  // 不是线程安全的：同一进程内由 GUI 的 busy 边界串行化，跨进程由
  // SchedulerLock 串行化。
  bool Evaluate(std::int64_t now_sec, ScheduleEvaluationResult* result,
                std::string* error_message);

  // 同一条评估流程，但**跳过"还没到点"这一条**。
  //
  // 这是"立即检查并运行"的语义，也是 §56 人工验收里连点几次的那个按钮：
  // 忽略时间表，现在就检查一遍。它**不是**强制备份——没有变化照样 skip，
  // 仍然不调用 BackupEngine、不产生 .bak。
  //
  // 之所以必须是产品入口而不是测试后门：人工验收和 GUI 的"立即检查并运行"
  // 都要用它，缺了它就只能干等一个完整周期，而那正好掩盖了变化检测本身。
  bool EvaluateNow(std::int64_t now_sec, ScheduleEvaluationResult* result,
                   std::string* error_message);

  // 只做 retention。供测试与显式维护入口使用。
  bool RunRetention(ScheduleDocument* document, std::uint64_t* deleted,
                    std::uint64_t* failed, std::string* error_message) const;

  // 自愈：把已经不在仓库里的 managed record 摘掉。
  //
  // 关键安全点：仓库本身不可用时**什么都不做**。如果只要 Resolve 失败就摘记录，
  // 那么"仓库没挂载"会被误判成"用户删了这些快照"，ownership 名单就被清空了，
  // 重新挂载之后那些旧快照再也不会被 retention 回收。
  static void ReconcileManagedSnapshots(const std::string& repository_path,
                                        ScheduleDocument* document);

 private:
  bool EvaluateInternal(std::int64_t now_sec, bool force,
                        ScheduleEvaluationResult* result,
                        std::string* error_message);

  std::string repository_path_;
  ScheduleStore* store_ = nullptr;
};

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_SCHEDULED_BACKUP_SERVICE_H_
