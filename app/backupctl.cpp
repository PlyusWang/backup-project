#include <iostream>
#include <string>

namespace {

void PrintUsage(const char* program_name) {
  std::cout << "Usage:\n"
            << "  " << program_name
            << " backup <source_directory> <repository>\n"
            << "  " << program_name
            << " restore <repository> <destination>\n";
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc == 2 && std::string(argv[1]) == "--help") {
    PrintUsage(argv[0]);
    return 0;
  }

  if (argc < 2) {
    PrintUsage(argv[0]);
    return 1;
  }

  std::cerr << "Backup/Restore is not implemented yet.\n";
  return 1;
}