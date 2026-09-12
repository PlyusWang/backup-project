// backupctl.cpp
//
// 命令行入口。这里只做三件事：解析参数、调用 BackupEngine、把结果翻译成
// 退出码。真正的打包 / 解包在引擎和归档读写器里，筛选在 Filter 里，
// main() 不直接碰文件系统。
//
// 用法：
//   backupctl backup <source_directory> <backup_file> [--include <rule>]...
//   [--exclude <rule>]... backupctl restore <backup_file>
//   <destination_directory>
//
// 备份产物是一个单独的归档文件（推荐扩展名 .bak），里面是自定义格式：
// 全局 header + 逐条 entry header + 原样照抄的文件正文。是打包不是压缩。
//
// 筛选规则语法见 docs/filter_usage.md：name: / path: / stem: / ext: /
// type: / size: / mtime:，通配符只支持 * ? **。restore 不接受筛选规则。
//
// 退出码：
//   0  成功
//   1  操作失败（路径不对、文件类型不支持、归档损坏、I/O 错误等）
//   2  命令行用法错误（含非法筛选规则）

#include <iostream>
#include <string>
#include <vector>

#include "backup_engine.h"
#include "filter.h"

namespace {

// 退出码集中定义在这里，main() 里不出现魔法数字。
constexpr int kExitSuccess = 0;
constexpr int kExitOperationFailed = 1;
constexpr int kExitUsageError = 2;

// 同一个用法文本：--help 时打到 stdout，用法错误时打到 stderr，
// 输出到哪个流由调用方传进来。
void PrintUsage(const std::string& program_name, std::ostream& output) {
  output
      << "Usage:\n"
      << "  " << program_name
      << " backup <source_directory> <backup_file> [--include <rule>]... "
         "[--exclude <rule>]...\n"
      << "  " << program_name
      << " restore <backup_file> <destination_directory>\n"
      << "\n"
      << "Filter rules (see docs/filter_usage.md):\n"
      << "  name: / path: / stem: / ext: / type:file|folder / size: / mtime:\n"
      << "  wildcards: * (no '/'), ? (one char, no '/'), ** (may cross '/')\n"
      << "  examples: --include 'ext:cpp;h' --exclude 'path:**/build/**'\n"
      << "\n"
      << "Options:\n"
      << "  --help, -h  Show this help message.\n";
}

// 从命令行解析筛选规则。规则非法时返回 false，并把原因写进 error_message。
// 注意：这里只做规则解析，真正的失败信息由 Filter 给出，CLI 不自己判断语法。
bool ParseFilterRules(int argc, char* argv[], int first_rule_index,
                      backupproject::Filter* filter,
                      std::string* error_message) {
  for (int index = first_rule_index; index < argc; ++index) {
    const std::string option = argv[index];
    backupproject::FilterAction action = backupproject::FilterAction::kInclude;
    if (option == "--include") {
      action = backupproject::FilterAction::kInclude;
    } else if (option == "--exclude") {
      action = backupproject::FilterAction::kExclude;
    } else {
      *error_message = "unknown option '" + option + "'";
      return false;
    }
    if (index + 1 >= argc) {
      *error_message = option + " needs a rule argument";
      return false;
    }
    if (!filter->AddRule(action, argv[index + 1], error_message)) {
      return false;
    }
    ++index;  // 跳过规则值本身
  }
  return true;
}

// 执行 backup 子命令。失败时把引擎给出的原因打到 stderr 并返回 1。
int RunBackup(const std::string& source_directory,
              const std::string& backup_file,
              const backupproject::Filter& filter) {
  backupproject::BackupEngine engine;
  std::string error_message;
  if (!engine.Backup(source_directory, backup_file, filter, &error_message)) {
    std::cerr << "Error: " << error_message << '\n';
    return kExitOperationFailed;
  }
  std::cout << "Backup completed successfully.\n";
  return kExitSuccess;
}

// 和 RunBackup 对称，只是走 Restore 流程。restore 不接受筛选规则：
// 归档里有什么就恢复什么，筛选只发生在打包阶段。
int RunRestore(const std::string& backup_file,
               const std::string& destination_directory) {
  backupproject::BackupEngine engine;
  std::string error_message;
  if (!engine.Restore(backup_file, destination_directory, &error_message)) {
    std::cerr << "Error: " << error_message << '\n';
    return kExitOperationFailed;
  }
  std::cout << "Restore completed successfully.\n";
  return kExitSuccess;
}

}  // namespace

int main(int argc, char* argv[]) {
  const std::string program_name = (argc > 0) ? argv[0] : "backupctl";

  // --help / -h 只打印帮助并正常退出，不进任何业务逻辑。
  if (argc == 2 &&
      (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
    PrintUsage(program_name, std::cout);
    return kExitSuccess;
  }

  // 连命令都没有时也是用法错误（帮助分支在上面已经处理过了）。
  if (argc < 2) {
    PrintUsage(program_name, std::cerr);
    return kExitUsageError;
  }

  const std::string command = argv[1];
  if (command == "backup") {
    // backup 至少要两个位置参数：源目录与备份文件。
    if (argc < 4) {
      std::cerr << "Error: 'backup' expects <source_directory> and "
                   "<backup_file>.\n\n";
      PrintUsage(program_name, std::cerr);
      return kExitUsageError;
    }
    backupproject::Filter filter;
    std::string rule_error;
    if (!ParseFilterRules(argc, argv, 4, &filter, &rule_error)) {
      // 非法规则属于命令行错误：明确报错、非 0 退出，而且此时还没有创建
      // 任何文件，自然不会留下半成品 .bak。
      std::cerr << "Error: " << rule_error << "\n\n";
      PrintUsage(program_name, std::cerr);
      return kExitUsageError;
    }
    return RunBackup(argv[2], argv[3], filter);
  }

  if (command == "restore") {
    // restore 固定两个位置参数，并且不接受 --include / --exclude。
    if (argc != 4) {
      std::cerr << "Error: 'restore' expects <backup_file> and "
                   "<destination_directory>.\n\n";
      PrintUsage(program_name, std::cerr);
      return kExitUsageError;
    }
    return RunRestore(argv[2], argv[3]);
  }

  // 其他命令名当前都不认识。保留非 0 退出码，方便脚本判断失败。
  std::cerr << "Error: unknown command '" << command << "'.\n\n";
  PrintUsage(program_name, std::cerr);
  return kExitUsageError;
}
