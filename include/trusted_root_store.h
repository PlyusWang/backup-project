// include/trusted_root_store.h
//
// TrustedRootStore —— "我到底信哪几把根"的唯一出处（PR #23 Phase 1）。
//
// 设计要点：
//   * 存储是**一个向量**，不是"一把写死的根"：官方根要能轮换
//     （Root-A / Root-B 同时在线，老证书还能验，新证书已经用新根签），
//     所以从第一天起就按多根设计，而不是等到要换根时再改协议；
//   * 公钥来源只有两个：内置常量（官方云端，编译进二进制，不能被"在旁边
//     放个文件"篡改）或调用方显式加载的根文件（自托管 / 测试）；
//   * **空存储 = 什么都不信**。没有根、issuer_id 对不上、根被吊销、根在
//     签发时刻还没生效、验签失败 —— 全部失败，绝不"没验签也先连上"。
//
// 根文件格式（每行一个根，'#' 开头是注释）：
//     <root-id> <public-key> [not_before] [not_after] [active|revoked]
// 其中 public-key 接受 "ed25519:<64 位十六进制>" 或纯十六进制。
// 官方云端的 resources/security/official-root-ed25519.pub 就是这个格式，
// 仓库里只放公钥，私钥离线保存。

#ifndef BACKUP_PROJECT_INCLUDE_TRUSTED_ROOT_STORE_H_
#define BACKUP_PROJECT_INCLUDE_TRUSTED_ROOT_STORE_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "bpcert.h"

namespace backupproject {
namespace crypto {

struct TrustedRoot {
  std::string root_id;      // 与证书里的 issuer_id 匹配
  std::string public_key;   // 32 字节 Ed25519 公钥
  std::int64_t not_before = 0;  // 0 = 不限制
  std::int64_t not_after = 0;   // 0 = 不限制
  bool revoked = false;
};

class TrustedRootStore {
 public:
  TrustedRootStore() = default;

  // 加一个根。root_id 重复、公钥长度不对、root_id 不合法都拒绝。
  bool AddRoot(const TrustedRoot& root, std::string* error_message);

  bool empty() const { return roots_.empty(); }
  std::size_t size() const { return roots_.size(); }
  const std::vector<TrustedRoot>& roots() const { return roots_; }

  // 按 root_id 找根（找不到返回 nullptr）。
  const TrustedRoot* FindRoot(const std::string& root_id) const;

  // 完整信任决策。返回 kOk 表示"结构合法 + issuer 可信 + 签名有效"。
  // 失败时 *matched_root_id 只有在"确实命中了某个根"时才有意义，
  // *message 给出中文原因。
  Bpcert1Error VerifyCertificate(const std::string& raw_certificate,
                                 std::string* matched_root_id,
                                 std::string* message) const;

  // 从文本 / 文件加载。加载失败时 out 保持不变（不会留下半个存储）。
  static bool LoadFromText(const std::string& text, TrustedRootStore* out,
                           std::string* error_message);
  static bool LoadFromFile(const std::string& path, TrustedRootStore* out,
                           std::string* error_message);

  // 官方云端内置根。常量随二进制走，客户端不需要、也不应该去读任何
  // "旁边的文件"来决定信谁。
  static TrustedRootStore OfficialCloudStore();
  static const char* OfficialCloudRootId();

 private:
  std::vector<TrustedRoot> roots_;
};

}  // namespace crypto
}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_TRUSTED_ROOT_STORE_H_
