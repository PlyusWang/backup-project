// source_digest.h
//
// 源树的**内容摘要**（content identity）。
//
// 为什么要有单独一层：变化检测要回答"这个文件到底变了吗"。既有的
// metadata-first manifest 用的是 size + mtime，它的已知盲区是
// same-size + same-mtime 的人为 in-place rewrite。
//
// 对完整备份来说，漏检最多造成一份 stale 快照，下一次完整备份可以重新闭合；
// 对增量备份来说，漏掉的这一次变化会成为**所有后代的错误祖先**——链上每一个
// 后续快照都会继承这个错误。所以增量不允许把 metadata-first 当作唯一内容身份。
//
// 这里**没有第二套 SHA-256**：实现仍然只有 src/crypto/sha256.cpp 那一份，
// 本层只把它包成一个"给源树用"的稳定契约：
//
//   * 表示固定为 64 个小写十六进制字符；
//   * 文件按块流式读取，不把整个文件读进内存；
//   * 打开用 O_NOFOLLOW：扫描之后有人把路径换成符号链接时直接失败，绝不跟着
//     读到别处的内容；
//   * 失败原样上报，不吞错、不返回"空摘要"冒充成功。
//
// 这一层没有任何口令、密钥、HMAC 逻辑，也不碰任何加密容器格式。
//
// 本文件是纯 C++17：不依赖 Qt，也不依赖任何第三方库。

#ifndef BACKUP_PROJECT_INCLUDE_SOURCE_DIGEST_H_
#define BACKUP_PROJECT_INCLUDE_SOURCE_DIGEST_H_

#include <cstddef>
#include <string>

namespace backupproject {

// 摘要文本长度：SHA-256 的 32 字节 = 64 个小写十六进制字符。
inline constexpr std::size_t kContentDigestHexSize = 64;

// 读文件的块大小。64 KiB 是"一次 read 就够大、内存又不值得一提"的折中，
// 与 sha256 的 64 字节分组没有关系。
inline constexpr std::size_t kContentDigestChunkBytes = 64u * 1024u;

// 正好 64 个 [0-9a-f]。空串不是合法摘要——"没有摘要"与"摘要是空的"必须能分开。
bool IsContentDigest(const std::string& text);

// 一段内存字节的摘要（symlink 目标、manifest 规范化文本等）。
std::string ContentDigestOfBytes(const std::string& data);

// 一个普通文件正文的摘要。成功时 *hex 是 64 个小写十六进制字符。
//
// 失败语义（返回 false，error_message 里是原因）：
//   * 路径打不开（含 O_NOFOLLOW 拒绝符号链接、权限不足）；
//   * 读失败；
//   * 路径指向的不是普通文件。
// 调用方必须把失败当成"这次扫描不可信"，而不是"内容为空"。
bool ContentDigestOfFile(const std::string& path, std::string* hex,
                         std::string* error_message);

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_SOURCE_DIGEST_H_
