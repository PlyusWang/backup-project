// operation_page.h
//
// 备份页和恢复页的控件结构完全一样：两个路径输入 + 一个主按钮 + 状态卡片，
// 区别只有文案、哪个字段是归档文件、以及最终调用的引擎方法，
// 所以合成一个类，用 OperationKind 区分。
// 这样避免两份几乎逐行重复的页面代码，也没有引入“Controller/Service”之类的层级。

#ifndef BACKUP_PROJECT_UI_DESKTOP_OPERATION_PAGE_H_
#define BACKUP_PROJECT_UI_DESKTOP_OPERATION_PAGE_H_

#include <QFutureWatcher>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <functional>

#include "theme.h"

class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;

namespace backup_gui {

// 两种操作共用一个页面类，只在这里区分最终调用哪个引擎方法。
enum class OperationKind { kBackup, kRestore };

// 后台任务的输入。只放值类型，跨线程传的是副本，不共享可变状态；
// 两个路径在语义上分别代表什么，由 kind 决定，后台函数不需要理解界面状态。
struct OperationRequest {
  OperationKind kind = OperationKind::kBackup;
  QString first_path;
  QString second_path;
  // 备份筛选规则（恢复页为空）。跨线程按值传递，后台线程用自己的副本，
  // 解析与匹配统一交给 C++ 的 Filter。
  QStringList include_rules;
  QStringList exclude_rules;
};

// 后台任务的返回值。成功与否 + 核心给出的原始错误信息。
// 失败时 error_message 一定来自 BackupEngine：GUI 不做二次加工，
// 否则“哪个路径、什么原因”这类关键信息会在转述里丢掉。
struct OperationResult {
  bool succeeded = false;
  QString error_message;
};

// 这一版刻意不使用 Q_OBJECT：信号槽全部用 Qt 现成的信号 + lambda 完成，
// 于是构建过程不需要 moc 生成步骤，Makefile 也就能保持简单。
class OperationPage : public QWidget {
 public:
  explicit OperationPage(OperationKind kind, QWidget* parent = nullptr);

  // 主题变化时只需要更新状态标题的颜色，其余样式由应用级 QSS 负责。
  void ApplyTheme(const ThemeColors& colors);

  // 主窗口用它判断“是否还有任务在跑”，以便决定要不要拦住关闭窗口。
  bool IsRunning() const;

  // 任务开始 / 结束时回调一次 true / false，交给 MainWindow 统一协调：
  // 同一时刻整个窗口只允许一个备份或恢复在跑，另一个页面的“开始”要锁住。
  // 用 std::function 而不是自定义信号，是为了继续避开 Q_OBJECT 与 moc。
  void SetBusyChangedCallback(std::function<void(bool)> callback);

  // 另一页正在跑任务时，把本页的“开始”按钮锁住（输入框仍然可以编辑）。
  void SetActionBlocked(bool blocked);

 private:
  // 真正的后台函数：必须是 static，而且不碰任何 QWidget。
  // 它跑在别的线程上，任何对 this 成员的隐式访问都可能是跨线程读写。
  static OperationResult RunOperation(const OperationRequest& request);

  void BuildLayout();
  // 备份页的筛选规则编辑：添加 / 删除 / 收集。规则合法性统一由核心 Filter
  // 判断。
  void AddFilterRule(bool exclude);
  void RemoveSelectedFilterRule();
  QStringList CollectFilterRules(bool exclude) const;
  // 两种字段用两种对话框：目录字段选目录，归档文件字段按操作类型
  // 走"另存"（备份）或"打开"（恢复）。
  void ChoosePath(int field_index);
  // 备份页的第二个字段、恢复页的第一个字段是归档文件，其余是目录。
  bool IsFileField(int field_index) const {
    return kind_ == OperationKind::kBackup ? field_index == 1
                                           : field_index == 0;
  }
  void StartOperation();
  void SetStatus(StatusKind kind, const QString& title, const QString& message);
  void ApplyStatusColors();
  // 按 running_ / action_blocked_ 两个状态统一刷新控件可用性，
  // 避免出现“按钮恢复了但输入框还锁着”这种只改一半的情况。
  void UpdateControlStates();

  QString WindowTitleText() const;
  QString SubtitleText() const;
  QString FirstFieldLabel() const;
  QString SecondFieldLabel() const;
  QString ActionButtonText() const;
  QString RunningTitle() const;
  QString RunningMessage() const;
  QString SucceededTitle() const;
  QString SucceededMessage() const;
  QString IdleMessage() const;

  // 界面层自己的状态：操作类型、当前主题颜色副本、状态卡片处于哪种状态，
  // 以及一串控件指针。这些只会在主线程被访问。
  OperationKind kind_;
  ThemeColors colors_;
  StatusKind status_kind_ = StatusKind::kIdle;

  QLabel* status_title_ = nullptr;
  QLabel* status_message_ = nullptr;
  QLineEdit* first_edit_ = nullptr;
  QLineEdit* second_edit_ = nullptr;
  // 两个“选择”按钮也要随 busy 一起禁用，所以必须留成员，
  // 不能像以前那样建成局部变量之后就不管了。
  QPushButton* choose_buttons_[2] = {nullptr, nullptr};
  QPushButton* action_button_ = nullptr;
  // 筛选规则控件只在备份页创建，恢复页保持为空指针。
  QLineEdit* filter_edit_ = nullptr;
  QListWidget* filter_list_ = nullptr;
  QProgressBar* progress_ = nullptr;
  // 本页是否在跑，和“另一页在跑”造成的锁定，两者分开记：
  // 前者决定输入框与选择按钮，后者只决定“开始”按钮。
  bool running_ = false;
  bool action_blocked_ = false;
  std::function<void(bool)> busy_changed_callback_;
  // watcher 作为成员存在，生命周期跟着页面走；页面被销毁时它会被一起析构，
  // 未完成的回调不会再命中任何已经释放的控件。
  QFutureWatcher<OperationResult> watcher_;
};

}  // namespace backup_gui

#endif  // BACKUP_PROJECT_UI_DESKTOP_OPERATION_PAGE_H_
