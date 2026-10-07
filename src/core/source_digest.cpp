// source_digest.cpp
//
// 见 include/source_digest.h。实现刻意薄：真正的 SHA-256 在
// src/crypto/sha256.cpp，这里只负责"怎么读、怎么表示、怎么失败"。

// 契约：对外的摘要形态固定为 64 个小写十六进制字符。本层不做缓存、不读
// 元数据、也不做"看起来一样就认为一样"的猜测 —— 身份只来自正文的字节。
// 摘要计算用的是 sha256.cpp 那一份实现，这里不引入第二套密码学代码。
#include "source_digest.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstring>
#include <vector>

#include "crypto.h"

namespace backupproject {

namespace {

void SetError(std::string* error_message, const std::string& text) {
  if (error_message != nullptr) *error_message = text;
}

// errno 文本只用于诊断，绝不参与控制流：判定永远基于返回值，报错文本取不到
// （strerror 返回 nullptr）也不改变失败语义。
std::string ErrnoText(int error_number) {
  char buffer[256];
  // strerror_r 的两种实现（GNU / XSI）返回值不同，这里统一走副本。
  const char* text = ::strerror(error_number);
  if (text == nullptr)
    return std::string("errno ") + std::to_string(error_number);
  std::snprintf(buffer, sizeof(buffer), "%s", text);
  return std::string(buffer);
}

// 定长写出 kContentDigestHexSize 个字符。hex 为空指针时返回 false —— 本文件
// 内部的调用点都不会传空，这个分支只为防御。
bool ReadAllHex(const unsigned char* digest, std::string* hex) {
  static const char kHexDigits[] = "0123456789abcdef";
  std::string out;
  out.resize(kContentDigestHexSize);
  for (std::size_t index = 0; index < crypto::kSha256DigestSize; ++index) {
    out[index * 2] = kHexDigits[digest[index] >> 4];
    out[index * 2 + 1] = kHexDigits[digest[index] & 0x0Fu];
  }
  if (hex == nullptr) return false;
  *hex = std::move(out);
  return true;
}

}  // namespace

// 用途：区分"这是一份内容摘要"与"这是一条路径"。**只接受小写**是有意的：
// 同一种摘要的两种大小写如果都被接受，按字符串做键的身份比较就会出现两个
// 键指向同一份内容。长度与字符集都必须精确匹配，不做 trim。
bool IsContentDigest(const std::string& text) {
  if (text.size() != kContentDigestHexSize) return false;
  for (char character : text) {
    const bool is_digit = character >= '0' && character <= '9';
    const bool is_lower = character >= 'a' && character <= 'f';
    if (!is_digit && !is_lower) return false;
  }
  return true;
}

std::string ContentDigestOfBytes(const std::string& data) {
  unsigned char digest[crypto::kSha256DigestSize];
  crypto::Sha256::Hash(data.data(), data.size(), digest);
  std::string hex;
  ReadAllHex(digest, &hex);
  return hex;
}

// 前置条件：path 指向**待哈希的普通文件**。返回 false 表示这次扫描不可信，
// 调用方不能把它当成"内容为空"或"跳过这个文件"。
// 副作用：先清空 *hex 再填；失败时它是空串，而不是上一次的残留值。
bool ContentDigestOfFile(const std::string& path, std::string* hex,
                         std::string* error_message) {
  if (hex == nullptr) {
    SetError(error_message, "Content digest output must not be null");
    return false;
  }
  hex->clear();

  // O_NOFOLLOW：扫描与读正文之间把路径换成符号链接时直接失败。
  // 这与项目其它地方（file_io / file_lock）的立场一致：不跟随。
  const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    SetError(error_message,
             "Cannot open for hashing: " + path + ": " + ErrnoText(errno));
    return false;
  }

  struct stat info;
  if (::fstat(fd, &info) != 0) {
    const int error_number = errno;
    ::close(fd);
    SetError(error_message, "Cannot stat for hashing: " + path + ": " +
                                ErrnoText(error_number));
    return false;
  }
  if (!S_ISREG(info.st_mode)) {
    ::close(fd);
    SetError(error_message, "Not a regular file: " + path);
    return false;
  }

  // 读完整个文件才 Final：流式吸收的中间状态与一次性哈希等价（见
  // Sha256::Update 的切分不变量），缓冲大小只影响吞吐，不影响结果。
  crypto::Sha256 hasher;
  std::vector<char> buffer(kContentDigestChunkBytes);
  for (;;) {
    const ssize_t got = ::read(fd, buffer.data(), buffer.size());
    if (got < 0) {
      if (errno == EINTR) continue;
      const int error_number = errno;
      ::close(fd);
      SetError(error_message, "Cannot read for hashing: " + path + ": " +
                                  ErrnoText(error_number));
      return false;
    }
    if (got == 0) break;
    hasher.Update(buffer.data(), static_cast<std::size_t>(got));
  }
  // close 的失败在这里不重要（只读，没有任何待落盘的元数据），但也不能
  // 完全忽略：留着返回值会触发 -Wunused-result。
  if (::close(fd) != 0) {
    SetError(error_message,
             "Cannot close after hashing: " + path + ": " + ErrnoText(errno));
    return false;
  }

  unsigned char digest[crypto::kSha256DigestSize];
  hasher.Final(digest);
  return ReadAllHex(digest, hex);
}

}  // namespace backupproject
