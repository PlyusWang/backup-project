// container_format.cpp
//
// 见 container_format.h。

// 模块职责：v2 外层容器头（160 字节）的**唯一**编解码实现。读侧
// （InspectContainerFile / restore）与写侧（archive_pipeline）都调用这里的
// Encode / Decode，因此"写出来的头"与"读得回来的头"无法各自漂移。
//
// 边界：本文件只处理头的字节，不碰 payload。压缩、加密、HMAC、PBKDF2 都由
// 调用方完成，"算法 id + 各阶段长度 + salt/IV/tag"这几样东西在这里汇合。
//
// 信任边界：DecodeContainerHeader 的输入是未信任的磁盘数据，所以它按白名单
// 校验并在任何不满足处 fail-closed，绝不"尽力解析"；EncodeContainerHeader
// 只接受内部构造好的结构，但仍然复核长度，防止写侧产出读侧会拒收的归档。
//
// 失败语义：所有函数返回 bool，失败时把英文原因写进 error_message（可传
// nullptr）；*out / *header 只在成功路径被赋值，失败时保持调用前的值。
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
// 定长字段的填充必须是 0 而不是任意字节：读侧会校验未使用的尾部全为 0，
// 否则同一份逻辑内容会有多种合法编码，MAC 归一化的结果也不再唯一。
void StorePadded(const std::string& value, std::size_t field_size,
                 std::string* out) {
  const std::size_t take =
      value.size() < field_size ? value.size() : field_size;
  out->append(value.data(), take);
  out->append(field_size - take, '\0');
}

// 保留区与定长字段的尾部必须逐字节为 0。这类检查看起来教条，但它把格式收紧
// 成一种规范表示：非零填充会被明确拒绝，而不是被悄悄忽略——后者会让"同一个
// 头有两种合法编码"，进而让 MAC 校验的语义变得含糊。
bool IsZeroField(const unsigned char* block, std::size_t offset,
                 std::size_t size) {
  for (std::size_t index = 0; index < size; ++index) {
    if (block[offset + index] != 0) {
      return false;
    }
  }
  return true;
}

}  // namespace

// 写侧与读侧共用同一个上界（理由见 container_format.h）：4 TiB 只是"显然
// 不合理"的拦截线，不是可信长度的来源，分配内存前仍要按实际文件大小复核。
bool IsAllowedStreamSize(std::uint64_t size) {
  return size <= container_v2::kMaxStreamSize;
}

// PKCS#7 的数学性质：结果一定是 8 的倍数且严格大于 plain_size，
// 因此容器里 payload_size 与 compressed_size 的关系可以被独立验算。
// 接近 UINT64_MAX 的输入饱和到 UINT64_MAX，让调用方的范围检查有机会拒绝它。
std::uint64_t DesPaddedSize(std::uint64_t plain_size) {
  if (plain_size > UINT64_MAX - 8) return UINT64_MAX;
  return (plain_size / 8 + 1) * 8;
}

// id -> 名字只用于日志与 JSON 输出，不参与磁盘格式：改名不会让旧归档读不出来。
// 未知 id 返回 "unknown" 而不是空指针，调用方无需判空。
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

// 这里的数字就是磁盘上的 method id，改动等于换格式：旧归档的 id 必须继续
// 映射到同一个枚举，新增算法只能追加新 id，不能重排。
// 未知 id 返回 false 并保持 *method 不变，调用方应把它当成"归档来自更新的
// 版本"直接拒绝，而不是回落到 kNone 继续解——那会解出垃圾数据。
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

// 写侧唯一的编码入口。字段顺序即磁盘布局，不能重排：读侧按固定 offset 取
// 字段，改动会让历史归档整片错位；新增字段只能占用保留区并提升 version。
// 校验顺序是刻意的：先"结构内部自洽"（salt/iv/tag 长度与 *_len 相等）、
// 再"格式上界"，这样错误信息指向的是调用方传错参数，而不是格式问题。
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
  // 写侧也必须守同一条边界：读侧会拒收任何一个 size 超过 kMaxStreamSize 的
  // 容器，所以写侧就不能把它产出来。少了这一步，writer 能写出一个自己
  // reader 随后拒绝的 .bak —— 用户会拿到一份"备份成功但恢复不了"的归档。
  //
  // 这一步发生在打开任何输出文件**之前**（调用方先去 workspace 里写），
  // 所以失败时不会有半成品 final archive，也不会有任何 state/catalog 变更。
  if (!IsAllowedStreamSize(header.packed_size) ||
      !IsAllowedStreamSize(header.compressed_size) ||
      !IsAllowedStreamSize(header.payload_size)) {
    SetError(error_message,
             "Refusing to write a container whose stream exceeds the format "
             "limit of " +
                 std::to_string(container_v2::kMaxStreamSize) +
                 " bytes (packed=" + std::to_string(header.packed_size) +
                 ", compressed=" + std::to_string(header.compressed_size) +
                 ", payload=" + std::to_string(header.payload_size) + ")");
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
  // 逐字段推进 cursor，每个字段都要求"缓冲区里确实还有这么多字节"：
  // 越界读取在这里直接变成 false，而不是读进相邻内存。size 允许大于
  // kHeaderSize（调用方可能传整块缓冲区），读取范围仍被限制在头部之内。
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

  // 解析进局部对象，最后一步才提交给 *header：任何一条校验失败都不会让
  // 半解析的结果泄漏给调用方，调用方也就不需要"失败了要不要清空"的知识。
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

  // 格式层的白名单：flags 与两个保留字段必须正好是本版本约定的取值。放宽它们
  // 等于允许未来的写入器静默改变语义，而今天的读侧看不懂那些语义——宁可
  // 报"不认识"，也不要按旧语义去猜。
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

  // 每种算法只承认一种参数组合：DES-CBC 要 8 字节 IV，AES-256-CTR 要 16 字节，
  // salt 固定 16 字节，tag 固定 32 字节，KDF 轮数固定 kProductionIterations。
  // 轮数写进格式是为了将来能升级，但本版本只认这一个值。
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

  // 三个 size 的关系由算法决定，读侧独立验算一遍：
  //   packed     -- 打包层输出的字节数
  //   compressed -- 压缩层输出的字节数
  //   payload    -- 加密层输出的字节数，也是容器里实际存的那一段长度
  // 不压缩则前两者必须相等；流密码（none / AES-CTR）不改变长度；DES-CBC 则
  // 必须正好等于 PKCS#7 补齐后的长度。
  // ---- 三个 size 字段的关系 ----
  if (!IsAllowedStreamSize(decoded.packed_size) ||
      !IsAllowedStreamSize(decoded.compressed_size) ||
      !IsAllowedStreamSize(decoded.payload_size)) {
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
  // 空归档不是合法的 v2 容器：写侧至少会写一条 root entry。声明 0 条通常说明
  // 头被改过，或者文件来自另一条代码路径，这里直接拒绝而不是交给上层去猜。
  if (decoded.entry_count == 0) {
    SetError(error_message, "Container declares zero entries");
    return false;
  }

  // 只有走到这里 decoded 才被提交：上面任何一条校验失败都不会让部分解析结果
  // 泄漏出去。
  *header = decoded;
  return true;
}

// 只看 magic，不看 version / header_size / 长度：调用方用它做"这是不是本项目
// 的容器"的分流判断，真正的合法性由 DecodeContainerHeader 决定。
// data 为 nullptr 或短于 magic 时返回 false，不做越界读取。
bool LooksLikeContainer(const unsigned char* data, std::size_t size) {
  return data != nullptr && size >= container_v2::kMagicSize &&
         std::memcmp(data, container_v2::kMagic, container_v2::kMagicSize) == 0;
}

// "没有密码也能读"的入口：只读前 160 字节并解码，不会因为缺少密码而失败，
// 因此可用于备份列表。失败时把路径附加到原因末尾，让批量扫描的调用方知道
// 是哪一个归档出的问题。
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

// 返回空串表示归一化失败（EncodeContainerHeader 拒绝了这个头）：调用方必须
// 把它当成错误，绝不能拿空串去算 MAC——那会让校验形同虚设。
// 归一化只清 auth_tag，其余字段原样参与，因此 sizes / salt / iv / 算法 id
// 中任何一处被改动都会让 MAC 对不上。
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

// KDF 入口。切分规则与算法绑定，必须与解密端的期望一致：
//   AES-256-CTR：derived[0,32) 是 CTR 密钥，derived[32,64) 是 HMAC 密钥；
//   DES-CBC：derived[0,8) 是 DES 密钥，derived[32,64) 是 HMAC 密钥，
//            中间 24 字节直接丢弃（DES 用不上 32 字节的密钥）。
// 两把密钥来自同一次 PBKDF2 输出但互不重叠，避免"一把密钥两用"。
// 未加密的归档即使传了密码也返回失败：这里不提供"忽略密码继续"的宽松路径。
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
