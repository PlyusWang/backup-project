// tests/unit/compression_test.cpp
//
// 压缩模块单元测试：Canonical Huffman（HUF1）与 LZSS + Canonical
// Huffman（LZH1）。
//
// 每个用例打印一行 PASS/FAIL，用例内部每条断言失败都会额外打印一行；
// 只要有一个用例失败，进程就返回非零。最后的汇总行
// "compression_test: N/M checks passed" 由 scripts/compression_test.sh 透传成
// "compression: N/M checks passed"。
//
// 坏流用例只要求"返回 false 且不崩"：崩溃由 ASan/UBSan 那一轮负责抓，
// 这里只管返回值与不写脏输出。

#include "compression.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace comp = backupproject::compression;

namespace {

int g_cases = 0;
int g_case_failed = 0;
int g_assertions = 0;
int g_assertion_failed = 0;
bool g_case_ok = true;

void Check(bool condition, const char* expression, int line) {
  ++g_assertions;
  if (!condition) {
    ++g_assertion_failed;
    g_case_ok = false;
    std::printf("    CHECK FAILED (line %d): %s\n", line, expression);
  }
}

#define CHECK(expression) Check((expression), #expression, __LINE__)

void ReportCase(const char* name) {
  ++g_cases;
  if (g_case_ok) {
    std::printf("PASS  %s\n", name);
  } else {
    ++g_case_failed;
    std::printf("FAIL  %s\n", name);
  }
  g_case_ok = true;
}

// ---- 语料 ---------------------------------------------------------------

std::string Repeat(const std::string& unit, std::size_t target_size) {
  std::string text;
  text.reserve(target_size + unit.size());
  while (text.size() < target_size) {
    text += unit;
  }
  text.resize(target_size);
  return text;
}

// 固定 seed 的确定性 PRNG，只用来生成测试数据。
std::string RandomBytes(std::uint32_t seed, std::size_t size) {
  std::mt19937 engine(seed);
  std::string data(size, '\0');
  for (std::size_t i = 0; i < size; ++i) {
    data[i] = static_cast<char>(engine() & 0xFFu);
  }
  return data;
}

std::string AllSymbolsInput() {
  std::string data;
  for (int round = 0; round < 5; ++round) {
    for (int symbol = 0; symbol < 256; ++symbol) {
      data.push_back(static_cast<char>(symbol));
    }
  }
  return data;
}

const char* kFixedText() {
  return "the quick brown fox jumps over the lazy dog. "
         "backup project compression module writes its own huffman and lzss; "
         "no third party library is linked, only the c++17 standard library. ";
}

// ---- 头部读写（用来构造和篡改流）----------------------------------------

std::uint64_t LoadLittleEndian64(const std::string& data, std::size_t offset) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    const unsigned char byte =
        static_cast<unsigned char>(data[offset + static_cast<std::size_t>(i)]);
    value |= static_cast<std::uint64_t>(byte) << (8 * i);
  }
  return value;
}

void StoreLittleEndian64(std::string* data, std::size_t offset,
                         std::uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    (*data)[offset + static_cast<std::size_t>(i)] =
        static_cast<char>((value >> (8 * i)) & 0xFF);
  }
}

std::string MakeHuffmanStream(std::uint64_t original_size,
                              std::uint64_t bit_count,
                              const std::vector<std::uint32_t>& lengths,
                              const std::string& payload) {
  std::string stream(comp::kHuffmanHeaderSize, '\0');
  std::memcpy(&stream[0], "HUF1", 4);
  StoreLittleEndian64(&stream, 4, original_size);
  for (std::size_t symbol = 0; symbol < lengths.size(); ++symbol) {
    stream[12 + symbol] = static_cast<char>(lengths[symbol]);
  }
  StoreLittleEndian64(&stream, 268, bit_count);
  stream += payload;
  return stream;
}

// 从码长字段重算 Kraft 和（放大 2^32 倍），确认流里的码长确实构成前缀码。
std::uint64_t KraftSum(const std::string& stream) {
  std::uint64_t kraft = 0;
  for (std::size_t symbol = 0; symbol < 256; ++symbol) {
    const std::uint32_t length =
        static_cast<unsigned char>(stream[12 + symbol]);
    if (length > 0) {
      kraft += std::uint64_t{1} << (comp::kMaxCodeLength - length);
    }
  }
  return kraft;
}

// 走一遍 control byte 数出 token 个数。只对合法流使用。
std::size_t CountTokens(const std::string& tokens) {
  std::size_t cursor = 0;
  std::size_t count = 0;
  while (cursor < tokens.size()) {
    const std::uint32_t control = static_cast<unsigned char>(tokens[cursor++]);
    for (int bit = 7; bit >= 0 && cursor < tokens.size(); --bit) {
      cursor += ((control >> bit) & 1u) != 0 ? 1 : 3;
      ++count;
    }
  }
  return count;
}

// ---- 往返 helpers -------------------------------------------------------

bool HuffmanRoundTrip(const std::string& input, const char* what,
                      std::size_t* compressed_size) {
  std::string error;
  std::string compressed;
  if (!comp::HuffmanCompress(input, &compressed, &error)) {
    std::printf("    HuffmanCompress(%s) 失败: %s\n", what, error.c_str());
    return false;
  }
  std::string restored;
  if (!comp::HuffmanDecompress(compressed, &restored, &error)) {
    std::printf("    HuffmanDecompress(%s) 失败: %s\n", what, error.c_str());
    return false;
  }
  if (restored != input) {
    std::printf("    Huffman 往返不一致(%s): %zu -> %zu\n", what, input.size(),
                restored.size());
    return false;
  }
  if (compressed_size != nullptr) {
    *compressed_size = compressed.size();
  }
  return true;
}

bool LzssRoundTrip(const std::string& input, const char* what) {
  std::string error;
  std::string tokens;
  if (!comp::LzssEncode(input, &tokens, &error)) {
    std::printf("    LzssEncode(%s) 失败: %s\n", what, error.c_str());
    return false;
  }
  std::string restored;
  if (!comp::LzssDecode(tokens, input.size(), &restored, &error)) {
    std::printf("    LzssDecode(%s) 失败: %s\n", what, error.c_str());
    return false;
  }
  if (restored != input) {
    std::printf("    LZSS 往返不一致(%s): %zu -> %zu\n", what, input.size(),
                restored.size());
    return false;
  }
  return true;
}

bool LzssHuffmanRoundTrip(const std::string& input, const char* what,
                          std::size_t* compressed_size) {
  std::string error;
  std::string compressed;
  if (!comp::LzssHuffmanCompress(input, &compressed, &error)) {
    std::printf("    LzssHuffmanCompress(%s) 失败: %s\n", what, error.c_str());
    return false;
  }
  std::string restored;
  if (!comp::LzssHuffmanDecompress(compressed, &restored, &error)) {
    std::printf("    LzssHuffmanDecompress(%s) 失败: %s\n", what,
                error.c_str());
    return false;
  }
  if (restored != input) {
    std::printf("    LZH1 往返不一致(%s): %zu -> %zu\n", what, input.size(),
                restored.size());
    return false;
  }
  if (compressed_size != nullptr) {
    *compressed_size = compressed.size();
  }
  return true;
}

// 一个输入同时走通三个方向：HUF1、裸 LZSS、LZH1。
bool FullRoundTrip(const std::string& input, const char* what,
                   std::size_t* huffman_size, std::size_t* lzh1_size) {
  bool ok = HuffmanRoundTrip(input, what, huffman_size);
  ok = LzssRoundTrip(input, what) && ok;
  ok = LzssHuffmanRoundTrip(input, what, lzh1_size) && ok;
  return ok;
}

bool RejectsHuffman(const std::string& stream, const char* what) {
  std::string output = "stale";
  std::string error;
  if (comp::HuffmanDecompress(stream, &output, &error)) {
    std::printf("    坏 HUF1 流被接受: %s\n", what);
    return false;
  }
  if (output != "stale") {
    std::printf("    坏 HUF1 流改写了输出: %s\n", what);
    return false;
  }
  return true;
}

bool RejectsLzss(const std::string& tokens, std::uint64_t original_size,
                 const char* what) {
  std::string output = "stale";
  std::string error;
  if (comp::LzssDecode(tokens, original_size, &output, &error)) {
    std::printf("    坏 token stream 被接受: %s\n", what);
    return false;
  }
  if (output != "stale") {
    std::printf("    坏 token stream 改写了输出: %s\n", what);
    return false;
  }
  return true;
}

bool RejectsLzh1(const std::string& stream, const char* what) {
  std::string output = "stale";
  std::string error;
  if (comp::LzssHuffmanDecompress(stream, &output, &error)) {
    std::printf("    坏 LZH1 流被接受: %s\n", what);
    return false;
  }
  return output == "stale";
}

// ---- 参考实现：只用来证明"某个分布不修正就会超过 32 bit" ----------------

std::size_t ReferenceHuffmanMaxDepth(std::vector<std::uint64_t> weights) {
  struct Item {
    std::uint64_t weight;
    std::size_t depth;
  };
  std::vector<Item> items;
  for (std::uint64_t weight : weights) {
    if (weight != 0) {
      items.push_back(Item{weight, 1});
    }
  }
  if (items.size() < 2) {
    return items.empty() ? 0 : 1;
  }
  while (items.size() > 1) {
    std::size_t first = 0;
    std::size_t second = 1;
    if (items[second].weight < items[first].weight) {
      std::swap(first, second);
    }
    for (std::size_t i = 2; i < items.size(); ++i) {
      if (items[i].weight < items[first].weight) {
        second = first;
        first = i;
      } else if (items[i].weight < items[second].weight) {
        second = i;
      }
    }
    Item merged;
    merged.weight = items[first].weight + items[second].weight;
    merged.depth = std::max(items[first].depth, items[second].depth) + 1;
    const std::size_t high = std::max(first, second);
    const std::size_t low = std::min(first, second);
    items.erase(items.begin() + static_cast<std::ptrdiff_t>(high));
    items.erase(items.begin() + static_cast<std::ptrdiff_t>(low));
    items.push_back(merged);
  }
  return items[0].depth;
}

// ---- 用例 ---------------------------------------------------------------

void HuffmanEmpty() {
  std::string error;
  std::string compressed;
  CHECK(comp::HuffmanCompress("", &compressed, &error));
  // 空输入的流必须正好是头部：original_size = 0、bit_count = 0、没有
  // bitstream。
  CHECK(compressed.size() == comp::kHuffmanHeaderSize);
  CHECK(std::memcmp(compressed.data(), "HUF1", 4) == 0);
  CHECK(LoadLittleEndian64(compressed, 4) == 0);
  CHECK(LoadLittleEndian64(compressed, 268) == 0);
  bool all_zero = true;
  for (std::size_t symbol = 0; symbol < 256; ++symbol) {
    if (compressed[12 + symbol] != '\0') {
      all_zero = false;
    }
  }
  CHECK(all_zero);
  CHECK(comp::LooksLikeHuffman(compressed));
  std::string restored = "stale";
  CHECK(comp::HuffmanDecompress(compressed, &restored, &error));
  CHECK(restored.empty());
  ReportCase("huffman/empty");
}

void HuffmanSingleSymbol() {
  const std::string input(100000, 'a');
  std::string error;
  std::string compressed;
  CHECK(comp::HuffmanCompress(input, &compressed, &error));
  // 单符号必须拿到 1 bit 的码，不能是 0 bit。
  CHECK(static_cast<unsigned char>(compressed[12 + 'a']) == 1);
  CHECK(LoadLittleEndian64(compressed, 268) == 100000);
  std::size_t size = 0;
  CHECK(HuffmanRoundTrip(input, "100000 x a", &size));
  // 1 bit/符号：100000 bit = 12500 字节，加上 276 字节头部。
  CHECK(size == comp::kHuffmanHeaderSize + 12500);
  CHECK(LzssRoundTrip(input, "100000 x a"));
  ReportCase("huffman/single-symbol");
}

void HuffmanAllSymbols() {
  const std::string input = AllSymbolsInput();
  std::size_t size = 0;
  CHECK(HuffmanRoundTrip(input, "all 256 symbols", &size));
  // 256 种符号各出现 5 次，等概率：每种 8 bit。
  CHECK(size == comp::kHuffmanHeaderSize + input.size());
  ReportCase("huffman/all-256-symbols");
}

void HuffmanSkewed() {
  std::string input;
  input.reserve(200000);
  std::mt19937 engine(20240925);
  for (std::size_t i = 0; i < 200000; ++i) {
    if (engine() % 100 < 95) {
      input.push_back('a');
    } else {
      input.push_back(static_cast<char>(engine() % 256));
    }
  }
  std::size_t size = 0;
  CHECK(HuffmanRoundTrip(input, "95% a", &size));
  CHECK(size < input.size() / 2);
  std::printf("    偏斜分布: %zu -> %zu\n", input.size(), size);
  ReportCase("huffman/skewed-distribution");
}

void HuffmanRandomBinary() {
  const std::string input = RandomBytes(12345, 262144);
  std::size_t size = 0;
  CHECK(HuffmanRoundTrip(input, "random", &size));
  // 随机数据不允许变小，但也不该离谱膨胀（头部 + 每符号约 8.0 bit）。
  std::printf("    随机数据: %zu -> %zu (%.4f)\n", input.size(), size,
              static_cast<double>(size) / static_cast<double>(input.size()));
  CHECK(size > input.size());
  ReportCase("huffman/random-binary");
}

void HuffmanManyZeros() {
  std::string input(300000, '\0');
  for (std::size_t i = 0; i < input.size(); i += 7) {
    input[i] = static_cast<char>(i % 251);
  }
  std::size_t size = 0;
  CHECK(HuffmanRoundTrip(input, "many zeros", &size));
  CHECK(FullRoundTrip(input, "many zeros", nullptr, nullptr));
  ReportCase("huffman/many-zeros");
}

void HuffmanLargeText() {
  const std::string input = Repeat(kFixedText(), 4u << 20);
  CHECK(input.size() >= (4u << 20));
  std::size_t size = 0;
  CHECK(HuffmanRoundTrip(input, "4 MiB text", &size));
  CHECK(size < input.size());
  std::printf("    4 MiB 文本: %zu -> %zu (%.4f)\n", input.size(), size,
              static_cast<double>(size) / static_cast<double>(input.size()));
  ReportCase("huffman/large-text");
}

void HuffmanDeterministic() {
  const std::string input = Repeat(kFixedText(), 40000);
  std::string error;
  std::string first;
  CHECK(comp::HuffmanCompress(input, &first, &error));
  for (int round = 0; round < 2; ++round) {
    std::string again;
    CHECK(comp::HuffmanCompress(input, &again, &error));
    CHECK(again == first);
  }
  ReportCase("huffman/deterministic");
}

void HuffmanCompressionRatio() {
  // 高度重复文本：一字节一符号的 Huffman 靠字符分布就能压到 60% 以下。
  const std::string input = Repeat(
      "backup backup backup compression compression compression "
      "test test test test test test test. ",
      1u << 20);
  std::size_t huffman_size = 0;
  std::size_t lzh1_size = 0;
  CHECK(FullRoundTrip(input, "repetitive text", &huffman_size, &lzh1_size));
  const double huffman_ratio =
      static_cast<double>(huffman_size) / static_cast<double>(input.size());
  const double lzh1_ratio =
      static_cast<double>(lzh1_size) / static_cast<double>(input.size());
  std::printf("    重复文本 %zu 字节: huffman %.4f, lzh1 %.4f\n", input.size(),
              huffman_ratio, lzh1_ratio);
  CHECK(huffman_ratio < 0.6);
  CHECK(lzh1_ratio < huffman_ratio);
  ReportCase("huffman/compression-ratio");
}

void HuffmanCodeLengthLimit() {
  // Fibonacci 频率是 Huffman 树最扁的输入：n 种符号的最深叶子深度大约是 n。
  // 逐步加符号，直到参考实现算出"不修正就会超过 32 bit"的规模。
  std::vector<std::uint64_t> weights;
  std::uint64_t previous = 1;
  std::uint64_t current = 1;
  for (int i = 0; i < 40; ++i) {
    weights.push_back(previous);
    const std::uint64_t next = previous + current;
    previous = current;
    current = next;
  }
  std::size_t used = 30;
  std::size_t reference_depth = 0;
  while (used < weights.size()) {
    const std::vector<std::uint64_t> candidate(
        weights.begin(), weights.begin() + static_cast<std::ptrdiff_t>(used));
    reference_depth = ReferenceHuffmanMaxDepth(candidate);
    if (reference_depth > comp::kMaxCodeLength) {
      break;
    }
    ++used;
  }
  CHECK(reference_depth > comp::kMaxCodeLength);
  std::string input;
  for (std::size_t symbol = 0; symbol < used; ++symbol) {
    input.append(static_cast<std::size_t>(weights[symbol]),
                 static_cast<char>(symbol));
  }
  std::printf("    fibonacci 语料: %zu 种符号, %zu 字节, 未修正深度 %zu\n",
              used, input.size(), reference_depth);
  std::string error;
  std::string compressed;
  CHECK(comp::HuffmanCompress(input, &compressed, &error));
  std::uint32_t max_length = 0;
  std::uint32_t min_length = comp::kMaxCodeLength;
  for (std::size_t symbol = 0; symbol < 256; ++symbol) {
    const std::uint32_t length =
        static_cast<unsigned char>(compressed[12 + symbol]);
    if (length == 0) {
      continue;
    }
    max_length = std::max(max_length, length);
    min_length = std::min(min_length, length);
  }
  std::printf("    修正后码长: min %u, max %u\n", min_length, max_length);
  CHECK(max_length <= comp::kMaxCodeLength);
  CHECK(max_length == comp::kMaxCodeLength);
  CHECK(min_length >= 1);
  CHECK(KraftSum(compressed) <= (std::uint64_t{1} << comp::kMaxCodeLength));
  std::size_t size = 0;
  CHECK(HuffmanRoundTrip(input, "fibonacci", &size));
  ReportCase("huffman/extreme-skew-code-length-limit");
}

void HuffmanRejectsBadStreams() {
  const std::string sample = Repeat("hello huffman world. ", 500);
  std::string error;
  std::string good;
  CHECK(comp::HuffmanCompress(sample, &good, &error));
  CHECK(HuffmanRoundTrip(sample, "sample", nullptr));

  std::string bad = good;
  bad[0] = 'X';
  CHECK(RejectsHuffman(bad, "magic 错"));

  bad = good;
  bad.resize(good.size() - 1);
  CHECK(RejectsHuffman(bad, "截断 1 字节"));

  bad = good;
  bad.push_back(static_cast<char>(0x7F));
  CHECK(RejectsHuffman(bad, "追加 1 字节垃圾"));

  // 码长 > 32：挑一个样例里没出现过的符号，只动它的码长字段。
  CHECK(static_cast<unsigned char>(good[12 + 7]) == 0);
  bad = good;
  bad[12 + 7] = 33;
  CHECK(RejectsHuffman(bad, "码长 = 33"));

  // Kraft 和不成立：256 个码长全设成 1（2^-1 * 256 = 128 > 1）。
  bad = good;
  for (std::size_t symbol = 0; symbol < 256; ++symbol) {
    bad[12 + symbol] = 1;
  }
  CHECK(KraftSum(bad) > (std::uint64_t{1} << comp::kMaxCodeLength));
  CHECK(RejectsHuffman(bad, "256 个码长全 1"));

  bad = good;
  StoreLittleEndian64(&bad, 268, 0xFFFFFFFFFFFFFFFFull);
  CHECK(RejectsHuffman(bad, "bit_count = 2^64-1 但流很短"));

  bad = good;
  StoreLittleEndian64(&bad, 4, sample.size() / 2);
  CHECK(RejectsHuffman(bad, "original_size 改小"));

  // 只有 1 个非零码长，但那个码长是 3。
  std::vector<std::uint32_t> lengths(256, 0);
  lengths[static_cast<std::size_t>('a')] = 3;
  CHECK(RejectsHuffman(MakeHuffmanStream(10, 30, lengths, std::string(4, '\0')),
                       "单符号码长 = 3"));

  // 256 个码长全 0，却声明 original_size > 0。
  lengths.assign(256, 0);
  CHECK(RejectsHuffman(MakeHuffmanStream(100, 0, lengths, ""),
                       "全 0 码长 + original_size > 0"));

  CHECK(RejectsHuffman(std::string(100, '\0'), "流短于 276 字节"));
  CHECK(RejectsHuffman("", "空流"));
  CHECK(RejectsHuffman("HUF1", "只有 magic"));
  ReportCase("huffman/rejects-bad-streams");
}

void LzssRoundTrips() {
  const std::string one = "a";
  const std::string two = "ab";
  const std::string three = "abc";
  CHECK(FullRoundTrip("", "empty", nullptr, nullptr));
  CHECK(FullRoundTrip(one, "1 byte", nullptr, nullptr));
  CHECK(FullRoundTrip(two, "2 bytes", nullptr, nullptr));
  CHECK(FullRoundTrip(three, "3 bytes", nullptr, nullptr));
  CHECK(
      FullRoundTrip(std::string(100000, 'a'), "100000 x a", nullptr, nullptr));
  CHECK(FullRoundTrip(RandomBytes(777, 200000), "random", nullptr, nullptr));
  CHECK(FullRoundTrip(Repeat(kFixedText(), 200000), "text", nullptr, nullptr));

  // 长匹配与短匹配混在一起：文本 + 随机碎片 + 长重复段 + 短周期重复。
  std::string mixed = Repeat(kFixedText(), 200);
  mixed += RandomBytes(4242, 5000);
  mixed += std::string(5000, 'z');
  mixed += "abcabcabcabc";
  mixed += RandomBytes(99, 300);
  mixed += Repeat("xy", 3000);
  CHECK(FullRoundTrip(mixed, "mixed", nullptr, nullptr));
  ReportCase("lzss/roundtrip");
}

void LzssOverlap() {
  const std::string input(100000, 'a');
  std::string error;
  std::string tokens;
  CHECK(comp::LzssEncode(input, &tokens, &error));
  const std::size_t token_count = CountTokens(tokens);
  std::printf("    100000 x a -> %zu 字节 token stream, %zu 个 token\n",
              tokens.size(), token_count);
  // 第一个 token 是 literal 'a'，之后全是 distance = 1 的 258 字节匹配：
  // 1 + ceil(99999 / 258) = 389 个 token。
  CHECK(token_count <= 400);
  CHECK(tokens.size() < input.size() / 50);
  CHECK(static_cast<unsigned char>(tokens[0]) ==
        0x80);  // 只有第 1 个是 literal
  CHECK(tokens[1] == 'a');
  CHECK(static_cast<unsigned char>(tokens[2]) == 0x01);  // distance = 1
  CHECK(static_cast<unsigned char>(tokens[3]) == 0x00);
  CHECK(static_cast<unsigned char>(tokens[4]) == 0xFF);  // length_minus_3 = 255
  std::string restored;
  CHECK(comp::LzssDecode(tokens, input.size(), &restored, &error));
  CHECK(restored == input);
  // 短周期重复（distance < length 的重叠复制）也要覆盖。
  CHECK(LzssRoundTrip(std::string(5000, 'q'), "5000 x q"));
  ReportCase("lzss/overlap");
}

void LzssRejectsBadStreams() {
  // distance = 0
  std::string tokens;
  tokens.push_back('\0');
  tokens.push_back('\0');
  tokens.push_back('\0');
  tokens.push_back('\0');
  CHECK(RejectsLzss(tokens, 10, "distance = 0"));

  // distance 超过已经产出的字节数：先 1 个 literal，再要 distance = 5 的匹配。
  tokens.clear();
  tokens.push_back(static_cast<char>(0x80));
  tokens.push_back('a');
  tokens.push_back(static_cast<char>(0x05));
  tokens.push_back('\0');
  tokens.push_back('\0');
  CHECK(RejectsLzss(tokens, 10, "distance > 已产出"));

  // distance > 32768
  tokens.clear();
  tokens.push_back('\0');
  tokens.push_back(static_cast<char>(0x40));  // 0x9C40 = 40000
  tokens.push_back(static_cast<char>(0x9C));
  tokens.push_back('\0');
  CHECK(RejectsLzss(tokens, 100000, "distance = 40000"));

  // 输出会超过 original_size：2 个 literal + 一个 length = 8 的匹配，却只给 3
  // 字节。
  tokens.clear();
  tokens.push_back(static_cast<char>(0xC0));
  tokens.push_back('a');
  tokens.push_back('b');
  tokens.push_back(static_cast<char>(0x02));  // distance = 2
  tokens.push_back('\0');
  tokens.push_back(static_cast<char>(0x05));  // length = 8
  CHECK(RejectsLzss(tokens, 3, "输出超过 original_size"));

  // 截断：只有 control byte。
  CHECK(RejectsLzss(std::string(1, '\0'), 10, "只有 control byte"));
  // 截断：literal 少操作数。
  CHECK(RejectsLzss(std::string(1, static_cast<char>(0x80)), 10,
                    "literal 少操作数"));
  // 截断：match 少操作数。
  tokens.assign(1, '\0');
  tokens.push_back(static_cast<char>(0x01));
  tokens.push_back('\0');
  CHECK(RejectsLzss(tokens, 10, "match 少操作数"));
  // 空 token stream 但要求输出。
  CHECK(RejectsLzss("", 10, "空 token stream"));
  // original_size = 0 却带着 token。
  tokens.clear();
  tokens.push_back(static_cast<char>(0x80));
  tokens.push_back('a');
  CHECK(RejectsLzss(tokens, 0, "original_size = 0 却有多余 token"));

  // 输出刚好等于 original_size 之后 token stream 还有剩余。
  tokens.clear();
  tokens.push_back(static_cast<char>(0xC0));
  tokens.push_back('a');
  tokens.push_back('b');
  CHECK(RejectsLzss(tokens, 1, "original_size 之后还有 token"));
  ReportCase("lzss/rejects-bad-streams");
}

void Lzh1HeaderConsistency() {
  const std::string input = Repeat(kFixedText(), 500);
  std::string error;
  std::string good;
  CHECK(comp::LzssHuffmanCompress(input, &good, &error));
  CHECK(std::memcmp(good.data(), "LZH1", 4) == 0);
  CHECK(LoadLittleEndian64(good, 4) == input.size());
  const std::uint64_t token_stream_size = LoadLittleEndian64(good, 12);
  CHECK(token_stream_size > 0);
  // 外层总长度必须正好等于 20 + 内层 HUF1 流长度，内层长度字段也要对得上。
  const std::string inner = good.substr(comp::kLzssHuffmanHeaderSize);
  CHECK(good.size() == comp::kLzssHuffmanHeaderSize + inner.size());
  CHECK(comp::LooksLikeHuffman(inner));
  CHECK(LoadLittleEndian64(inner, 4) == token_stream_size);

  std::string bad = good;
  StoreLittleEndian64(&bad, 12, token_stream_size + 1);
  CHECK(RejectsLzh1(bad, "token_stream_size 比实际大 1"));

  bad = good;
  StoreLittleEndian64(&bad, 12, token_stream_size - 1);
  CHECK(RejectsLzh1(bad, "token_stream_size 比实际小 1"));

  bad = good;
  StoreLittleEndian64(&bad, 4, input.size() + 5);
  CHECK(RejectsLzh1(bad, "original_size 与实际不符"));

  bad = good;
  bad[0] = 'X';
  CHECK(RejectsLzh1(bad, "外层 magic 错"));

  bad = good;
  bad[20] = 'X';
  CHECK(RejectsLzh1(bad, "内层 HUF1 magic 损坏"));

  bad = good;
  bad[20 + 268 + 7] = static_cast<char>(0x7F);
  CHECK(RejectsLzh1(bad, "内层 bit_count 损坏"));

  bad = good;
  bad.resize(good.size() - 1);
  CHECK(RejectsLzh1(bad, "外层截断 1 字节"));

  bad = good;
  bad.push_back('x');
  CHECK(RejectsLzh1(bad, "外层追加 1 字节"));

  CHECK(RejectsLzh1(std::string(10, '\0'), "流短于 20 字节"));
  CHECK(RejectsLzh1("LZH1", "只有 magic"));
  ReportCase("lzh1/header-consistency");
}

void Lzh1Deterministic() {
  std::string mixed = Repeat(kFixedText(), 3000);
  mixed += RandomBytes(31337, 20000);
  std::string first_tokens;
  std::string first_lzh1;
  std::string error;
  CHECK(comp::LzssEncode(mixed, &first_tokens, &error));
  CHECK(comp::LzssHuffmanCompress(mixed, &first_lzh1, &error));
  for (int round = 0; round < 2; ++round) {
    std::string tokens;
    std::string lzh1;
    CHECK(comp::LzssEncode(mixed, &tokens, &error));
    CHECK(comp::LzssHuffmanCompress(mixed, &lzh1, &error));
    CHECK(tokens == first_tokens);
    CHECK(lzh1 == first_lzh1);
  }
  ReportCase("lzh1/deterministic");
}

void Lzh1LargeRoundTrip() {
  const std::string input = Repeat(kFixedText(), 4u << 20);
  std::size_t lzh1_size = 0;
  CHECK(LzssHuffmanRoundTrip(input, "4 MiB text", &lzh1_size));
  std::printf(
      "    4 MiB 文本 LZH1: %zu -> %zu (%.4f)\n", input.size(), lzh1_size,
      static_cast<double>(lzh1_size) / static_cast<double>(input.size()));
  CHECK(lzh1_size < input.size() / 10);
  ReportCase("lzh1/large-roundtrip");
}

void HeaderProbe() {
  const std::string small = Repeat(kFixedText(), 20);
  std::string error;
  std::string huffman;
  std::string lzh1;
  CHECK(comp::HuffmanCompress(small, &huffman, &error));
  CHECK(comp::LzssHuffmanCompress(small, &lzh1, &error));
  CHECK(comp::LooksLikeHuffman(huffman));
  CHECK(comp::LooksLikeLzssHuffman(lzh1));
  CHECK(!comp::LooksLikeLzssHuffman(huffman));
  CHECK(!comp::LooksLikeHuffman(lzh1));
  CHECK(!comp::LooksLikeHuffman(""));
  CHECK(!comp::LooksLikeLzssHuffman(""));
  CHECK(!comp::LooksLikeHuffman("HUF1"));
  CHECK(!comp::LooksLikeHuffman(std::string()));
  std::string empty_stream;
  CHECK(comp::HuffmanCompress("", &empty_stream, &error));
  CHECK(comp::LooksLikeHuffman(empty_stream));

  std::string bad = huffman;
  bad[13] = static_cast<char>(0xFF);  // 码长被破坏（> 32），长度字段仍然自洽
  CHECK(!comp::LooksLikeHuffman(bad));
  bad = huffman;
  bad.push_back('x');
  CHECK(!comp::LooksLikeHuffman(bad));
  bad = lzh1;
  StoreLittleEndian64(&bad, 12, 1);
  CHECK(!comp::LooksLikeLzssHuffman(bad));
  ReportCase("header/probe");
}

}  // namespace

int main() {
  HuffmanEmpty();
  HuffmanSingleSymbol();
  HuffmanAllSymbols();
  HuffmanSkewed();
  HuffmanRandomBinary();
  HuffmanManyZeros();
  HuffmanLargeText();
  HuffmanDeterministic();
  HuffmanCompressionRatio();
  HuffmanCodeLengthLimit();
  HuffmanRejectsBadStreams();
  LzssRoundTrips();
  LzssOverlap();
  LzssRejectsBadStreams();
  Lzh1HeaderConsistency();
  Lzh1Deterministic();
  Lzh1LargeRoundTrip();
  HeaderProbe();

  const int passed = g_cases - g_case_failed;
  std::printf("compression_test: %d/%d checks passed", passed, g_cases);
  if (g_assertion_failed != 0) {
    std::printf(" (%d/%d assertions failed)", g_assertion_failed, g_assertions);
  }
  std::printf("\n");
  return g_case_failed == 0 ? 0 : 1;
}
