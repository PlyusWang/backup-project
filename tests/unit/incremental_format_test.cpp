// incremental_format_test.cpp
//
// PR #18 的 delta 磁盘格式（BKPINC1）专项测试。
//
// 要钉住的东西：
//   * 信封自描述、可校验：snapshot id 是自身内容的摘要，改一个字节就对不上；
//   * 旧 reader 不可能把 delta 当成完整归档（顶层 magic 不同）；
//   * payload 真的复用现有流水线：抽出来就是一份标准 v2 container，
//     现有 restore 能原样读它；
//   * 坏字节一律拒绝：截断、magic 不对、长度不符、摘要不符、USTAR 被拒；
//   * 只有变化部分进 payload（不是"把 Full 改个名字"）。

#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <vector>

#include "archive_pipeline.h"
#include "container_format.h"
#include "incremental_delta.h"
#include "source_digest.h"
#include "source_manifest.h"
#include "test_support.h"

namespace bp = backupproject;

namespace {

bp::DeltaEnvelope MakeEnvelope(const std::string& parent_name,
                               const std::string& parent_digest,
                               const std::string& generation) {
  bp::DeltaEnvelope envelope;
  envelope.parent_file_name = parent_name;
  envelope.parent_snapshot_id = bp::ContentDigestOfBytes("parent-snapshot");
  envelope.parent_manifest_digest = parent_digest;
  envelope.base_generation_id = generation;
  envelope.source_identity =
      bp::SourceIdentityDigest("/source", "repository-identity");
  envelope.filter_identity = bp::FilterIdentityDigest({}, {});
  envelope.strategy_identity = bp::StrategyIdentityDigest(
      bp::PackMethod::kMyPack, bp::CompressionMethod::kNone,
      bp::EncryptionMethod::kNone);
  envelope.current_manifest_digest = bp::ContentDigestOfBytes("current");
  // payload 摘要在真正写出时由 WriteDeltaFile 覆盖；这里给一个合法值，
  // 好让"纯粹的信封往返"也能通过解析（解析器要求每个 digest 字段形状合法）。
  envelope.payload_sha256 = bp::ContentDigestOfBytes("payload-placeholder");
  envelope.created_unix_seconds = 1700000000;
  // snapshot id 由信封自身内容决定；写侧会重算一次并覆盖它。
  envelope.snapshot_id = bp::ComputeDeltaSnapshotId(envelope);
  return envelope;
}

// payload 的条目表与备份流水线同约定：第一条是源根目录。
bp::ArchiveEntry RootEntry(const std::string& source_path) {
  bp::ArchiveEntry entry;
  entry.archive_path = ".";
  entry.source_path = source_path;
  entry.type = bp::EntryType::kDirectory;
  entry.mode = 0755;
  entry.uid = 1000;
  entry.gid = 1000;
  return entry;
}

bp::ArchiveEntry FileEntry(const std::string& source_path,
                           const std::string& archive_path,
                           const std::string& content) {
  bp::ArchiveEntry entry;
  entry.archive_path = archive_path;
  entry.source_path = source_path;
  entry.type = bp::EntryType::kRegularFile;
  entry.mode = 0644;
  entry.uid = 1000;
  entry.gid = 1000;
  entry.size = content.size();
  // 打包器会核对"读到的正文与 entry 记录的 size/mtime 是否一致"，所以测试
  // 造条目时必须从真实文件取，而不是随手写 0。
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

}  // namespace

int main() {
  const std::string work = test_support::FreshDir("inc-format");
  const std::string source = work + "/src";
  test_support::Mkdir(source, 0755);
  test_support::WriteFile(source + "/a.txt", "alpha", 0644);

  // ---- F1：信封往返 + 自校验 ----
  test_support::Section("INC-F 1. delta 信封：规范化往返与 snapshot id");
  {
    bp::DeltaEnvelope envelope =
        MakeEnvelope("base.bak", std::string(64, 'a'), std::string(64, 'b'));
    envelope.added = 2;
    envelope.modified = 1;
    envelope.metadata_changed = 3;
    envelope.removed = 2;
    envelope.tombstones = {"gone/one.txt", "gone/two.txt"};
    envelope.affected_directories = {".", "dir"};

    const std::string text = bp::SerializeDeltaEnvelope(envelope);
    bp::DeltaEnvelope parsed;
    std::string error;
    test_support::Check(bp::ParseDeltaEnvelope(text, &parsed, &error),
                        "INC-F T1 信封解析成功", error);
    test_support::Check(
        parsed.tombstones == envelope.tombstones &&
            parsed.affected_directories == envelope.affected_directories,
        "INC-F T1 tombstone 与目录列表往返一致");
    test_support::Check(parsed.added == 2 && parsed.modified == 1 &&
                            parsed.metadata_changed == 3 && parsed.removed == 2,
                        "INC-F T1 计数往返一致");
    test_support::Check(bp::SerializeDeltaEnvelope(parsed) == text,
                        "INC-F T1 序列化是确定的（同一信封两次逐字节相同）");

    // snapshot id 是"去掉该字段之后"的摘要，所以改任何一个字段都会变。
    const std::string id = bp::ComputeDeltaSnapshotId(envelope);
    test_support::Check(bp::IsContentDigest(id),
                        "INC-F T1 snapshot id 是合法摘要");
    bp::DeltaEnvelope changed = envelope;
    changed.added = 3;
    test_support::Check(bp::ComputeDeltaSnapshotId(changed) != id,
                        "INC-F T1 内容变化会改变 snapshot id");
  }

  // ---- F2：坏信封必须拒绝 ----
  test_support::Section("INC-F 2. 坏信封一律拒绝");
  {
    bp::DeltaEnvelope envelope =
        MakeEnvelope("base.bak", std::string(64, 'a'), std::string(64, 'b'));
    const std::string good = bp::SerializeDeltaEnvelope(envelope);
    struct Case {
      std::string label;
      std::string text;
    };
    std::vector<Case> cases;
    cases.push_back({"头行不对", "BPDELTA2\n" + good.substr(9)});
    cases.push_back(
        {"缺键", good.substr(0, good.find("added=")) +
                     good.substr(good.find("\n", good.find("added=")) + 1)});
    cases.push_back({"重复键", good + "added=1\n"});
    cases.push_back({"未知键", good + "unknown_key=1\n"});
    cases.push_back(
        {"digest 形状不对",
         good.substr(0, good.find("payload_sha256=")) + "payload_sha256=zz\n"});
    cases.push_back(
        {"removed 与 tombstone 数不符",
         good.substr(0, good.find("removed=")) + "removed=5\n" +
             good.substr(good.find("\n", good.find("removed=")) + 1)});
    cases.push_back(
        {"自称是自己的孩子",
         good.substr(0, good.find("parent_snapshot_id=")) +
             "parent_snapshot_id=" + std::string(64, 'a') + "\n" +
             "snapshot_id=" + std::string(64, 'a') + "\n" +
             good.substr(good.find("\n", good.find("parent_manifest_digest=")) +
                         1)});
    for (const Case& item : cases) {
      bp::DeltaEnvelope parsed;
      std::string error;
      const bool ok = bp::ParseDeltaEnvelope(item.text, &parsed, &error);
      test_support::Check(!ok, "INC-F T2 拒绝：" + item.label,
                          ok ? "(竟然解析成功)" : error);
    }
  }

  // ---- F3：写出 delta，payload 真的是 v2 container ----
  test_support::Section("INC-F 3. 写出 delta：payload 复用 v2 流水线");
  {
    const std::string delta = work + "/delta1.inc";
    bp::DeltaEnvelope envelope =
        MakeEnvelope("base.bak", std::string(64, 'a'), std::string(64, 'b'));
    envelope.added = 1;
    envelope.removed = 1;
    envelope.tombstones = {"old.txt"};

    std::vector<bp::ArchiveEntry> entries;
    entries.push_back(RootEntry(source));
    entries.push_back(FileEntry(source + "/a.txt", "a.txt", "alpha"));
    bp::BackupOptions options;
    std::string error;
    test_support::Check(
        bp::WriteDeltaFile(delta, envelope, entries, options, &error),
        "INC-F T3 写出 delta 成功", error);
    test_support::Check(test_support::Exists(delta), "INC-F T3 delta 文件存在");

    bp::SnapshotFileKind kind = bp::ClassifySnapshotFile(delta, &error);
    test_support::Check(kind == bp::SnapshotFileKind::kDelta,
                        "INC-F T3 分类为 delta（不是完整归档）");

    bp::DeltaEnvelope read_back;
    test_support::Check(bp::ReadDeltaEnvelope(delta, &read_back, &error),
                        "INC-F T3 读回信封成功", error);
    test_support::Check(read_back.tombstones == envelope.tombstones &&
                            read_back.removed == 1 &&
                            read_back.parent_file_name == "base.bak",
                        "INC-F T3 读回的信封与写入一致");
    test_support::Check(bp::IsContentDigest(read_back.payload_sha256),
                        "INC-F T3 信封带 payload 摘要");
    test_support::Check(bp::VerifyDeltaPayload(delta, &error),
                        "INC-F T3 payload 校验通过", error);

    // 抽出来的 payload 必须是一份标准 v2 container，现有 restore 直接能读。
    const std::string inner = work + "/inner.bak";
    test_support::Check(bp::ExtractDeltaPayload(delta, inner, &error),
                        "INC-F T3 抽出内层 container", error);
    bp::ContainerHeader header;
    test_support::Check(bp::InspectContainerFile(inner, &header, &error),
                        "INC-F T3 内层是合法的 v2 container", error);
    test_support::Check(header.entry_count == 2,
                        "INC-F T3 内层只有源根与变化的那一个条目",
                        std::to_string(header.entry_count));

    const std::string restored = work + "/restored";
    bp::RestoreReport report;
    test_support::Check(
        bp::RunRestorePipeline(inner, restored, bp::RestoreOptions{}, &report,
                               &error),
        "INC-F T3 内层可以被现有 restore 直接恢复", error);
    std::string content;
    test_support::Check(test_support::ReadFile(restored + "/a.txt", &content) &&
                            content == "alpha",
                        "INC-F T3 变化条目的内容恢复正确", content);
  }

  // ---- F4：坏 delta 文件必须拒绝 ----
  test_support::Section("INC-F 4. 坏 delta 文件一律拒绝");
  {
    const std::string delta = work + "/delta2.inc";
    bp::DeltaEnvelope envelope =
        MakeEnvelope("base.bak", std::string(64, 'a'), std::string(64, 'b'));
    envelope.added = 1;
    std::vector<bp::ArchiveEntry> entries;
    entries.push_back(RootEntry(source));
    entries.push_back(FileEntry(source + "/a.txt", "a.txt", "alpha"));
    bp::BackupOptions options;
    std::string error;
    test_support::Check(
        bp::WriteDeltaFile(delta, envelope, entries, options, &error),
        "INC-F T4 写出用于破坏的 delta", error);

    std::string bytes;
    test_support::Check(test_support::ReadFile(delta, &bytes),
                        "INC-F T4 读入 delta 字节");

    // 1) magic 被改
    std::string wrong_magic = bytes;
    wrong_magic[0] = 'X';
    const std::string path_magic = work + "/bad-magic.inc";
    test_support::Check(test_support::WriteFile(path_magic, wrong_magic, 0644),
                        "INC-F T4 写出坏 magic 的 delta");
    bp::DeltaEnvelope parsed;
    test_support::Check(!bp::ReadDeltaEnvelope(path_magic, &parsed, &error),
                        "INC-F T4 拒绝：magic 不对");

    // 2) 截断
    const std::string path_trunc = work + "/truncated.inc";
    test_support::Check(
        test_support::WriteFile(path_trunc, bytes.substr(0, bytes.size() / 2),
                                0644),
        "INC-F T4 写出截断的 delta");
    test_support::Check(!bp::ReadDeltaEnvelope(path_trunc, &parsed, &error),
                        "INC-F T4 拒绝：截断（文件长度与声明不符）");

    // 3) 篡改信封里的一个计数字段 -> snapshot id 不再匹配
    const std::string needle = "added=1\n";
    const std::size_t at = bytes.find(needle);
    test_support::Check(at != std::string::npos, "INC-F T4 找到 added 字段");
    std::string tampered = bytes;
    tampered.replace(at, needle.size(), "added=9\n");
    const std::string path_tamper = work + "/tampered.inc";
    test_support::Check(test_support::WriteFile(path_tamper, tampered, 0644),
                        "INC-F T4 写出篡改信封的 delta");
    test_support::Check(!bp::ReadDeltaEnvelope(path_tamper, &parsed, &error),
                        "INC-F T4 拒绝：信封被改过（snapshot id 对不上）");

    // 4) payload 被改 -> payload 摘要对不上（信封本身仍然自洽）
    std::string payload_tampered = bytes;
    payload_tampered[payload_tampered.size() - 1] ^= 0x5A;
    const std::string path_payload = work + "/payload-tampered.inc";
    test_support::Check(
        test_support::WriteFile(path_payload, payload_tampered, 0644),
        "INC-F T4 写出 payload 被改的 delta");
    test_support::Check(bp::ReadDeltaEnvelope(path_payload, &parsed, &error),
                        "INC-F T4 信封本身仍然读得出来");
    test_support::Check(!bp::VerifyDeltaPayload(path_payload, &error),
                        "INC-F T4 拒绝：payload 摘要不符");

    // 5) 完整归档不是 delta
    const std::string full = work + "/full.bak";
    bp::BackupOptions full_options;
    test_support::Check(
        bp::RunBackupPipelineFromEntries(entries, full, full_options, &error),
        "INC-F T4 写出一份完整归档", error);
    test_support::Check(
        bp::ClassifySnapshotFile(full, &error) ==
            bp::SnapshotFileKind::kContainer,
        "INC-F T4 完整归档被分类为 container（不会和 delta 混淆）");
    test_support::Check(!bp::ReadDeltaEnvelope(full, &parsed, &error),
                        "INC-F T4 拒绝：把完整归档当 delta 读");
  }

  // ---- F5：只允许 MyPack（第一版）----
  test_support::Section("INC-F 5. 第一版只支持 MyPack，其余明确拒绝");
  {
    const std::string delta = work + "/delta3.inc";
    bp::DeltaEnvelope envelope =
        MakeEnvelope("base.bak", std::string(64, 'a'), std::string(64, 'b'));
    std::vector<bp::ArchiveEntry> entries;
    entries.push_back(RootEntry(source));
    entries.push_back(FileEntry(source + "/a.txt", "a.txt", "alpha"));
    bp::BackupOptions options;
    options.pack_method = bp::PackMethod::kUstar;
    std::string error;
    test_support::Check(
        !bp::WriteDeltaFile(delta, envelope, entries, options, &error),
        "INC-F T5 USTAR 被明确拒绝，绝不静默降级");
    test_support::Check(error.find("MyPack") != std::string::npos,
                        "INC-F T5 拒绝理由说清了为什么", error);
    test_support::Check(!test_support::Exists(delta),
                        "INC-F T5 被拒绝时不留半成品文件");
  }

  // ---- F6：delta 只装变化部分 ----
  test_support::Section("INC-F 6. delta 不是改名的 Full");
  {
    const std::string big = work + "/big";
    test_support::Mkdir(big, 0755);
    for (int index = 0; index < 60; ++index) {
      test_support::WriteFile(
          big + "/f" + std::to_string(index) + ".dat",
          std::string(4096, static_cast<char>('a' + (index % 26))), 0644);
    }
    std::vector<bp::ArchiveEntry> all_entries;
    std::string error;
    // 用同一个扫描器造出"全部条目"，再只挑一条放进 delta。
    std::vector<bp::ManifestEntry> manifest;
    test_support::Check(
        bp::BuildStrongSourceManifest(big, nullptr, &manifest, &error),
        "INC-F T6 扫描大树成功", error);
    // 整份 manifest 都转过来（含源根与目录）：payload 的条目表约定与备份一致。
    for (const bp::ManifestEntry& item : manifest) {
      bp::ArchiveEntry entry;
      entry.archive_path = item.archive_path;
      entry.source_path = item.source_path;
      entry.type = item.type;
      entry.mode = item.mode;
      entry.uid = item.uid;
      entry.gid = item.gid;
      entry.size = item.size;
      entry.mtime_sec = item.mtime_sec;
      entry.mtime_nsec = item.mtime_nsec;
      entry.link_target = item.link_target;
      entry.dev_major = item.dev_major;
      entry.dev_minor = item.dev_minor;
      all_entries.push_back(entry);
    }
    test_support::Check(all_entries.size() == manifest.size() &&
                            all_entries.front().archive_path == ".",
                        "INC-F T6 条目表含源根，共 " +
                            std::to_string(all_entries.size()) + " 条",
                        std::to_string(all_entries.size()));

    const std::string full = work + "/big-full.bak";
    bp::BackupOptions options;
    test_support::Check(
        bp::RunBackupPipelineFromEntries(all_entries, full, options, &error),
        "INC-F T6 完整备份成功", error);

    // 单条变化的 delta：源根（约定要求）+ 一个普通文件。
    std::vector<bp::ArchiveEntry> one;
    one.push_back(all_entries.front());
    for (const bp::ArchiveEntry& entry : all_entries) {
      if (entry.type == bp::EntryType::kRegularFile) {
        one.push_back(entry);
        break;
      }
    }
    test_support::Check(one.size() == 2 && one.front().archive_path == "." &&
                            one.front().type == bp::EntryType::kDirectory,
                        "INC-F T6 单条变化的 delta 含源根与一个文件");
    const std::string delta = work + "/big-delta.inc";
    bp::DeltaEnvelope envelope = MakeEnvelope(
        "big-full.bak", std::string(64, 'a'), std::string(64, 'b'));
    envelope.modified = 1;
    test_support::Check(
        bp::WriteDeltaFile(delta, envelope, one, options, &error),
        "INC-F T6 单条变化的 delta 写出成功", error);

    struct stat full_info;
    struct stat delta_info;
    test_support::Check(test_support::StatOf(full, &full_info) &&
                            test_support::StatOf(delta, &delta_info),
                        "INC-F T6 取到两份文件的大小");
    // 60 个 4 KiB 文件 vs 1 个：delta 必须小一个数量级，否则就是"改名 Full"。
    test_support::Check(delta_info.st_size * 10 < full_info.st_size,
                        "INC-F T6 判别：delta 明显不含未变化条目的正文",
                        "delta=" + std::to_string(delta_info.st_size) +
                            " full=" + std::to_string(full_info.st_size));
  }


  // ---- F7：不可信路径字段的边界（PR #18 closure）----
  test_support::Section("INC-F 7. parent / tombstone：不可信字段一律 fail closed");
  {
    // PATH-01 / 02 / 03：parent_file_name 必须是"与 Catalog 管理的备份文件名
    // 同一条边界"的单组件名字。它来自不可信信封，格式层就要判死。
    const std::vector<std::string> bad_parents = {
        "../../evil.bak",     // PATH-01 目录穿越
        "../x",               // 上一级 + 没有扩展名
        "a/b.bak",            // PATH-02 多组件
        "a\\b.bak",           // PATH-02 反斜杠
        ".", "..", "", "plain", "x.txt"};
    for (const std::string& parent : bad_parents) {
      bp::DeltaEnvelope envelope =
          MakeEnvelope("base.bak", std::string(64, 'a'), std::string(64, 'b'));
      envelope.parent_file_name = parent;
      envelope.snapshot_id = bp::ComputeDeltaSnapshotId(envelope);
      bp::DeltaEnvelope parsed;
      std::string error;
      test_support::Check(!bp::ParseDeltaEnvelope(
                              bp::SerializeDeltaEnvelope(envelope), &parsed,
                              &error),
                          "INC-C PATH parent 被拒绝: '" + parent + "'");
    }
    // ".bak" 这种"只有扩展名"的名字 Catalog 也接受（它确实是合法的单组件
    // 普通文件名），所以增量这一侧必须接受同一件事——边界一致比"看起来更严"
    // 重要：两处不一致会出现"Catalog 认得、delta 读不了"的文件。
    const std::vector<std::string> good_parents = {
        "base.bak", "20260928_040000.bak", "中文 名字.bak", "a#b%.bak", ".bak"};
    for (const std::string& parent : good_parents) {
      bp::DeltaEnvelope envelope =
          MakeEnvelope(parent, std::string(64, 'a'), std::string(64, 'b'));
      bp::DeltaEnvelope parsed;
      std::string error;
      test_support::Check(bp::ParseDeltaEnvelope(
                              bp::SerializeDeltaEnvelope(envelope), &parsed,
                              &error) &&
                              parsed.parent_file_name == parent,
                          "INC-C PATH parent 被接受: '" + parent + "'", error);
    }

    // PATH-04 / 05 / 06：tombstone 是归档内相对路径，而且永远不能删源根。
    const std::vector<std::string> bad_tombstones = {
        "../victim",          // PATH-04 目录穿越
        ".",                  // PATH-05 源根
        "a/../../victim", "a/../b", "/abs/path", "a\\b", "a//b", "a/./b",
        "C:/windows", "dir/", ""};
    for (const std::string& tombstone : bad_tombstones) {
      bp::DeltaEnvelope envelope =
          MakeEnvelope("base.bak", std::string(64, 'a'), std::string(64, 'b'));
      envelope.tombstones = {tombstone};
      envelope.removed = 1;
      envelope.snapshot_id = bp::ComputeDeltaSnapshotId(envelope);
      bp::DeltaEnvelope parsed;
      std::string error;
      test_support::Check(!bp::ParseDeltaEnvelope(
                              bp::SerializeDeltaEnvelope(envelope), &parsed,
                              &error),
                          "INC-C PATH tombstone 被拒绝: '" + tombstone +
                              "'");
    }
    const std::vector<std::string> good_tombstones = {
        "gone.txt", "dir/sub/file.txt", "中文 目录/文件.txt"};
    for (const std::string& tombstone : good_tombstones) {
      bp::DeltaEnvelope envelope =
          MakeEnvelope("base.bak", std::string(64, 'a'), std::string(64, 'b'));
      envelope.tombstones = {tombstone};
      envelope.removed = 1;
      bp::DeltaEnvelope parsed;
      std::string error;
      test_support::Check(bp::ParseDeltaEnvelope(
                              bp::SerializeDeltaEnvelope(envelope), &parsed,
                              &error) &&
                              parsed.tombstones.size() == 1,
                          "INC-C PATH tombstone 被接受: '" + tombstone + "'",
                          error);
    }

    // 写侧与读侧过的是同一对校验：自己不产出自己随后拒绝的东西，
    // 而且失败时不留任何文件。
    bp::BackupOptions options;
    std::vector<bp::ArchiveEntry> entries;
    entries.push_back(RootEntry(source));
    const std::string escaped = work + "/bad-parent.inc";
    bp::DeltaEnvelope bad_parent = MakeEnvelope("../../evil.bak",
                                                std::string(64, 'a'),
                                                std::string(64, 'b'));
    std::string error;
    test_support::Check(!bp::WriteDeltaFile(escaped, bad_parent, entries,
                                            options, &error),
                        "INC-C PATH 写侧拒绝穿越型 parent");
    test_support::Check(!test_support::Exists(escaped),
                        "INC-C PATH 写侧拒绝时不留下文件");
    const std::string bad_tomb = work + "/bad-tomb.inc";
    bp::DeltaEnvelope bad_tombstone = MakeEnvelope("base.bak",
                                                   std::string(64, 'a'),
                                                   std::string(64, 'b'));
    bad_tombstone.tombstones = {"../victim"};
    bad_tombstone.removed = 1;
    error.clear();
    test_support::Check(!bp::WriteDeltaFile(bad_tomb, bad_tombstone, entries,
                                            options, &error),
                        "INC-C PATH 写侧拒绝穿越型 tombstone");
    test_support::Check(!test_support::Exists(bad_tomb),
                        "INC-C PATH 写侧拒绝时不留 tombstone 半成品");

    // 抽出内层 container 的 header：不需要密码，也不需要解压。
    const std::string good_delta = work + "/header.inc";
    bp::DeltaEnvelope good = MakeEnvelope("base.bak", std::string(64, 'a'),
                                          std::string(64, 'b'));
    error.clear();
    test_support::Check(bp::WriteDeltaFile(good_delta, good, entries, options,
                                           &error),
                        "INC-C PATH 正常 delta 可以写出", error);
    bp::ContainerHeader header;
    error.clear();
    test_support::Check(bp::InspectDeltaPayloadHeader(good_delta, &header, &error),
                        "INC-C PATH 内层 header 可读（无需密码）", error);
    test_support::Check(
        header.encryption_method ==
            static_cast<std::uint8_t>(bp::EncryptionMethod::kNone),
        "INC-C PATH 正常 delta 的内层容器未加密");
  }
  return test_support::Finish("incremental_format_test");
}
