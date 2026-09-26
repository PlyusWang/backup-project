// backup_controller.h
//
// QML 与 C++ 核心之间唯一的桥。它做四件事：读/写配置、把任务丢给 QtConcurrent、
// 把结果翻译成 QML 能绑定的状态、以及把 repository 相关的核心调用编排起来。
// 文件复制、路径拓扑校验、归档格式、清单与命名规则全部仍然由
// BackupEngine / BackupCatalog / ConfigManager 负责，这里一行都没有重写。
//
// 依赖方向是单向的：
//
//   QML ──► BackupController ──► ConfigManager / BackupCatalog / BackupEngine
//
// QML 不直接持有 ConfigManager 或 BackupCatalog，不拼 repository 路径，也不解析
// 归档头 —— 它只认识 controller 上的属性与 Q_INVOKABLE。
//
// 这一版使用 Q_OBJECT：QML 需要 Q_PROPERTY / Q_INVOKABLE / signal，
// 这正是 moc 存在的意义。#6 的 Widgets GUI 为了躲避 moc 才刻意不用，
// 这里没有那个约束，就不必绕。

#ifndef BACKUP_PROJECT_UI_MODERN_BACKUP_CONTROLLER_H_
#define BACKUP_PROJECT_UI_MODERN_BACKUP_CONTROLLER_H_

#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <vector>

#include "backup_catalog.h"
#include "config_manager.h"
#include "filter.h"

namespace backup_modern {

// 归档与筛选核心在 backupproject 命名空间里。这里用别名引入，正文就能直接写
// Filter / FilterAction，不必到处加限定名。
using Filter = backupproject::Filter;
using FilterAction = backupproject::FilterAction;

// 错误原文不翻译、不截断：核心的报错里带着具体路径和原因，
// 直接显示比在桥这层换成一句笼统提示有用得多。
// 后台任务的返回值。只带“成功与否 + 核心原文错误”，不加工。
struct OperationOutcome {
  bool succeeded = false;
  QString error_message;
};

// 仓库列表的后台结果。除数据本身外还带 repository_path：它让 GUI 线程能判断
// “这份结果是不是我已经不关心的那个仓库”，从而丢弃过期结果。
// 全部是值类型，跨线程只发生一次拷贝，不共享任何状态。
struct CatalogOutcome {
  bool succeeded = false;
  QString repository_path;
  std::vector<backupproject::BackupRecord> records;
  QString error_message;
};

class BackupController : public QObject {
  Q_OBJECT
  // 三个 status* 属性是分开的：kind 决定 banner 的颜色和图标，
  // title 与 message 是两行文案。用不透明的字符串而不是 C++ enum，
  // 是为了让 QML 直接做判断，将来加一种状态也不必注册新类型。
  Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
  Q_PROPERTY(QString statusKind READ statusKind NOTIFY statusChanged)
  Q_PROPERTY(QString statusTitle READ statusTitle NOTIFY statusChanged)
  Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusChanged)
  // 源目录双向绑定到界面输入框：手动输入会写回这里，
  // 点“浏览”选完目录也会写回这里，控制器只负责原样保存。
  Q_PROPERTY(QString sourcePath READ sourcePath WRITE setSourcePath NOTIFY
                 sourcePathChanged)
  // 筛选规则：界面只负责收集字符串，解析与匹配全部走 C++ 的 Filter，
  // QML 侧不实现任何 glob。
  Q_PROPERTY(QStringList includeRules READ includeRules NOTIFY filtersChanged)
  Q_PROPERTY(QStringList excludeRules READ excludeRules NOTIFY filtersChanged)

  // ---- repository-driven 属性 ----
  // repositoryPath 只读：它唯一的写入口是 saveRepositoryPath()，
  // 因为改仓库必须先过 EnsureRepository 再落盘，不能只改内存里的字符串。
  Q_PROPERTY(
      QString repositoryPath READ repositoryPath NOTIFY repositoryPathChanged)
  // “配置里有没有仓库”，与“这个目录此刻在不在”是两件事：用户完全可能先配好，
  // 之后自己把目录删掉。那时 configured 仍为 true，刷新列表会报目录不可用。
  Q_PROPERTY(bool repositoryConfigured READ repositoryConfigured NOTIFY
                 repositoryPathChanged)
  // 仓库列表。每条是一个 QVariantMap，字段见 backup_controller.cpp 的
  // RecordToVariant()：只暴露 file name 与展示用文本，不暴露 archive 完整路径。
  Q_PROPERTY(
      QVariantList backupRecords READ backupRecords NOTIFY backupRecordsChanged)
  Q_PROPERTY(bool catalogBusy READ catalogBusy NOTIFY catalogStateChanged)
  Q_PROPERTY(QString catalogError READ catalogError NOTIFY catalogStateChanged)

 public:
  // 配置文件路径由调用方显式给出（正常启动由 main.cpp 从
  // QStandardPaths::AppConfigLocation 算出，测试用 --config-file 覆盖）。
  // 控制器自己不猜 HOME、不用 QSettings、不碰 XDG —— 那些都不属于它。
  explicit BackupController(const QString& config_file_path,
                            QObject* parent = nullptr);

  bool busy() const { return busy_; }
  QString statusKind() const { return status_kind_; }
  QString statusTitle() const { return status_title_; }
  QString statusMessage() const { return status_message_; }

  QString sourcePath() const { return source_path_; }
  void setSourcePath(const QString& path);

  QString repositoryPath() const { return repository_path_; }
  bool repositoryConfigured() const { return !repository_path_.isEmpty(); }
  QVariantList backupRecords() const { return backup_records_; }
  bool catalogBusy() const { return catalog_busy_; }
  QString catalogError() const { return catalog_error_; }

  QStringList includeRules() const { return include_rules_; }
  QStringList excludeRules() const { return exclude_rules_; }

  // 目录对话框交出来的是 URL，界面必须换成真正的本地路径。
  // 这里只信 QUrl::toLocalFile()：percent-encoding（%20、%E4%B8%AD）和 #
  // 这类字符靠手写字符串处理还原必然出错，URL 解析也不该由 QML 自己实现。
  Q_INVOKABLE QString localPathFromUrl(const QUrl& url) const;
  // 目录对话框的起始位置由本地路径转成 URL。路径为空、不存在或不是目录时
  // 回退到主目录；这个回退只决定对话框从哪里打开，不会写回任何路径字段。
  Q_INVOKABLE QUrl directoryDialogStartUrl(const QString& path) const;

  // 添加一条筛选规则：action 取 "include" / "exclude"。
  // 规则非法时返回 false，并把原因写进状态条，界面直接显示。
  // 语法检查用的是和 CLI、归档层完全相同的 Filter 实现。
  Q_INVOKABLE bool addFilterRule(const QString& action, const QString& rule);
  // 按界面列表顺序删除：先 include，后 exclude。
  Q_INVOKABLE bool removeFilterRule(int index);
  // 清空所有规则：回到"没有筛选"的 PR #8 行为。
  Q_INVOKABLE void clearFilterRules();

  // ---- 产品入口 ----
  // 保存备份仓库设置。顺序是刻意的：先 EnsureRepository 再
  // ConfigManager::Save， 只有落盘成功才更新内存里的 repositoryPath。
  Q_INVOKABLE bool saveRepositoryPath(const QString& path);
  // 异步刷新仓库列表。未配置 repository 时只清空，不启动后台扫描。
  Q_INVOKABLE void refreshBackups();
  // 自动命名的备份：源目录 + 已配置的 repository，文件名由 BackupCatalog 生成，
  // 界面不再要求用户填写归档完整路径。
  // 产物是 v2 container（MyPack + 不压缩 + 不加密）：界面展示的 uid / gid /
  // symlink / FIFO 只有 v2 装得下，v0.1 会把它们丢掉或者直接失败。
  Q_INVOKABLE bool startBackup();
  // 从仓库里恢复一个备份。QML 只传 file name，解析成真实路径由 Catalog 负责。
  Q_INVOKABLE bool startManagedRestore(const QString& file_name,
                                       const QString& destination_path);
  // 删除仓库里的一个备份。坏掉的 .bak 同样可以删。
  Q_INVOKABLE bool deleteBackup(const QString& file_name);
  // 回到“空闲”文案：界面上一动输入就调用它，免得上一次的结果一直挂着；
  // 任务进行中不会被清掉。
  Q_INVOKABLE void clearStatus();

  // ---- 仅供 main.cpp 自测调用，刻意不是 Q_INVOKABLE ----
  // 它们精确测试 controller → engine 的 direct archive 路径，复用同一个
  // Start() / RunOperation()，不是第二套 BackupEngine 调用逻辑。
  // direct backup 仍然使用当前 include / exclude 筛选规则，并且固定产 legacy
  // v0.1：它是旧格式的回归入口，产品路径已经不再走它。
  bool startDirectBackupForTest(const QString& source,
                                const QString& archive_file);
  bool startDirectRestoreForTest(const QString& archive_file,
                                 const QString& destination);

  // 下面两个只给命令行模式用：那里没有 QML 绑定，需要一个同步等待点，
  // 否则主函数不知道怎么算“跑完了”。返回值区分“真的跑完了”和“等超时了”，
  // 超时不能被误判成成功。
  bool waitForIdle(int timeout_ms);
  // 等仓库列表真正稳定：如果期间还有排队的刷新，只有最新那次也结束才算 idle。
  bool waitForCatalogIdle(int timeout_ms);
  bool lastSucceeded() const { return last_succeeded_; }

  // busy / status / 路径 / 列表各有独立信号，QML 只订阅自己用到的那些。
 signals:
  void busyChanged();
  void statusChanged();
  void sourcePathChanged();
  void repositoryPathChanged();
  void backupRecordsChanged();
  void catalogStateChanged();
  // 规则列表变化（增删清空）时发一次。
  void filtersChanged();
  // 任务结束时发一次，附带结果，便于 QML 或测试代码做后续动作。
  void operationFinished(bool succeeded);

 private:
  // 两种操作的流程完全一样，只有调用的核心接口不同，
  // 所以用枚举区分，共用同一份 Start() 与 RunOperation()。
  enum class Kind { kBackup, kRestore };

  // 备份产物的格式。刻意只留在控制器内部：没有 Q_PROPERTY、没有 Q_INVOKABLE，
  // QML 既看不见也传不进来 —— 界面上"开始备份"只有一条路，不存在"用哪种格式
  // 备份"这个用户选项。
  //
  //   kModernV2  —— 仓库驱动的正常备份（startBackup）。界面已经展示
  //                 uid / gid / user / group / symlink / FIFO，筛选预览也会说
  //                 某个 special entry"进入归档"，产物就必须真的装得下这些；
  //                 v0.1 装不下，只有 v2 container 可以。
  //   kLegacyV01 —— 仅供 startDirectBackupForTest 使用的显式 legacy 路径。
  //                 它保留 v0.1 产物，让旧格式始终有一条被真实执行的回归入口。
  enum class BackupFlavor { kLegacyV01, kModernV2 };

  // 后台函数：static，运行在别的线程上，只碰值类型和核心对象。
  //
  // flavor 只被 kBackup 分支读取：恢复没有"产物格式"这一说，归档是什么格式由
  // 它自己的 magic 决定（BackupEngine::Restore 按 magic 分流）。
  static OperationOutcome RunOperation(Kind kind, BackupFlavor flavor,
                                       const QString& first_path,
                                       const QString& second_path,
                                       const Filter& filter);
  // 后台扫描：线程内自建 BackupCatalog，不与 GUI 线程共享任何对象。
  static CatalogOutcome RunCatalogList(const QString& repository);

  // file_name 只用于任务结束后的状态提示（备份是自动生成的名字，
  // 恢复是用户选的那个名字）；它不是输入状态，也不参与核心调用。
  bool Start(Kind kind, BackupFlavor flavor, const QString& first_path,
             const QString& second_path, const QString& file_name,
             const Filter& filter);

  // 把界面收集的规则编成 Filter；失败时 error_message 里是原因。
  bool BuildFilter(Filter* filter, std::string* error_message) const;
  void SetStatus(const QString& kind, const QString& title,
                 const QString& message);
  void SetBusy(bool busy);

  // 启动时读一次配置。坏配置只如实报告，不自动改写、不自动删除、不猜默认值。
  void LoadConfig();
  void StartCatalogList();
  void SetCatalogBusy(bool busy);

  // 界面状态：路径、忙碌标记、状态卡片文案。只在 GUI 线程访问。
  // 筛选规则原文，按 include / exclude 分两组保存。
  QStringList include_rules_;
  QStringList exclude_rules_;

  QString source_path_;

  // 两个核心对象都是值成员：它们没有全局状态，也不需要跨线程共享。
  // 构造顺序很重要 —— config_manager_ 先拿到路径，LoadConfig() 才能用它。
  backupproject::ConfigManager config_manager_;
  backupproject::BackupCatalog catalog_;

  QString repository_path_;
  QVariantList backup_records_;
  bool catalog_busy_ = false;
  QString catalog_error_;
  // latest request wins：扫描期间来的刷新请求只记这一位，当前扫描一结束就
  // 立刻用最新的 repository 重扫，不会堆出并行扫描。
  bool catalog_pending_ = false;

  // busy_ 是界面唯一的“正在干活”依据：按钮禁用、输入框禁用、
  // 进度条动画全部绑它，避免三处各判一次、判出不一致的结果。
  // 它只表示 backup / restore 这类会写盘的数据操作 —— 仓库列表刷新是只读的，
  // 不纳入 busy，所以也不纳入关窗守卫。
  bool busy_ = false;
  bool last_succeeded_ = false;
  // 任务结束时要区分是备份还是恢复，否则成功文案只能写成笼统的“操作完成”。
  Kind active_kind_ = Kind::kBackup;
  // 本次操作对应的 file name，只用于状态提示；不构成新的 QML 输入状态。
  QString active_file_name_;

  QString status_kind_ = QStringLiteral("idle");
  QString status_title_ = QStringLiteral("等待操作");
  QString status_message_;

  // 两个 watcher 职责分开：数据操作一个，仓库列表一个。两者可以同时进行，
  // 互不覆盖对方的生命周期。
  QFutureWatcher<OperationOutcome> watcher_;
  QFutureWatcher<CatalogOutcome> catalog_watcher_;
};

}  // namespace backup_modern

#endif  // BACKUP_PROJECT_UI_MODERN_BACKUP_CONTROLLER_H_
