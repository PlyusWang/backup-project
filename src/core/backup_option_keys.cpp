// backup_option_keys.cpp

// 本文件是"枚举 <-> 持久化键 <-> 显示文案"的单一映射表。三个字段不能混用：
//   key  —— 持久化标识，出现在 schedule.json、CLI 参数与 QML 选择器里，
//           改名等于让旧配置解析不出来；
//   enum —— 进程内的取值；
//   text —— 只给人看，随时可以改文案，不进任何持久化格式。
// 解析失败时**不改写**输出参数，调用方必须先看返回值再使用它。
#include "backup_option_keys.h"

namespace backupproject {
namespace {

// 兜底返回值永远不是 nullptr：显示文案会直接被塞进界面与日志，返回空指针
// 等于把一个"认不出的枚举"升级成崩溃；"unknown" 则是可见、可搜索的信号。
const char* kUnknown = "unknown";

struct PackEntry {
  const char* key;
  PackMethod method;
  const char* text;
};

// 注意：pack 的 key 有**两个**出处 —— 这里的 entry.key 只服务于解析，而
// PackMethodKey 直接转发 pack_stream.cpp 的 PackMethodName。两处字符串必须
// 逐字一致，改一处必须同时改另一处（这是本文件唯一没有单一来源的地方）。
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

// 压缩维度的表：解析只认 key，展示名里的 "+" 等符号纯粹给人看。
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

// "none" 在压缩表与加密表里各有一个，这是两个独立命名空间：解析必须调用
// 对应维度的 Parse* 函数，不能拿一个通用的 "none" 判断去推断另一个维度。
const EncryptionEntry kEncryptionEntries[] = {
    {"none", EncryptionMethod::kNone, "None"},
    {"des-cbc-hmac-sha256", EncryptionMethod::kDesCbcHmacSha256,
     "DES-CBC + HMAC-SHA256"},
    {"aes-256-ctr-hmac-sha256", EncryptionMethod::kAes256CtrHmacSha256,
     "AES-256-CTR + HMAC-SHA256"},
};

}  // namespace

// 键的三个转发函数：pack 的键来自 pack_stream.cpp（配置与归档头共用同一套
// 字符串），压缩与加密的键来自各自模块的 *MethodName。
const char* PackMethodKey(PackMethod method) { return PackMethodName(method); }

const char* CompressionMethodKey(CompressionMethod method) {
  return CompressionMethodName(method);
}

const char* EncryptionMethodKey(EncryptionMethod method) {
  return EncryptionMethodName(method);
}

// 展示文本是 **ASCII** 的，给 CLI 帮助与日志用；界面里的中文文案属于界面层
// （backup_controller.cpp 的 option*Labels），两者刻意不同：改文案不该牵动
// 日志格式，改日志也不该动界面。查不到时返回 "unknown"，绝不返回 nullptr。
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

// 解析是逐字节精确匹配：不 trim、不折叠大小写、不接受别名 —— key 来自配置
// 文件与命令行，任何"宽容"都会让同一个算法有多种拼法。method 为空指针或键
// 不认识时返回 false，且**不改写** *method。
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
