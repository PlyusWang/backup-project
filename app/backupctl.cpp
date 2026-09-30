// backupctl.cpp
//
// 命令行入口。这里只做参数分流：摘全局选项、拿全应用单实例锁、补默认路径、
// 按命令名分派。真正的打包 / 解包在引擎与归档读写器里，筛选在 Filter 里，
// 定时备份在 ScheduledBackupService 里，main() 不直接碰文件系统。
//
// ---- 当前语法（与实现逐条对应）----
//
//   backupctl backup <source_directory> [--include <rule>]... [--exclude
//   <rule>]...
//                                     [--pack ...] [--compression ...]
//                                     [--encryption ...]
//   backupctl preview <source_directory> [--include <rule>]... [--exclude
//   <rule>]... backupctl restore <file_name> <destination_directory> backupctl
//   schedule show|set|enable|disable|run|history|watch backupctl repository
//   list|delete <file_name> backupctl config repository show|set <path>
//
// ---- 业务模型：repository-driven ----
//
// 产品 CLI 与 Modern GUI 共用同一套模型，命令里**没有**"把备份写到哪就是哪"
// 这个能力：
//
//   backup  归档写进**配置好的仓库**，文件名由 BackupCatalog::BuildArchivePath
//           生成（<source-base>_YYYYMMDD_HHMMSS.bak），调用方给不出路径；
//           产物**永远是 v2 container**（magic "BKPCNT2"），不存在"不带
//           pipeline 选项就退回 legacy 写入"这种分支。
//   restore <file_name> 是仓库内的单组件 .bak 名字，解析交给
//           BackupCatalog::Resolve：拒绝空串、"."、".."、含 '/' 或 '\\'、
//           内嵌 NUL、不以 .bak 结尾，也拒绝解析结果不是仓库直接子项的情况。
//   preview 只读：列出"用这些筛选规则备份会进归档的条目"，不创建归档、不需要
//           仓库、不写任何状态。它与 Modern GUI 的 Manual Backup 预览调用同一个
//           核心（backupproject::PreviewBackupSelection）。
//
// ---- legacy v0.1 的边界 ----
//
// v0.1 归档（magic "BKPARCH"）的**读取**兼容仍然保留：restore 照样能恢复
// 现存的老归档。
//
// 但 v0.1 的**写入**不属于产品功能。产品 CLI 没有任何一条命令能创建任意路径的
// legacy 归档；那项能力只存在于测试夹具 tests/tools/archive_cli.cpp
// （build/archive-cli，不在默认构建目标里、不安装、不是用户命令）。
//
// 筛选规则语法见 docs/filter_usage.md：name: / path: / stem: / ext: /
// type: / size: / mtime:，通配符只支持 * ? **。restore 不接受筛选规则，
// preview 与 backup 接受同一组规则（含一条规则内的 compound AND）。
//
// 密码只从 /dev/tty 交互读取：命令行里没有密码选项，也没有环境变量默认值，
// 更不会从管道偷偷读。
//
// 全应用单实例：**整个产品同一时刻只允许一个进程**。这个命令在进入任何业务
// 逻辑（读配置、读仓库、写计划）之前先抢 ApplicationInstanceLock，GUI 与它
// 用的是同一把锁、同一个路径。preview 同样受这把锁约束——它不是"因为只读
// 所以可以并发"的例外。锁的顺序永远是
// ApplicationInstanceLock -> SchedulerLock。
//
// 退出码：
//   0  成功
//   1  操作失败（路径不对、源目录不存在、文件类型不支持、归档损坏、I/O 错误等）
//   2  命令行用法错误（未知命令/选项、缺少选项值、多余的位置参数、
//      非法筛选规则、重复的单值选项、越界数字）
//   3  已经有另一个 backup-project 实例在跑（GUI 或 CLI）
//
// 用法错误一律在**碰任何持久状态之前**返回：解析阶段不写任何文件。

#include <iostream>
#include <string>
#include <vector>

#include "app_paths.h"
#include "application_instance_lock.h"
#include "cli_app.h"
#include "remote_commands.h"

namespace {

// 全局选项可以在任何位置出现。摘掉之后业务子命令完全看不到它们，
// 也就不会和 backup / restore 的位置参数打架。
//
// 这里也是"重复的单值选项必须报错"这条规则的第一道：--config-file 出现两次
// 不是"后者覆盖前者"，而是用户对同一件事说了两遍——多半是脚本拼接出了问题，
// 静默取一个会让真正想指定的那个被丢掉。
bool ExtractGlobalOption(std::vector<std::string>* arguments,
                         const std::string& name, std::string* value,
                         bool* seen, std::string* error_message) {
  std::size_t index = 0;
  while (index < arguments->size()) {
    if ((*arguments)[index] != name) {
      ++index;
      continue;
    }
    if (*seen) {
      *error_message = name + " was given more than once";
      return false;
    }
    if (index + 1 >= arguments->size()) {
      *error_message = name + " needs a path argument";
      return false;
    }
    const std::string candidate = (*arguments)[index + 1];
    // "--config-file --schedule-file x" 这种写法里，"--schedule-file" 会被
    // 当成路径。那不是缺少参数，而是**参数被吃掉了**，必须明确报出来。
    if (candidate.size() > 2 && candidate[0] == '-' && candidate[1] == '-') {
      *error_message = name + " needs a path argument, but got the option '" +
                       candidate + "'";
      return false;
    }
    *value = candidate;
    *seen = true;
    arguments->erase(
        arguments->begin() + static_cast<std::ptrdiff_t>(index),
        arguments->begin() + static_cast<std::ptrdiff_t>(index) + 2);
    // 不 ++index：erase 之后同一个下标已经是下一个待检查的元素。
  }
  return true;
}

bool IsKnownCommand(const std::string& command) {
  if (command == "realtime") return true;
  if (command == "remote") return true;
  return command == "backup" || command == "restore" || command == "preview" ||
         command == "schedule" || command == "repository" ||
         command == "config";
}

}  // namespace

int main(int argc, char* argv[]) {
  backupproject::CliContext context;
  context.program_name = (argc > 0) ? argv[0] : "backupctl";

  std::vector<std::string> arguments;
  for (int index = 1; index < argc; ++index) arguments.push_back(argv[index]);

  std::string option_error;
  bool saw_config_file = false;
  bool saw_schedule_file = false;
  bool saw_realtime_file = false;
  if (!ExtractGlobalOption(&arguments, "--config-file",
                           &context.config_file_path, &saw_config_file,
                           &option_error) ||
      !ExtractGlobalOption(&arguments, "--schedule-file",
                           &context.schedule_file_path, &saw_schedule_file,
                           &option_error) ||
      !ExtractGlobalOption(&arguments, "--realtime-file",
                           &context.realtime_file_path, &saw_realtime_file,
                           &option_error)) {
    std::cerr << "Error: " << option_error << "\n\n";
    backupproject::PrintCliUsage(context.program_name, std::cerr);
    return backupproject::kCliExitUsageError;
  }

  if (arguments.size() == 1 &&
      (arguments[0] == "--help" || arguments[0] == "-h")) {
    // 纯粹的帮助输出不碰任何配置、仓库或计划状态，因此不需要单实例锁：
    // 已经有 GUI 在跑的时候，用户仍然应该能看到用法。
    backupproject::PrintCliUsage(context.program_name, std::cout);
    return backupproject::kCliExitSuccess;
  }
  if (arguments.empty()) {
    backupproject::PrintCliUsage(context.program_name, std::cerr);
    return backupproject::kCliExitUsageError;
  }

  // 先把"命令名认不认识"判掉再抢锁：未知命令是用法错误，不该被一句
  // "已有另一个实例"盖过去——那会把排障方向直接带偏。
  const std::string command = arguments[0];
  if (!IsKnownCommand(command)) {
    std::cerr << "Error: unknown command '" << command << "'.\n\n";
    backupproject::PrintCliUsage(context.program_name, std::cerr);
    return backupproject::kCliExitUsageError;
  }

  // ---- 全应用单实例 ----
  //
  // 位置很关键：必须在**任何** Load/Save 之前。先读配置再发现"已经有另一个
  // 实例"，并发访问已经发生了，那把锁也就没意义了。
  std::string application_lock_path;
  std::string lock_error;
  backupproject::ApplicationInstanceLock application_lock;
  if (!backupproject::DefaultApplicationInstanceLockPath(&application_lock_path,
                                                         &lock_error)) {
    std::cerr << "Error: " << lock_error << "\n";
    return backupproject::kCliExitOperationFailed;
  }
  const backupproject::ApplicationInstanceStatus lock_status =
      application_lock.Acquire(application_lock_path, &lock_error);
  if (lock_status != backupproject::ApplicationInstanceStatus::kAcquired) {
    std::cerr << "Error: " << lock_error << "\n";
    return lock_status ==
                   backupproject::ApplicationInstanceStatus::kAlreadyRunning
               ? backupproject::kApplicationAlreadyRunningExitCode
               : backupproject::kCliExitOperationFailed;
  }

  // 产品默认位置：与 Modern GUI 严格同源（见 app_paths.h）。
  // 空串表示连 HOME 都拿不到；那时只有真正用到它的子命令才会报错。
  if (context.config_file_path.empty()) {
    context.config_file_path = backupproject::DefaultConfigFilePath();
  }
  if (context.schedule_file_path.empty()) {
    context.schedule_file_path = backupproject::DefaultScheduleFilePath();
  }
  if (context.realtime_file_path.empty()) {
    context.realtime_file_path = backupproject::DefaultRealtimeFilePath();
  }

  const std::vector<std::string> rest(arguments.begin() + 1, arguments.end());

  if (command == "backup") {
    return backupproject::RunBackupCommand(context, rest);
  }
  if (command == "restore") {
    return backupproject::RunRestoreCommand(context, rest);
  }
  if (command == "preview") {
    return backupproject::RunPreviewCommand(context, rest);
  }
  if (command == "schedule") {
    return backupproject::RunScheduleCommand(context, rest);
  }
  if (command == "realtime") {
    return backupproject::RunRealtimeCommand(context, rest);
  }
  if (command == "remote") {
    return backupproject::RunRemoteCommand(context, rest);
  }
  if (command == "repository") {
    return backupproject::RunRepositoryCommand(context, rest);
  }
  return backupproject::RunConfigCommand(context, rest);
}
