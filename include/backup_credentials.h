// include/backup_credentials.h
//
// 无人值守备份的加密凭据来源。
//
// 定时 / 实时备份没有交互界面，不能像手动备份那样在启动时向用户要口令。
// 支持本次进程会话解锁，以及用户自己维护的高级口令文件。
//
// 边界（每一条都在 ReadCredentialFile 里执行，不靠调用方自觉）：
//   * 必须是普通文件，不是符号链接、目录、FIFO 或设备；
//   * 必须是当前用户拥有（st_uid == geteuid）；
//   * 权限不允许组或其他用户有任何位（mode & 0077 == 0），且属主可读；
//   * 大小 1..4096 字节，内容不含 NUL；末尾最多一个换行 / CRLF 被去掉
//     （编辑器自动补的那个换行不该成为口令的一部分）；
//   * 失败原因里**只出现路径与原因**，绝不出现文件内容。
//
// 为什么不做"把密码加密后存进配置"：本项目没有平台密钥库可用，任何由程序
// 自己保管的对称密钥都只是把明文换了个位置。会话引用不持久化秘密；高级
// 文件的权限检查把"误把 0644 的文件当秘密"变成明确失败。

#ifndef BACKUP_PROJECT_INCLUDE_BACKUP_CREDENTIALS_H_
#define BACKUP_PROJECT_INCLUDE_BACKUP_CREDENTIALS_H_

#include <cstddef>
#include <string>

namespace backupproject {

inline constexpr std::size_t kMaxCredentialFileBytes = 4096;

// 配置只保存此命名空间中的引用；秘密只存在于当前进程内，退出后必须重新解锁。
inline constexpr const char* kSessionCredentialPrefix =
    "/@backup-project-session/";
bool IsSessionCredentialReference(const std::string& path);
bool SetSessionCredential(const std::string& reference,
                          const std::string& secret,
                          std::string* error_message);
void ClearSessionCredential(const std::string& reference);
bool SessionCredentialAvailable(const std::string& reference);

// 读取并通过全部安全检查后，*secret 是**去掉行尾换行**的口令原文。
// 失败时 *secret 保持空串，error_message 是可展示的英文原因。
bool ReadCredentialFile(const std::string& path, std::string* secret,
                        std::string* error_message);

// 只做检查、不返回内容：启用计划 / 实时任务前用它，避免把口令读进一个
// 根本不打算运行的进程。检查项与 ReadCredentialFile 完全相同。
bool ValidateCredentialFile(const std::string& path,
                            std::string* error_message);

// 口令文件路径的**结构**检查（非空、长度上限、无 NUL、是绝对路径）。
// 它不做文件系统探测，供 ValidateScheduleConfig / ValidateRealtimeConfig
// 这类"只看配置文本"的校验使用。
bool IsUsableCredentialPath(const std::string& path,
                            std::string* error_message);

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_BACKUP_CREDENTIALS_H_
