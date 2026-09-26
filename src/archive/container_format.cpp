// container_format.cpp
//
// 见 container_format.h。

#include "container_format.h"

#include <cstring>
#include <string>

#include "byte_order.h"
#include "crypto.h"
#include "file_io.h"

namespace backupproject {

namespace {

void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) {
    *error_message = text;
  }
}

// 把最多 field_size 个字节写进定长字段，其余补 0。
void StorePadded(const std::string& value, std::size_t field_size,
                 std::string* out) {
  const std::size_t take =
      value.size() < field_size ? value.size() : field_size;
  out->append(value.data(), take);
  out->append(field_size - take, '\0');
}

bool IsZeroField(const unsigned char* block, std::size_t offset,
                 std::size_t size) {
  for (std::size_t index = 0; index < size; ++index) {
    if (block[offset + index] != 0) {
      return false;
    }
  }
  return true;
}

// PKCS#7 一定补 1..8 个字节，所以 DES-CBC 之后的长度是 8 的倍数且严格更大。
std::uint64_t DesPaddedSize(std::uint64_t plain_size) {
  return (plain_size / 8 + 1) * 8;
}

}  // namespace

const char* CompressionMethodName(CompressionMethod method) {
  switch (method) {
    case CompressionMethod::kNone:
      return "none";
    case CompressionMethod::kHuffman:
      return "huffman";
    case CompressionMethod::kLzssHuffman:
      return "lzss-huffman";
  }
  return "unknown";
}

const char* EncryptionMethodName(EncryptionMethod method) {
  switch (method) {
    case EncryptionMethod::kNone:
      return "none";
    case EncryptionMethod::kDesCbcHmacSha256:
      return "des-cbc-hmac-sha256";
    case EncryptionMethod::kAes256CtrHmacSha256:
      return "aes-256-ctr-hmac-sha256";
  }
  return "unknown";
}

bool ParseCompressionMethodId(std::uint8_t id, CompressionMethod* method) {
  switch (id) {
    case 0:
      *method = CompressionMethod::kNone;
      return true;
    case 1:
      *method = CompressionMethod::kHuffman;
      return true;
    case 2:
      *method = CompressionMethod::kLzssHuffman;
      return true;
    default:
      return false;
  }
}

bool ParseEncryptionMethodId(std::uint8_t id, EncryptionMethod* method) {
  switch (id) {
    case 0:
      *method = EncryptionMethod::kNone;
      return true;
    case 1:
      *method = EncryptionMethod::kDesCbcHmacSha256;
      return true;
    case 2:
      *method = EncryptionMethod::kAes256CtrHmacSha256;
      return true;
    default:
      return false;
  }
}

bool EncodeContainerHeader(const ContainerHeader& header, std::string* out,
                           std::string* error_message) {
  if (out == nullptr) {
    SetError(error_message, "Internal error: null header buffer");
    return false;
  }
  if (header.salt.size() != header.salt_len ||
      header.iv.size() != header.iv_len ||
      header.auth_tag.size() != header.tag_len) {
    SetError(error_message,
             "Internal error: container header salt/iv/tag length mismatch");
    return false;
  }
  if (header.salt.size() > container_v2::kSaltFieldSize ||
      header.iv.size() > container_v2::kIvFieldSize ||
      header.auth_tag.size() > container_v2::kAuthTagSize ||
      header.payload_sha256.size() > container_v2::kSha256Size) {
    SetError(error_message, "Internal error: container header field too long");
    return false;
  }
  out->clear();
  out->append(reinterpret_cast<const char*>(container_v2::kMagic),
              container_v2::kMagicSize);
  AppendU16LE(out, container_v2::kVersion);
  AppendU16LE(out, container_v2::kHeaderSizeField);
  out->push_back(static_cast<char>(header.pack_method));
  out->push_back(static_cast<char>(header.compression_method));
  out->push_back(static_cast<char>(header.encryption_method));
  out->push_back(static_cast<char>(header.flags));
  AppendU64LE(out, header.entry_count);
  AppendU64LE(out, header.packed_size);
  AppendU64LE(out, header.compressed_size);
  AppendU64LE(out, header.payload_size);
  AppendU32LE(out, header.kdf_iterations);
  out->push_back(static_cast<char>(header.salt_len));
  out->push_back(static_cast<char>(header.iv_len));
  out->push_back(static_cast<char>(header.tag_len));
  out->push_back(static_cast<char>(header.reserved0));
  StorePadded(header.salt, container_v2::kSaltFieldSize, out);
  StorePadded(header.iv, container_v2::kIvFieldSize, out);
  StorePadded(header.auth_tag, container_v2::kAuthTagSize, out);
  StorePadded(header.payload_sha256, container_v2::kSha256Size, out);
  out->append(container_v2::kReservedSize, '\0');
  if (out->size() != container_v2::kHeaderSize) {
    out->clear();
    SetError(error_message, "Internal error: bad container header size");
    return false;
  }
  return true;
}

bool DecodeContainerHeader(const unsigned char* block, std::size_t size,
                           ContainerHeader* header,
                           std::string* error_message) {
  if (block == nullptr || header == nullptr) {
    SetError(error_message, "Internal error: null container header");
    return false;
  }
  if (size < container_v2::kHeaderSize) {
    SetError(error_message, "Truncated container header");
    return false;
  }
  if (std::memcmp(block, container_v2::kMagic, container_v2::kMagicSize) != 0) {
    SetError(error_message, "Invalid container magic");
    return false;
  }
  std::size_t cursor = container_v2::kMagicSize;
  std::uint16_t version = 0;
  std::uint16_t header_size = 0;
  if (!ReadU16LE(block, size, &cursor, &version) ||
      !ReadU16LE(block, size, &cursor, &header_size)) {
    SetError(error_message, "Truncated container header");
    return false;
  }
  if (version != container_v2::kVersion) {
    SetError(error_message, "Unsupported container version: " +
                                std::to_string(static_cast<int>(version)));
    return false;
  }
  if (header_size != container_v2::kHeaderSizeField) {
    SetError(error_message,
             "Invalid container header size: " + std::to_string(header_size));
    return false;
  }

  ContainerHeader decoded;
  std::uint8_t pack_method = 0;
  std::uint8_t compression_method = 0;
  std::uint8_t encryption_method = 0;
  std::uint8_t flags = 0;
  if (!ReadU8(block, size, &cursor, &pack_method) ||
      !ReadU8(block, size, &cursor, &compression_method) ||
      !ReadU8(block, size, &cursor, &encryption_method) ||
      !ReadU8(block, size, &cursor, &flags) ||
      !ReadU64LE(block, size, &cursor, &decoded.entry_count) ||
      !ReadU64LE(block, size, &cursor, &decoded.packed_size) ||
      !ReadU64LE(block, size, &cursor, &decoded.compressed_size) ||
      !ReadU64LE(block, size, &cursor, &decoded.payload_size) ||
      !ReadU32LE(block, size, &cursor, &decoded.kdf_iterations) ||
      !ReadU8(block, size, &cursor, &decoded.salt_len) ||
      !ReadU8(block, size, &cursor, &decoded.iv_len) ||
      !ReadU8(block, size, &cursor, &decoded.tag_len) ||
      !ReadU8(block, size, &cursor, &decoded.reserved0)) {
    SetError(error_message, "Truncated container header");
    return false;
  }
  decoded.pack_method = pack_method;
  decoded.compression_method = compression_method;
  decoded.encryption_method = encryption_method;
  decoded.flags = flags;

  if (flags != 0) {
    SetError(error_message, "Unsupported container flags: " +
                                std::to_string(static_cast<int>(flags)));
    return false;
  }
  if (decoded.reserved0 != 0 ||
      !IsZeroField(block, container_v2::kReservedOffset,
                   container_v2::kReservedSize)) {
    SetError(error_message, "Non-zero reserved bytes in container header");
    return false;
  }
  PackMethod pack = PackMethod::kMyPack;
  if (!ParsePackMethodId(pack_method, &pack)) {
    SetError(error_message, "Unknown pack method id: " +
                                std::to_string(static_cast<int>(pack_method)));
    return false;
  }
  CompressionMethod compression = CompressionMethod::kNone;
  if (!ParseCompressionMethodId(compression_method, &compression)) {
    SetError(error_message,
             "Unknown compression method id: " +
                 std::to_string(static_cast<int>(compression_method)));
    return false;
  }
  EncryptionMethod encryption = EncryptionMethod::kNone;
  if (!ParseEncryptionMethodId(encryption_method, &encryption)) {
    SetError(error_message,
             "Unknown encryption method id: " +
                 std::to_string(static_cast<int>(encryption_method)));
    return false;
  }

  decoded.salt.assign(
      reinterpret_cast<const char*>(block + container_v2::kSaltOffset),
      container_v2::kSaltFieldSize);
  decoded.iv.assign(
      reinterpret_cast<const char*>(block + container_v2::kIvOffset),
      container_v2::kIvFieldSize);
  decoded.auth_tag.assign(
      reinterpret_cast<const char*>(block + container_v2::kAuthTagOffset),
      container_v2::kAuthTagSize);
  decoded.payload_sha256.assign(
      reinterpret_cast<const char*>(block + container_v2::kPayloadSha256Offset),
      container_v2::kSha256Size);

  // ---- 语义层校验：算法与长度字段的组合必须自洽 ----
  if (decoded.salt_len > container_v2::kSaltFieldSize ||
      decoded.iv_len > container_v2::kIvFieldSize ||
      decoded.tag_len > container_v2::kAuthTagSize) {
    SetError(error_message, "Container salt/iv/tag length out of range");
    return false;
  }
  if (!IsZeroField(block, container_v2::kSaltOffset + decoded.salt_len,
                   container_v2::kSaltFieldSize - decoded.salt_len) ||
      !IsZeroField(block, container_v2::kIvOffset + decoded.iv_len,
                   container_v2::kIvFieldSize - decoded.iv_len) ||
      !IsZeroField(block, container_v2::kAuthTagOffset + decoded.tag_len,
                   container_v2::kAuthTagSize - decoded.tag_len)) {
    SetError(error_message, "Non-zero padding in container salt/iv/tag field");
    return false;
  }
  // 编码时把长度字段当作真实长度：这里把内部字符串裁到声明长度，
  // 未使用的尾部字节在上面已经确认是 0。
  decoded.salt.resize(decoded.salt_len);
  decoded.iv.resize(decoded.iv_len);
  decoded.auth_tag.resize(decoded.tag_len);

  switch (encryption) {
    case EncryptionMethod::kNone:
      if (decoded.kdf_iterations != 0 || decoded.salt_len != 0 ||
          decoded.iv_len != 0 || decoded.tag_len != 0) {
        SetError(error_message,
                 "Unencrypted container must not carry KDF parameters");
        return false;
      }
      break;
    case EncryptionMethod::kDesCbcHmacSha256:
      if (decoded.kdf_iterations != container_v2::kProductionIterations ||
          decoded.salt_len != 16 || decoded.iv_len != 8 ||
          decoded.tag_len != container_v2::kAuthTagSize) {
        SetError(error_message, "Invalid DES container key parameters");
        return false;
      }
      break;
    case EncryptionMethod::kAes256CtrHmacSha256:
      if (decoded.kdf_iterations != container_v2::kProductionIterations ||
          decoded.salt_len != 16 || decoded.iv_len != 16 ||
          decoded.tag_len != container_v2::kAuthTagSize) {
        SetError(error_message, "Invalid AES container key parameters");
        return false;
      }
      break;
  }

  // ---- 三个 size 字段的关系 ----
  if (decoded.packed_size > container_v2::kMaxStreamSize ||
      decoded.compressed_size > container_v2::kMaxStreamSize ||
      decoded.payload_size > container_v2::kMaxStreamSize) {
    SetError(error_message, "Container stream size is implausibly large");
    return false;
  }
  if (compression == CompressionMethod::kNone &&
      decoded.compressed_size != decoded.packed_size) {
    SetError(error_message,
             "Container sizes disagree: no compression but sizes differ");
    return false;
  }
  if (encryption == EncryptionMethod::kNone ||
      encryption == EncryptionMethod::kAes256CtrHmacSha256) {
    // CTR 是流密码：长度不变。
    if (decoded.payload_size != decoded.compressed_size) {
      SetError(
          error_message,
          "Container sizes disagree: stream cipher must not change length");
      return false;
    }
  } else if (decoded.payload_size != DesPaddedSize(decoded.compressed_size)) {
    SetError(error_message,
             "Container sizes disagree: DES-CBC payload size is not the "
             "PKCS#7 padded length");
    return false;
  }
  if (decoded.entry_count == 0) {
    SetError(error_message, "Container declares zero entries");
    return false;
  }

  *header = decoded;
  return true;
}

bool LooksLikeContainer(const unsigned char* data, std::size_t size) {
  return data != nullptr && size >= container_v2::kMagicSize &&
         std::memcmp(data, container_v2::kMagic, container_v2::kMagicSize) == 0;
}

bool InspectContainerFile(const std::string& path, ContainerHeader* header,
                          std::string* error_message) {
  FileSource source;
  if (!source.Open(path, error_message)) {
    return false;
  }
  unsigned char block[container_v2::kHeaderSize];
  if (source.size() < container_v2::kHeaderSize) {
    SetError(error_message, "Truncated container header: " + path);
    return false;
  }
  if (!source.ReadAt(0, block, sizeof(block), error_message)) {
    return false;
  }
  if (!DecodeContainerHeader(block, sizeof(block), header, error_message)) {
    if (error_message != nullptr) {
      *error_message = *error_message + ": " + path;
    }
    return false;
  }
  return true;
}

std::string NormalizedHeaderForMac(const ContainerHeader& header) {
  ContainerHeader normalized = header;
  // auth_tag 字段本身不参与 MAC：先算 MAC 再写回去，是 Encrypt-then-MAC 的
  // 标准做法。其它字段（含 payload_sha256、sizes、salt、iv）全部参与，
  // 所以任何一处被改动都会导致校验失败。
  normalized.auth_tag.assign(normalized.tag_len, '\0');
  std::string encoded;
  if (!EncodeContainerHeader(normalized, &encoded, nullptr)) {
    return std::string();
  }
  return encoded;
}

bool DeriveKeys(const std::string& password, const ContainerHeader& header,
                std::string* cipher_key, std::string* mac_key,
                std::string* error_message) {
  EncryptionMethod method = EncryptionMethod::kNone;
  if (!ParseEncryptionMethodId(header.encryption_method, &method)) {
    SetError(error_message, "Unknown encryption method id");
    return false;
  }
  if (password.empty()) {
    SetError(error_message, "A password is required for this archive");
    return false;
  }
  if (method == EncryptionMethod::kNone) {
    SetError(error_message, "This archive is not encrypted");
    return false;
  }
  std::string derived;
  if (!crypto::Pbkdf2HmacSha256(password, header.salt, header.kdf_iterations,
                                container_v2::kDerivedKeySize, &derived,
                                error_message)) {
    return false;
  }
  if (derived.size() != container_v2::kDerivedKeySize) {
    SetError(error_message, "Internal error: PBKDF2 returned wrong key length");
    return false;
  }
  if (method == EncryptionMethod::kAes256CtrHmacSha256) {
    *cipher_key = derived.substr(0, 32);
    *mac_key = derived.substr(32, 32);
  } else {
    *cipher_key = derived.substr(0, 8);
    *mac_key = derived.substr(32, 32);
  }
  return true;
}

}  // namespace backupproject
