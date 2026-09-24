// tests/unit/compression_bench.cpp
//
// 压缩模块基准：四类语料 × 两个编解码器，打印字节数、压缩率与编解码耗时。
//
// 语料全部在本地确定性生成，不依赖任何外部文件：
//   1. repetitive-text  一段话循环到 4 MiB
//   2. source-text      仓库自己的 include/*.h 与 docs 文本循环到 4 MiB
//                       （读不到就退回内置文本）
//   3. binary-pattern   512 字节伪随机块重复到 4 MiB
//   4. random-bytes     固定 seed 的 PRNG 生成 4 MiB
//
// 随机数据本来就不该变小，这里如实打印膨胀，不做任何粉饰。
// 每类语料都会顺手验证一次往返；只要有一类对不上就返回非零。

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#include "compression.h"

namespace comp = backupproject::compression;
namespace fs = std::filesystem;

namespace {

constexpr std::size_t kTargetSize = 4u << 20;  // 每类语料 4 MiB

struct Result {
  std::string name;
  std::size_t raw = 0;
  std::size_t huffman_size = 0;
  std::size_t lzh1_size = 0;
  double huffman_encode_ms = 0.0;
  double huffman_decode_ms = 0.0;
  double lzh1_encode_ms = 0.0;
  double lzh1_decode_ms = 0.0;
  bool round_trip_ok = false;
};

std::string Cycle(const std::string& unit, std::size_t target_size) {
  std::string text;
  text.reserve(target_size + unit.size());
  while (text.size() < target_size) {
    text += unit;
  }
  text.resize(target_size);
  return text;
}

std::string RandomBytes(std::uint32_t seed, std::size_t size) {
  std::mt19937 engine(seed);
  std::string data(size, '\0');
  for (std::size_t i = 0; i < size; ++i) {
    data[i] = static_cast<char>(engine() & 0xFFu);
  }
  return data;
}

const char* kBuiltinText() {
  return "the quick brown fox jumps over the lazy dog. "
         "a handwritten canonical huffman coder and an lzss front end, "
         "no third party compression library is linked. ";
}

void CollectFiles(const fs::path& root,
                  const std::vector<std::string>& extensions,
                  std::vector<fs::path>* files) {
  std::error_code error;
  if (!fs::is_directory(root, error)) {
    return;
  }
  for (fs::recursive_directory_iterator it(root, error), end; it != end;
       it.increment(error)) {
    if (error) {
      break;
    }
    if (!it->is_regular_file(error)) {
      continue;
    }
    const std::string extension = it->path().extension().string();
    if (std::find(extensions.begin(), extensions.end(), extension) !=
        extensions.end()) {
      files->push_back(it->path());
    }
  }
}

// 从仓库里凑一段普通文本：include/src 下的 C++ 源码 + docs 下的 markdown。
// 读不到（例如换了工作目录）就退回内置文本，基准照样能跑。
std::string LoadRepositoryText(const std::string& root) {
  std::vector<fs::path> files;
  CollectFiles(fs::path(root) / "include", {".h"}, &files);
  CollectFiles(fs::path(root) / "src", {".cpp"}, &files);
  CollectFiles(fs::path(root) / "docs", {".md"}, &files);
  std::sort(files.begin(), files.end());
  std::string text;
  for (const fs::path& path : files) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
      continue;
    }
    text.append(std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>());
    text.push_back('\n');
    if (text.size() >= kTargetSize) {
      break;
    }
  }
  if (text.size() < 1024) {
    std::printf("    (读不到仓库文本，退回内置文本)\n");
    return kBuiltinText();
  }
  std::printf("    (仓库文本 %zu 字节, 来自 %zu 个文件)\n", text.size(),
              files.size());
  return text;
}

template <typename Callable>
double TimeMilliseconds(Callable callable) {
  const auto start = std::chrono::steady_clock::now();
  callable();
  const auto stop = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::milli>(stop - start).count();
}

Result Measure(const std::string& name, const std::string& corpus) {
  Result result;
  result.name = name;
  result.raw = corpus.size();
  std::string error;
  std::string huffman;
  std::string lzh1;
  bool huffman_encode_ok = false;
  bool lzh1_encode_ok = false;
  result.huffman_encode_ms = TimeMilliseconds([&] {
    huffman_encode_ok = comp::HuffmanCompress(corpus, &huffman, &error);
  });
  result.lzh1_encode_ms = TimeMilliseconds([&] {
    lzh1_encode_ok = comp::LzssHuffmanCompress(corpus, &lzh1, &error);
  });
  if (!huffman_encode_ok || !lzh1_encode_ok) {
    std::printf("    %s: 压缩失败: %s\n", name.c_str(), error.c_str());
    return result;
  }
  result.huffman_size = huffman.size();
  result.lzh1_size = lzh1.size();

  std::string huffman_restored;
  std::string lzh1_restored;
  bool huffman_decode_ok = false;
  bool lzh1_decode_ok = false;
  result.huffman_decode_ms = TimeMilliseconds([&] {
    huffman_decode_ok =
        comp::HuffmanDecompress(huffman, &huffman_restored, &error);
  });
  result.lzh1_decode_ms = TimeMilliseconds([&] {
    lzh1_decode_ok = comp::LzssHuffmanDecompress(lzh1, &lzh1_restored, &error);
  });
  if (!huffman_decode_ok || !lzh1_decode_ok) {
    std::printf("    %s: 解压失败: %s\n", name.c_str(), error.c_str());
    return result;
  }
  result.round_trip_ok = huffman_restored == corpus && lzh1_restored == corpus;
  if (!result.round_trip_ok) {
    std::printf("    %s: 往返不一致\n", name.c_str());
  }
  return result;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string root = argc > 1 ? argv[1] : ".";
  const std::string repetitive =
      Cycle("abcabcabc backup backup backup compression compression test. ",
            kTargetSize);
  const std::string source = Cycle(LoadRepositoryText(root), kTargetSize);
  const std::string pattern = Cycle(RandomBytes(2024, 512), kTargetSize);
  const std::string random = RandomBytes(424242, kTargetSize);

  std::vector<Result> results;
  results.push_back(Measure("repetitive-text", repetitive));
  results.push_back(Measure("source-text", source));
  results.push_back(Measure("binary-pattern", pattern));
  results.push_back(Measure("random-bytes", random));

  std::printf("\n");
  std::printf(
      "corpus\traw bytes\thuffman bytes\thuffman ratio\tlzh1 bytes\tlzh1 "
      "ratio\n");
  for (const Result& result : results) {
    const double huffman_ratio =
        result.raw == 0 ? 0.0
                        : static_cast<double>(result.huffman_size) /
                              static_cast<double>(result.raw);
    const double lzh1_ratio = result.raw == 0
                                  ? 0.0
                                  : static_cast<double>(result.lzh1_size) /
                                        static_cast<double>(result.raw);
    std::printf("%s\t%zu\t%zu\t%.4f\t%zu\t%.4f\n", result.name.c_str(),
                result.raw, result.huffman_size, huffman_ratio,
                result.lzh1_size, lzh1_ratio);
  }
  std::printf("\n");
  std::printf(
      "corpus\thuffman encode ms\thuffman decode ms\tlzh1 encode ms\tlzh1 "
      "decode ms\n");
  for (const Result& result : results) {
    std::printf("%s\t%.1f\t%.1f\t%.1f\t%.1f\n", result.name.c_str(),
                result.huffman_encode_ms, result.huffman_decode_ms,
                result.lzh1_encode_ms, result.lzh1_decode_ms);
  }

  int passed = 0;
  for (const Result& result : results) {
    if (result.round_trip_ok) {
      ++passed;
    }
  }
  std::printf("\ncompression_bench: %d/%zu corpora round-tripped\n", passed,
              results.size());
  return passed == static_cast<int>(results.size()) ? 0 : 1;
}
