// tests/review/bundle_fuzz.cpp
//
// PR #21 独立审查轮：BPSNAP1 材料包解析 / 解包的**变异模糊测试**（review-only，
// 不参与产品构建）。
//
// 为什么要有它：本轮最严重的一条缺陷（F8）就在 ExtractSnapshotBundle 的第二遍
// 解析里，而那是靠"40 轮里 6-9 轮"的竞态才暴露出来的。这里改成**确定性的随机
// 变异**（固定种子、可重放），把解析路径整片扫一遍，断言四条不变式：
//
//   I1 解包成功 => 目标目录里恰好是三件套、名字正确、**内容与原件逐字节一致**；
//   I2 解包失败 => 目标目录必须是空的（不留下半套、不留下 .part-*）；
//   I3 目标目录**之外**不允许出现任何新文件（F8 的直接回归）；
//   I4 整个进程不能崩、不能越界（ASan/UBSan 下跑）、单轮不能病态变慢。
//
// 用法：bundle_fuzz [iterations] [seed]
// 退出码：0 = 四条不变式全部保持。

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "incremental_backup.h"
#include "snapshot_bundle.h"

namespace {

std::uint64_t g_rng = 0x243F6A8885A308D3ull;

std::uint64_t NextRandom() {
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 7;
  g_rng ^= g_rng << 17;
  return g_rng;
}

std::size_t RandomBelow(std::size_t bound) {
  return bound == 0 ? 0 : static_cast<std::size_t>(NextRandom() % bound);
}

int g_failures = 0;

void Fail(const std::string& what, const std::string& detail) {
  ++g_failures;
  std::printf("FUZZ FAIL: %s [%s]\n", what.c_str(), detail.c_str());
}

bool EnsureDir(const std::string& path) {
  return ::mkdir(path.c_str(), 0700) == 0 || errno == EEXIST;
}

bool WriteFile(const std::string& path, const std::string& data) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    return false;
  }
  std::size_t written = 0;
  while (written < data.size()) {
    const ssize_t got =
        ::write(fd, data.data() + written, data.size() - written);
    if (got <= 0) {
      ::close(fd);
      return false;
    }
    written += static_cast<std::size_t>(got);
  }
  return ::close(fd) == 0;
}

bool ReadFile(const std::string& path, std::string* out) {
  out->clear();
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return false;
  }
  char buffer[8192];
  for (;;) {
    const ssize_t got = ::read(fd, buffer, sizeof(buffer));
    if (got < 0) {
      ::close(fd);
      return false;
    }
    if (got == 0) {
      break;
    }
    out->append(buffer, static_cast<std::size_t>(got));
  }
  ::close(fd);
  return true;
}

bool RemoveTree(const std::string& path) {
  DIR* dir = ::opendir(path.c_str());
  if (dir != nullptr) {
    while (struct dirent* entry = ::readdir(dir)) {
      const std::string name = entry->d_name;
      if (name == "." || name == "..") {
        continue;
      }
      RemoveTree(path + "/" + name);
    }
    ::closedir(dir);
    return ::rmdir(path.c_str()) == 0;
  }
  return ::unlink(path.c_str()) == 0 || errno == ENOENT;
}

std::vector<std::string> ListDir(const std::string& path) {
  std::vector<std::string> names;
  DIR* dir = ::opendir(path.c_str());
  if (dir == nullptr) {
    return names;
  }
  while (struct dirent* entry = ::readdir(dir)) {
    const std::string name = entry->d_name;
    if (name != "." && name != "..") {
      names.push_back(name);
    }
  }
  ::closedir(dir);
  std::sort(names.begin(), names.end());
  return names;
}

}  // namespace

int main(int argc, char** argv) {
  const long iterations = argc > 1 ? std::atol(argv[1]) : 20000;
  if (argc > 2) {
    g_rng = std::strtoull(argv[2], nullptr, 10) | 1ull;
  }
  const std::string root = "/tmp/bundle-fuzz";
  RemoveTree(root);
  const std::string repo = root + "/repo";
  const std::string archive = "remote-fuzz-g0.bak";
  const std::string manifest_name =
      backupproject::SnapshotManifestFileName(archive);
  const std::string identity_name =
      backupproject::SnapshotIdentityFileName(archive);
  if (!EnsureDir(root) || !EnsureDir(repo)) {
    std::printf("无法准备工作目录\n");
    return 2;
  }
  std::string bak(4096, '\0');
  std::string manifest(512, '\0');
  std::string identity(256, '\0');
  for (char& c : bak) c = static_cast<char>(NextRandom());
  for (char& c : manifest) c = static_cast<char>(NextRandom());
  for (char& c : identity) c = static_cast<char>(NextRandom());
  if (!WriteFile(repo + "/" + archive, bak) ||
      !WriteFile(repo + "/" + manifest_name, manifest) ||
      !WriteFile(repo + "/" + identity_name, identity)) {
    std::printf("无法写入源三件套\n");
    return 2;
  }
  const std::string good_bundle = root + "/good.bundle";
  backupproject::net::SnapshotBundleInfo info;
  std::string error;
  if (!backupproject::net::BuildSnapshotBundle(repo, archive, good_bundle,
                                               &info, &error)) {
    std::printf("打包失败: %s\n", error.c_str());
    return 2;
  }
  std::string good;
  if (!ReadFile(good_bundle, &good)) {
    std::printf("无法读回打包结果\n");
    return 2;
  }
  std::printf(
      "BUNDLE_FUZZ_START valid_bundle=%zu bytes members=%zu seed=%llu\n",
      good.size(), info.members.size(), static_cast<unsigned long long>(g_rng));

  const std::string case_bundle = root + "/case.bundle";
  const std::string target = root + "/out";
  double worst_ms = 0.0;
  std::string worst_detail;

  for (long i = 0; i < iterations; ++i) {
    std::string mutated = good;
    const int mutations = 1 + static_cast<int>(RandomBelow(8));
    for (int m = 0; m < mutations; ++m) {
      switch (static_cast<int>(RandomBelow(6))) {
        case 0:
          if (!mutated.empty()) {
            mutated[RandomBelow(mutated.size())] ^=
                static_cast<char>(1u << RandomBelow(8));
          }
          break;
        case 1:
          if (!mutated.empty()) {
            mutated[RandomBelow(mutated.size())] =
                static_cast<char>(NextRandom());
          }
          break;
        case 2:
          if (!mutated.empty()) {
            mutated.resize(RandomBelow(mutated.size()));
          }
          break;
        case 3:
          mutated.append(1 + RandomBelow(16), static_cast<char>(NextRandom()));
          break;
        case 4: {
          if (mutated.size() >= 8) {
            static const std::uint64_t kExtremes[] = {0ull,
                                                      1ull,
                                                      0xFFFFFFFFFFFFFFFFull,
                                                      0x7FFFFFFFFFFFFFFFull,
                                                      0x400000000ull,
                                                      0x1000000000ull};
            const std::size_t at = RandomBelow(mutated.size() - 7);
            const std::uint64_t value = kExtremes[RandomBelow(6)];
            for (int b = 0; b < 8; ++b) {
              mutated[at + static_cast<std::size_t>(b)] =
                  static_cast<char>((value >> (8 * (7 - b))) & 0xFF);
            }
          }
          break;
        }
        case 5: {
          if (mutated.size() > 16) {
            const std::size_t from = RandomBelow(mutated.size() - 8);
            const std::size_t length = 1 + RandomBelow(std::min<std::size_t>(
                                               64, mutated.size() - from));
            mutated.insert(RandomBelow(mutated.size()),
                           mutated.substr(from, length));
          }
          break;
        }
      }
    }

    ::unlink(case_bundle.c_str());
    if (!WriteFile(case_bundle, mutated)) {
      Fail("写不出变异后的包", std::to_string(i));
      break;
    }
    RemoveTree(target);
    if (!EnsureDir(target)) {
      Fail("建不出目标目录", std::to_string(i));
      break;
    }

    const auto started = std::chrono::steady_clock::now();
    backupproject::net::SnapshotBundleInfo inspected;
    std::string inspect_error;
    const bool inspect_ok = backupproject::net::InspectSnapshotBundle(
        case_bundle, &inspected, &inspect_error);
    backupproject::net::SnapshotBundleInfo extracted;
    std::string extract_error;
    const bool extract_ok = backupproject::net::ExtractSnapshotBundle(
        case_bundle, target, &extracted, &extract_error);
    const double elapsed_ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - started)
                                  .count();
    if (elapsed_ms > worst_ms) {
      worst_ms = elapsed_ms;
      worst_detail = "轮 " + std::to_string(i) +
                     " inspect=" + (inspect_ok ? "ok" : "fail") +
                     " extract=" + (extract_ok ? "ok" : "fail");
    }

    const std::vector<std::string> names = ListDir(target);

    // I1：解包成功 => 恰好三件套 + 内容与原件逐字节一致
    if (extract_ok) {
      // 注意：ListDir 返回的是**排序后**的名字，所以这里也要排序再比集合
      // （第一版忘了排序，把"顺序不同"误报成 21 次失败——harness 自己的 bug，
      // 记在这里以免下次再踩）。
      std::vector<std::string> expected = {archive, manifest_name,
                                           identity_name};
      std::sort(expected.begin(), expected.end());
      if (names != expected) {
        std::string joined;
        for (const std::string& n : names) joined += n + " ";
        Fail("解包成功但目标目录不是三件套", joined);
      } else {
        for (const std::string& pair : expected) {
          std::string got;
          if (!ReadFile(target + "/" + pair, &got)) {
            Fail("解包成功但读不回成员", pair);
            continue;
          }
          const std::string& want = pair == archive         ? bak
                                    : pair == manifest_name ? manifest
                                                            : identity;
          if (got != want) {
            Fail("解包成功但成员内容与原件不一致", pair);
          }
        }
      }
    } else if (!names.empty()) {
      // I2：解包失败 => 目标目录必须为空
      std::string joined;
      for (const std::string& n : names) joined += n + " ";
      Fail("解包失败却留下了文件", joined);
    }

    for (const std::string& name : names) {
      if (name.find(".part-") != std::string::npos) {
        Fail("目标目录里留下 .part 残留", name);  // I2 的一部分
      }
    }

    // I3：目标目录之外不允许出现任何新东西
    for (const std::string& entry : ListDir(root)) {
      if (entry != "repo" && entry != "good.bundle" && entry != "case.bundle" &&
          entry != "out") {
        Fail("目标目录之外出现了新文件", entry);
      }
    }

    if (g_failures > 20) {
      break;
    }
    if ((i + 1) % 5000 == 0) {
      std::printf("... %ld 轮，失败 %d\n", i + 1, g_failures);
    }
  }

  std::printf("worst case: %.1f ms (%s)\n", worst_ms, worst_detail.c_str());
  std::printf("BUNDLE_FUZZ_DONE iterations=%ld failures=%d\n", iterations,
              g_failures);
  RemoveTree(root);
  return g_failures == 0 ? 0 : 1;
}
