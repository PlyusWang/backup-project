// codec_io.cpp
//
// 见 codec_io.h。

#include "codec_io.h"

#include <cstring>

namespace backupproject {
namespace compression {

namespace {

void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) {
    *error_message = text;
  }
}

constexpr std::size_t kDiscardBufferSize = 64 * 1024;

}  // namespace

// ---- ByteSinkAdapter -------------------------------------------------------

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

void ByteSinkAdapter::OpenMemory(std::string* out) {
  file_ = nullptr;
  memory_ = out;
  bytes_written_ = 0;
}

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

void SequentialReader::OpenMemory(const std::string* data) {
  Close();
  memory_ = data;
  total_size_ = data == nullptr ? 0 : data->size();
  buffer_.assign(kStreamBufferSize, 0);
}

void SequentialReader::Close() {
  file_.Close();
  memory_ = nullptr;
  total_size_ = 0;
  memory_offset_ = 0;
  buffer_offset_ = 0;
  buffer_size_ = 0;
  consumed_ = 0;
}

std::uint64_t SequentialReader::remaining() const {
  return total_size_ > consumed_ ? total_size_ - consumed_ : 0;
}

// 只在缓冲区彻底用空时调用：此时 consumed_ 正好等于"已经从后端取走的字节数"，
// 所以文件后端可以安全地从 consumed_ 继续读。
bool SequentialReader::Refill(std::string* error_message) {
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

void BitReader::Open(SequentialReader* reader, std::uint64_t bit_count) {
  reader_ = reader;
  bit_count_ = bit_count;
  consumed_ = 0;
  current_ = 0;
  bits_left_ = 0;
}

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

bool RollingHistory::Push(std::uint8_t value, std::string* error_message) {
  return Emit(value, error_message);
}

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
