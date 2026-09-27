// incremental_manifest_test.cpp
//
// PR #18 的**内容身份**专项测试：增量备份的基线必须能回答"这个文件到底变了
// 没有"，而 PR #17 的 metadata-first manifest（size + mtime）回答不了。
//
// 这个文件存在的第一理由是把那条盲区钉成一条**可判别的**断言：
//
//     same-size + same-mtime 的 in-place rewrite
//       metadata-first diff：看不到（这就是为什么增量不能直接复用它）
//       strong diff：        modified
//
// 第二理由是把 v3 manifest 的格式契约钉死：确定性摘要、v1/v2 迁移、坏字节拒绝。
//
// 纯 C++17，不依赖 Qt，也不依赖任何测试框架。

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "source_digest.h"
#include "source_manifest.h"
#include "test_support.h"

namespace bp = backupproject;

namespace {

const bp::ManifestEntry* Find(const std::vector<bp::ManifestEntry>& entries,
                              const std::string& archive_path) {
  for (const bp::ManifestEntry& entry : entries) {
    if (entry.archive_path == archive_path) return &entry;
  }
  return nullptr;
}

// 就地改写：同样的长度、同样的 mtime，只有正文的字节不同。
// 这是 size+mtime 变化检测唯一看不见的那一类变化。
bool RewriteInPlace(const std::string& path, const std::string& content) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
  if (fd < 0) return false;
  const ssize_t written = ::pwrite(fd, content.data(), content.size(), 0);
  ::close(fd);
  return written == static_cast<ssize_t>(content.size());
}

bp::ManifestBinding Binding(const std::string& snapshot) {
  bp::ManifestBinding binding;
  binding.snapshot_file_name = snapshot;
  binding.repository_identity = "repo-identity";
  binding.source_path = "/source";
  return binding;
}

}  // namespace

int main() {
  // ---- M1：强化 manifest 真的带上了内容身份 ----
  test_support::Section("INC-M 1. strong manifest：普通文件与软链接都有摘要");
  {
    const std::string work = test_support::FreshDir("inc-manifest-strong");
    const std::string source = work + "/src";
    test_support::Mkdir(source, 0755);
    test_support::WriteFile(source + "/a.txt", "hello", 0644);
    test_support::WriteFile(source + "/b.txt", "hello", 0644);
    test_support::Mkdir(source + "/dir", 0755);
    test_support::CreateSymlink("a.txt", source + "/link");
    test_support::CreateFifo(source + "/pipe", 0644);

    std::vector<bp::ManifestEntry> entries;
    std::string error;
    const bool built =
        bp::BuildStrongSourceManifest(source, nullptr, &entries, &error);
    test_support::Check(built, "INC-M T1 强化 manifest 构建成功", error);
    test_support::Check(bp::HasContentDigests(entries),
                        "INC-M T1 每个普通文件与软链接都有合法摘要");

    const bp::ManifestEntry* a = Find(entries, "a.txt");
    const bp::ManifestEntry* b = Find(entries, "b.txt");
    const bp::ManifestEntry* link = Find(entries, "link");
    const bp::ManifestEntry* dir = Find(entries, "dir");
    const bp::ManifestEntry* fifo = Find(entries, "pipe");
    test_support::Check(a != nullptr && b != nullptr && link != nullptr &&
                            dir != nullptr && fifo != nullptr,
                        "INC-M T1 条目集合完整");
    test_support::Check(
        a != nullptr && a->content_digest == bp::ContentDigestOfBytes("hello"),
        "INC-M T1 正文摘要 = 正文字节的 SHA-256",
        a == nullptr ? "missing" : a->content_digest);
    test_support::Check(
        b != nullptr && a != nullptr && b->content_digest == a->content_digest,
        "INC-M T1 内容相同的两个文件摘要相同");
    test_support::Check(
        link != nullptr &&
            link->content_digest == bp::ContentDigestOfBytes("a.txt"),
        "INC-M T1 软链接摘要 = 目标字节的 SHA-256");
    test_support::Check(dir != nullptr && dir->content_digest.empty(),
                        "INC-M T1 目录不带内容摘要");
    test_support::Check(fifo != nullptr && fifo->content_digest.empty(),
                        "INC-M T1 FIFO 不带内容摘要");
    test_support::Check(bp::IsContentDigest(bp::ContentDigestOfBytes("")),
                        "INC-M T1 空内容的摘要也是合法的 64 位十六进制");
  }

  // ---- M2：manifest digest 必须确定 ----
  test_support::Section("INC-M 2. manifest digest：确定性、与遍历顺序无关");
  {
    const std::string work = test_support::FreshDir("inc-manifest-digest");
    const std::string source = work + "/src";
    test_support::Mkdir(source, 0755);
    for (int index = 0; index < 8; ++index) {
      test_support::WriteFile(source + "/f" + std::to_string(index) + ".dat",
                              std::string(64, static_cast<char>('a' + index)),
                              0644);
    }
    test_support::Mkdir(source + "/nested", 0755);
    test_support::WriteFile(source + "/nested/deep.txt", "deep", 0644);

    std::vector<bp::ManifestEntry> first;
    std::vector<bp::ManifestEntry> second;
    std::string error;
    test_support::Check(
        bp::BuildStrongSourceManifest(source, nullptr, &first, &error),
        "INC-M T2 第一次构建成功", error);
    test_support::Check(
        bp::BuildStrongSourceManifest(source, nullptr, &second, &error),
        "INC-M T2 第二次构建成功", error);
    const std::string digest_first = bp::ManifestDigest(first);
    test_support::Check(bp::IsContentDigest(digest_first),
                        "INC-M T2 manifest digest 是合法摘要", digest_first);
    test_support::Check(digest_first == bp::ManifestDigest(second),
                        "INC-M T2 同一 source state 两次摘要一致");

    // 打乱条目顺序：摘要只取决于源，不取决于遍历恰好怎么产出。
    std::reverse(second.begin(), second.end());
    test_support::Check(digest_first == bp::ManifestDigest(second),
                        "INC-M T2 条目顺序不同、摘要仍然一致（规范顺序）");

    // 内容变了，摘要必须跟着变。
    test_support::WriteFile(source + "/f0.dat", std::string(64, 'z'), 0644);
    std::vector<bp::ManifestEntry> third;
    test_support::Check(
        bp::BuildStrongSourceManifest(source, nullptr, &third, &error),
        "INC-M T2 改动之后重新构建成功", error);
    test_support::Check(digest_first != bp::ManifestDigest(third),
                        "INC-M T2 内容变化会改变 manifest digest");
  }

  // ---- M3：本文件的核心：判别测试 ----
  test_support::Section(
      "INC-M 3. 判别：same-size + same-mtime 的 in-place rewrite");
  {
    const std::string work = test_support::FreshDir("inc-manifest-rewrite");
    const std::string source = work + "/src";
    test_support::Mkdir(source, 0755);
    const std::string file = source + "/a.txt";
    test_support::WriteFile(file, "AAAA", 0644);
    const std::int64_t fixed_time = 1700000000;
    test_support::Check(test_support::SetTimes(file, fixed_time, 0),
                        "INC-M T3 固定 mtime");

    std::vector<bp::ManifestEntry> meta_before;
    std::vector<bp::ManifestEntry> strong_before;
    std::string error;
    test_support::Check(
        bp::BuildSourceManifest(source, nullptr, &meta_before, &error),
        "INC-M T3 metadata-first manifest（改动前）", error);
    test_support::Check(
        bp::BuildStrongSourceManifest(source, nullptr, &strong_before, &error),
        "INC-M T3 strong manifest（改动前）", error);

    // 就地改写：长度一样、mtime 再压回同一个值。
    test_support::Check(RewriteInPlace(file, "BBBB"),
                        "INC-M T3 就地改写正文（长度不变）");
    test_support::Check(test_support::SetTimes(file, fixed_time, 0),
                        "INC-M T3 把 mtime 压回原值");

    struct stat info;
    test_support::Check(
        test_support::StatOf(file, &info) && info.st_size == 4 &&
            info.st_mtim.tv_sec == fixed_time && info.st_mtim.tv_nsec == 0,
        "INC-M T3 改动后 size 与 mtime 与改动前完全一致");

    std::vector<bp::ManifestEntry> meta_after;
    std::vector<bp::ManifestEntry> strong_after;
    test_support::Check(
        bp::BuildSourceManifest(source, nullptr, &meta_after, &error),
        "INC-M T3 metadata-first manifest（改动后）", error);
    test_support::Check(
        bp::BuildStrongSourceManifest(source, nullptr, &strong_after, &error),
        "INC-M T3 strong manifest（改动后）", error);

    bp::ChangeSummary meta_summary;
    bp::ChangeSummary strong_summary;
    test_support::Check(bp::DiffManifests(meta_before, meta_after,
                                          &meta_summary, nullptr, &error),
                        "INC-M T3 metadata-first diff 可比较", error);
    test_support::Check(bp::DiffManifests(strong_before, strong_after,
                                          &strong_summary, nullptr, &error),
                        "INC-M T3 strong diff 可比较", error);

    // 这就是判别：旧实现看不见，新实现必须看得见。
    test_support::Check(
        meta_summary.empty(),
        "INC-M T3 判别：metadata-first 看不见这次改写（已知盲区）",
        "added=" + std::to_string(meta_summary.added) +
            " modified=" + std::to_string(meta_summary.modified));
    test_support::Check(strong_summary.modified == 1 &&
                            strong_summary.added == 0 &&
                            strong_summary.removed == 0,
                        "INC-M T3 判别：strong manifest 把它判为 modified",
                        "modified=" + std::to_string(strong_summary.modified));

    const bp::ManifestEntry* before = Find(strong_before, "a.txt");
    const bp::ManifestEntry* after = Find(strong_after, "a.txt");
    test_support::Check(
        before != nullptr && after != nullptr &&
            before->content_digest != after->content_digest,
        "INC-M T3 判别：摘要确实变了（size/mtime 没变）",
        before == nullptr || after == nullptr
            ? "missing"
            : before->content_digest + " -> " + after->content_digest);
    test_support::Check(
        bp::ManifestDigest(strong_before) != bp::ManifestDigest(strong_after),
        "INC-M T3 判别：manifest digest 也变了");
  }

  // ---- M4：格式版本与迁移 ----
  test_support::Section("INC-M 4. 格式版本：v3 写出 / v1 v2 仍然读得出来");
  {
    const std::string work = test_support::FreshDir("inc-manifest-format");
    const std::string source = work + "/src";
    test_support::Mkdir(source, 0755);
    test_support::WriteFile(source + "/a.txt", "payload", 0644);
    test_support::CreateSymlink("a.txt", source + "/link");

    std::vector<bp::ManifestEntry> strong;
    std::vector<bp::ManifestEntry> weak;
    std::string error;
    test_support::Check(
        bp::BuildStrongSourceManifest(source, nullptr, &strong, &error),
        "INC-M T4 strong manifest 构建成功", error);
    test_support::Check(bp::BuildSourceManifest(source, nullptr, &weak, &error),
                        "INC-M T4 metadata-first manifest 构建成功", error);

    const std::string v3 = bp::SerializeManifestV3(strong, Binding("s1.bak"));
    test_support::Check(!v3.empty() && v3.compare(0, 12, "BPMANIFEST3 ") == 0,
                        "INC-M T4 v3 头行正确", v3.substr(0, 16));
    std::vector<bp::ManifestEntry> parsed;
    bp::ManifestBinding binding;
    test_support::Check(bp::ParseManifest(v3, &parsed, &binding, &error),
                        "INC-M T4 v3 解析成功", error);
    test_support::Check(binding.snapshot_file_name == "s1.bak" &&
                            binding.repository_identity == "repo-identity" &&
                            binding.source_path == "/source",
                        "INC-M T4 v3 保留 baseline binding");
    test_support::Check(
        parsed.size() == strong.size() && bp::HasContentDigests(parsed),
        "INC-M T4 v3 解析回来的摘要完整");
    test_support::Check(
        bp::ManifestDigest(parsed) == bp::ManifestDigest(strong),
        "INC-M T4 v3 往返之后 manifest digest 不变");

    // v2 仍然写得出来、读得出来：它是已发布格式，不能因为 v3 就不能读。
    const std::string v2 = bp::SerializeManifest(weak, Binding("s1.bak"));
    std::vector<bp::ManifestEntry> parsed_v2;
    bp::ManifestBinding binding_v2;
    test_support::Check(bp::ParseManifest(v2, &parsed_v2, &binding_v2, &error),
                        "INC-M T4 v2 仍然解析得出来（迁移）", error);
    test_support::Check(!bp::HasContentDigests(parsed_v2),
                        "INC-M T4 v2 解析结果没有内容身份 —— 不能当增量基线");

    // v1：读得出来，但 binding 为空，也就是"不可信基线"。
    const std::string v1 = bp::SerializeManifestV1(weak);
    std::vector<bp::ManifestEntry> parsed_v1;
    bp::ManifestBinding binding_v1;
    test_support::Check(bp::ParseManifest(v1, &parsed_v1, &binding_v1, &error),
                        "INC-M T4 v1 仍然解析得出来（迁移）", error);
    test_support::Check(binding_v1.empty() && !bp::HasContentDigests(parsed_v1),
                        "INC-M T4 v1 既没有归属也没有内容身份");

    // v3 缺摘要时**写都不写**：绝不产出"自称 v3 却没有内容身份"的文件。
    test_support::Check(
        bp::SerializeManifestV3(weak, Binding("s1.bak")).empty(),
        "INC-M T4 缺摘要时 SerializeManifestV3 返回空串");
  }

  // ---- M5：坏字节必须被拒绝 ----
  test_support::Section("INC-M 5. v3 的坏字节一律拒绝");
  {
    const std::string good_digest = bp::ContentDigestOfBytes("x");
    const std::string header = "BPMANIFEST3 1\ts1.bak\trepo\t/src\n";
    const std::string prefix =
        "2\t1\t1700000000\t0\t420\t1000\t1000\t0\t0\t0\ta.txt\t\t";

    struct Case {
      std::string label;
      std::string text;
      bool expect_ok;
    };
    std::vector<Case> cases;
    cases.push_back(
        {"v3 正常条目", header + prefix + good_digest + "\n", true});
    cases.push_back({"v3 普通文件缺摘要", header + prefix + "\n", false});
    cases.push_back({"v3 摘要长度不对",
                     header + prefix + good_digest.substr(1) + "\n", false});
    cases.push_back({"v3 摘要含非十六进制",
                     header + prefix + std::string(63, 'a') + "g\n", false});
    cases.push_back(
        {"v3 只有 12 个字段",
         header + "2\t1\t1700000000\t0\t420\t1000\t1000\t0\t0\t0\ta.txt\t\n",
         false});
    cases.push_back({"v3 目录带摘要",
                     "BPMANIFEST3 1\ts1.bak\trepo\t/src\n"
                     "1\t0\t1700000000\t0\t493\t1000\t1000\t0\t0\t0\tdir\t\t" +
                         good_digest + "\n",
                     false});
    cases.push_back(
        {"v3 条数与正文不符",
         "BPMANIFEST3 2\ts1.bak\trepo\t/src\n" + prefix + good_digest + "\n",
         false});
    cases.push_back({"v3 头行缺 binding",
                     "BPMANIFEST3 1\n" + prefix + good_digest + "\n", false});

    for (const Case& item : cases) {
      std::vector<bp::ManifestEntry> parsed;
      bp::ManifestBinding binding;
      std::string error;
      const bool ok = bp::ParseManifest(item.text, &parsed, &binding, &error);
      test_support::Check(ok == item.expect_ok, "INC-M T5 " + item.label,
                          ok ? "(解析成功)" : error);
    }
  }

  // ---- M6：其它类型的身份 ----
  test_support::Section("INC-M 6. 其它 EntryType 的内容身份");
  {
    const std::string work = test_support::FreshDir("inc-manifest-types");
    const std::string source = work + "/src";
    test_support::Mkdir(source, 0755);
    test_support::WriteFile(source + "/file.txt", "content", 0644);
    test_support::CreateSymlink("file.txt", source + "/link");
    test_support::Mkdir(source + "/dir", 0755);
    test_support::CreateFifo(source + "/pipe", 0644);
    test_support::CreateHardlink(source + "/file.txt", source + "/hard");

    std::vector<bp::ManifestEntry> before;
    std::string error;
    test_support::Check(
        bp::BuildStrongSourceManifest(source, nullptr, &before, &error),
        "INC-M T6 基线构建成功", error);
    const bp::ManifestEntry* hard = Find(before, "hard");
    test_support::Check(
        hard != nullptr && hard->type == bp::EntryType::kHardLink &&
            hard->link_target == "file.txt" && hard->content_digest.empty(),
        "INC-M T6 hardlink 只记录 link_target，不带摘要");
    const bp::ManifestEntry* leader = Find(before, "file.txt");
    test_support::Check(leader != nullptr && leader->hardlink_degree == 1,
                        "INC-M T6 leader 记录 hardlink degree");

    // 每一步都以上一步的结果为基线：否则后面的断言会把前面已经制造出来的
    // 变化一起算进来，测的就不是它自己那一件事了。
    std::vector<bp::ManifestEntry> baseline = before;

    // 1) 软链接目标改变 -> modified
    ::unlink((source + "/link").c_str());
    test_support::CreateSymlink("dir", source + "/link");
    std::vector<bp::ManifestEntry> after_link;
    test_support::Check(
        bp::BuildStrongSourceManifest(source, nullptr, &after_link, &error),
        "INC-M T6 改软链接后构建成功", error);
    bp::ChangeSummary summary;
    test_support::Check(
        bp::DiffManifests(baseline, after_link, &summary, nullptr, &error),
        "INC-M T6 软链接 diff 可比较", error);
    baseline = after_link;
    test_support::Check(summary.modified == 1,
                        "INC-M T6 软链接目标改变 = modified",
                        std::to_string(summary.modified));

    // 2) 只有 mode 变 -> metadata_changed，摘要不变
    test_support::Check(::chmod((source + "/file.txt").c_str(), 0600) == 0,
                        "INC-M T6 chmod 成功");
    std::vector<bp::ManifestEntry> after_mode;
    test_support::Check(
        bp::BuildStrongSourceManifest(source, nullptr, &after_mode, &error),
        "INC-M T6 改 mode 后构建成功", error);
    bp::ChangeSummary mode_summary;
    test_support::Check(
        bp::DiffManifests(baseline, after_mode, &mode_summary, nullptr, &error),
        "INC-M T6 mode diff 可比较", error);
    baseline = after_mode;
    test_support::Check(
        mode_summary.metadata_changed >= 1 && mode_summary.modified == 0,
        "INC-M T6 只改 mode = metadata_changed",
        "meta=" + std::to_string(mode_summary.metadata_changed) +
            " modified=" + std::to_string(mode_summary.modified));

    // 3) 只 touch 目录 -> 不产生任何变化（产品决定，不翻案）
    test_support::Check(
        test_support::SetTimes(source + "/dir", 1800000000, 123456789),
        "INC-M T6 touch 目录");
    std::vector<bp::ManifestEntry> after_touch;
    test_support::Check(
        bp::BuildStrongSourceManifest(source, nullptr, &after_touch, &error),
        "INC-M T6 touch 目录后构建成功", error);
    bp::ChangeSummary touch_summary;
    test_support::Check(bp::DiffManifests(baseline, after_touch, &touch_summary,
                                          nullptr, &error),
                        "INC-M T6 touch diff 可比较", error);
    baseline = after_touch;
    test_support::Check(
        touch_summary.empty(),
        "INC-M T6 只 touch 目录不产生变化（mtime 不参与比较）",
        "added=" + std::to_string(touch_summary.added) +
            " modified=" + std::to_string(touch_summary.modified));

    // 4) FIFO 新增 -> added
    test_support::CreateFifo(source + "/pipe2", 0644);
    std::vector<bp::ManifestEntry> after_fifo;
    test_support::Check(
        bp::BuildStrongSourceManifest(source, nullptr, &after_fifo, &error),
        "INC-M T6 新增 FIFO 后构建成功", error);
    bp::ChangeSummary fifo_summary;
    test_support::Check(
        bp::DiffManifests(baseline, after_fifo, &fifo_summary, nullptr, &error),
        "INC-M T6 FIFO diff 可比较", error);
    test_support::Check(fifo_summary.added == 1, "INC-M T6 新增 FIFO = added",
                        std::to_string(fifo_summary.added));
  }

  // ---- M7：socket 仍然必须失败 ----
  test_support::Section("INC-M 7. 不会被排除的 socket 仍然让整次构建失败");
  {
    const std::string work = test_support::FreshDir("inc-manifest-socket");
    const std::string source = work + "/src";
    test_support::Mkdir(source, 0755);
    test_support::WriteFile(source + "/a.txt", "x", 0644);
    const int socket_fd = test_support::CreateUnixSocket(source + "/sock");
    if (socket_fd < 0) {
      test_support::Note("INC-M T7 无法创建 unix socket，跳过这一条");
    } else {
      std::vector<bp::ManifestEntry> entries;
      std::string error;
      const bool built =
          bp::BuildStrongSourceManifest(source, nullptr, &entries, &error);
      test_support::Check(!built && entries.empty(),
                          "INC-M T7 强化 manifest 也 fail closed", error);
      ::unlink((source + "/sock").c_str());
      ::close(socket_fd);
    }
  }

  return test_support::Finish("incremental_manifest_test");
}
