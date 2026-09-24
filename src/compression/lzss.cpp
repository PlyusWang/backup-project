// lzss.cpp
//
// LZSS 一层（token stream）与 LZSS + Canonical Huffman（LZH1）容器。
//
// token stream 的形状：每 8 个 token 前面有 1 个 control byte，bit7 对应第 1 个
// token。bit = 1 是 literal（后面 1 字节原文），bit = 0 是 match（后面
// uint16 distance + uint8 length_minus_3，都是 little-endian）。最后一组不足
// 8 个 token 时，control byte 里没用到的 bit 必须是 0；读侧不靠 control byte
// 判断结束，而是靠 original_size 停，所以多出来的 token 会被当成坏流。
//
// 匹配查找用 3 字节哈希 + 链式表：head[hash] 是最近一次出现该哈希的位置，
// prev[pos & (kWindowSize-1)] 串起同一个哈希的历史位置。每个位置最多看 128 个
// 候选，而且每个候选先比较"当前最优长度 + 1"处的那个字节，因此不会对每个字节
// 都去扫整个 32 KiB 窗口。
//
// 解码的 match 必须逐字节复制：distance 可以小于 length，源和目标重叠，复制
// 过程中刚写出去的字节会立刻被再次读到（例如 100000 个 'a'，distance = 1）。

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "compression.h"

namespace backupproject {
namespace compression {

namespace {

constexpr char kLzssHuffmanMagic[4] = {'L', 'Z', 'H', '1'};
constexpr std::size_t kHashBits = 15;
constexpr std::size_t kHashSize = std::size_t{1} << kHashBits;
// 每个位置最多回溯的候选数。128 已经足够吃掉长重复，再大只是让最坏情况变慢。
constexpr std::size_t kMaxChainLength = 128;
constexpr std::size_t kNoPosition = static_cast<std::size_t>(-1);
constexpr std::size_t kTokensPerControlByte = 8;
// 坏头部里的 original_size 可以是任意 64 bit 值，预留内存前先夹住。
constexpr std::uint64_t kMaxReserveBytes = std::uint64_t{1} << 26;

void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) {
    *error_message = text;
  }
}

std::uint8_t ByteAt(const std::string& data, std::size_t index) {
  return static_cast<std::uint8_t>(data[index]);
}

// 外层 LZH1 头部同样是小端，不假设主机字节序。
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

// 3 字节哈希：先拼成 24 bit，再乘一个奇数常量取高位，避免 "aaa"/"aab" 这类
// 只差低位的串挤在相邻桶里。
std::size_t HashAt(const std::uint8_t* data) {
  const std::uint32_t value = (static_cast<std::uint32_t>(data[0]) << 16) |
                              (static_cast<std::uint32_t>(data[1]) << 8) |
                              static_cast<std::uint32_t>(data[2]);
  return static_cast<std::size_t>((value * std::uint32_t{2654435761}) >>
                                  (32 - kHashBits));
}

class LzssEncoder {
 public:
  LzssEncoder(const char* data, std::size_t size)
      : data_(reinterpret_cast<const std::uint8_t*>(data)), size_(size) {}

  void Encode(std::string* tokens) {
    head_.assign(kHashSize, kNoPosition);
    prev_.assign(kWindowSize, kNoPosition);
    tokens->clear();
    // 全是 literal 时是 1 + 1/8 字节一个 token，再留一点余量。
    tokens->reserve(size_ + size_ / kTokensPerControlByte + 16);
    std::size_t control_index = 0;
    std::uint32_t control = 0;
    std::size_t slot = 0;
    tokens->push_back('\0');  // 先占位，凑满 8 个 token 再回填
    std::size_t pos = 0;
    while (pos < size_) {
      std::size_t distance = 0;
      const std::size_t length = FindMatch(pos, &distance);
      if (length >= kMinMatchLength) {
        // match：control 里对应的 bit 保持 0
        tokens->push_back(static_cast<char>(distance & 0xFF));
        tokens->push_back(static_cast<char>((distance >> 8) & 0xFF));
        tokens->push_back(static_cast<char>(length - kMinMatchLength));
        for (std::size_t offset = 0; offset < length; ++offset) {
          Insert(pos + offset);
        }
        pos += length;
      } else {
        control |= std::uint32_t{1} << (7 - slot);
        tokens->push_back(static_cast<char>(data_[pos]));
        Insert(pos);
        ++pos;
      }
      ++slot;
      if (slot == kTokensPerControlByte) {
        (*tokens)[control_index] = static_cast<char>(control);
        control = 0;
        slot = 0;
        control_index = tokens->size();
        tokens->push_back('\0');
      }
    }
    if (slot == 0) {
      tokens->pop_back();  // 最后一组正好凑满，去掉多余占位
    } else {
      (*tokens)[control_index] = static_cast<char>(control);
    }
  }

 private:
  // 记住 pos 处的 3 字节哈希，供后面的位置回溯。
  void Insert(std::size_t pos) {
    if (pos + kMinMatchLength > size_) {
      return;
    }
    const std::size_t bucket = HashAt(data_ + pos);
    prev_[pos % kWindowSize] = head_[bucket];
    head_[bucket] = pos;
  }

  // 返回最长匹配长度（不足 kMinMatchLength 时返回 0）与对应 distance。
  std::size_t FindMatch(std::size_t pos, std::size_t* distance) {
    if (pos + kMinMatchLength > size_) {
      return 0;
    }
    const std::size_t max_length = std::min(kMaxMatchLength, size_ - pos);
    const std::size_t limit = pos > kWindowSize ? pos - kWindowSize : 0;
    std::size_t best_length = 0;
    std::size_t best_distance = 0;
    std::size_t candidate = head_[HashAt(data_ + pos)];
    std::size_t chain = kMaxChainLength;
    while (candidate != kNoPosition && candidate >= limit && chain > 0) {
      --chain;
      // 先比 data[candidate + best_length]：不相等说明这个候选连当前最优都追
      // 不上，直接换下一个，省掉一次完整比较。
      if (data_[candidate + best_length] == data_[pos + best_length]) {
        std::size_t length = 0;
        while (length < max_length &&
               data_[candidate + length] == data_[pos + length]) {
          ++length;
        }
        if (length > best_length) {
          best_length = length;
          best_distance = pos - candidate;
          if (best_length >= max_length) {
            break;
          }
        }
      }
      candidate = prev_[candidate % kWindowSize];
    }
    if (best_length < kMinMatchLength) {
      return 0;
    }
    *distance = best_distance;
    return best_length;
  }

  const std::uint8_t* data_;
  std::size_t size_;
  std::vector<std::size_t> head_;
  std::vector<std::size_t> prev_;
};

bool ValidateLzssHuffmanHeader(const std::string& input,
                               std::uint64_t* original_size,
                               std::uint64_t* token_stream_size,
                               std::string* error_message) {
  if (input.size() < kLzssHuffmanHeaderSize) {
    SetError(error_message, "lzh1: 流短于 20 字节");
    return false;
  }
  if (std::memcmp(input.data(), kLzssHuffmanMagic, sizeof(kLzssHuffmanMagic)) !=
      0) {
    SetError(error_message, "lzh1: magic 不是 LZH1");
    return false;
  }
  *original_size = ReadLittleEndian64(input, 4);
  *token_stream_size = ReadLittleEndian64(input, 12);
  return true;
}

}  // namespace

bool LzssEncode(const std::string& input, std::string* tokens,
                std::string* error_message) {
  if (tokens == nullptr) {
    SetError(error_message, "lzss: tokens 是空指针");
    return false;
  }
  LzssEncoder encoder(input.data(), input.size());
  encoder.Encode(tokens);
  return true;
}

bool LzssDecode(const std::string& tokens, std::uint64_t original_size,
                std::string* output, std::string* error_message) {
  if (output == nullptr) {
    SetError(error_message, "lzss: output 是空指针");
    return false;
  }
  std::string result;
  result.reserve(static_cast<std::size_t>(
      std::min<std::uint64_t>(original_size, kMaxReserveBytes)));
  std::size_t cursor = 0;
  while (static_cast<std::uint64_t>(result.size()) < original_size) {
    if (cursor >= tokens.size()) {
      SetError(error_message,
               "lzss: token stream 提前结束（缺少 control byte）");
      return false;
    }
    const std::uint32_t control = ByteAt(tokens, cursor++);
    for (int bit = 7; bit >= 0; --bit) {
      // original_size 是唯一的结束条件：凑够就停，control byte 里剩下的 bit
      // （编码器保证是 0）不再产生 token。
      if (static_cast<std::uint64_t>(result.size()) >= original_size) {
        break;
      }
      if (((control >> bit) & 1) != 0) {
        if (cursor >= tokens.size()) {
          SetError(error_message, "lzss: literal 缺少操作数");
          return false;
        }
        result.push_back(tokens[cursor++]);
        continue;
      }
      if (tokens.size() - cursor < 3) {
        SetError(error_message, "lzss: match 缺少操作数");
        return false;
      }
      const std::uint32_t distance =
          ByteAt(tokens, cursor) |
          (static_cast<std::uint32_t>(ByteAt(tokens, cursor + 1)) << 8);
      const std::uint32_t length =
          static_cast<std::uint32_t>(ByteAt(tokens, cursor + 2)) +
          static_cast<std::uint32_t>(kMinMatchLength);
      cursor += 3;
      if (distance == 0) {
        SetError(error_message, "lzss: distance = 0 不是合法的 match");
        return false;
      }
      if (distance > kWindowSize) {
        SetError(error_message, "lzss: distance 超过 32768");
        return false;
      }
      if (distance > result.size()) {
        SetError(error_message, "lzss: distance 超过已经产出的字节数");
        return false;
      }
      if (static_cast<std::uint64_t>(result.size()) + length > original_size) {
        SetError(error_message, "lzss: 解码输出会超过 original_size");
        return false;
      }
      // overlap copy：distance < length 时源和目标重叠，必须逐字节复制，
      // 先把字节读出来再 push_back，避免自引用。
      for (std::uint32_t i = 0; i < length; ++i) {
        const char value = result[result.size() - distance];
        result.push_back(value);
      }
    }
  }
  if (cursor != tokens.size()) {
    SetError(error_message, "lzss: original_size 之后还有剩余 token");
    return false;
  }
  *output = std::move(result);
  return true;
}

bool LzssHuffmanCompress(const std::string& input, std::string* output,
                         std::string* error_message) {
  if (output == nullptr) {
    SetError(error_message, "lzh1: output 是空指针");
    return false;
  }
  std::string tokens;
  if (!LzssEncode(input, &tokens, error_message)) {
    return false;
  }
  std::string inner;
  if (!HuffmanCompress(tokens, &inner, error_message)) {
    return false;
  }
  std::string result;
  result.reserve(kLzssHuffmanHeaderSize + inner.size());
  result.append(kLzssHuffmanMagic, sizeof(kLzssHuffmanMagic));
  AppendLittleEndian64(static_cast<std::uint64_t>(input.size()), &result);
  AppendLittleEndian64(static_cast<std::uint64_t>(tokens.size()), &result);
  result.append(inner);
  *output = std::move(result);
  return true;
}

bool LzssHuffmanDecompress(const std::string& input, std::string* output,
                           std::string* error_message) {
  if (output == nullptr) {
    SetError(error_message, "lzh1: output 是空指针");
    return false;
  }
  std::uint64_t original_size = 0;
  std::uint64_t token_stream_size = 0;
  if (!ValidateLzssHuffmanHeader(input, &original_size, &token_stream_size,
                                 error_message)) {
    return false;
  }
  // 内层 HUF1 流就是第 20 字节之后的全部内容；它的总长度必须正好用完外层
  // 剩下的字节，否则说明外层长度字段和实际不符。
  const std::string inner = input.substr(kLzssHuffmanHeaderSize);
  if (!LooksLikeHuffman(inner)) {
    SetError(error_message, "lzh1: 内层 HUF1 流不合法");
    return false;
  }
  // 内层 HUF1 的 original_size 就是 token stream 的字节数，必须和外层字段
  // 完全一致，不一致直接拒绝，不必先解压。
  if (ReadLittleEndian64(inner, 4) != token_stream_size) {
    SetError(error_message, "lzh1: token_stream_size 与内层 HUF1 不一致");
    return false;
  }
  std::string tokens;
  if (!HuffmanDecompress(inner, &tokens, error_message)) {
    return false;
  }
  if (static_cast<std::uint64_t>(tokens.size()) != token_stream_size) {
    SetError(error_message, "lzh1: 解出的 token stream 长度与头部不符");
    return false;
  }
  return LzssDecode(tokens, original_size, output, error_message);
}

bool LooksLikeLzssHuffman(const std::string& data) {
  std::uint64_t original_size = 0;
  std::uint64_t token_stream_size = 0;
  if (!ValidateLzssHuffmanHeader(data, &original_size, &token_stream_size,
                                 nullptr)) {
    return false;
  }
  const std::string inner = data.substr(kLzssHuffmanHeaderSize);
  if (!LooksLikeHuffman(inner)) {
    return false;
  }
  return ReadLittleEndian64(inner, 4) == token_stream_size;
}

}  // namespace compression
}  // namespace backupproject
