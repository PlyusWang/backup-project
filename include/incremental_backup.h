// incremental_backup.h
//
// PR #18：把"增量策略"接在既有备份流水线上。
//
// 一次增量备份要回答三个问题，这个模块就是这三个问题本身：
//
//   1. 有没有**可信的基线**？
//      —— 必须有基线快照 + 它自己的强 manifest（内容摘要），而且它声明的
//         repository / source / 规则身份都要和这一次对得上。任何一条不成立就
//         老老实实建一份全新 Full baseline，并如实报告"这次其实是 baseline"。
//   2. 与基线相比，有效备份集合变了没有？
//      —— 用强 manifest 对比（内容摘要，不是 size+mtime）。没有变化就不建
//         新快照。目录 mtime-only 的变化不算变化（沿用既有产品决定）。
//   3. 变了什么？
//      —— added / modified / metadata-only / removed / type changed，
//         其中 removed 变成 tombstone，其余进 delta 的 payload。
//
// manifest 作为**基线的副文件**保存在仓库里（<snapshot>.manifest），因为它记录
// 的是"那一份快照对应的源状态"，和快照本身同生共死才不会被错误沿用。
//
// 本文件是纯 C++17：不依赖 Qt，也不依赖任何第三方库。

#ifndef BACKUP_PROJECT_INCLUDE_INCREMENTAL_BACKUP_H_
#define BACKUP_PROJECT_INCLUDE_INCREMENTAL_BACKUP_H_

#include <cstdint>
#include <string>
#include <vector>

#include "archive_entry.h"
#include "archive_pipeline.h"
#include "filter.h"
#include "source_manifest.h"

namespace backupproject {

// 仓库里某一份快照的 manifest 副文件名：<snapshot 文件名>.manifest。
// catalog 只列 *.bak，所以它不会被当成一份快照。
std::string SnapshotManifestFileName(const std::string& snapshot_file_name);

// 同一份快照的**身份副文件**：<snapshot 文件名>.identity。
//
// 它记的是"这份快照属于哪条链"的三件事：源 / 规则 / 策略（pack+compression+
// encryption）。三者任何一个变了都必须重新建立完整基线：
//
//   * 源或仓库变了：源树根本是另一棵树，沿用旧链毫无意义；
//   * 规则变了：有效备份集合变了，把"因为规则变化而不再包含的路径"当成一堆
//     tombstone 去删，是拿用户的数据赌一个配置改动；
//   * pack/compression/encryption 变了：同一条链里的 delta 必须用同一套参数
//     才能被正确应用（见 PR18 设计第 13 节），跨参数继续链是不合法的。
std::string SnapshotIdentityFileName(const std::string& snapshot_file_name);

// 一次增量备份实际做了什么。调用方（CLI / GUI / 计划服务）据此如实告诉用户：
// "你要的是增量，但这次建的是完整基线"这句话必须能被说出来。
struct IncrementalOutcome {
  enum class Kind {
    // 没有可信基线：建了一份完整基线快照。
    kFullBaseline,
    // 有基线、有真实变化：写了一份 delta。
    kDelta,
    // 有基线、有效备份集合没有变化：什么都不写。
    kNoChanges,
  };
  Kind kind = Kind::kNoChanges;
  std::string snapshot_file_name;
  std::string parent_file_name;
  ChangeSummary summary;
  // 为什么建基线（kind == kFullBaseline 时非空）：没有基线 / 基线不可信 /
  // 源、规则、仓库、策略身份变了。
  std::string baseline_reason;
  // 面向人的一句话，CLI 与 GUI 用同一句。
  std::string summary_text;
};

// 在仓库里找"当前可以当作基线的快照"：按文件名倒序找第一份既有 manifest
// 副文件、binding 又与给定身份一致、且自身仍然存在的快照。
// 找不到时返回 false 并把原因写进 reason（不是错误——那是"需要建基线"）。
bool FindIncrementalBaseline(const std::string& repository_directory,
                             const std::string& source_path,
                             const std::string& repository_identity,
                             const std::string& filter_identity,
                             const std::string& strategy_identity,
                             std::string* baseline_file_name,
                             std::string* reason);

// 执行一次增量备份。
//
// snapshot_file_name 由调用方给出（BackupCatalog 负责命名），必须是单组件名字。
// baseline_snapshot_name 非空时强制以它为父；为空时自动找当前基线。
//
// 失败时不留下半成品：delta 用临时文件 + rename 发布，manifest 在快照发布
// 成功之后才写，写失败也不会让快照变成"看起来可用但没基线"的状态。
bool RunIncrementalBackup(const std::string& source_directory,
                          const std::string& repository_directory,
                          const std::string& snapshot_file_name,
                          const std::string& repository_identity,
                          const Filter& filter, const BackupOptions& options,
                          const std::vector<std::string>& include_rules,
                          const std::vector<std::string>& exclude_rules,
                          const std::string& baseline_snapshot_name,
                          IncrementalOutcome* outcome,
                          std::string* error_message);

// 从磁盘上的强 manifest 副文件读一份基线 manifest。
bool LoadSnapshotManifest(const std::string& repository_directory,
                          const std::string& snapshot_file_name,
                          std::vector<ManifestEntry>* entries,
                          std::string* error_message);

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_INCREMENTAL_BACKUP_H_
