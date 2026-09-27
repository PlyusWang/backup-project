// tree_scanner.h
//
// 统一的源目录树扫描：MyPack v2 / baseline USTAR / Fast USTAR 三种打包策略
// 消费同一份 std::vector<ArchiveEntry>，谁也不许自己发明一套遍历。
//
// 放在 src/core/ 而不是某个 pack 后端里，是因为它承担的是语义而不是格式：
//   * lstat（不 follow 软链接）
//   * 类型判定（目录 / 普通文件 / 软链接 / 硬链接 / FIFO / 字符设备 / 块设备）
//   * 完整 POSIX metadata（07777 mode、uid/gid、mtime 秒+纳秒、设备号）
//   * hardlink 检测（同一 (st_dev, st_ino) 只存一份 payload）
//   * Filter 决策与目录剪枝
//   * socket 的明确失败语义
// 这些规则只有一份，三种 pack 的输出才可能一致。

#ifndef BACKUP_PROJECT_INCLUDE_TREE_SCANNER_H_
#define BACKUP_PROJECT_INCLUDE_TREE_SCANNER_H_

#include <string>
#include <vector>

#include "archive_entry.h"
#include "filter.h"
#include "source_tree_walker.h"

namespace backupproject {

// 扫描结果保证是 DFS 先序：第一条永远是 source root 自己（archive_path == "."，
// 类型是目录），父目录一定排在孩子前面，同级按名字升序。顺序固定下来，
// 同一棵源树每次产出的条目序列就是确定的。
//
// filter 为 nullptr 表示没有规则（等价于空 Filter）。
//
// 失败语义（整次扫描失败，不产出半个结果）：
//   * lstat / opendir 出错；
//   * 某条归档路径超过 kMaxArchivePathLength；
//   * 出现 socket，而且 Filter 没有明确排除它——socket 不作为可恢复备份，
//     静默跳过、跟随它、把它当普通文件复制这三种做法都会让"备份成功"变成假话。
bool ScanSourceTree(const std::string& source_directory, const Filter* filter,
                    std::vector<ArchiveEntry>* entries,
                    std::string* error_message);

// 同一件事，但允许注入 filesystem 失败。
//
// 只给测试用：生产调用方一律传 nullptr（也就是上面那个重载）。存在的理由是
// "备份遇到 opendir/readdir/lstat 失败也必须 fail closed" 这条语义需要被测到，
// 而权限类失败在 root / CAP_DAC_OVERRIDE 下造不出来——用 chmod 000 拼出来的
// 用例会在那种环境里静默变成"通过"。见 source_tree_walker.h 里的
// SourceWalkFaults：它只能把一次 syscall 变成失败，不能伪造文件系统内容。
bool ScanSourceTree(const std::string& source_directory, const Filter* filter,
                    std::vector<ArchiveEntry>* entries,
                    std::string* error_message, const SourceWalkFaults* faults);

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_TREE_SCANNER_H_
