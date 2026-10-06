// incremental_delta.h
//
// 增量 delta 的磁盘格式（BKPINC1）。
//
// ---- 为什么要一个新的顶层格式 ----
//
// 完整备份继续用 v2 container（BKPCNT2），一个字节都不改。增量 delta 必须是
// **另一种顶层 magic**：如果 delta 复用 BKPCNT2，旧 reader 会把它当成一份完整
// 归档读进去，然后"成功"恢复出一个只包含变化部分的目录树——那是静默的错误
// 结果，比明确拒绝危险得多。所以 delta 用 BKPINC1，旧 reader 在 magic 这一层
// 就不认识它。
//
// ---- 布局 ----
//
//   offset  size  field
//   0       8     magic "BKPINC1\0"
//   8       2     format version (u16, little endian)，当前 1
//   10      2     fixed header size (u16)，当前 24
//   12      4     envelope length (u32)
//   16      8     payload length (u64)
//   24      N     envelope：规范化文本（自描述，见下）
//   24+N    M     payload：一份完整、自包含的 v2 container（BKPCNT2）
//
// payload 直接复用现有流水线：pack → compression → encryption 一行代码都没有
// 复制。第一版只允许 MyPack（见下），压缩与加密照旧可组合。
//
// ---- 自描述 ----
//
// 单独拿走一个 .bak 文件也必须能判断它是 delta、它的父亲是谁、它属于哪条链。
// envelope 因此携带（全部明文，Catalog 不需要密码就能读）：
//
//   format / snapshot_id / parent_file_name / parent_snapshot_id /
//   parent_manifest_digest / base_generation_id / source_identity /
//   filter_identity / strategy_identity / current_manifest_digest /
//   created / counts / tombstones / affected_directories / payload_sha256
//
// parent 同时绑定**文件名与 manifest digest**：只有文件名时，同名文件被替换
// 之后链会静默指向错误内容；有了 digest，替换必然被发现。
//
// ---- 明文 / 认证 / 加密的边界（说清楚，不假装）----
//
//   * payload（所有真实路径与内容）在容器里，受容器自己的 MAC 保护；选了加密
//     时它就是密文。
//   * envelope 是**明文**：Catalog 必须在没有密码的情况下知道"这是 delta、
//     父是谁、能不能恢复"。它刻意不写源目录绝对路径，只写 identity 摘要。
//   * envelope 自身没有独立的 MAC。篡改它不会得到"悄悄错恢复"：parent digest
//     与上一份快照记录的 digest 对不上时，解析链会直接拒绝。也就是说攻击者
//     能造成的后果是"链被拒绝"，不是"恢复到错误内容"。
//
// 本文件是纯 C++17：不依赖 Qt，也不依赖任何第三方库。

#ifndef BACKUP_PROJECT_INCLUDE_INCREMENTAL_DELTA_H_
#define BACKUP_PROJECT_INCLUDE_INCREMENTAL_DELTA_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "archive_entry.h"
#include "archive_pipeline.h"

namespace backupproject {

inline constexpr unsigned char kDeltaMagic[8] = {'B', 'K', 'P', 'I',
                                                 'N', 'C', '1', '\0'};
inline constexpr std::size_t kDeltaMagicSize = sizeof(kDeltaMagic);
inline constexpr std::uint16_t kDeltaFormatVersion = 1;
// magic + version + header_size + envelope_len + payload_len
inline constexpr std::size_t kDeltaFixedHeaderSize = 24;
inline constexpr std::size_t kMaxDeltaEnvelopeBytes = 8u * 1024u * 1024u;
inline constexpr std::size_t kMaxDeltaChainDepth = 64;
inline constexpr std::size_t kMaxDeltaTombstones = 2000000u;
// 一条 delta 最多承载多少个"新增/修改"条目。与 manifest 的上界同量级。
inline constexpr std::size_t kMaxDeltaEntries = 2000000u;

// delta 的自描述信封。字段全部是值类型。
struct DeltaEnvelope {
  std::uint32_t format_version = kDeltaFormatVersion;

  // 这份 delta 自己的身份：64 位十六进制，内容 = 规范化 envelope 去掉本字段
  // 之后的摘要。它让"同一个 snapshot 被引用两次"可以被识别出来。
  std::string snapshot_id;

  // 父快照：文件名（单组件）+ 它的 snapshot id + 它的 manifest digest。
  // 三者必须同时对上，链才成立。
  std::string parent_file_name;
  std::string parent_snapshot_id;
  std::string parent_manifest_digest;

  // 整条链共用的 generation 身份（Full baseline 的 snapshot id）。
  std::string base_generation_id;

  // 源 / 规则 / 策略的身份摘要。任何一个变了都不允许继续沿用旧链，
  // 必须重新建立 Full baseline。刻意存摘要而不是原文：envelope 是明文。
  std::string source_identity;
  std::string filter_identity;
  std::string strategy_identity;

  // 应用完这份 delta 之后，源树应该等于的 manifest digest。
  std::string current_manifest_digest;

  std::int64_t created_unix_seconds = 0;

  // 变化计数（added / modified / metadata_changed / removed）。
  // removed 必须等于 tombstones.size()。
  std::uint64_t added = 0;
  std::uint64_t modified = 0;
  std::uint64_t metadata_changed = 0;
  std::uint64_t removed = 0;

  // 被删除的 archive_path。apply 时按深度优先（深的先删）执行。
  std::vector<std::string> tombstones;

  // 需要在新 restore point 上校正 metadata 的目录（含 "."）。
  std::vector<std::string> affected_directories;

  // payload 字节的 SHA-256（十六进制）。它是"envelope 与 payload 属于彼此"
  // 的那条纽带：写侧算，读侧验。
  std::string payload_sha256;
};

// 计算身份摘要。四者都是 64 位十六进制。
std::string SourceIdentityDigest(const std::string& source_path,
                                 const std::string& repository_identity);
std::string FilterIdentityDigest(const std::vector<std::string>& include_rules,
                                 const std::vector<std::string>& exclude_rules);
std::string StrategyIdentityDigest(PackMethod pack,
                                   CompressionMethod compression,
                                   EncryptionMethod encryption);

// 规范化序列化 / 严格解析。解析要求每个键出现且只出现一次、数值范围合法、
// digest 形状合法、removed == tombstones.size()。
std::string SerializeDeltaEnvelope(const DeltaEnvelope& envelope);
bool ParseDeltaEnvelope(const std::string& text, DeltaEnvelope* envelope,
                        std::string* error_message);

// snapshot_id 由 envelope 自身内容决定：写侧在序列化之前调用它填好字段。
std::string ComputeDeltaSnapshotId(const DeltaEnvelope& envelope);

// 写出 delta。
//
// changed_entries 走完整 pipeline（pack → compression → encryption）生成内层
// v2 container，再包进 BKPINC1 信封。第一版只接受 PackMethod::kMyPack：
// USTAR 无法表达 tombstone / parent dependency，与其"伪装支持"不如明确拒绝，
// 由调用方在共享校验里报同一句话。
//
// 条目表与备份流水线的约定完全一致：**第一条必须是源根目录**
// （archive_path == "."，type == kDirectory）。delta 也要带上根目录的
// metadata， 否则应用完 delta
// 之后根目录自身的时间戳与权限就没有人负责了。表不满足这条
// 约定时明确失败，不替调用方伪造一条根记录。
//
// 原子发布：先在目标目录写唯一临时文件，fsync，再 rename；失败删掉半成品。
bool WriteDeltaFile(const std::string& delta_file,
                    const DeltaEnvelope& envelope,
                    const std::vector<ArchiveEntry>& changed_entries,
                    const BackupOptions& options, std::string* error_message);

// 只读信封：不需要密码、不读 payload。
bool ReadDeltaEnvelope(const std::string& delta_file, DeltaEnvelope* envelope,
                       std::string* error_message);

// 把内层 container 原样抽到一个文件（restore 用）。
bool ExtractDeltaPayload(const std::string& delta_file,
                         const std::string& container_file,
                         std::string* error_message);

// 读内层 container 的 header：不需要密码、不抽取 payload、不触碰内容。
//
// 恢复路径用它确认"这份 delta 的 payload 没有被加密"。v1 的合同是
// Incremental + encryption 明确拒绝，创建路径由共享校验拦住；读侧要有同一条
// 合同，因为旧版本写出来的加密 delta 的**明文外层信封**（parent / tombstones）
// 并不受内层 HMAC 覆盖，接受它等于接受一组未经认证的路径指令。
bool InspectDeltaPayloadHeader(const std::string& delta_file,
                               ContainerHeader* header,
                               std::string* error_message);

// ---- 不可信信封字段的边界 ----
//
// parent_file_name 与 tombstones 都来自**不可信归档**：它们是字符串，却会被用在
// 真实文件系统上。边界放在格式层，因为每一个读者（Catalog 列表、恢复、
// retention）都要先解析信封；写侧过的也是同一对函数。
//
// parent_file_name：非空、单组件、不是 "." / ".."、不含 '/' 或 '\\' 或 NUL，
//   且以 .bak 结尾——与 BackupCatalog 管理的备份文件名同一条边界。
// tombstone：先复用 IsValidArchivePath，再额外拒绝 "."（源根永远不能被
// tombstone
//   删掉）。绝对路径、'..' 组件、空组件、反斜杠、盘符、结尾 '/'、NUL 全部
//   在 IsValidArchivePath 里就已经被拒。
bool IsValidDeltaParentFileName(const std::string& name,
                                std::string* error_message);
bool IsValidDeltaTombstone(const std::string& path, std::string* error_message);

// ---- 快照身份 ----
//
// delta 的 snapshot_id 是"信封内容的摘要"，自校验。完整快照没有信封，所以它的
// 身份从容器自己的 payload 摘要派生：
//
//     FullSnapshotId = SHA-256("BPFULL1\n" + <container payload_sha256 hex>)
//
// 这样父绑定就不只是"文件名"：同名文件被换成另一份归档时，父身份立刻对不上。
// 它不需要密码（容器 header 本来就是"未知密码也能读"的），也不改 v2 格式。
//
// 注意它读的是 header 里的**声明值**：这是"廉价身份"，只适合展示与快速分类，
// 不足以支撑信任判断。任何 baseline / parent / 恢复链成员的身份都必须走
// LoadVerifiedSnapshotIdentity（先证明实际 payload 字节与声明一致）。
bool FullSnapshotId(const std::string& container_file, std::string* id,
                    std::string* error_message);

// 任意快照文件（delta 或完整归档）的**声明**身份。kUnknown 时返回 false。
// 与 FullSnapshotId 同样的边界：展示用，信任判断不用它。
bool SnapshotIdOfFile(const std::string& path, std::string* id,
                      std::string* error_message);

// 这段字节是不是以 BKPINC1 开头。
bool LooksLikeDelta(const unsigned char* data, std::size_t size);

// 读文件头判断"这是 delta 还是完整归档"。
//   kDelta / kContainer / kUnknown
enum class SnapshotFileKind { kUnknown, kDelta, kContainer };
SnapshotFileKind ClassifySnapshotFile(const std::string& path,
                                      std::string* error_message);

// 校验 payload：布局、**实际 payload 字节的
// SHA-256（就地流式，不落临时文件）**、 以及内层 container 的 header 与 payload
// 长度自洽。不需要密码的部分都在这里； HMAC 认证仍然由容器自己的恢复路径负责。
bool VerifyDeltaPayload(const std::string& delta_file,
                        std::string* error_message);

// 验证一份**完整快照**的实际 payload 字节（复用 v2 容器自己的完整性规则：
// header 可解码、文件长度 == 160 + payload_size、实际 payload 区的 SHA-256 ==
// header 声明的 payload_sha256），成功时把那个摘要以十六进制带出来。
//
// 它是 "actual archive-byte identity" 在完整快照这一侧的唯一入口：
// payload_sha256_hex 是**验证过的实际字节**的摘要，不是 header 里的声明值
// （两者相等才算成功）。加密容器同样适用——这一层不需要密码，HMAC 仍归恢复路径。
bool VerifyFullSnapshotPayload(const std::string& container_file,
                               std::string* payload_sha256_hex,
                               std::string* error_message);

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_INCREMENTAL_DELTA_H_
