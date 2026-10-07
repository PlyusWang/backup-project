// include/remote_incremental.h
//
// 远端增量备份与恢复的**产品闭环**（客户端侧）。
//
// 这一层不重新实现任何增量算法：变化的判断、delta 的生成、链的校验、恢复的
// 应用，全部交给既有的本地增量核心（include/incremental_backup.h /
// incremental_restore.h）。它只补三件远端才需要的事：
//
//   1. 本地"已验证缓存"：把远端的三件套（.bak / .manifest / .identity）下载、
//      逐字节验证、解包进一个本地目录，让增量引擎仍然面对它熟悉的"本地仓库"；
//   2. 链关系：把远端元数据里的 parent/generation 翻译成引擎要的
//      baseline_snapshot_name，并把新快照的 kind/parent/lineage 声明给服务端；
//   3. 材料打包（BPSNAP1，见 include/snapshot_bundle.h）。
//
// 三条硬规则（每一条都有对应的自动化测试）：
//
//   * **服务端元数据只用来定位，不用来信任。** 下载到的每一个字节都要过
//     SHA-256（下载路径 + 材料包成员 + 引擎自己的 payload 验证），解包之后
//     还要用 LoadVerifiedSnapshotIdentity 确认"这份文件真的就是那个
//     snapshot_id"。 服务端说"这是 R2 的父"不算数，归档自己的信封说了才算。
//   * **缓存里没有凭据。** 不存口令、不存 token、不存服务端私钥。缓存目录里
//     只有三件套本身，删掉它最多让下一次备份重新下载一遍链。
//   * **链不能从中间断开。** 删除由服务端做依赖检查（还有后代就不许删），
//     客户端这边则保证"要续链就先把父材料验证到位，否则就重建完整基线"。

#ifndef BACKUP_PROJECT_INCLUDE_REMOTE_INCREMENTAL_H_
#define BACKUP_PROJECT_INCLUDE_REMOTE_INCREMENTAL_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "archive_pipeline.h"
#include "file_io.h"
#include "filter.h"
#include "remote_backup_client.h"

namespace backupproject {
namespace net {

// 本地已验证缓存的布局与逻辑身份。
//
//   <root>/<指纹前 16 位>/<用户名>/        三件套（引擎眼中的"仓库目录"）
//
// repository_identity 是**跨机器稳定**的逻辑身份（"remote:<指纹>:<用户名>"），
// 不是本机路径：否则同一份数据从另一台机器继续增量时会被判成另一条链，
// 白白重建一次完整基线。这也是"新机器 bootstrap"能成立的前提。
struct RemoteCacheLayout {
  std::string root_directory;
  std::string server_fingerprint;
  std::string username;
  std::string cache_directory;
  std::string repository_identity;
};

// 建出缓存目录并算好逻辑身份。root_directory 为空时用应用配置目录下的
// remote-cache（与 schedule.json / realtime.json 同一处）。
bool PrepareRemoteCache(const std::string& root_directory,
                        const std::string& server_fingerprint,
                        const std::string& username, RemoteCacheLayout* layout,
                        std::string* error_message);

// 这条链的标识（64 个小写十六进制字符）：与增量引擎的 source identity 用
// 同一套输入（源路径 + 逻辑仓库身份），因此同一份数据在同一账户下只有一条链。
std::string RemoteLineageId(const RemoteCacheLayout& layout,
                            const std::string& source_directory);

// 只读地把远端链解析出来：base 在前、目标在最后。会校验 kind 与 generation
// 的自洽性（full 必须是根且 generation 0，每一跳恰好 +1），不自洽就报错。
bool ResolveRemoteChain(const std::vector<RemoteSnapshotInfo>& snapshots,
                        const std::string& target_snapshot_id,
                        std::vector<RemoteSnapshotInfo>* chain,
                        std::string* error_message);

// 找一个尚未建立起链的空目录里可用的远端 head：同一条 lineage 里没有被
// 任何其他快照当作父引用的那一个。多个候选时取创建时间最新的（并列时取
// generation 更大、id 更小的）。没有候选时返回 false 并给出原因（不是错误）。
bool FindRemoteLineageHead(const std::vector<RemoteSnapshotInfo>& snapshots,
                           const std::string& lineage, RemoteSnapshotInfo* head,
                           std::string* reason);

struct RemoteBackupRequest {
  RemoteArchiveClient* client = nullptr;
  RemoteCacheLayout cache;
  std::string source_directory;
  std::vector<std::string> include_rules;
  std::vector<std::string> exclude_rules;
  Filter filter;
  BackupOptions options;
  // true = 允许续链（有远端 head 就做增量）；false = 强制新建一份完整基线。
  bool allow_incremental = true;
  std::string display_name;
  RemoteProgressCallback progress;
};

struct RemoteBackupOutcome {
  // 这次实际产出的类型：true = delta，false = 完整基线。
  bool produced_delta = false;
  // 引擎判断无法续链（身份不符、链太深、缓存不可信……），于是重建了完整基线。
  bool rebuilt_full_baseline = false;
  bool no_changes = false;
  std::string snapshot_id;
  std::string parent_snapshot_id;
  std::uint64_t generation = 0;
  std::string archive_name;
  // 真正发出去的字节（材料包大小）与"如果每次都传完整链根"的对比值。
  std::uint64_t uploaded_bytes = 0;
  std::uint64_t chain_root_bytes = 0;
  std::string baseline_reason;
};

bool RunRemoteBackup(const RemoteBackupRequest& request,
                     RemoteBackupOutcome* outcome, std::string* error_message);

struct RemoteRestoreOutcome {
  std::string archive_name;
  std::uint64_t chain_length = 0;
  std::uint64_t delta_count = 0;
  std::uint64_t downloaded_bytes = 0;
  std::uint64_t reused_bytes = 0;
  std::uint64_t restored_entries = 0;
};

bool RunRemoteRestore(RemoteArchiveClient* client,
                      const RemoteCacheLayout& cache,
                      const std::string& snapshot_id,
                      const std::string& destination_directory,
                      const RestoreOptions& restore_options,
                      RemoteRestoreOutcome* outcome,
                      std::string* error_message);

// ---- 原始归档的"单独恢复" ----
//
// 远端对象分两类，恢复机制**不是**同一套：
//
//   * 产品级备份链成员（lineage 非空：完整基线 + 增量）——走
//     RunRemoteRestore：解析依赖链、把整条链的材料取回来、逐跳应用；
//   * 原始归档（lineage 为空：用户把本机的一个 .bak 直接传上去的旧式条目）——
//     没有链、没有父、没有 .manifest / .identity 副文件，所以**不能**走链恢复。
//     但如果它本身是这个软件能独立恢复的归档，就可以"下载下来按本地格式恢复"。
//
// 这个函数就是第二条路：下载 -> 校验 -> 交给**既有的本地恢复核心**。它不解析
// MyPack/USTAR、不解密、不解压、不自己写文件：那些全部在 BackupEngine /
// RunRestorePipeline / ArchiveReader 里，与 backupctl restore、GUI 的本地恢复
// 是同一份实现。
//
// 硬性行为（都有对应的自动化测试）：
//
//   * 按**内容**判断格式，不看文件名、不看扩展名：随机文件即使叫 .bak 也必须
//     失败，并且明确说"不是受支持的备份归档"；
//   * 单独的 delta 一定失败，并且说清"它属于某条链、不能脱离依赖链单独恢复"；
//   * 加密归档没有密码时不尝试绕过：明确要求密码（password_required），拿到
//     密码之后用**同一份已经下载并校验过的字节**重试，不重新下载；
//   * 下载下来的字节必须先通过服务端声明的 SHA-256（由 DownloadArchiveFile
//     完成），任何失败都只删掉自己的临时文件，不在目标目录留下半成品
//     （原子发布由既有的恢复流水线保证）。
//
// 失败原因用**稳定的英文前缀**区分（界面文案在 controller 里，core 不把内部
// 错误码当用户文案）：
//
//   raw restore: not a supported archive  按内容不是任何已知归档格式
//   raw restore: corrupted archive 认得出是我们自己的容器，但字节与它自己
//                                         的声明对不上（含 payload_sha256
//                                         不符）
//   raw restore: unsupported version      认得出容器，但版本号这个版本不支持
//   raw restore: delta needs its chain    单独一个增量
//   raw restore: needs a password         加密归档 + 没给密码
//   raw restore: authentication failed    HMAC 没过：密码错**或**容器头被改动
//   raw restore: destination rejected     目标目录不符合本地恢复的契约
//   raw restore: download failed          下载 / SHA-256 校验失败
//   raw restore: 按本地格式恢复失败        其它本地恢复失败
//
// 关于"密码错"与"归档损坏"能不能分开（界面文案必须如实，不许猜）：
// 容器头里的 payload_sha256 覆盖**原始 payload 字节**，而且在 HMAC 之前就比对
// （见 AuthenticatePayload），所以
//
//   * payload 区字节被改动 / 文件被截断 / 头里声明的长度与文件长度不符
//     -> payload_sha256（或长度规则）先失败，这与密码无关，可以如实说"已损坏"；
//   * 密码错，或者**容器头本身**（salt / iv / sizes / auth_tag）被改动
//     -> HMAC 失败，两者在密码学上不可区分（tag 由 PBKDF2(密码, salt) 派生）。
//
// 因此 raw-auth 那一类的界面文案是"恢复密码错误，或备份完整性校验失败"，
// 不宣称"密码一定错"。
struct RemoteRawRestoreRequest {
  RemoteArchiveClient* client = nullptr;
  RemoteCacheLayout cache;
  std::string snapshot_id;
  // 服务端登记的显示名：只用来给下载下来的临时文件起一个看得懂的名字。
  // 它是**不可信输入**（可能含 '/'、控制字符），会被降成单组件文件名；
  // 留空就用 snapshot id。格式判断与它无关。
  std::string display_name;
  std::string destination_directory;
  RestoreOptions restore_options;
  RemoteProgressCallback progress;
};

struct RemoteRawRestoreOutcome {
  // 下载并校验通过的那份归档：名字、字节数与**实际字节**的 SHA-256。
  std::string archive_name;
  std::uint64_t downloaded_bytes = 0;
  std::string verified_sha256;
  // 按内容识别出来的格式（"v2-container" / "legacy-v0.1"）。
  std::string archive_format;
  std::uint64_t restored_entries = 0;
  // 归档是加密的、而调用方没给密码：界面据此提示"请输入恢复密码"。
  bool password_required = false;
};

// 一次"原始归档恢复"交互的会话：**下载一次**，可以用不同密码反复重试。
//
// 为什么需要它：如果对话框一开始就摆一个"可选密码"，等于把内部不确定性泄漏到
// 界面上（大多数归档并没有加密）。现在的顺序是：先只问目标目录 -> 下载 + 校验 +
// 按内容识别 -> 只有 core 明确说"这份归档是加密的、需要密码"时才向用户要密码
// -> 用**同一份已经下载并校验过的字节**再恢复一次。
//
// 生命周期：Prepare() 成功之后工作目录一直留着，直到 Run() 成功、Abandon() 被
// 调用，或者对象析构——析构一定会删掉整个工作目录（含那份下载下来的归档）。
// 这个对象不是线程安全的：同一时刻只由一个线程使用。一次交互里它会在
// "后台线程 -> 主线程 -> 后台线程"之间交接所有权，交接点都有 happens-before
// 边（QtConcurrent 的 future 结果返回），两个线程不会同时用它。
class RemoteRawRestoreSession {
 public:
  RemoteRawRestoreSession() = default;
  ~RemoteRawRestoreSession();
  RemoteRawRestoreSession(const RemoteRawRestoreSession&) = delete;
  RemoteRawRestoreSession& operator=(const RemoteRawRestoreSession&) = delete;

  // 下载 + 服务端声明的 SHA-256 校验 + 按内容识别格式。**不**写目标目录。
  // 失败（不是归档 / 损坏 / 版本不支持 / 单独 delta / 下载校验失败）时返回
  // false，并且立刻释放工作目录。
  bool Prepare(const RemoteRawRestoreRequest& request,
               RemoteRawRestoreOutcome* outcome, std::string* error_message);

  // 用 Prepare 留下的那份字节恢复；password 为空表示"没给密码"。
  // 返回 false 且 outcome->password_required 为 true 时**会话仍然有效**：
  // 调用方拿到密码之后再 Run 一次即可，不需要重新下载。
  bool Run(const std::string& password, RemoteRawRestoreOutcome* outcome,
           std::string* error_message);

  // 放弃这次交互：立刻删掉工作目录（幂等）。取消 / 成功 / 致命失败都走它。
  void Abandon();
  bool prepared() const { return prepared_; }
  // 真正的远端下载次数（Prepare 里那次算 1 次）。界面自检据此证明
  // "错密码 -> 正确密码"期间没有重复下载。
  int download_count() const { return download_count_; }
  // 下载下来那份归档的绝对路径。只给自检用：比较 inode 就能证明没有重新下载。
  const std::string& archive_path_for_test() const { return archive_path_; }
  // 这次交互的目标目录（准备阶段固化）。界面在"输入密码"那一段回显它，
  // 这样用户不必、也不能重新选一次位置。
  const std::string& destination_directory() const {
    return destination_directory_;
  }

 private:
  TempDirectoryGuard workspace_;
  std::string archive_path_;
  std::string archive_leaf_;
  std::uint64_t downloaded_bytes_ = 0;
  std::string verified_sha256_;
  std::string destination_directory_;
  RestoreOptions restore_options_;
  std::string password_hint_;
  bool prepared_ = false;
  bool requires_password_ = false;
  bool legacy_v01_ = false;
  int download_count_ = 0;
};

// 一次性入口（下载 + 恢复，不支持"再输一次密码"）：Qt 无关的健壮性回归
// 用它。需要交互式重试的调用方（GUI）用上面的会话；产品 CLI 没有这条
// 路径。
bool RunRemoteRawRestore(const RemoteRawRestoreRequest& request,
                         RemoteRawRestoreOutcome* outcome,
                         std::string* error_message);

}  // namespace net
}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_REMOTE_INCREMENTAL_H_
