// pack_stream.h
//
// 打包层的统一接口：三种 PackMethod 产出的都是"packed 流"，
// 之后的压缩层与加密层只把它当成一段字节，完全不关心里面有路径还是 uid。
//
//   PackMethod::kMyPack     → BKPARCH\0 + version 2 的扩展格式（本文件同目录的
//   mypack_v2.cpp） PackMethod::kUstar      → 标准 USTAR（POSIX tar）
//   PackMethod::kFastUstar  → 同样是标准 USTAR，只是 I/O 策略不同
//
// kUstar 与 kFastUstar 的 wire format 完全兼容：restore 走同一个 USTAR reader，
// 两者的差别只在 syscall 数量、缓冲大小和数据搬运方式。

#ifndef BACKUP_PROJECT_INCLUDE_PACK_STREAM_H_
#define BACKUP_PROJECT_INCLUDE_PACK_STREAM_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "archive_entry.h"
#include "file_io.h"

namespace backupproject {

// 打包策略 id。数值就是 v2 外层 container header 里的 pack_method 字段。
enum class PackMethod : std::uint8_t {
  kMyPack = 0,
  kUstar = 1,
  kFastUstar = 2,
};

const char* PackMethodName(PackMethod method);
bool ParsePackMethodId(std::uint8_t id, PackMethod* method);

// MyPack v2 的格式常量，与 docs/format/archive_v2_container.md
// 的偏移表一一对应。
namespace mypack_v2 {

inline constexpr std::size_t kGlobalHeaderSize = 32;
inline constexpr std::size_t kEntryHeaderSize = 64;
inline constexpr std::uint16_t kFormatVersion = 2;
inline constexpr std::uint16_t kFormatFlags = 0;
inline constexpr std::uint32_t kPermissionMask = 07777;
inline constexpr std::uint32_t kMaxMtimeNsec = 999999999;
inline constexpr std::size_t kMaxLinkLength = 4096;
// entry header 里保留区必须真的为 0，读侧不"反正没人用"地放过它。
inline constexpr std::size_t kEntryHeaderReservedSize = 12;

}  // namespace mypack_v2

// packed 流里的一条记录。payload 用"偏移 + 长度"描述，不把内容读进内存。
struct PackedEntry {
  ArchiveEntry entry;
  std::uint64_t data_offset = 0;
  std::uint64_t data_size = 0;
};

// packed 流的头部摘要：不需要遍历 entry、不需要密码就能读到。
struct PackedSummary {
  PackMethod method = PackMethod::kMyPack;
  std::uint64_t entry_count = 0;
  std::uint64_t packed_size = 0;
};

// 按 method 把条目列表写成 packed 流。
//
// 输出文件必须不存在：三种后端都用 O_CREAT|O_EXCL 创建，失败时删掉自己写的
// 半成品。失败保证不留下任何输出文件。
bool PackEntries(PackMethod method, const std::vector<ArchiveEntry>& entries,
                 const std::string& output_file, std::string* error_message);

// 探测 packed 流的格式：读文件开头，看 MyPack 的 magic/version，
// 否则按 USTAR 检查第一个 512 字节块的 checksum 与 "ustar" magic。
bool DetectPackMethod(const std::string& packed_file, PackMethod* method,
                      std::string* error_message);

// packed 流读取器：打开一次 → preflight 一次 → 逐条抽取 payload。
//
// preflight（Scan）完整校验整个流，任何一条不合法都返回 false，并且此时
// 还没有写过任何东西。抽取 payload 是第二阶段的事。
class PackedStreamReader {
 public:
  PackedStreamReader() = default;

  // 打开文件。不解析 entry，也不猜格式。
  bool Open(const std::string& packed_file, std::string* error_message);
  // preflight：按 method（来自 container header）完整校验并解出条目表。
  bool Scan(PackMethod method, std::string* error_message);

  PackMethod method() const { return method_; }
  const std::vector<PackedEntry>& entries() const { return entries_; }
  const PackedSummary& summary() const { return summary_; }

  // 把第 index 条的 payload 写到 destination_file（O_CREAT|O_EXCL）。
  // 非普通文件的条目（目录/软链接/FIFO/设备/硬链接）没有 payload，
  // 调用它会失败——这是调用方的编排错误，不是数据问题。
  bool ExtractPayload(std::size_t index, const std::string& destination_file,
                      std::string* error_message) const;
  bool ExtractPayloadToMemory(std::size_t index, std::string* output,
                              std::string* error_message) const;

  bool valid() const { return source_.valid(); }
  std::uint64_t packed_size() const { return source_.size(); }

 private:
  FileSource source_;
  PackMethod method_ = PackMethod::kMyPack;
  std::vector<PackedEntry> entries_;
  PackedSummary summary_;
};

// ---- MyPack v2 的读写实现（src/archive/mypack_v2.cpp）----
//
// 这两个函数由 PackedStreamReader / PackEntries 调用，公开出来是为了让
// "v2 格式只有一份读写实现"这件事在头文件里也看得见。
bool WriteMyPackV2(const std::vector<ArchiveEntry>& entries, FileSink* sink,
                   std::string* error_message);
bool ScanMyPackV2(const FileSource& source, std::vector<PackedEntry>* entries,
                  std::uint64_t* entry_count, std::string* error_message);

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_PACK_STREAM_H_
