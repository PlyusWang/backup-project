// incremental_delta.cpp
//
// 见 include/incremental_delta.h。这里只做三件事：信封的规范化编解码、
// 把内层 container 包进来（复用现有流水线）、把 envelope 与 payload 的
// 一致性在写侧和读侧都检查一遍。

#include "incremental_delta.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "archive_path.h"
#include "backup_catalog.h"
#include "container_format.h"
#include "crypto.h"
#include "source_digest.h"

namespace backupproject {

namespace {

void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) *error_message = text;
}

std::string ErrnoText(int error_number) {
  const char* text = ::strerror(error_number);
  return text == nullptr ? std::string("errno ") + std::to_string(error_number)
                         : std::string(text);
}

// 转义只作用于字符串字段：反斜杠、TAB、换行、回车。
std::string EscapeField(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (char character : value) {
    switch (character) {
      case '\\':
        out += "\\\\";
        break;
      case '\t':
        out += "\\t";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      default:
        out += character;
    }
  }
  return out;
}

bool UnescapeField(const std::string& text, std::string* value) {
  if (value == nullptr) return false;
  value->clear();
  value->reserve(text.size());
  for (std::size_t index = 0; index < text.size(); ++index) {
    const char character = text[index];
    if (character != '\\') {
      *value += character;
      continue;
    }
    if (index + 1 >= text.size()) return false;
    const char next = text[++index];
    switch (next) {
      case '\\':
        *value += '\\';
        break;
      case 't':
        *value += '\t';
        break;
      case 'n':
        *value += '\n';
        break;
      case 'r':
        *value += '\r';
        break;
      default:
        return false;
    }
  }
  return true;
}

bool ParseUint64(const std::string& text, std::uint64_t* value) {
  if (value == nullptr || text.empty() || text.size() > 20) return false;
  std::uint64_t result = 0;
  for (char character : text) {
    if (character < '0' || character > '9') return false;
    const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
    if (result > (0xFFFFFFFFFFFFFFFFull - digit) / 10ull) return false;
    result = result * 10ull + digit;
  }
  *value = result;
  return true;
}

bool ParseInt64(const std::string& text, std::int64_t* value) {
  if (value == nullptr || text.empty() || text.size() > 20) return false;
  std::string digits = text;
  bool negative = false;
  if (digits[0] == '-') {
    negative = true;
    digits = digits.substr(1);
    if (digits.empty()) return false;
  }
  std::uint64_t magnitude = 0;
  if (!ParseUint64(digits, &magnitude)) return false;
  if (magnitude > 0x7FFFFFFFFFFFFFFFull) return false;
  *value = negative ? -static_cast<std::int64_t>(magnitude)
                    : static_cast<std::int64_t>(magnitude);
  return true;
}

// 每行 key=value；列表键可以出现多次（tombstone / affected_dir）。
void AppendKeyValue(const std::string& key, const std::string& value,
                    std::string* out) {
  *out += key;
  *out += '=';
  *out += EscapeField(value);
  *out += '\n';
}

// 文件大小；失败返回 false。
bool FileSizeOf(const std::string& path, std::uint64_t* size,
                std::string* error_message) {
  struct stat info;
  if (::stat(path.c_str(), &info) != 0) {
    SetError(error_message, "Cannot stat " + path + ": " + ErrnoText(errno));
    return false;
  }
  if (!S_ISREG(info.st_mode)) {
    SetError(error_message, "Not a regular file: " + path);
    return false;
  }
  *size = static_cast<std::uint64_t>(info.st_size);
  return true;
}

// 流式算一个文件的 SHA-256（读侧校验 payload 用）。
bool DigestOfFile(const std::string& path, std::string* hex,
                  std::string* error_message) {
  return ContentDigestOfFile(path, hex, error_message);
}

bool WriteAll(int fd, const void* data, std::size_t size) {
  const char* cursor = static_cast<const char*>(data);
  std::size_t remaining = size;
  while (remaining > 0) {
    const ssize_t written = ::write(fd, cursor, remaining);
    if (written < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    cursor += written;
    remaining -= static_cast<std::size_t>(written);
  }
  return true;
}

bool CopyFileContents(const std::string& from, int to_fd,
                      std::string* error_message) {
  const int from_fd = ::open(from.c_str(), O_RDONLY | O_CLOEXEC);
  if (from_fd < 0) {
    SetError(error_message, "Cannot open " + from + ": " + ErrnoText(errno));
    return false;
  }
  std::vector<char> buffer(64u * 1024u);
  for (;;) {
    const ssize_t got = ::read(from_fd, buffer.data(), buffer.size());
    if (got < 0) {
      if (errno == EINTR) continue;
      const std::string text = ErrnoText(errno);
      ::close(from_fd);
      SetError(error_message, "Cannot read " + from + ": " + text);
      return false;
    }
    if (got == 0) break;
    if (!WriteAll(to_fd, buffer.data(), static_cast<std::size_t>(got))) {
      const std::string text = ErrnoText(errno);
      ::close(from_fd);
      SetError(error_message, "Cannot write delta payload: " + text);
      return false;
    }
  }
  if (::close(from_fd) != 0) {
    SetError(error_message, "Cannot close " + from + ": " + ErrnoText(errno));
    return false;
  }
  return true;
}

std::string UniqueSiblingPath(const std::string& path, const char* suffix) {
  return path + "." + std::to_string(static_cast<unsigned long>(::getpid())) +
         suffix;
}

}  // namespace

std::string SourceIdentityDigest(const std::string& source_path,
                                 const std::string& repository_identity) {
  // 领域分隔：前缀不同，两个 identity 永远不会撞在一起。
  return ContentDigestOfBytes("BPSOURCE1\n" + source_path + "\n" +
                              repository_identity);
}

std::string FilterIdentityDigest(
    const std::vector<std::string>& include_rules,
    const std::vector<std::string>& exclude_rules) {
  std::string text = "BPFILTER1\n";
  text += "include=" + std::to_string(include_rules.size()) + "\n";
  for (const std::string& rule : include_rules) text += "i:" + rule + "\n";
  text += "exclude=" + std::to_string(exclude_rules.size()) + "\n";
  for (const std::string& rule : exclude_rules) text += "e:" + rule + "\n";
  return ContentDigestOfBytes(text);
}

std::string StrategyIdentityDigest(PackMethod pack,
                                   CompressionMethod compression,
                                   EncryptionMethod encryption) {
  std::string text = "BPSTRATEGY1\n";
  text += std::to_string(static_cast<unsigned>(pack)) + "\n";
  text += std::to_string(static_cast<unsigned>(compression)) + "\n";
  text += std::to_string(static_cast<unsigned>(encryption)) + "\n";
  return ContentDigestOfBytes(text);
}

bool IsValidDeltaParentFileName(const std::string& name,
                                std::string* error_message) {
  if (name.empty()) {
    SetError(error_message, "A delta parent file name must not be empty");
    return false;
  }
  if (name == "." || name == "..") {
    SetError(error_message, "Invalid delta parent file name: " + name);
    return false;
  }
  if (name.find('/') != std::string::npos ||
      name.find('\\') != std::string::npos) {
    SetError(
        error_message,
        "A delta parent file name must be a single path component: " + name);
    return false;
  }
  if (name.find('\0') != std::string::npos) {
    SetError(error_message, "A delta parent file name contains a NUL byte");
    return false;
  }
  // 扩展名规则与 Catalog 管理的备份文件名是同一条：这里复用它的判断，而不是
  // 自己再写一遍 ".bak"。
  if (!IsManagedBackupFileName(name)) {
    SetError(error_message,
             "A delta parent file name must be a managed snapshot name "
             "(ending in .bak): " +
                 name);
    return false;
  }
  return true;
}

bool IsValidDeltaTombstone(const std::string& path,
                           std::string* error_message) {
  // 先复用归档路径的唯一语法实现：长度、绝对路径、空组件、"."/".." 组件、
  // 反斜杠、盘符、结尾 '/'、NUL 都在那里被拒。
  if (!IsValidArchivePath(path, /*is_first_entry=*/false,
                          /*is_directory=*/false, kMaxArchivePathLength,
                          error_message)) {
    return false;
  }
  // "." 是源根本身：它不是"某一条被删掉的路径"，而是整棵树的身份。
  // IsValidArchivePath 在 is_first_entry = false
  // 时已经拒绝它，这里再明确说一句，
  // 免得将来有人放宽那条规则时把根也一起放进来。
  if (path == ".") {
    SetError(error_message, "A tombstone may not remove the source root");
    return false;
  }
  return true;
}

std::string SerializeDeltaEnvelope(const DeltaEnvelope& envelope) {
  std::string out;
  out += "BPDELTA1\n";
  AppendKeyValue("format_version", std::to_string(envelope.format_version),
                 &out);
  AppendKeyValue("snapshot_id", envelope.snapshot_id, &out);
  AppendKeyValue("parent_file_name", envelope.parent_file_name, &out);
  AppendKeyValue("parent_snapshot_id", envelope.parent_snapshot_id, &out);
  AppendKeyValue("parent_manifest_digest", envelope.parent_manifest_digest,
                 &out);
  AppendKeyValue("base_generation_id", envelope.base_generation_id, &out);
  AppendKeyValue("source_identity", envelope.source_identity, &out);
  AppendKeyValue("filter_identity", envelope.filter_identity, &out);
  AppendKeyValue("strategy_identity", envelope.strategy_identity, &out);
  AppendKeyValue("current_manifest_digest", envelope.current_manifest_digest,
                 &out);
  AppendKeyValue("created", std::to_string(envelope.created_unix_seconds),
                 &out);
  AppendKeyValue("added", std::to_string(envelope.added), &out);
  AppendKeyValue("modified", std::to_string(envelope.modified), &out);
  AppendKeyValue("metadata_changed", std::to_string(envelope.metadata_changed),
                 &out);
  AppendKeyValue("removed", std::to_string(envelope.removed), &out);
  AppendKeyValue("payload_sha256", envelope.payload_sha256, &out);
  for (const std::string& path : envelope.tombstones) {
    AppendKeyValue("tombstone", path, &out);
  }
  for (const std::string& path : envelope.affected_directories) {
    AppendKeyValue("affected_dir", path, &out);
  }
  return out;
}

std::string ComputeDeltaSnapshotId(const DeltaEnvelope& envelope) {
  DeltaEnvelope copy = envelope;
  copy.snapshot_id.clear();
  return ContentDigestOfBytes(SerializeDeltaEnvelope(copy));
}

bool ParseDeltaEnvelope(const std::string& text, DeltaEnvelope* envelope,
                        std::string* error_message) {
  if (envelope == nullptr) {
    SetError(error_message, "Delta envelope output must not be null");
    return false;
  }
  *envelope = DeltaEnvelope{};

  if (text.size() > kMaxDeltaEnvelopeBytes) {
    SetError(error_message, "Delta envelope is too large: " +
                                std::to_string(text.size()) + " bytes");
    return false;
  }
  const std::string header = "BPDELTA1\n";
  if (text.compare(0, std::min(header.size(), text.size()), header) != 0) {
    SetError(error_message,
             "Invalid delta envelope: missing or wrong version header");
    return false;
  }

  // 每个单值键必须出现且只出现一次：delta 是机器写的，偏差就是状态坏了。
  std::map<std::string, int> seen;
  std::size_t position = header.size();
  while (position < text.size()) {
    const std::size_t newline = text.find('\n', position);
    if (newline == std::string::npos) {
      SetError(error_message, "Invalid delta envelope: truncated line");
      return false;
    }
    const std::string line = text.substr(position, newline - position);
    position = newline + 1;
    if (line.empty()) continue;
    const std::size_t equals = line.find('=');
    if (equals == std::string::npos) {
      SetError(error_message, "Invalid delta envelope: a line has no '='");
      return false;
    }
    const std::string key = line.substr(0, equals);
    std::string value;
    if (!UnescapeField(line.substr(equals + 1), &value)) {
      SetError(error_message,
               "Invalid delta envelope: bad escape in key '" + key + "'");
      return false;
    }

    const bool is_list = key == "tombstone" || key == "affected_dir";
    if (!is_list) {
      const int count = ++seen[key];
      if (count > 1) {
        SetError(error_message,
                 "Invalid delta envelope: duplicate key '" + key + "'");
        return false;
      }
    }

    std::uint64_t number = 0;
    std::int64_t signed_number = 0;
    if (key == "format_version") {
      if (!ParseUint64(value, &number) || number != kDeltaFormatVersion) {
        SetError(error_message,
                 "Invalid delta envelope: unsupported format version '" +
                     value + "'");
        return false;
      }
      envelope->format_version = static_cast<std::uint32_t>(number);
    } else if (key == "snapshot_id") {
      envelope->snapshot_id = value;
    } else if (key == "parent_file_name") {
      envelope->parent_file_name = value;
    } else if (key == "parent_snapshot_id") {
      envelope->parent_snapshot_id = value;
    } else if (key == "parent_manifest_digest") {
      envelope->parent_manifest_digest = value;
    } else if (key == "base_generation_id") {
      envelope->base_generation_id = value;
    } else if (key == "source_identity") {
      envelope->source_identity = value;
    } else if (key == "filter_identity") {
      envelope->filter_identity = value;
    } else if (key == "strategy_identity") {
      envelope->strategy_identity = value;
    } else if (key == "current_manifest_digest") {
      envelope->current_manifest_digest = value;
    } else if (key == "created") {
      if (!ParseInt64(value, &signed_number)) {
        SetError(error_message, "Invalid delta envelope: bad created time");
        return false;
      }
      envelope->created_unix_seconds = signed_number;
    } else if (key == "added") {
      if (!ParseUint64(value, &number)) {
        SetError(error_message, "Invalid delta envelope: bad added count");
        return false;
      }
      envelope->added = number;
    } else if (key == "modified") {
      if (!ParseUint64(value, &number)) {
        SetError(error_message, "Invalid delta envelope: bad modified count");
        return false;
      }
      envelope->modified = number;
    } else if (key == "metadata_changed") {
      if (!ParseUint64(value, &number)) {
        SetError(error_message,
                 "Invalid delta envelope: bad metadata_changed count");
        return false;
      }
      envelope->metadata_changed = number;
    } else if (key == "removed") {
      if (!ParseUint64(value, &number)) {
        SetError(error_message, "Invalid delta envelope: bad removed count");
        return false;
      }
      envelope->removed = number;
    } else if (key == "payload_sha256") {
      envelope->payload_sha256 = value;
    } else if (key == "tombstone") {
      if (envelope->tombstones.size() >= kMaxDeltaTombstones) {
        SetError(error_message, "Invalid delta envelope: too many tombstones");
        return false;
      }
      envelope->tombstones.push_back(value);
    } else if (key == "affected_dir") {
      if (envelope->affected_directories.size() >= kMaxDeltaTombstones) {
        SetError(error_message,
                 "Invalid delta envelope: too many affected directories");
        return false;
      }
      envelope->affected_directories.push_back(value);
    } else {
      SetError(error_message,
               "Invalid delta envelope: unknown key '" + key + "'");
      return false;
    }
  }

  // 单值键一个都不能少。
  static const char* kRequired[] = {"format_version",
                                    "snapshot_id",
                                    "parent_file_name",
                                    "parent_snapshot_id",
                                    "parent_manifest_digest",
                                    "base_generation_id",
                                    "source_identity",
                                    "filter_identity",
                                    "strategy_identity",
                                    "current_manifest_digest",
                                    "created",
                                    "added",
                                    "modified",
                                    "metadata_changed",
                                    "removed",
                                    "payload_sha256"};
  for (const char* key : kRequired) {
    if (seen[key] != 1) {
      SetError(
          error_message,
          std::string("Invalid delta envelope: missing key '") + key + "'");
      return false;
    }
  }
  if (!IsContentDigest(envelope->snapshot_id) ||
      !IsContentDigest(envelope->parent_snapshot_id) ||
      !IsContentDigest(envelope->parent_manifest_digest) ||
      !IsContentDigest(envelope->base_generation_id) ||
      !IsContentDigest(envelope->source_identity) ||
      !IsContentDigest(envelope->filter_identity) ||
      !IsContentDigest(envelope->strategy_identity) ||
      !IsContentDigest(envelope->current_manifest_digest) ||
      !IsContentDigest(envelope->payload_sha256)) {
    SetError(error_message,
             "Invalid delta envelope: an identity or digest field is not a "
             "SHA-256 hex string");
    return false;
  }
  if (envelope->removed != envelope->tombstones.size()) {
    SetError(error_message,
             "Invalid delta envelope: removed count does not match the "
             "tombstone list");
    return false;
  }
  // 自引用必须被拒绝：否则链会出现一个指向自己的节点。
  if (envelope->snapshot_id == envelope->parent_snapshot_id) {
    SetError(error_message,
             "Invalid delta envelope: the snapshot is its own parent");
    return false;
  }
  // 不可信字段的边界：这两个字段会被用在真实文件系统上，格式层就必须把它们
  // 判死。放在这里而不是只放在应用路径，是因为每一个读者（Catalog 列表、
  // 恢复、retention）都要先过这一关。
  if (!IsValidDeltaParentFileName(envelope->parent_file_name, error_message)) {
    return false;
  }
  for (const std::string& tombstone : envelope->tombstones) {
    if (!IsValidDeltaTombstone(tombstone, error_message)) return false;
  }
  return true;
}

bool FullSnapshotId(const std::string& container_file, std::string* id,
                    std::string* error_message) {
  if (id == nullptr) {
    SetError(error_message, "Snapshot id output must not be null");
    return false;
  }
  id->clear();
  ContainerHeader header;
  if (!InspectContainerFile(container_file, &header, error_message)) {
    return false;
  }
  if (header.payload_sha256.size() != container_v2::kSha256Size) {
    SetError(
        error_message,
        "Cannot derive a snapshot id: the container has no payload digest");
    return false;
  }
  const std::string hex = crypto::ToHex(
      reinterpret_cast<const unsigned char*>(header.payload_sha256.data()),
      header.payload_sha256.size());
  *id = ContentDigestOfBytes("BPFULL1\n" + hex);
  return true;
}

bool SnapshotIdOfFile(const std::string& path, std::string* id,
                      std::string* error_message) {
  const SnapshotFileKind kind = ClassifySnapshotFile(path, error_message);
  if (kind == SnapshotFileKind::kDelta) {
    DeltaEnvelope envelope;
    if (!ReadDeltaEnvelope(path, &envelope, error_message)) return false;
    if (id == nullptr) {
      SetError(error_message, "Snapshot id output must not be null");
      return false;
    }
    *id = envelope.snapshot_id;
    return true;
  }
  if (kind == SnapshotFileKind::kContainer) {
    return FullSnapshotId(path, id, error_message);
  }
  SetError(error_message, "Unknown snapshot file: " + path);
  return false;
}

bool LooksLikeDelta(const unsigned char* data, std::size_t size) {
  if (data == nullptr || size < kDeltaMagicSize) return false;
  for (std::size_t index = 0; index < kDeltaMagicSize; ++index) {
    if (data[index] != kDeltaMagic[index]) return false;
  }
  return true;
}

SnapshotFileKind ClassifySnapshotFile(const std::string& path,
                                      std::string* error_message) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    SetError(error_message, "Cannot open " + path + ": " + ErrnoText(errno));
    return SnapshotFileKind::kUnknown;
  }
  unsigned char head[kDeltaMagicSize] = {0};
  const ssize_t got = ::read(fd, head, sizeof(head));
  ::close(fd);
  if (got < 0) {
    SetError(error_message, "Cannot read " + path + ": " + ErrnoText(errno));
    return SnapshotFileKind::kUnknown;
  }
  if (LooksLikeDelta(head, static_cast<std::size_t>(got))) {
    return SnapshotFileKind::kDelta;
  }
  if (LooksLikeContainer(head, static_cast<std::size_t>(got))) {
    return SnapshotFileKind::kContainer;
  }
  return SnapshotFileKind::kUnknown;
}

namespace {

// BKPINC1 的 24 字节固定头：解析 + 全部长度层校验。多处以同一套规则读它，
// 所以只有这一份实现（magic / version / header size / envelope 长度上界 /
// payload 长度上界 / 文件长度必须正好等于三段之和）。
struct DeltaLayout {
  std::uint32_t envelope_len = 0;
  std::uint64_t payload_len = 0;
};

bool ReadDeltaLayout(int fd, std::uint64_t file_size, DeltaLayout* layout,
                     std::string* error_message) {
  unsigned char header[kDeltaFixedHeaderSize] = {0};
  std::size_t filled = 0;
  while (filled < sizeof(header)) {
    const ssize_t got = ::pread(fd, header + filled, sizeof(header) - filled,
                                static_cast<off_t>(filled));
    if (got < 0) {
      if (errno == EINTR) continue;
      SetError(error_message, "Cannot read delta header: " + ErrnoText(errno));
      return false;
    }
    if (got == 0) break;
    filled += static_cast<std::size_t>(got);
  }
  if (filled < sizeof(header)) {
    SetError(error_message, "Invalid delta: the fixed header is truncated");
    return false;
  }
  if (!LooksLikeDelta(header, sizeof(header))) {
    SetError(error_message,
             "Invalid delta: wrong magic (this file is not a BKPINC1 delta)");
    return false;
  }
  const std::uint16_t version =
      static_cast<std::uint16_t>(header[8] | (header[9] << 8));
  const std::uint16_t fixed_size =
      static_cast<std::uint16_t>(header[10] | (header[11] << 8));
  if (version != kDeltaFormatVersion || fixed_size != kDeltaFixedHeaderSize) {
    SetError(error_message,
             "Invalid delta: unsupported format version or header size");
    return false;
  }
  const std::uint32_t envelope_len =
      static_cast<std::uint32_t>(header[12]) |
      (static_cast<std::uint32_t>(header[13]) << 8) |
      (static_cast<std::uint32_t>(header[14]) << 16) |
      (static_cast<std::uint32_t>(header[15]) << 24);
  std::uint64_t payload_len = 0;
  for (int index = 0; index < 8; ++index) {
    payload_len |= static_cast<std::uint64_t>(header[16 + index])
                   << (8 * index);
  }
  if (envelope_len == 0 || envelope_len > kMaxDeltaEnvelopeBytes) {
    SetError(error_message, "Invalid delta: implausible envelope length");
    return false;
  }
  if (payload_len == 0 || !IsAllowedStreamSize(payload_len)) {
    SetError(error_message, "Invalid delta: implausible payload length");
    return false;
  }
  // 文件长度必须正好等于 header + envelope + payload：多一个字节都不接受。
  const std::uint64_t expected =
      static_cast<std::uint64_t>(kDeltaFixedHeaderSize) + envelope_len +
      payload_len;
  if (file_size != expected) {
    SetError(error_message,
             "Invalid delta: file size does not match the declared envelope "
             "and payload lengths");
    return false;
  }
  layout->envelope_len = envelope_len;
  layout->payload_len = payload_len;
  return true;
}

// 从 fd 的给定偏移读满一段字节（pread，不改文件偏移）。
bool ReadExactlyAt(int fd, std::uint64_t offset, void* buffer, std::size_t size,
                   std::string* error_message) {
  unsigned char* cursor = static_cast<unsigned char*>(buffer);
  std::size_t filled = 0;
  while (filled < size) {
    const ssize_t got = ::pread(fd, cursor + filled, size - filled,
                                static_cast<off_t>(offset + filled));
    if (got < 0) {
      if (errno == EINTR) continue;
      SetError(error_message, "Cannot read delta bytes: " + ErrnoText(errno));
      return false;
    }
    if (got == 0) {
      SetError(error_message, "Invalid delta: truncated while reading");
      return false;
    }
    filled += static_cast<std::size_t>(got);
  }
  return true;
}

}  // namespace

bool ReadDeltaEnvelope(const std::string& delta_file, DeltaEnvelope* envelope,
                       std::string* error_message) {
  if (envelope == nullptr) {
    SetError(error_message, "Delta envelope output must not be null");
    return false;
  }
  *envelope = DeltaEnvelope{};

  const int fd = ::open(delta_file.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    SetError(error_message,
             "Cannot open delta " + delta_file + ": " + ErrnoText(errno));
    return false;
  }
  unsigned char header[kDeltaFixedHeaderSize] = {0};
  std::size_t filled = 0;
  while (filled < sizeof(header)) {
    const ssize_t got = ::read(fd, header + filled, sizeof(header) - filled);
    if (got < 0) {
      if (errno == EINTR) continue;
      const std::string text = ErrnoText(errno);
      ::close(fd);
      SetError(error_message, "Cannot read delta header: " + text);
      return false;
    }
    if (got == 0) break;
    filled += static_cast<std::size_t>(got);
  }
  if (filled < sizeof(header)) {
    ::close(fd);
    SetError(error_message, "Invalid delta: the fixed header is truncated");
    return false;
  }
  if (!LooksLikeDelta(header, sizeof(header))) {
    ::close(fd);
    SetError(error_message,
             "Invalid delta: wrong magic (this file is not a BKPINC1 delta)");
    return false;
  }
  const std::uint16_t version =
      static_cast<std::uint16_t>(header[8] | (header[9] << 8));
  const std::uint16_t fixed_size =
      static_cast<std::uint16_t>(header[10] | (header[11] << 8));
  const std::uint32_t envelope_len =
      static_cast<std::uint32_t>(header[12]) |
      (static_cast<std::uint32_t>(header[13]) << 8) |
      (static_cast<std::uint32_t>(header[14]) << 16) |
      (static_cast<std::uint32_t>(header[15]) << 24);
  std::uint64_t payload_len = 0;
  for (int index = 0; index < 8; ++index) {
    payload_len |= static_cast<std::uint64_t>(header[16 + index])
                   << (8 * index);
  }
  if (version != kDeltaFormatVersion || fixed_size != kDeltaFixedHeaderSize) {
    ::close(fd);
    SetError(error_message,
             "Invalid delta: unsupported format version or header size");
    return false;
  }
  if (envelope_len == 0 || envelope_len > kMaxDeltaEnvelopeBytes) {
    ::close(fd);
    SetError(error_message, "Invalid delta: implausible envelope length");
    return false;
  }
  if (payload_len == 0 || !IsAllowedStreamSize(payload_len)) {
    ::close(fd);
    SetError(error_message, "Invalid delta: implausible payload length");
    return false;
  }

  // 文件长度必须正好等于 header + envelope + payload：多一个字节都不接受。
  struct stat info;
  if (::fstat(fd, &info) != 0) {
    const std::string text = ErrnoText(errno);
    ::close(fd);
    SetError(error_message, "Cannot stat delta: " + text);
    return false;
  }
  const std::uint64_t expected =
      static_cast<std::uint64_t>(kDeltaFixedHeaderSize) + envelope_len +
      payload_len;
  if (static_cast<std::uint64_t>(info.st_size) != expected) {
    ::close(fd);
    SetError(error_message,
             "Invalid delta: file size does not match the declared envelope "
             "and payload lengths");
    return false;
  }

  std::string text;
  text.resize(envelope_len);
  std::size_t read_bytes = 0;
  while (read_bytes < text.size()) {
    const ssize_t got = ::read(fd, &text[read_bytes], text.size() - read_bytes);
    if (got < 0) {
      if (errno == EINTR) continue;
      const std::string error_text = ErrnoText(errno);
      ::close(fd);
      SetError(error_message, "Cannot read delta envelope: " + error_text);
      return false;
    }
    if (got == 0) break;
    read_bytes += static_cast<std::size_t>(got);
  }
  if (::close(fd) != 0) {
    SetError(error_message, "Cannot close delta: " + ErrnoText(errno));
    return false;
  }
  if (read_bytes != text.size()) {
    SetError(error_message, "Invalid delta: the envelope is truncated");
    return false;
  }
  if (!ParseDeltaEnvelope(text, envelope, error_message)) return false;
  // 自校验：snapshot_id 必须真的是这份 envelope 的摘要。
  if (ComputeDeltaSnapshotId(*envelope) != envelope->snapshot_id) {
    SetError(error_message,
             "Invalid delta: the snapshot id does not match the envelope "
             "contents");
    return false;
  }
  return true;
}

bool ExtractDeltaPayload(const std::string& delta_file,
                         const std::string& container_file,
                         std::string* error_message) {
  const int in_fd = ::open(delta_file.c_str(), O_RDONLY | O_CLOEXEC);
  if (in_fd < 0) {
    SetError(error_message,
             "Cannot open delta " + delta_file + ": " + ErrnoText(errno));
    return false;
  }
  unsigned char header[kDeltaFixedHeaderSize] = {0};
  std::size_t filled = 0;
  while (filled < sizeof(header)) {
    const ssize_t got = ::read(in_fd, header + filled, sizeof(header) - filled);
    if (got < 0) {
      if (errno == EINTR) continue;
      const std::string text = ErrnoText(errno);
      ::close(in_fd);
      SetError(error_message, "Cannot read delta header: " + text);
      return false;
    }
    if (got == 0) break;
    filled += static_cast<std::size_t>(got);
  }
  if (filled != sizeof(header) || !LooksLikeDelta(header, sizeof(header))) {
    ::close(in_fd);
    SetError(error_message, "Invalid delta: bad header while extracting");
    return false;
  }
  const std::uint32_t envelope_len =
      static_cast<std::uint32_t>(header[12]) |
      (static_cast<std::uint32_t>(header[13]) << 8) |
      (static_cast<std::uint32_t>(header[14]) << 16) |
      (static_cast<std::uint32_t>(header[15]) << 24);
  std::uint64_t payload_len = 0;
  for (int index = 0; index < 8; ++index) {
    payload_len |= static_cast<std::uint64_t>(header[16 + index])
                   << (8 * index);
  }
  if (::lseek(in_fd, static_cast<off_t>(kDeltaFixedHeaderSize + envelope_len),
              SEEK_SET) < 0) {
    const std::string text = ErrnoText(errno);
    ::close(in_fd);
    SetError(error_message, "Cannot seek to the delta payload: " + text);
    return false;
  }

  const int out_fd = ::open(container_file.c_str(),
                            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (out_fd < 0) {
    const std::string text = ErrnoText(errno);
    ::close(in_fd);
    SetError(error_message, "Cannot create " + container_file + ": " + text);
    return false;
  }
  std::vector<char> buffer(64u * 1024u);
  std::uint64_t remaining = payload_len;
  while (remaining > 0) {
    const std::size_t want = static_cast<std::size_t>(
        std::min<std::uint64_t>(remaining, buffer.size()));
    const ssize_t got = ::read(in_fd, buffer.data(), want);
    if (got < 0) {
      if (errno == EINTR) continue;
      const std::string text = ErrnoText(errno);
      ::close(in_fd);
      ::close(out_fd);
      ::unlink(container_file.c_str());
      SetError(error_message, "Cannot read delta payload: " + text);
      return false;
    }
    if (got == 0) {
      ::close(in_fd);
      ::close(out_fd);
      ::unlink(container_file.c_str());
      SetError(error_message, "Invalid delta: the payload is truncated");
      return false;
    }
    if (!WriteAll(out_fd, buffer.data(), static_cast<std::size_t>(got))) {
      const std::string text = ErrnoText(errno);
      ::close(in_fd);
      ::close(out_fd);
      ::unlink(container_file.c_str());
      SetError(error_message, "Cannot write the extracted payload: " + text);
      return false;
    }
    remaining -= static_cast<std::uint64_t>(got);
  }
  if (::close(in_fd) != 0 || ::close(out_fd) != 0) {
    ::unlink(container_file.c_str());
    SetError(error_message, "Cannot close during payload extraction");
    return false;
  }
  return true;
}

bool InspectDeltaPayloadHeader(const std::string& delta_file,
                               ContainerHeader* header,
                               std::string* error_message) {
  if (header == nullptr) {
    SetError(error_message, "Container header output must not be null");
    return false;
  }
  *header = ContainerHeader{};
  // 布局规则只有一份实现：先让 ReadDeltaEnvelope 把外层整个读一遍
  // （magic / version / 长度自洽 / 信封自校验），再用 ReadDeltaLayout 取偏移。
  DeltaEnvelope envelope;
  if (!ReadDeltaEnvelope(delta_file, &envelope, error_message)) return false;

  const int fd = ::open(delta_file.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    SetError(error_message,
             "Cannot open delta " + delta_file + ": " + ErrnoText(errno));
    return false;
  }
  struct stat info;
  if (::fstat(fd, &info) != 0) {
    const std::string text = ErrnoText(errno);
    ::close(fd);
    SetError(error_message, "Cannot stat delta: " + text);
    return false;
  }
  DeltaLayout layout;
  if (!ReadDeltaLayout(fd, static_cast<std::uint64_t>(info.st_size), &layout,
                       error_message)) {
    ::close(fd);
    return false;
  }
  unsigned char block[container_v2::kHeaderSize] = {0};
  if (!ReadExactlyAt(fd,
                     static_cast<std::uint64_t>(kDeltaFixedHeaderSize) +
                         layout.envelope_len,
                     block, sizeof(block), error_message)) {
    ::close(fd);
    return false;
  }
  if (::close(fd) != 0) {
    SetError(error_message, "Cannot close delta: " + ErrnoText(errno));
    return false;
  }
  return DecodeContainerHeader(block, sizeof(block), header, error_message);
}

bool VerifyDeltaPayload(const std::string& delta_file,
                        std::string* error_message) {
  // 1) 信封：布局、字段边界、自身摘要（ReadDeltaEnvelope 内部全做）。
  DeltaEnvelope envelope;
  if (!ReadDeltaEnvelope(delta_file, &envelope, error_message)) return false;

  const int fd = ::open(delta_file.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    SetError(error_message,
             "Cannot open delta " + delta_file + ": " + ErrnoText(errno));
    return false;
  }
  struct stat info;
  if (::fstat(fd, &info) != 0) {
    const std::string text = ErrnoText(errno);
    ::close(fd);
    SetError(error_message, "Cannot stat delta: " + text);
    return false;
  }
  DeltaLayout layout;
  if (!ReadDeltaLayout(fd, static_cast<std::uint64_t>(info.st_size), &layout,
                       error_message)) {
    ::close(fd);
    return false;
  }

  // 2) **实际 payload 字节**的 SHA-256 必须等于信封声明的 payload_sha256。
  //    就地流式读，不落任何临时文件：这一步是"身份可信"的前提，不是可选装饰。
  const std::uint64_t payload_offset =
      static_cast<std::uint64_t>(kDeltaFixedHeaderSize) + layout.envelope_len;
  crypto::Sha256 sha;
  std::vector<unsigned char> buffer(64u * 1024u);
  std::uint64_t offset = 0;
  while (offset < layout.payload_len) {
    const std::uint64_t remaining = layout.payload_len - offset;
    const std::size_t want = static_cast<std::size_t>(
        remaining < buffer.size() ? remaining : buffer.size());
    const ssize_t got = ::pread(fd, buffer.data(), want,
                                static_cast<off_t>(payload_offset + offset));
    if (got < 0) {
      if (errno == EINTR) continue;
      const std::string text = ErrnoText(errno);
      ::close(fd);
      SetError(error_message, "Cannot read delta payload: " + text);
      return false;
    }
    if (got == 0) {
      ::close(fd);
      SetError(error_message, "Invalid delta: the payload is truncated");
      return false;
    }
    sha.Update(buffer.data(), static_cast<std::size_t>(got));
    offset += static_cast<std::uint64_t>(got);
  }
  unsigned char digest[crypto::kSha256DigestSize];
  sha.Final(digest);
  const std::string actual = crypto::ToHex(digest, crypto::kSha256DigestSize);
  if (actual != envelope.payload_sha256) {
    ::close(fd);
    SetError(error_message,
             "Delta payload checksum mismatch: the actual payload bytes do not "
             "match the digest declared in the envelope (" +
                 delta_file + ")");
    return false;
  }

  // 3) 内层 container 自己也要自洽：header 可解码，而且它的长度正好等于
  //    这份 payload 的长度（payload 就是那个 container，一个字节都不多不少）。
  unsigned char header_block[container_v2::kHeaderSize] = {0};
  if (!ReadExactlyAt(fd, payload_offset, header_block, sizeof(header_block),
                     error_message)) {
    ::close(fd);
    return false;
  }
  ContainerHeader header;
  if (!DecodeContainerHeader(header_block, sizeof(header_block), &header,
                             error_message)) {
    ::close(fd);
    return false;
  }
  if (static_cast<std::uint64_t>(container_v2::kHeaderSize) +
          header.payload_size !=
      layout.payload_len) {
    ::close(fd);
    SetError(error_message,
             "Invalid delta: the inner container length does not match the "
             "delta payload length");
    return false;
  }
  if (::close(fd) != 0) {
    SetError(error_message, "Cannot close delta: " + ErrnoText(errno));
    return false;
  }
  return true;
}

bool VerifyFullSnapshotPayload(const std::string& container_file,
                               std::string* payload_sha256_hex,
                               std::string* error_message) {
  ContainerHeader header;
  if (!VerifyContainerPayloadBytes(container_file, &header, error_message)) {
    return false;
  }
  if (payload_sha256_hex != nullptr) {
    *payload_sha256_hex = crypto::ToHex(
        reinterpret_cast<const unsigned char*>(header.payload_sha256.data()),
        header.payload_sha256.size());
  }
  return true;
}

bool WriteDeltaFile(const std::string& delta_file,
                    const DeltaEnvelope& envelope,
                    const std::vector<ArchiveEntry>& changed_entries,
                    const BackupOptions& options, std::string* error_message) {
  if (changed_entries.size() > kMaxDeltaEntries) {
    SetError(error_message, "Too many entries for one delta: " +
                                std::to_string(changed_entries.size()));
    return false;
  }
  // 第一版只支持 MyPack：USTAR 表达不了 tombstone 与 parent dependency，
  // 与其"伪装支持"不如明确拒绝。共享校验层会报同一句话。
  if (options.pack_method != PackMethod::kMyPack) {
    SetError(error_message,
             "Incremental deltas require the MyPack pack method; USTAR cannot "
             "express tombstone or parent dependencies");
    return false;
  }
  if (envelope.removed != envelope.tombstones.size()) {
    SetError(error_message,
             "Delta envelope: removed count must equal the tombstone count");
    return false;
  }
  // 写侧过的是与读侧同一对校验：自己不产出自己随后拒绝的东西。
  if (!IsValidDeltaParentFileName(envelope.parent_file_name, error_message)) {
    return false;
  }
  for (const std::string& tombstone : envelope.tombstones) {
    if (!IsValidDeltaTombstone(tombstone, error_message)) return false;
  }
  // payload 复用备份流水线，条目表的约定也就与备份完全一致：第一条是源根。
  // 不替调用方伪造一条根记录——根目录的 metadata 必须来自真实扫描。
  if (changed_entries.empty() || changed_entries.front().archive_path != "." ||
      changed_entries.front().type != EntryType::kDirectory) {
    SetError(error_message,
             "Incremental delta entries must start with the source root");
    return false;
  }

  const std::string temp_container =
      UniqueSiblingPath(delta_file, ".container");
  const std::string temp_delta = UniqueSiblingPath(delta_file, ".part");
  ::unlink(temp_container.c_str());
  ::unlink(temp_delta.c_str());

  bool ok = false;
  do {
    if (!RunBackupPipelineFromEntries(changed_entries, temp_container, options,
                                      error_message)) {
      break;
    }
    std::uint64_t payload_len = 0;
    if (!FileSizeOf(temp_container, &payload_len, error_message)) break;

    DeltaEnvelope filled = envelope;
    if (!DigestOfFile(temp_container, &filled.payload_sha256, error_message)) {
      break;
    }
    filled.snapshot_id = ComputeDeltaSnapshotId(filled);
    const std::string text = SerializeDeltaEnvelope(filled);
    if (text.size() > kMaxDeltaEnvelopeBytes) {
      SetError(error_message, "Delta envelope is too large");
      break;
    }

    unsigned char header[kDeltaFixedHeaderSize] = {0};
    for (std::size_t index = 0; index < kDeltaMagicSize; ++index) {
      header[index] = kDeltaMagic[index];
    }
    header[8] = static_cast<unsigned char>(kDeltaFormatVersion & 0xFF);
    header[9] = static_cast<unsigned char>((kDeltaFormatVersion >> 8) & 0xFF);
    header[10] = static_cast<unsigned char>(kDeltaFixedHeaderSize & 0xFF);
    header[11] =
        static_cast<unsigned char>((kDeltaFixedHeaderSize >> 8) & 0xFF);
    const std::uint32_t envelope_len = static_cast<std::uint32_t>(text.size());
    for (int index = 0; index < 4; ++index) {
      header[12 + index] =
          static_cast<unsigned char>((envelope_len >> (8 * index)) & 0xFF);
    }
    for (int index = 0; index < 8; ++index) {
      header[16 + index] =
          static_cast<unsigned char>((payload_len >> (8 * index)) & 0xFF);
    }

    // 唯一临时文件 + fsync + rename：任何一步失败都不留半成品。
    const int fd = ::open(temp_delta.c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
      SetError(error_message,
               "Cannot create " + temp_delta + ": " + ErrnoText(errno));
      break;
    }
    if (!WriteAll(fd, header, sizeof(header)) ||
        !WriteAll(fd, text.data(), text.size()) ||
        !CopyFileContents(temp_container, fd, error_message)) {
      ::close(fd);
      ::unlink(temp_delta.c_str());
      if (error_message != nullptr && error_message->empty()) {
        SetError(error_message, "Cannot write the delta file");
      }
      break;
    }
    if (::fsync(fd) != 0) {
      const std::string text_error = ErrnoText(errno);
      ::close(fd);
      ::unlink(temp_delta.c_str());
      SetError(error_message, "Cannot fsync the delta file: " + text_error);
      break;
    }
    if (::close(fd) != 0) {
      ::unlink(temp_delta.c_str());
      SetError(error_message,
               "Cannot close the delta file: " + ErrnoText(errno));
      break;
    }
    if (::rename(temp_delta.c_str(), delta_file.c_str()) != 0) {
      const std::string text_error = ErrnoText(errno);
      ::unlink(temp_delta.c_str());
      SetError(error_message, "Cannot publish the delta file: " + text_error);
      break;
    }
    ok = true;
  } while (false);

  ::unlink(temp_container.c_str());
  if (!ok) ::unlink(temp_delta.c_str());
  return ok;
}

}  // namespace backupproject
