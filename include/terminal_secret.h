// terminal_secret.h
//
// 最小 Linux TTY 秘密读取器。
//
// 为什么必须有它：手动加密备份 / 恢复需要一个密码，而"密码从哪来"这件事
// 只有三种做法，前两种是错的：
//
//   --password xxx       密码进 argv。ps、/proc/<pid>/cmdline、shell history
//                        三处立刻泄漏，而且备份工具的 argv 经常被写进日志。
//   环境变量默认密码      看起来好一点，实际同样会进 /proc/<pid>/environ，
//                        还会被"顺手 export 一下"固化进 .bashrc。
//   交互式 TTY 读取       密码只在本进程内存里活着：不写 config、不写日志、
//                        不进归档、不回显、用完即弃。  <- 本模块
//
// 另一条硬规则：**不是 TTY 就明确失败**。绝不偷偷从 stdin pipe 读秘密——
// 那会让 "cat password.txt | backupctl backup ... --encryption aes..." 变成
// 一个看起来能用的用法，然后密码就被写进了别人的 shell history 和管道日志。
//
// 本文件是纯 C++17 + POSIX：不依赖 Qt，也不依赖 readline。

#ifndef BACKUP_PROJECT_INCLUDE_TERMINAL_SECRET_H_
#define BACKUP_PROJECT_INCLUDE_TERMINAL_SECRET_H_

#include <cstddef>
#include <string>

namespace backupproject {

inline constexpr std::size_t kMaxSecretBytes = 4096;

// 打开进程的控制终端 /dev/tty。失败返回 -1 并写 error_message。
// 打开的是控制终端而不是 stdin：这样即使 stdin 被重定向，交互式提示照样能用；
// 反过来，stdin 是管道时我们就真的拿不到终端，必须失败。
int OpenControllingTerminal(std::string* error_message);

// 从已经打开的 fd 读一行秘密。
//
//   require_tty = true  -> fd 必须是终端，否则明确失败（产品路径）；
//   require_tty = false -> 允许非终端 fd（只给单元测试用 PTY / 管道注入）。
//
// 关闭 ECHO、保留 ICANON（行编辑交给终端驱动，退格键照常可用）、
// Ctrl+C 与 Ctrl+D 都会先把终端模式恢复再退出。
bool ReadSecretLine(int fd, bool require_tty, const std::string& prompt,
                    std::string* secret, std::string* error_message);

// 产品入口：打开 /dev/tty 读一次。用完关闭 fd。
bool ReadSecretFromTerminal(const std::string& prompt, std::string* secret,
                            std::string* error_message);

// 产品入口：备份用。问两次并要求一致；不一致明确失败，绝不"以第一次为准"。
bool ReadSecretFromTerminalTwice(const std::string& prompt,
                                 const std::string& confirm_prompt,
                                 std::string* secret,
                                 std::string* error_message);

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_TERMINAL_SECRET_H_
