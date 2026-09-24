// mypack_v2.cpp
//
// MyPack v2：v0.1 的后继格式，把"只认目录和普通文件"扩到完整的 POSIX 条目集合。
//
// 与 v0.1 的关系：
//   * magic 仍然是 BKPARCH\0，靠 version 字段分流：1 = legacy v0.1（实现留在
//     archive.cpp，一个字都没动），2 = 本文件的扩展格式，其它值一律拒绝；
//   * 全局 header 从 24 字节扩到 32 字节，entry header 从 32 字节扩到 64 字节；
//   * 新增 uid/gid/07777 mode/软链接目标/硬链接目标/设备号。
//
// 读侧仍然是"白名单式"判断：不认识的 type、非 0 的保留字段、越界的长度、
// 多出来的尾巴字节全部拒绝。宽松解析能让更多坏样本通过，代价是用户拿到一份
// 不完整的恢复结果却以为成功了。

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "archive_path.h"
#include "byte_order.h"
#include "pack_stream.h"

namespace backupproject {

namespace {

void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) {
    *error_message = text;
  }
}

std::string Describe(int error_number, const std::string& action,
                     const std::string& path) {
  return action + ": " + path + ": " + std::strerror(error_number);
}

constexpr unsigned char kMagic[8] = {'B', 'K', 'P', 'A', 'R', 'C', 'H', '\0'};
constexpr std::size_t kGlobalMagicSize = sizeof(kMagic);
constexpr std::size_t kGlobalReservedSize = 8;

// ---- 写侧 -----------------------------------------------------------------

void EncodeGlobalHeader(std::uint64_t entry_count, std::string* out) {
  out->clear();
  out->append(reinterpret_cast<const char*>(kMagic), kGlobalMagicSize);
  AppendU16LE(out, mypack_v2::kFormatVersion);
  AppendU16LE(out, mypack_v2::kFormatFlags);
  AppendU32LE(out, static_cast<std::uint32_t>(mypack_v2::kGlobalHeaderSize));
  AppendU64LE(out, entry_count);
  out->append(kGlobalReservedSize, '\0');
}

// entry header 的字段顺序与宽度见 docs/format/archive_v2_container.md。
// 顺序写错或漏掉一个字段，读侧就会整片错位，而这种错位在写入时看不出来，
// 所以这里当场自检长度。
void EncodeEntryHeader(const ArchiveEntry& entry, std::uint64_t payload_size,
                       std::string* out) {
  out->clear();
  out->push_back(static_cast<char>(static_cast<std::uint8_t>(entry.type)));
  out->push_back('\0');  // flags
  AppendU16LE(out, 0);   // reserved0
  AppendU32LE(out, static_cast<std::uint32_t>(entry.archive_path.size()));
  AppendU32LE(out, static_cast<std::uint32_t>(entry.link_target.size()));
  AppendU32LE(out, entry.mode);
  AppendU32LE(out, entry.uid);
  AppendU32LE(out, entry.gid);
  AppendU32LE(out, entry.mtime_nsec);
  // mtime_sec 是有符号的，按补码转成无符号再按 LE 写。
  AppendU64LE(out, static_cast<std::uint64_t>(entry.mtime_sec));
  AppendU64LE(out, payload_size);
  AppendU32LE(out, entry.dev_major);
  AppendU32LE(out, entry.dev_minor);
  out->append(mypack_v2::kEntryHeaderReservedSize, '\0');
  if (out->size() != mypack_v2::kEntryHeaderSize) {
    out->clear();
  }
}

// 这条 entry 在流里带多少 payload。
std::uint64_t PayloadSizeOf(const ArchiveEntry& entry) {
  return entry.type == EntryType::kRegularFile ? entry.size : 0;
}

// 写侧对每条 entry 的语义自检：格式允许的组合是固定的，任何"不可能组合"
// 都应该在写出去之前就失败，而不是产出一个读侧必然拒绝的归档。
bool ValidateEntryForWriting(const ArchiveEntry& entry, bool is_first,
                             std::string* error_message) {
  if (entry.type == EntryType::kSocket) {
    SetError(error_message,
             "Unsupported special type: socket: " + entry.archive_path);
    return false;
  }
  if (!IsValidArchivePath(entry.archive_path, is_first,
                          entry.type == EntryType::kDirectory,
                          kMaxArchivePathLength, error_message)) {
    return false;
  }
  if ((entry.mode & ~mypack_v2::kPermissionMask) != 0) {
    SetError(error_message, "Entry mode out of range: " + entry.archive_path);
    return false;
  }
  if (entry.mtime_nsec > mypack_v2::kMaxMtimeNsec) {
    SetError(error_message,
             "Entry mtime nanoseconds out of range: " + entry.archive_path);
    return false;
  }
  if (entry.link_target.size() > mypack_v2::kMaxLinkLength) {
    SetError(error_message,
             "Entry link target too long: " + entry.archive_path);
    return false;
  }
  switch (entry.type) {
    case EntryType::kRegularFile:
      if (!entry.link_target.empty()) {
        SetError(error_message, "Regular file entry must not carry a link: " +
                                    entry.archive_path);
        return false;
      }
      break;
    case EntryType::kDirectory:
    case EntryType::kFifo:
    case EntryType::kCharDevice:
    case EntryType::kBlockDevice:
      if (!entry.link_target.empty()) {
        SetError(error_message,
                 "Entry must not carry a link: " + entry.archive_path);
        return false;
      }
      break;
    case EntryType::kSymlink:
    case EntryType::kHardLink:
      if (entry.link_target.empty()) {
        SetError(error_message,
                 "Entry is missing its link target: " + entry.archive_path);
        return false;
      }
      break;
    case EntryType::kSocket:
      return false;
  }
  return true;
}

// 普通文件正文必须流式照抄：固定 64 KiB 缓冲，绝不把文件读进内存。
//
// 与 v0.1 一样有两层保护，缺一层都会产出"看起来成功、其实内容对不上"的归档：
// 读取循环以扫描时的 size 为准，读不满就是源文件被截短了；读满之后再 fstat
// 一次，size 或 mtime 变了就失败。v2 不做快照，只是不假装成功。
bool WriteRegularPayload(const std::string& disk_path,
                         const ArchiveEntry& entry, FileSink* sink,
                         std::string* error_message) {
  const int raw_fd =
      ::open(disk_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (raw_fd < 0) {
    SetError(error_message,
             Describe(errno, "Failed to open source file", disk_path));
    return false;
  }
  std::vector<unsigned char> buffer(64 * 1024);
  std::uint64_t copied = 0;
  bool ok = true;
  while (copied < entry.size) {
    const std::uint64_t remaining = entry.size - copied;
    const std::size_t want = static_cast<std::size_t>(
        remaining < buffer.size() ? remaining : buffer.size());
    const ssize_t got = ::read(raw_fd, buffer.data(), want);
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      SetError(error_message,
               Describe(errno, "Failed to read source file", disk_path));
      ok = false;
      break;
    }
    if (got == 0) {
      SetError(error_message, "Source file shrank while packing: " + disk_path);
      ok = false;
      break;
    }
    if (!sink->Write(buffer.data(), static_cast<std::size_t>(got),
                     error_message)) {
      ok = false;
      break;
    }
    copied += static_cast<std::uint64_t>(got);
  }
  if (ok) {
    struct stat after;
    if (::fstat(raw_fd, &after) != 0) {
      SetError(error_message,
               Describe(errno, "Failed to re-inspect source file", disk_path));
      ok = false;
    } else {
      const bool size_changed =
          static_cast<std::uint64_t>(after.st_size) != entry.size;
      const bool mtime_changed =
          static_cast<std::int64_t>(after.st_mtim.tv_sec) != entry.mtime_sec ||
          static_cast<std::uint32_t>(after.st_mtim.tv_nsec) != entry.mtime_nsec;
      if (size_changed || mtime_changed) {
        SetError(error_message,
                 "Source file changed while packing: " + disk_path);
        ok = false;
      }
    }
  }
  ::close(raw_fd);
  return ok;
}

// ---- 读侧 -----------------------------------------------------------------

struct GlobalHeaderFields {
  std::uint64_t entry_count = 0;
};

bool DecodeGlobalHeader(const unsigned char* block, std::size_t size,
                        const std::string& archive_path,
                        GlobalHeaderFields* fields,
                        std::string* error_message) {
  if (size < mypack_v2::kGlobalHeaderSize) {
    SetError(error_message, "Truncated MyPack v2 header: " + archive_path);
    return false;
  }
  if (std::memcmp(block, kMagic, kGlobalMagicSize) != 0) {
    SetError(error_message, "Invalid archive magic: " + archive_path);
    return false;
  }
  std::size_t cursor = kGlobalMagicSize;
  std::uint16_t version = 0;
  std::uint16_t flags = 0;
  std::uint32_t header_size = 0;
  std::uint64_t entry_count = 0;
  if (!ReadU16LE(block, size, &cursor, &version) ||
      !ReadU16LE(block, size, &cursor, &flags) ||
      !ReadU32LE(block, size, &cursor, &header_size) ||
      !ReadU64LE(block, size, &cursor, &entry_count)) {
    SetError(error_message, "Truncated MyPack v2 header: " + archive_path);
    return false;
  }
  if (version != mypack_v2::kFormatVersion) {
    SetError(error_message, "Unsupported archive version: " +
                                std::to_string(static_cast<int>(version)));
    return false;
  }
  if (flags != mypack_v2::kFormatFlags) {
    SetError(error_message, "Unsupported archive flags: " +
                                std::to_string(static_cast<int>(flags)));
    return false;
  }
  if (header_size != mypack_v2::kGlobalHeaderSize) {
    SetError(error_message,
             "Invalid archive header size: " + std::to_string(header_size));
    return false;
  }
  for (std::size_t index = 0; index < kGlobalReservedSize; ++index) {
    if (block[cursor + index] != 0) {
      SetError(error_message,
               "Non-zero reserved bytes in archive header: " + archive_path);
      return false;
    }
  }
  fields->entry_count = entry_count;
  return true;
}

bool TypeFromId(std::uint8_t id, EntryType* type) {
  switch (id) {
    case 1:
      *type = EntryType::kDirectory;
      return true;
    case 2:
      *type = EntryType::kRegularFile;
      return true;
    case 3:
      *type = EntryType::kSymlink;
      return true;
    case 4:
      *type = EntryType::kHardLink;
      return true;
    case 5:
      *type = EntryType::kFifo;
      return true;
    case 6:
      *type = EntryType::kCharDevice;
      return true;
    case 7:
      *type = EntryType::kBlockDevice;
      return true;
    default:
      return false;
  }
}

// 读一条 entry header，并把"类型与其它字段是否自洽"一并查完。
bool DecodeEntryHeader(const unsigned char* block, const std::string& hint,
                       ArchiveEntry* entry, std::uint64_t* payload_size,
                       std::uint32_t* path_length, std::uint32_t* link_length,
                       std::string* error_message) {
  std::size_t cursor = 0;
  std::uint8_t type_id = 0;
  std::uint8_t flags = 0;
  std::uint16_t reserved0 = 0;
  std::uint32_t mode = 0;
  std::uint32_t uid = 0;
  std::uint32_t gid = 0;
  std::uint32_t mtime_nsec = 0;
  std::uint64_t mtime_sec = 0;
  std::uint64_t size = 0;
  std::uint32_t dev_major = 0;
  std::uint32_t dev_minor = 0;
  if (!ReadU8(block, mypack_v2::kEntryHeaderSize, &cursor, &type_id) ||
      !ReadU8(block, mypack_v2::kEntryHeaderSize, &cursor, &flags) ||
      !ReadU16LE(block, mypack_v2::kEntryHeaderSize, &cursor, &reserved0) ||
      !ReadU32LE(block, mypack_v2::kEntryHeaderSize, &cursor, path_length) ||
      !ReadU32LE(block, mypack_v2::kEntryHeaderSize, &cursor, link_length) ||
      !ReadU32LE(block, mypack_v2::kEntryHeaderSize, &cursor, &mode) ||
      !ReadU32LE(block, mypack_v2::kEntryHeaderSize, &cursor, &uid) ||
      !ReadU32LE(block, mypack_v2::kEntryHeaderSize, &cursor, &gid) ||
      !ReadU32LE(block, mypack_v2::kEntryHeaderSize, &cursor, &mtime_nsec) ||
      !ReadU64LE(block, mypack_v2::kEntryHeaderSize, &cursor, &mtime_sec) ||
      !ReadU64LE(block, mypack_v2::kEntryHeaderSize, &cursor, &size) ||
      !ReadU32LE(block, mypack_v2::kEntryHeaderSize, &cursor, &dev_major) ||
      !ReadU32LE(block, mypack_v2::kEntryHeaderSize, &cursor, &dev_minor)) {
    SetError(error_message, "Truncated entry header: " + hint);
    return false;
  }
  for (std::size_t index = 0; index < mypack_v2::kEntryHeaderReservedSize;
       ++index) {
    if (block[cursor + index] != 0) {
      SetError(error_message,
               "Non-zero reserved bytes in entry header: " + hint);
      return false;
    }
  }
  if (flags != 0 || reserved0 != 0) {
    SetError(error_message, "Non-zero entry flags: " + hint);
    return false;
  }
  EntryType type = EntryType::kRegularFile;
  if (!TypeFromId(type_id, &type)) {
    SetError(error_message,
             "Unknown entry type " + std::to_string(type_id) + ": " + hint);
    return false;
  }
  if (*path_length == 0 || *path_length > kMaxArchivePathLength) {
    SetError(error_message, "Invalid entry path length: " + hint);
    return false;
  }
  if (*link_length > mypack_v2::kMaxLinkLength) {
    SetError(error_message, "Invalid entry link length: " + hint);
    return false;
  }
  if (mode > mypack_v2::kPermissionMask) {
    SetError(error_message, "Entry mode out of range: " + hint);
    return false;
  }
  if (mtime_nsec > mypack_v2::kMaxMtimeNsec) {
    SetError(error_message, "Entry mtime nanoseconds out of range: " + hint);
    return false;
  }
  // 类型与其它字段的自洽性：目录/FIFO/设备不带 link 也不带 payload，
  // 软链接/硬链接必须带 link 但 payload 为 0，普通文件不带 link。
  switch (type) {
    case EntryType::kDirectory:
    case EntryType::kFifo:
    case EntryType::kCharDevice:
    case EntryType::kBlockDevice:
      if (*link_length != 0 || size != 0) {
        SetError(error_message,
                 "Entry of this type must not carry link or payload: " + hint);
        return false;
      }
      break;
    case EntryType::kSymlink:
    case EntryType::kHardLink:
      if (*link_length == 0 || size != 0) {
        SetError(error_message, "Invalid link entry layout: " + hint);
        return false;
      }
      break;
    case EntryType::kRegularFile:
      if (*link_length != 0) {
        SetError(error_message,
                 "Regular file entry must not carry a link: " + hint);
        return false;
      }
      break;
    case EntryType::kSocket:
      SetError(error_message, "Unsupported special type: socket: " + hint);
      return false;
  }

  entry->type = type;
  entry->mode = mode;
  entry->uid = uid;
  entry->gid = gid;
  entry->mtime_sec = static_cast<std::int64_t>(mtime_sec);
  entry->mtime_nsec = mtime_nsec;
  entry->dev_major = dev_major;
  entry->dev_minor = dev_minor;
  *payload_size = size;
  return true;
}

}  // namespace

bool WriteMyPackV2(const std::vector<ArchiveEntry>& entries, FileSink* sink,
                   std::string* error_message) {
  if (sink == nullptr) {
    SetError(error_message, "Internal error: null sink");
    return false;
  }
  if (entries.empty() || entries.front().archive_path != "." ||
      entries.front().type != EntryType::kDirectory) {
    SetError(
        error_message,
        "Internal error: MyPack v2 requires the source root as first entry");
    return false;
  }

  std::string buffer;
  EncodeGlobalHeader(entries.size(), &buffer);
  if (buffer.size() != mypack_v2::kGlobalHeaderSize) {
    SetError(error_message, "Internal error: bad MyPack v2 global header size");
    return false;
  }
  if (!sink->Write(buffer.data(), buffer.size(), error_message)) {
    return false;
  }

  ArchivePathRegistry registry(/*require_parent_first=*/true);
  for (std::size_t index = 0; index < entries.size(); ++index) {
    const ArchiveEntry& entry = entries[index];
    if (!ValidateEntryForWriting(entry, index == 0, error_message)) {
      return false;
    }
    if (!registry.Add(entry.archive_path, entry.type == EntryType::kDirectory,
                      error_message)) {
      return false;
    }
    const std::uint64_t payload_size = PayloadSizeOf(entry);
    EncodeEntryHeader(entry, payload_size, &buffer);
    if (buffer.size() != mypack_v2::kEntryHeaderSize) {
      SetError(error_message,
               "Internal error: bad MyPack v2 entry header size");
      return false;
    }
    if (!sink->Write(buffer.data(), buffer.size(), error_message)) {
      return false;
    }
    if (!sink->Write(entry.archive_path.data(), entry.archive_path.size(),
                     error_message)) {
      return false;
    }
    if (!entry.link_target.empty() &&
        !sink->Write(entry.link_target.data(), entry.link_target.size(),
                     error_message)) {
      return false;
    }
    if (payload_size > 0 &&
        !WriteRegularPayload(entry.source_path, entry, sink, error_message)) {
      return false;
    }
  }
  return true;
}

bool ScanMyPackV2(const FileSource& source, std::vector<PackedEntry>* entries,
                  std::uint64_t* entry_count, std::string* error_message) {
  const std::string& archive_path = source.path();
  const std::uint64_t file_size = source.size();

  unsigned char block[mypack_v2::kGlobalHeaderSize];
  if (file_size < mypack_v2::kGlobalHeaderSize) {
    SetError(error_message, "Truncated MyPack v2 header: " + archive_path);
    return false;
  }
  if (!source.ReadAt(0, block, sizeof(block), error_message)) {
    return false;
  }
  GlobalHeaderFields fields;
  if (!DecodeGlobalHeader(block, sizeof(block), archive_path, &fields,
                          error_message)) {
    return false;
  }

  entries->clear();
  ArchivePathRegistry registry(/*require_parent_first=*/true);
  // hardlink 目标必须在整条流里都存在，所以先把"路径 -> 类型"记下来，
  // 等全部条目解完再统一校验。
  std::unordered_map<std::string, EntryType> declared;
  std::vector<std::size_t> hardlinks;

  std::uint64_t cursor = mypack_v2::kGlobalHeaderSize;
  unsigned char entry_block[mypack_v2::kEntryHeaderSize];
  for (std::uint64_t index = 0; index < fields.entry_count; ++index) {
    // "还够不够读下一条 header"用减法判断，避免 cursor + size 溢出。
    if (mypack_v2::kEntryHeaderSize > file_size - cursor) {
      SetError(error_message, "Truncated entry header: " + archive_path);
      return false;
    }
    if (!source.ReadAt(cursor, entry_block, sizeof(entry_block),
                       error_message)) {
      return false;
    }
    const std::string hint = archive_path + " entry " + std::to_string(index);
    ArchiveEntry entry;
    std::uint64_t payload_size = 0;
    std::uint32_t path_length = 0;
    std::uint32_t link_length = 0;
    if (!DecodeEntryHeader(entry_block, hint, &entry, &payload_size,
                           &path_length, &link_length, error_message)) {
      return false;
    }
    cursor += mypack_v2::kEntryHeaderSize;

    const std::uint64_t metadata_size =
        static_cast<std::uint64_t>(path_length) + link_length;
    if (metadata_size > file_size - cursor) {
      SetError(error_message, "Truncated entry path: " + hint);
      return false;
    }
    std::string names(static_cast<std::size_t>(metadata_size), '\0');
    if (metadata_size > 0 &&
        !source.ReadAt(cursor, &names[0], names.size(), error_message)) {
      return false;
    }
    entry.archive_path = names.substr(0, path_length);
    if (link_length > 0) {
      entry.link_target = names.substr(path_length);
    }
    cursor += metadata_size;

    if (!IsValidArchivePath(entry.archive_path, index == 0,
                            entry.type == EntryType::kDirectory,
                            kMaxArchivePathLength, error_message)) {
      return false;
    }
    if (!registry.Add(entry.archive_path, entry.type == EntryType::kDirectory,
                      error_message)) {
      return false;
    }

    if (payload_size > file_size - cursor) {
      SetError(error_message,
               "Entry payload out of bounds: " + entry.archive_path);
      return false;
    }

    PackedEntry record;
    record.entry = entry;
    record.data_offset = cursor;
    record.data_size = payload_size;
    record.entry.size =
        entry.type == EntryType::kRegularFile ? payload_size : 0;
    if (entry.type == EntryType::kHardLink) {
      hardlinks.push_back(entries->size());
    }
    declared.emplace(entry.archive_path, entry.type);
    entries->push_back(std::move(record));
    cursor += payload_size;
  }

  // 读满 entry_count 条之后必须正好到文件末尾：不接受 trailing bytes，
  // 也不"忽略尾巴"。
  if (cursor != file_size) {
    SetError(error_message,
             "Unexpected trailing bytes in archive: " + archive_path);
    return false;
  }
  if (entries->empty() || entries->front().entry.archive_path != "." ||
      entries->front().entry.type != EntryType::kDirectory) {
    SetError(error_message,
             "Archive does not start with the source root: " + archive_path);
    return false;
  }

  // 硬链接目标必须真的存在，而且必须是一条普通文件条目：指向目录、软链接
  // 或另一条硬链接都是坏数据。恢复时要保证目标先被创建。
  for (const std::size_t index : hardlinks) {
    const std::string& target = (*entries)[index].entry.link_target;
    const auto found = declared.find(target);
    if (found == declared.end()) {
      SetError(error_message, "Hard link target is missing: " + target);
      return false;
    }
    if (found->second != EntryType::kRegularFile) {
      SetError(error_message,
               "Hard link target is not a regular file: " + target);
      return false;
    }
  }

  if (entry_count != nullptr) {
    *entry_count = fields.entry_count;
  }
  return true;
}

}  // namespace backupproject
