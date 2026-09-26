// archive_pipeline.h
//
// 可组合归档流水线的对外接口。
//
//   backup:  source tree → pack → compress → encrypt → v2 container(.bak)
//   restore: .bak → header 校验 → HMAC 认证 → 解密 → 解压 → unpack → metadata
//
// 三层各自只认字节：
//   * pack 层知道路径、uid、软链接；压缩与加密完全不知道这些；
//   * compression 层只看到一条 packed 字节流；
//   * encryption 层只看到一条 compressed 字节流。
// 顺序是硬性的：PACK → COMPRESS → ENCRYPT。反过来先加密再压缩没有意义
// （密文不可压缩），本项目也不提供那条路径。
//
// 旧 API（ArchiveWriter / ArchiveReader 的 v0.1 路径）与本文件互不影响：
// 不显式选择 v2 pipeline 的调用方，行为与以前完全一致。

#ifndef BACKUP_PROJECT_INCLUDE_ARCHIVE_PIPELINE_H_
#define BACKUP_PROJECT_INCLUDE_ARCHIVE_PIPELINE_H_

#include <cstdint>
#include <string>
#include <vector>

#include "archive_entry.h"
#include "container_format.h"
#include "filter.h"
#include "pack_stream.h"

namespace backupproject {

// 备份选项。默认值等价于"跟以前一样"：MyPack + 不压缩 + 不加密。
struct BackupOptions {
  PackMethod pack_method = PackMethod::kMyPack;
  CompressionMethod compression_method = CompressionMethod::kNone;
  EncryptionMethod encryption_method = EncryptionMethod::kNone;
  // 为空且 encryption_method != kNone 时明确失败：不做"空密码也能加密"这种
  // 自己骗自己的事。密码只在本进程内存里活着，不写 config、不写日志、
  // 不进归档、不回显。
  std::string password;
};

struct RestoreOptions {
  std::string password;
};

// 恢复过程里"能做但不一定做得成"的步骤留下的诊断。
//
// 最典型的是 ownership：归档精确保存 uid/gid，但非 root 进程不允许把文件
// 改成任意属主。这时的正确做法是尽力而为 + 如实记录，而不是让整个恢复不可用，
// 也不是假装已经完整恢复。
struct RestoreReport {
  std::uint64_t restored_entries = 0;
  std::uint64_t skipped_ownership = 0;
  std::vector<std::string> notes;
};

// 走 v2 container 的备份。source_directory 必须是目录；archive_file
// 必须不存在， 也不能落在源目录内部。任何一步失败都会删掉自己创建的半成品：
// 用户看到目录里有个 .bak 就会当它是可用备份。
bool RunBackupPipeline(const std::string& source_directory,
                       const std::string& archive_file, const Filter& filter,
                       const BackupOptions& options,
                       std::string* error_message);

// 从一份已经准备好的条目表走完整条流水线（扫描是唯一被跳过的步骤）。
// 给测试与工具用：非 root 环境造不出真的字符/块设备，但格式层与 metadata 层
// 必须能被完整覆盖，所以需要一条"直接喂条目表"的入口。
bool RunBackupPipelineFromEntries(const std::vector<ArchiveEntry>& entries,
                                  const std::string& archive_file,
                                  const BackupOptions& options,
                                  std::string* error_message);

// 走 v2 container 的恢复。destination_directory 不存在或存在但为空时都可以。
//
// 失败时的保证：destination 一定不会出现。恢复先写进一个唯一的兄弟暂存目录，
// metadata 全部收尾之后才 rename 成 destination；中途失败只会删掉暂存目录。
// 所以 wrong password、坏压缩流、坏 TAR、hardlink 冲突都不会先建出最终目标。
//
// report 可以为空。
bool RunRestorePipeline(const std::string& archive_file,
                        const std::string& destination_directory,
                        const RestoreOptions& options, RestoreReport* report,
                        std::string* error_message);

// 从一条已经 unpack 好的 packed 流恢复（不做 container / 压缩 / 加密处理）。
//
// 与 RunRestorePipeline 共用同一段 preflight + 暂存目录 + metadata 收尾逻辑，
// 所以对外部工具生成的裸 USTAR（例如 GNU tar --format=ustar 的输出）做
// 互操作测试时，走的确实是产品代码那条恢复路径。
bool RunRestorePackedStream(const std::string& packed_file,
                            PackMethod pack_method,
                            std::uint64_t expected_entry_count,
                            const std::string& destination_directory,
                            RestoreReport* report, std::string* error_message);

// 只读地辨认一个 .bak：v0.1 legacy 还是 v2 container。不看扩展名，只看 magic。
struct ArchiveFileInfo {
  enum class Kind { kUnknown, kLegacyV01, kContainerV2 };

  Kind kind = Kind::kUnknown;
  std::uint16_t format_version = 0;
  std::uint64_t entry_count = 0;
  PackMethod pack_method = PackMethod::kMyPack;
  CompressionMethod compression_method = CompressionMethod::kNone;
  EncryptionMethod encryption_method = EncryptionMethod::kNone;
  std::string password_hint;  // 非空表示这个归档需要密码才能恢复
};

bool IdentifyArchiveFile(const std::string& archive_file, ArchiveFileInfo* info,
                         std::string* error_message);

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_ARCHIVE_PIPELINE_H_
