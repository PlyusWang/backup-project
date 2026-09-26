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
// 编码器与解码器各**只有一份实现**：输入是内存还是文件由 SequentialReader
// 决定，输出是内存还是 FileSink 由 ByteSinkAdapter 决定。文件路径用一个
// 32 KiB 滑动窗口 + 261 字节前瞻，绝对位置与内存路径完全一致，所以同一输入
// 产出 byte-for-byte 相同的 token stream（冻结 fixture 会卡住这一点）。
//
// 解码的 match 必须逐字节复制：distance 可以小于 length，源和目标重叠，复制
// 过程中刚写出去的字节会立刻被再次读到（例如 100000 个 'a'，distance = 1）。

#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "codec_io.h"
#include "compression.h"

namespace backupproject {
namespace compression {

namespace {

constexpr char kLzssHuffmanMagic[4] = {'L', 'Z', 'H', '1'};
constexpr std::size_t kHashBits = 15;
constexpr std::size_t kHashSize = std::size_t{1} << kHashBits;
constexpr std::size_t kSymbolCount = 256;
// 每个位置最多回溯的候选数。128 已经足够吃掉长重复，再大只是让最坏情况变慢。
constexpr std::size_t kMaxChainLength = 128;
constexpr std::size_t kNoPosition = static_cast<std::size_t>(-1);
constexpr std::size_t kTokensPerControlByte = 8;

// 处理位置 pos 时，编码器可能读到的最远字节是 pos + kMaxMatchLength + 2
// （match 跑到底时 Insert() 会看 pos+length-1 处的 3 字节哈希，而 Insert 自己
// 会用 pos+3 > size 提前返回）。所以前瞻必须留足 kMaxMatchLength +
// kMinMatchLength。
constexpr std::size_t kLookahead = kMaxMatchLength + kMinMatchLength;
constexpr std::size_t kWindowBufferSize = kWindowSize + kLookahead;

void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) {
    *error_message = text;
  }
}

// 外层 LZH1 头部同样是小端，不假设主机字节序。
void AppendLittleEndian64(std::uint64_t value, std::string* out) {
  for (int shift = 0; shift < 64; shift += 8) {
    out->push_back(static_cast<char>((value >> shift) & 0xFF));
  }
}

std::uint64_t ReadLittleEndian64(const unsigned char* data) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(data[i]) << (8 * i);
  }
  return value;
}

// 3 字节哈希：先拼成 24 bit，再乘一个奇数常量取高位，避免 "aaa"/"aab" 这类
// 只差低位的串挤在相邻桶里。
std::size_t HashTriple(std::uint8_t a, std::uint8_t b, std::uint8_t c) {
  const std::uint32_t value = (static_cast<std::uint32_t>(a) << 16) |
                              (static_cast<std::uint32_t>(b) << 8) |
                              static_cast<std::uint32_t>(c);
  return static_cast<std::size_t>((value * std::uint32_t{2654435761}) >>
                                  (32 - kHashBits));
}

// token 临时文件的 RAII 守卫：无论成功还是失败，析构时都 unlink。
class ScopedTokenFile {
 public:
  explicit ScopedTokenFile(std::string path) : path_(std::move(path)) {}
  ~ScopedTokenFile() {
    if (!path_.empty()) {
      ::unlink(path_.c_str());
    }
  }
  ScopedTokenFile(const ScopedTokenFile&) = delete;
  ScopedTokenFile& operator=(const ScopedTokenFile&) = delete;
  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

// 流式 LZSS 编码器。
//
// 内存只保留：32 KiB 窗口 + 261 字节前瞻 + 哈希头表 + prev 环 + 一个最多
// 25 字节的当前 token 组。绝对位置和内存版完全一致，所以匹配决策也完全一致。
class LzssStreamEncoder {
 public:
  LzssStreamEncoder(SequentialReader* input, ByteSinkAdapter* sink)
      : input_(input), sink_(sink) {}

  bool Encode(std::uint64_t* original_size, std::uint64_t* token_bytes,
              std::string* error_message) {
    size_ = input_->remaining();
    base_ = 0;
    filled_ = 0;
    buffer_.assign(kWindowBufferSize, 0);
    head_.assign(kHashSize, kNoPosition);
    prev_.assign(kWindowSize, kNoPosition);
    group_.clear();
    group_.push_back('\0');  // control byte 占位
    group_tokens_ = 0;
    control_ = 0;
    token_bytes_ = 0;

    std::size_t pos = 0;
    while (pos < size_) {
      if (!EnsureLookahead(pos, error_message)) {
        return false;
      }
      std::size_t distance = 0;
      const std::size_t length = FindMatch(pos, &distance);
      if (length >= kMinMatchLength) {
        for (std::size_t offset = 0; offset < length; ++offset) {
          Insert(pos + offset);
        }
        if (!EmitMatch(distance, length, error_message)) {
          return false;
        }
        pos += length;
      } else {
        const std::uint8_t value = At(pos);
        Insert(pos);
        if (!EmitLiteral(value, error_message)) {
          return false;
        }
        pos += 1;
      }
    }
    if (!FlushGroup(error_message)) {
      return false;
    }
    if (original_size != nullptr) {
      *original_size = size_;
    }
    if (token_bytes != nullptr) {
      *token_bytes = token_bytes_;
    }
    return true;
  }

 private:
  // 保证 buffer_ 覆盖 [limit(pos), min(size_, pos + kLookahead))。
  bool EnsureLookahead(std::size_t pos, std::string* error_message) {
    const std::uint64_t need_end =
        (size_ - pos > kLookahead) ? pos + kLookahead : size_;
    while (base_ + filled_ < need_end) {
      if (filled_ == buffer_.size()) {
        std::size_t keep_from = pos > kWindowSize ? pos - kWindowSize : 0;
        if (keep_from < base_) {
          keep_from = base_;
        }
        const std::size_t drop = keep_from - base_;
        if (drop == 0) {
          SetError(error_message, "internal: lzss 窗口缓冲不足");
          return false;
        }
        std::memmove(buffer_.data(), buffer_.data() + drop, filled_ - drop);
        base_ += drop;
        filled_ -= drop;
      }
      const std::uint64_t want = std::min<std::uint64_t>(
          buffer_.size() - filled_, size_ - (base_ + filled_));
      if (want == 0) {
        break;
      }
      if (!input_->ReadExact(buffer_.data() + filled_,
                             static_cast<std::size_t>(want), error_message)) {
        return false;
      }
      filled_ += static_cast<std::size_t>(want);
    }
    return true;
  }

  // 绝对位置 -> 缓冲区下标。所有调用点的边界都在 EnsureLookahead 里证明过。
  std::uint8_t At(std::size_t index) const { return buffer_[index - base_]; }

  std::size_t HashAt(std::size_t pos) const {
    return HashTriple(At(pos), At(pos + 1), At(pos + 2));
  }

  // 记住 pos 处的 3 字节哈希，供后面的位置回溯。
  void Insert(std::size_t pos) {
    if (pos + kMinMatchLength > size_) {
      return;
    }
    const std::size_t bucket = HashAt(pos);
    prev_[pos % kWindowSize] = head_[bucket];
    head_[bucket] = pos;
  }

  // 返回最长匹配长度（不足 kMinMatchLength 时返回 0）与对应 distance。
  std::size_t FindMatch(std::size_t pos, std::size_t* distance) const {
    if (pos + kMinMatchLength > size_) {
      return 0;
    }
    const std::size_t max_length = std::min(kMaxMatchLength, size_ - pos);
    const std::size_t limit = pos > kWindowSize ? pos - kWindowSize : 0;
    std::size_t best_length = 0;
    std::size_t best_distance = 0;
    std::size_t candidate = head_[HashAt(pos)];
    std::size_t chain = kMaxChainLength;
    while (candidate != kNoPosition && candidate >= limit && chain > 0) {
      --chain;
      // 先比 data[candidate + best_length]：不相等说明这个候选连当前最优都追
      // 不上，直接换下一个，省掉一次完整比较。
      if (At(candidate + best_length) == At(pos + best_length)) {
        std::size_t length = 0;
        while (length < max_length &&
               At(candidate + length) == At(pos + length)) {
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

  bool EmitLiteral(std::uint8_t value, std::string* error_message) {
    control_ |= std::uint32_t{1} << (7 - group_tokens_);
    group_.push_back(static_cast<char>(value));
    ++group_tokens_;
    if (group_tokens_ == kTokensPerControlByte) {
      return FlushGroup(error_message);
    }
    return true;
  }

  bool EmitMatch(std::size_t distance, std::size_t length,
                 std::string* error_message) {
    group_.push_back(static_cast<char>(distance & 0xFF));
    group_.push_back(static_cast<char>((distance >> 8) & 0xFF));
    group_.push_back(static_cast<char>(length - kMinMatchLength));
    ++group_tokens_;
    if (group_tokens_ == kTokensPerControlByte) {
      return FlushGroup(error_message);
    }
    return true;
  }

  bool FlushGroup(std::string* error_message) {
    if (group_tokens_ == 0) {
      return true;  // 最后一组正好凑满时没有多余占位要写
    }
    group_[0] = static_cast<char>(control_);
    token_bytes_ += group_.size();
    if (!sink_->Write(group_.data(), group_.size(), error_message)) {
      return false;
    }
    group_.clear();
    group_.push_back('\0');
    group_tokens_ = 0;
    control_ = 0;
    return true;
  }

  SequentialReader* input_;
  ByteSinkAdapter* sink_;
  std::vector<unsigned char> buffer_;
  std::size_t base_ = 0;
  std::size_t filled_ = 0;
  std::uint64_t size_ = 0;
  std::vector<std::size_t> head_;
  std::vector<std::size_t> prev_;
  std::string group_;
  std::size_t group_tokens_ = 0;
  std::uint32_t control_ = 0;
  std::uint64_t token_bytes_ = 0;
};

// 流式 LZSS 解码器。expected_original_size 是唯一的结束条件。
bool LzssDecodeCore(SequentialReader* tokens, ByteSinkAdapter* sink,
                    std::uint64_t expected_original_size,
                    std::uint64_t* written, std::string* error_message) {
  RollingHistory history;
  if (!history.Open(sink, kWindowSize, error_message)) {
    return false;
  }
  while (history.produced() < expected_original_size) {
    std::uint8_t control = 0;
    if (!tokens->ReadByte(&control, error_message)) {
      SetError(error_message,
               "lzss: token stream 提前结束（缺少 control byte）");
      return false;
    }
    for (int bit = 7; bit >= 0; --bit) {
      // original_size 是唯一的结束条件：凑够就停。
      if (history.produced() >= expected_original_size) {
        // 剩下的 bit 是编码器保证为 0 的 padding。以前这里直接 break，
        // 于是"最后一个 control byte 的低位被改脏"的流也能通过——格式既然
        // 规定了它们必须是 0，就必须真的查。
        const std::uint32_t unused_mask = (std::uint32_t{1} << (bit + 1)) - 1;
        if ((control & unused_mask) != 0) {
          SetError(error_message, "lzss: unused control bits must be zero");
          return false;
        }
        break;
      }
      if (((control >> bit) & 1) != 0) {
        std::uint8_t value = 0;
        if (!tokens->ReadByte(&value, error_message)) {
          SetError(error_message, "lzss: literal 缺少操作数");
          return false;
        }
        if (!history.Push(value, error_message)) {
          return false;
        }
        continue;
      }
      unsigned char raw[3];
      if (!tokens->ReadExact(raw, sizeof(raw), error_message)) {
        SetError(error_message, "lzss: match 缺少操作数");
        return false;
      }
      const std::uint32_t distance = static_cast<std::uint32_t>(raw[0]) |
                                     (static_cast<std::uint32_t>(raw[1]) << 8);
      const std::uint32_t length = static_cast<std::uint32_t>(raw[2]) +
                                   static_cast<std::uint32_t>(kMinMatchLength);
      if (distance == 0) {
        SetError(error_message, "lzss: distance = 0 不是合法的 match");
        return false;
      }
      if (distance > kWindowSize) {
        SetError(error_message, "lzss: distance 超过 32768");
        return false;
      }
      if (distance > history.produced()) {
        SetError(error_message, "lzss: distance 超过已经产出的字节数");
        return false;
      }
      if (history.produced() + length > expected_original_size) {
        SetError(error_message, "lzss: 解码输出会超过 original_size");
        return false;
      }
      if (!history.CopyMatch(distance, length, error_message)) {
        return false;
      }
    }
  }
  if (tokens->remaining() != 0) {
    SetError(error_message, "lzss: original_size 之后还有剩余 token");
    return false;
  }
  if (!history.Flush(error_message)) {
    return false;
  }
  if (written != nullptr) {
    *written = history.produced();
  }
  return true;
}

// 读 LZH1 的 20 字节外层头部并校验 magic。
bool ReadOuterHeader(SequentialReader* reader, std::uint64_t* original_size,
                     std::uint64_t* token_stream_size,
                     std::string* error_message) {
  unsigned char bytes[kLzssHuffmanHeaderSize];
  if (!reader->ReadExact(bytes, sizeof(bytes), error_message)) {
    SetError(error_message, "lzh1: 流短于 20 字节");
    return false;
  }
  if (std::memcmp(bytes, kLzssHuffmanMagic, sizeof(kLzssHuffmanMagic)) != 0) {
    SetError(error_message, "lzh1: magic 不是 LZH1");
    return false;
  }
  *original_size = ReadLittleEndian64(bytes + 4);
  *token_stream_size = ReadLittleEndian64(bytes + 12);
  return true;
}

bool WriteOuterHeader(std::uint64_t original_size,
                      std::uint64_t token_stream_size, ByteSinkAdapter* sink,
                      std::string* error_message) {
  std::string header;
  header.reserve(kLzssHuffmanHeaderSize);
  header.append(kLzssHuffmanMagic, sizeof(kLzssHuffmanMagic));
  AppendLittleEndian64(original_size, &header);
  AppendLittleEndian64(token_stream_size, &header);
  if (header.size() != kLzssHuffmanHeaderSize) {
    SetError(error_message, "internal: lzh1 header 长度不对");
    return false;
  }
  return sink->Write(header.data(), header.size(), error_message);
}

}  // namespace

bool LzssEncode(const std::string& input, std::string* tokens,
                std::string* error_message) {
  if (tokens == nullptr) {
    SetError(error_message, "lzss: tokens 是空指针");
    return false;
  }
  SequentialReader reader;
  reader.OpenMemory(&input);
  std::string result;
  ByteSinkAdapter sink;
  sink.OpenMemory(&result);
  LzssStreamEncoder encoder(&reader, &sink);
  if (!encoder.Encode(nullptr, nullptr, error_message)) {
    return false;
  }
  *tokens = std::move(result);
  return true;
}

bool LzssDecode(const std::string& tokens, std::uint64_t original_size,
                std::string* output, std::string* error_message) {
  if (output == nullptr) {
    SetError(error_message, "lzss: output 是空指针");
    return false;
  }
  SequentialReader reader;
  reader.OpenMemory(&tokens);
  std::string result;
  ByteSinkAdapter sink;
  sink.OpenMemory(&result);
  if (!LzssDecodeCore(&reader, &sink, original_size, nullptr, error_message)) {
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
  SequentialReader reader;
  reader.OpenMemory(&input);
  std::uint64_t original_size = 0;
  std::uint64_t token_stream_size = 0;
  if (!ReadOuterHeader(&reader, &original_size, &token_stream_size,
                       error_message)) {
    return false;
  }
  // 内层 HUF1 流就是第 20 字节之后的全部内容。先只验头部（魔数、码长、
  // Kraft、精确流长度），再检查它的 original_size 是否等于外层声明的 token
  // stream 长度——不一致直接拒绝，不必先解压。
  const std::string inner = input.substr(kLzssHuffmanHeaderSize);
  if (!LooksLikeHuffman(inner)) {
    SetError(error_message, "lzh1: 内层 HUF1 流不合法");
    return false;
  }
  if (ReadLittleEndian64(reinterpret_cast<const unsigned char*>(inner.data()) +
                         4) != token_stream_size) {
    SetError(error_message, "lzh1: token_stream_size 与内层 HUF1 不一致");
    return false;
  }
  std::string tokens;
  if (!HuffmanDecompress(inner, &tokens, error_message)) {
    SetError(error_message, "lzh1: 内层 HUF1 流不合法");
    return false;
  }
  if (static_cast<std::uint64_t>(tokens.size()) != token_stream_size) {
    SetError(error_message, "lzh1: 解出的 token stream 长度与头部不符");
    return false;
  }
  return LzssDecode(tokens, original_size, output, error_message);
}

bool LooksLikeLzssHuffman(const std::string& data) {
  SequentialReader reader;
  reader.OpenMemory(&data);
  std::uint64_t original_size = 0;
  std::uint64_t token_stream_size = 0;
  if (!ReadOuterHeader(&reader, &original_size, &token_stream_size, nullptr)) {
    return false;
  }
  const std::string inner = data.substr(kLzssHuffmanHeaderSize);
  if (!LooksLikeHuffman(inner)) {
    return false;
  }
  return ReadLittleEndian64(
             reinterpret_cast<const unsigned char*>(inner.data()) + 4) ==
         token_stream_size;
}

bool LzssHuffmanReadStreamInfo(const std::string& input_file,
                               LzssHuffmanStreamInfo* info,
                               std::string* error_message) {
  if (info == nullptr) {
    SetError(error_message, "lzh1: info 是空指针");
    return false;
  }
  *info = LzssHuffmanStreamInfo();
  SequentialReader reader;
  if (!reader.OpenFile(input_file, error_message)) {
    return false;
  }
  if (!ReadOuterHeader(&reader, &info->original_size, &info->token_stream_size,
                       error_message)) {
    return false;
  }
  // 内层 HUF1 从第 20 字节开始，必须正好用完文件剩下的字节——这条由
  // HuffmanReadStreamInfo 内部的"流长度必须精确"检查保证。
  if (!HuffmanReadStreamInfo(input_file, kLzssHuffmanHeaderSize, &info->inner,
                             error_message)) {
    SetError(error_message, "lzh1: 内层 HUF1 流不合法");
    return false;
  }
  // 内层 HUF1 的 original_size 就是 token stream 的字节数，必须等于外层声明。
  if (info->inner.original_size != info->token_stream_size) {
    SetError(error_message, "lzh1: token_stream_size 与内层 HUF1 不一致");
    return false;
  }
  info->header_bytes = kLzssHuffmanHeaderSize;
  info->stream_bytes = kLzssHuffmanHeaderSize + info->inner.stream_bytes;
  return true;
}

bool LzssHuffmanCompressStream(const std::string& input_file, FileSink* sink,
                               const std::string& workspace_directory,
                               std::uint64_t* original_size,
                               std::uint64_t* stream_bytes,
                               std::string* error_message) {
  if (sink == nullptr) {
    SetError(error_message, "lzh1: sink 是空指针");
    return false;
  }
  if (workspace_directory.empty()) {
    SetError(error_message, "lzh1: 缺少私有工作目录");
    return false;
  }
  SequentialReader input;
  if (!input.OpenFile(input_file, error_message)) {
    return false;
  }
  // token 流先进私有目录里的临时文件：它是 LZSS 与 Huffman 之间唯一需要落地的
  // 中间产物，也是"整条流不进内存"的关键。
  FileSink token_sink;
  if (!token_sink.OpenTemp(workspace_directory, "lzss-tokens-",
                           error_message)) {
    return false;
  }
  ScopedTokenFile token_file(token_sink.path());
  std::uint64_t token_bytes = 0;
  std::uint64_t input_size = 0;
  ByteSinkAdapter token_adapter;
  if (!token_adapter.OpenFile(&token_sink, error_message)) {
    token_sink.Abandon();
    return false;
  }
  LzssStreamEncoder encoder(&input, &token_adapter);
  if (!encoder.Encode(&input_size, &token_bytes, error_message)) {
    token_sink.Abandon();
    return false;
  }
  if (!token_sink.Close(error_message)) {
    token_sink.Abandon();
    return false;
  }

  ByteSinkAdapter out;
  if (!out.OpenFile(sink, error_message) ||
      !WriteOuterHeader(input_size, token_bytes, &out, error_message)) {
    return false;
  }
  std::uint64_t inner_original = 0;
  std::uint64_t inner_bytes = 0;
  if (!HuffmanCompressStream(token_file.path(), sink, &inner_original,
                             &inner_bytes, error_message)) {
    return false;
  }
  if (inner_original != token_bytes) {
    SetError(error_message, "internal: token stream 长度与内层 HUF1 不一致");
    return false;
  }
  if (original_size != nullptr) {
    *original_size = input_size;
  }
  if (stream_bytes != nullptr) {
    *stream_bytes = kLzssHuffmanHeaderSize + inner_bytes;
  }
  return true;
}

bool LzssHuffmanDecompressStream(const std::string& input_file, FileSink* sink,
                                 const std::string& workspace_directory,
                                 std::uint64_t expected_original_size,
                                 std::uint64_t* written,
                                 std::string* error_message) {
  if (sink == nullptr) {
    SetError(error_message, "lzh1: sink 是空指针");
    return false;
  }
  if (workspace_directory.empty()) {
    SetError(error_message, "lzh1: 缺少私有工作目录");
    return false;
  }
  // 先把两层头部读完、把三处长度对上，才允许真的开始解压。
  LzssHuffmanStreamInfo info;
  if (!LzssHuffmanReadStreamInfo(input_file, &info, error_message)) {
    return false;
  }
  if (info.original_size != expected_original_size) {
    SetError(error_message, "lzh1: original_size 与容器声明的长度不符（" +
                                std::to_string(info.original_size) + " vs " +
                                std::to_string(expected_original_size) + "）");
    return false;
  }
  FileSink token_sink;
  if (!token_sink.OpenTemp(workspace_directory, "lzss-tokens-",
                           error_message)) {
    return false;
  }
  ScopedTokenFile token_file(token_sink.path());
  std::uint64_t decoded_tokens = 0;
  if (!HuffmanDecompressStream(input_file, kLzssHuffmanHeaderSize, &token_sink,
                               info.token_stream_size, &decoded_tokens,
                               error_message)) {
    token_sink.Abandon();
    return false;
  }
  if (!token_sink.Close(error_message)) {
    token_sink.Abandon();
    return false;
  }
  if (decoded_tokens != info.token_stream_size) {
    SetError(error_message, "lzh1: 解出的 token stream 长度与头部不符");
    return false;
  }
  SequentialReader tokens;
  if (!tokens.OpenFile(token_file.path(), error_message)) {
    return false;
  }
  ByteSinkAdapter out;
  if (!out.OpenFile(sink, error_message)) {
    return false;
  }
  std::uint64_t produced = 0;
  if (!LzssDecodeCore(&tokens, &out, expected_original_size, &produced,
                      error_message)) {
    return false;
  }
  if (produced != expected_original_size) {
    SetError(error_message, "lzh1: 解出的长度与 original_size 不符");
    return false;
  }
  if (written != nullptr) {
    *written = produced;
  }
  return true;
}

}  // namespace compression
}  // namespace backupproject
