// include/remote_backup_client.h
//
// PR #20：远程备份客户端。
//
// **CLI 与 Modern GUI 共用这一个客户端**：不存在"CLI 一套 socket、GUI 再写
// 一套 socket"的结构。它只负责：
//
//   连接 / 帧收发 / 会话（token 只在内存里）/
//   流式上传（读一个块发一个块）/ 流式下载（先写 .part，校验后原子改名）/
//   列表 / 删除
//
// 它**不解析归档内容**：客户端送出去的是本地已经生成好的 .bak 字节，
// 归档格式的语义属于 BackupEngine，网络层只认"名字 + 长度 + SHA-256"。
//
// 失败约定：所有方法返回 false 时 *error_message 里有一句能给人看的原因，
// 但里面**不含** token、口令或服务端内部路径。

#ifndef BACKUP_PROJECT_INCLUDE_REMOTE_BACKUP_CLIENT_H_
#define BACKUP_PROJECT_INCLUDE_REMOTE_BACKUP_CLIENT_H_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "network_protocol.h"

namespace backupproject {
namespace net {

inline constexpr const char* kDefaultRemoteHost = "127.0.0.1";
inline constexpr std::uint16_t kDefaultRemotePort = 18765;

struct RemoteEndpoint {
  std::string host = kDefaultRemoteHost;
  std::uint16_t port = kDefaultRemotePort;
  int timeout_seconds = 60;
};

struct RemoteSnapshotInfo {
  std::string snapshot_id;
  std::string display_name;
  std::uint64_t size_bytes = 0;
  std::string sha256;
  std::uint64_t created_at = 0;
};

// 进度回调。CLI 用它打印进度行，GUI 用它更新进度条；
// 两者共用同一个回调，不需要各自再实现一遍传输逻辑。
struct RemoteTransferProgress {
  std::string phase;  // "upload" 或 "download"
  std::uint64_t bytes_done = 0;
  std::uint64_t bytes_total = 0;
};

using RemoteProgressCallback =
    std::function<void(const RemoteTransferProgress&)>;

// 把协议状态码翻成能给人看的一句话（中文，不带内部细节）。
std::string RemoteStatusMessage(std::uint32_t status);

class RemoteArchiveClient {
 public:
  RemoteArchiveClient();
  ~RemoteArchiveClient();

  RemoteArchiveClient(const RemoteArchiveClient&) = delete;
  RemoteArchiveClient& operator=(const RemoteArchiveClient&) = delete;

  bool Connect(const RemoteEndpoint& endpoint, std::string* error_message);
  void Disconnect();
  bool connected() const { return fd_ >= 0; }
  bool authenticated() const { return authenticated_; }
  const RemoteEndpoint& endpoint() const { return endpoint_; }

  bool Ping(std::string* software, std::uint16_t* protocol_version,
            std::uint64_t* server_time, std::string* error_message);

  bool Register(const std::string& username, const std::string& password,
                std::string* error_message);
  bool Login(const std::string& username, const std::string& password,
             std::string* error_message);
  bool Logout(std::string* error_message);

  bool List(std::vector<RemoteSnapshotInfo>* snapshots,
            std::string* error_message);

  // 上传一个已经存在的本地文件。两遍读：第一遍算 SHA-256（UPLOAD_BEGIN 之前
  // 必须给出），第二遍分块发送。文件不会被整份读进内存。
  bool UploadArchiveFile(const std::string& local_path,
                         const std::string& display_name,
                         const RemoteProgressCallback& progress,
                         RemoteSnapshotInfo* uploaded,
                         std::string* error_message);

  // 下载到 target_path。目标已存在且 allow_overwrite 为假时**不做任何事**
  // 直接失败；允许覆盖时也是"先写 .part、校验通过才原子改名"，
  // 所以下载失败不会破坏已有的目标文件。
  bool DownloadArchiveFile(const std::string& snapshot_id,
                           const std::string& target_path, bool allow_overwrite,
                           const RemoteProgressCallback& progress,
                           RemoteSnapshotInfo* downloaded,
                           std::string* error_message);

  bool Delete(const std::string& snapshot_id, std::string* error_message);

  // 注销当前账户：服务端删除该账户以及它的全部云端备份。
  //
  // 与 Logout 的区别是本模块对外语义的一部分：Logout 只清掉本机内存里的会话
  // （云端数据一点没动），DeleteAccount 是不可撤销的服务端删除。载荷是当前
  // 口令——服务端会用同一个账户的口令再校验一次，所以一个被捡到的 token
  // 不足以删掉账户。口令只走这条连接，不写日志、不落盘。
  // 成功之后本客户端的会话立即失效（token 在内存里被丢弃，连接仍可继续
  // 用于注册 / 登录）；失败（例如口令不对）不动会话。
  bool DeleteAccount(const std::string& password, std::string* error_message);

  // 最近一次失败的原始原因（英文协议层原因），供日志与测试使用。
  const std::string& last_error() const { return last_error_; }

 private:
  bool Request(Opcode opcode, const std::string& payload, FrameHeader* header,
               std::string* response, std::string* error_message);
  bool RequireAuthenticated(const std::string& what,
                            std::string* error_message);
  void Fail(const std::string& reason);

  int fd_ = -1;
  std::uint64_t next_request_id_ = 1;
  bool authenticated_ = false;
  RemoteEndpoint endpoint_;
  std::string token_;
  std::string last_error_;
};

}  // namespace net
}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_REMOTE_BACKUP_CLIENT_H_
