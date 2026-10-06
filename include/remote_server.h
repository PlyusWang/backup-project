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
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "crypto.h"
#include "network_protocol.h"
#include "secure_transport.h"

namespace backupproject {

// 数据目录上的独占锁（实现见 file_lock.h）。这里只前向声明：不需要把
// <sys/file.h> 那一套带进每一个包含 remote_server.h 的地方。
class FileLock;

namespace net {

inline constexpr const char* kServerSoftwareName = "backup-server";
inline constexpr const char* kServerSoftwareVersion = "0.1.0";

struct RemoteServerConfig {
  // 监听地址。本版本**只接受** "127.0.0.1"：没有原生 TLS，机密性由 SSH 隧道
  // 提供，Configure() 对任何非环回地址（0.0.0.0 / 私网 / 公网）都 fail closed。
  std::string bind_address = "127.0.0.1";
  std::uint16_t port = 18765;
  // blob 根目录：<root>/users/<user-id>/<snapshot-id>.bak
  std::string root_directory;
  // SQLite 元数据库文件。
  std::string database_path;
  // 含 BACKUP_TOKEN_SECRET 的 secrets.env（600）。只读，绝不打印内容。
  std::string secret_file_path;
  // BPSEC1 的服务端长期身份私钥文件（32 字节原始 X25519
  // 标量，0600，O_NOFOLLOW）。
  //
  // **必填**：Configure/Start 都拒绝空值。这不是"可选加固"——BPNET1 的口令、
  // token、用户名与快照元数据全部由 BPSEC1 保护，缺了它就没有任何一条可以
  // 安全服务的路径，因此不存在"不配密钥就退回明文"的分支。
  std::string transport_key_file_path;
  // BPSEC2：服务器身份证书（BPCERT1 原始字节所在文件）。空 = 不出示证书，
  // 只能走 BPSEC1（人工 pin）。
  std::string certificate_file_path;
  // 只接受 BPSEC2（签名身份）的客户端。收到 BPSEC1 的 ClientHello 直接拒绝，
  // 这就是"拒绝降级"的开关；打开时必须同时配置证书。
  bool require_bpsec2 = false;
  // PR #23：允许**公网**监听（--bind 不是 127.0.0.1）。默认 false，且必须
  // 同时满足「配置了 BPSEC2 证书」与「给出一句话理由」，理由会写进启动日志。
  // 没有这个开关时行为与过去完全一致：任何非回环地址一律 fail closed。
  bool allow_public_bind = false;
  std::string public_bind_reason;
  // §33：登录失败节流。同一个用户名连续失败达到 max_login_failures 之后，
  // 接下来 login_lockout_seconds 秒内即使口令正确也拒绝；成功一次即清零。
  // 0 = 关闭（公网部署不要关）。
  int max_login_failures = 5;
  int login_lockout_seconds = 60;
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

  // PR #21：这次上传要登记的链关系。UPLOAD_BEGIN 时校验并定下来，
  // UPLOAD_END 发布时写进元数据。generation 由**服务端**按父的 generation
  // 推导（父 + 1），客户端没有机会自己填一个数。
  std::uint16_t upload_kind = 0;
  std::string upload_parent_id;
  std::uint64_t upload_generation = 0;
  std::string upload_lineage;

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

  // ---- 生命周期合同（三条，违反其中任意一条都是 UB）----
  //
  //   RequestStop()   唯一允许与 Run() 并发调用的入口：只置停止标志，
  //                   线程安全，不碰任何 fd / 线程 / 数据库。
  //   Run()           accept 循环与 worker 池的 owner：它在返回之前自己
  //                   会 shutdown 掉 pending 连接、join 全部 worker、清空
  //                   workers_。
  //   Stop()          只能在 Run() 返回之后调用（或者从来没有调用过 Run()），
  //                   负责释放 listener / store / maintenance / 数据目录锁。
  //                   **不得与 Run() 并发**：两者都会 join 同一个 workers_，
  //                   并且 Stop() 会关掉 Run() 正在使用的 listener。
  //
  // “运行中停止”只有一个正确写法：
  //
  //   std::thread runner([&]{ server.Run(&error); });
  //   ...
  //   server.RequestStop();     // 线程安全
  //   runner.join();            // Run() 在这里收尾
  //   server.Stop();            // 现在才可以做最终清理
  //
  // 产品路径（server/main.cpp）就是上面这个顺序。析构函数调用 Stop()，所以
  // “Run() 还在跑的时候销毁对象”同样是 UB。
  //
  // Stop() 内部会检查 Run() 是否仍在进行：如果在，它直接返回不做任何拆除
  // （宁可留下资源让 Run() 自己收尾，也不在 worker 还在用 store_/listener 的
  // 时候把它们销毁）。这是一道防误用的闸，不是并发调用的许可。
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
  // 让服务端**处理完这一帧之后**直接断开连接。它用来确定性地制造"请求可能已经
  // 被服务端执行、但客户端读不到响应"的中间态（进程被 kill、隧道重启都属于这
  // 一类）。产品代码里没有任何地方调用它。
  void FailNextResponseForTesting() { fail_next_response_.store(true); }

  // 按操作码统计收到的请求数。测试用它证明"客户端不会在失败之后偷偷重发一次
  // 请求"：重发会让计数变成 2。
  std::uint64_t request_count_for_testing(std::uint16_t opcode) const;
  void FailNextMetadataInsertForTesting();
  // "注销过程中元数据事务失败时不能留下半删除状态"这条路径必须能真的被触发
  // 一次。产品代码里没有任何地方调用它。
  void FailNextAccountDeleteForTesting();

  // 阻塞式运行：accept + 固定 worker 池。RequestStop() 之后返回，返回前自己
  // join 掉全部 worker。Stop() 只能在它返回之后调用（见上面的生命周期合同）。
  bool Run(std::string* error_message);
  void RequestStop();

  const std::string& last_error() const { return last_error_; }

 private:
  bool OpenListener(std::string* error_message);
  bool LoadSecret(std::string* error_message);
  // 读 BPSEC1 长期身份私钥并推导公钥。文件必须是 0600 的普通文件，
  // 不跟随符号链接；任何一步失败都让 Start() 整体失败。
  bool LoadTransportIdentityKey(std::string* error_message);
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
  // 在新连接上用 token 恢复会话（见 network_protocol.h 里 kResume 的说明）。
  bool HandleResume(int fd, const FrameHeader& header,
                    const std::string& payload, ConnectionContext* context,
                    std::string* error_message);
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
  // BPSEC1 长期身份密钥。客户端 pin 的就是它的公钥（或公钥的 SHA-256 指纹）。
  // 私钥只在内存与 0600 文件里存在，绝不打印、绝不进日志。
  TransportIdentity transport_identity_;
  std::string last_error_;

  // §33：失败节流表。按**用户名字符串**计数，不论该用户是否存在 ——
  // 对不存在的名字也限速，否则限速本身就成了「这个用户名存在吗」的探针。
  struct LoginThrottle {
    int consecutive_failures = 0;
    std::int64_t locked_until = 0;  // Unix 秒
  };
  std::int64_t LoginLockRemainingSeconds(const std::string& username);
  void RecordLoginFailure(const std::string& username);
  void ClearLoginFailures(const std::string& username);
  std::mutex login_throttle_mutex_;
  std::map<std::string, LoginThrottle> login_throttle_;
  std::mutex log_mutex_;
  std::mutex work_mutex_;
  std::condition_variable work_ready_;
  std::condition_variable slot_free_;
  std::deque<int> pending_;
  std::vector<std::thread> workers_;
  std::atomic<bool> stop_requested_{false};
  // Run() 是否正在执行。只用来挡住“Run() 与 Stop() 并发”这种误用：
  // Stop() 在它为 true 时不做任何拆除（细节见头文件的生命周期合同）。
  std::atomic<bool> run_in_progress_{false};
  std::size_t busy_workers_ = 0;
  std::size_t worker_count_ = 0;
  bool fail_next_write_ = false;
  std::atomic<bool> fail_next_response_{false};
  // 每个操作码收到的请求数。协议里用到的操作码都 < 256，超出范围的不统计。
  std::atomic<std::uint64_t> request_counts_[256];
};

}  // namespace net
}  // namespace backupproject

#endif  // BACKUP_PROJECT_INCLUDE_REMOTE_SERVER_H_
