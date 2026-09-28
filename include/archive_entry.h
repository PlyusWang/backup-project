// archive_entry.h
//
// 归档流水线的统一条目模型。
//
// 扫描层（TreeScanner）lstat 源目录树后产出 std::vector<ArchiveEntry>，
// 所有 pack 后端（MyPack v2 / baseline USTAR / Fast USTAR）消费同一份数据。
// 这样"什么是一个条目"只有一处定义，三种打包策略不可能各自漂移。
//
// 本文件是纯 C++17：不依赖 Qt，也不依赖任何第三方库。

#ifndef BACKUP_PROJECT_INCLUDE_ARCHIVE_ENTRY_H_
#define BACKUP_PROJECT_INCLUDE_ARCHIVE_ENTRY_H_

#include <cstdint>
#include <string>
#include <vector>

namespace backupproject {

// 条目类型。数值就是 MyPack v2 entry header 的 type 字段与 USTAR typeflag 的
// 映射来源，属于格式契约，不能改数值。
enum class EntryType : std::uint8_t {
  kDirectory = 1,
  kRegularFile = 2,
  kSymlink = 3,
  kHardLink = 4,
  kFifo = 5,
  kCharDevice = 6,
  kBlockDevice = 7,
  // socket 只存在于扫描 / Filter / preview 层：归档格式里没有它，
  // 遇到会进入归档的 socket 时整次备份明确失败。数值 8 不写进任何归档文件。
  kSocket = 8,
};

// filter DSL 的 type: 取值；socket 只在 filter 层存在（归档格式里没有它）。
inline const char* EntryTypeName(EntryType type) {
  switch (type) {
    case EntryType::kDirectory:
      return "directory";
    case EntryType::kRegularFile:
      return "regular";
    case EntryType::kSymlink:
      return "symlink";
    case EntryType::kHardLink:
      return "hardlink";
    case EntryType::kFifo:
      return "fifo";
    case EntryType::kCharDevice:
      return "char";
    case EntryType::kBlockDevice:
      return "block";
    case EntryType::kSocket:
      return "socket";
  }
  return "unknown";
}

// 一个条目的全部元数据。
//
// archive_path 用归档内部形式：相对 source root、'/' 分隔，source root 自身是
// "."。 source_path 是磁盘上的真实路径，扫描层与 pack 层靠它读 payload。
struct ArchiveEntry {
  std::string archive_path;
  std::string source_path;
  EntryType type = EntryType::kRegularFile;

  // 07777：rwx + setuid + setgid + sticky。
  std::uint32_t mode = 0;
  std::uint32_t uid = 0;
  std::uint32_t gid = 0;
  std::int64_t mtime_sec = 0;
  std::uint32_t mtime_nsec = 0;

  // 普通文件的正文长度；其余类型为 0。
  std::uint64_t size = 0;

  // 软链接：目标原文（不 follow）。硬链接：归档内第一个出现的相同 inode 的
  // archive_path。其余类型为空。
  std::string link_target;

  // 字符/块设备的 major/minor；其余类型为 0。
  std::uint32_t dev_major = 0;
  std::uint32_t dev_minor = 0;

  // 可选的名字缓存，扫描层通过 getpwuid_r / getgrgid_r 解析；解析失败时留空，
  // 数字 uid/gid 仍然可用。所有类型都会尝试解析（包括软链接：lstat 拿到的
  // 就是链接自己的 uid/gid，这不涉及 follow）。
  std::string user_name;
  std::string group_name;

  // 扫描那一刻的 (st_dev, st_ino)。**这是内部快照字段，不写进任何归档格式**，
  // 存在的意义只有一个：打包时确认"我现在读的还是扫描时那一个 inode"。
  // 它为 0 表示扫描层没有提供，写侧就不做这项检查。
  std::uint64_t source_dev = 0;
  std::uint64_t source_ino = 0;

  // ---- 增量流水线的内容绑定（不写进任何归档格式）----
  //
  // 强 manifest 已经算过每个普通文件的正文摘要、每个软链接的目标摘要。真正写
  // payload 时会**再读一遍源**，两次之间源可能被改写——same-size + 原 mtime
  // 的人为改写正好躲得过上面那套 size/mtime 检查。所以写侧把期望摘要带在
  // 身上：写完 payload 立刻核对，不一致就整次失败，绝不发布一份
  // "manifest 说的"和"payload 里装的"不是同一份内容的快照。
  //
  // 普通文件 = 正文摘要；软链接 = 目标字节摘要；其余类型为空。
  // 空串的含义是"这一版没有期望值"（完整备份路径、测试直接构造的条目表）。
  std::string expected_content_digest;

  // 这条条目来自一次真实扫描。为真时，写侧会对**没有正文**的类型
  // （FIFO / 字符设备 / 块设备）做最后一次 lstat 比对：它们的身份只能靠
  // 类型 + 设备号 + metadata 表达，plan 与 payload 之间被改掉就必须失败。
  bool expect_source_unchanged = false;
};

// Linux dev_t 的 major/minor 拆分（glibc 的编码不是简单的位移）。
std::uint32_t DeviceMajor(std::uint64_t device);
std::uint32_t DeviceMinor(std::uint64_t device);
std::uint64_t MakeDevice(std::uint32_t major, std::uint32_t minor);

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_ARCHIVE_ENTRY_H_
