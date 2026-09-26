// cli_app.h
//
// backupctl 的命令层。
//
// main() 只做三件事：摘出全局选项（--config-file / --schedule-file）、
// 按默认位置补齐路径、把剩下的参数交给对应的子命令。真正的业务逻辑在
// src/cli/cli_commands.cpp 里，而它自己也只做"解析参数 -> 调用核心 ->
// 翻译结果"。
//
// 一条贯穿全文件的架构约束：CLI 与 Modern GUI 用的是**同一份**核心。
//
//   scheduled 相关   -> ScheduledBackupService / ScheduleStore
//   repository 相关  -> ConfigManager / BackupCatalog
//   算法 key         -> backup_option_keys（与 GUI 同一张表）
//   密码             -> terminal_secret（argv / 环境变量一律不用）
//
// CLI 不复制第二套 scheduler，也不自己拼 repository 路径。

#ifndef BACKUP_PROJECT_INCLUDE_CLI_APP_H_
#define BACKUP_PROJECT_INCLUDE_CLI_APP_H_

#include <ostream>
#include <string>
#include <vector>

namespace backupproject {

// 退出码集中定义在这里，main() 与各子命令共用，代码里不出现魔法数字。
inline constexpr int kCliExitSuccess = 0;
inline constexpr int kCliExitOperationFailed = 1;
inline constexpr int kCliExitUsageError = 2;

// 一次调用的上下文。三个路径都由 main() 定下来：
// 产品默认值来自 app_paths.h（与 Modern GUI 严格同源），
// 自动测试用 --config-file / --schedule-file 指到临时目录。
struct CliContext {
  std::string program_name = "backupctl";
  std::string config_file_path;
  std::string schedule_file_path;
};

// 同一个用法文本：--help 与用法错误共用，输出到哪个流由调用方决定。
void PrintCliUsage(const std::string& program_name, std::ostream& output);

// arguments 是去掉命令名之后的参数（即 backupctl <command> 之后的部分）。
int RunBackupCommand(const CliContext& context,
                     const std::vector<std::string>& arguments);
int RunRestoreCommand(const CliContext& context,
                      const std::vector<std::string>& arguments);
int RunScheduleCommand(const CliContext& context,
                       const std::vector<std::string>& arguments);
int RunRepositoryCommand(const CliContext& context,
                         const std::vector<std::string>& arguments);
int RunConfigCommand(const CliContext& context,
                     const std::vector<std::string>& arguments);

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_CLI_APP_H_
