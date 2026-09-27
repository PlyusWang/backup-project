// incremental_crash_test.cpp
//
// PR #18：增量快照的崩溃一致性。
//
// PR #17 的教训是：archive / manifest / state 是三个各自原子替换的文件，
// 没有任何时刻能让三个一起提交。增量多了一份 identity 副文件，问题是同一类：
//
//     快照发布了，但它的副文件没写成功
//
// 这条用例把每一个"写了一半"的状态**造出来**（不是杀进程：造出来的状态更
// 确定，而且正是崩溃会留下的那个状态），然后要求下一轮的行为是安全的：
//
//   * 没有 manifest / 没有 identity 的快照永远不被当成基线；
//   * 于是下一轮老老实实重建完整基线，而不是接着一条无法验证的链往下写；
//   * 副文件写失败时，产品自己撤掉刚发布的快照，不留孤儿。

#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <vector>

#include "incremental_backup.h"
#include "incremental_delta.h"
#include "source_manifest.h"
#include "test_support.h"

namespace bp = backupproject;

namespace {

const char* kRepoIdentity = "crash-repo";

bool RunOnce(const std::string& source, const std::string& repository,
             const std::string& snapshot, bp::IncrementalOutcome* outcome,
             std::string* error) {
  bp::Filter filter;
  bp::BackupOptions options;
  return bp::RunIncrementalBackup(source, repository, snapshot, kRepoIdentity,
                                  filter, options, {}, {}, "", outcome, error);
}

std::string SnapshotPath(const std::string& repository,
                         const std::string& name) {
  return repository + "/" + name;
}

}  // namespace

int main() {
  // ---- C1：快照发布了，manifest 没写 ----
  test_support::Section("INC-C 1. delta 已发布但没有 manifest");
  {
    const std::string work = test_support::FreshDir("inc-crash-manifest");
    const std::string source = work + "/src";
    const std::string repo = work + "/repo";
    test_support::Mkdir(source, 0755);
    test_support::Mkdir(repo, 0755);
    test_support::WriteFile(source + "/a.txt", "one", 0644);

    bp::IncrementalOutcome outcome;
    std::string error;
    test_support::Check(RunOnce(source, repo, "s1.bak", &outcome, &error),
                        "INC-C T1 建立基线", error);
    test_support::Check(
        outcome.kind == bp::IncrementalOutcome::Kind::kFullBaseline,
        "INC-C T1 第一次是完整基线");

    test_support::WriteFile(source + "/a.txt", "two", 0644);
    test_support::Check(RunOnce(source, repo, "s2.bak", &outcome, &error),
                        "INC-C T1 写出 delta", error);
    test_support::Check(outcome.kind == bp::IncrementalOutcome::Kind::kDelta,
                        "INC-C T1 第二次是 delta");

    // 崩溃点：delta 已经 rename 到位，manifest 还没写。
    test_support::Check(
        ::unlink(SnapshotPath(repo, "s2.bak.manifest").c_str()) == 0,
        "INC-C T1 造出崩溃状态：删掉 delta 的 manifest");
    test_support::Check(test_support::Exists(SnapshotPath(repo, "s2.bak")),
                        "INC-C T1 快照文件本身还在（这就是崩溃留下的样子）");

    // 下一轮：s2 没有 manifest，因此绝不能被当成基线。基线搜索会跳过它，
    // 继续往前找到仍然可信的 s1 —— 从"最新的**可信**基线"继续写 delta
    // 是正确的，而且比无谓地重建一个 generation 更好。
    test_support::WriteFile(source + "/a.txt", "three", 0644);
    bp::IncrementalOutcome after;
    test_support::Check(RunOnce(source, repo, "s3.bak", &after, &error),
                        "INC-C T1 崩溃之后下一轮仍然成功", error);
    test_support::Check(
        after.kind == bp::IncrementalOutcome::Kind::kDelta &&
            after.parent_file_name == "s1.bak",
        "INC-C T1 判别：没有 manifest 的 s2 不被当成父，链从可信的 s1 继续",
        after.parent_file_name +
            " kind=" + std::to_string(static_cast<int>(after.kind)));
    test_support::Check(outcome.snapshot_file_name != after.snapshot_file_name,
                        "INC-C T1 新快照是另一份文件");
  }

  // ---- C1b：唯一那份基线的 manifest 坏了 ----
  test_support::Section("INC-C 1b. 没有可信基线时重建完整基线");
  {
    const std::string work = test_support::FreshDir("inc-crash-nobase");
    const std::string source = work + "/src";
    const std::string repo = work + "/repo";
    test_support::Mkdir(source, 0755);
    test_support::Mkdir(repo, 0755);
    test_support::WriteFile(source + "/a.txt", "one", 0644);

    bp::IncrementalOutcome outcome;
    std::string error;
    test_support::Check(RunOnce(source, repo, "s1.bak", &outcome, &error),
                        "INC-C T1b 建立基线", error);
    // 把 manifest 写坏：内容不是合法 manifest，解析必然失败。
    test_support::Check(
        test_support::WriteFile(SnapshotPath(repo, "s1.bak.manifest"),
                                "BPMANIFEST3 1\tgarbage\n", 0644),
        "INC-C T1b 把 manifest 写坏");
    test_support::WriteFile(source + "/a.txt", "two", 0644);
    bp::IncrementalOutcome fresh;
    test_support::Check(RunOnce(source, repo, "s2.bak", &fresh, &error),
                        "INC-C T1b 坏 manifest 之后仍然成功", error);
    test_support::Check(
        fresh.kind == bp::IncrementalOutcome::Kind::kFullBaseline,
        "INC-C T1b 判别：唯一基线的 manifest 坏了 -> 重建完整基线",
        fresh.baseline_reason);
  }

  // ---- C2：有 manifest 但没有 identity ----
  test_support::Section("INC-C 2. delta 有 manifest 但没有 identity");
  {
    const std::string work = test_support::FreshDir("inc-crash-identity");
    const std::string source = work + "/src";
    const std::string repo = work + "/repo";
    test_support::Mkdir(source, 0755);
    test_support::Mkdir(repo, 0755);
    test_support::WriteFile(source + "/a.txt", "one", 0644);

    bp::IncrementalOutcome outcome;
    std::string error;
    test_support::Check(RunOnce(source, repo, "s1.bak", &outcome, &error),
                        "INC-C T2 建立基线", error);
    test_support::WriteFile(source + "/a.txt", "two", 0644);
    test_support::Check(RunOnce(source, repo, "s2.bak", &outcome, &error),
                        "INC-C T2 写出 delta", error);
    test_support::Check(
        ::unlink(SnapshotPath(repo, "s2.bak.identity").c_str()) == 0,
        "INC-C T2 造出崩溃状态：删掉 delta 的 identity");
    test_support::Check(
        test_support::Exists(SnapshotPath(repo, "s2.bak.manifest")),
        "INC-C T2 manifest 还在");

    test_support::WriteFile(source + "/a.txt", "three", 0644);
    bp::IncrementalOutcome after;
    test_support::Check(RunOnce(source, repo, "s3.bak", &after, &error),
                        "INC-C T2 崩溃之后下一轮仍然成功", error);
    test_support::Check(
        after.kind == bp::IncrementalOutcome::Kind::kDelta &&
            after.parent_file_name == "s1.bak",
        "INC-C T2 判别：缺 identity 的 s2 不被当成基线，链从 s1 继续",
        after.parent_file_name);
  }

  // ---- C3：副文件写失败时不留孤儿 ----
  test_support::Section("INC-C 3. 副文件写不出去时撤回刚发布的快照");
  {
    const std::string work = test_support::FreshDir("inc-crash-orphan");
    const std::string source = work + "/src";
    const std::string repo = work + "/repo";
    test_support::Mkdir(source, 0755);
    test_support::Mkdir(repo, 0755);
    test_support::WriteFile(source + "/a.txt", "one", 0644);

    bp::IncrementalOutcome outcome;
    std::string error;
    test_support::Check(RunOnce(source, repo, "s1.bak", &outcome, &error),
                        "INC-C T3 建立基线", error);

    // 注入：把 manifest 的目标路径做成一个目录，rename 必然失败。
    test_support::Check(
        test_support::Mkdir(SnapshotPath(repo, "s2.bak.manifest"), 0755),
        "INC-C T3 把 manifest 路径做成目录");
    test_support::WriteFile(source + "/a.txt", "two", 0644);
    const bool failed = !RunOnce(source, repo, "s2.bak", &outcome, &error);
    test_support::Check(failed, "INC-C T3 副文件写失败时整次失败并报错", error);
    test_support::Check(!test_support::Exists(SnapshotPath(repo, "s2.bak")),
                        "INC-C T3 判别：不留孤儿快照（刚发布的 delta 被撤回）");
  }

  // ---- C4：正常路径的对照 ----
  test_support::Section("INC-C 4. 对照组：三个文件都写成功时链正常延续");
  {
    const std::string work = test_support::FreshDir("inc-crash-ok");
    const std::string source = work + "/src";
    const std::string repo = work + "/repo";
    test_support::Mkdir(source, 0755);
    test_support::Mkdir(repo, 0755);
    test_support::WriteFile(source + "/a.txt", "one", 0644);

    bp::IncrementalOutcome outcome;
    std::string error;
    test_support::Check(RunOnce(source, repo, "s1.bak", &outcome, &error),
                        "INC-C T4 建立基线", error);
    test_support::WriteFile(source + "/a.txt", "two", 0644);
    bp::IncrementalOutcome second;
    test_support::Check(RunOnce(source, repo, "s2.bak", &second, &error),
                        "INC-C T4 写出 delta", error);
    test_support::Check(second.kind == bp::IncrementalOutcome::Kind::kDelta &&
                            second.parent_file_name == "s1.bak",
                        "INC-C T4 delta 的父是刚才那份基线",
                        second.parent_file_name);
    test_support::Check(
        test_support::Exists(SnapshotPath(repo, "s2.bak")) &&
            test_support::Exists(SnapshotPath(repo, "s2.bak.manifest")) &&
            test_support::Exists(SnapshotPath(repo, "s2.bak.identity")),
        "INC-C T4 快照与两个副文件都在");
  }

  return test_support::Finish("incremental_crash_test");
}
