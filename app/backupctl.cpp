// backupctl.cpp
//
// Command line interface for the backup engine (v0.1).
//
// Usage:
//   backupctl backup <source_directory> <repository>
//   backupctl restore <repository> <destination>
//
// Exit codes:
//   0  success
//   1  operation failed (bad path, unsupported file type, I/O error)
//   2  command line usage error

#include <iostream>
#include <string>

#include "backup_engine.h"

namespace {

constexpr int kExitSuccess = 0;
constexpr int kExitOperationFailed = 1;
constexpr int kExitUsageError = 2;

// Prints the usage text to |output| (stdout for --help, stderr for usage
// errors).
void PrintUsage(const std::string& program_name, std::ostream& output) {
  output << "Usage:\n"
         << "  " << program_name << " backup <source_directory> <repository>\n"
         << "  " << program_name << " restore <repository> <destination>\n"
         << "\n"
         << "Options:\n"
         << "  --help, -h  Show this help message.\n";
}

// Runs the backup command and prints the outcome.
int RunBackup(const std::string& source_directory,
              const std::string& repository) {
  backupproject::BackupEngine engine;
  std::string error_message;
  if (!engine.Backup(source_directory, repository, &error_message)) {
    std::cerr << "Error: " << error_message << '\n';
    return kExitOperationFailed;
  }
  std::cout << "Backup completed successfully.\n";
  return kExitSuccess;
}

// Runs the restore command and prints the outcome.
int RunRestore(const std::string& repository, const std::string& destination) {
  backupproject::BackupEngine engine;
  std::string error_message;
  if (!engine.Restore(repository, destination, &error_message)) {
    std::cerr << "Error: " << error_message << '\n';
    return kExitOperationFailed;
  }
  std::cout << "Restore completed successfully.\n";
  return kExitSuccess;
}

}  // namespace

int main(int argc, char* argv[]) {
  const std::string program_name = (argc > 0) ? argv[0] : "backupctl";

  if (argc == 2 &&
      (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
    PrintUsage(program_name, std::cout);
    return kExitSuccess;
  }

  if (argc < 2) {
    PrintUsage(program_name, std::cerr);
    return kExitUsageError;
  }

  const std::string command = argv[1];
  if (command == "backup") {
    if (argc != 4) {
      std::cerr << "Error: 'backup' expects <source_directory> and "
                   "<repository>.\n\n";
      PrintUsage(program_name, std::cerr);
      return kExitUsageError;
    }
    return RunBackup(argv[2], argv[3]);
  }

  if (command == "restore") {
    if (argc != 4) {
      std::cerr << "Error: 'restore' expects <repository> and "
                   "<destination>.\n\n";
      PrintUsage(program_name, std::cerr);
      return kExitUsageError;
    }
    return RunRestore(argv[2], argv[3]);
  }

  std::cerr << "Error: unknown command '" << command << "'.\n\n";
  PrintUsage(program_name, std::cerr);
  return kExitUsageError;
}
