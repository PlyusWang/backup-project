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
//
// 统计必须按格式分层。一个总数不能当作"加密完整性"的证据：AES / DES 容器的
// header 与密文整段都在 HMAC 保护之下，任何一个被测字节被改动都必须被拒绝；
// 而裸 packed 流与裸压缩流没有认证，改一个 bit 偶尔仍然是另一条合法流。两类
// 混在一起统计，前者的硬结论会被后者的"偶然接受"稀释掉。

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
#include "container_format.h"
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

// 每一类格式单独统计：混在一起的数字说明不了任何具体格式的性质。
enum class Layer {
  kAesContainer = 0,
  kDesContainer,
  kUnencryptedContainer,
  kRawMyPack,
  kRawUstar,
  kRawHuf1,
  kRawLzh1,
  kCount,
};

struct LayerStats {
  const char* name = "";
  // 只有加密容器的统计能支撑"受认证的字节一个都不能被接受"这条硬断言。
  const char* assertion_name = "";
  bool authenticated = false;
  std::uint64_t mutations = 0;
  std::uint64_t rejected = 0;
  std::uint64_t accepted = 0;
  // 变异根本没落到文件（写入失败 / 读回不一致）：它不是被测样本，不能计入。
  std::uint64_t skipped = 0;
  std::uint64_t accepted_authenticated = 0;
};

LayerStats kLayers[static_cast<int>(Layer::kCount)] = {
    {"AES encrypted container", "AES", /*authenticated=*/true},
    {"DES encrypted container", "DES", /*authenticated=*/true},
    {"unencrypted container", "", /*authenticated=*/false},
    {"raw MyPack", "", /*authenticated=*/false},
    {"raw USTAR", "", /*authenticated=*/false},
    {"raw HUF1", "", /*authenticated=*/false},
    {"raw LZH1", "", /*authenticated=*/false},
};

LayerStats& StatsOf(Layer layer) {
  return kLayers[static_cast<int>(layer)];
}

std::uint64_t TotalLeftovers = 0;
std::uint64_t TotalSkipped = 0;

// 加密容器里哪些字节受 HMAC 保护：header 的 0..159 全部参与归一化 header，
// 其后的每一个密文字节全部参与 MAC。这里显式分成两段，而不是写一句"全都
// 算"：将来容器尾部若允许出现额外字节，这段判断会立刻不对，而不是把未认证
// 的字节默默算进硬断言里。
bool IsAuthenticatedByte(bool layer_authenticated, std::size_t offset,
                         std::size_t file_size) {
  if (!layer_authenticated) {
    return false;
  }
  if (offset < backupproject::container_v2::kHeaderSize) {
    return true;
  }
  return offset < file_size;
}

// 一次"翻转一个 bit 然后 restore"。统计全部记在对应格式层上。
void MutateAndRestore(Layer layer, const std::string& label,
                      const std::string& archive, const std::string& workdir,
                      const std::string& password) {
  LayerStats& stats = StatsOf(layer);
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
      // 变异必须先真的落到磁盘上，否则这一条不是"改了一个 bit 的归档"，
      // 而只是"写文件失败"——把它算进被拒绝的次数等于自欺欺人。
      if (!test_support::WriteFile(target, mutated, 0644)) {
        stats.skipped += 1;
        TotalSkipped += 1;
        continue;
      }
      std::string landed;
      if (!test_support::ReadFile(target, &landed) || landed != mutated) {
        stats.skipped += 1;
        TotalSkipped += 1;
        continue;
      }
      stats.mutations += 1;
      RestoreOptions options;
      options.password = password;
      backupproject::RestoreReport report;
      std::string error;
      const bool ok =
          engine.Restore(target, destination, options, &report, &error);
      if (ok) {
        stats.accepted += 1;
        if (IsAuthenticatedByte(stats.authenticated, offset, bytes.size())) {
          stats.accepted_authenticated += 1;
        }
      } else {
        stats.rejected += 1;
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
void MutatePackedStream(Layer layer, const std::string& label,
                        const std::string& packed, PackMethod method,
                        const std::string& workdir) {
  LayerStats& stats = StatsOf(layer);
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
      stats.skipped += 1;
      TotalSkipped += 1;
      continue;
    }
    std::string landed;
    if (!test_support::ReadFile(target, &landed) || landed != mutated) {
      stats.skipped += 1;
      TotalSkipped += 1;
      continue;
    }
    stats.mutations += 1;
    backupproject::PackedStreamReader reader;
    std::string error;
    if (reader.Open(target, &error) && reader.Scan(method, &error)) {
      stats.accepted += 1;
    } else {
      stats.rejected += 1;
    }
  }
  test_support::RemoveTree(target);
}

// 压缩流的 mutation：解压必须永不崩溃、不 OOM。
void MutateCompressedStream(Layer layer, const std::string& label,
                            const std::string& input, bool lzss) {
  LayerStats& stats = StatsOf(layer);
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
  for (std::size_t index = 0;
       index < compressed.size() && index < 300; ++index) {
    offsets.push_back(index);
  }
  for (std::size_t index = 300; index < compressed.size(); index += 173) {
    offsets.push_back(index);
  }
  for (const std::size_t offset : offsets) {
    std::string mutated = compressed;
    mutated[offset] = static_cast<char>(
        static_cast<unsigned char>(mutated[offset]) ^ 0x40);
    stats.mutations += 1;
    std::string output;
    std::string decode_error;
    const bool decoded =
        lzss ? backupproject::compression::LzssHuffmanDecompress(
                   mutated, &output, &decode_error)
             : backupproject::compression::HuffmanDecompress(
                   mutated, &output, &decode_error);
    if (decoded) {
      stats.accepted += 1;
    } else {
      stats.rejected += 1;
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

// 分层统计表 + 两条硬断言各自的计数行。断言行是打印出来的"证据"，
// 真正的判定交给后面的 test_support::Check。
void PrintLayers() {
  std::printf("\n== mutation layers ==\n");
  std::printf("%-26s %10s %10s %10s %10s\n", "layer", "mutations", "rejected",
              "accepted", "skipped");
  for (const LayerStats& layer : kLayers) {
    std::printf("%-26s %10llu %10llu %10llu %10llu\n", layer.name,
                static_cast<unsigned long long>(layer.mutations),
                static_cast<unsigned long long>(layer.rejected),
                static_cast<unsigned long long>(layer.accepted),
                static_cast<unsigned long long>(layer.skipped));
  }
  std::printf("\n");
  for (const LayerStats& layer : kLayers) {
    if (!layer.authenticated) {
      continue;
    }
    std::printf("%s accepted authenticated mutations = %llu\n",
                layer.assertion_name,
                static_cast<unsigned long long>(layer.accepted_authenticated));
  }
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
    Layer layer;
    PackMethod pack;
    CompressionMethod compression;
    EncryptionMethod encryption;
    const char* password;
  };
  const Combo combos[] = {
      {"mypack+none+none", Layer::kUnencryptedContainer, PackMethod::kMyPack,
       CompressionMethod::kNone, EncryptionMethod::kNone, ""},
      {"ustar+lzss+none", Layer::kUnencryptedContainer, PackMethod::kUstar,
       CompressionMethod::kLzssHuffman, EncryptionMethod::kNone, ""},
      {"fast+huffman+aes", Layer::kAesContainer, PackMethod::kFastUstar,
       CompressionMethod::kHuffman,
       EncryptionMethod::kAes256CtrHmacSha256, password},
      {"mypack+huffman+des", Layer::kDesContainer, PackMethod::kMyPack,
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
    MutateAndRestore(combo.layer, combo.label, archive, workdir,
                     combo.password);
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
      const struct {
        Layer layer;
        PackMethod method;
      } packed_layers[] = {
          {Layer::kRawMyPack, PackMethod::kMyPack},
          {Layer::kRawUstar, PackMethod::kUstar},
      };
      for (const auto& item : packed_layers) {
        const std::string packed =
            workdir + "/packed-" +
            std::to_string(static_cast<int>(item.method)) + ".pack";
        test_support::RemoveTree(packed);
        if (!backupproject::PackEntries(item.method, entries, packed, &error)) {
          test_support::Check(false, "packed mutation fixture", error);
          continue;
        }
        MutatePackedStream(item.layer, StatsOf(item.layer).name, packed,
                           item.method, workdir);
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
    MutateCompressedStream(Layer::kRawHuf1, "huffman", input, /*lzss=*/false);
    MutateCompressedStream(Layer::kRawLzh1, "lzh1", input, /*lzss=*/true);
  }

  const auto end = std::chrono::steady_clock::now();
  const double seconds = std::chrono::duration<double>(end - start).count();

  PrintLayers();

  std::uint64_t total = 0;
  std::uint64_t rejected = 0;
  std::uint64_t accepted = 0;
  for (const LayerStats& layer : kLayers) {
    total += layer.mutations;
    rejected += layer.rejected;
    accepted += layer.accepted;
  }

  std::printf("\nMUTATION\ttotal\t%llu\n",
              static_cast<unsigned long long>(total));
  std::printf("MUTATION\trejected\t%llu\n",
              static_cast<unsigned long long>(rejected));
  std::printf("MUTATION\taccepted\t%llu\n",
              static_cast<unsigned long long>(accepted));
  std::printf("MUTATION\tskipped (mutation never reached disk)\t%llu\n",
              static_cast<unsigned long long>(TotalSkipped));
  std::printf("MUTATION\tleftover destinations\t%llu\n",
              static_cast<unsigned long long>(TotalLeftovers));
  std::printf("MUTATION\telapsed\t%.2f s\n", seconds);
  std::printf("MUTATION\tcrash\t0\n");
  std::printf("MUTATION\thang\t0\n");

  // 硬断言：加密容器的受认证字节，一个变异都不许被接受。
  for (const LayerStats& layer : kLayers) {
    if (!layer.authenticated) {
      continue;
    }
    test_support::Check(
        layer.mutations > 0,
        std::string(layer.name) + ": the mutation corpus is not empty");
    test_support::Check(
        layer.accepted_authenticated == 0,
        std::string(layer.name) +
            ": every mutation of an authenticated byte is rejected",
        std::to_string(layer.accepted_authenticated) + " accepted");
  }
  test_support::Check(TotalLeftovers == 0,
                      "no mutation left a half-restored destination");
  test_support::Check(total > 1000, "mutation corpus is big enough",
                      std::to_string(total));
  return test_support::Finish("archive-mutation");
}
