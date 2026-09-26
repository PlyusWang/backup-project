// backupctl.cpp
//
// 命令行入口。这里只做参数分流：摘全局选项、补默认路径、按命令名分派。
// 真正的打包 / 解包在引擎与归档读写器里，筛选在 Filter 里，
// 定时备份在 ScheduledBackupService 里，main() 不直接碰文件系统。
//
// 用法：
//   backupctl backup <source_directory> <backup_file> [filter...] [pipeline...]
//   backupctl restore <backup_file> <destination_directory>
//   backupctl schedule show|set|enable|disable|run|history|watch
//   backupctl repository list|delete
//   backupctl config repository show|set
//
// 备份产物是一个单独的归档文件（推荐扩展名 .bak）。
//
// 两条互不干扰的路径：
//   * 完全不出现 --pack / --compression / --encryption 时，行为与 v0.1 一字不差
//     （legacy 归档，magic "BKPARCH"）；
//   * 只要出现任意一个 pipeline 选项就走 v2 container（magic "BKPCNT2"）。
//   这样现存的 CLI 回归脚本一条都不会被打破。
//
// 筛选规则语法见 docs/filter_usage.md：name: / path: / stem: / ext: /
// type: / size: / mtime:，通配符只支持 * ? **。restore 不接受筛选规则。
//
// 密码只从 /dev/tty 交互读取：命令行里没有密码选项，也没有环境变量默认值，
// 更不会从管道偷偷读。
//
// 退出码：
//   0  成功
//   1  操作失败（路径不对、文件类型不支持、归档损坏、I/O 错误等）
//   2  命令行用法错误（含非法筛选规则、未知选项）

#include <iostream>
#include <string>
#include <vector>

#include "app_paths.h"
#include "cli_app.h"

namespace {

// 全局选项可以在任何位置出现。摘掉之后业务子命令完全看不到它们，
// 也就不会和 backup / restore 的位置参数打架。
bool ExtractGlobalOption(std::vector<std::string>* arguments,
                         const std::string& name, std::string* value,
                         std::string* error_message) {
  for (std::size_t index = 0; index < arguments->size(); ++index) {
    if ((*arguments)[index] != name) continue;
    if (index + 1 >= arguments->size()) {
      *error_message = name + " needs a path argument";
      return false;
    }
    *value = (*arguments)[index + 1];
    arguments->erase(
        arguments->begin() + static_cast<std::ptrdiff_t>(index),
        arguments->begin() + static_cast<std::ptrdiff_t>(index) + 2);
    return true;
  }
  return true;
}

}  // namespace

int main(int argc, char* argv[]) {
  backupproject::CliContext context;
  context.program_name = (argc > 0) ? argv[0] : "backupctl";

  std::vector<std::string> arguments;
  for (int index = 1; index < argc; ++index) arguments.push_back(argv[index]);

  std::string option_error;
  if (!ExtractGlobalOption(&arguments, "--config-file",
                           &context.config_file_path, &option_error) ||
      !ExtractGlobalOption(&arguments, "--schedule-file",
                           &context.schedule_file_path, &option_error)) {
    std::cerr << "Error: " << option_error << "\n\n";
    backupproject::PrintCliUsage(context.program_name, std::cerr);
    return backupproject::kCliExitUsageError;
  }

  if (arguments.size() == 1 &&
      (arguments[0] == "--help" || arguments[0] == "-h")) {
    backupproject::PrintCliUsage(context.program_name, std::cout);
    return backupproject::kCliExitSuccess;
  }
  if (arguments.empty()) {
    backupproject::PrintCliUsage(context.program_name, std::cerr);
    return backupproject::kCliExitUsageError;
  }

  // 产品默认位置：与 Modern GUI 严格同源（见 app_paths.h）。
  // 空串表示连 HOME 都拿不到；那时只有真正用到它的子命令才会报错。
  if (context.config_file_path.empty()) {
    context.config_file_path = backupproject::DefaultConfigFilePath();
  }
  if (context.schedule_file_path.empty()) {
    context.schedule_file_path = backupproject::DefaultScheduleFilePath();
  }

  const std::string command = arguments[0];
  const std::vector<std::string> rest(arguments.begin() + 1, arguments.end());

  if (command == "backup") {
    return backupproject::RunBackupCommand(context, rest);
  }
  if (command == "restore") {
    return backupproject::RunRestoreCommand(context, rest);
  }
  if (command == "schedule") {
    return backupproject::RunScheduleCommand(context, rest);
  }
  if (command == "repository") {
    return backupproject::RunRepositoryCommand(context, rest);
  }
  if (command == "config") {
    return backupproject::RunConfigCommand(context, rest);
  }

  // 其他命令名当前都不认识。保留非 0 退出码，方便脚本判断失败。
  std::cerr << "Error: unknown command '" << command << "'.\n\n";
  backupproject::PrintCliUsage(context.program_name, std::cerr);
  return backupproject::kCliExitUsageError;
}
