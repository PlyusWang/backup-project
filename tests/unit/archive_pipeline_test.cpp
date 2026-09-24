// archive_pipeline_test.cpp
//
// 归档流水线的专项测试：27 组合矩阵、metadata、特殊文件、socket 语义、
// Filter 与扫描层的衔接。
//
// 全部结论都来自真实执行：backup 出来的 .bak 会被真的 restore 到一个临时目录，
// 再和源树逐项比较（类型 / mode / mtime 秒+纳秒 / 属主 / 内容 / 软链接目标 /
// 设备号 / 硬链接分组）。

#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "archive_pipeline.h"
#include "backup_engine.h"
#include "filter.h"
#include "pack_stream.h"
#include "test_support.h"
#include "tree_scanner.h"

namespace {

using backupproject::ArchiveEntry;
using backupproject::BackupEngine;
using backupproject::BackupOptions;
using backupproject::CompressionMethod;
using backupproject::EncryptionMethod;
using backupproject::EntryType;
using backupproject::Filter;
using backupproject::FilterAction;
using backupproject::PackMethod;
using backupproject::PackedStreamReader;
using backupproject::RestoreOptions;
using backupproject::RestoreReport;

const char* PackLabel(PackMethod method) {
  switch (method) {
    case PackMethod::kMyPack:
      return "MyPack";
    case PackMethod::kUstar:
      return "USTAR";
    case PackMethod::kFastUstar:
      return "FastUSTAR";
  }
  return "?";
}

const char* CompressionLabel(CompressionMethod method) {
  switch (method) {
    case CompressionMethod::kNone:
      return "None";
    case CompressionMethod::kHuffman:
      return "Huffman";
    case CompressionMethod::kLzssHuffman:
      return "LZSS-Huffman";
  }
  return "?";
}

const char* EncryptionLabel(EncryptionMethod method) {
  switch (method) {
    case EncryptionMethod::kNone:
      return "None";
    case EncryptionMethod::kDesCbcHmacSha256:
      return "DES-CBC";
    case EncryptionMethod::kAes256CtrHmacSha256:
      return "AES-256-CTR";
  }
  return "?";
}

const char* Password() { return "correct horse battery staple"; }

// 一棵"普通目录树"：矩阵的 27 个组合都用它。
void BuildRegularTree(const std::string& root) {
  test_support::Mkdir(root, 0755);
  test_support::WriteFile(root + "/hello.txt", "hello world\n", 0644);
  test_support::WriteFile(root + "/empty.bin", "", 0644);
  test_support::WriteFile(root + "/with space.txt", "spaces matter\n", 0600);
  test_support::WriteFile(root + "/\u4e2d\u6587.txt", "unicode payload\n", 0640);
  test_support::Mkdir(root + "/sub", 0750);
  test_support::WriteFile(root + "/sub/nested.txt", std::string(4096, 'n'), 0644);
  test_support::Mkdir(root + "/sub/deep", 0700);
  test_support::WriteFile(root + "/sub/deep/script.sh", "#!/bin/sh\necho hi\n",
                          0755);
  // USTAR 的 mtime 只有秒级精度，所以矩阵 fixture 统一用整秒；
  // 纳秒精度由 MyPack v2 的 metadata 专项测试单独验证。
  test_support::NormalizeTimes(root, 1700000000);
}

// 一条目录项的类型 / mode / mtime 是否与预期一致。
void CheckNode(const std::string& label, const std::string& path,
               mode_t expected_type, std::uint32_t expected_mode,
               std::int64_t mtime_sec, std::uint32_t mtime_nsec) {
  struct stat info;
  if (!test_support::StatOf(path, &info)) {
    test_support::Check(false, label, "missing: " + path);
    return;
  }
  const bool type_ok = (info.st_mode & S_IFMT) == expected_type;
  const bool mode_ok = (info.st_mode & 07777) == expected_mode;
  // mtime_sec < 0 表示"这条不检查时间"，用于那些没被显式设过时间的节点。
  const bool time_ok =
      mtime_sec < 0 ||
      (info.st_mtim.tv_sec == mtime_sec &&
       static_cast<std::uint32_t>(info.st_mtim.tv_nsec) == mtime_nsec);
  std::string detail;
  if (!type_ok) {
    detail += "entry type mismatch; ";
  }
  if (!mode_ok) {
    detail += "mode " + test_support::Octal(info.st_mode & 07777) + " != " +
              test_support::Octal(expected_mode) + "; ";
  }
  if (!time_ok) {
    detail += "mtime " + std::to_string(info.st_mtim.tv_sec) + "." +
              std::to_string(info.st_mtim.tv_nsec) + " != " +
              std::to_string(mtime_sec) + "." + std::to_string(mtime_nsec);
  }
  test_support::Check(type_ok && mode_ok && time_ok, label, detail);
}

struct MatrixRow {
  std::string pack;
  std::string compression;
  std::string encryption;
  std::string backup;
  std::string restore;
  std::string diff;
  std::string result;
  std::string wrong_password;
};

// 27 组合矩阵：每个组合都真的 backup → restore → 比较。
void RunMatrix(const std::string& workdir, std::vector<MatrixRow>* rows) {
  const PackMethod packs[3] = {PackMethod::kMyPack, PackMethod::kUstar,
                               PackMethod::kFastUstar};
  const CompressionMethod compressions[3] = {CompressionMethod::kNone,
                                             CompressionMethod::kHuffman,
                                             CompressionMethod::kLzssHuffman};
  const EncryptionMethod encryptions[3] = {
      EncryptionMethod::kNone, EncryptionMethod::kDesCbcHmacSha256,
      EncryptionMethod::kAes256CtrHmacSha256};

  const std::string source = workdir + "/source";
  BuildRegularTree(source);

  for (const PackMethod pack : packs) {
    for (const CompressionMethod compression : compressions) {
      for (const EncryptionMethod encryption : encryptions) {
        MatrixRow row;
        row.pack = PackLabel(pack);
        row.compression = CompressionLabel(compression);
        row.encryption = EncryptionLabel(encryption);
        row.backup = "?";
        row.restore = "?";
        row.diff = "?";
        row.wrong_password = "n/a";
        row.result = "FAIL";

        const std::string tag = std::string(PackLabel(pack)) + "+" +
                                CompressionLabel(compression) + "+" +
                                EncryptionLabel(encryption);
        const std::string archive = workdir + "/matrix-" + tag + ".bak";
        const std::string destination = workdir + "/matrix-out-" + tag;

        BackupOptions options;
        options.pack_method = pack;
        options.compression_method = compression;
        options.encryption_method = encryption;
        if (encryption != EncryptionMethod::kNone) {
          options.password = Password();
        }
        const Filter no_filter;
        BackupEngine engine;
        std::string error;
        if (engine.Backup(source, archive, no_filter, options, &error)) {
          row.backup = "PASS";
        } else {
          row.backup = "FAIL";
          test_support::Check(false, "matrix backup " + tag, error);
          rows->push_back(row);
          continue;
        }

        RestoreOptions restore_options;
        restore_options.password = options.password;
        RestoreReport report;
        error.clear();
        if (engine.Restore(archive, destination, restore_options, &report,
                           &error)) {
          row.restore = "PASS";
        } else {
          row.restore = "FAIL";
          test_support::Check(false, "matrix restore " + tag, error);
          rows->push_back(row);
          continue;
        }

        std::string detail;
        if (test_support::CompareTrees(source, destination, &detail)) {
          row.diff = "PASS";
        } else {
          row.diff = "FAIL";
          test_support::Check(false, "matrix tree comparison " + tag, detail);
          rows->push_back(row);
          continue;
        }
        if (report.restored_entries == 0) {
          test_support::Check(false, "matrix entry count " + tag,
                              "report says zero entries were restored");
        }

        // 加密组合必须验证 wrong password：既要失败，也不能留下半成品。
        if (encryption != EncryptionMethod::kNone) {
          const std::string bad_destination = workdir + "/matrix-bad-" + tag;
          RestoreOptions bad_options;
          bad_options.password = "definitely not the password";
          error.clear();
          RestoreReport bad_report;
          const bool wrong_ok = engine.Restore(archive, bad_destination,
                                               bad_options, &bad_report, &error);
          const bool failed = !wrong_ok;
          const bool no_leftover = !test_support::Exists(bad_destination);
          const bool auth_message =
              error.find("Authentication failed") != std::string::npos;
          row.wrong_password =
              (failed && no_leftover && auth_message) ? "PASS" : "FAIL";
          test_support::Check(failed,
                              "matrix wrong password rejected " + tag, error);
          test_support::Check(no_leftover,
                              "matrix wrong password leaves no destination " +
                                  tag);
          test_support::Check(auth_message,
                              "matrix wrong password fails on HMAC " + tag,
                              error);
        }

        row.result = "PASS";
        rows->push_back(row);
        test_support::Check(true, "matrix combination " + tag);
      }
    }
  }
}

void PrintMatrix(const std::vector<MatrixRow>& rows) {
  std::printf("\nMATRIX\tPack\tCompression\tEncryption\tBackup\tRestore\tDiff\tWrongPwd\tResult\n");
  for (const MatrixRow& row : rows) {
    std::printf("MATRIX\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n",
                row.pack.c_str(), row.compression.c_str(),
                row.encryption.c_str(), row.backup.c_str(),
                row.restore.c_str(), row.diff.c_str(),
                row.wrong_password.c_str(), row.result.c_str());
  }
  std::fflush(stdout);
}

// ---- metadata 07777 / uid / gid / mtime ------------------------------------

void RunMetadata(const std::string& workdir) {
  test_support::Section("metadata: 07777 / uid / gid / mtime sec+nsec");
  const std::string source = workdir + "/meta-source";
  test_support::Mkdir(source, 0755);
  test_support::WriteFile(source + "/setuid.bin", "setuid\n", 0644);
  test_support::WriteFile(source + "/setgid.bin", "setgid\n", 0644);
  test_support::Mkdir(source + "/sticky", 0755);
  std::string error;
  if (!test_support::SetTimes(source + "/setuid.bin", 1000000000, 999999999)) {
    test_support::Note("could not set nanosecond timestamps");
  }
  (void)::chmod((source + "/setuid.bin").c_str(), 04755);
  (void)::chmod((source + "/setgid.bin").c_str(), 02755);
  (void)::chmod((source + "/sticky").c_str(), 01777);
  (void)::chmod(source.c_str(), 0751);

  const std::string archive = workdir + "/meta.bak";
  const std::string destination = workdir + "/meta-out";
  BackupOptions options;
  options.pack_method = PackMethod::kMyPack;
  BackupEngine engine;
  const bool backed_up =
      engine.Backup(source, archive, Filter(), options, &error);
  test_support::Check(backed_up, "metadata backup", error);
  if (!backed_up) {
    return;
  }
  RestoreOptions restore_options;
  RestoreReport report;
  const bool restored = engine.Restore(archive, destination, restore_options,
                                       &report, &error);
  test_support::Check(restored, "metadata restore", error);
  if (!restored) {
    return;
  }
  CheckNode("setuid bit survives", destination + "/setuid.bin", S_IFREG, 04755,
            1000000000, 999999999);
  CheckNode("setgid bit survives", destination + "/setgid.bin", S_IFREG, 02755,
            -1, 0);
  CheckNode("sticky bit survives", destination + "/sticky", S_IFDIR, 01777, -1, 0);
  CheckNode("source root mode survives", destination, S_IFDIR, 0751, -1, 0);

  struct stat source_info;
  struct stat restored_info;
  if (test_support::StatOf(source + "/setuid.bin", &source_info) &&
      test_support::StatOf(destination + "/setuid.bin", &restored_info)) {
    test_support::Check(source_info.st_uid == restored_info.st_uid &&
                            source_info.st_gid == restored_info.st_gid,
                        "uid/gid survive the round trip",
                        std::to_string(source_info.st_uid) + ":" +
                            std::to_string(source_info.st_gid) + " vs " +
                            std::to_string(restored_info.st_uid) + ":" +
                            std::to_string(restored_info.st_gid));
  }
  test_support::Check(report.skipped_ownership == 0,
                      "ownership restored exactly for the current user");
  test_support::Note("report: restored=" +
                     std::to_string(report.restored_entries) + " skipped_owner=" +
                     std::to_string(report.skipped_ownership));
}

// ---- 特殊文件：真实文件系统上的 symlink / hardlink / FIFO -------------------

void RunSpecialFiles(const std::string& workdir, PackMethod pack) {
  const std::string tag = PackLabel(pack);
  test_support::Section(std::string("special files (") + tag + ")");
  const std::string source = workdir + "/special-" + tag;
  test_support::Mkdir(source, 0755);
  test_support::WriteFile(source + "/data.txt", "hard link payload\n", 0644);
  test_support::Mkdir(source + "/dir", 0755);
  test_support::WriteFile(source + "/dir/inner.txt", "inner\n", 0600);

  const bool made_link =
      test_support::CreateSymlink("../data.txt", source + "/dir/up.link");
  const bool made_hard =
      test_support::CreateHardlink(source + "/data.txt", source + "/hard.txt");
  const bool made_fifo = test_support::CreateFifo(source + "/pipe", 0644);
  test_support::Check(made_link, "fixture symbolic link created");
  test_support::Check(made_hard, "fixture hard link created");
  test_support::Check(made_fifo, "fixture FIFO created");
  if (!made_link || !made_hard || !made_fifo) {
    return;
  }
  (void)::chmod((source + "/pipe").c_str(), 0620);
  test_support::NormalizeTimes(source, 1650000000);

  const std::string archive = workdir + "/special-" + tag + ".bak";
  const std::string destination = workdir + "/special-out-" + tag;
  BackupOptions options;
  options.pack_method = pack;
  BackupEngine engine;
  std::string error;
  const bool backed_up =
      engine.Backup(source, archive, Filter(), options, &error);
  test_support::Check(backed_up, std::string(tag) + " special backup", error);
  if (!backed_up) {
    return;
  }
  RestoreOptions restore_options;
  RestoreReport report;
  const bool restored = engine.Restore(archive, destination, restore_options,
                                       &report, &error);
  test_support::Check(restored, std::string(tag) + " special restore", error);
  if (!restored) {
    return;
  }
  std::string detail;
  test_support::Check(test_support::CompareTrees(source, destination, &detail),
                      std::string(tag) + " special tree comparison", detail);

  struct stat first;
  struct stat second;
  const bool have_first =
      test_support::StatOf(destination + "/data.txt", &first);
  const bool have_second =
      test_support::StatOf(destination + "/hard.txt", &second);
  test_support::Check(have_first && have_second &&
                          first.st_ino == second.st_ino &&
                          first.st_nlink >= 2,
                      std::string(tag) +
                          " hard link keeps a shared inode (st_ino equal)");
  struct stat fifo_info;
  test_support::Check(test_support::StatOf(destination + "/pipe", &fifo_info) &&
                          S_ISFIFO(fifo_info.st_mode),
                      std::string(tag) + " FIFO restored as FIFO");
  struct stat link_info;
  test_support::Check(
      test_support::StatOf(destination + "/dir/up.link", &link_info) &&
          S_ISLNK(link_info.st_mode),
      std::string(tag) + " symbolic link restored as symlink");
  char target[4096];
  const ssize_t length =
      ::readlink((destination + "/dir/up.link").c_str(), target, sizeof(target));
  test_support::Check(length == 11 && std::string(target, 11) == "../data.txt",
                      std::string(tag) + " symbolic link target preserved");
}

// ---- socket 语义 -----------------------------------------------------------

void RunSocketSemantics(const std::string& workdir) {
  test_support::Section("socket: excluded is fine, included must fail");
  const std::string ok_source = workdir + "/socket-excluded";
  test_support::Mkdir(ok_source, 0755);
  test_support::WriteFile(ok_source + "/normal.txt", "normal\n", 0644);
  test_support::Mkdir(ok_source + "/excluded", 0755);
  const int socket_fd =
      test_support::CreateUnixSocket(ok_source + "/excluded/sock");
  test_support::Check(socket_fd >= 0, "fixture unix socket created");

  std::string error;
  PackedStreamReader probe;
  std::vector<ArchiveEntry> entries;
  Filter prune;
  const bool rule_ok =
      prune.AddRule(FilterAction::kExclude, "type:folder name:excluded", &error);
  test_support::Check(rule_ok, "prune rule parsed", error);
  test_support::Check(
      backupproject::ScanSourceTree(ok_source, &prune, &entries, &error),
      "directory prune hides the socket", error);
  bool saw_socket = false;
  for (const ArchiveEntry& entry : entries) {
    if (entry.type == EntryType::kSocket) {
      saw_socket = true;
    }
  }
  test_support::Check(!saw_socket, "pruned subtree contributed no socket entry");

  const std::string ok_archive = workdir + "/socket-excluded.bak";
  const std::string ok_destination = workdir + "/socket-excluded-out";
  BackupOptions options;
  BackupEngine engine;
  const bool backed_up =
      engine.Backup(ok_source, ok_archive, prune, options, &error);
  test_support::Check(backed_up, "backup succeeds when the socket is excluded",
                      error);
  if (backed_up) {
    RestoreOptions restore_options;
    RestoreReport report;
    const bool restored = engine.Restore(ok_archive, ok_destination,
                                         restore_options, &report, &error);
    test_support::Check(restored, "restore after excluded socket", error);
  }

  // 同一个源目录、没有规则：socket 会进入归档，整次备份必须明确失败。
  const std::string fail_archive = workdir + "/socket-included.bak";
  error.clear();
  const bool failed_backup =
      engine.Backup(ok_source, fail_archive, Filter(), options, &error);
  test_support::Check(!failed_backup,
                      "backup fails when a socket would enter the archive");
  test_support::Check(error.find("socket") != std::string::npos,
                      "socket failure message names the type", error);
  test_support::Check(!test_support::Exists(fail_archive),
                      "failed socket backup leaves no archive file");

  if (socket_fd >= 0) {
    ::close(socket_fd);
  }
}

// ---- 直接喂条目表：设备号 / 硬链接 / 未知类型 -------------------------------

std::vector<ArchiveEntry> MakeSpecialEntries(const std::string& payload_path) {
  std::vector<ArchiveEntry> entries;
  ArchiveEntry root;
  root.archive_path = ".";
  root.source_path = payload_path;
  root.type = EntryType::kDirectory;
  root.mode = 0755;
  root.uid = 1000;
  root.gid = 2000;
  root.mtime_sec = 1500000000;
  root.mtime_nsec = 424242;
  entries.push_back(root);

  ArchiveEntry file;
  file.archive_path = "payload.bin";
  file.source_path = payload_path;
  file.type = EntryType::kRegularFile;
  file.mode = 0640;
  file.uid = 1234;
  file.gid = 5678;
  file.mtime_sec = 1400000000;
  file.mtime_nsec = 7;
  struct stat info;
  if (::lstat(payload_path.c_str(), &info) == 0) {
    file.size = static_cast<std::uint64_t>(info.st_size);
    file.mtime_sec = static_cast<std::int64_t>(info.st_mtim.tv_sec);
    file.mtime_nsec = static_cast<std::uint32_t>(info.st_mtim.tv_nsec);
  }
  entries.push_back(file);

  ArchiveEntry hard;
  hard.archive_path = "payload.hard";
  hard.type = EntryType::kHardLink;
  hard.link_target = "payload.bin";
  hard.mode = 0640;
  hard.uid = 1234;
  hard.gid = 5678;
  hard.mtime_sec = 1400000000;
  hard.mtime_nsec = 7;
  entries.push_back(hard);

  ArchiveEntry link;
  link.archive_path = "target.link";
  link.type = EntryType::kSymlink;
  link.link_target = "../outside/wherever";
  link.mode = 0777;
  link.uid = 4242;
  link.gid = 2424;
  link.mtime_sec = 1300000000;
  link.mtime_nsec = 999999999;
  entries.push_back(link);

  ArchiveEntry fifo;
  fifo.archive_path = "pipe";
  fifo.type = EntryType::kFifo;
  fifo.mode = 0620;
  fifo.uid = 3333;
  fifo.gid = 4444;
  fifo.mtime_sec = 1200000000;
  fifo.mtime_nsec = 5;
  entries.push_back(fifo);

  ArchiveEntry character;
  character.archive_path = "null";
  character.type = EntryType::kCharDevice;
  character.mode = 0666;
  character.uid = 0;
  character.gid = 0;
  character.mtime_sec = 1100000000;
  character.mtime_nsec = 11;
  character.dev_major = 1;
  character.dev_minor = 3;
  entries.push_back(character);

  ArchiveEntry block;
  block.archive_path = "disk";
  block.type = EntryType::kBlockDevice;
  block.mode = 0660;
  block.uid = 0;
  block.gid = 6;
  block.mtime_sec = 1000000000;
  block.mtime_nsec = 13;
  block.dev_major = 8;
  block.dev_minor = 17;
  entries.push_back(block);
  return entries;
}

void RunDeviceAndFormat(const std::string& workdir) {
  test_support::Section("device numbers / links through the entry model");
  const std::string payload = workdir + "/payload.bin";
  test_support::WriteFile(payload, "device test payload\n", 0644);
  const std::vector<ArchiveEntry> entries = MakeSpecialEntries(payload);

  const PackMethod packs[3] = {PackMethod::kMyPack, PackMethod::kUstar,
                               PackMethod::kFastUstar};
  for (const PackMethod pack : packs) {
    const std::string tag = PackLabel(pack);
    const std::string packed = workdir + "/entries-" + tag + ".pack";
    test_support::RemoveTree(packed);
    std::string error;
    const bool packed_ok =
        backupproject::PackEntries(pack, entries, packed, &error);
    test_support::Check(packed_ok, std::string(tag) + " pack entries", error);
    if (!packed_ok) {
      continue;
    }
    PackedStreamReader reader;
    const bool opened = reader.Open(packed, &error);
    test_support::Check(opened, std::string(tag) + " open packed stream", error);
    if (!opened) {
      continue;
    }
    const bool scanned = reader.Scan(pack, &error);
    test_support::Check(scanned, std::string(tag) + " preflight packed stream",
                        error);
    if (!scanned) {
      continue;
    }
    test_support::Check(reader.entries().size() == entries.size(),
                        std::string(tag) + " entry count preserved");
    bool device_ok = true;
    bool link_ok = true;
    bool fifo_ok = true;
    bool hard_ok = true;
    std::string device_detail;
    for (const auto& record : reader.entries()) {
      const ArchiveEntry& entry = record.entry;
      if (entry.archive_path == "null" &&
          (entry.type != EntryType::kCharDevice || entry.dev_major != 1 ||
           entry.dev_minor != 3 || entry.mode != 0666)) {
        device_ok = false;
        device_detail += "char device metadata mismatch; ";
      }
      if (entry.archive_path == "disk" &&
          (entry.type != EntryType::kBlockDevice || entry.dev_major != 8 ||
           entry.dev_minor != 17 || entry.mode != 0660 || entry.gid != 6)) {
        device_ok = false;
        device_detail += "block device metadata mismatch; ";
      }
      if (entry.archive_path == "target.link" &&
          (entry.type != EntryType::kSymlink ||
           entry.link_target != "../outside/wherever")) {
        link_ok = false;
      }
      if (entry.archive_path == "pipe" &&
          (entry.type != EntryType::kFifo || entry.mode != 0620 ||
           entry.uid != 3333)) {
        fifo_ok = false;
      }
      if (entry.archive_path == "payload.hard" &&
          (entry.type != EntryType::kHardLink ||
           entry.link_target != "payload.bin")) {
        hard_ok = false;
      }
    }
    test_support::Check(device_ok, std::string(tag) + " device major/minor",
                        device_detail);
    test_support::Check(link_ok, std::string(tag) + " symlink target");
    test_support::Check(fifo_ok, std::string(tag) + " FIFO metadata");
    test_support::Check(hard_ok, std::string(tag) + " hard link target");

    // container 走一遍：非 root 时 mknod 一定会失败，但必须失败得干净利落。
    const std::string archive = workdir + "/entries-" + tag + ".bak";
    const std::string destination = workdir + "/entries-out-" + tag;
    BackupOptions options;
    options.pack_method = pack;
    const bool container_ok = backupproject::RunBackupPipelineFromEntries(
        entries, archive, options, &error);
    test_support::Check(container_ok,
                        std::string(tag) + " container from entries", error);
    if (!container_ok) {
      continue;
    }
    RestoreOptions restore_options;
    RestoreReport report;
    const bool restored = backupproject::RunRestorePipeline(
        archive, destination, restore_options, &report, &error);
    if (::geteuid() == 0) {
      test_support::Check(restored, std::string(tag) + " device restore", error);
      if (restored) {
        struct stat info;
        test_support::Check(
            test_support::StatOf(destination + "/null", &info) &&
                S_ISCHR(info.st_mode) && info.st_rdev == makedev(1, 3),
            std::string(tag) + " char device recreated with the same rdev");
        test_support::Check(
            test_support::StatOf(destination + "/disk", &info) &&
                S_ISBLK(info.st_mode) && info.st_rdev == makedev(8, 17),
            std::string(tag) + " block device recreated with the same rdev");
      }
    } else {
      test_support::Check(!restored,
                          std::string(tag) +
                              " device restore fails clearly without CAP_MKNOD",
                          error);
      test_support::Check(error.find("CAP_MKNOD") != std::string::npos,
                          std::string(tag) +
                              " failure message explains the privilege need",
                          error);
      test_support::Check(!test_support::Exists(destination),
                          std::string(tag) +
                              " failed device restore leaves no destination");
      test_support::Note(std::string(tag) +
                         ": SKIP restore char/block: insufficient privilege");
    }
  }

  // socket 不能被任何 pack 后端接受。
  std::vector<ArchiveEntry> with_socket = entries;
  ArchiveEntry socket_entry;
  socket_entry.archive_path = "sock";
  socket_entry.type = EntryType::kSocket;
  socket_entry.mode = 0777;
  with_socket.push_back(socket_entry);
  for (const PackMethod pack : packs) {
    const std::string archive = workdir + "/socket-" + PackLabel(pack) + ".bak";
    std::string error;
    BackupOptions options;
    options.pack_method = pack;
    const bool ok = backupproject::RunBackupPipelineFromEntries(
        with_socket, archive, options, &error);
    test_support::Check(!ok, std::string(PackLabel(pack)) +
                                 " rejects a socket entry");
    test_support::Check(!test_support::Exists(archive),
                        std::string(PackLabel(pack)) +
                            " leaves no archive after rejecting a socket");
  }
}

// ---- Filter 与扫描层的衔接 -------------------------------------------------

int32_t CurrentUid() { return static_cast<int32_t>(::geteuid()); }

void RunFilterScan(const std::string& workdir) {
  test_support::Section("filter semantics through the scanner");
  const std::string source = workdir + "/filter-source";
  test_support::Mkdir(source, 0755);
  test_support::WriteFile(source + "/keep.txt", "keep\n", 0644);
  test_support::WriteFile(source + "/drop.log", "drop\n", 0644);
  test_support::Mkdir(source + "/cache", 0755);
  test_support::WriteFile(source + "/cache/tmp.bin", "tmp\n", 0644);
  test_support::CreateFifo(source + "/pipe", 0644);
  test_support::CreateSymlink("keep.txt", source + "/link");

  std::string error;
  {
    Filter filter;
    const bool ok = filter.AddRule(FilterAction::kExclude, "ext:log", &error) &&
                    filter.AddRule(FilterAction::kExclude, "type:folder name:cache",
                                   &error) &&
                    filter.AddRule(FilterAction::kExclude, "type:fifo", &error) &&
                    filter.AddRule(FilterAction::kExclude, "type:symlink", &error);
    test_support::Check(ok, "filter rules parsed", error);
    std::vector<ArchiveEntry> entries;
    const bool scanned =
        backupproject::ScanSourceTree(source, &filter, &entries, &error);
    test_support::Check(scanned, "filtered scan succeeds", error);
    bool saw_log = false;
    bool saw_cache = false;
    bool saw_fifo = false;
    bool saw_link = false;
    for (const ArchiveEntry& entry : entries) {
      if (entry.archive_path == "drop.log") {
        saw_log = true;
      }
      if (entry.archive_path.rfind("cache", 0) == 0) {
        saw_cache = true;
      }
      if (entry.type == EntryType::kFifo) {
        saw_fifo = true;
      }
      if (entry.type == EntryType::kSymlink) {
        saw_link = true;
      }
    }
    test_support::Check(!saw_log, "ext:log excluded");
    test_support::Check(!saw_cache, "directory prune removed the subtree");
    test_support::Check(!saw_fifo, "type:fifo excluded");
    test_support::Check(!saw_link, "type:symlink excluded");
  }

  {
    // uid 规则必须真的按 uid 命中（当前进程的文件全都是自己的 uid）。
    Filter filter;
    const std::string rule = "uid:" + std::to_string(CurrentUid());
    const bool ok = filter.AddRule(FilterAction::kInclude, rule, &error);
    test_support::Check(ok, "uid rule parsed", error);
    std::vector<ArchiveEntry> entries;
    const bool scanned =
        backupproject::ScanSourceTree(source, &filter, &entries, &error);
    test_support::Check(scanned, "scan with a uid include rule", error);
    std::size_t regular = 0;
    for (const ArchiveEntry& entry : entries) {
      if (entry.type == EntryType::kRegularFile) {
        ++regular;
      }
    }
    test_support::Check(regular == 3,
                        "uid include rule kept all three regular files",
                        "kept " + std::to_string(regular));
  }

  {
    // 不存在的用户名的规则不能崩，也不能匹配任何人。
    Filter filter;
    const bool ok = filter.AddRule(FilterAction::kInclude,
                                   "user:no-such-user-xyz", &error);
    test_support::Check(ok, "user rule parsed", error);
    std::vector<ArchiveEntry> entries;
    const bool scanned =
        backupproject::ScanSourceTree(source, &filter, &entries, &error);
    test_support::Check(scanned, "scan with an unmatched user rule", error);
    std::size_t regular = 0;
    for (const ArchiveEntry& entry : entries) {
      if (entry.type == EntryType::kRegularFile) {
        ++regular;
      }
    }
    test_support::Check(regular == 0,
                        "unknown user matches nothing (and does not crash)");
  }
}

// ---- 三种 pack 后端的语义一致性 + 备份确定性 -------------------------------

// 同一个条目模型交给三种 pack 后端，解出来的字段必须逐项一致：
// 只要有一个后端悄悄少存或多存了什么，"换个 pack 方法备份"就会得到不同的东西。
void RunPackAgreement(const std::string& workdir) {
  test_support::Section("three pack backends agree on the entry model");
  const std::string source = workdir + "/agree-source";
  BuildRegularTree(source);
  test_support::CreateSymlink("hello.txt", source + "/link");
  test_support::CreateFifo(source + "/pipe", 0644);
  test_support::CreateHardlink(source + "/hello.txt", source + "/hard.txt");
  test_support::NormalizeTimes(source, 1700000000);

  std::string error;
  std::vector<ArchiveEntry> entries;
  if (!backupproject::ScanSourceTree(source, nullptr, &entries, &error)) {
    test_support::Check(false, "agreement fixture scan", error);
    return;
  }

  const PackMethod packs[3] = {PackMethod::kMyPack, PackMethod::kUstar,
                               PackMethod::kFastUstar};
  std::vector<std::vector<backupproject::PackedEntry>> scanned(3);
  for (int index = 0; index < 3; ++index) {
    const std::string packed =
        workdir + "/agree-" + std::to_string(index) + ".pack";
    test_support::RemoveTree(packed);
    if (!backupproject::PackEntries(packs[index], entries, packed, &error)) {
      test_support::Check(false, std::string("agree pack ") +
                                      backupproject::PackMethodName(packs[index]),
                          error);
      continue;
    }
    backupproject::PackedStreamReader reader;
    if (!reader.Open(packed, &error) ||
        !reader.Scan(packs[index], &error)) {
      test_support::Check(false, std::string("agree scan ") +
                                      backupproject::PackMethodName(packs[index]),
                          error);
      continue;
    }
    scanned[index] = reader.entries();
  }

  for (int index = 0; index < 3; ++index) {
    const std::string label =
        std::string(backupproject::PackMethodName(packs[index]));
    if (scanned[index].size() != entries.size()) {
      test_support::Check(false, label + " keeps every entry",
                          std::to_string(scanned[index].size()) + " vs " +
                              std::to_string(entries.size()));
      continue;
    }
    std::string detail;
    for (std::size_t position = 0; position < entries.size(); ++position) {
      const ArchiveEntry& before = entries[position];
      const ArchiveEntry& after = scanned[index][position].entry;
      if (before.archive_path != after.archive_path) {
        detail = "path: " + before.archive_path + " vs " + after.archive_path;
        break;
      }
      if (before.type != after.type) {
        detail = "type of " + before.archive_path;
        break;
      }
      if (before.mode != after.mode || before.uid != after.uid ||
          before.gid != after.gid || before.mtime_sec != after.mtime_sec ||
          before.size != after.size || before.link_target != after.link_target ||
          before.dev_major != after.dev_major ||
          before.dev_minor != after.dev_minor) {
        detail = "metadata of " + before.archive_path;
        break;
      }
    }
    test_support::Check(detail.empty(), label + " preserves every field", detail);
  }

  // 备份确定性：同样的源 + 同样的选项（不加密）必须产出逐字节相同的 .bak；
  // 加密之后 salt/IV 是随机的，字节必然不同，但两份都必须能恢复。
  for (const PackMethod pack : packs) {
    const std::string label =
        std::string(backupproject::PackMethodName(pack));
    BackupOptions options;
    options.pack_method = pack;
    options.compression_method = CompressionMethod::kLzssHuffman;
    const std::string first = workdir + "/det-a.bak";
    const std::string second = workdir + "/det-b.bak";
    test_support::RemoveTree(first);
    test_support::RemoveTree(second);
    BackupEngine engine;
    error.clear();
    const bool ok_first =
        engine.Backup(source, first, Filter(), options, &error);
    const bool ok_second =
        engine.Backup(source, second, Filter(), options, &error);
    std::string bytes_first;
    std::string bytes_second;
    test_support::ReadFile(first, &bytes_first);
    test_support::ReadFile(second, &bytes_second);
    test_support::Check(ok_first && ok_second && !bytes_first.empty() &&
                            bytes_first == bytes_second,
                        label + " unencrypted backup is byte-for-byte reproducible");

    options.encryption_method = EncryptionMethod::kAes256CtrHmacSha256;
    options.password = "determinism check";
    const std::string third = workdir + "/det-c.bak";
    const std::string fourth = workdir + "/det-d.bak";
    test_support::RemoveTree(third);
    test_support::RemoveTree(fourth);
    error.clear();
    const bool ok_third =
        engine.Backup(source, third, Filter(), options, &error);
    const bool ok_fourth =
        engine.Backup(source, fourth, Filter(), options, &error);
    std::string bytes_third;
    std::string bytes_fourth;
    test_support::ReadFile(third, &bytes_third);
    test_support::ReadFile(fourth, &bytes_fourth);
    test_support::Check(ok_third && ok_fourth && !bytes_third.empty() &&
                            bytes_third != bytes_fourth,
                        label + " encrypted backup uses a fresh random salt/IV");
    const std::string out_third = workdir + "/det-c-out";
    const std::string out_fourth = workdir + "/det-d-out";
    test_support::RemoveTree(out_third);
    test_support::RemoveTree(out_fourth);
    RestoreOptions restore_options;
    restore_options.password = options.password;
    RestoreReport report;
    std::string detail_third;
    std::string detail_fourth;
    error.clear();
    const bool restored_third =
        engine.Restore(third, out_third, restore_options, &report, &error) &&
        test_support::CompareTrees(source, out_third, &detail_third);
    const bool restored_fourth =
        engine.Restore(fourth, out_fourth, restore_options, &report, &error) &&
        test_support::CompareTrees(source, out_fourth, &detail_fourth);
    test_support::Check(restored_third && restored_fourth,
                        label + " both encrypted backups restore",
                        error + detail_third + detail_fourth);
  }
}

}  // namespace

int main() {
  std::printf("archive pipeline test\n");
  std::printf("euid=%ld\n", static_cast<long>(::geteuid()));
  const std::string workdir = test_support::FreshDir("pipeline");

  std::vector<MatrixRow> rows;
  test_support::Section("27-combination matrix");
  RunMatrix(workdir, &rows);
  PrintMatrix(rows);
  std::size_t passed = 0;
  for (const MatrixRow& row : rows) {
    if (row.result == "PASS") {
      ++passed;
    }
  }
  std::printf("\nMATRIX_TOTAL\t%zu/%zu\n", passed, rows.size());
  test_support::Check(rows.size() == 27 && passed == 27,
                      "27/27 matrix combinations pass");

  RunMetadata(workdir);
  RunSpecialFiles(workdir, PackMethod::kMyPack);
  RunSpecialFiles(workdir, PackMethod::kUstar);
  RunSpecialFiles(workdir, PackMethod::kFastUstar);
  RunSocketSemantics(workdir);
  RunDeviceAndFormat(workdir);
  RunFilterScan(workdir);
  RunPackAgreement(workdir);

  return test_support::Finish("archive-pipeline");
}
