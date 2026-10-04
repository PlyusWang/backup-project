// include/snapshot_bundle.h
//
// PR #21：远端快照的**材料包**（BPSNAP1）。
//
// 为什么需要它：增量引擎眼中的一份快照不是单个文件，而是**三件套**——
//   <name>.bak       归档本体（完整快照或 BKPINC1 delta）
//   <name>.manifest  强 manifest（带每个文件的内容摘要）
//   <name>.identity  BPIDENT2 身份记录
// 生成下一份 delta 需要父的三件套，恢复链上的每一跳同样需要。远端如果只存
// .bak，收到的链在"副文件验证"这一步必然失败——这正是 PR #18 踩过的坑。
//
// 所以远端存的是把三件套按顺序装进一个容器的字节流：
//
//   offset  size  字段
//   0       8     magic "BPSNAP1\0"
//   8       2     format version (u16, 大端) = 1
//   10      2     member count (u16, 大端) = 3
//   然后每个成员重复：
//     u16  名字长度（<= 255）
//     N    名字（单组件文件名，不含任何路径分隔符）
//     u64  数据长度（大端）
//     32   SHA-256(数据)
//     M    数据本身
//
// 这个容器**不解释**里面的字节：.bak 的格式仍然完全由增量引擎拥有，这里
// 只是把三个文件按名字与摘要绑在一起传输。服务端存的就是这段字节，它既
// 不知道也不需要知道里面是什么。
//
// 信任链（三层，缺一不可）：
//   1. 下载时对**整个 bundle** 校验长度与 SHA-256（远程客户端的下载路径）；
//   2. 解包时对**每个成员**校验长度与 SHA-256（本模块）；
//   3. 交给增量引擎之后，由 LoadVerifiedSnapshotIdentity / RestoreSnapshotChain
//      再对归档内容与副文件绑定做一次完整验证。
// 任何一层不过，材料都不会被当成可信状态使用。

#ifndef BACKUP_PROJECT_INCLUDE_SNAPSHOT_BUNDLE_H_
#define BACKUP_PROJECT_INCLUDE_SNAPSHOT_BUNDLE_H_

#include <cstdint>
#include <string>
#include <vector>

namespace backupproject {
namespace net {

inline constexpr std::size_t kSnapshotBundleHeaderSize = 12;
inline constexpr std::size_t kSnapshotBundleMemberCount = 3;
inline constexpr std::uint16_t kSnapshotBundleVersion = 1;
inline constexpr std::size_t kSnapshotBundleMaxNameBytes = 255;
// 单个成员的上限：比服务端默认的 8 GiB 上传上限宽一点，多出来的部分留给
// 未来放宽服务端配置；超过它的包头一律拒绝，绝不按声明值分配内存。
inline constexpr std::uint64_t kSnapshotBundleMaxMemberBytes =
    16ull * 1024ull * 1024ull * 1024ull;

struct SnapshotBundleMember {
  std::string name;  // 单组件文件名
  std::uint64_t size = 0;
  std::string sha256;  // 64 个小写十六进制字符
};

struct SnapshotBundleInfo {
  std::string archive_name;  // 第一个成员（.bak）的名字
  std::vector<SnapshotBundleMember> members;
  std::uint64_t bundle_size = 0;  // 容器文件自身的字节数
};

// 把 repository_directory 里的三件套打成一个 bundle 文件。
//
//   * 三个成员都必须存在、都是普通文件、都不是符号链接；
//   * 输出用唯一临时名 + fsync + 原子发布，**不覆盖**已存在的目标
//     （NoReplace）：并发或重试都不会悄悄换掉一份已经上传过的包；
//   * 整个过程不修改源文件。
bool BuildSnapshotBundle(const std::string& repository_directory,
                         const std::string& archive_name,
                         const std::string& bundle_path,
                         SnapshotBundleInfo* info, std::string* error_message);

// 只读成员表（不解包）。诊断与测试用它。
bool InspectSnapshotBundle(const std::string& bundle_path,
                           SnapshotBundleInfo* info,
                           std::string* error_message);

// 解包到 target_directory。
//
//   * 成员表必须与归档名严格对应：正好是 archive、archive.manifest、
//     archive.identity，多一个少一个都拒绝；
//   * 每个成员都先写进唯一临时文件、边写边算 SHA-256，长度与摘要都对得上
//     才算通过；
//   * **全部成员都验证通过之后**才逐个发布（NoReplace）。任何失败都会清掉
//     本次产生的临时文件；已经发布的成员也会被撤掉，绝不留下"三件套缺一件"
//     的中间态；
//   * 目标已存在同名文件时明确失败（缓存目录里的东西要么是验证过的，要么
//     不存在，不允许被覆盖）。
bool ExtractSnapshotBundle(const std::string& bundle_path,
                           const std::string& target_directory,
                           SnapshotBundleInfo* info,
                           std::string* error_message);

}  // namespace net
}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_SNAPSHOT_BUNDLE_H_
