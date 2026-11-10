// A03: candidate inspection and actual continuation must use the same checks.
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

#include "backup_catalog.h"
#include "incremental_backup.h"
#include "incremental_restore.h"
#include "test_support.h"

namespace bp = backupproject;
namespace ts = test_support;
using Availability = bp::BaselineAvailability;

namespace {
const bp::IncrementalBaselineCandidate* Find(
    const std::vector<bp::IncrementalBaselineCandidate>& candidates,
    const std::string& name) {
  const auto found = std::find_if(
      candidates.begin(), candidates.end(),
      [&](const auto& candidate) { return candidate.file_name == name; });
  return found == candidates.end() ? nullptr : &*found;
}

bool Status(const std::vector<bp::IncrementalBaselineCandidate>& candidates,
            const std::string& name, Availability expected) {
  const auto* candidate = Find(candidates, name);
  return candidate != nullptr && candidate->availability == expected;
}
}  // namespace

int main() {
  const std::string work = ts::FreshDir("incremental-baseline");
  const std::string source = work + "/source";
  const std::string repo = work + "/repository";
  const std::string foreign = work + "/foreign";
  ts::Check(ts::Mkdir(source, 0755) && ts::Mkdir(repo, 0755) &&
                ts::Mkdir(foreign, 0755),
            "isolated source and repository");
  ts::Check(ts::Mkdir(source + "/子目录", 0755) &&
                ts::Mkdir(source + "/empty-dir", 0755) &&
                ts::WriteFile(source + "/中文.txt", "first\n", 0644) &&
                ts::WriteFile(source + "/empty", "", 0644) &&
                ts::WriteFile(source + "/子目录/nested", "nested\n", 0644),
            "Chinese, empty and nested fixture");
  bp::Filter filter;
  bp::BackupOptions options;
  options.encryption_method = bp::EncryptionMethod::kAes256CtrHmacSha256;
  options.password = "synthetic-password-A";
  const std::string identity = bp::RepositoryIdentity(repo);
  bp::IncrementalOutcome outcome;
  std::string error;
  const auto full = [&](const std::string& name,
                        const bp::BackupOptions& selected) {
    error.clear();
    return bp::RunManagedFullBackup(source, repo, name, identity, filter,
                                    selected, {}, {}, &outcome, &error);
  };
  const auto delta = [&](const std::string& name, const std::string& parent,
                         const bp::BackupOptions& selected) {
    error.clear();
    return bp::RunIncrementalBackup(source, repo, name, identity, filter,
                                    selected, {}, {}, parent, &outcome, &error);
  };
  std::vector<bp::IncrementalBaselineCandidate> candidates;
  const auto list = [&](const bp::BackupOptions& selected,
                        const std::vector<std::string>& includes =
                            std::vector<std::string>()) {
    return bp::ListIncrementalBaselineCandidates(
        repo, source, identity, selected, includes, {}, &candidates, &error);
  };

  ts::Section("equivalent source spellings share one identity");
  const std::string normal_source = work + "/normal-source";
  const std::string normal_repo = work + "/normal-repository";
  ts::Check(ts::Mkdir(normal_source, 0755) && ts::Mkdir(normal_repo, 0755) &&
                ts::WriteFile(normal_source + "/data", "before\n", 0644),
            "normalization fixture");
  bp::BackupOptions plain;
  const std::string normal_identity = bp::RepositoryIdentity(normal_repo);
  ts::Check(bp::RunManagedFullBackup(normal_source + "/./", normal_repo,
                                     "F0.bak", normal_identity, filter, plain,
                                     {}, {}, &outcome, &error),
            "full from absolute dot and trailing slash path", error);
  ts::Check(ts::WriteFile(normal_source + "/data", "after\n", 0644) &&
                bp::RunIncrementalBackup(normal_source + "/", normal_repo,
                                         "D1.bak", normal_identity, filter,
                                         plain, {}, {}, "", &outcome, &error) &&
                outcome.kind == bp::IncrementalOutcome::Kind::kDelta &&
                outcome.parent_file_name == "F0.bak",
            "equivalent trailing-slash source continues as a real delta",
            error);
  char previous_cwd[4096];
  const bool cwd_saved =
      ::getcwd(previous_cwd, sizeof(previous_cwd)) != nullptr;
  const bool changed_cwd = cwd_saved && ::chdir(work.c_str()) == 0;
  ts::Check(changed_cwd, "enter isolated cwd for relative path check");
  const bool relative_ok =
      changed_cwd &&
      bp::RunIncrementalBackup("normal-source/./", normal_repo, "unchanged.bak",
                               normal_identity, filter, plain, {}, {}, "",
                               &outcome, &error);
  const bool cwd_restored = cwd_saved && ::chdir(previous_cwd) == 0;
  ts::Check(cwd_restored && relative_ok &&
                outcome.kind == bp::IncrementalOutcome::Kind::kNoChanges &&
                !ts::Exists(normal_repo + "/unchanged.bak"),
            "relative equivalent source detects no changes", error);
  ts::Check(bp::ListIncrementalBaselineCandidates(
                normal_repo, normal_source + "/./", normal_identity, plain, {},
                {}, &candidates, &error) &&
                Status(candidates, "D1.bak", Availability::kUsable),
            "candidate API normalizes the same source spellings", error);
  bp::RestoreOptions plain_restore;
  ts::Check(bp::RestoreSnapshotChain(normal_repo, "D1.bak",
                                     work + "/normalized-restored",
                                     plain_restore, nullptr, &error),
            "normalized identity chain remains restorable", error);
  std::string normal_detail;
  ts::Check(ts::CompareTrees(normal_source, work + "/normalized-restored",
                             &normal_detail),
            "normalized identity restoration matches real source",
            normal_detail);
  const std::string source_link = work + "/source-link";
  ts::Check(ts::CreateSymlink(normal_source, source_link) &&
                !bp::RunManagedFullBackup(
                    source_link + "/", normal_repo, "root-link.bak",
                    normal_identity, filter, plain, {}, {}, &outcome, &error) &&
                !ts::Exists(normal_repo + "/root-link.bak"),
            "normalization preserves root symlink refusal", error);
  ts::Check(!bp::ListIncrementalBaselineCandidates(
                normal_repo, source_link + "/.", normal_identity, plain, {}, {},
                &candidates, &error) &&
                candidates.empty(),
            "candidate API also refuses disguised root symlink", error);
  ts::Check(ts::Mkdir(work + "/foreign-parent", 0755) &&
                ts::Mkdir(work + "/foreign-parent/child", 0755) &&
                ts::CreateSymlink(work + "/foreign-parent/child",
                                  work + "/redirect") &&
                !bp::RunManagedFullBackup(
                    work + "/redirect/..", normal_repo, "changed-meaning.bak",
                    normal_identity, filter, plain, {}, {}, &outcome, &error) &&
                !ts::Exists(normal_repo + "/changed-meaning.bak"),
            "symlink-dotdot path cannot silently select another directory",
            error);

  ts::Section("multiple encrypted chains and automatic fallback");
  ts::Check(full("z-full-A.bak", options), "AES F0 with identity", error);
  ts::Check(ts::SetTimes(repo + "/z-full-A.bak", 1000, 0), "set F0 time");
  ts::Check(ts::WriteFile(source + "/中文.txt", "second\n", 0644),
            "modify one small file");
  ts::Check(delta("y-delta-A.bak", "z-full-A.bak", options) &&
                outcome.kind == bp::IncrementalOutcome::Kind::kDelta,
            "D1 is a real AES delta", error);
  ts::Check(ts::SetTimes(repo + "/y-delta-A.bak", 2000, 0), "set D1 time");
  bp::BackupOptions other_password = options;
  other_password.password = "synthetic-password-B";
  ts::Check(full("a-new-full-B.bak", other_password),
            "second encrypted lineage with a different password", error);
  ts::Check(ts::SetTimes(repo + "/a-new-full-B.bak", 3000, 0),
            "set newer archive time independent of filename");
  ts::Check(list(options), "inspect candidates with password A", error);
  ts::Check(candidates.size() == 3 &&
                candidates.front().file_name == "a-new-full-B.bak",
            "candidate order follows archive time, not filename");
  ts::Check(Status(candidates, "a-new-full-B.bak",
                   Availability::kAuthenticationFailed) &&
                Status(candidates, "y-delta-A.bak", Availability::kUsable) &&
                Status(candidates, "z-full-A.bak", Availability::kUsable),
            "new unauthenticated chain does not hide an older usable chain");
  const auto* f0 = Find(candidates, "z-full-A.bak");
  const auto* d1 = Find(candidates, "y-delta-A.bak");
  ts::Check(
      f0 != nullptr && d1 != nullptr &&
          d1->parent_snapshot_id == f0->snapshot_id &&
          d1->lineage_id == f0->snapshot_id &&
          d1->parent_file_name == f0->file_name && !d1->manifest_digest.empty(),
      "candidate exposes verified parent, lineage and manifest identities");
  ts::Check(ts::WriteFile(source + "/new", "new file\n", 0600) &&
                ::unlink((source + "/子目录/nested").c_str()) == 0,
            "add and remove files");
  ts::Check(delta("auto-D2.bak", "", options) &&
                outcome.kind == bp::IncrementalOutcome::Kind::kDelta &&
                outcome.parent_file_name == "y-delta-A.bak",
            "automatic mode skips wrong-password newest chain", error);
  ts::Check(ts::SetTimes(repo + "/auto-D2.bak", 4000, 0), "set D2 time");

  ts::Section("explicit older restore point creates a branch");
  ts::Check(delta("branch-D2.bak", "z-full-A.bak", options) &&
                outcome.kind == bp::IncrementalOutcome::Kind::kDelta &&
                outcome.parent_file_name == "z-full-A.bak",
            "explicit selection keeps its requested parent", error);
  bp::RestoreOptions restore;
  restore.password = options.password;
  bp::RestoreReport report;
  const std::string destination = work + "/restored-branch";
  ts::Check(bp::RestoreSnapshotChain(repo, "branch-D2.bak", destination,
                                     restore, &report, &error),
            "restore selected branch", error);
  std::string detail;
  ts::Check(ts::CompareTrees(source, destination, &detail),
            "selected branch restores actual file bytes and metadata", detail);
  ts::Check(delta("no-change.bak", "branch-D2.bak", options) &&
                outcome.kind == bp::IncrementalOutcome::Kind::kNoChanges &&
                !ts::Exists(repo + "/no-change.bak"),
            "no changes do not create a snapshot", error);

  ts::Section("invalid explicit selections refuse without publishing");
  ts::Check(
      !delta("wrong-password.bak", "a-new-full-B.bak", options) &&
          error.find("Requested incremental baseline") != std::string::npos &&
          !ts::Exists(repo + "/wrong-password.bak") &&
          !ts::Exists(repo + "/wrong-password.bak.identity"),
      "explicit password mismatch refuses instead of making a full", error);
  ts::Check(!delta("bad-name.bak", "../z-full-A.bak", options) &&
                !ts::Exists(repo + "/bad-name.bak"),
            "explicit baseline path traversal refuses", error);
  ts::Check(!delta("missing.bak", "missing-parent.bak", options) &&
                !ts::Exists(repo + "/missing.bak"),
            "missing explicit baseline refuses", error);
  bp::BackupOptions wrong = options;
  wrong.password = "synthetic-wrong-password";
  ts::Check(list(wrong), "inspect all candidates with wrong password", error);
  ts::Check(std::none_of(candidates.begin(), candidates.end(),
                         [](const auto& c) {
                           return c.availability == Availability::kUsable;
                         }),
            "wrong password cannot select any encrypted baseline");
  ts::Check(
      list(options, {"ext:txt"}) &&
          Status(candidates, "z-full-A.bak", Availability::kDifferentFilters),
      "filter identity mismatch is unavailable", error);
  bp::BackupOptions different_strategy = options;
  different_strategy.compression_method = bp::CompressionMethod::kHuffman;
  ts::Check(
      list(different_strategy) &&
          Status(candidates, "z-full-A.bak", Availability::kDifferentStrategy),
      "pipeline identity mismatch is unavailable", error);
  ts::Check(
      ts::WriteFile(foreign + "/data", "foreign\n", 0644) &&
          bp::RunManagedFullBackup(foreign, repo, "foreign.bak", identity,
                                   filter, options, {}, {}, &outcome, &error),
      "foreign source fixture", error);
  ts::Check(list(options) && Status(candidates, "foreign.bak",
                                    Availability::kDifferentSource),
            "different source cannot be selected", error);
  ts::Check(!delta("cross-source.bak", "foreign.bak", options) &&
                !ts::Exists(repo + "/cross-source.bak"),
            "actual continuation rejects cross-source parent", error);

  ts::Section("read-only query cancellation");
  int cancellation_checks = 0;
  ts::Check(!bp::ListIncrementalBaselineCandidates(
                repo, source, identity, options, {}, {}, &candidates, &error,
                [&]() { return ++cancellation_checks > 1; }) &&
                candidates.empty() &&
                error.find("cancelled") != std::string::npos,
            "cancelled query returns no partially verified candidates", error);

  ts::Section("ancestor integrity is part of candidate validation");
  ts::Check(::rename((repo + "/z-full-A.bak").c_str(),
                     (repo + "/saved-full").c_str()) == 0,
            "temporarily remove parent archive");
  ts::Check(list(options) &&
                Status(candidates, "y-delta-A.bak", Availability::kBrokenChain),
            "missing parent marks candidate unavailable", error);
  ts::Check(!delta("broken.bak", "y-delta-A.bak", options) &&
                !ts::Exists(repo + "/broken.bak"),
            "broken explicit chain publishes nothing", error);
  ts::Check(::rename((repo + "/saved-full").c_str(),
                     (repo + "/z-full-A.bak").c_str()) == 0,
            "restore parent archive");
  std::string original;
  const std::string sidecar = repo + "/z-full-A.bak.identity";
  ts::Check(ts::ReadFile(sidecar, &original), "read parent identity fixture");
  std::string tampered = original;
  const auto tag = tampered.find("metadata_hmac=");
  if (tag != std::string::npos)
    tampered[tag + 14] = tampered[tag + 14] == '0' ? '1' : '0';
  ts::Check(tag != std::string::npos && ts::WriteFile(sidecar, tampered, 0600),
            "tamper parent BPIDENT3 tag only");
  ts::Check(list(options) && Status(candidates, "y-delta-A.bak",
                                    Availability::kAuthenticationFailed),
            "intact child cannot authenticate a tampered ancestor identity",
            error);
  ts::Check(!delta("tampered.bak", "y-delta-A.bak", options) &&
                !ts::Exists(repo + "/tampered.bak"),
            "actual continuation refuses tampered ancestor", error);
  ts::Check(ts::WriteFile(sidecar, original, 0600) && list(options) &&
                Status(candidates, "y-delta-A.bak", Availability::kUsable),
            "restored identity is usable again", error);

  const int result = ts::Finish("incremental_baseline_test");
  if (result == 0)
    ts::RemoveTree(work);
  else
    ts::Note("preserved failure fixture: " + work);
  return result;
}
