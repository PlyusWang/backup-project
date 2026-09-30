// ui/modern/remote_controller.h
//
// QML 与"远程备份网络层"之间唯一的桥。
//
// 依赖方向：
//
//   QML ──► RemoteController ──► RemoteArchiveClient ──► BPNET1（TCP）
//
// 与 backupctl remote 用的是**同一个** RemoteArchiveClient：不存在
// "CLI 一套 socket、GUI 再写一套"。本文件里没有 socket()、没有帧编解码、
// 没有认证、没有哈希、没有协议状态码表——它只做四件事：
//
//   1. 把 QML 的输入做**用户级**校验（端口是不是整数、用户名密码合不合规），
//      合规判断复用 network_protocol.cpp 里那一套共享校验器；
//   2. 把阻塞的网络调用搬到后台线程，主线程不被 PBKDF2（实测约 1.5 s）冻住；
//   3. 把协议的结构化原因归类成一句用户能看懂的中文；
//   4. 把结果摆成 QML 好绑定的形状（列表、进度、状态条）。
//
// ---- 凭据 ----
//
// password 与 token **只存在于内存**：password 是本对象的成员，token 在
// RemoteArchiveClient 内部。两者都不写 config.json / schedule.json /
// realtime.json，不写 QSettings，不进日志，不进 argv，不进界面文本。
// 退出登录与进程退出时 password 被显式擦除，token 随连接一起消失。
// 诚实的边界：QString 可能因隐式共享留下副本，也没有 mlock，所以这是
// "尽力而为的进程内保密"，不是内存加密，也不等于 native TLS。
//
// ---- 并发 ----
//
// 同一个控制器同一时刻最多一个网络操作。busy_ 在**提交任务之前**同步置位，
// 所以同一事件循环回合里的第二个请求一定会被拒绝——不是排队，也不是竞态。
// QML 上的 enabled: !remote.busy 只是可见性；真正的闸门在这一层。
//
// 本控制器**不**占用 OperationGate：那是"同一时刻只有一个本地仓库 writer"
// 的载体（手动备份 / 恢复 / 删除 / 计划 / 实时）。远程网络 I/O 不改动本地
// 仓库的任何持久状态——上传只读一个用户选定的 .bak，下载写的是用户指定的
// 新路径——为了"形式统一"把所有网络 I/O 塞进那把闸门只会凭空制造互斥。

#ifndef BACKUP_PROJECT_UI_MODERN_REMOTE_CONTROLLER_H_
#define BACKUP_PROJECT_UI_MODERN_REMOTE_CONTROLLER_H_

#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "remote_backup_client.h"

namespace backup_modern {

// 一次后台网络操作的返回值。
//
// 只用值类型：跨线程只发生一次拷贝，后台线程不读控制器的任何成员，也就不需要
// 加锁，更不可能碰到 QObject / QML 状态。
struct RemoteOpResult {
  enum class Kind {
    kRegister,
    kLogin,
    kList,
    kUpload,
    kDownload,
    kDelete,
  };

  Kind kind = Kind::kList;
  bool ok = false;
  // 网络层给出的结构化原始原因（英文；服务端拒绝时形如
  // "UPLOAD_BEGIN -> ALREADY_EXISTS"）。只进 stderr 日志与测试断言，
  // **不**直接显示给用户——用户看到的是 message。
  std::string detail;
  // 服务端拒绝时由共享函数反查出来的状态名（ ALREADY_EXISTS 等），
  // 为空串表示这次失败不是服务端状态拒绝，而是本地或传输问题。
  std::string status_name;
  // 归类后的失败类别，页面据此决定要不要给"换名字 / 覆盖 / 重试"这类动作。
  QString error_kind = QStringLiteral("none");
  // 面向用户的一句话；失败时一定有。
  QString message;

  std::vector<backupproject::net::RemoteSnapshotInfo> snapshots;
  backupproject::net::RemoteSnapshotInfo archive;
  std::uint64_t bytes_done = 0;
  std::uint64_t bytes_total = 0;
};

// 后台线程需要的全部输入。刻意做成一个值类型：后台线程只读它，
// 调用方在提交之后可以立刻改自己的状态，两者之间没有任何共享。
struct RemoteRequest {
  RemoteOpResult::Kind kind = RemoteOpResult::Kind::kList;
  backupproject::net::RemoteEndpoint endpoint;
  std::string username;
  std::string password;
  std::string local_path;
  std::string display_name;
  std::string snapshot_id;
  std::string target_path;
  bool allow_overwrite = false;
};

class RemoteController : public QObject {
  Q_OBJECT

  // ---- 端点（只读回读）----
  //
  // 真正的输入由 registerAccount / login **一次性**带入：端口是不是整数、
  // 用户名密码合不合规，都在那一次调用里判完，不存在"先写一半再登录"的
  // 中间态。这里回读的是**上一次真正生效**的那一组值。
  //
  // 本轮不做持久化：config.json 里只有仓库路径，没有自然落点，就不为了三个
  // 字段新造一套配置系统（见 06-KNOWN-LIMITATIONS.md）。
  Q_PROPERTY(QString host READ host NOTIFY endpointChanged)
  Q_PROPERTY(QString portText READ portText NOTIFY endpointChanged)
  Q_PROPERTY(QString username READ username NOTIFY endpointChanged)

  // ---- 会话 ----
  Q_PROPERTY(bool connected READ connected NOTIFY sessionChanged)
  Q_PROPERTY(bool authenticated READ authenticated NOTIFY sessionChanged)
  Q_PROPERTY(QString sessionText READ sessionText NOTIFY sessionChanged)
  Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
  Q_PROPERTY(QString busyAction READ busyAction NOTIFY busyChanged)

  // ---- 云端备份列表 ----
  // 每一项是 {id, name, sizeBytes, sizeText, createdText, sha256Short}：
  // 页面只做展示，不再自己算大小与时间。
  Q_PROPERTY(QVariantList snapshots READ snapshots NOTIFY snapshotsChanged)
  Q_PROPERTY(QString listSummary READ listSummary NOTIFY snapshotsChanged)
  Q_PROPERTY(bool listLoaded READ listLoaded NOTIFY snapshotsChanged)

  // ---- 传输进度 ----
  // 刻意不叫 progressValue / percent：modern_gui_check.sh 里有一条"不许出现
  // 假进度字段"的断言，而这里的进度是网络层的真实字节数，不是估出来的。
  Q_PROPERTY(bool transferActive READ transferActive NOTIFY progressChanged)
  Q_PROPERTY(
      QString transferPhaseText READ transferPhaseText NOTIFY progressChanged)
  Q_PROPERTY(qint64 bytesDone READ bytesDone NOTIFY progressChanged)
  Q_PROPERTY(qint64 bytesTotal READ bytesTotal NOTIFY progressChanged)
  Q_PROPERTY(double progressRatio READ progressRatio NOTIFY progressChanged)
  Q_PROPERTY(QString progressText READ progressText NOTIFY progressChanged)

  // ---- 状态条 ----
  Q_PROPERTY(QString statusKind READ statusKind NOTIFY statusChanged)
  Q_PROPERTY(QString statusTitle READ statusTitle NOTIFY statusChanged)
  Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusChanged)
  Q_PROPERTY(QString statusScope READ statusScope NOTIFY statusChanged)
  // 最近一次失败的类别（"target-exists" 等）与结构化技术原因。
  // 前者让页面在"目标已存在"时只多给一个明确的动作（覆盖并重新下载），
  // 后者只出现在默认折叠的"技术详情"里——诊断信息不丢，但也不喧宾夺主。
  Q_PROPERTY(QString lastErrorKind READ lastErrorKind NOTIFY statusChanged)
  Q_PROPERTY(QString diagnosticText READ diagnosticText NOTIFY statusChanged)

 public:
  explicit RemoteController(QObject* parent = nullptr);
  ~RemoteController() override;

  // ---- 只读访问器 ----
  QString host() const { return QString::fromStdString(endpoint_.host); }
  QString portText() const { return QString::number(endpoint_.port); }
  QString username() const { return username_; }
  bool connected() const { return client_.connected(); }
  bool authenticated() const { return authenticated_; }
  QString sessionText() const;
  bool busy() const { return busy_; }
  QString busyAction() const { return busy_action_; }
  QVariantList snapshots() const { return snapshot_items_; }
  QString listSummary() const { return list_summary_; }
  bool listLoaded() const { return list_loaded_; }
  bool transferActive() const { return transfer_active_; }
  QString transferPhaseText() const { return transfer_phase_text_; }
  qint64 bytesDone() const { return static_cast<qint64>(bytes_done_.load()); }
  qint64 bytesTotal() const { return static_cast<qint64>(bytes_total_.load()); }
  double progressRatio() const;
  QString progressText() const;
  QString statusKind() const { return status_kind_; }
  QString statusTitle() const { return status_title_; }
  QString statusMessage() const { return status_message_; }
  QString statusScope() const;
  QString lastErrorKind() const { return last_error_kind_; }
  QString diagnosticText() const {
    return QString::fromStdString(last_detail_);
  }

  // ---- QML 入口 ----
  //
  // 注册与登录都把整份输入带进来；返回 false 表示请求**没有被受理**
  // （输入不合法、已经有一个操作在跑），此时状态条里已经有原因。
  Q_INVOKABLE bool registerAccount(const QString& host,
                                   const QString& port_text,
                                   const QString& username,
                                   const QString& password);
  Q_INVOKABLE bool login(const QString& host, const QString& port_text,
                         const QString& username, const QString& password);

  // 本地退出：清掉内存里的会话（token 随连接一起消失，密码清空）。
  // 不新增服务端 logout opcode —— 服务端 token 到期前仍然有效，这一点写在
  // 06-KNOWN-LIMITATIONS.md 里，不假装它是撤销。
  Q_INVOKABLE void logoutLocal();

  Q_INVOKABLE bool refreshList();
  Q_INVOKABLE bool uploadArchive(const QString& local_path,
                                 const QString& display_name);
  Q_INVOKABLE bool downloadArchive(const QString& snapshot_id,
                                   const QString& target_path,
                                   bool allow_overwrite);
  Q_INVOKABLE bool deleteSnapshot(const QString& snapshot_id);
  Q_INVOKABLE void clearStatus();

  // 文件对话框的 URL 互转与其它页面同一套实现。
  Q_INVOKABLE QString localPathFromUrl(const QUrl& url) const;
  // 选择器的起始位置：目录就用它本身，文件就用它所在目录，
  // 都没有（或不存在）时退回主目录。
  Q_INVOKABLE QUrl fileDialogStartUrl(const QString& path) const;
  Q_INVOKABLE QString suggestedDownloadName(const QString& display_name) const;

  // ---- 仅供 main.cpp 的自动化测试使用（刻意不是 Q_INVOKABLE）----
  bool waitForIdle(int timeout_ms);
  QString lastErrorKindForTest() const { return last_error_kind_; }
  QString lastDetailForTest() const {
    return QString::fromStdString(last_detail_);
  }
  int snapshotCountForTest() const { return snapshot_items_.size(); }
  // 真实进度回调被调用过几次（进程生命周期内的累计值）；
  // 用来证明进度不是画上去的。
  int progressCallbackCountForTest() const {
    return progress_callbacks_.load();
  }

 signals:
  void endpointChanged();
  void sessionChanged();
  void busyChanged();
  void snapshotsChanged();
  void progressChanged();
  void statusChanged();
  // 一次后台操作结束。kind 与 RemoteOpResult::Kind 同名，供测试分辨。
  void operationFinished(const QString& kind, bool succeeded);

 private:
  // 传输方向。用整数原子变量跨线程传，避免在后台线程碰 QString。
  enum class Phase { kNone = 0, kUpload = 1, kDownload = 2 };

  // 输入校验：合规时填好 endpoint_/username_，否则写状态条并返回 false。
  bool AcceptEndpoint(const QString& host, const QString& port_text,
                      const QString& username);
  // 口令校验：长度下限来自共享常量，上限与 NUL 规则来自共享校验器。
  bool AcceptPassword(const QString& password);
  // 提交前的统一闸门：busy 与"是否已登录"都在这里挡住。
  bool BeginOperation(const QString& action_text, bool need_login);
  void Submit(const RemoteRequest& request);
  // 把后台线程的返回值落到界面状态上。结果只从 QFuture 里读一次：
  // 后台线程写、主线程读的就是同一个值，不存在第二份"待读结果"。
  void OnOperationFinished();
  void ApplyResult(const RemoteOpResult& result);
  // 空闲基线：把这一页的临时提示换成"当前真实的连接状态"。
  void ResetIdleStatus();

  // 后台线程里跑的那一段。只用参数与局部变量，不碰任何成员状态。
  static RemoteOpResult RunOperation(
      backupproject::net::RemoteArchiveClient* client, RemoteRequest request,
      RemoteController* progress_sink);

  // 进度回调在后台线程里被调用：只写原子变量，再往主线程投一个"来读一下"的
  // 通知。进度值本身走原子变量，不走信号参数，避免跨线程碰 QML 状态。
  void PublishProgress(const backupproject::net::RemoteTransferProgress& p);
  void OnProgressNotify();

  void SetStatus(const QString& kind, const QString& title,
                 const QString& message);
  void SetBusy(bool busy, const QString& action);
  void SetSnapshots(
      const std::vector<backupproject::net::RemoteSnapshotInfo>& snapshots);

  // 把网络层的结构化原因归类成用户措辞（见 .cpp 顶部的说明）。
  static QString ClassifyFailure(const std::string& status_name,
                                 const std::string& detail,
                                 RemoteOpResult::Kind kind);
  static QString DescribeFailure(const QString& error_kind);
  static QString TitleForFailure(const QString& error_kind);
  static QString KindName(RemoteOpResult::Kind kind);
  static QString FormatSize(std::uint64_t bytes);
  static QString FormatTime(std::uint64_t unix_seconds);

  backupproject::net::RemoteArchiveClient client_;
  backupproject::net::RemoteEndpoint endpoint_;
  QString username_;
  // 只存在于内存；退出登录与析构时擦除。绝不落盘、绝不进日志。
  QString password_;

  bool authenticated_ = false;
  bool busy_ = false;
  QString busy_action_;
  QFutureWatcher<RemoteOpResult> watcher_;
  // 析构时先置位，让还在跑的进度回调立刻放手。
  std::atomic<bool> shutting_down_{false};

  QVariantList snapshot_items_;
  QString list_summary_ = QStringLiteral("尚未登录");
  bool list_loaded_ = false;

  std::atomic<std::uint64_t> bytes_done_{0};
  std::atomic<std::uint64_t> bytes_total_{0};
  // 累计值，不随每次提交清零：它是"进度回调真的被调用过"的证据。
  std::atomic<int> progress_callbacks_{0};
  std::atomic<int> phase_{0};
  bool transfer_active_ = false;
  QString transfer_phase_text_;

  QString last_error_kind_ = QStringLiteral("none");
  std::string last_detail_;

  QString status_kind_ = QStringLiteral("idle");
  QString status_title_ = QStringLiteral("尚未连接服务器");
  QString status_message_;
};

}  // namespace backup_modern

#endif  // BACKUP_PROJECT_UI_MODERN_REMOTE_CONTROLLER_H_
