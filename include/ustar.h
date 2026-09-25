// ustar.h
//
// 标准 USTAR（POSIX 1003.1 tar）的编解码、写入器与读取器。
//
// 与 docs/format/archive_v0.1.md 里那套自制格式不同：这里产出和接受的是通用
// ustar，GNU tar 能直接读我们写的归档，我们的 Scan 也能读 GNU tar 写出来的
// ustar。因此本文件里的字段布局是**外部契约**，不是我们自己的方便约定：
// 改一个偏移就不再是 ustar 了。
//
// 三条贯穿实现的约束：
//
//   1. 数字字段是八进制 ASCII（7 位数字 + NUL，或 11 位数字 + NUL）。数值放不下
//      时明确失败，绝不截断——把 8 GiB 的文件声明成 8 GiB-1 比直接报错危险得多；
//   2. 写侧 header 里的每个字节都来自 ArchiveEntry：不 follow
//      软链接（O_NOFOLLOW），也不从磁盘回填任何字段。磁盘只在两处被用到：
//      按条目给出的 source_path 打开源文件读 payload；
//      读完 payload 之后再 fstat 一次，确认读到的还是扫描时那一份
//      （普通文件 + size / mtime / dev+ino）。
//      复核结论只决定"这次写入是否失败"，不会被写进 header；
//   3. 读侧先 preflight：Scan 校验完 header、checksum、路径、边界之后才返回
//      Member 列表，而且它自己不动文件系统；payload 只在 ExtractData 里按
//      Member 给出的偏移读。
//
// 不做的事：压缩、加密、pax 扩展头（长路径 / 高精度时间 / 大数值）、GNU 扩展
// （sparse、base-256 数值、L/K 长名字头）。遇到就明确报错，不猜。
//
// 负 mtime（1970 之前）在 ustar 的 11 位八进制字段里没有表示，写侧会失败。
// 这是格式本身的边界，不是实现偷懒：与其写一个自己都不信的时间戳，不如报错。

#ifndef BACKUP_PROJECT_INCLUDE_USTAR_H_
#define BACKUP_PROJECT_INCLUDE_USTAR_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "archive_entry.h"

namespace backupproject {
namespace ustar {

// 一个 block 固定 512 字节：header、payload、padding、结尾标记都以它为单位。
inline constexpr std::size_t kBlockSize = 512;

// header 里三个路径字段的容量。prefix + '/' + name 合计上限是 POSIX 允许的
// 255 字节；linkname 是单独的 100 字节字段。
inline constexpr std::size_t kNameSize = 100;
inline constexpr std::size_t kPrefixSize = 155;
inline constexpr std::size_t kMaxPathSize = 255;
inline constexpr std::size_t kLinkNameSize = 100;

// Scan 的条目数上限：超过就明确报错，而不是把内存吃光。
inline constexpr std::size_t kMaxMembers = 1000000;

// 一个 ustar header 的字段值。字段名与 POSIX 布局一一对应，编码/解码时不做
// 任何"顺手规范化"：name 与 prefix 是分开的两个字段，怎么拆由 SplitPath 决定。
struct Header {
  char typeflag = '0';
  std::string name;        // <= 100 字节
  std::string prefix;      // <= 155 字节
  std::string linkname;    // <= 100 字节
  std::uint32_t mode = 0;  // 07777
  std::uint32_t uid = 0;
  std::uint32_t gid = 0;
  std::uint64_t size = 0;
  std::int64_t mtime = 0;
  std::uint32_t devmajor = 0;
  std::uint32_t devminor = 0;
  std::string uname;
  std::string gname;
};

// 路径拆分：总长 <= 255 且能按 '/' 切成 prefix(<=155) + name(<=100)
// 才算可表示。
//
// archive_path 用归档内部形式：相对路径、'/' 分隔、source root 自身是 "."
// （与 ArchiveEntry::archive_path 完全一致）。绝对路径、'..'、空 component、
// '.' component、结尾 '/'、NUL 字节、超过 255 字节都会失败并给出原因。
// 拆分点尽量靠后：prefix 里放尽可能多的目录，name 里只留最后一段。
bool SplitPath(const std::string& archive_path, std::string* name,
               std::string* prefix, std::string* error_message);

// EntryType <-> typeflag。socket 没有 ustar 表示：写侧会明确拒绝，读侧遇到
// GNU 的 's' 报 "unsupported special type: socket"。
char TypeFlagFor(EntryType type);
bool TypeFromFlag(char typeflag, EntryType* type, std::string* error_message);

// 512 字节 header 编码 / 解码。
//
// 解码必须校验：magic/version、checksum（把 chksum 字段当 8 个空格重算）、
// 数字字段的八进制合法性与终止符、字段边界、typeflag 合法性、非普通条目的
// size 必须为 0。任何一条不满足都返回 false 并写明原因。
//
// 编码只做字段级校验（长度、数值范围、typeflag）；路径语义由 SplitPath 与
// Scan 负责。uname/gname 超过 31 字节时按 tar 惯例截断，其余字段一律不截断。
bool EncodeHeader(const Header& header, std::string* block,
                  std::string* error_message);
bool DecodeHeader(const char* block /*512 字节*/, Header* header,
                  std::string* error_message);

// 把一个条目列表写成 ustar 归档。
//
// entries 已经是 DFS 先序（父先于子），archive_path 已经过校验：扫描层负责
// 这件事，写入器只按顺序落盘，不重新排树。
//
// 两个写入器的格式逻辑完全共用，产出的 wire format 也逐字节相同，区别只在
// I/O 策略：
//
//   * WriteBaseline：64 KiB 输出缓冲、逐 entry 处理、每条 entry 结束就 flush，
//     payload 先读进 64 KiB 临时数组再 append 进缓冲，写得直白，作为"格式对
//     不对"的参照；
//   * WriteFast：1 MiB 统一缓冲，header / payload / padding 聚合进同一个缓冲，
//     payload 用大块 read 直接读进缓冲空闲区，并给源 fd 加顺序读提示。
//
// 普通文件的 payload 由两者共用的内部函数 CopyVerifiedPayload 读：读满
// entry.size 之后**再** fstat 一次，确认源文件仍是扫描时那一份（普通文件、
// size 与 mtime（秒 + 纳秒）未变，entry.source_ino 非 0 时 dev+ino 也未变）。
// 任何一条不满足都让整次写入失败，磁盘上不留归档。这条检查挡不住"原地改写、
// 长度不变、mtime 被改回原值、inode 也没换"的替换——那需要内容哈希，不在本轮
// 范围内。
//
// 两者都用 O_WRONLY|O_CREAT|O_EXCL、权限 0600 创建输出（不覆盖已有文件），
// 失败时删掉自己创建的半成品；写完的归档恰好以两个全零 block 结束，后面没有
// 多余字节。
bool WriteBaseline(const std::vector<ArchiveEntry>& entries,
                   const std::string& archive_file, std::string* error_message);
bool WriteFast(const std::vector<ArchiveEntry>& entries,
               const std::string& archive_file, std::string* error_message);

// 归档里的一个成员。data_offset 是 payload 在归档文件里的起始偏移（512 对齐），
// data_size 是 payload 长度；目录 / FIFO / 设备 / 软链接 / 硬链接没有 payload，
// 这两种情况下 data_size 为 0。
struct Member {
  ArchiveEntry entry;             // source_path 留空
  std::uint64_t data_offset = 0;  // payload 起始偏移（512 对齐）
  std::uint64_t data_size = 0;    // 目录/FIFO/设备/软链接/硬链接为 0
};

// preflight：把整个归档校验一遍，只读不写。
//
// 校验内容：结尾标记（两个全零 block，之后的补零可以容忍）、每个 header 的
// checksum 与字段、payload 与 padding 边界、路径安全（绝对路径 / '..' / 空
// component / NUL）、重复路径、父子冲突、typeflag、硬链接目标、条目数上限。
// 任何一条失败都返回 false，此时 *members 的内容没有意义。
//
// 硬链接目标允许出现在硬链接之后（forward link）：恢复侧有 pending 队列，标准
// tar 也允许。但它必须存在于完整 member set 里，并且顺着 link_target 走到终点
// 必须是一条普通文件条目——链（A -> B -> regular）合法；指向目录 / 软链接 /
// FIFO / 设备、自指、以及环都判失败。
//
// 目录条目的结尾 '/' 与 GNU tar 的 "./" 前缀在这里被归一化掉，所以
// Member::entry.archive_path 与我们自己写出来的一模一样。
bool Scan(const std::string& archive_file, std::vector<Member>* members,
          std::string* error_message);

// 把某个成员的 payload 写到目标文件。只对普通文件成员有意义：其它类型没有
// payload，传进来会明确失败，而不是悄悄建一个空文件。
//
// 目标文件用 O_CREAT|O_TRUNC 打开（已存在就被覆盖），失败时删掉半成品。
// 归档在 ExtractData 里会重新校验一次边界：Scan 的结果是可信的，但
// "可信"不等于"可以省略检查"。
bool ExtractData(const std::string& archive_file, const Member& member,
                 const std::string& destination_file,
                 std::string* error_message);

// 同上，但读进内存。供需要在不落盘的情况下检查 payload 的调用方使用；
// output 在失败时的内容没有意义。
bool ExtractDataToString(const std::string& archive_file, const Member& member,
                         std::string* output, std::string* error_message);

}  // namespace ustar
}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_USTAR_H_
