// archive.h
//
// 归档格式 v0.1（Archive Format v0.1）的读写接口。
//
// 这是"打包"而不是"压缩"：每个普通文件的正文原样写进归档文件，
// 只是前面加了我们自己的全局 header 和逐条 entry 元数据。
// 归档里的 payload 与源文件 byte-for-byte 相同，没有压缩、编码或加密。
//
// ---- 在备份流水线里的位置 ----
//
//   源目录 ──► ArchiveWriter ──► 未压缩归档文件 ──► ArchiveReader ──► 恢复目录
//                                      ▲
//                        （未来：Compression 作为独立一层接在这里）
//
// 本 PR 交付的是中间那一层：它自己就是完整可用的功能，同时也注定是将来
// 压缩层的输入。所以这两个类刻意不依赖 BackupEngine / CLI / GUI，只认路径
// 参数，可以单独测试、单独复用；格式常量也放在这个头文件里而不是 .cpp 里，
// 将来压缩层或外部工具要认这些数字时不必再抄一份。
//
// 压缩不在 v0.1：这里没有压缩算法，也没有为压缩预留的实现骨架，
// 只有版本号、flags 字段这类正常的可演进设计。
//
// 逐字段的格式说明见 docs/format/archive_v0.1.md，两边必须保持一致。

#ifndef BACKUP_PROJECT_INCLUDE_ARCHIVE_H_
#define BACKUP_PROJECT_INCLUDE_ARCHIVE_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace backupproject {

class Filter;  // 前向声明：归档层只需要 Filter 的指针

// v0.1 的格式常量，数值与 docs/format/archive_v0.1.md 的偏移表一一对应。
// 这些字段是格式契约的一部分，改动任何一个都必须同时改文档和版本号。
namespace archive_v01 {

// 全局 header 固定 24 字节，entry header 固定 32 字节。
inline constexpr std::size_t kGlobalHeaderSize = 24;
inline constexpr std::size_t kEntryHeaderSize = 32;
// entry_count 在全局 header 内的偏移：先写占位，扫完源目录再回填。
inline constexpr std::uint64_t kEntryCountOffset = 16;

inline constexpr std::uint16_t kFormatVersion = 1;
inline constexpr std::uint16_t kFormatFlags = 0;
inline constexpr std::uint32_t kMaxPathLength = 4096;

inline constexpr std::uint8_t kTypeDirectory = 1;
inline constexpr std::uint8_t kTypeRegularFile = 2;

// v0.1 只保存 0777 这 9 个权限位；类型位与 setuid/setgid/sticky 都不进归档。
inline constexpr std::uint32_t kPermissionMask = 0777;
inline constexpr std::uint32_t kMaxMtimeNsec = 999999999;

// 8 字节 magic：'B' 'K' 'P' 'A' 'R' 'C' 'H' '\0'。
inline constexpr unsigned char kMagic[8] = {'B', 'K', 'P', 'A',
                                            'R', 'C', 'H', '\0'};

}  // namespace archive_v01

// 把一棵目录树写成一个归档文件（推荐扩展名 .bak，但格式判断只认文件里的
// magic + version，不看扩展名）。
class ArchiveWriter {
 public:
  ArchiveWriter() = default;

  // source_directory 必须是已存在的目录；archive_file 必须还不存在。
  // archive_file 也不能等于 source_directory 或落在它里面：否则扫描过程中
  // 归档文件自己会成为输入树的一部分。这些检查都在创建输出文件之前完成，
  // 非法输入不会留下任何文件系统改动。
  //
  // 只支持普通目录和普通文件；遇到软链接、FIFO、设备、socket 时整次
  // 打包失败，不跳过、不跟随、也不当普通文件复制。
  bool Write(const std::string& source_directory,
             const std::string& archive_file, std::string* error_message) const;

  // 带筛选的版本：filter 为 nullptr 表示"没有规则"，行为与上面完全一致。
  // Filter 只决定条目是否进入归档，归档格式本身不因为筛选而变化。
  bool Write(const std::string& source_directory,
             const std::string& archive_file, const Filter* filter,
             std::string* error_message) const;
};

// 从归档文件恢复目录树。
class ArchiveReader {
 public:
  ArchiveReader() = default;

  // 两阶段执行：先把整个归档完整校验一遍（preflight），确认结构合法之后
  // 才动磁盘。校验失败时 destination 保持原状，连一个空目录都不会留下。
  bool Extract(const std::string& archive_file,
               const std::string& destination_directory,
               std::string* error_message) const;
};

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_ARCHIVE_H_
