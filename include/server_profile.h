// include/server_profile.h
//
// ServerProfile —— "要连哪台服务器、怎么确认它是它"的一份完整描述（PR #23）。
//
// 三件事在这里合流：
//   * 官方云端：**编译进二进制**的唯一一份 profile，用户侧零配置。地址、端口、
//     期望的 server_id、信任哪把根，全都不需要用户知道，也不允许在运行时被
//     旁边的一个文件改掉；
//   * 自托管：.bpserver 文件（文本 key=value），用户可以自己写、可以随部署
//     一起发给同事；
//   * 三种身份方式（identity）：签名身份（证书，BPSEC2）/ 手工指纹（pin，
//     BPSEC1）/ SSH 隧道（在隧道里再用 pin）。
//
// 为什么要有这一层而不是到处传 host/port/pin：模式一旦分散在界面、CLI、
// 配置文件三处，就一定会出现"界面以为在用证书、CLI 却还在比对指纹"这种
// 半配置状态。这里把"模式 + 该模式需要的全部字段"绑成一个结构，并用
// ToRemoteEndpoint 做**唯一**的翻译点。

#ifndef BACKUP_PROJECT_INCLUDE_SERVER_PROFILE_H_
#define BACKUP_PROJECT_INCLUDE_SERVER_PROFILE_H_

#include <cstdint>
#include <string>

#include "remote_backup_client.h"

namespace backupproject {
namespace net {

// 官方云端的编译期常量。
//
// 主机名来自部署侧的事实，推导命令记录在这里，改的时候要一起改：
//     ssh -G aliyun-ecs | awk '/^hostname /{print $2}'   -> 8.130.9.200
// （2026-10-05 核对）。客户端把它编进二进制，而不是去读 ~/.ssh/config：
// 官方云端的用户不应该需要任何本地配置，更不应该因为本机 ssh 配置不同而
// 连到别的地方去。
inline constexpr const char* kOfficialCloudDisplayName = "Backup Project Cloud";
inline constexpr const char* kOfficialCloudHost = "8.130.9.200";
inline constexpr std::uint16_t kOfficialCloudPort = 18765;
inline constexpr const char* kOfficialCloudServerId =
    "backup-project-cloud-production";

// 身份方式。
inline constexpr const char* kIdentityCertificate = "certificate";
inline constexpr const char* kIdentityPin = "pin";
inline constexpr const char* kIdentitySsh = "ssh";

struct ServerProfile {
  std::string display_name;
  std::string host;
  std::uint16_t port = kOfficialCloudPort;
  // certificate / pin / ssh
  std::string identity = kIdentityCertificate;
  // identity == certificate：证书里必须出现的 server_id。
  std::string expected_server_id;
  // identity == certificate：可信根文件；**空 = 用内置官方根**。
  std::string trusted_roots_file;
  // identity == pin / ssh：手工指纹（"sha256:<64 位十六进制>"）。
  std::string server_key_pin;
  // identity == ssh：ssh 目标（user@host），隧道由调用方建立。
  std::string ssh_target;
  // 是不是内置的官方云端 profile（用于界面文案与"不许改"的判断）。
  bool official_cloud = false;
};

// 官方云端 profile：display_name = "Backup Project Cloud"、
// identity = certificate、expected_server_id = 上面那个常量、
// trusted_roots_file 为空（= 内置官方根）。用户侧不需要任何输入。
const ServerProfile& OfficialCloudProfile();

// .bpserver 文本格式（'#' 注释、key = value、未知键一律报错）：
//     display_name = 我的服务器
//     host = 192.168.1.10
//     port = 18765
//     identity = certificate | pin | ssh
//     expected_server_id = my-server          # certificate 必填
//     trusted_roots = /path/root.pub          # certificate；留空 = 内置官方根
//     server_key_pin = sha256:<64 位十六进制>  # pin / ssh 必填
//     ssh_target = user@host                   # ssh 必填
bool ParseBpserverProfile(const std::string& text, ServerProfile* out,
                          std::string* error_message);
bool LoadBpserverProfileFile(const std::string& path, ServerProfile* out,
                             std::string* error_message);
// 反向输出（用于生成模板 / 展示当前配置）。
std::string FormatBpserverProfile(const ServerProfile& profile);

// profile -> 网络层端点。**唯一**的翻译点，三种身份方式都在这里决定
// identity_mode / expected_server_id / trusted_roots_file / server_key_pin
// 怎么落到 RemoteEndpoint 上。ssh 方式只填端点与指纹，隧道由调用方建立。
bool ToRemoteEndpoint(const ServerProfile& profile, RemoteEndpoint* out,
                      std::string* error_message);

}  // namespace net
}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_SERVER_PROFILE_H_
