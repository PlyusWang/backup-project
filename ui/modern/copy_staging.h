// ui/modern/copy_staging.h
//
// 复制任务的受控暂存目录。目录只用于本地材料/下载中转，不属于备份仓库，
// 备份列表不会把它当成备份。
//
// 归属与清理规则（与 docs/crash-consistent-publishing.md 一致）：
//   * 目录由 mkdtemp 创建，0700；名字前缀只是线索，不是归属证明；
//   * 每个目录里有一个 .lease，写入固定的 BPSCOPY1 头和随机任务令牌；
//   * 活跃任务对 .lease 持有 flock 排他锁；
//   * 清理只处理“真实目录 + 当前用户 + 0700 + 合法 lease 头 + 能拿到锁”的
//     候选，并在整个删除过程持有锁和目录 fd；
//   * 路径被替换/重命名时用 (dev, ino) 复核，不删除替换后的对象。
#ifndef BACKUP_PROJECT_UI_MODERN_COPY_STAGING_H_
#define BACKUP_PROJECT_UI_MODERN_COPY_STAGING_H_

#include <string>

namespace backup_modern {

enum class CopyStagingKind { kUpload, kDownload };

struct CopyStagingCleanupReport {
  int removed = 0;
  int skipped_live = 0;
  int skipped_invalid = 0;
  int failed = 0;
};

// 一次复制任务的暂存目录与租约。析构时在仍持锁的情况下删除目录。
class CopyStagingLease {
 public:
  CopyStagingLease() = default;
  ~CopyStagingLease();
  CopyStagingLease(const CopyStagingLease&) = delete;
  CopyStagingLease& operator=(const CopyStagingLease&) = delete;

  bool Create(const std::string& repository, CopyStagingKind kind,
              std::string* error_message);
  const std::string& path() const { return path_; }
  bool active() const { return !path_.empty(); }

 private:
  std::string path_;
  int dir_fd_ = -1;
  int lease_fd_ = -1;
};

// 清理无主暂存目录。失败只累计到报告，不影响正式备份。
CopyStagingCleanupReport CleanupStaleCopyStaging(const std::string& repository);

}  // namespace backup_modern

#endif  // BACKUP_PROJECT_UI_MODERN_COPY_STAGING_H_
