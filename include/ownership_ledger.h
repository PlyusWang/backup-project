#ifndef BACKUP_PROJECT_INCLUDE_OWNERSHIP_LEDGER_H_
#define BACKUP_PROJECT_INCLUDE_OWNERSHIP_LEDGER_H_

#include <string>
#include <vector>

namespace backupproject {

struct OwnershipRecord {
  std::string snapshot_id;
  std::string sha256;
};

// 仓库身份 + 任务身份 -> 应用配置目录下的长期归属账本路径。
std::string OwnershipLedgerPath(const std::string& repository_id,
                                const std::string& task_identity);

// 追加一条记录。目录 0700、文件 0600、O_NOFOLLOW、flock 排他、fsync。
bool AppendOwnershipRecord(const std::string& ledger_path,
                           const OwnershipRecord& record,
                           std::string* error_message);

// 读取并严格校验。损坏、权限不对、符号链接、同 id 不同摘要都返回 false。
bool ReadOwnershipRecords(const std::string& ledger_path,
                          std::vector<OwnershipRecord>* records,
                          std::string* error_message);

#ifdef BACKUP_TEST_HOOKS
// 测试专用：在已读取 after_bytes 后一次性注入 errno。after_bytes<0 关闭。
void SetOwnershipLedgerReadFaultForTesting(int after_bytes, int error_number);
#endif

}  // namespace backupproject

#endif
