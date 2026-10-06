// codec_io.cpp
//
// 见 codec_io.h。

// 本文件是 codec_io.h 里那几组 I/O 原语的实现。huffman.cpp / lzss.cpp 只用
// 这些原语，不直接碰 FileSource / FileSink，也不自己管理缓冲。
//
// 职责边界：这里只搬运字节与比特，不认识任何压缩格式的语义 —— 哪个字节是
// token、哪几位是 padding，全部由调用方按自己的格式解释。
//
// 借用关系（生命周期）：ByteSinkAdapter 借用外部的 FileSink 或 std::string；
// SequentialReader 借用 FileSource 或 std::string；BitWriter 与 RollingHistory
// 再借用 ByteSinkAdapter，BitReader 借用 SequentialReader。这些指针一律不持有
// 所有权、不负责释放，调用方必须保证被借用对象活过整条编解码流程；链条上
// 任何一环提前析构，剩下的对象就是悬垂指针。
//
// 失败语义：bool 接口失败时**不回滚**已经写出去、已经读进来的字节，调用方
// 一旦看到 false 就必须放弃整条流，不能把半截输出当成合法结果。
// error_message 允许为 nullptr，失败时覆盖写，成功时不保证被清空。
#include "codec_io.h"

#include <cstring>

namespace backupproject {
namespace compression {

namespace {

// 所有诊断的唯一出口：error_message 允许为空（自测与"只关心成败"的调用方
// 常这么传），所以每个失败分支都必须经过这里，而不是直接解引用。
void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) {
    *error_message = text;
  }
}

// Discard 的跳读缓冲：内存占用与"要丢多少字节"无关，恒为 64 KiB。
constexpr std::size_t kDiscardBufferSize = 64 * 1024;

}  // namespace

// ---- ByteSinkAdapter -------------------------------------------------------

// 切到文件后端（并清掉可能残留的内存后端与计数）。sink 只被借用：
// 本对象不 delete 它，调用方要保证它活到最后一次 Write 之后。
bool ByteSinkAdapter::OpenFile(FileSink* sink, std::string* error_message) {
  if (sink == nullptr) {
    SetError(error_message, "codec: sink 是空指针");
    return false;
  }
  file_ = sink;
  memory_ = nullptr;
  bytes_written_ = 0;
  return true;
}

// 内存后端是**追加**语义：不清空 out，多次编码可以拼进同一个字符串。
// out 同样只被借用，而且必须活过本对象。
void ByteSinkAdapter::OpenMemory(std::string* out) {
  file_ = nullptr;
  memory_ = out;
  bytes_written_ = 0;
}

// size == 0 视为成功且不计入 bytes_written()，调用方不必为"空块"特判。
// 两种后端互斥且内存优先；两个都没打开属于内部错误，不是用户输入错误。
bool ByteSinkAdapter::Write(const void* data, std::size_t size,
                            std::string* error_message) {
  if (size == 0) {
    return true;
  }
  if (memory_ != nullptr) {
    memory_->append(static_cast<const char*>(data), size);
    bytes_written_ += size;
    return true;
  }
  if (file_ == nullptr) {
    SetError(error_message, "internal: codec sink 没有打开");
    return false;
  }
  if (!file_->Write(data, size, error_message)) {
    return false;
  }
  bytes_written_ += size;
  return true;
}

// ---- SequentialReader ------------------------------------------------------

// 打开即"从头开始"：先 Close() 复位全部游标，同一个对象可以反复复用。
// total_size_ 只在打开时取一次快照：解码期间文件被外部追加或截断，都不该
// 改变我们对"还剩多少"的判断。
bool SequentialReader::OpenFile(const std::string& path,
                                std::string* error_message) {
  Close();
  if (!file_.Open(path, error_message)) {
    return false;
  }
  total_size_ = file_.size();
  buffer_.assign(kStreamBufferSize, 0);
  return true;
}

// data 为 nullptr 等价于空流（remaining() == 0）。指针只被借用，
// 调用方必须保证它活过整个读取过程。
void SequentialReader::OpenMemory(const std::string* data) {
  Close();
  memory_ = data;
  total_size_ = data == nullptr ? 0 : data->size();
  buffer_.assign(kStreamBufferSize, 0);
}

// 幂等：重复 Close、"打开后立刻 Close"都是合法状态，所有计数器归零。
void SequentialReader::Close() {
  file_.Close();
  memory_ = nullptr;
  total_size_ = 0;
  memory_offset_ = 0;
  buffer_offset_ = 0;
  buffer_size_ = 0;
  consumed_ = 0;
}

// 饱和减法：consumed_ 不会超过 total_size_，真超了也只报 0，不返回负数。
std::uint64_t SequentialReader::remaining() const {
  return total_size_ > consumed_ ? total_size_ - consumed_ : 0;
}

// 只在缓冲区彻底用空时调用：此时 consumed_ 正好等于"已经从后端取走的字节数"，
// 所以文件后端可以安全地从 consumed_ 继续读。
bool SequentialReader::Refill(std::string* error_message) {
  // 缓冲在第一次落地时才分配：空流或只读几个字节的调用不会白占 256 KiB。
  if (buffer_.size() < kStreamBufferSize) {
    buffer_.assign(kStreamBufferSize, 0);
  }
  if (memory_ != nullptr) {
    if (memory_offset_ >= memory_->size()) {
      return false;
    }
    const std::size_t want = memory_->size() - memory_offset_;
    const std::size_t take =
        want < kStreamBufferSize ? want : kStreamBufferSize;
    std::memcpy(buffer_.data(), memory_->data() + memory_offset_, take);
    memory_offset_ += take;
    buffer_offset_ = 0;
    buffer_size_ = take;
    return true;
  }
  if (!file_.valid()) {
    return false;
  }
  const std::uint64_t left = file_.size() - consumed_;
  if (left == 0) {
    return false;
  }
  const std::size_t want = static_cast<std::size_t>(
      left < kStreamBufferSize ? left : kStreamBufferSize);
  if (!file_.ReadAt(consumed_, buffer_.data(), want, error_message)) {
    return false;
  }
  buffer_offset_ = 0;
  buffer_size_ = want;
  return true;
}

// 短读语义：中途 EOF 直接返回 false，但已经拷进 out 的字节**不清零**，
// 调用方只能把这次读取整体当成失败。正常 EOF 不写 error_message（是不是
// 错误由格式层判断），只有真正的 I/O 错误才留下诊断。
bool SequentialReader::ReadExact(void* out, std::size_t size,
                                 std::string* error_message) {
  unsigned char* bytes = static_cast<unsigned char*>(out);
  std::size_t done = 0;
  while (done < size) {
    if (buffer_offset_ >= buffer_size_) {
      if (!Refill(error_message)) {
        return false;
      }
      continue;
    }
    const std::size_t available = buffer_size_ - buffer_offset_;
    const std::size_t take =
        (size - done) < available ? (size - done) : available;
    std::memcpy(bytes + done, buffer_.data() + buffer_offset_, take);
    buffer_offset_ += take;
    consumed_ += take;
    done += take;
  }
  return true;
}

// 单字节版本供比特层按需拉取：一次只消费 1 字节，缓冲区里剩下的留给下一次。
bool SequentialReader::ReadByte(std::uint8_t* out, std::string* error_message) {
  if (buffer_offset_ >= buffer_size_) {
    if (!Refill(error_message)) {
      return false;
    }
  }
  *out = buffer_.data()[buffer_offset_++];
  consumed_ += 1;
  return true;
}

// 用跳读而不是 seek：内存后端没有 seek，文件后端也用 ReadAt 顺序推进，
// 两条路的 consumed_ 语义这才完全一致。代价是必须真的读一遍被丢掉的字节。
bool SequentialReader::Discard(std::uint64_t bytes,
                               std::string* error_message) {
  std::vector<unsigned char> scratch(kDiscardBufferSize);
  std::uint64_t left = bytes;
  while (left > 0) {
    const std::size_t want = static_cast<std::size_t>(
        left < kDiscardBufferSize ? left : kDiscardBufferSize);
    if (!ReadExact(scratch.data(), want, error_message)) {
      return false;
    }
    left -= want;
  }
  return true;
}

// ---- BitWriter -------------------------------------------------------------

// 前置条件：code 只占低 length 位（本函数不做掩码，多出来的高位会被一起
// 移进累加器）。比特序是 MSB-first：先写的比特落在字节的高位。
// accumulator_ 里的历史位**故意**不清：取字节时只取低 buffered_bits_ 那几位，
// 残留位永远不会落进输出，所以不需要每次掩码。
bool BitWriter::WriteBits(std::uint32_t code, std::uint32_t length,
                          std::string* error_message) {
  if (length == 0) {
    return true;
  }
  if (sink_ == nullptr) {
    SetError(error_message, "internal: BitWriter 没有打开");
    return false;
  }
  accumulator_ = (accumulator_ << length) | code;
  buffered_bits_ += static_cast<int>(length);
  bits_written_ += length;
  while (buffered_bits_ >= 8) {
    buffered_bits_ -= 8;
    const std::uint8_t byte =
        static_cast<std::uint8_t>((accumulator_ >> buffered_bits_) & 0xFF);
    if (!sink_->Write(&byte, 1, error_message)) {
      return false;
    }
    payload_bytes_ += 1;
  }
  return true;
}

// 结束一条比特流的唯一合法方式：把不足一字节的部分补 0 写出去。不调用它就
// 会丢掉最后几个比特；重复调用是安全的（buffered_bits_ 已经是 0）。
bool BitWriter::Flush(std::string* error_message) {
  if (buffered_bits_ == 0) {
    return true;
  }
  if (sink_ == nullptr) {
    SetError(error_message, "internal: BitWriter 没有打开");
    return false;
  }
  const std::uint8_t byte =
      static_cast<std::uint8_t>((accumulator_ << (8 - buffered_bits_)) & 0xFF);
  if (!sink_->Write(&byte, 1, error_message)) {
    return false;
  }
  payload_bytes_ += 1;
  buffered_bits_ = 0;
  return true;
}

// ---- BitReader -------------------------------------------------------------

// bit_count 是硬上界：读满这么多比特就停，后面多出来的字节留给下一段格式
// （LZH1 的外层头部就在内层 HUF1 流前面）。reader 只借用，不拥有。
void BitReader::Open(SequentialReader* reader, std::uint64_t bit_count) {
  reader_ = reader;
  bit_count_ = bit_count;
  consumed_ = 0;
  current_ = 0;
  bits_left_ = 0;
}

// 两种 false 的含义不同：比特预算用完（正常结束，不写 error_message）与底层
// 字节流提前结束（写 error_message）。由调用方按语法判断哪一种是错误。
bool BitReader::ReadBit(std::uint32_t* bit, std::string* error_message) {
  if (consumed_ >= bit_count_) {
    return false;
  }
  if (bits_left_ == 0) {
    if (!reader_->ReadByte(&current_, error_message)) {
      SetError(error_message, "codec: bitstream 提前结束");
      return false;
    }
    bits_left_ = 8;
  }
  *bit = (current_ >> (bits_left_ - 1)) & 1;
  --bits_left_;
  consumed_ += 1;
  return true;
}

// ---- RollingHistory --------------------------------------------------------

// window 必须是 2 的幂：环形下标用 & mask_ 而不是 %，这是热路径上的取模
// 消除。ring_ 清零表示"历史里全是 0"。尺寸在打开时定死，中途不再分配。
bool RollingHistory::Open(ByteSinkAdapter* sink, std::size_t window,
                          std::string* error_message) {
  if (sink == nullptr) {
    SetError(error_message, "codec: sink 是空指针");
    return false;
  }
  if (window == 0 || (window & (window - 1)) != 0) {
    SetError(error_message, "codec: 历史窗口必须是 2 的幂");
    return false;
  }
  sink_ = sink;
  ring_.assign(window, 0);
  mask_ = window - 1;
  produced_ = 0;
  out_.clear();
  out_.reserve(kStreamBufferSize);
  return true;
}

// 唯一的写入口：先入环形历史再进输出缓冲，所以 match 取到的历史一定是当前
// 时刻最新的。输出攒够 kStreamBufferSize 才下发，减少 syscall 次数。
bool RollingHistory::Emit(std::uint8_t value, std::string* error_message) {
  ring_[static_cast<std::size_t>(produced_) & mask_] = value;
  produced_ += 1;
  out_.push_back(static_cast<char>(value));
  if (out_.size() >= kStreamBufferSize) {
    if (!sink_->Write(out_.data(), out_.size(), error_message)) {
      return false;
    }
    out_.clear();
  }
  return true;
}

// Push 与 CopyMatch 共用 Emit：字面量与 match 走同一条"入历史 + 出缓冲"的路。
bool RollingHistory::Push(std::uint8_t value, std::string* error_message) {
  return Emit(value, error_message);
}

// 前置条件由格式层保证：1 <= distance <= window 且 distance <= produced_。
// 违反时读到的只是清零后的历史，这里不做检查 —— 检查发生在 token 解析处。
bool RollingHistory::CopyMatch(std::size_t distance, std::size_t length,
                               std::string* error_message) {
  for (std::size_t index = 0; index < length; ++index) {
    // 逐字节取：distance < length 时刚写出去的字节会被立刻再读到。
    const std::uint8_t value =
        ring_[static_cast<std::size_t>(produced_ - distance) & mask_];
    if (!Emit(value, error_message)) {
      return false;
    }
  }
  return true;
}

// 解码结束时必须调用，否则输出缓冲里最后不足一块的数据不会交给 sink。
bool RollingHistory::Flush(std::string* error_message) {
  if (out_.empty()) {
    return true;
  }
  if (!sink_->Write(out_.data(), out_.size(), error_message)) {
    return false;
  }
  out_.clear();
  return true;
}

}  // namespace compression
}  // namespace backupproject
