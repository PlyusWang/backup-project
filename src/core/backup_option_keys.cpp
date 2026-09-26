// backup_option_keys.cpp

#include "backup_option_keys.h"

namespace backupproject {
namespace {

const char* kUnknown = "unknown";

struct PackEntry {
  const char* key;
  PackMethod method;
  const char* text;
};

const PackEntry kPackEntries[] = {
    {"mypack", PackMethod::kMyPack, "MyPack"},
    {"ustar", PackMethod::kUstar, "USTAR"},
    {"fast-ustar", PackMethod::kFastUstar, "Fast USTAR"},
};

struct CompressionEntry {
  const char* key;
  CompressionMethod method;
  const char* text;
};

const CompressionEntry kCompressionEntries[] = {
    {"none", CompressionMethod::kNone, "None"},
    {"huffman", CompressionMethod::kHuffman, "Huffman"},
    {"lzss-huffman", CompressionMethod::kLzssHuffman, "LZSS + Huffman"},
};

struct EncryptionEntry {
  const char* key;
  EncryptionMethod method;
  const char* text;
};

const EncryptionEntry kEncryptionEntries[] = {
    {"none", EncryptionMethod::kNone, "None"},
    {"des-cbc-hmac-sha256", EncryptionMethod::kDesCbcHmacSha256,
     "DES-CBC + HMAC-SHA256"},
    {"aes-256-ctr-hmac-sha256", EncryptionMethod::kAes256CtrHmacSha256,
     "AES-256-CTR + HMAC-SHA256"},
};

}  // namespace

const char* PackMethodKey(PackMethod method) { return PackMethodName(method); }

const char* CompressionMethodKey(CompressionMethod method) {
  return CompressionMethodName(method);
}

const char* EncryptionMethodKey(EncryptionMethod method) {
  return EncryptionMethodName(method);
}

const char* PackMethodDisplayName(PackMethod method) {
  for (const PackEntry& entry : kPackEntries) {
    if (entry.method == method) return entry.text;
  }
  return kUnknown;
}

const char* CompressionMethodDisplayName(CompressionMethod method) {
  for (const CompressionEntry& entry : kCompressionEntries) {
    if (entry.method == method) return entry.text;
  }
  return kUnknown;
}

const char* EncryptionMethodDisplayName(EncryptionMethod method) {
  for (const EncryptionEntry& entry : kEncryptionEntries) {
    if (entry.method == method) return entry.text;
  }
  return kUnknown;
}

bool ParsePackMethodKey(const std::string& key, PackMethod* method) {
  if (method == nullptr) return false;
  for (const PackEntry& entry : kPackEntries) {
    if (key == entry.key) {
      *method = entry.method;
      return true;
    }
  }
  return false;
}

bool ParseCompressionMethodKey(const std::string& key,
                               CompressionMethod* method) {
  if (method == nullptr) return false;
  for (const CompressionEntry& entry : kCompressionEntries) {
    if (key == entry.key) {
      *method = entry.method;
      return true;
    }
  }
  return false;
}

bool ParseEncryptionMethodKey(const std::string& key,
                              EncryptionMethod* method) {
  if (method == nullptr) return false;
  for (const EncryptionEntry& entry : kEncryptionEntries) {
    if (key == entry.key) {
      *method = entry.method;
      return true;
    }
  }
  return false;
}

}  // namespace backupproject
