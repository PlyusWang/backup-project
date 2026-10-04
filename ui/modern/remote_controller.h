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
// ---- 服务端身份（serverKeyPin）----
//
// 与 password 正相反：pin 是服务端的**公钥 / 公钥指纹**，不是秘密——它可以
// 显示在界面上、可以抄进部署文档、也可以在内存里长期保留。它是"我要连的
// 到底是哪一台服务器"的期望值：为空或格式不对时 RemoteArchiveClient::Connect()
// **直接失败**（kNoPinConfigured，信息里是"没有配置服务端传输公钥/指纹"），
// 客户端不做"第一次见到谁就信谁"。
//
// 它**不落盘**：本页的 host / port 本来就没有持久化落点（见下面端点的说明），
// pin 与它们是同一类东西，不为了它单独新造一套配置系统。所以它只存在于内存，
// 由调用方在**第一次连接之前**设置：界面上的"服务器身份指纹"输入框，
// 或者 main.cpp 里 --remote-test / --remote-smoke 两条自检路径。
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
#include <memory>
#include <string>
#include <vector>

#include "remote_backup_client.h"
#include "remote_incremental.h"
#include "ssh_tunnel_manager.h"

namespace backup_modern {

// 服务器可达性。
//
// 只在一个**真的发生过的连接尝试**之后才有一个确定的答案：
//   kUnknown      还没有试过（界面什么都不说）
//   kReachable    刚刚连上过
//   kUnreachable  刚刚的连接尝试失败了
//
// 为什么不直接用 client_.connected()：BPNET1 是"每次操作建立连接"，那个
// 布尔值既短命又容易被读成"服务器健康状态"。旧界面把"还没有试过连接"
// 写成"未连接"，人工验收时被理解成"服务器挂了 / 隧道断了"。
enum class RemoteReachability {
  kUnknown = 0,
  kReachable = 1,
  kUnreachable = 2,
};

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
    // 注销账户：服务端删除，不是"退出登录"。
    kDeleteAccount,
    // PR #21 产品级远端增量：把**目录**备份到远端（完整或增量）——
    // 对应 backupctl remote backup。与 kUpload（上传一个本地 .bak）是
    // 两件事：那一条是低层 raw 归档操作，这一条走材料包 + 链。
    kBackup,
    // 产品级链恢复：把某个远端快照（连同它的依赖链）恢复到本地目录——
    // 对应 backupctl remote restore。与 kDownload（下载一个 blob）也是
    // 两件事。
    kRestore,
    // 原始归档的"单独恢复"（PR #21 UI closure）：下载那一个 blob，按**内容**
    // 认出它是不是本机能独立恢复的归档，然后交给既有的本地恢复核心。
    // 它**不是**链恢复：原始归档没有 lineage / parent / 副文件。
    kRestoreRaw,
  };

  Kind kind = Kind::kList;
  bool ok = false;
  // 这次操作对"服务器是否可达"给出的证据（没有尝试连接时是 kUnknown）。
  RemoteReachability reachability = RemoteReachability::kUnknown;
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

  // ---- kBackup：产品级远端备份的结果（全部来自共享 core 的 outcome）----
  //
  // 关键的一条是"用户选的策略"与"这次实际产出的类型"**不是**同一件事：
  // 允许增量但本地没有可信基线（或链太深、缓存不可信）时，core 会重建完整
  // 基线。界面必须按 produced_delta / rebuilt_full_baseline 如实说，不能因为
  // 用户点了"增量"就报"增量成功"。
  bool backup_produced_delta = false;
  bool backup_rebuilt_full = false;
  bool backup_no_changes = false;
  std::string backup_snapshot_id;
  std::string backup_parent_snapshot_id;
  std::uint64_t backup_generation = 0;
  std::uint64_t backup_uploaded_bytes = 0;
  std::uint64_t backup_chain_root_bytes = 0;
  std::string backup_baseline_reason;
  std::string backup_archive_name;
  // 用户这次点的是不是“增量”。结论文案必须区分“用户就是要一份完整备份”和
  // “用户点了增量、但云端没有可续的链，于是实际给了完整基线”——两者的
  // 实际产物相同，但对用户说的话**不能**相同。
  bool backup_incremental_requested = false;

  // ---- kRestore：产品级链恢复的结果 ----
  std::uint64_t restore_chain_length = 0;
  std::uint64_t restore_delta_count = 0;
  std::uint64_t restore_downloaded_bytes = 0;
  std::uint64_t restore_reused_bytes = 0;
  std::uint64_t restore_restored_entries = 0;

  // ---- kRestoreRaw：原始归档单独恢复的结果 ----
  std::uint64_t raw_downloaded_bytes = 0;
  std::string raw_verified_sha256;
  std::string raw_archive_format;
  std::uint64_t raw_restored_entries = 0;
  bool raw_password_required = false;
  // 这次交互一共真正下载过几次（正常情况下是 1 次：错密码重试用的是同一份
  // 已经校验过的字节）。自检据此证明没有偷偷重复下载。
  int raw_download_count = 0;
  // 那份归档在临时工作目录里的绝对路径（只给自检用：比较 inode 即可证明
  // "再输一次密码"没有重新下载）。
  std::string raw_archive_path;
  // 需要"再输一次密码"时，这次交互的会话被交回主线程：页面据此从"选目标目录"
  // 切到"输入恢复密码"，并且复用**同一份**字节。其它情况为空——成功即释放，
  // 致命失败即放弃（工作目录随之删掉）。
  std::shared_ptr<backupproject::net::RemoteRawRestoreSession> raw_session;
};

// 后台线程需要的全部输入。刻意做成一个值类型：后台线程只读它，
// 调用方在提交之后可以立刻改自己的状态，两者之间没有任何共享。
struct RemoteRequest {
  RemoteOpResult::Kind kind = RemoteOpResult::Kind::kList;
  backupproject::net::RemoteEndpoint endpoint;
  std::string username;
  std::string password;
  std::string local_path;
  // kUpload：上传时用的显示名。
  // kRestoreRaw：只用来给下载下来的临时文件起名（不可信输入，core 会降级成
  // 单组件文件名）；它**不参与**任何格式判断。
  std::string display_name;
  std::string snapshot_id;
  std::string target_path;
  bool allow_overwrite = false;
  // kBackup：源目录；allow_incremental 就是界面上的策略（false = Full，
  // true = Incremental）。除此之外的一切（能不能续链、父是谁、代数、
  // lineage、要不要 bootstrap 缓存）都由 core 决定，GUI 不参与。
  std::string source_directory;
  bool allow_incremental = false;
  // kBackup：显示名留空时由 core 按链规则生成。
  // kRestore：目标目录。kRestoreRaw：下载之后本地恢复的目标目录。
  std::string restore_destination;
  // kRestoreRaw：如果那份归档是加密的，这里是用户在界面上填的恢复密码。
  // kRestore（链恢复）不需要它：增量链的外层信封由内层身份记录保护。
  // 只在本对象的生命周期内存在：提交之后立刻被就地抹掉，不落盘、不进日志。
  std::string restore_password;
  // kRestoreRaw：非空表示这是"再输一次密码"的重试——沿用会话里已经下载并
  // 校验过的那份字节与第一次选定的目标目录，snapshot_id / display_name 都不再
  // 参与（它们已经固化在会话里）。
  std::shared_ptr<backupproject::net::RemoteRawRestoreSession> raw_session;
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
  // 服务端传输身份 pin（"sha256:<64 位十六进制>" 或 "hex:<64 位十六进制>"）。
  // 与地址 / 端口同属"连接配置"，但**没有默认值**：不填就不能连接。
  Q_PROPERTY(QString serverKeyPin READ serverKeyPin NOTIFY serverKeyPinChanged)
  // 这个输入框自己的错误行。没填、格式不对都写在这里：不弹对话框，也不占用
  // 页面底部的横幅——与 loginError / registerError 是同一条规矩。
  Q_PROPERTY(QString serverKeyPinError READ serverKeyPinError NOTIFY
                 serverKeyPinErrorChanged)

  // ---- 连接方式与 SSH 安全通道（PR #22）----
  //
  // 当前部署里 backup-server 只监听 ECS 的 127.0.0.1:18765，公网没有开这个
  // 端口、安全组也没有改。于是"连 127.0.0.1:18765"只有在**本机存在一条到
  // ECS 的 SSH 端口转发**时才成立。PR #21 把这件事留给用户自己在另一个终端
  // 里记着，人工验收的结果就是一句 connection refused，而产品一句话都没说。
  //
  // 所以连接方式成了产品的一部分：
  //   ssh    —— GUI 自己起 ssh -N -L：本地随机回环端口 -> SSH 主机 -> 远端服务
  //   direct —— 直连（高级）：逻辑与 PR #21 完全一样
  //
  // SSH 通道只是**传输层**。BPSEC1 握手与 server pin 校验一个字节都没有少，
  // 仍然在隧道**里面**跑：OpenSSH 的 host key 与 BPSEC1 的 pin 是两层独立
  // 证据，不是替代关系。
  Q_PROPERTY(QString connectionMode READ connectionMode WRITE setConnectionMode
                 NOTIFY connectionChanged)
  // ~/.ssh/config 里的别名（当前部署是 aliyun-ecs）或 user@host。
  Q_PROPERTY(
      QString sshHost READ sshHost WRITE setSshHost NOTIFY connectionChanged)
  // 本地端口：空 / "0" = 自动挑一个空闲回环端口（默认，避免与用户自己的
  // 18765 撞车）。
  Q_PROPERTY(QString sshLocalPort READ sshLocalPort WRITE setSshLocalPort NOTIFY
                 connectionChanged)
  // ssh 可执行文件：空 = 在 PATH 里找。
  Q_PROPERTY(QString sshProgram READ sshProgram WRITE setSshProgram NOTIFY
                 connectionChanged)
  Q_PROPERTY(QString tunnelState READ tunnelState NOTIFY tunnelChanged)
  Q_PROPERTY(QString tunnelStateText READ tunnelStateText NOTIFY tunnelChanged)
  Q_PROPERTY(bool tunnelReady READ tunnelReady NOTIFY tunnelChanged)
  Q_PROPERTY(bool tunnelBusy READ tunnelBusy NOTIFY tunnelChanged)
  Q_PROPERTY(
      QString tunnelFailureKind READ tunnelFailureKind NOTIFY tunnelChanged)
  Q_PROPERTY(
      QString tunnelFailureText READ tunnelFailureText NOTIFY tunnelChanged)
  Q_PROPERTY(QString tunnelLocalEndpointText READ tunnelLocalEndpointText NOTIFY
                 tunnelChanged)
  Q_PROPERTY(QString tunnelRemoteEndpointText READ tunnelRemoteEndpointText
                 NOTIFY tunnelChanged)
  Q_PROPERTY(bool tunnelOwnedByApp READ tunnelOwnedByApp NOTIFY tunnelChanged)
  Q_PROPERTY(
      bool tunnelExternalReuse READ tunnelExternalReuse NOTIFY tunnelChanged)
  Q_PROPERTY(QString tunnelDiagnosticText READ tunnelDiagnosticText NOTIFY
                 tunnelChanged)

  // ---- 服务器身份指纹：草稿 vs 已应用（PR #22）----
  //
  // serverKeyPin 是**已经生效**的那一个（appliedServerKeyPin 是它的别名，
  // 页面用它做 dirty 判断）。用户点"应用"以后必须**看得见**结果：
  // pinApplyState 是机器可读的 applied / unchanged / invalid，pinApplyMessage
  // 是那一行绿色反馈。人工验收发现的"点了应用什么都不发生"就是这里缺的。
  Q_PROPERTY(
      QString appliedServerKeyPin READ serverKeyPin NOTIFY serverKeyPinChanged)
  Q_PROPERTY(QString pinApplyState READ pinApplyState NOTIFY pinApplyChanged)
  Q_PROPERTY(
      QString pinApplyMessage READ pinApplyMessage NOTIFY pinApplyChanged)
  Q_PROPERTY(bool pinApplyOk READ pinApplyOk NOTIFY pinApplyChanged)

  // ---- 会话 ----
  Q_PROPERTY(bool connected READ connected NOTIFY sessionChanged)
  Q_PROPERTY(bool authenticated READ authenticated NOTIFY sessionChanged)
  Q_PROPERTY(QString sessionText READ sessionText NOTIFY sessionChanged)
  // 服务器可达性：只有在**真的试过一次连接**之后才有内容，空串表示"还没试
  // 过"。它是"上一次连接尝试的结果"，不是一个持续探测的连接状态。
  Q_PROPERTY(QString serverReachabilityText READ serverReachabilityText NOTIFY
                 reachabilityChanged)
  Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
  Q_PROPERTY(QString busyAction READ busyAction NOTIFY busyChanged)

  // ---- 云端备份列表 ----
  // 每一项是 {id, name, sizeBytes, sizeText, createdText, sha256Short} 加上
  // PR #21 的链元数据 {kind, kindText, generation, parentId, parentShort,
  // lineageShort, restorable, restoreHint}：页面只做展示，不再自己算大小、
  // 时间，也不自己推断"这一条能不能恢复"。
  Q_PROPERTY(QVariantList snapshots READ snapshots NOTIFY snapshotsChanged)
  Q_PROPERTY(QString listSummary READ listSummary NOTIFY snapshotsChanged)
  Q_PROPERTY(bool listLoaded READ listLoaded NOTIFY snapshotsChanged)

  // ---- 最近一次产品级远端备份的结论（给界面直接显示）----
  //
  // 为什么单独留一个属性，而不是只写进状态条：状态条是"临时提示"，会被下一次
  // 操作顶掉；而"本次实际创建的是完整基线"这条信息必须在用户看着 Remote
  // Backup 区域时一直成立。kind 取值：""（还没做过）/ "full" / "incremental"
  // / "no-change"。summary 是给用户看的一句话。
  Q_PROPERTY(
      QString backupSummary READ backupSummary NOTIFY backupSummaryChanged)
  Q_PROPERTY(QString backupSummaryKind READ backupSummaryKind NOTIFY
                 backupSummaryChanged)
  // core 给出的“为什么这次不是增量”的原始理由（英文，来自共享 core）。
  // 它只进默认折叠的“技术详情”：用户要的是结论，诊断信息不能丢，但也不该
  // 摆在结论那一行。
  Q_PROPERTY(QString backupBaselineReason READ backupBaselineReason NOTIFY
                 backupSummaryChanged)

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
  // 注销对话框自己的错误行。注销失败的原因必须出现在**对话框里**，而不是只
  // 出现在这一页底部：用户是在对话框里点的"确认注销"，结果也应该在那里看到。
  Q_PROPERTY(QString deleteAccountError READ deleteAccountError NOTIFY
                 deleteAccountErrorChanged)
  // 登录 / 注册表单各自的错误行。同一条规则：错误写在**触发它的那个表单**
  // 里——用户点了"登录"，反馈就出现在登录框下面，而不是页面最底下。
  Q_PROPERTY(QString loginError READ loginError NOTIFY loginErrorChanged)
  Q_PROPERTY(
      QString registerError READ registerError NOTIFY registerErrorChanged)

  // ---- 原始归档恢复：密码只有在 core 说"这份归档加密了"之后才索要 ----
  //
  // 这三个属性描述**同一个交互**的状态，页面据此决定显示哪一段：
  //   * rawRestoreAwaitingPassword = true：那份归档已经拿到手（下载 + SHA-256
  //     校验 + 按内容识别都过了），core 明确要求密码。会话里的字节留着，用户
  //     可以反复输密码重试，**不会**重新下载。
  //   * rawRestorePasswordError
  //   非空：上一次密码没通过（或归档完整性校验失败）。
  //     页面要给出"重新输入"的动作，而不是把整个流程关掉。
  //   * rawRestoreDestinationText：这次恢复的目标目录（回显，不让用户重选）。
  Q_PROPERTY(bool rawRestoreAwaitingPassword READ rawRestoreAwaitingPassword
                 NOTIFY rawRestoreStateChanged)
  Q_PROPERTY(QString rawRestorePasswordError READ rawRestorePasswordError NOTIFY
                 rawRestoreStateChanged)
  Q_PROPERTY(QString rawRestoreDestinationText READ rawRestoreDestinationText
                 NOTIFY rawRestoreStateChanged)

 public:
  explicit RemoteController(QObject* parent = nullptr);
  ~RemoteController() override;

  // ---- 只读访问器 ----
  QString host() const { return QString::fromStdString(endpoint_.host); }
  QString portText() const { return QString::number(endpoint_.port); }
  QString username() const { return username_; }
  // 上一次被接受的服务器身份 pin（原样回读，供界面回填与自检比对）。
  QString serverKeyPin() const { return server_key_pin_; }
  // 只是"此刻这条 socket 在不在"。它是短命的实现细节，界面上**不**允许
  // 把它渲染成一个常驻的"已连接 / 未连接"状态（见 serverReachabilityText）。
  bool connected() const { return client_.connected(); }
  bool authenticated() const { return authenticated_; }
  QString sessionText() const;
  QString serverReachabilityText() const;
  bool busy() const { return busy_; }
  QString busyAction() const { return busy_action_; }
  QVariantList snapshots() const { return snapshot_items_; }
  QString listSummary() const { return list_summary_; }
  bool listLoaded() const { return list_loaded_; }
  // 最近一次产品级远端备份的结论（给 Remote Backup 区域常驻显示；
  // 与 status_* 的临时提示分开，见文件末尾的成员说明）。
  QString backupSummary() const { return backup_summary_; }
  QString backupSummaryKind() const { return backup_summary_kind_; }
  QString backupBaselineReason() const { return last_backup_baseline_reason_; }
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
  QString deleteAccountError() const { return delete_account_error_; }
  QString loginError() const { return login_error_; }
  QString registerError() const { return register_error_; }
  QString serverKeyPinError() const { return server_key_pin_error_; }
  // ---- 连接方式 / 通道（PR #22）----
  QString connectionMode() const;
  QString sshHost() const { return ssh_host_; }
  QString sshLocalPort() const;
  QString sshProgram() const { return ssh_program_; }
  QString tunnelState() const;
  QString tunnelStateText() const { return tunnel_.stateText(); }
  bool tunnelReady() const { return tunnel_.IsReady(); }
  bool tunnelBusy() const { return tunnel_.IsBusy(); }
  QString tunnelFailureKind() const {
    return tunnel_.state() == SshTunnelManager::State::kFailed
               ? tunnel_.failureKindName()
               : QString();
  }
  QString tunnelFailureText() const {
    return tunnel_.state() == SshTunnelManager::State::kFailed
               ? tunnel_.failureText()
               : QString();
  }
  QString tunnelLocalEndpointText() const {
    return tunnel_.localEndpointText();
  }
  QString tunnelRemoteEndpointText() const {
    return tunnel_.remoteEndpointText();
  }
  bool tunnelOwnedByApp() const { return tunnel_.OwnsProcess(); }
  bool tunnelExternalReuse() const { return tunnel_.IsExternalReuse(); }
  QString tunnelDiagnosticText() const { return tunnel_.diagnosticText(); }
  // ---- pin 应用反馈（PR #22）----
  QString pinApplyState() const { return pin_apply_state_; }
  QString pinApplyMessage() const { return pin_apply_message_; }
  bool pinApplyOk() const {
    return pin_apply_state_ == QStringLiteral("applied") ||
           pin_apply_state_ == QStringLiteral("unchanged");
  }
  bool rawRestoreAwaitingPassword() const { return raw_awaiting_password_; }
  QString rawRestorePasswordError() const { return raw_password_error_; }
  QString rawRestoreDestinationText() const { return raw_destination_text_; }
  QString diagnosticText() const {
    return QString::fromStdString(last_detail_);
  }

  // ---- QML 入口 ----
  //
  // 注册与登录都把整份输入带进来；返回 false 表示请求**没有被受理**
  // （输入不合法、已经有一个操作在跑），此时状态条里已经有原因。
  // 注册。两次口令必须一致：不一致时**一个字节都不发**，本地直接拒绝。
  Q_INVOKABLE bool registerAccount(const QString& host,
                                   const QString& port_text,
                                   const QString& username,
                                   const QString& password,
                                   const QString& confirm_password);
  Q_INVOKABLE bool login(const QString& host, const QString& port_text,
                         const QString& username, const QString& password);

  // 本地退出：清掉内存里的会话（token 随连接一起消失，密码清空）。
  // 不新增服务端 logout opcode —— 服务端 token 到期前仍然有效，这一点写在
  // 06-KNOWN-LIMITATIONS.md 里，不假装它是撤销。
  Q_INVOKABLE void logoutLocal();

  // 注销账户：服务端删除该账户以及它的全部云端备份（不可撤销）。
  //
  // 两道本地闸门（都不发网络请求）：必须已登录；必须逐字输入当前账户名。
  // 然后服务端还要用当前口令再校验一次——一个被捡到的 token 不足以注销账户。
  // 与"退出登录"是两个不同的动作：退出登录只清本机内存。
  Q_INVOKABLE bool deleteAccount(const QString& password,
                                 const QString& username_confirmation);

  Q_INVOKABLE bool refreshList();
  Q_INVOKABLE bool uploadArchive(const QString& local_path,
                                 const QString& display_name);
  Q_INVOKABLE bool downloadArchive(const QString& snapshot_id,
                                   const QString& target_path,
                                   bool allow_overwrite);
  Q_INVOKABLE bool deleteSnapshot(const QString& snapshot_id);
  Q_INVOKABLE void clearStatus();
  // 打开注销对话框 / 重新提交时清掉上一次的错误行。
  Q_INVOKABLE void clearDeleteAccountError();
  // 输入框一被编辑就清掉对应表单的错误行：旧原因不能挂在新输入上。
  Q_INVOKABLE void clearLoginError();
  Q_INVOKABLE void clearRegisterError();

  // 设置服务器身份指纹（连接配置，不是口令）。
  //
  // 校验复用共享的 ParseServerKeyPin：只接受 "sha256:<64 位十六进制>" 与
  // "hex:<64 位十六进制>" 两种写法；不带前缀的裸十六进制会被拒——公钥与指纹
  // 长度相同，混起来就会把指纹当成公钥用。不合法时原因写进 serverKeyPinError
  // 并返回 false（一个字节都不发，也不改动上一次被接受的值）；合法则保存并
  // 返回 true。**下一次连接**（以及它之后的每一次）都会用这个新值。
  Q_INVOKABLE bool setServerKeyPin(const QString& pin);
  // 输入框一被编辑就清掉这一行：旧原因不能挂在新输入上。
  Q_INVOKABLE void clearServerKeyPinError();

  // ---- "应用"按钮的可见反馈（PR #22，人工验收发现的 UX bug）----
  //
  // 旧实现里"应用"只调用 setServerKeyPin，控制器合法时返回 true，而 QML 把
  // 返回值丢掉了 —— 于是用户点完"应用"，界面上**什么都没有发生**。
  //
  // applyServerKeyPin 是"应用"按钮该调的那一个：它做同样的校验与提交，另外
  // 把结果落成一段**看得见**的反馈（pinApplyMessage + pinApplyState）：
  //
  //   合法且是新值，当前没有活动连接 -> applied   "✓ 已应用，将在下一次连接时
  //                                               用于服务器身份校验"
  //   合法且是新值，当前有活动连接   -> applied   "✓ 已应用；当前连接保持不变，
  //                                               下次重连时生效"
  //   合法但与已生效的完全相同       -> unchanged "✓ 已是当前服务器身份指纹"
  //   不合法                         -> invalid   红色错误照旧写在输入框下面，
  //                                               **上一次生效的 pin 一个字节
  //                                               都不改**
  //
  // 它**只**改配置：不联网、不登录、不断开当前连接。这是配置动作，不是
  // connection test。
  //
  // 返回值是机器可读的 "applied" / "unchanged" / "invalid"，供自检断言。
  Q_INVOKABLE QString applyServerKeyPin(const QString& pin);
  // 输入框被编辑时清掉上一次的绿色反馈：旧结论不能挂在新输入上。
  Q_INVOKABLE void clearPinApplyState();

  // ---- 连接方式与 SSH 安全通道（PR #22）----
  //
  // setConnectionMode 接受 "ssh" / "direct"；其它值被拒并返回 false。
  // 切换模式不会自动联网，也不会杀掉正在用的通道 —— 它是配置动作。
  Q_INVOKABLE bool setConnectionMode(const QString& mode);
  // 下面三个是 Q_PROPERTY 的 WRITE 落点：它们只改配置，不联网、不动进程。
  void setSshHost(const QString& host);
  void setSshLocalPort(const QString& port_text);
  void setSshProgram(const QString& program);
  // 建立安全通道（ssh 模式）或明确告知直连模式不需要通道。
  // 它**不**需要 pin：建通道是传输层的事，BPSEC1 的 pin 在真正连接时才用。
  // 已经登录时顺带刷新一次列表 —— 这正是"用户点了建立连接之后能看到东西"。
  Q_INVOKABLE bool ensureConnection(const QString& host,
                                    const QString& port_text);
  // 关掉**本程序启动的**通道。用户自己在外面开的隧道不会被结束。
  Q_INVOKABLE void stopTunnel();

  // ---- 登录 / 注册：自动采用当前输入框里的 pin（PR #22）----
  //
  // 人工验收里最常见的一条路径是：填 pin -> 填用户名密码 -> 直接点登录。
  // 旧实现会用**上一次应用过的** pin（没有就是空），于是用户要么莫名其妙地
  // 失败，要么以为自己填的已经生效了。这两个入口在提交之前先
  // validate + commit 当前 draft pin，再开始网络操作：
  //
  //   填 pin -> 点登录        ✔ 现在就生效，不需要先点"应用"
  //   填 pin -> 应用 -> 登录   ✔ 与上面完全等价
  //
  // pin 不合法时**一个字节都不发**：红色错误照旧贴在指纹输入框下面，
  // 上一次生效的 pin 保持不变。
  Q_INVOKABLE bool loginWithPin(const QString& host, const QString& port_text,
                                const QString& username,
                                const QString& password,
                                const QString& base_pin);
  Q_INVOKABLE bool registerAccountWithPin(const QString& host,
                                          const QString& port_text,
                                          const QString& username,
                                          const QString& password,
                                          const QString& confirm_password,
                                          const QString& base_pin);

  // 文件对话框的 URL 互转与其它页面同一套实现。
  Q_INVOKABLE QString localPathFromUrl(const QUrl& url) const;
  // 选择器的起始位置：目录就用它本身，文件就用它所在目录，
  // 都没有（或不存在）时退回主目录。
  Q_INVOKABLE QUrl fileDialogStartUrl(const QString& path) const;
  Q_INVOKABLE QString suggestedDownloadName(const QString& display_name) const;

  // ---- 产品级远端备份 / 链恢复（QML 入口）----
  //
  // 与 backupctl remote backup / remote restore 走**同一个** core：
  //   QML -> RemoteController -> RunRemoteBackup / RunRemoteRestore
  // GUI 只传"源目录 + 策略"和"目标快照 + 目标目录"，其余全部由 core 决定。
  // 返回 false 表示请求没有被受理（输入不合法 / 忙碌 / 未登录 / 没有 pin），
  // 此时页面上已经有原因。
  Q_INVOKABLE bool backupRemote(const QString& source_directory,
                                bool allow_incremental);
  // 恢复某个远端快照（自动解析并下载整条依赖链）。snapshot_id 为空或目标目录
  // 为空都在本地被拒，一个字节都不发。
  Q_INVOKABLE bool restoreSnapshot(const QString& snapshot_id,
                                   const QString& destination_directory);
  // 恢复一份**原始归档**（lineage 为空的远端条目）——用户看到的按钮与产品级
  // 一样叫"恢复"，区别只在实现：它下载那一个 blob，由 core 按**内容**认出格式，
  // 再交给既有的本地恢复核心独立恢复；它不需要、也不使用任何远端依赖链。
  // 随机文件、损坏归档、版本不支持、单独的 delta 都会明确失败，并且不会在目标
  // 目录留下半成品。
  //
  // 密码只有在 core 明确说"这份归档加密了"之后才索要：第一次调用（password
  // 留空）会以 rawRestoreAwaitingPassword = true 结束，界面切到密码那一段，
  // 之后用 restoreRawArchiveWithPassword 重试——用的是同一份已经下载并校验过的
  // 字节，不重新下载。
  Q_INVOKABLE bool restoreRawArchive(const QString& snapshot_id,
                                     const QString& destination_directory,
                                     const QString& password);
  // 用户在密码那一段点"继续恢复"：用**同一份已经下载并校验过的字节**再恢复
  // 一次，不重新下载。目标目录沿用第一次选定的那一个。
  Q_INVOKABLE bool restoreRawArchiveWithPassword(const QString& password);
  // 用户在密码那一段取消：终止这次交互、清零口令、删掉临时归档。
  // 目标目录不变（失败本来就不会碰它），状态回到空闲。
  Q_INVOKABLE void cancelRawRestore();
  // 用户改了源目录 / 策略或离开了这一页时清掉上一次的结论：旧结论挂在新输入上
  // 会误导（"增量备份完成"是上一次的事）。
  Q_INVOKABLE void clearBackupSummary();

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
  // ---- 产品级远端备份 / 恢复：自检需要读的结构化结果 ----
  bool lastBackupProducedDeltaForTest() const {
    return last_backup_produced_delta_;
  }
  bool lastBackupRebuiltFullForTest() const {
    return last_backup_rebuilt_full_;
  }
  bool lastBackupNoChangesForTest() const { return last_backup_no_changes_; }
  QString lastBackupSnapshotIdForTest() const {
    return last_backup_snapshot_id_;
  }
  qint64 lastBackupUploadedBytesForTest() const {
    return static_cast<qint64>(last_backup_uploaded_bytes_);
  }
  qint64 lastBackupGenerationForTest() const {
    return static_cast<qint64>(last_backup_generation_);
  }
  qint64 lastBackupChainRootBytesForTest() const {
    return static_cast<qint64>(last_backup_chain_root_bytes_);
  }
  QString lastBackupBaselineReasonForTest() const {
    return last_backup_baseline_reason_;
  }
  qint64 lastRestoreChainLengthForTest() const {
    return static_cast<qint64>(last_restore_chain_length_);
  }
  qint64 lastRestoreDeltaCountForTest() const {
    return static_cast<qint64>(last_restore_delta_count_);
  }
  qint64 lastRestoreDownloadedBytesForTest() const {
    return static_cast<qint64>(last_restore_downloaded_bytes_);
  }
  qint64 lastRestoreEntriesForTest() const {
    return static_cast<qint64>(last_restore_entries_);
  }
  // ---- 原始归档单独恢复（kRestoreRaw）：自检需要读的结构化结果 ----
  QString lastRawRestoreSha256ForTest() const {
    return last_raw_restore_sha256_;
  }
  QString lastRawRestoreFormatForTest() const {
    return last_raw_restore_format_;
  }
  qint64 lastRawRestoreDownloadedBytesForTest() const {
    return static_cast<qint64>(last_raw_restore_downloaded_bytes_);
  }
  qint64 lastRawRestoreEntriesForTest() const {
    return static_cast<qint64>(last_raw_restore_entries_);
  }
  bool lastRawRestorePasswordRequiredForTest() const {
    return last_raw_restore_password_required_;
  }
  int lastRawRestoreDownloadCountForTest() const {
    return last_raw_restore_download_count_;
  }
  QString lastRawRestoreArchivePathForTest() const {
    return last_raw_restore_archive_path_;
  }
  bool rawRestoreSessionAliveForTest() const { return raw_session_ != nullptr; }
  // ---- PR #22：连接层自检需要读的结构化结果 ----
  QString tunnelStateForTest() const { return tunnelState(); }
  QString tunnelFailureKindForTest() const { return tunnelFailureKind(); }
  QString tunnelLocalEndpointForTest() const {
    return tunnel_.localEndpointText();
  }
  qint64 tunnelPidForTest() const { return tunnel_.processId(); }
  bool tunnelOwnedForTest() const { return tunnel_.OwnsProcess(); }
  bool tunnelExternalReuseForTest() const { return tunnel_.IsExternalReuse(); }
  QString pinApplyStateForTest() const { return pin_apply_state_; }
  // 上一次因为"要先建立安全通道"而被**挂起**的网络操作（空 = 没有挂起过）。
  // 挂起是连接层的实现细节，界面看不到，但自检要能证明它真的发生过。
  QString lastDeferredActionForTest() const { return last_deferred_action_; }
  int deferredSubmitCountForTest() const { return deferred_submit_count_; }
  // 等通道状态机稳定（不是 kStarting / kStopping）。自检用。
  bool waitForTunnelIdle(int timeout_ms);
  // 直接换一个 ssh 可执行文件（自检用它模拟"本机没有 ssh"），
  // 走的是产品自己的"高级"配置项，不是测试专用的后门。
  void setSshProgramForTest(const QString& program) {
    ssh_program_ = program;
    emit connectionChanged();
  }
  void setSshHostForTest(const QString& host) { setSshHost(host); }
  void setSshLocalPortForTest(const QString& port_text) {
    setSshLocalPort(port_text);
  }
  void setConnectionModeForTest(const QString& mode) {
    setConnectionMode(mode);
  }
  // 故意杀掉**本程序启动的** ssh，用来验证 C12（通道死了 -> 下一次操作
  // 自动重建 + RESUME）。返回是否真的发出了信号。
  bool killOwnedTunnelForTest();

 signals:
  void endpointChanged();
  void sessionChanged();
  void reachabilityChanged();
  void deleteAccountErrorChanged();
  void loginErrorChanged();
  void registerErrorChanged();
  void serverKeyPinChanged();
  void serverKeyPinErrorChanged();
  // 连接方式 / SSH 主机 / 本地端口等连接配置发生变化。
  void connectionChanged();
  // 安全通道状态机、失败原因、本地端口或诊断文本发生变化。
  void tunnelChanged();
  // "应用"按钮的绿色反馈发生变化。
  void pinApplyChanged();
  // 最近一次产品级备份的结论（完整 / 增量 / 无变化）发生变化。
  void backupSummaryChanged();
  // 原始归档恢复交互的状态（是否需要密码 / 上一次密码错没错）发生变化。
  void rawRestoreStateChanged();
  void busyChanged();
  void snapshotsChanged();
  void progressChanged();
  void statusChanged();
  // 一次后台操作结束。kind 与 RemoteOpResult::Kind 同名，供测试分辨。
  void operationFinished(const QString& kind, bool succeeded);

 private:
  // 传输方向。用整数原子变量跨线程传，避免在后台线程碰 QString。
  enum class Phase { kNone = 0, kUpload = 1, kDownload = 2 };

  // 客户端怎么到达服务端。见上面的属性说明。
  enum class ConnectionMode { kSshTunnel = 0, kDirect = 1 };

  // 一条错误该出现在哪里。每个表单各有自己的错误行（登录 / 注册 / 注销
  // 对话框 / 连接设置里的服务器身份指纹），页面级操作用底部横幅。
  // 原始归档恢复交互的状态落地（会话、等待密码、密码错误、目标目录回显）。
  void ApplyRawRestoreState(const RemoteOpResult& result);

  enum class ErrorSurface {
    kLogin,
    kRegister,
    kDeleteAccount,
    kServerKey,
    kBanner
  };

  // 一条"等通道就绪之后再发"的请求。字段刻意与提交点一一对应：request 里
  // 已经带了 pin 与用户填的地址，flush 时只把 host/port 换成通道的本地端点。
  struct DeferredRequest {
    bool active = false;
    RemoteRequest request;
    QString action_text;
    ErrorSurface surface = ErrorSurface::kBanner;
  };

  // 把一句话送到指定的错误容器（同一个容器里不重复发信号）。
  void ReportSurfaceError(ErrorSurface surface, const QString& message);
  void ClearSurfaceError(ErrorSurface surface);
  // 输入校验：合规时把结果写进 out_*，否则把原因写进 surface 的错误行并返回
  // false。**不**改动 endpoint_/username_——那是 CommitEndpoint 的事，所以
  // 被拒的输入不会污染"上一次生效的地址"。
  bool ValidateEndpoint(const QString& host, const QString& port_text,
                        const QString& username, ErrorSurface surface,
                        QString* out_host, int* out_port,
                        QString* out_username);
  void CommitEndpoint(const QString& host, int port, const QString& username);
  // 口令校验：长度下限来自共享常量，上限与 NUL 规则来自共享校验器。
  bool ValidatePassword(const QString& password, ErrorSurface surface);
  // 注销对话框里的"当前密码"：空值、过短各给一句自己的话。
  bool ValidateCurrentPassword(const QString& password);
  // 失败信息按**操作**改写：同一个 error_kind 在登录 / 注册 / 注销三种场景
  // 下要说三句不同的话，而且都要说清"数据有没有变化"。
  QString SurfaceFailureMessage(RemoteOpResult::Kind kind,
                                const QString& error_kind,
                                const QString& fallback) const;
  // 提交前的统一闸门：busy 与"是否已登录"都在这里挡住，原因写进 surface。
  bool BeginOperation(const QString& action_text, bool need_login,
                      ErrorSurface surface = ErrorSurface::kBanner);

  // ---- 连接层（PR #22）----
  //
  // 一条网络操作在真正提交之前要经过的**唯一**通道就是 Submit() 本身
  // （见 .cpp）：直连直接提交；ssh 模式先确认通道真的还能用、而且仍然指向
  // 当前填的那个远端服务，没好的话把这条请求挂起、去建通道，建好之后自动
  // 把它接着发出去。
  //
  // 这样"填服务器/账号 -> 点登录"就真的能用：前置动作由系统自己补，而不是
  // 又制造一个隐藏顺序（先建通道 -> 再应用 pin -> 再登录）。
  //
  // 通道参数是否仍然与当前填写的远端服务一致。地址改了就必须重建 ——
  // 否则界面显示 B、实际却还走在通往 A 的隧道上。**纯函数**，不产生副作用。
  bool TunnelMatchesEndpoint() const;
  // 通道**此刻**是不是真的还能用。与 TunnelMatchesEndpoint 分开：
  //
  //   * 自有的 ssh：QProcess 的 finished / errorOccurred 已经把"死了"变成
  //     状态机里的 failed，所以这里只读状态，不做任何探测；
  //   * 外部**复用**的 listener：不是本进程启动的，没有信号可听，只能在
  //     **真正要提交一次操作之前**当场问一次（有界的一次 connect）。它不在了
  //     就**只解除借用**（绝不碰别人的进程），返回 false，让调用方去建自己的
  //     通道 —— 这就是"外部隧道死掉之后下一次操作自动恢复"的全部机制。
  //
  // 刻意不是后台周期探测：空闲就不该产生 TCP 流量（见 ssh_tunnel_manager.h
  // 顶部关于 ssh -L 的说明）。
  bool TransportUsableForSubmit();
  // 起一条通道（幂等：已经在建就什么都不做）。
  bool StartTunnelForEndpoint(const QString& action_text);
  void DeferRequest(const RemoteRequest& request, const QString& action_text,
                    ErrorSurface surface);
  // 通道就绪 -> 把挂起的那条请求接着发出去；通道失败 -> 用**通道的**原因
  // 结束这次操作（而不是笼统的网络错误）。
  void FlushDeferredRequest();
  void FailDeferredRequest();
  // 把 pin 的草稿提交成"已应用"。校验复用共享的 ParseServerKeyPin，不合法时
  // 把红色原因写进输入框那一行，并且**一个字节都不改**上一次生效的值。
  // 返回值："applied" / "unchanged" / "invalid"。setServerKeyPin 与
  // applyServerKeyPin 都走它，登录 / 注册的自动提交也走它 —— "应用"这个动作
  // 只有一份实现。
  QString CommitServerKeyPin(const QString& pin);
  void SetPinApplyFeedback(const QString& state, const QString& message);
  // 把 "applied" / "unchanged" / "invalid" 落成那一段**看得见**的反馈。
  // "应用"按钮与登录 / 注册的自动提交走的是同一个函数 —— 两条路径给出同一句
  // 话，是结构性的，而不是靠两处文案恰好写得一样。
  void PublishPinApplyFeedback(const QString& state);
  // 只提交 host / port（不动 username_）：给"建立连接"用，它只需要地址。
  void CommitHostPort(const QString& host, int port);
  // 把一条请求真正交给后台线程。Submit() 负责"先解决传输层"，这里只负责发。
  void DispatchRequest(const RemoteRequest& request);
  // 通道状态机的落点：有请求在等就接着发 / 如实报失败；只是"建立连接"在等
  // 就结束忙碌并给一句结论。
  void OnTunnelStateChanged();
  // 提交一次后台操作。**调用前必须已经通过 BeginOperation**：busy_ 在提交之前
  // 同步置位，所以任何一个时刻只可能有一个 watcher 在跑，也就不存在"旧结果
  // 覆盖新状态"的窗口（async stale-result 的结构性防线）。
  void Submit(const RemoteRequest& request);
  // 把后台线程的返回值落到界面状态上。结果只从 QFuture 里读一次：
  // 后台线程写、主线程读的就是同一个值，不存在第二份"待读结果"。
  void OnOperationFinished();
  void ApplyResult(const RemoteOpResult& result);
  // 空闲基线：把这一页的临时提示换成"当前真实的连接状态"。
  void ResetIdleStatus();
  // 同一条基线，但**不看 busy_**：一次操作的失败结果是在 SetBusy(false) 之前
  // 处理的，那时横幅上还挂着"正在登录"这类已经过期的运行状态。
  void SetIdleBaseline();

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
  // 只有真的变化时才发信号：界面上的可达性提示不该被无谓地刷新。
  void SetReachability(RemoteReachability value);
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
  // endpoint_.server_key_pin 与 server_key_pin_ 必须始终是同一个值：每一次
  // 提交（request.endpoint = endpoint_ 的每一处）都要再显式带一次 pin，
  // 少一处就等于那一条操作在"没有 pin"的情况下连接，会以 kNoPinConfigured
  // 失败。见 .cpp 里 CommitEndpoint / setServerKeyPin / 各个提交点。
  backupproject::net::RemoteEndpoint endpoint_;
  QString username_;
  // 服务器身份 pin：可以公开，但同样只存在于内存（不落盘，见文件顶部说明）。
  QString server_key_pin_;
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
  QString delete_account_error_;
  QString login_error_;
  QString register_error_;
  QString server_key_pin_error_;
  // ---- PR #22：pin 的"应用"反馈 ----
  QString pin_apply_state_;  // "" / "applied" / "unchanged" / "invalid"
  QString pin_apply_message_;  // 绿色那一行；invalid 时为空（红字在输入框下）

  // ---- PR #22：连接方式与 SSH 安全通道 ----
  //
  // 连接方式与通道配置。与 host / port / pin 一样**只存在于内存**：当前
  // 部署的唯一事实来源是 ~/.ssh/config，产品不另造一套持久化。
  ConnectionMode connection_mode_ = ConnectionMode::kSshTunnel;
  QString ssh_host_ = QStringLiteral("aliyun-ecs");
  // 0 = 自动挑一个空闲回环端口。
  int ssh_local_port_ = 0;
  QString ssh_program_;
  SshTunnelManager tunnel_;
  // 一条正在等通道就绪的请求（最多一条：busy_ 在提交之前就置位了）。
  DeferredRequest deferred_;
  QString last_deferred_action_;
  int deferred_submit_count_ = 0;
  // 只等通道、不等某一条请求（用户点了"建立连接"）。
  bool tunnel_only_wait_ = false;
  // BeginOperation 收到的错误落点：请求被挂起之后要用它把失败送回**原来的
  // 那个表单**，而不是一律丢到页面底部的横幅里。
  ErrorSurface pending_surface_ = ErrorSurface::kBanner;

  // 上一次连接尝试的结果。默认"不知道"：界面在真的试过之前什么都不说。
  RemoteReachability reachability_ = RemoteReachability::kUnknown;

  QString status_kind_ = QStringLiteral("idle");
  QString status_title_ = QStringLiteral("未登录");
  QString status_message_;

  // ---- 最近一次产品级远端备份的结论 ----
  //
  // 与 status_* 分开存放：状态条是"临时提示"，这一组是"当前这一页关于上一次
  // 远端备份的事实"，不会被别的操作顺手清掉，直到用户改了输入或又做了一次。
  QString backup_summary_;
  QString backup_summary_kind_;
  // 最近一次远端备份请求里用户选的策略（结论文案要用它区分“完整”与“兜底”）。
  bool last_backup_incremental_requested_ = false;
  bool last_backup_produced_delta_ = false;
  bool last_backup_rebuilt_full_ = false;
  bool last_backup_no_changes_ = false;
  QString last_backup_snapshot_id_;
  std::uint64_t last_backup_uploaded_bytes_ = 0;
  std::uint64_t last_backup_chain_root_bytes_ = 0;
  std::uint64_t last_backup_generation_ = 0;
  QString last_backup_baseline_reason_;
  std::uint64_t last_restore_chain_length_ = 0;
  std::uint64_t last_restore_delta_count_ = 0;
  std::uint64_t last_restore_downloaded_bytes_ = 0;
  std::uint64_t last_restore_entries_ = 0;
  std::uint64_t last_raw_restore_downloaded_bytes_ = 0;
  std::uint64_t last_raw_restore_entries_ = 0;
  bool last_raw_restore_password_required_ = false;
  QString last_raw_restore_sha256_;
  QString last_raw_restore_format_;
  // ---- 原始归档恢复交互（见上面的属性说明）----
  // raw_session_ 非空 = 那份归档已经下载并校验过、正等着用户输入密码。
  // 它是**唯一**持有临时工作目录所有权的地方：清掉它就等于删掉那份临时归档。
  std::shared_ptr<backupproject::net::RemoteRawRestoreSession> raw_session_;
  bool raw_awaiting_password_ = false;
  QString raw_password_error_;
  QString raw_destination_text_;
  int last_raw_restore_download_count_ = 0;
  QString last_raw_restore_archive_path_;
};

}  // namespace backup_modern

#endif  // BACKUP_PROJECT_UI_MODERN_REMOTE_CONTROLLER_H_
