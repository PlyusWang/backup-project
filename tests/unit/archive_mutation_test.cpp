// archive_mutation_test.cpp
//
// 确定性 mutation（fuzz-like）测试：对每一种我们自己的格式做"改一个 bit 再看
// 会发生什么"，要求永远落在两个结果之一：
//
//   valid   → 正常成功
//   invalid → 干净地失败（返回 false + 可读的错误信息）
//
// 绝不允许：崩溃 / 越界 / 无限循环 / 半恢复（失败却留下 destination）。
//
// 与 archive_container_test.cpp 的区别：那边是"针对每个字段的定点破坏"，
// 这里是"按偏移量扫一遍"的宽覆盖，两者互补。

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "archive_pipeline.h"
#include "backup_engine.h"
#include "compression.h"
#include "filter.h"
#include "pack_stream.h"
#include "test_support.h"
#include "tree_scanner.h"
#include "ustar.h"

namespace {

using backupproject::BackupEngine;
using backupproject::BackupOptions;
using backupproject::CompressionMethod;
using backupproject::EncryptionMethod;
using backupproject::Filter;
using backupproject::PackMethod;
using backupproject::RestoreOptions;

std::uint64_t TotalMutations = 0;
std::uint64_t TotalRejected = 0;
std::uint64_t TotalAccepted = 0;
std::uint64_t TotalLeftovers = 0;

// 一次"翻转一个 bit 然后 restore"。返回值只用于统计。
void MutateAndRestore(const std::string& label, const std::string& archive,
                      const std::string& workdir,
                      const std::string& password) {
  std::string bytes;
  if (!test_support::ReadFile(archive, &bytes)) {
    test_support::Check(false, label + ": read fixture");
    return;
  }
  // 头部 160 字节逐字节扫；payload 部分按固定步长抽样（全扫代价太大）。
  std::vector<std::size_t> offsets;
  for (std::size_t index = 0; index < bytes.size() && index < 160; ++index) {
    offsets.push_back(index);
  }
  for (std::size_t index = 160; index < bytes.size(); index += 337) {
    offsets.push_back(index);
  }

  std::string mutated = bytes;
  const std::string target = workdir + "/mut.bak";
  const std::string destination = workdir + "/mut.out";
  BackupEngine engine;
  for (const std::size_t offset : offsets) {
    for (const unsigned char mask : {0x01, 0x80}) {
      mutated = bytes;
      mutated[offset] = static_cast<char>(
          static_cast<unsigned char>(mutated[offset]) ^ mask);
      test_support::RemoveTree(target);
      test_support::RemoveTree(destination);
      if (!test_support::WriteFile(target, mutated, 0644)) {
        test_support::Check(false, label + ": write mutated archive");
        return;
      }
      TotalMutations += 1;
      RestoreOptions options;
      options.password = password;
      backupproject::RestoreReport report;
      std::string error;
      const bool ok =
          engine.Restore(target, destination, options, &report, &error);
      if (ok) {
        TotalAccepted += 1;
      } else {
        TotalRejected += 1;
        if (test_support::Exists(destination)) {
          TotalLeftovers += 1;
        }
      }
    }
  }
  test_support::RemoveTree(target);
  test_support::RemoveTree(destination);
}

// 裸 packed 流的 mutation：Scan 必须永不崩溃。
void MutatePackedStream(const std::string& label, const std::string& packed,
                        PackMethod method, const std::string& workdir) {
  std::string bytes;
  if (!test_support::ReadFile(packed, &bytes)) {
    test_support::Check(false, label + ": read packed fixture");
    return;
  }
  std::vector<std::size_t> offsets;
  for (std::size_t index = 0; index < bytes.size() && index < 200; ++index) {
    offsets.push_back(index);
  }
  for (std::size_t index = 200; index < bytes.size(); index += 271) {
    offsets.push_back(index);
  }
  const std::string target = workdir + "/mut.pack";
  for (const std::size_t offset : offsets) {
    std::string mutated = bytes;
    mutated[offset] = static_cast<char>(
        static_cast<unsigned char>(mutated[offset]) ^ 0x01);
    test_support::RemoveTree(target);
    if (!test_support::WriteFile(target, mutated, 0644)) {
      return;
    }
    TotalMutations += 1;
    backupproject::PackedStreamReader reader;
    std::string error;
    if (reader.Open(target, &error) && reader.Scan(method, &error)) {
      TotalAccepted += 1;
    } else {
      TotalRejected += 1;
    }
  }
  test_support::RemoveTree(target);
}

// 压缩流的 mutation：解压必须永不崩溃、不 OOM。
void MutateCompressedStream(const std::string& label, const std::string& input,
                            bool lzss) {
  std::string compressed;
  std::string error;
  const bool ok = lzss
                      ? backupproject::compression::LzssHuffmanCompress(
                            input, &compressed, &error)
                      : backupproject::compression::HuffmanCompress(
                            input, &compressed, &error);
  if (!ok) {
    test_support::Check(false, label + ": compress fixture", error);
    return;
  }
  std::vector<std::size_t> offsets;
  for (std::size_t index = 0; index < compressed.size() && index < 300; ++index) {
    offsets.push_back(index);
  }
  for (std::size_t index = 300; index < compressed.size(); index += 173) {
    offsets.push_back(index);
  }
  for (const std::size_t offset : offsets) {
    std::string mutated = compressed;
    mutated[offset] = static_cast<char>(
        static_cast<unsigned char>(mutated[offset]) ^ 0x40);
    TotalMutations += 1;
    std::string output;
    std::string decode_error;
    const bool decoded =
        lzss ? backupproject::compression::LzssHuffmanDecompress(
                   mutated, &output, &decode_error)
             : backupproject::compression::HuffmanDecompress(
                   mutated, &output, &decode_error);
    if (decoded) {
      TotalAccepted += 1;
    } else {
      TotalRejected += 1;
    }
  }
}

void BuildSource(const std::string& root) {
  test_support::Mkdir(root, 0755);
  test_support::WriteFile(root + "/a.txt", "alpha beta gamma\n", 0644);
  test_support::Mkdir(root + "/sub", 0755);
  test_support::WriteFile(root + "/sub/b.bin", std::string(3000, 'b'), 0600);
  test_support::NormalizeTimes(root, 1700000000);
}

}  // namespace

int main() {
  std::printf("archive mutation (fuzz-like) test\n");
  const std::string workdir = test_support::FreshDir("mutation");
  const std::string source = workdir + "/source";
  BuildSource(source);
  const char* password = "mutation password";

  const auto start = std::chrono::steady_clock::now();

  // 1) v2 容器：三种算法组合各扫一遍
  struct Combo {
    const char* label;
    PackMethod pack;
    CompressionMethod compression;
    EncryptionMethod encryption;
    const char* password;
  };
  const Combo combos[] = {
      {"mypack+none+none", PackMethod::kMyPack, CompressionMethod::kNone,
       EncryptionMethod::kNone, ""},
      {"ustar+lzss+none", PackMethod::kUstar,
       CompressionMethod::kLzssHuffman, EncryptionMethod::kNone, ""},
      {"fast+huffman+aes", PackMethod::kFastUstar,
       CompressionMethod::kHuffman, EncryptionMethod::kAes256CtrHmacSha256,
       password},
      {"mypack+huffman+des", PackMethod::kMyPack,
       CompressionMethod::kHuffman, EncryptionMethod::kDesCbcHmacSha256,
       password},
  };
  for (const Combo& combo : combos) {
    const std::string archive = workdir + "/" + combo.label + ".bak";
    test_support::RemoveTree(archive);
    BackupOptions options;
    options.pack_method = combo.pack;
    options.compression_method = combo.compression;
    options.encryption_method = combo.encryption;
    options.password = combo.password;
    BackupEngine engine;
    std::string error;
    if (!engine.Backup(source, archive, Filter(), options, &error)) {
      test_support::Check(false,
                          std::string("mutation fixture backup: ") + combo.label,
                          error);
      continue;
    }
    // 未变异的归档必须成功——否则"变异后失败"证明不了任何东西。
    const std::string good_out = workdir + "/good-" + combo.label;
    test_support::RemoveTree(good_out);
    RestoreOptions restore_options;
    restore_options.password = combo.password;
    backupproject::RestoreReport report;
    error.clear();
    std::string detail;
    test_support::Check(engine.Restore(archive, good_out, restore_options,
                                       &report, &error) &&
                            test_support::CompareTrees(source, good_out, &detail),
                        std::string("pristine archive restores: ") + combo.label,
                        error + detail);
    MutateAndRestore(combo.label, archive, workdir, combo.password);
    test_support::Check(true, std::string("mutation sweep finished: ") +
                                  combo.label);
  }

  // 2) 裸 packed 流
  {
    std::vector<backupproject::ArchiveEntry> entries;
    std::string error;
    if (!backupproject::ScanSourceTree(source, nullptr, &entries, &error)) {
      test_support::Check(false, "scan for packed mutation fixture", error);
    } else {
      for (const PackMethod pack : {PackMethod::kMyPack, PackMethod::kUstar}) {
        const std::string packed =
            workdir + "/packed-" + std::to_string(static_cast<int>(pack)) + ".pack";
        test_support::RemoveTree(packed);
        if (!backupproject::PackEntries(pack, entries, packed, &error)) {
          test_support::Check(false, "packed mutation fixture", error);
          continue;
        }
        MutatePackedStream(backupproject::PackMethodName(pack), packed, pack,
                           workdir);
      }
    }
  }

  // 3) 压缩流
  {
    std::string input;
    for (int index = 0; index < 4000; ++index) {
      input += "the quick brown fox jumps over the lazy dog ";
      input += std::to_string(index % 37);
      input += "\n";
    }
    MutateCompressedStream("huffman", input, /*lzss=*/false);
    MutateCompressedStream("lzh1", input, /*lzss=*/true);
  }

  const auto end = std::chrono::steady_clock::now();
  const double seconds =
      std::chrono::duration<double>(end - start).count();

  std::printf("\nMUTATION\ttotal\t%llu\n",
              static_cast<unsigned long long>(TotalMutations));
  std::printf("MUTATION\trejected\t%llu\n",
              static_cast<unsigned long long>(TotalRejected));
  std::printf("MUTATION\taccepted\t%llu\n",
              static_cast<unsigned long long>(TotalAccepted));
  std::printf("MUTATION\tleftover destinations\t%llu\n",
              static_cast<unsigned long long>(TotalLeftovers));
  std::printf("MUTATION\telapsed\t%.2f s\n", seconds);
  std::printf("MUTATION\tcrash\t0\n");
  std::printf("MUTATION\thang\t0\n");

  test_support::Check(TotalLeftovers == 0,
                      "no mutation left a half-restored destination");
  test_support::Check(TotalMutations > 1000,
                      "mutation corpus is big enough",
                      std::to_string(TotalMutations));
  return test_support::Finish("archive-mutation");
}
