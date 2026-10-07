// byte_order.h
//
// 固定宽度整数的 little-endian 编解码原语。
//
// 归档格式一律"逐字节拼/逐字节解"，绝不 write(fd, &struct, sizeof(struct))：
// 结构体布局受 padding、对齐和主机字节序影响，换个编译器就会变。
// 这些函数是 inline 的：它们太小，不值得为每个格式模块各留一份。
// 边界与不变量：
//   * 只处理定长整数；字符串与变长字段的长度前缀由各格式模块自己决定；
//   * Append* 只追加，绝不覆盖或重排已有内容 —— 缓冲区内容就是磁盘布局；
//   * Store* 没有长度参数，调用方必须保证目标缓冲区至少有 N 字节；
//   * Read* 只在字段完整时才推进 cursor：失败时 cursor 原地不动，调用方
//     可以“读失败就整体放弃”，不会留下一个半移动的游标；
//   * cursor 永不超过 size，这是 Read* 自己维持的不变量，也是
//     “*cursor + N”不会整数回绕的前提；
//   * Load* 不做边界检查，只在整块长度已被校验（或块本身定长）时使用。
// 没有异常、没有动态分配，失败一律用返回值表达。

#ifndef BACKUP_PROJECT_INCLUDE_BYTE_ORDER_H_
#define BACKUP_PROJECT_INCLUDE_BYTE_ORDER_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace backupproject {

inline void AppendU16LE(std::string* out, std::uint16_t value) {
  out->push_back(static_cast<char>(value & 0xFFu));
  out->push_back(static_cast<char>((value >> 8) & 0xFFu));
}

inline void AppendU32LE(std::string* out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out->push_back(static_cast<char>((value >> shift) & 0xFFu));
  }
}

inline void AppendU64LE(std::string* out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out->push_back(static_cast<char>((value >> shift) & 0xFFu));
  }
}

// 把一个值写进调用方给的定长缓冲区，不改变缓冲区长度。
inline void StoreU16LE(unsigned char* out, std::uint16_t value) {
  out[0] = static_cast<unsigned char>(value & 0xFFu);
  out[1] = static_cast<unsigned char>((value >> 8) & 0xFFu);
}

inline void StoreU32LE(unsigned char* out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out[shift / 8] = static_cast<unsigned char>((value >> shift) & 0xFFu);
  }
}

inline void StoreU64LE(unsigned char* out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out[shift / 8] = static_cast<unsigned char>((value >> shift) & 0xFFu);
  }
}

// 读侧：字段不完整就返回 false，并且不改动 cursor。size 是缓冲区长度。
inline bool ReadU8(const unsigned char* data, std::size_t size,
                   std::size_t* cursor, std::uint8_t* out) {
  if (*cursor + 1 > size) {
    return false;
  }
  *out = data[*cursor];
  *cursor += 1;
  return true;
}

// 手工逐字节组装而不是 reinterpret_cast：归档 header 里的字段大多紧挨着，
// data 未必对齐到 2/4/8 字节；未对齐访问在部分架构上是陷阱，移位拼装则
// 在任何架构上都正确。
inline bool ReadU16LE(const unsigned char* data, std::size_t size,
                      std::size_t* cursor, std::uint16_t* out) {
  if (*cursor + 2 > size) {
    return false;
  }
  *out = static_cast<std::uint16_t>(data[*cursor]) |
         static_cast<std::uint16_t>(
             static_cast<std::uint16_t>(data[*cursor + 1]) << 8);
  *cursor += 2;
  return true;
}

inline bool ReadU32LE(const unsigned char* data, std::size_t size,
                      std::size_t* cursor, std::uint32_t* out) {
  if (*cursor + 4 > size) {
    return false;
  }
  std::uint32_t value = 0;
  for (int index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(data[*cursor + index]) << (8 * index);
  }
  *out = value;
  *cursor += 4;
  return true;
}

inline bool ReadU64LE(const unsigned char* data, std::size_t size,
                      std::size_t* cursor, std::uint64_t* out) {
  if (*cursor + 8 > size) {
    return false;
  }
  std::uint64_t value = 0;
  for (int index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(data[*cursor + index]) << (8 * index);
  }
  *out = value;
  *cursor += 8;
  return true;
}

// 定长缓冲区版本（读侧从已经读进内存的固定长度块里取值）。
// 读侧的无 cursor 版本：偏移由调用方自己知道，适合定长 header 内部取值。
// 一旦需要边读边校验长度，就应该改用上面的 Read* 系列。
inline std::uint16_t LoadU16LE(const unsigned char* data) {
  return static_cast<std::uint16_t>(data[0]) |
         static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[1]) << 8);
}

inline std::uint32_t LoadU32LE(const unsigned char* data) {
  std::uint32_t value = 0;
  for (int index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(data[index]) << (8 * index);
  }
  return value;
}

inline std::uint64_t LoadU64LE(const unsigned char* data) {
  std::uint64_t value = 0;
  for (int index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(data[index]) << (8 * index);
  }
  return value;
}

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_BYTE_ORDER_H_
