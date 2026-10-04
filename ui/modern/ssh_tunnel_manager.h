// ui/modern/ssh_tunnel_manager.h
//
// 客户端侧的 SSH 安全通道管理器（PR #22）。
//
// ---- 它为什么存在 ----
//
// 当前部署里 backup-server **只监听 ECS 的 127.0.0.1:18765**（见
// server/main.cpp 的 --listen 默认值与部署脚本）。公网没有开 18765，安全组也
// 没有改。于是"GUI 连 127.0.0.1:18765"这句话只有在**本机存在一条到 ECS 的
// SSH 端口转发**时才成立。PR #21 之前这件事靠用户自己在另一个终端里手敲
// ssh -N -L；忘了开就是一句 connection refused，而产品没有任何地方说得清
// "少了一个前置动作"。
//
// 本类把那条前置动作变成产品的一部分：由 GUI 自己启动、自己确认、自己回收。
//
// ---- 它是什么 / 不是什么 ----
//
// 是：**transport / deployment utility**。它只负责把本地一个回环端口接到远端
// 的 127.0.0.1:<remote_port>。
//
// 不是：密码学实现的一部分。BPSEC1 的握手、X25519、HKDF、AES-GCM、HMAC 全部
// 照旧在**隧道里面**跑，pin 校验一个字节都没有少。OpenSSH 在身份上提供的是
// **第二层**独立证据（SSH host key），与 BPSEC1 的 server pin 构成
// defense-in-depth，而不是替代关系。
//
// ---- 安全边界（硬要求）----
//
//   * 只用 QProcess + **参数向量**启动 ssh。绝不用 system() / sh -c /
//     bash -c，也不把用户输入拼进任何命令行字符串 —— 没有 shell，就没有
//     shell injection 面。
//   * 不弱化 SSH host verification：不设 StrictHostKeyChecking=no，不设
//     UserKnownHostsFile=/dev/null，不自动接受未知 host key。用户正常的
//     ~/.ssh/config、known_hosts、ssh-agent、密钥文件就是全部依据。
//   * 不收集 SSH 口令。BatchMode=yes 明确关掉一切交互式提问：需要密码时
//     ssh **立刻失败**并给出 Permission denied，本类把它归类成
//     kAuthFailed 并告诉用户"请先配置密钥或 ssh-agent"，而不是把终端提示藏到
//     后台把 GUI 卡死。
//   * ssh_target 会被校验：不接受空、不接受以 '-' 开头的值（否则会被 ssh
//     当选项解析），不接受空白与控制字符。参数向量里仍然再加一个 "--"。
//
// ---- Ready 的判定 ----
//
// "进程还活着"不等于"通道可用"。本类必须真的连上过
// 127.0.0.1:<local_port> 才进入 kReady，而且用的是**有界**重试：固定间隔的
// 定时器 + 单调时钟截止时间（QElapsedTimer），既没有无限 sleep 也没有无限
// 轮询。超时就 kTimeout，并把进程收干净。
//
// Ready 之后仍然以**低频异步探测**监视那条本地 listener（不是常驻连接，
// 连上就立刻放手），连续 kLivenessFailureTolerance 次拒绝才判定通道已死 ——
// 单次抖动不该让界面喊"通道断了"。
//
// ---- 进程所有权 ----
//
// owned_ 只在"本对象真的启动了这个进程"时为真。只有 owned_ 的进程会在
// Stop() / 析构时被 terminate → 有界等待 → 必要时 kill。用户自己已经在外面
// 开好的隧道永远不属于本对象，因此**永远不会被本程序杀掉**（复用走
// AdoptExternalListener，只借端口，不碰进程）。

#ifndef BACKUP_PROJECT_UI_MODERN_SSH_TUNNEL_MANAGER_H_
#define BACKUP_PROJECT_UI_MODERN_SSH_TUNNEL_MANAGER_H_

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QTcpSocket>
#include <QTimer>
#include <memory>

namespace backup_modern {

class SshTunnelManager : public QObject {
  Q_OBJECT

 public:
  // 通道状态机。刻意不是 bool connected：界面要区分"没启动 / 正在建 /
  // 建好了 / 建失败 / 正在关"，而"建立失败"本身还有十来种不同的原因。
  enum class State {
    kStopped = 0,
    kStarting = 1,
    kReady = 2,
    kFailed = 3,
    kStopping = 4,
  };

  // 失败的结构化分类。每一种在界面上都有一句**不同**的、可以照着做的中文，
  // 不允许被压成一句"网络错误"。
  enum class Failure {
    kNone = 0,
    kInvalidTarget,    // 目标别名本身不合法
    kSshMissing,       // 找不到 ssh 可执行文件
    kSpawnFailed,      // 进程起不来（权限 / 资源）
    kHostUnreachable,  // SSH 主机解析不了或连不上
    kHostKeyFailed,    // SSH 服务器身份校验失败（fail closed）
    kAuthFailed,       // SSH 认证失败 / 需要交互式认证
    kForwardFailed,    // 本地端口转发建立失败
    kTimeout,          // 在截止时间内没有变成 connectable
    kExited,           // 已经 Ready 之后进程自己退了
  };

  struct Options {
    // 空 = 在 PATH 里找 ssh（QStandardPaths::findExecutable）。
    QString ssh_program;
    // ~/.ssh/config 里的别名（例如 aliyun-ecs），或 user@host。
    QString ssh_target;
    QString remote_host = QStringLiteral("127.0.0.1");
    int remote_port = 18765;
    // 0 = 自动挑一个空闲回环端口（默认，避免与用户自己的 18765 撞车）。
    int local_port = 0;
    int connect_timeout_seconds = 10;
    int ready_timeout_ms = 20000;
  };

  explicit SshTunnelManager(QObject* parent = nullptr);
  ~SshTunnelManager() override;

  // ---- 只读访问器 ----
  State state() const { return state_; }
  Failure failure() const { return failure_; }
  bool IsReady() const { return state_ == State::kReady; }
  bool IsBusy() const {
    return state_ == State::kStarting || state_ == State::kStopping;
  }
  int localPort() const { return local_port_; }
  QString localEndpointText() const;
  QString remoteEndpointText() const;
  // 本程序启动的那个 ssh 的 PID；不是本程序启动的（外部复用）时为 0。
  qint64 processId() const;
  bool OwnsProcess() const { return owned_; }
  bool IsExternalReuse() const { return external_reuse_; }
  // 普通用户能看懂的状态句。
  QString stateText() const;
  QString failureText() const;
  // 失败类别的稳定英文名（进日志与测试断言，不进主界面）。
  QString failureKindName() const;
  // 已经脱敏的 ssh stderr（默认折叠的"技术详情"里显示）。
  QString diagnosticText() const { return diagnostic_; }
  const Options& options() const { return options_; }

  // ---- 控制 ----
  //
  // 建立通道。返回 false 表示请求**没有被受理**（正在建立 / 正在关闭），
  // 此时 state() 与 failure() 已经说明了原因。返回 true 只代表"受理了"：
  // 结果要看状态机，不要在调用点写"Start() 为真就当通道可用"。
  bool Start(const Options& options);
  // 关闭**本对象拥有的**通道：terminate → 有界等待 → 必要时 kill。
  // 外部复用的监听者一个字节都不动。可重复调用（幂等）。
  void Stop();
  // 复用用户已经开好的本地监听者：只借端口，不接管、不杀进程。
  bool AdoptExternalListener(const Options& options, int local_port);
  // 换一个 ssh 可执行文件（产品上是"高级"设置，测试用它模拟 ssh 不存在）。
  void SetSshProgram(const QString& program);

  // ---- 静态工具（独立可测，不依赖对象状态）----
  //
  // 自动挑一个空闲的回环端口：向 127.0.0.1:0 绑定一次，读回内核给的端口，
  // 立刻关掉。返回 true 时 *out_port 一定在 1024..65535。
  static bool PickFreeLoopbackPort(int* out_port, QString* error);
  // 127.0.0.1:<port> 现在能不能连上（有界等待，给 C12/C14 的活性探测用）。
  static bool IsLoopbackPortOpen(int port, int timeout_ms, QString* error);
  // 把 ssh 的 stderr 变成可以放进界面与日志的文本：去掉 ANSI 转义与控制
  // 字符、去掉可能出现的口令字面量、限制长度。
  static QString SanitizeSshStderr(const QByteArray& raw);
  // 把 ssh 的 stderr 归类成 Failure。
  static Failure ClassifySshStderr(const QString& sanitized);
  static QString FailureKindName(Failure failure);
  static QString FailureUserText(Failure failure);
  // 短标题（状态横幅 / 表单错误行的一行标题）。
  static QString FailureTitle(Failure failure);
  // kind 名字的反查：kNone 表示"这不是一个 SSH/隧道层的 kind"。
  // 让"文案只有一份"成为结构性事实，而不是靠两处字符串恰好写得一样。
  static Failure FailureFromKindName(const QString& name);
  // ssh_target 的合法性（不接受 '-' 开头、空白、控制字符）。
  static bool IsValidSshTarget(const QString& target, QString* error);

 signals:
  // 状态 / 失败原因 / 本地端口 / 诊断文本任一变化。页面只订阅这一个信号。
  void stateChanged();

 private:
  bool StartInternal(const Options& options);
  void SetState(State state);
  void Fail(Failure failure, const QString& detail);
  void OnPollTick();
  void OnProcessFinished(int exit_code, QProcess::ExitStatus exit_status);
  void OnProcessErrorOccurred(QProcess::ProcessError error);
  void OnProbeConnected();
  void ReadStderrIntoDiagnostic();
  void CloseProcess();
  void DisarmProbe();

  Options options_;
  State state_ = State::kStopped;
  Failure failure_ = Failure::kNone;
  QString failure_detail_;
  QString diagnostic_;
  QByteArray stderr_buffer_;
  int local_port_ = 0;
  bool owned_ = false;
  bool external_reuse_ = false;
  bool stop_requested_ = false;
  // 建立阶段的探测次数上限（第二道闸门，第一道是单调截止时间）。
  int readiness_attempts_ = 0;
  // Ready 之后连续探测失败的次数；成功一次就清零。
  int liveness_failures_ = 0;
  // 自动端口撞车时的重挑次数（有界）。
  int auto_port_retries_ = 0;
  // 只允许一个 ssh 子进程存在：unique_ptr 让"换一个进程"必然是先放手旧的。
  std::unique_ptr<QProcess> process_;
  std::unique_ptr<QTcpSocket> probe_;
  // 有界重试的载体：固定间隔 + 单调截止时间。
  QTimer poll_;
  QElapsedTimer deadline_;
};

}  // namespace backup_modern

#endif  // BACKUP_PROJECT_UI_MODERN_SSH_TUNNEL_MANAGER_H_
