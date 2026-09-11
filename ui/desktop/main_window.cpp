// main_window.cpp
//
// 布局、导航、主题切换都在这里。页面内部的逻辑在 OperationPage，这里不重复。

#include "main_window.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCloseEvent>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

namespace backup_gui {

// 构造顺序：先定窗口尺寸和标题，再搭界面，最后套主题。
// 主题必须放在搭界面之后——QSS 依赖控件上已经设置好的 objectName。
MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
  setWindowTitle(QString::fromUtf8("备份工具"));
  resize(1100, 700);
  // 1100x700 在普通笔记本上排得比较舒服；
  // 最小尺寸保证侧栏加内容区都还放得下，再小就会出现挤压。
  setMinimumSize(900, 600);
  BuildLayout();
  ApplyTheme(LoadThemeKind());
}

// QSS 完全依赖 objectName
// 选控件（#Sidebar、#Card、#NavButton、#PrimaryButton……），
// 换句话说改这些名字就等于改样式：动字符串时记得对照 theme.cpp 看一眼。
void MainWindow::BuildLayout() {
  // 中央区域只有一层水平布局：左边固定宽度侧栏，右边自适应内容区。
  auto* central = new QWidget(this);
  auto* root = new QHBoxLayout(central);
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(0);

  // 侧栏宽度固定：导航项只有两个，做成自适应反而会让文字换行、按钮忽宽忽窄。
  auto* sidebar = new QWidget(central);
  sidebar->setObjectName("Sidebar");
  sidebar->setFixedWidth(200);
  BuildSidebar(sidebar);
  root->addWidget(sidebar);

  // 用 QStackedWidget 而不是每次重建页面：页面只有两个，
  // 切来切去时保留各自状态（已填路径、上次结果）更符合直觉。
  stack_ = new QStackedWidget(central);
  backup_page_ = new OperationPage(OperationKind::kBackup, stack_);
  restore_page_ = new OperationPage(OperationKind::kRestore, stack_);
  // 加入顺序就是页面索引，必须和侧栏导航按钮的顺序保持一致。
  stack_->addWidget(backup_page_);
  stack_->addWidget(restore_page_);
  root->addWidget(stack_, 1);

  // QMainWindow 必须显式接收中央控件，否则内容区不会被显示。
  setCentralWidget(central);
}

void MainWindow::BuildSidebar(QWidget* sidebar) {
  auto* layout = new QVBoxLayout(sidebar);
  layout->setContentsMargins(16, 20, 16, 20);
  layout->setSpacing(8);

  // 侧栏信息密度刻意做低：只有工具名、两个导航和一个主题按钮。
  auto* title = new QLabel(QString::fromUtf8("备份工具"), sidebar);
  title->setObjectName("AppTitle");
  layout->addWidget(title);
  layout->addSpacing(16);

  // QButtonGroup 负责互斥：两个导航必须只有一个处于选中态，
  // 否则会出现“两个页面同时高亮”的假象。
  auto* group = new QButtonGroup(sidebar);
  const QString names[2] = {tr("备份"), tr("恢复")};
  for (int index = 0; index < 2; ++index) {
    auto* button = new QPushButton(names[index], sidebar);
    button->setObjectName("NavButton");
    button->setCheckable(true);
    button->setCursor(Qt::PointingHandCursor);
    group->addButton(button, index);
    // 捕获 index 而不是按钮指针：回调里只需要知道切到第几页。
    connect(button, &QPushButton::clicked, this,
            [this, index]() { ShowPage(index); });
    nav_buttons_[index] = button;
    layout->addWidget(button);
  }
  nav_buttons_[0]->setChecked(true);

  // 弹性空间把主题按钮压到侧栏底部：窗口变高时它不会飘到中间去。
  layout->addStretch(1);

  theme_button_ = new QPushButton(sidebar);
  theme_button_->setCursor(Qt::PointingHandCursor);
  connect(theme_button_, &QPushButton::clicked, this,
          [this]() { ToggleTheme(); });
  layout->addWidget(theme_button_);
}

void MainWindow::ShowPage(int index) {
  if (index < 0 || index >= stack_->count()) {
    return;
  }
  stack_->setCurrentIndex(index);
  // 程序化切页也要同步勾选状态，否则导航高亮会和当前页面对不上。
  nav_buttons_[index]->setChecked(true);
  FadeInCurrentPage();
}

void MainWindow::ApplyTheme(ThemeKind kind) {
  theme_kind_ = kind;
  const ThemeColors colors =
      kind == ThemeKind::kDark ? DarkTheme() : LightTheme();
  // 应用级 QSS 一次设置，所有控件跟着变，不需要每个页面自己拼样式。
  // 应用级设置一次 QSS：主窗口、页面以及文件选择对话框都会跟着变。
  qApp->setStyleSheet(BuildStyleSheet(colors));
  // 两个页面都要单独通知：状态标题的颜色不经过 QSS，是页面自己画的。
  backup_page_->ApplyTheme(colors);
  restore_page_->ApplyTheme(colors);
  UpdateThemeButtonText();
  // 立刻落盘：用户切完主题直接关窗口，下次启动也应该沿用这次的选择。
  SaveThemeKind(kind);
}

// 目前只有两套主题，直接取反即可；以后加主题时这里换成循环选择。
void MainWindow::ToggleTheme() {
  ApplyTheme(theme_kind_ == ThemeKind::kLight ? ThemeKind::kDark
                                              : ThemeKind::kLight);
}

void MainWindow::UpdateThemeButtonText() {
  // 按钮写的是“点下去会变成什么”，比写“当前是什么”更少歧义。
  theme_button_->setText(theme_kind_ == ThemeKind::kLight ? tr("深色主题")
                                                          : tr("浅色主题"));
}

void MainWindow::FadeInCurrentPage() {
  QWidget* page = stack_->currentWidget();
  if (!animation_enabled_ || page == nullptr) {
    return;
  }
  // 切页只做 120ms 淡入：够表达“换页了”，又不会拖慢连续操作。
  // 动画只改透明度，不动布局参数，所以不会触发重排；
  // 页面内容较多时也只是淡入，不会出现控件跳动。
  auto* effect = new QGraphicsOpacityEffect(page);
  page->setGraphicsEffect(effect);
  auto* animation = new QPropertyAnimation(effect, "opacity", page);
  animation->setDuration(120);
  animation->setStartValue(0.0);
  animation->setEndValue(1.0);
  connect(animation, &QPropertyAnimation::finished, page, [page]() {
    // 延后一拍再摘掉 effect：此刻动画对象还在收尾，
    // 立刻删掉它的目标对象容易踩到生命周期问题。
    QTimer::singleShot(0, page, [page]() { page->setGraphicsEffect(nullptr); });
  });
  animation->start(QAbstractAnimation::DeleteWhenStopped);
}

void MainWindow::closeEvent(QCloseEvent* event) {
  // 后台任务是线程池里的复制操作，没法安全地中途掐断；
  // 这里只决定“窗口关不关”，不尝试取消任务。
  const bool busy = backup_page_->IsRunning() || restore_page_->IsRunning();
  if (!busy) {
    event->accept();
    return;
  }
  // 后台任务是线程池里的复制操作，没法安全地中途掐断；这里只问用户要不要走，
  // 选择退出时任务会继续跑到结束，界面控件已经不再被回调碰到。
  const QMessageBox::StandardButton answer = QMessageBox::question(
      this, tr("正在执行操作"),
      tr("备份或恢复还没有结束。现在退出会关闭界面，后台任务会继续执行到结束。"
         "确定退出吗？"),
      QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
  if (answer == QMessageBox::Yes) {
    event->accept();
  } else {
    event->ignore();
  }
}

}  // namespace backup_gui
