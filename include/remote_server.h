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

#ifndef BACKUP_PROJECT_INCLUDE_REMOTE_SERVER_H_
#define BACKUP_PROJECT_INCLUDE_REMOTE_SERVER_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "network_protocol.h"

namespace backupproject {
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

  // 下载中
  std::string download_snapshot_id;
  std::uint64_t download_size = 0;
  std::uint64_t download_sent = 0;
  int download_fd = -1;
};

class RemoteMetadataStore;
class RemoteAuth;

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

  // 阻塞式运行：accept + 固定 worker 池，Stop() 之后返回。
  bool Run(std::string* error_message);
  void RequestStop();

  const std::string& last_error() const { return last_error_; }

 private:
  bool OpenListener(std::string* error_message);
  bool LoadSecret(std::string* error_message);
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
  void WorkerLoop();
  void Log(const std::string& message);

  RemoteServerConfig config_;
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
};

}  // namespace net
}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_REMOTE_SERVER_H_
