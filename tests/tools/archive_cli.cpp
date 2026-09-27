// archive_cli.cpp
//
// **测试专用**的归档命令行工具，不是产品界面的一部分。
//
// 为什么需要它：产品 CLI（backupctl）的业务模型与 Modern GUI 完全一致 ——
// repository-driven：归档由 BackupCatalog 在**配置好的仓库里**命名，调用方
// 不能指定任意路径。但归档格式本身（v0.1 legacy 与 v2 container）仍然需要
// 端到端回归：手工构造的坏归档、路径拓扑、metadata 往返、两个 pack 后端的
// 逐字节比较……这些用例都需要"把归档写到指定路径、再从指定路径恢复"。
//
// 那部分能力因此搬到这里：
//
//   archive-cli backup <source_directory> <backup_file> [filter...] [pipeline...]
//   archive-cli restore <backup_file> <destination_directory>
//
// 它**不**出现在 backupctl --help 里，不参与 GUI/CLI parity，也不是用户功能：
// 它是"归档格式与引擎"的测试夹具。产品语义基准是 Modern GUI，产品 CLI 与它
// 保持一致；这个工具只负责让格式回归继续测得到真实代码。
//
// 退出码与备份引擎的约定一致：0 成功、1 操作失败、2 用法错误。

#include <iostream>
#include <string>
#include <vector>

#include "archive_pipeline.h"
#include "backup_engine.h"
#include "backup_option_keys.h"
#include "filter.h"
#include "terminal_secret.h"

namespace bp = backupproject;

namespace {

constexpr int kExitSuccess = 0;
constexpr int kExitOperationFailed = 1;
constexpr int kExitUsageError = 2;

void PrintUsage(std::ostream& output) {
  output << "Usage (test-only archive fixture tool):\n"
         << "  archive-cli backup <source_directory> <backup_file> "
            "[--include <rule>]... [--exclude <rule>]...\n"
         << "  archive-cli restore <backup_file> <destination_directory>\n"
         << "\n"
         << "Pipeline options (any of them switches the command to the v2 "
            "container;\n"
         << "without them the legacy v0.1 archive format is used):\n"
         << "  --pack mypack|ustar|fast-ustar\n"
         << "  --compression none|huffman|lzss-huffman\n"
         << "  --encryption none|aes-256-ctr-hmac-sha256|des-cbc-hmac-sha256\n"
         << "\n"
         << "Passwords are read interactively from /dev/tty, never from an "
            "argument.\n";
}

int UsageError(const std::string& text) {
  std::cerr << "Error: " << text << "\n\n";
  PrintUsage(std::cerr);
  return kExitUsageError;
}

bool TakeValue(const std::vector<std::string>& arguments, std::size_t* index,
               const std::string& option, std::string* value,
               std::string* error_message) {
  if (*index + 1 >= arguments.size()) {
    *error_message = option + " needs a value";
    return false;
  }
  *index += 1;
  *value = arguments[*index];
  return true;
}

int RunBackup(const std::vector<std::string>& arguments) {
  if (arguments.size() < 2) {
    return UsageError("'backup' expects <source_directory> and <backup_file>.");
  }
  const std::string source_directory = arguments[0];
  const std::string backup_file = arguments[1];

  bp::Filter filter;
  bp::BackupOptions options;
  bool has_pipeline_option = false;
  bool encryption_requested = false;
  std::string error_message;

  for (std::size_t index = 2; index < arguments.size(); ++index) {
    const std::string option = arguments[index];
    std::string value;
    if (option == "--include" || option == "--exclude") {
      if (!TakeValue(arguments, &index, option, &value, &error_message)) {
        return UsageError(error_message);
      }
      const bp::FilterAction action = option == "--include"
                                          ? bp::FilterAction::kInclude
                                          : bp::FilterAction::kExclude;
      if (!filter.AddRule(action, value, &error_message)) {
        return UsageError(error_message);
      }
      continue;
    }
    if (option == "--pack") {
      if (!TakeValue(arguments, &index, option, &value, &error_message)) {
        return UsageError(error_message);
      }
      if (!bp::ParsePackMethodKey(value, &options.pack_method)) {
        return UsageError("unknown pack method '" + value + "'");
      }
      has_pipeline_option = true;
      continue;
    }
    if (option == "--compression") {
      if (!TakeValue(arguments, &index, option, &value, &error_message)) {
        return UsageError(error_message);
      }
      if (!bp::ParseCompressionMethodKey(value, &options.compression_method)) {
        return UsageError("unknown compression method '" + value + "'");
      }
      has_pipeline_option = true;
      continue;
    }
    if (option == "--encryption") {
      if (!TakeValue(arguments, &index, option, &value, &error_message)) {
        return UsageError(error_message);
      }
      if (!bp::ParseEncryptionMethodKey(value, &options.encryption_method)) {
        return UsageError("unknown encryption method '" + value + "'");
      }
      has_pipeline_option = true;
      encryption_requested =
          options.encryption_method != bp::EncryptionMethod::kNone;
      continue;
    }
    return UsageError("unknown option '" + option + "'");
  }

  if (encryption_requested) {
    std::string secret;
    if (!bp::ReadSecretFromTerminalTwice("Backup password: ",
                                         "Confirm password: ", &secret,
                                         &error_message)) {
      std::cerr << "Error: " << error_message << "\n";
      return kExitOperationFailed;
    }
    options.password = secret;
    for (char& character : secret) character = '\0';
  }

  bp::BackupEngine engine;
  const bool ok = has_pipeline_option
                      ? engine.Backup(source_directory, backup_file, filter,
                                      options, &error_message)
                      : engine.Backup(source_directory, backup_file, filter,
                                      &error_message);
  if (!ok) {
    std::cerr << "Error: " << error_message << "\n";
    return kExitOperationFailed;
  }
  std::cout << "Backup completed successfully.\n";
  // 与旧 backupctl 的 direct-path 成功输出逐字一致：B.03/B.06 这类断言
  // grep 的就是这两行（legacy 报 v0.1，pipeline 报真实算法）。
  if (has_pipeline_option) {
    std::cout << "Pipeline: pack=" << bp::PackMethodKey(options.pack_method)
              << " compression="
              << bp::CompressionMethodKey(options.compression_method)
              << " encryption="
              << bp::EncryptionMethodKey(options.encryption_method) << '\n';
  } else {
    std::cout
        << "Archive format: legacy v0.1 (no pipeline options were given)\n";
  }
  return kExitSuccess;
}

int RunRestore(const std::vector<std::string>& arguments) {
  if (arguments.size() != 2) {
    return UsageError(
        "'restore' expects <backup_file> and <destination_directory>.");
  }
  const std::string backup_file = arguments[0];
  const std::string destination_directory = arguments[1];

  bp::BackupEngine engine;
  std::string error_message;

  bp::ArchiveFileInfo info;
  std::string identify_error;
  if (bp::IdentifyArchiveFile(backup_file, &info, &identify_error) &&
      !info.password_hint.empty()) {
    std::string secret;
    if (!bp::ReadSecretFromTerminal("Restore password: ", &secret,
                                    &error_message)) {
      std::cerr << "Error: " << error_message << "\n";
      return kExitOperationFailed;
    }
    bp::RestoreOptions options;
    options.password = secret;
    for (char& character : secret) character = '\0';
    if (!engine.Restore(backup_file, destination_directory, options, nullptr,
                        &error_message)) {
      std::cerr << "Error: " << error_message << "\n";
      return kExitOperationFailed;
    }
  } else if (!engine.Restore(backup_file, destination_directory,
                             &error_message)) {
    std::cerr << "Error: " << error_message << "\n";
    return kExitOperationFailed;
  }
  std::cout << "Restore completed successfully.\n";
  return kExitSuccess;
}

}  // namespace

int main(int argc, char* argv[]) {
  std::vector<std::string> arguments;
  for (int index = 1; index < argc; ++index) arguments.push_back(argv[index]);

  if (arguments.size() == 1 &&
      (arguments[0] == "--help" || arguments[0] == "-h")) {
    PrintUsage(std::cout);
    return kExitSuccess;
  }
  if (arguments.empty()) {
    PrintUsage(std::cerr);
    return kExitUsageError;
  }

  const std::string command = arguments[0];
  const std::vector<std::string> rest(arguments.begin() + 1, arguments.end());
  if (command == "backup") return RunBackup(rest);
  if (command == "restore") return RunRestore(rest);

  std::cerr << "Error: unknown command '" << command << "'.\n\n";
  PrintUsage(std::cerr);
  return kExitUsageError;
}
