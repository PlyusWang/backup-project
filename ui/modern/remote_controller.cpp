// ui/modern/remote_controller.cpp

#include "remote_controller.h"

#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QMetaObject>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <cstdio>
#include <utility>

#include "network_protocol.h"
#include "remote_auth.h"

namespace backup_modern {
namespace {

using backupproject::net::RemoteSnapshotInfo;
using backupproject::net::RemoteTransferProgress;
using backupproject::net::Status;

// ---- 失败归类 ----
//
// 网络层的失败原因有两类形状：
//
//   * 服务端明确拒绝 -> 客户端把 error_message 填成
//   RemoteStatusMessage(status)，
//     也就是**共享函数**给出的那句中文；
//   * 本地或传输失败 -> 客户端把 error_message 填成一句英文原因
//     （"not connected" / "cannot open ..." / "... already exists ..."）。
//
// 第一类不靠猜字符串：把 error_message 与共享函数对每个状态码的输出比一遍，
// 反查出状态码，再取共享的 StatusName()。所以本文件里**没有**第二份状态表——
// 候选项就是共享函数自己产出的那几句。
bool ResolveSharedStatus(const std::string& message, std::uint32_t* status) {
  static const Status kCandidates[] = {
      Status::kInvalidRequest, Status::kUnauthorized,
      Status::kForbidden,      Status::kNotFound,
      Status::kAlreadyExists,  Status::kInvalidState,
      Status::kTooLarge,       Status::kIntegrityMismatch,
      Status::kInternalError,  Status::kUnsupportedVersion,
      Status::kMalformedFrame, Status::kUnsupported,
  };
  for (const Status candidate : kCandidates) {
    if (backupproject::net::RemoteStatusMessage(
            static_cast<std::uint32_t>(candidate)) == message) {
      if (status != nullptr) {
        *status = static_cast<std::uint32_t>(candidate);
      }
      return true;
    }
  }
  return false;
}

bool Contains(const std::string& haystack, const char* needle) {
  return haystack.find(needle) != std::string::npos;
}

}  // namespace

RemoteController::RemoteController(QObject* parent) : QObject(parent) {
  endpoint_.host = backupproject::net::kDefaultRemoteHost;
  endpoint_.port = backupproject::net::kDefaultRemotePort;
  QObject::connect(&watcher_, &QFutureWatcher<RemoteOpResult>::finished, this,
                   &RemoteController::OnOperationFinished);
}

RemoteController::~RemoteController() {
  // 后台线程可能正拿着 client_ 在跑：先让进度回调放手，再等它结束，
  // 最后才销毁任何东西。关窗守卫（Main.qml 的 onClosing）已经挡住"传输中
  // 关窗"，这里是最后一道保险。
  shutting_down_.store(true);
  if (watcher_.isRunning()) {
    watcher_.waitForFinished();
  }
  client_.Disconnect();
  // 尽力而为地擦掉口令。QString 可能因隐式共享留下副本，也没有 mlock，
  // 所以这只是缩小窗口，不是内存加密。
  password_.fill(QChar(0));
  password_.clear();
  password_.squeeze();
  username_.clear();
}

QString RemoteController::sessionText() const {
  if (authenticated_) {
    return QStringLiteral("当前账户：%1（已登录）").arg(username_);
  }
  return QStringLiteral("未登录");
}

// 服务器可达性：空串表示"还没有试过"，于是界面什么都不说。
QString RemoteController::serverReachabilityText() const {
  switch (reachability_) {
    case RemoteReachability::kReachable:
      return QStringLiteral("服务器可达");
    case RemoteReachability::kUnreachable:
      return QStringLiteral("服务器暂时不可达");
    case RemoteReachability::kUnknown:
      break;
  }
  return QString();
}

void RemoteController::SetReachability(RemoteReachability value) {
  if (reachability_ == value) {
    return;
  }
  reachability_ = value;
  emit reachabilityChanged();
}

QString RemoteController::statusScope() const {
  // 只有"正在传输"这一种运行状态是全局的：它与"正在备份 / 正在恢复"同类，
  // 任何页面都能显示，切页不会把它清掉。其余（空闲基线 / 成功 / 失败 /
  // 忙时拒绝）都是 remote 页自己的临时提示，离开即消费。
  return status_kind_ == QStringLiteral("running") ? QString()
                                                   : QStringLiteral("remote");
}

double RemoteController::progressRatio() const {
  const std::uint64_t total = bytes_total_.load();
  if (total == 0) {
    return 0.0;
  }
  const double ratio =
      static_cast<double>(bytes_done_.load()) / static_cast<double>(total);
  if (ratio < 0.0) {
    return 0.0;
  }
  return ratio > 1.0 ? 1.0 : ratio;
}

QString RemoteController::progressText() const {
  const std::uint64_t total = bytes_total_.load();
  if (total == 0) {
    return QString();
  }
  return QStringLiteral("%1 / %2").arg(FormatSize(bytes_done_.load()),
                                       FormatSize(total));
}

// ---- 格式化：列表与进度都只展示这两样的结果 ----

QString RemoteController::FormatSize(std::uint64_t bytes) {
  const double value = static_cast<double>(bytes);
  if (bytes < 1024ull) {
    return QStringLiteral("%1 字节").arg(static_cast<qulonglong>(bytes));
  }
  if (bytes < 1024ull * 1024ull) {
    return QStringLiteral("%1 KiB").arg(
        QString::number(value / 1024.0, 'f', 1));
  }
  if (bytes < 1024ull * 1024ull * 1024ull) {
    return QStringLiteral("%1 MiB").arg(
        QString::number(value / (1024.0 * 1024.0), 'f', 1));
  }
  return QStringLiteral("%1 GiB").arg(
      QString::number(value / (1024.0 * 1024.0 * 1024.0), 'f', 2));
}

QString RemoteController::FormatTime(std::uint64_t unix_seconds) {
  if (unix_seconds == 0) {
    return QStringLiteral("时间未知");
  }
  const QDateTime when =
      QDateTime::fromSecsSinceEpoch(static_cast<qint64>(unix_seconds));
  return when.toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

QString RemoteController::KindName(RemoteOpResult::Kind kind) {
  switch (kind) {
    case RemoteOpResult::Kind::kRegister:
      return QStringLiteral("register");
    case RemoteOpResult::Kind::kLogin:
      return QStringLiteral("login");
    case RemoteOpResult::Kind::kList:
      return QStringLiteral("list");
    case RemoteOpResult::Kind::kUpload:
      return QStringLiteral("upload");
    case RemoteOpResult::Kind::kDownload:
      return QStringLiteral("download");
    case RemoteOpResult::Kind::kDelete:
      return QStringLiteral("delete");
    case RemoteOpResult::Kind::kDeleteAccount:
      return QStringLiteral("delete-account");
  }
  return QStringLiteral("unknown");
}

QString RemoteController::ClassifyFailure(const std::string& status_name,
                                          const std::string& detail,
                                          RemoteOpResult::Kind kind) {
  if (!status_name.empty()) {
    if (status_name == "UNAUTHORIZED") {
      // 登录被拒 = 用户名或密码不对；注销账户被拒 = 当前口令不对（服务端对
      // "口令错"和"账户已经不存在"回同一个码，不泄漏账户是否存在）；
      // 别的操作用到它 = 会话已经不在（例如账户刚被注销）。
      if (kind == RemoteOpResult::Kind::kLogin) {
        return QStringLiteral("credentials");
      }
      if (kind == RemoteOpResult::Kind::kDeleteAccount) {
        return QStringLiteral("account-password");
      }
      return QStringLiteral("not-logged-in");
    }
    if (status_name == "ALREADY_EXISTS") {
      return kind == RemoteOpResult::Kind::kRegister
                 ? QStringLiteral("name-taken")
                 : QStringLiteral("name-taken");
    }
    if (status_name == "NOT_FOUND") {
      return QStringLiteral("not-found");
    }
    if (status_name == "INTEGRITY_MISMATCH") {
      return QStringLiteral("integrity");
    }
    if (status_name == "FORBIDDEN") {
      return QStringLiteral("forbidden");
    }
    if (status_name == "TOO_LARGE") {
      return QStringLiteral("too-large");
    }
    if (status_name == "INVALID_STATE") {
      return QStringLiteral("not-logged-in");
    }
    if (status_name == "INVALID_REQUEST" || status_name == "MALFORMED_FRAME" ||
        status_name == "UNSUPPORTED_VERSION" || status_name == "UNSUPPORTED") {
      return QStringLiteral("rejected");
    }
    return QStringLiteral("server");
  }
  if (Contains(detail, "before login") || Contains(detail, "needs a session")) {
    return QStringLiteral("not-logged-in");
  }
  if (Contains(detail, "already exists")) {
    return QStringLiteral("target-exists");
  }
  if (Contains(detail, "not connected") || Contains(detail, "cannot send") ||
      Contains(detail, "cannot read the response") ||
      Contains(detail, "connect")) {
    return QStringLiteral("network");
  }
  if (Contains(detail, "cannot open") || Contains(detail, "cannot create") ||
      Contains(detail, "is empty") || Contains(detail, "No such file")) {
    return QStringLiteral("local");
  }
  return QStringLiteral("unknown");
}

QString RemoteController::DescribeFailure(const QString& error_kind) {
  if (error_kind == QStringLiteral("credentials")) {
    return QStringLiteral("登录失败：用户名或密码不正确。");
  }
  if (error_kind == QStringLiteral("account-password")) {
    return QStringLiteral("当前密码不正确，账户与全部云端备份都没有被删除。");
  }
  if (error_kind == QStringLiteral("password-mismatch")) {
    return QStringLiteral("两次输入的密码不一致，请重新输入。");
  }
  if (error_kind == QStringLiteral("confirm-mismatch")) {
    return QStringLiteral("账户名不一致，请输入当前账户名以确认注销。");
  }
  if (error_kind == QStringLiteral("name-taken")) {
    return QStringLiteral("这个用户名已经被占用，换一个再试。");
  }
  if (error_kind == QStringLiteral("not-logged-in")) {
    return QStringLiteral("尚未登录，请先登录再操作。");
  }
  if (error_kind == QStringLiteral("not-found")) {
    return QStringLiteral("这个云端备份已经不存在了，刷新列表看看。");
  }
  if (error_kind == QStringLiteral("target-exists")) {
    return QStringLiteral("目标文件已经存在。换一个文件名，或勾选“允许覆盖”。");
  }
  if (error_kind == QStringLiteral("integrity")) {
    return QStringLiteral("文件完整性校验失败，这次传输的结果没有被采用。");
  }
  if (error_kind == QStringLiteral("forbidden")) {
    return QStringLiteral("没有权限访问这个云端备份。");
  }
  if (error_kind == QStringLiteral("too-large")) {
    return QStringLiteral("文件超过服务器允许的大小。");
  }
  if (error_kind == QStringLiteral("rejected")) {
    return QStringLiteral("服务器拒绝了这个请求（两端版本可能不一致）。");
  }
  if (error_kind == QStringLiteral("network")) {
    return QStringLiteral("网络连接中断，请确认服务器地址与端口后重试。");
  }
  if (error_kind == QStringLiteral("local")) {
    return QStringLiteral("本地文件不可用（不存在、不是普通文件，或者为空）。");
  }
  if (error_kind == QStringLiteral("server")) {
    return QStringLiteral("服务器暂时无法完成操作，请稍后重试。");
  }
  if (error_kind == QStringLiteral("busy")) {
    return QStringLiteral("已经有一个操作在进行，等它结束之后再试。");
  }
  return QStringLiteral("操作没有完成，请稍后重试。");
}

QString RemoteController::TitleForFailure(const QString& error_kind) {
  if (error_kind == QStringLiteral("credentials")) {
    return QStringLiteral("登录失败");
  }
  if (error_kind == QStringLiteral("account-password")) {
    return QStringLiteral("当前密码不正确");
  }
  if (error_kind == QStringLiteral("password-mismatch")) {
    return QStringLiteral("两次输入的密码不一致");
  }
  if (error_kind == QStringLiteral("confirm-mismatch")) {
    return QStringLiteral("账户名不一致");
  }
  if (error_kind == QStringLiteral("name-taken")) {
    return QStringLiteral("用户名已被占用");
  }
  if (error_kind == QStringLiteral("not-logged-in")) {
    return QStringLiteral("尚未登录");
  }
  if (error_kind == QStringLiteral("not-found")) {
    return QStringLiteral("云端备份不存在");
  }
  if (error_kind == QStringLiteral("target-exists")) {
    return QStringLiteral("目标文件已存在");
  }
  if (error_kind == QStringLiteral("integrity")) {
    return QStringLiteral("完整性校验失败");
  }
  if (error_kind == QStringLiteral("forbidden")) {
    return QStringLiteral("没有权限");
  }
  if (error_kind == QStringLiteral("too-large")) {
    return QStringLiteral("文件太大");
  }
  if (error_kind == QStringLiteral("rejected") ||
      error_kind == QStringLiteral("server")) {
    return QStringLiteral("服务器拒绝了请求");
  }
  if (error_kind == QStringLiteral("network")) {
    return QStringLiteral("网络连接中断");
  }
  if (error_kind == QStringLiteral("local")) {
    return QStringLiteral("本地文件不可用");
  }
  if (error_kind == QStringLiteral("busy")) {
    return QStringLiteral("操作正在进行");
  }
  return QStringLiteral("操作失败");
}

// ---- 状态条 / 忙碌 / 列表 ----

void RemoteController::SetStatus(const QString& kind, const QString& title,
                                 const QString& message) {
  if (status_kind_ == kind && status_title_ == title &&
      status_message_ == message) {
    return;
  }
  status_kind_ = kind;
  status_title_ = title;
  status_message_ = message;
  emit statusChanged();
}

void RemoteController::SetBusy(bool busy, const QString& action) {
  if (busy_ == busy && busy_action_ == action) {
    return;
  }
  busy_ = busy;
  busy_action_ = action;
  emit busyChanged();
}

void RemoteController::ResetIdleStatus() {
  if (busy_) {
    return;
  }
  if (authenticated_) {
    SetStatus(QStringLiteral("idle"),
              QStringLiteral("已登录：%1").arg(username_), QString());
    return;
  }
  // 未登录。这里**不**说"未连接"：BPNET1 是每次操作建立连接，没有长期连接，
  // 把那个短命的 socket 状态写成页面的常驻状态，用户只会读成"服务器挂了"。
  // 只有真的试过一次连接并且失败了，才说一句"不可达"。
  if (reachability_ == RemoteReachability::kUnreachable) {
    SetStatus(QStringLiteral("idle"), QStringLiteral("服务器暂时不可达"),
              QStringLiteral("上一次连接没有成功。请确认服务器正在运行，"
                             "地址与端口正确。"));
    return;
  }
  SetStatus(QStringLiteral("idle"), QStringLiteral("未登录"),
            QStringLiteral("填写服务器地址与账号后点“登录”。"));
}

void RemoteController::clearStatus() { ResetIdleStatus(); }

void RemoteController::SetSnapshots(
    const std::vector<RemoteSnapshotInfo>& snapshots) {
  // 服务端不保证顺序，最新的一律排在最前面：列表的第一行就是"最近一次备份"。
  std::vector<RemoteSnapshotInfo> ordered = snapshots;
  std::sort(
      ordered.begin(), ordered.end(),
      [](const RemoteSnapshotInfo& left, const RemoteSnapshotInfo& right) {
        return left.created_at > right.created_at;
      });
  QVariantList items;
  for (const RemoteSnapshotInfo& info : ordered) {
    QVariantMap item;
    item.insert(QStringLiteral("id"), QString::fromStdString(info.snapshot_id));
    item.insert(QStringLiteral("name"),
                QString::fromStdString(info.display_name));
    item.insert(QStringLiteral("sizeBytes"),
                static_cast<qulonglong>(info.size_bytes));
    item.insert(QStringLiteral("sizeText"), FormatSize(info.size_bytes));
    item.insert(QStringLiteral("createdText"), FormatTime(info.created_at));
    item.insert(QStringLiteral("sha256Short"),
                QString::fromStdString(info.sha256).left(12));
    items.append(item);
  }
  snapshot_items_ = items;
  emit snapshotsChanged();
}

// ---- 进度：后台线程写原子变量，主线程读 ----

void RemoteController::PublishProgress(const RemoteTransferProgress& p) {
  if (shutting_down_.load(std::memory_order_relaxed)) {
    return;
  }
  bytes_done_.store(p.bytes_done, std::memory_order_relaxed);
  bytes_total_.store(p.bytes_total, std::memory_order_relaxed);
  phase_.store(p.phase == "download" ? static_cast<int>(Phase::kDownload)
                                     : static_cast<int>(Phase::kUpload),
               std::memory_order_relaxed);
  progress_callbacks_.fetch_add(1, std::memory_order_relaxed);
  // 跨线程只投一个"来读一下"的通知：进度值走原子变量，不走信号参数，
  // 所以主线程读到的永远是一个自洽的快照，也不会在后台线程碰 QML 状态。
  QMetaObject::invokeMethod(
      this, [this]() { OnProgressNotify(); }, Qt::QueuedConnection);
}

void RemoteController::OnProgressNotify() {
  const int phase = phase_.load();
  transfer_active_ = phase != static_cast<int>(Phase::kNone);
  if (phase == static_cast<int>(Phase::kDownload)) {
    transfer_phase_text_ = QStringLiteral("正在下载");
  } else if (phase == static_cast<int>(Phase::kUpload)) {
    transfer_phase_text_ = QStringLiteral("正在上传");
  } else {
    transfer_phase_text_.clear();
  }
  if (!busy_) {
    emit progressChanged();
    return;
  }
  // 运行状态跟着阶段走：第一条进度回调到达之前是"连接 / 校验"，
  // 到了之后才是真正的"上传 / 下载"。
  const QString title = transfer_phase_text_.isEmpty()
                            ? busy_action_
                            : transfer_phase_text_ + QStringLiteral("备份");
  SetStatus(QStringLiteral("running"), title, progressText());
  emit progressChanged();
}

// ---- 输入校验 ----

bool RemoteController::AcceptEndpoint(const QString& host,
                                      const QString& port_text,
                                      const QString& username) {
  const QString trimmed_host = host.trimmed();
  if (trimmed_host.isEmpty()) {
    SetStatus(QStringLiteral("error"), QStringLiteral("服务器地址不能为空"),
              QStringLiteral("填写服务器地址，例如 127.0.0.1。"));
    return false;
  }
  bool port_ok = false;
  const int port = port_text.trimmed().toInt(&port_ok);
  if (!port_ok || port < 1 || port > 65535) {
    SetStatus(QStringLiteral("error"), QStringLiteral("端口不合法"),
              QStringLiteral("端口要填 1 到 65535 之间的整数。"));
    return false;
  }
  const std::string user = username.trimmed().toStdString();
  std::string user_error;
  // 复用共享校验器：规则只有一份，"3..64 字节、[A-Za-z0-9_.-]"不在 QML 里重写。
  if (!backupproject::net::IsValidUsername(user, &user_error)) {
    SetStatus(QStringLiteral("error"), QStringLiteral("用户名不合法"),
              QStringLiteral("用户名要 3 到 64 个字节，只能用字母、数字、"
                             "点、下划线或减号。"));
    std::fprintf(stderr, "[remote] 用户名不合法：%s\n", user_error.c_str());
    return false;
  }
  const QString trimmed_username = QString::fromStdString(user);
  if (endpoint_.host != trimmed_host.toStdString() ||
      endpoint_.port != static_cast<std::uint16_t>(port) ||
      username_ != trimmed_username) {
    endpoint_.host = trimmed_host.toStdString();
    endpoint_.port = static_cast<std::uint16_t>(port);
    username_ = trimmed_username;
    emit endpointChanged();
  }
  return true;
}

bool RemoteController::AcceptPassword(const QString& password) {
  const std::string bytes = password.toStdString();
  // 长度下限来自共享常量（服务端注册时的同一条策略），数字不在 QML 里重写。
  if (bytes.size() < backupproject::net::kMinPasswordBytes) {
    SetStatus(QStringLiteral("error"), QStringLiteral("密码太短"),
              QStringLiteral("密码至少要 %1 个字符。")
                  .arg(backupproject::net::kMinPasswordBytes));
    return false;
  }
  std::string password_error;
  if (!backupproject::net::IsValidPassword(bytes, &password_error)) {
    SetStatus(QStringLiteral("error"), QStringLiteral("密码不合法"),
              QStringLiteral("密码不能超过 256 字节，也不能包含空字符。"));
    std::fprintf(stderr, "[remote] 密码不合法：%s\n", password_error.c_str());
    return false;
  }
  return true;
}

bool RemoteController::BeginOperation(const QString& action_text,
                                      bool need_login) {
  if (busy_) {
    // 控制器层的真闸门：QML 的 enabled 只是可见性，同一个回合里的第二个请求
    // 在这里被拒——不排队，也不和正在跑的那一个抢同一条连接。
    last_error_kind_ = QStringLiteral("busy");
    SetStatus(QStringLiteral("warning"), QStringLiteral("操作正在进行"),
              QStringLiteral("正在%1，等它结束之后再试。")
                  .arg(busy_action_.isEmpty() ? QStringLiteral("处理上一个请求")
                                              : busy_action_));
    return false;
  }
  if (need_login && !authenticated_) {
    last_error_kind_ = QStringLiteral("not-logged-in");
    SetStatus(QStringLiteral("error"), TitleForFailure(last_error_kind_),
              DescribeFailure(last_error_kind_));
    return false;
  }
  last_error_kind_ = QStringLiteral("none");
  last_detail_.clear();
  SetBusy(true, action_text);
  SetStatus(QStringLiteral("running"), action_text, QString());
  return true;
}

void RemoteController::Submit(const RemoteRequest& request) {
  // 进度归零：新一次操作的百分比不能沿用上一次的。
  bytes_done_.store(0);
  bytes_total_.store(0);
  phase_.store(static_cast<int>(Phase::kNone));
  transfer_active_ = false;
  transfer_phase_text_.clear();
  emit progressChanged();
  // QtConcurrent::run 的返回值就是这次操作的结果：后台线程写它，
  // 主线程在 OnOperationFinished 里读它，中间没有第二份拷贝。
  watcher_.setFuture(QtConcurrent::run(&RemoteController::RunOperation,
                                       &client_, request, this));
}

// ---- 后台线程：真正的网络调用全在这里 ----

RemoteOpResult RemoteController::RunOperation(
    backupproject::net::RemoteArchiveClient* client, RemoteRequest request,
    RemoteController* progress_sink) {
  RemoteOpResult result;
  result.kind = request.kind;
  const backupproject::net::RemoteProgressCallback progress =
      [progress_sink](const RemoteTransferProgress& p) {
        if (progress_sink != nullptr) {
          progress_sink->PublishProgress(p);
        }
      };
  std::string error;
  switch (request.kind) {
    case RemoteOpResult::Kind::kRegister:
      if (!client->connected() && !client->Connect(request.endpoint, &error)) {
        result.ok = false;
        // 连不上就是"不可达"的证据：这是本页唯一会得出这个结论的地方，
        // 不靠任何猜测或定时探测。
        result.reachability = RemoteReachability::kUnreachable;
        break;
      }
      result.reachability = RemoteReachability::kReachable;
      result.ok = client->Register(request.username, request.password, &error);
      break;
    case RemoteOpResult::Kind::kLogin:
      if (!client->connected() && !client->Connect(request.endpoint, &error)) {
        result.ok = false;
        result.reachability = RemoteReachability::kUnreachable;
        break;
      }
      result.reachability = RemoteReachability::kReachable;
      result.ok = client->Login(request.username, request.password, &error);
      break;
    case RemoteOpResult::Kind::kList:
      result.ok = client->List(&result.snapshots, &error);
      break;
    case RemoteOpResult::Kind::kUpload:
      result.ok =
          client->UploadArchiveFile(request.local_path, request.display_name,
                                    progress, &result.archive, &error);
      break;
    case RemoteOpResult::Kind::kDownload:
      result.ok = client->DownloadArchiveFile(
          request.snapshot_id, request.target_path, request.allow_overwrite,
          progress, &result.archive, &error);
      break;
    case RemoteOpResult::Kind::kDelete:
      result.ok = client->Delete(request.snapshot_id, &error);
      break;
    case RemoteOpResult::Kind::kDeleteAccount:
      result.ok = client->DeleteAccount(request.password, &error);
      break;
  }
  if (!result.ok) {
    result.detail =
        error.empty() ? std::string("(the client reported no reason)") : error;
    std::uint32_t status = 0;
    if (ResolveSharedStatus(error, &status)) {
      result.status_name = backupproject::net::StatusName(status);
    }
    // 归类必须在这里做：ApplyResult 只认 error_kind 与 message，
    // 少了这一步就会把"失败"当成"成功"报出去。
    result.error_kind =
        ClassifyFailure(result.status_name, result.detail, result.kind);
    result.message = DescribeFailure(result.error_kind);
  }
  return result;
}

// ---- 主线程：结果落地 ----

void RemoteController::OnOperationFinished() {
  const RemoteOpResult result = watcher_.result();
  ApplyResult(result);

  SetBusy(false, QString());
  transfer_active_ = false;
  transfer_phase_text_.clear();
  emit progressChanged();
  emit operationFinished(KindName(result.kind), result.ok);

  // 登录成功、上传成功、删除成功之后自动读一次列表：列表永远是"刚刚发生过
  // 什么"的样子，不需要用户再点一次刷新。这是一次新的、同样受 busy 保护的
  // 操作，不是在同一次操作里做两件事。
  const bool chain = result.ok && !shutting_down_.load() &&
                     (result.kind == RemoteOpResult::Kind::kLogin ||
                      result.kind == RemoteOpResult::Kind::kUpload ||
                      result.kind == RemoteOpResult::Kind::kDelete);
  if (chain) {
    refreshList();
  }
}

void RemoteController::ApplyResult(const RemoteOpResult& result) {
  if (result.reachability != RemoteReachability::kUnknown) {
    // 只在这条后台线程真的试过连接时更新：登录 / 注册会连接，其它操作复用
    // 已有连接，因此对"可达性"没有新证据。
    SetReachability(result.reachability);
  }
  if (result.ok) {
    last_error_kind_ = QStringLiteral("none");
    last_detail_.clear();
  } else {
    last_error_kind_ = result.error_kind;
    last_detail_ = result.detail;
    std::fprintf(
        stderr, "[remote] %s 没有完成：%s%s%s\n",
        KindName(result.kind).toUtf8().constData(),
        result.status_name.empty() ? "" : result.status_name.c_str(),
        result.status_name.empty() ? "" : " ",
        result.detail.empty() ? "(没有更多信息)" : result.detail.c_str());
    // 连接已经不可信时如实降级：不让界面继续显示"已登录"，否则用户会对着
    // 一个假的登录状态反复重试。
    if (result.error_kind == QStringLiteral("network") ||
        result.error_kind == QStringLiteral("not-logged-in")) {
      if (authenticated_) {
        authenticated_ = false;
        emit sessionChanged();
      }
      client_.Disconnect();
    }
    SetStatus(QStringLiteral("error"), TitleForFailure(result.error_kind),
              result.message);
    return;
  }

  switch (result.kind) {
    case RemoteOpResult::Kind::kRegister:
      authenticated_ = false;
      SetStatus(QStringLiteral("success"), QStringLiteral("注册成功"),
                QStringLiteral("账号已经创建，现在可以点“登录”。"));
      break;
    case RemoteOpResult::Kind::kLogin:
      if (!authenticated_) {
        authenticated_ = true;
        emit sessionChanged();
      }
      SetStatus(QStringLiteral("success"), QStringLiteral("登录成功"),
                QStringLiteral("正在读取云端备份列表…"));
      break;
    case RemoteOpResult::Kind::kList: {
      SetSnapshots(result.snapshots);
      list_loaded_ = true;
      if (result.snapshots.empty()) {
        list_summary_ = QStringLiteral("云端还没有备份");
        SetStatus(QStringLiteral("idle"), QStringLiteral("云端还没有备份"),
                  QStringLiteral("点“上传当前备份”把一份本地备份放到云端。"));
      } else {
        list_summary_ =
            QStringLiteral("云端共 %1 个备份").arg(result.snapshots.size());
        SetStatus(QStringLiteral("success"),
                  QStringLiteral("云端备份列表已更新"), list_summary_);
      }
      emit snapshotsChanged();
      break;
    }
    case RemoteOpResult::Kind::kUpload:
      SetStatus(QStringLiteral("success"), QStringLiteral("上传完成"),
                QStringLiteral("%1（%2）已经上传到云端。")
                    .arg(QString::fromStdString(result.archive.display_name),
                         FormatSize(result.archive.size_bytes)));
      break;
    case RemoteOpResult::Kind::kDownload:
      SetStatus(QStringLiteral("success"), QStringLiteral("下载完成"),
                QStringLiteral("%1（%2）已经保存到本机，"
                               "可以到“备份管理”里恢复它。")
                    .arg(QString::fromStdString(result.archive.display_name),
                         FormatSize(result.archive.size_bytes)));
      break;
    case RemoteOpResult::Kind::kDelete:
      SetStatus(QStringLiteral("success"), QStringLiteral("已删除"),
                QStringLiteral("云端的那份备份已经删除。"));
      break;
    case RemoteOpResult::Kind::kDeleteAccount:
      // 账户已经不存在了：本机内存里的会话、口令与列表全部清掉，界面回到
      // "未登录"。云端数据由服务端删除，这里不做任何本地清理。
      authenticated_ = false;
      client_.Disconnect();
      password_.fill(QChar(0));
      password_.clear();
      password_.squeeze();
      list_loaded_ = false;
      SetSnapshots(std::vector<RemoteSnapshotInfo>());
      list_summary_ = QStringLiteral("尚未登录");
      SetReachability(RemoteReachability::kUnknown);
      emit sessionChanged();
      emit snapshotsChanged();
      SetStatus(QStringLiteral("success"), QStringLiteral("账户已注销"),
                QStringLiteral("该账户以及它的全部云端备份已经被删除。"));
      break;
  }
}

// ---- QML 入口 ----

bool RemoteController::registerAccount(const QString& host,
                                       const QString& port_text,
                                       const QString& username,
                                       const QString& password,
                                       const QString& confirm_password) {
  // 两次输入必须一致：不一致时**一个字节都不发**。这条检查放在最前面
  // （早于 AcceptEndpoint），所以连"上一次生效的地址"都不会被这次输入改掉。
  if (password != confirm_password) {
    last_error_kind_ = QStringLiteral("password-mismatch");
    SetStatus(QStringLiteral("error"), QStringLiteral("两次输入的密码不一致"),
              QStringLiteral("请重新输入，两个密码框必须完全相同。"));
    return false;
  }
  if (!AcceptEndpoint(host, port_text, username)) {
    return false;
  }
  if (!AcceptPassword(password)) {
    return false;
  }
  if (!BeginOperation(QStringLiteral("正在注册账号"),
                      /*need_login=*/false)) {
    return false;
  }
  password_ = password;
  RemoteRequest request;
  request.kind = RemoteOpResult::Kind::kRegister;
  request.endpoint = endpoint_;
  request.username = username_.toStdString();
  request.password = password.toStdString();
  Submit(request);
  return true;
}

bool RemoteController::login(const QString& host, const QString& port_text,
                             const QString& username, const QString& password) {
  if (!AcceptEndpoint(host, port_text, username)) {
    return false;
  }
  if (!AcceptPassword(password)) {
    return false;
  }
  if (!BeginOperation(QStringLiteral("正在登录"),
                      /*need_login=*/false)) {
    return false;
  }
  password_ = password;
  RemoteRequest request;
  request.kind = RemoteOpResult::Kind::kLogin;
  request.endpoint = endpoint_;
  request.username = username_.toStdString();
  request.password = password.toStdString();
  Submit(request);
  return true;
}

void RemoteController::logoutLocal() {
  if (busy_) {
    // 传输中不给退：会话正是这次传输的一部分。
    SetStatus(QStringLiteral("warning"), QStringLiteral("操作正在进行"),
              QStringLiteral("等当前操作结束之后再退出登录。"));
    return;
  }
  client_.Disconnect();
  // 断开之后"服务器可不可达"重新变成未知：界面不再声称任何结论。
  SetReachability(RemoteReachability::kUnknown);
  if (authenticated_) {
    authenticated_ = false;
    emit sessionChanged();
  }
  password_.fill(QChar(0));
  password_.clear();
  password_.squeeze();
  list_loaded_ = false;
  SetSnapshots(std::vector<RemoteSnapshotInfo>());
  list_summary_ = QStringLiteral("尚未登录");
  emit snapshotsChanged();
  SetStatus(QStringLiteral("idle"), QStringLiteral("已退出登录"),
            QStringLiteral("本机内存里的登录状态已经清除。"));
}

bool RemoteController::refreshList() {
  if (!BeginOperation(QStringLiteral("正在读取云端备份列表"),
                      /*need_login=*/true)) {
    return false;
  }
  RemoteRequest request;
  request.kind = RemoteOpResult::Kind::kList;
  request.endpoint = endpoint_;
  Submit(request);
  return true;
}

bool RemoteController::uploadArchive(const QString& local_path,
                                     const QString& display_name) {
  if (local_path.isEmpty()) {
    SetStatus(QStringLiteral("error"), QStringLiteral("还没有选择本地备份"),
              QStringLiteral("先点“选择本地备份”，挑一个本机上的 .bak 文件。"));
    return false;
  }
  const QFileInfo info(local_path);
  if (!info.exists() || !info.isFile()) {
    SetStatus(QStringLiteral("error"), QStringLiteral("本地备份不可用"),
              QStringLiteral("这个路径不存在，或者不是一个普通文件：%1")
                  .arg(info.fileName()));
    return false;
  }
  // 名称留空时用文件名，与 backupctl remote upload 的默认值一致。
  //
  // 注意：这里**不**重写用户填的路径，也不做任何 trim——Linux 下空格和大小写
  // 都是文件名的一部分。
  const QString chosen_name = display_name.trimmed().isEmpty()
                                  ? info.fileName()
                                  : display_name.trimmed();
  std::string name_error;
  if (!backupproject::net::IsValidDisplayName(chosen_name.toStdString(),
                                              &name_error)) {
    SetStatus(
        QStringLiteral("error"), QStringLiteral("备份名称不合法"),
        QStringLiteral("名称不能为空，也不能超过 255 字节或含控制字符。"));
    std::fprintf(stderr, "[remote] 名称不合法：%s\n", name_error.c_str());
    return false;
  }
  if (!BeginOperation(QStringLiteral("正在连接并校验本地备份"),
                      /*need_login=*/true)) {
    return false;
  }
  RemoteRequest request;
  request.kind = RemoteOpResult::Kind::kUpload;
  request.endpoint = endpoint_;
  request.local_path = local_path.toStdString();
  request.display_name = chosen_name.toStdString();
  Submit(request);
  return true;
}

bool RemoteController::downloadArchive(const QString& snapshot_id,
                                       const QString& target_path,
                                       bool allow_overwrite) {
  if (snapshot_id.isEmpty()) {
    SetStatus(QStringLiteral("error"), QStringLiteral("没有选中云端备份"),
              QStringLiteral("先在列表里选中一行，再点“下载”。"));
    return false;
  }
  if (target_path.isEmpty()) {
    SetStatus(QStringLiteral("error"), QStringLiteral("还没有选择保存位置"),
              QStringLiteral("点“选择保存位置”，指定下载到哪里。"));
    return false;
  }
  if (!BeginOperation(QStringLiteral("正在连接并准备下载"),
                      /*need_login=*/true)) {
    return false;
  }
  RemoteRequest request;
  request.kind = RemoteOpResult::Kind::kDownload;
  request.endpoint = endpoint_;
  request.snapshot_id = snapshot_id.toStdString();
  request.target_path = target_path.toStdString();
  request.allow_overwrite = allow_overwrite;
  Submit(request);
  return true;
}

bool RemoteController::deleteSnapshot(const QString& snapshot_id) {
  if (snapshot_id.isEmpty()) {
    SetStatus(QStringLiteral("error"), QStringLiteral("没有选中云端备份"),
              QStringLiteral("先在列表里选中一行，再点“删除”。"));
    return false;
  }
  if (!BeginOperation(QStringLiteral("正在删除云端备份"),
                      /*need_login=*/true)) {
    return false;
  }
  RemoteRequest request;
  request.kind = RemoteOpResult::Kind::kDelete;
  request.endpoint = endpoint_;
  request.snapshot_id = snapshot_id.toStdString();
  Submit(request);
  return true;
}

bool RemoteController::deleteAccount(const QString& password,
                                       const QString& username_confirmation) {
  // 二次确认：必须逐字敲出当前账户名。少一个字符都不发请求——注销是不可撤销
  // 的服务端删除，不能是一个"点快了就没了"的按钮。
  if (username_confirmation.trimmed() != username_) {
    last_error_kind_ = QStringLiteral("confirm-mismatch");
    SetStatus(QStringLiteral("error"), QStringLiteral("账户名不一致"),
              QStringLiteral("请输入当前账户名 %1 以确认注销。").arg(username_));
    return false;
  }
  if (!AcceptPassword(password)) {
    return false;
  }
  if (!BeginOperation(QStringLiteral("正在注销账户"), /*need_login=*/true)) {
    return false;
  }
  RemoteRequest request;
  request.kind = RemoteOpResult::Kind::kDeleteAccount;
  request.endpoint = endpoint_;
  request.password = password.toStdString();
  Submit(request);
  return true;
}

bool RemoteController::waitForIdle(int timeout_ms) {
  QEventLoop loop;
  QTimer poll;
  poll.setInterval(10);
  QObject::connect(&poll, &QTimer::timeout, &loop, [this, &loop]() {
    if (!busy_) {
      loop.quit();
    }
  });
  QTimer guard;
  guard.setSingleShot(true);
  QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
  if (!busy_) {
    return true;
  }
  poll.start();
  guard.start(timeout_ms);
  loop.exec();
  return !busy_;
}

QString RemoteController::localPathFromUrl(const QUrl& url) const {
  return url.toLocalFile();
}

QUrl RemoteController::fileDialogStartUrl(const QString& path) const {
  const QFileInfo info(path);
  if (!path.isEmpty()) {
    if (info.isDir()) {
      return QUrl::fromLocalFile(info.absoluteFilePath());
    }
    if (info.exists()) {
      return QUrl::fromLocalFile(info.absolutePath());
    }
  }
  return QUrl::fromLocalFile(QDir::homePath());
}

QString RemoteController::suggestedDownloadName(
    const QString& display_name) const {
  // 建议名就是云端那份备份的显示名（上传时用的就是本地文件名）。
  // 显示名允许含 '/'，所以只取最后一段，避免它意外变成一条路径。
  const QString name = QFileInfo(display_name.trimmed()).fileName();
  if (name.isEmpty()) {
    return QStringLiteral("remote-backup.bak");
  }
  return name;
}

}  // namespace backup_modern
