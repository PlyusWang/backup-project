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
#include <string>
#include <vector>

#include "remote_backup_client.h"

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

  // ---- kRestore：产品级链恢复的结果 ----
  std::uint64_t restore_chain_length = 0;
  std::uint64_t restore_delta_count = 0;
  std::uint64_t restore_downloaded_bytes = 0;
  std::uint64_t restore_reused_bytes = 0;
  std::uint64_t restore_restored_entries = 0;
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
  // kBackup：源目录；allow_incremental 就是界面上的策略（false = Full，
  // true = Incremental）。除此之外的一切（能不能续链、父是谁、代数、
  // lineage、要不要 bootstrap 缓存）都由 core 决定，GUI 不参与。
  std::string source_directory;
  bool allow_incremental = false;
  // kBackup：显示名。留空时由 core 按链规则生成。
  // kRestore：目标目录。
  std::string restore_destination;
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

 signals:
  void endpointChanged();
  void sessionChanged();
  void reachabilityChanged();
  void deleteAccountErrorChanged();
  void loginErrorChanged();
  void registerErrorChanged();
  void serverKeyPinChanged();
  void serverKeyPinErrorChanged();
  // 最近一次产品级备份的结论（完整 / 增量 / 无变化）发生变化。
  void backupSummaryChanged();
  void busyChanged();
  void snapshotsChanged();
  void progressChanged();
  void statusChanged();
  // 一次后台操作结束。kind 与 RemoteOpResult::Kind 同名，供测试分辨。
  void operationFinished(const QString& kind, bool succeeded);

 private:
  // 传输方向。用整数原子变量跨线程传，避免在后台线程碰 QString。
  enum class Phase { kNone = 0, kUpload = 1, kDownload = 2 };

  // 一条错误该出现在哪里。每个表单各有自己的错误行（登录 / 注册 / 注销
  // 对话框 / 连接设置里的服务器身份指纹），页面级操作用底部横幅。
  enum class ErrorSurface {
    kLogin,
    kRegister,
    kDeleteAccount,
    kServerKey,
    kBanner
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
};

}  // namespace backup_modern

#endif  // BACKUP_PROJECT_UI_MODERN_REMOTE_CONTROLLER_H_
