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
// 编码与解码各**只有一份实现**：字节从内存来还是从文件来，由 SequentialReader
// 决定；写到内存还是写到 FileSink，由 ByteSinkAdapter 决定。字符串接口与
// Stream 接口因此不可能漂移——tests/fixtures/compression 里的冻结流会同时
// 卡住两条路。
//
// 内存边界：文件后端全程用固定 256 KiB 缓冲；频次表是 256 个 uint64。
// 峰值内存与输入大小无关。
//
// 确定性：所有遍历都按符号 0..255 升序，建树时用"子树里最小的符号"打破权重
// 平局，因此同一输入永远得到 byte-for-byte 相同的流。这里不序列化指针，也不
// 依赖任何无序容器的迭代顺序。
//
// 边界：空输入 → original_size = 0、bit_count = 0、256 个码长全 0、没有
// bitstream；只出现 1 种符号 → 该符号码长固定为 1（0 bit 读不出任何东西，也会
// 让解码端无法确定符号）。失败时 *output 保持原样，只有 *error_message 被写。
// 线程模型：本文件没有全局可变状态，也没有 static 缓存；每个函数只读写自己
// 的参数，因此不同 reader / sink / output 之间可以并发调用，同一个对象则不行。
// 失败契约：失败时返回 false，并在 error_message 非空时写入中文原因；
// *output 只在成功路径上被整体赋值，失败时调用方手里的旧值不会被改动。

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#include "codec_io.h"
#include "compression.h"

namespace backupproject {
namespace compression {

namespace {

constexpr std::size_t kSymbolCount = 256;
constexpr char kHuffmanMagic[4] = {'H', 'U', 'F', '1'};

void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) {
    *error_message = text;
  }
}

// 头部所有多字节字段都是 little-endian，这里不假设主机字节序。
void AppendLittleEndian64(std::uint64_t value, std::string* out) {
  for (int shift = 0; shift < 64; shift += 8) {
    out->push_back(static_cast<char>((value >> shift) & 0xFF));
  }
}

std::uint64_t ReadLittleEndian64(const unsigned char* data,
                                 std::size_t offset) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |=
        static_cast<std::uint64_t>(data[offset + static_cast<std::size_t>(i)])
        << (8 * i);
  }
  return value;
}

// 磁盘布局（全部 little-endian，共 276 字节；无对齐要求，逐字段读取）：
//   offset  size  field
//   0       4     magic "HUF1"
//   4       8     original_size  解压后的字节数
//   12      256   lengths[256]   每个符号的码长（0..32），0 表示不出现
//   268     8     bit_count      bitstream 的比特数，不含末尾 padding
// 头部之后紧跟 ceil(bit_count / 8) 字节净荷。流的总长必须精确等于
// 276 + ceil(bit_count / 8)：多一个字节或少一个字节都算格式错误。
struct HuffmanHeader {
  std::uint64_t original_size = 0;
  std::uint64_t bit_count = 0;
  std::array<std::uint32_t, kSymbolCount> lengths{};
};

// 纯结构解析：按固定偏移取字段，不做任何合法性判断（判断在
// ValidateHuffmanHeader 里）。输入是**未信任的解压输入**，因此每次读取都以
// size >= kHuffmanHeaderSize 为前提，bytes 与 size 必须描述同一块缓冲区。
// 多字节字段一律按 little-endian 解释，不假设主机字节序。
bool ParseHuffmanHeader(const unsigned char* bytes, std::size_t size,
                        HuffmanHeader* header) {
  if (size < kHuffmanHeaderSize) {
    return false;
  }
  if (std::memcmp(bytes, kHuffmanMagic, sizeof(kHuffmanMagic)) != 0) {
    return false;
  }
  header->original_size = ReadLittleEndian64(bytes, 4);
  for (std::size_t symbol = 0; symbol < kSymbolCount; ++symbol) {
    header->lengths[symbol] = bytes[12 + symbol];
  }
  header->bit_count = ReadLittleEndian64(bytes, 268);
  return true;
}

// 头部自洽性。四条检查都不需要解压，坏流在分配内存之前就被挡掉。
// 校验强度说明：Kraft 和 <= 2^32 只保证"码长组合没有超订"，并不要求它正好
// 用满整个码空间（允许不完整的码表）。因此逐比特解码必须自己处理"读到 32
// bit 仍无匹配"的情况，不能假定输入一定能解出符号。
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

// bit_count 对应的净荷字节数。用除法算上取整，避免 bit_count + 7 溢出。
// 这是编码端与解码端共用的唯一定义：(bit_count + 7) / 8 这类写法在
// bit_count 接近 uint64 上限时会溢出，从而接受一个长度错误的流。
std::uint64_t PayloadBytesFor(std::uint64_t bit_count) {
  return bit_count / 8 + (bit_count % 8 != 0 ? 1 : 0);
}

// 标准 Huffman 建树，只保留每个叶子的深度（即码长）。
// 权重相同时比较"子树里最小的符号"：这个值在同一次建树里互不相同，所以堆的
// 比较是全序，输出与 std::priority_queue 的内部实现无关。
// 节点之间用 vector 下标互相引用，不用指针也不用 shared_ptr：树只在这一次
// 建树里活着，push_back 只发生在尾部，已有下标不会失效，也不引入所有权问题。
// node.left < 0 是叶子判据；叶子的 min_symbol 就是符号本身，内部节点的
// min_symbol 是两个孩子里的较小者，仅用于打破权重平局。
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

// 码长的唯一权威入口：空输入 / 单符号 / 常规三种情况都在这里分流，
// 建树与上限修正只是它的两步内部实现。调用方不应绕过它直接调
// BuildRawCodeLengths——那会让空输入与单符号这两条边界静默缺失，
// 产出的头部随后会被解码端拒绝。
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
// 三张辅助表都以码长为下标（1..32，下标 0 永不使用）；sorted_symbols 是按
// (码长, 符号) 升序排好的符号序列，解码时用 first_index 直接定位段首，
// 因此每条码字的查找是 O(1)，不需要在候选符号里线性搜索。
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

// bit_count = Σ frequency[s] * length[s]。frequency 是 uint64、length 最多 32，
// 乘积可能溢出；一个回绕后的 bit_count 会让编码器写出一个自己都读不回来的流，
// 所以这里必须查。
// 计算过程本身也要防溢出：limit 取"还能再加多少个 length"的上界，
// 比较通过才做乘法。把检查放在乘法之前，而不是算完再判断结果是否变小，
// 是因为回绕后的值没有任何可识别的特征。
bool ComputeBitCount(const std::array<std::uint64_t, kSymbolCount>& frequency,
                     const std::array<std::uint32_t, kSymbolCount>& lengths,
                     std::uint64_t* bit_count, std::string* error_message) {
  std::uint64_t total = 0;
  for (std::size_t symbol = 0; symbol < kSymbolCount; ++symbol) {
    const std::uint64_t length = lengths[symbol];
    if (length == 0 || frequency[symbol] == 0) {
      continue;
    }
    const std::uint64_t limit =
        (std::numeric_limits<std::uint64_t>::max() - total) / length;
    if (frequency[symbol] > limit) {
      SetError(error_message,
               "huffman: bit_count 溢出（频率与码长的乘积超出 uint64）");
      return false;
    }
    total += frequency[symbol] * length;
  }
  *bit_count = total;
  return true;
}

// 顺序读完整条输入，攒出频次表与原始长度。
// 只顺序读一遍且不保留输入字节，所以统计阶段的内存占用与输入大小无关；
// 频次用 uint64，单个符号出现 2^64 次在物理上不可能。失败时 frequency 与
// total 的内容未定义，调用方应整体放弃这次编码，而不是拿半份频次继续。
bool CountFrequency(SequentialReader* reader,
                    std::array<std::uint64_t, kSymbolCount>* frequency,
                    std::uint64_t* total, std::string* error_message) {
  frequency->fill(0);
  *total = 0;
  std::vector<unsigned char> buffer(kStreamBufferSize);
  while (reader->remaining() > 0) {
    const std::size_t want = static_cast<std::size_t>(
        reader->remaining() < kStreamBufferSize ? reader->remaining()
                                                : kStreamBufferSize);
    if (!reader->ReadExact(buffer.data(), want, error_message)) {
      return false;
    }
    for (std::size_t index = 0; index < want; ++index) {
      ++(*frequency)[buffer[index]];
    }
    *total += want;
  }
  return true;
}

// 头部先在内存里拼成完整的 276 字节，再一次性交给 sink：sink 可能是文件，
// 半截头部写出去之后没有任何补救手段；单次 Write 要么在写出任何字节之前
// 失败，要么把整块交给下层。
// header.size() 的自检是最后一道保险——字段顺序或宽度写错时立刻失败，
// 而不是产出一个连自己都读不回来的流。
bool WriteHeader(const std::array<std::uint32_t, kSymbolCount>& lengths,
                 std::uint64_t original_size, std::uint64_t bit_count,
                 ByteSinkAdapter* sink, std::string* error_message) {
  std::string header;
  header.reserve(kHuffmanHeaderSize);
  header.append(kHuffmanMagic, sizeof(kHuffmanMagic));
  AppendLittleEndian64(original_size, &header);
  for (std::size_t symbol = 0; symbol < kSymbolCount; ++symbol) {
    header.push_back(static_cast<char>(lengths[symbol]));
  }
  AppendLittleEndian64(bit_count, &header);
  if (header.size() != kHuffmanHeaderSize) {
    SetError(error_message, "internal: huffman header 长度不对");
    return false;
  }
  return sink->Write(header.data(), header.size(), error_message);
}

// 编码主体：body_reader 从头顺序提供 original_size 个字节。
// 两遍式编码的代价是输入被读两遍，换来的是与输入大小无关的内存占用：
// 频次统计阶段不保存输入字节，这里再逐块重读一次。
// 失败语义：sink 上可能已经留下了本流的部分字节（头部或半截比特流），
// 本函数不回滚；调用方必须丢弃整个输出，不能把失败结果当成截断的流使用。
bool EncodeCore(std::uint64_t original_size,
                const std::array<std::uint64_t, kSymbolCount>& frequency,
                SequentialReader* body_reader, ByteSinkAdapter* sink,
                std::uint64_t* stream_bytes, std::string* error_message) {
  std::array<std::uint32_t, kSymbolCount> lengths{};
  BuildCodeLengths(frequency, &lengths);
  std::array<std::uint32_t, kSymbolCount> codes{};
  BuildCanonicalTable(lengths, &codes);
  std::uint64_t bit_count = 0;
  if (!ComputeBitCount(frequency, lengths, &bit_count, error_message)) {
    return false;
  }
  if (!WriteHeader(lengths, original_size, bit_count, sink, error_message)) {
    return false;
  }

  BitWriter writer;
  writer.Open(sink);
  std::vector<unsigned char> buffer(kStreamBufferSize);
  while (body_reader->remaining() > 0) {
    const std::size_t want = static_cast<std::size_t>(
        body_reader->remaining() < kStreamBufferSize ? body_reader->remaining()
                                                     : kStreamBufferSize);
    if (!body_reader->ReadExact(buffer.data(), want, error_message)) {
      return false;
    }
    for (std::size_t index = 0; index < want; ++index) {
      const std::uint8_t symbol = buffer[index];
      if (!writer.WriteBits(codes[symbol], lengths[symbol], error_message)) {
        return false;
      }
    }
  }
  if (!writer.Flush(error_message)) {
    return false;
  }
  if (writer.bits_written() != bit_count ||
      writer.payload_bytes() != PayloadBytesFor(bit_count)) {
    SetError(error_message, "internal: huffman 写出的比特数与 bit_count 不符");
    return false;
  }
  if (stream_bytes != nullptr) {
    *stream_bytes = kHuffmanHeaderSize + writer.payload_bytes();
  }
  return true;
}

// 从 reader 的当前位置读 276 字节头部并做全部头部级校验，同时检查"流长度
// 必须精确等于 276 + ceil(bit_count/8)"。不解码、不分配。
// 只在 reader 的当前位置读取，返回时 reader 停在净荷开头（偏移由调用方用
// Discard 自行推进）。reader->remaining() 在这里被当作"本流还剩多少字节"，
// 所以调用方必须先把它定位到流起点，否则长度检查会得出错误结论。
bool ReadHeaderAt(SequentialReader* reader, HuffmanHeader* header,
                  std::uint64_t* payload_bytes, std::string* error_message) {
  unsigned char bytes[kHuffmanHeaderSize];
  if (!reader->ReadExact(bytes, sizeof(bytes), error_message)) {
    SetError(error_message, "huffman: magic 不是 HUF1 或流短于 276 字节");
    return false;
  }
  if (!ParseHuffmanHeader(bytes, sizeof(bytes), header)) {
    SetError(error_message, "huffman: magic 不是 HUF1 或流短于 276 字节");
    return false;
  }
  if (!ValidateHuffmanHeader(*header, error_message)) {
    return false;
  }
  *payload_bytes = PayloadBytesFor(header->bit_count);
  if (reader->remaining() != *payload_bytes) {
    SetError(error_message, "huffman: 流长度与 bit_count 不符");
    return false;
  }
  return true;
}

// 解码主体：从 reader 的当前位置（头部已经读过）解出 header.original_size 个
// 字节写进 sink。
// 输出按 kStreamBufferSize 分块写进 sink，不在内存里攒下 original_size
// 字节：被篡改成 original_size = 2^40 的头部不会触发 1 TiB 的分配，
// 它会在读到流尾时以"bitstream 提前结束"失败。
// 解出的字节数由 header.original_size 精确控制，多一个少一个都算失败。
bool DecodeBody(SequentialReader* reader, ByteSinkAdapter* sink,
                const HuffmanHeader& header, std::string* error_message) {
  const CanonicalTable table = BuildCanonicalTable(header.lengths, nullptr);
  BitReader bits;
  bits.Open(reader, header.bit_count);
  std::string out;
  out.reserve(kStreamBufferSize);
  std::uint64_t produced = 0;
  while (produced < header.original_size) {
    std::uint32_t code = 0;
    std::uint32_t length = 0;
    int symbol = -1;
    while (length < kMaxCodeLength) {
      std::uint32_t bit = 0;
      if (!bits.ReadBit(&bit, error_message)) {
        SetError(error_message, "huffman: bitstream 提前结束");
        return false;
      }
      code = (code << 1) | bit;
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
    out.push_back(static_cast<char>(symbol));
    ++produced;
    if (out.size() >= kStreamBufferSize) {
      if (!sink->Write(out.data(), out.size(), error_message)) {
        return false;
      }
      out.clear();
    }
  }
  // 解码消耗的比特必须正好等于 bit_count：既能挡住"original_size 被改小"，
  // 也能挡住流尾多余的数据。
  if (bits.consumed() != header.bit_count) {
    SetError(error_message, "huffman: bitstream 的比特数与 original_size 不符");
    return false;
  }
  // 最后一个字节里没被 bit_count 覆盖的低位必须是 0：编码器就是这么写的，
  // 格式也是这么定的。不查的话，把 padding 位改脏的流照样解得出来，
  // 等于 wire format 只约束了一半。
  if (!bits.PaddingBitsAreZero()) {
    SetError(error_message, "huffman: non-zero padding bits");
    return false;
  }
  if (!out.empty() && !sink->Write(out.data(), out.size(), error_message)) {
    return false;
  }
  return true;
}

}  // namespace

// 内存 -> 内存：输入是完整明文串，输出是完整的 HUF1 流。
// 成功时 *output 被整体赋值；失败时 *output 保持调用前的值不变（结果先写在
// 局部变量里，最后一步才 move 出去），error_message 被写成中文原因。
// output == nullptr 是调用方的编程错误，用错误码而不是崩溃来报告。
bool HuffmanCompress(const std::string& input, std::string* output,
                     std::string* error_message) {
  if (output == nullptr) {
    SetError(error_message, "huffman: output 是空指针");
    return false;
  }
  SequentialReader counter;
  counter.OpenMemory(&input);
  std::array<std::uint64_t, kSymbolCount> frequency{};
  std::uint64_t total = 0;
  if (!CountFrequency(&counter, &frequency, &total, error_message)) {
    return false;
  }
  counter.Close();

  std::string result;
  ByteSinkAdapter sink;
  sink.OpenMemory(&result);
  SequentialReader body;
  body.OpenMemory(&input);
  if (!EncodeCore(total, frequency, &body, &sink, nullptr, error_message)) {
    return false;
  }
  *output = std::move(result);
  return true;
}

// 内存 -> 内存的解压。除了 DecodeBody 的逐比特校验，这里还复核
// result.size() == original_size：解码循环以保证产量为终止条件，这次复核
// 是对接口承诺的检查，而不是对循环的重复。
bool HuffmanDecompress(const std::string& input, std::string* output,
                       std::string* error_message) {
  if (output == nullptr) {
    SetError(error_message, "huffman: output 是空指针");
    return false;
  }
  SequentialReader reader;
  reader.OpenMemory(&input);
  HuffmanHeader header;
  std::uint64_t payload_bytes = 0;
  if (!ReadHeaderAt(&reader, &header, &payload_bytes, error_message)) {
    return false;
  }
  std::string result;
  ByteSinkAdapter sink;
  sink.OpenMemory(&result);
  if (!DecodeBody(&reader, &sink, header, error_message)) {
    return false;
  }
  if (result.size() != header.original_size) {
    SetError(error_message, "huffman: 解出的长度与 original_size 不符");
    return false;
  }
  *output = std::move(result);
  return true;
}

// 只读头部、不解压：容器层需要在打开一条流之前就知道它的 original_size 与
// payload_bytes，用来校验长度、排版后续流。*info 在入口被重置，
// 因此失败时它是全零，而不是上一次调用的残留值。
bool HuffmanReadStreamInfo(const std::string& input_file,
                           std::uint64_t input_offset, HuffmanStreamInfo* info,
                           std::string* error_message) {
  if (info == nullptr) {
    SetError(error_message, "huffman: info 是空指针");
    return false;
  }
  *info = HuffmanStreamInfo();
  SequentialReader reader;
  if (!reader.OpenFile(input_file, error_message)) {
    return false;
  }
  if (input_offset > reader.remaining() ||
      !reader.Discard(input_offset, error_message)) {
    SetError(error_message, "huffman: input_offset 超出文件长度");
    return false;
  }
  HuffmanHeader header;
  std::uint64_t payload_bytes = 0;
  if (!ReadHeaderAt(&reader, &header, &payload_bytes, error_message)) {
    return false;
  }
  info->original_size = header.original_size;
  info->bit_count = header.bit_count;
  info->header_bytes = kHuffmanHeaderSize;
  info->payload_bytes = payload_bytes;
  info->stream_bytes = kHuffmanHeaderSize + payload_bytes;
  return true;
}

// 文件 -> 文件：input_file 会被完整读两遍（见 EncodeCore）。若源文件在这
// 期间被改写，产出的流就是两次读取的混合体；本层不做一致性校验，
// 需要一致性的调用方要自己保证读取期间源文件不被修改。
// original_size / stream_bytes 只在成功时写入，允许传 nullptr。
bool HuffmanCompressStream(const std::string& input_file, FileSink* sink,
                           std::uint64_t* original_size,
                           std::uint64_t* stream_bytes,
                           std::string* error_message) {
  if (sink == nullptr) {
    SetError(error_message, "huffman: sink 是空指针");
    return false;
  }
  // pass 1：只统计频次与总长度，不保留任何输入字节。
  SequentialReader counter;
  if (!counter.OpenFile(input_file, error_message)) {
    return false;
  }
  std::array<std::uint64_t, kSymbolCount> frequency{};
  std::uint64_t total = 0;
  if (!CountFrequency(&counter, &frequency, &total, error_message)) {
    return false;
  }
  counter.Close();

  // pass 2：再顺序读一遍，边读边写比特。
  SequentialReader body;
  if (!body.OpenFile(input_file, error_message)) {
    return false;
  }
  ByteSinkAdapter adapter;
  if (!adapter.OpenFile(sink, error_message)) {
    return false;
  }
  std::uint64_t written_bytes = 0;
  if (!EncodeCore(total, frequency, &body, &adapter, &written_bytes,
                  error_message)) {
    return false;
  }
  if (original_size != nullptr) {
    *original_size = total;
  }
  if (stream_bytes != nullptr) {
    *stream_bytes = written_bytes;
  }
  return true;
}

// 文件 -> 文件：input_offset 是流在文件里的起点（通常是容器头部之后的偏移）。
// expected_original_size 来自容器 header，与流内声明做交叉校验：两者不一致
// 时在写出任何数据之前失败，避免产出一份长度对不上的解压结果。
// 失败时 sink 里可能已经有部分解压数据，调用方应丢弃整个输出。
bool HuffmanDecompressStream(const std::string& input_file,
                             std::uint64_t input_offset, FileSink* sink,
                             std::uint64_t expected_original_size,
                             std::uint64_t* written,
                             std::string* error_message) {
  if (sink == nullptr) {
    SetError(error_message, "huffman: sink 是空指针");
    return false;
  }
  SequentialReader reader;
  if (!reader.OpenFile(input_file, error_message)) {
    return false;
  }
  if (input_offset > reader.remaining() ||
      !reader.Discard(input_offset, error_message)) {
    SetError(error_message, "huffman: input_offset 超出文件长度");
    return false;
  }
  HuffmanHeader header;
  std::uint64_t payload_bytes = 0;
  if (!ReadHeaderAt(&reader, &header, &payload_bytes, error_message)) {
    return false;
  }
  // 这一条是资源契约：在做任何大规模输出之前，先把"头部声明的长度"和
  // "容器声明的长度"对上。不一致就直接失败，不等解完几个 GB 才发现。
  if (header.original_size != expected_original_size) {
    SetError(error_message, "huffman: original_size 与容器声明的长度不符（" +
                                std::to_string(header.original_size) + " vs " +
                                std::to_string(expected_original_size) + "）");
    return false;
  }
  ByteSinkAdapter adapter;
  if (!adapter.OpenFile(sink, error_message)) {
    return false;
  }
  if (!DecodeBody(&reader, &adapter, header, error_message)) {
    return false;
  }
  if (adapter.bytes_written() != expected_original_size) {
    SetError(error_message, "huffman: 解出的长度与 original_size 不符");
    return false;
  }
  if (written != nullptr) {
    *written = adapter.bytes_written();
  }
  return true;
}

// 探测入口：只跑 ReadHeaderAt（magic + 全部头部自洽性 + 精确流长度），
// 不解压、不分配与 original_size 成比例的内存。返回 true 只说明"头部像
// HUF1"，不构成"这段数据一定能解压成功"的保证。
bool LooksLikeHuffman(const std::string& data) {
  SequentialReader reader;
  reader.OpenMemory(&data);
  HuffmanHeader header;
  std::uint64_t payload_bytes = 0;
  return ReadHeaderAt(&reader, &header, &payload_bytes, nullptr);
}

}  // namespace compression
}  // namespace backupproject
