// codec_io.h
//
// 压缩编解码器共用的流式 I/O 原语：顺序读、字节写、比特写、比特读、滚动历史。
//
// 这一层存在的理由是"HUF1 / LZH1 的 wire format 只有一份实现"：
//   * 字符串接口（单元测试、已知向量、小数据）用内存后端；
//   * 产品流水线用文件后端，整条流不进内存。
// 两条路走的是同一套比特语义，差别只在字节从哪儿来、到哪儿去。
//
// 内存边界：所有缓冲都是固定的（默认 256 KiB），与输入规模无关。

#ifndef BACKUP_PROJECT_SRC_COMPRESSION_CODEC_IO_H_
#define BACKUP_PROJECT_SRC_COMPRESSION_CODEC_IO_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "file_io.h"

namespace backupproject {
namespace compression {

// 默认 I/O 缓冲。足够大，能摊掉 syscall；又足够小，与输入规模无关。
inline constexpr std::size_t kStreamBufferSize = 256 * 1024;

// ---- 字节写：文件或内存 -----------------------------------------------------
class ByteSinkAdapter {
 public:
  ByteSinkAdapter() = default;
  ByteSinkAdapter(const ByteSinkAdapter&) = delete;
  ByteSinkAdapter& operator=(const ByteSinkAdapter&) = delete;

  bool OpenFile(FileSink* sink, std::string* error_message);
  void OpenMemory(std::string* out);
  bool Write(const void* data, std::size_t size, std::string* error_message);
  bool valid() const { return file_ != nullptr || memory_ != nullptr; }
  // 已经接受的字节数（内存后端也计）。
  std::uint64_t bytes_written() const { return bytes_written_; }

 private:
  FileSink* file_ = nullptr;
  std::string* memory_ = nullptr;
  std::uint64_t bytes_written_ = 0;
};

// ---- 顺序读：文件或内存 -----------------------------------------------------
//
// 只支持从头往后读。
class SequentialReader {
 public:
  SequentialReader() = default;
  SequentialReader(const SequentialReader&) = delete;
  SequentialReader& operator=(const SequentialReader&) = delete;

  bool OpenFile(const std::string& path, std::string* error_message);
  void OpenMemory(const std::string* data);
  void Close();

  // 读满 size 个字节；不足时返回 false（已读到的部分仍然写进 out）。
  bool ReadExact(void* out, std::size_t size, std::string* error_message);
  // 读一个字节。EOF 返回 false，且**不写** error_message——EOF 是不是错误由
  // 调用方按格式语义判断（token stream 截断是错误，正常读完不是）。
  bool ReadByte(std::uint8_t* out, std::string* error_message);
  // 丢掉接下来的 bytes 个字节（LZH1 里内层 HUF1 流前面有 20 字节外层头部）。
  bool Discard(std::uint64_t bytes, std::string* error_message);
  // 还没交付给调用方的字节数（含缓冲区里那部分）。
  std::uint64_t remaining() const;
  // 总共已经交付的字节数。
  std::uint64_t consumed() const { return consumed_; }

 private:
  bool Refill(std::string* error_message);

  FileSource file_;
  const std::string* memory_ = nullptr;
  std::uint64_t total_size_ = 0;
  std::size_t memory_offset_ = 0;
  std::vector<unsigned char> buffer_;
  std::size_t buffer_offset_ = 0;
  std::size_t buffer_size_ = 0;
  std::uint64_t consumed_ = 0;
};

// ---- 比特写 ----------------------------------------------------------------
//
// 比特序与 HUF1 一致：字节内从最高位开始填，最后一个字节剩下的低位补 0。
class BitWriter {
 public:
  BitWriter() = default;
  BitWriter(const BitWriter&) = delete;
  BitWriter& operator=(const BitWriter&) = delete;

  void Open(ByteSinkAdapter* sink) { sink_ = sink; }
  bool WriteBits(std::uint32_t code, std::uint32_t length,
                 std::string* error_message);
  // 把不足一字节的部分补 0 写出去。
  bool Flush(std::string* error_message);

  std::uint64_t bits_written() const { return bits_written_; }
  // 已经写出去的净荷字节数（含最后那个补位字节），不含任何头部。
  std::uint64_t payload_bytes() const {
    return payload_bytes_ + (buffered_bits_ > 0 ? 1 : 0);
  }

 private:
  ByteSinkAdapter* sink_ = nullptr;
  std::uint64_t accumulator_ = 0;
  int buffered_bits_ = 0;
  std::uint64_t bits_written_ = 0;
  std::uint64_t payload_bytes_ = 0;
};

// ---- 比特读 ----------------------------------------------------------------
class BitReader {
 public:
  BitReader() = default;
  BitReader(const BitReader&) = delete;
  BitReader& operator=(const BitReader&) = delete;

  void Open(SequentialReader* reader, std::uint64_t bit_count);
  // 读 1 bit；超过 bit_count（或字节流提前结束）返回 false。
  bool ReadBit(std::uint32_t* bit, std::string* error_message);
  std::uint64_t consumed() const { return consumed_; }

 private:
  SequentialReader* reader_ = nullptr;
  std::uint64_t bit_count_ = 0;
  std::uint64_t consumed_ = 0;
  std::uint8_t current_ = 0;
  int bits_left_ = 0;
};

// ---- LZSS 解码用的滚动历史 --------------------------------------------------
//
// 32 KiB 环形历史 + 固定输出缓冲。match 必须逐字节复制（distance 可以小于
// length，源和目标重叠），所以这里不提供"整段拷贝"。
class RollingHistory {
 public:
  RollingHistory() = default;
  RollingHistory(const RollingHistory&) = delete;
  RollingHistory& operator=(const RollingHistory&) = delete;

  bool Open(ByteSinkAdapter* sink, std::size_t window,
            std::string* error_message);
  bool Push(std::uint8_t value, std::string* error_message);
  bool CopyMatch(std::size_t distance, std::size_t length,
                 std::string* error_message);
  bool Flush(std::string* error_message);

  std::uint64_t produced() const { return produced_; }

 private:
  bool Emit(std::uint8_t value, std::string* error_message);

  ByteSinkAdapter* sink_ = nullptr;
  std::vector<unsigned char> ring_;
  std::size_t mask_ = 0;
  std::uint64_t produced_ = 0;
  std::string out_;
};

}  // namespace compression
}  // namespace backupproject

#endif  // BACKUP_PROJECT_SRC_COMPRESSION_CODEC_IO_H_
