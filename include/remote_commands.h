// include/remote_commands.h
//
// backupctl remote 子命令。
//
// 与其他子命令同构：解析参数 -> 调用共享核心 -> 翻译结果。
// 这里没有自己的 socket：CLI 与 Modern GUI 用的是同一个 RemoteArchiveClient。
//
//   ping     -> RemoteArchiveClient::Ping
//   register -> Register（口令来自 /dev/tty，或测试专用的环境变量）
//   login    -> Login（token 只在这个进程的内存里）
//   list     -> List
//   upload   -> UploadArchiveFile（可选 --repository 走
//               LoadVerifiedSnapshotIdentity 先证明归档身份）
//   download -> DownloadArchiveFile（唯一临时文件 + 校验 + 原子发布）
//   delete   -> Delete
//
// 命令行里没有 --password：口令只从终端读，或者由自动测试通过
// BACKUP_REMOTE_PASSWORD 提供（该变量只读不打印）。

#ifndef BACKUP_PROJECT_INCLUDE_REMOTE_COMMANDS_H_
#define BACKUP_PROJECT_INCLUDE_REMOTE_COMMANDS_H_

#include <string>
#include <vector>

#include "cli_app.h"

namespace backupproject {

// arguments 是去掉 "remote" 之后的参数（ping / register / login / list /
// upload / download / delete ...）。
int RunRemoteCommand(const CliContext& context,
                     const std::vector<std::string>& arguments);

// 供 PrintCliUsage 复用的用法片段。
void PrintRemoteUsage(std::ostream& output);

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_REMOTE_COMMANDS_H_
