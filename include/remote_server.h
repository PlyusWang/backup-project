// include/remote_server.h
//
// PR #20：远程备份服务端（backup-server）。
//
// 定位：**存储后端 + 传输边界**。它负责
//
//   监听 / 帧编解码 / 连接状态机 / 用户认证 / SQLite 元数据 /
//   流式落盘 / 原子发布 / 安全删除
//
// 它**不**负责，也永远不许负责：
//
//   Filter / MyPack / USTAR / 压缩 / 加密 / 增量链 / 恢复引擎
//
// 客户端送来的是本地已经生成并验证过的 .bak 归档字节，服务端只把它当作
// "一段有名字、有长度、有 SHA-256 的不透明字节"存起来。第二个 BackupEngine
// 一旦出现在这个进程里，架构就错了。
//
// 并发模型（foundation 版本，刻意保持简单且**有界**）：
//
//   主线程         accept 循环
//   worker 线程    worker_count 个固定线程，每个同时服务一个连接
//   排队           worker 全忙时新连接留在 listen backlog 里，不生成线程
//
// 所以"最大并发客户端"就是 worker_count，不存在"每来一个连接 new 一个
// detached 线程"的路径。SQLite 访问用一把互斥锁串行化 + busy timeout，
// 因此并发写不会随机得到 "database is locked"。
//
// 数据目录锁：Start() 会在 <root>/.backup-server.lock 上抢一把 flock 独占锁，
// 并一直持有到 Stop()（或进程退出）。它同时是两件事的依据：
//   * "同一个数据目录只有一个写者"——第二个服务端会直接拒绝启动；
//   * backup-server-admin 判断"服务器是否正在运行"——不是 pgrep 猜的，
//     而是内核持有的锁；进程崩溃也会被自动释放，不留 stale 状态。

#ifndef BACKUP_PROJECT_INCLUDE_REMOTE_SERVER_H_
#define BACKUP_PROJECT_INCLUDE_REMOTE_SERVER_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "crypto.h"
#include "network_protocol.h"

namespace backupproject {

// 数据目录上的独占锁（实现见 file_lock.h）。这里只前向声明：不需要把
// <sys/file.h> 那一套带进每一个包含 remote_server.h 的地方。
class FileLock;

namespace net {

inline constexpr const char* kServerSoftwareName = "backup-server";
inline constexpr const char* kServerSoftwareVersion = "0.1.0";

struct RemoteServerConfig {
  std::string bind_address = "127.0.0.1";
  std::uint16_t port = 18765;
  // blob 根目录：<root>/users/<user-id>/<snapshot-id>.bak
  std::string root_directory;
  // SQLite 元数据库文件。
  std::string database_path;
  // 含 BACKUP_TOKEN_SECRET 的 secrets.env（600）。只读，绝不打印内容。
  std::string secret_file_path;
  // 追加日志文件；空串表示只写 stderr。
  std::string log_file_path;
  std::uint64_t max_upload_bytes = kDefaultMaxUploadBytes;
  std::size_t worker_count = 4;
  int io_timeout_seconds = 30;
  // 不往 stderr 打运行期日志（只写 log_file_path）。
  // 自动测试用它保持输出干净；产品默认关闭。
  bool quiet = false;
};

// 每个连接的会话状态。非法状态一律回 kInvalidState，不"猜用户想干什么"。
enum class ConnectionState {
  kConnected,
  kAuthenticated,
  kUploadInProgress,
  kDownloadInProgress,
};

const char* ConnectionStateName(ConnectionState state);

// 一个连接的会话数据。服务端侧所有"跨请求"的东西都在这里，
// 函数之间不通过全局变量传递。
struct ConnectionContext {
  ConnectionState state = ConnectionState::kConnected;
  std::uint64_t user_id = 0;
  std::string username;

  // 上传中（UPLOAD_BEGIN 之后、UPLOAD_END 之前）
  std::string upload_display_name;
  std::uint64_t upload_declared_size = 0;
  std::string upload_sha256;
  std::string upload_snapshot_id;
  std::string upload_temp_path;
  std::uint64_t upload_received = 0;
  int upload_fd = -1;

  // 上传中的增量摘要器。Sha256 只能 Final 一次，所以复位靠整体赋值一个新的。
  crypto::Sha256 upload_hasher;

  // 下载中
  std::string download_snapshot_id;
  std::uint64_t download_size = 0;
  std::string download_sha256;
  std::uint64_t download_sent = 0;
  int download_fd = -1;
};

class RemoteMetadataStore;
class RemoteAuth;
class RemoteMaintenance;

class RemoteServer {
 public:
  RemoteServer();
  ~RemoteServer();

  RemoteServer(const RemoteServer&) = delete;
  RemoteServer& operator=(const RemoteServer&) = delete;

  bool Configure(const RemoteServerConfig& config, std::string* error_message);

  // 建监听 socket、建目录、打开数据库、读 secret。任何一步失败都返回 false，
  // 并且**不留下**半开状态（listener 会关掉）。
  bool Start(std::string* error_message);
  void Stop();

  // 实际绑定到的端口（port == 0 时由内核分配，测试用这个拿真实端口）。
  std::uint16_t bound_port() const { return bound_port_; }
  int listener_fd() const { return listener_fd_; }
  bool running() const { return listener_fd_ >= 0; }

  // 服务一个已经 accept 的 fd，直到对端关闭或发生致命错误。
  bool ServeConnection(int fd, std::string* error_message);

  // 连接收尾：中止未完成的上传（删掉临时文件）、关闭下载句柄。
  // ServeConnection 的每一条返回路径都会调用它，所以"客户端半路断了"
  // 不会留下 .part 文件，也不会泄漏 fd。
  void CleanupConnection(ConnectionContext* context);

  // ---- 测试专用故障注入 ----
  //
  // "磁盘写失败时不能留下已发布的 blob"这条路径必须能真的被触发一次。
  // 产品代码里没有任何地方调用这两个方法。
  void FailNextBlobWriteForTesting() { fail_next_write_ = true; }
  void FailNextMetadataInsertForTesting();
  // "注销过程中元数据事务失败时不能留下半删除状态"这条路径必须能真的被触发
  // 一次。产品代码里没有任何地方调用它。
  void FailNextAccountDeleteForTesting();

  // 阻塞式运行：accept + 固定 worker 池，Stop() 之后返回。
  bool Run(std::string* error_message);
  void RequestStop();

  const std::string& last_error() const { return last_error_; }

 private:
  bool OpenListener(std::string* error_message);
  bool LoadSecret(std::string* error_message);
  // 抢 <root>/.backup-server.lock。抢不到说明另一个 backup-server（或者一个
  // 正在做破坏性操作的管理工具）正拿着它。
  bool AcquireDataLock(std::string* error_message);
  // 单个帧的业务处理。返回 false 表示连接必须断开。
  bool HandleFrame(int fd, const FrameHeader& header,
                   const std::string& payload, ConnectionContext* context,
                   std::string* error_message);
  bool SendStatus(int fd, const FrameHeader& request, Status status,
                  const std::string& payload, std::string* error_message);
  bool SendError(int fd, std::uint16_t opcode, std::uint64_t request_id,
                 Status status, std::string* error_message);
  bool HandlePing(int fd, const FrameHeader& header,
                  std::string* error_message);
  bool HandleRegister(int fd, const FrameHeader& header,
                      const std::string& payload, ConnectionContext* context,
                      std::string* error_message);
  bool HandleLogin(int fd, const FrameHeader& header,
                   const std::string& payload, ConnectionContext* context,
                   std::string* error_message);
  bool HandleLogout(int fd, const FrameHeader& header,
                    ConnectionContext* context, std::string* error_message);
  bool HandleList(int fd, const FrameHeader& header, ConnectionContext* context,
                  std::string* error_message);
  bool HandleUploadBegin(int fd, const FrameHeader& header,
                         const std::string& payload, ConnectionContext* context,
                         std::string* error_message);
  bool HandleUploadChunk(int fd, const FrameHeader& header,
                         const std::string& payload, ConnectionContext* context,
                         std::string* error_message);
  bool HandleUploadEnd(int fd, const FrameHeader& header,
                       const std::string& payload, ConnectionContext* context,
                       std::string* error_message);
  bool HandleDownloadBegin(int fd, const FrameHeader& header,
                           const std::string& payload,
                           ConnectionContext* context,
                           std::string* error_message);
  bool HandleDownloadChunk(int fd, const FrameHeader& header,
                           const std::string& payload,
                           ConnectionContext* context,
                           std::string* error_message);
  bool HandleDownloadEnd(int fd, const FrameHeader& header,
                         const std::string& payload, ConnectionContext* context,
                         std::string* error_message);
  bool HandleDelete(int fd, const FrameHeader& header,
                    const std::string& payload, ConnectionContext* context,
                    std::string* error_message);
  // 注销账户：重新校验当前口令 -> 隔离数据目录 -> 事务删元数据 -> 物理清理。
  bool HandleDeleteAccount(int fd, const FrameHeader& header,
                           const std::string& payload,
                           ConnectionContext* context,
                           std::string* error_message);

  // 每个"会碰到数据"的操作都先过这一关。
  //
  // token 的签名只证明"这条 token 是服务端签发的"，不证明"这个账户还在"：
  // 服务端不保存会话表，注销之后旧 token 的签名依然有效。所以账户是否仍然
  // 存在必须每次回查数据库。
  //
  // 返回 true 表示"账户已经不存在（或者读不出来），错误帧已经发出了"，
  // 调用方必须立刻 return true，不要再做任何事。
  bool RejectIfAccountMissing(int fd, const FrameHeader& header,
                              ConnectionContext* context,
                              std::string* error_message);

  // 清掉上传状态但**不**删文件（发布成功之后用）。
  void ResetUploadState(ConnectionContext* context);
  // 中止上传：关 fd + 删临时文件 + 清状态。任何失败路径都走这里。
  void AbortUpload(ConnectionContext* context);
  void CloseDownload(ConnectionContext* context);
  std::string UserDirectory(std::int64_t user_id) const;
  bool EnsureUserDirectory(std::int64_t user_id, std::string* directory,
                           std::string* error_message);
  bool WriteAll(int fd, const char* data, std::size_t size,
                std::string* error_message);
  bool GenerateSnapshotId(std::string* snapshot_id, std::string* error_message);
  void WorkerLoop();
  void Log(const std::string& message);

  RemoteServerConfig config_;
  // 元数据库只在服务端进程里存在：桌面端不链接 SQLite。
  std::unique_ptr<RemoteMetadataStore> store_;
  // 数据维护原语：DELETE / DELETE_ACCOUNT 与 ECS 本地管理工具共用同一份实现。
  std::unique_ptr<RemoteMaintenance> maintenance_;
  // 数据目录独占锁：Start() 之后一直持有到 Stop()（或进程退出）。
  std::unique_ptr<backupproject::FileLock> data_lock_;
  int listener_fd_ = -1;
  std::uint16_t bound_port_ = 0;
  std::string secret_;
  std::string last_error_;

  std::mutex log_mutex_;
  std::mutex work_mutex_;
  std::condition_variable work_ready_;
  std::condition_variable slot_free_;
  std::deque<int> pending_;
  std::vector<std::thread> workers_;
  std::atomic<bool> stop_requested_{false};
  std::size_t busy_workers_ = 0;
  std::size_t worker_count_ = 0;
  bool fail_next_write_ = false;
};

}  // namespace net
}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_REMOTE_SERVER_H_
