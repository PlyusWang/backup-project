// pack_stream.cpp
//
// 打包层的调度：三种 PackMethod 的入口、格式探测、以及统一的读取器。
//
// 这里不实现任何一种格式：MyPack v2 在 mypack_v2.cpp，USTAR 在 ustar.cpp。
// 本文件只负责"按 id 选后端"和"把 USTAR 的 Member 翻译成通用的 PackedEntry"，
// 这样上游（压缩层、加密层、恢复编排）只认一种数据类型。

// 职责边界：只做"按 id 选后端 + 类型翻译 + 统一的读取/抽取入口"。本文件
// 不实现任何容器格式，也不碰压缩、加密与 manifest —— 那些属于各自的层。
// 数据流：ArchiveEntry 列表 -> PackEntries() -> MyPack v2 / USTAR 写出；
// packed_file -> DetectPackMethod() -> Scan() -> PackedEntry -> 抽取正文。
//
// 不变量：Scan 之后 entries_ 与 method_ 必须来自同一次后端调用 —— method_
// 决定抽取走哪个分支，entries_ 的下标就是抽取索引，两者错配会读到错误的
// 字节区间。Scan 因此每次整体重置这两个成员，而不是增量追加。
//
// 失败语义：全部入口返回 bool + 可选 error_message（可为 nullptr），不抛
// 异常。写路径的半成品由 FileSink::Abandon 回收，本层不留下截断的文件。
//
// 线程约束：无可变全局状态；但 PackedStreamReader 不加锁、不可重入，
// 同一实例同一时刻只能由一个线程使用。
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

// PackedEntry 是上游唯一认识的通用条目类型。USTAR 的 Member 里没有
// source_path（那是 scheduler 侧的归属信息，不属于 tar 头），所以两个方向
// 都显式清空它，避免把上一次的残留值当成事实漏给上层。
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

// 这些字符串是持久化 id（进配置、进 QML 选择器），不是显示名：显示名在
// backup_option_keys.cpp。改名等于让旧配置解析不出来，只能追加不能替换。
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

// 归档头里记录的数值 id：0=mypack、1=ustar、2=fast-ustar。这个映射由写入
// 方决定，不能重排。未知 id 返回 false 且**不写** *method，调用方必须据此
// 报"不认识的格式"，而不是沿用默认值继续读。
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

// 写出一个完整的打包层文件。
// 前置条件：output_file 非空，且**尚不存在** —— FileSink::Open 与两个
// USTAR 后端都用 O_CREAT|O_EXCL 独占创建，覆盖已有文件在这里是错误而不是
// 覆盖语义。entries 的顺序就是落盘顺序，本层不重排。
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

// 探测顺序是有意的：先认 MyPack 的 8 字节 magic（要么确定命中，要么明确
// 报版本不符），再退回 USTAR —— USTAR 没有 magic，只能靠第一个 512 字节块
// 的 header checksum 判断，代价更高，而且理论上可能与任意数据碰撞。
// 失败时 *method 不被改写：调用方只能按返回值决定要不要使用它。
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

// 按调用方给定的 method 解析索引，刻意**不**自己再探测一次：上层已经把
// DetectPackMethod 的结果传了下来，重探测只会给"读的格式"与"认的格式"
// 制造分歧的机会。
// 失败时 entries_ 可能已被后端部分填充，调用方只能看返回值，不能拿
// entries_ 是否为空来判断成功与否。
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

// 把第 index 条条目的正文写到 destination_file（路径由调用方选定）。
// 前置条件：Scan 已成功，index 是 entries_ 的下标。
// 只有 kRegularFile 有正文：目录 / 符号链接走到这里是数据问题，不是调用方
// 的编码错误，所以按普通失败返回并带上 archive_path 便于定位。
// 失败时目标文件被 Abandon 回收，不留截断正文 —— 恢复流程可以直接重试。
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

// 内存版抽取，供 manifest 与小文件校验使用；上限由调用方把关，本函数不限。
// 注意：失败时 *output 已经被 resize 成 data_size 字节的全零缓冲区，只是
// 内容不可信；调用方必须按返回值决定要不要使用它。
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
