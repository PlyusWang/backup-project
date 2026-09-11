// main.cpp
//
// 现代 QML GUI 的入口。除了正常启动，还带三个开发期开关：
//   --smoke-test                        建引擎、建窗口、跑几帧就退出
//   --screenshot <目录>                 三个页面 × 两套主题渲染成 PNG
//   --self-test <源> <仓库> <恢复目录>   真跑一次备份 + 恢复并报告结果
// 另有 --native-frame：退回系统原生标题栏（Wayland
//
// 这些开关让没有显示器的环境也能验证界面：离屏平台插件把窗口真正建出来，
// 自检模式再切一遍页面、换一次主题、跑一次备份恢复，不需要人盯着屏幕。
// 上自绘标题栏万一不稳时的兜底）。

#include <QDir>
#include <QEventLoop>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStringList>
#include <QTimer>
#include <cstdio>

#include "app_theme.h"
#include "backup_controller.h"

namespace {

const int kPageCount = 3;
int g_qml_warnings = 0;

// QML 的运行期问题（binding loop、类型错误、模块缺失……）都以 Qt warning 发出。
// 这里显式计数：--smoke-test / --screenshot 一旦出现 QML 警告就直接判失败，
// 免得靠人去一屏日志里翻。
// 分流之后两类信息一眼可辨：QML 问题计进 g_qml_warnings 决定退出码，
// 其它警告只打印，不会误伤。
void MessageHandler(QtMsgType type, const QMessageLogContext& context,
                    const QString& message) {
  Q_UNUSED(context);
  const bool is_warning =
      type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg;
  if (!is_warning) {
    std::fprintf(stdout, "%s\n", qPrintable(message));
    return;
  }
  const bool looks_like_qml =
      message.contains(QStringLiteral(".qml")) ||
      message.contains(QStringLiteral("QML")) ||
      message.contains(QStringLiteral("Binding loop")) ||
      message.contains(QStringLiteral("is not installed"));
  if (looks_like_qml) {
    ++g_qml_warnings;
    std::fprintf(stderr, "[qml-warning] %s\n", qPrintable(message));
  } else {
    std::fprintf(stderr, "[warning] %s\n", qPrintable(message));
  }
  if (type == QtFatalMsg) {
    std::abort();
  }
}

// 抓图前等动画真正结束再抓。
// 页面切换带 150ms 淡入，早先只跑几轮 processEvents 会在动画中途抓帧，
// 拿到的是半透明画面（卡片底色被混成背景色），所以这里改成按时间等。
// 用事件循环等，而不是空转：等待期间合成器还要处理帧回调，
// 把主线程占死反而会让动画停在原地。
void WaitForAnimation(int milliseconds) {
  QEventLoop loop;
  QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
  loop.exec();
}

// --screenshot：三个页面 × 两套主题各抓一张 PNG。
// 抓帧走窗口自己的 grabWindow()，和用户看到的是同一条渲染路径，
// 不是另画一份示意图。
int CaptureScreenshots(QQuickWindow* window, backup_modern::AppTheme* theme,
                       const QString& directory) {
  if (!QDir().mkpath(directory)) {
    std::fprintf(stderr, "无法创建截图目录: %s\n", qPrintable(directory));
    return 1;
  }
  // 文件名固定成 页面-主题.png，方便文档和脚本按名字引用，
  // 不用在文档里写死带时间戳的路径。
  const char* page_names[kPageCount] = {"home", "backup", "restore"};
  for (int dark = 0; dark < 2; ++dark) {
    theme->setDark(dark == 1);
    for (int page = 0; page < kPageCount; ++page) {
      window->setProperty("currentPage", page);
      WaitForAnimation(300);
      const QImage image = window->grabWindow();
      if (image.isNull()) {
        std::fprintf(stderr, "grabWindow() 返回空图像\n");
        return 1;
      }
      const QString path =
          QStringLiteral("%1/%2-%3.png")
              .arg(
                  directory, QString::fromLatin1(page_names[page]),
                  dark == 1 ? QStringLiteral("dark") : QStringLiteral("light"));
      if (!image.save(path)) {
        std::fprintf(stderr, "截图保存失败: %s\n", qPrintable(path));
        return 1;
      }
      std::printf("screenshot: %s\n", qPrintable(path));
    }
  }
  return 0;
}

// 它验的是桥加核心这一整条链路：先备份再恢复，任何一步失败
// 就把核心的原文错误打到 stderr 并以非 0 退出。
// --self-test：命令行下没有 QML 绑定，用控制器自带的状态等待任务结束。
int RunSelfTest(backup_modern::BackupController* controller,
                const QString& source, const QString& repository,
                const QString& destination) {
  controller->setSourcePath(source);
  controller->setRepositoryPath(repository);
  controller->setRestorePath(destination);

  if (!controller->startBackup() || !controller->waitForIdle(600000) ||
      !controller->lastSucceeded()) {
    std::fprintf(stderr, "backup failed: %s\n",
                 qPrintable(controller->statusMessage()));
    return 1;
  }
  std::printf("backup ok\n");

  if (!controller->startRestore() || !controller->waitForIdle(600000) ||
      !controller->lastSucceeded()) {
    std::fprintf(stderr, "restore failed: %s\n",
                 qPrintable(controller->statusMessage()));
    return 1;
  }
  std::printf("restore ok\n");
  return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  QGuiApplication app(argc, argv);
  // QSettings 依赖这两个名字决定配置落盘位置，必须在读主题之前设置好。
  QCoreApplication::setApplicationName("backup-gui-modern");
  QCoreApplication::setOrganizationName("backup-project");
  // 固定用 Basic 风格：不跟随发行版的 GTK/GNOME 主题，
  // 否则同一份 QML 在不同 Linux 上会长得完全不一样。
  // 样式名必须和 QML 里 import 的 QtQuick.Controls.Basic 对得上。
  QQuickStyle::setStyle(QStringLiteral("Basic"));

  // 开关解析保持最朴素的形式：出现即生效，顺序无关，
  // 不需要参数解析库，也不会因为多一个没见过的参数就退出。
  const QStringList arguments = QCoreApplication::arguments();
  const bool smoke_test = arguments.contains(QStringLiteral("--smoke-test"));
  const bool native_frame =
      arguments.contains(QStringLiteral("--native-frame"));
  const int screenshot_index =
      arguments.indexOf(QStringLiteral("--screenshot"));
  const int self_test_index = arguments.indexOf(QStringLiteral("--self-test"));

  qInstallMessageHandler(MessageHandler);

  backup_modern::AppTheme theme;
  backup_modern::BackupController controller;

  QQmlApplicationEngine engine;
  // 用上下文属性而不是注册 QML 类型：QML 侧直接写 theme.accent /
  // controller.busy， 不需要任何 import 声明，也就不会碰到模块路径问题。
  engine.rootContext()->setContextProperty(QStringLiteral("theme"), &theme);
  engine.rootContext()->setContextProperty(QStringLiteral("controller"),
                                           &controller);
  // 窗口用不用系统边框由 C++ 决定、QML 只读：窗口标志必须在窗口创建时定下来，
  // 之后再改会出现“已经画了一帧才换边框”的闪动。
  engine.rootContext()->setContextProperty(QStringLiteral("useNativeFrame"),
                                           native_frame);
  engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));

  if (engine.rootObjects().isEmpty()) {
    std::fprintf(stderr, "QML 根对象创建失败\n");
    return 1;
  }
  auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
  if (window == nullptr) {
    std::fprintf(stderr, "根对象不是 QQuickWindow\n");
    return 1;
  }
  // 先把窗口显示出来再往下走：--self-test 与 --screenshot 都依赖一个
  // 已经建好的窗口，提前返回会让这两条路径测的不是真实情况。
  window->show();
  WaitForAnimation(200);

  if (self_test_index >= 0) {
    if (self_test_index + 3 >= arguments.size()) {
      std::fprintf(
          stderr, "--self-test 需要三个参数: <源目录> <备份仓库> <恢复目录>\n");
      return 2;
    }
    return RunSelfTest(&controller, arguments.at(self_test_index + 1),
                       arguments.at(self_test_index + 2),
                       arguments.at(self_test_index + 3));
  }

  if (screenshot_index >= 0) {
    if (screenshot_index + 1 >= arguments.size()) {
      std::fprintf(stderr, "--screenshot 需要一个输出目录参数\n");
      return 2;
    }
    const int result =
        CaptureScreenshots(window, &theme, arguments.at(screenshot_index + 1));
    if (result != 0) {
      return result;
    }
    return g_qml_warnings == 0 ? 0 : 1;
  }

  if (smoke_test) {
    // 至少跑一轮事件循环：加载 QML、建窗口、切页、换主题都真正执行一遍。
    QTimer::singleShot(150, &app, [window, &theme]() {
      window->setProperty("currentPage", 1);
      theme.toggle();
    });
    QTimer::singleShot(320, &app, &QCoreApplication::quit);
  }

  const int exit_code = app.exec();
  if (exit_code != 0) {
    return exit_code;
  }
  // smoke / screenshot 之外的正常退出也报告 QML 警告数量，便于脚本判断。
  return g_qml_warnings == 0 ? 0 : 1;
}
