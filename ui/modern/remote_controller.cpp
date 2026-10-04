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

// PR #21 产品级远端备份 / 链恢复：与 backupctl remote backup / remote restore
// 用的是**同一个** core。GUI 不重新实现任何增量判断（有没有可信基线、父是谁、
// 代数、lineage、要不要 bootstrap 缓存）——它只把"源目录 + 策略"和
// "目标快照 + 目标目录"交给这一层。
#include "incremental_backup.h"
#include "incremental_restore.h"
#include "network_protocol.h"
#include "remote_auth.h"
#include "remote_incremental.h"
#include "secure_transport.h"

#if defined(Q_OS_UNIX)
#include <signal.h>  // 只给自检用的 killOwnedTunnelForTest
#endif

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

// 尽力而为地抹掉内存里的一份口令。std::string 也可能有副本（分配器、COW），
// 所以这只是缩小窗口，不是内存加密——与 password_ 的擦除同一条边界。
void WipeSecret(std::string* secret) {
  if (secret == nullptr || secret->empty()) {
    return;
  }
  volatile char* bytes = &(*secret)[0];
  for (std::size_t index = 0; index < secret->size(); ++index) {
    bytes[index] = 0;
  }
  secret->clear();
}

// 本地恢复核心的原始诊断串 -> 给用户看的"原因"。两件事：
//
//   1. 只留"为什么"那一句：core 的前缀（"raw restore: destination rejected —"）
//      是给归类用的，不是给用户读的；
//   2. 把本机临时工作目录（.../raw-restore-XXXXXX/xxx.bak）换成占位符：用户
//      不需要、也不该在界面上看到本机的临时路径。
QString SanitizeLocalRestoreReason(const std::string& detail) {
  QString text = QString::fromStdString(detail);
  const int dash = text.lastIndexOf(QStringLiteral("— "));
  const int colon = text.lastIndexOf(QStringLiteral("："));
  const int cut = dash >= 0 ? dash + 2 : (colon >= 0 ? colon + 1 : -1);
  if (cut > 0) {
    text = text.mid(cut);
  }
  int guard = 0;
  while (guard++ < 8) {
    const int at = text.indexOf(QStringLiteral("raw-restore-"));
    if (at < 0) {
      break;
    }
    // 码点直接写出来：QLatin1Char 只接受单字节，中文标点会变成 overflow 警告
    // （而这个仓库的构建要求 0 警告）。
    const QChar kFullwidthColon(0xFF1A);       // ：
    const QChar kFullwidthCloseParen(0xFF09);  // ）
    int begin = at;
    while (begin > 0 && !text.at(begin - 1).isSpace() &&
           text.at(begin - 1) != kFullwidthColon &&
           text.at(begin - 1) != QLatin1Char('/')) {
      --begin;
    }
    int end = at;
    while (end < text.size() && !text.at(end).isSpace() &&
           text.at(end) != kFullwidthCloseParen &&
           text.at(end) != QLatin1Char(')')) {
      ++end;
    }
    text.replace(begin, end - begin, QStringLiteral("(临时归档)"));
  }
  text = text.simplified();
  if (text.size() > 160) {
    text = text.left(160) + QStringLiteral("…");
  }
  return text;
}

// 把核心校验器给出的用户名原因翻译成用户看得懂的一句话。这里是**唯一**的
// 翻译点：登录与注册共用它，所以两张表单不可能对同一段输入说出不同的话。
//
// 长度与字符集的规则只在 ValidateUsername 里实现一次；这里不判断长度，也不
// 遍历字符。允许的字符全部是单字节 ASCII，所以"字节"与"字符"是同一个数，
// 但对用户说"字符"更自然。
QString UsernameReasonText(backupproject::net::UsernameValidation reason) {
  using backupproject::net::UsernameValidation;
  switch (reason) {
    case UsernameValidation::kEmpty:
      return QStringLiteral("请输入用户名");
    case UsernameValidation::kTooShort:
    case UsernameValidation::kTooLong:
      return QStringLiteral("用户名长度需要为 %1～%2 个字符")
          .arg(backupproject::net::kMinUsernameBytes)
          .arg(backupproject::net::kMaxUsernameBytes);
    case UsernameValidation::kInvalidCharacter:
      return QStringLiteral("用户名只能包含字母、数字、点、下划线或减号");
    case UsernameValidation::kOk:
      break;
  }
  return QString();
}

// 终端上仍然打印核心给的英文原因（诊断用），并带上分类名，便于对数。
const char* UsernameReasonName(backupproject::net::UsernameValidation reason) {
  using backupproject::net::UsernameValidation;
  switch (reason) {
    case UsernameValidation::kEmpty:
      return "empty";
    case UsernameValidation::kTooShort:
      return "too-short";
    case UsernameValidation::kTooLong:
      return "too-long";
    case UsernameValidation::kInvalidCharacter:
      return "invalid-character";
    case UsernameValidation::kOk:
      break;
  }
  return "ok";
}

}  // namespace

RemoteController::RemoteController(QObject* parent) : QObject(parent) {
  endpoint_.host = backupproject::net::kDefaultRemoteHost;
  endpoint_.port = backupproject::net::kDefaultRemotePort;
  // pin 没有默认值（空 = 不能连接）。这里只是把"同一个值的两个落点"从一开始
  // 就对齐：调用方 setServerKeyPin() 之后两处一起更新。
  endpoint_.server_key_pin = serverKeyPin().toStdString();
  QObject::connect(&watcher_, &QFutureWatcher<RemoteOpResult>::finished, this,
                   &RemoteController::OnOperationFinished);
  // 安全通道状态机的唯一落点：有请求在等它就接着发，只是"建立连接"在等就
  // 结束忙碌。订阅放在构造函数里，保证**任何**状态变化都不会被漏掉。
  QObject::connect(&tunnel_, &SshTunnelManager::stateChanged, this,
                   &RemoteController::OnTunnelStateChanged);
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
  // GUI 退出不允许留下孤儿 ssh -N：本程序启动的那一条在这里被有界地收掉
  // （terminate → 有界等待 → 必要时 kill）。用户自己在外面开的隧道不属于
  // 本对象，一个字节都不会动它。
  tunnel_.Stop();
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
    case RemoteOpResult::Kind::kBackup:
      return QStringLiteral("backup");
    case RemoteOpResult::Kind::kRestore:
      return QStringLiteral("restore");
    case RemoteOpResult::Kind::kRestoreRaw:
      return QStringLiteral("restore-raw");
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
    if (status_name == "CHAIN_CONFLICT") {
      // 服务端在链的边界上说不（父已经有一个孩子 / 代数超上限 / lineage
      // 不符）。
      // 这不是"网络失败"，也不是"服务器忙"：界面必须说清"本次备份没有创建"。
      return QStringLiteral("chain-conflict");
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
  // 原始归档单独恢复（kRestoreRaw）的三个专有失败。必须排在通用规则**之前**：
  // 那些原因里含"依赖链""加密"等字样，落到通用规则上会被归成 integrity 之类，
  // 用户就拿不到"改用产品级恢复 / 请填密码 /
  // 这不是备份归档"这三句可照做的提示。 标记来自共享
  // core（remote_incremental.cpp），是稳定的英文前缀。
  if (Contains(detail, "raw restore: not a supported archive")) {
    return QStringLiteral("raw-unsupported");
  }
  if (Contains(detail, "raw restore: needs a password")) {
    return QStringLiteral("raw-password");
  }
  if (Contains(detail, "raw restore: download failed")) {
    return QStringLiteral("raw-download");
  }
  if (Contains(detail, "raw restore: delta needs its chain")) {
    return QStringLiteral("raw-delta");
  }
  // 密码错 / 归档损坏 / 版本不支持 / 目标目录契约：这四类由 core 用**它自己的**
  // 诊断串区分（见 remote_incremental.cpp 的 DescribeLocalRestoreFailure），
  // 不是 GUI 猜的。尤其是"密码错"与"payload 被改动"：后者在 HMAC 之前就被
  // payload_sha256 拦下，所以可以如实分开说；而"密码错"与"容器头被改动"在
  // 密码学上不可区分，raw-auth 的文案必须把两种可能都写出来。
  if (Contains(detail, "raw restore: authentication failed")) {
    return QStringLiteral("raw-auth");
  }
  if (Contains(detail, "raw restore: corrupted archive")) {
    return QStringLiteral("raw-corrupt");
  }
  if (Contains(detail, "raw restore: unsupported version")) {
    return QStringLiteral("raw-version");
  }
  if (Contains(detail, "raw restore: destination rejected")) {
    return QStringLiteral("raw-destination");
  }
  if (Contains(detail, "raw restore:")) {
    return QStringLiteral("raw-restore");
  }
  if (Contains(detail, "before login") || Contains(detail, "needs a session")) {
    return QStringLiteral("not-logged-in");
  }
  if (Contains(detail, "already exists")) {
    return QStringLiteral("target-exists");
  }
  // ---- 连接层：失败必须分层报，不能全压成一句"网络错误"（PR #22）----
  //
  // 层次从下到上是：
  //   本地/隧道 -> BPSEC1 握手 -> 服务端身份(pin) -> 账号口令 -> 服务端业务
  // 每一层都有自己的一句话与自己的下一步动作。人工验收里"服务器暂时不可达"
  // 这一句同时盖住了四五种完全不同的原因，用户无从下手。
  //
  // (1) 服务端身份 pin 不符：这是"连错服务器 / 服务器换了密钥"，不是网络抖动。
  //     单独一类，界面才能给出"去找管理员核对指纹"这句话，而不是让人一直重试。
  if (Contains(detail, "server-key-mismatch") ||
      Contains(detail, "服务端传输身份公钥与本地 pin") ||
      Contains(detail, "ServerHello 里的身份公钥与本地 pin") ||
      Contains(detail, "与本地 pin 的指纹不一致")) {
    return QStringLiteral("pin-mismatch");
  }
  // (2) 根本没有 pin：本地配置缺失，与服务端无关。
  if (Contains(detail, "no-server-key-pin") ||
      Contains(detail, "没有配置服务端传输公钥")) {
    return QStringLiteral("pin-missing");
  }
  // (3) 隧道通了、但 BPSEC1 握手失败。与"pin 不符"**分开**：前者是两端没能
  //     就会话密钥达成一致（改过的字节、版本不匹配），后者是服务器身份不对。
  //     两者要用户做的事完全不同。
  if (Contains(detail, "BPSEC1") || Contains(detail, "握手") ||
      Contains(detail, "X25519") || Contains(detail, "加密通道状态") ||
      Contains(detail, "加密记录")) {
    return QStringLiteral("handshake");
  }
  // (4) 端口拒绝连接。直连模式下这是"服务器没起 / 端口不对"；ssh 模式下
  //     最常见的原因是**本地通道没起来**，所以文案要把两种可能都写出来。
  if (Contains(detail, "cannot connect to") &&
      Contains(detail, "Connection refused")) {
    return QStringLiteral("connect-refused");
  }
  if (Contains(detail, "not connected") || Contains(detail, "cannot send") ||
      Contains(detail, "cannot read the response") ||
      Contains(detail, "connect")) {
    return QStringLiteral("network");
  }
  // 材料包这一层的失败（中文原因来自 snapshot_bundle.cpp /
  // remote_incremental.cpp）：
  // 解包失败、成员校验失败、链解析失败都归到"材料不可信"，而不是笼统的
  // unknown。
  if (Contains(detail, "BPSNAP1") || Contains(detail, "材料包") ||
      Contains(detail, "解包")) {
    return QStringLiteral("not-a-bundle");
  }
  if (Contains(detail, "依赖链") || Contains(detail, "lineage") ||
      Contains(detail, "generation")) {
    return QStringLiteral("integrity");
  }
  if (Contains(detail, "cannot open") || Contains(detail, "cannot create") ||
      Contains(detail, "is empty") || Contains(detail, "No such file") ||
      Contains(detail, "不是目录") || Contains(detail, "源目录")) {
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
    // 与 SurfaceFailureMessage 里的注册文案保持一致：同一个事实只有一种说法。
    return QStringLiteral("该用户名已被使用，请更换用户名。");
  }
  if (error_kind == QStringLiteral("not-logged-in")) {
    return QStringLiteral("尚未登录或登录状态已经失效，请重新登录后再试。");
  }
  if (error_kind == QStringLiteral("not-found")) {
    return QStringLiteral("这个云端备份已经不存在了，刷新列表看看。");
  }
  if (error_kind == QStringLiteral("pin-mismatch")) {
    return QStringLiteral(
        "服务器身份校验没有通过：当前填的指纹与服务器上的身份不符。"
        "请找服务器管理员核对“服务器身份指纹”，不要在这里反复重试。");
  }
  if (error_kind == QStringLiteral("target-exists")) {
    return QStringLiteral("目标文件已经存在。换一个文件名，或勾选“允许覆盖”。");
  }
  if (error_kind == QStringLiteral("integrity")) {
    return QStringLiteral("文件完整性校验失败，这次传输的结果没有被采用。");
  }
  if (error_kind == QStringLiteral("chain-conflict")) {
    return QStringLiteral(
        "服务端拒绝了这个链关系（同一个父已经有一条增量，或者代数超过上限）："
        "本次远端备份没有创建任何东西，可以直接再试一次（会重新判断基线）。");
  }
  if (error_kind == QStringLiteral("not-a-bundle")) {
    return QStringLiteral(
        "这一条不是远端备份链的材料包（它是原始归档上传）：不能用链恢复，"
        "请改用“下载归档”。");
  }
  // ---- 原始归档恢复：每一种失败各有各的动作 ----
  //
  // 面向用户的文案只有中文，不含内部错误码（那些留在诊断详情里）。
  if (error_kind == QStringLiteral("raw-unsupported")) {
    return QStringLiteral(
        "该文件不是受支持的备份归档（按内容识别，与文件名、扩展名无关）。"
        "目标目录没有被创建，也没有留下任何文件。");
  }
  if (error_kind == QStringLiteral("raw-password")) {
    return QStringLiteral("这份备份已加密，请输入恢复密码。");
  }
  if (error_kind == QStringLiteral("raw-auth")) {
    // core 只能证明"这把钥匙不对"：密码错、或者容器头被改过。不许只说前者。
    return QStringLiteral("恢复密码错误，或备份完整性校验失败，请重新输入。");
  }
  if (error_kind == QStringLiteral("raw-corrupt")) {
    return QStringLiteral(
        "备份归档已损坏或完整性校验失败：它认得出是我们的备份格式，但内容与"
        "它自己的声明不符，无法恢复。目标目录没有被改动。");
  }
  if (error_kind == QStringLiteral("raw-version")) {
    return QStringLiteral(
        "当前版本不支持该备份格式（归档是更新版本写出来的）。"
        "请用生成它的那个版本恢复。");
  }
  if (error_kind == QStringLiteral("raw-destination")) {
    return QStringLiteral(
        "目标目录不能用于恢复：本地恢复要求它是一个不存在的位置，或者一个"
        "空目录。目标目录没有被改动。");
  }
  if (error_kind == QStringLiteral("raw-delta")) {
    return QStringLiteral(
        "这是增量备份的一部分，缺少父备份，无法单独恢复。"
        "请使用远端备份链中的“恢复”。");
  }
  if (error_kind == QStringLiteral("raw-download")) {
    return QStringLiteral(
        "下载失败：网络中断，或者字节没有通过 SHA-256 校验。"
        "本地没有留下任何文件，可以直接再试一次。");
  }
  if (error_kind == QStringLiteral("raw-restore")) {
    return QStringLiteral(
        "恢复失败：这份备份的内容与它自己的声明不一致。"
        "目标目录没有被改动，也没有留下任何文件。");
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
  // ---- PR #22：连接层的每一种失败各有各的说法与下一步 ----
  //
  // ssh-* / tunnel-not-ready 的文案只有一份，在 SshTunnelManager 里
  // （FailureUserText）。这里按 kind 反查，避免同一个事实写两遍、改一处忘
  // 一处。
  const SshTunnelManager::Failure tunnel_failure =
      SshTunnelManager::FailureFromKindName(error_kind);
  if (tunnel_failure != SshTunnelManager::Failure::kNone) {
    return SshTunnelManager::FailureUserText(tunnel_failure);
  }
  if (error_kind == QStringLiteral("tunnel-not-ready")) {
    return QStringLiteral(
        "安全通道还没有建立，所以这次操作没有发出去。"
        "点“建立连接”先把 SSH 安全通道建起来，或者改用直连模式。");
  }
  if (error_kind == QStringLiteral("handshake")) {
    return QStringLiteral(
        "已经连上服务器，但 BPSEC1 安全握手没有通过：两端没有就会话密钥"
        "达成一致（握手字节被改动、或两端版本不一致）。"
        "这次操作一个字节都没有发出去，云端数据没有变化。");
  }
  if (error_kind == QStringLiteral("pin-missing")) {
    return QStringLiteral(
        "还没有配置服务器身份指纹，客户端拒绝连接（不做首次连接自动信任）。"
        "请向服务器管理员索取 sha256:… 指纹并填进“服务器身份指纹”。");
  }
  if (error_kind == QStringLiteral("connect-refused")) {
    return QStringLiteral(
        "目标端口拒绝连接。如果服务端只监听它自己的回环地址，"
        "请先用“SSH 安全通道”建立本地转发；直连模式下请确认服务端已经启动、"
        "地址与端口正确。");
  }
  if (error_kind == QStringLiteral("network")) {
    return QStringLiteral(
        "网络连接中断，这次操作没有完成；登录状态与云端数据都"
        "没有变化，可以直接再试一次。");
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
  if (error_kind == QStringLiteral("pin-mismatch")) {
    return QStringLiteral("服务器身份校验失败");
  }
  if (error_kind == QStringLiteral("target-exists")) {
    return QStringLiteral("目标文件已存在");
  }
  if (error_kind == QStringLiteral("integrity")) {
    return QStringLiteral("完整性校验失败");
  }
  if (error_kind == QStringLiteral("chain-conflict")) {
    return QStringLiteral("链关系被拒绝");
  }
  if (error_kind == QStringLiteral("not-a-bundle")) {
    return QStringLiteral("不是可恢复的远端备份");
  }
  if (error_kind == QStringLiteral("raw-unsupported")) {
    return QStringLiteral("不是可独立恢复的备份归档");
  }
  if (error_kind == QStringLiteral("raw-password")) {
    return QStringLiteral("需要恢复密码");
  }
  if (error_kind == QStringLiteral("raw-delta")) {
    return QStringLiteral("增量备份不能单独恢复");
  }
  if (error_kind == QStringLiteral("raw-download")) {
    return QStringLiteral("下载原始归档失败");
  }
  if (error_kind == QStringLiteral("raw-restore")) {
    return QStringLiteral("按本地格式恢复失败");
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
  // ---- PR #22：连接层的标题 ----
  const SshTunnelManager::Failure tunnel_failure =
      SshTunnelManager::FailureFromKindName(error_kind);
  if (tunnel_failure != SshTunnelManager::Failure::kNone) {
    return SshTunnelManager::FailureTitle(tunnel_failure);
  }
  if (error_kind == QStringLiteral("tunnel-not-ready")) {
    return QStringLiteral("安全通道未启动");
  }
  if (error_kind == QStringLiteral("handshake")) {
    return QStringLiteral("安全握手失败");
  }
  if (error_kind == QStringLiteral("pin-missing")) {
    return QStringLiteral("没有配置服务器身份指纹");
  }
  if (error_kind == QStringLiteral("connect-refused")) {
    return QStringLiteral("连接被拒绝");
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
  SetIdleBaseline();
}

void RemoteController::SetIdleBaseline() {
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
        if (left.created_at != right.created_at) {
          return left.created_at > right.created_at;
        }
        // 同一秒创建的快照（一次链式备份就是这种情况：R0/R1/R2 落在同一秒里）
        // 必须有一个**确定**的顺序，否则 std::sort 给出的排列可能让子快照排到
        // 父快照下面——用户看到的"最新的一份"就不是最新的了，验收截图也不可
        // 复现。规则：代数大的在前，最后用 id 兜底。
        if (left.generation != right.generation) {
          return left.generation > right.generation;
        }
        return left.snapshot_id < right.snapshot_id;
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
    // ---- 远端对象的**类型**：三类，界面必须一眼分得开 ----
    //
    // 唯一依据是服务端元数据（lineage / snapshot_kind），不按文件名、不按
    // 显示名猜：
    //
    //   rawArchive   lineage 为空、不是增量
    //                -> PR #20 时代的原始归档上传 / 低层 remote upload。
    //                   它不是 BPSNAP1 材料包，**没有**链语义（没有父、没有
    //                   代数），所以既不能显示成"完整备份"，也没有"代数 0"
    //                   可言。它可以"下载之后按本地格式独立恢复"，但那不是
    //                   链恢复——两者的按钮与说明必须是两套话。
    //   productFull  lineage 非空、不是增量
    //                -> 产品级完整基线（链根，代数 0）。
    //   incremental  增量
    //                -> 产品级增量：有代数、有父。
    const bool incremental = info.snapshot_kind == 1;
    const bool raw_archive = !incremental && info.lineage.empty();
    const bool product_full = !incremental && !info.lineage.empty();
    const QString parent = QString::fromStdString(info.parent_snapshot_id);
    item.insert(QStringLiteral("kind"), incremental
                                            ? QStringLiteral("incremental")
                                        : raw_archive ? QStringLiteral("raw")
                                                      : QStringLiteral("full"));
    // badge 文案：三类三个词。raw 绝不能显示成"完整"——那样用户会以为它是
    // 一份完整备份，然后问"为什么不能恢复"。
    item.insert(QStringLiteral("kindText"),
                incremental   ? QStringLiteral("增量备份")
                : raw_archive ? QStringLiteral("原始归档")
                              : QStringLiteral("完整备份"));
    item.insert(QStringLiteral("rawArchive"), raw_archive);
    // "代数"只对产品链成员有意义：raw 没有链，显示"代数 0"就是编出来的语义。
    item.insert(QStringLiteral("generationVisible"), !raw_archive);
    item.insert(QStringLiteral("generation"),
                static_cast<qulonglong>(info.generation));
    item.insert(QStringLiteral("parentId"), parent);
    item.insert(QStringLiteral("parentShort"), parent.left(12));
    item.insert(QStringLiteral("lineageShort"),
                QString::fromStdString(info.lineage).left(12));
    // 主操作的文案：三种类型都叫"恢复"。用户不需要理解 raw / 产品级 / 链
    // 恢复这些内部 pipeline 差异——那是三个 badge（原始归档 / 完整备份 /
    // 增量备份）要说的事；按钮只表达意图。内部实现当然还是两条路，tooltip
    // 里如实说明这一条会怎么做。
    item.insert(QStringLiteral("restoreLabel"), QStringLiteral("恢复"));
    item.insert(QStringLiteral("restoreTooltip"),
                raw_archive
                    ? QStringLiteral("下载这份原始归档，按本地备份格式恢复。"
                                     "它不属于远端增量链，不会取回别的对象。")
                    : QStringLiteral("自动获取完整依赖链并恢复此快照。"));
    // 卡片上那一句说明。raw 需要一句"它是什么"，产品级不需要。
    item.insert(QStringLiteral("typeNote"),
                raw_archive ? QStringLiteral(
                                  "手动上传的备份归档，不属于远端增量备份链。")
                            : QString());
    // 能不能点主操作：
    //   * 产品级（full / incremental）-> 由链语义决定，能恢复；
    //   * raw -> 也能恢复：能不能**成功**要看内容，所以这里允许点击，真正的
    //     失败（随机文件 / 损坏 / 版本不支持 / 单独的 delta / 缺密码）由 core
    //     给出明确原因，而不是在这里凭元数据猜，更不是把内部不确定性写进
    //     按钮名字。
    item.insert(QStringLiteral("restorable"),
                product_full || incremental || raw_archive);
    // restoreHint 只用于"这一条现在确实不能恢复"的情况（目前只剩产品级里
    // 链自洽性异常这种理论情形）。raw 的说明走 typeNote，不再写"请用下载归档"
    // ——那句话在"原始归档也能独立恢复"之后已经不成立了。
    item.insert(QStringLiteral("restoreHint"), QString());
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

void RemoteController::ReportSurfaceError(ErrorSurface surface,
                                          const QString& message) {
  // 错误只写在**触发它的那个位置**：登录表单、注册表单、注销对话框，或者
  // 页面底部的状态横幅。last_error_kind_ 由调用方先设置好（横幅标题要用
  // 它），这里只负责把同一句话送到正确的容器里，且同一个容器不重复发信号。
  switch (surface) {
    case ErrorSurface::kLogin:
      if (login_error_ != message) {
        login_error_ = message;
        emit loginErrorChanged();
      }
      return;
    case ErrorSurface::kRegister:
      if (register_error_ != message) {
        register_error_ = message;
        emit registerErrorChanged();
      }
      return;
    case ErrorSurface::kDeleteAccount:
      if (delete_account_error_ != message) {
        delete_account_error_ = message;
        emit deleteAccountErrorChanged();
      }
      return;
    case ErrorSurface::kServerKey:
      // 连接设置里的"服务器身份指纹"有自己的一行：指纹填错了就是一个输入
      // 问题，反馈必须贴在**这个输入框**下面，而不是页面底部的横幅。
      if (server_key_pin_error_ != message) {
        server_key_pin_error_ = message;
        emit serverKeyPinErrorChanged();
      }
      return;
    case ErrorSurface::kBanner: {
      // 页面级操作（上传 / 下载 / 刷新 / 删除云端备份）没有"自己的表单"，
      // 它们的触发点就在页面上，所以继续用这一页底部的状态横幅。
      //
      // busy 是"稍后再试"的提示而不是失败：颜色与文案都不该像一次报错。
      const QString kind = last_error_kind_ == QStringLiteral("busy")
                               ? QStringLiteral("warning")
                               : QStringLiteral("error");
      SetStatus(kind, TitleForFailure(last_error_kind_), message);
      return;
    }
  }
}

void RemoteController::ClearSurfaceError(ErrorSurface surface) {
  switch (surface) {
    case ErrorSurface::kLogin:
      if (!login_error_.isEmpty()) {
        login_error_.clear();
        emit loginErrorChanged();
      }
      return;
    case ErrorSurface::kRegister:
      if (!register_error_.isEmpty()) {
        register_error_.clear();
        emit registerErrorChanged();
      }
      return;
    case ErrorSurface::kDeleteAccount:
      if (!delete_account_error_.isEmpty()) {
        delete_account_error_.clear();
        emit deleteAccountErrorChanged();
      }
      return;
    case ErrorSurface::kServerKey:
      if (!server_key_pin_error_.isEmpty()) {
        server_key_pin_error_.clear();
        emit serverKeyPinErrorChanged();
      }
      return;
    case ErrorSurface::kBanner:
      return;
  }
}

bool RemoteController::ValidateEndpoint(const QString& host,
                                        const QString& port_text,
                                        const QString& username,
                                        ErrorSurface surface, QString* out_host,
                                        int* out_port, QString* out_username) {
  last_error_kind_ = QStringLiteral("validation");
  const QString trimmed_host = host.trimmed();
  if (trimmed_host.isEmpty()) {
    ReportSurfaceError(surface, QStringLiteral("请输入服务器地址"));
    return false;
  }
  bool port_ok = false;
  const int port = port_text.trimmed().toInt(&port_ok);
  if (!port_ok || port < 1 || port > 65535) {
    ReportSurfaceError(surface,
                       QStringLiteral("端口要填 1 到 65535 之间的整数"));
    return false;
  }
  const QString trimmed_username = username.trimmed();
  if (trimmed_username.isEmpty()) {
    // "还没填"和"填错了"是两句不同的话：前者用户只是没写完，后者才是格式问题。
    ReportSurfaceError(surface, QStringLiteral("请输入用户名"));
    return false;
  }
  const std::string user = trimmed_username.toStdString();
  std::string user_error;
  // 复用共享校验器，而且用**结构化**的那一个：规则只有一份，界面只把原因翻
  // 译成文案。旧实现把"长度不合法"和"字符不合法"合成一句固定文案，于是输入
  // "W"（1 个字符）被显示成"字符有问题"——人工验收发现的最后一处 UX mismatch。
  const backupproject::net::UsernameValidation username_reason =
      backupproject::net::ValidateUsername(user, &user_error);
  if (username_reason != backupproject::net::UsernameValidation::kOk) {
    ReportSurfaceError(surface, UsernameReasonText(username_reason));
    std::fprintf(stderr, "[remote] 用户名不合法（%s）：%s\n",
                 UsernameReasonName(username_reason), user_error.c_str());
    return false;
  }
  if (out_host != nullptr) {
    *out_host = trimmed_host;
  }
  if (out_port != nullptr) {
    *out_port = port;
  }
  if (out_username != nullptr) {
    *out_username = trimmed_username;
  }
  return true;
}

void RemoteController::CommitEndpoint(const QString& host, int port,
                                      const QString& username) {
  // 只有全部输入都通过校验之后才动"上一次生效的地址"：被拒的输入连这个状态
  // 都不改，界面上显示的连接目标永远是**真的用过**的那一个。
  if (endpoint_.host == host.toStdString() &&
      endpoint_.port == static_cast<std::uint16_t>(port) &&
      username_ == username) {
    return;
  }
  endpoint_.host = host.toStdString();
  endpoint_.port = static_cast<std::uint16_t>(port);
  // pin 与地址 / 端口是同一份"上一次真正生效的连接配置"：这里再对齐一次，
  // 免得出现"地址换了、pin 掉了"的组合。
  endpoint_.server_key_pin = serverKeyPin().toStdString();
  username_ = username;
  emit endpointChanged();
}

bool RemoteController::ValidatePassword(const QString& password,
                                        ErrorSurface surface) {
  last_error_kind_ = QStringLiteral("validation");
  if (password.isEmpty()) {
    ReportSurfaceError(surface, QStringLiteral("请输入密码"));
    return false;
  }
  const std::string bytes = password.toStdString();
  // 长度下限来自共享常量（服务端注册时的同一条策略），数字不在 QML 里重写。
  if (bytes.size() < backupproject::net::kMinPasswordBytes) {
    ReportSurfaceError(surface,
                       QStringLiteral("密码至少需要 %1 个字符")
                           .arg(backupproject::net::kMinPasswordBytes));
    return false;
  }
  std::string password_error;
  if (!backupproject::net::IsValidPassword(bytes, &password_error)) {
    ReportSurfaceError(
        surface, QStringLiteral("密码不能超过 256 字节，也不能包含空字符"));
    std::fprintf(stderr, "[remote] 密码不合法：%s\n", password_error.c_str());
    return false;
  }
  return true;
}

bool RemoteController::ValidateCurrentPassword(const QString& password) {
  last_error_kind_ = QStringLiteral("validation");
  if (password.isEmpty()) {
    ReportSurfaceError(ErrorSurface::kDeleteAccount,
                       QStringLiteral("请输入当前密码"));
    return false;
  }
  const std::string bytes = password.toStdString();
  if (bytes.size() < backupproject::net::kMinPasswordBytes) {
    ReportSurfaceError(ErrorSurface::kDeleteAccount,
                       QStringLiteral("当前密码至少需要 %1 个字符")
                           .arg(backupproject::net::kMinPasswordBytes));
    return false;
  }
  return true;
}

QString RemoteController::SurfaceFailureMessage(RemoteOpResult::Kind kind,
                                                const QString& error_kind,
                                                const QString& fallback) const {
  const bool network = error_kind == QStringLiteral("network");
  const bool server_side = error_kind == QStringLiteral("server") ||
                           error_kind == QStringLiteral("rejected") ||
                           error_kind == QStringLiteral("unknown");
  // 连接层（隧道 / 握手 / 身份 / 端口拒绝）的失败**不**允许被压成
  // "无法连接到服务器，请稍后重试"：那些原因各自都有明确得多的说法，
  // 而"请稍后重试"恰好是用户最不该做的事（重试 100 次通道也不会自己起来）。
  const bool connection_layer =
      SshTunnelManager::FailureFromKindName(error_kind) !=
          SshTunnelManager::Failure::kNone ||
      error_kind == QStringLiteral("tunnel-not-ready") ||
      error_kind == QStringLiteral("handshake") ||
      error_kind == QStringLiteral("pin-missing") ||
      error_kind == QStringLiteral("connect-refused") ||
      error_kind == QStringLiteral("pin-mismatch");
  if (connection_layer) {
    return DescribeFailure(error_kind);
  }
  switch (kind) {
    case RemoteOpResult::Kind::kLogin:
      // 账户枚举防护：用户不存在与密码错误回**同一句话**，界面不泄露账号是否存在。
      if (error_kind == QStringLiteral("credentials")) {
        return QStringLiteral("用户名或密码错误");
      }
      if (network) {
        return QStringLiteral("无法连接到服务器，请稍后重试");
      }
      if (server_side) {
        return QStringLiteral("服务器暂时无法完成登录");
      }
      break;
    case RemoteOpResult::Kind::kRegister:
      if (error_kind == QStringLiteral("name-taken")) {
        return QStringLiteral("该用户名已被使用，请更换用户名");
      }
      if (network) {
        return QStringLiteral("无法连接到服务器，请稍后重试");
      }
      if (server_side) {
        return QStringLiteral("服务器暂时无法完成注册");
      }
      break;
    case RemoteOpResult::Kind::kBackup:
      if (network) {
        return QStringLiteral(
            "网络连接中断，本次远端备份没有完成；云端与本地都没有变化，"
            "可以直接再试一次");
      }
      if (error_kind == QStringLiteral("chain-conflict")) {
        return QStringLiteral(
            "服务端拒绝了这个链关系（父已经有一条增量，或者代数超过上限）："
            "本次没有创建任何云端备份，可以直接再试一次");
      }
      if (server_side) {
        return QStringLiteral("服务器暂时无法完成这次远端备份");
      }
      break;
    case RemoteOpResult::Kind::kRestore:
      if (error_kind == QStringLiteral("not-a-bundle")) {
        return QStringLiteral(
            "这一条不是远端备份链的材料包（原始归档上传）：不能用链恢复，"
            "请改用“下载归档”");
      }
      if (error_kind == QStringLiteral("integrity")) {
        return QStringLiteral(
            "下载到的备份材料没有通过完整性校验，本次恢复没有完成，"
            "目标目录没有被改动");
      }
      if (network) {
        return QStringLiteral(
            "网络连接中断，本次恢复没有完成；目标目录没有被改动，"
            "可以直接再试一次");
      }
      if (server_side) {
        return QStringLiteral("服务器暂时无法完成这次恢复");
      }
      break;
    case RemoteOpResult::Kind::kRestoreRaw:
      // 原始归档恢复的每一种失败都**只有一处**中文文案：DescribeFailure。
      // 这里刻意不再各写一份——两份文案是同一个事实的两次表述，改一处忘一处
      // 就会让横幅和详情自相矛盾（本轮就抓到过这个漂移：横幅还在说"不能脱离
      // 依赖链"，而详情里已经写明"缺少父备份"）。所以只有与类型无关的网络 /
      // 服务端两种情形在这里特判，其余一律落回 fallback。
      if (network) {
        return QStringLiteral(
            "网络连接中断，这份原始归档没有被恢复；目标目录没有被改动");
      }
      if (server_side) {
        return QStringLiteral("服务器暂时无法取回这份原始归档");
      }
      break;
    case RemoteOpResult::Kind::kDeleteAccount:
      if (error_kind == QStringLiteral("account-password")) {
        return QStringLiteral("当前密码不正确，账户与全部云端备份都没有被删除");
      }
      if (error_kind == QStringLiteral("not-logged-in")) {
        return QStringLiteral("登录状态已经失效，账户未注销，请重新登录");
      }
      if (network) {
        return QStringLiteral("网络连接中断，账户未注销，请重试");
      }
      if (server_side) {
        return QStringLiteral("服务器暂时无法完成注销，账户未注销");
      }
      break;
    default:
      break;
  }
  return fallback;
}
bool RemoteController::BeginOperation(const QString& action_text,
                                      bool need_login, ErrorSurface surface) {
  if (busy_) {
    // 控制器层的真闸门：QML 的 enabled 只是可见性，同一个回合里的第二个请求
    // 在这里被拒——不排队，也不和正在跑的那一个抢同一条连接。
    last_error_kind_ = QStringLiteral("busy");
    ReportSurfaceError(surface, QStringLiteral("正在%1，请等它结束后再试")
                                    .arg(busy_action_.isEmpty()
                                             ? QStringLiteral("处理上一个请求")
                                             : busy_action_));
    return false;
  }
  if (need_login && !authenticated_) {
    last_error_kind_ = QStringLiteral("not-logged-in");
    // 在注销对话框里点"确认注销"时登录态已经失效：这句话要留在对话框里，
    // 而且必须明确说"账户没有被删"。
    ReportSurfaceError(
        surface,
        surface == ErrorSurface::kDeleteAccount
            ? QStringLiteral("登录状态已经失效，账户未注销，请重新登录")
            : DescribeFailure(last_error_kind_));
    return false;
  }
  // 连接之前必须有服务端身份 pin：没有它客户端会**直接拒绝连接**（不做首次
  // 连接自动信任），而那是一条协议级的底层原因，用户读不懂"我到底少做了什么"。
  // 与地址 / 端口 / 口令同一条规矩：本地就挡住，一个字节都不发。
  if (server_key_pin_.isEmpty()) {
    last_error_kind_ = QStringLiteral("validation");
    ReportSurfaceError(
        surface,
        QStringLiteral("还没有填写服务器身份指纹：把服务器管理员给出的 "
                       "sha256:… 指纹填进“服务器身份指纹”再试一次"));
    return false;
  }
  last_error_kind_ = QStringLiteral("none");
  last_detail_.clear();
  // 记住这条操作自己的错误落点：如果它因为"要先建安全通道"而被挂起，失败时
  // 仍然要回到**触发它的那个表单**（登录框的错误就该写在登录框下面）。
  pending_surface_ = surface;
  SetBusy(true, action_text);
  SetStatus(QStringLiteral("running"), action_text, QString());
  return true;
}

void RemoteController::DispatchRequest(const RemoteRequest& request) {
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

void RemoteController::Submit(const RemoteRequest& request) {
  // 传输层的唯一闸门。直连模式与 PR #21 完全一样；SSH 模式先保证"本地有一个
  // 能连上的端口"这件事成立，再把请求发出去。
  //
  // 调用点一个都不用改：所有提交都经过这里，所以不可能出现"某一条操作忘了
  // 走隧道"这种漏网（那种漏网的症状恰好就是 connection refused）。
  if (connection_mode_ == ConnectionMode::kDirect) {
    DispatchRequest(request);
    return;
  }
  if (TunnelMatchesEndpoint() && tunnel_.localPort() > 0) {
    RemoteRequest dialed = request;
    // 客户端拨的是**本地**那一段：127.0.0.1:<自动挑到的端口>。远端地址在
    // ssh 的 -L 参数里，BPSEC1 的 pin 校验仍然在隧道里面照跑。
    dialed.endpoint.host = std::string("127.0.0.1");
    dialed.endpoint.port = static_cast<std::uint16_t>(tunnel_.localPort());
    DispatchRequest(dialed);
    return;
  }
  // 通道还没好（或者用户改了远端地址，旧通道已经不指向它了）：把这条请求
  // 挂起，先建通道，建好之后自动接着发。用户不需要知道这个顺序。
  DeferRequest(request, busy_action_, pending_surface_);
  SetStatus(QStringLiteral("running"), busy_action_,
            QStringLiteral("正在建立安全通道：%1 -> %2…")
                .arg(ssh_host_, tunnelRemoteEndpointText()));
  // 先收干净再按当前参数重来：地址改过时这一步是必须的，否则界面显示 B、
  // 实际却还走在通往 A 的隧道上。
  tunnel_.Stop();
  emit tunnelChanged();
  StartTunnelForEndpoint(busy_action_);
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
  // 每一次操作都先用**当前**的 endpoint / pin 更新重连参数：用户刚改过服务器
  // 身份指纹时，重连必须用新的那一份（否则界面说的"下一次连接生效"就是假的）。
  // 它不发字节、不动 token，会话与 RESUME 语义不变。
  client->SetReconnectEndpoint(request.endpoint);
  result.backup_incremental_requested = request.allow_incremental;
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
    case RemoteOpResult::Kind::kBackup: {
      // 与 backupctl remote backup 逐行一致的三步：准备缓存 -> 组装请求 ->
      // 交给共享 core。缓存根用空串 = 应用配置目录下的 remote-cache（与 CLI
      // 的默认值相同；自检通过隔离 HOME/XDG 来隔离它）。
      backupproject::net::RemoteCacheLayout cache;
      if (!backupproject::net::PrepareRemoteCache(
              std::string(), client->server_fingerprint(), request.username,
              &cache, &error)) {
        result.ok = false;
        break;
      }
      backupproject::net::RemoteBackupRequest backup;
      backup.client = client;
      backup.cache = cache;
      backup.source_directory = request.source_directory;
      backup.allow_incremental = request.allow_incremental;
      backup.display_name = request.display_name;
      backup.progress = progress;
      backupproject::net::RemoteBackupOutcome outcome;
      result.ok = backupproject::net::RunRemoteBackup(backup, &outcome, &error);
      if (result.ok) {
        result.backup_produced_delta = outcome.produced_delta;
        result.backup_rebuilt_full = outcome.rebuilt_full_baseline;
        result.backup_no_changes = outcome.no_changes;
        result.backup_snapshot_id = outcome.snapshot_id;
        result.backup_parent_snapshot_id = outcome.parent_snapshot_id;
        result.backup_generation = outcome.generation;
        result.backup_uploaded_bytes = outcome.uploaded_bytes;
        result.backup_chain_root_bytes = outcome.chain_root_bytes;
        result.backup_baseline_reason = outcome.baseline_reason;
        result.backup_archive_name = outcome.archive_name;
      }
      break;
    }
    case RemoteOpResult::Kind::kRestore: {
      backupproject::net::RemoteCacheLayout cache;
      if (!backupproject::net::PrepareRemoteCache(
              std::string(), client->server_fingerprint(), request.username,
              &cache, &error)) {
        result.ok = false;
        break;
      }
      // 恢复不需要口令：增量链的外层信封由内层身份记录保护（与 CLI 同一条
      // 结论）。整条依赖链的解析、下载、逐成员校验、逐跳应用全在 core 里。
      backupproject::net::RemoteRestoreOutcome outcome;
      result.ok = backupproject::net::RunRemoteRestore(
          client, cache, request.snapshot_id, request.restore_destination,
          backupproject::RestoreOptions(), &outcome, &error);
      if (result.ok) {
        result.restore_chain_length = outcome.chain_length;
        result.restore_delta_count = outcome.delta_count;
        result.restore_downloaded_bytes = outcome.downloaded_bytes;
        result.restore_reused_bytes = outcome.reused_bytes;
        result.restore_restored_entries = outcome.restored_entries;
      }
      break;
    }
    case RemoteOpResult::Kind::kRestoreRaw: {
      // 原始归档的单独恢复：下载那一个 blob，按内容认出格式，再交给**既有的
      // 本地恢复核心**（BackupEngine / RunRestorePipeline）。这里没有链、
      // 没有父、没有副文件，也没有任何 GUI 自己解析归档的代码。
      backupproject::net::RemoteCacheLayout cache;
      if (!backupproject::net::PrepareRemoteCache(
              std::string(), client->server_fingerprint(), request.username,
              &cache, &error)) {
        result.ok = false;
        break;
      }
      // 一次交互 = 一次 Prepare（下载 + SHA-256 校验 + 按内容识别）+ 一到多次
      // Run（带密码重试）。需要密码时把这个会话交回主线程：用户下一次输入的
      // 密码用的是**同一份**已经下载并校验过的字节，不重新下载。
      std::shared_ptr<backupproject::net::RemoteRawRestoreSession> session =
          request.raw_session;
      if (session == nullptr) {
        session =
            std::make_shared<backupproject::net::RemoteRawRestoreSession>();
        backupproject::net::RemoteRawRestoreRequest raw;
        raw.client = client;
        raw.cache = cache;
        raw.snapshot_id = request.snapshot_id;
        raw.display_name = request.display_name;
        raw.destination_directory = request.restore_destination;
        // 第一次请求**不带**密码：界面也是一开始只问目标目录——只有当 core
        // 明确说"这份归档加密了"之后才向用户要密码。
        raw.restore_options.password.clear();
        raw.progress = progress;
        backupproject::net::RemoteRawRestoreOutcome prepared;
        if (!session->Prepare(raw, &prepared, &error)) {
          result.ok = false;
          // 会话只活在这次 Prepare 里：离开作用域就会删掉工作目录。
          session.reset();
          break;
        }
      }
      backupproject::net::RemoteRawRestoreOutcome outcome;
      result.ok = session->Run(request.restore_password, &outcome, &error);
      result.raw_password_required = outcome.password_required;
      result.raw_downloaded_bytes = outcome.downloaded_bytes;
      result.raw_verified_sha256 = outcome.verified_sha256;
      result.raw_archive_format = outcome.archive_format;
      result.raw_restored_entries = outcome.restored_entries;
      result.raw_download_count = session->download_count();
      result.raw_archive_path = session->archive_path_for_test();
      if (outcome.password_required || !result.ok) {
        // "还需要密码"（第一次问、或密码又输错了）时保留这次交互：那份归档与
        // 已经选好的目标目录都要留着，用户可以接着输。其余失败一律放弃
        // （工作目录立刻删掉），成功也一样——远端那一份还在。
        const bool ask_again =
            outcome.password_required ||
            Contains(error, "raw restore: authentication failed");
        if (ask_again) {
          result.raw_session = session;
        }
      }
      break;
    }
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
    // 目标目录这一类失败：本地恢复自己给的那一句才是可照做的（"目录不为空"、
    // "已经存在同名文件"…）。技术原话放在括号里当次要信息，主文案仍是中文
    // 的一句话（见 §9F：用现有本地 Restore
    // 的明确错误，而不是笼统的"恢复失败"）。
    if (result.kind == RemoteOpResult::Kind::kRestoreRaw &&
        (result.error_kind == QStringLiteral("raw-destination") ||
         result.error_kind == QStringLiteral("raw-restore"))) {
      const QString reason = SanitizeLocalRestoreReason(result.detail);
      if (!reason.isEmpty()) {
        result.message +=
            QStringLiteral("（原因：") + reason + QStringLiteral("）");
      }
    }
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
                      result.kind == RemoteOpResult::Kind::kDelete ||
                      // 产品级远端备份之后也自动读一次列表：新快照（或者
                      // "没有变化、没有新快照"这个事实）必须马上反映在列表上。
                      result.kind == RemoteOpResult::Kind::kBackup);
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
  // 原始归档恢复是一次**多步交互**（下载 + 识别 -> 可能要密码 -> 恢复），
  // 状态必须先落地：界面据此决定这次的结果是"一条错误"还是"还差一样东西"。
  const bool raw_needs_password =
      result.kind == RemoteOpResult::Kind::kRestoreRaw && !result.ok &&
      result.raw_password_required && result.raw_session != nullptr;
  if (result.kind == RemoteOpResult::Kind::kRestoreRaw) {
    ApplyRawRestoreState(result);
  }
  if (result.ok) {
    last_error_kind_ = QStringLiteral("none");
    last_detail_.clear();
  } else {
    last_error_kind_ = result.error_kind;
    last_detail_ = result.detail;
    // 原始归档单独恢复的**失败**也要把结构化结果留下来：缺密码与"不是备份
    // 归档"必须能被区分开，界面（以及自检）才能给出各自可照做的动作。
    if (result.kind == RemoteOpResult::Kind::kRestoreRaw) {
      last_raw_restore_password_required_ = result.raw_password_required;
      last_raw_restore_sha256_ =
          QString::fromStdString(result.raw_verified_sha256);
      last_raw_restore_format_ =
          QString::fromStdString(result.raw_archive_format);
      last_raw_restore_downloaded_bytes_ = result.raw_downloaded_bytes;
      last_raw_restore_entries_ = result.raw_restored_entries;
    }
    std::fprintf(
        stderr, "[remote] %s 没有完成：%s%s%s\n",
        KindName(result.kind).toUtf8().constData(),
        result.status_name.empty() ? "" : result.status_name.c_str(),
        result.status_name.empty() ? "" : " ",
        result.detail.empty() ? "(没有更多信息)" : result.detail.c_str());
    if (raw_needs_password) {
      // 这不是失败，是"还差一样东西"。那份归档与目标目录都留着，页面切到
      // 密码那一段；横幅只说下一步要做什么，不报错误——报错误会让用户以为
      // 整个恢复已经完了。
      SetStatus(QStringLiteral("idle"), QStringLiteral("需要恢复密码"),
                QStringLiteral("这份备份已加密：请输入恢复密码之后再继续。"));
      return;
    }
    // 连接已经不可信时如实降级：不让界面继续显示"已登录"，否则用户会对着
    // 一个假的登录状态反复重试。
    // 会话要不要清掉，取决于**服务端说了什么**，而不是"网络有没有抖一下"：
    //
    //   not-logged-in（服务端明确 UNAUTHORIZED）-> token 真的没用了，清会话
    //   network / local / 其它传输层失败        -> 只关这条连接、**保留
    //   token**；
    //                                            下一次操作会自动重连并恢复会话
    //
    // 这一条是人工验收里"点一次刷新就被退出登录"的根因：以前任何一次网络抖动
    // 都会清掉登录态，而 token 明明是好的。现在网络错误只影响这一次操作，
    // 登录状态与云端数据都不动。
    if (result.error_kind == QStringLiteral("not-logged-in")) {
      client_.Disconnect();
      list_loaded_ = false;
      SetSnapshots(std::vector<RemoteSnapshotInfo>());
      list_summary_ = QStringLiteral("尚未登录");
      emit snapshotsChanged();
      if (authenticated_) {
        authenticated_ = false;
        emit sessionChanged();
      }
    }
    if (result.kind == RemoteOpResult::Kind::kLogin) {
      // 登录失败 = 没有会话。哪怕这一次尝试之前是登录状态，它也已经把那条会话
      // 换掉了（客户端在 Login 之前先丢掉旧会话）。界面上不允许同时出现
      // "登录失败"和"当前账户：xxx（已登录）"——那正是人工验收里看到的
      // "注销之后居然还能显示登录成功"的那类自相矛盾状态。
      client_.Disconnect();
      list_loaded_ = false;
      SetSnapshots(std::vector<RemoteSnapshotInfo>());
      list_summary_ = QStringLiteral("尚未登录");
      emit snapshotsChanged();
      if (authenticated_) {
        authenticated_ = false;
        emit sessionChanged();
      }
    }
    // 失败的归属：登录失败进登录表单的错误行，注册失败进注册表单的错误行，
    // 注销失败进对话框自己的错误行——三个界面各自说自己的话。页面底部的横幅
    // 留给上传 / 下载 / 刷新 / 删除云端备份这些**页面级**操作：它们在页面上
    // 触发，也在页面上回报。
    const QString message =
        SurfaceFailureMessage(result.kind, result.error_kind, result.message);
    // 登录 / 注册 / 注销各自有错误行：原因写在那里，页面底部的横幅也不能继续
    // 挂着"正在登录"这种已经过期的运行状态（那是"点了没反应"的另一面：按钮
    // 早就停了，横幅还在转，而且 running 还是跨页可见的全局状态）。
    const bool has_own_surface =
        result.kind == RemoteOpResult::Kind::kLogin ||
        result.kind == RemoteOpResult::Kind::kRegister ||
        result.kind == RemoteOpResult::Kind::kDeleteAccount;
    if (has_own_surface) {
      SetIdleBaseline();
    }
    switch (result.kind) {
      case RemoteOpResult::Kind::kLogin:
        ReportSurfaceError(ErrorSurface::kLogin, message);
        return;
      case RemoteOpResult::Kind::kRegister:
        ReportSurfaceError(ErrorSurface::kRegister, message);
        return;
      case RemoteOpResult::Kind::kDeleteAccount:
        ReportSurfaceError(ErrorSurface::kDeleteAccount, message);
        return;
      default:
        ReportSurfaceError(ErrorSurface::kBanner, message);
        return;
    }
  }

  switch (result.kind) {
    case RemoteOpResult::Kind::kRegister:
      ClearSurfaceError(ErrorSurface::kRegister);
      authenticated_ = false;
      SetStatus(QStringLiteral("success"), QStringLiteral("注册成功"),
                QStringLiteral("账号已经创建，现在可以点“登录”。"));
      break;
    case RemoteOpResult::Kind::kLogin:
      ClearSurfaceError(ErrorSurface::kLogin);
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
    case RemoteOpResult::Kind::kBackup: {
      // 用户选择的"策略"与这次**实际**产出的类型不是同一件事，界面必须如实说：
      //   no_changes            -> 没有创建新快照（列表不会多一行）
      //   produced_delta        -> 真的是增量
      //   否则（含 rebuilt_full）-> 本次创建的是完整基线，并给出 core 的理由
      const auto set_summary = [this](const QString& kind,
                                      const QString& text) {
        if (backup_summary_kind_ == kind && backup_summary_ == text) {
          return;
        }
        backup_summary_kind_ = kind;
        backup_summary_ = text;
        emit backupSummaryChanged();
      };
      last_backup_produced_delta_ = result.backup_produced_delta;
      last_backup_rebuilt_full_ = result.backup_rebuilt_full;
      last_backup_no_changes_ = result.backup_no_changes;
      last_backup_snapshot_id_ =
          QString::fromStdString(result.backup_snapshot_id);
      last_backup_uploaded_bytes_ = result.backup_uploaded_bytes;
      last_backup_chain_root_bytes_ = result.backup_chain_root_bytes;
      last_backup_generation_ = result.backup_generation;
      last_backup_baseline_reason_ =
          QString::fromStdString(result.backup_baseline_reason);
      last_backup_incremental_requested_ = result.backup_incremental_requested;
      emit backupSummaryChanged();
      if (result.backup_no_changes) {
        set_summary(QStringLiteral("no-change"),
                    QStringLiteral("没有检测到有效变化，本次未创建新备份。"));
        SetStatus(QStringLiteral("idle"), QStringLiteral("没有检测到有效变化"),
                  QStringLiteral("源目录与上一次远端备份相比没有变化，"
                                 "本次没有创建新的云端备份。"));
        break;
      }
      const QString short_id = last_backup_snapshot_id_.left(12);
      const QString size_text = FormatSize(result.backup_uploaded_bytes);
      if (result.backup_produced_delta) {
        const QString parent_short =
            QString::fromStdString(result.backup_parent_snapshot_id).left(12);
        set_summary(QStringLiteral("incremental"),
                    QStringLiteral("增量备份完成：快照 %1（父 %2，代数 %3），"
                                   "本次上传 %4。")
                        .arg(short_id, parent_short)
                        .arg(static_cast<qulonglong>(result.backup_generation))
                        .arg(size_text));
        SetStatus(QStringLiteral("success"), QStringLiteral("增量备份完成"),
                  QStringLiteral("快照 %1 是增量（父 %2，代数 %3），"
                                 "本次只上传了 %4。")
                      .arg(short_id, parent_short)
                      .arg(static_cast<qulonglong>(result.backup_generation))
                      .arg(size_text));
      } else if (last_backup_incremental_requested_) {
        // 用户点了“增量”，实际产出的却是一份完整基线：这是**兜底**，必须
        // 明说“本次创建的是完整基线”，不能让用户以为增量成功了。core 的
        // 英文理由不进这一行（它在技术详情里），用户看到的是中文结论。
        set_summary(QStringLiteral("full"),
                    QStringLiteral("本次创建的是完整基线：快照 %1，"
                                   "本次上传 %2。")
                        .arg(short_id, size_text));
        SetStatus(QStringLiteral("success"), QStringLiteral("已创建完整基线"),
                  QStringLiteral("云端没有可续的链，本次创建的是完整基线 %1，"
                                 "上传 %2；下一次改过文件之后再点“增量”就是"
                                 "真正的增量。")
                      .arg(short_id, size_text));
      } else {
        // 用户选的就是“完整”：这是一次正常的完整备份，不是兜底。
        set_summary(QStringLiteral("full"),
                    QStringLiteral("完整备份完成：快照 %1（完整基线，代数 0），"
                                   "本次上传 %2。")
                        .arg(short_id, size_text));
        SetStatus(QStringLiteral("success"), QStringLiteral("完整备份完成"),
                  QStringLiteral("快照 %1 是一份完整基线（代数 0），"
                                 "本次上传 %2。")
                      .arg(short_id, size_text));
      }
      break;
    }
    case RemoteOpResult::Kind::kRestoreRaw:
      // 原始归档的恢复：对用户来说就是"恢复完成"——内部走的是哪条流水线
      // （链 vs 单份归档）由列表里的类型 badge 说明，不写进结论里，更不写
      // "尝试…成功"那种措辞（那是内部不确定性，不该出现在结果文案里）。
      last_raw_restore_downloaded_bytes_ = result.raw_downloaded_bytes;
      last_raw_restore_entries_ = result.raw_restored_entries;
      last_raw_restore_sha256_ =
          QString::fromStdString(result.raw_verified_sha256);
      last_raw_restore_format_ =
          QString::fromStdString(result.raw_archive_format);
      last_raw_restore_password_required_ = result.raw_password_required;
      SetStatus(
          QStringLiteral("success"), QStringLiteral("恢复完成"),
          QStringLiteral("已下载并校验归档（SHA-256 %1…），按本地格式 %2 恢复，"
                         "恢复了 %3 个条目。")
              .arg(QString::fromStdString(result.raw_verified_sha256).left(12),
                   QString::fromStdString(result.raw_archive_format))
              .arg(static_cast<qulonglong>(result.raw_restored_entries)));
      break;
    case RemoteOpResult::Kind::kRestore:
      last_restore_chain_length_ = result.restore_chain_length;
      last_restore_delta_count_ = result.restore_delta_count;
      last_restore_downloaded_bytes_ = result.restore_downloaded_bytes;
      last_restore_entries_ = result.restore_restored_entries;
      SetStatus(
          QStringLiteral("success"), QStringLiteral("恢复完成"),
          QStringLiteral("依赖链 %1 份快照（%2 个增量），本次下载 %3，"
                         "恢复了 %4 个条目。")
              .arg(static_cast<qulonglong>(result.restore_chain_length))
              .arg(static_cast<qulonglong>(result.restore_delta_count))
              .arg(FormatSize(result.restore_downloaded_bytes))
              .arg(static_cast<qulonglong>(result.restore_restored_entries)));
      break;
    case RemoteOpResult::Kind::kDeleteAccount:
      // 账户已经不存在了：本机内存里的会话、口令与列表全部清掉，界面回到
      // "未登录"。云端数据由服务端删除，这里不做任何本地清理。
      //
      // 这一组更新必须一起发生：只要还有一处留着旧状态，界面就会出现"服务端
      // 已经删了、本机还显示已登录"这种自相矛盾的样子（人工验收见过）。
      // 账户已经不存在了：三个错误行一起清掉，免得注销成功之后登录表单里还
      // 留着上一次"用户名或密码错误"这种已经不成立的话。
      ClearSurfaceError(ErrorSurface::kDeleteAccount);
      ClearSurfaceError(ErrorSurface::kLogin);
      ClearSurfaceError(ErrorSurface::kRegister);
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
  // 上一次的失败原因先清掉：重新提交之后，表单里留下的必须是这一次的结果。
  ClearSurfaceError(ErrorSurface::kRegister);
  // 校验顺序是"填没填 → 合不合法 → 两次一不一样"。任何一步被拒都**一个字节
  // 都不发**，而且被拒的输入连"上一次生效的地址"都不会改：地址要等全部校验
  // 通过之后才提交。
  QString valid_host;
  QString valid_username;
  int valid_port = 0;
  if (!ValidateEndpoint(host, port_text, username, ErrorSurface::kRegister,
                        &valid_host, &valid_port, &valid_username)) {
    return false;
  }
  if (!ValidatePassword(password, ErrorSurface::kRegister)) {
    return false;
  }
  if (confirm_password.isEmpty()) {
    last_error_kind_ = QStringLiteral("validation");
    ReportSurfaceError(ErrorSurface::kRegister,
                       QStringLiteral("请再输入一次密码以确认"));
    return false;
  }
  if (password != confirm_password) {
    last_error_kind_ = QStringLiteral("password-mismatch");
    ReportSurfaceError(ErrorSurface::kRegister,
                       QStringLiteral("两次输入的密码不一致，请重新输入"));
    return false;
  }
  CommitEndpoint(valid_host, valid_port, valid_username);
  if (!BeginOperation(QStringLiteral("正在注册账号"), /*need_login=*/false,
                      ErrorSurface::kRegister)) {
    return false;
  }
  password_ = password;
  RemoteRequest request;
  request.kind = RemoteOpResult::Kind::kRegister;
  request.endpoint = endpoint_;
  // pin 与地址是同一份快照：每一处提交都显式带上它（见头文件里的说明）。
  request.endpoint.server_key_pin = serverKeyPin().toStdString();
  request.username = valid_username.toStdString();
  request.password = password.toStdString();
  Submit(request);
  return true;
}

bool RemoteController::login(const QString& host, const QString& port_text,
                             const QString& username, const QString& password) {
  ClearSurfaceError(ErrorSurface::kLogin);
  QString valid_host;
  QString valid_username;
  int valid_port = 0;
  if (!ValidateEndpoint(host, port_text, username, ErrorSurface::kLogin,
                        &valid_host, &valid_port, &valid_username)) {
    return false;
  }
  if (!ValidatePassword(password, ErrorSurface::kLogin)) {
    return false;
  }
  CommitEndpoint(valid_host, valid_port, valid_username);
  if (!BeginOperation(QStringLiteral("正在登录"), /*need_login=*/false,
                      ErrorSurface::kLogin)) {
    return false;
  }
  password_ = password;
  RemoteRequest request;
  request.kind = RemoteOpResult::Kind::kLogin;
  request.endpoint = endpoint_;
  // pin 与地址是同一份快照：每一处提交都显式带上它（见头文件里的说明）。
  request.endpoint.server_key_pin = serverKeyPin().toStdString();
  request.username = valid_username.toStdString();
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
  // pin 与地址是同一份快照：每一处提交都显式带上它（见头文件里的说明）。
  request.endpoint.server_key_pin = serverKeyPin().toStdString();
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
  // pin 与地址是同一份快照：每一处提交都显式带上它（见头文件里的说明）。
  request.endpoint.server_key_pin = serverKeyPin().toStdString();
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
  // pin 与地址是同一份快照：每一处提交都显式带上它（见头文件里的说明）。
  request.endpoint.server_key_pin = serverKeyPin().toStdString();
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
  // pin 与地址是同一份快照：每一处提交都显式带上它（见头文件里的说明）。
  request.endpoint.server_key_pin = serverKeyPin().toStdString();
  request.snapshot_id = snapshot_id.toStdString();
  Submit(request);
  return true;
}

void RemoteController::clearDeleteAccountError() {
  ClearSurfaceError(ErrorSurface::kDeleteAccount);
}

void RemoteController::clearLoginError() {
  ClearSurfaceError(ErrorSurface::kLogin);
}

void RemoteController::clearRegisterError() {
  ClearSurfaceError(ErrorSurface::kRegister);
}

void RemoteController::clearServerKeyPinError() {
  ClearSurfaceError(ErrorSurface::kServerKey);
}

bool RemoteController::setServerKeyPin(const QString& pin) {
  // 重新提交先把上一次的原因清掉：这一行里留下的必须是这一次的结果。
  ClearSurfaceError(ErrorSurface::kServerKey);
  // 首尾空白丢掉：从终端复制 --server-key 那一行时经常带上空格。除此之外
  // 一个字符都不改——大小写由共享解析器归一化，界面不做第二套"看起来对"的
  // 判断（规则只有一份）。
  const QString text = pin.trimmed();
  if (text.isEmpty()) {
    last_error_kind_ = QStringLiteral("validation");
    ReportSurfaceError(
        ErrorSurface::kServerKey,
        QStringLiteral("请填写服务器身份指纹（向服务器管理员索取，"
                       "形如 sha256: 开头的 64 位十六进制）"));
    return false;
  }
  backupproject::net::ServerKeyPin parsed;
  std::string parse_error;
  if (!backupproject::net::ParseServerKeyPin(text.toStdString(), &parsed,
                                             &parse_error)) {
    last_error_kind_ = QStringLiteral("validation");
    ReportSurfaceError(ErrorSurface::kServerKey,
                       QStringLiteral("服务器身份指纹不合法：%1")
                           .arg(QString::fromStdString(parse_error)));
    // 终端上留一条原始原因（诊断用），界面上只出现上面那一句。
    std::fprintf(stderr, "[remote] 服务端 pin 不合法：%s\n",
                 parse_error.c_str());
    return false;
  }
  if (server_key_pin_ == text) {
    return true;
  }
  server_key_pin_ = text;
  // 两个落点一起更新：request.endpoint 由 endpoint_ 拷贝而来，各个提交点再
  // 显式带一次 pin（见头文件）。已经建立的连接不受影响——pin 只在握手时用，
  // 下一次连接才会用到新值。
  endpoint_.server_key_pin = server_key_pin_.toStdString();
  emit serverKeyPinChanged();
  return true;
}

bool RemoteController::deleteAccount(const QString& password,
                                     const QString& username_confirmation) {
  // 重新提交时先把上一次的错误行清掉：否则用户看到的会是两次不同尝试的原因。
  clearDeleteAccountError();
  const QString confirm = username_confirmation.trimmed();
  // "两样都没填"和"只填了一样"要给三句不同的话：否则用户按完按钮还是不知道
  // 自己漏了什么。全部检查都在**本地**做，任何一条不过都不发请求。
  if (password.isEmpty() && confirm.isEmpty()) {
    last_error_kind_ = QStringLiteral("validation");
    ReportSurfaceError(ErrorSurface::kDeleteAccount,
                       QStringLiteral("请输入当前密码，并输入账户名以确认"));
    return false;
  }
  if (!ValidateCurrentPassword(password)) {
    return false;
  }
  if (confirm.isEmpty()) {
    last_error_kind_ = QStringLiteral("validation");
    ReportSurfaceError(ErrorSurface::kDeleteAccount,
                       QStringLiteral("请输入账户名以确认注销"));
    return false;
  }
  // 二次确认：必须逐字敲出当前账户名。少一个字符都不发请求——注销是不可撤销
  // 的服务端删除，不能是一个"点快了就没了"的按钮。
  if (confirm != username_) {
    last_error_kind_ = QStringLiteral("confirm-mismatch");
    ReportSurfaceError(ErrorSurface::kDeleteAccount,
                       QStringLiteral("输入的账户名与当前账户不一致"));
    return false;
  }
  if (!BeginOperation(QStringLiteral("正在注销账户"), /*need_login=*/true,
                      ErrorSurface::kDeleteAccount)) {
    return false;
  }
  RemoteRequest request;
  request.kind = RemoteOpResult::Kind::kDeleteAccount;
  request.endpoint = endpoint_;
  // pin 与地址是同一份快照：每一处提交都显式带上它（见头文件里的说明）。
  request.endpoint.server_key_pin = serverKeyPin().toStdString();
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

// ---- 产品级远端备份 / 链恢复（与 backupctl 共用同一套 core）----

void RemoteController::clearBackupSummary() {
  if (backup_summary_.isEmpty() && backup_summary_kind_.isEmpty()) {
    return;
  }
  backup_summary_.clear();
  backup_summary_kind_.clear();
  emit backupSummaryChanged();
}

bool RemoteController::backupRemote(const QString& source_directory,
                                    bool allow_incremental) {
  const QString source = source_directory.trimmed();
  if (source.isEmpty()) {
    SetStatus(QStringLiteral("error"), QStringLiteral("还没有选择源目录"),
              QStringLiteral("先点“选择源目录”，挑一个要备份到远端的目录。"));
    return false;
  }
  const QFileInfo info(source);
  if (!info.exists() || !info.isDir()) {
    SetStatus(QStringLiteral("error"), QStringLiteral("源目录不可用"),
              QStringLiteral("这个路径不存在，或者不是一个目录：%1")
                  .arg(info.fileName().isEmpty() ? source : info.fileName()));
    return false;
  }
  // 策略只决定"允不允许续链"：真正的判断（有没有可信基线、链深、缓存是否
  // 可信）在 core 里。GUI 不传递 parent / generation / lineage。
  if (!BeginOperation(allow_incremental
                          ? QStringLiteral("正在准备增量远端备份")
                          : QStringLiteral("正在准备完整远端备份"),
                      /*need_login=*/true)) {
    return false;
  }
  RemoteRequest request;
  request.kind = RemoteOpResult::Kind::kBackup;
  request.endpoint = endpoint_;
  // pin 与地址是同一份快照：每一处提交都显式带上它（见头文件里的说明）。
  request.endpoint.server_key_pin = serverKeyPin().toStdString();
  // 远端缓存的路径里带用户名（<指纹>/<用户名>/），所以这两条产品级操作必须把
  // 当前账户名一起带进后台线程——raw 上/下载不需要它，这里需要。
  request.username = username_.toStdString();
  request.source_directory = source.toStdString();
  request.allow_incremental = allow_incremental;
  // 显示名留空：链的命名（remote-<lineage 前 12 位>-<时间>-g<代数>.bak）由
  // core 按链规则生成——GUI 不参与命名，也就不会造出与链不一致的名字。
  Submit(request);
  return true;
}

bool RemoteController::restoreSnapshot(const QString& snapshot_id,
                                       const QString& destination_directory) {
  const QString id = snapshot_id.trimmed();
  const QString destination = destination_directory.trimmed();
  if (id.isEmpty()) {
    SetStatus(QStringLiteral("error"), QStringLiteral("没有选中云端备份"),
              QStringLiteral("先在列表里选中一条远端备份，再点“恢复”。"));
    return false;
  }
  if (destination.isEmpty()) {
    SetStatus(QStringLiteral("error"), QStringLiteral("还没有选择恢复位置"),
              QStringLiteral("点“选择恢复位置”，指定恢复到哪个目录。"));
    return false;
  }
  if (!BeginOperation(QStringLiteral("正在解析远端依赖链"),
                      /*need_login=*/true)) {
    return false;
  }
  RemoteRequest request;
  request.kind = RemoteOpResult::Kind::kRestore;
  request.endpoint = endpoint_;
  // pin 与地址是同一份快照：每一处提交都显式带上它（见头文件里的说明）。
  request.endpoint.server_key_pin = serverKeyPin().toStdString();
  // 与 backupRemote 同理：缓存路径需要用户名。
  request.username = username_.toStdString();
  request.snapshot_id = id.toStdString();
  request.restore_destination = destination.toStdString();
  Submit(request);
  return true;
}

bool RemoteController::restoreRawArchive(const QString& snapshot_id,
                                         const QString& destination_directory,
                                         const QString& password) {
  const QString id = snapshot_id.trimmed();
  const QString destination = destination_directory.trimmed();
  if (id.isEmpty()) {
    SetStatus(QStringLiteral("error"), QStringLiteral("没有选中云端备份"),
              QStringLiteral("先在列表里选中一条原始归档，再点“恢复”。"));
    return false;
  }
  if (destination.isEmpty()) {
    SetStatus(QStringLiteral("error"), QStringLiteral("还没有选择恢复位置"),
              QStringLiteral("点“选择恢复位置”，指定恢复到哪个目录。"));
    return false;
  }
  // 密码**不是**必填项：只有那份归档确实是加密的，core 才会要求它。这里不做
  // 任何"看起来像密码"的猜测，也不把口令写进日志或状态文本。
  if (!BeginOperation(QStringLiteral("正在下载并校验原始归档"),
                      /*need_login=*/true)) {
    return false;
  }
  // 新的交互：上一次那个"正等着输密码"的会话立刻作废——它的临时归档被删掉，
  // 页面回到"只问目标目录"的第一段。此时 busy_ 已经为真，后台线程不可能还拿着
  // 那个会话（同一时刻只有一个操作）。
  raw_session_.reset();
  raw_awaiting_password_ = false;
  raw_password_error_.clear();
  raw_destination_text_.clear();
  emit rawRestoreStateChanged();
  RemoteRequest request;
  request.kind = RemoteOpResult::Kind::kRestoreRaw;
  request.endpoint = endpoint_;
  // pin 与地址是同一份快照：每一处提交都显式带上它（见头文件里的说明）。
  request.endpoint.server_key_pin = serverKeyPin().toStdString();
  // 缓存路径里带用户名（<指纹>/<用户名>/），下载与恢复都要用它。
  request.username = username_.toStdString();
  request.snapshot_id = id.toStdString();
  request.restore_destination = destination.toStdString();
  request.restore_password = password.toStdString();
  // 显示名只用来给下载下来的临时文件起一个看得懂的名字（core 会把它降级成
  // 单组件文件名）。它**不参与**任何格式判断——那只看内容。
  for (const QVariant& entry : snapshot_items_) {
    const QVariantMap item = entry.toMap();
    if (item.value(QStringLiteral("id")).toString() == id) {
      request.display_name =
          item.value(QStringLiteral("name")).toString().toStdString();
      break;
    }
  }
  Submit(request);
  // Submit 已经把请求**按值**拷进后台任务里了；本地这一份里的密码立刻抹掉：
  // 它不留在主线程的任何对象里。
  WipeSecret(&request.restore_password);
  return true;
}

bool RemoteController::restoreRawArchiveWithPassword(const QString& password) {
  if (raw_session_ == nullptr) {
    // 没有待输入的密码 = 这次交互已经结束了（成功 / 取消 / 致命失败）。
    // 与其对着一个空会话重试，不如让用户重新点"恢复"。
    SetStatus(QStringLiteral("error"), QStringLiteral("这次恢复已经结束"),
              QStringLiteral("请重新点“恢复”，重新选择目标目录。"));
    return false;
  }
  if (!BeginOperation(QStringLiteral("正在恢复原始归档"),
                      /*need_login=*/true)) {
    return false;
  }
  RemoteRequest request;
  request.kind = RemoteOpResult::Kind::kRestoreRaw;
  request.endpoint = endpoint_;
  request.endpoint.server_key_pin = serverKeyPin().toStdString();
  request.username = username_.toStdString();
  request.restore_password = password.toStdString();
  // 交互的其余部分全在会话里：哪个快照、恢复到哪、以及那份**已经下载并校验过
  // 的字节**。所以这一次不会再下载一遍。
  request.raw_session = raw_session_;
  Submit(request);
  WipeSecret(&request.restore_password);
  return true;
}

void RemoteController::cancelRawRestore() {
  if (raw_session_ == nullptr) {
    return;  // 幂等：没有正在进行的交互
  }
  if (busy_) {
    // 后台线程正拿着同一个会话在跑：这时**不能**碰它（会话不是线程安全的）。
    // 界面上的取消按钮在忙碌时本来就是禁用的，这里是第二道闸门。
    SetStatus(QStringLiteral("idle"), QStringLiteral("正在恢复"),
              QStringLiteral("这一步结束之后才能取消。"));
    return;
  }
  // 取消 = 终止这次交互：临时目录（以及里面那份已校验的归档）立刻删掉，
  // 口令清零，界面回到空闲。目标目录本来就没有被改动过（失败即无副作用）。
  raw_session_->Abandon();
  raw_session_.reset();
  raw_awaiting_password_ = false;
  raw_password_error_.clear();
  raw_destination_text_.clear();
  emit rawRestoreStateChanged();
  SetStatus(QStringLiteral("idle"), QStringLiteral("已取消恢复"),
            QStringLiteral("这次恢复已经取消：临时下载的归档已经删掉，"
                           "目标目录没有被改动。"));
}

void RemoteController::ApplyRawRestoreState(const RemoteOpResult& result) {
  // 会话非空 = 这次交互还没结束，正等着用户输入密码。
  raw_session_ = result.raw_session;
  raw_awaiting_password_ = raw_session_ != nullptr;
  last_raw_restore_download_count_ = result.raw_download_count;
  last_raw_restore_archive_path_ =
      QString::fromStdString(result.raw_archive_path);
  if (raw_session_ != nullptr) {
    // 回显**会话里的**目标目录：用户重输密码时不会再选一次，界面上的输入框
    // 就算被改过也不能换一个地方恢复。
    raw_destination_text_ =
        QString::fromStdString(raw_session_->destination_directory());
  } else {
    raw_destination_text_.clear();
  }
  raw_password_error_.clear();
  if (!result.ok && result.error_kind == QStringLiteral("raw-auth")) {
    // 密码那一段自己的错误行：整个流程不关掉，用户直接再输一次。
    raw_password_error_ = result.message.isEmpty()
                              ? DescribeFailure(result.error_kind)
                              : result.message;
  }
  emit rawRestoreStateChanged();
}

// ================= PR #22：连接方式 / SSH 安全通道 / pin 应用反馈
// =================
//
// 这一段的定位：它是"用户点了一下之后**看得见**发生了什么"的全部实现。
// 两类问题都在这里收口：
//
//   1. "应用"没有任何反馈 —— 见 applyServerKeyPin / SetPinApplyFeedback。
//   2. connection refused 说不出原因 —— 见 ConnectionMode + tunnel_ +
//   分层归类。
//
// 它没有、也不会碰的东西：BPSEC1 的线格式、X25519 / HKDF / AES / HMAC、
// remote metadata、增量语义。SSH 只是把 socket 送到服务端门口的那一段路。

QString RemoteController::connectionMode() const {
  return connection_mode_ == ConnectionMode::kSshTunnel
             ? QStringLiteral("ssh")
             : QStringLiteral("direct");
}

QString RemoteController::sshLocalPort() const {
  return ssh_local_port_ > 0 ? QString::number(ssh_local_port_) : QString();
}

QString RemoteController::tunnelState() const {
  switch (tunnel_.state()) {
    case SshTunnelManager::State::kStopped:
      return QStringLiteral("stopped");
    case SshTunnelManager::State::kStarting:
      return QStringLiteral("starting");
    case SshTunnelManager::State::kReady:
      return QStringLiteral("ready");
    case SshTunnelManager::State::kFailed:
      return QStringLiteral("failed");
    case SshTunnelManager::State::kStopping:
      return QStringLiteral("stopping");
  }
  return QStringLiteral("stopped");
}

bool RemoteController::setConnectionMode(const QString& mode) {
  ConnectionMode next = connection_mode_;
  if (mode == QStringLiteral("ssh")) {
    next = ConnectionMode::kSshTunnel;
  } else if (mode == QStringLiteral("direct")) {
    next = ConnectionMode::kDirect;
  } else {
    // 不认识的模式明确拒绝，而不是悄悄当成直连 —— 那会让"我以为在用隧道"
    // 变成一个说不清的 connection refused。
    return false;
  }
  if (connection_mode_ == next) {
    return true;
  }
  connection_mode_ = next;
  // 只改配置：不联网、不登录、不断开正在跑的通道、也不杀进程。切换模式与
  // "应用 pin"是同一类动作 —— 配置，不是连接测试。
  emit connectionChanged();
  return true;
}

void RemoteController::setSshHost(const QString& host) {
  const QString text = host.trimmed();
  if (ssh_host_ == text) {
    return;
  }
  ssh_host_ = text;
  emit connectionChanged();
}

void RemoteController::setSshLocalPort(const QString& port_text) {
  const QString text = port_text.trimmed();
  int value = 0;
  if (!text.isEmpty() && text != QStringLiteral("0")) {
    bool ok = false;
    const int parsed = text.toInt(&ok);
    // 这一项没有自己的错误行，所以非法输入**退回自动分配**而不是留着用不了
    // 的数字：自动分配永远是安全的（它只挑空闲端口）。
    value = (ok && parsed >= 1 && parsed <= 65535) ? parsed : 0;
  }
  if (ssh_local_port_ == value) {
    return;
  }
  ssh_local_port_ = value;
  emit connectionChanged();
}

void RemoteController::setSshProgram(const QString& program) {
  const QString text = program.trimmed();
  if (ssh_program_ == text) {
    return;
  }
  ssh_program_ = text;
  emit connectionChanged();
}

bool RemoteController::TunnelMatchesEndpoint() const {
  if (!tunnel_.IsReady()) {
    return false;
  }
  if (tunnel_.IsExternalReuse()) {
    // 外部复用的监听者不是本程序启动的，它的转发目标不归本程序管，也无从
    // 查询。复用的安全性由**下一层**保证：BPSEC1 的 pin 会验证隧道那头到底
    // 是谁（§15 的 B 类）。所以这里不做"猜它转发到哪"的假判断。
    return true;
  }
  const SshTunnelManager::Options& used = tunnel_.options();
  return used.ssh_target == ssh_host_ &&
         used.remote_host == QString::fromStdString(endpoint_.host) &&
         used.remote_port == static_cast<int>(endpoint_.port) &&
         used.local_port == ssh_local_port_ && used.ssh_program == ssh_program_;
}

bool RemoteController::StartTunnelForEndpoint(const QString& action_text) {
  Q_UNUSED(action_text);
  SshTunnelManager::Options options;
  options.ssh_program = ssh_program_;
  options.ssh_target = ssh_host_;
  options.remote_host = QString::fromStdString(endpoint_.host);
  options.remote_port = static_cast<int>(endpoint_.port);
  options.local_port = ssh_local_port_;
  // 返回 false = 请求没被受理（正在建 / 正在关）。调用方不需要额外处理：
  // 状态机的下一次变化会走到 OnTunnelStateChanged。
  const bool accepted = tunnel_.Start(options);
  emit tunnelChanged();
  return accepted;
}

void RemoteController::DeferRequest(const RemoteRequest& request,
                                    const QString& action_text,
                                    ErrorSurface surface) {
  deferred_.active = true;
  deferred_.request = request;
  deferred_.action_text = action_text;
  deferred_.surface = surface;
  last_deferred_action_ = action_text;
}

void RemoteController::FlushDeferredRequest() {
  if (!deferred_.active || !tunnel_.IsReady() || tunnel_.localPort() <= 0) {
    return;
  }
  RemoteRequest request = deferred_.request;
  const QString action = deferred_.action_text;
  deferred_ = DeferredRequest();
  request.endpoint.host = std::string("127.0.0.1");
  request.endpoint.port = static_cast<std::uint16_t>(tunnel_.localPort());
  ++deferred_submit_count_;
  SetStatus(QStringLiteral("running"), action, QString());
  DispatchRequest(request);
}

void RemoteController::FailDeferredRequest() {
  if (!deferred_.active) {
    return;
  }
  const ErrorSurface surface = deferred_.surface;
  deferred_ = DeferredRequest();
  last_error_kind_ = tunnel_.failureKindName();
  last_detail_ = tunnel_.diagnosticText().toStdString();
  SetBusy(false, QString());
  ReportSurfaceError(surface, tunnel_.failureText());
  if (surface != ErrorSurface::kBanner) {
    // 表单里已经有原因了，横幅不能继续挂着"正在登录"这种过期状态。
    SetIdleBaseline();
  }
  emit operationFinished(QStringLiteral("tunnel"), false);
}

void RemoteController::OnTunnelStateChanged() {
  emit tunnelChanged();
  if (tunnel_only_wait_) {
    if (tunnel_.IsReady()) {
      tunnel_only_wait_ = false;
      SetBusy(false, QString());
      SetStatus(
          QStringLiteral("idle"), QStringLiteral("安全通道已建立"),
          QStringLiteral("%1 -> %2（本程序启动的 ssh，退出时会自动结束）")
              .arg(tunnel_.localEndpointText(), tunnel_.remoteEndpointText()));
      // 已经登录的用户点"建立连接"，要看到的是**数据**，不是一句"通道好了"。
      if (authenticated_) {
        refreshList();
      }
      return;
    }
    if (tunnel_.state() == SshTunnelManager::State::kFailed) {
      tunnel_only_wait_ = false;
      last_error_kind_ = tunnel_.failureKindName();
      last_detail_ = tunnel_.diagnosticText().toStdString();
      SetBusy(false, QString());
      ReportSurfaceError(ErrorSurface::kBanner, tunnel_.failureText());
      return;
    }
  }
  if (deferred_.active) {
    if (tunnel_.IsReady()) {
      FlushDeferredRequest();
      return;
    }
    if (tunnel_.state() == SshTunnelManager::State::kFailed) {
      FailDeferredRequest();
      return;
    }
  }
}

bool RemoteController::ensureConnection(const QString& host,
                                        const QString& port_text) {
  ClearSurfaceError(ErrorSurface::kBanner);
  // 只校验地址与端口：建通道是传输层的事，与用户名、口令、pin 都无关。
  const QString trimmed_host = host.trimmed();
  if (trimmed_host.isEmpty()) {
    last_error_kind_ = QStringLiteral("validation");
    ReportSurfaceError(ErrorSurface::kBanner,
                       QStringLiteral("请输入服务器地址"));
    return false;
  }
  bool port_ok = false;
  const int port = port_text.trimmed().toInt(&port_ok);
  if (!port_ok || port < 1 || port > 65535) {
    last_error_kind_ = QStringLiteral("validation");
    ReportSurfaceError(ErrorSurface::kBanner,
                       QStringLiteral("端口要填 1 到 65535 之间的整数"));
    return false;
  }
  CommitHostPort(trimmed_host, port);

  if (connection_mode_ == ConnectionMode::kDirect) {
    // 直连模式没有"通道"这个前置动作。如实说明，而不是假装做了一个动作。
    SetStatus(QStringLiteral("idle"), QStringLiteral("直连模式"),
              QStringLiteral("当前是直连模式，不需要安全通道；"
                             "点“登录”会直接连接 %1:%2。")
                  .arg(trimmed_host)
                  .arg(port));
    return true;
  }
  if (TunnelMatchesEndpoint() && tunnel_.localPort() > 0) {
    SetStatus(QStringLiteral("idle"), QStringLiteral("安全通道已建立"),
              QStringLiteral("%1 -> %2:%3")
                  .arg(tunnel_.localEndpointText(), trimmed_host)
                  .arg(port));
    if (authenticated_) {
      return refreshList();
    }
    return true;
  }
  if (busy_) {
    last_error_kind_ = QStringLiteral("busy");
    ReportSurfaceError(
        ErrorSurface::kBanner,
        QStringLiteral("正在%1，请等它结束后再试")
            .arg(busy_action_.isEmpty() ? QStringLiteral("处理上一个请求")
                                        : busy_action_));
    return false;
  }
  SetBusy(true, QStringLiteral("正在建立安全通道"));
  SetStatus(
      QStringLiteral("running"), QStringLiteral("正在建立安全通道"),
      QStringLiteral("SSH %1 -> %2:%3").arg(ssh_host_, trimmed_host).arg(port));
  tunnel_only_wait_ = true;
  // Stop() 是有界的同步操作，先做掉它，后面的 Start() 就一定能被受理
  // （否则"上一个通道正在关闭"会让这次点击变成一次静默的 no-op）。
  tunnel_.Stop();
  emit tunnelChanged();
  StartTunnelForEndpoint(QStringLiteral("正在建立安全通道"));
  return true;
}

void RemoteController::stopTunnel() {
  const bool waiting = deferred_.active || tunnel_only_wait_;
  const ErrorSurface surface =
      deferred_.active ? deferred_.surface : ErrorSurface::kBanner;
  deferred_ = DeferredRequest();
  tunnel_only_wait_ = false;
  tunnel_.Stop();
  emit tunnelChanged();
  if (waiting) {
    // 用户在等一次操作时手动关了通道：明确结束那次操作，而不是让它挂着。
    last_error_kind_ = QStringLiteral("tunnel-not-ready");
    SetBusy(false, QString());
    ReportSurfaceError(surface, DescribeFailure(last_error_kind_));
    return;
  }
  SetIdleBaseline();
}

QString RemoteController::CommitServerKeyPin(const QString& pin) {
  // 与 setServerKeyPin 同一份校验与落点：解析器只有共享的那一个，界面不做
  // 第二套"看起来对"的判断。
  ClearSurfaceError(ErrorSurface::kServerKey);
  const QString text = pin.trimmed();
  if (text.isEmpty()) {
    last_error_kind_ = QStringLiteral("validation");
    ReportSurfaceError(
        ErrorSurface::kServerKey,
        QStringLiteral("请填写服务器身份指纹（向服务器管理员索取，"
                       "形如 sha256: 开头的 64 位十六进制）"));
    return QStringLiteral("invalid");
  }
  backupproject::net::ServerKeyPin parsed;
  std::string parse_error;
  if (!backupproject::net::ParseServerKeyPin(text.toStdString(), &parsed,
                                             &parse_error)) {
    last_error_kind_ = QStringLiteral("validation");
    ReportSurfaceError(ErrorSurface::kServerKey,
                       QStringLiteral("服务器身份指纹不合法：%1")
                           .arg(QString::fromStdString(parse_error)));
    std::fprintf(stderr, "[remote] 服务端 pin 不合法：%s\n",
                 parse_error.c_str());
    return QStringLiteral("invalid");
  }
  if (server_key_pin_ == text) {
    return QStringLiteral("unchanged");
  }
  server_key_pin_ = text;
  endpoint_.server_key_pin = server_key_pin_.toStdString();
  emit serverKeyPinChanged();
  return QStringLiteral("applied");
}

void RemoteController::SetPinApplyFeedback(const QString& state,
                                           const QString& message) {
  if (pin_apply_state_ == state && pin_apply_message_ == message) {
    return;
  }
  pin_apply_state_ = state;
  pin_apply_message_ = message;
  emit pinApplyChanged();
}

void RemoteController::clearPinApplyState() {
  if (pin_apply_state_.isEmpty() && pin_apply_message_.isEmpty()) {
    return;
  }
  pin_apply_state_.clear();
  pin_apply_message_.clear();
  emit pinApplyChanged();
}

void RemoteController::PublishPinApplyFeedback(const QString& state) {
  if (state == QStringLiteral("applied")) {
    // "活动连接"= 此刻真的有一条 socket，或者有一条操作正在跑。已经登录但
    // 空闲不算：BPNET1 是每次操作建立连接，下一次连接就会用上新 pin。
    const bool active_connection = client_.connected() || busy_;
    SetPinApplyFeedback(
        QStringLiteral("applied"),
        active_connection
            ? QStringLiteral("✓ 已应用；当前连接保持不变，下次重连时生效")
            : QStringLiteral("✓ 已应用，将在下一次连接时用于服务器身份校验"));
    return;
  }
  if (state == QStringLiteral("unchanged")) {
    // 重复提交同一个值不是一个"什么都没发生"：它本身就是一个确定的结果。
    SetPinApplyFeedback(QStringLiteral("unchanged"),
                        QStringLiteral("✓ 已是当前服务器身份指纹"));
    return;
  }
  // 不合法：红色原因已经在输入框下面了，绿色那一行必须收起来 ——
  // 不能同时显示"不合法"和"✓ 已应用"。
  SetPinApplyFeedback(QStringLiteral("invalid"), QString());
}

QString RemoteController::applyServerKeyPin(const QString& pin) {
  const QString state = CommitServerKeyPin(pin);
  PublishPinApplyFeedback(state);
  return state;
}

void RemoteController::CommitHostPort(const QString& host, int port) {
  // 与 CommitEndpoint 同一份落点逻辑，但不动 username_：建通道时用户名不
  // 属于这次动作的一部分。
  if (endpoint_.host == host.toStdString() &&
      endpoint_.port == static_cast<std::uint16_t>(port)) {
    return;
  }
  endpoint_.host = host.toStdString();
  endpoint_.port = static_cast<std::uint16_t>(port);
  endpoint_.server_key_pin = serverKeyPin().toStdString();
  emit endpointChanged();
}

bool RemoteController::registerAccountWithPin(const QString& host,
                                              const QString& port_text,
                                              const QString& username,
                                              const QString& password,
                                              const QString& confirm_password,
                                              const QString& base_pin) {
  const QString pin_state = CommitServerKeyPin(base_pin);
  if (pin_state == QStringLiteral("invalid")) {
    last_error_kind_ = QStringLiteral("validation");
    ReportSurfaceError(
        ErrorSurface::kRegister,
        QStringLiteral("服务器身份指纹不合法，请先按上面的提示修正，再点注册"));
    return false;
  }
  // 自动提交同样要给出"已应用"的可见结论（§4：登录 / 注册自动 commit 成功
  // 之后界面上的状态也是"已应用"）。少了这一句，用户点完登录只会看到指纹框
  // 还是老样子，又会以为自己填的没生效。
  PublishPinApplyFeedback(pin_state);
  return registerAccount(host, port_text, username, password, confirm_password);
}

bool RemoteController::loginWithPin(const QString& host,
                                    const QString& port_text,
                                    const QString& username,
                                    const QString& password,
                                    const QString& base_pin) {
  // 提交之前先把**用户此刻看得见的那一个** pin 提交掉。人工验收里最自然的
  // 路径就是"填 pin -> 填账号 -> 点登录"，旧实现会让这次登录用上一次的
  // 值（没有就是空），于是要么莫名失败、要么用户以为填的已经生效了。
  const QString pin_state = CommitServerKeyPin(base_pin);
  if (pin_state == QStringLiteral("invalid")) {
    // 一个字节都不发。红色原因已经贴在指纹输入框下面；这里再给一句"所以这次
    // 登录没有开始"，否则用户会以为登录按钮坏了。
    last_error_kind_ = QStringLiteral("validation");
    ReportSurfaceError(
        ErrorSurface::kLogin,
        QStringLiteral("服务器身份指纹不合法，请先按上面的提示修正，再点登录"));
    return false;
  }
  // 与"应用"按钮同一句话：这两条路径不能各说各的。
  PublishPinApplyFeedback(pin_state);
  return login(host, port_text, username, password);
}

bool RemoteController::waitForTunnelIdle(int timeout_ms) {
  QEventLoop loop;
  QTimer poll;
  poll.setInterval(10);
  QObject::connect(&poll, &QTimer::timeout, &loop, [this, &loop, timeout_ms] {
    Q_UNUSED(timeout_ms);
    if (!tunnel_.IsBusy()) {
      loop.quit();
    }
  });
  QTimer deadline;
  deadline.setSingleShot(true);
  QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
  poll.start();
  deadline.start(timeout_ms < 1 ? 1 : timeout_ms);
  loop.exec();
  poll.stop();
  return !tunnel_.IsBusy();
}

bool RemoteController::killOwnedTunnelForTest() {
#if defined(Q_OS_UNIX)
  const qint64 pid = tunnel_.processId();
  if (pid <= 0) {
    return false;
  }
  // 模拟"隧道在使用中被打断"：直接 SIGKILL 那条 ssh。产品代码里没有这条
  // 路径，它只服务于自动化验证（C12）。
  return ::kill(static_cast<pid_t>(pid), SIGKILL) == 0;
#else
  return false;
#endif
}

}  // namespace backup_modern
