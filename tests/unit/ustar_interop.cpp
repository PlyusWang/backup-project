// ustar_interop.cpp
//
// 只在测试里使用的命令行小工具：让 shell 脚本能直接驱动"我们写 USTAR / 我们读
// USTAR"，从而把 GNU tar 当互操作 oracle。
//
// 它把全部实际工作都交给产品代码（ScanSourceTree / ustar::WriteBaseline /
// ustar::WriteFast / ustar::Scan / RunRestorePackedStream），自己只做参数解析
// 和打印。产品二进制里没有 tar，也没有这个工具。
//
// 子命令：
//   write-baseline <source-dir> <out.tar>
//   write-fast     <source-dir> <out.tar>
//   scan           <archive>           按行打印每条 entry 的类型/权限/属主/大小/路径
//   restore        <archive> <dest>

#include <sys/stat.h>
#include <sys/types.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "archive_pipeline.h"
#include "test_support.h"
#include "tree_scanner.h"
#include "ustar.h"

namespace {

using backupproject::ArchiveEntry;
using backupproject::EntryType;
using backupproject::PackMethod;
using backupproject::RestoreReport;

const char* TypeName(EntryType type) {
  switch (type) {
    case EntryType::kDirectory:
      return "dir";
    case EntryType::kRegularFile:
      return "file";
    case EntryType::kSymlink:
      return "symlink";
    case EntryType::kHardLink:
      return "hardlink";
    case EntryType::kFifo:
      return "fifo";
    case EntryType::kCharDevice:
      return "char";
    case EntryType::kBlockDevice:
      return "block";
    case EntryType::kSocket:
      return "socket";
  }
  return "unknown";
}

int WriteArchive(const std::string& source, const std::string& output, bool fast) {
  std::string error;
  std::vector<ArchiveEntry> entries;
  if (!backupproject::ScanSourceTree(source, nullptr, &entries, &error)) {
    std::fprintf(stderr, "scan failed: %s\n", error.c_str());
    return 1;
  }
  const bool ok = fast ? backupproject::ustar::WriteFast(entries, output, &error)
                       : backupproject::ustar::WriteBaseline(entries, output,
                                                             &error);
  if (!ok) {
    std::fprintf(stderr, "write failed: %s\n", error.c_str());
    return 1;
  }
  std::printf("WROTE\t%s\t%zu entries\n", output.c_str(), entries.size());
  return 0;
}

int ScanArchive(const std::string& archive) {
  std::string error;
  std::vector<backupproject::ustar::Member> members;
  if (!backupproject::ustar::Scan(archive, &members, &error)) {
    std::fprintf(stderr, "scan failed: %s\n", error.c_str());
    return 1;
  }
  for (const auto& member : members) {
    const ArchiveEntry& entry = member.entry;
    std::printf("ENTRY\t%s\t%o\t%u\t%u\t%lld\t%llu\t%s\t%s\t%s\t%s\n",
                TypeName(entry.type), entry.mode, entry.uid, entry.gid,
                static_cast<long long>(entry.mtime_sec),
                static_cast<unsigned long long>(entry.size),
                entry.user_name.c_str(), entry.group_name.c_str(),
                entry.archive_path.c_str(), entry.link_target.c_str());
  }
  std::printf("SCANNED\t%s\t%zu entries\n", archive.c_str(), members.size());
  return 0;
}

int RestoreArchive(const std::string& archive, const std::string& destination) {
  std::string error;
  std::vector<backupproject::ustar::Member> members;
  if (!backupproject::ustar::Scan(archive, &members, &error)) {
    std::fprintf(stderr, "scan failed: %s\n", error.c_str());
    return 1;
  }
  RestoreReport report;
  // 走产品代码那条恢复路径：preflight → 暂存目录 → metadata 收尾 → rename。
  const bool ok = backupproject::RunRestorePackedStream(
      archive, PackMethod::kUstar, members.size(), destination, &report, &error);
  if (!ok) {
    std::fprintf(stderr, "restore failed: %s\n", error.c_str());
    return 1;
  }
  std::printf("RESTORED\t%s\t%llu entries\t%llu ownership notes\n",
              destination.c_str(),
              static_cast<unsigned long long>(report.restored_entries),
              static_cast<unsigned long long>(report.skipped_ownership));
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: ustar_interop <write-baseline|write-fast|scan|restore> "
                 "...\n");
    return 2;
  }
  const std::string command = argv[1];
  if (command == "write-baseline" && argc == 4) {
    return WriteArchive(argv[2], argv[3], /*fast=*/false);
  }
  if (command == "write-fast" && argc == 4) {
    return WriteArchive(argv[2], argv[3], /*fast=*/true);
  }
  if (command == "scan" && argc == 3) {
    return ScanArchive(argv[2]);
  }
  if (command == "restore" && argc == 4) {
    return RestoreArchive(argv[2], argv[3]);
  }
  std::fprintf(stderr, "bad arguments for: %s\n", command.c_str());
  return 2;
}
