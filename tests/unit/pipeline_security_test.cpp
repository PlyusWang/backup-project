// pipeline_security_test.cpp
//
// 流水线的"安全 / 资源 / 完整性"回归：文件权限、失败之后不留半成品、父目录里
// 不留临时残余，以及"运行中观察"私有工作目录。
//
// 为什么全部走真实 stat 而不是读代码：0600 / 0700 / O_EXCL / 失败后清理都是
// 内核侧行为，umask、mkstemp 的默认模式、link() 的 EEXIST 语义都只有落盘才
// 能验证。这里不 mock 任何东西——被测的就是 src/ 下的真实实现。
//
// "运行中观察"一条写成"观察到才断言"：备份本身只要几十毫秒到几秒，父进程
// 未必撞上窗口。没撞上就 Note，撞上了每一条都是硬断言——既不会随机变红，也
// 不会假装观察到了。

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "archive_pipeline.h"
#include "backup_engine.h"
#include "file_io.h"
#include "filter.h"
#include "pack_stream.h"
#include "test_support.h"
#include "tree_scanner.h"
#include "ustar.h"

namespace {

using backupproject::ArchiveEntry;
using backupproject::BackupEngine;
using backupproject::BackupOptions;
using backupproject::CompressionMethod;
using backupproject::EncryptionMethod;
using backupproject::FileSink;
using backupproject::Filter;
using backupproject::PackMethod;
using backupproject::RestoreOptions;
using backupproject::TempDirectoryGuard;

// ---- 共用小工具 ------------------------------------------------------------

std::uint32_t ModeOf(const std::string& path) {
  struct stat info;
  if (::lstat(path.c_str(), &info) != 0) {
    return 0;
  }
  return static_cast<std::uint32_t>(info.st_mode & 07777);
}

void CheckMode(const std::string& path, std::uint32_t expected,
               const std::string& label) {
  const std::uint32_t actual = ModeOf(path);
  const std::string detail =
      actual == 0 ? std::string("file is missing")
                  : "expected " + test_support::Octal(expected) + ", got " +
                        test_support::Octal(actual);
  test_support::Check(actual == expected, label, detail);
}

// 流水线的中间产物永远是点开头的 .bp* 名字（.bp-work- / .bp-work- 下的文件 /
// <dest>.bptmp-）。用户自己的文件不会长这样，所以用前缀判定"临时残余"。
bool IsTempArtifact(const std::string& name) {
  return name.size() > 3 && name.compare(0, 3, ".bp") == 0;
}

std::string JoinList(const std::vector<std::string>& names) {
  std::string text;
  for (const std::string& name : names) {
    if (!text.empty()) {
      text += ", ";
    }
    text += name;
  }
  return text.empty() ? std::string("<empty>") : text;
}

// 整个目录的精确内容：比"没有 .bp*"更严——多出来的任何东西都要能解释。
void CheckListing(const std::string& directory,
                  std::vector<std::string> expected,
                  const std::string& label) {
  const std::vector<std::string> actual = test_support::DirEntries(directory);
  std::sort(expected.begin(), expected.end());
  test_support::Check(actual == expected, label,
                      "expected [" + JoinList(expected) + "], got [" +
                          JoinList(actual) + "]");
}

void CheckNoResidue(const std::string& directory, const std::string& label) {
  std::vector<std::string> leftovers;
  for (const std::string& name : test_support::DirEntries(directory)) {
    if (IsTempArtifact(name)) {
      leftovers.push_back(name);
    }
  }
  test_support::Check(leftovers.empty(), label,
                      "leftovers: " + JoinList(leftovers));
}

bool CopyFile(const std::string& from, const std::string& to) {
  std::string bytes;
  return test_support::ReadFile(from, &bytes) &&
         test_support::WriteFile(to, bytes, 0600);
}

// 小而有代表性的源树：普通文件 + 子目录 + 已知 mtime（便于整树比较）。
void BuildSource(const std::string& root) {
  test_support::Mkdir(root, 0755);
  test_support::WriteFile(root + "/a.txt", "alpha beta gamma\n", 0644);
  test_support::Mkdir(root + "/sub", 0755);
  test_support::WriteFile(root + "/sub/b.bin", std::string(3000, 'b'), 0600);
  test_support::NormalizeTimes(root, 1700000000);
}

BackupOptions PlainOptions() {
  BackupOptions options;
  options.pack_method = PackMethod::kUstar;
  options.compression_method = CompressionMethod::kNone;
  options.encryption_method = EncryptionMethod::kNone;
  return options;
}

BackupOptions AesOptions() {
  BackupOptions options;
  options.pack_method = PackMethod::kMyPack;
  options.compression_method = CompressionMethod::kHuffman;
  options.encryption_method = EncryptionMethod::kAes256CtrHmacSha256;
  options.password = "correct horse battery staple";
  return options;
}

BackupOptions DesOptions() {
  BackupOptions options;
  options.pack_method = PackMethod::kMyPack;
  options.compression_method = CompressionMethod::kHuffman;
  options.encryption_method = EncryptionMethod::kDesCbcHmacSha256;
  options.password = "des legacy password";
  return options;
}

// ---- §24.1 备份产物权限 ----------------------------------------------------

void TestBackupPermissions(const std::string& root) {
  test_support::Section("§24 备份产物的真实权限");
  const std::string source = root + "/src";
  const std::string out = root + "/out";
  const std::string dest = root + "/dest";
  BuildSource(source);
  test_support::Check(test_support::Mkdir(out, 0755), "archive parent created");
  test_support::Check(test_support::Mkdir(dest, 0755), "restore parent created");

  BackupEngine engine;
  std::string error;
  const std::string plain = out + "/plain.bak";
  BackupOptions plain_options = PlainOptions();
  test_support::Check(
      engine.Backup(source, plain, Filter(), plain_options, &error),
      "unencrypted v2 backup succeeds", error);
  CheckMode(plain, 0600, "unencrypted .bak is 0600");
  CheckListing(out, {"plain.bak"},
               "archive parent holds only the .bak after a success");
  CheckNoResidue(out, "no temp residue after an unencrypted backup");

  const std::string encrypted = out + "/enc.bak";
  error.clear();
  BackupOptions aes_options = AesOptions();
  test_support::Check(
      engine.Backup(source, encrypted, Filter(), aes_options, &error),
      "AES v2 backup succeeds", error);
  CheckMode(encrypted, 0600, "AES .bak is 0600");
  CheckListing(out, {"enc.bak", "plain.bak"},
               "archive parent holds only the .bak files after a success");
  CheckNoResidue(out, "no temp residue after an encrypted backup");

  const std::string des = out + "/des.bak";
  error.clear();
  BackupOptions des_options = DesOptions();
  test_support::Check(engine.Backup(source, des, Filter(), des_options, &error),
                      "DES v2 backup succeeds", error);
  CheckMode(des, 0600, "DES .bak is 0600");
  CheckListing(out, {"des.bak", "enc.bak", "plain.bak"},
               "archive parent holds only the .bak files");
  CheckNoResidue(out, "no temp residue after the whole batch");

  // 恢复也必须走同一条"暂存目录 + rename"的发布路径并留下干净的父目录。
  const std::string plain_out = dest + "/plain-out";
  RestoreOptions no_password;
  backupproject::RestoreReport report;
  error.clear();
  std::string detail;
  test_support::Check(engine.Restore(plain, plain_out, no_password, &report,
                                     &error) &&
                          test_support::CompareTrees(source, plain_out, &detail),
                      "unencrypted .bak restores identically", error + detail);
  const std::string enc_out = dest + "/enc-out";
  RestoreOptions with_password;
  with_password.password = aes_options.password;
  error.clear();
  detail.clear();
  test_support::Check(engine.Restore(encrypted, enc_out, with_password, &report,
                                     &error) &&
                          test_support::CompareTrees(source, enc_out, &detail),
                      "AES .bak restores identically", error + detail);
  CheckListing(dest, {"enc-out", "plain-out"},
               "restore parent holds only the restored trees");
  CheckNoResidue(dest, "no temp residue after successful restores");

  // 恢复目录本身不该比源树露出更多权限（0600 的产物恢复成源树的 0644）。
  CheckMode(plain_out, 0755, "restored root keeps the archived mode");
  CheckMode(plain_out + "/a.txt", 0644, "restored file keeps the archived mode");
}

// ---- §24.2 备份失败：不留最终产物、不留残余 ---------------------------------

void TestBackupFailureLeavesNothing(const std::string& root) {
  test_support::Section("§24 备份失败之后不留最终 .bak 与残余");
  const std::string source = root + "/src";
  const std::string out = root + "/out-fail";
  test_support::Check(test_support::Mkdir(out, 0755), "failure parent created");

  BackupEngine engine;
  std::string error;

  // (a) 目标已存在：必须失败，且原文件一个字节都不能动。
  const std::string taken = out + "/taken.bak";
  test_support::Check(test_support::WriteFile(taken, "not yours", 0640),
                      "pre-existing archive written");
  BackupOptions aes_options = AesOptions();
  test_support::Check(!engine.Backup(source, taken, Filter(), aes_options,
                                     &error),
                      "backup refuses to overwrite an existing .bak");
  test_support::Check(!error.empty(), "the refusal is explained", error);
  std::string content;
  test_support::Check(test_support::ReadFile(taken, &content) &&
                          content == "not yours",
                      "the existing .bak content is untouched");
  CheckMode(taken, 0640, "the existing .bak mode is untouched");
  CheckListing(out, {"taken.bak"}, "no new file appeared for a refused backup");
  CheckNoResidue(out, "no temp residue after a refused backup");

  // (b) 加密但密码为空：明确失败，且连最终路径都不该出现。
  const std::string empty_password = out + "/empty-password.bak";
  BackupOptions broken = AesOptions();
  broken.password.clear();
  error.clear();
  test_support::Check(!engine.Backup(source, empty_password, Filter(), broken,
                                     &error),
                      "encryption with an empty password fails");
  test_support::Check(!test_support::Exists(empty_password),
                      "no final .bak appears for the failed encryption");
  CheckListing(out, {"taken.bak"}, "the parent is unchanged after the failure");
  CheckNoResidue(out, "no temp residue after the empty-password failure");

  // (c) 源目录不存在：失败得更早，同样不留残余。
  const std::string missing = out + "/missing-source.bak";
  error.clear();
  test_support::Check(!engine.Backup(source + "-does-not-exist", missing,
                                     Filter(), aes_options, &error),
                      "backup of a missing source fails");
  test_support::Check(!test_support::Exists(missing),
                      "no final .bak appears for a missing source");
  CheckListing(out, {"taken.bak"}, "the parent is unchanged after case (c)");
  CheckNoResidue(out, "no temp residue after case (c)");

  // (d) 归档路径落在源目录内部：必须拒绝，否则归档文件自己会变成输入。
  const std::string inside = source + "/inside.bak";
  error.clear();
  test_support::Check(!engine.Backup(source, inside, Filter(), aes_options,
                                     &error),
                      "backup into the source tree is refused");
  test_support::Check(!test_support::Exists(inside),
                      "no archive appeared inside the source tree");
  CheckListing(source, {"a.txt", "sub"},
               "the source tree is unchanged by the refusal");
  CheckNoResidue(source, "no temp residue inside the source tree");
}

// ---- §24.3 恢复失败：不留目标目录 -------------------------------------------

void TestRestoreFailureLeavesNothing(const std::string& root) {
  test_support::Section("§24 恢复失败之后不留目标目录与残余");
  const std::string source = root + "/src";
  const std::string out = root + "/out";
  const std::string dest = root + "/dest-fail";
  test_support::Check(test_support::Mkdir(dest, 0755), "failure parent created");

  const std::string encrypted = out + "/enc.bak";
  const std::string des = out + "/des.bak";
  test_support::Check(test_support::Exists(encrypted) &&
                          test_support::Exists(des),
                      "encrypted fixtures from the earlier section exist");

  std::string original;
  test_support::Check(test_support::ReadFile(encrypted, &original),
                      "the AES fixture can be read");
  const std::uint32_t original_mode = ModeOf(encrypted);

  BackupEngine engine;

  // (a) wrong password：认证必须在写一个字节之前失败。
  RestoreOptions wrong;
  wrong.password = "definitely not the password";
  backupproject::RestoreReport report;
  std::string error;
  const std::string wrong_out = dest + "/wrong-password";
  test_support::Check(!engine.Restore(encrypted, wrong_out, wrong, &report,
                                      &error),
                      "restore with a wrong password fails");
  test_support::Check(!error.empty(), "the authentication failure is explained",
                      error);
  test_support::Check(!test_support::Exists(wrong_out),
                      "no destination directory after a wrong password");
  CheckListing(dest, {}, "the destination parent stays empty (wrong password)");
  CheckNoResidue(dest, "no temp residue after a wrong password");

  // (b) DES 容器同样如此（另一条加密路径，同样的"先认证后解密"约定）。
  RestoreOptions des_wrong;
  des_wrong.password = "not the des password";
  error.clear();
  const std::string des_out = dest + "/des-wrong-password";
  test_support::Check(!engine.Restore(des, des_out, des_wrong, &report, &error),
                      "restore of a DES container with a wrong password fails");
  test_support::Check(!test_support::Exists(des_out),
                      "no destination directory for the DES failure");
  CheckListing(dest, {}, "the destination parent stays empty (DES)");
  CheckNoResidue(dest, "no temp residue after the DES failure");

  // (c) 篡改密文：HMAC / SHA-256 必须拦下它。
  const std::string tampered_dir = root + "/tampered";
  test_support::Check(test_support::Mkdir(tampered_dir, 0755),
                      "tampered fixture dir created");
  const std::string tampered = tampered_dir + "/enc.bak";
  test_support::Check(CopyFile(encrypted, tampered), "encrypted fixture copied");
  std::string bytes;
  test_support::Check(test_support::ReadFile(tampered, &bytes),
                      "the copy can be read");
  test_support::Check(bytes.size() > 200, "the fixture has a ciphertext payload",
                      std::to_string(bytes.size()));
  bytes[200] = static_cast<char>(static_cast<unsigned char>(bytes[200]) ^ 0x01);
  test_support::Check(test_support::WriteFile(tampered, bytes, 0600),
                      "the ciphertext bit flip landed on disk");
  RestoreOptions correct;
  correct.password = AesOptions().password;
  error.clear();
  const std::string tampered_out = dest + "/tampered";
  test_support::Check(!engine.Restore(tampered, tampered_out, correct, &report,
                                      &error),
                      "a tampered ciphertext is rejected");
  test_support::Check(!error.empty(), "the tamper rejection is explained",
                      error);
  test_support::Check(!test_support::Exists(tampered_out),
                      "no destination directory after tampering");
  CheckListing(tampered_dir, {"enc.bak"},
               "the tampered fixture dir has no residue");
  CheckListing(dest, {}, "the destination parent stays empty (tampered)");
  CheckNoResidue(dest, "no temp residue after tampering");

  // (d) 篡改 header 里的 salt：它属于归一化 header，必须参与 MAC。
  const std::string salt_tampered = tampered_dir + "/enc-salt.bak";
  test_support::Check(CopyFile(encrypted, salt_tampered),
                      "second encrypted fixture copied");
  std::string salt_bytes;
  test_support::Check(test_support::ReadFile(salt_tampered, &salt_bytes),
                      "the second copy can be read");
  const std::size_t salt_offset = backupproject::container_v2::kSaltOffset;
  test_support::Check(salt_bytes.size() > salt_offset,
                      "the fixture has a salt field");
  salt_bytes[salt_offset] = static_cast<char>(
      static_cast<unsigned char>(salt_bytes[salt_offset]) ^ 0x80);
  test_support::Check(test_support::WriteFile(salt_tampered, salt_bytes, 0600),
                      "the header salt flip landed on disk");
  error.clear();
  const std::string salt_out = dest + "/salt";
  test_support::Check(!engine.Restore(salt_tampered, salt_out, correct, &report,
                                      &error),
                      "a mutated header salt is rejected");
  test_support::Check(!test_support::Exists(salt_out),
                      "no destination directory after header tampering");

  // (e) 截断：连 header 都不完整。
  const std::string truncated = tampered_dir + "/truncated.bak";
  test_support::Check(
      test_support::WriteFile(truncated, original.substr(0, 100), 0600),
      "truncated fixture written");
  error.clear();
  const std::string truncated_out = dest + "/truncated";
  test_support::Check(!engine.Restore(truncated, truncated_out, correct,
                                      &report, &error),
                      "a truncated archive is rejected");
  test_support::Check(!test_support::Exists(truncated_out),
                      "no destination directory after truncation");

  // 所有失败路径跑完之后：源归档一个字节、一个权限位都不能变。
  std::string after;
  test_support::Check(test_support::ReadFile(encrypted, &after) &&
                          after == original,
                      "the source .bak content is untouched by failed restores");
  test_support::Check(ModeOf(encrypted) == original_mode,
                      "the source .bak mode is untouched by failed restores");
  CheckListing(dest, {}, "the destination parent is still empty");
  CheckNoResidue(dest, "no temp residue after every failed restore");
}

// ---- §24.4 单元级权限 ------------------------------------------------------

void TestUnitPermissions(const std::string& root) {
  test_support::Section("§24 单元级：FileSink / 工作目录 / 打包器都是 0600");
  const std::string perms = root + "/perms";
  test_support::Check(test_support::Mkdir(perms, 0755), "perms dir created");

  std::string error;
  const std::string sink_path = perms + "/sink.bin";
  {
    FileSink sink;
    test_support::Check(sink.Open(sink_path, &error), "FileSink::Open succeeds",
                        error);
    test_support::Check(sink.Write("payload", 7, &error) && sink.Close(&error),
                        "FileSink::Close succeeds", error);
  }
  CheckMode(sink_path, 0600, "FileSink::Open output is 0600");

  {
    FileSink temp;
    test_support::Check(temp.OpenTemp(perms, "chunk-", &error),
                        "FileSink::OpenTemp succeeds", error);
    CheckMode(temp.path(), 0600, "FileSink::OpenTemp output is 0600");
    test_support::Check(
        temp.path().compare(0, perms.size() + 1, perms + "/") == 0,
        "FileSink::OpenTemp lands in the requested directory");
    temp.Abandon();
  }

  {
    TempDirectoryGuard guard;
    test_support::Check(guard.Create(perms, ".bp-work-", &error),
                        "TempDirectoryGuard::Create succeeds", error);
    CheckMode(guard.path(), 0700, "TempDirectoryGuard workspace is 0700");
    const std::string child = guard.Child("payload.bin");
    FileSink child_sink;
    test_support::Check(child_sink.Open(child, &error) &&
                            child_sink.Write("x", 1, &error) &&
                            child_sink.Close(&error),
                        "a file inside the workspace can be committed", error);
    CheckMode(child, 0600, "a file inside the workspace is 0600");
  }

  // 打包器写出来的文件同样是 0600：它们可能包含明文，没理由让别的用户读到。
  const std::string source = root + "/src";
  std::vector<ArchiveEntry> entries;
  error.clear();
  test_support::Check(backupproject::ScanSourceTree(source, nullptr, &entries,
                                                    &error),
                      "source tree scanned for the packer checks", error);

  const std::string baseline = perms + "/baseline.tar";
  error.clear();
  test_support::Check(backupproject::ustar::WriteBaseline(entries, baseline,
                                                          &error),
                      "ustar::WriteBaseline succeeds", error);
  CheckMode(baseline, 0600, "ustar::WriteBaseline output is 0600");

  const std::string fast = perms + "/fast.tar";
  error.clear();
  test_support::Check(backupproject::ustar::WriteFast(entries, fast, &error),
                      "ustar::WriteFast succeeds", error);
  CheckMode(fast, 0600, "ustar::WriteFast output is 0600");

  const std::string mypack = perms + "/mypack.pack";
  error.clear();
  test_support::Check(backupproject::PackEntries(PackMethod::kMyPack, entries,
                                                 mypack, &error),
                      "PackEntries(kMyPack) succeeds", error);
  CheckMode(mypack, 0600, "PackEntries(kMyPack) output is 0600");

  const std::string ustar_pack = perms + "/ustar.pack";
  error.clear();
  test_support::Check(backupproject::PackEntries(PackMethod::kUstar, entries,
                                                 ustar_pack, &error),
                      "PackEntries(kUstar) succeeds", error);
  CheckMode(ustar_pack, 0600, "PackEntries(kUstar) output is 0600");

  const std::string fast_pack = perms + "/fast.pack";
  error.clear();
  test_support::Check(backupproject::PackEntries(PackMethod::kFastUstar, entries,
                                                 fast_pack, &error),
                      "PackEntries(kFastUstar) succeeds", error);
  CheckMode(fast_pack, 0600, "PackEntries(kFastUstar) output is 0600");

  CheckNoResidue(perms, "no temp residue in the perms directory");
}

// ---- §24.5 运行中观察 ------------------------------------------------------

struct Observation {
  std::uint64_t scans_with_entries = 0;
  std::uint64_t directories = 0;
  std::uint64_t files = 0;
};

// 递归看一眼：目录必须 0700，普通文件必须 0600。lstat 失败说明条目刚好在
// 观察窗口里被清理掉了，这不算违规。
void ObservePath(const std::string& path, Observation* stats,
                 std::vector<std::string>* violations) {
  struct stat info;
  if (::lstat(path.c_str(), &info) != 0) {
    return;
  }
  const std::uint32_t mode = static_cast<std::uint32_t>(info.st_mode & 07777);
  if (S_ISDIR(info.st_mode)) {
    stats->directories += 1;
    if (mode != 0700) {
      violations->push_back(path + " is " + test_support::Octal(mode) +
                            ", expected 0700");
    }
    if (violations->size() > 8) {
      return;
    }
    for (const std::string& name : test_support::DirEntries(path)) {
      ObservePath(path + "/" + name, stats, violations);
    }
    return;
  }
  if (S_ISREG(info.st_mode)) {
    stats->files += 1;
    if (mode != 0600) {
      violations->push_back(path + " is " + test_support::Octal(mode) +
                            ", expected 0600");
    }
  }
}

void ObserveOnce(const std::string& directory, Observation* stats,
                 std::vector<std::string>* violations) {
  bool saw_any = false;
  for (const std::string& name : test_support::DirEntries(directory)) {
    if (!IsTempArtifact(name)) {
      continue;
    }
    saw_any = true;
    ObservePath(directory + "/" + name, stats, violations);
  }
  if (saw_any) {
    stats->scans_with_entries += 1;
  }
}

std::string BuildMegabyteBlock() {
  std::string block;
  block.reserve(1024 * 1024 + 128);
  std::uint32_t state = 0x12345678u;
  while (block.size() < 1024 * 1024) {
    state = state * 1103515245u + 12345u;
    block += "block-" + std::to_string(state % 100000) +
             "-payload-payload-payload\n";
  }
  block.resize(1024 * 1024);
  return block;
}

// 32 MiB 的 LZSS+AES 备份需要若干秒，父进程在这个窗口里循环扫描 archive
// parent，凡是看到的 .bp* 条目都断言权限。窗口期没撞上就只 Note。
void TestRuntimeWorkspaceObservation(const std::string& root) {
  test_support::Section("§24 运行中观察私有工作目录（尽力而为）");
  const std::string source = root + "/bigsrc";
  const std::string out = root + "/bigout";
  test_support::Check(test_support::Mkdir(source, 0755),
                      "big source dir created");
  test_support::Check(test_support::Mkdir(out, 0755), "big archive dir created");
  const std::string block = BuildMegabyteBlock();
  bool wrote_all = true;
  for (int index = 0; index < 32; ++index) {
    char name[32];
    std::snprintf(name, sizeof(name), "/part-%02d.bin", index);
    if (!test_support::WriteFile(source + name, block, 0644)) {
      wrote_all = false;
      break;
    }
  }
  test_support::Check(wrote_all, "32 MiB source tree created");
  if (!wrote_all) {
    return;
  }

  const std::string archive = out + "/big.bak";
  const pid_t child = ::fork();
  if (child == 0) {
    // 子进程只做这一件事。alarm 是兜底：万一备份真的挂住，宁可被杀也不要
    // 让测试进程永远等下去。
    ::alarm(600);
    std::string error;
    BackupOptions options;
    options.pack_method = PackMethod::kMyPack;
    options.compression_method = CompressionMethod::kLzssHuffman;
    options.encryption_method = EncryptionMethod::kAes256CtrHmacSha256;
    options.password = "observation password";
    const bool ok = backupproject::RunBackupPipeline(source, archive, Filter(),
                                                     options, &error);
    if (!ok) {
      std::fprintf(stderr, "observation backup failed: %s\n", error.c_str());
    }
    ::_exit(ok ? 0 : 1);
  }
  if (child < 0) {
    test_support::Check(false, "fork for the observation window");
    return;
  }

  Observation stats;
  std::vector<std::string> violations;
  int status = 0;
  bool finished = false;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(120);
  while (std::chrono::steady_clock::now() < deadline) {
    ObserveOnce(out, &stats, &violations);
    const pid_t done = ::waitpid(child, &status, WNOHANG);
    if (done == child) {
      finished = true;
      break;
    }
    if (done < 0 && errno != EINTR) {
      break;
    }
    struct timespec pause;
    pause.tv_sec = 0;
    pause.tv_nsec = 1000 * 1000;  // 1 ms：窗口期内尽量多扫几次
    ::nanosleep(&pause, nullptr);
  }
  if (!finished) {
    ::kill(child, SIGKILL);
    ::waitpid(child, &status, 0);
  }

  std::string detail;
  if (!finished) {
    detail = "killed after the 120 s deadline";
  } else if (WIFSIGNALED(status)) {
    detail = "child died from signal " + std::to_string(WTERMSIG(status));
  } else if (WIFEXITED(status)) {
    detail = "child exit status " + std::to_string(WEXITSTATUS(status));
  }
  test_support::Check(finished, "the observed backup finished in the window",
                      detail);
  test_support::Check(finished && WIFEXITED(status) &&
                          WEXITSTATUS(status) == 0,
                      "32 MiB LZSS+AES backup succeeds", detail);
  test_support::Check(violations.empty(),
                      "every observed workspace entry is 0700/0600",
                      JoinList(violations).substr(0, 400));

  if (stats.directories == 0 && stats.files == 0) {
    test_support::Note("没有在窗口期内观察到 workspace");
  } else {
    test_support::Note("观察到 " + std::to_string(stats.scans_with_entries) +
                       " 次扫描命中：目录 " +
                       std::to_string(stats.directories) + " 个，普通文件 " +
                       std::to_string(stats.files) + " 个");
  }

  if (finished && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
    CheckMode(archive, 0600, "the observed .bak is 0600 after publishing");
    CheckListing(out, {"big.bak"},
                 "the archive parent holds only the .bak after the run");
    CheckNoResidue(out, "no temp residue after the observed run");
  }
}

}  // namespace

int main() {
  std::printf("pipeline security / permissions / residue test\n");
  const std::string root = test_support::FreshDir("security");
  TestBackupPermissions(root);
  TestBackupFailureLeavesNothing(root);
  TestRestoreFailureLeavesNothing(root);
  TestUnitPermissions(root);
  TestRuntimeWorkspaceObservation(root);
  return test_support::Finish("pipeline_security_test");
}
