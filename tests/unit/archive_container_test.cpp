// archive_container_test.cpp
//
// 容器格式的"坏输入"专项：header 语义校验、截断、尾部垃圾、篡改、
// 密码策略、目标目录原子性，以及 MyPack v2 / v0.1 的边界。
//
// 每一条的核心断言都是同一句话：valid → success，invalid → clean fail，
// 而且失败时 destination 一定不存在。

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "archive.h"
#include "archive_pipeline.h"
#include "backup_engine.h"
#include "container_format.h"
#include "filter.h"
#include "pack_stream.h"
#include "test_support.h"

namespace {

using backupproject::ArchiveEntry;
using backupproject::ArchiveReader;
using backupproject::ArchiveWriter;
using backupproject::BackupEngine;
using backupproject::BackupOptions;
using backupproject::CompressionMethod;
using backupproject::ContainerHeader;
using backupproject::EncryptionMethod;
using backupproject::EntryType;
using backupproject::Filter;
using backupproject::PackMethod;
using backupproject::RestoreOptions;
using backupproject::RestoreReport;

const char* kPassword = "container test password";

bool ReadAll(const std::string& path, std::string* bytes) {
  return test_support::ReadFile(path, bytes);
}

bool WriteAll(const std::string& path, const std::string& bytes) {
  return test_support::WriteFile(path, bytes, 0644);
}

// 复制一份 archive，改动其中一个字节，用于篡改测试。
bool CopyAndMutate(const std::string& source, const std::string& target,
                   std::size_t offset, unsigned char mask) {
  std::string bytes;
  if (!ReadAll(source, &bytes)) {
    return false;
  }
  if (offset >= bytes.size()) {
    return false;
  }
  bytes[offset] = static_cast<char>(static_cast<unsigned char>(bytes[offset]) ^
                                    mask);
  return WriteAll(target, bytes);
}

// 把某个字节直接设成指定值。相比 XOR，"把字段改成什么"在测试里更直观，
// 也不会出现"XOR 之后这个字段仍然合法"这种假测试。
bool CopyAndSet(const std::string& source, const std::string& target,
                std::size_t offset, unsigned char value) {
  std::string bytes;
  if (!ReadAll(source, &bytes)) {
    return false;
  }
  if (offset >= bytes.size()) {
    return false;
  }
  bytes[offset] = static_cast<char>(value);
  return WriteAll(target, bytes);
}

bool CopyAndTruncate(const std::string& source, const std::string& target,
                     std::size_t keep) {
  std::string bytes;
  if (!ReadAll(source, &bytes)) {
    return false;
  }
  if (keep > bytes.size()) {
    keep = bytes.size();
  }
  return WriteAll(target, bytes.substr(0, keep));
}

// 一次"必须失败，且失败后 destination 不存在"的恢复。
void ExpectRestoreRejected(const std::string& label,
                           const std::string& archive,
                           const std::string& destination,
                           const std::string& password) {
  BackupEngine engine;
  RestoreOptions options;
  options.password = password;
  RestoreReport report;
  std::string error;
  const bool ok =
      engine.Restore(archive, destination, options, &report, &error);
  test_support::Check(!ok, label + ": restore rejected", error);
  test_support::Check(!test_support::Exists(destination),
                      label + ": destination not created");
  test_support::Check(error.find("Inspect") == std::string::npos,
                      label + ": error message is meaningful", error);
}

void BuildSource(const std::string& root) {
  test_support::Mkdir(root, 0755);
  test_support::WriteFile(root + "/a.txt", "alpha\n", 0644);
  test_support::Mkdir(root + "/sub", 0755);
  test_support::WriteFile(root + "/sub/b.txt", std::string(2048, 'b'), 0600);
  test_support::NormalizeTimes(root, 1700000000);
}

// ---- 1) container header 语义 ----------------------------------------------

void RunHeaderValidation(const std::string& workdir) {
  test_support::Section("container header: semantics and reserved bytes");
  const std::string source = workdir + "/hdr-source";
  BuildSource(source);

  struct Case {
    const char* label;
    std::size_t offset;
    unsigned char value;
  };
  // 偏移表见 docs/format/archive_v2_container.md：magic 0..7、version 8..9、
  // header_size 10..11、三个 method 12/13/14、flags 15、entry_count 16..23、
  // sizes 24..47、kdf_iterations 48..51、长度字段 52..55、salt 56..71、
  // iv 72..87、tag 88..119、sha 120..151、reserved 152..159。
  const Case header_cases[] = {
      {"magic corrupted", 0, 0x00},
      {"version changed", 8, 3},
      {"header_size changed", 10, 161},
      {"pack method unknown", 12, 0x7F},
      {"compression method unknown", 13, 0x7F},
      {"encryption method unknown", 14, 0x7F},
      {"flags non-zero", 15, 1},
      {"kdf_iterations non-zero while unencrypted", 48, 1},
      {"entry count zero", 16, 0},
      {"entry count changed", 17, 0x7F},
      {"reserved0 non-zero", 55, 1},
      {"reserved tail non-zero", 152, 1},
  };

  for (const PackMethod pack : {PackMethod::kMyPack, PackMethod::kUstar}) {
    BackupOptions options;
    options.pack_method = pack;
    BackupEngine engine;
    std::string error;
    const std::string archive = workdir + "/hdr.bak";
    test_support::RemoveTree(archive);
    if (!engine.Backup(source, archive, Filter(), options, &error)) {
      test_support::Check(false, "header fixture backup", error);
      continue;
    }
    for (const Case& item : header_cases) {
      const std::string mutated =
          workdir + "/hdr-" + std::to_string(item.offset) + "-" +
          std::to_string(item.value) + ".bak";
      test_support::RemoveTree(mutated);
      if (!CopyAndSet(archive, mutated, item.offset, item.value)) {
        test_support::Check(false, std::string("mutation setup: ") + item.label);
        continue;
      }
      ExpectRestoreRejected(item.label, mutated, mutated + ".out", "");
    }
    test_support::RemoveTree(archive);
  }

  // 加密容器：算法字段被改成别的已知取值时，参数组合必须被拒绝。
  BackupOptions encrypted;
  encrypted.pack_method = PackMethod::kMyPack;
  encrypted.encryption_method = EncryptionMethod::kAes256CtrHmacSha256;
  encrypted.password = kPassword;
  BackupEngine engine;
  std::string error;
  const std::string aes_archive = workdir + "/hdr-aes.bak";
  test_support::RemoveTree(aes_archive);
  if (engine.Backup(source, aes_archive, Filter(), encrypted, &error)) {
    const std::string changed = workdir + "/hdr-aes-none.bak";
    test_support::RemoveTree(changed);
    // encryption_method 2 -> 0：长度字段仍然是 AES 的那一套，必须被拒绝。
    std::string bytes;
    if (ReadAll(aes_archive, &bytes)) {
      bytes[14] = 0;
      WriteAll(changed, bytes);
      ExpectRestoreRejected("encryption method mismatch", changed,
                            changed + ".out", "");
    }
    const std::string bad_kdf = workdir + "/hdr-aes-kdf.bak";
    test_support::RemoveTree(bad_kdf);
    if (ReadAll(aes_archive, &bytes)) {
      bytes[48] = static_cast<char>(0x01);  // kdf_iterations 低位被改
      WriteAll(bad_kdf, bytes);
      ExpectRestoreRejected("kdf iterations mismatch", bad_kdf,
                            bad_kdf + ".out", kPassword);
    }
    const std::string bad_len = workdir + "/hdr-aes-len.bak";
    test_support::RemoveTree(bad_len);
    if (ReadAll(aes_archive, &bytes)) {
      bytes[53] = 8;  // iv_len 16 -> 8，与 AES 不符
      WriteAll(bad_len, bytes);
      ExpectRestoreRejected("iv length mismatch", bad_len, bad_len + ".out",
                            kPassword);
    }
    const std::string bad_pad = workdir + "/hdr-aes-pad.bak";
    test_support::RemoveTree(bad_pad);
    if (ReadAll(aes_archive, &bytes)) {
      // sal_len 是 16，salt 字段里 16..31 字节应当是 0；把其中一个改成非 0。
      bytes[70] = static_cast<char>(0x5A);
      WriteAll(bad_pad, bytes);
      ExpectRestoreRejected("non-zero salt padding", bad_pad, bad_pad + ".out",
                            kPassword);
    }
    test_support::RemoveTree(aes_archive);
  } else {
    test_support::Check(false, "AES header fixture backup", error);
  }
}

// ---- 2) 截断 / 尾部垃圾 ----------------------------------------------------

void RunTruncation(const std::string& workdir) {
  test_support::Section("truncation and trailing bytes");
  const std::string source = workdir + "/trunc-source";
  BuildSource(source);
  BackupOptions options;
  options.pack_method = PackMethod::kMyPack;
  options.compression_method = CompressionMethod::kLzssHuffman;
  options.encryption_method = EncryptionMethod::kAes256CtrHmacSha256;
  options.password = kPassword;
  BackupEngine engine;
  std::string error;
  const std::string archive = workdir + "/trunc.bak";
  test_support::RemoveTree(archive);
  if (!engine.Backup(source, archive, Filter(), options, &error)) {
    test_support::Check(false, "truncation fixture backup", error);
    return;
  }
  std::string bytes;
  test_support::Check(ReadAll(archive, &bytes), "read the container back");
  const std::size_t size = bytes.size();

  const std::size_t keeps[] = {0, 8, 100, 159, 160, 161, size / 2,
                               size - 1};
  for (const std::size_t keep : keeps) {
    const std::string truncated =
        workdir + "/trunc-" + std::to_string(keep) + ".bak";
    test_support::RemoveTree(truncated);
    if (!CopyAndTruncate(archive, truncated, keep)) {
      continue;
    }
    ExpectRestoreRejected("truncated to " + std::to_string(keep) + " bytes",
                          truncated, truncated + ".out", kPassword);
  }

  const std::string appended = workdir + "/trunc-append.bak";
  test_support::RemoveTree(appended);
  WriteAll(appended, bytes + "X");
  ExpectRestoreRejected("trailing byte appended", appended,
                        appended + ".out", kPassword);

  const std::string zero_appended = workdir + "/trunc-zero-append.bak";
  test_support::RemoveTree(zero_appended);
  WriteAll(zero_appended, bytes + std::string(64, '\0'));
  ExpectRestoreRejected("64 zero bytes appended", zero_appended,
                        zero_appended + ".out", kPassword);

  test_support::RemoveTree(archive);
}

// ---- 3) 篡改 ---------------------------------------------------------------

void RunTamper(const std::string& workdir) {
  test_support::Section("tamper: salt / iv / ciphertext / tag / sha");
  const std::string source = workdir + "/tamper-source";
  BuildSource(source);

  for (const EncryptionMethod method : {EncryptionMethod::kDesCbcHmacSha256,
                                        EncryptionMethod::kAes256CtrHmacSha256}) {
    const bool des = method == EncryptionMethod::kDesCbcHmacSha256;
    const char* tag = des ? "DES" : "AES";
    BackupOptions options;
    options.pack_method = PackMethod::kMyPack;
    options.encryption_method = method;
    options.password = kPassword;
    BackupEngine engine;
    std::string error;
    const std::string archive = workdir + "/tamper-" + tag + ".bak";
    test_support::RemoveTree(archive);
    if (!engine.Backup(source, archive, Filter(), options, &error)) {
      test_support::Check(false, std::string(tag) + " tamper fixture", error);
      continue;
    }
    // 密码绝对不能出现在归档里。
    std::string bytes;
    if (ReadAll(archive, &bytes)) {
      test_support::Check(bytes.find(kPassword) == std::string::npos,
                          std::string(tag) + " archive stores no plaintext password");
    }

    struct Spot {
      const char* label;
      std::size_t offset;
    };
    const std::size_t ciphertext_offset = 160;
    const Spot spots[] = {
        {"salt", 56},
        {"iv", 72},
        {"auth tag", 88},
        {"payload sha256", 120},
        {"ciphertext first byte", ciphertext_offset},
        {"ciphertext second block", ciphertext_offset + 20},
    };
    for (const Spot& spot : spots) {
      const std::string mutated = workdir + "/tamper-" + tag + "-" +
                                  std::to_string(spot.offset) + ".bak";
      test_support::RemoveTree(mutated);
      if (!CopyAndMutate(archive, mutated, spot.offset, 0xFF)) {
        test_support::Check(false, std::string(tag) + " mutation " + spot.label);
        continue;
      }
      ExpectRestoreRejected(std::string(tag) + " " + spot.label + " tampered",
                            mutated, mutated + ".out", kPassword);
    }

    // wrong password 必须失败在认证上，而不是靠 padding 失败。
    {
      const std::string destination = workdir + "/tamper-" + tag + "-wrong";
      RestoreOptions wrong;
      wrong.password = "not the password";
      RestoreReport report;
      error.clear();
      const bool ok =
          engine.Restore(archive, destination, wrong, &report, &error);
      test_support::Check(!ok, std::string(tag) + " wrong password rejected",
                          error);
      test_support::Check(error.find("Authentication failed") != std::string::npos,
                          std::string(tag) +
                              " wrong password fails on HMAC, not on padding",
                          error);
      test_support::Check(!test_support::Exists(destination),
                          std::string(tag) +
                              " wrong password leaves no destination");
    }
    // 空密码策略：既不能备份，也不能恢复。
    {
      BackupOptions empty = options;
      empty.password.clear();
      const std::string empty_archive = workdir + "/tamper-empty.bak";
      test_support::RemoveTree(empty_archive);
      error.clear();
      const bool ok = engine.Backup(source, empty_archive, Filter(), empty,
                                    &error);
      test_support::Check(!ok, std::string(tag) + " empty password rejected");
      test_support::Check(!test_support::Exists(empty_archive),
                          std::string(tag) +
                              " empty password leaves no archive");
      ExpectRestoreRejected(std::string(tag) + " empty password restore",
                            archive, workdir + "/tamper-empty-out", "");
    }
    // 正确密码仍然必须成功——否则上面那些"失败"证明不了任何东西。
    {
      const std::string destination = workdir + "/tamper-" + tag + "-ok";
      RestoreOptions good;
      good.password = kPassword;
      RestoreReport report;
      error.clear();
      const bool ok = engine.Restore(archive, destination, good, &report, &error);
      std::string detail;
      test_support::Check(ok &&
                              test_support::CompareTrees(source, destination,
                                                         &detail),
                          std::string(tag) +
                              " untampered archive still restores correctly",
                          error + detail);
    }
  }
}

// ---- 4) destination 原子性 -------------------------------------------------

void RunDestinationAtomicity(const std::string& workdir) {
  test_support::Section("destination atomicity");
  const std::string source = workdir + "/atomic-source";
  BuildSource(source);
  BackupOptions options;
  options.pack_method = PackMethod::kMyPack;
  options.encryption_method = EncryptionMethod::kAes256CtrHmacSha256;
  options.password = kPassword;
  BackupEngine engine;
  std::string error;
  const std::string archive = workdir + "/atomic.bak";
  test_support::RemoveTree(archive);
  if (!engine.Backup(source, archive, Filter(), options, &error)) {
    test_support::Check(false, "atomicity fixture backup", error);
    return;
  }

  // 非空目标：必须在动手之前失败，而且不能动里面的东西。
  const std::string busy = workdir + "/atomic-busy";
  test_support::Mkdir(busy, 0755);
  test_support::WriteFile(busy + "/existing.txt", "do not touch\n", 0644);
  RestoreOptions good;
  good.password = kPassword;
  RestoreReport report;
  error.clear();
  const bool ok = engine.Restore(archive, busy, good, &report, &error);
  test_support::Check(!ok, "non-empty destination rejected", error);
  std::string existing;
  test_support::Check(test_support::ReadFile(busy + "/existing.txt", &existing) &&
                          existing == "do not touch\n",
                      "existing destination content untouched");

  // 目标是普通文件：拒绝。
  const std::string file_target = workdir + "/atomic-file";
  test_support::WriteFile(file_target, "i am a file\n", 0644);
  error.clear();
  const bool file_ok = engine.Restore(archive, file_target, good, &report, &error);
  test_support::Check(!file_ok, "destination that is a file rejected", error);

  // 空目标目录：允许，并且必须被恢复出来的内容替换。
  const std::string empty = workdir + "/atomic-empty";
  test_support::Mkdir(empty, 0755);
  error.clear();
  const bool empty_ok = engine.Restore(archive, empty, good, &report, &error);
  std::string detail;
  test_support::Check(empty_ok && test_support::CompareTrees(source, empty, &detail),
                      "empty destination is filled correctly", error + detail);

  // 失败之后不能在目标旁边留下暂存目录。
  const std::string root = workdir;
  const std::vector<std::string> names = test_support::DirEntries(root);
  bool leftover = false;
  for (const std::string& name : names) {
    if (name.find(".bptmp-") != std::string::npos ||
        name.find(".bp-unpack-") != std::string::npos ||
        name.find(".bp-packed-") != std::string::npos) {
      leftover = true;
    }
  }
  test_support::Check(!leftover, "no temporary files left behind in the workdir");
}

// ---- 5) MyPack v2 格式边界 -------------------------------------------------

void WriteMyPackFixture(const std::string& path,
                        const std::vector<ArchiveEntry>& entries) {
  test_support::RemoveTree(path);
  std::string error;
  const bool ok = backupproject::PackEntries(PackMethod::kMyPack, entries, path,
                                             &error);
  test_support::Check(ok, "MyPack v2 fixture written", error);
}

// 用条目表建一条最小的 MyPack v2 流，再按偏移破坏一个字节。
void RunMyPackBoundaries(const std::string& workdir) {
  test_support::Section("MyPack v2 malformed inputs");
  const std::string payload = workdir + "/mypack-payload.bin";
  test_support::WriteFile(payload, "0123456789", 0644);

  std::vector<ArchiveEntry> entries;
  ArchiveEntry root;
  root.archive_path = ".";
  root.type = EntryType::kDirectory;
  root.mode = 0755;
  entries.push_back(root);
  ArchiveEntry file;
  file.archive_path = "payload.bin";
  file.source_path = payload;
  file.type = EntryType::kRegularFile;
  file.mode = 0644;
  file.size = 10;
  struct stat info;
  if (::lstat(payload.c_str(), &info) == 0) {
    file.mtime_sec = static_cast<std::int64_t>(info.st_mtim.tv_sec);
    file.mtime_nsec = static_cast<std::uint32_t>(info.st_mtim.tv_nsec);
  }
  entries.push_back(file);
  ArchiveEntry link;
  link.archive_path = "link";
  link.type = EntryType::kSymlink;
  link.link_target = "payload.bin";
  link.mode = 0777;
  entries.push_back(link);

  const std::string archive = workdir + "/mypack.pack";
  WriteMyPackFixture(archive, entries);
  std::string bytes;
  if (!ReadAll(archive, &bytes)) {
    test_support::Check(false, "read the MyPack fixture");
    return;
  }
  const std::size_t first_entry = 32;
  const std::size_t first_path = first_entry + 64;
  // 第二条 entry 的 header 紧跟在 "." 的 path 之后。
  const std::size_t second_entry = first_path + 1;

  struct Spot {
    const char* label;
    std::size_t offset;
    unsigned char value;
  };
  const Spot spots[] = {
      {"global reserved non-zero", 24, 1},
      {"entry type unknown", first_entry + 0, 0x7F},
      {"entry flags non-zero", first_entry + 1, 1},
      {"entry reserved0 non-zero", first_entry + 3, 1},
      {"entry reserved tail non-zero", first_entry + 52, 1},
      {"directory carries a payload", first_entry + 36, 0x10},
      {"entry mode out of range", first_entry + 13, 0xFF},
      {"entry mtime nanoseconds out of range", first_entry + 27, 0xFF},
      {"second entry type unknown", second_entry + 0, 0x7F},
  };
  for (const Spot& spot : spots) {
    const std::string mutated = workdir + "/mypack-" +
                                std::to_string(spot.offset) + ".pack";
    test_support::RemoveTree(mutated);
    if (!CopyAndMutate(archive, mutated, spot.offset, 0xFF)) {
      test_support::Check(false, std::string("mypack mutation ") + spot.label);
      continue;
    }
    backupproject::PackedStreamReader reader;
    std::string error;
    const bool opened = reader.Open(mutated, &error);
    const bool scanned = opened && reader.Scan(PackMethod::kMyPack, &error);
    test_support::Check(!scanned, std::string("MyPack ") + spot.label +
                                      " rejected", error);
  }

  // 合法流仍然必须成功——否则上面的"拒绝"没有意义。
  {
    backupproject::PackedStreamReader reader;
    std::string error;
    const bool ok = reader.Open(archive, &error) &&
                    reader.Scan(PackMethod::kMyPack, &error);
    test_support::Check(ok, "unmutated MyPack stream accepted", error);
    if (ok) {
      test_support::Check(reader.entries().size() == 3,
                          "MyPack entry count is 3");
    }
  }
  // 尾部垃圾。
  {
    const std::string appended = workdir + "/mypack-trailing.pack";
    test_support::RemoveTree(appended);
    WriteAll(appended, bytes + "ZZZZ");
    backupproject::PackedStreamReader reader;
    std::string error;
    const bool scanned = reader.Open(appended, &error) &&
                         reader.Scan(PackMethod::kMyPack, &error);
    test_support::Check(!scanned, "MyPack trailing bytes rejected", error);
  }
  // 截断。
  for (const std::size_t keep : {std::size_t(10), std::size_t(40),
                                 std::size_t(96), bytes.size() - 1}) {
    const std::string truncated =
        workdir + "/mypack-trunc-" + std::to_string(keep) + ".pack";
    test_support::RemoveTree(truncated);
    CopyAndTruncate(archive, truncated, keep);
    backupproject::PackedStreamReader reader;
    std::string error;
    const bool scanned = reader.Open(truncated, &error) &&
                         reader.Scan(PackMethod::kMyPack, &error);
    test_support::Check(!scanned,
                        "MyPack truncated to " + std::to_string(keep) +
                            " bytes rejected",
                        error);
  }
  // 重复路径 / 父目录不是目录：直接喂一份手工条目表，写侧必须拦住。
  {
    std::vector<ArchiveEntry> duplicated = entries;
    ArchiveEntry again = file;
    again.archive_path = "payload.bin";
    duplicated.push_back(again);
    const std::string path = workdir + "/mypack-duplicate.pack";
    test_support::RemoveTree(path);
    std::string error;
    const bool ok = backupproject::PackEntries(PackMethod::kMyPack, duplicated,
                                               path, &error);
    test_support::Check(!ok, "duplicate archive path rejected by the writer",
                        error);
    test_support::Check(!test_support::Exists(path),
                        "no packed stream after a rejected entry list");
  }
  {
    std::vector<ArchiveEntry> conflict = entries;
    ArchiveEntry under_file = link;
    under_file.archive_path = "payload.bin/inside";
    conflict.push_back(under_file);
    const std::string path = workdir + "/mypack-conflict.pack";
    test_support::RemoveTree(path);
    std::string error;
    const bool ok =
        backupproject::PackEntries(PackMethod::kMyPack, conflict, path, &error);
    test_support::Check(!ok, "file used as a parent directory rejected", error);
  }
  {
    std::vector<ArchiveEntry> bad_target = entries;
    ArchiveEntry hard = link;
    hard.archive_path = "hard";
    hard.type = EntryType::kHardLink;
    hard.link_target = "does-not-exist";
    bad_target.push_back(hard);
    const std::string path = workdir + "/mypack-bad-hardlink.pack";
    test_support::RemoveTree(path);
    std::string error;
    if (backupproject::PackEntries(PackMethod::kMyPack, bad_target, path,
                                   &error)) {
      backupproject::PackedStreamReader reader;
      const bool scanned = reader.Open(path, &error) &&
                           reader.Scan(PackMethod::kMyPack, &error);
      test_support::Check(!scanned, "hard link with a missing target rejected",
                          error);
    } else {
      test_support::Check(true, "hard link with a missing target rejected");
    }
  }
  // 路径遍历 / 绝对路径。
  for (const char* bad : {"../escape", "/absolute", "a/../b", "a//b"}) {
    std::vector<ArchiveEntry> traversal = entries;
    ArchiveEntry entry = link;
    entry.archive_path = bad;
    entry.link_target = "x";
    traversal.push_back(entry);
    const std::string path = workdir + "/mypack-traversal.pack";
    test_support::RemoveTree(path);
    std::string error;
    const bool ok =
        backupproject::PackEntries(PackMethod::kMyPack, traversal, path, &error);
    test_support::Check(!ok, std::string("path rejected: ") + bad, error);
  }
}

// ---- 6) v0.1 兼容与格式识别 ------------------------------------------------

void RunLegacyCompatibility(const std::string& workdir) {
  test_support::Section("legacy v0.1 archives and format identification");
  const std::string source = workdir + "/legacy-source";
  BuildSource(source);

  const std::string archive = workdir + "/legacy.bak";
  test_support::RemoveTree(archive);
  ArchiveWriter writer;
  std::string error;
  if (!writer.Write(source, archive, &error)) {
    test_support::Check(false, "legacy v0.1 backup", error);
    return;
  }

  const char* password = "";
  // 1) 新的流水线入口必须能恢复 v0.1：格式判断只看 magic。
  const std::string pipeline_out = workdir + "/legacy-pipeline-out";
  RestoreOptions options;
  RestoreReport report;
  const bool pipeline_ok = backupproject::RunRestorePipeline(
      archive, pipeline_out, options, &report, &error);
  std::string detail;
  test_support::Check(pipeline_ok && test_support::CompareTrees(source, pipeline_out, &detail),
                      "v0.1 restore through the pipeline entry point",
                      error + detail);
  (void)password;

  // 2) 旧的 BackupEngine::Restore 仍然工作，而且能认出 v2 容器。
  const std::string engine_out = workdir + "/legacy-engine-out";
  BackupEngine engine;
  error.clear();
  test_support::Check(engine.Restore(archive, engine_out, &error) &&
                          test_support::CompareTrees(source, engine_out, &detail),
                      "v0.1 restore through BackupEngine::Restore",
                      error + detail);

  // 3) 格式识别。
  backupproject::ArchiveFileInfo info;
  error.clear();
  const bool identified = backupproject::IdentifyArchiveFile(archive, &info, &error);
  test_support::Check(identified &&
                          info.kind == backupproject::ArchiveFileInfo::Kind::kLegacyV01 &&
                          info.format_version == 1,
                      "v0.1 identified as legacy", error);
  test_support::Check(info.entry_count > 0, "v0.1 entry count reported");

  // 4) v2 容器识别 + 不需要密码就能读外层 header。
  BackupOptions v2;
  v2.pack_method = PackMethod::kFastUstar;
  v2.compression_method = CompressionMethod::kLzssHuffman;
  v2.encryption_method = EncryptionMethod::kAes256CtrHmacSha256;
  v2.password = kPassword;
  const std::string container = workdir + "/identified.bak";
  test_support::RemoveTree(container);
  error.clear();
  if (!engine.Backup(source, container, Filter(), v2, &error)) {
    test_support::Check(false, "v2 container backup", error);
    return;
  }
  error.clear();
  const bool v2_identified =
      backupproject::IdentifyArchiveFile(container, &info, &error);
  test_support::Check(v2_identified &&
                          info.kind == backupproject::ArchiveFileInfo::Kind::kContainerV2 &&
                          info.format_version == 2,
                      "v2 container identified", error);
  test_support::Check(info.pack_method == PackMethod::kFastUstar &&
                          info.compression_method == CompressionMethod::kLzssHuffman &&
                          info.encryption_method ==
                              EncryptionMethod::kAes256CtrHmacSha256,
                      "v2 container reports its three algorithm ids");
  test_support::Check(info.entry_count == 4,
                      "v2 container reports the entry count without a password",
                      std::to_string(info.entry_count));
  test_support::Check(!info.password_hint.empty(),
                      "v2 container warns that a password is required");

  // 5) BackupCatalog 的 InspectHeader 也能认出 v2。
  ArchiveReader reader;
  backupproject::ArchiveSummary summary;
  error.clear();
  const bool inspected = reader.InspectHeader(container, &summary, &error);
  test_support::Check(inspected && summary.format_version == 2 &&
                          summary.entry_count == 4,
                      "ArchiveReader::InspectHeader recognizes v2", error);

  // 6) 覆盖保护：归档文件已存在时必须拒绝，且不改动原文件。
  std::string before;
  ReadAll(container, &before);
  error.clear();
  const bool overwrite = engine.Backup(source, container, Filter(), v2, &error);
  test_support::Check(!overwrite, "backup refuses to overwrite an existing .bak",
                      error);
  std::string after;
  ReadAll(container, &after);
  test_support::Check(before == after, "existing .bak left byte-identical");
}

// ---- 7) 空输入 -------------------------------------------------------------

void RunEmptyInputs(const std::string& workdir) {
  test_support::Section("empty directory and empty file");
  const std::string source = workdir + "/empty-source";
  test_support::Mkdir(source, 0755);
  test_support::WriteFile(source + "/zero.bin", "", 0644);
  test_support::Mkdir(source + "/empty-dir", 0700);
  test_support::NormalizeTimes(source, 1600000000);

  const PackMethod packs[3] = {PackMethod::kMyPack, PackMethod::kUstar,
                               PackMethod::kFastUstar};
  for (const PackMethod pack : packs) {
    const std::string archive =
        workdir + "/empty-" + std::to_string(static_cast<int>(pack)) + ".bak";
    const std::string destination = archive + ".out";
    test_support::RemoveTree(archive);
    BackupOptions options;
    options.pack_method = pack;
    options.compression_method = CompressionMethod::kHuffman;
    BackupEngine engine;
    std::string error;
    const bool ok = engine.Backup(source, archive, Filter(), options, &error);
    if (!ok) {
      test_support::Check(false,
                          std::string("empty fixture backup (") +
                              backupproject::PackMethodName(pack) + ")",
                          error);
      continue;
    }
    RestoreOptions restore_options;
    RestoreReport report;
    const bool restored =
        engine.Restore(archive, destination, restore_options, &report, &error);
    std::string detail;
    test_support::Check(restored &&
                            test_support::CompareTrees(source, destination, &detail),
                        std::string("empty fixture round trip (") +
                            backupproject::PackMethodName(pack) + ")",
                        error + detail);
  }
}

}  // namespace

int main() {
  std::printf("archive container / malformed input test\n");
  const std::string workdir = test_support::FreshDir("container");

  RunHeaderValidation(workdir);
  RunTruncation(workdir);
  RunTamper(workdir);
  RunDestinationAtomicity(workdir);
  RunMyPackBoundaries(workdir);
  RunLegacyCompatibility(workdir);
  RunEmptyInputs(workdir);

  return test_support::Finish("archive-container");
}
