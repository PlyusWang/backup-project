// pack_stream.cpp
//
// 打包层的调度：三种 PackMethod 的入口、格式探测、以及统一的读取器。
//
// 这里不实现任何一种格式：MyPack v2 在 mypack_v2.cpp，USTAR 在 ustar.cpp。
// 本文件只负责"按 id 选后端"和"把 USTAR 的 Member 翻译成通用的 PackedEntry"，
// 这样上游（压缩层、加密层、恢复编排）只认一种数据类型。

#include "pack_stream.h"

#include <cstring>
#include <utility>

#include "archive_path.h"
#include "byte_order.h"
#include "ustar.h"

namespace backupproject {

namespace {

void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) {
    *error_message = text;
  }
}

PackedEntry FromUstarMember(const ustar::Member& member) {
  PackedEntry record;
  record.entry = member.entry;
  record.entry.source_path.clear();
  record.data_offset = member.data_offset;
  record.data_size = member.data_size;
  return record;
}

ustar::Member ToUstarMember(const PackedEntry& record) {
  ustar::Member member;
  member.entry = record.entry;
  member.entry.source_path.clear();
  member.data_offset = record.data_offset;
  member.data_size = record.data_size;
  return member;
}

}  // namespace

const char* PackMethodName(PackMethod method) {
  switch (method) {
    case PackMethod::kMyPack:
      return "mypack";
    case PackMethod::kUstar:
      return "ustar";
    case PackMethod::kFastUstar:
      return "fast-ustar";
  }
  return "unknown";
}

bool ParsePackMethodId(std::uint8_t id, PackMethod* method) {
  switch (id) {
    case 0:
      *method = PackMethod::kMyPack;
      return true;
    case 1:
      *method = PackMethod::kUstar;
      return true;
    case 2:
      *method = PackMethod::kFastUstar;
      return true;
    default:
      return false;
  }
}

bool PackEntries(PackMethod method, const std::vector<ArchiveEntry>& entries,
                 const std::string& output_file, std::string* error_message) {
  if (output_file.empty()) {
    SetError(error_message, "Packed stream path is empty.");
    return false;
  }
  if (method != PackMethod::kMyPack) {
    // 两个 USTAR 后端自己用 O_CREAT|O_EXCL 创建输出，失败时删掉半成品。
    return method == PackMethod::kUstar
               ? ustar::WriteBaseline(entries, output_file, error_message)
               : ustar::WriteFast(entries, output_file, error_message);
  }
  FileSink sink;
  if (!sink.Open(output_file, error_message)) {
    return false;
  }
  if (!WriteMyPackV2(entries, &sink, error_message)) {
    sink.Abandon();
    return false;
  }
  if (!sink.Close(error_message)) {
    sink.Abandon();
    return false;
  }
  return true;
}

bool DetectPackMethod(const std::string& packed_file, PackMethod* method,
                      std::string* error_message) {
  FileSource source;
  if (!source.Open(packed_file, error_message)) {
    return false;
  }
  unsigned char head[mypack_v2::kGlobalHeaderSize];
  if (source.size() < mypack_v2::kGlobalHeaderSize ||
      !source.ReadAt(0, head, sizeof(head), error_message)) {
    SetError(error_message,
             "File is too small to be a packed stream: " + packed_file);
    return false;
  }
  constexpr unsigned char kMagic[8] = {'B', 'K', 'P', 'A', 'R', 'C', 'H', '\0'};
  if (std::memcmp(head, kMagic, sizeof(kMagic)) == 0) {
    const std::uint16_t version = LoadU16LE(head + 8);
    if (version == mypack_v2::kFormatVersion) {
      *method = PackMethod::kMyPack;
      return true;
    }
    SetError(error_message, "BKPARCH stream is version " +
                                std::to_string(version) +
                                ", not MyPack v2: " + packed_file);
    return false;
  }
  // USTAR 没有 magic，只能靠第一个 512 字节块的 checksum + "ustar" magic 判断。
  if (source.size() >= ustar::kBlockSize) {
    unsigned char block[ustar::kBlockSize];
    if (!source.ReadAt(0, block, sizeof(block), error_message)) {
      return false;
    }
    ustar::Header header;
    std::string decode_error;
    if (ustar::DecodeHeader(reinterpret_cast<const char*>(block), &header,
                            &decode_error)) {
      *method = PackMethod::kUstar;
      return true;
    }
  }
  SetError(error_message, "Unrecognized packed stream format: " + packed_file);
  return false;
}

bool PackedStreamReader::Open(const std::string& packed_file,
                              std::string* error_message) {
  entries_.clear();
  if (!source_.Open(packed_file, error_message)) {
    return false;
  }
  summary_.packed_size = source_.size();
  return true;
}

bool PackedStreamReader::Scan(PackMethod method, std::string* error_message) {
  if (!source_.valid()) {
    SetError(error_message, "Internal error: packed stream is not open");
    return false;
  }
  method_ = method;
  entries_.clear();
  std::uint64_t entry_count = 0;
  switch (method) {
    case PackMethod::kMyPack:
      if (!ScanMyPackV2(source_, &entries_, &entry_count, error_message)) {
        return false;
      }
      break;
    case PackMethod::kUstar:
    case PackMethod::kFastUstar: {
      // 两种 USTAR 的 wire format 相同，所以这里用同一个 reader。
      std::vector<ustar::Member> members;
      if (!ustar::Scan(source_.path(), &members, error_message)) {
        return false;
      }
      entries_.reserve(members.size());
      for (const ustar::Member& member : members) {
        entries_.push_back(FromUstarMember(member));
      }
      entry_count = entries_.size();
      break;
    }
  }
  if (entries_.empty()) {
    SetError(error_message, "Packed stream has no entries: " + source_.path());
    return false;
  }
  summary_.method = method;
  summary_.entry_count = entry_count;
  summary_.packed_size = source_.size();
  return true;
}

bool PackedStreamReader::ExtractPayload(std::size_t index,
                                        const std::string& destination_file,
                                        std::string* error_message) const {
  if (index >= entries_.size()) {
    SetError(error_message, "Internal error: entry index out of range");
    return false;
  }
  const PackedEntry& record = entries_[index];
  if (record.entry.type != EntryType::kRegularFile) {
    SetError(error_message,
             "Entry has no payload: " + record.entry.archive_path);
    return false;
  }
  if (method_ != PackMethod::kMyPack) {
    return ustar::ExtractData(source_.path(), ToUstarMember(record),
                              destination_file, error_message);
  }
  FileSink sink;
  if (!sink.Open(destination_file, error_message)) {
    return false;
  }
  if (!source_.CopyRangeTo(record.data_offset, record.data_size, &sink,
                           error_message)) {
    sink.Abandon();
    return false;
  }
  if (!sink.Close(error_message)) {
    sink.Abandon();
    return false;
  }
  return true;
}

bool PackedStreamReader::ExtractPayloadToMemory(
    std::size_t index, std::string* output, std::string* error_message) const {
  if (index >= entries_.size()) {
    SetError(error_message, "Internal error: entry index out of range");
    return false;
  }
  const PackedEntry& record = entries_[index];
  if (record.entry.type != EntryType::kRegularFile) {
    SetError(error_message,
             "Entry has no payload: " + record.entry.archive_path);
    return false;
  }
  if (method_ != PackMethod::kMyPack) {
    return ustar::ExtractDataToString(source_.path(), ToUstarMember(record),
                                      output, error_message);
  }
  output->assign(static_cast<std::size_t>(record.data_size), '\0');
  if (record.data_size > 0 && !source_.ReadAt(record.data_offset, &(*output)[0],
                                              output->size(), error_message)) {
    return false;
  }
  return true;
}

}  // namespace backupproject
