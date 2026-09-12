// backup_controller.cpp
//
// 桥的实现。刻意保持很薄：真正的备份/恢复只有一行调用，
// 其余代码都在处理“界面该显示什么”。

#include "backup_controller.h"

#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <string>

#include "backup_engine.h"

namespace backup_modern {

namespace {

// 这四个字符串本身也是接口的一部分：改名要同时改 StatusBanner.qml。
// 状态种类，QML 用它决定 banner 的颜色和图标。
const char kIdle[] = "idle";
const char kRunning[] = "running";
const char kSuccess[] = "success";
const char kError[] = "error";

}  // namespace

BackupController::BackupController(QObject* parent) : QObject(parent) {
  // watcher 以 this 为上下文：对象销毁时连接自动断开，后台任务即使还在跑
  // 也不会回调到已经释放的控制器上。
  connect(
      &watcher_, &QFutureWatcher<OperationOutcome>::finished, this, [this]() {
        const OperationOutcome outcome = watcher_.result();
        last_succeeded_ = outcome.succeeded;
        SetBusy(false);
        if (outcome.succeeded) {
          SetStatus(QString::fromLatin1(kSuccess), QStringLiteral("操作完成"),
                    QStringLiteral("目录已按要求处理完毕。"));
        } else {
          // 核心给的错误信息原样展示：里面写清了是哪个路径、什么原因，
          // 在桥这层改写成“操作失败，请重试”会把最有用的部分丢掉。
          SetStatus(QString::fromLatin1(kError), QStringLiteral("操作失败"),
                    outcome.error_message);
        }
        emit operationFinished(outcome.succeeded);
      });
}

// 相等就不发信号：QML 的双向绑定会把输入框的值再写回来一次，
// 少了这个判断会来回触发，形成绑定环。
void BackupController::setSourcePath(const QString& path) {
  if (source_path_ == path) {
    return;
  }
  // 路径原样保存：不做 trim、不补斜杠、不 lowercase，
  // Linux 下空格和大小写都是文件名的一部分。
  source_path_ = path;
  emit sourcePathChanged();
}

void BackupController::setBackupFilePath(const QString& path) {
  if (backup_file_path_ == path) {
    return;
  }
  backup_file_path_ = path;
  emit backupFilePathChanged();
}

void BackupController::setRestorePath(const QString& path) {
  if (restore_path_ == path) {
    return;
  }
  restore_path_ = path;
  emit restorePathChanged();
}

QString BackupController::localPathFromUrl(const QUrl& url) const {
  // 非本地 URL（远端、qrc 等）返回空串，由调用方决定怎么办，
  // 而不是硬拼出一个看起来像路径的字符串。
  return url.isLocalFile() ? url.toLocalFile() : QString();
}

QUrl BackupController::fileDialogStartUrl(const QString& path) const {
  // 保存归档时目标文件通常还不存在，所以这里不能像目录版那样要求路径存在。
  if (path.isEmpty()) {
    return QUrl::fromLocalFile(QDir::homePath());
  }
  return QUrl::fromLocalFile(path);
}

QUrl BackupController::directoryDialogStartUrl(const QString& path) const {
  const QFileInfo info(path);
  if (path.isEmpty() || !info.exists() || !info.isDir()) {
    return QUrl::fromLocalFile(QDir::homePath());
  }
  return QUrl::fromLocalFile(path);
}

bool BackupController::startBackup() {
  if (source_path_.isEmpty() || backup_file_path_.isEmpty()) {
    // 只做“有没有填”的检查；路径是否存在、拓扑是否合法都交给核心判断。
    SetStatus(QString::fromLatin1(kError), QStringLiteral("操作失败"),
              QStringLiteral("请先填写源目录与备份文件。"));
    return false;
  }
  return Start(Kind::kBackup, source_path_, backup_file_path_);
}

bool BackupController::startRestore() {
  if (backup_file_path_.isEmpty() || restore_path_.isEmpty()) {
    SetStatus(QString::fromLatin1(kError), QStringLiteral("操作失败"),
              QStringLiteral("请先填写备份文件与恢复目录。"));
    return false;
  }
  return Start(Kind::kRestore, backup_file_path_, restore_path_);
}

// 先置忙再启动线程：QML 收到 busyChanged 之后才会禁用按钮，
// 顺序反过来的话，线程已经跑起来而界面还允许再点一次。
// 忙的时候直接返回 false，不排队——界面上的按钮本来就是禁用的。
bool BackupController::Start(Kind kind, const QString& first_path,
                             const QString& second_path) {
  if (busy_) {
    // 双保险：QML 侧已经用 busy
    // 禁用了按钮，但快捷键或程序化调用仍可能走到这里。
    return false;
  }
  SetBusy(true);
  SetStatus(QString::fromLatin1(kRunning),
            kind == Kind::kBackup ? QStringLiteral("正在备份……")
                                  : QStringLiteral("正在恢复……"),
            QStringLiteral("正在复制目录，期间界面仍可正常操作。"));
  // 函数指针 + 值拷贝的参数：后台线程拿到的是自己的副本，不需要加锁。
  watcher_.setFuture(QtConcurrent::run(&BackupController::RunOperation, kind,
                                       first_path, second_path));
  return true;
}

// 每次调用都新建一个 BackupEngine：核心没有全局状态，
// 一个任务一个实例最省心，也不存在后台线程共享对象的问题。
// QString 到 std::string 走的是 UTF-8，中文路径能原样传给核心。
OperationOutcome BackupController::RunOperation(Kind kind,
                                                const QString& first_path,
                                                const QString& second_path) {
  // 这个函数跑在后台线程：只创建引擎、调一次接口，绝不触碰任何 QML 对象。
  backupproject::BackupEngine engine;
  std::string error_message;
  const std::string first = first_path.toStdString();
  const std::string second = second_path.toStdString();

  OperationOutcome outcome;
  if (kind == Kind::kBackup) {
    outcome.succeeded = engine.Backup(first, second, &error_message);
  } else {
    outcome.succeeded = engine.Restore(first, second, &error_message);
  }
  if (!outcome.succeeded) {
    outcome.error_message = QString::fromStdString(error_message);
  }
  return outcome;
}

// 只在真正变化时发信号，避免多余的界面重算。
void BackupController::SetBusy(bool busy) {
  if (busy_ == busy) {
    return;
  }
  busy_ = busy;
  emit busyChanged();
}

void BackupController::SetStatus(const QString& kind, const QString& title,
                                 const QString& message) {
  status_kind_ = kind;
  status_title_ = title;
  status_message_ = message;
  emit statusChanged();
}

// 用户一动输入就把上一次的结果提示收回去；任务进行中不清，
// 否则会把“正在备份”这条提示提前抹掉。
void BackupController::clearStatus() {
  if (busy_) {
    return;
  }
  SetStatus(QString::fromLatin1(kIdle), QStringLiteral("等待操作"), QString());
}

// busyChanged 先到就立即返回；超时则返回 false，让调用方知道任务可能还在跑。
// --self-test 用它替代 QML 的事件循环，不起窗口也能等任务结束。
bool BackupController::waitForIdle(int timeout_ms) {
  // 只给 --self-test 用：命令行没有 QML 绑定，需要一个同步等待点。
  QEventLoop loop;
  QTimer timer;
  timer.setSingleShot(true);
  connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
  connect(this, &BackupController::busyChanged, &loop, [&loop, this]() {
    if (!busy_) {
      loop.quit();
    }
  });
  if (!busy_) {
    return true;
  }
  timer.start(timeout_ms);
  loop.exec();
  return !busy_;
}

}  // namespace backup_modern
