// backup_controller.h
//
// QML 与 C++ 核心之间唯一的桥。它只做三件事：保存界面上的路径、把任务丢给
// QtConcurrent、把结果翻译成 QML 能绑定的状态；文件复制、路径拓扑校验、
// 错误文案全部仍然由 BackupEngine / FileSystem 负责，这里一行都没有重写。
//
// 这一版使用 Q_OBJECT：QML 需要 Q_PROPERTY / Q_INVOKABLE / signal，
// 这正是 moc 存在的意义。#6 的 Widgets GUI 为了躲避 moc 才刻意不用，
// 这里没有那个约束，就不必绕。

#ifndef BACKUP_PROJECT_UI_MODERN_BACKUP_CONTROLLER_H_
#define BACKUP_PROJECT_UI_MODERN_BACKUP_CONTROLLER_H_

#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <QUrl>

namespace backup_modern {

// 错误原文不翻译、不截断：核心的报错里带着具体路径和原因，
// 直接显示比在桥这层换成一句笼统提示有用得多。
// 后台任务的返回值。只带“成功与否 + 核心原文错误”，不加工。
struct OperationOutcome {
  bool succeeded = false;
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
  // 三条路径属性双向绑定到界面输入框：手动输入会写回这里，
  // 点“浏览”选完目录也会写回这里，控制器只负责原样保存。
  Q_PROPERTY(QString sourcePath READ sourcePath WRITE setSourcePath NOTIFY
                 sourcePathChanged)
  Q_PROPERTY(QString repositoryPath READ repositoryPath WRITE setRepositoryPath
                 NOTIFY repositoryPathChanged)
  Q_PROPERTY(QString restorePath READ restorePath WRITE setRestorePath NOTIFY
                 restorePathChanged)

 public:
  // 构造期只建立 finished 连接，不做任何文件系统访问，
  // 界面起来的快慢不受核心影响。
  explicit BackupController(QObject* parent = nullptr);

  bool busy() const { return busy_; }
  QString statusKind() const { return status_kind_; }
  QString statusTitle() const { return status_title_; }
  QString statusMessage() const { return status_message_; }

  QString sourcePath() const { return source_path_; }
  void setSourcePath(const QString& path);
  QString repositoryPath() const { return repository_path_; }
  void setRepositoryPath(const QString& path);
  QString restorePath() const { return restore_path_; }
  void setRestorePath(const QString& path);

  // 目录对话框交出来的是 URL，界面必须换成真正的本地路径。
  // 这里只信 QUrl::toLocalFile()：percent-encoding（%20、%E4%B8%AD）和 #
  // 这类字符靠手写字符串处理还原必然出错，URL 解析也不该由 QML 自己实现。
  Q_INVOKABLE QString localPathFromUrl(const QUrl& url) const;
  // 反向：对话框的 currentFolder 是 URL，起始位置由本地路径转过去。
  // 路径为空、不存在或不是目录时回退到主目录；这个回退只决定对话框从哪里
  // 打开，不会写回路径字段，所以用户照样可以手工输入尚不存在的仓库路径。
  Q_INVOKABLE QUrl directoryDialogStartUrl(const QString& path) const;
  // 供 QML 的按钮调用：返回 false 表示这次点击没有启动任务（busy 或输入为空）。
  Q_INVOKABLE bool startBackup();
  Q_INVOKABLE bool startRestore();
  // 回到“空闲”文案：界面上一动输入就调用它，免得上一次的结果一直挂着；
  // 任务进行中不会被清掉。
  Q_INVOKABLE void clearStatus();

  // 下面两个只给 --self-test 用：命令行模式下没有 QML 绑定，
  // 需要一个同步等待点，否则主函数不知道怎么算“跑完了”。
  // 返回值区分“真的跑完了”和“等超时了”，--self-test 依赖这个差别，
  // 否则超时会被误判成成功。
  bool waitForIdle(int timeout_ms);
  bool lastSucceeded() const { return last_succeeded_; }

  // busy / status / 三条路径各有独立信号，QML 只订阅自己用到的那些。
 signals:
  void busyChanged();
  void statusChanged();
  void sourcePathChanged();
  void repositoryPathChanged();
  void restorePathChanged();
  // 任务结束时发一次，附带结果，便于 QML 或测试代码做后续动作。
  void operationFinished(bool succeeded);

 private:
  // 两种操作的流程完全一样，只有调用的核心接口不同，
  // 所以用枚举区分，共用同一份 Start() 与 RunOperation()。
  enum class Kind { kBackup, kRestore };

  // 后台函数：static，运行在别的线程上，只碰值类型和 BackupEngine。
  static OperationOutcome RunOperation(Kind kind, const QString& first_path,
                                       const QString& second_path);

  bool Start(Kind kind, const QString& first_path, const QString& second_path);
  void SetStatus(const QString& kind, const QString& title,
                 const QString& message);
  void SetBusy(bool busy);

  // 界面状态：路径、忙碌标记、状态卡片文案。只在 GUI 线程访问。
  QString source_path_;
  QString repository_path_;
  QString restore_path_;
  // busy_ 是界面唯一的“正在干活”依据：按钮禁用、输入框禁用、
  // 进度条动画全部绑它，避免三处各判一次、判出不一致的结果。
  bool busy_ = false;
  bool last_succeeded_ = false;
  QString status_kind_ = QStringLiteral("idle");
  QString status_title_ = QStringLiteral("等待操作");
  QString status_message_;

  // 整个 GUI 只有这一个 watcher，所以“任何时刻最多一个备份/恢复”是天然成立的，
  // 不需要像 #6 那样让两个页面互相协调。
  // 任务期间 busy_ 为真，Start() 会直接拒绝第二次点击，
  // 所以这个 watcher 不会出现“上一个还没结束就被换掉”的情况。
  QFutureWatcher<OperationOutcome> watcher_;
};

}  // namespace backup_modern

#endif  // BACKUP_PROJECT_UI_MODERN_BACKUP_CONTROLLER_H_
