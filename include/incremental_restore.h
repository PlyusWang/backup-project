// incremental_restore.h
//
// PR #18：把"任意一个 restore point"恢复出来。
//
// 用户只需要选一份快照（完整或增量），依赖链由这里解析：
//
//     Full baseline → Δ1 → Δ2 → … → ΔN
//
// 每一步都必须能被验证，而不是"按文件名顺着找"：
//   * parent 必须存在、必须是合法单组件名字；
//   * parent 的身份必须等于子节点记录的 parent_snapshot_id
//     （完整快照的身份由它自己的 payload 摘要派生，见 incremental_delta.h）；
//   * 不允许环、不允许自指、链深度有上界；
//   * 整条链必须属于同一个 generation（同一个 Full baseline）。
//
// restore 仍然沿用 PR #17 的原子原则：先完整恢复到 staging，全部成功之后才
// 发布成 destination。任何失败都不会留下半个目标目录。
//
// 本文件是纯 C++17：不依赖 Qt，也不依赖任何第三方库。

#ifndef BACKUP_PROJECT_INCLUDE_INCREMENTAL_RESTORE_H_
#define BACKUP_PROJECT_INCLUDE_INCREMENTAL_RESTORE_H_

#include <cstdint>
#include <string>
#include <vector>

#include "archive_pipeline.h"
#include "incremental_delta.h"

namespace backupproject {

// 一条已解析好的依赖链。
struct SnapshotChain {
  // 绝对路径，base 在前、目标在最后。
  std::vector<std::string> files;
  // 单组件名字，与 files 一一对应。
  std::vector<std::string> file_names;
  // 链上的 delta 个数（files.size() - 1）。
  std::uint64_t delta_count = 0;
  // 目标 delta 声明的"应用完它之后源树应有的 manifest digest"；
  // 目标是完整快照时为空。
  std::string target_manifest_digest;
  // 这条链属于哪个 generation（= base 的 snapshot id）。
  std::string base_generation_id;
};

// 解析 target 的依赖链。target_file_name 必须是仓库内的单组件名字。
bool ResolveSnapshotChain(const std::string& repository_directory,
                          const std::string& target_file_name,
                          SnapshotChain* chain, std::string* error_message);

// 恢复到任意 restore point。
//
//   * 目标是完整快照时，行为与既有 RunRestorePipeline 完全一致；
//   * 目标是 delta 时，先恢复 base，再逐个 delta 应用（先 tombstone，深的先删；
//     再覆盖新增/修改/类型变化），最后把 staging 发布成 destination。
//
// destination_directory 不存在或存在但为空时都可以。失败时不留下目的地。
bool RestoreSnapshotChain(const std::string& repository_directory,
                          const std::string& target_file_name,
                          const std::string& destination_directory,
                          const RestoreOptions& options, RestoreReport* report,
                          std::string* error_message);

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_INCREMENTAL_RESTORE_H_
