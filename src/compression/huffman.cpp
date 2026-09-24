// huffman.cpp
//
// Canonical Huffman（HUF1）。整个文件只有三段：
//
//   编码：统计频率 → 建 Huffman 树得到码长 → 把超过 32 bit 的码长修正回来
//         → 按 (码长, 符号) 升序分配 canonical 码字 → 逐比特写进 bitstream。
//   解码：校验头部（magic、码长上限、单符号特例、Kraft 和、精确的流长度）
//         → 用同一套 canonical 规则重建"每个码长段的起始码字" → 逐比特解码。
//   探测：LooksLikeHuffman 只做头部自洽性检查，不解压。
//
// 确定性：所有遍历都按符号 0..255 升序，建树时用"子树里最小的符号"打破权重
// 平局，因此同一输入永远得到 byte-for-byte 相同的流。这里不序列化指针，也不
// 依赖任何无序容器的迭代顺序。
//
// 边界：空输入 → original_size = 0、bit_count = 0、256 个码长全 0、没有
// bitstream；只出现 1 种符号 → 该符号码长固定为 1（0 bit 读不出任何东西，也会
// 让解码端无法确定符号）。失败时 *output 保持原样，只有 *error_message 被写。

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#include "compression.h"

namespace backupproject {
namespace compression {

namespace {

constexpr std::size_t kSymbolCount = 256;
constexpr char kHuffmanMagic[4] = {'H', 'U', 'F', '1'};

// 坏头部里的 original_size 可以是任意 64 bit 值，预留内存前先夹住：
// 真正需要的内存由后面的解码循环按需增长，不会因为一个字段就分配几个 GB。
constexpr std::uint64_t kMaxReserveBytes = std::uint64_t{1} << 26;

void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) {
    *error_message = text;
  }
}

std::uint8_t ByteAt(const std::string& data, std::size_t index) {
  return static_cast<std::uint8_t>(data[index]);
}

// 头部所有多字节字段都是 little-endian，这里不假设主机字节序。
void AppendLittleEndian64(std::uint64_t value, std::string* out) {
  for (int shift = 0; shift < 64; shift += 8) {
    out->push_back(static_cast<char>((value >> shift) & 0xFF));
  }
}

std::uint64_t ReadLittleEndian64(const std::string& data, std::size_t offset) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(
                 ByteAt(data, offset + static_cast<std::size_t>(i)))
             << (8 * i);
  }
  return value;
}

struct HuffmanHeader {
  std::uint64_t original_size = 0;
  std::uint64_t bit_count = 0;
  std::array<std::uint32_t, kSymbolCount> lengths{};
};

bool ParseHuffmanHeader(const std::string& data, HuffmanHeader* header) {
  if (data.size() < kHuffmanHeaderSize) {
    return false;
  }
  if (std::memcmp(data.data(), kHuffmanMagic, sizeof(kHuffmanMagic)) != 0) {
    return false;
  }
  header->original_size = ReadLittleEndian64(data, 4);
  for (std::size_t symbol = 0; symbol < kSymbolCount; ++symbol) {
    header->lengths[symbol] = ByteAt(data, 12 + symbol);
  }
  header->bit_count = ReadLittleEndian64(data, 268);
  return true;
}

// 头部自洽性。四条检查都不需要解压，坏流在分配内存之前就被挡掉。
bool ValidateHuffmanHeader(const HuffmanHeader& header,
                           std::string* error_message) {
  std::size_t used = 0;
  std::size_t last_symbol = 0;
  std::uint64_t kraft = 0;
  for (std::size_t symbol = 0; symbol < kSymbolCount; ++symbol) {
    const std::uint32_t length = header.lengths[symbol];
    if (length > kMaxCodeLength) {
      SetError(error_message, "huffman: 码长超过 32 bit");
      return false;
    }
    if (length == 0) {
      continue;
    }
    ++used;
    last_symbol = symbol;
    kraft += std::uint64_t{1} << (kMaxCodeLength - length);
  }
  if (used == 0) {
    // 一个码字都没有，就只能表示空输入，而且不能带 bitstream。
    if (header.original_size != 0 || header.bit_count != 0) {
      SetError(error_message, "huffman: 没有任何码字却声明了数据");
      return false;
    }
    return true;
  }
  if (used == 1 && header.lengths[last_symbol] != 1) {
    // 只有一种符号时唯一的合法码长是 1：0 bit 读不出来，更长的码则是编码器
    // 单方面的约定，本格式不承认。
    SetError(error_message, "huffman: 只有一种符号时码长必须是 1");
    return false;
  }
  if (kraft > (std::uint64_t{1} << kMaxCodeLength)) {
    SetError(error_message, "huffman: Kraft 和不成立（码长组合不是前缀码）");
    return false;
  }
  // 每个符号至少占 1 bit，所以 bit_count 不可能小于 original_size。
  if (header.bit_count < header.original_size) {
    SetError(error_message, "huffman: bit_count 装不下 original_size 个符号");
    return false;
  }
  return true;
}

// 流长度必须精确等于 276 + ceil(bit_count / 8)：多一个字节或少一个字节都算
// 坏流。用除法算上取整，避免 bit_count + 7 溢出。
bool PayloadSizeMatches(const std::string& data, const HuffmanHeader& header) {
  const std::uint64_t payload =
      header.bit_count / 8 + (header.bit_count % 8 != 0 ? 1 : 0);
  return static_cast<std::uint64_t>(data.size() - kHuffmanHeaderSize) ==
         payload;
}

// 标准 Huffman 建树，只保留每个叶子的深度（即码长）。
// 权重相同时比较"子树里最小的符号"：这个值在同一次建树里互不相同，所以堆的
// 比较是全序，输出与 std::priority_queue 的内部实现无关。
void BuildRawCodeLengths(
    const std::array<std::uint64_t, kSymbolCount>& frequency,
    std::array<std::uint32_t, kSymbolCount>* lengths) {
  struct Node {
    std::uint64_t weight = 0;
    int min_symbol = 0;
    int left = -1;
    int right = -1;
  };
  std::vector<Node> nodes;
  nodes.reserve(2 * kSymbolCount);
  for (std::size_t symbol = 0; symbol < kSymbolCount; ++symbol) {
    if (frequency[symbol] != 0) {
      nodes.push_back(
          Node{frequency[symbol], static_cast<int>(symbol), -1, -1});
    }
  }
  if (nodes.size() < 2) {
    return;  // 0 种 / 1 种符号由调用方特判
  }
  const auto worse = [&nodes](int a, int b) {
    const Node& first = nodes[static_cast<std::size_t>(a)];
    const Node& second = nodes[static_cast<std::size_t>(b)];
    if (first.weight != second.weight) {
      return first.weight > second.weight;
    }
    return first.min_symbol > second.min_symbol;
  };
  std::priority_queue<int, std::vector<int>, decltype(worse)> heap(worse);
  for (std::size_t index = 0; index < nodes.size(); ++index) {
    heap.push(static_cast<int>(index));
  }
  while (heap.size() > 1) {
    const int first = heap.top();
    heap.pop();
    const int second = heap.top();
    heap.pop();
    Node parent;
    parent.weight = nodes[static_cast<std::size_t>(first)].weight +
                    nodes[static_cast<std::size_t>(second)].weight;
    parent.min_symbol =
        std::min(nodes[static_cast<std::size_t>(first)].min_symbol,
                 nodes[static_cast<std::size_t>(second)].min_symbol);
    parent.left = first;
    parent.right = second;
    nodes.push_back(parent);
    heap.push(static_cast<int>(nodes.size()) - 1);
  }
  // 用显式栈求深度：树最深可以有 255 层，递归没必要。
  std::vector<std::pair<int, std::uint32_t>> pending;
  pending.push_back({static_cast<int>(nodes.size()) - 1, 0});
  while (!pending.empty()) {
    const std::pair<int, std::uint32_t> current = pending.back();
    pending.pop_back();
    const Node& node = nodes[static_cast<std::size_t>(current.first)];
    if (node.left < 0) {
      (*lengths)[static_cast<std::size_t>(node.min_symbol)] = current.second;
      continue;
    }
    pending.push_back({node.left, current.second + 1});
    pending.push_back({node.right, current.second + 1});
  }
}

// 码长上限修正。近似 Fibonacci 的频率分布可以让建树结果超过 32 bit，做法是
// 先把超限码长砍到 32，再用"加深当前频率最小的叶子"把 Kraft 和压回 1 以内：
//   - 砍码长只会让 Kraft 和变大，所以砍完必须修；
//   - 每次加深让 Kraft 和减少 2^(32-length-1) >= 1，而砍码长最多让它超出 256
//     （每个被砍的叶子原本贡献不到 1，砍完贡献正好 1），所以循环次数有限；
//   - 加深只挑 length < 32 的叶子，因此原来就超限的叶子修完正好停在 32。
// 这是贪心，不保证码长最优，但保证是合法前缀码，而且完全确定。
void LimitCodeLengths(const std::array<std::uint64_t, kSymbolCount>& frequency,
                      std::array<std::uint32_t, kSymbolCount>* lengths) {
  bool overflowed = false;
  for (std::size_t symbol = 0; symbol < kSymbolCount; ++symbol) {
    if ((*lengths)[symbol] > kMaxCodeLength) {
      (*lengths)[symbol] = kMaxCodeLength;
      overflowed = true;
    }
  }
  if (!overflowed) {
    return;
  }
  const std::uint64_t full = std::uint64_t{1} << kMaxCodeLength;
  std::uint64_t kraft = 0;
  for (std::size_t symbol = 0; symbol < kSymbolCount; ++symbol) {
    const std::uint32_t length = (*lengths)[symbol];
    if (length != 0) {
      kraft += std::uint64_t{1} << (kMaxCodeLength - length);
    }
  }
  while (kraft > full) {
    std::size_t target = kSymbolCount;
    for (std::size_t symbol = 0; symbol < kSymbolCount; ++symbol) {
      const std::uint32_t length = (*lengths)[symbol];
      if (length == 0 || length >= kMaxCodeLength) {
        continue;
      }
      if (target == kSymbolCount || frequency[symbol] < frequency[target]) {
        target = symbol;
      }
    }
    if (target == kSymbolCount) {
      break;  // 理论上到不了：所有码长都被压到 32 时 Kraft 和 <= 256
    }
    kraft -= std::uint64_t{1} << (kMaxCodeLength - (*lengths)[target] - 1);
    ++(*lengths)[target];
  }
}

void BuildCodeLengths(const std::array<std::uint64_t, kSymbolCount>& frequency,
                      std::array<std::uint32_t, kSymbolCount>* lengths) {
  lengths->fill(0);
  std::size_t used = 0;
  std::size_t last_symbol = 0;
  for (std::size_t symbol = 0; symbol < kSymbolCount; ++symbol) {
    if (frequency[symbol] != 0) {
      ++used;
      last_symbol = symbol;
    }
  }
  if (used == 0) {
    return;  // 空输入：256 个码长全 0
  }
  if (used == 1) {
    (*lengths)[last_symbol] = 1;  // 单符号：1 bit，绝不能用 0 bit
    return;
  }
  BuildRawCodeLengths(frequency, lengths);
  LimitCodeLengths(frequency, lengths);
}

// canonical 分配需要的三张表：每个码长段有多少个码字、段内第一个码字、
// 段内第一个符号在 sorted_symbols 里的下标。symbols 按 (码长, 符号) 升序。
struct CanonicalTable {
  std::array<std::uint32_t, kMaxCodeLength + 1> length_count{};
  std::array<std::uint32_t, kMaxCodeLength + 1> first_code{};
  std::array<std::uint32_t, kMaxCodeLength + 1> first_index{};
  std::array<std::uint8_t, kSymbolCount> sorted_symbols{};
};

// 标准 canonical Huffman：码字按 (码长, 符号) 升序排列，同一段内从段首码字
// 开始递增；换到更长的码长时整体左移一位。codes 为 nullptr 时只建解码表。
CanonicalTable BuildCanonicalTable(
    const std::array<std::uint32_t, kSymbolCount>& lengths,
    std::array<std::uint32_t, kSymbolCount>* codes) {
  CanonicalTable table;
  for (std::size_t symbol = 0; symbol < kSymbolCount; ++symbol) {
    if (lengths[symbol] != 0) {
      ++table.length_count[lengths[symbol]];
    }
  }
  std::uint32_t code = 0;
  std::uint32_t index = 0;
  for (std::uint32_t length = 1; length <= kMaxCodeLength; ++length) {
    table.first_code[length] = code;
    table.first_index[length] = index;
    index += table.length_count[length];
    // 最长的那一段之后不再左移：此时 code 可能正好用满 32 bit 的码空间。
    if (length < kMaxCodeLength) {
      code = (code + table.length_count[length]) << 1;
    }
  }
  std::array<std::uint32_t, kMaxCodeLength + 1> cursor = table.first_index;
  for (std::size_t symbol = 0; symbol < kSymbolCount; ++symbol) {
    const std::uint32_t length = lengths[symbol];
    if (length == 0) {
      continue;
    }
    const std::uint32_t slot = cursor[length]++;
    table.sorted_symbols[slot] = static_cast<std::uint8_t>(symbol);
    if (codes != nullptr) {
      (*codes)[symbol] =
          table.first_code[length] + (slot - table.first_index[length]);
    }
  }
  return table;
}

}  // namespace

bool HuffmanCompress(const std::string& input, std::string* output,
                     std::string* error_message) {
  if (output == nullptr) {
    SetError(error_message, "huffman: output 是空指针");
    return false;
  }
  std::array<std::uint64_t, kSymbolCount> frequency{};
  for (std::size_t i = 0; i < input.size(); ++i) {
    ++frequency[ByteAt(input, i)];
  }
  std::array<std::uint32_t, kSymbolCount> lengths{};
  BuildCodeLengths(frequency, &lengths);
  std::array<std::uint32_t, kSymbolCount> codes{};
  BuildCanonicalTable(lengths, &codes);

  std::uint64_t bit_count = 0;
  for (std::size_t symbol = 0; symbol < kSymbolCount; ++symbol) {
    bit_count += frequency[symbol] * lengths[symbol];
  }

  std::string result;
  result.reserve(kHuffmanHeaderSize + static_cast<std::size_t>(bit_count / 8) +
                 1);
  result.append(kHuffmanMagic, sizeof(kHuffmanMagic));
  AppendLittleEndian64(static_cast<std::uint64_t>(input.size()), &result);
  for (std::size_t symbol = 0; symbol < kSymbolCount; ++symbol) {
    result.push_back(static_cast<char>(lengths[symbol]));
  }
  AppendLittleEndian64(bit_count, &result);

  // bitstream：每个字节从最高位开始填，最后一个字节剩下的低位补 0。
  // buffer 里只保证低 buffered_bits 位有效，左移溢出的高位不参与输出。
  std::uint64_t buffer = 0;
  int buffered_bits = 0;
  for (std::size_t i = 0; i < input.size(); ++i) {
    const std::uint8_t symbol = ByteAt(input, i);
    const std::uint32_t length = lengths[symbol];
    buffer = (buffer << length) | codes[symbol];
    buffered_bits += static_cast<int>(length);
    while (buffered_bits >= 8) {
      buffered_bits -= 8;
      result.push_back(static_cast<char>((buffer >> buffered_bits) & 0xFF));
    }
  }
  if (buffered_bits > 0) {
    result.push_back(static_cast<char>((buffer << (8 - buffered_bits)) & 0xFF));
  }
  *output = std::move(result);
  return true;
}

bool HuffmanDecompress(const std::string& input, std::string* output,
                       std::string* error_message) {
  if (output == nullptr) {
    SetError(error_message, "huffman: output 是空指针");
    return false;
  }
  HuffmanHeader header;
  if (!ParseHuffmanHeader(input, &header)) {
    SetError(error_message, "huffman: magic 不是 HUF1 或流短于 276 字节");
    return false;
  }
  if (!ValidateHuffmanHeader(header, error_message)) {
    return false;
  }
  if (!PayloadSizeMatches(input, header)) {
    SetError(error_message, "huffman: 流长度与 bit_count 不符");
    return false;
  }
  const CanonicalTable table = BuildCanonicalTable(header.lengths, nullptr);
  std::string result;
  result.reserve(static_cast<std::size_t>(
      std::min<std::uint64_t>(header.original_size, kMaxReserveBytes)));
  const std::uint8_t* payload =
      reinterpret_cast<const std::uint8_t*>(input.data()) + kHuffmanHeaderSize;
  std::uint64_t consumed = 0;
  while (static_cast<std::uint64_t>(result.size()) < header.original_size) {
    std::uint32_t code = 0;
    std::uint32_t length = 0;
    int symbol = -1;
    while (length < kMaxCodeLength) {
      if (consumed >= header.bit_count) {
        SetError(error_message, "huffman: bitstream 提前结束");
        return false;
      }
      const int bit = (payload[consumed >> 3] >> (7 - (consumed & 7))) & 1;
      ++consumed;
      code = (code << 1) | static_cast<std::uint32_t>(bit);
      ++length;
      if (table.length_count[length] != 0 && code >= table.first_code[length] &&
          code - table.first_code[length] < table.length_count[length]) {
        symbol = table.sorted_symbols[table.first_index[length] +
                                      (code - table.first_code[length])];
        break;
      }
    }
    if (symbol < 0) {
      SetError(error_message, "huffman: 32 bit 内没有匹配的码字");
      return false;
    }
    result.push_back(static_cast<char>(symbol));
  }
  // 解码消耗的比特必须正好等于 bit_count：既能挡住"original_size 被改小"，
  // 也能挡住流尾多余的数据。
  if (consumed != header.bit_count) {
    SetError(error_message, "huffman: bitstream 的比特数与 original_size 不符");
    return false;
  }
  *output = std::move(result);
  return true;
}

bool LooksLikeHuffman(const std::string& data) {
  HuffmanHeader header;
  if (!ParseHuffmanHeader(data, &header)) {
    return false;
  }
  if (!ValidateHuffmanHeader(header, nullptr)) {
    return false;
  }
  return PayloadSizeMatches(data, header);
}

}  // namespace compression
}  // namespace backupproject
