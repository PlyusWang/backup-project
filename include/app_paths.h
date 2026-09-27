// app_paths.h
//
// Linux 上 GUI 与 CLI 共同使用的应用路径。
//
// 为什么需要它：Modern GUI 的配置文件路径来自
// QStandardPaths::AppConfigLocation，而 backupctl 是纯 C++（不含 Qt）。
// 如果 CLI 自己拼一个 "~/.config/backup-project/schedule.json"，
// 只要"印象里的路径"和 Qt 实际算出来的不一样，就会出现
// "GUI 存的计划 CLI 读不到"这种最难查的 bug。
//
// 因此这里把 Qt 在 Linux 上算 AppConfigLocation 的规则完整复刻一遍，
// 并且把应用身份（组织名 / 应用名）也放在这里——GUI 侧的
// setOrganizationName / setApplicationName 直接读这两个常量，
// 两边不可能各自漂移。
//
// 复刻的规则（已在 VM 上实测确认，不是凭印象）：
//
//   GenericConfigLocation = $XDG_CONFIG_HOME  若它非空且是绝对路径
//                         否则 $HOME/.config
//   AppConfigLocation     =
//   <GenericConfigLocation>/<organization>/<application>
//
// 实测：HOME=/home/pw-is-123、XDG_CONFIG_HOME 未设置时
//   QStandardPaths::AppConfigLocation
//     = /home/pw-is-123/.config/backup-project/backup-gui-modern
// 设置 XDG_CONFIG_HOME=/tmp/pr17-probe/xdg 后
//     = /tmp/pr17-probe/xdg/backup-project/backup-gui-modern
//
// 本模块只解析环境变量，不创建目录、不碰文件系统。

#ifndef BACKUP_PROJECT_INCLUDE_APP_PATHS_H_
#define BACKUP_PROJECT_INCLUDE_APP_PATHS_H_

#include <string>

namespace backupproject {

// 应用身份。GUI 侧必须用这两个常量调用
// QCoreApplication::setOrganizationName / setApplicationName，
// 否则 QStandardPaths 与本模块的结果会不一致。
inline constexpr const char* kAppOrganizationName = "backup-project";
inline constexpr const char* kAppApplicationName = "backup-gui-modern";

// 配置文件名。GUI 与 CLI 共用同一个 config.json。
inline constexpr const char* kAppConfigFileName = "config.json";
// 定时备份 store 的默认文件名。
inline constexpr const char* kAppScheduleFileName = "schedule.json";

// $XDG_CONFIG_HOME（绝对路径）或 $HOME/.config。两者都拿不到时返回空串。
std::string GenericConfigDirectory();

// 失败时写 error_message 并返回 false（典型原因：HOME 与 XDG_CONFIG_HOME
// 都不可用）。
bool AppConfigDirectory(std::string* directory, std::string* error_message);

// <AppConfigDirectory>/<file_name>。
bool AppConfigFilePath(const std::string& file_name, std::string* path,
                       std::string* error_message);

// 产品默认位置；拿不到时返回空串（调用方必须显式检查，不能当成相对路径用）。
std::string DefaultConfigFilePath();
std::string DefaultScheduleFilePath();

}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_APP_PATHS_H_
