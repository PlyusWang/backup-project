// include/remote_backup_client.h
//
// PR #20：远程备份客户端。
//
// **CLI 与 Modern GUI 共用这一个客户端**：不存在"CLI 一套 socket、GUI 再写
// 一套 socket"的结构。它只负责：
//
//   连接 / 帧收发 / 会话（token 只在内存里）/
//   流式上传（读一个块发一个块）/ 流式下载（唯一临时文件 + 校验 + 原子发布）/
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
#include "secure_transport.h"

namespace backupproject {
namespace net {

inline constexpr const char* kDefaultRemoteHost = "127.0.0.1";
inline constexpr std::uint16_t kDefaultRemotePort = 18765;

struct RemoteEndpoint {
  std::string host = kDefaultRemoteHost;
  std::uint16_t port = kDefaultRemotePort;
  int timeout_seconds = 60;
  // 服务端 BPSEC1 身份公钥的 pin：
  //   "sha256:<64 个十六进制字符>"  只 pin 指纹
  //   "hex:<64 个十六进制字符>"     直接 pin 公钥
  //
  // **必须配置**：空白会让 Connect() 直接失败（错误分类 kNoPinConfigured）。
  // 本客户端不做"第一次见到谁就信谁"（TOFU）——否则中间人可以随便换密钥，
  // 传输加密就只剩一个好看的名字。
  std::string server_key_pin;
};

struct RemoteSnapshotInfo {
  std::string snapshot_id;
  std::string display_name;
  std::uint64_t size_bytes = 0;
  std::string sha256;
  std::uint64_t created_at = 0;

  // ---- PR #21：远端增量链 ----
  //
  // 这些字段**只用来定位与展示**。它们来自服务端元数据，因此在恢复路径上
  // 永远不能替代"下载到的实际字节 + SHA-256 验证"（见 docs/remote_incremental.md）。
  // 0 = full（链根），1 = incremental。
  std::uint16_t snapshot_kind = 0;
  std::string parent_snapshot_id;
  std::uint64_t generation = 0;
  std::string lineage;
};

// 上传时要声明的链关系。full 用默认值即可（parent 为空、generation 由服务端
// 定为 0）；incremental 必须给出父快照 id 与 lineage。
struct RemoteUploadOptions {
  std::uint16_t snapshot_kind = 0;
  std::string parent_snapshot_id;
  std::string lineage;
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
  // 彻底放弃会话：关闭连接**并且**丢掉 token。退出登录、以及服务端明确说
  // token 无效（UNAUTHORIZED）时用它。
  void Disconnect();
  // 只关连接、**留着 token**：网络抖动、隧道重启、服务端按 io_timeout 关掉
  // 空闲连接都属于这一种。下一次请求会自动重连并用 token 恢复会话，用户
  // 不该因为这些原因"被退出登录"。
  void DisconnectSocket();
  bool connected() const { return fd_ >= 0; }
  // 这条连接上现在有没有一个被服务端确认过的会话。
  bool authenticated() const { return authenticated_; }
  // 手里还留着一个可以在新连接上恢复会话的 token。
  bool session_resumable() const { return !token_.empty(); }
  // 最近一次请求里服务端给出的状态码；0 表示这次失败不是服务端状态拒绝，
  // 而是本地或传输层问题。调用方据此区分"会话真的失效"和"网络抖了一下"。
  std::uint32_t last_status() const { return last_status_; }
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
  // UPLOAD_BEGIN 被接受之后的本地失败（读文件出错、文件被截短）会关掉连接：
  // 服务端据此删掉上传临时文件，token 保留，下一次操作自动重连 + RESUME。
  // 这一次失败的上传不会被自动重发。
  bool UploadArchiveFile(const std::string& local_path,
                         const std::string& display_name,
                         const RemoteProgressCallback& progress,
                         RemoteSnapshotInfo* uploaded,
                         std::string* error_message);

  // PR #21：带链关系的上传。与上一个函数的差别只有 UPLOAD_BEGIN 里多声明的
  // 三个字段（类型 / 父 id / lineage）以及 UPLOAD_END 响应里多回来的
  // （类型 / generation / 父 id）。服务端会校验父必须存在、属于同一个用户、
  // lineage 相同，并自己推导 generation。
  bool UploadSnapshotFile(const std::string& local_path,
                          const std::string& display_name,
                          const RemoteUploadOptions& options,
                          const RemoteProgressCallback& progress,
                          RemoteSnapshotInfo* uploaded,
                          std::string* error_message);

  // 下载到 target_path。
  //
  // 中间产物是**目标目录里唯一命名的**临时文件（mkstemp，0600），不是固定的
  // target + ".part"：用户自己放在那里的 <target>.part 一个字节都不会被动。
  // 长度与 SHA-256 都通过、fsync 并 close 之后才发布：
  //   * allow_overwrite 为假 -> 原子的"不覆盖"发布（内核保证），所以目标即使是
  //     下载过程中才被别的进程创建，也不会被覆盖，而是明确失败；
  //   * allow_overwrite 为真 -> 原子替换（用户明确同意覆盖）。
  // 任何失败都不会破坏已有目标文件，也不会留下临时文件。
  // DOWNLOAD_BEGIN 被接受之后，无论哪一步在本地失败，都会显式给服务端发一次
  // DOWNLOAD_END（服务端允许提前结束），所以连接不会被留在"下载中"状态。
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

  // 最近一次会话里服务端出示的身份公钥指纹（64 个小写十六进制字符）。
  // 传输层握手成功之前是空串。它不是秘密，可以写进日志。
  const std::string& server_fingerprint() const {
    return channel_.peer_fingerprint();
  }
  // 最近一次加密传输层的错误分类（握手失败、记录校验失败、重放等）。
  // 调用方据此把"服务端密钥不对"和"网络抖了一下"分开报。
  SecureTransportError last_secure_error() const { return channel_.last_error(); }

 private:
  bool Request(Opcode opcode, const std::string& payload, FrameHeader* header,
               std::string* response, std::string* error_message);
  bool RequireAuthenticated(const std::string& what,
                            std::string* error_message);
  // 发请求**之前**确认连接可用：没有连接就重连，对端已经关掉就换一条，并在
  // 手里还有 token 时先恢复会话。它发生在发送任何字节之前，所以这不是"失败
  // 之后偷偷重发"——本项目明确禁止自动重发（见交付报告的 no-retry 一节）。
  bool PrepareConnection(std::string* error_message);
  // 在新连接上用 token 恢复会话。服务端明确回 UNAUTHORIZED 时才丢掉 token。
  bool ResumeSession(std::string* error_message);
  void Fail(const std::string& reason);

  int fd_ = -1;
  std::uint64_t next_request_id_ = 1;
  bool authenticated_ = false;
  RemoteEndpoint endpoint_;
  std::string token_;
  // 这条 TCP 连接的 BPSEC1 通道：每条连接都是一次新的握手、一套新的会话密钥。
  SecureChannel channel_;
  ServerKeyPin server_key_pin_;
  std::string last_error_;
  std::uint32_t last_status_ = 0;
};

}  // namespace net
}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_REMOTE_BACKUP_CLIENT_H_
