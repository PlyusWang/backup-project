// main.cpp
//
// 桌面 GUI 的入口。除了正常启动，还留了两个开发期开关：
//   --smoke-test          构造窗口、切页、换主题，跑完一个事件循环就退出
//   --screenshot <目录>   把“两套主题 × 两个页面”渲染成 PNG，便于人工检查
// 两个开关都不需要真实显示器，配合 QT_QPA_PLATFORM=offscreen 即可使用。

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QPixmap>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <iostream>

#include "main_window.h"

namespace {

const int kThemeCount = 2;
const int kPageCount = 2;

// 截图模式不是给用户用的，而是给开发和评审留一份能复查的证据：
// 四张图覆盖“两套主题 × 两个页面”，不必真的坐到显示器前逐个点。
// grab() 会直接把控件画进 QPixmap，所以截屏不依赖窗口管理器。
int SaveScreenshots(backup_gui::MainWindow* window, const QString& directory) {
  if (!QDir().mkpath(directory)) {
    std::cerr << "无法创建截图目录: " << directory.toStdString() << "\n";
    return 1;
  }

  const backup_gui::ThemeKind themes[kThemeCount] = {
      backup_gui::ThemeKind::kLight, backup_gui::ThemeKind::kDark};
  // 主题枚举、主题名、页面索引、页面名四个数组按下标一一对应，
  // 以后加主题或加页面时要一起改，别只改一半。
  const char* theme_names[kThemeCount] = {"light", "dark"};
  const int page_indexes[kPageCount] = {0, 1};
  const char* page_names[kPageCount] = {"backup", "restore"};

  // 主题在外层、页面在内层：换主题会重建整份 QSS，
  // 随后两个页面都会重新套一遍样式，顺序反了会抓到混色的中间态。
  for (int theme_index = 0; theme_index < kThemeCount; ++theme_index) {
    window->ApplyTheme(themes[theme_index]);
    for (int page_index = 0; page_index < kPageCount; ++page_index) {
      window->ShowPage(page_indexes[page_index]);
      // 切换之后先让布局和重绘跑完，否则抓到的可能是上一帧的尺寸。
      // 切页之后先跑一轮事件循环，让布局算出最终尺寸再抓图，
      // 否则可能抓到上一页的几何或者还没排好的控件位置。
      QCoreApplication::processEvents();
      const QString path =
          QString::fromUtf8("%1/%2-%3.png")
              .arg(directory, QString::fromUtf8(page_names[page_index]),
                   QString::fromUtf8(theme_names[theme_index]));
      if (!window->grab().save(path)) {
        std::cerr << "截图失败: " << path.toStdString() << "\n";
        return 1;
      }
      std::cout << "screenshot: " << path.toStdString() << "\n";
    }
  }
  return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  QApplication app(argc, argv);
  QApplication::setApplicationName("backup-gui");
  QApplication::setApplicationDisplayName(QString::fromUtf8("备份工具"));
  // QSettings 依赖这两个名字决定配置文件的落盘位置，
  // 必须在任何一次读取主题之前设置好。
  QApplication::setOrganizationName("backup-project");

  // 只解析两个开发期开关，其余参数保持 Qt 默认行为，不做额外解释。
  const QStringList arguments = QApplication::arguments();
  const bool smoke_test = arguments.contains("--smoke-test");
  const int screenshot_index = arguments.indexOf("--screenshot");

  // 窗口放在栈上：退出时先析构界面再结束进程，
  // 比堆分配更不容易留下悬空的对象或漏掉的父子关系。
  backup_gui::MainWindow window;
  // 冒烟测试和截图都要求画面稳定，动画会让结果带上半透明的中间帧，先关掉。
  window.SetAnimationEnabled(!smoke_test && screenshot_index < 0);
  window.show();

  // 截图模式跑完直接返回，不进事件循环，无显示环境下也不会卡在那里等事件。
  if (screenshot_index >= 0) {
    if (screenshot_index + 1 >= arguments.size()) {
      std::cerr << "--screenshot 需要一个输出目录参数\n";
      return 2;
    }
    return SaveScreenshots(&window, arguments.at(screenshot_index + 1));
  }

  if (smoke_test) {
    // 至少跑一轮事件循环：建窗口、应用主题、切页都实际执行一遍，
    // 有问题在这时就暴露，不用等用户点开界面才发现。
    QTimer::singleShot(150, &app, [&window]() {
      window.ShowPage(1);
      window.ApplyTheme(backup_gui::ThemeKind::kDark);
      window.ShowPage(0);
    });
    QTimer::singleShot(300, &app, &QCoreApplication::quit);
  }

  // 正常启动：进入事件循环，备份/恢复都在里面的异步任务中完成，
  // 主线程只负责响应界面事件。
  return app.exec();
}
