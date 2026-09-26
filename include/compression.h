// compression.h
//
// 手写压缩编解码：Canonical Huffman（HUF1）与 LZSS + Canonical
// Huffman（LZH1）。
//
// 每个格式只有**一份**实现，两种后端：
//   * 字符串接口（单元测试、已知向量、小数据）：内存后端；
//   * Stream 接口（产品流水线）：文件后端，整条流不进内存。
// 头部字段全部 little-endian：
//
//   HUF1  offset 0   4 字节  magic = "HUF1"
//         offset 4   8 字节  uint64 original_size
//         offset 12  256 字节 每个符号的 code length（0 表示该符号不出现）
//         offset 268 8 字节   uint64 bit_count（有效 bit 数，不是字节数）
//         offset 276 ...      bitstream，每个字节从最高位开始填
//         流总长度必须正好是 276 + (bit_count + 7) / 8。
//
//   LZH1  offset 0   4 字节  magic = "LZH1"
//         offset 4   8 字节  uint64 original_size
//         offset 12  8 字节  uint64 token_stream_size
//         offset 20  ...      一个完整的 HUF1 流，它的 original_size 必须等于
//                             token_stream_size
//         流总长度必须正好是 20 + 内层 HUF1 流长度。
//
// 这条路径不依赖也不链接任何第三方压缩库，只用 C++17 标准库。
//
// 所有 Compress / Decompress / Encode / Decode 在失败时返回 false、把原因写进
// *error_message（可为 nullptr），并且**不修改** *output：调用方可以放心地在
// 失败后继续用原来的字符串。输入可以包含任意字节（含 NUL）。

#ifndef BACKUP_PROJECT_INCLUDE_COMPRESSION_H_
#define BACKUP_PROJECT_INCLUDE_COMPRESSION_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include "file_io.h"

namespace backupproject {
namespace compression {

// LZSS 参数：
//   - 窗口 32 KiB，所以 distance 取值 1..32768；
//   - 最短匹配 3，更短的匹配不值得 3 字节操作数；
//   - 最长匹配 258，正好让 length-3 装进一个字节。
inline constexpr std::size_t kWindowSize = 32768;
inline constexpr std::size_t kMinMatchLength = 3;
inline constexpr std::size_t kMaxMatchLength = 258;

// 头部固定长度与 code length 上限。超过 32 的码长无法用一次读入 32 bit 的
// 解码器处理，因此编码器会用溢出修正把码长压回 32 以内，解码器也会拒绝它。
inline constexpr std::size_t kHuffmanHeaderSize = 276;
inline constexpr std::size_t kLzssHuffmanHeaderSize = 20;
inline constexpr std::uint32_t kMaxCodeLength = 32;

// Canonical Huffman（HUF1）。同一输入必须产出 byte-for-byte 相同的流：
// 频率相同、符号相同，结果就相同，不依赖任何容器的迭代顺序。
bool HuffmanCompress(const std::string& input, std::string* output,
                     std::string* error_message);
bool HuffmanDecompress(const std::string& input, std::string* output,
                       std::string* error_message);

// LZSS + Canonical Huffman（LZH1）：先做 LZSS 得到 token stream，
// 再对 token stream 整体做一次 HUF1。
bool LzssHuffmanCompress(const std::string& input, std::string* output,
                         std::string* error_message);
bool LzssHuffmanDecompress(const std::string& input, std::string* output,
                           std::string* error_message);

// 裸 LZSS 一层（调试与单测用）：输出是 token stream，不再过 Huffman。
// Decode 必须知道原始长度，token stream 本身不带结束标记。
bool LzssEncode(const std::string& input, std::string* tokens,
                std::string* error_message);
bool LzssDecode(const std::string& tokens, std::uint64_t original_size,
                std::string* output, std::string* error_message);

// 头部探测：只看 magic 与字段自洽性（码长上限、单符号特例、Kraft 和、精确的流
// 长度），不做完整解压，也不分配与 original_size 同量级的内存。
bool LooksLikeHuffman(const std::string& data);
bool LooksLikeLzssHuffman(const std::string& data);

// ---- 流式（文件到文件）接口 ------------------------------------------------
//
// 产品流水线只走这一组。峰值内存与输入大小无关：
//   Huffman：256 个频次 + 两个固定 I/O 缓冲；
//   LZSS：32 KiB 窗口 + 261 字节前瞻 + 固定 I/O 缓冲 + 一个私有 token
//   临时文件。
//
// expected_original_size 是调用方从 container header 知道的期望输出长度。
// 头部里的 original_size 与它不一致时**立刻失败**，不等解完几 GB 才发现。

struct HuffmanStreamInfo {
  std::uint64_t original_size = 0;
  std::uint64_t bit_count = 0;
  std::uint64_t header_bytes = kHuffmanHeaderSize;
  std::uint64_t payload_bytes = 0;  // ceil(bit_count / 8)
  std::uint64_t stream_bytes = 0;  // header + payload，必须精确等于流长度
};

// 只读 HUF1 头部并做全部头部级校验，不解码。流从 input_offset 开始，
// 必须正好用完文件剩下的字节。
bool HuffmanReadStreamInfo(const std::string& input_file,
                           std::uint64_t input_offset, HuffmanStreamInfo* info,
                           std::string* error_message);

// 把整个 input_file 压成一条 HUF1 流写进 sink。
bool HuffmanCompressStream(const std::string& input_file, FileSink* sink,
                           std::uint64_t* original_size,
                           std::uint64_t* stream_bytes,
                           std::string* error_message);

// 解码 input_file 里从 input_offset 开始的 HUF1 流，输出写进 sink。
bool HuffmanDecompressStream(const std::string& input_file,
                             std::uint64_t input_offset, FileSink* sink,
                             std::uint64_t expected_original_size,
                             std::uint64_t* written,
                             std::string* error_message);

struct LzssHuffmanStreamInfo {
  std::uint64_t original_size = 0;
  std::uint64_t token_stream_size = 0;
  std::uint64_t header_bytes = kLzssHuffmanHeaderSize;
  HuffmanStreamInfo inner;  // inner.original_size 必须等于 token_stream_size
  std::uint64_t stream_bytes = 0;  // 20 + inner.stream_bytes
};

// 只读 LZH1 外层头部与内层 HUF1 头部，并交叉校验两者的长度字段。
bool LzssHuffmanReadStreamInfo(const std::string& input_file,
                               LzssHuffmanStreamInfo* info,
                               std::string* error_message);

// workspace_directory 必须是调用方已经建好的 0700 私有目录：LZSS 的中间
// token 流会以 0600 落在那里，成功或失败都会被清掉。
bool LzssHuffmanCompressStream(const std::string& input_file, FileSink* sink,
                               const std::string& workspace_directory,
                               std::uint64_t* original_size,
                               std::uint64_t* stream_bytes,
                               std::string* error_message);

bool LzssHuffmanDecompressStream(const std::string& input_file, FileSink* sink,
                                 const std::string& workspace_directory,
                                 std::uint64_t expected_original_size,
                                 std::uint64_t* written,
                                 std::string* error_message);

}  // namespace compression
}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_COMPRESSION_H_
