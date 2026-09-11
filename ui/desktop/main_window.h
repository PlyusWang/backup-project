// main_window.h
//
// 主窗口：左侧导航 + 右侧内容区。导航只有“备份 / 恢复”两项，底部是主题切换。
// 窗口本身不碰任何备份逻辑，只负责页面切换、主题应用和关闭时的收尾判断。

#ifndef BACKUP_PROJECT_UI_DESKTOP_MAIN_WINDOW_H_
#define BACKUP_PROJECT_UI_DESKTOP_MAIN_WINDOW_H_

#include <QMainWindow>

#include "operation_page.h"
#include "theme.h"

class QPushButton;
class QStackedWidget;

namespace backup_gui {

// 和 OperationPage 一样，这里也不使用 Q_OBJECT：
// 导航点击、主题切换都是标准信号 + lambda，不需要额外的 moc 步骤。
//
// 主窗口只做三件事：搭出侧栏和内容区、在导航与页面之间同步状态、
// 把主题应用到整个应用。备份/恢复本身完全由 OperationPage 转交核心执行。
class MainWindow : public QMainWindow {
 public:
  explicit MainWindow(QWidget* parent = nullptr);

  // 给 --smoke-test 和 --screenshot 用：直接切页、直接换主题，不需要模拟点击。
  // 这两个接口也是自动化验证的入口，所以放在 public 而不是做成私有槽。
  void ShowPage(int index);
  void ApplyTheme(ThemeKind kind);

  // 截图要的是稳定画面，动画会让 grab() 抓到半透明的中间帧，所以允许关掉。
  void SetAnimationEnabled(bool enabled) { animation_enabled_ = enabled; }

 protected:
  void closeEvent(QCloseEvent* event) override;

 private:
  void BuildLayout();
  void BuildSidebar(QWidget* sidebar);
  void ToggleTheme();
  void UpdateThemeButtonText();
  void FadeInCurrentPage();
  // 某个页面开始 / 结束任务时调用，把“全局同时只跑一个操作”这条规则落到按钮上。
  void HandleOperationBusyChanged(OperationPage* source, bool busy);

  ThemeKind theme_kind_ = ThemeKind::kLight;
  bool animation_enabled_ = true;

  // stack_ 持有两个页面，索引与导航按钮一一对应，顺序不要随意调整。
  QStackedWidget* stack_ = nullptr;
  OperationPage* backup_page_ = nullptr;
  OperationPage* restore_page_ = nullptr;
  // 主题按钮不参与导航互斥；theme_kind_ 是主题的唯一真值来源，
  // 按钮文字、QSS、状态颜色全部由它推导出来。
  QPushButton* theme_button_ = nullptr;
  QPushButton* nav_buttons_[2] = {nullptr, nullptr};
};

}  // namespace backup_gui

#endif  // BACKUP_PROJECT_UI_DESKTOP_MAIN_WINDOW_H_
