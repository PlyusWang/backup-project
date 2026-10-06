// ui/modern/ssh_tunnel_manager.cpp
//
// 见头文件的说明。这里只强调三条实现纪律：
//
//   1. **没有 shell**。QProcess::setProgram + setArguments，参数一个一个进
//      向量，用户输入永远不被拼进任何命令行字符串。
//   2. **有界**。等待 Ready 是"固定间隔定时器 + 单调截止时间"，停止进程是
//      "terminate → 有界等待 → 必要时 kill"，两处都没有无限循环。
//   3. **fail closed**。SSH 自己拒绝 host key 时，本类把失败如实报上去，
//      绝不加 StrictHostKeyChecking=no / UserKnownHostsFile=/dev/null 绕过。
//
// 另外一处容易被写错的地方：QProcess 的 finished 信号是在 QProcess 自己的
// 栈上发出来的，**不能**在那个信号里同步 delete 这个 QProcess。CloseProcess()
// 因此统一走 disconnect + deleteLater()，任何调用路径都安全。
// ---- 数据流 ----
//
// 调用方填 Options -> Start() -> 参数向量交给 QProcess -> kStarting 阶段用
// QTcpSocket 探 127.0.0.1:<local_port>，探通才 kReady。stateChanged() 是唯一
// 的对外通知出口，页面只订阅它，不轮询本类。
//
// ---- 边界 ----
//
// 不管凭据与身份：口令、私钥、known_hosts 全部由 ssh 自己处理，服务端身份由
// 隧道里的 BPSEC1 pin 校验，本类不做自动重连（进程退出只报 kFailed）。
// 全部成员只在 GUI 线程触碰（含 waitForFinished 的阻塞等待），所以内部无锁。

#include "ssh_tunnel_manager.h"

#include <QAbstractSocket>
#include <QFileInfo>
#include <QHostAddress>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringList>
#include <QTcpServer>

namespace backup_modern {
namespace {

// 建立阶段的探测间隔与次数上限。截止时间（单调时钟）才是真正的闸门。
//
// 刻意**没有** kReadyPollMs："Ready 之后还要不要继续探"这个问题的答案是
// "不要" —— ssh -L 的本地 listener 每接受一次连接就可能真的向 ECS 建一条
// 转发通道，空闲 GUI 周期性地去 connect 等于持续给 backup-server 制造连接。
constexpr int kStartingPollMs = 120;
constexpr int kMaxReadinessAttempts = 400;
// 自动端口撞车时最多重挑几次。
constexpr int kMaxAutoPortRetries = 3;
// 诊断文本的保留上限：ssh 正常时几乎不输出，异常时可能刷屏。
constexpr int kStderrKeepBytes = 8192;
constexpr int kDiagnosticDisplayChars = 2000;

}  // namespace

SshTunnelManager::SshTunnelManager(QObject* parent) : QObject(parent) {
  poll_.setSingleShot(false);
  connect(&poll_, &QTimer::timeout, this, &SshTunnelManager::OnPollTick);
}

SshTunnelManager::~SshTunnelManager() {
  // 析构必须把**自己启动的**进程收干净：GUI 退出后不允许留下孤儿 ssh -N。
  // Stop() 内部是有界等待，所以这里不会卡住退出流程。
  Stop();
}

// ---- 静态工具 ----

bool SshTunnelManager::PickFreeLoopbackPort(int* out_port, QString* error) {
  if (out_port == nullptr) {
    if (error != nullptr) {
      *error = QStringLiteral("输出指针为空");
    }
    return false;
  }
  QTcpServer server;
  // 这是"先向内核要一个端口，立刻放掉，稍后再让 ssh 去绑"的两步操作，中间
  // 有天然竞态：别的进程可能抢走同一个端口。所以返回值只是**建议**不是预留，
  // 真撞车由 OnProcessFinished() 的有界重挑兜底。监听 socket 从未 accept 过
  // 连接，close() 不会留下 TIME_WAIT，端口可以立刻被 ssh 复用。
  // 只绑回环：这个端口是给本机 GUI 用的，不应该出现在任何其它接口上。
  if (!server.listen(QHostAddress::LocalHost, 0)) {
    if (error != nullptr) {
      *error = QStringLiteral("无法在 127.0.0.1 上申请空闲端口：%1")
                   .arg(server.errorString());
    }
    return false;
  }
  const quint16 port = server.serverPort();
  server.close();
  if (port < 1024) {
    if (error != nullptr) {
      *error = QStringLiteral("内核给出的端口 %1 不在预期范围内")
                   .arg(static_cast<int>(port));
    }
    return false;
  }
  *out_port = static_cast<int>(port);
  return true;
}

// 语义是"此刻有没有 listener 在接"。它是**阻塞**调用（waitForConnected），
// 只在 Start() 的受理路径上用一次、超时上限 300ms；不要放进任何周期路径 ——
// 每次成功 connect 都可能让 ssh -L 真的向远端建一条 forwarded channel，
// 那份代价不在本机，而是记在 backup-server 头上。
bool SshTunnelManager::IsLoopbackPortOpen(int port, int timeout_ms,
                                          QString* error) {
  if (port < 1 || port > 65535) {
    if (error != nullptr) {
      *error = QStringLiteral("端口 %1 不在 1..65535 内").arg(port);
    }
    return false;
  }
  QTcpSocket socket;
  socket.connectToHost(QHostAddress::LocalHost, static_cast<quint16>(port));
  if (!socket.waitForConnected(timeout_ms < 0 ? 300 : timeout_ms)) {
    if (error != nullptr) {
      *error = QStringLiteral("127.0.0.1:%1 连不上：%2")
                   .arg(port)
                   .arg(socket.errorString());
    }
    return false;
  }
  // 只确认"有 listener 在接"，拿到结果就立刻放手：不在对端留常驻连接。
  socket.abort();
  return true;
}

QString SshTunnelManager::SanitizeSshStderr(const QByteArray& raw) {
  if (raw.isEmpty()) {
    return QString();
  }
  const QByteArray kept =
      raw.size() > kStderrKeepBytes ? raw.right(kStderrKeepBytes) : raw;
  // 处理顺序不能换：先按**字节**截尾（UTF-8 可能被截在多字节字符中间，交给
  // 后面的替换吸收），再删 ANSI 序列 —— 必须赶在控制字符替换之前，否则 ESC
  // 已经变成空格，CSI 正则再也匹配不上，界面里就会留下乱码方块。
  QString text = QString::fromUtf8(kept);
  // ANSI 转义（CSI）：先整段删掉，否则界面里会出现乱码方块。
  static const QRegularExpression ansi(
      QStringLiteral("\x1b\\[[0-9;?]*[ -/]*[@-~]"));
  text.remove(ansi);
  // 其余控制字符（含 BEL / NUL / 退格）换成空格：诊断文本要能安全地进
  // 界面、进日志、进证据包。
  QString cleaned;
  cleaned.reserve(text.size());
  for (const QChar ch : text) {
    if (ch == QLatin1Char('\n')) {
      cleaned.append(ch);
    } else if (ch.unicode() < 0x20 || ch.unicode() == 0x7f) {
      cleaned.append(QLatin1Char(' '));
    } else {
      cleaned.append(ch);
    }
  }
  // 防御性脱敏：BatchMode=yes 之后 ssh 不可能问口令，所以正常情况下这里
  // 一个字都不会改；但诊断文本最终会进证据包，"万一"不能变成"泄漏"。
  static const QRegularExpression secretish(
      QStringLiteral("(?i)(pass(word|phrase)?)\\s*[:=]?\\s*\\S+"));
  cleaned.replace(secretish, QStringLiteral("\\1=<redacted>"));
  // 连续空行压掉：ssh 的警告框会带一大堆分隔行。
  static const QRegularExpression blanks(QStringLiteral("\n{3,}"));
  cleaned.replace(blanks, QStringLiteral("\n\n"));
  cleaned = cleaned.trimmed();
  if (cleaned.size() > kDiagnosticDisplayChars) {
    // 保留**尾部**：最后几行才是真正的原因。
    cleaned = QStringLiteral("…（前文已省略）…\n") +
              cleaned.right(kDiagnosticDisplayChars);
  }
  return cleaned;
}

SshTunnelManager::Failure SshTunnelManager::ClassifySshStderr(
    const QString& sanitized) {
  // 模式串是 OpenSSH 的英文原文，全部小写匹配。前提假设：ssh 的 stderr 没有被
  // gettext 本地化。假设不成立时也不会误判成更"安全"的类别，只会一路落到末尾
  // 的 kExited，也就是退化成"通道已断开"这条通用结论。
  const QString text = sanitized.toLower();
  const auto has = [&text](const char* needle) {
    return text.contains(QLatin1String(needle));
  };
  // 顺序即优先级。host key 必须排在 auth 之前：known_hosts 冲突时 ssh 也会
  // 打印一堆别的行，但那不是认证问题。
  if (has("host key verification failed") ||
      has("remote host identification has changed") ||
      has("no matching host key") || has("no host key") ||
      has("host key has changed")) {
    return Failure::kHostKeyFailed;
  }
  if (has("permission denied") || has("too many authentication failures") ||
      has("no supported authentication methods") ||
      has("authentication failed")) {
    return Failure::kAuthFailed;
  }
  if (has("address already in use") || has("cannot listen to port") ||
      has("channel_setup_fwd_listener") || has("failed to allocate") ||
      has("bind: ") || has("administratively prohibited") ||
      has("port forwarding failed")) {
    return Failure::kForwardFailed;
  }
  if (has("could not resolve hostname") || has("connection refused") ||
      has("connection timed out") || has("network is unreachable") ||
      has("no route to host") || has("operation timed out") ||
      has("connection closed by remote host") ||
      has("connection reset by peer") || has("broken pipe") ||
      has("name or service not known")) {
    return Failure::kHostUnreachable;
  }
  return Failure::kExited;
}

QString SshTunnelManager::FailureKindName(Failure failure) {
  switch (failure) {
    case Failure::kNone:
      return QStringLiteral("none");
    case Failure::kInvalidTarget:
      return QStringLiteral("ssh-invalid-target");
    case Failure::kSshMissing:
      return QStringLiteral("ssh-missing");
    case Failure::kSpawnFailed:
      return QStringLiteral("ssh-spawn");
    case Failure::kHostUnreachable:
      return QStringLiteral("ssh-host-unreachable");
    case Failure::kHostKeyFailed:
      return QStringLiteral("ssh-hostkey");
    case Failure::kAuthFailed:
      return QStringLiteral("ssh-auth");
    case Failure::kForwardFailed:
      return QStringLiteral("ssh-forward");
    case Failure::kTimeout:
      return QStringLiteral("ssh-timeout");
    case Failure::kExited:
      return QStringLiteral("ssh-exit");
  }
  return QStringLiteral("ssh-exit");
}

QString SshTunnelManager::FailureTitle(Failure failure) {
  switch (failure) {
    case Failure::kNone:
      return QString();
    case Failure::kInvalidTarget:
      return QStringLiteral("SSH 主机填写有误");
    case Failure::kSshMissing:
      return QStringLiteral("找不到 ssh 命令");
    case Failure::kSpawnFailed:
      return QStringLiteral("ssh 启动失败");
    case Failure::kHostUnreachable:
      return QStringLiteral("SSH 服务器连不上");
    case Failure::kHostKeyFailed:
      return QStringLiteral("SSH 服务器身份校验失败");
    case Failure::kAuthFailed:
      return QStringLiteral("SSH 认证失败");
    case Failure::kForwardFailed:
      return QStringLiteral("本地端口转发失败");
    case Failure::kTimeout:
      return QStringLiteral("建立安全通道超时");
    case Failure::kExited:
      return QStringLiteral("安全通道已断开");
  }
  return QStringLiteral("安全通道建立失败");
}

SshTunnelManager::Failure SshTunnelManager::FailureFromKindName(
    const QString& name) {
  // 顺序与 FailureKindName 一一对应；两处必须同时改。有独立测试
  // （SSH-U20）遍历全部枚举值验证来回一致。
  static const struct {
    const char* name;
    Failure failure;
  } kTable[] = {
      {"ssh-invalid-target", Failure::kInvalidTarget},
      {"ssh-missing", Failure::kSshMissing},
      {"ssh-spawn", Failure::kSpawnFailed},
      {"ssh-host-unreachable", Failure::kHostUnreachable},
      {"ssh-hostkey", Failure::kHostKeyFailed},
      {"ssh-auth", Failure::kAuthFailed},
      {"ssh-forward", Failure::kForwardFailed},
      {"ssh-timeout", Failure::kTimeout},
      {"ssh-exit", Failure::kExited},
  };
  for (const auto& entry : kTable) {
    if (name == QLatin1String(entry.name)) {
      return entry.failure;
    }
  }
  return Failure::kNone;
}

QString SshTunnelManager::FailureUserText(Failure failure) {
  switch (failure) {
    case Failure::kNone:
      return QString();
    case Failure::kInvalidTarget:
      return QStringLiteral(
          "SSH 主机填写不合法：请填 ~/.ssh/config 里的别名（例如 aliyun-ecs）"
          "或 用户名@主机名，不要以减号开头，也不要包含空格。");
    case Failure::kSshMissing:
      return QStringLiteral(
          "本机找不到 ssh 命令，无法建立安全通道。请安装 OpenSSH 客户端，"
          "或指定 ssh 可执行文件的路径。");
    case Failure::kSpawnFailed:
      return QStringLiteral("ssh 进程启动失败（权限或系统资源问题）。");
    case Failure::kHostUnreachable:
      return QStringLiteral(
          "连不上 SSH 服务器：主机名解析失败、端口不通或被拒绝。"
          "请确认 SSH 主机写对了、网络可达。");
    case Failure::kHostKeyFailed:
      return QStringLiteral(
          "SSH 服务器身份校验失败，请先检查 SSH 配置。"
          "本程序不会自动接受未知的 SSH 主机密钥。");
    case Failure::kAuthFailed:
      return QStringLiteral(
          "SSH 连接需要交互式认证，请先配置 SSH 密钥或 ssh-agent。"
          "本程序不会在后台弹出密码提示。");
    case Failure::kForwardFailed:
      return QStringLiteral(
          "本地端口转发没有建立起来（本地端口被占用，或服务端拒绝了转发）。");
    case Failure::kTimeout:
      return QStringLiteral(
          "建立安全通道超时：ssh 进程还在，但本地端口一直没有变成可连接。");
    case Failure::kExited:
      return QStringLiteral("安全通道已断开（ssh 进程退出了）。");
  }
  return QString();
}

bool SshTunnelManager::IsValidSshTarget(const QString& target, QString* error) {
  const QString text = target.trimmed();
  if (text.isEmpty()) {
    if (error != nullptr) {
      *error = QStringLiteral("SSH 主机为空");
    }
    return false;
  }
  if (text != target) {
    if (error != nullptr) {
      *error = QStringLiteral("SSH 主机首尾有空白字符");
    }
    return false;
  }
  // 以 '-' 开头的值会被 ssh 当成选项解析（参数注入）。这里直接拒绝；
  // 真正的进程参数里仍然再加一个 "--" 作为第二道防线。
  if (text.startsWith(QLatin1Char('-'))) {
    if (error != nullptr) {
      *error = QStringLiteral("SSH 主机不可以 '-' 开头");
    }
    return false;
  }
  for (const QChar ch : text) {
    if (ch.isSpace() || ch.unicode() < 0x20 || ch.unicode() == 0x7f) {
      if (error != nullptr) {
        *error = QStringLiteral("SSH 主机里不能有空白或控制字符");
      }
      return false;
    }
  }
  return true;
}

// ---- 只读访问器 ----

QString SshTunnelManager::localEndpointText() const {
  if (local_port_ <= 0) {
    return QString();
  }
  return QStringLiteral("127.0.0.1:%1").arg(local_port_);
}

QString SshTunnelManager::remoteEndpointText() const {
  return QStringLiteral("%1:%2")
      .arg(options_.remote_host)
      .arg(options_.remote_port);
}

qint64 SshTunnelManager::processId() const {
  if (process_ == nullptr || !owned_) {
    return 0;
  }
  return static_cast<qint64>(process_->processId());
}

QString SshTunnelManager::stateText() const {
  switch (state_) {
    case State::kStopped:
      return QStringLiteral("安全通道未启动");
    case State::kStarting:
      return QStringLiteral("正在建立安全通道…");
    case State::kReady:
      return external_reuse_ ? QStringLiteral("正在使用已有的本地通道")
                             : QStringLiteral("安全通道已建立");
    case State::kFailed:
      return QStringLiteral("安全通道建立失败");
    case State::kStopping:
      return QStringLiteral("正在关闭安全通道…");
  }
  return QStringLiteral("安全通道未启动");
}

QString SshTunnelManager::failureText() const {
  return FailureUserText(failure_);
}

QString SshTunnelManager::failureKindName() const {
  return FailureKindName(failure_);
}

// ---- 状态机 ----

void SshTunnelManager::SetState(State state) {
  if (state_ == state) {
    return;
  }
  state_ = state;
  emit stateChanged();
}

void SshTunnelManager::DisarmProbe() {
  if (probe_ == nullptr) {
    return;
  }
  // 与 CloseProcess() 同理：这个函数会在 QTcpSocket 自己的 connected 信号栈上
  // 被调用（进入 Ready 的那一刻），在那里同步析构 socket 会让 Qt 继续使用一块
  // 已经释放的内存。先断开信号、再交给事件循环。
  QTcpSocket* dying = probe_.release();
  dying->disconnect(this);
  dying->abort();
  dying->deleteLater();
}

void SshTunnelManager::CloseProcess() {
  // 三条纪律，顺序都不能换：
  //
  //   1. **先断开信号**。terminate() + waitForFinished() 会在等待期间把
  //      finished 派发出来，重入 OnProcessFinished -> Fail -> CloseProcess，
  //      内层会把 process_ 置空；外层回来后如果还去碰 process_，就是一次
  //      已经发生过的空指针解引用（症状是 QObject::disconnect 与
  //      postEvent 各报一条 "Unexpected nullptr"）。
  //   2. 再 terminate → 有界等待 → 必要时 kill。
  //   3. 最后 release + deleteLater()：绝不在 QProcess 自己的信号栈上同步
  //      析构它。
  if (process_ == nullptr) {
    return;
  }
  process_->disconnect(this);
  // 最坏情况这里会阻塞 GUI 线程约 5 秒（terminate 等 3s，kill 后再等 2s）。
  // 这是有意的取舍：宁可短暂卡住界面，也不留孤儿 ssh -N 或僵尸进程。
  if (owned_ && process_->state() != QProcess::NotRunning) {
    process_->terminate();
    if (!process_->waitForFinished(3000)) {
      process_->kill();
      process_->waitForFinished(2000);
    }
  }
  QProcess* dying = process_.release();
  dying->deleteLater();
  owned_ = false;
}

// 失败是**终态**：进来就把进程、probe、定时器全拆掉，并把 local_port_ 清零，
// 免得界面还显示一个已经不存在的端点。
// 这里直接给 state_ 赋值再 emit 而不走 SetState()：状态很可能本来就是
// kFailed，只是失败原因变了（先超时，随后又读到真正的 stderr），相等短路会把
// 这条更新吞掉，用户就会一直看到过期原因。
void SshTunnelManager::Fail(Failure failure, const QString& detail) {
  failure_ = failure;
  if (!detail.isEmpty()) {
    diagnostic_ = detail;
  }
  // 失败之后不留任何进程：本对象自己的收掉，外部复用的只解除借用。
  //
  // stop_requested_ 在整个拆除过程中保持置位：这样万一还有一条 finished
  // 走在路上，OnProcessFinished 会直接返回，而不是把"超时"改写成另一个
  // 原因（那正是 SSH-U07b 抓到过的问题）。
  stop_requested_ = true;
  poll_.stop();
  DisarmProbe();
  CloseProcess();
  stop_requested_ = false;
  external_reuse_ = false;
  local_port_ = 0;
  readiness_attempts_ = 0;
  state_ = State::kFailed;
  emit stateChanged();
}

void SshTunnelManager::SetSshProgram(const QString& program) {
  options_.ssh_program = program;
}

// 幂等：已经 kStopped 且既无自有进程也无外部借用时立刻返回，连信号都不发，
// 关闭流程里重复调用不会引起多余重绘。
// 拆除期间 stop_requested_ 保持置位，屏蔽在途的 finished / errorOccurred
// 回调；否则"用户主动关闭"会被改写成一次失败（与 Fail() 同一手法）。
void SshTunnelManager::Stop() {
  if (state_ == State::kStopped && process_ == nullptr && !external_reuse_) {
    return;
  }
  const bool had_owned_process = owned_ && process_ != nullptr;
  if (had_owned_process) {
    state_ = State::kStopping;
    emit stateChanged();
  }
  stop_requested_ = true;
  poll_.stop();
  DisarmProbe();
  CloseProcess();
  stop_requested_ = false;
  external_reuse_ = false;
  local_port_ = 0;
  failure_ = Failure::kNone;
  failure_detail_.clear();
  stderr_buffer_.clear();
  diagnostic_.clear();
  readiness_attempts_ = 0;
  state_ = State::kStopped;
  emit stateChanged();
}

// 只在这里重置自动重挑预算：一次新的用户请求算一份新的重试额度。内部的自动
// 重挑走 StartInternal()，刻意绕过这一行，才能在有限次数内把额度用掉。
bool SshTunnelManager::Start(const Options& options) {
  auto_port_retries_ = 0;
  return StartInternal(options);
}

bool SshTunnelManager::StartInternal(const Options& options) {
  if (IsBusy()) {
    // "正在建立时又点一次"：不是失败，是请求没被受理。状态与原因都已经在
    // 对象里，界面照旧显示"正在建立安全通道…"。
    diagnostic_ = QStringLiteral("已经在建立或正在关闭安全通道，忽略重复请求");
    return false;
  }
  if (state_ == State::kReady && external_reuse_) {
    // 之前借的是外部监听者：先解除借用，再按新参数重新决定。
    external_reuse_ = false;
    local_port_ = 0;
    state_ = State::kStopped;
    emit stateChanged();
  }
  if (state_ == State::kReady && owned_ && process_ != nullptr &&
      process_->state() == QProcess::Running) {
    // 已经有一条自己启动的、确认过可用的通道：不重启（重启会打断正在用的
    // 连接）。参数变化由调用方先 Stop() 再 Start() 表达。
    return true;
  }

  // options_ 到这一步才赋值：上面两个早退分支（重复请求、已有可用通道）不应该
  // 用被拒绝的参数覆盖当前生效的 Options。
  options_ = options;
  stop_requested_ = false;
  stderr_buffer_.clear();
  diagnostic_.clear();
  failure_detail_.clear();
  readiness_attempts_ = 0;

  QString target_error;
  if (!IsValidSshTarget(options_.ssh_target, &target_error)) {
    failure_ = Failure::kInvalidTarget;
    diagnostic_ = target_error;
    state_ = State::kFailed;
    emit stateChanged();
    return true;  // 受理了：结果是"参数不合法"，由状态机如实报出
  }
  if (options_.remote_host.trimmed().isEmpty() || options_.remote_port < 1 ||
      options_.remote_port > 65535) {
    failure_ = Failure::kInvalidTarget;
    diagnostic_ = QStringLiteral("远端服务地址或端口不合法");
    state_ = State::kFailed;
    emit stateChanged();
    return true;
  }

  // ---- 本地端口 ----
  //
  // 顺序是有意的：**先看端口，再找 ssh**。用户已经在外面开好一条隧道时，
  // 本程序只是借那个端口用，根本不需要本机有 ssh；把"找不到 ssh"排在前面
  // 会让这种完全正常的用法莫名其妙地失败。
  if (options_.local_port > 0) {
    QString probe_error;
    if (IsLoopbackPortOpen(options_.local_port, 300, &probe_error)) {
      // 已经有人在监听这个端口：那是一条**外部**通道（用户自己开的），
      // 或者一个无关程序。两种情况都不允许本程序杀它。
      // 复用是安全的：BPSEC1 的 pin 最终仍然会验证"隧道那头到底是谁"。
      return AdoptExternalListener(options_, options_.local_port);
    }
    local_port_ = options_.local_port;
  } else {
    int picked = 0;
    QString pick_error;
    if (!PickFreeLoopbackPort(&picked, &pick_error)) {
      failure_ = Failure::kForwardFailed;
      diagnostic_ = pick_error;
      state_ = State::kFailed;
      emit stateChanged();
      return true;
    }
    local_port_ = picked;
  }
  external_reuse_ = false;

  // ---- ssh 可执行文件（只有真的要起进程时才需要）----
  // 显式指定的路径在这里就校验 exists/isFile/isExecutable：目录或不可执行的
  // 文件要当场变成 kSshMissing，而不是等 QProcess 报一句含糊的启动失败。
  QString program = options_.ssh_program.trimmed();
  if (!program.isEmpty()) {
    const QFileInfo info(program);
    if (!info.exists() || !info.isFile() || !info.isExecutable()) {
      failure_ = Failure::kSshMissing;
      diagnostic_ = QStringLiteral("指定的 ssh 可执行文件不存在或不可执行：%1")
                        .arg(program);
      state_ = State::kFailed;
      emit stateChanged();
      return true;
    }
  } else {
    program = QStandardPaths::findExecutable(QStringLiteral("ssh"));
    if (program.isEmpty()) {
      failure_ = Failure::kSshMissing;
      diagnostic_ = QStringLiteral("PATH 里找不到 ssh");
      state_ = State::kFailed;
      emit stateChanged();
      return true;
    }
  }

  // ---- 启动进程 ----
  process_ = std::make_unique<QProcess>();
  process_->setProcessChannelMode(QProcess::SeparateChannels);
  // 环境原样继承：ssh 读的是用户自己的 ~/.ssh/config 与 known_hosts。
  // 这里刻意**没有**任何"换个 HOME / 换个 known_hosts"的口子 —— 产品路径
  // 上不存在可以指向别处身份库的开关，也就没有"顺手绕过 host verification"
  // 的入口。
  process_->setProcessEnvironment(QProcessEnvironment::systemEnvironment());
  process_->setProgram(program);
  // 参数向量里**永远不放凭据**：命令行会出现在 ps 与 /proc/<pid>/cmdline 里，
  // 把口令写进去等于公开它，认证完全交给密钥与 ssh-agent。
  // 这些 -o 写在命令行而不是依赖 ~/.ssh/config，让行为不随用户配置漂移：
  // 用户自己的 config 仍然生效，但上面这几条不会被它放宽。
  process_->setArguments(QStringList{
      QStringLiteral("-N"),
      // 关掉一切交互式提问：需要口令时立刻失败，而不是把 GUI 卡在提示上。
      QStringLiteral("-o"), QStringLiteral("BatchMode=yes"),
      // 转发建不起来就让 ssh 退出，而不是"连上了但什么都没转发"。
      QStringLiteral("-o"), QStringLiteral("ExitOnForwardFailure=yes"),
      QStringLiteral("-o"),
      QStringLiteral("ConnectTimeout=%1").arg(options_.connect_timeout_seconds),
      // 隧道断了要及时发现：靠 SSH 自己的 keepalive，而不是靠猜。
      QStringLiteral("-o"), QStringLiteral("ServerAliveInterval=15"),
      QStringLiteral("-o"), QStringLiteral("ServerAliveCountMax=3"),
      // 只绑回环：这个本地端口不对局域网开放。
      // 绑定地址写字面量 127.0.0.1 而不是 localhost：后者在 IPv6 优先的机器
      // 上可能解析到 ::1，而探针只连 IPv4 回环，会一直探不通直到 kTimeout。
      QStringLiteral("-L"),
      QStringLiteral("127.0.0.1:%1:%2:%3")
          .arg(local_port_)
          .arg(options_.remote_host)
          .arg(options_.remote_port),
      // 第二道防线：即使目标以 '-' 开头（上面已经拒绝），"--" 之后也不会
      // 再被当成选项。
      QStringLiteral("--"), options_.ssh_target});

  connect(process_.get(), &QProcess::finished, this,
          &SshTunnelManager::OnProcessFinished);
  connect(process_.get(), &QProcess::errorOccurred, this,
          &SshTunnelManager::OnProcessErrorOccurred);

  process_->start();
  // 起不来时手工拆除而不调 CloseProcess()：owned_ 此刻还是 false，那个函数的
  // terminate 分支本来就不适用。顺序仍然照抄它：先取 errorString() 当诊断，
  // 再 disconnect，最后 release + deleteLater，绝不在信号栈上同步析构。
  if (!process_->waitForStarted(5000)) {
    const QString error_text = process_->errorString();
    process_->disconnect(this);
    QProcess* dying = process_.release();
    dying->deleteLater();
    owned_ = false;
    failure_ = Failure::kSpawnFailed;
    diagnostic_ = QStringLiteral("ssh 启动失败：%1").arg(error_text);
    state_ = State::kFailed;
    emit stateChanged();
    return true;
  }
  owned_ = true;

  probe_ = std::make_unique<QTcpSocket>();
  connect(probe_.get(), &QTcpSocket::connected, this,
          &SshTunnelManager::OnProbeConnected);
  connect(probe_.get(), &QTcpSocket::errorOccurred, this,
          [](QAbstractSocket::SocketError) {
            // 建立阶段：连不上就是"还没好"，下一个 tick 再试；超时由
            // OnPollTick 的截止时间判定。Ready 之后 probe_ 已经被销毁，
            // 所以这里根本不会再有回调 —— 也就没有"后台探测"这回事。
          });
  // stderr 由管道可读事件驱动地抽干：Ready 之后没有定时器了，不接这个信号
  // 的话，一条话多的 ssh 会把管道缓冲区写满然后卡住。
  connect(process_.get(), &QProcess::readyReadStandardError, this,
          &SshTunnelManager::ReadStderrIntoDiagnostic);

  failure_ = Failure::kNone;
  state_ = State::kStarting;
  emit stateChanged();
  deadline_.start();
  poll_.setInterval(kStartingPollMs);
  poll_.start();
  // 立刻探一次，别白等一个 tick。
  OnPollTick();
  return true;
}

// 只借端口，不接管进程：owned_ 保持 false，Stop() 与析构都不会去动用户自己在
// 外面开的那条 ssh。进入 kReady 的前置条件由调用方保证：StartInternal() 用
// IsLoopbackPortOpen() 确认过端口上真的有 listener。不做身份校验是有意的，
// 隧道那头是谁由隧道内的 BPSEC1 pin 判定，另加一层检查只会制造安全错觉。
bool SshTunnelManager::AdoptExternalListener(const Options& options,
                                             int local_port) {
  options_ = options;
  local_port_ = local_port;
  owned_ = false;
  external_reuse_ = true;
  failure_ = Failure::kNone;
  failure_detail_.clear();
  diagnostic_ = QStringLiteral(
                    "复用已经存在的本地监听者 127.0.0.1:%1（不是本程序启动的，"
                    "退出时不会结束它）")
                    .arg(local_port);
  state_ = State::kReady;
  emit stateChanged();
  return true;
}

// 有两个调用者：readyReadStandardError（事件驱动）和 OnPollTick（兜底），因此
// 必须可重复调用 —— readAllStandardError() 已经抽干管道缓冲区，重复调用只会
// 拿到空数据直接返回，不会把同一段文本追加两次。
void SshTunnelManager::ReadStderrIntoDiagnostic() {
  if (process_ == nullptr) {
    return;
  }
  const QByteArray chunk = process_->readAllStandardError();
  if (chunk.isEmpty()) {
    return;
  }
  stderr_buffer_.append(chunk);
  if (stderr_buffer_.size() > kStderrKeepBytes) {
    stderr_buffer_ = stderr_buffer_.right(kStderrKeepBytes);
  }
  diagnostic_ = SanitizeSshStderr(stderr_buffer_);
  emit stateChanged();
}

void SshTunnelManager::OnProbeConnected() {
  if (state_ != State::kStarting) {
    // Ready 之后不应该再有 probe 回调（probe_ 已经被销毁）；真出现了也只
    // 说明有一条迟到的通知，什么都不做。
    return;
  }
  // 真的连上了才算 Ready：进程活着不算证据。
  readiness_attempts_ = 0;
  failure_ = Failure::kNone;
  state_ = State::kReady;
  // 停表 + 把 probe 整个销毁：从这一刻起本类**不再发起任何 TCP 连接**。
  // 这里曾经只是把间隔改成 400ms，于是空闲 GUI
  // 每 400ms 就往本地转发端口 connect 一次，而 ssh -L 每接受一次连接就可能
  // 真的向 ECS 建一条 forwarded channel，等于持续给 backup-server 送连接。
  //
  // DisarmProbe() 会在本函数的调用栈上（connected 信号里）把 probe_ 交给
  // deleteLater，所以这一行必须是本函数最后一次碰 probe_。
  poll_.stop();
  emit stateChanged();
  DisarmProbe();
}

// 每个 tick 的顺序是固定的：读 stderr -> 判断进程是否已经退出 -> 判断截止时间
// 与次数 -> 发起一次探测。先读 stderr 是为了让超时与退出路径拿到尽量完整的
// 诊断；先判死再判超时，是为了让"ssh 自己退了"优先于"我们等超时了"报给用户。
void SshTunnelManager::OnPollTick() {
  // 这个定时器**只服务于建立阶段**。Ready 之后它已经被 OnProbeConnected
  // 停掉；真跑到了这里也只可能是迟到的 tick，直接收摊。
  if (state_ != State::kStarting) {
    poll_.stop();
    return;
  }
  ReadStderrIntoDiagnostic();
  if (process_ != nullptr && process_->state() == QProcess::NotRunning) {
    // finished 信号会把状态推到 kFailed；这里是兜底，防止某些平台上信号
    // 顺序与定时器错开时状态卡在 kStarting。
    OnProcessFinished(process_->exitCode(), process_->exitStatus());
    return;
  }
  // 单调时钟 + 次数上限：两个都是**有界**的，没有无限轮询。
  if (deadline_.elapsed() >= options_.ready_timeout_ms ||
      readiness_attempts_ >= kMaxReadinessAttempts) {
    const QString detail = SanitizeSshStderr(stderr_buffer_);
    Fail(Failure::kTimeout,
         detail.isEmpty() ? QStringLiteral("在 %1 毫秒内本地端口没有变成可连接")
                                .arg(options_.ready_timeout_ms)
                          : detail);
    return;
  }
  ++readiness_attempts_;
  if (probe_ == nullptr) {
    return;
  }
  // 建立阶段才发起探测（连上就由 OnProbeConnected 收摊）。
  probe_->abort();
  probe_->connectToHost(QHostAddress::LocalHost,
                        static_cast<quint16>(local_port_));
}

void SshTunnelManager::OnProcessErrorOccurred(QProcess::ProcessError error) {
  if (state_ == State::kStopping || stop_requested_) {
    return;
  }
  if (error == QProcess::FailedToStart) {
    const QString text =
        process_ == nullptr ? QString() : process_->errorString();
    Fail(Failure::kSpawnFailed, QStringLiteral("ssh 启动失败：%1").arg(text));
  }
  // 其余错误（Crashed / Timedout / WriteError / ReadError）统一由 finished
  // 处理，避免同一件事报两次。
}

// 进程退出是"通道没了"的唯一证据（Ready 之后没有周期探测）。先把管道里剩下
// 的 stderr 读干净：进程退出与 readyRead 的派发顺序在平台之间并不一致，漏读
// 会让诊断文本缺掉最关键的最后几行。
void SshTunnelManager::OnProcessFinished(int exit_code,
                                         QProcess::ExitStatus exit_status) {
  if (process_ == nullptr) {
    return;  // 进程已经被收掉，这一条是迟到的通知
  }
  ReadStderrIntoDiagnostic();
  if (state_ == State::kStopping || stop_requested_) {
    return;
  }
  const QString sanitized = SanitizeSshStderr(stderr_buffer_);
  const Failure classified = ClassifySshStderr(sanitized);
  QString detail = sanitized;
  if (detail.isEmpty()) {
    detail =
        QStringLiteral("ssh 退出（exit=%1，status=%2）")
            .arg(exit_code)
            .arg(exit_status == QProcess::NormalExit ? QStringLiteral("normal")
                                                     : QStringLiteral("crash"));
  }
  const bool was_ready = state_ == State::kReady;
  // 自动挑的端口撞车（本地端口竞态）时重挑一次：这是**有界**重试，
  // 而不是把失败直接甩给用户。
  // 重挑端口前先把现场拆干净（停表 -> 收 probe -> 收进程 -> kStopped），复用
  // 与 Stop() 相同的拆除序列，保证任何时刻最多只有一个 ssh 子进程；递归深度
  // 由 auto_port_retries_ 封顶，不会无限重试。
  if (!was_ready && classified == Failure::kForwardFailed &&
      options_.local_port == 0 && auto_port_retries_ < kMaxAutoPortRetries) {
    ++auto_port_retries_;
    const Options retry_options = options_;
    poll_.stop();
    DisarmProbe();
    CloseProcess();
    state_ = State::kStopped;
    emit stateChanged();
    StartInternal(retry_options);
    return;
  }
  if (was_ready) {
    Fail(Failure::kExited, detail);
    return;
  }
  // 没进过 Ready 又没输出：最可能是连不上（BatchMode 会压掉交互提示）。归成
  // kHostUnreachable 比"通道已断开"更能指向一个可执行的排查动作。
  Fail(classified == Failure::kExited && sanitized.isEmpty()
           ? Failure::kHostUnreachable
           : classified,
       detail);
}

}  // namespace backup_modern
