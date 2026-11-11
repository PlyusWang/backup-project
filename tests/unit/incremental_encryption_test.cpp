// incremental_encryption_test.cpp
//
// 完整基线与真实 delta 的加密闭环：兼容旧未加密归档、独立进程恢复、
// 错误密码、外层元数据认证、密码变更和缺失父快照。

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "backup_catalog.h"
#include "crypto.h"
#include "incremental_backup.h"
#include "incremental_delta.h"
#include "incremental_restore.h"
#include "snapshot_bundle.h"
#include "source_digest.h"
#include "test_support.h"

namespace bp = backupproject;

namespace {

std::string BinaryFixture(std::size_t size) {
  std::string bytes(size, '\0');
  std::uint32_t state = 0x582ab7u;
  for (char& byte : bytes) {
    state = state * 1664525u + 1013904223u;
    byte = static_cast<char>(state >> 24);
  }
  return bytes;
}

bool SameContents(const std::string& source, const std::string& destination) {
  for (const std::string name :
       {"bulk.bin", "small.txt", "中文.txt", "empty"}) {
    std::string actual;
    std::string expected;
    if (!bp::ContentDigestOfFile(source + "/" + name, &expected, nullptr) ||
        !bp::ContentDigestOfFile(destination + "/" + name, &actual, nullptr) ||
        actual != expected)
      return false;
  }
  return !test_support::Exists(destination + "/deleted.txt");
}

bool RestoreInNewProcess(const std::string& repository, const std::string& name,
                         const std::string& destination,
                         const std::string& password) {
  int secret_pipe[2];
  if (::pipe(secret_pipe) != 0) return false;
  const pid_t child = ::fork();
  if (child == 0) {
    ::close(secret_pipe[1]);
    if (::dup2(secret_pipe[0], STDIN_FILENO) < 0) ::_exit(120);
    ::close(secret_pipe[0]);
    ::execl("/proc/self/exe", "incremental_encryption_test", "--restore",
            repository.c_str(), name.c_str(), destination.c_str(),
            static_cast<char*>(nullptr));
    ::_exit(121);
  }
  ::close(secret_pipe[0]);
  if (child < 0) {
    ::close(secret_pipe[1]);
    return false;
  }
  const std::string secret_line = password + "\n";
  const bool sent =
      ::write(secret_pipe[1], secret_line.data(), secret_line.size()) ==
      static_cast<ssize_t>(secret_line.size());
  ::close(secret_pipe[1]);
  int status = 0;
  return ::waitpid(child, &status, 0) == child && sent && WIFEXITED(status) &&
         WEXITSTATUS(status) == 0;
}

bool RunIncremental(const std::string& source, const std::string& repository,
                    const std::string& name, const bp::BackupOptions& options,
                    bp::IncrementalOutcome* outcome, std::string* error,
                    const bp::ArchiveProgressCallback& progress = {}) {
  return bp::RunIncrementalBackup(
      source, repository, name, bp::RepositoryIdentity(repository),
      bp::Filter(), options, {}, {}, "", outcome, error, progress);
}

bool RunIncrementalFrom(const std::string& source,
                        const std::string& repository, const std::string& name,
                        const std::string& baseline,
                        const bp::BackupOptions& options,
                        bp::IncrementalOutcome* outcome, std::string* error) {
  return bp::RunIncrementalBackup(
      source, repository, name, bp::RepositoryIdentity(repository),
      bp::Filter(), options, {}, {}, baseline, outcome, error);
}

bool SamePackChainContents(const std::string& source,
                           const std::string& destination) {
  for (const char* name :
       {"bulk.bin", "small.txt", "empty", "中文.txt", "new.txt"}) {
    const std::string path(name);
    std::string expected;
    std::string actual;
    if (!bp::ContentDigestOfFile(source + "/" + path, &expected, nullptr) ||
        !bp::ContentDigestOfFile(destination + "/" + path, &actual, nullptr) ||
        expected != actual) {
      return false;
    }
  }
  return !test_support::Exists(destination + "/deleted.txt");
}

bool StandardTarLists(const std::string& path) {
  const pid_t child = ::fork();
  if (child < 0) return false;
  if (child == 0) {
    const int null_fd = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (null_fd >= 0) {
      ::dup2(null_fd, STDOUT_FILENO);
      ::dup2(null_fd, STDERR_FILENO);
      ::close(null_fd);
    }
    ::execlp("tar", "tar", "-tf", path.c_str(), static_cast<char*>(nullptr));
    ::_exit(127);
  }
  int status = 0;
  return ::waitpid(child, &status, 0) == child && WIFEXITED(status) &&
         WEXITSTATUS(status) == 0;
}

bool TamperIdentityMetadataTag(const std::string& path) {
  std::string identity;
  if (!test_support::ReadFile(path, &identity)) return false;
  const std::string key = "metadata_hmac=";
  const std::size_t tag = identity.find(key);
  if (tag == std::string::npos) return false;
  const std::size_t value = tag + key.size();
  if (value >= identity.size() || identity[value] == '\n') return false;
  identity[value] = identity[value] == 'a' ? 'b' : 'a';
  return test_support::WriteFile(path, identity, 0600);
}

bool RemoveIdentityMetadataTag(std::string* identity) {
  const std::size_t tag = identity->find("metadata_hmac=");
  if (tag == std::string::npos) return false;
  const std::size_t end = identity->find('\n', tag);
  if (end == std::string::npos) return false;
  identity->erase(tag, end - tag + 1);
  return true;
}

bool ConvertIdentityV3ToV2(const std::string& path) {
  std::string identity;
  if (!test_support::ReadFile(path, &identity) ||
      identity.compare(0, 9, "BPIDENT3\n") != 0 ||
      !RemoveIdentityMetadataTag(&identity)) {
    return false;
  }
  identity.replace(0, std::string("BPIDENT3\nversion=3\n").size(),
                   "BPIDENT2\nversion=2\n");
  return test_support::WriteFile(path, identity, 0600);
}

void TestPackChain(
    bp::PackMethod pack, bp::EncryptionMethod encryption,
    const std::string& pack_name, const std::string& encryption_name,
    bp::CompressionMethod compression = bp::CompressionMethod::kNone) {
  const std::string label = pack_name + "-" + encryption_name + "-" +
                            bp::CompressionMethodName(compression);
  test_support::Section("INC-PACK " + label);
  const std::string work = test_support::FreshDir("inc-pack-" + label);
  const std::string source = work + "/src";
  const std::string repository = work + "/repo";
  test_support::Check(test_support::Mkdir(source, 0755) &&
                          test_support::Mkdir(repository, 0755),
                      label + " isolated source and repository");
  test_support::Check(
      test_support::WriteFile(source + "/bulk.bin", BinaryFixture(1u << 20),
                              0644) &&
          test_support::WriteFile(source + "/small.txt", "F0", 0644) &&
          test_support::WriteFile(source + "/deleted.txt", "remove in D2",
                                  0644) &&
          test_support::WriteFile(source + "/empty", "", 0644),
      label + " F0 fixtures");
  bp::BackupOptions options;
  options.pack_method = pack;
  options.compression_method = compression;
  options.encryption_method = encryption;
  if (encryption != bp::EncryptionMethod::kNone) {
    options.password = "A09 test archive password";
  }
  std::string error;
  bp::IncrementalOutcome outcome;
  test_support::Check(
      bp::RunManagedFullBackup(source, repository, "base.bak",
                               bp::RepositoryIdentity(repository), bp::Filter(),
                               options, {}, {}, &outcome, &error),
      label + " F0 full baseline", error);
  test_support::WriteFile(source + "/small.txt", "D1 modified content", 0644);
  test_support::WriteFile(source + "/中文.txt", "D1 added content", 0644);
  test_support::Check(
      RunIncrementalFrom(source, repository, "delta1.bak", "base.bak", options,
                         &outcome, &error) &&
          outcome.kind == bp::IncrementalOutcome::Kind::kDelta,
      label + " D1 is a true incremental delta", error);
  test_support::WriteFile(source + "/small.txt", "D2 modified content", 0644);
  test_support::WriteFile(source + "/new.txt", "D2 added content", 0644);
  ::unlink((source + "/deleted.txt").c_str());
  test_support::Check(
      RunIncrementalFrom(source, repository, "delta2.bak", "delta1.bak",
                         options, &outcome, &error) &&
          outcome.kind == bp::IncrementalOutcome::Kind::kDelta,
      label + " D2 chains from D1 with add/modify/delete", error);
  bp::DeltaEnvelope first;
  bp::DeltaEnvelope second;
  test_support::Check(
      bp::ReadDeltaEnvelope(repository + "/delta1.bak", &first, &error) &&
          bp::ReadDeltaEnvelope(repository + "/delta2.bak", &second, &error) &&
          first.parent_file_name == "base.bak" &&
          second.parent_file_name == "delta1.bak" && second.added == 1 &&
          second.modified == 1 && second.removed == 1,
      label + " BKPINC envelope records exact parent and change counts", error);
  const std::string inner = work + "/delta2-inner.bak";
  bp::ContainerHeader header;
  test_support::Check(
      bp::ExtractDeltaPayload(repository + "/delta2.bak", inner, &error) &&
          bp::InspectContainerFile(inner, &header, &error) &&
          header.pack_method == static_cast<std::uint8_t>(pack) &&
          header.compression_method == static_cast<std::uint8_t>(compression),
      label + " inner container retains the selected pack", error);
  if ((pack == bp::PackMethod::kUstar || pack == bp::PackMethod::kFastUstar) &&
      encryption == bp::EncryptionMethod::kNone &&
      compression == bp::CompressionMethod::kNone) {
    std::string container;
    const bool have_container = test_support::ReadFile(inner, &container);
    const std::uint64_t payload_offset = bp::container_v2::kHeaderSize;
    const bool payload_in_bounds =
        have_container && header.compressed_size == header.packed_size &&
        header.payload_size == header.compressed_size &&
        payload_offset <= container.size() &&
        header.packed_size <= container.size() - payload_offset;
    const std::string packed_path = work + "/delta2.tar";
    const bool extracted =
        payload_in_bounds &&
        test_support::WriteFile(
            packed_path,
            container.substr(static_cast<std::size_t>(payload_offset),
                             static_cast<std::size_t>(header.packed_size)),
            0600);
    test_support::Check(extracted && StandardTarLists(packed_path),
                        label + " packed payload is readable by standard tar",
                        error);
  }
  test_support::Check(
      RunIncrementalFrom(source, repository, "unchanged.bak", "delta2.bak",
                         options, &outcome, &error) &&
          outcome.kind == bp::IncrementalOutcome::Kind::kNoChanges &&
          !test_support::Exists(repository + "/unchanged.bak"),
      label + " no-change creates no snapshot", error);
  const std::string restored = work + "/restored";
  bp::RestoreOptions restore;
  restore.password = options.password;
  test_support::Check(
      bp::RestoreSnapshotChain(repository, "delta2.bak", restored, restore,
                               nullptr, &error) &&
          SamePackChainContents(source, restored),
      label + " R2 restores D2 content and per-file SHA-256", error);
  if (pack == bp::PackMethod::kMyPack &&
      compression == bp::CompressionMethod::kZstd &&
      encryption == bp::EncryptionMethod::kAes256CtrHmacSha256) {
    for (const std::string name : {"base.bak", "delta1.bak", "delta2.bak"}) {
      const std::string identity_path =
          repository + "/" + name + ".identity";
      std::string original_identity;
      const bool saved =
          test_support::ReadFile(identity_path, &original_identity);
      const bool tampered = saved && TamperIdentityMetadataTag(identity_path);
      const std::string rejected = work + "/tampered-" + name;
      error.clear();
      const bool restore_rejected =
          !bp::RestoreSnapshotChain(repository, "delta2.bak", rejected,
                                    restore, nullptr, &error) &&
          !test_support::Exists(rejected);
      test_support::Check(tampered && restore_rejected,
                          label + " restore rejects tampered " + name +
                              " identity before publishing output",
                          error);
      test_support::Check(
          saved && test_support::WriteFile(identity_path, original_identity,
                                           0600),
          label + " restores original " + name + " identity fixture");
    }

    std::string base_identity;
    const std::string base_identity_path = repository + "/base.bak.identity";
    test_support::ReadFile(base_identity_path, &base_identity);
    const std::string malformed_identity_path =
        work + "/malformed-base-identity";
    std::string malformed_identity = base_identity;
    const bool malformed_written =
        RemoveIdentityMetadataTag(&malformed_identity) &&
        test_support::WriteFile(base_identity_path, malformed_identity, 0600);
    error.clear();
    const bool malformed_rejected =
        malformed_written &&
        !bp::RestoreSnapshotChain(repository, "base.bak",
                                  malformed_identity_path, restore, nullptr,
                                  &error) &&
        !test_support::Exists(malformed_identity_path);
    test_support::Check(malformed_rejected,
                        label + " restore rejects malformed BPIDENT3 before "
                                "publishing output",
                        error);
    test_support::Check(
        test_support::WriteFile(base_identity_path, base_identity, 0600),
        label + " restores BPIDENT3 fixture after malformed-field test");

    const std::string no_base_identity = work + "/base-identity.saved";
    const bool moved_base_identity =
        ::rename(base_identity_path.c_str(), no_base_identity.c_str()) == 0;
    const std::string legacy_full_restore = work + "/missing-identity-full";
    error.clear();
    const bool full_without_identity =
        moved_base_identity &&
        bp::RestoreSnapshotChain(repository, "base.bak", legacy_full_restore,
                                 restore, nullptr, &error);
    const bool base_identity_restored =
        !moved_base_identity ||
        ::rename(no_base_identity.c_str(), base_identity_path.c_str()) == 0;
    std::string base_small;
    test_support::Check(
        full_without_identity && base_identity_restored &&
            test_support::ReadFile(legacy_full_restore + "/small.txt",
                                   &base_small) &&
            base_small == "F0",
        label + " standalone full restore retains missing-identity "
                "compatibility",
        error);

    const std::string delta2_identity = repository + "/delta2.bak.identity";
    const std::string no_delta2_identity = work + "/delta2-identity.saved";
    const bool moved_delta2_identity =
        ::rename(delta2_identity.c_str(), no_delta2_identity.c_str()) == 0;
    const std::string missing_target_identity_restore =
        work + "/missing-target-identity";
    error.clear();
    const bool target_delta_without_identity =
        moved_delta2_identity &&
        bp::RestoreSnapshotChain(repository, "delta2.bak",
                                 missing_target_identity_restore, restore,
                                 nullptr, &error) &&
        SamePackChainContents(source, missing_target_identity_restore);
    const bool delta2_identity_restored =
        !moved_delta2_identity ||
        ::rename(no_delta2_identity.c_str(), delta2_identity.c_str()) == 0;
    test_support::Check(
        target_delta_without_identity && delta2_identity_restored,
        label + " target delta without identity retains existing restore "
                "behavior",
        error);

    test_support::Check(
        ConvertIdentityV3ToV2(base_identity_path),
        label + " converts an encrypted full snapshot fixture to BPIDENT2");
    const std::string legacy_v2_restore = work + "/legacy-v2-full";
    error.clear();
    const bool restored_legacy_v2 =
        bp::RestoreSnapshotChain(repository, "base.bak", legacy_v2_restore,
                                 restore, nullptr, &error);
    std::string legacy_small;
    test_support::Check(
        restored_legacy_v2 &&
            test_support::ReadFile(legacy_v2_restore + "/small.txt",
                                   &legacy_small) &&
            legacy_small == "F0",
        label + " encrypted BPIDENT2 full snapshot remains restorable", error);
  }
  if (encryption != bp::EncryptionMethod::kNone) {
    bp::RestoreOptions wrong_password;
    wrong_password.password = "incorrect password";
    const std::string rejected = work + "/wrong-password";
    test_support::Check(
        !bp::RestoreSnapshotChain(repository, "delta2.bak", rejected,
                                  wrong_password, nullptr, &error) &&
            !test_support::Exists(rejected),
        label + " wrong password rejects without publishing a destination",
        error);
  }
}

// 仅修改本测试刚生成的合成归档；重算公开摘要后认证仍必须拒绝元数据改变。
bool ChangeFixtureEnvelope(const std::string& path) {
  bp::DeltaEnvelope envelope;
  std::string bytes;
  if (!bp::ReadDeltaEnvelope(path, &envelope, nullptr) ||
      !test_support::ReadFile(path, &bytes))
    return false;
  const std::string original = bp::SerializeDeltaEnvelope(envelope);
  envelope.created_unix_seconds += 1;
  envelope.snapshot_id = bp::ComputeDeltaSnapshotId(envelope);
  const std::string changed = bp::SerializeDeltaEnvelope(envelope);
  if (original.size() != changed.size()) return false;
  bytes.replace(bp::kDeltaFixedHeaderSize, original.size(), changed);
  return test_support::WriteFile(path, bytes, 0600);
}

void TestRoundTrip(bp::EncryptionMethod encryption, const std::string& label) {
  test_support::Section("INC-ENC " + label);
  const std::string work =
      test_support::FreshDir("incremental-encryption-" + label);
  const std::string source = work + "/src";
  const std::string repository = work + "/repo";
  test_support::Check(test_support::Mkdir(source, 0755) &&
                          test_support::Mkdir(repository, 0755),
                      label + " isolated directories");
  test_support::Check(
      test_support::WriteFile(source + "/bulk.bin", BinaryFixture(1u << 20),
                              0644) &&
          test_support::WriteFile(source + "/small.txt", "before", 0644) &&
          test_support::WriteFile(source + "/deleted.txt", "remove me", 0644) &&
          test_support::WriteFile(source + "/empty", "", 0644),
      label + " synthetic binary, empty and small fixtures");
  std::string error;
  std::string random_bytes;
  if (!bp::crypto::RandomBytes(96, &random_bytes, &error)) {
    test_support::Check(false, label + " runtime secret generation", error);
    return;
  }
  const std::string first_password = bp::crypto::ToHex(
      reinterpret_cast<const unsigned char*>(random_bytes.data()), 32);
  const std::string second_password = bp::crypto::ToHex(
      reinterpret_cast<const unsigned char*>(random_bytes.data() + 32), 32);
  const std::string wrong_password = bp::crypto::ToHex(
      reinterpret_cast<const unsigned char*>(random_bytes.data() + 64), 32);
  bp::BackupOptions options;
  options.encryption_method = encryption;
  if (encryption != bp::EncryptionMethod::kNone)
    options.password = first_password;
  bp::IncrementalOutcome outcome;
  test_support::Check(
      bp::RunManagedFullBackup(source, repository, "base.bak",
                               bp::RepositoryIdentity(repository), bp::Filter(),
                               options, {}, {}, &outcome, &error),
      label + " full backup creates a reusable baseline", error);
  bp::SnapshotIdentity identity;
  test_support::Check(
      bp::LoadVerifiedSnapshotIdentity(repository, "base.bak", &identity,
                                       nullptr, &error) &&
          identity.sidecars_verified,
      label + " full manifest and identity are bound to actual bytes", error);

  test_support::WriteFile(source + "/small.txt", "after: a small edit", 0644);
  test_support::WriteFile(source + "/中文.txt", "new file in Chinese path",
                          0644);
  ::unlink((source + "/deleted.txt").c_str());
  std::vector<bp::ArchiveProgress> progress;
  test_support::Check(
      RunIncremental(source, repository, "delta.bak", options, &outcome, &error,
                     [&progress](const bp::ArchiveProgress& value) {
                       progress.push_back(value);
                     }) &&
          outcome.kind == bp::IncrementalOutcome::Kind::kDelta,
      label + " full then small edit creates an actual delta", error);
  bool writing_start = false;
  bool writing_end = false;
  for (const auto& value : progress) {
    if (value.phase != "writing" || value.bytes_total == 0) continue;
    writing_start = writing_start || value.bytes_done == 0;
    writing_end = writing_end || value.bytes_done == value.bytes_total;
  }
  test_support::Check(writing_start && writing_end,
                      label + " delta forwards actual payload byte progress");
  struct stat full_info;
  struct stat delta_info;
  test_support::Check(
      ::stat((repository + "/base.bak").c_str(), &full_info) == 0 &&
          ::stat((repository + "/delta.bak").c_str(), &delta_info) == 0 &&
          delta_info.st_size < full_info.st_size / 8,
      label + " physical delta excludes unchanged 1 MiB payload");
  bp::DeltaEnvelope envelope;
  test_support::Check(
      bp::ReadDeltaEnvelope(repository + "/delta.bak", &envelope, &error) &&
          envelope.format_version ==
              (encryption == bp::EncryptionMethod::kNone ? 1u : 2u),
      label + " format version matches authentication requirements", error);
  test_support::Check(bp::VerifyDeltaAuthentication(repository + "/delta.bak",
                                                    options.password, &error),
                      label + " envelope authentication succeeds", error);
  const std::string destination = work + "/restored";
  test_support::Check(
      RestoreInNewProcess(repository, "delta.bak", destination,
                          options.password) &&
          SameContents(source, destination),
      label +
          " fresh process restores added/changed/deleted files with SHA-256");
  std::vector<bp::BackupRecord> listed;
  bp::BackupCatalog catalog;
  bool truthful = false;
  if (catalog.List(repository, &listed, &error)) {
    for (const auto& record : listed) {
      if (record.file_name == "delta.bak") {
        truthful = record.incremental_delta && record.recognized_archive &&
                   record.encryption_method == encryption &&
                   record.password_required ==
                       (encryption != bp::EncryptionMethod::kNone);
      }
    }
  }
  test_support::Check(truthful,
                      label + " catalog reports true type and encryption");
  test_support::Check(
      RunIncremental(source, repository, "unchanged.bak", options, &outcome,
                     &error) &&
          outcome.kind == bp::IncrementalOutcome::Kind::kNoChanges &&
          !test_support::Exists(repository + "/unchanged.bak"),
      label + " unchanged content produces no new snapshot", error);

  if (encryption == bp::EncryptionMethod::kNone) {
    std::string existing;
    std::string after_attempt;
    test_support::ReadFile(repository + "/delta.bak", &existing);
    test_support::WriteFile(source + "/small.txt", "later fixture content",
                            0644);
    test_support::Check(
        !RunIncremental(source, repository, "delta.bak", options, &outcome,
                        &error) &&
            test_support::ReadFile(repository + "/delta.bak", &after_attempt) &&
            after_attempt == existing,
        label + " publishing an existing delta name preserves the original",
        error);
  }

  if (encryption != bp::EncryptionMethod::kNone) {
    const std::string downloaded = work + "/downloaded";
    test_support::Mkdir(downloaded, 0700);
    bp::net::SnapshotBundleInfo bundle;
    const bool bundled =
        bp::net::BuildSnapshotBundle(repository, "base.bak",
                                     work + "/base.bundle", &bundle, &error) &&
        bp::net::BuildSnapshotBundle(repository, "delta.bak",
                                     work + "/delta.bundle", &bundle, &error);
    std::string orphan_identity;
    test_support::ReadFile(repository + "/delta.bak.identity",
                           &orphan_identity);
    const bool extracted =
        bundled &&
        bp::net::ExtractSnapshotBundle(work + "/base.bundle", downloaded,
                                       &bundle, &error) &&
        test_support::WriteFile(downloaded + "/delta.bak.identity",
                                orphan_identity, 0600) &&
        bp::net::ExtractSnapshotBundle(work + "/delta.bundle", downloaded,
                                       &bundle, &error);
    test_support::Check(
        extracted &&
            RestoreInNewProcess(downloaded, "delta.bak",
                                work + "/bundle-restored", options.password) &&
            SameContents(source, work + "/bundle-restored"),
        label +
            " BPSNAP transports v2/v3 material and recovers v3 orphan identity",
        error);
    bp::RestoreOptions wrong;
    wrong.password = wrong_password;
    test_support::Check(!bp::RestoreSnapshotChain(repository, "delta.bak",
                                                  work + "/wrong-password",
                                                  wrong, nullptr, &error) &&
                            !test_support::Exists(work + "/wrong-password"),
                        label + " wrong password publishes no destination",
                        error);
    options.password = second_password;
    test_support::Check(
        RunIncremental(source, repository, "changed-password.bak", options,
                       &outcome, &error) &&
            outcome.kind == bp::IncrementalOutcome::Kind::kFullBaseline &&
            !outcome.baseline_reason.empty(),
        label +
            " changed password starts full baseline even without source "
            "changes",
        error);
    test_support::Check(
        RestoreInNewProcess(repository, "changed-password.bak",
                            work + "/new-password", options.password) &&
            SameContents(source, work + "/new-password"),
        label + " new password restores without the old password");
    std::string identity_bytes;
    const std::string identity_path =
        repository + "/changed-password.bak.identity";
    test_support::ReadFile(identity_path, &identity_bytes);
    const std::size_t tag = identity_bytes.find("metadata_hmac=");
    if (tag != std::string::npos) {
      const std::size_t value = tag + std::string("metadata_hmac=").size();
      identity_bytes[value] = identity_bytes[value] == 'a' ? 'b' : 'a';
    }
    test_support::Check(
        tag != std::string::npos &&
            test_support::WriteFile(identity_path, identity_bytes, 0600) &&
            !bp::RunIncrementalBackup(
                source, repository, "metadata-explicit.bak",
                bp::RepositoryIdentity(repository), bp::Filter(), options, {},
                {}, "changed-password.bak", &outcome, &error) &&
            !test_support::Exists(repository + "/metadata-explicit.bak"),
        label +
            " explicitly selected untrusted metadata refuses without output",
        error);
    error.clear();
    test_support::Check(
        bp::RunIncrementalBackup(source, repository, "metadata-rebuilt.bak",
                                 bp::RepositoryIdentity(repository),
                                 bp::Filter(), options, {}, {}, "", &outcome,
                                 &error) &&
            outcome.kind == bp::IncrementalOutcome::Kind::kFullBaseline,
        label + " automatic mode cannot skip changes with untrusted metadata",
        error);
    options.password = first_password;
    std::string original_delta;
    test_support::ReadFile(repository + "/delta.bak", &original_delta);
    test_support::Check(
        ChangeFixtureEnvelope(repository + "/delta.bak") &&
            !bp::VerifyDeltaAuthentication(repository + "/delta.bak",
                                           options.password, &error),
        label + " metadata change fails MAC even after public ID recomputation",
        error);
    test_support::WriteFile(repository + "/delta.bak", original_delta, 0600);
  }
  test_support::Check(::rename((repository + "/base.bak").c_str(),
                               (repository + "/base.off").c_str()) == 0,
                      label + " isolate missing-parent case");
  bp::RestoreOptions restore;
  restore.password = options.password;
  test_support::Check(
      !bp::RestoreSnapshotChain(repository, "delta.bak",
                                work + "/missing-parent", restore, nullptr,
                                &error) &&
          !test_support::Exists(work + "/missing-parent"),
      label + " missing parent refuses restore without publishing output",
      error);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 5 && std::string(argv[1]) == "--restore") {
    bp::RestoreOptions options;
    if (!std::getline(std::cin, options.password)) return 2;
    std::string error;
    const bool restored = bp::RestoreSnapshotChain(argv[2], argv[3], argv[4],
                                                   options, nullptr, &error);
    if (!restored) std::fprintf(stderr, "restore failed: %s\n", error.c_str());
    return restored ? 0 : 1;
  }
  const bool restore_auth_only =
      argc == 2 && std::string(argv[1]) == "--restore-auth-only";
  const bool zstd_only = argc == 2 && std::string(argv[1]) == "--zstd";
  if (restore_auth_only) {
    TestPackChain(bp::PackMethod::kMyPack,
                  bp::EncryptionMethod::kAes256CtrHmacSha256,
                  "mypack", "aes", bp::CompressionMethod::kZstd);
    return test_support::Finish("incremental_encryption_test");
  }
  if (!zstd_only) {
    TestRoundTrip(bp::EncryptionMethod::kNone, "none");
    TestRoundTrip(bp::EncryptionMethod::kAes256CtrHmacSha256, "aes");
    TestRoundTrip(bp::EncryptionMethod::kDesCbcHmacSha256, "des");
  }
  for (const auto& pack : {std::pair<bp::PackMethod, const char*>{
                               bp::PackMethod::kMyPack, "mypack"},
                           {bp::PackMethod::kUstar, "ustar"},
                           {bp::PackMethod::kFastUstar, "fast-ustar"}}) {
    if (!zstd_only) {
      TestPackChain(pack.first, bp::EncryptionMethod::kNone, pack.second,
                    "none");
      TestPackChain(pack.first, bp::EncryptionMethod::kAes256CtrHmacSha256,
                    pack.second, "aes");
      TestPackChain(pack.first, bp::EncryptionMethod::kDesCbcHmacSha256,
                    pack.second, "des");
    }
    TestPackChain(pack.first, bp::EncryptionMethod::kNone, pack.second, "none",
                  bp::CompressionMethod::kZstd);
    TestPackChain(pack.first, bp::EncryptionMethod::kAes256CtrHmacSha256,
                  pack.second, "aes", bp::CompressionMethod::kZstd);
    TestPackChain(pack.first, bp::EncryptionMethod::kDesCbcHmacSha256,
                  pack.second, "des", bp::CompressionMethod::kZstd);
  }
  return test_support::Finish("incremental_encryption_test");
}
