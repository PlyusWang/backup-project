// src/network/network_protocol.cpp
//
// BPNET1 的编解码实现。见 include/network_protocol.h 里的协议说明。
//
// 这个文件里没有任何"按主机字节序直接读结构体"的捷径：所有多字节整数都是
// 手工移位拼出来的，因此把小端机器上的字节流喂给大端机器解析也不会读错。

#include "network_protocol.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace backupproject {
namespace net {
namespace {

// ---- 大端原语 ----
//
// byte_order.h 里那套是归档格式用的 little-endian；线上协议固定大端，
// 两者不能混用，所以这里单独实现，绝不共享。

void AppendU16BE(std::string* out, std::uint16_t value) {
  out->push_back(static_cast<char>((value >> 8) & 0xFFu));
  out->push_back(static_cast<char>(value & 0xFFu));
}

void AppendU32BE(std::string* out, std::uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    out->push_back(static_cast<char>((value >> shift) & 0xFFu));
  }
}

void AppendU64BE(std::string* out, std::uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out->push_back(static_cast<char>((value >> shift) & 0xFFu));
  }
}

std::uint16_t LoadU16BE(const unsigned char* data) {
  return static_cast<std::uint16_t>((static_cast<std::uint32_t>(data[0]) << 8) |
                                    static_cast<std::uint32_t>(data[1]));
}

std::uint32_t LoadU32BE(const unsigned char* data) {
  std::uint32_t value = 0;
  for (int index = 0; index < 4; ++index) {
    value = (value << 8) | static_cast<std::uint32_t>(data[index]);
  }
  return value;
}

std::uint64_t LoadU64BE(const unsigned char* data) {
  std::uint64_t value = 0;
  for (int index = 0; index < 8; ++index) {
    value = (value << 8) | static_cast<std::uint64_t>(data[index]);
  }
  return value;
}

bool IsLowerHex(const std::string& value, std::size_t expected) {
  if (value.size() != expected) {
    return false;
  }
  for (const char character : value) {
    const bool digit = character >= '0' && character <= '9';
    const bool lower = character >= 'a' && character <= 'f';
    if (!digit && !lower) {
      return false;
    }
  }
  return true;
}

}  // namespace

// ---- 帧头 ----

std::string EncodeFrameHeader(const FrameHeader& header) {
  std::string out;
  out.reserve(kFrameHeaderSize);
  AppendU32BE(&out, kProtocolMagic);
  AppendU16BE(&out, header.version);
  AppendU16BE(&out, header.opcode);
  AppendU16BE(&out, header.flags);
  AppendU16BE(&out, header.reserved);
  AppendU32BE(&out, header.status);
  AppendU64BE(&out, header.request_id);
  AppendU64BE(&out, header.payload_length);
  return out;
}

bool DecodeFrameHeader(const unsigned char* data, std::size_t size,
                       FrameHeader* header, std::string* error_message) {
  if (data == nullptr || header == nullptr) {
    if (error_message != nullptr) {
      *error_message = "frame header buffer is null";
    }
    return false;
  }
  if (size < kFrameHeaderSize) {
    if (error_message != nullptr) {
      *error_message = "frame header is shorter than 32 bytes";
    }
    return false;
  }
  if (LoadU32BE(data) != kProtocolMagic) {
    if (error_message != nullptr) {
      *error_message = "frame magic is not BPN1";
    }
    return false;
  }
  FrameHeader parsed;
  parsed.version = LoadU16BE(data + 4);
  parsed.opcode = LoadU16BE(data + 6);
  parsed.flags = LoadU16BE(data + 8);
  parsed.reserved = LoadU16BE(data + 10);
  parsed.status = LoadU32BE(data + 12);
  parsed.request_id = LoadU64BE(data + 16);
  parsed.payload_length = LoadU64BE(data + 24);

  if (parsed.version != kProtocolVersion) {
    if (error_message != nullptr) {
      *error_message = "unsupported protocol version";
    }
    return false;
  }
  if (parsed.flags != 0) {
    if (error_message != nullptr) {
      *error_message = "frame flags must be zero";
    }
    return false;
  }
  if (parsed.reserved != 0) {
    if (error_message != nullptr) {
      *error_message = "frame reserved field must be zero";
    }
    return false;
  }
  // 长度检查在**任何分配之前**：这里就是"客户端说 payload 有 2^64-1 字节"
  // 被挡住的地方。
  if (parsed.payload_length > kMaxPayloadBytes) {
    if (error_message != nullptr) {
      *error_message = "frame payload length exceeds the 1 MiB limit";
    }
    return false;
  }
  *header = parsed;
  return true;
}

void DecodeFrameHeaderFields(const unsigned char* data, std::size_t size,
                             FrameHeader* header) {
  if (data == nullptr || header == nullptr || size < kFrameHeaderSize) {
    return;
  }
  header->version = LoadU16BE(data + 4);
  header->opcode = LoadU16BE(data + 6);
  header->flags = LoadU16BE(data + 8);
  header->reserved = LoadU16BE(data + 10);
  header->status = LoadU32BE(data + 12);
  header->request_id = LoadU64BE(data + 16);
  header->payload_length = LoadU64BE(data + 24);
}

// ---- 名字表 ----

bool IsKnownSnapshotKind(std::uint16_t kind) {
  return kind == static_cast<std::uint16_t>(SnapshotKind::kFull) ||
         kind == static_cast<std::uint16_t>(SnapshotKind::kIncremental);
}

bool IsKnownOpcode(std::uint16_t opcode) {
  switch (static_cast<Opcode>(opcode)) {
    case Opcode::kPing:
    case Opcode::kRegister:
    case Opcode::kLogin:
    case Opcode::kLogout:
    case Opcode::kResume:
    case Opcode::kList:
    case Opcode::kUploadBegin:
    case Opcode::kUploadChunk:
    case Opcode::kUploadEnd:
    case Opcode::kDownloadBegin:
    case Opcode::kDownloadChunk:
    case Opcode::kDownloadEnd:
    case Opcode::kDelete:
    case Opcode::kDeleteAccount:
    case Opcode::kError:
      return true;
  }
  return false;
}

const char* OpcodeName(std::uint16_t opcode) {
  switch (static_cast<Opcode>(opcode)) {
    case Opcode::kPing:
      return "PING";
    case Opcode::kRegister:
      return "REGISTER";
    case Opcode::kLogin:
      return "LOGIN";
    case Opcode::kLogout:
      return "LOGOUT";
    case Opcode::kResume:
      return "RESUME";
    case Opcode::kList:
      return "LIST";
    case Opcode::kUploadBegin:
      return "UPLOAD_BEGIN";
    case Opcode::kUploadChunk:
      return "UPLOAD_CHUNK";
    case Opcode::kUploadEnd:
      return "UPLOAD_END";
    case Opcode::kDownloadBegin:
      return "DOWNLOAD_BEGIN";
    case Opcode::kDownloadChunk:
      return "DOWNLOAD_CHUNK";
    case Opcode::kDownloadEnd:
      return "DOWNLOAD_END";
    case Opcode::kDelete:
      return "DELETE";
    case Opcode::kDeleteAccount:
      return "DELETE_ACCOUNT";
    case Opcode::kError:
      return "ERROR";
  }
  return "UNKNOWN";
}

const char* StatusName(std::uint32_t status) {
  switch (static_cast<Status>(status)) {
    case Status::kOk:
      return "OK";
    case Status::kInvalidRequest:
      return "INVALID_REQUEST";
    case Status::kUnauthorized:
      return "UNAUTHORIZED";
    case Status::kForbidden:
      return "FORBIDDEN";
    case Status::kNotFound:
      return "NOT_FOUND";
    case Status::kAlreadyExists:
      return "ALREADY_EXISTS";
    case Status::kInvalidState:
      return "INVALID_STATE";
    case Status::kTooLarge:
      return "TOO_LARGE";
    case Status::kIntegrityMismatch:
      return "INTEGRITY_MISMATCH";
    case Status::kInternalError:
      return "INTERNAL_ERROR";
    case Status::kUnsupportedVersion:
      return "UNSUPPORTED_VERSION";
    case Status::kMalformedFrame:
      return "MALFORMED_FRAME";
    case Status::kUnsupported:
      return "UNSUPPORTED";
  }
  return "UNKNOWN_STATUS";
}

bool OpcodeAllowsEmptyPayload(std::uint16_t opcode) {
  switch (static_cast<Opcode>(opcode)) {
    case Opcode::kPing:
    case Opcode::kLogout:
    case Opcode::kList:
    case Opcode::kUploadEnd:
    case Opcode::kDownloadChunk:
    case Opcode::kDownloadEnd:
      return true;
    default:
      return false;
  }
}

// ---- payload 构造 ----

void PayloadBuilder::AppendU8(std::uint8_t value) {
  data_.push_back(static_cast<char>(value & 0xFFu));
}

void PayloadBuilder::AppendU16(std::uint16_t value) {
  AppendU16BE(&data_, value);
}

void PayloadBuilder::AppendU32(std::uint32_t value) {
  AppendU32BE(&data_, value);
}

void PayloadBuilder::AppendU64(std::uint64_t value) {
  AppendU64BE(&data_, value);
}

bool PayloadBuilder::AppendString(const std::string& value,
                                  std::size_t max_bytes,
                                  std::string* error_message) {
  if (value.size() > max_bytes || value.size() > 0xFFFFu) {
    if (error_message != nullptr) {
      *error_message = "string field exceeds its maximum length";
    }
    return false;
  }
  AppendU16BE(&data_, static_cast<std::uint16_t>(value.size()));
  data_.append(value);
  return true;
}

void PayloadBuilder::AppendBytes(const void* data, std::size_t size) {
  if (data == nullptr || size == 0) {
    return;
  }
  data_.append(static_cast<const char*>(data), size);
}

// ---- payload 解析 ----

PayloadReader::PayloadReader(const std::string& payload) : payload_(payload) {}

bool PayloadReader::Fail(const std::string& reason) {
  if (error_message_.empty()) {
    error_message_ = reason;
  }
  return false;
}

bool PayloadReader::Require(std::size_t count) {
  if (count > payload_.size() - cursor_) {
    return Fail("payload ended before the declared field was complete");
  }
  return true;
}

bool PayloadReader::ReadU8(std::uint8_t* out) {
  if (out == nullptr) {
    return Fail("output pointer is null");
  }
  if (!Require(1)) {
    return false;
  }
  *out = static_cast<std::uint8_t>(payload_[cursor_]);
  cursor_ += 1;
  return true;
}

bool PayloadReader::ReadU16(std::uint16_t* out) {
  if (out == nullptr) {
    return Fail("output pointer is null");
  }
  if (!Require(2)) {
    return false;
  }
  *out = LoadU16BE(reinterpret_cast<const unsigned char*>(payload_.data()) +
                   cursor_);
  cursor_ += 2;
  return true;
}

bool PayloadReader::ReadU32(std::uint32_t* out) {
  if (out == nullptr) {
    return Fail("output pointer is null");
  }
  if (!Require(4)) {
    return false;
  }
  *out = LoadU32BE(reinterpret_cast<const unsigned char*>(payload_.data()) +
                   cursor_);
  cursor_ += 4;
  return true;
}

bool PayloadReader::ReadU64(std::uint64_t* out) {
  if (out == nullptr) {
    return Fail("output pointer is null");
  }
  if (!Require(8)) {
    return false;
  }
  *out = LoadU64BE(reinterpret_cast<const unsigned char*>(payload_.data()) +
                   cursor_);
  cursor_ += 8;
  return true;
}

bool PayloadReader::ReadString(std::size_t max_bytes, std::string* out) {
  if (out == nullptr) {
    return Fail("output pointer is null");
  }
  std::uint16_t length = 0;
  if (!ReadU16(&length)) {
    return false;
  }
  // 先比上限再分配：length 是客户端给的，不能直接 resize。
  if (length > max_bytes) {
    return Fail("string field exceeds its maximum length");
  }
  if (!Require(length)) {
    return false;
  }
  out->assign(payload_, cursor_, length);
  cursor_ += length;
  return true;
}

bool PayloadReader::ReadBytes(std::size_t count, std::string* out) {
  if (out == nullptr) {
    return Fail("output pointer is null");
  }
  if (!Require(count)) {
    return false;
  }
  out->assign(payload_, cursor_, count);
  cursor_ += count;
  return true;
}

// ---- 定长收发 ----

bool SendAll(int fd, const void* data, std::size_t size,
             std::string* error_message) {
  const char* cursor = static_cast<const char*>(data);
  std::size_t remaining = size;
  while (remaining > 0) {
    const ssize_t written = ::send(fd, cursor, remaining, MSG_NOSIGNAL);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (error_message != nullptr) {
        *error_message = std::string("send failed: ") + std::strerror(errno);
      }
      return false;
    }
    if (written == 0) {
      if (error_message != nullptr) {
        *error_message = "send returned zero before the buffer was flushed";
      }
      return false;
    }
    cursor += written;
    remaining -= static_cast<std::size_t>(written);
  }
  return true;
}

bool ReceiveAll(int fd, void* data, std::size_t size, bool* closed_by_peer,
                std::string* error_message) {
  if (closed_by_peer != nullptr) {
    *closed_by_peer = false;
  }
  char* cursor = static_cast<char*>(data);
  std::size_t remaining = size;
  while (remaining > 0) {
    const ssize_t got = ::recv(fd, cursor, remaining, 0);
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (error_message != nullptr) {
        *error_message = std::string("recv failed: ") + std::strerror(errno);
      }
      return false;
    }
    if (got == 0) {
      // 一个字节都没读到就 EOF 是"对端正常关闭"；读到一半断开是错误。
      if (remaining == size) {
        if (closed_by_peer != nullptr) {
          *closed_by_peer = true;
        }
        if (error_message != nullptr) {
          *error_message = "peer closed the connection";
        }
        return false;
      }
      if (error_message != nullptr) {
        *error_message = "peer closed the connection in the middle of a frame";
      }
      return false;
    }
    cursor += got;
    remaining -= static_cast<std::size_t>(got);
  }
  return true;
}

bool SendFrame(int fd, std::uint16_t opcode, std::uint32_t status,
               std::uint64_t request_id, const std::string& payload,
               std::string* error_message) {
  if (payload.size() > kMaxPayloadBytes) {
    if (error_message != nullptr) {
      *error_message = "refusing to send a payload larger than 1 MiB";
    }
    return false;
  }
  FrameHeader header;
  header.version = kProtocolVersion;
  header.opcode = opcode;
  header.flags = 0;
  header.reserved = 0;
  header.status = status;
  header.request_id = request_id;
  header.payload_length = payload.size();
  const std::string encoded = EncodeFrameHeader(header);
  if (!SendAll(fd, encoded.data(), encoded.size(), error_message)) {
    return false;
  }
  if (!payload.empty() &&
      !SendAll(fd, payload.data(), payload.size(), error_message)) {
    return false;
  }
  return true;
}

FrameReadStatus ReceiveFrame(int fd, FrameHeader* header, std::string* payload,
                             std::string* error_message) {
  if (header == nullptr || payload == nullptr) {
    if (error_message != nullptr) {
      *error_message = "frame output pointers are null";
    }
    return FrameReadStatus::kIoError;
  }
  unsigned char raw[kFrameHeaderSize];
  bool closed = false;
  std::string io_error;
  if (!ReceiveAll(fd, raw, kFrameHeaderSize, &closed, &io_error)) {
    if (error_message != nullptr) {
      *error_message = io_error;
    }
    return closed ? FrameReadStatus::kClosed : FrameReadStatus::kIoError;
  }

  // 先把字段尽力解出来（不校验），这样错误响应也能回填 request_id。
  DecodeFrameHeaderFields(raw, kFrameHeaderSize, header);
  FrameHeader validated;
  std::string header_error;
  if (!DecodeFrameHeader(raw, kFrameHeaderSize, &validated, &header_error)) {
    if (error_message != nullptr) {
      *error_message = header_error;
    }
    // 分两类：magic 不对或长度超限时，流里已经找不到下一个帧的边界，
    // 只能断开；版本/操作码/flags 的问题可以回一个错误帧继续。
    const bool magic_bad = LoadU32BE(raw) != kProtocolMagic;
    const bool length_bad = LoadU64BE(raw + 24) > kMaxPayloadBytes;
    return (magic_bad || length_bad) ? FrameReadStatus::kCorruptStream
                                     : FrameReadStatus::kInvalidFrame;
  }
  *header = validated;

  payload->clear();
  if (header->payload_length > 0) {
    // 长度已经在上面的 DecodeFrameHeader 里过了 1 MiB 上限，
    // 这里才允许按它分配。
    payload->resize(static_cast<std::size_t>(header->payload_length));
    bool payload_closed = false;
    std::string payload_error;
    if (!ReceiveAll(fd, &(*payload)[0], payload->size(), &payload_closed,
                    &payload_error)) {
      if (error_message != nullptr) {
        *error_message = payload_error;
      }
      return FrameReadStatus::kIoError;
    }
  }
  return FrameReadStatus::kOk;
}

// ---- 字段校验 ----

UsernameValidation ValidateUsername(const std::string& username,
                                    std::string* error_message) {
  if (username.empty()) {
    if (error_message != nullptr) {
      *error_message = "username must be 3 to 64 bytes long";
    }
    return UsernameValidation::kEmpty;
  }
  if (username.size() < kMinUsernameBytes) {
    if (error_message != nullptr) {
      *error_message = "username must be 3 to 64 bytes long";
    }
    return UsernameValidation::kTooShort;
  }
  if (username.size() > kMaxUsernameBytes) {
    if (error_message != nullptr) {
      *error_message = "username must be 3 to 64 bytes long";
    }
    return UsernameValidation::kTooLong;
  }
  if (username == "." || username == "..") {
    if (error_message != nullptr) {
      *error_message = "username must not be a dot path";
    }
    // 长度下限是 3，所以这两个值其实到不了这里；分类上归入"字符不合法"。
    return UsernameValidation::kInvalidCharacter;
  }
  for (const char character : username) {
    const bool digit = character >= '0' && character <= '9';
    const bool upper = character >= 'A' && character <= 'Z';
    const bool lower = character >= 'a' && character <= 'z';
    const bool symbol =
        character == '_' || character == '.' || character == '-';
    if (!digit && !upper && !lower && !symbol) {
      if (error_message != nullptr) {
        *error_message =
            "username may only contain letters, digits, dot, dash and "
            "underscore";
      }
      return UsernameValidation::kInvalidCharacter;
    }
  }
  return UsernameValidation::kOk;
}

bool IsValidUsername(const std::string& username, std::string* error_message) {
  return ValidateUsername(username, error_message) == UsernameValidation::kOk;
}

bool IsValidPassword(const std::string& password, std::string* error_message) {
  if (password.empty() || password.size() > kMaxPasswordBytes) {
    if (error_message != nullptr) {
      *error_message = "password must be 1 to 256 bytes long";
    }
    return false;
  }
  if (password.find('\0') != std::string::npos) {
    if (error_message != nullptr) {
      *error_message = "password must not contain a NUL byte";
    }
    return false;
  }
  return true;
}

bool IsValidDisplayName(const std::string& name, std::string* error_message) {
  if (name.empty() || name.size() > kMaxDisplayNameBytes) {
    if (error_message != nullptr) {
      *error_message = "display name must be 1 to 255 bytes long";
    }
    return false;
  }
  for (const char character : name) {
    const unsigned char byte = static_cast<unsigned char>(character);
    if (byte < 0x20 || byte == 0x7F) {
      if (error_message != nullptr) {
        *error_message = "display name must not contain control characters";
      }
      return false;
    }
  }
  return true;
}

bool IsValidSnapshotId(const std::string& snapshot_id,
                       std::string* error_message) {
  if (!IsLowerHex(snapshot_id, 32)) {
    if (error_message != nullptr) {
      *error_message = "snapshot id must be 32 lowercase hex characters";
    }
    return false;
  }
  return true;
}

bool IsValidSha256Hex(const std::string& hex, std::string* error_message) {
  if (!IsLowerHex(hex, kSha256HexBytes)) {
    if (error_message != nullptr) {
      *error_message = "sha256 must be 64 lowercase hex characters";
    }
    return false;
  }
  return true;
}

}  // namespace net
}  // namespace backupproject
