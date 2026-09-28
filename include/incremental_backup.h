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
#include "incremental_delta.h"
#include "source_manifest.h"

namespace backupproject {

// 增量第一版只支持 MyPack 打包方式：USTAR 表达不了 tombstone 与 parent
// dependency，硬套只会产出"恢复语义对不上"的链。
//
// CLI 与 GUI 必须在**启动任务之前**用这一个判断拒绝，而不是等第一次 delta
// 才失败——那时用户已经拿到一份看起来可用的基线，错误来得太晚了。
// 两个前端问的是同一个函数，所以拒绝的理由不可能分叉。
bool IsSupportedIncrementalPack(PackMethod pack);
std::string UnsupportedIncrementalPackReason();

// 增量第一版**不支持加密**，而且必须是"明确拒绝"，不是"先跑一次基线、
// 第二次 delta 才失败"，更不是静默降级成不加密。
//
// 原因不是"没实现"，而是产品语义没有闭环：
//   * BKPINC1 外层信封（parent 绑定 + tombstone 列表）是明文，内层容器的
//     HMAC 覆盖不到它；接受一个加密 delta 等于接受一组未经认证的路径指令；
//   * snapshot_id 是内容摘要，不是认证标签，它证明不了"写它的人有密码"。
//
// 未来的 authenticated incremental envelope 才能重新开放这条组合。
bool IsSupportedIncrementalEncryption(EncryptionMethod encryption);
std::string UnsupportedIncrementalEncryptionReason();

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
//
// ---- 为什么它还必须绑定"这一个 .bak"（BPIDENT2）----
//
// 上面三件事只说明"上一轮是用什么参数跑的"，它们**证明不了**磁盘上这个
// .bak 还是当初那一份：把 F0.bak 换成另一份合法归档、副文件原样留着，
// 旧实现照样把它当成可信基线，于是要么错误地报"没有变化"，要么在一个不是
// 自己祖先的快照上写 delta。所以 v2 的身份副文件额外记录：
//
//   * snapshot_file_name：这份副文件属于哪一个文件名；
//   * snapshot_id：归档**内容**的身份（完整快照 = 容器 payload 摘要派生，
//     delta = 信封自校验摘要）；
//   * manifest_digest：这份快照对应源树的 manifest 摘要；
//   * format_version：副文件自己的版本（旧格式只按"不可信"处理）。
//
// 任何一个对不上，这份快照就不再是基线：老实重建完整基线，绝不继续链。
// BPIDENT1 仍然读得出来，但解析结果是"没有绑定"，同样不可信。
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

// ---- 一份快照在仓库里的**真实**身份 ----
//
// 全部来自文件本身与它自己的两个副文件，没有一处来自"调用方说的"。
// 完整快照与 delta 都通过它：链上的每一跳都用同一个函数核对，不存在
// "full 一套、delta 另一套"的分叉。
struct SnapshotIdentity {
  // 单组件文件名与解析出来的绝对路径。
  std::string snapshot_file_name;
  std::string archive_path;
  SnapshotFileKind kind = SnapshotFileKind::kUnknown;

  // 归档内容的身份（完整快照由容器 payload 摘要派生；delta 是信封自校验摘要）。
  std::string snapshot_id;
  // 这份快照对应源树的 manifest 摘要。完整快照来自身份副文件；delta 还额外要求
  // 它与信封里的 current_manifest_digest 一致。
  std::string manifest_digest;

  // 源 / 规则 / 策略身份（来自身份副文件）。
  std::string source_identity;
  std::string filter_identity;
  std::string strategy_identity;

  // delta 才有：父文件名、父身份、父 manifest 摘要、generation。
  std::string parent_file_name;
  std::string parent_snapshot_id;
  std::string parent_manifest_digest;
  std::string base_generation_id;
  // delta 的完整信封（完整快照时保持默认值）。
  DeltaEnvelope envelope;

  // manifest 副文件的归属（文件名为空表示没有可用的 manifest 副文件）。
  ManifestBinding manifest_binding;

  // 两个副文件都读出来、并且**逐项与磁盘上的事实对上**时才为真。
  // 为假时 sidecar_diagnostic 说明原因；调用方必须把这种快照当成不可信基线。
  bool sidecars_verified = false;
  std::string sidecar_diagnostic;
};

// 读取一份快照的真实身份。
//
// 返回 false 只有一种情况：这个文件名不是仓库的直接子项、不是普通文件、
// 或者归档本身读不出来（这时 error_message 是原因）。
// 副文件缺失 / 版本旧 / 与磁盘事实对不上**不算**失败：返回 true，但
// sidecars_verified = false，原因在 sidecar_diagnostic 里。调用方据此决定
// "跳过这一份，去建新基线"还是"拒绝这条链"。
//
// manifest_entries 可以为空指针。非空时，只有在两个副文件都验证通过之后才会
// 填入 manifest 条目——调用方拿到的条目与"已验证的归属"是同一份，不存在
// "先读条目、再验归属"的中间窗口。
bool LoadSnapshotIdentity(const std::string& repository_directory,
                          const std::string& snapshot_file_name,
                          SnapshotIdentity* identity,
                          std::vector<ManifestEntry>* manifest_entries,
                          std::string* error_message);

// 在仓库里找"当前可以当作基线的快照"：按文件名倒序找第一份两个副文件都在、
// 都验证通过、身份又与给定值一致的快照。
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

// ---- 依赖图与副文件生命周期 ----
//
// 有了链之后，"删一份快照"不再是单文件操作：
//
//   * 还有活着的后代时删掉祖先 = 让那些后代永远不可恢复；
//   * 只删 .bak 而留下 .manifest / .identity = 仓库里慢慢攒一堆孤儿副文件。
//
// 这两件事都由这里回答，CLI、GUI、retention 用的是同一份实现。

// snapshot_file_name 的所有**可达后代**（直接子节点、孙节点……，只算还在仓库
// 里的）。只从读得出来的直接子项快照建边：坏文件、环、缺失的父都不会让整次
// 调用失败，它们只是不产生边。结果按升序去重，不含自己。
bool FindReachableDescendants(const std::string& repository_directory,
                              const std::string& snapshot_file_name,
                              std::vector<std::string>* descendants,
                              std::string* error_message);

// 一份快照拥有的副文件名（<name>.manifest 与 <name>.identity）。
std::vector<std::string> SnapshotSidecarFileNames(
    const std::string& snapshot_file_name);

// 仓库里"副文件还在、对应的 .bak 已经不在了"的孤儿副文件（按名字升序）。
// 普通 List 只用它做诊断，不做任何破坏性动作。
bool FindOrphanSidecars(const std::string& repository_directory,
                        std::vector<std::string>* orphan_file_names,
                        std::string* error_message);

// 显式清理孤儿副文件：只删证明得了是孤儿的那些（对应的 .bak 不存在）。
// retention 在删完该删的快照之后调用它；每个失败都进 diagnostics，不静默。
bool CleanOrphanSidecars(const std::string& repository_directory,
                         std::vector<std::string>* removed_file_names,
                         std::vector<std::string>* diagnostics,
                         std::string* error_message);

// ---- 测试接缝（只有一个）----
//
// "强 manifest 已经建好、payload 还没读"这一刻在真实系统里无法从外部精确命中，
// 而它恰好是 manifest ↔ payload 绑定的唯一窗口。为了让这条绑定有**可复现的**
// discrimination 证据，RunIncrementalBackup 在这个时刻调用一次这里注册的回调。
//
// 默认是空指针：产品的任何路径都不会注册它，生产行为不受影响。
void SetIncrementalManifestBuiltHookForTesting(void (*hook)(void* context),
                                               void* context);

// ---- 依赖感知的 retention ----
//
// Full 的 retention 可以放心删最旧的一份。有链之后不行：
//
//     F0 → Δ1 → Δ2 → Δ3        retain = 2
//     删掉 F0 与 Δ1，留下 Δ2、Δ3  →  Δ2/Δ3 全部不可恢复
//
// 这是**数据丢失**，不是"少留一份快照"。所以删除集合必须先过依赖检查。
//
// 第一版采用"保留最近 N 个 restore point，但保住它们需要的祖先"（设计文档
// 第 11 节的方案 B）：用户看到的仍然是"最近 N 个还原点"，而为了让这些点真的
// 能恢复，链上必须的祖先即使不算可见点也留下来，并在诊断里说明是
// dependency retained。
struct RetentionPlan {
  // 可见的 restore point（最近 retain_count 个）。
  std::vector<std::string> keep_visible;
  // 不在可见集合里、但被可见点依赖，因此必须保留的祖先。
  std::vector<std::string> keep_ancestors;
  // 可以删除的（最旧的在前）。调用方按这个顺序删。
  std::vector<std::string> remove;
  // 读不出依赖关系的快照（坏文件、或者根本不是本产品的快照）。
  //
  // 它们一律**不删**：读不出依赖就证明不了"删它不会断链"，而删除是不可逆的。
  // 但也不再往上走——连它自己都读不出来，它的祖先是谁无从得知。如实记下来，
  // 让调用方报出去，而不是替用户猜一个删除集合。
  std::vector<std::string> unreadable;
};

// candidates 按**最旧在前**给出（调用方原本的 retention 顺序）。
// 只有 candidates 里的文件会被放进 remove —— 链上不属于本计划管理的祖先
// （例如用户手工建的完整备份）一律不动，那是别人的东西。
bool PlanDependencyAwareRetention(
    const std::string& repository_directory,
    const std::vector<std::string>& candidates_oldest_first,
    std::size_t retain_count, RetentionPlan* plan, std::string* error_message);

// 一份快照的父快照文件名；完整归档返回空串。读不出来时返回 false。
bool SnapshotParentOf(const std::string& repository_directory,
                      const std::string& snapshot_file_name,
                      std::string* parent_file_name,
                      std::string* error_message);

// 一份快照**下面**已经有几个 delta（完整快照 = 0）。读不出来时返回 false。
//
// 恢复侧的上界是 kMaxDeltaChainDepth：含 64 个 delta 的链可以恢复，65 个不行。
// 写侧因此在接着父写 delta 之前先问一句"再挂一个会不会越界"——否则会产出一份
// 创建时成功、恢复时才失败的快照，那正是"先存进去、运行时才炸"。
bool SnapshotDeltaDepth(const std::string& repository_directory,
                        const std::string& snapshot_file_name,
                        std::size_t* depth, std::string* error_message);

// 从磁盘上的强 manifest 副文件读一份基线 manifest。
bool LoadSnapshotManifest(const std::string& repository_directory,
                          const std::string& snapshot_file_name,
                          std::vector<ManifestEntry>* entries,
                          std::string* error_message);

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_INCREMENTAL_BACKUP_H_
