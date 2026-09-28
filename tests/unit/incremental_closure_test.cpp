// incremental_closure_test.cpp
//
// PR #18 closure：正确性 / 安全收口的专项测试。
//
// 这里钉住的都是"旧 HEAD 会给出错误结果、本轮必须 fail closed"的东西：
//
//   * 选项组合的唯一答案来源（含"存得进去、跑起来才炸"的那两种组合）；
//   * 不可信 delta 路径在**应用**那一刻的边界（软链接祖先 / final symlink）；
//   * 父绑定三件事（名字、快照身份、manifest 摘要）与"换掉 .bak、留着旧副文件"；
//   * hardlink group 只改内容时的整组扩张；
//   * manifest 与真正写进 payload 的字节绑定（same-size + 原 mtime 的改写）；
//   * 依赖感知的手工删除与副文件生命周期；
//   * 深度上界的 off-by-one。
//
// 每个拒绝用例都额外断言"没有留下目标目录 / 没有留下任何半成品"。

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "archive_path.h"
#include "backup_catalog.h"
#include "backup_mode.h"
#include "incremental_backup.h"
#include "incremental_delta.h"
#include "incremental_restore.h"
#include "source_digest.h"
#include "source_manifest.h"
#include "test_support.h"
#include "tree_scanner.h"

namespace bp = backupproject;

namespace {

// 一次增量备份的便捷入口：MyPack + 不压缩 + 不加密（唯一受支持的增量组合）。
bool RunIncremental(const std::string& source, const std::string& repository,
                    const std::string& name, const std::string& baseline,
                    bp::IncrementalOutcome* outcome, std::string* error) {
  bp::Filter filter;
  bp::BackupOptions options;
  return bp::RunIncrementalBackup(source, repository, name,
                                  bp::RepositoryIdentity(repository), filter,
                                  options, std::vector<std::string>(),
                                  std::vector<std::string>(), baseline, outcome,
                                  error);
}

// 指定选项的那一条（用来证明核心自己也会拒绝非法组合）。
bool RunIncrementalWith(const std::string& source,
                        const std::string& repository,
                        const std::string& name,
                        const std::string& baseline,
                        const bp::BackupOptions& options,
                        bp::IncrementalOutcome* outcome, std::string* error) {
  bp::Filter filter;
  return bp::RunIncrementalBackup(source, repository, name,
                                  bp::RepositoryIdentity(repository), filter,
                                  options, std::vector<std::string>(),
                                  std::vector<std::string>(), baseline, outcome,
                                  error);
}

// 用产品自己的完整备份路径造一份"外来归档"：替换攻击需要一份**合法但不同**
// 的 .bak，坏文件本来就过不了识别这一关。
bool MakeForeignArchive(const std::string& source, const std::string& path,
                        std::string* error) {
  bp::Filter filter;
  std::vector<bp::ArchiveEntry> entries;
  if (!bp::ScanSourceTree(source, &filter, &entries, error)) return false;
  bp::BackupOptions options;
  return bp::RunBackupPipelineFromEntries(entries, path, options, error);
}

bp::ArchiveEntry RootEntry(const std::string& source_path) {
  bp::ArchiveEntry entry;
  entry.archive_path = ".";
  entry.source_path = source_path;
  entry.type = bp::EntryType::kDirectory;
  entry.mode = 0755;
  entry.uid = 1000;
  entry.gid = 1000;
  struct stat info;
  if (test_support::StatOf(source_path, &info)) {
    entry.mtime_sec = static_cast<std::int64_t>(info.st_mtim.tv_sec);
    entry.mtime_nsec = static_cast<std::uint32_t>(info.st_mtim.tv_nsec);
    entry.mode = info.st_mode & 07777;
    entry.uid = info.st_uid;
    entry.gid = info.st_gid;
  }
  return entry;
}

// 手工拼一份 delta 文件（BKPINC1 = 24 字节头 + 信封 + payload）。
// 给"必须造出具体坏形状"的用例用：父身份写错、payload 加密。
bool AssembleDelta(const std::string& out_path, const bp::DeltaEnvelope& input,
                   const std::string& payload_path, std::string* error) {
  std::string payload;
  if (!test_support::ReadFile(payload_path, &payload)) {
    if (error != nullptr) *error = "cannot read the payload";
    return false;
  }
  bp::DeltaEnvelope envelope = input;
  envelope.payload_sha256 = bp::ContentDigestOfBytes(payload);
  envelope.snapshot_id = bp::ComputeDeltaSnapshotId(envelope);
  const std::string text = bp::SerializeDeltaEnvelope(envelope);
  unsigned char header[bp::kDeltaFixedHeaderSize];
  for (std::size_t index = 0; index < sizeof(header); ++index) {
    header[index] = 0;
  }
  for (std::size_t index = 0; index < bp::kDeltaMagicSize; ++index) {
    header[index] = bp::kDeltaMagic[index];
  }
  header[8] = static_cast<unsigned char>(bp::kDeltaFormatVersion & 0xFF);
  header[9] = static_cast<unsigned char>((bp::kDeltaFormatVersion >> 8) & 0xFF);
  header[10] = static_cast<unsigned char>(bp::kDeltaFixedHeaderSize & 0xFF);
  header[11] =
      static_cast<unsigned char>((bp::kDeltaFixedHeaderSize >> 8) & 0xFF);
  const std::size_t envelope_len = text.size();
  for (int index = 0; index < 4; ++index) {
    header[12 + index] =
        static_cast<unsigned char>((envelope_len >> (8 * index)) & 0xFF);
  }
  for (int index = 0; index < 8; ++index) {
    header[16 + index] =
        static_cast<unsigned char>((payload.size() >> (8 * index)) & 0xFF);
  }
  std::string out(reinterpret_cast<const char*>(header), sizeof(header));
  out += text;
  out += payload;
  return test_support::WriteFile(out_path, out, 0640);
}

// 把一份 delta 的信封字段填成"真的接在 baseline_name 后面"。
bool ChainedEnvelope(const std::string& repository, const std::string& source,
                     const std::string& baseline_name,
                     bp::DeltaEnvelope* envelope, std::string* error) {
  bp::SnapshotIdentity parent;
  if (!bp::LoadSnapshotIdentity(repository, baseline_name, &parent, nullptr,
                                error)) {
    return false;
  }
  if (!parent.sidecars_verified) {
    if (error != nullptr) *error = parent.sidecar_diagnostic;
    return false;
  }
  bp::DeltaEnvelope value;
  value.parent_file_name = baseline_name;
  value.parent_snapshot_id = parent.snapshot_id;
  value.parent_manifest_digest = parent.manifest_digest;
  value.base_generation_id = parent.kind == bp::SnapshotFileKind::kContainer
                                 ? parent.snapshot_id
                                 : parent.base_generation_id;
  value.source_identity =
      bp::SourceIdentityDigest(source, bp::RepositoryIdentity(repository));
  value.filter_identity = bp::FilterIdentityDigest({}, {});
  value.strategy_identity = bp::StrategyIdentityDigest(
      bp::PackMethod::kMyPack, bp::CompressionMethod::kNone,
      bp::EncryptionMethod::kNone);
  value.current_manifest_digest = parent.manifest_digest;
  value.created_unix_seconds = 1700000000;
  *envelope = value;
  return true;
}

// 写出只带源根的 delta payload。
bool WriteRootOnlyDelta(const std::string& repository,
                        const std::string& source,
                        const std::string& delta_name,
                        const bp::DeltaEnvelope& envelope,
                        std::string* error) {
  std::vector<bp::ArchiveEntry> entries;
  entries.push_back(RootEntry(source));
  bp::BackupOptions options;
  return bp::WriteDeltaFile(repository + "/" + delta_name, envelope, entries,
                            options, error);
}

std::string SnapshotPath(const std::string& repository,
                         const std::string& name) {
  return repository + "/" + name;
}

// 测试接缝：在"强 manifest 建好、payload 还没读"这一刻改写源。
struct MutationHook {
  std::string path;
  std::string content;
  bool replace_symlink = false;
  std::int64_t mtime_sec = 0;
  std::uint32_t mtime_nsec = 0;
  bool fired = false;
};

void MutateSourceHook(void* context) {
  MutationHook* hook = static_cast<MutationHook*>(context);
  hook->fired = true;
  if (hook->replace_symlink) {
    ::unlink(hook->path.c_str());
    ::symlink(hook->content.c_str(), hook->path.c_str());
  } else {
    test_support::WriteFile(hook->path, hook->content, 0644);
  }
  test_support::SetTimes(hook->path, hook->mtime_sec, hook->mtime_nsec);
}

}  // namespace

int main() {
  // ---- C1：选项组合的唯一答案来源 ----
  test_support::Section("INC-C 1. 选项组合（trigger x strategy x pack x encryption）");
  {
    struct Case {
      bp::BackupTrigger trigger;
      bp::BackupStrategy strategy;
      bp::PackMethod pack;
      bp::CompressionMethod compression;
      bp::EncryptionMethod encryption;
      bool supported;
      std::string label;
    };
    const std::vector<Case> cases = {
        {bp::BackupTrigger::kManual, bp::BackupStrategy::kFull,
         bp::PackMethod::kMyPack, bp::CompressionMethod::kNone,
         bp::EncryptionMethod::kNone, true, "Manual + Full + mypack + none"},
        {bp::BackupTrigger::kManual, bp::BackupStrategy::kFull,
         bp::PackMethod::kUstar, bp::CompressionMethod::kLzssHuffman,
         bp::EncryptionMethod::kAes256CtrHmacSha256, true,
         "Manual + Full + ustar + lzss + aes 仍然支持"},
        {bp::BackupTrigger::kManual, bp::BackupStrategy::kIncremental,
         bp::PackMethod::kMyPack, bp::CompressionMethod::kNone,
         bp::EncryptionMethod::kNone, true,
         "Manual + Incremental + mypack + none"},
        {bp::BackupTrigger::kManual, bp::BackupStrategy::kIncremental,
         bp::PackMethod::kMyPack, bp::CompressionMethod::kHuffman,
         bp::EncryptionMethod::kNone, true,
         "Manual + Incremental + huffman 支持"},
        {bp::BackupTrigger::kManual, bp::BackupStrategy::kIncremental,
         bp::PackMethod::kMyPack, bp::CompressionMethod::kNone,
         bp::EncryptionMethod::kAes256CtrHmacSha256, false,
         "Manual + Incremental + aes 被拒绝"},
        {bp::BackupTrigger::kManual, bp::BackupStrategy::kIncremental,
         bp::PackMethod::kMyPack, bp::CompressionMethod::kNone,
         bp::EncryptionMethod::kDesCbcHmacSha256, false,
         "Manual + Incremental + des 被拒绝"},
        {bp::BackupTrigger::kManual, bp::BackupStrategy::kIncremental,
         bp::PackMethod::kUstar, bp::CompressionMethod::kNone,
         bp::EncryptionMethod::kNone, false,
         "Manual + Incremental + ustar 被拒绝"},
        {bp::BackupTrigger::kManual, bp::BackupStrategy::kIncremental,
         bp::PackMethod::kFastUstar, bp::CompressionMethod::kNone,
         bp::EncryptionMethod::kNone, false,
         "Manual + Incremental + fast-ustar 被拒绝"},
        {bp::BackupTrigger::kScheduled, bp::BackupStrategy::kFull,
         bp::PackMethod::kMyPack, bp::CompressionMethod::kNone,
         bp::EncryptionMethod::kNone, true, "Scheduled + Full + none"},
        {bp::BackupTrigger::kScheduled, bp::BackupStrategy::kFull,
         bp::PackMethod::kUstar, bp::CompressionMethod::kNone,
         bp::EncryptionMethod::kAes256CtrHmacSha256, false,
         "Scheduled + Full + aes 被拒绝"},
        {bp::BackupTrigger::kScheduled, bp::BackupStrategy::kIncremental,
         bp::PackMethod::kMyPack, bp::CompressionMethod::kNone,
         bp::EncryptionMethod::kNone, true,
         "Scheduled + Incremental + mypack + none"},
        {bp::BackupTrigger::kScheduled, bp::BackupStrategy::kIncremental,
         bp::PackMethod::kUstar, bp::CompressionMethod::kNone,
         bp::EncryptionMethod::kNone, false,
         "Scheduled + Incremental + ustar 被拒绝"},
        {bp::BackupTrigger::kScheduled, bp::BackupStrategy::kIncremental,
         bp::PackMethod::kMyPack, bp::CompressionMethod::kNone,
         bp::EncryptionMethod::kAes256CtrHmacSha256, false,
         "Scheduled + Incremental + aes 被拒绝"},
        {bp::BackupTrigger::kRealtime, bp::BackupStrategy::kFull,
         bp::PackMethod::kMyPack, bp::CompressionMethod::kNone,
         bp::EncryptionMethod::kNone, false, "Realtime + Full 被拒绝"},
        {bp::BackupTrigger::kRealtime, bp::BackupStrategy::kIncremental,
         bp::PackMethod::kMyPack, bp::CompressionMethod::kNone,
         bp::EncryptionMethod::kNone, false, "Realtime + Incremental 被拒绝"},
    };
    for (const Case& item : cases) {
      bp::BackupOptionCombination combination;
      combination.trigger = item.trigger;
      combination.strategy = item.strategy;
      combination.pack_method = item.pack;
      combination.compression_method = item.compression;
      combination.encryption_method = item.encryption;
      const bool supported = bp::IsSupportedBackupOptionCombination(combination);
      test_support::Check(supported == item.supported,
                          "INC-C OPT " + item.label);
      const std::string reason =
          bp::UnsupportedBackupOptionCombinationReason(combination);
      test_support::Check(supported ? reason.empty() : !reason.empty(),
                          "INC-C OPT 理由与结论一致: " + item.label);
    }
    // 共享表的拒绝理由必须点名真正的原因（GUI/CLI 直接显示原文）。
    bp::BackupOptionCombination encrypted;
    encrypted.strategy = bp::BackupStrategy::kIncremental;
    encrypted.encryption_method = bp::EncryptionMethod::kAes256CtrHmacSha256;
    test_support::Check(
        bp::UnsupportedBackupOptionCombinationReason(encrypted).find(
            "does not support encryption") != std::string::npos,
        "INC-C OPT 增量 + 加密的理由说明信封未被认证");
    bp::BackupOptionCombination ustar;
    ustar.strategy = bp::BackupStrategy::kIncremental;
    ustar.pack_method = bp::PackMethod::kUstar;
    test_support::Check(
        bp::UnsupportedBackupOptionCombinationReason(ustar).find("MyPack") !=
            std::string::npos,
        "INC-C OPT 增量 + USTAR 的理由点名 MyPack");
  }

  // ---- C2：核心自己必须拒绝非法组合（不能只靠前端）----
  test_support::Section("INC-C 2. 核心防御：非法组合在写盘之前失败");
  {
    const std::string work = test_support::FreshDir("inc-closure-core");
    const std::string source = work + "/src";
    const std::string repository = work + "/repo";
    test_support::Mkdir(source, 0755);
    test_support::WriteFile(source + "/a.txt", "alpha", 0644);
    test_support::Mkdir(repository, 0755);

    bp::IncrementalOutcome outcome;
    std::string error;
    bp::BackupOptions encrypted;
    encrypted.encryption_method = bp::EncryptionMethod::kAes256CtrHmacSha256;
    encrypted.password = "correct horse battery staple";
    test_support::Check(!RunIncrementalWith(source, repository, "enc.bak", "",
                                            encrypted, &outcome, &error),
                        "INC-C CORE 增量 + 加密被核心拒绝");
    test_support::Check(error.find("does not support encryption") !=
                            std::string::npos,
                        "INC-C CORE 拒绝理由说明信封未被认证", error);
    test_support::Check(!test_support::Exists(SnapshotPath(repository, "enc.bak")),
                        "INC-C CORE 拒绝后没有留下快照");

    error.clear();
    bp::BackupOptions ustar;
    ustar.pack_method = bp::PackMethod::kUstar;
    test_support::Check(!RunIncrementalWith(source, repository, "ustar.bak", "",
                                            ustar, &outcome, &error),
                        "INC-C CORE 增量 + USTAR 被核心拒绝");
    test_support::Check(!test_support::Exists(
                            SnapshotPath(repository, "ustar.bak")),
                        "INC-C CORE 拒绝后没有留下 USTAR 快照");
  }

  // ---- C3：delta 路径在应用那一刻的边界 ----
  test_support::Section("INC-C 3. tombstone 应用：软链接祖先与 final symlink");
  {
    const std::string work = test_support::FreshDir("inc-closure-paths");
    const std::string source = work + "/src";
    const std::string repository = work + "/repo";
    const std::string outside = work + "/outside";
    test_support::Mkdir(source, 0755);
    test_support::Mkdir(repository, 0755);
    test_support::Mkdir(outside, 0755);
    test_support::WriteFile(outside + "/victim", "outside data", 0644);
    test_support::CreateSymlink(outside, source + "/link");

    bp::IncrementalOutcome outcome;
    std::string error;
    test_support::Check(RunIncremental(source, repository, "base.bak", "",
                                       &outcome, &error),
                        "INC-C PATH 基线建立成功", error);

    // 合法的父绑定 + 一个穿越软链接祖先的 tombstone。
    bp::DeltaEnvelope envelope;
    test_support::Check(ChainedEnvelope(repository, source, "base.bak",
                                        &envelope, &error),
                        "INC-C PATH 取到父身份", error);
    envelope.tombstones = {"link/victim"};
    envelope.removed = 1;
    error.clear();
    test_support::Check(WriteRootOnlyDelta(repository, source, "evil.bak",
                                           envelope, &error),
                        "INC-C PATH 穿越型 tombstone 能写出来（它是坏数据）",
                        error);

    bp::RestoreOptions restore_options;
    bp::RestoreReport report;
    const std::string destination = work + "/restored";
    error.clear();
    test_support::Check(!bp::RestoreSnapshotChain(repository, "evil.bak",
                                                  destination, restore_options,
                                                  &report, &error),
                        "INC-C PATH-07 恢复拒绝穿过软链接祖先的 tombstone",
                        error);
    test_support::Check(!test_support::Exists(destination),
                        "INC-C PATH-07 拒绝时不留下目标目录");
    std::string victim;
    test_support::Check(test_support::ReadFile(outside + "/victim", &victim) &&
                            victim == "outside data",
                        "INC-C PATH-07 staging 之外的文件未被删除");
  }
  {
    const std::string work = test_support::FreshDir("inc-closure-final-link");
    const std::string source = work + "/src";
    const std::string repository = work + "/repo";
    test_support::Mkdir(source, 0755);
    test_support::Mkdir(source + "/d", 0755);
    test_support::Mkdir(repository, 0755);
    test_support::WriteFile(source + "/d/real.txt", "keep", 0644);
    test_support::CreateSymlink("real.txt", source + "/d/link.txt");

    bp::IncrementalOutcome outcome;
    std::string error;
    test_support::Check(RunIncremental(source, repository, "base.bak", "",
                                       &outcome, &error),
                        "INC-C PATH-08 基线建立成功", error);
    bp::DeltaEnvelope envelope;
    test_support::Check(ChainedEnvelope(repository, source, "base.bak",
                                        &envelope, &error),
                        "INC-C PATH-08 取到父身份", error);
    envelope.tombstones = {"d/link.txt"};
    envelope.removed = 1;
    error.clear();
    test_support::Check(WriteRootOnlyDelta(repository, source, "unlink.bak",
                                           envelope, &error),
                        "INC-C PATH-08 final symlink 的 tombstone 能写出来",
                        error);

    bp::RestoreOptions restore_options;
    bp::RestoreReport report;
    const std::string destination = work + "/restored";
    error.clear();
    test_support::Check(bp::RestoreSnapshotChain(repository, "unlink.bak",
                                                 destination, restore_options,
                                                 &report, &error),
                        "INC-C PATH-08 删除 final symlink 本身应当成功", error);
    test_support::Check(!test_support::Exists(destination + "/d/link.txt"),
                        "INC-C PATH-08 软链接被删掉");
    std::string kept;
    test_support::Check(
        test_support::ReadFile(destination + "/d/real.txt", &kept) &&
            kept == "keep",
        "INC-C PATH-08 软链接指向的目标没有被删");
  }
  {
    // PATH-09：正常的嵌套 tombstone 必须照常工作（不能为了安全把功能关掉）。
    const std::string work = test_support::FreshDir("inc-closure-nested");
    const std::string source = work + "/src";
    const std::string repository = work + "/repo";
    test_support::Mkdir(source, 0755);
    test_support::Mkdir(source + "/dir", 0755);
    test_support::Mkdir(source + "/dir/sub", 0755);
    test_support::Mkdir(repository, 0755);
    test_support::WriteFile(source + "/dir/sub/gone.txt", "bye", 0644);
    test_support::WriteFile(source + "/dir/sub/keep.txt", "hi", 0644);

    bp::IncrementalOutcome outcome;
    std::string error;
    test_support::Check(RunIncremental(source, repository, "base.bak", "",
                                       &outcome, &error),
                        "INC-C PATH-09 基线建立成功", error);
    bp::DeltaEnvelope envelope;
    test_support::Check(ChainedEnvelope(repository, source, "base.bak",
                                        &envelope, &error),
                        "INC-C PATH-09 取到父身份", error);
    envelope.tombstones = {"dir/sub/gone.txt"};
    envelope.removed = 1;
    error.clear();
    test_support::Check(WriteRootOnlyDelta(repository, source, "nested.bak",
                                           envelope, &error),
                        "INC-C PATH-09 嵌套 tombstone 能写出来", error);
    bp::RestoreOptions restore_options;
    bp::RestoreReport report;
    const std::string destination = work + "/restored";
    error.clear();
    test_support::Check(bp::RestoreSnapshotChain(repository, "nested.bak",
                                                 destination, restore_options,
                                                 &report, &error),
                        "INC-C PATH-09 嵌套 tombstone 正常生效", error);
    test_support::Check(!test_support::Exists(destination + "/dir/sub/gone.txt"),
                        "INC-C PATH-09 被删的文件确实不在");
    test_support::Check(test_support::Exists(destination + "/dir/sub/keep.txt"),
                        "INC-C PATH-09 同目录的其它文件仍在");
  }

  // ---- C4：父绑定三件事 ----
  test_support::Section("INC-C 4. 父绑定：名字 + 快照身份 + manifest 摘要");
  {
    const std::string work = test_support::FreshDir("inc-closure-binding");
    const std::string source = work + "/src";
    const std::string repository = work + "/repo";
    test_support::Mkdir(source, 0755);
    test_support::Mkdir(repository, 0755);
    test_support::WriteFile(source + "/a.txt", "alpha", 0644);

    bp::IncrementalOutcome outcome;
    std::string error;
    test_support::Check(RunIncremental(source, repository, "base.bak", "",
                                       &outcome, &error),
                        "INC-C BIND 基线建立成功", error);

    bp::DeltaEnvelope good;
    test_support::Check(ChainedEnvelope(repository, source, "base.bak", &good,
                                        &error),
                        "INC-C BIND 取到父身份", error);

    // 正确的一条必须能解析（否则后面的拒绝就没有判别力）。
    error.clear();
    test_support::Check(WriteRootOnlyDelta(repository, source, "ok.bak", good,
                                           &error),
                        "INC-C BIND 正确父绑定的 delta 能写出", error);
    bp::SnapshotChain chain;
    error.clear();
    test_support::Check(bp::ResolveSnapshotChain(repository, "ok.bak", &chain,
                                                 &error),
                        "INC-C BIND 正确父绑定的链可以解析", error);

    // BIND-01：parent_snapshot_id 对、parent_manifest_digest 错。
    bp::DeltaEnvelope wrong_manifest = good;
    wrong_manifest.parent_manifest_digest = bp::ContentDigestOfBytes("not-the-parent");
    error.clear();
    test_support::Check(WriteRootOnlyDelta(repository, source, "bad-manifest.bak",
                                           wrong_manifest, &error),
                        "INC-C BIND-01 错摘要的 delta 能写出来（它是坏数据）",
                        error);
    error.clear();
    test_support::Check(!bp::ResolveSnapshotChain(repository, "bad-manifest.bak",
                                                  &chain, &error),
                        "INC-C BIND-01 manifest 摘要不符 -> 拒绝", error);
    test_support::Check(error.find("manifest") != std::string::npos,
                        "INC-C BIND-01 拒绝理由点名 manifest 摘要", error);

    // BIND-02：manifest 摘要对、parent_snapshot_id 错。
    bp::DeltaEnvelope wrong_id = good;
    wrong_id.parent_snapshot_id = bp::ContentDigestOfBytes("not-the-parent-id");
    error.clear();
    test_support::Check(WriteRootOnlyDelta(repository, source, "bad-id.bak",
                                           wrong_id, &error),
                        "INC-C BIND-02 错身份的 delta 能写出来（它是坏数据）",
                        error);
    error.clear();
    test_support::Check(!bp::ResolveSnapshotChain(repository, "bad-id.bak",
                                                  &chain, &error),
                        "INC-C BIND-02 快照身份不符 -> 拒绝", error);
    test_support::Check(error.find("identity") != std::string::npos,
                        "INC-C BIND-02 拒绝理由点名身份", error);

    // BIND-03：换掉父 .bak（内容合法但不同），副文件原样留着。
    const std::string other_source = work + "/other";
    test_support::Mkdir(other_source, 0755);
    test_support::WriteFile(other_source + "/b.txt", "beta", 0644);
    const std::string foreign = work + "/foreign.bak";
    error.clear();
    test_support::Check(MakeForeignArchive(other_source, foreign, &error),
                        "INC-C BIND-03 造出替换用的合法归档", error);
    std::string foreign_bytes;
    test_support::Check(test_support::ReadFile(foreign, &foreign_bytes) &&
                            test_support::WriteFile(
                                SnapshotPath(repository, "base.bak"),
                                foreign_bytes, 0640),
                        "INC-C BIND-03 用另一份合法归档替换 base.bak");
    std::string reason;
    std::string baseline;
    test_support::Check(!bp::FindIncrementalBaseline(
                            repository, source, bp::RepositoryIdentity(repository),
                            bp::FilterIdentityDigest({}, {}),
                            bp::StrategyIdentityDigest(
                                bp::PackMethod::kMyPack,
                                bp::CompressionMethod::kNone,
                                bp::EncryptionMethod::kNone),
                            &baseline, &reason),
                        "INC-C BIND-03 被替换的 .bak 不再被当作基线");
    test_support::Check(reason.find("replaced") != std::string::npos ||
                            reason.find("not trustworthy") != std::string::npos,
                        "INC-C BIND-03 理由是「不信任」而不是「没有变化」", reason);

    // 源没有变，但基线不可信 -> 必须重建完整基线，而不是报 no-changes。
    bp::IncrementalOutcome replaced_outcome;
    error.clear();
    test_support::Check(RunIncremental(source, repository, "next.bak", "",
                                       &replaced_outcome, &error),
                        "INC-C BIND-03 替换之后仍能完成一次备份", error);
    test_support::Check(
        replaced_outcome.kind == bp::IncrementalOutcome::Kind::kFullBaseline,
        "INC-C BIND-03 判别：不信任的基线导致重建完整基线（不是 no-changes）");
  }
  {
    // BIND-04：父的副文件缺失 -> 链一律拒绝（少一条都不算合法链）。
    const std::string work = test_support::FreshDir("inc-closure-nosidecar");
    const std::string source = work + "/src";
    const std::string repository = work + "/repo";
    test_support::Mkdir(source, 0755);
    test_support::Mkdir(repository, 0755);
    test_support::WriteFile(source + "/a.txt", "alpha", 0644);
    bp::IncrementalOutcome outcome;
    std::string error;
    test_support::Check(RunIncremental(source, repository, "base.bak", "",
                                       &outcome, &error),
                        "INC-C BIND-04 基线建立成功", error);
    test_support::WriteFile(source + "/a.txt", "alpha2", 0644);
    error.clear();
    test_support::Check(RunIncremental(source, repository, "d1.bak", "base.bak",
                                       &outcome, &error) &&
                            outcome.kind == bp::IncrementalOutcome::Kind::kDelta,
                        "INC-C BIND-04 delta 建立成功", error);
    ::unlink(SnapshotPath(repository, "base.bak.manifest").c_str());
    ::unlink(SnapshotPath(repository, "base.bak.identity").c_str());
    bp::SnapshotChain chain;
    error.clear();
    test_support::Check(!bp::ResolveSnapshotChain(repository, "d1.bak", &chain,
                                                  &error),
                        "INC-C BIND-04 父没有副文件 -> 链被拒绝", error);
  }

  // ---- C5：hardlink group 只改内容 ----
  test_support::Section("INC-C 5. hardlink group：只改共享内容");
  {
    const std::string work = test_support::FreshDir("inc-closure-hardlink");
    const std::string source = work + "/src";
    const std::string repository = work + "/repo";
    test_support::Mkdir(source, 0755);
    test_support::Mkdir(repository, 0755);
    test_support::WriteFile(source + "/leader.txt", "AAAA", 0644);
    test_support::Check(test_support::CreateHardlink(source + "/leader.txt",
                                                     source + "/peer.txt"),
                        "INC-C HL 造出 hardlink 组");

    bp::IncrementalOutcome outcome;
    std::string error;
    test_support::Check(RunIncremental(source, repository, "base.bak", "",
                                       &outcome, &error),
                        "INC-C HL 基线建立成功", error);

    // 只改共享 inode 的内容：链接关系一个字都没动。
    test_support::WriteFile(source + "/leader.txt", "BBBB", 0644);
    error.clear();
    test_support::Check(RunIncremental(source, repository, "d1.bak", "base.bak",
                                       &outcome, &error) &&
                            outcome.kind == bp::IncrementalOutcome::Kind::kDelta,
                        "INC-C HL 内容变化写成 delta", error);

    bp::RestoreOptions restore_options;
    bp::RestoreReport report;
    const std::string destination = work + "/restored";
    error.clear();
    test_support::Check(bp::RestoreSnapshotChain(repository, "d1.bak",
                                                 destination, restore_options,
                                                 &report, &error),
                        "INC-C HL 恢复最新点成功", error);
    std::string leader;
    std::string peer;
    test_support::Check(test_support::ReadFile(destination + "/leader.txt",
                                               &leader) &&
                            leader == "BBBB",
                        "INC-C HL leader 拿到新内容", leader);
    test_support::Check(test_support::ReadFile(destination + "/peer.txt", &peer) &&
                            peer == "BBBB",
                        "INC-C HL 判别：peer 也拿到新内容（组没有被拆开）", peer);
    struct stat leader_info;
    struct stat peer_info;
    test_support::Check(
        test_support::StatOf(destination + "/leader.txt", &leader_info) &&
            test_support::StatOf(destination + "/peer.txt", &peer_info) &&
            leader_info.st_ino == peer_info.st_ino,
        "INC-C HL 两个成员仍然共享同一个 inode");
  }

  // ---- C6：manifest 与 payload 的字节绑定 ----
  test_support::Section("INC-C 6. manifest <-> payload：same-size 改写");
  {
    const std::string work = test_support::FreshDir("inc-closure-toctou");
    const std::string source = work + "/src";
    const std::string repository = work + "/repo";
    test_support::Mkdir(source, 0755);
    test_support::Mkdir(repository, 0755);
    test_support::WriteFile(source + "/a.txt", "0000000000", 0644);
    bp::IncrementalOutcome outcome;
    std::string error;
    test_support::Check(RunIncremental(source, repository, "base.bak", "",
                                       &outcome, &error),
                        "INC-C TOCTOU 基线建立成功", error);

    // 先做一次**真实**的变化（这样才会走到"写 payload"那一步），
    // 再在 manifest 建好之后把它改成同样大小、同样 mtime 的第三个版本。
    test_support::WriteFile(source + "/a.txt", "1111111111", 0644);
    MutationHook hook;
    hook.path = source + "/a.txt";
    hook.content = "2222222222";
    struct stat before;
    test_support::Check(test_support::StatOf(hook.path, &before),
                        "INC-C TOCTOU 取到改写前的 mtime");
    hook.mtime_sec = static_cast<std::int64_t>(before.st_mtim.tv_sec);
    hook.mtime_nsec = static_cast<std::uint32_t>(before.st_mtim.tv_nsec);

    bp::SetIncrementalManifestBuiltHookForTesting(MutateSourceHook, &hook);
    error.clear();
    const bool written =
        RunIncremental(source, repository, "d1.bak", "base.bak", &outcome,
                       &error);
    bp::SetIncrementalManifestBuiltHookForTesting(nullptr, nullptr);
    test_support::Check(hook.fired, "INC-C TOCTOU 测试接缝被调用");
    test_support::Check(!written,
                        "INC-C TOCTOU 判别：manifest 之后被改写 -> 整次失败",
                        error);
    test_support::Check(error.find("manifest digest") != std::string::npos,
                        "INC-C TOCTOU 拒绝理由点名内容摘要不符", error);
    test_support::Check(!test_support::Exists(SnapshotPath(repository, "d1.bak")),
                        "INC-C TOCTOU 失败时不发布快照");
    test_support::Check(
        !test_support::Exists(SnapshotPath(repository, "d1.bak.manifest")) &&
            !test_support::Exists(SnapshotPath(repository, "d1.bak.identity")),
        "INC-C TOCTOU 失败时不留下副文件");
    std::string baseline;
    std::string reason;
    test_support::Check(
        bp::FindIncrementalBaseline(
            repository, source, bp::RepositoryIdentity(repository),
            bp::FilterIdentityDigest({}, {}),
            bp::StrategyIdentityDigest(bp::PackMethod::kMyPack,
                                       bp::CompressionMethod::kNone,
                                       bp::EncryptionMethod::kNone),
            &baseline, &reason) &&
            baseline == "base.bak",
        "INC-C TOCTOU 失败之后父基线没有被改动", reason);
  }
  {
    // 软链接目标被改写：manifest 记的是目标字节的摘要。
    const std::string work = test_support::FreshDir("inc-closure-toctou-link");
    const std::string source = work + "/src";
    const std::string repository = work + "/repo";
    test_support::Mkdir(source, 0755);
    test_support::Mkdir(repository, 0755);
    test_support::WriteFile(source + "/target-a", "x", 0644);
    test_support::WriteFile(source + "/target-b", "x", 0644);
    test_support::CreateSymlink("target-a", source + "/link");
    bp::IncrementalOutcome outcome;
    std::string error;
    test_support::Check(RunIncremental(source, repository, "base.bak", "",
                                       &outcome, &error),
                        "INC-C TOCTOU-L 基线建立成功", error);

    // 同样先做一次真实变化（target-a -> target-c），再在 manifest 之后把它
    // 换成第三个等长目标。
    ::unlink((source + "/link").c_str());
    test_support::Check(test_support::CreateSymlink("target-c", source + "/link"),
                        "INC-C TOCTOU-L 先做一次真实的目标变化");
    MutationHook hook;
    hook.path = source + "/link";
    hook.content = "target-b";  // 与 "target-c" 等长
    hook.replace_symlink = true;
    bp::SetIncrementalManifestBuiltHookForTesting(MutateSourceHook, &hook);
    error.clear();
    const bool written = RunIncremental(source, repository, "d1.bak",
                                        "base.bak", &outcome, &error);
    bp::SetIncrementalManifestBuiltHookForTesting(nullptr, nullptr);
    test_support::Check(hook.fired, "INC-C TOCTOU-L 测试接缝被调用");
    test_support::Check(!written,
                        "INC-C TOCTOU-L 判别：软链接目标被改写 -> 整次失败",
                        error);
    test_support::Check(!test_support::Exists(SnapshotPath(repository, "d1.bak")),
                        "INC-C TOCTOU-L 失败时不发布快照");
  }

  // ---- C7：依赖感知的手工删除 + 副文件生命周期 ----
  test_support::Section("INC-C 7. 依赖感知删除与副文件生命周期");
  {
    const std::string work = test_support::FreshDir("inc-closure-delete");
    const std::string source = work + "/src";
    const std::string repository = work + "/repo";
    test_support::Mkdir(source, 0755);
    test_support::Mkdir(repository, 0755);
    test_support::WriteFile(source + "/a.txt", "one", 0644);
    bp::IncrementalOutcome outcome;
    std::string error;
    test_support::Check(RunIncremental(source, repository, "f0.bak", "",
                                       &outcome, &error),
                        "INC-C DEL 基线 f0 建立成功", error);
    test_support::WriteFile(source + "/a.txt", "two", 0644);
    error.clear();
    test_support::Check(RunIncremental(source, repository, "d1.bak", "f0.bak",
                                       &outcome, &error) &&
                            outcome.kind == bp::IncrementalOutcome::Kind::kDelta,
                        "INC-C DEL d1 建立成功", error);
    test_support::WriteFile(source + "/a.txt", "three", 0644);
    error.clear();
    test_support::Check(RunIncremental(source, repository, "d2.bak", "d1.bak",
                                       &outcome, &error) &&
                            outcome.kind == bp::IncrementalOutcome::Kind::kDelta,
                        "INC-C DEL d2 建立成功", error);

    bp::BackupCatalog catalog;
    error.clear();
    test_support::Check(!catalog.Delete(repository, "f0.bak", &error),
                        "INC-C DEL-01 删除有后代的基线被拒绝", error);
    test_support::Check(test_support::Exists(SnapshotPath(repository, "f0.bak")),
                        "INC-C DEL-01 拒绝时文件仍在");
    error.clear();
    test_support::Check(!catalog.Delete(repository, "d1.bak", &error),
                        "INC-C DEL-02 删除中间节点被拒绝", error);
    error.clear();
    test_support::Check(catalog.Delete(repository, "d2.bak", &error),
                        "INC-C DEL-03 删除叶子成功", error);
    test_support::Check(
        !test_support::Exists(SnapshotPath(repository, "d2.bak")) &&
            !test_support::Exists(SnapshotPath(repository, "d2.bak.manifest")) &&
            !test_support::Exists(SnapshotPath(repository, "d2.bak.identity")),
        "INC-C DEL-03 副文件跟随叶子一起被清理");
    error.clear();
    test_support::Check(catalog.Delete(repository, "d1.bak", &error),
                        "INC-C DEL-04 叶子删掉之后中间节点变成叶子，可以删", error);
    error.clear();
    test_support::Check(catalog.Delete(repository, "f0.bak", &error),
                        "INC-C DEL-05 链尾最后也能删掉", error);
    test_support::Check(test_support::DirEntries(repository).empty(),
                        "INC-C DEL 仓库已空（没有留下任何副文件）");

    // 孤儿副文件：只检测、不清理；显式清理才动手。
    test_support::WriteFile(repository + "/ghost.bak.manifest", "x", 0640);
    test_support::WriteFile(repository + "/ghost.bak.identity", "y", 0640);
    std::vector<std::string> orphans;
    error.clear();
    test_support::Check(bp::FindOrphanSidecars(repository, &orphans, &error) &&
                            orphans.size() == 2,
                        "INC-C DEL-06 孤儿副文件被检测出来", error);
    test_support::Check(test_support::Exists(repository + "/ghost.bak.manifest"),
                        "INC-C DEL-06 检测本身不删除任何东西");
    std::vector<std::string> removed;
    std::vector<std::string> diagnostics;
    error.clear();
    test_support::Check(bp::CleanOrphanSidecars(repository, &removed,
                                                &diagnostics, &error) &&
                            removed.size() == 2 && diagnostics.empty(),
                        "INC-C DEL-07 显式清理删掉孤儿副文件", error);
    test_support::Check(!test_support::Exists(repository + "/ghost.bak.manifest"),
                        "INC-C DEL-07 孤儿副文件确实没了");
  }

  // ---- C8：深度上界（64 允许 / 65 拒绝）----
  test_support::Section("INC-C 8. 依赖链深度上界");
  {
    const std::string work = test_support::FreshDir("inc-closure-depth");
    const std::string source = work + "/src";
    const std::string repository = work + "/repo";
    test_support::Mkdir(source, 0755);
    test_support::Mkdir(repository, 0755);
    test_support::WriteFile(source + "/a.txt", "0", 0644);
    bp::IncrementalOutcome outcome;
    std::string error;
    test_support::Check(RunIncremental(source, repository, "s000.bak", "",
                                       &outcome, &error),
                        "INC-C DEPTH 基线建立成功", error);
    std::string previous = "s000.bak";
    std::size_t made = 0;
    for (int index = 1; index <= 64; ++index) {
      char name[32];
      std::snprintf(name, sizeof(name), "s%03d.bak", index);
      char content[16];
      std::snprintf(content, sizeof(content), "%d", index);
      test_support::WriteFile(source + "/a.txt", content, 0644);
      error.clear();
      if (!RunIncremental(source, repository, name, previous, &outcome,
                          &error) ||
          outcome.kind != bp::IncrementalOutcome::Kind::kDelta) {
        test_support::Check(false, "INC-C DEPTH 第 " + std::to_string(index) +
                                       " 个 delta 建立成功",
                            error);
        break;
      }
      previous = name;
      made += 1;
    }
    test_support::Check(made == 64, "INC-C DEPTH 建出 64 个 delta 的链");
    std::size_t depth = 0;
    error.clear();
    test_support::Check(bp::SnapshotDeltaDepth(repository, previous, &depth,
                                               &error) &&
                            depth == 64,
                        "INC-C DEPTH 深度读数是 64", error);
    bp::RestoreOptions restore_options;
    bp::RestoreReport report;
    const std::string destination = work + "/restored";
    error.clear();
    test_support::Check(bp::RestoreSnapshotChain(repository, previous,
                                                 destination, restore_options,
                                                 &report, &error),
                        "INC-C DEPTH 64 个 delta 的链可以恢复", error);

    // 写侧：再挂一个就会越界 -> 不产出不可恢复的快照，而是重建完整基线。
    test_support::WriteFile(source + "/a.txt", "65", 0644);
    bp::IncrementalOutcome beyond;
    error.clear();
    test_support::Check(RunIncremental(source, repository, "s065.bak", previous,
                                       &beyond, &error),
                        "INC-C DEPTH 越界时仍然完成一次备份", error);
    test_support::Check(
        beyond.kind == bp::IncrementalOutcome::Kind::kFullBaseline &&
            beyond.baseline_reason.find("maximum depth") != std::string::npos,
        "INC-C DEPTH 判别：越界时重建完整基线而不是写 delta",
        beyond.baseline_reason);

    // 读侧：手工在 64 深的链上再挂一个 delta -> 解析阶段就必须拒绝。
    bp::DeltaEnvelope envelope;
    error.clear();
    test_support::Check(ChainedEnvelope(repository, source, previous, &envelope,
                                        &error),
                        "INC-C DEPTH 取到最深处那一份的父身份", error);
    envelope.tombstones = {"a.txt"};
    envelope.removed = 1;
    error.clear();
    test_support::Check(WriteRootOnlyDelta(repository, source, "too-deep.bak",
                                           envelope, &error),
                        "INC-C DEPTH 65 深的链能拼出来（它是坏数据）", error);
    bp::SnapshotChain chain;
    error.clear();
    test_support::Check(!bp::ResolveSnapshotChain(repository, "too-deep.bak",
                                                  &chain, &error),
                        "INC-C DEPTH 65 个 delta 的链在解析阶段被拒绝", error);
    test_support::Check(error.find("deeper than") != std::string::npos,
                        "INC-C DEPTH 拒绝理由点名深度上界", error);
  }


  // ---- C9：读侧同样拒绝加密的 delta ----
  test_support::Section("INC-C 9. 加密的 delta：读侧也拒绝");
  {
    const std::string work = test_support::FreshDir("inc-closure-encrypted");
    const std::string source = work + "/src";
    const std::string repository = work + "/repo";
    test_support::Mkdir(source, 0755);
    test_support::Mkdir(repository, 0755);
    test_support::WriteFile(source + "/a.txt", "alpha", 0644);
    bp::IncrementalOutcome outcome;
    std::string error;
    test_support::Check(RunIncremental(source, repository, "base.bak", "",
                                       &outcome, &error),
                        "INC-C ENC 基线建立成功", error);

    // 用产品自己的流水线造一份**加密**的内层 container：老版本会把这样的
    // payload 包进 BKPINC1 信封里，本轮起创建路径拒绝它。
    bp::Filter filter;
    std::vector<bp::ArchiveEntry> entries;
    error.clear();
    test_support::Check(bp::ScanSourceTree(source, &filter, &entries, &error),
                        "INC-C ENC 扫描源树", error);
    bp::BackupOptions encrypted_options;
    encrypted_options.encryption_method =
        bp::EncryptionMethod::kAes256CtrHmacSha256;
    encrypted_options.password = "correct horse battery staple";
    const std::string payload = work + "/payload.bak";
    error.clear();
    test_support::Check(
        bp::RunBackupPipelineFromEntries(entries, payload, encrypted_options,
                                         &error),
        "INC-C ENC 造出加密的内层 container", error);

    bp::DeltaEnvelope envelope;
    error.clear();
    test_support::Check(ChainedEnvelope(repository, source, "base.bak",
                                        &envelope, &error),
                        "INC-C ENC 取到父身份", error);
    error.clear();
    test_support::Check(AssembleDelta(repository + "/encrypted.bak", envelope,
                                      payload, &error),
                        "INC-C ENC 拼出加密 delta（老版本的产物形状）", error);

    bp::ContainerHeader header;
    error.clear();
    test_support::Check(bp::InspectDeltaPayloadHeader(
                            repository + "/encrypted.bak", &header, &error) &&
                            header.encryption_method ==
                                static_cast<std::uint8_t>(
                                    bp::EncryptionMethod::kAes256CtrHmacSha256),
                        "INC-C ENC 内层 header 可读且报告加密", error);

    bp::RestoreOptions restore_options;
    restore_options.password = "correct horse battery staple";
    bp::RestoreReport report;
    const std::string destination = work + "/restored";
    error.clear();
    test_support::Check(!bp::RestoreSnapshotChain(repository, "encrypted.bak",
                                                  destination, restore_options,
                                                  &report, &error),
                        "INC-C ENC 判别：加密 delta 不再被恢复路径接受", error);
    test_support::Check(error.find("does not support encryption") !=
                            std::string::npos,
                        "INC-C ENC 拒绝理由说明信封未被认证", error);
    test_support::Check(!test_support::Exists(destination),
                        "INC-C ENC 拒绝时不留下目标目录");
  }

  return test_support::Finish("incremental_closure_test");
}
