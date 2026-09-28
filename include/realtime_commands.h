// realtime_commands.h
//
// PR #19：backupctl realtime 子命令。
//
// 与 schedule 子命令同构：解析参数 → 调共享核心 → 翻译结果。
// 这里没有自己的策略/校验/retention 实现：
//
//   config      -> RealtimeStore / ValidateRealtimeConfig /
//   ValidateRealtimeForEnable 触发        -> InotifyWatcher + RealtimeDebouncer
//   执行        -> RunRealtimeBackupOnce（内部再走 BackupEngine /
//   RunIncrementalBackup） 历史        -> ListRealtimeSnapshots
//
// 前端只负责"把用户的话翻译成核心调用"，以及把核心的结论如实打印出来。

#ifndef BACKUP_PROJECT_INCLUDE_REALTIME_COMMANDS_H_
#define BACKUP_PROJECT_INCLUDE_REALTIME_COMMANDS_H_

#include <string>
#include <vector>

#include "cli_app.h"

namespace backupproject {

// arguments 是去掉 "realtime" 之后的参数（show / set / enable / disable /
// watch / history ...）。
int RunRealtimeCommand(const CliContext& context,
                       const std::vector<std::string>& arguments);

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_REALTIME_COMMANDS_H_
